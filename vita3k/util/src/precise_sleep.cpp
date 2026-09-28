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
    // a high-resolution timer usually wakes 0.25 ms late
    constexpr auto spin_time = microseconds(400);
    const auto remaining = deadline - steady_clock::now();
    if (!spin_end) {
        if (remaining > nanoseconds::zero())
            timer.wait(remaining);
        return;
    }
    if (remaining > spin_time)
        timer.wait(remaining - spin_time);
    while (steady_clock::now() < deadline)
        std::this_thread::yield();
#else
    (void)spin_end;
    std::this_thread::sleep_until(deadline);
#endif
}

} // namespace util
