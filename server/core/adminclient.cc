/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "maxscale/adminclient.hh"
#include "maxscale/httprequest.hh"
#include "maxscale/httpresponse.hh"

#include <string>
#include <sstream>

#include <maxscale/atomic.h>
#include <maxscale/hk_heartbeat.h>
#include <maxscale/log_manager.h>
#include <maxscale/thread.h>
#include <maxscale/spinlock.hh>

using std::string;
using std::stringstream;
using mxs::SpinLockGuard;

AdminClient::AdminClient(int fd, const struct sockaddr_storage& addr, int timeout):
    m_fd(fd),
    m_last_activity(atomic_read_int64(&hkheartbeat)),
    m_addr(addr)
{
}

void AdminClient::close_connection()
{
    SpinLockGuard guard(m_lock);

    if (m_fd != -1)
    {
        close(m_fd);
        m_fd = -1;
    }
}

AdminClient::~AdminClient()
{
    close_connection();
}

static bool read_request(int fd, string& output)
{
    while (true)
    {
        char buf[1024];
        int bufsize = sizeof(buf) - 1;

        int rc = read(fd, buf, bufsize);

        if (rc == -1)
        {
            return false;
        }

        buf[rc] = '\0';
        output += buf;

        if (rc < bufsize)
        {
            /** Complete request read */
            break;
        }
    }

    return true;
}

static bool write_response(int fd, const string& body)
{
    return write(fd, body.c_str(), body.length()) != -1;
}

void AdminClient::process()
{
    string request;
    atomic_write_int64(&m_last_activity, hkheartbeat);

    if (read_request(m_fd, request))
    {
        string response;
        SHttpRequest parser(HttpRequest::parse(request));
        atomic_write_int64(&m_last_activity, hkheartbeat);

        if (parser.get())
        {
            /** Valid request */
            response = HttpResponse(parser->get_json_str()).get_response();
        }
        else
        {
            request = HttpResponse("", HTTP_400_BAD_REQUEST).get_response();
        }

        if (!write_response(m_fd, response))
        {
            MXS_ERROR("Failed to write response to client: %d, %s", errno, mxs_strerror(errno));
        }
    }
    else
    {
        MXS_ERROR("Failed to read client request: %d, %s", errno, mxs_strerror(errno));
    }
}