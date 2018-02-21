#pragma once

/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/cppdefs.hh>
#include <string>
#include <tr1/unordered_map>
#include <vector>

#include <maxscale/json_api.h>
#include <maxscale/monitor.h>
#include <maxscale/thread.h>

#include "utilities.hh"

/** Utility macro for printing both MXS_ERROR and json error */
#define PRINT_MXS_JSON_ERROR(err_out, format, ...)\
    do {\
       MXS_ERROR(format, ##__VA_ARGS__);\
       if (err_out)\
       {\
            *err_out = mxs_json_error_append(*err_out, format, ##__VA_ARGS__);\
       }\
    } while (false)

extern const int PORT_UNKNOWN;
extern const int64_t SERVER_ID_UNKNOWN;
class MariaDBMonitor;

typedef std::tr1::unordered_map<const MXS_MONITORED_SERVER*, MySqlServerInfo> ServerInfoMap;
typedef std::vector<MXS_MONITORED_SERVER*> ServerVector;

MySqlServerInfo* get_server_info(MariaDBMonitor* handle, const MXS_MONITORED_SERVER* db);
const MySqlServerInfo* get_server_info(const MariaDBMonitor* handle, const MXS_MONITORED_SERVER* db);

// MariaDB Monitor instance data
class MariaDBMonitor
{
private:
    MariaDBMonitor(const MariaDBMonitor&);
    MariaDBMonitor& operator = (const MariaDBMonitor&);
public:

    /**
     * Start the monitor instance and return the instance data, creating it if starting for the first time.
     * This function creates a thread to execute the monitoring.
     *
     * @param monitor General monitor data
     * @param params Configuration parameters
     * @return A pointer to MariaDBMonitor specific data.
     */
    static MariaDBMonitor* start_monitor(MXS_MONITOR *monitor, const MXS_CONFIG_PARAMETER* params);

    /**
     * Stop the monitor. Waits until monitor has stopped.
     */
    void stop_monitor();

    /**
     * (Re)join given servers to the cluster. The servers in the array are assumed to be joinable.
     * Usually the list is created by get_joinable_servers().
     *
     * @param joinable_servers Which servers to rejoin
     * @return The number of servers successfully rejoined
     */
    uint32_t do_rejoin(const ServerVector& joinable_servers);

    /**
     * Check if the cluster is a valid rejoin target.
     *
     * @return True if master and gtid domain are known
     */
    bool cluster_can_be_joined();

    MXS_MONITOR* monitor;          /**< Generic monitor object */
    volatile int shutdown;         /**< Flag to shutdown the monitor thread.
                                    *   Accessed from multiple threads. */
    int status;                    /**< Monitor status. TODO: This should be in MXS_MONITOR */
    MXS_MONITORED_SERVER *master;  /**< Master server for MySQL Master/Slave replication */
    ServerInfoMap server_info;     /**< Contains server specific information */
    unsigned long id;              /**< Monitor ID */
    bool warn_set_standalone_master; /**< Log a warning when setting standalone master */

    // Values updated by monitor
    int64_t master_gtid_domain;    /**< Gtid domain currently used by the master */
    string external_master_host;   /**< External master host, for fail/switchover */
    int external_master_port;      /**< External master port */

    // Replication topology detection settings
    bool mysql51_replication;      /**< Use MySQL 5.1 replication */
    bool detectStaleMaster;        /**< Monitor flag for MySQL replication Stale Master detection */
    bool detectStaleSlave;         /**< Monitor flag for MySQL replication Stale Master detection */
    bool multimaster;              /**< Detect and handle multi-master topologies */
    bool ignore_external_masters;  /**< Ignore masters outside of the monitor configuration */
    bool detect_standalone_master; /**< If standalone master are detected */
    bool replicationHeartbeat;     /**< Monitor flag for MySQL replication heartbeat */

    // Failover, switchover and rejoin settings
    int failcount;                 /**< How many monitoring cycles master must be down before auto-failover
                                    *   begins */
    uint32_t failover_timeout;     /**< Timeout in seconds for the master failover */
    uint32_t switchover_timeout;   /**< Timeout in seconds for the master switchover */
    bool verify_master_failure;    /**< Whether master failure is verified via slaves */
    int master_failure_timeout;    /**< Master failure verification (via slaves) time in seconds */
    bool auto_failover;            /**< If automatic master failover is enabled */
    bool auto_rejoin;              /**< Attempt to start slave replication on standalone servers or servers
                                    *   replicating from the wrong master automatically. */
    ServerVector excluded_servers; /**< Servers banned for master promotion during auto-failover. */

    // Other settings
    string script;                 /**< Script to call when state changes occur on servers */
    uint64_t events;               /**< enabled events */
    bool allow_cluster_recovery;   /**< Allow failed servers to rejoin the cluster */

private:
    THREAD m_thread;               /**< Monitor thread */

    // Failover, switchover and rejoin settings
    string m_replication_user;     /**< Replication user for CHANGE MASTER TO-commands */
    string m_replication_password; /**< Replication password for CHANGE MASTER TO-commands */

    MariaDBMonitor(MXS_MONITOR* monitor_base);
    ~MariaDBMonitor();
    bool load_config_params(const MXS_CONFIG_PARAMETER* params);

public:
    // Following methods should be private, change it once refactoring is done.
    string generate_change_master_cmd(const string& master_host, int master_port);
    int redirect_slaves(MXS_MONITORED_SERVER* new_master, const ServerVector& slaves,
                        ServerVector* redirected_slaves);
    bool set_replication_credentials(const MXS_CONFIG_PARAMETER* params);
    bool start_external_replication(MXS_MONITORED_SERVER* new_master, json_t** err_out);
    bool switchover_start_slave(MXS_MONITORED_SERVER* old_master, SERVER* new_master);
    bool redirect_one_slave(MXS_MONITORED_SERVER* slave, const char* change_cmd);
    bool join_cluster(MXS_MONITORED_SERVER* server, const char* change_cmd);
};
