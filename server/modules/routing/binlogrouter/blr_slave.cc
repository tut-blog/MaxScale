/*
 * Copyright (c) 2016 MariaDB Corporation Ab
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

/**
 * @file blr_slave.c - contains code for the router to slave communication
 *
 * The binlog router is designed to be used in replication environments to
 * increase the replication fanout of a master server. It provides a transparant
 * mechanism to read the binlog entries for multiple slaves while requiring
 * only a single connection to the actual master to support the slaves.
 *
 * The current prototype implement is designed to support MySQL 5.6 and has
 * a number of limitations. This prototype is merely a proof of concept and
 * should not be considered production ready.
 */

#include "blr.hh"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include <vector>
#include <maxscale/alloc.h>
#include <maxbase/atomic.h>
#include <maxscale/clock.h>
#include <maxscale/dcb.h>
#include <maxscale/housekeeper.h>
#include <maxscale/log.h>
#include <maxscale/maxscale.h>
#include <maxscale/poll.h>
#include <maxscale/router.h>
#include <maxscale/server.h>
#include <maxscale/service.h>
#include <maxscale/utils.h>
#include <maxscale/version.h>

using std::string;
using std::vector;

/**
 * This struct is used by sqlite3_exec callback routine
 * for SHOW BINARY LOGS.
 *
 * It stores the next row sequence number,
 * the last binlog file name read from gtid_maps storage
 * and the connected client DCB.
 */
typedef struct
{
    int         seq_no;     /* Output sequence in result set */
    char*       last_file;  /* Last binlog file found in GTID repo */
    const char* binlogdir;  /* Binlog files cache dir */
    DCB*        client;     /* Connected client DCB */
    bool        use_tree;   /* Binlog structure type */
    size_t      n_files;    /* How many files */
    uint64_t    rowid;      /* ROWID of router current file*/
} BINARY_LOG_DATA_RESULT;

/** Slave file read EOF handling */
typedef enum
{
    SLAVE_EOF_ROTATE = 0,
    SLAVE_EOF_WARNING,
    SLAVE_EOF_ERROR
} slave_eof_action_t;

static char*    get_next_token(char* str, const char* delim, char** saveptr);
extern int      load_mysql_users(SERV_LISTENER* listener);
extern void     blr_master_close(ROUTER_INSTANCE* router);
extern int      blr_file_new_binlog(ROUTER_INSTANCE* router, char* file);
extern int      blr_file_write_master_config(ROUTER_INSTANCE* router, char* error);
extern char*    blr_extract_column(GWBUF* buf, int col);
extern uint32_t extract_field(uint8_t* src, int bits);
void            blr_extract_header(register uint8_t* ptr, register REP_HEADER* hdr);
int             blr_file_get_next_binlogname(ROUTER_INSTANCE* router);
static void     encode_value(unsigned char* data, unsigned int value, int len);
static int      blr_slave_query(ROUTER_INSTANCE* router,
                                ROUTER_SLAVE* slave,
                                GWBUF* queue);
static int blr_slave_replay(ROUTER_INSTANCE* router,
                            ROUTER_SLAVE* slave,
                            GWBUF* master);
static void blr_slave_send_error(ROUTER_INSTANCE* router,
                                 ROUTER_SLAVE* slave,
                                 const char*   msg);
static int blr_slave_send_timestamp(ROUTER_INSTANCE* router,
                                    ROUTER_SLAVE* slave);
static int blr_slave_register(ROUTER_INSTANCE* router,
                              ROUTER_SLAVE* slave,
                              GWBUF* queue);
static int blr_slave_binlog_dump(ROUTER_INSTANCE* router,
                                 ROUTER_SLAVE* slave,
                                 GWBUF* queue);
int blr_slave_catchup(ROUTER_INSTANCE* router,
                      ROUTER_SLAVE* slave,
                      bool large);
uint8_t*   blr_build_header(GWBUF* pkt, REP_HEADER* hdr);
int        blr_slave_callback(DCB* dcb, DCB_REASON reason, void* data);
static int blr_slave_fake_rotate(ROUTER_INSTANCE* router,
                                 ROUTER_SLAVE* slave,
                                 BLFILE** filep,
                                 const char* new_file);
static uint32_t blr_slave_send_fde(ROUTER_INSTANCE* router,
                                   ROUTER_SLAVE* slave,
                                   GWBUF* fde);
static int blr_slave_send_maxscale_version(ROUTER_INSTANCE* router,
                                           ROUTER_SLAVE* slave);
static int blr_slave_send_server_id(ROUTER_INSTANCE* router,
                                    ROUTER_SLAVE* slave);
static int blr_slave_send_maxscale_variables(ROUTER_INSTANCE* router,
                                             ROUTER_SLAVE* slave);
static int blr_slave_send_master_status(ROUTER_INSTANCE* router,
                                        ROUTER_SLAVE* slave);
static int blr_slave_send_slave_status(ROUTER_INSTANCE* router,
                                       ROUTER_SLAVE* slave,
                                       bool all_slaves);
static int blr_slave_send_slave_hosts(ROUTER_INSTANCE* router,
                                      ROUTER_SLAVE* slave);
static int blr_slave_send_fieldcount(ROUTER_INSTANCE* router,
                                     ROUTER_SLAVE* slave,
                                     int count);
static int blr_slave_send_columndef(ROUTER_INSTANCE* router,
                                    ROUTER_SLAVE* slave,
                                    const char*   name,
                                    int type,
                                    int len,
                                    uint8_t seqno);
static int blr_slave_send_eof(ROUTER_INSTANCE* router,
                              ROUTER_SLAVE* slave,
                              int seqno);
static int blr_slave_send_disconnected_server(ROUTER_INSTANCE* router,
                                              ROUTER_SLAVE* slave,
                                              int server_id,
                                              int found);
static int blr_slave_disconnect_all(ROUTER_INSTANCE* router,
                                    ROUTER_SLAVE* slave);
static int blr_slave_disconnect_server(ROUTER_INSTANCE* router,
                                       ROUTER_SLAVE* slave,
                                       int server_id);
static int blr_slave_send_ok(ROUTER_INSTANCE* router,
                             ROUTER_SLAVE* slave);
static int blr_stop_slave(ROUTER_INSTANCE* router,
                          ROUTER_SLAVE* slave);
static int blr_start_slave(ROUTER_INSTANCE* router,
                           ROUTER_SLAVE* slave);
static void blr_slave_send_error_packet(ROUTER_SLAVE* slave,
                                        const char*   msg,
                                        unsigned int  err_num,
                                        const char*   status);
static int blr_apply_change_master(ROUTER_INSTANCE* router,
                                   int index,
                                   const ChangeMasterConfig& new_config,
                                   char* error);
static int blr_handle_change_master(ROUTER_INSTANCE* router,
                                    char* command,
                                    char* error);
static int   blr_set_master_hostname(ROUTER_INSTANCE* router, const char* hostname);
static int   blr_set_master_hostname(ROUTER_INSTANCE* router, const std::string& hostname);
static int   blr_set_master_port(ROUTER_INSTANCE* router, int port);
static char* blr_set_master_logfile(ROUTER_INSTANCE* router,
                                    const char* filename,
                                    char* error);
static void blr_master_get_config(ROUTER_INSTANCE* router,
                                  MasterServerConfig* current_master);
static void blr_master_restore_config(ROUTER_INSTANCE* router,
                                      const MasterServerConfig& current_master);
static void blr_master_set_empty_config(ROUTER_INSTANCE* router);
static void blr_master_apply_config(ROUTER_INSTANCE* router,
                                    const MasterServerConfig& prev_master);
static int blr_slave_send_ok_message(ROUTER_INSTANCE* router,
                                     ROUTER_SLAVE* slave,
                                     char* message);
static bool         blr_get_parsed_command_value(char* input, std::string* output);
static std::string* blr_validate_change_master_option(const char* option, ChangeMasterOptions* config);
static int          blr_set_master_user(ROUTER_INSTANCE* router, const char* user);
static int          blr_set_master_user(ROUTER_INSTANCE* router, const std::string& user);
static int          blr_set_master_password(ROUTER_INSTANCE* router, const char* password);
static int          blr_set_master_password(ROUTER_INSTANCE* router, const std::string& password);

static int blr_parse_change_master_command(char* input,
                                           char* error_string,
                                           ChangeMasterOptions* config);
static int blr_handle_change_master_token(char* input,
                                          char* error,
                                          ChangeMasterOptions* config);
static int blr_slave_send_var_value(ROUTER_INSTANCE* router,
                                    ROUTER_SLAVE* slave,
                                    const char*   variable,
                                    const char*   value,
                                    int column_type);
static int blr_slave_send_variable(ROUTER_INSTANCE* router,
                                   ROUTER_SLAVE* slave,
                                   const char*   variable,
                                   const char*   value,
                                   int column_type);
static int blr_slave_send_columndef_with_info_schema(ROUTER_INSTANCE* router,
                                                     ROUTER_SLAVE* slave,
                                                     const char*   name,
                                                     int type,
                                                     int len,
                                                     uint8_t seqno);
int blr_test_parse_change_master_command(char* input,
                                         char* error_string,
                                         ChangeMasterOptions* config);
char* blr_test_set_master_logfile(ROUTER_INSTANCE* router,
                                  const char* filename,
                                  char* error);
static int blr_slave_handle_variables(ROUTER_INSTANCE* router,
                                      ROUTER_SLAVE* slave,
                                      char* stmt);
static int blr_slave_send_warning_message(ROUTER_INSTANCE* router,
                                          ROUTER_SLAVE* slave,
                                          const char*   message);
static int blr_slave_show_warnings(ROUTER_INSTANCE* router,
                                   ROUTER_SLAVE* slave);
static int blr_slave_send_status_variable(ROUTER_INSTANCE* router,
                                          ROUTER_SLAVE* slave,
                                          const char*   variable,
                                          const char*   value,
                                          int column_type);
static int blr_slave_handle_status_variables(ROUTER_INSTANCE* router,
                                             ROUTER_SLAVE* slave,
                                             char* stmt);
static int blr_slave_send_columndef_with_status_schema(ROUTER_INSTANCE* router,
                                                       ROUTER_SLAVE* slave,
                                                       const char*   name,
                                                       int type,
                                                       int len,
                                                       uint8_t seqno);
static bool blr_send_slave_heartbeat(void* inst);
static int  blr_slave_send_heartbeat(ROUTER_INSTANCE* router,
                                     ROUTER_SLAVE* slave);
static int blr_set_master_ssl(ROUTER_INSTANCE* router,
                              const ChangeMasterConfig& config,
                              char* error_message);
static int blr_slave_read_ste(ROUTER_INSTANCE* router,
                              ROUTER_SLAVE* slave,
                              uint32_t fde_end_pos);
static GWBUF* blr_slave_read_fde(ROUTER_INSTANCE* router,
                                 ROUTER_SLAVE* slave);
static bool blr_handle_simple_select_stmt(ROUTER_INSTANCE* router,
                                          ROUTER_SLAVE* slave,
                                          char* select_stmt);
static GWBUF* blr_build_fake_rotate_event(ROUTER_SLAVE* slave,
                                          unsigned long pos,
                                          const char*   filename,
                                          unsigned long serverid);
static int blr_send_connect_fake_rotate(ROUTER_INSTANCE* router,
                                        ROUTER_SLAVE* slave);
static bool blr_slave_gtid_request(ROUTER_INSTANCE* router,
                                   ROUTER_SLAVE* slave,
                                   bool req_file,
                                   unsigned long req_pos);

static int blr_send_fake_gtid_list(ROUTER_SLAVE* slave,
                                   const char*   gtid,
                                   uint32_t serverid);
static bool blr_handle_maxwell_stmt(ROUTER_INSTANCE* router,
                                    ROUTER_SLAVE* slave,
                                    const char*   maxwell_stmt);
static bool blr_handle_show_stmt(ROUTER_INSTANCE* router,
                                 ROUTER_SLAVE* slave,
                                 char* show_stmt);
static bool blr_handle_set_stmt(ROUTER_INSTANCE* router,
                                ROUTER_SLAVE* slave,
                                char* set_stmt);
static bool blr_handle_admin_stmt(ROUTER_INSTANCE* router,
                                  ROUTER_SLAVE* slave,
                                  char* admin_stmt,
                                  char* admin_options);
extern unsigned int blr_file_get_next_seqno(const char* filename);
extern uint32_t     blr_slave_get_file_size(const char* filename);
static void         blr_slave_skip_empty_files(ROUTER_INSTANCE* router,
                                               ROUTER_SLAVE* slave);

static inline void blr_get_file_fullpath(const char* binlog_file,
                                         const char* root_dir,
                                         char* full_path,
                                         const char* f_prefix);
static int blr_show_binary_logs(ROUTER_INSTANCE* router,
                                ROUTER_SLAVE* slave,
                                const char*   extra_data);

extern bool blr_parse_gtid(const char* gtid, MARIADB_GTID_ELEMS* info);
static int  binary_logs_select_cb(void* data,
                                  int cols,
                                  char** values,
                                  char** names);
static GWBUF* blr_create_result_row(const char* name,
                                    const char* value,
                                    int seq_no);
static int blr_slave_send_id_ro(ROUTER_INSTANCE* router,
                                ROUTER_SLAVE* slave);
static bool blr_handle_complex_select(ROUTER_INSTANCE* router,
                                      ROUTER_SLAVE* slave,
                                      const char*   col1,
                                      const char*   coln);
extern bool blr_is_current_binlog(ROUTER_INSTANCE* router,
                                  ROUTER_SLAVE* slave);
extern bool blr_compare_binlogs(const ROUTER_INSTANCE* router,
                                const MARIADB_GTID_ELEMS* info,
                                const char* r_file,
                                const char* s_file);
static bool blr_purge_binary_logs(ROUTER_INSTANCE* router,
                                  ROUTER_SLAVE* slave,
                                  char* purge_stmt);
static int binary_logs_find_file_cb(void* data,
                                    int   cols,
                                    char** values,
                                    char** names);
static void blr_log_config_changes(ROUTER_INSTANCE* router,
                                   const MasterServerConfig& current_master,
                                   const ChangeMasterConfig& change_master);
extern void blr_log_disabled_heartbeat(const ROUTER_INSTANCE* inst);
extern void blr_close_master_in_main(void* data);
static bool blr_check_connecting_slave(const ROUTER_INSTANCE* router,
                                       ROUTER_SLAVE* slave,
                                       enum blr_slave_check check);
static void blr_abort_change_master(ROUTER_INSTANCE* router,
                                    const MasterServerConfig& current_master,
                                    const char* error);
static void blr_slave_abort_dump_request(ROUTER_SLAVE* slave,
                                         const char*   errmsg);
static bool blr_binlog_change_check(const ROUTER_INSTANCE* router,
                                    const ChangeMasterConfig& new_config,
                                    char* error);
static bool blr_change_binlog_name(ROUTER_INSTANCE* router,
                                   const char* log_file,
                                   char** new_logfile,
                                   char*  error);
static bool blr_change_binlog_name(ROUTER_INSTANCE* router,
                                   const std::string& log_file,
                                   char** new_logfile,
                                   char*  error);
static bool blr_apply_changes(ROUTER_INSTANCE* router,
                              const ChangeMasterConfig& new_config,
                              char* new_logfile,
                              char* error);
static void blr_slave_info_save(const MARIADB_GTID_INFO* info,
                                MARIADB_GTID_INFO* save_info,
                                char* save_prefix);
static void blr_slave_log_next_file_action(const ROUTER_INSTANCE* router,
                                           const ROUTER_SLAVE* slave,
                                           const char* c_prefix,
                                           const char* next_file,
                                           slave_eof_action_t log_action);

/**
 * Process a request packet from the slave server.
 *
 * The router can handle a limited subset of requests from the slave, these
 * include a subset of general SQL queries, a slave registeration command and
 * the binlog dump command.
 *
 * The strategy for responding to these commands is to use caches responses
 * for the the same commands that have previously been made to the real master
 * if this is possible, if it is not then the router itself will synthesize a
 * response.
 *
 * @param router    The router instance: the master for this replication chain
 * @param slave     The slave specific data
 * @param queue     The incoming request packet
 */
int blr_slave_request(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, GWBUF* queue)
{
    int rv = 0;
    if (slave->state < 0 || slave->state > BLRS_MAXSTATE)
    {
        MXS_ERROR("Invalid slave state machine state (%d) for binlog router.",
                  slave->state);
        gwbuf_free(queue);
        return 0;
    }

    slave->stats.n_requests++;
    switch (MYSQL_COMMAND(queue))
    {
    case COM_QUERY:
        slave->stats.n_queries++;
        rv = blr_slave_query(router, slave, queue);
        break;

    case COM_REGISTER_SLAVE:
        /* Continue with slave registration */
        rv = blr_slave_register(router, slave, queue);
        break;

    case COM_BINLOG_DUMP:
        /* Check whether binlog server can accept slave requests */
        if (!blr_check_connecting_slave(router,
                                        slave,
                                        BLR_SLAVE_CONNECTING)
            ||      /* Check whether connecting slaves can be only MariaDB 10 ones */
            !blr_check_connecting_slave(router,
                                        slave,
                                        BLR_SLAVE_IS_MARIADB10)
            ||      /**
                     * If MariaDB 10 GTID master replication is set
                     * only MariaDB 10 GTID slaves can continue the registration.
                     */
            !blr_check_connecting_slave(router,
                                        slave,
                                        BLR_SLAVE_HAS_MARIADB10_GTID))
        {
            dcb_close(slave->dcb);
            return 1;
        }

        /* Request now the binlog records */
        rv = blr_slave_binlog_dump(router, slave, queue);

        /* Check whether to add the heartbeat check for this slave */
        if (rv && slave->state == BLRS_DUMPING
            && router->send_slave_heartbeat
            && slave->heartbeat > 0)
        {
            char task_name[BLRM_TASK_NAME_LEN + 1] = "";
            snprintf(task_name,
                     BLRM_TASK_NAME_LEN,
                     "%s slaves heartbeat send",
                     router->service->name);

            /* Add slave heartbeat check task with 1 second frequency */
            hktask_add(task_name, blr_send_slave_heartbeat, router, 1);
        }
        break;

    case COM_STATISTICS:
        rv = blr_statistics(router, slave, queue);
        break;

    case COM_PING:
        rv = blr_ping(router, slave, queue);
        break;

    case COM_QUIT:
        MXS_DEBUG("COM_QUIT received from slave with server_id %d",
                  slave->serverid);
        rv = 1;
        break;

    default:
        blr_send_custom_error(slave->dcb,
                              1,
                              0,
                              "You have an error in your SQL syntax; Check the "
                              "syntax the MaxScale binlog router accepts.",
                              "42000",
                              1064);
        MXS_ERROR("Unexpected MySQL Command (%d) received from slave",
                  MYSQL_COMMAND(queue));
        break;
    }

    return rv;
}

/*
 * Return a pointer to where the actual SQL query starts, skipping initial
 * comments and whitespace characters, if there are any.
 */
const char* blr_skip_leading_sql_comments(const char* sql_query)
{
    const char* p = sql_query;

    while (*p)
    {
        if (*p == '/' && p[1] == '*')
        {
            ++p;    // skip '/'
            ++p;    // skip '*'
            while (*p)
            {
                if (*p == '*' && p[1] == '/')
                {
                    ++p;    // skip '*'
                    ++p;    // skip '/'
                    break;
                }
                else
                {
                    ++p;
                }
            }
        }
        else if (isspace(*p))
        {
            ++p;
        }
        else
        {
            return p;
        }
    }
    return p;
}

/**
 * Handle a query from the slave. This is expected to be one of the "standard"
 * queries we expect as part of the registraton process. Most of these can
 * be dealt with by replying the stored responses we got from the master
 * when MaxScale registered as a slave. The exception to the rule is the
 * request to obtain the current timestamp value of the server.
 *
 * The original set added for the registration process has been enhanced in
 * order to support some commands that are useful for monitoring the binlog
 * router.
 *
 * 16 select statements are currently supported:
 *  SELECT UNIX_TIMESTAMP();
 *  SELECT @master_binlog_checksum
 *  SELECT @@GLOBAL.GTID_MODE
 *  SELECT VERSION()
 *  SELECT 1
 *  SELECT @@version_comment limit 1
 *  SELECT @@hostname
 *  SELECT @@max_allowed_packet
 *  SELECT @@maxscale_version
 *  SELECT @@[GLOBAL.]server_id
 *  SELECT @@version
 *  SELECT @@[GLOBAL.]server_uuid
 *  SELECT USER()
 *  SELECT @@GLOBAL.gtid_domain_id
 *  SELECT @@[GLOBAL].gtid_current_pos
 *  SELECT @@[global.]server_id, @@[global.]read_only
 *
 * 10 show commands are supported:
 *  SHOW [GLOBAL] VARIABLES LIKE 'SERVER_ID'
 *  SHOW [GLOBAL] VARIABLES LIKE 'SERVER_UUID'
 *  SHOW [GLOBAL] VARIABLES LIKE 'MAXSCALE%'
 *  SHOW SLAVE STATUS
 *  SHOW MASTER STATUS
 *  SHOW SLAVE HOSTS
 *  SHOW WARNINGS
 *  SHOW [GLOBAL] STATUS LIKE 'Uptime'
 *  SHOW [GLOBAL] STATUS LIKE 'slave_received_heartbeats'
 *  SHOW BINARY LOGS
 *
 * 13 set commands are supported:
 *  SET @master_binlog_checksum = @@global.binlog_checksum
 *  SET @master_heartbeat_period=...
 *  SET @slave_slave_uuid=...
 *  SET NAMES latin1
 *  SET NAMES utf8
 *  SET NAMES XXX
 *  SET mariadb_slave_capability=...
 *  SET autocommit=
 *  SET @@session.autocommit=
 *  SET @slave_connect_state=
 *  SET @slave_gtid_strict_mode=
 *  SET @slave_gtid_ignore_duplicates=
 *  SET SQL_MODE=''
 *
 * 4 administrative commands are supported:
 *  STOP SLAVE
 *  START SLAVE
 *  CHANGE MASTER TO
 *  RESET SLAVE
 *
 * @param router    The router instance
 * @param slave     The slave specific data
 * @param queue     The incoming request packet
 * @return          Non-zero if data has been sent
 */
static int blr_slave_query(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, GWBUF* queue)
{
    char* qtext, * query_text;
    const char* sep = "     ,=";
    char* word, * brkb;
    int query_len;
    char* ptr;
    bool unexpected = true;

    qtext = (char*)GWBUF_DATA(queue);
    query_len = extract_field((uint8_t*)qtext, 24) - 1;
    qtext += MYSQL_HEADER_LEN + 1;      // Skip header and first byte of the payload
    query_text = strndup(qtext, query_len);

    /* Don't log the full statement containg 'password', just trucate it */
    ptr = strcasestr(query_text, "password");
    if (ptr != NULL)
    {
        char* new_text = MXS_STRDUP_A(query_text);
        int trucate_at = (ptr - query_text);
        if (trucate_at > 0)
        {
            if ((trucate_at + 3) <= (int)strlen(new_text))
            {
                int i;
                for (i = 0; i < 3; i++)
                {
                    new_text[trucate_at + i] = '.';
                }
                new_text[trucate_at + 3] = '\0';
            }
            else
            {
                new_text[trucate_at] = '\0';
            }
        }

        MXS_INFO("Execute statement (truncated, it contains password)"
                 " from the slave '%s'",
                 new_text);
        MXS_FREE(new_text);
    }
    else
    {
        MXS_INFO("Execute statement from the slave '%s'", query_text);
    }

    /*
     * Implement a very rudimental "parsing" of the query text by extarcting the
     * words from the statement and matchng them against the subset of queries we
     * are expecting from the slave. We already have responses to these commands,
     * except for the select of UNIX_TIMESTAMP(), that we have saved from MaxScale's
     * own interaction with the real master. We simply replay these saved responses
     * to the slave.
     */

    /*  - 1 - Check and handle possible Maxwell input statement */
    if (blr_handle_maxwell_stmt(router,
                                slave,
                                query_text))
    {
        MXS_FREE(query_text);
        return 1;
    }   /* - 2 - Handle SELECT, SET, SHOW and Admin commands */
    else if ((word = strtok_r(query_text, sep, &brkb)) == NULL)
    {
        MXS_ERROR("%s: Incomplete query.", router->service->name);
    }
    else if (strcasecmp(word, "SELECT") == 0)
    {
        /* Handle SELECT */
        if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Incomplete select query.", router->service->name);
        }
        else
        {
            if (brkb && strlen(brkb)
                && blr_handle_complex_select(router,
                                             slave,
                                             word,
                                             brkb))
            {
                MXS_FREE(query_text);
                return 1;
            }

            if (blr_handle_simple_select_stmt(router,
                                              slave,
                                              word))
            {
                MXS_FREE(query_text);
                return 1;
            }
            else
            {
                /* Handle a special case */
                unexpected = strcasestr(word, "binlog_gtid_pos") == NULL;
            }
        }
    }
    else if (strcasecmp(word, "SHOW") == 0)
    {
        /* Handle SHOW */
        if (blr_handle_show_stmt(router,
                                 slave,
                                 brkb))
        {
            MXS_FREE(query_text);
            return 1;
        }
    }
    else if (strcasecmp(query_text, "SET") == 0)
    {
        /* Handle SET */
        if (blr_handle_set_stmt(router,
                                slave,
                                brkb))
        {
            MXS_FREE(query_text);
            return 1;
        }
    }
    else
    {
        /* Handle ADMIN commands */
        if (blr_handle_admin_stmt(router,
                                  slave,
                                  word,
                                  brkb))
        {
            MXS_FREE(query_text);
            return 1;
        }
    }

    /* - 3 - Handle unsuppored statements from client */
    MXS_FREE(query_text);

    query_text = strndup(qtext, query_len);

    if (unexpected)
    {
        MXS_ERROR("Unexpected query from '%s'@'%s': %s",
                  slave->dcb->user,
                  slave->dcb->remote,
                  query_text);
    }
    else
    {
        MXS_INFO("Unexpected query from '%s'@'%s', possibly a 10.1 slave: %s",
                 slave->dcb->user,
                 slave->dcb->remote,
                 query_text);
    }

    MXS_FREE(query_text);
    blr_slave_send_error(router,
                         slave,
                         "You have an error in your SQL syntax; Check the syntax "
                         "the MaxScale binlog router accepts.");
    return 1;
}


/**
 * Send a reply to a command we have received from the slave. The reply itself
 * is merely a copy of a previous message we received from the master when we
 * registered as a slave. Hence we just replay this saved reply.
 *
 * @param   router    The binlog router instance
 * @param   slave     The slave server to which we are sending the response
 * @param   master    The saved master response
 * @return            Non-zero if data was sent
 */
static int blr_slave_replay(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, GWBUF* master)
{
    if (router->master_state == BLRM_UNCONFIGURED)
    {
        return blr_slave_send_ok(router, slave);
    }

    if (!master)
    {
        return 0;
    }

    /**
     * Note: The following manipulation of the buffers bypasses the thread-local checks that are present
     * to prevent cross-thread usage of buffers. As the binlogrouter doesn't care about that, we have to
     * avoid the checks in order to prevent debug assertions. This is definitely not something that should
     * be done and a better solution would be to just store the data inside a vector and protect access with
     * a lock.
     */
    size_t len = 0;

    for (GWBUF* b = master; b; b = b->next)
    {
        len += GWBUF_LENGTH(b);
    }

    GWBUF* clone = gwbuf_alloc(len);

    if (clone)
    {
        uint8_t* data = GWBUF_DATA(clone);

        for (GWBUF* b = master; b; b = b->next)
        {
            memcpy(data, GWBUF_DATA(b), GWBUF_LENGTH(b));
            data += GWBUF_LENGTH(b);
        }

        return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, clone);
    }
    else
    {
        MXS_ERROR("Failed to clone server response to send to slave.");
        return 0;
    }
}

/**
 * Construct an error response
 *
 * @param router    The router instance
 * @param slave     The slave server instance
 * @param msg       The error message to send
 */
static void blr_slave_send_error(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, const char* msg)
{
    GWBUF* pkt;
    unsigned char* data;
    int len;

    if ((pkt = gwbuf_alloc(strlen(msg) + 13)) == NULL)
    {
        return;
    }
    data = GWBUF_DATA(pkt);
    len = strlen(msg) + 9;
    encode_value(&data[0], len, 24);// Payload length
    data[3] = 1;                    // Sequence id
    // Payload
    data[4] = 0xff;                     // Error indicator
    encode_value(&data[5], 1064, 16);   // Error Code
    memcpy((char*)&data[7], "#42000", 6);
    memcpy(&data[13], msg, strlen(msg));    // Error Message
    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}

/*
 * Some standard packets that have been captured from a network trace of server
 * interactions. These packets are the schema definition sent in response to
 * a SELECT UNIX_TIMESTAMP() statement and the EOF packet that marks the end
 * of transmission of the result set.
 */
static uint8_t timestamp_def[] =
{
    0x01, 0x00, 0x00, 0x01, 0x01, 0x26, 0x00, 0x00, 0x02, 0x03, 0x64, 0x65,
    0x66, 0x00, 0x00, 0x00, 0x10, 0x55, 0x4e, 0x49, 0x58, 0x5f, 0x54, 0x49,
    0x4d, 0x45, 0x53, 0x54, 0x41, 0x4d, 0x50, 0x28, 0x29, 0x00, 0x0c, 0x3f,
    0x00, 0x0b, 0x00, 0x00, 0x00, 0x08, 0x81, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x03, 0xfe, 0x00, 0x00, 0x02, 0x00
};
static uint8_t timestamp_eof[] = {0x05, 0x00, 0x00, 0x05,
                                  0xfe, 0x00, 0x00, 0x02, 0x00};

/**
 * Send a response to a "SELECT UNIX_TIMESTAMP()" request.
 * This differs from the other
 * requests since we do not save a copy of the original interaction
 * with the master and simply replay it.
 * We want to always send the current time. We have stored a typcial
 * response, which gives us the schema information normally returned.
 * This is sent to the * client and then we add a dynamic part that will
 * insert the current timestamp data.
 * Finally we send a preprepaed EOF packet to end the response stream.
 *
 * @param   router    The binlog router instance
 * @param   slave     The slave server to which we are sending the response
 * @return            Non-zero if data was sent
 */
static int blr_slave_send_timestamp(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    GWBUF* pkt;
    char timestamp[20];
    uint8_t* ptr;
    int len, ts_len;

    sprintf(timestamp, "%ld", time(0));
    ts_len = strlen(timestamp);
    len = sizeof(timestamp_def) + sizeof(timestamp_eof)
        + MYSQL_HEADER_LEN + 1 + ts_len;
    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    memcpy(ptr, timestamp_def, sizeof(timestamp_def));      // Fixed preamble
    ptr += sizeof(timestamp_def);
    encode_value(ptr, ts_len + 1, 24);      // Add length of data packet
    ptr += 3;
    *ptr++ = 0x04;                          // Sequence number in response
    *ptr++ = ts_len;                        // Length of result string
    memcpy((char*)ptr, timestamp, ts_len);  // Result string
    ptr += ts_len;
    // EOF packet to terminate result
    memcpy(ptr, timestamp_eof, sizeof(timestamp_eof));
    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}

/**
 * Send a response the the SQL command SELECT @@MAXSCALE_VERSION
 *
 * @param   router      The binlog router instance
 * @param   slave       The slave server to which we are sending the response
 * @return              Non-zero if data was sent
 */
static int blr_slave_send_maxscale_version(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    GWBUF* pkt;
    char version[80] = "";
    uint8_t* ptr;
    int len, vers_len;

    sprintf(version, "%s", MAXSCALE_VERSION);
    vers_len = strlen(version);
    blr_slave_send_fieldcount(router, slave, 1);
    blr_slave_send_columndef(router,
                             slave,
                             "MAXSCALE_VERSION",
                             BLR_TYPE_STRING,
                             vers_len,
                             2);
    blr_slave_send_eof(router, slave, 3);

    len = MYSQL_HEADER_LEN + 1 + vers_len;
    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, vers_len + 1, 24);        // Add length of data packet
    ptr += 3;
    *ptr++ = 0x04;                              // Sequence number in response
    *ptr++ = vers_len;                          // Length of result string
    memcpy((char*)ptr, version, vers_len);      // Result string
    /*  ptr += vers_len;  Not required unless more data is to be added */
    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
    return blr_slave_send_eof(router, slave, 5);
}

/**
 * Send a response the the SQL command SELECT @@server_id
 *
 * @param   router    The binlog router instance
 * @param   slave     The slave server to which we are sending the response
 * @return            Non-zero if data was sent
 */
static int blr_slave_send_server_id(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    GWBUF* pkt;
    char server_id[40];
    uint8_t* ptr;
    int len, id_len;

    sprintf(server_id, "%d", router->masterid);
    id_len = strlen(server_id);
    blr_slave_send_fieldcount(router, slave, 1);
    blr_slave_send_columndef(router,
                             slave,
                             "SERVER_ID",
                             BLR_TYPE_INT,
                             id_len,
                             2);
    blr_slave_send_eof(router, slave, 3);

    len = MYSQL_HEADER_LEN + 1 + id_len;
    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, id_len + 1, 24);      // Add length of data packet
    ptr += 3;
    *ptr++ = 0x04;                          // Sequence number in response
    *ptr++ = id_len;                        // Length of result string
    memcpy((char*)ptr, server_id, id_len);  // Result string
    /* ptr += id_len; Not required unless more data is to be added */
    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
    return blr_slave_send_eof(router, slave, 5);
}


/**
 * Send the response to the SQL command "SHOW VARIABLES LIKE 'MAXSCALE%'
 *
 * @param   router      The binlog router instance
 * @param   slave       The slave server to which we are sending the response
 * @return              Non-zero if data was sent
 */
static int blr_slave_send_maxscale_variables(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    GWBUF* pkt;
    char name[40];
    char version[80];
    uint8_t* ptr;
    int len, vers_len, seqno = 2;

    blr_slave_send_fieldcount(router, slave, 2);
    blr_slave_send_columndef(router,
                             slave,
                             "Variable_name",
                             BLR_TYPE_STRING,
                             40,
                             seqno++);
    blr_slave_send_columndef(router,
                             slave,
                             "Value",
                             BLR_TYPE_STRING,
                             40,
                             seqno++);
    blr_slave_send_eof(router, slave, seqno++);

    sprintf(version, "%s", MAXSCALE_VERSION);
    vers_len = strlen(version);
    strcpy(name, "MAXSCALE_VERSION");
    len = MYSQL_HEADER_LEN + 1 + vers_len + strlen(name) + 1;
    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    // Add length of data packet
    encode_value(ptr, vers_len + 2 + strlen(name), 24);
    ptr += 3;
    *ptr++ = seqno++;                           // Sequence number in response
    *ptr++ = strlen(name);                      // Length of result string
    memcpy((char*)ptr, name, strlen(name));     // Result string
    ptr += strlen(name);
    *ptr++ = vers_len;                          // Length of result string
    memcpy((char*)ptr, version, vers_len);      // Result string
    /* ptr += vers_len; Not required unless more data is to be added */
    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
    return blr_slave_send_eof(router, slave, seqno++);
}


/**
 * Send the response to the SQL command "SHOW MASTER STATUS"
 *
 * @param   router      The binlog router instance
 * @param   slave       The slave server to which we are sending the response
 * @return              Non-zero if data was sent
 */
static int blr_slave_send_master_status(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    GWBUF* pkt;
    char file[BINLOG_FNAMELEN + 1];
    char position[BINLOG_FNAMELEN + 1];
    uint8_t* ptr;
    int len, file_len;

    blr_slave_send_fieldcount(router, slave, 5);
    blr_slave_send_columndef(router,
                             slave,
                             "File",
                             BLR_TYPE_STRING,
                             40,
                             2);
    blr_slave_send_columndef(router,
                             slave,
                             "Position",
                             BLR_TYPE_STRING,
                             40,
                             3);
    blr_slave_send_columndef(router,
                             slave,
                             "Binlog_Do_DB",
                             BLR_TYPE_STRING,
                             40,
                             4);
    blr_slave_send_columndef(router,
                             slave,
                             "Binlog_Ignore_DB",
                             BLR_TYPE_STRING,
                             40,
                             5);
    blr_slave_send_columndef(router,
                             slave,
                             "Execute_Gtid_Set",
                             BLR_TYPE_STRING,
                             40,
                             6);
    blr_slave_send_eof(router, slave, 7);

    snprintf(file, sizeof(file), "%s", router->binlog_name);
    file_len = strlen(file);

    snprintf(position, sizeof(position), "%lu", router->binlog_position);

    len = MYSQL_HEADER_LEN + 1 + file_len + strlen(position) + 1 + 3;
    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    // Add length of data packet
    encode_value(ptr, len - MYSQL_HEADER_LEN, 24);
    ptr += 3;
    *ptr++ = 0x08;                              // Sequence number in response
    *ptr++ = strlen(file);                      // Length of result string
    memcpy((char*)ptr, file, strlen(file));     // Result string
    ptr += strlen(file);
    *ptr++ = strlen(position);                  // Length of result string
    // Result string
    memcpy((char*)ptr, position, strlen(position));
    ptr += strlen(position);
    *ptr++ = 0;     // Send 3 empty values
    *ptr++ = 0;
    *ptr++ = 0;
    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
    return blr_slave_send_eof(router, slave, 9);
}

/*
 * Columns to send for GTID in "SHOW SLAVE STATUS" command
 */
static const char* slave_status_columns[] =
{
    "Slave_IO_State",
    "Master_Host",
    "Master_User",
    "Master_Port",
    "Connect_Retry",
    "Master_Log_File",
    "Read_Master_Log_Pos",
    "Relay_Log_File",
    "Relay_Log_Pos",
    "Relay_Master_Log_File",
    "Slave_IO_Running",
    "Slave_SQL_Running",
    "Replicate_Do_DB",
    "Replicate_Ignore_DB",
    "Replicate_Do_Table",
    "Replicate_Ignore_Table",
    "Replicate_Wild_Do_Table",
    "Replicate_Wild_Ignore_Table",
    "Last_Errno",
    "Last_Error",
    "Skip_Counter",
    "Exec_Master_Log_Pos",
    "Relay_Log_Space",
    "Until_Condition",
    "Until_Log_File",
    "Until_Log_Pos",
    "Master_SSL_Allowed",
    "Master_SSL_CA_File",
    "Master_SSL_CA_Path",
    "Master_SSL_Cert",
    "Master_SSL_Cipher",
    "Master_SSL_Key",
    "Seconds_Behind_Master",
    "Master_SSL_Verify_Server_Cert",
    "Last_IO_Errno",
    "Last_IO_Error",
    "Last_SQL_Errno",
    "Last_SQL_Error",
    "Replicate_Ignore_Server_Ids",
    "Master_Server_Id",
    "Master_UUID",
    "Master_Info_File",
    "SQL_Delay",
    "SQL_Remaining_Delay",
    "Slave_SQL_Running_State",
    "Master_Retry_Count",
    "Master_Bind",
    "Last_IO_Error_TimeStamp",
    "Last_SQL_Error_Timestamp",
    "Master_SSL_Crl",
    "Master_SSL_Crlpath",
    NULL
};

/*
 * New columns to send for GTID in "SHOW ALL SLAVES STATUS" command
 */
static const char* all_slaves_status_columns[] =
{
    "Connection_name",
    "Slave_SQL_State",
    NULL
};

/*
 * Columns to send for GTID in "SHOW SLAVE STATUS" MySQL 5.6/7 command
 */
static const char* mysql_gtid_status_columns[] =
{
    "Retrieved_Gtid_Set",
    "Executed_Gtid_Set",
    "Auto_Position",
    NULL
};

/*
 * Columns to send for GTID in "SHOW SLAVE STATUS" MariaDB 10 command
 * and SHOW ALL SLAVES STATUS as well
 */
static const char* mariadb10_gtid_status_columns[] =
{
    "Using_Gtid",
    "Gtid_IO_Pos",
    NULL
};

/*
 * Extra Columns to send in "SHOW ALL SLAVES STATUS" MariaDB 10 command
 */
static const char* mariadb10_extra_status_columns[] =
{
    "Retried_transactions",
    "Max_relay_log_size",
    "Executed_log_entries",
    "Slave_received_heartbeats",
    "Slave_heartbeat_period",
    "Gtid_Slave_Pos",
    NULL
};

/**
 * Send the response to the SQL command "SHOW SLAVE STATUS" or
 * SHOW ALL SLAVES STATUS
 *
 * @param   router        The binlog router instance
 * @param   slave         The slave server to which we are sending the response
 * @param   all_slaves    Whether to use SHOW ALL SLAVES STATUS
 * @return                Non-zero if data was sent
 */
static int blr_slave_send_slave_status(ROUTER_INSTANCE* router,
                                       ROUTER_SLAVE* slave,
                                       bool all_slaves)
{
    GWBUF* pkt;
    char column[2048] = "";
    uint8_t* ptr;
    int len, actual_len, col_len, seqno, i;
    char* dyn_column = NULL;
    int max_column_size = sizeof(column);
    int ncols = 0;
    int gtid_cols = 0;

    /* Count SHOW SLAVE STATUS the columns */
    ncols += MXS_ARRAY_NELEMS(slave_status_columns) - 1;

    /* Add the new SHOW ALL SLAVES STATUS columns */
    if (all_slaves)
    {
        ncols += MXS_ARRAY_NELEMS(all_slaves_status_columns) - 1;
        ncols += MXS_ARRAY_NELEMS(mariadb10_extra_status_columns) - 1;
    }

    /* Get the right GTID columns array */
    const char** gtid_status_columns = router->mariadb10_gtid ?
        mariadb10_gtid_status_columns :
        mysql_gtid_status_columns;
    /* Increment ncols with the right GTID columns */
    while (gtid_status_columns[gtid_cols++])
    {
        ncols++;
    }

    /* Send number of columns */
    blr_slave_send_fieldcount(router, slave, ncols);

    seqno = 2;
    if (all_slaves)
    {
        /* Send first the column definitions for the all_slaves */
        for (i = 0; all_slaves_status_columns[i]; i++)
        {
            blr_slave_send_columndef(router,
                                     slave,
                                     all_slaves_status_columns[i],
                                     BLR_TYPE_STRING,
                                     40,
                                     seqno++);
        }
    }

    /* Now send column definitions for slave status */
    for (i = 0; slave_status_columns[i]; i++)
    {
        blr_slave_send_columndef(router,
                                 slave,
                                 slave_status_columns[i],
                                 BLR_TYPE_STRING,
                                 40,
                                 seqno++);
    }

    /* Send MariaDB 10 or MySQL 5.6/7 GTID columns */
    for (i = 0; gtid_status_columns[i]; i++)
    {
        blr_slave_send_columndef(router,
                                 slave,
                                 gtid_status_columns[i],
                                 BLR_TYPE_STRING,
                                 40,
                                 seqno++);
    }

    /* Send extra columns for SHOW ALL SLAVES STATUS */
    if (all_slaves)
    {
        for (i = 0; mariadb10_extra_status_columns[i]; i++)
        {
            blr_slave_send_columndef(router,
                                     slave,
                                     mariadb10_extra_status_columns[i],
                                     BLR_TYPE_STRING,
                                     40,
                                     seqno++);
        }
    }

    /* Send EOF for columns def */
    blr_slave_send_eof(router, slave, seqno++);

    // Max length + 250 bytes error message
    len = MYSQL_HEADER_LEN + 1 + ncols * max_column_size + 250;

    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    // Add length of data packet
    encode_value(ptr, len - MYSQL_HEADER_LEN, 24);
    ptr += 3;
    // Sequence number in response
    *ptr++ = seqno++;

    if (all_slaves)
    {
        for (i = 0; all_slaves_status_columns[i]; i++)
        {
            *ptr++ = 0;     // Empty value
        }
    }

    // Slave_IO_State
    snprintf(column,
             max_column_size,
             "%s",
             blrm_states[router->master_state]);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    // Master_Host
    snprintf(column,
             max_column_size,
             "%s",
             router->service->dbref->server->address ?
             router->service->dbref->server->address :
             "");
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    // Master_User
    snprintf(column,
             max_column_size,
             "%s",
             router->user ? router->user : "");
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    // Master_Port
    sprintf(column, "%d", router->service->dbref->server->port);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    sprintf(column, "%d", router->retry_interval);      // Connect retry
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    snprintf(column, max_column_size, "%s", router->binlog_name);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    /* if router->trx_safe report current_pos*/
    if (router->trx_safe)
    {
        sprintf(column, "%lu", router->current_pos);
    }
    else
    {
        sprintf(column, "%lu", router->binlog_position);
    }

    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    /* We have no relay log, we relay the binlog, so we will send the same data */
    snprintf(column, max_column_size, "%s", router->binlog_name);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    sprintf(column, "%ld", router->binlog_position);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    /* We have no relay log, we relay the binlog, so we will send the same data */
    snprintf(column, max_column_size, "%s", router->binlog_name);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    if (router->master_state != BLRM_SLAVE_STOPPED)
    {
        if (router->master_state < BLRM_BINLOGDUMP)
        {
            strcpy(column, "Connecting");
        }
        else
        {
            strcpy(column, "Yes");
        }
    }
    else
    {
        strcpy(column, "No");
    }
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    if (router->master_state != BLRM_SLAVE_STOPPED)
    {
        strcpy(column, "Yes");
    }
    else
    {
        strcpy(column, "No");
    }
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    *ptr++ = 0;                                 // Send 6 empty values
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;

    /* Last error information */
    sprintf(column, "%lu", router->m_errno);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    /* Last error message */
    if (router->m_errmsg == NULL)
    {
        *ptr++ = 0;
    }
    else
    {
        dyn_column = (char*)router->m_errmsg;
        col_len = strlen(dyn_column);
        if (col_len > 250)
        {
            col_len = 250;
        }
        *ptr++ = col_len;                           // Length of result string
        memcpy((char*)ptr, dyn_column, col_len);    // Result string
        ptr += col_len;
    }

    /* Skip_Counter */
    sprintf(column, "%d", 0);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    sprintf(column, "%ld", router->binlog_position);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    sprintf(column, "%ld", router->binlog_position);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    strcpy(column, "None");
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    *ptr++ = 0;

    /* Until_Log_Pos */
    sprintf(column, "%d", 0);
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    /* Master_SSL_Allowed */
    if (router->ssl_enabled)
    {
        strcpy(column, "Yes");
    }
    else
    {
        strcpy(column, "No");
    }
    col_len = strlen(column);
    *ptr++ = col_len;                           // Length of result string
    memcpy((char*)ptr, column, col_len);        // Result string
    ptr += col_len;

    /* Check whether to report SSL master connection details */
    if (router->ssl_ca && router->ssl_cert && router->ssl_key)
    {
        char big_column[250 + 1] = "";

        // set Master_SSL_Cert
        strncpy(big_column, router->ssl_ca, 250);
        col_len = strlen(big_column);
        *ptr++ = col_len;                       // Length of result string
        memcpy((char*)ptr, big_column, col_len);// Result string
        ptr += col_len;

        *ptr++ = 0;                 // Empty Master_SSL_CA_Path column

        // set Master_SSL_Cert
        strncpy(big_column, router->ssl_cert, 250);
        col_len = strlen(big_column);
        *ptr++ = col_len;                       // Length of result string
        memcpy((char*)ptr, big_column, col_len);// Result string
        ptr += col_len;

        *ptr++ = 0;                 // Empty Master_SSL_Cipher column

        // set Master_SSL_Key
        strncpy(big_column, router->ssl_key, 250);
        col_len = strlen(big_column);
        *ptr++ = col_len;                       // Length of result string
        memcpy((char*)ptr, big_column, col_len);// Result string
        ptr += col_len;
    }
    else
    {
        *ptr++ = 0;                 // Empty SSL columns
        *ptr++ = 0;
        *ptr++ = 0;
        *ptr++ = 0;
        *ptr++ = 0;
    }

    /* Seconds_Behind_Master */
    sprintf(column, "%d", 0);
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    /* Master_SSL_Verify_Server_Cert */
    strcpy(column, "No");
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    /* Last_IO_Error */
    sprintf(column, "%d", 0);
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    *ptr++ = 0;

    /* Last_SQL_Error */
    sprintf(column, "%d", 0);
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    *ptr++ = 0;
    *ptr++ = 0;

    /* Master_Server_Id */
    sprintf(column, "%d", router->orig_masterid);
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    /* Master_server_UUID */
    snprintf(column,
             max_column_size,
             "%s",
             router->master_uuid ?
             router->master_uuid : router->uuid);
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    /* Master_info_file */
    snprintf(column, max_column_size, "%s/master.ini", router->binlogdir);
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    /* SQL_Delay*/
    sprintf(column, "%d", 0);
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    *ptr++ = 0xfb;                          // NULL value

    /* Slave_Running_State */
    if (router->master_state == BLRM_SLAVE_STOPPED)
    {
        strcpy(column, "Slave stopped");
    }
    else if (!router->m_errno)
    {
        strcpy(column, "Slave running");
    }
    else
    {
        if (router->master_state < BLRM_BINLOGDUMP)
        {
            strcpy(column, "Registering");
        }
        else
        {
            strcpy(column, "Error");
        }
    }
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    /* Master_Retry_Count */
    sprintf(column, "%d", router->retry_limit);
    col_len = strlen(column);
    *ptr++ = col_len;                       // Length of result string
    memcpy((char*)ptr, column, col_len);    // Result string
    ptr += col_len;

    *ptr++ = 0;                             // Send 5 empty values
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;

    if (!router->mariadb10_gtid)
    {
        // No GTID support send empty values
        *ptr++ = 0;
        *ptr++ = 0;
        *ptr++ = 0;
    }
    else
    {
        // MariaDB 10 GTID
        // 1 - Add "Using_Gtid"
        sprintf(column,
                "%s",
                router->mariadb10_master_gtid ?
                "Slave_pos" :
                "No");
        col_len = strlen(column);
        *ptr++ = col_len;               // Length of result string
        memcpy(ptr, column, col_len);   // Result string
        ptr += col_len;

        // 2 - Add "Gtid_IO_Pos"
        sprintf(column,
                "%s",
                router->last_mariadb_gtid);
        col_len = strlen(column);
        *ptr++ = col_len;               // Length of result string
        memcpy(ptr, column, col_len);   // Result string
        ptr += col_len;
    }

    if (all_slaves)
    {
        // Retried_transactions
        sprintf(column, "%d", 0);
        col_len = strlen(column);
        *ptr++ = col_len;                       // Length of result string
        memcpy((char*)ptr, column, col_len);    // Result string
        ptr += col_len;

        *ptr++ = 0;     // Max_relay_log_size
        *ptr++ = 0;     // Executed_log_entries

        // Slave_received_heartbeats
        sprintf(column, "%d", router->stats.n_heartbeats);
        col_len = strlen(column);
        *ptr++ = col_len;                       // Length of result string
        memcpy((char*)ptr, column, col_len);    // Result string
        ptr += col_len;

        // Slave_heartbeat_period
        sprintf(column, "%lu", router->heartbeat);
        col_len = strlen(column);
        *ptr++ = col_len;                       // Length of result string
        memcpy((char*)ptr, column, col_len);    // Result string
        ptr += col_len;

        // Gtid_Slave_Pos
        if (!router->mariadb10_gtid)
        {
            // No GTID support send empty values
            *ptr++ = 0;
        }
        else
        {
            sprintf(column,
                    "%s",
                    router->last_mariadb_gtid);
            col_len = strlen(column);
            *ptr++ = col_len;               // Length of result string
            memcpy(ptr, column, col_len);   // Result string
            ptr += col_len;
        }
    }

    *ptr++ = 0;

    actual_len = ptr - (uint8_t*)GWBUF_DATA(pkt);
    ptr = GWBUF_DATA(pkt);
    // Add length of data packet
    encode_value(ptr, actual_len - MYSQL_HEADER_LEN, 24);

    // Trim the buffer to the actual size
    pkt = gwbuf_rtrim(pkt, len - actual_len);

    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
    return blr_slave_send_eof(router, slave, seqno++);
}

/**
 * Send the response to the SQL command "SHOW SLAVE HOSTS"
 *
 * @param    router    The binlog router instance
 * @param    slave     The connected slave server
 * @return             Non-zero if data was sent
 */
static int blr_slave_send_slave_hosts(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    GWBUF* pkt;
    char server_id[40];
    char host[40];
    char port[40];
    char master_id[40];
    char slave_uuid[40];
    uint8_t* ptr;
    int len, seqno;
    ROUTER_SLAVE* sptr;

    blr_slave_send_fieldcount(router, slave, 5);
    blr_slave_send_columndef(router, slave, "Server_id", BLR_TYPE_STRING, 40, 2);
    blr_slave_send_columndef(router, slave, "Host", BLR_TYPE_STRING, 40, 3);
    blr_slave_send_columndef(router, slave, "Port", BLR_TYPE_STRING, 40, 4);
    blr_slave_send_columndef(router, slave, "Master_id", BLR_TYPE_STRING, 40, 5);
    blr_slave_send_columndef(router, slave, "Slave_UUID", BLR_TYPE_STRING, 40, 6);
    blr_slave_send_eof(router, slave, 7);

    seqno = 8;
    pthread_mutex_lock(&router->lock);
    sptr = router->slaves;
    while (sptr)
    {
        if (sptr->state == BLRS_DUMPING || sptr->state == BLRS_REGISTERED)
        {
            sprintf(server_id, "%d", sptr->serverid);
            sprintf(host, "%s", sptr->hostname ? sptr->hostname : "");
            sprintf(port, "%d", sptr->port);
            sprintf(master_id, "%d", router->serverid);
            sprintf(slave_uuid, "%s", sptr->uuid ? sptr->uuid : "");
            len = MYSQL_HEADER_LEN + strlen(server_id) + strlen(host) + strlen(port)
                + strlen(master_id) + strlen(slave_uuid) + 5;
            if ((pkt = gwbuf_alloc(len)) == NULL)
            {
                return 0;
            }
            ptr = GWBUF_DATA(pkt);
            encode_value(ptr, len - MYSQL_HEADER_LEN, 24);      // Add length of data packet
            ptr += 3;
            *ptr++ = seqno++;                                   // Sequence number in response
            *ptr++ = strlen(server_id);                         // Length of result string
            memcpy((char*)ptr, server_id, strlen(server_id));   // Result string
            ptr += strlen(server_id);
            *ptr++ = strlen(host);                      // Length of result string
            memcpy((char*)ptr, host, strlen(host));     // Result string
            ptr += strlen(host);
            *ptr++ = strlen(port);                      // Length of result string
            memcpy((char*)ptr, port, strlen(port));     // Result string
            ptr += strlen(port);
            *ptr++ = strlen(master_id);                         // Length of result string
            memcpy((char*)ptr, master_id, strlen(master_id));   // Result string
            ptr += strlen(master_id);
            *ptr++ = strlen(slave_uuid);                        // Length of result string
            memcpy((char*)ptr, slave_uuid, strlen(slave_uuid)); // Result string
            ptr += strlen(slave_uuid);
            MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
        }
        sptr = sptr->next;
    }
    pthread_mutex_unlock(&router->lock);
    return blr_slave_send_eof(router, slave, seqno);
}

/**
 * Process a slave replication registration message.
 *
 * We store the various bits of information the slave gives us and generate
 * a reply message: OK packet.
 *
 * @param   router      The router instance
 * @param   slave       The slave server
 * @param   queue       The BINLOG_DUMP packet
 * @return              Non-zero if data was sent
 */
static int blr_slave_register(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, GWBUF* queue)
{
    uint8_t* ptr;
    int slen;

    ptr = GWBUF_DATA(queue);
    ptr += 4;       // Skip length and sequence number
    if (*ptr++ != COM_REGISTER_SLAVE)
    {
        return 0;
    }
    slave->serverid = extract_field(ptr, 32);
    ptr += 4;
    slen = *ptr++;
    if (slen != 0)
    {
        slave->hostname = strndup((char*)ptr, slen);
        ptr += slen;
    }
    else
    {
        slave->hostname = NULL;
    }
    slen = *ptr++;
    if (slen != 0)
    {
        ptr += slen;
        slave->user = strndup((char*)ptr, slen);
    }
    else
    {
        slave->user = NULL;
    }
    slen = *ptr++;
    if (slen != 0)
    {
        slave->passwd = strndup((char*)ptr, slen);
        ptr += slen;
    }
    else
    {
        slave->passwd = NULL;
    }
    slave->port = extract_field(ptr, 16);
    ptr += 2;
    slave->rank = extract_field(ptr, 32);

    slave->state = BLRS_REGISTERED;

    /*
     * Send OK response
     */
    return blr_slave_send_ok(router, slave);
}

/**
 * Process a COM_BINLOG_DUMP message from the slave. This is the
 * final step in the process of registration. The new master, MaxScale
 * must send a response packet and generate a fake BINLOG_ROTATE event
 * with the binlog file requested by the slave. And then send a
 * FORMAT_DESCRIPTION_EVENT that has been saved from the real master.
 *
 * Once send MaxScale must continue to send binlog events to the slave.
 *
 * @param   router      The router instance
 * @param   slave       The slave server
 * @param   queue       The BINLOG_DUMP packet
 * @return          The number of bytes written to the slave
 */
static int blr_slave_binlog_dump(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, GWBUF* queue)
{
    GWBUF* resp;
    uint8_t* ptr;
    int len, binlognamelen;
    REP_HEADER hdr;
    uint32_t chksum;
    uint32_t fde_end_pos;
    uint32_t requested_pos;

    ptr = GWBUF_DATA(queue);
    len = extract_field(ptr, 24);
    binlognamelen = len - 11;

    ptr += 4;       // Skip length and sequence number
    if (*ptr++ != COM_BINLOG_DUMP)
    {
        MXS_ERROR("blr_slave_binlog_dump expected a COM_BINLOG_DUMP but received %d",
                  *(ptr - 1));
        slave->state = BLRS_ERRORED;
        dcb_close(slave->dcb);
        return 1;
    }

    /* Get the current router binlog file */
    pthread_mutex_lock(&router->binlog_lock);
    strcpy(slave->binlog_name, router->binlog_name);
    pthread_mutex_unlock(&router->binlog_lock);

    /* Set the safe pos */
    slave->binlog_pos = 4;

    /* Get the requested pos from packet */
    requested_pos = extract_field(ptr, 32);

    /* Go ahead: after 4 bytes pos, 2 bytes flag and 4 bytes serverid */
    ptr += 4;

    uint16_t flags = gw_mysql_get_byte2(ptr);
    /* Check whether connected slave is asking for ANNOTATE_ROWS events */
    if (flags & BLR_REQUEST_ANNOTATE_ROWS_EVENT)
    {
        slave->annotate_rows = true;
        MXS_INFO("Registering slave (server-id %d) asks "
                 "for ANNOTATE_ROWS events.",
                 slave->serverid);
    }

    /* Go ahead: after 2 bytes flags and 4 bytes serverid */
    ptr += 2;
    ptr += 4;

    /* ptr now points to requested filename, if present */
    if (binlognamelen)
    {
        if (binlognamelen > BINLOG_FNAMELEN)
        {
            /* Abort the request */
            char req_file[binlognamelen + 1];
            char errmsg[BINLOG_ERROR_MSG_LEN + 1];
            memcpy(req_file, (char*)ptr, binlognamelen);
            req_file[binlognamelen] = 0;

            snprintf(errmsg,
                     BINLOG_ERROR_MSG_LEN,
                     "Requested filename %s is longer than max %d chars.",
                     req_file,
                     BINLOG_FNAMELEN);
            errmsg[BINLOG_ERROR_MSG_LEN] = '\0';

            // ERROR
            slave->seqno++;
            blr_slave_abort_dump_request(slave, errmsg);

            slave->state = BLRS_ERRORED;
            dcb_close(slave->dcb);
            return 1;
        }

        /* Set the received filename from packet: it could be changed later */
        memcpy(slave->binlog_name, (char*)ptr, binlognamelen);
        slave->binlog_name[binlognamelen] = 0;
    }

    /**
     * Check MariaDB GTID request
     */
    if (slave->mariadb10_compat
        && slave->mariadb_gtid)
    {
        /* Set file and pos accordingly to GTID lookup */
        if (!blr_slave_gtid_request(router,
                                    slave,
                                    binlognamelen > 0,
                                    requested_pos))
        {
            // ERROR
            slave->state = BLRS_ERRORED;
            dcb_close(slave->dcb);
            return 1;
        }
    }
    else
    {
        /**
         * Binlog file has been set from packet data.
         * Now just set the position from packet as well.
         */
        slave->binlog_pos = requested_pos;
    }

    /**
     * Check for a pending transaction and possible unsafe position.
     * Force slave disconnection if requested position is unsafe.
     */
    if (router->trx_safe)
    {
        bool force_disconnect = false;

        pthread_mutex_lock(&router->binlog_lock);
        if (router->pending_transaction.state > BLRM_NO_TRANSACTION
            && blr_is_current_binlog(router, slave)
            && (slave->binlog_pos > router->binlog_position))
        {
            force_disconnect = true;
        }
        pthread_mutex_unlock(&router->binlog_lock);

        if (force_disconnect)
        {
            MXS_ERROR("%s: Slave %s:%i, server-id %d, binlog '%s', blr_slave_binlog_dump failure: "
                      "Requested binlog position %lu. Position is unsafe so disconnecting. "
                      "Latest safe position %lu, end of binlog file %lu",
                      router->service->name,
                      slave->dcb->remote,
                      dcb_get_port(slave->dcb),
                      slave->serverid,
                      slave->binlog_name,
                      (unsigned long)slave->binlog_pos,
                      router->binlog_position,
                      router->current_pos);

            slave->state = BLRS_ERRORED;

            /*
             * Close the slave session and socket
             * The slave will try to reconnect
             */
            dcb_close(slave->dcb);

            return 1;
        }
    }

    MXS_DEBUG("%s: Slave %s:%i, COM_BINLOG_DUMP: binlog name '%s', length %lu, "
              "from position %lu.",
              router->service->name,
              slave->dcb->remote,
              dcb_get_port(slave->dcb),
              slave->binlog_name,
              strlen(slave->binlog_name),
              (unsigned long)slave->binlog_pos);

    /* Check first the requested file exists */
    if (!blr_binlog_file_exists(router, &slave->f_info))
    {
        char errmsg[BINLOG_ERROR_MSG_LEN + 1];

        snprintf(errmsg,
                 BINLOG_ERROR_MSG_LEN,
                 "Requested file name '%s' doesn't exist",
                 slave->binlog_name);
        errmsg[BINLOG_ERROR_MSG_LEN] = '\0';

        // ERROR
        slave->seqno++;
        blr_slave_abort_dump_request(slave, errmsg);

        slave->state = BLRS_ERRORED;
        dcb_close(slave->dcb);
        return 1;
    }

    /* First reply starts from seq = 1 */
    slave->seqno = 1;

    /**
     * Check whether the request file is empty
     * and try using next file in sequence or next one
     * based on GTID mpas.
     * If one or more files have been skipped then
     * the slave->binlog_pos is set to 4 and
     * slave->binlogname set to new filename.
     */
    blr_slave_skip_empty_files(router, slave);

    /* Build and send Fake Rotate Event */
    if (!blr_send_connect_fake_rotate(router, slave))
    {
        char errmsg[BINLOG_ERROR_MSG_LEN + 1];
        snprintf(errmsg,
                 BINLOG_ERROR_MSG_LEN,
                 "Cannot send Fake Rotate Event for '%s'",
                 slave->binlog_name);
        errmsg[BINLOG_ERROR_MSG_LEN] = '\0';
        // ERROR
        blr_slave_abort_dump_request(slave, errmsg);
        slave->state = BLRS_ERRORED;
        dcb_close(slave->dcb);
        return 1;
    }

    /* set lastEventReceived */
    slave->lastEventReceived = ROTATE_EVENT;

    /* set lastReply for slave heartbeat check */
    if (router->send_slave_heartbeat)
    {
        slave->lastReply = time(0);
    }

    /* Read Format Description Event */
    GWBUF* fde = blr_slave_read_fde(router, slave);
    if (fde == NULL)
    {
        char errmsg[BINLOG_ERROR_MSG_LEN + 1];
        snprintf(errmsg,
                 BINLOG_ERROR_MSG_LEN,
                 "Cannot read FDE event from file '%s'",
                 slave->binlog_name);
        errmsg[BINLOG_ERROR_MSG_LEN] = '\0';
        // ERROR
        blr_slave_abort_dump_request(slave, errmsg);
        slave->state = BLRS_ERRORED;
        dcb_close(slave->dcb);
        return 1;
    }

    /* FDE ends at pos 4 + FDE size */
    fde_end_pos = 4 + GWBUF_LENGTH(fde);

    /* Send a Fake FORMAT_DESCRIPTION_EVENT */
    if (slave->binlog_pos != 4)
    {
        if (!blr_slave_send_fde(router, slave, fde))
        {
            char errmsg[BINLOG_ERROR_MSG_LEN + 1];
            snprintf(errmsg,
                     BINLOG_ERROR_MSG_LEN,
                     "Cannot send FDE for file '%s'",
                     slave->binlog_name);
            errmsg[BINLOG_ERROR_MSG_LEN] = '\0';
            // ERROR
            blr_slave_abort_dump_request(slave, errmsg);
            slave->state = BLRS_ERRORED;
            dcb_close(slave->dcb);
            gwbuf_free(fde);
            return 1;
        }
    }

    /* set lastEventReceived */
    slave->lastEventReceived = FORMAT_DESCRIPTION_EVENT;

    /**
     * Check for MXS_START_ENCRYPTION_EVENT (after FDE) if
     * client request pos is greater than 4
     *
     * TODO: If router has binlog encryption take it
     * otherwise error
     * If no encryption and event found return error
     *
     * If event is found the contest is set into slave struct
     */
    if (slave->binlog_pos != 4)
    {
        blr_slave_read_ste(router, slave, fde_end_pos);
    }

    /**
     * Add GTID_LIST Fake Event before sending any new event
     * Note: slave->binlog_pos must not be 4
     */
    if (slave->binlog_pos != 4
        && slave->mariadb10_compat
        && slave->mariadb_gtid)
    {
        if (!blr_send_fake_gtid_list(slave,
                                     slave->mariadb_gtid,
                                     router->masterid))
        {
            char errmsg[BINLOG_ERROR_MSG_LEN + 1];
            snprintf(errmsg,
                     BINLOG_ERROR_MSG_LEN,
                     "Cannot send Fake GTID List Event for '%s'",
                     slave->binlog_name);
            errmsg[BINLOG_ERROR_MSG_LEN] = '\0';
            // ERROR
            blr_slave_abort_dump_request(slave, errmsg);
            slave->state = BLRS_ERRORED;
            dcb_close(slave->dcb);
            gwbuf_free(fde);
            return 1;
        }
        slave->lastEventReceived = MARIADB10_GTID_GTID_LIST_EVENT;
    }

    /* Set dcb_callback for the events reading routine */
    dcb_add_callback(slave->dcb, DCB_REASON_DRAINED, blr_slave_callback, slave);

    slave->state = BLRS_DUMPING;

    MXS_NOTICE("%s: Slave [%s]:%d, server id %d requested binlog file %s from position %lu",
               router->service->name,
               slave->dcb->remote,
               dcb_get_port(slave->dcb),
               slave->serverid,
               slave->binlog_name,
               (unsigned long)slave->binlog_pos);

    /* Force the slave to call catchup routine */
    poll_fake_write_event(slave->dcb);

    gwbuf_free(fde);
    return 1;
}

/**
 * Encode a value into a number of bits in a MySQL packet
 *
 * @param   data    Pointer to location in target packet
 * @param   value   The value to encode into the buffer
 * @param   len Number of bits to encode value into
 */
static void encode_value(unsigned char* data, unsigned int value, int len)
{
    while (len > 0)
    {
        *data++ = value & 0xff;
        value >>= 8;
        len -= 8;
    }
}

/**
 * Populate a header structure for a replication message from a GWBUF.
 *
 * @param pkt   The incoming packet in a GWBUF chain
 * @param hdr   The packet header to populate
 * @return  A pointer to the first byte following the event header
 */
uint8_t* blr_build_header(GWBUF* pkt, REP_HEADER* hdr)
{
    uint8_t* ptr;

    ptr = GWBUF_DATA(pkt);

    encode_value(ptr, hdr->payload_len, 24);
    ptr += 3;
    *ptr++ = hdr->seqno;
    *ptr++ = hdr->ok;
    encode_value(ptr, hdr->timestamp, 32);
    ptr += 4;
    *ptr++ = hdr->event_type;
    encode_value(ptr, hdr->serverid, 32);
    ptr += 4;
    encode_value(ptr, hdr->event_size, 32);
    ptr += 4;
    encode_value(ptr, hdr->next_pos, 32);
    ptr += 4;
    encode_value(ptr, hdr->flags, 16);
    ptr += 2;

    return ptr;
}

/**
 * We have a registered slave that is behind the current leading edge of the
 * binlog. We must replay the log entries to bring this node up to speed.
 *
 * There may be a large number of records to send to the slave, the process
 * is triggered by the slave COM_BINLOG_DUMP message and all the events must
 * be sent without receiving any new event. This measn there is no trigger into
 * MaxScale other than this initial message. However, if we simply send all the
 * events we end up with an extremely long write queue on the DCB and risk
 * running the server out of resources.
 *
 * The slave catchup routine will send a burst of replication events per single
 * call. The paramter "long" control the number of events in the burst. The
 * short burst is intended to be used when the master receive an event and
 * needs to put the slave into catchup mode. This prevents the slave taking
 * too much time away from the thread that is processing the master events.
 *
 * At the end of the burst a fake EPOLLOUT event is added to the poll event
 * queue. This ensures that the slave callback for processing DCB write drain
 * will be called and future catchup requests will be handled on another thread.
 *
 * @param   router      The binlog router
 * @param   slave       The slave that is behind
 * @param   large       Send a long or short burst of events
 * @return          The number of bytes written
 */
int blr_slave_catchup(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, bool large)
{
    GWBUF* record;
    REP_HEADER hdr;
    int rval = 1, burst;
    int rotating = 0;
    long burst_size;
    char read_errmsg[BINLOG_ERROR_MSG_LEN + 1];
    MARIADB_GTID_INFO* f_tree = router->storage_type == BLR_BINLOG_STORAGE_TREE ?
        &slave->f_info :
        NULL;

    read_errmsg[BINLOG_ERROR_MSG_LEN] = '\0';

    if (large)
    {
        burst = router->long_burst;
    }
    else
    {
        burst = router->short_burst;
    }

    burst_size = router->burst_size;

    int do_return;

    pthread_mutex_lock(&router->binlog_lock);

    do_return = 0;

    /* check for a pending transaction and safe position */
    if (router->pending_transaction.state > BLRM_NO_TRANSACTION
        && blr_is_current_binlog(router, slave)
        && (slave->binlog_pos > router->binlog_position))
    {
        do_return = 1;
    }

    pthread_mutex_unlock(&router->binlog_lock);

    if (do_return)
    {
        pthread_mutex_lock(&slave->catch_lock);
        slave->cstate &= ~CS_BUSY;
        slave->cstate |= CS_EXPECTCB;
        pthread_mutex_unlock(&slave->catch_lock);
        poll_fake_write_event(slave->dcb);

        return 0;
    }

    BLFILE* file;
#ifdef BLFILE_IN_SLAVE
    file = slave->file;
    slave->file = NULL;
#else
    file = NULL;
#endif

    // Prefix for BLR_BINLOG_STORAGE_TREE
    char t_prefix[BINLOG_FILE_EXTRA_INFO] = "";

    if (file == NULL)
    {
        rotating = router->rotating;
        if ((file = blr_open_binlog(router,
                                    slave->binlog_name,
                                    f_tree)) == NULL)
        {
            char err_msg[BINLOG_ERROR_MSG_LEN + 1];
            err_msg[BINLOG_ERROR_MSG_LEN] = '\0';

            if (rotating)
            {
                pthread_mutex_lock(&slave->catch_lock);
                slave->cstate |= CS_EXPECTCB;
                slave->cstate &= ~CS_BUSY;
                pthread_mutex_unlock(&slave->catch_lock);
                poll_fake_write_event(slave->dcb);
                return rval;
            }

            /* Fill the  file prefix */
            if (f_tree)
            {
                sprintf(t_prefix,
                        "%" PRIu32 "/%" PRIu32 "/",
                        f_tree->gtid_elms.domain_id,
                        f_tree->gtid_elms.server_id);
            }

            MXS_ERROR("Slave %s:%i, server-id %d, binlog '%s%s': blr_slave_catchup "
                      "failed to open binlog file.",
                      slave->dcb->remote,
                      dcb_get_port(slave->dcb),
                      slave->serverid,
                      t_prefix,
                      slave->binlog_name);

            slave->cstate &= ~CS_BUSY;
            slave->state = BLRS_ERRORED;

            snprintf(err_msg,
                     BINLOG_ERROR_MSG_LEN,
                     "Failed to open binlog '%s'",
                     slave->binlog_name);

            /* Send error that stops slave replication */
            blr_send_custom_error(slave->dcb,
                                  slave->seqno++,
                                  0,
                                  err_msg,
                                  "HY000",
                                  BINLOG_FATAL_ERROR_READING);

            dcb_close(slave->dcb);
            return 0;
        }
    }

    slave->stats.n_bursts++;

#ifdef BLSLAVE_IN_FILE
    slave->file = file;
#endif
    int events_before = slave->stats.n_events;

    /* Loop read binlog events from slave binlog file */
    while (burst-- && burst_size > 0
           &&   /* Read one binlog event */
           (record = blr_read_binlog(router,
                                     file,
                                     slave->binlog_pos,
                                     &hdr,
                                     read_errmsg,
                                     slave->encryption_ctx)) != NULL)
    {
        char binlog_name[BINLOG_FNAMELEN + 1];
        uint32_t binlog_pos;
        uint32_t event_size;

        /* Get up to date file prefix */
        if (f_tree)
        {
            sprintf(t_prefix,
                    "%" PRIu32 "/%" PRIu32 "/",
                    slave->f_info.gtid_elms.domain_id,
                    slave->f_info.gtid_elms.server_id);
        }

        strcpy(binlog_name, slave->binlog_name);
        binlog_pos = slave->binlog_pos;

        /**
         * Don't sent special events generated by MaxScale
         * or ANNOTATE_ROWS events if not requested
         */
        if (hdr.event_type == MARIADB10_START_ENCRYPTION_EVENT
            || hdr.event_type == IGNORABLE_EVENT
            || (!slave->annotate_rows
                && hdr.event_type == MARIADB_ANNOTATE_ROWS_EVENT))
        {
            /* In case of file rotation or pos = 4 the events
             * are sent from position 4 and the new FDE at pos 4 is read.
             * We need to check whether the first event after FDE
             * is the MARIADB10_MXS_START_ENCRYPTION_EVENT of the new file.
             *
             * Read it if slave->encryption_ctx is NULL and
             * set the slave->encryption_ctx accordingly
             */
            pthread_mutex_lock(&slave->catch_lock);

            if (hdr.event_type == MARIADB10_START_ENCRYPTION_EVENT
                && !slave->encryption_ctx)
            {
                /* read it, set slave & file context */
                uint8_t* record_ptr = GWBUF_DATA(record);
                void* mem = MXS_CALLOC(1, sizeof(SLAVE_ENCRYPTION_CTX));
                SLAVE_ENCRYPTION_CTX* encryption_ctx;
                encryption_ctx = static_cast<SLAVE_ENCRYPTION_CTX*>(mem);

                MXS_ABORT_IF_NULL(encryption_ctx);
                record_ptr += BINLOG_EVENT_HDR_LEN;
                encryption_ctx->binlog_crypto_scheme = record_ptr[0];
                memcpy(&encryption_ctx->binlog_key_version,
                       record_ptr + 1,
                       BLRM_KEY_VERSION_LENGTH);
                memcpy(encryption_ctx->nonce,
                       record_ptr + 1 + BLRM_KEY_VERSION_LENGTH,
                       BLRM_NONCE_LENGTH);

                /* Save current first_enc_event_pos */
                encryption_ctx->first_enc_event_pos = hdr.next_pos;

                /* set the encryption ctx into slave */
                slave->encryption_ctx = encryption_ctx;

                MXS_INFO("Start Encryption event found while reading. "
                         "Binlog '%s%s' is encrypted. First event at %lu",
                         t_prefix,
                         slave->binlog_name,
                         (unsigned long)hdr.next_pos);
            }
            /* MARIADB_ANNOTATE_ROWS_EVENT is skipped: just log that */
            else if (hdr.event_type == MARIADB_ANNOTATE_ROWS_EVENT)
            {
                MXS_INFO("Skipping ANNOTATE_ROWS event [%s] of size %lu while "
                         "reading binlog '%s%s' at %lu",
                         blr_get_event_description(router, hdr.event_type),
                         (unsigned long)hdr.event_size,
                         t_prefix,
                         slave->binlog_name,
                         (unsigned long)slave->binlog_pos);
            }
            else
            {
                MXS_INFO("Found ignorable event [%s] of size %lu while "
                         "reading binlog '%s%s' at %lu",
                         blr_get_event_description(router, hdr.event_type),
                         (unsigned long)hdr.event_size,
                         t_prefix,
                         slave->binlog_name,
                         (unsigned long)slave->binlog_pos);
            }

            /* set next pos */
            slave->binlog_pos = hdr.next_pos;

            pthread_mutex_unlock(&slave->catch_lock);

            gwbuf_free(record);
            record = NULL;

            break;
        }

        /* Handle ROTATE_EVENT */
        if (hdr.event_type == ROTATE_EVENT)
        {
            int64_t beat1 = mxs_clock();
            blr_close_binlog(router, file);
            int64_t beat2 = mxs_clock();
            if (beat2 - beat1 > 1)
            {
                MXS_ERROR("blr_close_binlog took %ld maxscale beats", beat2 - beat1);
            }
            /* Set new file in slave->binlog_name */
            blr_slave_rotate(router, slave, GWBUF_DATA(record));

            /* reset the encryption context */
            MXS_FREE(slave->encryption_ctx);
            slave->encryption_ctx = NULL;

            beat1 = mxs_clock();

#ifdef BLFILE_IN_SLAVE
            if ((slave->file = blr_open_binlog(router,
                                               slave->binlog_name,
                                               f_tree)) == NULL)
#else
            if ((file = blr_open_binlog(router,
                                        slave->binlog_name,
                                        f_tree)) == NULL)
#endif
            {
                char err_msg[BINLOG_ERROR_MSG_LEN + 1];
                err_msg[BINLOG_ERROR_MSG_LEN] = '\0';
                if (rotating)
                {
                    pthread_mutex_lock(&slave->catch_lock);
                    slave->cstate |= CS_EXPECTCB;
                    slave->cstate &= ~CS_BUSY;
                    pthread_mutex_unlock(&slave->catch_lock);
                    poll_fake_write_event(slave->dcb);
                    return rval;
                }

                /* Refresh file prefix */
                if (f_tree)
                {
                    sprintf(t_prefix,
                            "%" PRIu32 "/%" PRIu32 "/",
                            slave->f_info.gtid_elms.domain_id,
                            slave->f_info.gtid_elms.server_id);
                }

                MXS_ERROR("Slave %s:%i, server-id %d, binlog '%s%s': blr_slave_catchup "
                          "failed to open binlog file in rotate event",
                          slave->dcb->remote,
                          dcb_get_port(slave->dcb),
                          slave->serverid,
                          t_prefix,
                          slave->binlog_name);

                slave->state = BLRS_ERRORED;

                snprintf(err_msg,
                         BINLOG_ERROR_MSG_LEN,
                         "Failed to open binlog '%s' in rotate event",
                         slave->binlog_name);

                /* Send error that stops slave replication */
                blr_send_custom_error(slave->dcb,
                                      slave->seqno,
                                      0,
                                      err_msg,
                                      "HY000",
                                      BINLOG_FATAL_ERROR_READING);

                gwbuf_free(record);
                record = NULL;

                slave->state = BLRS_ERRORED;
                dcb_close(slave->dcb);

                return 0;
            }

#ifdef BLFILE_IN_SLAVE
            file = slave->file;
#endif
            if (mxs_clock() - beat1 > 1)
            {
                MXS_ERROR("blr_open_binlog took %lu beats",
                          mxs_clock() - beat1);
            }
        }

        /* Send the binlog event */
        if (blr_send_event(BLR_THREAD_ROLE_SLAVE,
                           binlog_name,
                           binlog_pos,
                           slave,
                           &hdr,
                           (uint8_t*)record->start))
        {
            if (hdr.event_type != ROTATE_EVENT)
            {
                slave->binlog_pos = hdr.next_pos;
            }
            slave->stats.n_events++;
            burst_size -= hdr.event_size;
        }
        else
        {
            MXS_WARNING("Slave %s:%i, server-id %d, binlog '%s%s', position %u: "
                        "Slave-thread could not send event to slave, "
                        "closing connection.",
                        slave->dcb->remote,
                        dcb_get_port(slave->dcb),
                        slave->serverid,
                        t_prefix,
                        binlog_name,
                        binlog_pos);
#ifndef BLFILE_IN_SLAVE
            blr_close_binlog(router, file);
#endif
            slave->state = BLRS_ERRORED;
            dcb_close(slave->dcb);
            return 0;
        }

        gwbuf_free(record);
        record = NULL;

        /* set lastReply for slave heartbeat check */
        if (router->send_slave_heartbeat)
        {
            slave->lastReply = time(0);
        }
    }

    /**
     * End of while reading
     *
     * Note:
     * - the reading routine can set 'record' pointer to NULL
     * - after reading & sending it is always set to NULL
     * - if the above loop ends due to burst and burstsize values,
     *   the 'record' is NULL as well (no reads at all or at least
     *   one succesfull read).
     *
     * Now checking read error indicator.
     */

    mxb_assert(record == NULL);

    /* Refresh file prefix */
    if (f_tree)
    {
        sprintf(t_prefix,
                "%" PRIu32 "/%" PRIu32 "/",
                slave->f_info.gtid_elms.domain_id,
                slave->f_info.gtid_elms.server_id);
    }

    if (hdr.ok != SLAVE_POS_READ_OK)
    {
        slave->stats.n_failed_read++;

        if (hdr.ok == SLAVE_POS_BAD_FD)
        {
            MXS_ERROR("%s Slave %s:%i, server-id %d, binlog '%s%s', %s",
                      router->service->name,
                      slave->dcb->remote,
                      dcb_get_port(slave->dcb),
                      slave->serverid,
                      t_prefix,
                      slave->binlog_name,
                      read_errmsg);
        }

        if (hdr.ok == SLAVE_POS_BEYOND_EOF)
        {
            MXS_ERROR("%s Slave %s:%i, server-id %d, binlog '%s%s', %s",
                      router->service->name,
                      slave->dcb->remote,
                      dcb_get_port(slave->dcb),
                      slave->serverid,
                      t_prefix,
                      slave->binlog_name,
                      read_errmsg);

            /*
             * Close the slave session and socket
             * The slave will try to reconnect
             */
            dcb_close(slave->dcb);

#ifndef BLFILE_IN_SLAVE
            blr_close_binlog(router, file);
#endif
            return 0;
        }

        if (hdr.ok == SLAVE_POS_READ_ERR)
        {
            MXS_ERROR("%s Slave %s:%i, server-id %d, binlog '%s%s', %s",
                      router->service->name,
                      slave->dcb->remote,
                      dcb_get_port(slave->dcb),
                      slave->serverid,
                      t_prefix,
                      slave->binlog_name,
                      read_errmsg);

            pthread_mutex_lock(&slave->catch_lock);

            slave->state = BLRS_ERRORED;

            pthread_mutex_unlock(&slave->catch_lock);

            /*
             * Send an error that will stop slave replication
             */
            blr_send_custom_error(slave->dcb,
                                  slave->seqno++,
                                  0,
                                  read_errmsg,
                                  "HY000",
                                  BINLOG_FATAL_ERROR_READING);

            dcb_close(slave->dcb);
#ifndef BLFILE_IN_SLAVE
            blr_close_binlog(router, file);
#endif
            return 0;
        }

        if (hdr.ok == SLAVE_POS_READ_UNSAFE)
        {

            MXS_NOTICE("%s: Slave %s:%i, server-id %d, binlog '%s%s', read %d events, "
                       "current committed transaction event being sent: %lu, %s",
                       router->service->name,
                       slave->dcb->remote,
                       dcb_get_port(slave->dcb),
                       slave->serverid,
                       t_prefix,
                       slave->binlog_name,
                       slave->stats.n_events - events_before,
                       router->current_safe_event,
                       read_errmsg);
            hdr.ok = SLAVE_POS_READ_OK;
        }
    }

    /* Remove BUSY state */
    pthread_mutex_lock(&slave->catch_lock);
    slave->cstate &= ~CS_BUSY;
    pthread_mutex_unlock(&slave->catch_lock);

    mxb_assert(hdr.ok == SLAVE_POS_READ_OK);

    /**
     * Check now slave position with read indicator = SLAVE_POS_READ_OK
     *
     * Two cases handled:
     * (1) The slave is Up To Date
     * (2) The slave is at EOF of a file which is not the current router file
     *
     */
    if (slave->binlog_pos == router->binlog_position
        && blr_is_current_binlog(router, slave))
    {
        /**
         * (1) Same name and pos as current router file: aka Up To Date
         */
        pthread_mutex_lock(&router->binlog_lock);
        pthread_mutex_lock(&slave->catch_lock);

        /*
         * Now check again since we hold the router->binlog_lock
         * and slave->catch_lock.
         */
        if (slave->binlog_pos != router->binlog_position
            || !blr_is_current_binlog(router, slave))
        {
            slave->cstate |= CS_EXPECTCB;
            pthread_mutex_unlock(&slave->catch_lock);
            pthread_mutex_unlock(&router->binlog_lock);

            /* Force slave to read events via catchup routine */
            poll_fake_write_event(slave->dcb);
        }
        else
        {
            /**
             * The slave server is up to date!
             *
             * set the CS_WAIT_DATA: this allows notification
             * when new events are received from master server,
             * the call back routine will be called later.
             */
            slave->cstate |= CS_WAIT_DATA;

            pthread_mutex_unlock(&slave->catch_lock);
            pthread_mutex_unlock(&router->binlog_lock);
        }
    }
    else
    {
        /**
         * (2) Checking End Of File of the slave binlog file
         * and current router file
         */
        if (slave->binlog_pos >= blr_file_size(file)
            && router->rotating == 0
            && !blr_is_current_binlog(router, slave))
        {
            /**
             * This is end of current slave file
             * which is not the current router binlog file
             */
            char next_file[BINLOG_FNAMELEN + 1] = "";
            MARIADB_GTID_INFO current_info;
            char c_prefix[BINLOG_FILE_EXTRA_INFO] = "";
            bool have_heartbeat = router->send_slave_heartbeat
                && (slave->heartbeat > 0);

            /**
             * Save current MARIADB_GTID_INFO detail because
             * calling blr_file_next_exists() overwrites that
             */
            if (f_tree)
            {
                pthread_mutex_lock(&slave->catch_lock);
                blr_slave_info_save(&slave->f_info, &current_info, c_prefix);
                pthread_mutex_unlock(&slave->catch_lock);
            }

            /**
             * Check now whether the next file exists and it's readable
             *
             * If not, handle some cases
             * if found issue a fake_rotate event
             */
            if (!blr_file_next_exists(router, slave, next_file))
            {
                /**
                 * The next binlog file to read doesn't exist
                 * or it's not set.
                 */

                if (router->mariadb10_master_gtid
                    && router->master_state == BLRM_SLAVE_STOPPED
                    && !router->binlog_name[0])
                {
                    /**
                     * (1) Don't care about empty router->binlogname in
                     * BLRM_SLAVE_STOPPED state when GTID
                     * registration is on:
                     * set CS_WAIT_DATA and return.
                     */
                    pthread_mutex_lock(&slave->catch_lock);

                    if (f_tree)
                    {
                        /**
                         * We need to deal with current slave file:
                         * restore first the GTID info into slave->f_info
                         */
                        memcpy(&slave->f_info,
                               &current_info,
                               sizeof(MARIADB_GTID_INFO));
                    }

                    /**
                     * We force cachtup state to CS_WAIT_DATA now:
                     *
                     * The slave can be called by any new master
                     * event received (no matter which is the binlog file)
                     * or by an heartbeat event.
                     */
                    slave->cstate = CS_WAIT_DATA;

                    pthread_mutex_unlock(&slave->catch_lock);

#ifndef BLFILE_IN_SLAVE
                    /* Close file */
                    blr_close_binlog(router, file);
#endif
                    return rval;
                }

                /**
                 * (2) The next file is not available/existent, actions:
                 *
                 * If router state is BLRM_BINLOGDUMP
                 * - abort slave connection if MISSING_FILE_READ_RETRIES is hit
                 * or
                 * - just log a warning message
                 *
                 * Note: in any other router state we don't log messages
                 */
                if (router->master_state == BLRM_BINLOGDUMP)
                {
                    pthread_mutex_lock(&slave->catch_lock);
                    /* Router state is BLRM_BINLOGDUMP (aka replicating) */
                    if (slave->stats.n_failed_read < MISSING_FILE_READ_RETRIES)
                    {
                        slave->stats.n_failed_read++;

                        pthread_mutex_unlock(&slave->catch_lock);

                        /* Log warning for missing file */
                        blr_slave_log_next_file_action(router,
                                                       slave,
                                                       c_prefix,
                                                       next_file,
                                                       SLAVE_EOF_WARNING);
                    }
                    else
                    {
                        /**
                         * Force error and disconnect
                         * when exceeding error counter limit
                         */
                        slave->state = BLRS_ERRORED;

                        pthread_mutex_unlock(&slave->catch_lock);

                        /* Log error for missing file */
                        blr_slave_log_next_file_action(router,
                                                       slave,
                                                       c_prefix,
                                                       next_file,
                                                       SLAVE_EOF_ERROR);

                        /* Send error that stops slave replication */
                        blr_send_custom_error(slave->dcb,
                                              slave->seqno++,
                                              0,
                                              "next binlog file to read doesn't exist",
                                              "HY000",
                                              BINLOG_FATAL_ERROR_READING);

#ifndef BLFILE_IN_SLAVE
                        /* Close file */
                        blr_close_binlog(router, file);
#endif
                        /* Disconnect client */
                        dcb_close(slave->dcb);

                        return 0;
                    }
                }   // No else branch: no further actions

                /**
                 * We need to deal with current slave file:
                 * restore first the GTID info into slave->f_info
                 */
                pthread_mutex_lock(&slave->catch_lock);
                if (f_tree)
                {
                    memcpy(&slave->f_info,
                           &current_info,
                           sizeof(MARIADB_GTID_INFO));
                }

                /**
                 * We force cachtup state to CS_WAIT_DATA now:
                 *
                 * The slave can be called by any new master
                 * event received (no matter which is the binlog file)
                 * or by an heartbeat event.
                 */
                slave->cstate = CS_WAIT_DATA;

                pthread_mutex_unlock(&slave->catch_lock);
            }
            else
            {
                /* We may have reached the end of file of a non-current
                 * binlog file.
                 *
                 * Note if the master is rotating there is a window during
                 * which the rotate event has been written to the old binlog
                 * but the new binlog file has not yet been created. Therefore
                 * we ignore these issues during the rotate processing.
                 *
                 * We send a fake_rotate_event to 'next_file'
                 * Note:
                 *     slave->f_info updated by previous call to
                 *     blr_file_next_exists()
                 */
                blr_slave_log_next_file_action(router,
                                               slave,
                                               c_prefix,
                                               next_file,
                                               SLAVE_EOF_ROTATE);

                /* Reset encryption context */
                MXS_FREE(slave->encryption_ctx);
                slave->encryption_ctx = NULL;

                /* Now pass the next_file to blr_slave_fake_rotate() */
#ifdef BLFILE_IN_SLAVE
                if (blr_slave_fake_rotate(router,
                                          slave,
                                          &slave->file,
                                          next_file))
#else
                if (blr_slave_fake_rotate(router,
                                          slave,
                                          &file,
                                          next_file))
#endif
                {
                    pthread_mutex_lock(&slave->catch_lock);
                    slave->cstate |= CS_EXPECTCB;
                    pthread_mutex_unlock(&slave->catch_lock);
                    /**
                     * Note:
                     * Fake rotate just written to client,
                     * no need to call poll_fake_write_event()
                     */

                    // blr_slave_fake_rotate closes the file on success
                    file = NULL;
                }
                else
                {
                    /* Set ERROR */
                    slave->state = BLRS_ERRORED;
                    /* Disconnect client */
                    dcb_close(slave->dcb);
#ifndef BLFILE_IN_SLAVE
                    /* Close file */
                    blr_close_binlog(router, file);
#endif
                    return 0;
                }
            }
        }
        else
        {   /**
             * Still reading from current slave file but
             * nothing has been written to client right now
             * (perhaps some ignorable / skipped events)
             * just retry to read again.
             */
            pthread_mutex_lock(&slave->catch_lock);
            slave->cstate |= CS_EXPECTCB;
            pthread_mutex_unlock(&slave->catch_lock);
            poll_fake_write_event(slave->dcb);
        }
    }

#ifndef BLFILE_IN_SLAVE
    if (file)
    {
        /* Close file */
        blr_close_binlog(router, file);
    }
#endif

    return rval;
}

/**
 * The DCB callback used by the slave to obtain DCB_REASON_LOW_WATER callbacks
 * when the server sends all the the queue data for a DCB. This is the mechanism
 * that is used to implement the flow control mechanism for the sending of
 * large quantities of binlog records during the catchup process.
 *
 * @param dcb       The DCB of the slave connection
 * @param reason    The reason the callback was called
 * @param data      The user data, in this case the server structure
 */
int blr_slave_callback(DCB* dcb, DCB_REASON reason, void* data)
{
    ROUTER_SLAVE* slave = (ROUTER_SLAVE*)data;
    ROUTER_INSTANCE* router = slave->router;

    if (NULL == dcb->session->router_session)
    {
        /*
         * The following processing will fail if there is no router session,
         * because the "data" parameter will not contain meaningful data,
         * so we have no choice but to stop here.
         */
        return 0;
    }
    if (reason == DCB_REASON_DRAINED)
    {
        if (slave->state == BLRS_DUMPING)
        {
            pthread_mutex_lock(&slave->catch_lock);
            if (slave->cstate & CS_BUSY)
            {
                pthread_mutex_unlock(&slave->catch_lock);
                return 0;
            }
            slave->cstate &= ~(CS_EXPECTCB);
            slave->cstate |= CS_BUSY;
            pthread_mutex_unlock(&slave->catch_lock);

            slave->stats.n_dcb++;

            blr_slave_catchup(router, slave, true);
        }
        else
        {
            MXS_DEBUG("Ignored callback due to slave state %s",
                      blrs_states[slave->state]);
        }
    }

    if (reason == DCB_REASON_LOW_WATER)
    {
        if (slave->state == BLRS_DUMPING)
        {
            slave->stats.n_cb++;
            blr_slave_catchup(router, slave, true);
        }
        else
        {
            slave->stats.n_cbna++;
        }
    }
    return 0;
}

/**
 * Rotate the slave to the new binlog file
 *
 * @param router    The router instance
 * @param slave     The slave instance
 * @param ptr       The rotate event (minus header and OK byte)
 */
void blr_slave_rotate(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, uint8_t* ptr)
{
    int len = EXTRACT24(ptr + 9);   // Extract the event length

    // Remove length of header and position
    len = len - (BINLOG_EVENT_HDR_LEN + 8);
    if (router->master_chksum)
    {
        len -= MYSQL_HEADER_LEN;
    }
    if (len > BINLOG_FNAMELEN)
    {
        len = BINLOG_FNAMELEN;
    }
    ptr += BINLOG_EVENT_HDR_LEN;    // Skip header
    slave->binlog_pos = extract_field(ptr, 32);
    slave->binlog_pos += (((uint64_t)extract_field(ptr + 4, 32)) << 32);
    memcpy(slave->binlog_name, ptr + 8, len);
    slave->binlog_name[len] = 0;
}

/**
 * Generate an internal rotate event that we can use to cause
 * the slave to move beyond a binlog file
 * that is missisng the rotate event at the end.
 *
 * The curret binlog file is only closed on success.
 *
 * @param router    The router instance
 * @param slave     The slave to rotate
 * @return          Non-zero if the rotate took place
 */
static int blr_slave_fake_rotate(ROUTER_INSTANCE* router,
                                 ROUTER_SLAVE* slave,
                                 BLFILE** filep,
                                 const char* new_file)
{
    const char* sptr;
    int filenum;
    GWBUF* r_event;
    MARIADB_GTID_INFO* f_tree = router->storage_type == BLR_BINLOG_STORAGE_TREE ?
        &slave->f_info :
        NULL;

    if ((sptr = strrchr(new_file, '.')) == NULL)
    {
        return 0;
    }

    /* Set Pos = 4 */
    slave->binlog_pos = 4;
    /* Set Filename */
    strcpy(slave->binlog_name, new_file);

    if ((*filep = blr_open_binlog(router,
                                  new_file,
                                  f_tree)) == NULL)
    {
        return 0;
    }

    /* Build Fake Rotate Event */
    r_event = blr_build_fake_rotate_event(slave,
                                          slave->binlog_pos,
                                          new_file,
                                          router->masterid);

    int ret = r_event ? MXS_SESSION_ROUTE_REPLY(slave->dcb->session, r_event) : 0;

    /* Close binlog file on success */
    if (ret)
    {
        blr_close_binlog(router, *filep);
    }

    return ret;
}

/**
 * Read the format description event FDE from current slave logfile
 *
 * @param router    The router instance
 * @param slave     The slave to send the event to
 * @return          The read FDE event on success or NULL on error
 */
static GWBUF* blr_slave_read_fde(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    BLFILE* file;
    REP_HEADER hdr;
    GWBUF* record, * head;
    uint8_t* ptr;
    uint32_t chksum;
    char err_msg[BINLOG_ERROR_MSG_LEN + 1];
    MARIADB_GTID_INFO* f_tree = router->storage_type == BLR_BINLOG_STORAGE_TREE ?
        &slave->f_info :
        NULL;

    err_msg[BINLOG_ERROR_MSG_LEN] = '\0';

    memset(&hdr, 0, BINLOG_EVENT_HDR_LEN);

    if ((file = blr_open_binlog(router,
                                slave->binlog_name,
                                f_tree)) == NULL)
    {
        return NULL;
    }
    /* FDE, at pos 4, is not encrypted, pass NULL to last parameter */
    if ((record = blr_read_binlog(router,
                                  file,
                                  4,
                                  &hdr,
                                  err_msg,
                                  NULL)) == NULL)
    {
        if (hdr.ok != SLAVE_POS_READ_OK)
        {
            MXS_ERROR("Slave %s:%i, server-id %d, binlog '%s', "
                      "blr_read_binlog failure: %s",
                      slave->dcb->remote,
                      dcb_get_port(slave->dcb),
                      slave->serverid,
                      slave->binlog_name,
                      err_msg);
        }

        blr_close_binlog(router, file);
        return NULL;
    }
    blr_close_binlog(router, file);

    return record;
}

/**
 * Send a "fake" format description event to the newly connected slave
 *
 * @param router    The router instance
 * @param slave     The slave to send the event to
 * @return          The FDE event size on success or 0 on error
 */
static uint32_t blr_slave_send_fde(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, GWBUF* fde)
{
    GWBUF* event;
    uint8_t* ptr;
    uint32_t chksum;
    uint32_t event_size;
    uint8_t* event_ptr;

    if (fde == NULL)
    {
        return 0;
    }

    event_size = GWBUF_LENGTH(fde);

    if ((event = gwbuf_alloc(MYSQL_HEADER_LEN + 1 + event_size)) == NULL)
    {
        return 0;
    }

    ptr = GWBUF_DATA(event);

    event_size = GWBUF_LENGTH(fde);

    /* Set payload to event_size + 1 (the ok/err byte) */
    encode_value(ptr, event_size + 1, 32);
    ptr += 3;
    *ptr++ = slave->seqno++;
    *ptr++ = 0;     // OK/ERR byte

    // Copy FDE data
    memcpy(ptr, GWBUF_DATA(fde), event_size);

    encode_value(ptr, time(0), 32); // Overwrite timestamp
    ptr += 13;                      // 4 time + 1 type + 4 server_id + 4 event_size

    /* event_ptr points to position of the next event */
    encode_value(ptr, 0, 32);       // Set next position to 0

    /*
     * Since we have changed the timestamp we must recalculate the CRC
     *
     * Position ptr to the start of the event header,
     * calculate a new checksum
     * and write it into the header
     */
    ptr = GWBUF_DATA(event) + MYSQL_HEADER_LEN + 1 + event_size - BINLOG_EVENT_CRC_SIZE;
    chksum = crc32(0L, NULL, 0);
    chksum = crc32(chksum,
                   GWBUF_DATA(event) + MYSQL_HEADER_LEN + 1,
                   event_size - BINLOG_EVENT_CRC_SIZE);
    encode_value(ptr, chksum, 32);

    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, event);
}


/**
 * Send the field count packet in a response packet sequence.
 *
 * @param router    The router
 * @param slave     The slave connection
 * @param count     Number of columns in the result set
 * @return          Non-zero on success
 */
static int blr_slave_send_fieldcount(ROUTER_INSTANCE* router,
                                     ROUTER_SLAVE* slave,
                                     int count)
{
    GWBUF* pkt;
    uint8_t* ptr;

    if ((pkt = gwbuf_alloc(MYSQL_HEADER_LEN + 1)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, 1, 24);   // Add length of data packet
    ptr += 3;
    *ptr++ = 0x01;              // Sequence number in response
    *ptr++ = count;             // Number of columns
    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}


/**
 * Send the column definition packet in a response packet sequence.
 *
 * @param router    The router
 * @param slave     The slave connection
 * @param name      Name of the column
 * @param type      Column type
 * @param len       Column length
 * @param seqno     Packet sequence number
 * @return          Non-zero on success
 */
static int blr_slave_send_columndef(ROUTER_INSTANCE* router,
                                    ROUTER_SLAVE* slave,
                                    const char*   name,
                                    int type,
                                    int len,
                                    uint8_t seqno)
{
    GWBUF* pkt;
    uint8_t* ptr;

    if ((pkt = gwbuf_alloc(26 + strlen(name))) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, 22 + strlen(name), 24);   // Add length of data packet
    ptr += 3;
    *ptr++ = seqno;             // Sequence number in response
    *ptr++ = 3;                 // Catalog is always def
    *ptr++ = 'd';
    *ptr++ = 'e';
    *ptr++ = 'f';
    *ptr++ = 0;                 // Schema name length
    *ptr++ = 0;                 // virtual table name length
    *ptr++ = 0;                 // Table name length
    *ptr++ = strlen(name);      // Column name length;
    while (*name)
    {
        *ptr++ = *name++;       // Copy the column name
    }
    *ptr++ = 0;                 // Orginal column name
    *ptr++ = 0x0c;              // Length of next fields always 12
    *ptr++ = 0x3f;              // Character set
    *ptr++ = 0;
    encode_value(ptr, len, 32);         // Add length of column
    ptr += 4;
    *ptr++ = type;
    *ptr++ = 0x81;              // Two bytes of flags
    if (type == 0xfd)
    {
        *ptr++ = 0x1f;
    }
    else
    {
        *ptr++ = 0x00;
    }
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;
    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}


/**
 * Send an EOF packet in a response packet sequence.
 *
 * @param router    The router
 * @param slave     The slave connection
 * @param seqno     The sequence number of the EOF packet
 * @return          Non-zero on success
 */
static int blr_slave_send_eof(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave, int seqno)
{
    GWBUF* pkt;
    uint8_t* ptr;

    if ((pkt = gwbuf_alloc(9)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, 5, 24);       // Add length of data packet
    ptr += 3;
    *ptr++ = seqno;                 // Sequence number in response
    *ptr++ = 0xfe;                  // Length of result string
    encode_value(ptr, 0, 16);       // No errors
    ptr += 2;
    encode_value(ptr, 2, 16);       // Autocommit enabled
    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}

/**
 * Send the reply only to the SQL command "DISCONNECT SERVER $server_id'
 *
 * @param   router      The binlog router instance
 * @param   slave       The slave server to which we are sending the response
 * @return              Non-zero if data was sent
 */
static int blr_slave_send_disconnected_server(ROUTER_INSTANCE* router,
                                              ROUTER_SLAVE* slave,
                                              int server_id,
                                              int found)
{
    GWBUF* pkt;
    char state[40];
    char serverid[40];
    uint8_t* ptr;
    int len, id_len, seqno = 2;

    sprintf(serverid, "%d", server_id);
    if (found)
    {
        strcpy(state, "disconnected");
    }
    else
    {
        strcpy(state, "not found");
    }

    id_len = strlen(serverid);
    len = MYSQL_HEADER_LEN + (1 + id_len) + (1 + strlen(state));

    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }

    blr_slave_send_fieldcount(router, slave, 2);
    blr_slave_send_columndef(router,
                             slave,
                             "server_id",
                             BLR_TYPE_INT,
                             40,
                             seqno++);
    blr_slave_send_columndef(router,
                             slave,
                             "state",
                             BLR_TYPE_STRING,
                             40,
                             seqno++);
    blr_slave_send_eof(router, slave, seqno++);

    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, len - MYSQL_HEADER_LEN, 24);      // Add length of data packet
    ptr += 3;
    *ptr++ = seqno++;                   // Sequence number in response

    *ptr++ = id_len;                        // Length of result string
    memcpy((char*)ptr, serverid, id_len);   // Result string
    ptr += id_len;

    *ptr++ = strlen(state);                     // Length of result string
    memcpy((char*)ptr, state, strlen(state));   // Result string
    /* ptr += strlen(state); Not required unless more data is to be added */

    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
    return blr_slave_send_eof(router, slave, seqno++);
}


/**
 * Send the response to the SQL command "DISCONNECT SERVER $server_id'
 * and close the connection to that server
 *
 * @param   router      The binlog router instance
 * @param   slave       The connected slave server
 * @param   server_id   The slave server_id to disconnect
 * @return              Non-zero if data was sent to the client
 */
static int blr_slave_disconnect_server(ROUTER_INSTANCE* router,
                                       ROUTER_SLAVE* slave,
                                       int server_id)
{
    ROUTER_SLAVE* sptr;
    int n;
    int server_found = 0;

    pthread_mutex_lock(&router->lock);

    sptr = router->slaves;
    /* look for server_id among all registered slaves */
    while (sptr)
    {
        /* don't examine slaves with state = 0 */
        if ((sptr->state == BLRS_REGISTERED
             || sptr->state == BLRS_DUMPING)
            && sptr->serverid == server_id)
        {
            /* server_id found */
            server_found = 1;
            MXS_NOTICE("%s: Slave %s, server id %d, disconnected by %s@%s",
                       router->service->name,
                       sptr->dcb->remote,
                       server_id,
                       slave->dcb->user,
                       slave->dcb->remote);

            /* send server_id with disconnect state to client */
            n = blr_slave_send_disconnected_server(router,
                                                   slave,
                                                   server_id,
                                                   1);

            sptr->state = BLRS_UNREGISTERED;
            dcb_close_in_owning_thread(sptr->dcb);

            break;
        }
        else
        {
            sptr = sptr->next;
        }
    }

    pthread_mutex_unlock(&router->lock);

    /**
     * Server id was not found
     * send server_id with not found state to the client
     */
    if (!server_found)
    {
        n = blr_slave_send_disconnected_server(router,
                                               slave,
                                               server_id,
                                               0);
    }

    if (n == 0)
    {
        MXS_ERROR("gwbuf memory allocation in "
                  "DISCONNECT SERVER server_id [%d]",
                  sptr->serverid);

        blr_slave_send_error(router,
                             slave,
                             "Memory allocation error for DISCONNECT SERVER");
    }

    return 1;
}

/**
 * Send the response to the SQL command "DISCONNECT ALL'
 * and close the connection to all slave servers
 *
 * @param   router    The binlog router instance
 * @param   slave     The connected slave server
 * @return            Non-zero if data was sent to the client
 */
static int blr_slave_disconnect_all(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    ROUTER_SLAVE* sptr;
    char server_id[40];
    char state[40];
    uint8_t* ptr;
    int len, seqno = 2;
    GWBUF* pkt;

    /* preparing output result */
    blr_slave_send_fieldcount(router, slave, 2);
    blr_slave_send_columndef(router,
                             slave,
                             "server_id",
                             BLR_TYPE_INT,
                             40,
                             seqno++);
    blr_slave_send_columndef(router,
                             slave,
                             "state",
                             BLR_TYPE_STRING,
                             40,
                             seqno++);
    blr_slave_send_eof(router, slave, seqno++);

    pthread_mutex_lock(&router->lock);
    sptr = router->slaves;

    while (sptr)
    {
        /* skip servers with state = 0 */
        if (sptr->state == BLRS_REGISTERED || sptr->state == BLRS_DUMPING)
        {
            sprintf(server_id, "%d", sptr->serverid);
            sprintf(state, "disconnected");

            len = MYSQL_HEADER_LEN + 1 + strlen(server_id) + strlen(state) + 1;

            if ((pkt = gwbuf_alloc(len)) == NULL)
            {
                MXS_ERROR("gwbuf memory allocation in "
                          "DISCONNECT ALL for [%s], server_id [%d]",
                          sptr->dcb->remote,
                          sptr->serverid);

                pthread_mutex_unlock(&router->lock);

                blr_slave_send_error(router,
                                     slave,
                                     "Memory allocation error for DISCONNECT ALL");

                return 1;
            }

            MXS_NOTICE("%s: Slave %s, server id %d, disconnected by %s@%s",
                       router->service->name,
                       sptr->dcb->remote,
                       sptr->serverid,
                       slave->dcb->user,
                       slave->dcb->remote);

            ptr = GWBUF_DATA(pkt);
            encode_value(ptr, len - MYSQL_HEADER_LEN, 24);          // Add length of data packet

            ptr += 3;
            *ptr++ = seqno++;                                   // Sequence number in response
            *ptr++ = strlen(server_id);                         // Length of result string
            memcpy((char*)ptr, server_id, strlen(server_id));   // Result string
            ptr += strlen(server_id);
            *ptr++ = strlen(state);                             // Length of result string
            memcpy((char*)ptr, state, strlen(state));           // Result string
            ptr += strlen(state);

            MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);

            sptr->state = BLRS_UNREGISTERED;
            dcb_close_in_owning_thread(sptr->dcb);
        }
        sptr = sptr->next;
    }

    pthread_mutex_unlock(&router->lock);

    blr_slave_send_eof(router, slave, seqno);

    return 1;
}

/**
 * Send a MySQL OK packet to the connected client
 *
 * @param   router    The binlog router instance
 * @param   slave     The slave server to which we are sending data
 *
 * @return            Result of a write call, non-zero if successful
 */

static int blr_slave_send_ok(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    GWBUF* pkt;
    uint8_t ok_packet[] =
    {
        7, 0, 0,// Payload length
        1,      // Seqno,
        0,      // OK,
        0, 0, 2, 0, 0, 0
    };

    if ((pkt = gwbuf_alloc(sizeof(ok_packet))) == NULL)
    {
        return 0;
    }

    memcpy(GWBUF_DATA(pkt), ok_packet, sizeof(ok_packet));

    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}

/**
 * Send a MySQL OK packet with a message to the client
 *
 * @param    router     The binlog router instance
 * @param    message    The message to send
 * @param    slave      The slave server to which we are sending data
 *
 * @return              The write call result: non-zero on success
 */

static int blr_slave_send_ok_message(ROUTER_INSTANCE* router,
                                     ROUTER_SLAVE* slave,
                                     char* message)
{
    GWBUF* pkt;
    uint8_t* ptr;

    if ((pkt = gwbuf_alloc(11 + strlen(message) + 1)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    *ptr++ = 7 + strlen(message) + 1;   // Payload length
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 1;     // Seqno
    *ptr++ = 0;     // ok
    *ptr++ = 0;
    *ptr++ = 0;

    *ptr++ = 2;
    *ptr++ = 0;

    if (strlen(message) == 0)
    {
        *ptr++ = 0;
        *ptr++ = 0;
    }
    else
    {
        *ptr++ = 1;
        *ptr++ = 0;
        *ptr++ = strlen(message);
        strcpy((char*)ptr, message);
    }

    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}

/**
 * Stop current replication from master
 *
 * @param router    The binlog router instance
 * @param slave     The slave server to which we are sending the response
 * @return           Always 1 for error, for send_ok the bytes sent
 *
 */

static int blr_stop_slave(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    /* if unconfigured return an error */
    if (router->master_state == BLRM_UNCONFIGURED)
    {
        blr_slave_send_warning_message(router,
                                       slave,
                                       "1255:Slave already has been stopped");

        return 1;
    }

    /* if already stopped return an error */
    if (router->master_state == BLRM_SLAVE_STOPPED)
    {
        blr_slave_send_warning_message(router,
                                       slave,
                                       "1255:Slave already has been stopped");

        return 1;
    }

    if (router->master)
    {
        if (router->master->fd != -1
            && router->master->state == DCB_STATE_POLLING)
        {
            blr_close_master_in_main(router);
        }
    }

    pthread_mutex_lock(&router->lock);

    router->master_state = BLRM_SLAVE_STOPPED;

    /* set last_safe_pos */
    router->last_safe_pos = router->binlog_position;

    /**
     * Set router->prevbinlog to router->binlog_name
     * The FDE event with current filename may arrive
     * after STOP SLAVE is received
     */

    if (strcmp(router->binlog_name, router->prevbinlog) != 0)
    {
        strcpy(router->prevbinlog, router->binlog_name);    // Same size
    }

    if (router->client)
    {
        if (router->client->fd != -1
            && router->client->state == DCB_STATE_POLLING)
        {
            // Is this dead code? dcb->fd for internal DCBs is always -1
            dcb_close(router->client);
            router->client = NULL;
        }
    }

    /* Now it is safe to unleash other threads on this router instance */
    router->reconnect_pending = 0;
    router->active_logs = 0;

    pthread_mutex_unlock(&router->lock);

    MXS_NOTICE("%s: STOP SLAVE executed by %s@%s. Disconnecting from master [%s]:%d, "
               "read up to log %s, pos %lu, transaction safe pos %lu",
               router->service->name,
               slave->dcb->user,
               slave->dcb->remote,
               router->service->dbref->server->address,
               router->service->dbref->server->port,
               router->binlog_name,
               router->current_pos,
               router->binlog_position);

    if (router->trx_safe
        && router->pending_transaction.state > BLRM_NO_TRANSACTION)
    {
        char message[BINLOG_ERROR_MSG_LEN + 1] = "";
        snprintf(message,
                 BINLOG_ERROR_MSG_LEN,
                 "1105:Stopped slave mid-transaction in binlog file %s, "
                 "pos %lu, incomplete transaction starts at pos %lu",
                 router->binlog_name,
                 router->current_pos,
                 router->binlog_position);

        return blr_slave_send_warning_message(router, slave, message);
    }
    else
    {
        return blr_slave_send_ok(router, slave);
    }
}

/**
 * Start replication from current configured master
 *
 * @param router    The binlog router instance
 * @param slave     The slave server to which we are sending the response
 * @return          Always 1 for error, for send_ok the bytes sent
 *
 */

static int blr_start_slave(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    /* if unconfigured return an error */
    if (router->master_state == BLRM_UNCONFIGURED)
    {
        blr_slave_send_error_packet(slave,
                                    "The server is not configured as slave; "
                                    "fix in config file or with CHANGE MASTER TO",
                                    (unsigned int)1200,
                                    NULL);

        return 1;
    }

    /* if running return an error */
    if (router->master_state != BLRM_UNCONNECTED
        && router->master_state != BLRM_SLAVE_STOPPED
        && router->master_state != BLRM_CONNECTING)
    {
        blr_slave_send_warning_message(router,
                                       slave,
                                       "1254:Slave is already running");

        return 1;
    }

    pthread_mutex_lock(&router->lock);
    router->master_state = BLRM_UNCONNECTED;
    router->retry_count = 0;
    router->config_index = 0;   // Always start from the default configuration.
    pthread_mutex_unlock(&router->lock);

    /**
     * Check whether to create the new binlog (router->binlog_name)
     *
     * File handling happens only if mariadb10_master_gtid is off:
     * with Master GTID the first file will be created/opened
     * by the fake Rotate Event.
     */

    /* Check first for incomplete transaction */
    if (strlen(router->prevbinlog)
        && strcmp(router->prevbinlog, router->binlog_name) != 0)
    {
        if (router->trx_safe
            && router->pending_transaction.state > BLRM_NO_TRANSACTION)
        {
            char msg[BINLOG_ERROR_MSG_LEN + 1] = "";
            char file[PATH_MAX + 1] = "";
            struct stat statb;
            unsigned long filelen = 0;
            char t_prefix[BINLOG_FILE_EXTRA_INFO] = "";

            // Add file prefix
            if (router->storage_type == BLR_BINLOG_STORAGE_TREE)
            {
                sprintf(t_prefix,
                        "%" PRIu32 "/%" PRIu32 "/",
                        router->mariadb10_gtid_domain,
                        router->orig_masterid);
            }

            // Router current file
            snprintf(file,
                     PATH_MAX,
                     "%s/%s%s",
                     router->binlogdir,
                     t_prefix,
                     router->prevbinlog);

            /* Get file size */
            if (stat(file, &statb) == 0)
            {
                filelen = statb.st_size;
            }

            /* Prepare warning message */
            snprintf(msg,
                     BINLOG_ERROR_MSG_LEN,
                     "1105:Truncated partial transaction in file %s%s, "
                     "starting at pos %lu, "
                     "ending at pos %lu. File %s now has length %lu.",
                     t_prefix,
                     router->prevbinlog,
                     router->last_safe_pos,
                     filelen,
                     router->prevbinlog,
                     router->last_safe_pos);

            /* Truncate previous binlog file to last_safe pos */
            if (truncate(file, router->last_safe_pos) == -1)
            {
                MXS_ERROR("Failed to truncate file: %d, %s",
                          errno,
                          mxs_strerror(errno));
            }

            /* Log it */
            MXS_WARNING("A transaction is still opened at pos %lu"
                        " File %s%s will be truncated. "
                        "Next binlog file is %s at pos %d, "
                        "START SLAVE is required again.",
                        router->last_safe_pos,
                        t_prefix,
                        router->prevbinlog,
                        router->binlog_name,
                        4);

            pthread_mutex_lock(&router->lock);

            router->pending_transaction.state = BLRM_NO_TRANSACTION;
            router->last_safe_pos = 0;
            router->master_state = BLRM_UNCONNECTED;
            router->current_pos = 4;
            router->binlog_position = 4;
            router->current_safe_event = 4;

            pthread_mutex_unlock(&router->lock);

            /* Send warning message to mysql command */
            blr_slave_send_warning_message(router, slave, msg);

            return 1;
        }
        /* No pending transaction */
        else
        {
            /**
             * If router->mariadb10_master_gtid is Off then
             * handle file create/append.
             * This means the domain_id and server_id
             * are not taken into account for filename prefix.
             */
            if (!router->mariadb10_master_gtid)
            {
                /* If the router file is not open, create a new binlog file */
                if (router->binlog_fd == -1)
                {
                    blr_file_new_binlog(router, router->binlog_name);
                }
                else
                {
                    /* A new binlog file has been created and opened
                     * by CHANGE MASTER TO: use it
                     */
                    blr_file_append(router, router->binlog_name);
                }
            }
        }
    }

    /** Start replication from master */
    blr_start_master_in_main(router);

    MXS_NOTICE("%s: START SLAVE executed by %s@%s. Trying connection to master [%s]:%d, "
               "binlog %s, pos %lu, transaction safe pos %lu",
               router->service->name,
               slave->dcb->user,
               slave->dcb->remote,
               router->service->dbref->server->address,
               router->service->dbref->server->port,
               router->binlog_name,
               router->current_pos,
               router->binlog_position);

    /* Try reloading new users and update cached credentials */
    service_refresh_users(router->service);

    return blr_slave_send_ok(router, slave);
}

/**
 * Construct an error packet reply with specified code and status
 *
 * @param slave     The slave server instance
 * @param msg       The error message to send
 * @param err_num   The error number to send
 * @param status    The error status
 */
static void blr_slave_send_error_packet(ROUTER_SLAVE* slave,
                                        const char*   msg,
                                        unsigned int  err_num,
                                        const char*   status)
{
    GWBUF* pkt;
    unsigned char* data;
    int len;
    unsigned int mysql_errno = 0;
    const char* mysql_state;

    if ((pkt = gwbuf_alloc(strlen(msg) + 13)) == NULL)
    {
        return;
    }

    if (status != NULL)
    {
        mysql_state = status;
    }
    else
    {
        mysql_state = "HY000";
    }

    if (err_num > 0)
    {
        mysql_errno = err_num;
    }
    else
    {
        mysql_errno = (unsigned int)2003;
    }

    data = GWBUF_DATA(pkt);
    len = strlen(msg) + 9;

    encode_value(&data[0], len, 24);    // Payload length

    data[3] = 1;                        // Sequence id

    data[4] = 0xff;                     // Error indicator

    encode_value(&data[5], mysql_errno, 16);    // Error Code

    data[7] = '#';                          // Status message first char
    memcpy((char*)&data[8], mysql_state, 5);// Status message

    memcpy(&data[13], msg, strlen(msg));        // Error Message

    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}

bool ChangeMasterOptions::validate(ROUTER_INSTANCE* router,
                                   char* error,
                                   ChangeMasterConfig* config)
{
    /* Abort if MASTER_USE_GTID is in use and
     * router->mariadb10_master_gtid is not set
     */
    if (!router->mariadb10_master_gtid && !this->use_mariadb10_gtid.empty())
    {
        snprintf(error,
                 BINLOG_ERROR_MSG_LEN,
                 "Cannot use MASTER_USE_GTID. "
                 "Enable 'mariadb10_master_gtid' option first.");

        MXS_ERROR("%s: %s", router->service->name, error);

        return false;
    }

    int heartbeat_period = -1;
    if (!this->heartbeat_period.empty())
    {
        heartbeat_period = (int)strtol(this->heartbeat_period.c_str(), NULL, 10);

        if (heartbeat_period < 0
            || (errno == ERANGE)
            || heartbeat_period > BLR_HEARTBEAT_MAX_INTERVAL)
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "The requested value for the heartbeat period is "
                     "either negative or exceeds the maximum allowed "
                     "(%d seconds).",
                     BLR_HEARTBEAT_MAX_INTERVAL);

            MXS_ERROR("%s: %s", router->service->name, error);

            return false;
        }
    }

    int connect_retry = -1;
    if (!this->connect_retry.empty())
    {
        connect_retry = (int)strtol(this->connect_retry.c_str(), NULL, 10);
        if (connect_retry <= 0
            || (errno == ERANGE))
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "The requested value for MASTER_CONNECT_RETRY "
                     "interval is not valid: %s.",
                     this->connect_retry.c_str());

            MXS_ERROR("%s: %s", router->service->name, error);

            return false;
        }
    }

    int port = -1;
    if (!this->port.empty())
    {
        port = (int)strtol(this->port.c_str(), NULL, 10);
        if ((port < 0) || (port > std::numeric_limits<unsigned short>::max()))
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "The specified value for MASTER_PORT "
                     "is not valid: %s.",
                     this->port.c_str());

            MXS_ERROR("%s: %s", router->service->name, error);

            return false;
        }
    }

    config->connection_name = this->connection_name;
    config->host = this->host;
    config->port = port;
    config->binlog_file = this->binlog_file;
    config->binlog_pos = this->binlog_pos;
    config->user = this->user;
    config->password = this->password;
    config->ssl_key = this->ssl_key;
    config->ssl_cert = this->ssl_cert;
    config->ssl_ca = this->ssl_ca;
    config->ssl_enabled = this->ssl_enabled.empty() ? false : atoi(this->ssl_enabled.c_str());
    config->ssl_version = this->ssl_version;
    config->use_mariadb10_gtid = this->use_mariadb10_gtid;
    config->heartbeat_period = heartbeat_period;
    config->connect_retry = connect_retry;

    return true;
}

namespace
{

int validate_connection_name(ROUTER_INSTANCE* router, const std::string& name, char* error)
{
    static const char DEFAULT_MESSAGE[] =
        "If a connection name is provided, it must be of the format ':N' where N "
        "is an integer larger than 1.";

    int index = -1;

    char custom_message[BINLOG_ERROR_MSG_LEN + 1];

    const char* message = DEFAULT_MESSAGE;

    if (name.length() == 0)
    {
        index = 0;
        message = nullptr;
    }
    else if (name.length() == 1)
    {
        // We are fine with 'index' == -1, and the message being the default message.
    }
    else    // name.length() >= 2.
    {
        if (name.front() == ':')
        {
            string tail = name.substr(1);
            int n = strtol(tail.c_str(), NULL, 10);

            if ((n > 1) && (std::to_string(n) == tail))     // Nothing funky in the string
            {
                if (router->configs.size() == static_cast<size_t>(n - 1))
                {
                    index = n - 1;
                    message = nullptr;
                }
                else if (router->configs.size() == 0)
                {
                    snprintf(custom_message,
                             BINLOG_ERROR_MSG_LEN,
                             "The provided connection name '%s' is not valid. Currently "
                             "no default connection exists and it must be specified using "
                             "a 'CHANGE MASTER TO ...' command without a connection name.",
                             name.c_str());
                    message = custom_message;
                }
                else
                {
                    snprintf(custom_message,
                             BINLOG_ERROR_MSG_LEN,
                             "The provided connection name '%s' is not valid. Currently "
                             "the default connection and %d secondary connections have "
                             "been specified and the next valid name for an secondary "
                             "connection is ':%d'.",
                             name.c_str(),
                             (int)router->configs.size() - 1,
                             (int)router->configs.size() + 1);
                    message = custom_message;
                }
            }
        }
    }

    if (message)
    {
        snprintf(error, BINLOG_ERROR_MSG_LEN, "%s", message);
    }

    return index;
}
}

namespace
{

int blr_apply_change_master_0(ROUTER_INSTANCE* router,
                              const ChangeMasterConfig& new_config,
                              char* error)
{
    mxb_assert(new_config.connection_name.empty());

    MasterServerConfig current_master;

    /* save current config option data */
    blr_master_get_config(router, &current_master);

    /* Change the heartbeat */
    if (new_config.heartbeat_period != -1)
    {
        if (new_config.heartbeat_period == 0)
        {
            blr_log_disabled_heartbeat(router);
        }
        router->heartbeat = new_config.heartbeat_period;
    }

    /* Change the connect retry */
    if (new_config.connect_retry != -1)
    {
        router->retry_interval = new_config.connect_retry;
    }

    /* Change the replication user */
    blr_set_master_user(router, new_config.user);

    /* Change the replication password */
    blr_set_master_password(router, new_config.password);

    /* Change the master name/address */
    blr_set_master_hostname(router, new_config.host);

    /* Change the master port */
    blr_set_master_port(router, new_config.port);

    /* Handle SSL options */
    int ssl_error;
    ssl_error = blr_set_master_ssl(router, new_config, error);

    if (ssl_error != -1
        &&      // No CA cert is defined or only one of CERT or KEY is defined
        (new_config.ssl_ca.empty() || new_config.ssl_cert.empty() != new_config.ssl_key.empty()))
    {
        if (new_config.ssl_enabled)
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "MASTER_SSL=1 but some required options are missing: "
                     "check that at least MASTER_SSL_CA is defined");
            ssl_error = -1;
        }
    }

    if (ssl_error == -1)
    {
        blr_abort_change_master(router,
                                current_master,
                                error);

        return -1;
    }

    /**
     * Check if binlog file can be changed
     *
     * Change the binlog filename as from MASTER_LOG_FILE
     * New binlog file could be the next one or current one
     * or empty if router->mariadb10_master_gtid is set.
     */
    char* master_logfile = NULL;
    if (!blr_binlog_change_check(router,
                                 new_config,
                                 error)
        || !blr_change_binlog_name(router,
                                   new_config.binlog_file,
                                   &master_logfile,
                                   error)
        || !blr_apply_changes(router,
                              new_config,
                              master_logfile,
                              error))
    {
        blr_abort_change_master(router,
                                current_master,
                                error);
        MXS_FREE(master_logfile);

        return -1;
    }

    /* Log config changes (without passwords) */
    blr_log_config_changes(router, current_master, new_config);

    MXS_FREE(master_logfile);

    int change_binlog = 0;
    if (router->master_state == BLRM_UNCONFIGURED)
    {
        change_binlog = 1;
    }

    return change_binlog;
}
}

static int blr_apply_change_master(ROUTER_INSTANCE* router,
                                   int index,
                                   const ChangeMasterConfig& config,
                                   char* error)
{
    int rc = 0;

    pthread_mutex_lock(&router->lock);

    if (index == static_cast<int>(router->configs.size()))
    {
        router->configs.push_back(config);
    }
    else
    {
        mxb_assert(index < static_cast<int>(router->configs.size()));
        router->configs[index] = config;
    }

    if (index == 0)
    {
        rc = blr_apply_change_master_0(router, config, error);
    }

    pthread_mutex_unlock(&router->lock);

    return rc;
}

/**
 * Get the connection name from a CHANGE MASTER command.
 *
 * @param       command           What follows "CHANGE MASTER", that is " ['connection-name'] TO ..."
 * @param [out] pConnection_name  The connection name, if provided.
 *
 * @return The string following "TO".
 */
static char* get_connection_name(char* command, std::string* pConnection_name)
{
    command = trim_leading(command);

    char* to = strcasestr(command, "TO");

    if (!to)
    {
        // No "TO", can't be valid.
        command = nullptr;
    }
    else if (to == command)
    {
        // No connection name.
        command = to + 2;
    }
    else
    {
        // We may have the case "'connection-name' TO"
        char quote = *command;

        if ((quote == '\'') || (quote == '"'))
        {
            // At least there was a quote.
            ++command;

            char* end = strchr(command, quote);

            if (!end || (end > to))
            {
                // No closing quote.
                command = nullptr;
            }
            else
            {
                *pConnection_name = std::string(command, end);
                command = to + 2;
            }
        }
        else
        {
            // No, must be an invalid command.
            command = nullptr;
        }
    }

    return command;
}

/**
 * handle a 'change master' operation
 *
 * @param router       The router instance
 * @param command      The change master SQL command
 * @param error        The error message, preallocated
 *                     BINLOG_ERROR_MSG_LEN + 1 bytes
 * @return          0 on success,
 *                  1 on success with new binlog, -1 on failure
 */
static
int blr_handle_change_master(ROUTER_INSTANCE* router,
                             char* command,
                             char* error)
{
    std::string connection_name;
    command = get_connection_name(command, &connection_name);
    if (!command)
    {
        static const char MESSAGE[] = "statement doesn't have the CHANGE MASTER TO syntax";
        mxb_assert(sizeof(MESSAGE) <= BINLOG_ERROR_MSG_LEN);
        strcpy(error, MESSAGE);
        return -1;
    }

    int index = validate_connection_name(router, connection_name, error);

    if (index == -1)
    {
        // An error was already generated.
        return -1;
    }

    if (index != 0)
    {
        // Index 0 refers to the default configuration.
        string use_gtid = router->configs[0].use_mariadb10_gtid;

        if (use_gtid.empty() || (strcasecmp(use_gtid.c_str(), "slave_pos") != 0))
        {
            static const char MESSAGE[] =
                "Secondary masters can only be used in conjunction with GTID based replication. "
                "Specify MASTER_USE_GTID=Slave_pos for the default master connection.";
            mxb_assert(sizeof(MESSAGE) <= BINLOG_ERROR_MSG_LEN);
            strcpy(error, MESSAGE);
            return -1;
        }
    }

    std::vector<char> cmd_string(command, command + strlen(command) + 1);   // Include the NULL

    /* Parse SQL command and populate the change_master struct */
    ChangeMasterOptions options(connection_name);

    if (index < static_cast<int>(router->configs.size()))
    {
        // An existing configuration, pick defaults from it.
        options.set_defaults(router->configs[index]);
    }
    else if (index != 0)
    {
        mxb_assert(index == static_cast<int>(router->configs.size()));
        // A new configuration, pick defaults from the default configuration.
        options.set_defaults(router->configs[0]);
        options.host.clear();
    }

    string host = options.host;
    options.host.clear();   // So that we can detect whether it is set, even to the same value.

    if (blr_parse_change_master_command(&cmd_string.front(),
                                        error,
                                        &options) != 0)
    {
        MXS_ERROR("%s CHANGE MASTER TO parse error: %s",
                  router->service->name,
                  error);

        return -1;
    }

    string use_gtid = options.use_mariadb10_gtid;

    if (!use_gtid.empty() && (strcasecmp(use_gtid.c_str(), "slave_pos") != 0))
    {
        static const char MESSAGE[] =
            "Only MASTER_USE_GTID=Slave_pos is allowed.";
        mxb_assert(sizeof(MESSAGE) <= BINLOG_ERROR_MSG_LEN);
        strcpy(error, MESSAGE);
        return -1;
    }

    ChangeMasterConfig config;
    if (!options.validate(router, error, &config))
    {
        return -1;
    }

    if (config.host.empty())    // Empty, if it was not specified in the options.
    {
        config.host = host;
    }

    if ((index == 0) && !options.host.empty())
    {
        // If we are manipulating the default configuration and a host is specified,
        // even if it would be the same, then we reset the setup.
        router->configs.clear();
    }

    return blr_apply_change_master(router, index, config, error);
}

/*
 * Set new master hostname
 *
 * @param router      Current router instance
 * @param hostname    The hostname to set
 * @return            1 for applied change, 0 otherwise
 */
static int blr_set_master_hostname(ROUTER_INSTANCE* router, const char* hostname)
{
    if (hostname)
    {
        mxb_assert((*hostname != '\'') && (*hostname != '"'));

        server_update_address(router->service->dbref->server, hostname);

        MXS_INFO("%s: New MASTER_HOST is [%s]",
                 router->service->name,
                 router->service->dbref->server->address);

        return 1;
    }

    return 0;
}

static int blr_set_master_hostname(ROUTER_INSTANCE* router, const std::string& hostname)
{
    return blr_set_master_hostname(router, hostname.empty() ? nullptr : hostname.c_str());
}

/**
 * Set new master port
 *
 * @param router    Current router instance
 * @param port      The server TCP port
 * @return      1 for applied change, 0 otherwise
 */

static int blr_set_master_port(ROUTER_INSTANCE* router, int port)
{
    if (port > 0)
    {
        server_update_port(router->service->dbref->server, port);

        MXS_INFO("%s: New MASTER_PORT is [%i]",
                 router->service->name,
                 router->service->dbref->server->port);

        return 1;
    }

    return 0;
}

/*
 * Set new master binlog file
 *
 * The routing must be called holding router->lock
 *
 * @param router      Current router instance
 * @param filename    Binlog file name
 * @param error       The error msg for command,
 *                    pre-allocated BINLOG_ERROR_MSG_LEN + 1 bytes
 * @return            New binlog file or NULL on error
 */
static char* blr_set_master_logfile(ROUTER_INSTANCE* router,
                                    const char* filename,
                                    char* error)
{
    char* new_binlog_file = NULL;

    if (filename)
    {
        mxb_assert((*filename != '\'') && (*filename != '"'));

        long next_binlog_seqname;
        char* file_ptr;
        char* end;

        file_ptr = (char*)filename;

        /* check binlog filename format */
        end = strchr(file_ptr, '.');

        if (!end)
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "Selected binlog [%s] is not in the format"
                     " '%s.yyyyyy'",
                     file_ptr,
                     router->fileroot);

            return NULL;
        }

        end++;

        if (router->master_state == BLRM_UNCONFIGURED)
        {
            char* stem_end;
            stem_end = strrchr(file_ptr, '.');
            /* set filestem */
            if (stem_end)
            {
                if (router->fileroot)
                {
                    MXS_FREE(router->fileroot);
                }
                router->fileroot = strndup(file_ptr, stem_end - file_ptr);
            }
        }
        else
        {
            /* get next binlog file name, assuming filestem is the same */
            next_binlog_seqname = blr_file_get_next_binlogname(router);

            if (!next_binlog_seqname)
            {
                snprintf(error,
                         BINLOG_ERROR_MSG_LEN,
                         "Cannot get the next MASTER_LOG_FILE name "
                         "from current binlog [%s]",
                         router->binlog_name);

                return NULL;
            }

            /* Compare binlog file name with current one */
            if (strcmp(router->binlog_name, file_ptr) == 0)
            {
                /**
                 * No binlog name change,
                 * a new position will be checked later
                 */
            }
            else
            {
                /**
                 * This is a new binlog file request
                 * If file is not the next one return an error
                 */
                if (atoi(end) != next_binlog_seqname)
                {
                    snprintf(error,
                             BINLOG_ERROR_MSG_LEN,
                             "Can not set MASTER_LOG_FILE to %s: "
                             "Permitted binlog file names are "
                             "%s or %s.%06li. Current master_log_file=%s, "
                             "master_log_pos=%lu",
                             file_ptr,
                             router->binlog_name,
                             router->fileroot,
                             next_binlog_seqname,
                             router->binlog_name,
                             router->current_pos);

                    return NULL;
                }

                /* Binlog file name succesfully changed */
            }
        }

        if (strlen(file_ptr) <= BINLOG_FNAMELEN)
        {
            new_binlog_file = MXS_STRDUP(file_ptr);
        }
        else
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "Can not set MASTER_LOG_FILE to %s: Maximum length is %d.",
                     file_ptr,
                     BINLOG_FNAMELEN);
        }
    }

    return new_binlog_file;
}

static char* blr_set_master_logfile(ROUTER_INSTANCE* router,
                                    const std::string& filename,
                                    char* error)
{
    return blr_set_master_logfile(router, filename.empty() ? nullptr : filename.c_str(), error);
}

/**
 * Get master configuration store it
 *
 * @param router        Current router instance
 * @param curr_master   Preallocated struct to fill
 */
static void blr_master_get_config(ROUTER_INSTANCE* router, MasterServerConfig* curr_master)
{
    SSL_LISTENER* server_ssl;

    curr_master->port = router->service->dbref->server->port;
    curr_master->host = router->service->dbref->server->address;
    curr_master->pos = router->current_pos;
    curr_master->safe_pos = router->binlog_position;
    curr_master->logfile = router->binlog_name;
    curr_master->user = router->user;
    curr_master->password = router->password;
    curr_master->filestem = router->fileroot;
    /* SSL options */
    if (router->service->dbref->server->server_ssl)
    {
        server_ssl = router->service->dbref->server->server_ssl;
        curr_master->ssl_enabled = router->ssl_enabled;
        if (router->ssl_version)
        {
            curr_master->ssl_version = router->ssl_version;
        }
        if (server_ssl->ssl_key)
        {
            curr_master->ssl_key = server_ssl->ssl_key;
        }
        if (server_ssl->ssl_cert)
        {
            curr_master->ssl_cert = server_ssl->ssl_cert;
        }
        if (server_ssl->ssl_ca_cert)
        {
            curr_master->ssl_ca = server_ssl->ssl_ca_cert;
        }
    }
    /* Connect options */
    curr_master->heartbeat = router->heartbeat;
}

/**
 * Restore master configuration values for host and port
 *
 * @param router        Current router instance
 * @param prev_master   Previous saved master configuration
 */
static void blr_master_restore_config(ROUTER_INSTANCE* router,
                                      const MasterServerConfig& prev_master)
{
    server_update_address(router->service->dbref->server, prev_master.host.c_str());
    server_update_port(router->service->dbref->server, prev_master.port);

    router->ssl_enabled = prev_master.ssl_enabled;
    if (!prev_master.ssl_version.empty())
    {
        MXS_FREE(router->ssl_version);
        router->ssl_version = MXS_STRDUP_A(prev_master.ssl_version.c_str());
    }

    router->heartbeat = prev_master.heartbeat;
}

/**
 * Set all the master configuration fields to empty values
 *
 * @param router    Current router instance
 */
static void blr_master_set_empty_config(ROUTER_INSTANCE* router)
{
    server_update_address(router->service->dbref->server, "none");
    server_update_port(router->service->dbref->server, (unsigned short)3306);

    router->current_pos = 4;
    router->binlog_position = 4;
    router->current_safe_event = 4;
    strcpy(router->binlog_name, "");
    strcpy(router->prevbinlog, "");
    /* Set Empty master id */
    router->orig_masterid = 0;
    /* Set Default GTID domain */
    router->mariadb10_gtid_domain = BLR_DEFAULT_GTID_DOMAIN_ID;
}

/**
 * Restore all master configuration values
 *
 * @param router        Current router instance
 * @param prev_master   Previous saved master configuration
 */
static void blr_master_apply_config(ROUTER_INSTANCE* router, const MasterServerConfig& prev_master)
{
    server_update_address(router->service->dbref->server, prev_master.host.c_str());
    server_update_port(router->service->dbref->server, prev_master.port);
    router->current_pos = prev_master.pos;
    router->binlog_position = prev_master.safe_pos;
    router->current_safe_event = prev_master.safe_pos;
    strcpy(router->binlog_name, prev_master.logfile.c_str());
    if (router->user)
    {
        MXS_FREE(router->user);
        router->user = MXS_STRDUP_A(prev_master.user.c_str());
    }
    if (router->password)
    {
        MXS_FREE(router->password);
        router->password = MXS_STRDUP_A(prev_master.password.c_str());
    }
    if (router->fileroot)
    {
        MXS_FREE(router->fileroot);
        router->fileroot = MXS_STRDUP_A(prev_master.filestem.c_str());
    }

    router->heartbeat = prev_master.heartbeat;
}

/**
 * Change the replication user
 *
 * @param router    Current router instance
 * @param user      The userto set
 * @return      1 for applied change, 0 otherwise
 */
static int blr_set_master_user(ROUTER_INSTANCE* router, const char* user)
{
    if (user != NULL)
    {
        mxb_assert((*user != '\'') && (*user != '"'));

        if (router->user)
        {
            MXS_FREE(router->user);
        }
        router->user = MXS_STRDUP_A(user);

        MXS_INFO("%s: New MASTER_USER is [%s]",
                 router->service->name,
                 router->user);

        return 1;
    }

    return 0;
}

static int blr_set_master_user(ROUTER_INSTANCE* router, const std::string& user)
{
    return blr_set_master_user(router, user.empty() ? nullptr : user.c_str());
}

/**
 * Change the replication password
 *
 * @param router    Current router instance
 * @param password  The password to set
 * @return      1 for applied change, 0 otherwise
 */
static int blr_set_master_password(ROUTER_INSTANCE* router, const char* password)
{
    if (password != NULL)
    {
        mxb_assert((*password != '\'') && (*password != '"'));

        if (router->password)
        {
            MXS_FREE(router->password);
        }
        router->password = MXS_STRDUP_A(password);

        /* don't log new password */

        return 1;
    }

    return 0;
}

static int blr_set_master_password(ROUTER_INSTANCE* router, const std::string& password)
{
    return blr_set_master_password(router, password.empty() ? nullptr : password.c_str());
}

/**
 * Get next token
 *
 * Works exactly like strtok_t except that a delim character which appears
 * anywhere within quotes is ignored. For instance, if delim is "," then
 * a string like "MASTER_USER='maxscale_repl_user',MASTER_PASSWORD='a,a'"
 * will be tokenized into the following two tokens:
 *
 *   MASTER_USER='maxscale_repl_user'
 *   MASTER_PASSWORD='a,a'
 *
 * @see strtok_r
 */
static char* get_next_token(char* str, const char* delim, char** saveptr)
{
    if (str)
    {
        *saveptr = str;
    }

    if (!*saveptr)
    {
        return NULL;
    }

    bool delim_found = true;

    // Skip any delims in the beginning.
    while (**saveptr && delim_found)
    {
        const char* d = delim;

        while (*d)
        {
            if (*d == **saveptr)
            {
                break;
            }

            ++d;
        }

        if (*d == 0)
        {
            delim_found = false;
        }
        else
        {
            ++*saveptr;
        }
    }

    if (!**saveptr)
    {
        return NULL;
    }

    delim_found = false;

    char* token = *saveptr;
    char* p = *saveptr;

    char quote = 0;

    while (*p && !delim_found)
    {
        switch (*p)
        {
        case '\'':
        case '"':
        case '`':
            if (!quote)
            {
                quote = *p;
            }
            else if (quote == *p)
            {
                quote = 0;
            }
            break;

        default:
            if (!quote)
            {
                const char* d = delim;
                while (*d && !delim_found)
                {
                    if (*p == *d)
                    {
                        delim_found = true;
                        *p = 0;
                    }
                    else
                    {
                        ++d;
                    }
                }
            }
        }

        ++p;
    }

    if (*p == 0)
    {
        *saveptr = NULL;
    }
    else if (delim_found)
    {
        *saveptr = p;

        delim_found = true;

        while (**saveptr && delim_found)
        {
            const char* d = delim;
            while (*d)
            {
                if (**saveptr == *d)
                {
                    break;
                }
                else
                {
                    ++d;
                }
            }

            if (*d == 0)
            {
                delim_found = false;
            }
            else
            {
                ++*saveptr;
            }
        }
    }

    return token;
}

/**
 * Parse a CHANGE MASTER TO SQL command
 *
 * @param input           The command to be parsed
 * @param error_string    Pre-allocated string for error message,
 *                        BINLOG_ERROR_MSG_LEN + 1 bytes
 * @param config          master option struct to fill
 * @return                0 on success, 1 on failure
 */
static int blr_parse_change_master_command(char* input,
                                           char* error_string,
                                           ChangeMasterOptions* config)
{
    const char* sep = ",";
    char* word, * brkb;

    if ((word = get_next_token(input, sep, &brkb)) == NULL)
    {
        snprintf(error_string,
                 BINLOG_ERROR_MSG_LEN,
                 "Unable to parse query [%s]",
                 input);
        return 1;
    }
    else
    {
        /* parse options key=val */
        if (blr_handle_change_master_token(word,
                                           error_string,
                                           config))
        {
            return 1;
        }
    }

    while ((word = get_next_token(NULL, sep, &brkb)) != NULL)
    {
        /* parse options key=val */
        if (blr_handle_change_master_token(word,
                                           error_string,
                                           config))
        {
            return 1;
        }
    }

    return 0;
}

/**
 * Validate option and set the value for a change master option
 *
 * @param input     Current option with value
 * @param error     pre-allocted string for error message,
 *                  BINLOG_ERROR_MSG_LEN + 1 bytes
 * @param config    master option struct to fill
 * @return          0 on success, 1 on error
 */
static int blr_handle_change_master_token(char* input,
                                          char* error,
                                          ChangeMasterOptions* config)
{
    /* space+TAB+= */
    const char* sep = " \t=";
    char* word, * brkb;

    if ((word = get_next_token(input, sep, &brkb)) == NULL)
    {
        snprintf(error,
                 BINLOG_ERROR_MSG_LEN,
                 "error parsing %s",
                 brkb);
        return 1;
    }
    else
    {
        std::string* option_field;

        if ((option_field = blr_validate_change_master_option(word, config)) == NULL)
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "option '%s' is not supported",
                     word);

            return 1;
        }

        std::string value;
        if (!blr_get_parsed_command_value(brkb, &value))
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "missing value for '%s'",
                     word);
            return 1;
        }
        else
        {
            *option_field = value;
        }
    }

    return 0;
}

/**
 * Get value of a change master option
 *
 * @param input    Current option with value
 * @return         The new allocated option value or NULL
 */
static bool blr_get_parsed_command_value(char* input, std::string* output)
{
    bool ret = false;

    if (input && *input)
    {
        char value[strlen(input) + 1];
        strcpy(value, input);

        /* space+TAB+= */
        const char* sep = " \t=";
        char* word;

        if ((word = get_next_token(NULL, sep, &input)) != NULL)
        {
            /* remove trailing spaces */
            char* ptr = value + strlen(value) - 1;
            while (ptr > value && isspace(*ptr))
            {
                *ptr-- = 0;
            }

            // Remove surrounding quotes.
            char* p = strstr(value, word);
            if (*p == '\'' || *p == '"')
            {
                char quote = *p;
                ++p;
                int len = strlen(p);
                if (len > 0 && p[len - 1] == quote)
                {
                    p[len - 1] = 0;
                }
            }

            *output = p;
            ret = true;
        }
    }

    return ret;
}

/**
 * Validate a change master option
 *
 * @param option    The option to check
 * @param config    The option structure
 * @return          A pointer to the field in the option strucure or NULL
 */
static std::string* blr_validate_change_master_option(const char* option, ChangeMasterOptions* config)
{
    if (strcasecmp(option, "master_host") == 0)
    {
        return &config->host;
    }
    else if (strcasecmp(option, "master_port") == 0)
    {
        return &config->port;
    }
    else if (strcasecmp(option, "master_log_file") == 0)
    {
        return &config->binlog_file;
    }
    else if (strcasecmp(option, "master_log_pos") == 0)
    {
        return &config->binlog_pos;
    }
    else if (strcasecmp(option, "master_user") == 0)
    {
        return &config->user;
    }
    else if (strcasecmp(option, "master_password") == 0)
    {
        return &config->password;
    }
    else if (strcasecmp(option, "master_ssl") == 0)
    {
        return &config->ssl_enabled;
    }
    else if (strcasecmp(option, "master_ssl_key") == 0)
    {
        return &config->ssl_key;
    }
    else if (strcasecmp(option, "master_ssl_cert") == 0)
    {
        return &config->ssl_cert;
    }
    else if (strcasecmp(option, "master_ssl_ca") == 0)
    {
        return &config->ssl_ca;
    }
    else if (strcasecmp(option, "master_ssl_version") == 0
             || strcasecmp(option, "master_tls_version") == 0)
    {
        return &config->ssl_version;
    }
    else if (strcasecmp(option, "master_use_gtid") == 0)
    {
        return &config->use_mariadb10_gtid;
    }
    else if (strcasecmp(option, "master_heartbeat_period") == 0)
    {
        return &config->heartbeat_period;
    }
    else if (strcasecmp(option, "master_connect_retry") == 0)
    {
        return &config->connect_retry;
    }
    else
    {
        return NULL;
    }
}

/**
 * Send a MySQL protocol response for selected variable
 *
 * @param    router         The binlog router instance
 * @param    slave          The connected slave server
 * @param    variable       The variable name
 * @param    value          The variable value
 * @param    column_type    The variable value type (string or int)
 * @return                  Non-zero if data was sent
 */
static int blr_slave_send_var_value(ROUTER_INSTANCE* router,
                                    ROUTER_SLAVE* slave,
                                    const char*   variable,
                                    const char*   value,
                                    int column_type)
{
    GWBUF* pkt;
    uint8_t* ptr;
    int len, vers_len;

    if (value == NULL)
    {
        return blr_slave_send_ok(router, slave);
    }

    vers_len = strlen(value);
    blr_slave_send_fieldcount(router, slave, 1);
    blr_slave_send_columndef(router,
                             slave,
                             variable,
                             column_type,
                             vers_len,
                             2);
    blr_slave_send_eof(router, slave, 3);

    len = MYSQL_HEADER_LEN + (1 + vers_len);
    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, vers_len + 1, 24);    // Add length of data packet
    ptr += 3;
    *ptr++ = 0x04;                      // Sequence number in response
    *ptr++ = vers_len;                  // Length of result string
    memcpy((char*)ptr, value, vers_len);// Result string

    /* ptr += vers_len; Not required unless more data is added */
    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);

    return blr_slave_send_eof(router, slave, 5);
}

/**
 * Send the response to the SQL command "SHOW VARIABLES LIKE 'xxx'
 *
 * @param    router         The binlog router instance
 * @param    slave          The connected slave server
 * @param    variable       The variable name
 * @param    value          The variable value
 * @param    column_type    The variable value type (string or int)
 * @return                  Non-zero if data was sent
 */
static int blr_slave_send_variable(ROUTER_INSTANCE* router,
                                   ROUTER_SLAVE* slave,
                                   const char*   variable,
                                   const char*   value,
                                   int column_type)
{
    GWBUF* pkt;
    uint8_t* ptr;
    int len, vers_len, seqno = 2;
    char* p = MXS_STRDUP_A(variable);
    int var_len;
    char* old_ptr = p;

    if (value == NULL)
    {
        return 0;
    }

    /* Remove heading and trailing "'" */
    if (*p == '\'')
    {
        p++;
    }
    if (p[strlen(p) - 1] == '\'')
    {
        p[strlen(p) - 1] = '\0';
    }

    var_len = strlen(p);

    /* force lowercase */
    for (int i = 0; i < var_len; i++)
    {
        p[i] = tolower(p[i]);
    }

    blr_slave_send_fieldcount(router, slave, 2);

    blr_slave_send_columndef_with_info_schema(router,
                                              slave,
                                              "Variable_name",
                                              BLR_TYPE_STRING,
                                              40,
                                              seqno++);
    blr_slave_send_columndef_with_info_schema(router,
                                              slave,
                                              "Value",
                                              column_type,
                                              40,
                                              seqno++);

    blr_slave_send_eof(router, slave, seqno++);

    vers_len = strlen(value);
    len = MYSQL_HEADER_LEN + (1 + vers_len) + (1 + var_len);
    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, vers_len + 2 + var_len, 24);      // Add length of data packet
    ptr += 3;
    *ptr++ = seqno++;               // Sequence number in response
    *ptr++ = var_len;               // Length of result string
    memcpy((char*)ptr, p, var_len); // Result string with var name
    ptr += var_len;
    *ptr++ = vers_len;                  // Length of result string
    memcpy((char*)ptr, value, vers_len);// Result string with var value
    /* ptr += vers_len; Not required unless more data is added */
    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);

    MXS_FREE(old_ptr);

    return blr_slave_send_eof(router, slave, seqno++);
}

/**
 * Send the column definition packet for a variable
 * in a response packet sequence.
 *
 * It adds information_schema and variables and variable_name
 *
 * @param router    The router
 * @param slave     The slave connection
 * @param name      Name of the column
 * @param type      Column type
 * @param len       Column length
 * @param seqno     Packet sequence number
 * @return          Non-zero on success
 */
static int blr_slave_send_columndef_with_info_schema(ROUTER_INSTANCE* router,
                                                     ROUTER_SLAVE* slave,
                                                     const char*   name,
                                                     int type,
                                                     int len,
                                                     uint8_t seqno)
{
    GWBUF* pkt;
    uint8_t* ptr;
    int info_len = strlen("information_schema");
    int virtual_table_name_len = strlen("VARIABLES");
    int table_name_len = strlen("VARIABLES");
    int column_name_len = strlen(name);
    int orig_column_name_len = strlen("VARIABLE_NAME");
    int packet_data_len = 22 + strlen(name) + info_len
        + virtual_table_name_len + table_name_len
        + orig_column_name_len;

    if ((pkt = gwbuf_alloc(MYSQL_HEADER_LEN + packet_data_len)) == NULL)
    {
        return 0;
    }

    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, packet_data_len, 24);     // Add length of data packet
    ptr += 3;
    *ptr++ = seqno;                 // Sequence number in response
    *ptr++ = 3;                     // Catalog is always def
    *ptr++ = 'd';
    *ptr++ = 'e';
    *ptr++ = 'f';
    *ptr++ = info_len;              // Schema name length
    strcpy((char*)ptr, "information_schema");
    ptr += info_len;
    *ptr++ = virtual_table_name_len;        // virtual table name length
    strcpy((char*)ptr, "VARIABLES");
    ptr += virtual_table_name_len;
    *ptr++ = table_name_len;                // Table name length
    strcpy((char*)ptr, "VARIABLES");
    ptr += table_name_len;
    *ptr++ = column_name_len;               // Column name length;
    while (*name)
    {
        *ptr++ = *name++;                   // Copy the column name
    }
    *ptr++ = orig_column_name_len;          // Orginal column name
    strcpy((char*)ptr, "VARIABLE_NAME");
    ptr += orig_column_name_len;
    *ptr++ = 0x0c;                  // Length of next fields always 12
    *ptr++ = 0x3f;                  // Character set
    *ptr++ = 0;
    encode_value(ptr, len, 32);             // Add length of column
    ptr += 4;
    *ptr++ = type;
    *ptr++ = 0x81;                          // Two bytes of flags
    if (type == 0xfd)
    {
        *ptr++ = 0x1f;
    }
    else
    {
        *ptr++ = 0x00;
    }
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;

    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}

static char* bypass_change_master(char* input)
{
    // Very simple, basically ASSUMES there is a "CHANGE MASTER" at the beginning.

    static const char CHANGE[] = "change";

    char* p = strcasestr(input, CHANGE);
    if (p)
    {
        input = p + sizeof(CHANGE) - 1;

        static const char MASTER[] = "master";

        p = strcasestr(input, MASTER);
        if (p)
        {
            input = p + sizeof(MASTER) - 1;
        }
    }

    return input;
}

/**
 * Interface for testing blr_parse_change_master_command()
 *
 * @param input           The command to be parsed
 * @param error_string    Pre-allocated string for error message,
 *                        BINLOG_ERROR_MSG_LEN + 1 bytes
 * @param config          master option struct to fill
 * @return                0 on success, 1 on failure
 */
int blr_test_parse_change_master_command(char* input,
                                         char* error_string,
                                         ChangeMasterOptions* config)
{
    return blr_parse_change_master_command(bypass_change_master(input),
                                           error_string,
                                           config);
}

/*
 * Interface for testing set new master binlog file
 *
 *
 * @param router      Current router instance
 * @param filename    Binlog file name
 * @param error       The error msg for command,
 *                    pre-allocated BINLOG_ERROR_MSG_LEN + 1 bytes
 * @return            New binlog file or NULL on error
 */
char* blr_test_set_master_logfile(ROUTER_INSTANCE* router,
                                  const char* filename,
                                  char* error)
{
    return blr_set_master_logfile(router, filename, error);
}

/**
 * Interface for testing a 'change master' operation
 *
 * @param router    The router instance
 * @param command   The change master SQL command
 * @param error     The error message,
 *                  preallocated BINLOG_ERROR_MSG_LEN + 1 bytes
 * @return          0 on success, 1 on success with new binlog, -1 on failure
 */
int blr_test_handle_change_master(ROUTER_INSTANCE* router,
                                  char* command,
                                  char* error)
{
    return blr_handle_change_master(router, bypass_change_master(command), error);
}


/**
 * Handle the response to the SQL command
 * "SHOW GLOBAL VARIABLES LIKE or SHOW VARIABLES LIKE
 *
 * @param    router    The binlog router instance
 * @param    slave     The connected slave server
 * @param    stmt      The SQL statement
 * @return             Non-zero if the variable is handled,
 *                     0 if variable is unknown, -1 for syntax error
 */
static int blr_slave_handle_variables(ROUTER_INSTANCE* router,
                                      ROUTER_SLAVE* slave,
                                      char* stmt)
{
    char* brkb;
    char* word;
    /* SPACE,TAB,= */
    const char* sep = "     ,=";

    if ((word = strtok_r(stmt, sep, &brkb)) == NULL)
    {
        return -1;
    }
    else if (strcasecmp(word, "LIKE") == 0)
    {
        if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Missing LIKE clause in SHOW [GLOBAL] VARIABLES.",
                      router->service->name);
            return -1;
        }
        else if (strcasecmp(word, "'SERVER_ID'") == 0)
        {
            if (router->set_master_server_id)
            {
                char server_id[40];
                sprintf(server_id, "%d", router->masterid);
                return blr_slave_send_variable(router,
                                               slave,
                                               "'SERVER_ID'",
                                               server_id,
                                               BLR_TYPE_INT);
            }
            else
            {
                return blr_slave_replay(router,
                                        slave,
                                        router->saved_master.server_id);
            }
        }
        else if (strcasecmp(word, "'SERVER_UUID'") == 0)
        {
            if (router->set_master_uuid)
            {
                return blr_slave_send_variable(router,
                                               slave,
                                               "'SERVER_UUID'",
                                               router->master_uuid,
                                               BLR_TYPE_STRING);
            }
            else
            {
                return blr_slave_replay(router,
                                        slave,
                                        router->saved_master.uuid);
            }
        }
        else if (strcasecmp(word, "'MAXSCALE%'") == 0)
        {
            return blr_slave_send_maxscale_variables(router, slave);
        }
        else
        {
            return 0;
        }
    }
    else
    {
        return -1;
    }
}

/**
 * Send a MySQL OK packet with a warning flag to the slave backend
 * and set the warning message in slave structure
 * The message should be retrieved by SHOW WARNINGS command
 *
 * @param    router     The binlog router instance
 * @param    message    The message to send
 * @param    slave      The slave server to which we are sending the response
 *
 * @return              The write call result: non-zero if write was successful
 */

static int blr_slave_send_warning_message(ROUTER_INSTANCE* router,
                                          ROUTER_SLAVE* slave,
                                          const char*   message)
{
    GWBUF* pkt;
    uint8_t* ptr;

    if ((pkt = gwbuf_alloc(11)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    *ptr++ = 7;     // Payload length
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 1;     // Seqno
    *ptr++ = 0;     // ok
    *ptr++ = 0;
    *ptr++ = 0;

    *ptr++ = 2;
    *ptr++ = 0;

    if (strlen(message) == 0)
    {
        *ptr++ = 0;
        *ptr++ = 0;
    }
    else
    {
        *ptr++ = 1;     /* warning byte set to 1 */
        *ptr++ = 0;
    }

    /* set the new warning in this slave connection */
    if (slave->warning_msg)
    {
        MXS_FREE(slave->warning_msg);
    }
    slave->warning_msg = MXS_STRDUP_A(message);

    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}

/**
 * Send a MySQL SHOW WARNINGS packet with a message
 * that has been stored in slave struct.
 *
 * If there is no warning message an OK packet is sent
 *
 * @param    router     The binlog router instance
 * @param    message    The message to send
 * @param    slave      The slave server to which we are sending the response
 *
 * @return              The write call result: non-zero if write was successful
 */

static int blr_slave_show_warnings(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    GWBUF* pkt;
    uint8_t* ptr;
    int len;
    int msg_len = 0;
    int code_len = 0;
    int level_len = 0;

    /* check whether a warning message is available */
    if (slave->warning_msg)
    {
        const char* level = "Warning";
        char* msg_ptr;
        char err_code[16 + 1] = "";
        msg_ptr = strchr(slave->warning_msg, ':');
        if (msg_ptr)
        {
            size_t len = (msg_ptr - slave->warning_msg > 16) ?
                16 :
                (msg_ptr - slave->warning_msg);
            memcpy(err_code, slave->warning_msg, len);
            err_code[len] = 0;
            code_len = strlen(err_code);

            msg_ptr++;
        }
        else
        {
            msg_ptr = slave->warning_msg;
        }

        msg_len = strlen(msg_ptr);
        level_len = strlen(level);

        blr_slave_send_fieldcount(router, slave, 3);    // 3 columns

        blr_slave_send_columndef(router, slave, "Level", BLR_TYPE_STRING, 40, 2);
        blr_slave_send_columndef(router, slave, "Code", BLR_TYPE_STRING, 40, 3);
        blr_slave_send_columndef(router, slave, "Message", BLR_TYPE_STRING, 80, 4);

        blr_slave_send_eof(router, slave, 5);

        len = MYSQL_HEADER_LEN + (1 + level_len) + (1 + code_len) + (1 + msg_len);

        if ((pkt = gwbuf_alloc(len)) == NULL)
        {
            return blr_slave_send_ok(router, slave);
        }

        ptr = GWBUF_DATA(pkt);

        encode_value(ptr, len - MYSQL_HEADER_LEN, 24);      // Add length of data packet
        ptr += 3;

        *ptr++ = 0x06;              // Sequence number in response

        *ptr++ = level_len;                     // Length of result string
        memcpy((char*)ptr, level, level_len);   // Result string
        ptr += level_len;

        *ptr++ = code_len;          // Length of result string
        if (code_len)
        {
            memcpy((char*)ptr, err_code, code_len);     // Result string
            ptr += code_len;
        }

        *ptr++ = msg_len;           // Length of result string
        if (msg_len)
        {
            memcpy((char*)ptr, msg_ptr, msg_len);   // Result string
            /* ptr += msg_len; Not required unless more data is added */
        }

        MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);

        return blr_slave_send_eof(router, slave, 7);
    }
    else
    {
        return blr_slave_send_ok(router, slave);
    }
}

/**
 * Handle the response to the SQL command
 * "SHOW [GLOBAL] STATUS LIKE or SHOW STATUS LIKE
 *
 * @param    router    The binlog router instance
 * @param    slave     The slave server to which we are sending the response
 * @param    stmt      The SQL statement
 * @return             Non-zero if the variable is handled,
 *                     0 if variable is unknown, -1 for syntax errors.
 */
static int blr_slave_handle_status_variables(ROUTER_INSTANCE* router,
                                             ROUTER_SLAVE* slave,
                                             char* stmt)
{
    char* brkb = NULL;
    char* word = NULL;
    /* SPACE,TAB,= */
    const char* sep = "     ,=";

    if ((word = strtok_r(stmt, sep, &brkb)) == NULL)
    {
        return -1;
    }
    else if (strcasecmp(word, "LIKE") == 0)
    {
        if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Missing LIKE clause in SHOW [GLOBAL] STATUS.",
                      router->service->name);
            return -1;
        }
        else if (strcasecmp(word, "'Uptime'") == 0)
        {
            char uptime[41] = "";
            snprintf(uptime, 40, "%d", maxscale_uptime());
            return blr_slave_send_status_variable(router,
                                                  slave,
                                                  "Uptime",
                                                  uptime,
                                                  BLR_TYPE_INT);
        }
        else if (strcasecmp(word, "'slave_received_heartbeats'") == 0)
        {
            char hkbeats[41] = "";
            snprintf(hkbeats, 40, "%d", router->stats.n_heartbeats);
            return blr_slave_send_status_variable(router,
                                                  slave,
                                                  "Slave_received_heartbeats",
                                                  hkbeats,
                                                  BLR_TYPE_INT);
        }
        else
        {
            return 0;
        }
    }
    else
    {
        return -1;
    }
}

/**
 * Send the response to the SQL command "SHOW [GLOBAL] STATUS LIKE 'xxx'
 *
 * @param    router         The binlog router instance
 * @param    slave          The connected slave server
 * @param    variable       The variable name
 * @param    value          The variable value
 * @param    column_type    The variable value type (string or int)
 * @return                  Non-zero if data was sent
 */
static int blr_slave_send_status_variable(ROUTER_INSTANCE* router,
                                          ROUTER_SLAVE* slave,
                                          const char*   variable,
                                          const char*   value,
                                          int column_type)
{
    GWBUF* pkt;
    uint8_t* ptr;
    int len, vers_len, seqno = 2;
    char* p = MXS_STRDUP_A(variable);
    int var_len;
    char* old_ptr = p;

    /* Remove heading and trailing "'" */
    if (*p == '\'')
    {
        p++;
    }
    if (p[strlen(p) - 1] == '\'')
    {
        p[strlen(p) - 1] = '\0';
    }

    var_len = strlen(p);

    /* force lowercase */
    for (int i = 0; i < var_len; i++)
    {
        p[i] = tolower(p[i]);
    }

    /* First char is uppercase */
    p[0] = toupper(p[0]);

    blr_slave_send_fieldcount(router, slave, 2);

    blr_slave_send_columndef_with_status_schema(router,
                                                slave,
                                                "Variable_name",
                                                BLR_TYPE_STRING,
                                                40,
                                                seqno++);
    blr_slave_send_columndef_with_status_schema(router,
                                                slave,
                                                "Value",
                                                column_type,
                                                40,
                                                seqno++);

    blr_slave_send_eof(router, slave, seqno++);

    vers_len = strlen(value);
    len = MYSQL_HEADER_LEN + (1 + vers_len) + (1 + var_len);
    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return 0;
    }
    ptr = GWBUF_DATA(pkt);
    // Add length of data packet
    encode_value(ptr, vers_len + 2 + var_len, 24);
    ptr += 3;
    // Sequence number in response
    *ptr++ = seqno++;
    // Length of result string
    *ptr++ = var_len;
    // Result string with var name
    memcpy((char*)ptr, p, var_len);
    ptr += var_len;
    // Length of result string
    *ptr++ = vers_len;
    // Result string with var value
    memcpy((char*)ptr, value, vers_len);

    /* ptr += vers_len; Not required unless more data is added */
    MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);

    MXS_FREE(old_ptr);

    return blr_slave_send_eof(router, slave, seqno++);
}

/**
 * Send the column definition packet for a STATUS variable
 * in a response packet sequence.
 *
 * It adds information_schema.STATUS and variables and variable_name
 *
 * @param router    The router
 * @param slave     The slave connection
 * @param name      Name of the column
 * @param type      Column type
 * @param len       Column length
 * @param seqno     Packet sequence number
 * @return          Non-zero on success
 */
static int blr_slave_send_columndef_with_status_schema(ROUTER_INSTANCE* router,
                                                       ROUTER_SLAVE* slave,
                                                       const char*   name,
                                                       int type,
                                                       int len,
                                                       uint8_t seqno)
{
    GWBUF* pkt;
    uint8_t* ptr;
    int info_len = strlen("information_schema");
    int virtual_table_name_len = strlen("STATUS");
    int table_name_len = strlen("STATUS");
    int column_name_len = strlen(name);
    int orig_column_name_len = strlen("VARIABLE_NAME");
    int packet_data_len = 0;
    const char* ptr_name_start = name;

    if (strcasecmp(ptr_name_start, "value") == 0)
    {
        orig_column_name_len = strlen("VARIABLE_VALUE");
    }

    packet_data_len = 22 + strlen(name) + info_len + virtual_table_name_len
        + table_name_len + orig_column_name_len;

    if ((pkt = gwbuf_alloc(MYSQL_HEADER_LEN + packet_data_len)) == NULL)
    {
        return 0;
    }

    ptr = GWBUF_DATA(pkt);
    encode_value(ptr, packet_data_len, 24);     // Add length of data packet
    ptr += 3;
    *ptr++ = seqno;             // Sequence number in response
    *ptr++ = 3;                 // Catalog is always def
    *ptr++ = 'd';
    *ptr++ = 'e';
    *ptr++ = 'f';
    *ptr++ = info_len;          // Schema name length
    strcpy((char*)ptr, "information_schema");
    ptr += info_len;
    *ptr++ = virtual_table_name_len;    // virtual table name length
    strcpy((char*)ptr, "STATUS");
    ptr += virtual_table_name_len;
    *ptr++ = table_name_len;            // Table name length
    strcpy((char*)ptr, "STATUS");
    ptr += table_name_len;
    *ptr++ = column_name_len;           // Column name length;
    while (*name)
    {
        *ptr++ = *name++;               // Copy the column name
    }
    *ptr++ = orig_column_name_len;      // Orginal column name

    if (strcasecmp(ptr_name_start, "value") == 0)
    {
        strcpy((char*)ptr, "VARIABLE_VALUE");
    }
    else
    {
        strcpy((char*)ptr, "VARIABLE_NAME");
    }
    ptr += orig_column_name_len;
    *ptr++ = 0x0c;                  // Length of next fields always 12
    *ptr++ = 0x3f;                  // Character set
    *ptr++ = 0;
    encode_value(ptr, len, 32);     // Add length of column
    ptr += 4;
    *ptr++ = type;
    *ptr++ = 0x81;                  // Two bytes of flags
    if (type == 0xfd)
    {
        *ptr++ = 0x1f;
    }
    else
    {
        *ptr++ = 0x00;
    }
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;

    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
}

/**
 * The heartbeat check function called
 * from the housekeeper for registered slaves.
 *
 * @param router        Current router instance
 */

static bool blr_send_slave_heartbeat(void* inst)
{
    ROUTER_SLAVE* sptr = NULL;
    ROUTER_INSTANCE* router = (ROUTER_INSTANCE*) inst;
    time_t t_now = time(0);

    pthread_mutex_lock(&router->lock);

    sptr = router->slaves;

    while (sptr)
    {

        /* skip servers with state = 0 */
        if ((sptr->state == BLRS_DUMPING)
            && (sptr->heartbeat > 0)
            && ((t_now + 1 - sptr->lastReply) >= sptr->heartbeat))
        {
            MXS_NOTICE("Sending Heartbeat to slave server-id %d. "
                       "Heartbeat interval is %d, last event time is %lu",
                       sptr->serverid,
                       sptr->heartbeat,
                       (unsigned long)sptr->lastReply);

            if (blr_slave_send_heartbeat(router, sptr))
            {
                /* Set last event */
                sptr->lastEventReceived = HEARTBEAT_EVENT;
                /* Set last time */
                sptr->lastReply = t_now;
            }
        }

        sptr = sptr->next;
    }

    pthread_mutex_unlock(&router->lock);

    return true;
}

/**
 * Create and send an hearbeat packet to be sent to a registered slave server
 *
 * @param router    The current route rinstance
 * @param slave     The current slave connection
 * @return          Number of bytes sent or 0 in case of failure
 */
static int blr_slave_send_heartbeat(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
    REP_HEADER hdr;
    GWBUF* h_event;
    uint8_t* ptr;
    int len = BINLOG_EVENT_HDR_LEN;
    uint32_t chksum;
    int filename_len = strlen(slave->binlog_name);

    /* Add CRC32 4 bytes */
    if (!slave->nocrc)
    {
        len += BINLOG_EVENT_CRC_SIZE;
    }

    /* add binlogname to data content len */
    len += filename_len;

    /**
     * Alloc buffer for network binlog stream:
     *
     * 4 bytes header (3 for pkt len + 1 seq.no)
     * 1 byte for Ok / ERR
     * n bytes data content
     *
     * Total = 5 bytes + len
     */
    if ((h_event = gwbuf_alloc(MYSQL_HEADER_LEN + 1 + len)) == NULL)
    {
        return 0;
    }

    /* The OK/Err byte is part of payload */
    hdr.payload_len = len + 1;

    /* Add sequence no */
    hdr.seqno = slave->seqno++;

    /* Add OK */
    hdr.ok = 0;

    /* Add timestamp: 0 */
    hdr.timestamp = 0L;

    /* Set Event Type */
    hdr.event_type = HEARTBEAT_EVENT;

    /* Add master server id */
    hdr.serverid = router->masterid;

    /* Add event size */
    hdr.event_size = len;

    /* Add Next Pos */
    hdr.next_pos = slave->binlog_pos;

    /* Add Artificial flags */
    hdr.flags = 0x20;

    /* point just after the header */
    ptr = blr_build_header(h_event, &hdr);

    /* Copy binlog name */
    memcpy(ptr, slave->binlog_name, filename_len);

    ptr += filename_len;

    /* Add the CRC32 */
    if (!slave->nocrc)
    {
        chksum = crc32(0L, NULL, 0);
        chksum = crc32(chksum,
                       GWBUF_DATA(h_event) + MYSQL_HEADER_LEN + 1,
                       hdr.event_size - BINLOG_EVENT_CRC_SIZE);
        encode_value(ptr, chksum, 32);
    }

    /* Write the packet */
    return MXS_SESSION_ROUTE_REPLY(slave->dcb->session, h_event);
}

/**
 *  Change the replication SSL options
 *
 * @param    router           Current router instance
 * @param    config           The current config
 * @param    error_message    Pre-allocated string for error message is
 *                            BINLOG_ERROR_MSG_LEN + 1 bytes
 * @return                    1 for applied change,
 *                            0 no changes and -1 for errors
 */
static int blr_set_master_ssl(ROUTER_INSTANCE* router,
                              const ChangeMasterConfig& config,
                              char* error_message)
{
    SSL_LISTENER* server_ssl = NULL;
    int updated = 0;

    if (config.ssl_enabled)
    {
        router->ssl_enabled = config.ssl_enabled;
        updated++;
    }

    if (router->ssl_enabled == false)
    {
        /* Free SSL struct */
        blr_free_ssl_data(router);
    }
    else
    {
        /* Check for existing SSL struct */
        if (router->service->dbref->server->server_ssl)
        {
            server_ssl = router->service->dbref->server->server_ssl;
            server_ssl->ssl_init_done = false;
        }
        else
        {
            /* Allocate SSL struct for backend connection */
            server_ssl = static_cast<SSL_LISTENER*>(MXS_CALLOC(1, sizeof(SSL_LISTENER)));
            if (server_ssl == NULL)
            {
                router->ssl_enabled = false;

                /* Report back the error */
                snprintf(error_message,
                         BINLOG_ERROR_MSG_LEN,
                         "CHANGE MASTER TO: Error allocating memory for SSL struct"
                         " in blr_set_master_ssl");

                return -1;
            }

            /* Set some SSL defaults */
            server_ssl->ssl_init_done = false;
            server_ssl->ssl_method_type = SERVICE_SSL_TLS_MAX;
            server_ssl->ssl_cert_verify_depth = 9;

            /* Set the pointer */
            router->service->dbref->server->server_ssl = server_ssl;
        }
    }

    /* Update options in router fields and in server_ssl struct, if present */
    if (!config.ssl_key.empty())
    {
        mxb_assert((config.ssl_key.front() != '\'') && (config.ssl_key.front() != '"'));

        if (server_ssl)
        {
            MXS_FREE(server_ssl->ssl_key);
            server_ssl->ssl_key = MXS_STRDUP_A(config.ssl_key.c_str());
        }
        MXS_FREE(router->ssl_key);
        router->ssl_key = MXS_STRDUP_A(config.ssl_key.c_str());
        updated++;
    }
    if (!config.ssl_ca.empty())
    {
        mxb_assert((config.ssl_ca.front() != '\'') && (config.ssl_ca.front() != '"'));

        if (server_ssl)
        {
            MXS_FREE(server_ssl->ssl_ca_cert);
            server_ssl->ssl_ca_cert = MXS_STRDUP_A(config.ssl_ca.c_str());
        }
        MXS_FREE(router->ssl_ca);
        router->ssl_ca = MXS_STRDUP_A(config.ssl_ca.c_str());
        updated++;
    }
    if (!config.ssl_cert.empty())
    {
        mxb_assert((config.ssl_cert.front() != '\'') && (config.ssl_cert.front() != '"'));

        if (server_ssl)
        {
            MXS_FREE(server_ssl->ssl_cert);
            server_ssl->ssl_cert = MXS_STRDUP_A(config.ssl_cert.c_str());
        }
        MXS_FREE(router->ssl_cert);
        router->ssl_cert = MXS_STRDUP_A(config.ssl_cert.c_str());
        updated++;
    }

    if (!config.ssl_version.empty() && server_ssl)
    {
        mxb_assert((config.ssl_version.front() != '\'') && (config.ssl_version.front() != '"'));

        if (!config.ssl_version.empty())
        {
            if (listener_set_ssl_version(server_ssl, config.ssl_version.c_str()) != 0)
            {
                /* Report back the error */
                snprintf(error_message,
                         BINLOG_ERROR_MSG_LEN,
                         "Unknown parameter value for 'ssl_version': %s",
                         config.ssl_version.c_str());
                return -1;
            }
            /* Set provided ssl_version in router SSL cfg anyway */
            MXS_FREE(router->ssl_version);
            router->ssl_version = MXS_STRDUP_A(config.ssl_version.c_str());
            updated++;
        }
    }

    if (updated)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

/**
 * Notify a waiting slave that new events are stored in binlog file
 *
 * @param    slave    The current connected slave
 * @return            True if slave has been notified
 *
 */
bool blr_notify_waiting_slave(ROUTER_SLAVE* slave)
{
    bool ret = false;
    pthread_mutex_lock(&slave->catch_lock);
    if (slave->cstate & CS_WAIT_DATA)
    {
        ret = true;
        /* Add fake event that will call the blr_slave_callback routine */
        poll_fake_write_event(slave->dcb);
        slave->cstate &= ~CS_WAIT_DATA;
    }
    pthread_mutex_unlock(&slave->catch_lock);

    return ret;
}

/**
 * Read MXS_START_ENCRYPTION_EVENT, after FDE
 *
 * @param router         The router instance
 * @param slave          The connected slave server
 * @param fde_end_pos    The position of MXS_START_ENCRYPTION_EVENT, after FDE
 * @return               Non zero on success
 */
static int blr_slave_read_ste(ROUTER_INSTANCE* router,
                              ROUTER_SLAVE* slave,
                              uint32_t fde_end_pos)
{
    REP_HEADER hdr;
    GWBUF* record, * head;
    uint8_t* ptr;
    uint32_t chksum;
    char err_msg[BINLOG_ERROR_MSG_LEN + 1];
    MARIADB_GTID_INFO* f_tree = router->storage_type == BLR_BINLOG_STORAGE_TREE ?
        &slave->f_info :
        NULL;

    err_msg[BINLOG_ERROR_MSG_LEN] = '\0';

    memset(&hdr, 0, BINLOG_EVENT_HDR_LEN);

    BLFILE* file;
    if ((file = blr_open_binlog(router,
                                slave->binlog_name,
                                f_tree)) == NULL)
    {
        return 0;
    }
    /* Start Encryption Event is not encrypted, we pass NULL to last param */
    if ((record = blr_read_binlog(router,
                                  file,
                                  fde_end_pos,
                                  &hdr,
                                  err_msg,
                                  NULL)) == NULL)
    {
        if (hdr.ok != SLAVE_POS_READ_OK)
        {
            MXS_ERROR("Slave %s:%i, server-id %d, binlog '%s', "
                      "blr_read_binlog failure: %s",
                      slave->dcb->remote,
                      dcb_get_port(slave->dcb),
                      slave->serverid,
                      slave->binlog_name,
                      err_msg);
        }

        blr_close_binlog(router, file);
        return 0;
    }

    blr_close_binlog(router, file);

    /* check for MXS_START_ENCRYPTION_EVENT */
    if (hdr.event_type == MARIADB10_START_ENCRYPTION_EVENT)
    {
        uint8_t* record_ptr = GWBUF_DATA(record);
        void* mem = MXS_CALLOC(1, sizeof(SLAVE_ENCRYPTION_CTX));
        SLAVE_ENCRYPTION_CTX* new_encryption_ctx =
            static_cast<SLAVE_ENCRYPTION_CTX*>(mem);

        if (!new_encryption_ctx)
        {
            gwbuf_free(record);
            return 0;
        }
        record_ptr += BINLOG_EVENT_HDR_LEN;
        // Set schema, 1 Byte
        new_encryption_ctx->binlog_crypto_scheme = record_ptr[0];
        // Set key version
        memcpy(&new_encryption_ctx->binlog_key_version,
               record_ptr + 1,
               BLRM_KEY_VERSION_LENGTH);
        // Set nonce
        memcpy(new_encryption_ctx->nonce,
               record_ptr + 1 + BLRM_KEY_VERSION_LENGTH,
               BLRM_NONCE_LENGTH);

        /* Set the pos of first encrypted event */
        new_encryption_ctx->first_enc_event_pos = fde_end_pos + hdr.event_size;

        pthread_mutex_lock(&slave->catch_lock);

        SLAVE_ENCRYPTION_CTX* old_encryption_ctx = slave->encryption_ctx;
        /* Set the new encryption ctx into slave */
        slave->encryption_ctx = new_encryption_ctx;

        pthread_mutex_unlock(&slave->catch_lock);

        /* Free previous encryption ctx */
        MXS_FREE(old_encryption_ctx);

        MXS_INFO("Start Encryption event found. Binlog %s is encrypted. "
                 "First event at %lu",
                 slave->binlog_name,
                 (unsigned long)fde_end_pos + hdr.event_size);
        /**
         * Note: if the requested pos is equal to MXS_START_ENCRYPTION_EVENT pos
         * the event will be skipped by blr_read_binlog() routine
         */
        gwbuf_free(record);
        return 1;
    }

    gwbuf_free(record);
    return 0;
}

/**
 * Handle received SELECT statements from clients
 *
 * if a SELECT statement is one of suported one
 * a proper reply to the connected client is done
 *
 * @param    router         Router instance
 * @param    slave          Connected client/slave server
 * @param    select_stmt    The SELECT statement
 * @return                  True for handled queries, False otherwise
 */
static bool blr_handle_simple_select_stmt(ROUTER_INSTANCE* router,
                                          ROUTER_SLAVE* slave,
                                          char* select_stmt)
{
    char* word;
    char* brkb;
    const char* sep = " \t,=";

    if ((word = strtok_r(select_stmt, sep, &brkb)) == NULL)
    {
        MXS_ERROR("%s: Incomplete select query.", router->service->name);
        return false;
    }
    else if (strcasecmp(word, "UNIX_TIMESTAMP()") == 0)
    {
        blr_slave_send_timestamp(router, slave);
        return true;
    }
    else if (strcasecmp(word, "@master_binlog_checksum") == 0
             || strcasecmp(word, "@@global.binlog_checksum") == 0)
    {
        blr_slave_replay(router, slave, router->saved_master.chksum2);
        return true;
    }
    else if (strcasecmp(word, "@@GLOBAL.GTID_MODE") == 0)
    {
        if (router->saved_master.gtid_mode)
        {
            blr_slave_replay(router, slave, router->saved_master.gtid_mode);
        }
        else
        {
            blr_slave_send_var_value(router,
                                     slave,
                                     "@@GLOBAL.GTID_MODE",
                                     "OFF",
                                     BLR_TYPE_STRING);
        }
        return true;
    }
    else if (strcasecmp(word, "1") == 0)
    {
        blr_slave_replay(router, slave, router->saved_master.select1);
        return true;
    }
    else if (strcasecmp(word, "VERSION()") == 0)
    {
        if (router->set_master_version)
        {
            blr_slave_send_var_value(router,
                                     slave,
                                     "VERSION()",
                                     router->set_master_version,
                                     BLR_TYPE_STRING);
            return true;
        }
        else
        {
            blr_slave_replay(router, slave, router->saved_master.selectver);
            return true;
        }
    }
    else if (strcasecmp(word, "USER()") == 0)
    {
        /* Return user@host */
        char user_host[MYSQL_USER_MAXLEN + 1 + MYSQL_HOST_MAXLEN + 1] = "";

        snprintf(user_host,
                 sizeof(user_host),
                 "%s@%s",
                 slave->dcb->user,
                 slave->dcb->remote);

        blr_slave_send_var_value(router,
                                 slave,
                                 "USER()",
                                 user_host,
                                 BLR_TYPE_STRING);
        return true;
    }
    else if (strcasecmp(word, "@@version") == 0)
    {
        if (router->set_master_version)
        {
            blr_slave_send_var_value(router,
                                     slave,
                                     "@@version",
                                     router->set_master_version,
                                     BLR_TYPE_STRING);
            return true;
        }
        else
        {
            char* version = blr_extract_column(router->saved_master.selectver,
                                               1);

            blr_slave_send_var_value(router,
                                     slave,
                                     "@@version",
                                     version == NULL ? "" : version,
                                     BLR_TYPE_STRING);
            return true;
        }
    }

    else if (strcasecmp(word, "@@version_comment") == 0)
    {
        if (!router->saved_master.selectvercom)
        /**
         * This allows mysql client to get in when
         * @@version_comment is not available
         */
        {
            blr_slave_send_ok(router, slave);
            return true;
        }
        else
        {
            blr_slave_replay(router,
                             slave,
                             router->saved_master.selectvercom);
            return true;
        }
    }
    else if (strcasecmp(word, "@@hostname") == 0)
    {
        if (router->set_master_hostname)
        {
            blr_slave_send_var_value(router,
                                     slave,
                                     "@@hostname",
                                     router->set_master_hostname,
                                     BLR_TYPE_STRING);
            return true;
        }
        else
        {
            blr_slave_replay(router,
                             slave,
                             router->saved_master.selecthostname);
            return true;
        }
    }
    else if ((strcasecmp(word, "@@server_uuid") == 0)
             || (strcasecmp(word, "@@global.server_uuid") == 0))
    {
        /* to ensure we match the case in query and response */
        char heading[40];
        strcpy(heading, word);

        if (router->set_master_uuid)
        {
            blr_slave_send_var_value(router,
                                     slave,
                                     heading,
                                     router->master_uuid,
                                     BLR_TYPE_STRING);
            return true;
        }
        else
        {
            char* master_uuid = blr_extract_column(router->saved_master.uuid,
                                                   2);
            blr_slave_send_var_value(router,
                                     slave,
                                     heading,
                                     master_uuid == NULL ? "" : master_uuid,
                                     BLR_TYPE_STRING);
            MXS_FREE(master_uuid);
            return true;
        }
    }
    else if (strcasecmp(word, "@@max_allowed_packet") == 0)
    {
        blr_slave_replay(router, slave, router->saved_master.map);
        return true;
    }
    else if (strcasecmp(word, "@@maxscale_version") == 0)
    {
        blr_slave_send_maxscale_version(router, slave);
        return true;
    }
    else if ((strcasecmp(word, "@@server_id") == 0)
             || (strcasecmp(word, "@@global.server_id") == 0))
    {
        char server_id[40];
        /* to ensure we match the case in query and response */
        char heading[40];

        sprintf(server_id, "%d", router->masterid);
        strcpy(heading, word);

        blr_slave_send_var_value(router,
                                 slave,
                                 heading,
                                 server_id,
                                 BLR_TYPE_INT);
        return true;
    }
    /* Handle MariaDB 10 GTID vars */
    else if ((strcasecmp(word, "@@gtid_current_pos") == 0)
             || (strcasecmp(word, "@@global.gtid_current_pos") == 0)
             || (strcasecmp(word, "@@gtid_binlog_pos") == 0)
             || (strcasecmp(word, "@@global.gtid_binlog_pos") == 0)
             || (strcasecmp(word, "@@gtid_slave_pos") == 0)
             || (strcasecmp(word, "@@global.gtid_slave_pos") == 0))
    {
        char heading[40];
        char mariadb_gtid[GTID_MAX_LEN + 1];
        mariadb_gtid[0] = 0;
        strcpy(heading, word);

        if (router->mariadb10_compat
            && router->mariadb10_gtid)
        {
            pthread_mutex_lock(&router->binlog_lock);
            strcpy(mariadb_gtid, router->last_mariadb_gtid);
            pthread_mutex_unlock(&router->binlog_lock);
        }

        /* Return empty gtid_slave_pos if master GTID registration is off */
        if (!router->mariadb10_master_gtid
            && strcasestr(word, "gtid_slave_pos") != NULL)
        {
            mariadb_gtid[0] = 0;
        }

        blr_slave_send_var_value(router,
                                 slave,
                                 heading,
                                 mariadb_gtid,
                                 BLR_TYPE_INT);
        return true;
    }
    else if (strcasecmp(word, "@@GLOBAL.gtid_domain_id") == 0)
    {
        /* If not mariadb10 mastergtid an error message will be returned */
        if (slave->mariadb10_compat
            && router->mariadb10_gtid)
        {
            char heading[40];
            char gtid_domain[40];
            sprintf(gtid_domain,
                    "%" PRIu32 "",
                    router->mariadb10_gtid_domain);
            strcpy(heading, word);

            blr_slave_send_var_value(router,
                                     slave,
                                     heading,
                                     gtid_domain,
                                     BLR_TYPE_INT);
            return true;
        }
    }
    else if ((strcasecmp(word, "@@global.max_connections") == 0)
             || (strcasecmp(word, "@@max_connections") == 0))
    {
        char max_conns[40];
        /* to ensure we match the case in query and response */
        char heading[40];

        sprintf(max_conns,
                "%d",
                !router->service->max_connections ?
                BLR_DEFAULT_MAX_CONNS :
                router->service->max_connections);
        strcpy(heading, word);

        blr_slave_send_var_value(router,
                                 slave,
                                 heading,
                                 max_conns,
                                 BLR_TYPE_INT);
    }
    else if ((strcasecmp(word, "@@global.read_only") == 0)
             || (strcasecmp(word, "@@read_only") == 0))
    {
        /* to ensure we match the case in query and response */
        char heading[40];
        strcpy(heading, word);

        blr_slave_send_var_value(router,
                                 slave,
                                 heading,
                                 "0",
                                 BLR_TYPE_INT);
    }
    else if ((strcasecmp(word, "@@global.log_bin") == 0)
             || (strcasecmp(word, "@@log_bin") == 0))
    {
        /* to ensure we match the case in query and response */
        char heading[40];
        strcpy(heading, word);

        blr_slave_send_var_value(router,
                                 slave,
                                 heading,
                                 "1",
                                 BLR_TYPE_INT);
    }

    return false;
}

/**
 * Build and send a Fake Rotate event to the new client
 *
 * @param router    The router instance
 * @param slave     The new connected client
 * @return          Non-zero if the rotate was sent
 */

static int blr_send_connect_fake_rotate(ROUTER_INSTANCE* router,
                                        ROUTER_SLAVE* slave)
{
    /* Build Fake Rotate Event */
    GWBUF* r_event = blr_build_fake_rotate_event(slave,
                                                 slave->binlog_pos,
                                                 slave->binlog_name,
                                                 router->masterid);

    /* Send Fake Rotate Event or return 0*/
    return r_event ? MXS_SESSION_ROUTE_REPLY(slave->dcb->session, r_event) : 0;
}

/**
 * Build a fake rotate event
 *
 * @param slave       The current connected client
 * @param pos         The position to set in the event
 * @param filename    The filename to set in the event
 * @param serverid    The serverid to set in the event
 * @return            A GWBUF with the binlog event or NULL
 */
static GWBUF* blr_build_fake_rotate_event(ROUTER_SLAVE* slave,
                                          unsigned long pos,
                                          const char*   filename,
                                          unsigned long serverid)
{
    GWBUF* r_event;
    uint8_t* ptr;
    int len;
    int flen;
    REP_HEADER hdr;
    uint32_t chksum;

    flen = strlen(filename);

    /* Event size: header + 8 bytes pos + filename */
    len = BINLOG_EVENT_HDR_LEN + 8 + flen;

    /* Add CRC32 bytes if needed */
    len += slave->nocrc ? 0 : BINLOG_EVENT_CRC_SIZE;

    /* Allocate space for packet header, status and data */
    if ((r_event = gwbuf_alloc(MYSQL_HEADER_LEN + 1 + len)) == NULL)
    {
        return NULL;
    }

    /* Add 1 byte to paylod for status indicator */
    hdr.payload_len = len + 1;

    /* Add sequence and increment it */
    hdr.seqno = slave->seqno++;

    /* Set status indicator byte to OK */
    hdr.ok = 0;

    /* No timestamp is required */
    hdr.timestamp = 0L;

    /* Rotate Event Type */
    hdr.event_type = ROTATE_EVENT;
    hdr.serverid = serverid;
    hdr.event_size = len;

    /* Next pos is not needed */
    hdr.next_pos = 0;

    /* Artificial Event Flag */
    hdr.flags = 0x20;

    /* Add replication hdr to resp */
    ptr = blr_build_header(r_event, &hdr);

    /* Add 8 bytes pos */
    encode_value(ptr, pos, 64);
    ptr += 8;

    /* Add binlog filename, no trailing 0 */
    memcpy(ptr, filename, flen);
    ptr += flen;

    /* Now add the CRC to the fake binlog rotate event */
    if (!slave->nocrc)
    {
        /*
         * First checksum of an empty buffer
         * then the checksum of the event portion of the message:
         * we do not include the len, seq number and ok byte that are part of
         * first 5 bytes of the message.
         * We also do not include the 4 byte checksum itself.
         */
        chksum = crc32(0L, NULL, 0);
        chksum = crc32(chksum,
                       GWBUF_DATA(r_event) + MYSQL_HEADER_LEN + 1,
                       hdr.event_size - BINLOG_EVENT_CRC_SIZE);
        encode_value(ptr, chksum, 32);
    }

    return r_event;
}

/**
 * Look for a MariaDB GTID in the gtid maps database
 *
 * The caller specifies the position from COM_BINLOG_DUMP
 * packet and if a filename is present or not in the request.
 *
 * Default position is 4, default file is router->binlog_file.
 *
 * If req_file is false then the file to read data from
 * could be either router->binlog_file or the file the GTID
 * belongs to.
 *
 * Note: rmpty GTID means send data from router->binlog_file pos 4.
 *
 * @param router    The router instance
 * @param slave     The current slave server connected
 * @param req_file  Using binlog filename or not
 * @param req_pos   The requested file pos
 * @return          False if GTID is not found and slave
 *                  is connectig with gtid_strict_mode=1,
 *                  other errors.
 *                  True otherwise.
 */
static bool blr_slave_gtid_request(ROUTER_INSTANCE* router,
                                   ROUTER_SLAVE* slave,
                                   bool req_file,
                                   unsigned long req_pos)
{
    MARIADB_GTID_INFO f_gtid = {};
    uint32_t router_pos;
    char router_curr_file[BINLOG_FNAMELEN + 1];
    char last_gtid[GTID_MAX_LEN + 1];

    memset(&f_gtid, 0, sizeof(f_gtid));

    pthread_mutex_lock(&router->binlog_lock);
    // Set gtid as current router gtid
    strcpy(last_gtid, router->last_mariadb_gtid);
    // Set file as router current file
    strcpy(router_curr_file, router->binlog_name);
    // Set safe postion of current ruter file
    router_pos = router->binlog_position;
    // Set domain_id, server_id in case of empty/not found GTID
    if (router->storage_type == BLR_BINLOG_STORAGE_TREE)
    {
        f_gtid.gtid_elms.domain_id = router->mariadb10_gtid_domain;
        f_gtid.gtid_elms.server_id = router->orig_masterid;
    }
    pthread_mutex_unlock(&router->binlog_lock);

    MXS_INFO("Slave %lu is registering with MariaDB GTID '%s'",
             (unsigned long)slave->serverid,
             slave->mariadb_gtid);

    if (!slave->mariadb_gtid[0])
    {
        /**
         * Empty GTID:
         * Sending data from the router current file and pos 4
         */
        char t_prefix[BINLOG_FILE_EXTRA_INFO] = "";

        // Add file prefix
        if (router->storage_type == BLR_BINLOG_STORAGE_TREE)
        {
            sprintf(t_prefix,
                    "%" PRIu32 "/%" PRIu32 "/",
                    f_gtid.gtid_elms.domain_id,
                    f_gtid.gtid_elms.server_id);
        }

        strcpy(slave->binlog_name, router_curr_file);
        slave->binlog_pos = 4;

        MXS_INFO("Slave %d is registering with empty GTID:"
                 " sending events from current binlog file %s%s,"
                 " pos %" PRIu32 "",
                 slave->serverid,
                 t_prefix,
                 slave->binlog_name,
                 slave->binlog_pos);

        /* Add GTID details to slave struct */
        memcpy(&slave->f_info, &f_gtid, sizeof(MARIADB_GTID_INFO));
        return true;
    }
    else
    {
        char dbpath[PATH_MAX + 1];
        snprintf(dbpath,
                 sizeof(dbpath),
                 "/%s/%s",
                 router->binlogdir,
                 GTID_MAPS_DB);

        /* Open GTID maps read-only database */
        if (sqlite3_open_v2(dbpath,
                            &slave->gtid_maps,
                            SQLITE_OPEN_READONLY,
                            NULL) != SQLITE_OK)
        {
            char errmsg[BINLOG_ERROR_MSG_LEN + sizeof(dbpath) + 1];
            snprintf(errmsg,
                     sizeof(errmsg),
                     "Slave %lu: failed to open GTID maps db '%s': %s",
                     (unsigned long)slave->serverid,
                     dbpath,
                     sqlite3_errmsg(slave->gtid_maps));
            errmsg[BINLOG_ERROR_MSG_LEN] = '\0';

            MXS_ERROR("%s", errmsg);
            strcpy(slave->binlog_name, "");
            slave->binlog_pos = 0;
            blr_send_custom_error(slave->dcb,
                                  slave->seqno + 1,
                                  0,
                                  "Cannot open GTID maps storage.",
                                  "HY000",
                                  BINLOG_FATAL_ERROR_READING);

            slave->gtid_maps = NULL;
            return false;
        }
        else
        {
            /* Fetch the GTID from the maps storage */
            blr_fetch_mariadb_gtid(slave, slave->mariadb_gtid, &f_gtid);

            /* Close GTID maps database */
            sqlite3_close_v2(slave->gtid_maps);
            slave->gtid_maps = NULL;
        }

        /* Requested GTID Not Found */
        if (!f_gtid.gtid[0])
        {
            char errmsg[BINLOG_ERROR_MSG_LEN + 1];
            snprintf(errmsg,
                     BINLOG_ERROR_MSG_LEN,
                     "Requested MariaDB GTID '%s' by server %lu"
                     " not found. GTID_STRICT_MODE=%s",
                     slave->mariadb_gtid,
                     (unsigned long)slave->serverid,
                     slave->gtid_strict_mode ? "ON" : "OFF");
            errmsg[BINLOG_ERROR_MSG_LEN] = '\0';

            /* Check strict mode */
            if (slave->gtid_strict_mode)
            {
                MXS_ERROR("%s", errmsg);
                strcpy(slave->binlog_name, "");
                slave->binlog_pos = 0;
                blr_send_custom_error(slave->dcb,
                                      slave->seqno + 1,
                                      0,
                                      "connecting slave requested to start"
                                      " from non existent GTID.",
                                      "HY000",
                                      BINLOG_FATAL_ERROR_READING);
                return false;
            }
            else
            {
                /* No strict mode: */
                MXS_WARNING("%s", errmsg);

                // - 1 -Set request GTID as current master one
                MXS_FREE(slave->mariadb_gtid);
                slave->mariadb_gtid = MXS_STRDUP_A(last_gtid);
                // - 2 - Use current router file and position
                strcpy(slave->binlog_name, router_curr_file);
                slave->binlog_pos = router_pos;

                // - 3 Set GTID details for filename
                if (router->storage_type == BLR_BINLOG_STORAGE_TREE)
                {
                    memcpy(&slave->f_info, &f_gtid, sizeof(MARIADB_GTID_INFO));
                }
            }
        }
        else
        {
            /* GTID has been found */
            MXS_INFO("Found GTID '%s' for slave %" PRIu32 ""
                                                          " at %" PRIu32 "/%" PRIu32 "/%s:%" PRIu64 ""
                                                                                                    ". Next event at %" PRIu64 "",
                     slave->mariadb_gtid,
                     slave->serverid,
                     f_gtid.gtid_elms.domain_id,
                     f_gtid.gtid_elms.server_id,
                     f_gtid.binlog_name,
                     f_gtid.start,
                     f_gtid.end);

            /**
             * Checks:
             * a) GTID request has no binlog file at all:
             *   use GTID info file
             * b) binlog file & position:
             *   if the requested binlog file is equal to GTID info file use it.
             */
            if (!req_file
                || (strcmp(slave->binlog_name, f_gtid.binlog_name) == 0))
            {
                /* Set binlog file to the GTID one */
                strcpy(slave->binlog_name, f_gtid.binlog_name);

                /* Set pos to GTID next event pos */
                slave->binlog_pos = f_gtid.end;
            }
            else
            {
                /**
                 * The requested binlog file is not the GTID info file.
                 * The binlog file could be different due to:
                 * a rotate event or other non GTID events written
                 * after that GTID.
                 * If file exists and pos >=4, events will be sent
                 * from requested file@pos, otherwise from GTID file & pos.
                 */

                // Add tree prefix
                char t_prefix[BINLOG_FILE_EXTRA_INFO] = "";
                char file_path[PATH_MAX + 1] = "";
                if (router->storage_type == BLR_BINLOG_STORAGE_TREE)
                {
                    sprintf(t_prefix,
                            "%" PRIu32 "/%" PRIu32 "/",
                            f_gtid.gtid_elms.domain_id,
                            f_gtid.gtid_elms.server_id);
                }

                // Get binlog filename full-path
                blr_get_file_fullpath(slave->binlog_name,
                                      router->binlogdir,
                                      file_path,
                                      t_prefix[0] ? t_prefix : NULL);
                // File size is >=4 read: set pos.
                if (blr_slave_get_file_size(file_path) >= 4)
                {
                    slave->binlog_pos = req_pos;
                }
                else
                {
                    /* Set binlog file to the GTID one */
                    strcpy(slave->binlog_name, f_gtid.binlog_name);

                    /* Set pos to GTID next event pos */
                    slave->binlog_pos = f_gtid.end;
                }
            }

            /* Set GTID details in f_info */
            memcpy(&slave->f_info, &f_gtid, sizeof(MARIADB_GTID_INFO));
        }
    }

    return true;
}

/**
 * Create a Fake GTID_LIST event
 *
 * The routine creates a Fake GTID_LIST event
 *
 * @param slave       The connected client
 * @param gtid        The requested GTID from client
 * @param serverid    The router server_id to add
 *                    in the replication event header
 * @return            Fake GTID_LIST event on success or NULL
 *
 */
static GWBUF* blr_build_fake_gtid_list_event(ROUTER_SLAVE* slave,
                                             const char*   gtid,
                                             uint32_t serverid)
{
    int len;
    GWBUF* gl_event;
    uint8_t* ptr;
    REP_HEADER hdr;
    uint32_t chksum;
    MARIADB_GTID_ELEMS req_gtid = {};

    if (!blr_parse_gtid(gtid, &req_gtid))
    {
        return NULL;
    }

    /**
     * We only support one GTID in the GTID_LIST event
     *
     * Paylod is:
     * BINLOG_EVENT_HDR_LEN + 4 bytes GTID count + 1 GTID
     */
    len = BINLOG_EVENT_HDR_LEN + 4 + 1 * (4 + 4 + 8);

    /* Add CRC32 bytes if needed */
    len += slave->nocrc ? 0 : BINLOG_EVENT_CRC_SIZE;

    /* Allocate space for packet header, status and data */
    if ((gl_event = gwbuf_alloc(MYSQL_HEADER_LEN + 1 + len)) == NULL)
    {
        return NULL;
    }
    /* Add 1 byte to paylod for status indicator */
    hdr.payload_len = len + 1;

    /* Add sequence and increment it */
    hdr.seqno = slave->seqno++;

    /* Set status indicator byte to OK */
    hdr.ok = 0;

    /* No timestamp is required */
    hdr.timestamp = 0L;

    /* GTID Event Type */
    hdr.event_type = MARIADB10_GTID_GTID_LIST_EVENT;
    hdr.serverid = serverid;
    hdr.event_size = len;

    /* Next pos is set */
    hdr.next_pos = slave->binlog_pos;

    /* Artificial Event Flag */
    hdr.flags = 0x20;

    /* Add replication hdr to resp */
    ptr = blr_build_header(gl_event, &hdr);

    /*
     * Add 4 bytes count
     * Note: We set only 1 GTID in GTID_LIST Event
     */
    encode_value(ptr, 1, 32);
    ptr += 4;

    /* Add 4 bytes domain id */
    encode_value(ptr, req_gtid.domain_id, 32);
    ptr += 4;

    /* Add 4 bytes server id*/
    encode_value(ptr, req_gtid.server_id, 32);
    ptr += 4;

    /* Add 8 bytes sequence */
    encode_value(ptr, req_gtid.seq_no, 64);
    ptr += 8;

    /* Now add the CRC to the fake binlog rotate event */
    if (!slave->nocrc)
    {
        /*
         * First checksum of an empty buffer
         * then the checksum of the event portion of the message:
         * we do not include the len, seq number and ok byte that are part of
         * first 5 bytes of the message.
         * We also do not include the 4 byte checksum itself.
         */
        chksum = crc32(0L, NULL, 0);
        chksum = crc32(chksum,
                       GWBUF_DATA(gl_event) + MYSQL_HEADER_LEN + 1,
                       hdr.event_size - BINLOG_EVENT_CRC_SIZE);
        encode_value(ptr, chksum, 32);
    }

    return gl_event;
}

/**
 * Create and send a Fake GTID_LIST event
 *
 * @param slave       Current slave server
 * @param gtid        The requested GTID from client
 * @oaram serverid    The server_id to use in replication header
 * @return            Non-zero if data has been sent
 */
static int blr_send_fake_gtid_list(ROUTER_SLAVE* slave,
                                   const char*   gtid,
                                   uint32_t serverid)
{
    /* Build Fake GTID_LIST Event */
    GWBUF* gl_event = blr_build_fake_gtid_list_event(slave,
                                                     gtid,
                                                     serverid);

    /* Send Fake GTID_LIST Event or return 0*/
    return gl_event ? MXS_SESSION_ROUTE_REPLY(slave->dcb->session, gl_event) : 0;
}

/**
 * Handle received Maxwell statements from clients
 *
 * if a Maxwell statement is suported
 * a proper reply to the connected client is done
 *
 * @param    router          Router instance
 * @param    slave           Connected client/slave server
 * @param    maxwell_stmt    The admin command options
 * @return                   True for handled queries, False otherwise
 */
static bool blr_handle_maxwell_stmt(ROUTER_INSTANCE* router,
                                    ROUTER_SLAVE* slave,
                                    const char*   maxwell_stmt)
{
    static const char mysql_connector_results_charset_query[] = "SET character_set_results = NULL";
    static const char maxwell_server_id_query[] = "SELECT @@server_id as server_id";
    static const char maxwell_log_bin_query[] = "SHOW VARIABLES LIKE 'log_bin'";
    static const char maxwell_binlog_format_query[] = "SHOW VARIABLES LIKE 'binlog_format'";
    static const char maxwell_binlog_row_image_query[] = "SHOW VARIABLES LIKE 'binlog_row_image'";
    static const char maxwell_lower_case_tables_query[] = "select @@lower_case_table_names";

    if (strcmp(blr_skip_leading_sql_comments(maxwell_stmt),
               MYSQL_CONNECTOR_SERVER_VARS_QUERY) == 0)
    {
        int rc = blr_slave_replay(router,
                                  slave,
                                  router->saved_master.server_vars);
        if (!rc)
        {
            MXS_ERROR("Error sending mysql-connector-j server variables");
        }
        return true;
    }
    else if (router->maxwell_compat
             && strcmp(maxwell_stmt, mysql_connector_results_charset_query) == 0)
    {
        blr_slave_send_ok(router, slave);
        return true;
    }
    else if (router->maxwell_compat
             && strcmp(maxwell_stmt, MYSQL_CONNECTOR_SQL_MODE_QUERY) == 0)
    {
        blr_slave_send_ok(router, slave);
        return true;
    }
    else if (strcmp(maxwell_stmt, maxwell_server_id_query) == 0)
    {
        char server_id[40];
        sprintf(server_id, "%d", router->masterid);
        blr_slave_send_var_value(router,
                                 slave,
                                 "server_id",
                                 server_id,
                                 BLR_TYPE_STRING);
        return true;
    }
    else if (strcmp(maxwell_stmt, maxwell_log_bin_query) == 0)
    {
        char* log_bin = blr_extract_column(router->saved_master.binlog_vars, 1);
        blr_slave_send_var_value(router,
                                 slave,
                                 "Value",
                                 log_bin == NULL ? "" : log_bin,
                                 BLR_TYPE_STRING);
        MXS_FREE(log_bin);
        return true;
    }
    else if (strcmp(maxwell_stmt, maxwell_binlog_format_query) == 0)
    {
        char* binlog_format = blr_extract_column(router->saved_master.binlog_vars, 2);
        blr_slave_send_var_value(router,
                                 slave,
                                 "Value",
                                 binlog_format == NULL ? "" : binlog_format,
                                 BLR_TYPE_STRING);
        MXS_FREE(binlog_format);
        return true;
    }
    else if (strcmp(maxwell_stmt, maxwell_binlog_row_image_query) == 0)
    {
        char* binlog_row_image = blr_extract_column(router->saved_master.binlog_vars, 3);
        blr_slave_send_var_value(router,
                                 slave,
                                 "Value",
                                 binlog_row_image == NULL ? "" : binlog_row_image,
                                 BLR_TYPE_STRING);
        MXS_FREE(binlog_row_image);
        return true;
    }
    else if (strcmp(maxwell_stmt, maxwell_lower_case_tables_query) == 0)
    {
        int rc = blr_slave_replay(router,
                                  slave,
                                  router->saved_master.lower_case_tables);
        if (!rc)
        {
            MXS_ERROR("Error sending lower_case_tables query response");
        }
        return true;
    }

    return false;
}

/**
 * Handle received SHOW statements from clients
 *
 * if a SHOW statement is one of suported one
 * a proper reply to the connected client is done
 *
 * @param    router       Router instance
 * @param    slave        Connected client/slave server
 * @param    show_stmt    The SHOW statement
 * @return                True for handled queries, False otherwise
 */
static bool blr_handle_show_stmt(ROUTER_INSTANCE* router,
                                 ROUTER_SLAVE* slave,
                                 char* show_stmt)
{
    char* word;
    char* brkb;
    const char* sep = " \t,=";
    if ((word = strtok_r(show_stmt, sep, &brkb)) == NULL)
    {
        MXS_ERROR("%s: Incomplete show query.", router->service->name);
        return false;
    }
    else if (strcasecmp(word, "WARNINGS") == 0)
    {
        blr_slave_show_warnings(router, slave);
        return true;
    }
    else if (strcasecmp(word, "BINARY") == 0)
    {
        if (router->mariadb10_gtid)
        {
            blr_show_binary_logs(router, slave, word);
        }
        else
        {
            const char* errmsg =
                "SHOW [FULL] BINARY LOGS needs the"
                " 'mariadb10_slave_gtid' option to be set.";
            MXS_ERROR("%s: %s",
                      errmsg,
                      router->service->name);

            blr_slave_send_error_packet(slave,
                                        errmsg,
                                        1198,
                                        NULL);
        }
        return true;
    }
    else if (strcasecmp(word, "GLOBAL") == 0)
    {
        if (router->master_state == BLRM_UNCONFIGURED)
        {
            blr_slave_send_ok(router, slave);
            return true;
        }

        if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Expected VARIABLES in SHOW GLOBAL",
                      router->service->name);
            return false;
        }
        else if (strcasecmp(word, "VARIABLES") == 0)
        {
            int rc = blr_slave_handle_variables(router, slave, brkb);

            /* if no var found, send empty result set */
            if (rc == 0)
            {
                blr_slave_send_ok(router, slave);
            }

            if (rc >= 0)
            {
                return true;
            }
            else
            {
                MXS_ERROR("%s: Expected LIKE clause in SHOW GLOBAL VARIABLES.",
                          router->service->name);
                return false;
            }
        }
        else if (strcasecmp(word, "STATUS") == 0)
        {
            int rc = blr_slave_handle_status_variables(router, slave, brkb);

            /* if no var found, send empty result set */
            if (rc == 0)
            {
                blr_slave_send_ok(router, slave);
            }

            if (rc >= 0)
            {
                return true;
            }
            else
            {
                MXS_ERROR("%s: Expected LIKE clause in SHOW GLOBAL STATUS.",
                          router->service->name);
                return false;
            }
        }
    }
    else if (strcasecmp(word, "VARIABLES") == 0)
    {
        int rc;
        if (router->master_state == BLRM_UNCONFIGURED)
        {
            blr_slave_send_ok(router, slave);
            return true;
        }

        rc = blr_slave_handle_variables(router, slave, brkb);

        /* if no var found, send empty result set */
        if (rc == 0)
        {
            blr_slave_send_ok(router, slave);
        }

        if (rc >= 0)
        {
            return true;
        }
        else
        {
            MXS_ERROR("%s: Expected LIKE clause in SHOW VARIABLES.",
                      router->service->name);
            return false;
        }
    }
    else if (strcasecmp(word, "MASTER") == 0)
    {
        if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Expected SHOW MASTER STATUS command",
                      router->service->name);
            return false;
        }
        else if (strcasecmp(word, "STATUS") == 0)
        {
            /* if state is BLRM_UNCONFIGURED return empty result */

            if (router->master_state > BLRM_UNCONFIGURED)
            {
                blr_slave_send_master_status(router, slave);
            }
            else
            {
                blr_slave_send_ok(router, slave);
            }
            return true;
        }
    }
    /* Added support for SHOW ALL SLAVES STATUS */
    else if (strcasecmp(word, "SLAVE") == 0
             || (strcasecmp(word, "ALL") == 0))
    {
        if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Expected SHOW SLAVE STATUS command",
                      router->service->name);
            return false;
        }
        else if (strcasecmp(word, "STATUS") == 0
                 || (strcasecmp(word, "SLAVES") == 0
                     && strcasecmp(brkb, "STATUS") == 0))
        {
            /* if state is BLRM_UNCONFIGURED return empty result */
            if (router->master_state > BLRM_UNCONFIGURED)
            {
                bool s_all = strcasecmp(word, "SLAVES") == 0 ? true : false;
                blr_slave_send_slave_status(router, slave, s_all);
            }
            else
            {
                blr_slave_send_ok(router, slave);
            }
            return true;
        }
        else if (strcasecmp(word, "HOSTS") == 0)
        {
            /* if state is BLRM_UNCONFIGURED return empty result */
            if (router->master_state > BLRM_UNCONFIGURED)
            {
                blr_slave_send_slave_hosts(router, slave);
            }
            else
            {
                blr_slave_send_ok(router, slave);
            }
            return true;
        }
    }
    else if (strcasecmp(word, "STATUS") == 0)
    {
        int rc = blr_slave_handle_status_variables(router, slave, brkb);

        /* if no var found, send empty result set */
        if (rc == 0)
        {
            blr_slave_send_ok(router, slave);
        }

        if (rc >= 0)
        {
            return true;
        }
        else
        {
            MXS_ERROR("%s: Expected LIKE clause in SHOW STATUS.",
                      router->service->name);
            return false;
        }
    }

    return false;
}

/**
 * Handle received SET statements from clients
 *
 * if a SHOW statement is one of suported one
 * a proper reply to the connected client is done
 *
 * @param    router      Router instance
 * @param    slave       Connected client/slave server
 * @param    set_stmt    The SET statement
 * @return               True for handled queries, False otherwise
 */
static bool blr_handle_set_stmt(ROUTER_INSTANCE* router,
                                ROUTER_SLAVE* slave,
                                char* set_stmt)
{
    char* word;
    char* brkb;
    const char* sep = " \t,=";

    if ((word = strtok_r(set_stmt, sep, &brkb)) == NULL)
    {
        MXS_ERROR("%s: Incomplete set command.", router->service->name);
        return false;
    }
    else if ((strcasecmp(word, "autocommit") == 0)
             || (strcasecmp(word, "@@session.autocommit") == 0))
    {
        blr_slave_send_ok(router, slave);
        return true;
    }
    else if (strcasecmp(word, "@master_heartbeat_period") == 0)
    {
        int v_len = 0;
        word = strtok_r(NULL, sep, &brkb);
        if (word)
        {
            char* new_val;
            v_len = strlen(word);
            if (v_len > 6)
            {
                new_val = mxs_strndup_a(word, v_len - 6);
                slave->heartbeat = atoi(new_val) / 1000;
            }
            else
            {
                new_val = mxs_strndup_a(word, v_len);
                slave->heartbeat = atoi(new_val) / 1000000;
            }

            MXS_FREE(new_val);
        }
        blr_slave_replay(router, slave, router->saved_master.heartbeat);
        return true;
    }
    else if (strcasecmp(word, "@mariadb_slave_capability") == 0)
    {
        /* mariadb10 compatibility is set for the slave */
        slave->mariadb10_compat = true;

        if (router->mariadb10_compat)
        {
            blr_slave_replay(router, slave, router->saved_master.mariadb10);
        }
        else
        {
            blr_slave_send_ok(router, slave);
        }
        return true;
    }
    else if (strcasecmp(word, "@master_binlog_checksum") == 0)
    {
        word = strtok_r(NULL, sep, &brkb);
        if (word && (strcasecmp(word, "'none'") == 0))
        {
            slave->nocrc = 1;
        }
        else if (word && (strcasecmp(word, "@@global.binlog_checksum") == 0))
        {
            slave->nocrc = !router->master_chksum;
        }
        else
        {
            slave->nocrc = 0;
        }

        blr_slave_replay(router, slave, router->saved_master.chksum1);
        return true;
    }
    else if (strcasecmp(word, "@slave_uuid") == 0)
    {
        if ((word = strtok_r(NULL, sep, &brkb)) != NULL)
        {
            int len = strlen(word);
            char* word_ptr = word;
            if (len)
            {
                if (word[len - 1] == '\'')
                {
                    word[len - 1] = '\0';
                }
                if (word[0] == '\'')
                {
                    word[0] = '\0';
                    word_ptr++;
                }
            }
            /* Free previous value */
            MXS_FREE(slave->uuid);
            slave->uuid = MXS_STRDUP_A(word_ptr);
        }

        if (router->saved_master.setslaveuuid)
        {
            blr_slave_replay(router, slave, router->saved_master.setslaveuuid);
        }
        else
        {
            blr_slave_send_ok(router, slave);
        }
        return true;
    }
    else if (strcasecmp(word, "@@global.gtid_slave_pos") == 0)
    {
        if (slave->serverid != 0)
        {
            MXS_ERROR("Master GTID registration can be sent only"
                      " via administration connection");
            blr_slave_send_error_packet(slave,
                                        "Master GTID registration cannot be"
                                        " issued by a registrating slave.",
                                        1198,
                                        NULL);
            return false;
        }
        if (router->master_state != BLRM_SLAVE_STOPPED
            && router->master_state != BLRM_UNCONFIGURED)
        {
            const char* err_msg_u = "configured replication: Issue CHANGE MASTER TO first.";
            const char* err_msg_s = "stopped replication: issue STOP SLAVE first.";
            char error_string[BINLOG_ERROR_MSG_LEN + 1] = "";
            MXS_ERROR("GTID registration without %s",
                      router->master_state == BLRM_SLAVE_STOPPED ?
                      err_msg_s : err_msg_u);

            snprintf(error_string,
                     BINLOG_ERROR_MSG_LEN,
                     "Cannot use Master GTID registration without %s",
                     router->master_state == BLRM_SLAVE_STOPPED ?
                     err_msg_s :
                     err_msg_u);

            blr_slave_send_error_packet(slave,
                                        error_string,
                                        1198,
                                        NULL);
            return true;
        }
        /* If not mariadb GTID an error message will be returned */
        if (router->mariadb10_master_gtid)
        {
            if ((word = strtok_r(NULL, sep, &brkb)) != NULL)
            {
                char heading[GTID_MAX_LEN + 1];
                MARIADB_GTID_ELEMS gtid_elms = {};

                // TODO: gtid_strip_chars routine for this
                strcpy(heading, word + 1);
                heading[strlen(heading) - 1] = '\0';

                MXS_INFO("Requesting GTID (%s) from Master server.",
                         !heading[0] ? "empty value" : heading);

                /* Parse the non empty GTID value */
                if (heading[0] && !blr_parse_gtid(heading, &gtid_elms))
                {
                    const char err_fmt[] = "Invalid format for GTID ('%s')"
                                           " set request; use 'X-Y-Z'";
                    char err_msg[sizeof(err_fmt) + GTID_MAX_LEN + 1];
                    sprintf(err_msg, err_fmt, heading);

                    MXS_ERROR("%s", err_msg);

                    /* Stop Master registration */
                    blr_slave_send_error_packet(slave,
                                                err_msg,
                                                1198,
                                                NULL);
                }
                else
                {
                    strcpy(router->last_mariadb_gtid, heading);
                    blr_slave_send_ok(router, slave);
                }
                return true;
            }
        }
        else
        {
            MXS_ERROR("Master GTID registration needs 'mariadb10_master_gtid'"
                      " option to be set.");
            blr_slave_send_error_packet(slave,
                                        "Master GTID registration needs"
                                        " 'mariadb10_master_gtid' option"
                                        " to be set first.",
                                        1198,
                                        NULL);
            return true;
        }
    }
    else if (strcasestr(word, "@slave_connect_state") != NULL)
    {
        /* If not mariadb an error message will be returned */
        if (slave->mariadb10_compat
            && router->mariadb10_gtid
            && (word = strtok_r(NULL, sep, &brkb)) != NULL)
        {
            char heading[GTID_MAX_LEN + 1];

            MXS_DEBUG("Received GTID request '%s' from slave %u",
                      word,
                      slave->serverid);

            strcpy(heading, word + 1);
            heading[strlen(heading) - 1] = '\0';

            /**
             * Set the GTID string, it could be an empty
             * in case of a fresh new setup.
             */
            MXS_FREE(slave->mariadb_gtid);
            slave->mariadb_gtid = MXS_STRDUP_A(heading);

            blr_slave_send_ok(router, slave);
            return true;
        }
        else
        {
            MXS_ERROR("GTID Master registration is not enabled");
            return false;
        }
    }
    else if (strcasecmp(word, "@slave_gtid_strict_mode") == 0)
    {
        /* If not mariadb an error message will be returned */
        if (slave->mariadb10_compat
            && router->mariadb10_gtid
            && (word = strtok_r(NULL, sep, &brkb)) != NULL)
        {
            /* Set strict mode */
            slave->gtid_strict_mode = atoi(word);
            blr_slave_send_ok(router, slave);
            return true;
        }
    }
    else if (strcasecmp(word, "@slave_gtid_ignore_duplicates") == 0)
    {
        /* If not mariadb an error message will be returned */
        if (slave->mariadb10_compat
            && router->mariadb10_gtid)
        {
            blr_slave_send_ok(router, slave);
            return true;
        }
    }
    else if (strcasecmp(word, "NAMES") == 0)
    {
        if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Truncated SET NAMES command.",
                      router->service->name);
            return false;
        }
        else if (strcasecmp(word, "latin1") == 0)
        {
            blr_slave_replay(router, slave, router->saved_master.setnames);
            return true;
        }
        else if (strcasecmp(word, "utf8") == 0)
        {
            blr_slave_replay(router, slave, router->saved_master.utf8);
            return true;
        }
        else
        {
            blr_slave_send_ok(router, slave);
            return true;
        }
    }
    else if (strcasecmp(word, "SQL_MODE") == 0)
    {
        blr_slave_send_ok(router, slave);
        return true;
    }

    return false;
}

/**
 * Handle received admin statements from clients
 *
 * if an admin statement is one of suported one
 * a proper reply to the connected client is done
 *
 * @param    router        Router instance
 * @param    slave         Connected client/slave server
 * @param    admin_stmt    The admin statement
 * @param    admin_opts    The admin command options
 * @return                 True for handled queries, False otherwise
 */
static bool blr_handle_admin_stmt(ROUTER_INSTANCE* router,
                                  ROUTER_SLAVE* slave,
                                  char* admin_stmt,
                                  char* admin_opts)
{
    char* word;
    char* brkb;
    const char* sep = " \t,=";

    if (admin_opts == NULL || !admin_opts[0])
    {
        MXS_ERROR("%s: Incomplete admin command.", router->service->name);
        return false;
    }
    /* Handle PURGE command */
    else if (strcasecmp(admin_stmt, "PURGE") == 0)
    {
        if (router->master_state != BLRM_SLAVE_STOPPED)
        {
            blr_slave_send_error_packet(slave,
                                        "Cannot execute PURGE BINARY LOGS "
                                        "with a running slave; "
                                        "run STOP SLAVE first.",
                                        1198,
                                        NULL);
            return true;
        }

        /* Check for GTID support */
        if (router->mariadb10_gtid)
        {
            blr_purge_binary_logs(router, slave, admin_opts);
        }
        else
        {
            const char* errmsg =
                "PURGE BINARY LOGS needs the "
                "'mariadb10_slave_gtid' option to be set.";
            MXS_ERROR("%s: %s",
                      errmsg,
                      router->service->name);

            blr_slave_send_error_packet(slave,
                                        errmsg,
                                        1198,
                                        NULL);
        }
        return true;
    }
    /* Handle RESET command */
    else if (strcasecmp(admin_stmt, "RESET") == 0)
    {
        if ((word = strtok_r(admin_opts, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Incomplete RESET command.", router->service->name);
            return false;
        }
        /* RESET the current configured master cfg */
        else if (strcasecmp(word, "SLAVE") == 0)
        {
            if (router->master_state == BLRM_SLAVE_STOPPED)
            {
                char error_string[BINLOG_ERROR_MSG_LEN + 1] = "";
                MasterServerConfig current_master;
                int removed_cfg = 0;

                /* get current data */
                blr_master_get_config(router, &current_master);

                MXS_NOTICE("%s: 'RESET SLAVE executed'. Previous state MASTER_HOST='%s', "
                           "MASTER_PORT=%i, MASTER_LOG_FILE='%s', MASTER_LOG_POS=%lu, "
                           "MASTER_USER='%s'",
                           router->service->name,
                           current_master.host.c_str(),
                           current_master.port,
                           current_master.logfile.c_str(),
                           current_master.pos,
                           current_master.user.c_str());

                /* remove master.ini */
                static const char MASTER_INI[] = "/master.ini";
                char path[strlen(router->binlogdir) + sizeof(MASTER_INI)];

                strcpy(path, router->binlogdir);
                strcat(path, MASTER_INI);

                /* remove master.ini */
                removed_cfg = unlink(path);

                if (removed_cfg == -1)
                {
                    snprintf(error_string,
                             BINLOG_ERROR_MSG_LEN,
                             "Error removing %s, %s, errno %u",
                             path,
                             mxs_strerror(errno),
                             errno);
                    MXS_ERROR("%s: %s", router->service->name, error_string);
                }

                pthread_mutex_lock(&router->lock);

                /* Set the BLRM_UNCONFIGURED state */
                router->master_state = BLRM_UNCONFIGURED;
                blr_master_set_empty_config(router);

                /* Remove any error message and errno */
                free(router->m_errmsg);
                router->m_errmsg = NULL;
                router->m_errno = 0;

                pthread_mutex_unlock(&router->lock);

                if (removed_cfg == -1)
                {
                    blr_slave_send_error_packet(slave,
                                                error_string,
                                                1201,
                                                NULL);
                }
                else
                {
                    blr_slave_send_ok(router, slave);
                }
                return true;
            }
            else
            {
                if (router->master_state == BLRM_UNCONFIGURED)
                {
                    blr_slave_send_ok(router, slave);
                }
                else
                {
                    blr_slave_send_error_packet(slave,
                                                "This operation cannot be performed "
                                                "with a running slave; run STOP SLAVE first",
                                                1198,
                                                NULL);
                }
                return true;
            }
        }
    }
    /* Start replication from the current configured master */
    else if (strcasecmp(admin_stmt, "START") == 0)
    {
        if ((word = strtok_r(admin_opts, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Incomplete START command.",
                      router->service->name);
            return false;
        }
        else if (strcasecmp(word, "SLAVE") == 0)
        {
            blr_start_slave(router, slave);
            return true;
        }
    }
    /* Stop replication from the current master*/
    else if (strcasecmp(admin_stmt, "STOP") == 0)
    {
        if ((word = strtok_r(admin_opts, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Incomplete STOP command.", router->service->name);
            return false;
        }
        else if (strcasecmp(word, "SLAVE") == 0)
        {
            blr_stop_slave(router, slave);
            return true;
        }
    }
    /* Change the server to replicate from */
    else if (strcasecmp(admin_stmt, "CHANGE") == 0)
    {
        if ((word = strtok_r(admin_opts, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Incomplete CHANGE command.", router->service->name);
            return false;
        }
        else if (strcasecmp(word, "MASTER") == 0)
        {
            if (router->master_state != BLRM_SLAVE_STOPPED
                && router->master_state != BLRM_UNCONFIGURED)
            {
                blr_slave_send_error_packet(slave,
                                            "Cannot change master with a running slave; "
                                            "run STOP SLAVE first",
                                            (unsigned int)1198,
                                            NULL);
                return true;
            }
            else
            {
                int rc;
                char error_string[BINLOG_ERROR_MSG_LEN + 1 + BINLOG_ERROR_MSG_LEN + 1] = "";
                // TODO: Why is this without a lock, but blr_master_apply_config() below with
                // TODO: a lock. One of them must be wrong.
                MasterServerConfig current_master;
                blr_master_get_config(router, &current_master);
                vector<ChangeMasterConfig> configs = router->configs;

                rc = blr_handle_change_master(router, brkb, error_string);

                if (rc < 0)
                {
                    /* CHANGE MASTER TO has failed */
                    blr_slave_send_error_packet(slave,
                                                error_string,
                                                1234,
                                                "42000");

                    return true;
                }
                else
                {
                    int ret;
                    char error[BINLOG_ERROR_MSG_LEN + 1];

                    /* Write/Update master config into master.ini file */
                    ret = blr_file_write_master_config(router, error);

                    if (ret != 0)
                    {
                        /* file operation failure: restore config */
                        pthread_mutex_lock(&router->lock);

                        blr_master_apply_config(router, current_master);

                        pthread_mutex_unlock(&router->lock);

                        snprintf(error_string,
                                 sizeof(error_string),
                                 "Error writing into %s/master.ini: %s",
                                 router->binlogdir,
                                 error);
                        MXS_ERROR("%s: %s",
                                  router->service->name,
                                  error_string);

                        blr_slave_send_error_packet(slave,
                                                    error_string,
                                                    1201,
                                                    NULL);

                        return true;
                    }

                    /* Mark as active the master server struct */
                    pthread_mutex_lock(&router->lock);
                    if (!router->service->dbref->server->is_active)
                    {
                        router->service->dbref->server->is_active = true;
                        router->service->dbref->active = true;
                    }
                    pthread_mutex_unlock(&router->lock);

                    /**
                     * check if router is BLRM_UNCONFIGURED
                     * and change state to BLRM_SLAVE_STOPPED
                     */
                    if (rc == 1 || router->master_state == BLRM_UNCONFIGURED)
                    {
                        pthread_mutex_lock(&router->lock);

                        router->master_state = BLRM_SLAVE_STOPPED;

                        pthread_mutex_unlock(&router->lock);

                        /*
                         * The binlog server has just been configured
                         * master.ini file written in router->binlogdir.
                         *
                         * Create the binlog_name specified in MASTER_LOG_FILE
                         * only if MariaDB GTID 'mariadb10_master_gtid' is Off
                         */

                        if (!router->mariadb10_master_gtid
                            && blr_file_new_binlog(router, router->binlog_name))
                        {
                            MXS_INFO("%s: 'master.ini' created, binlog file '%s' created",
                                     router->service->name,
                                     router->binlog_name);
                        }
                        blr_slave_send_ok(router, slave);
                        return true;
                    }

                    if (router->trx_safe
                        && router->pending_transaction.state > BLRM_NO_TRANSACTION)
                    {
                        if (strcmp(router->binlog_name, router->prevbinlog) != 0)
                        {
                            char message[BINLOG_ERROR_MSG_LEN + 1] = "";
                            snprintf(message,
                                     BINLOG_ERROR_MSG_LEN,
                                     "1105:Partial transaction in file %s starting at pos %lu, "
                                     "ending at pos %lu will be lost with next START SLAVE command",
                                     current_master.logfile.c_str(),
                                     current_master.safe_pos,
                                     current_master.pos);

                            blr_slave_send_warning_message(router, slave, message);
                            return true;
                        }
                    }

                    /*
                     * The CHAMGE MASTER command might specify a new binlog file.
                     * Let's create the binlog_name specified in MASTER_LOG_FILE
                     * only if MariaDB GTID 'mariadb10_master_gtid' is Off
                     */

                    if (!router->mariadb10_master_gtid
                        && (strlen(router->prevbinlog)
                            && strcmp(router->prevbinlog, router->binlog_name) != 0))
                    {
                        if (blr_file_new_binlog(router, router->binlog_name))
                        {
                            MXS_INFO("%s: created new binlog file '%s' by "
                                     "'CHANGE MASTER TO' command",
                                     router->service->name,
                                     router->binlog_name);
                        }
                    }
                    blr_slave_send_ok(router, slave);
                    return true;
                }
            }
        }
    }
    /* Discnnect conneted client(s) */
    else if (strcasecmp(admin_stmt, "DISCONNECT") == 0)
    {
        if ((word = strtok_r(admin_opts, sep, &brkb)) == NULL)
        {
            MXS_ERROR("%s: Incomplete DISCONNECT command.",
                      router->service->name);
            return false;
        }
        else if (strcasecmp(word, "ALL") == 0)
        {
            blr_slave_disconnect_all(router, slave);
            return true;
        }
        else if (strcasecmp(word, "SERVER") == 0)
        {
            if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
            {
                MXS_ERROR("%s: Expected DISCONNECT SERVER $server_id",
                          router->service->name);
                return false;
            }
            else
            {
                int serverid = atoi(word);
                blr_slave_disconnect_server(router, slave, serverid);
                return true;
            }
        }
    }

    return false;
}

/**
 * Skip reading empty binlog files (4 bytes only)
 *
 * @param    router    Current router instance
 * @param    slave     Current connected slave
 */
static void blr_slave_skip_empty_files(ROUTER_INSTANCE* router,
                                       ROUTER_SLAVE* slave)
{
    char binlog_file[BINLOG_FNAMELEN + 1];
    char router_curr_file[BINLOG_FNAMELEN + 1];
    char file_path[PATH_MAX + 1] = "";
    unsigned int seqno;
    bool skipped_files = false;
    char t_prefix[BINLOG_FILE_EXTRA_INFO] = "";
    MARIADB_GTID_INFO* f_tree = router->storage_type == BLR_BINLOG_STORAGE_TREE ?
        &slave->f_info :
        NULL;
    char next_file[BINLOG_FNAMELEN + 1] = "";

    // Save the current router binlog filename
    pthread_mutex_lock(&router->binlog_lock);
    strcpy(router_curr_file, router->binlog_name);
    pthread_mutex_unlock(&router->binlog_lock);

    // Set the starting filename
    strcpy(binlog_file, slave->binlog_name);

    // Add tree prefix
    if (f_tree)
    {
        sprintf(t_prefix,
                "%" PRIu32 "/%" PRIu32 "/",
                f_tree->gtid_elms.domain_id,
                f_tree->gtid_elms.server_id);
    }

    // Get binlog filename full-path
    blr_get_file_fullpath(binlog_file,
                          router->binlogdir,
                          file_path,
                          t_prefix[0] ? t_prefix : NULL);

    /**
     * Get the next file in sequence or next by GTID maps
     * if current file has 4 bytes size or it doesn't exist at all.
     * Stop if the new file is the current binlog file.
     */
    while (!blr_compare_binlogs(router,
                                &f_tree->gtid_elms,
                                router_curr_file,
                                binlog_file)
           && blr_slave_get_file_size(file_path) <= 4
           && blr_file_next_exists(router, slave, next_file))
    {
        // Log skipped file
        MXS_INFO("Slave %s:%i, skip reading empty file '%s' "
                 "(0 or 4 bytes size).",
                 slave->dcb->remote,
                 dcb_get_port(slave->dcb),
                 binlog_file);

        // Update binlog_file name
        strcpy(binlog_file, next_file);

        // Get binlog file full-path
        blr_get_file_fullpath(binlog_file,
                              router->binlogdir,
                              file_path,
                              t_prefix[0] ? t_prefix : NULL);

        skipped_files = true;
    }

    // One or more files skipped: set last found filename and pos = 4
    if (skipped_files)
    {
        strcpy(slave->binlog_name, binlog_file);
        slave->binlog_pos = 4;
    }
}

/**
 * Get the full path of a binlog filename.
 *
 * @param    binlog_file    The binlog filename
 * @param    root_dir       The binlog storage directory
 * @param    full_path      The output fullpahth name:
 *                          the memory area must be preallocated.
 * @param    t_prefix       The file_tree prefix with rep_domain
 *                          and server_id.
 */
static inline void blr_get_file_fullpath(const char* binlog_file,
                                         const char* root_dir,
                                         char* full_path,
                                         const char* t_prefix)
{
    strcpy(full_path, root_dir);
    strcat(full_path, "/");
    if (t_prefix)
    {
        strcat(full_path, t_prefix);
    }
    strcat(full_path, binlog_file);
}

/**
 * Returns the list of binlog files
 * saved in GTID repo.
 *
 * It's called olny if mariadb10_slave_gtid option is set
 *
 * @param   router        The router instance
 * @param   slave         The connected client
 * @param   extra_data    Whether to dispay path file
 *                        info before filename
 * @retun                 Sent bytes
 */
static int blr_show_binary_logs(ROUTER_INSTANCE* router,
                                ROUTER_SLAVE* slave,
                                const char*   extra_data)
{
    char current_file[BINLOG_FNAMELEN];
    uint64_t current_pos = 0;
    static const char select_query[] = "SELECT binlog_file, "
                                       "MAX(end_pos) AS size, "
                                       "rep_domain, "
                                       "server_id "
                                       "FROM gtid_maps "
                                       "GROUP BY binlog_file "
                                       "ORDER BY id ASC;";
    static const char select_query_full[] = "SELECT binlog_file, "
                                            "MAX(end_pos) AS size, "
                                            "rep_domain, "
                                            "server_id "
                                            "FROM gtid_maps "
                                            "GROUP BY rep_domain, "
                                            "server_id, "
                                            "binlog_file "
                                            "ORDER BY id ASC;";
    int seqno;
    char* errmsg = NULL;
    BINARY_LOG_DATA_RESULT result = {};

    /* Get current binlog finename and position */
    pthread_mutex_lock(&router->binlog_lock);

    strcpy(current_file, router->binlog_name);
    current_pos = router->current_pos;

    pthread_mutex_unlock(&router->binlog_lock);

    /**
     * First part of result set:
     * send 2 columns and their defintions.
     */

    /* This call sets seq to 1 in the packet */
    blr_slave_send_fieldcount(router, slave, 2);
    /* Set 'seqno' counter to next value: 2 */
    seqno = 2;
    /* Col 1 def */
    blr_slave_send_columndef(router,
                             slave,
                             "Log_name",
                             BLR_TYPE_STRING,
                             40,
                             seqno++);
    /* Col 2 def */
    blr_slave_send_columndef(router,
                             slave,
                             "File_size",
                             BLR_TYPE_INT,
                             40,
                             seqno++);
    /* Cols EOF */
    blr_slave_send_eof(router, slave, seqno);
    /* Increment sequence */
    seqno++;

    /* Initialise the result data struct */
    result.seq_no = seqno;
    result.client = slave->dcb;
    result.last_file = NULL;
    result.binlogdir = router->binlogdir;
    result.use_tree = router->storage_type == BLR_BINLOG_STORAGE_TREE;

    /**
     * Second part of result set:
     *
     * add rows for select binlog files.
     *
     * Note:
     * - result.last_file is freed and updated by binary_logs_select_cb()
     * - result.seq_no is increased
     */

    if (sqlite3_exec(router->gtid_maps,
                     !result.use_tree ?
                     select_query :
                     select_query_full,
                     binary_logs_select_cb,
                     &result,
                     &errmsg) != SQLITE_OK)
    {
        MXS_ERROR("Failed to exec 'SELECT binlog_file FROM gtid_maps': "
                  "%s",
                  errmsg ? errmsg : "database is not available");
        sqlite3_free(errmsg);

        /* Free last_file */
        MXS_FREE(result.last_file);
        result.last_file = NULL;

        /* Add EOF for empty result set */
        return blr_slave_send_eof(router, slave, result.seq_no);
    }

    /* Use seqno of last sent packet */
    seqno = result.seq_no;

    /**
     * Check whether the last file is the current binlog file,
     * GTID repo might also contain no data at all.
     *
     * Add the new row if needed.
     */
    if (!result.last_file || strcmp(current_file, result.last_file) != 0)
    {
        char pos[40];   // Buffer for a 64-bit integer.
        GWBUF* pkt;
        /* Free last file */
        MXS_FREE(result.last_file);
        /* Create the string value for pos */
        sprintf(pos, "%" PRIu64, current_pos);

        char* filename;
        char last_filename[BINLOG_FILE_EXTRA_INFO + strlen(current_file) + 1];
        if (result.use_tree)
        {
            char t_prefix[BINLOG_FILE_EXTRA_INFO];
            sprintf(t_prefix,
                    "%" PRIu32 "/%" PRIu32 "/",
                    router->mariadb10_gtid_domain,
                    router->orig_masterid);

            // Add prefix before filename
            sprintf(last_filename,
                    "%s%s",
                    t_prefix,
                    current_file);
            filename = last_filename;
        }
        else
        {
            filename = current_file;
        }

        /* Create & write the new row */
        if ((pkt = blr_create_result_row(filename,
                                         pos,
                                         seqno)) != NULL)
        {
            MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
            /* Increment sequence */
            seqno++;
        }
    }

    /* Add the result set EOF */
    return blr_slave_send_eof(router, slave, seqno);
}

/**
 * Creates a Result Set row with two STRING columns
 *
 * @param   val1      First column value
 * @param   val2      Second column value
 * @param   seq_no    Sequence number for this row
 * @return            An allocated GWBUF or NULL
 */
GWBUF* blr_create_result_row(const char* val1,
                             const char* val2,
                             int seq_no)
{
    int val1_len = strlen(val1);
    int val2_len = strlen(val2);
    GWBUF* pkt;
    uint8_t* ptr;
    int len = MYSQL_HEADER_LEN + (1 + val1_len + (1 + val2_len));

    // Allocate a new GWBUF buffer
    if ((pkt = gwbuf_alloc(len)) == NULL)
    {
        return NULL;
    }
    ptr = GWBUF_DATA(pkt);
    // Add length of data packet
    encode_value(ptr, len - MYSQL_HEADER_LEN, 24);
    ptr += 3;
    // Sequence number in response
    *ptr++ = seq_no;
    // Length of result string "val1"
    *ptr++ = val1_len;
    memcpy((char*)ptr, val1, val1_len);
    ptr += val1_len;
    // Length of result string "val2"
    *ptr++ = val2_len;
    memcpy((char*)ptr, val2, val2_len);

    return pkt;
}

/**
 * Binary logs select callback for sqlite3 database
 *
 * @param data      Data pointer from caller
 * @param cols      Number of columns
 * @param values    The values
 * @param names     The column names
 *
 * @return          0 on success, 1 otherwise
 */
static int binary_logs_select_cb(void* data,
                                 int   cols,
                                 char** values,
                                 char** names)
{
    BINARY_LOG_DATA_RESULT* data_set = (BINARY_LOG_DATA_RESULT*)data;
    DCB* dcb = data_set->client;
    int ret = 1;    // Failure
    uint32_t fsize;
    char file_size[40];

    mxb_assert(cols >= 4 && dcb);

    if (values[0]       // File Name
        && values[1]    // File Size
        && values[2]    // Domain ID
        && values[3])   // Server ID
    {
        GWBUF* pkt;
        char file_path[PATH_MAX + 1];
        char filename[1
                      + strlen(values[0])
                      + BINLOG_FILE_EXTRA_INFO];
        char t_prefix[BINLOG_FILE_EXTRA_INFO] = "";

        sprintf(t_prefix,
                "%s/%s/",
                values[2],      // domain ID
                values[3]);     // server ID

        fsize = atoll(values[1]);

        /* File size != 0 && server ID != 0 */
        mxb_assert(fsize && atoll(values[3]));

        /**
         * In GTID repo binlog file last pos is last GTID.
         * In case of rotate_event or any event the "file_size"
         * it's not correct.
         * In case of binlog files with no transactions at all
         * the saved size is 4.
         *
         * Let's get the real size by calling blr_slave_get_file_size()
         */

        // Get filename full-path, use prefix only if binlog_structure is TREE
        blr_get_file_fullpath(values[0],
                              data_set->binlogdir,
                              file_path,
                              data_set->use_tree ?
                              t_prefix :
                              NULL);
        // Get the file size
        fsize = blr_slave_get_file_size(file_path);

        sprintf(file_size, "%" PRIu32 "", fsize);

        // Include prefix in the output
        if (data_set->use_tree)
        {
            sprintf(filename,
                    "%s%s",
                    t_prefix,
                    values[0]);     // filename
        }
        else
        {
            sprintf(filename, "%s", values[0]);     // filename only
        }

        /* Create the MySQL Result Set row */
        if ((pkt = blr_create_result_row(filename,      // File name
                                         file_size,     // File size
                                         data_set->seq_no)) != NULL)
        {
            /* Increase sequence for next row */
            data_set->seq_no++;
            /* Free last file name */
            MXS_FREE(data_set->last_file);
            /* Set last file name */
            data_set->last_file = MXS_STRDUP_A(values[0]);
            /* Write packet to client */
            MXS_SESSION_ROUTE_REPLY(dcb->session, pkt);
            /* Set success */
            ret = 0;
        }
        return ret;     /* Return success or fallure */
    }
    else
    {
        return 0;   /* Success: no data from db or end of result set */
    }
}

/**
 * Handle SELECT @@server_id, @@read_only
 * that MaxScale MySQL monitor sends to monitored servers
 *
 * @param   router   The router instance
 * @param   slave    The connected client
 * @return           Number of bytes written
 */
static int blr_slave_send_id_ro(ROUTER_INSTANCE* router,
                                ROUTER_SLAVE* slave)
{
    int seqno;
    GWBUF* pkt;
    /**
     * First part of result set:
     * send 2 columns and their defintions.
     */

    /* This call sets seq to 1 in the packet */
    blr_slave_send_fieldcount(router, slave, 2);
    /* Set 'seqno' counter to next value: 2 */
    seqno = 2;
    /* Col 1 def */
    blr_slave_send_columndef(router,
                             slave,
                             "@@server_id",
                             BLR_TYPE_INT,
                             40,
                             seqno++);
    /* Col 2 def */
    blr_slave_send_columndef(router,
                             slave,
                             "@@read_only",
                             BLR_TYPE_INT,
                             40,
                             seqno++);
    /* Cols EOF */
    blr_slave_send_eof(router, slave, seqno++);

    /* Create the MySQL Result Set row */
    char server_id[40] = "";
    /* Set identy for MySQL replication monitor */
    sprintf(server_id,
            "%d",
            router->set_master_server_id ?
            router->masterid :
            router->serverid);

    if ((pkt = blr_create_result_row(server_id, // File name
                                     "0",       // o = OFF
                                     seqno++)) != NULL)
    {
        /* Write packet to client */
        MXS_SESSION_ROUTE_REPLY(slave->dcb->session, pkt);
    }

    /* Add the result set EOF and return */
    return blr_slave_send_eof(router, slave, seqno);
}

/**
 * Handle a SELECT with more than one column.
 *
 * Only SELECT @@server_id, @@read_only is supported.
 * That query is sent by MaxScale MySQL monitor.
 *
 * @param    router    The router instance
 * @param    slave     The connected client
 * @param    col1      The first column
 * @param    coln      Whatever is after first column
 * @return             True is handled, false otherwise
 */
static bool blr_handle_complex_select(ROUTER_INSTANCE* router,
                                      ROUTER_SLAVE* slave,
                                      const char*   col1,
                                      const char*   coln)
{
    /* Strip leading spaces */
    while (isspace(*coln))
    {
        coln++;
    }

    if ((strcasecmp(col1, "@@server_id") == 0
         || strcasecmp(col1, "@@global.server_id") == 0)
        && (strcasecmp(coln, "@@read_only") == 0
            || strcasecmp(coln, "@@global.read_only") == 0))
    {
        blr_slave_send_id_ro(router, slave);
        return true;
    }
    else
    {
        return false;
    }
}

/**
 * Purge binary logs find binlog callback for sqlite3 database
 *
 * @param data      Data pointer from caller
 * @param cols      Number of columns
 * @param values    The values
 * @param names     The column names
 *
 * @return          0 on success, 1 otherwise
 */
static int binary_logs_find_file_cb(void* data,
                                    int   cols,
                                    char** values,
                                    char** names)
{
    mxb_assert(cols == 2);
    BINARY_LOG_DATA_RESULT* data_set = (BINARY_LOG_DATA_RESULT*)data;

    if (values[0])      // Server ID
    {
        data_set->rowid = atoll(values[0]);
    }
    return 0;
}

/**
 * Purge binary logs delete files callback for sqlite3 database
 *
 * @param data      Data pointer from caller
 * @param cols      Number of columns
 * @param values    The values
 * @param names     The column names
 *
 * @return          0 on success, 1 otherwise
 */
static int binary_logs_purge_cb(void* data,
                                int   cols,
                                char** values,
                                char** names)
{
    mxb_assert(cols == 2);

    BINARY_LOG_DATA_RESULT* result_data = (BINARY_LOG_DATA_RESULT*)data;

    if (values[0] && values[1])
    {
        char* filename;
        char full_path[PATH_MAX + 1];

        /* values[0] is filename, values[1] is prefix + file */
        filename = !result_data->use_tree ?
            values[0] :
            values[1];

        sprintf(full_path, "%s/%s", result_data->binlogdir, filename);

        MXS_DEBUG("Deleting binlog file %s", full_path);

        if (unlink(full_path) == -1 && errno != ENOENT)
        {
            MXS_ERROR("Failed to remove binlog file '%s': %d, %s",
                      full_path,
                      errno,
                      mxs_strerror(errno));
        }
        result_data->n_files++;
    }

    return 0;
}

/**
 * Parse the PURGE BINARY LOGS TO 'file' SQL statement.
 *
 * @param purge_command    The SQL command to parse
 * @return                 The file found in the command.
 *                         or NULL in case of parse errors.
 */
static const char* blr_purge_getfile(char* purge_command)
{
    char* word;
    char* brkb;
    const char* sep = " \t";

    word = strtok_r(purge_command, sep, &brkb);

    // Check BINARY
    if (strcasecmp(word, "BINARY") != 0)
    {
        MXS_ERROR("Invalid PURGE command: PURGE %s", word);
        return NULL;
    }

    word = strtok_r(NULL, sep, &brkb);

    // Check LOGS
    if (!word || strcasecmp(word, "LOGS") != 0)
    {
        MXS_ERROR("Invalid PURGE command: PURGE BINARY %s",
                  word ? word : "");
        return NULL;
    }

    word = strtok_r(NULL, sep, &brkb);

    // Nothing else, return error
    if (!word)
    {
        MXS_ERROR("Invalid PURGE command: PURGE BINARY LOGS");
        return NULL;
    }
    else
    // Check for TO 'file'
    {
        if (strcasecmp(word, "TO") != 0)
        {
            MXS_ERROR("Invalid PURGE command: PURGE BINARY LOGS %s", word);
            return NULL;
        }
        // Get filename
        if ((word = strtok_r(NULL, sep, &brkb)) != NULL)
        {
            // Remove heading and trailing "'"
            char* p = word;
            if (*p == '\'')
            {
                word++;
            }
            if (p[strlen(p) - 1] == '\'')
            {
                p[strlen(p) - 1] = '\0';
            }
            return word;
        }
        else
        {
            MXS_ERROR("Invalid PURGE command: PURGE BINARY LOGS TO");
            return NULL;
        }
    }
    return NULL;
}

/**
 * Purge MaxScale binlog files
 *
 * The routine it's called olny if mariadb10_slave_gtid option is set
 * as the up to date list of binlog files is in the GTID maps repo.
 *
 * Note: the current binlog file is not deleted frm disk/db.
 *
 * @param   router        The router instance
 * @param   slave         The connected client
 * @param   purge_opts    The PURGE BINARY LOGS options
 * @retun                 Sent bytes
 */
static bool blr_purge_binary_logs(ROUTER_INSTANCE* router,
                                  ROUTER_SLAVE* slave,
                                  char* purge_opts)
{
    char* errmsg = NULL;
    size_t n_delete = 0;
    // Select first ROWID of user specifed file
    static const char find_file_tpl[] = "SELECT MIN(id) AS min_id, "
                                        "(rep_domain || '/' || "
                                        "server_id || '/' || "
                                        "binlog_file) AS file "
                                        "FROM gtid_maps "
                                        "WHERE binlog_file = '%s' "
                                        "GROUP BY binlog_file "
                                        "ORDER BY id ASC;";
    // SELECT files with ROWID < given one and DELETE
    static const char delete_list_tpl[] = "SELECT binlog_file, "
                                          "(rep_domain || '/' || "
                                          "server_id || '/' || "
                                          "binlog_file) AS file "
                                          "FROM gtid_maps "
                                          "WHERE id < %" PRIu64 " "
                                                                "GROUP BY file "
                                                                "ORDER BY id ASC; "
                                                                "DELETE FROM gtid_maps "
                                                                "WHERE id < %" PRIu64 ";";
    static char sql_stmt[GTID_SQL_BUFFER_SIZE];
    BINARY_LOG_DATA_RESULT result;
    static const char* selected_file;

    /**
     * Parse PURGE BINARY LOGS TO 'file' statement
     */
    if ((selected_file = blr_purge_getfile(purge_opts)) == NULL)
    {
        // Abort on parsing failure
        blr_slave_send_error_packet(slave,
                                    "Malformed PURGE BINARY LOGS TO 'file' detected.",
                                    1064,
                                    "42000");
        return false;
    }

    /* Initialise result data fields */
    result.rowid = 0;
    result.n_files = 0;
    result.binlogdir = router->binlogdir;
    result.use_tree = router->storage_type == BLR_BINLOG_STORAGE_TREE;

    /* Use the provided name, no prefix: find the first row */
    sprintf(sql_stmt,
            find_file_tpl,
            selected_file);

    /* Get file rowid */
    if (sqlite3_exec(router->gtid_maps,
                     sql_stmt,
                     binary_logs_find_file_cb,
                     &result,
                     &errmsg) != SQLITE_OK)
    {
        MXS_ERROR("PURGE BINARY LOGS: failed to select ROWID of current file "
                  "from GTID maps DB, %s, select [%s]",
                  errmsg,
                  sql_stmt);
        sqlite3_free(errmsg);

        blr_slave_send_error_packet(slave,
                                    "Cannot find current file in binlog GTID DB.",
                                    1373,
                                    NULL);
        return false;
    }

    if (result.rowid)
    {
        /* Prepare SQL statement for ROWID < result.rowid */
        sprintf(sql_stmt,
                delete_list_tpl,
                result.rowid,
                result.rowid);

        /* Purge all files with ROWID < result.rowid */
        if (sqlite3_exec(router->gtid_maps,
                         sql_stmt,
                         binary_logs_purge_cb,
                         &result,
                         &errmsg) != SQLITE_OK)
        {
            MXS_ERROR("Failed to select list of files to purge"
                      "from GTID maps DB: %s, select [%s]",
                      errmsg,
                      sql_stmt);
            sqlite3_free(errmsg);

            blr_slave_send_error_packet(slave,
                                        "Cannot build the purge list of files.",
                                        1373,
                                        NULL);
            return false;
        }
    }
    else
    {
        blr_slave_send_error_packet(slave,
                                    "Target log not found in binlog index",
                                    1373,
                                    NULL);
        return false;
    }

    MXS_INFO("Deleted %lu binlog files in %s",
             result.n_files,
             result.binlogdir);

    // Send OK and nithing else
    blr_slave_send_ok(router, slave);

    return true;
}

/**
 * Log previus master details configuration and new added options.
 *
 * @param router            The current router instance
 * @param current_master    The current master server details
 * @param change_master     The options in CHANGE MASTER TO command
 */
static void blr_log_config_changes(ROUTER_INSTANCE* router,
                                   const MasterServerConfig& current_master,
                                   const ChangeMasterConfig& change_master)
{
    /* Prepare heartbeat and retry msgs */
    static const char heartbeat[] = ", MASTER_HEARTBEAT_PERIOD=";
    static const char retry[] = ", MASTER_CONNECT_RETRY=";
    std::string h;
    if (change_master.heartbeat_period != -1)
    {
        h = std::to_string(change_master.heartbeat_period);
    }
    std::string r;
    if (change_master.connect_retry != -1)
    {
        r = std::to_string(change_master.connect_retry);
    }
    char heartbeat_msg[sizeof(heartbeat) + h.length()];
    char retry_msg[sizeof(retry) + r.length()];
    heartbeat_msg[0] = 0;
    retry_msg[0] = 0;

    if (!h.empty())
    {
        sprintf(heartbeat_msg,
                "%s%lu",
                heartbeat,
                router->heartbeat);     // Display the current "long" value
    }

    if (!r.empty())
    {
        sprintf(retry_msg,
                "%s%d",
                retry,
                router->retry_interval);    // Display the current "long" value
    }

    /* Prepare GTID msg */
    const char* gtid_msg =
        !change_master.use_mariadb10_gtid.empty() ?
        ", MASTER_USE_GTID=Slave_pos" :
        "";

    /* Log previous state and new changes */
    MXS_NOTICE("%s: 'CHANGE MASTER TO executed'. Previous state "
               "MASTER_HOST='%s', MASTER_PORT=%i, MASTER_LOG_FILE='%s', "
               "MASTER_LOG_POS=%lu, MASTER_USER='%s'. "
               "New state is MASTER_HOST='%s', MASTER_PORT=%i, "
               "MASTER_LOG_FILE='%s', MASTER_LOG_POS=%lu, "
               "MASTER_USER='%s'"
               "%s%s%s",
               router->service->name,
               current_master.host.c_str(),
               current_master.port,
               current_master.logfile.c_str(),
               current_master.pos,
               current_master.user.c_str(),
               router->service->dbref->server->address,
               router->service->dbref->server->port,
               router->binlog_name,
               router->current_pos,
               router->user,
               gtid_msg,
               heartbeat_msg,
               retry_msg);
}

/**
 * Check whether connecting slave server can continue
 * the MySQL Slave protocol registration to binlog server.
 *
 * (1) Binlog Server should be configured.
 *
 * (2) Connecting Slave must be MariaDB 10 one,
 *   if master->mariadb10_compat is set
 *
 * (3) If mariadb10_master_gtid option is set,
 *   a slave without GTID Request will be rejected.
 *
 * @param router    The router instance
 * @param slave     The registering slave
 * @param check     The check requested by the caller
 * @return          true on succes, false otherwise
 */
static bool blr_check_connecting_slave(const ROUTER_INSTANCE* router,
                                       ROUTER_SLAVE* slave,
                                       enum blr_slave_check check)
{
    int rv = true;
    const char* err_msg = NULL;
    const char* err_status = "HY000";
    int err_code = BINLOG_FATAL_ERROR_READING;
    const char* msg_detail = "";

    switch (check)
    {
    case BLR_SLAVE_CONNECTING:      // (1)
        if (router->master_state == BLRM_UNCONFIGURED)
        {
            err_msg = "Binlog router is not yet configured"
                      " for replication.";
            rv = false;
        }
        break;

    case BLR_SLAVE_IS_MARIADB10:    // (2)
        /**
         * If Master is MariaDB10 don't allow registration from
         * MariaDB/Mysql 5 Slaves
         */
        if (router->mariadb10_compat && !slave->mariadb10_compat)
        {
            err_msg = "MariaDB 10 Slave is required"
                      " for Slave registration.";
            rv = false;
        }
        break;

    case BLR_SLAVE_HAS_MARIADB10_GTID:      // (3)
        /**
         * Check for mariadb10_master_gtid option set to On and
         * connecting slave with GTID request.
         */
        if (router->mariadb10_master_gtid && !slave->mariadb_gtid)
        {
            err_msg = "MariaDB 10 Slave GTID is required"
                      " for Slave registration.";
            msg_detail = " Please use: CHANGE MASTER TO master_use_gtid=slave_pos.";

            rv = false;
        }
        break;

    default:
        MXS_WARNING("%s: Slave %s: Unkwon status check %d.",
                    router->service->name,
                    slave->dcb->remote,
                    check);
        break;
    }

    if (!rv)
    {
        /* Force BLRS_ERRORED state */
        pthread_mutex_lock(&slave->catch_lock);
        slave->state = BLRS_ERRORED;
        pthread_mutex_unlock(&slave->catch_lock);

        /* Send error that stops slave replication */
        blr_send_custom_error(slave->dcb,
                              ++slave->seqno,
                              0,
                              err_msg,
                              err_status,
                              err_code);
        MXS_ERROR("%s: Slave %s: %s%s",
                  router->service->name,
                  slave->dcb->remote,
                  err_msg,
                  msg_detail);
    }

    return rv;
}

/**
 * Abort the change master process
 *
 * @param router            The router instance
 * @param current_master    Current master configuration
 * @param change_master     The CHANGE MASTER TO options
 * @param error             Error message to log
 */
static void blr_abort_change_master(ROUTER_INSTANCE* router,
                                    const MasterServerConfig& current_master,
                                    const char* error)
{
    MXS_ERROR("%s: %s", router->service->name, error);
    /* restore previous master_host and master_port */
    blr_master_restore_config(router, current_master);
}

/**
 * Abort the COM_BINLOG_DUMP slave request
 *
 * @param slave     The connecting slave
 * @param errmsg    Error message to send and log
 */
static void blr_slave_abort_dump_request(ROUTER_SLAVE* slave,
                                         const char*   errmsg)
{
    MXS_ERROR("Slave %lu requests COM_BINLOG_DUMP: %s. Aborting.",
              (unsigned long)slave->serverid,
              errmsg);

    blr_send_custom_error(slave->dcb,
                          slave->seqno++,
                          0,
                          errmsg,
                          "HY000",
                          BINLOG_FATAL_ERROR_READING);
}

/**
 * Check whether the current binlog file can be changed
 *
 * @param router          The curret router instance
 * @param change_master   The options in CHANGE_MASTER TO command
 * @param error           Pointer to outout error message
 *
 * @return                True if binlog can be changed or false.
 */
static bool blr_binlog_change_check(const ROUTER_INSTANCE* router,
                                    const ChangeMasterConfig& change_master,
                                    char* error)
{
    char* master_logfile = NULL;

    /**
     * MASTER_LOG_FILE is not set in CHANGE MASTER TO.
     * If binlog server is not configured and
     * mariadb10_master_gtid is not set, then return an error.
     */
    if (change_master.binlog_file.empty())
    {
        if (router->master_state == BLRM_UNCONFIGURED
            && !router->mariadb10_master_gtid)
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "Router is not configured for master connection, "
                     "MASTER_LOG_FILE is required");

            return false;
        }
        else
        {
            return true;
        }
    }
    else
    /**
     * If binlog file is set in CHANGE MASTER TO
     * and MASTER_USE_GTID option is on, then return an error.
     */
    {
        /**
         * Check first MASTER_USE_GTID option:
         *
         * if not present return an error:
         */
        if (router->mariadb10_master_gtid
            && change_master.use_mariadb10_gtid.empty())
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "%s MASTER_USE_GTID=Slave_pos is required",
                     router->master_state == BLRM_UNCONFIGURED ?
                     "Router is not configured for master connection," :
                     "Cannot use MASTER_LOG_FILE for master connection,");

            return false;
        }
        else
        {
            return true;
        }
    }

    return true;
}

/**
 * Set the new binlog filenanme from MASTER_LOG_FILE
 *
 * If no name change, the current router file is set.
 * Note:
 *  Empty filename, MASTER_LOG_FILE = '', can be passed only
 *  if mariadb10_master_gtid option is set.
 *
 * @param router         The router instance
 * @param binlog_file    The binlog file in MASTER_LOG_FILE
 * @param error          Ouput error message
 * @param new_logfile    The new log file name
 *
 * @return    True if binlog name has been set or false
 *            on errors
 */
static bool blr_change_binlog_name(ROUTER_INSTANCE* router,
                                   const char* binlog_file,
                                   char** new_logfile,
                                   char*  error)
{
    bool ret = true;

    /* MASTER_LOG_FILE is not present in CHANGE MASTER TO */
    if (binlog_file == NULL)
    {
        /* Use current binlog file */
        *new_logfile = MXS_STRDUP_A(router->binlog_name);
    }
    else
    {
        /**
         * Change the binlog filename as from MASTER_LOG_FILE
         * New binlog file can be:
         * - the next in sequence router file
         * - current router file
         * - empty if router->mariadb10_master_gtid is set.
         */
        *new_logfile = blr_set_master_logfile(router,
                                              binlog_file,
                                              error);
        if (*new_logfile == NULL)
        {
            if (!router->mariadb10_master_gtid
                || strlen(binlog_file) > 1)
            {
                /* Binlog name can not be changed */
                ret = false;
            }
            else
            {
                *new_logfile = MXS_STRDUP_A("");
                // Blank the error message
                error[0] = 0;
            }
        }
    }
    return ret;
}

static bool blr_change_binlog_name(ROUTER_INSTANCE* router,
                                   const std::string& binlog_file,
                                   char** new_logfile,
                                   char*  error)
{
    return blr_change_binlog_name(router,
                                  binlog_file.empty() ? nullptr : binlog_file.c_str(),
                                  new_logfile,
                                  error);
}

/**
 * Apply binlog filename and position changes
 *
 * @param router           The current router instance
 * @param change_master    The change master options
 * @param error            The error message to fill
 * @param new_logfile      The new binlog filename
 *                         set in previous stage
 *
 * @return                 True if changes have been set
 *                         or false on errors.
 */
static bool blr_apply_changes(ROUTER_INSTANCE* router,
                              const ChangeMasterConfig& change_master,
                              char* new_logfile,
                              char* error)
{
    bool ret = true;
    const char* master_log_pos = NULL;
    long long pos = 0;

    /* Set new binlog position from MASTER_LOG_POS */
    master_log_pos = change_master.binlog_pos.empty() ? nullptr : change_master.binlog_pos.c_str();
    if (master_log_pos == NULL)
    {
        pos = 0;
    }
    else
    {
        pos = atoll(master_log_pos);
    }

    /**
     * Binlog name and position are checked if:
     * - mariadb10_master_gtid is off and
     * - master connection is already configured.
     * Checks:
     *  (1) filename is different from current router log file and pos != 4
     *  (2) position is not current one for current file
     *
     * Binlog file and pos can set after all checks.
     */

    /* MariaDB 10 GTID request */
    if (router->mariadb10_master_gtid)
    {
        if (!change_master.use_mariadb10_gtid.empty())
        {
            /* MASTER_USE_GTID=Slave_pos is set */
            MXS_INFO("%s: MASTER_USE_GTID is [%s]",
                     router->service->name,
                     change_master.use_mariadb10_gtid.c_str());
        }

        /* Always log the current GTID value with CHANGE_MASTER TO */
        MXS_INFO("%s: CHANGE MASTER TO, current GTID value is [%s]",
                 router->service->name,
                 router->last_mariadb_gtid);

        /* Always set empty filename at pos 4 with CHANGE_MASTER TO */
        strcpy(router->binlog_name, "");

        router->current_pos = 4;
        router->binlog_position = 4;
        router->current_safe_event = 4;
    }
    /* The new filename is not the current one */
    else if (strcmp(new_logfile, router->binlog_name) != 0
             && router->master_state != BLRM_UNCONFIGURED)
    {
        if (master_log_pos == NULL)
        {
            snprintf(error,
                     BINLOG_ERROR_MSG_LEN,
                     "Please provide an explicit MASTER_LOG_POS "
                     "for new MASTER_LOG_FILE %s: "
                     "Permitted binlog pos is %d. "
                     "Current master_log_file=%s, master_log_pos=%lu",
                     new_logfile,
                     4,
                     router->binlog_name,
                     router->current_pos);

            ret = false;
        }
        else
        {
            if (pos != 4)
            {
                snprintf(error,
                         BINLOG_ERROR_MSG_LEN,
                         "Can not set MASTER_LOG_POS to %s for "
                         "MASTER_LOG_FILE %s: "
                         "Permitted binlog pos is %d. "
                         "Current master_log_file=%s, master_log_pos=%lu",
                         master_log_pos,
                         new_logfile,
                         4,
                         router->binlog_name,
                         router->current_pos);

                ret = false;
            }
        }

        /* Set new binlog name at pos 4 */
        if (ret)
        {
            strcpy(router->binlog_name, new_logfile);

            router->current_pos = 4;
            router->binlog_position = 4;
            router->current_safe_event = 4;

            /**
             * Close current file binlog file,
             * next start slave will create the new one
             */
            fsync(router->binlog_fd);
            close(router->binlog_fd);
            router->binlog_fd = -1;

            MXS_INFO("%s: New MASTER_LOG_FILE is [%s]",
                     router->service->name,
                     router->binlog_name);
        }
    }
    else
    {
        /**
         * Same binlog or master connection not configured
         * Position cannot be different from
         * current pos or 4 (if BLRM_UNCONFIGURED).
         * Note:
         * pos is not checked if router->mariadb10_master_gtid is set
         */
        if (router->master_state == BLRM_UNCONFIGURED)
        {
            if (master_log_pos != NULL && pos != 4)
            {
                snprintf(error,
                         BINLOG_ERROR_MSG_LEN,
                         "Can not set MASTER_LOG_POS to %s: "
                         "Permitted binlog pos is 4. Specified master_log_file=%s",
                         master_log_pos,
                         new_logfile);

                ret = false;
            }
            else
            {
                /**
                 * BLRM_UNCONFIGURED state:
                 * - set pos to 4
                 * - set binlog name
                 */
                router->current_pos = 4;
                router->binlog_position = 4;
                router->current_safe_event = 4;
                strcpy(router->binlog_name, new_logfile);

                MXS_INFO("%s: New MASTER_LOG_FILE is [%s]",
                         router->service->name,
                         router->binlog_name);
            }
        }
        else
        {
            if (master_log_pos != NULL
                && pos != static_cast<long long>(router->current_pos))
            {
                snprintf(error,
                         BINLOG_ERROR_MSG_LEN,
                         "Can not set MASTER_LOG_POS to %s: "
                         "Permitted binlog pos is %lu. "
                         "Current master_log_file=%s, master_log_pos=%lu",
                         master_log_pos,
                         router->current_pos,
                         router->binlog_name,
                         router->current_pos);

                ret = false;
            }
        }

        if (ret)
        {
            MXS_INFO("%s: New MASTER_LOG_POS is [%lu]",
                     router->service->name,
                     router->current_pos);
        }
    }

    return ret;
}

/**
 * Saves a MARIADB_GTID_INFO data for later usage
 *
 * @param info           The MARIADB_GTID_INFO data to copy
 * @param save_info      The MARIADB_GTID_INFO allocated
 *                       buffer to save data
 * @param save_prefix    The allocated buffer where
 *                       to save file prefix
 */
static void blr_slave_info_save(const MARIADB_GTID_INFO* info,
                                MARIADB_GTID_INFO* save_info,
                                char* save_prefix)
{
    /* Save current file details */
    memcpy(save_info, info, sizeof(MARIADB_GTID_INFO));

    /* Fill save file prefix */
    sprintf(save_prefix,
            "%" PRIu32 "/%" PRIu32 "/",
            save_info->gtid_elms.domain_id,
            save_info->gtid_elms.server_id);
}

/**
 * Log message for slave file End Of File
 *
 * @param router        The current router instance
 * @param slave         The connected slave
 * @param c_prefix      The file prefix of slave file
 * @param next_file     The next file to read or fake rotate to
 * @param log_action    The action type to log
 */
static void blr_slave_log_next_file_action(const ROUTER_INSTANCE* router,
                                           const ROUTER_SLAVE* slave,
                                           const char* c_prefix,
                                           const char* next_file,
                                           slave_eof_action_t log_action)
{
    char m_prefix[BINLOG_FILE_EXTRA_INFO] = "";
    char r_prefix[BINLOG_FILE_EXTRA_INFO] = "";
    bool s_tree = router->storage_type == BLR_BINLOG_STORAGE_TREE;
    bool have_heartbeat = router->send_slave_heartbeat
        && (slave->heartbeat > 0);

    pthread_mutex_lock(&router->binlog_lock);
    if (s_tree)
    {
        /* Get master file prefix */
        sprintf(m_prefix,
                "%" PRIu32 "/%" PRIu32 "/",
                router->mariadb10_gtid_domain,
                router->orig_masterid);
        /* Get rotating slave file prefix */
        sprintf(r_prefix,
                "%" PRIu32 "/%" PRIu32 "/",
                slave->f_info.gtid_elms.domain_id,
                slave->f_info.gtid_elms.server_id);
    }
    pthread_mutex_unlock(&router->binlog_lock);

    switch (log_action)
    {
    case SLAVE_EOF_ROTATE:
        /* This has to be always logged */
        MXS_WARNING("%s: Slave [%s]:%d, server-id %d reached end of file for binlog file [%s%s] "
                    "at %lu which is not the file currently being downloaded or last file found. "
                    "This may be caused by a previous failure of the master. "
                    "Current master binlog is [%s%s] at %lu, replication state is [%s]. "
                    "Now rotating to new file [%s%s]",
                    router->service->name,
                    slave->dcb->remote,
                    dcb_get_port(slave->dcb),
                    slave->serverid,
                    c_prefix,
                    slave->binlog_name,
                    (unsigned long)slave->binlog_pos,
                    m_prefix,
                    router->binlog_name[0] ? router->binlog_name : "no_set_yet",
                    router->binlog_position,
                    blrm_states[router->master_state],
                    r_prefix,
                    next_file);
        break;

    case SLAVE_EOF_ERROR:
        /* Log error */
        MXS_ERROR("%s: Slave [%s]:%d, server-id %d reached "
                  "end of file for '%s%s' and next file to read%s%s%s%s "
                  "is not %s. Force replication abort after %d retries.",
                  router->service->name,
                  slave->dcb->remote,
                  dcb_get_port(slave->dcb),
                  slave->serverid,
                  c_prefix,
                  slave->binlog_name,
                  next_file[0] ? " '" : "",
                  next_file[0] ? r_prefix : "",
                  next_file,
                  next_file[0] ? "'" : "",
                  next_file[0] ? "accessible" : "existent",
                  MISSING_FILE_READ_RETRIES);
        break;

    case SLAVE_EOF_WARNING:
        /* We don't have the next_file, just warning */
        MXS_WARNING("%s: Slave [%s]:%d, server-id %d reached end "
                    "of file for binlog file [%s%s] "
                    "at %lu. This is the last downloaded or "
                    "the last file found. "
                    "Next file%s%s%s%s is not %s. "
                    "This may be caused by a previous failure of "
                    "the master server. Current master binlog is "
                    "[%s%s] at %lu and replication state is [%s]. "
                    "The slave server is now in '%s' state.",
                    router->service->name,
                    slave->dcb->remote,
                    dcb_get_port(slave->dcb),
                    slave->serverid,
                    c_prefix,
                    slave->binlog_name,
                    (unsigned long)slave->binlog_pos,
                    next_file[0] ? " '" : "",
                    next_file[0] ? r_prefix : "",
                    next_file,
                    next_file[0] ? "'" : "",
                    next_file[0] ? "accessible" : "existent",
                    m_prefix,
                    router->binlog_name[0] ? router->binlog_name : "no_set_yet",
                    router->binlog_position,
                    blrm_states[router->master_state],
                    have_heartbeat ? "wait_state" : "read_again");
        break;

    default:
        break;
    }
}
