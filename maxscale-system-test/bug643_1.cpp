/**
 * @file bug643.cpp  regression case for bugs 643 ("Hints, RWSplit: MaxScale goes into infinite loop and
 * crashes") and bug645
 * - setup RWSplit in the following way for bug643
 * @verbatim
 *  [hints]
 *  type=filter
 *  module=hintfilter
 *
 *
 *  [regex]
 *  type=filter
 *  module=regexfilter
 *  match=fetch
 *  replace=select
 *
 *  [typo]
 *  type=filter
 *  module=regexfilter
 *  match=[Ff][Oo0][Rr][Mm]
 *  replace=from
 *
 *  [qla]
 *  type=filter
 *  module=qlafilter
 *  options=/tmp/QueryLog
 *
 *  [duplicate]
 *  type=filter
 *  module=tee
 *  service=RW Split2
 *
 *  [testfilter]
 *  type=filter
 *  module=foobar
 *
 *  [RW Split Router]
 *  type=service
 *  router=readwritesplit
 *  servers=server1,server2,server3,server4
 #servers=server1,server2
 *  max_slave_connections=100%
 *  use_sql_variables_in=all
 #use_sql_variables_in=master
 *  user=skysql
 *  passwd=skysql
 #filters=typo|qla|regex|hints|regex|hints
 #enable_root_user=1
 *  filters=duplicate
 *
 *  [RW Split2]
 *  type=service
 *  router=readwritesplit
 *  servers=server1,server2,server3,server4
 *  max_slave_connections=100%
 *  use_sql_variables_in=all
 *  user=skysql
 *  passwd=skysql
 *  filters=qla|tests|hints
 *
 *  @endverbatim
 * - try to connect
 * - try simple query using all services
 * - check warnig in the log "Failed to start service 'RW Split2"
 * - check if Maxscale still alive
 */


#include <iostream>
#include "testconnections.h"

int main(int argc, char* argv[])
{
    TestConnections* Test = new TestConnections(argc, argv);
    Test->set_timeout(10);

    Test->tprintf("Trying to connect to all Maxscale services\n");
    fflush(stdout);
    Test->maxscales->connect_maxscale(0);
    Test->tprintf("Trying to send query to RWSplit\n");
    fflush(stdout);
    execute_query(Test->maxscales->conn_rwsplit[0], (char*) "show processlist");
    Test->tprintf("Trying to send query to ReadConn master\n");
    fflush(stdout);
    execute_query(Test->maxscales->conn_master[0], (char*) "show processlist");
    Test->tprintf("Trying to send query to ReadConn slave\n");
    fflush(stdout);
    execute_query(Test->maxscales->conn_slave[0], (char*) "show processlist");
    Test->maxscales->close_maxscale_connections(0);

    Test->check_log_err(0, (char*) "Unable to find filter 'tests' for service 'RW Split2'", true);

    int rval = Test->global_result;
    delete Test;
    return rval;
}
