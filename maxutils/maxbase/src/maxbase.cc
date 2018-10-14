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

#include <maxbase/maxbase.hh>
#include <maxbase/log.hh>
#include <maxbase/messagequeue.hh>
#include <maxbase/worker.hh>

namespace maxbase
{

class Initer
{
    Initer() = delete;
    Initer(const Initer&) = delete;
    Initer& operator=(const Initer&) = delete;

public:
    static bool init()
    {
        bool rv = false;

        int i;
        for (i = 0; i < s_nComponents; ++i)
        {
            if (!s_components[i].init())
            {
                break;
            }
        }

        if (i == s_nComponents)
        {
            rv = true;
        }
        else if (i != 0)
        {
            // We need to finalize in reverse order the components that
            // successfully were initialized.
            for (int j = i - 1; j >= 0; --j)
            {
                s_components[j].finish();
            }
        }

        return rv;
    }

    static void finish()
    {
        for (int i = s_nComponents - 1; i >= 0; --i)
        {
            s_components[i].finish();
        }
    }

private:
    typedef bool (* init_function_t)();
    typedef void (* finish_function_t)();

    struct component_t
    {
        init_function_t   init;
        finish_function_t finish;
    };

    static component_t s_components[];
    static int         s_nComponents;
};

#define MAXBASE_COMPONENT(X) {&X::init, &X::finish}

Initer::component_t Initer::s_components[] =
{
    MAXBASE_COMPONENT(MessageQueue),
    MAXBASE_COMPONENT(Worker),
};

int Initer::s_nComponents = sizeof(Initer::s_components) / sizeof(Initer::s_components[0]);

MaxBase::MaxBase(const char* zIdent,
                 const char* zLogdir,
                 const char* zFilename,
                 mxb_log_target_t target,
                 mxb_log_context_provider_t context_provider)
    : m_log_inited(false)
{
    const char* zMessage = nullptr;

    if (maxbase::init())
    {
        m_log_inited = mxb_log_init(zIdent, zLogdir, zFilename, target, context_provider);

        if (!m_log_inited)
        {
            zMessage =
                "The initialization of the MaxScale base library succeeded, but the "
                "initialization of the MaxScale log failed.";
        }
    }
    else
    {
        zMessage = "The initialization of the MaxScale base library failed.";
    }

    if (zMessage)
    {
        throw std::runtime_error(zMessage);
    }
}

bool init()
{
    bool rv = false;
    bool log_exists = mxb_log_inited();
    bool log_inited_locally = false;

    if (!log_exists)
    {
        // We temporarily initialize a log logging to stdout, so that it is possible
        // to log in the initialization functions. This should always succeed.
        log_inited_locally = mxb_log_init(MXB_LOG_TARGET_STDOUT);

        if (log_inited_locally)
        {
            log_exists = true;
        }
        else
        {
            // Out of luck, just write to stderr.
            fprintf(stderr, "MaxScale: Fatal error, could not initialize a temporary log.\n");
        }
    }

    if (log_exists)
    {
        rv = Initer::init();
    }

    if (log_inited_locally)
    {
        // Finalize the temporary log.
        mxb_log_finish();
    }

    return rv;
}

void finish()
{
    Initer::finish();
}
}

bool maxbase_init()
{
    return maxbase::init();
}

void maxbase_finish()
{
    return maxbase::finish();
}
