// Vita3K emulator project
// Copyright (C) 2025 Vita3K team
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

#pragma once

#include <util/types.h>

#include <chrono>
#include <cstdint>
#include <string>

// This is the # of microseconds between January 1, 0001 and January 1, 1970.
// Grabbed from JPSCP
static constexpr auto RTC_OFFSET = 62135596800000000ULL;

// 400 years is a convenient number, since leap days and everything cycle every 400 years.
// 400 years is in other words 20871 full weeks.
constexpr std::uint64_t RTC_400_YEAR_TICKS = 20871ULL * 7 * 24 * 3600 * 1000000;

constexpr auto VITA_CLOCKS_PER_SEC = 1'000'000;

using VitaClocks = std::chrono::duration<std::uint64_t, std::ratio<1, VITA_CLOCKS_PER_SEC>>;

enum SceKernelPowerTickType {
    /** Cancel all timers */
    SCE_KERNEL_POWER_TICK_DEFAULT = 0,
    /** Cancel automatic suspension timer */
    SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND = 1,
    /** Cancel OLED-off timer */
    SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF = 4,
    /** Cancel OLED dimming timer */
    SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING = 6
};

enum SceRtcErrorCode {
    SCE_RTC_ERROR_INVALID_VALUE = 0x80251000,
    SCE_RTC_ERROR_INVALID_POINTER = 0x80251001,
    SCE_RTC_ERROR_NOT_INITIALIZED = 0x80251002,
    SCE_RTC_ERROR_ALREADY_REGISTERED = 0x80251003,
    SCE_RTC_ERROR_NOT_FOUND = 0x80251004,
    SCE_RTC_ERROR_BAD_PARSE = 0x80251080,
    SCE_RTC_ERROR_INVALID_YEAR = 0x80251081,
    SCE_RTC_ERROR_INVALID_MONTH = 0x80251082,
    SCE_RTC_ERROR_INVALID_DAY = 0x80251083,
    SCE_RTC_ERROR_INVALID_HOUR = 0x80251084,
    SCE_RTC_ERROR_INVALID_MINUTE = 0x80251085,
    SCE_RTC_ERROR_INVALID_SECOND = 0x80251086,
    SCE_RTC_ERROR_INVALID_MICROSECOND = 0x80251087
};

struct SceRtcTick {
    SceUInt64 tick;
};

#if defined(_WIN32)
inline time_t rtc_timegm(struct tm *tm) { return _mkgmtime(tm); }
#elif (defined(__GLIBC__) && !defined(__ANDROID__))
#define rtc_timegm timegm
#else
inline time_t rtc_timegm(struct tm *tm) {
    time_t ret;
    char *tz;
    std::string tzcopy;

    tz = getenv("TZ");
    if (tz)
        tzcopy = tz;

    setenv("TZ", "", 1);
    tzset();
    ret = mktime(tm);
    if (tz)
        setenv("TZ", tzcopy.c_str(), 1);
    else
        unsetenv("TZ");
    tzset();
    return ret;
}
#endif

std::uint64_t rtc_base_ticks();
std::uint64_t rtc_get_ticks(uint64_t base_tick);
void __RtcPspTimeToTm(tm *val, const SceDateTime *pt);
void __RtcTicksToPspTime(SceDateTime *t, std::uint64_t ticks);
std::uint64_t __RtcPspTimeToTicks(const SceDateTime *pt);
