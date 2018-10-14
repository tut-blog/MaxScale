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
 * @file readconnection.h - The read connection balancing query module heder file
 *
 * @verbatim
 * Revision History
 *
 * Date     Who     Description
 * 14/06/13 Mark Riddoch    Initial implementation
 * 27/06/14 Mark Riddoch    Addition of server weight percentage
 *
 * @endverbatim
 */

#define MXS_MODULE_NAME "readconnroute"

#include <maxscale/cdefs.h>
#include <maxscale/dcb.h>
#include <maxscale/service.h>
#include <maxscale/router.h>

/**
 * The client session structure used within this router.
 */
struct ROUTER_CLIENT_SES : MXS_ROUTER_SESSION
{
    SERVER_REF* backend;    /*< Backend used by the client session */
    DCB*        backend_dcb;/*< DCB Connection to the backend      */
    DCB*        client_dcb; /**< Client DCB */
    uint32_t    bitmask;    /*< Bitmask to apply to server->status */
    uint32_t    bitvalue;   /*< Session specific required value of server->status */
};

/**
 * The statistics for this router instance
 */
struct ROUTER_STATS
{
    int n_sessions;     /*< Number sessions created     */
    int n_queries;      /*< Number of queries forwarded */
};

/**
 * The per instance data for the router.
 */
struct ROUTER_INSTANCE : public MXS_ROUTER
{
    SERVICE*     service;               /*< Pointer to the service using this router */
    uint64_t     bitmask_and_bitvalue;  /*< Lower 32-bits for bitmask and upper for bitvalue */
    ROUTER_STATS stats;                 /*< Statistics for this router               */
};
