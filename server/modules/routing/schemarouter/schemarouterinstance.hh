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

#include "schemarouter.hh"

#include <mutex>
#include <set>
#include <string>

#include <maxscale/router.hh>
#include <maxscale/pcre2.h>

#include "schemaroutersession.hh"

namespace schemarouter
{

class SchemaRouterSession;

/**
 * The per instance data for the router.
 */
class SchemaRouter : public mxs::Router<SchemaRouter, SchemaRouterSession>
{
public:
    ~SchemaRouter();
    static SchemaRouter* create(SERVICE* pService, MXS_CONFIG_PARAMETER* params);
    SchemaRouterSession* newSession(MXS_SESSION* pSession);
    void                 diagnostics(DCB* pDcb);
    json_t*              diagnostics_json() const;
    uint64_t             getCapabilities();
    bool                 configure(MXS_CONFIG_PARAMETER* param);

private:
    friend class SchemaRouterSession;

    /** Internal functions */
    SchemaRouter(SERVICE* service, SConfig config);

    /** Member variables */
    SConfig      m_config;          /*< expanded config info from SERVICE */
    ShardManager m_shard_manager;   /*< Shard maps hashed by user name */
    SERVICE*     m_service;         /*< Pointer to service */
    std::mutex   m_lock;            /*< Lock for the instance data */
    Stats        m_stats;           /*< Statistics for this router */
};
}
