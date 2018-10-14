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

#include <chrono>
#include <iosfwd>
#include <string>

// Same StopWatch as in maxscale, but dropping support for gcc 4.4
namespace base
{
using Clock = std::chrono::steady_clock;

struct Duration : public Clock::duration    // for ADL
{
    using Clock::duration::duration;
    Duration() = default;
    Duration(Clock::duration d) : Clock::duration(d)
    {
    }
};

using TimePoint = std::chrono::time_point<Clock, Duration>;

class StopWatch
{
public:
    // Starts the stopwatch, which is always running.
    StopWatch();
    // Get elapsed time.
    Duration lap() const;
    // Get elapsed time, restart StopWatch.
    Duration restart();
private:
    TimePoint m_start;
};

// Returns the value as a double and string adjusted to a suffix like ms for milliseconds.
std::pair<double, std::string> dur_to_human_readable(Duration dur);

// Human readable output. No standard library for it yet.
std::ostream& operator<<(std::ostream&, Duration dur);

// TimePoint
std::string   time_point_to_string(TimePoint tp, const std::string& fmt = "%F %T");
std::ostream& operator<<(std::ostream&, TimePoint tp);
}   // base
