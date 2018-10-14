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

#include <iostream>
#include <maxbase/assert.h>
#include <maxbase/maxbase.hh>
#include <maxbase/worker.hh>

using namespace maxbase;
using namespace std;

namespace
{

// TODO: Put this in some common place.
int64_t get_monotonic_time_ms()
{
    struct timespec ts;
    MXB_AT_DEBUG(int rv = ) clock_gettime(CLOCK_MONOTONIC, &ts);
    mxb_assert(rv == 0);

    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

class TimerTest
{
public:
    static int s_ticks;

    TimerTest(Worker* pWorker, int* pRv, int32_t delay)
        : m_id(s_id++)
        , m_worker(*pWorker)
        , m_delay(delay)
        , m_at(get_monotonic_time_ms() + delay)
        , m_rv(*pRv)
    {
    }

    int32_t delay() const
    {
        return m_delay;
    }

    bool tick(Worker::Call::action_t action)
    {
        bool rv = false;

        if (action == Worker::Call::EXECUTE)
        {
            int64_t now = get_monotonic_time_ms();
            int64_t diff = abs(now - m_at);

            cout << m_id << ": " << diff << endl;

            if (diff > 50)
            {
                cout << "Error: Difference between expected and happened > 50: " << diff << endl;
                m_rv = EXIT_FAILURE;
            }

            m_at += m_delay;

            if (--s_ticks < 0)
            {
                m_worker.shutdown();
            }

            rv = true;
        }

        return rv;
    }

private:
    static int s_id;

    int     m_id;
    Worker& m_worker;
    int32_t m_delay;
    int64_t m_at;
    int&    m_rv;
};

int TimerTest::s_id = 1;
int TimerTest::s_ticks;

int run()
{
    int rv = EXIT_SUCCESS;

    TimerTest::s_ticks = 100;

    Worker w;

    TimerTest t1(&w, &rv, 200);
    TimerTest t2(&w, &rv, 300);
    TimerTest t3(&w, &rv, 400);
    TimerTest t4(&w, &rv, 500);
    TimerTest t5(&w, &rv, 600);

    w.delayed_call(t1.delay(), &TimerTest::tick, &t1);
    w.delayed_call(t2.delay(), &TimerTest::tick, &t2);
    w.delayed_call(t3.delay(), &TimerTest::tick, &t3);
    w.delayed_call(t4.delay(), &TimerTest::tick, &t4);
    w.delayed_call(t5.delay(), &TimerTest::tick, &t5);

    w.run();

    return EXIT_SUCCESS;
}
}

int main()
{
    mxb::MaxBase mxb(MXB_LOG_TARGET_STDOUT);

    return run();
}
