/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

/**
 * @file include/maxscale/monitor.h - The public monitor interface
 */

#include <maxscale/cdefs.h>

#include <openssl/sha.h>

#include <maxbase/jansson.h>
#include <maxscale/config.h>
#include <maxscale/dcb.h>
#include <maxscale/server.h>
#include <maxscale/protocol/mysql.h>

MXS_BEGIN_DECLS

struct mxs_monitor;
typedef struct mxs_monitor MXS_MONITOR;

/**
 * An opaque type from which types specific for a particular
 * monitor can be derived.
 */
typedef struct mxs_monitor_instance
{
} MXS_MONITOR_INSTANCE;

/**
 * @verbatim
 * The "module object" structure for a backend monitor module
 *
 * Monitor modules monitor the backend databases that MaxScale connects to.
 * The information provided by a monitor is used in routing decisions.
 *
 * The entry points are:
 *      startMonitor    Called by main to start the monitor
 *      stopMonitor     Called by main to shut down and destroy a monitor
 *      diagnostics     Called for diagnostic output
 *
 * startMonitor is called to start the monitoring process, it is called on the
 * MaxScale main thread and is responsible for creating a thread for the monitor
 * itself to run on. This should use the entry points defined in the thread.h
 * header file rather than make direct calls to the operating system threading
 * libraries. The return from startMonitor is a pointer that will be passed to
 * all other monitor API calls.
 *
 * @endverbatim
 *
 * @see load_module
 */
typedef struct mxs_monitor_api
{
    /**
     * @brief Create the monitor.
     *
     * This entry point is called once when MaxScale is started, for
     * creating the monitor.
     *
     * If the function fails, MaxScale will not start. That is, it
     * should fail only for fatal reasons such as not being able to
     * create vital resources.
     *
     * @param monitor  The monitor object.
     *
     * @return Pointer to the monitor specific data. Will be stored
     *         in @c monitor->handle.
     */
    MXS_MONITOR_INSTANCE*(*createInstance)(MXS_MONITOR * monitor);

    /**
     * @brief Destroy the monitor.
     *
     * This entry point is called once when MaxScale is shutting down, iff
     * the earlier call to @c initMonitor returned on object. The monitor should
     * perform all needed cleanup.
     *
     * @param monitor  The monitor object.
     */
    void (* destroyInstance)(MXS_MONITOR_INSTANCE* monitor);

    /**
     * @brief Start the monitor
     *
     * This entry point is called when the monitor is started. If the monitor
     * requires polling of the servers, it should create a separate monitoring
     * thread.
     *
     * @param monitor The monitor object
     * @param params  Parameters for this monitor
     *
     * @return True, if the monitor could be started, false otherwise.
     */
    bool (* startMonitor)(MXS_MONITOR_INSTANCE* monitor,
                          const MXS_CONFIG_PARAMETER* params);

    /**
     * @brief Stop the monitor
     *
     * This entry point is called when the monitor is stopped. If the monitor
     * uses a polling thread, the thread should be stopped.
     *
     * @param monitor The monitor object
     */
    void (* stopMonitor)(MXS_MONITOR_INSTANCE* monitor);

    /**
     * @brief Write diagnostic information to a DCB.
     *
     * @param monitor  The monitor object.
     * @param dcb      The dcb to write to.
     */
    void (* diagnostics)(const MXS_MONITOR_INSTANCE* monitor, DCB* dcb);

    /**
     * @brief Return diagnostic information about the monitor
     *
     * @param monitor  The monitor object.
     *
     * @return A JSON object representing the state of the monitor
     *
     * @see jansson.h
     */
    json_t* (*diagnostics_json)(const MXS_MONITOR_INSTANCE * monitor);
} MXS_MONITOR_API;

/**
 * The monitor API version number. Any change to the monitor module API
 * must change these versions using the rules defined in modinfo.h
 */
#define MXS_MONITOR_VERSION {4, 0, 0}

/**
 * Specifies capabilities specific for monitor.
 *
 * @see enum routing_capability
 *
 * @note The values of the capabilities here *must* be between 0x0001 0000 0000 0000
 *       and 0x0080 0000 0000 0000, that is, bits 48 to 55.
 */
typedef enum monitor_capability
{
    MCAP_TYPE_NONE = 0x0    // TODO: remove once monitor capabilities are defined
} monitor_capability_t;

/** Monitor's poll frequency */
#define MXS_MON_BASE_INTERVAL_MS 100

#define MXS_MONITOR_DEFAULT_ID 1UL      // unsigned long value

#define MAX_MONITOR_USER_LEN     512
#define MAX_MONITOR_PASSWORD_LEN 512

// Monitor state enum
typedef enum
{
    MONITOR_STATE_RUNNING,
    MONITOR_STATE_STOPPING,
    MONITOR_STATE_STOPPED
} monitor_state_t;

/* Return type of mon_ping_or_connect_to_db(). */
typedef enum
{
    MONITOR_CONN_EXISTING_OK,   /* Existing connection was ok and server replied to ping. */
    MONITOR_CONN_NEWCONN_OK,    /* No existing connection or no ping reply. New connection created
                                 * successfully. */
    MONITOR_CONN_REFUSED,       /* No existing connection or no ping reply. Server refused new connection. */
    MONITOR_CONN_TIMEOUT        /* No existing connection or no ping reply. Timeout on new connection. */
} mxs_connect_result_t;

/** Monitor events */
typedef enum
{
    UNDEFINED_EVENT   = 0,
    MASTER_DOWN_EVENT = (1 << 0),   /**< master_down */
    MASTER_UP_EVENT   = (1 << 1),   /**< master_up */
    SLAVE_DOWN_EVENT  = (1 << 2),   /**< slave_down */
    SLAVE_UP_EVENT    = (1 << 3),   /**< slave_up */
    SERVER_DOWN_EVENT = (1 << 4),   /**< server_down */
    SERVER_UP_EVENT   = (1 << 5),   /**< server_up */
    SYNCED_DOWN_EVENT = (1 << 6),   /**< synced_down */
    SYNCED_UP_EVENT   = (1 << 7),   /**< synced_up */
    DONOR_DOWN_EVENT  = (1 << 8),   /**< donor_down */
    DONOR_UP_EVENT    = (1 << 9),   /**< donor_up */
    NDB_DOWN_EVENT    = (1 << 10),  /**< ndb_down */
    NDB_UP_EVENT      = (1 << 11),  /**< ndb_up */
    LOST_MASTER_EVENT = (1 << 12),  /**< lost_master */
    LOST_SLAVE_EVENT  = (1 << 13),  /**< lost_slave */
    LOST_SYNCED_EVENT = (1 << 14),  /**< lost_synced */
    LOST_DONOR_EVENT  = (1 << 15),  /**< lost_donor */
    LOST_NDB_EVENT    = (1 << 16),  /**< lost_ndb */
    NEW_MASTER_EVENT  = (1 << 17),  /**< new_master */
    NEW_SLAVE_EVENT   = (1 << 18),  /**< new_slave */
    NEW_SYNCED_EVENT  = (1 << 19),  /**< new_synced */
    NEW_DONOR_EVENT   = (1 << 20),  /**< new_donor */
    NEW_NDB_EVENT     = (1 << 21),  /**< new_ndb */
} mxs_monitor_event_t;

/**
 * The linked list of servers that are being monitored by the monitor module.
 */
typedef struct monitored_server
{
    SERVER*                  server;/**< The server being monitored */
    MYSQL*                   con;   /**< The MySQL connection */
    bool                     log_version_err;
    int                      mon_err_count;
    uint64_t                 mon_prev_status;   /**< Status before starting the current monitor loop */
    uint64_t                 pending_status;    /**< Status during current monitor loop */
    int64_t                  disk_space_checked;/**< When was the disk space checked the last time */
    struct monitored_server* next;              /**< The next server in the list */
} MXS_MONITORED_SERVER;

/**
 * Representation of the running monitor.
 */
struct mxs_monitor
{
    char*                 name;                                 /**< The name of the monitor module */
    char                  user[MAX_MONITOR_USER_LEN];           /*< Monitor username */
    char                  password[MAX_MONITOR_PASSWORD_LEN];   /*< Monitor password */
    pthread_mutex_t       lock;
    MXS_CONFIG_PARAMETER* parameters;                       /*< configuration parameters */
    MXS_MONITORED_SERVER* monitored_servers;                /*< List of servers the monitor monitors */
    monitor_state_t       state;                            /**< The state of the monitor. This should ONLY be
                                                             * written to by the admin
                                                             *   thread. */
    int connect_timeout;                                    /**< Connect timeout in seconds for
                                                             * mysql_real_connect */
    int connect_attempts;                                   /**< How many times a connection is attempted */
    int read_timeout;                                       /**< Timeout in seconds to read from the server.
                                                             *   There are retries and the total effective
                                                             * timeout
                                                             *   value is three times the option value.
                                                             */
    int write_timeout;                                      /**< Timeout in seconds for each attempt to write
                                                             * to the server.
                                                             * There are retries and the total effective
                                                             * timeout value is
                                                             * two times the option value.
                                                             */
    MXS_MONITOR_API*      api;                              /**< The monitor api */
    char*                 module_name;                      /**< Name of the monitor module */
    MXS_MONITOR_INSTANCE* instance;                         /**< Instance returned from startMonitor */
    size_t                interval;                         /**< The monitor interval */
    int                   check_maintenance_flag;           /**< Set when admin requests a maintenance status
                                                             * change. */
    bool                   active;                          /**< True if monitor is active */
    time_t                 journal_max_age;                 /**< Maximum age of journal file */
    uint32_t               script_timeout;                  /**< Timeout in seconds for the monitor scripts */
    const char*            script;                          /**< Launchable script. */
    uint64_t               events;                          /**< Enabled monitor events. */
    uint8_t                journal_hash[SHA_DIGEST_LENGTH]; /**< SHA1 hash of the latest written journal */
    MxsDiskSpaceThreshold* disk_space_threshold;            /**< Disk space thresholds */
    int64_t                disk_space_check_interval;       /**< How often should a disk space check be made
                                                             * at most. */
    uint64_t            ticks;                              /**< Number of performed monitoring intervals */
    struct mxs_monitor* next;                               /**< Next monitor in the linked list */
};

/**
 * Monitor configuration parameters names
 */
extern const char CN_BACKEND_CONNECT_ATTEMPTS[];
extern const char CN_BACKEND_CONNECT_TIMEOUT[];
extern const char CN_BACKEND_READ_TIMEOUT[];
extern const char CN_BACKEND_WRITE_TIMEOUT[];
extern const char CN_DISK_SPACE_CHECK_INTERVAL[];
extern const char CN_EVENTS[];
extern const char CN_JOURNAL_MAX_AGE[];
extern const char CN_MONITOR_INTERVAL[];
extern const char CN_SCRIPT[];
extern const char CN_SCRIPT_TIMEOUT[];

bool check_monitor_permissions(MXS_MONITOR* monitor, const char* query);

void monitor_clear_pending_status(MXS_MONITORED_SERVER* ptr, uint64_t bit);
void monitor_set_pending_status(MXS_MONITORED_SERVER* ptr, uint64_t bit);
void monitor_check_maintenance_requests(MXS_MONITOR* monitor);

bool mon_status_changed(MXS_MONITORED_SERVER* mon_srv);
bool mon_print_fail_status(MXS_MONITORED_SERVER* mon_srv);

mxs_connect_result_t mon_ping_or_connect_to_db(MXS_MONITOR* mon, MXS_MONITORED_SERVER* database);
bool                 mon_connection_is_ok(mxs_connect_result_t connect_result);
void                 mon_log_connect_error(MXS_MONITORED_SERVER* database, mxs_connect_result_t rval);
const char*          mon_get_event_name(mxs_monitor_event_t event);

/**
 * Alter monitor parameters
 *
 * The monitor parameters should not be altered while the monitor is
 * running. To alter a parameter from outside a monitor module, stop the monitor,
 * do the alteration and then restart the monitor. The monitor "owns" the parameters
 * as long as it is running so if the monitor needs to change its own parameters,
 * it can do it without stopping itself.
 *
 * @param monitor Monitor whose parameter is altered
 * @param key     Parameter name to alter
 * @param value   New value for the parameter
 */
void mon_alter_parameter(MXS_MONITOR* monitor, const char* key, const char* value);

/**
 * @brief Handle state change events
 *
 * This function should be called by all monitors at the end of each monitoring
 * cycle. This will log state changes and execute any scripts that should be executed.
 *
 * @param monitor Monitor object
 * @param script Script to execute or NULL for no script
 * @param events Enabled events
 */
void mon_process_state_changes(MXS_MONITOR* monitor, const char* script, uint64_t events);

/**
 * @brief Hangup connections to failed servers
 *
 * Injects hangup events for DCB that are connected to servers that are down.
 *
 * @param monitor Monitor object
 */
void mon_hangup_failed_servers(MXS_MONITOR* monitor);

/**
 * @brief Report query errors
 *
 * @param db Database where the query failed
 */
void mon_report_query_error(MXS_MONITORED_SERVER* db);

/**
 * @brief Convert monitor to JSON
 *
 * @param monitor Monitor to convert
 * @param host    Hostname of this server
 *
 * @return JSON representation of the monitor
 */
json_t* monitor_to_json(const MXS_MONITOR* monitor, const char* host);

/**
 * @brief Convert all monitors to JSON
 *
 * @param host    Hostname of this server
 *
 * @return JSON array containing all monitors
 */
json_t* monitor_list_to_json(const char* host);

/**
 * @brief Get links to monitors that relate to a server
 *
 * @param server Server to inspect
 * @param host   Hostname of this server
 *
 * @return Array of monitor links or NULL if no relations exist
 */
json_t* monitor_relations_to_server(const SERVER* server, const char* host);

/**
 * @brief Store a journal of server states
 *
 * @param monitor Monitor to journal
 * @param master  The current master server or NULL if no master exists
 */
void store_server_journal(MXS_MONITOR* monitor, MXS_MONITORED_SERVER* master);

/**
 * @brief Load a journal of server states
 *
 * @param monitor Monitor where journal is loaded
 * @param master  Set to point to the current master
 */
void load_server_journal(MXS_MONITOR* monitor, MXS_MONITORED_SERVER** master);

/**
 * Find the monitored server representing the server.
 *
 * @param mon Cluster monitor
 * @param search_server Server to search for
 * @return Found monitored server or NULL if not found
 */
MXS_MONITORED_SERVER* mon_get_monitored_server(const MXS_MONITOR* mon, SERVER* search_server);

/**
 * Get an array of monitored servers. If a server defined in the config setting is not monitored by
 * the given monitor, that server is ignored and not inserted into the output array.
 *
 * @param params Config parameters
 * @param key Setting name
 * @param mon Monitor which should monitor the servers
 * @param monitored_servers_out Where to save output array. The caller should free the array, but not the
 * elements. The output must contain NULL before calling this function.
 * @return Output array size.
 */
int mon_config_get_servers(const MXS_CONFIG_PARAMETER* params,
                           const char* key,
                           const MXS_MONITOR* mon,
                           MXS_MONITORED_SERVER*** monitored_array_out);

/**
 * @brief Set the disk space threshold of a monitor
 *
 * @param server                The monitor.
 * @param disk_space_threshold  The disk space threshold as specified in the config file.
 *
 * @return True, if the provided string is valid and the threshold could be set.
 */
bool monitor_set_disk_space_threshold(MXS_MONITOR* monitor, const char* disk_space_threshold);

MXS_END_DECLS
