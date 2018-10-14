/**
 * @file session_limits.cpp - test for 'max_sescmd_history' and 'connection_timeout' parameters
 * - add follwoling to router configuration
 * @verbatim
 *  connection_timeout=30
 *  router_options=max_sescmd_history=10
 *  @endverbatim
 * - open session
 * - wait 20 seconds, check if session is alive, expect ok
 * - wait 20 seconds more, check if session is alive, expect failure
 * - open new session
 * - execute 10 session commands
 * - check if session is alive, expect ok
 * - execute one more session commad, excpect failure
 */

#include "testconnections.h"

int main(int argc, char* argv[])
{
    TestConnections test(argc, argv);
    int first_sleep = 5;
    int second_sleep = 12;

    test.set_timeout(200);

    test.tprintf("Open session, wait %d seconds and execute a query", first_sleep);
    test.maxscales->connect_maxscale(0);
    sleep(first_sleep);
    test.try_query(test.maxscales->conn_rwsplit[0], "SELECT 1");

    test.tprintf("Wait %d seconds and execute query, expecting failure", second_sleep);
    sleep(second_sleep);
    test.add_result(execute_query(test.maxscales->conn_rwsplit[0], "SELECT 1") == 0,
                    "Session was not closed after %d seconds",
                    second_sleep);
    test.maxscales->close_maxscale_connections(0);

    test.tprintf("Open session and execute 10 session commands");
    test.maxscales->connect_maxscale(0);
    for (int i = 0; i < 10; i++)
    {
        test.try_query(test.maxscales->conn_rwsplit[0],
                       "%s",
                       std::string("set @test=" + std::to_string(i)).c_str());
    }

    test.tprintf("Execute one more session command and expect message in error log");
    execute_query(test.maxscales->conn_rwsplit[0], "set @test=11");
    sleep(1);
    test.check_log_err(0, "Router session exceeded session command history limit", true);
    test.maxscales->close_maxscale_connections(0);

    return test.global_result;
}
