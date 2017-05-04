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

#include "maxscale/httprequest.hh"
#include "maxscale/admin.hh"

#include <ctype.h>
#include <string.h>

using std::string;
using std::deque;

#define HTTP_HOST_HEADER "Host"

const std::string HttpRequest::HTTP_PREFIX = "http://";
const std::string HttpRequest::HTTPS_PREFIX = "https://";

/** TODO: Move this to a C++ string utility header */
namespace maxscale
{
static inline string& trim(string& str)
{
    if (str.length())
    {
        if (isspace(*str.begin()))
        {
            string::iterator it = str.begin();

            while (it != str.end() && isspace(*it))
            {
                it++;
            }
            str.erase(str.begin(), it);
        }

        if (isspace(*str.rbegin()))
        {
            string::reverse_iterator it = str.rbegin();
            while (it != str.rend() && isspace(*it))
            {
                it++;
            }

            str.erase(it.base(), str.end());
        }
    }

    return str;
}
}

static void process_uri(string& uri, std::deque<string>& uri_parts)
{
    /** Clean up trailing slashes in requested resource */
    while (uri.length() > 1 && *uri.rbegin() == '/')
    {
        uri.erase(uri.find_last_of("/"));
    }

    string my_uri = uri;

    while (my_uri.length() && *my_uri.begin() == '/')
    {
        my_uri.erase(my_uri.begin());
    }

    while (my_uri.length() > 0)
    {
        size_t pos = my_uri.find("/");
        string part = pos == string::npos ? my_uri : my_uri.substr(0, pos);
        my_uri.erase(0, pos == string::npos ? pos : pos + 1);
        uri_parts.push_back(part);
    }
}

HttpRequest::HttpRequest(struct MHD_Connection *connection, string url, string method, json_t* data):
    m_json(data),
    m_json_string(data ? mxs::json_dump(data, 0) : ""),
    m_resource(url),
    m_verb(method),
    m_connection(connection)
{
    process_uri(url, m_resource_parts);

    m_hostname = mxs_admin_https_enabled() ? HttpRequest::HTTPS_PREFIX : HttpRequest::HTTP_PREFIX;
    m_hostname += get_header(HTTP_HOST_HEADER);

    if (m_hostname[m_hostname.size() - 1] != '/')
    {
        m_hostname += "/";
    }

    m_hostname += MXS_REST_API_VERSION;
}

HttpRequest::~HttpRequest()
{

}
bool HttpRequest::validate_api_version()
{
    bool rval = false;

    if (m_resource_parts.size() > 0 &&
        m_resource_parts[0] == MXS_REST_API_VERSION)
    {
        m_resource_parts.pop_front();
        rval = true;
    }

    return rval;
}
