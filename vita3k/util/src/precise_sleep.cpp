// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <util/precise_sleep.h>

#include <algorithm>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace util {

#ifdef _WIN32
namespace {
struct WaitableTimer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    WaitableTimer() {
        // high-resolution timers need Windows 10 1803
        if (!handle)
            handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~WaitableTimer() {
        if (handle)
            CloseHandle(handle);
    }

    void wait(std::chrono::nanoseconds duration) const {
        LARGE_INTEGER due;
        due.QuadPart = -duration.count() / 100;
        if (handle && SetWaitableTimerEx(handle, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(handle, INFINITE);
    }
};
} // namespace
#endif

void sleep_until_precise(std::chrono::steady_clock::time_point deadline, bool spin_end) {
    using namespace std::chrono;
#ifdef _WIN32
    thread_local WaitableTimer timer;
    // how late the timer wakes up varies between machines, so learn it and only spin through that part
    thread_local nanoseconds timer_lateness = microseconds(300);
    constexpr auto spin_margin = microseconds(50);
    const auto now = steady_clock::now();
    const auto remaining = deadline - now;
    if (!spin_end) {
        if (remaining > nanoseconds::zero())
            timer.wait(remaining);
        return;
    }
    const auto sleep_time = remaining - timer_lateness - spin_margin;
    if (sleep_time > nanoseconds::zero()) {
        timer.wait(sleep_time);
        // short waits are dominated by the timer granularity and would inflate the estimate
        if (sleep_time >= microseconds(500)) {
            const auto lateness = steady_clock::now() - (now + sleep_time);
            timer_lateness = std::clamp<nanoseconds>((timer_lateness * 7 + lateness) / 8, microseconds(20), milliseconds(1));
        }
    }
    while (steady_clock::now() < deadline)
        std::this_thread::yield();
#else
    (void)spin_end;
    std::this_thread::sleep_until(deadline);
#endif
}

} // namespace util
