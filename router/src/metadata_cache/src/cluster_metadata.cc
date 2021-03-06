/*
  Copyright (c) 2016, 2021, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "cluster_metadata.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <errmsg.h>
#include <mysql.h>

#include "dim.h"
#include "group_replication_metadata.h"
#include "mysql/harness/logging/logging.h"
#include "mysqld_error.h"
#include "mysqlrouter/mysql_session.h"
#include "mysqlrouter/uri.h"
#include "mysqlrouter/utils_sqlstring.h"
#include "tcp_address.h"

using mysqlrouter::ClusterType;
using mysqlrouter::MySQLSession;
using mysqlrouter::sqlstring;
using mysqlrouter::strtoi_checked;
using mysqlrouter::strtoui_checked;
IMPORT_LOG_FUNCTIONS()

/**
 * Return a string representation of the input character string.
 *
 * @param input_str A character string.
 *
 * @return A string object encapsulation of the input character string. An empty
 *         string if input string is nullptr.
 */
std::string get_string(const char *input_str) {
  if (input_str == nullptr) {
    return "";
  }
  return std::string(input_str);
}

ClusterMetadata::ClusterMetadata(const std::string &user,
                                 const std::string &password,
                                 int connect_timeout, int read_timeout,
                                 int /*connection_attempts*/,
                                 const mysqlrouter::SSLOptions &ssl_options)
    : user_(user),
      password_(password),
      connect_timeout_(connect_timeout),
      read_timeout_(read_timeout) {
  if (ssl_options.mode.empty()) {
    ssl_mode_ = SSL_MODE_PREFERRED;  // default mode
  } else {
    try {
      ssl_mode_ = MySQLSession::parse_ssl_mode(ssl_options.mode);
      log_info("Connections using ssl_mode '%s'", ssl_options.mode.c_str());
    } catch (const std::logic_error &) {
      throw metadata_cache::metadata_error(
          "Error initializing metadata cache: invalid configuration item "
          "'ssl_mode=" +
          ssl_options.mode + "'");
    }
  }
  ssl_options_ = ssl_options;
}

/** @brief Destructor
 *
 * Disconnect and release the connection to the metadata node.
 * (RAII will close the connection in metadata_connection_)
 */
ClusterMetadata::~ClusterMetadata() = default;

bool ClusterMetadata::do_connect(MySQLSession &connection,
                                 const metadata_cache::ManagedInstance &mi) {
  std::string host = (mi.host == "localhost" ? "127.0.0.1" : mi.host);
  try {
    connection.set_ssl_options(ssl_mode_, ssl_options_.tls_version,
                               ssl_options_.cipher, ssl_options_.ca,
                               ssl_options_.capath, ssl_options_.crl,
                               ssl_options_.crlpath);
    connection.connect(host, static_cast<unsigned int>(mi.port), user_,
                       password_, "" /* unix-socket */, "" /* default-schema */,
                       connect_timeout_, read_timeout_);
    return true;
  } catch (const MySQLSession::Error & /*e*/) {
    return false;  // error is logged in calling function
  }
}

bool ClusterMetadata::connect_and_setup_session(
    const metadata_cache::ManagedInstance &metadata_server) noexcept {
  // Get a clean metadata server connection object
  // (RAII will close the old one if needed).
  try {
    metadata_connection_ = mysql_harness::DIM::instance().new_MySQLSession();
  } catch (const std::logic_error &e) {
    // defensive programming, shouldn't really happen
    log_error("Failed connecting with Metadata Server: %s", e.what());
    return false;
  }

  if (do_connect(*metadata_connection_, metadata_server)) {
    const auto result =
        mysqlrouter::setup_metadata_session(*metadata_connection_);
    if (result) {
      log_debug("Connected with metadata server running on %s:%i",
                metadata_server.host.c_str(), metadata_server.port);
      return true;
    } else {
      log_warning("Failed setting up the session on Metadata Server %s:%d: %s",
                  metadata_server.host.c_str(), metadata_server.port,
                  result.error().c_str());
    }
  } else {
    // connection attempt failed
    log_warning("Failed connecting with Metadata Server %s:%d: %s (%i)",
                metadata_server.host.c_str(), metadata_server.port,
                metadata_connection_->last_error(),
                metadata_connection_->last_errno());
  }

  metadata_connection_.reset();
  return false;
}

mysqlrouter::MetadataSchemaVersion
ClusterMetadata::get_and_check_metadata_schema_version(
    mysqlrouter::MySQLSession &session) {
  const auto version = mysqlrouter::get_metadata_schema_version(&session);

  if (version == mysqlrouter::kUpgradeInProgressMetadataVersion) {
    throw mysqlrouter::MetadataUpgradeInProgressException();
  }

  if (!metadata_schema_version_is_compatible(
          mysqlrouter::kRequiredRoutingMetadataSchemaVersion, version)) {
    throw metadata_cache::metadata_error(mysqlrouter::string_format(
        "Unsupported metadata schema on %s. Expected Metadata Schema version "
        "compatible to %s, got %s",
        session.get_address().c_str(),
        to_string(mysqlrouter::kRequiredRoutingMetadataSchemaVersion).c_str(),
        to_string(version).c_str()));
  }

  return version;
}

bool set_instance_ports(metadata_cache::ManagedInstance &instance,
                        const mysqlrouter::MySQLSession::Row &row,
                        const size_t classic_port_column,
                        const size_t x_port_column) {
  {
    const std::string classic_port = get_string(row[classic_port_column]);

    auto make_res = mysql_harness::make_tcp_address(classic_port);
    if (!make_res) {
      log_warning(
          "Error parsing host:port in metadata for instance %s: '%s': %s",
          instance.mysql_server_uuid.c_str(), row[classic_port_column],
          make_res.error().message().c_str());
      return false;
    }

    instance.host = make_res->address();
    instance.port = make_res->port() != 0 ? make_res->port() : 3306;
  }

  // X protocol support is not mandatory
  if (row[x_port_column] && *row[x_port_column]) {
    const std::string x_port = get_string(row[x_port_column]);
    auto make_res = mysql_harness::make_tcp_address(x_port);
    if (!make_res) {
      // There is a Shell bug (#27677227) that can cause the mysqlx port be
      // invalid in the metadata (>65535). For the backward compatibility we
      // need to tolerate this and still let the node be used for classic
      // connections (as the older Router versions did).

      // log_warning(
      //   "Error parsing host:xport in metadata for instance %s:"
      //   "'%s': %s",
      //   instance.mysql_server_uuid.c_str(), row[x_port_column],
      //   e.what());
      instance.xport = 0;
    } else {
      instance.xport = make_res->port() != 0 ? make_res->port() : 33060;
    }
  } else {
    instance.xport = instance.port * 10;
  }

  return true;
}

bool ClusterMetadata::update_router_version(
    const metadata_cache::ManagedInstance &rw_instance,
    const unsigned router_id) {
  auto connection = mysql_harness::DIM::instance().new_MySQLSession();
  if (!do_connect(*connection, rw_instance)) {
    log_warning(
        "Updating the router version in metadata failed: Could not connect to "
        "the writable cluster member");

    return false;
  }

  const auto result = mysqlrouter::setup_metadata_session(*connection);
  if (!result) {
    log_warning(
        "Updating the router version in metadata failed: could not set up the "
        "metadata session (%s)",
        result.error().c_str());

    return false;
  }

  MySQLSession::Transaction transaction(connection.get());
  // throws metadata_cache::metadata_error and
  // MetadataUpgradeInProgressException
  get_and_check_metadata_schema_version(*connection);

  sqlstring query;
  if (get_cluster_type() == ClusterType::GR_V1) {
    query =
        "UPDATE mysql_innodb_cluster_metadata.routers"
        " SET attributes = JSON_SET(IF(attributes IS NULL, '{}', attributes), "
        "'$.version', ?) WHERE router_id = ?";
  } else {
    query =
        "UPDATE mysql_innodb_cluster_metadata.v2_routers set version = ? "
        "where router_id = ?";
  }

  query << MYSQL_ROUTER_VERSION << router_id << sqlstring::end;
  try {
    connection->execute(query);
  } catch (const MySQLSession::Error &e) {
    if (e.code() == ER_TABLEACCESS_DENIED_ERROR) {
      log_warning(
          "Updating the router version in metadata failed: %s (%u)\n"
          "Make sure to follow the correct steps to upgrade your metadata.\n"
          "Run the dba.upgradeMetadata() then launch the new Router version "
          "when prompted",
          e.message().c_str(), e.code());
    }
  } catch (const std::exception &e) {
    log_warning("Updating the router version in metadata failed: %s", e.what());
  }

  transaction.commit();

  return true;
}

bool ClusterMetadata::update_router_last_check_in(
    const metadata_cache::ManagedInstance &rw_instance,
    const unsigned router_id) {
  // only relevant to for metadata V2
  if (get_cluster_type() == ClusterType::GR_V1) return true;

  auto connection = mysql_harness::DIM::instance().new_MySQLSession();
  if (!do_connect(*connection, rw_instance)) {
    log_warning(
        "Updating the router last_check_in in metadata failed: Could not "
        "connect to the writable cluster member");

    return false;
  }

  const auto result = mysqlrouter::setup_metadata_session(*connection);
  if (!result) {
    log_warning(
        "Updating the router last_check_in in metadata failed: could not set "
        "up the metadata session (%s)",
        result.error().c_str());

    return false;
  }

  MySQLSession::Transaction transaction(connection.get());
  // throws metadata_cache::metadata_error and
  // MetadataUpgradeInProgressException
  get_and_check_metadata_schema_version(*connection);

  sqlstring query =
      "UPDATE mysql_innodb_cluster_metadata.v2_routers set last_check_in = "
      "NOW() where router_id = ?";

  query << router_id << sqlstring::end;
  try {
    connection->execute(query);
  } catch (const std::exception &e) {
    log_warning("Updating the router last_check_in in metadata failed: %s",
                e.what());
  }

  transaction.commit();
  return true;
}

ClusterMetadata::auth_credentials_t ClusterMetadata::fetch_auth_credentials(
    const std::string &cluster_name) {
  ClusterMetadata::auth_credentials_t auth_credentials;
  sqlstring query =
      "SELECT user, authentication_string, privileges, authentication_method "
      "FROM mysql_innodb_cluster_metadata.v2_router_rest_accounts WHERE "
      "cluster_id=(SELECT cluster_id FROM "
      "mysql_innodb_cluster_metadata.v2_clusters WHERE cluster_name=?)";
  query << cluster_name << sqlstring::end;

  auto result_processor =
      [&auth_credentials](const MySQLSession::Row &row) -> bool {
    JsonDocument privileges;
    if (row[2] != nullptr) privileges.Parse<0>(get_string(row[2]).c_str());

    const auto username = get_string(row[0]);
    if (privileges.HasParseError()) {
      log_warning(
          "Skipping user '%s': invalid privilege format '%s', authentication "
          "will not be possible",
          username.c_str(), get_string(row[2]).c_str());
    } else if (get_string(row[3]) != "modular_crypt_format") {
      log_warning(
          "Skipping user '%s': authentication method '%s' is not supported for "
          "metadata_cache authentication",
          username.c_str(), get_string(row[3]).c_str());
    } else {
      auth_credentials[username] =
          std::make_pair(get_string(row[1]), std::move(privileges));
    }
    return true;
  };

  if (metadata_connection_)
    metadata_connection_->query(query, result_processor);
  return auth_credentials;
}

/**
 * @brief Returns value fo the bool tag set in the attributes
 *
 * @param attributes    string containing JSON with the attributes
 * @param name          name of the tag to be fetched
 * @param default_value value to be returned if the given tag is missing or
 * invalid or if the JSON string is invalid
 * @param[out] out_warning  output parameter where the function sets the
 * descriptive warning in case there was a JSON parsing error
 *
 * @note the function always sets out_warning to "" at the beginning
 *
 * @return value of the bool tag
 */
static bool get_bool_tag(const std::string &attributes, const std::string &name,
                         bool default_value, std::string &out_warning) {
  out_warning = "";
  if (attributes.empty()) return default_value;

  rapidjson::Document json_doc;
  json_doc.Parse(attributes.c_str(), attributes.length());

  if (!json_doc.IsObject()) {
    out_warning = "not a valid JSON object";
    return default_value;
  }

  if (!json_doc.HasMember("tags")) {
    return default_value;
  }

  if (!json_doc["tags"].IsObject()) {
    out_warning = "tags - not a valid JSON object";
    return default_value;
  }

  const auto tags = json_doc["tags"].GetObject();

  if (!tags.HasMember(name.c_str())) {
    return default_value;
  }

  if (!tags[name.c_str()].IsBool()) {
    out_warning = "tags." + name + " not a boolean";
    return default_value;
  }

  return tags[name.c_str()].GetBool();
}

bool get_hidden(const std::string &attributes, std::string &out_warning) {
  return get_bool_tag(attributes, metadata_cache::kNodeTagHidden,
                      metadata_cache::kNodeTagHiddenDefault, out_warning);
}

bool get_disconnect_existing_sessions_when_hidden(const std::string &attributes,
                                                  std::string &out_warning) {
  return get_bool_tag(attributes, metadata_cache::kNodeTagDisconnectWhenHidden,
                      metadata_cache::kNodeTagDisconnectWhenHiddenDefault,
                      out_warning);
}

// helper class - helps to log the warning about the instance only when the
// warning condition changes
struct LogSuppressor {
  static LogSuppressor &instance() {
    static LogSuppressor instance_;
    return instance_;
  }

  std::string get_warning_hidden(const std::string &instance_uuid) const {
    if (warnings_.count(instance_uuid) == 0) {
      return "";
    }

    return warnings_.at(instance_uuid).warning_hidden;
  }

  void set_warning_hidden(const std::string &instance_uuid,
                          const std::string &warning) {
    warnings_[instance_uuid].warning_hidden = warning;
  }

  std::string get_warning_disconnect_existing_sessions_when_hidden(
      const std::string &instance_uuid) {
    if (warnings_.count(instance_uuid) == 0) {
      return "";
    }

    return warnings_.at(instance_uuid)
        .warning_disconnect_existing_sessions_when_hidden;
  }

  void set_warning_disconnect_existing_sessions_when_hidden(
      const std::string &instance_uuid, const std::string &warning) {
    warnings_[instance_uuid].warning_disconnect_existing_sessions_when_hidden =
        warning;
  }

 private:
  struct instance_warnings {
    /* warning about the incorrect JSON for _hidden in the metadata from the
     * last query */
    std::string warning_hidden;

    /* last warning about the incorrect JSON for
     * _disconnect_existing_sessions_when_hidden from the last query */
    std::string warning_disconnect_existing_sessions_when_hidden;
  };

  // the key in the map is the instance_id
  std::map<std::string, instance_warnings> warnings_;

  // singleton
  LogSuppressor() = default;
  LogSuppressor(const LogSuppressor &) = delete;
  LogSuppressor &operator=(const LogSuppressor &) = delete;
};

void set_instance_attributes(metadata_cache::ManagedInstance &instance,
                             const std::string &attributes) {
  std::string warning;
  auto &log_suppressor = LogSuppressor::instance();

  instance.hidden = get_hidden(attributes, warning);
  // we want to log the warning only when it's changing
  if (warning !=
      log_suppressor.get_warning_hidden(instance.mysql_server_uuid)) {
    if (!warning.empty()) {
      log_warning("Error parsing _hidden from attributes JSON string: %s",
                  warning.c_str());
    } else {
      log_warning("Successfully parsed _hidden from attributes JSON string");
    }
    log_suppressor.set_warning_hidden(instance.mysql_server_uuid, warning);
  }

  instance.disconnect_existing_sessions_when_hidden =
      get_disconnect_existing_sessions_when_hidden(attributes, warning);
  // we want to log the warning only when it's changing
  if (warning !=
      log_suppressor.get_warning_disconnect_existing_sessions_when_hidden(
          instance.mysql_server_uuid)) {
    if (!warning.empty()) {
      log_warning(
          "Error parsing _disconnect_existing_sessions_when_hidden from "
          "attributes JSON string: %s",
          warning.c_str());
    } else {
      log_warning(
          "Successfully parsed _disconnect_existing_sessions_when_hidden from "
          "attributes JSON string");
    }
    log_suppressor.set_warning_disconnect_existing_sessions_when_hidden(
        instance.mysql_server_uuid, warning);
  }
}
