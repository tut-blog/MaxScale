#pragma once
#ifndef TEST_UTILS_H
#define TEST_UTILS_H
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

#include <maxscale/cdefs.h>
#include <maxbase/maxbase.hh>
#include <maxscale/dcb.h>
#include <maxscale/housekeeper.h>
#include <maxscale/maxscale_test.h>
#include <maxscale/log.h>
#include <maxscale/config.h>
#include <maxscale/query_classifier.h>
#include <maxscale/paths.h>
#include <maxscale/alloc.h>
#include <maxscale/routingworker.hh>

#include <sys/stat.h>

#include "../internal/poll.hh"


void init_test_env(char* path)
{
    config_get_global_options()->n_threads = 1;

    if (!mxs_log_init(NULL, NULL, MXS_LOG_TARGET_STDOUT))
    {
        exit(1);
    }
    atexit(mxs_log_finish);
    dcb_global_init();
    set_libdir(MXS_STRDUP(TEST_DIR "/query_classifier/qc_sqlite/"));
    qc_setup(NULL, QC_SQL_MODE_DEFAULT, NULL, NULL);
    qc_process_init(QC_INIT_BOTH);
    poll_init();
    maxbase::init();
    maxscale::RoutingWorker::init();
    hkinit();
}

#endif
