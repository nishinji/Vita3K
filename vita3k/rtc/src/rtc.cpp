// Vita3K emulator project
// Copyright (C) 2024 Vita3K team
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

#include <rtc/rtc.h>

#include <util/log.h>

static std::uint64_t rtc_ticks_since_epoch() {
    const auto now = std::chrono::high_resolution_clock::now();
    const auto now_timepoint = std::chrono::time_point_cast<VitaClocks>(now);
    return now_timepoint.time_since_epoch().count();
}

std::uint64_t rtc_base_ticks() {
    return RTC_OFFSET + std::time(nullptr) * VITA_CLOCKS_PER_SEC - rtc_ticks_since_epoch();
}

std::uint64_t rtc_get_ticks(uint64_t base_ticks) {
    return base_ticks + rtc_ticks_since_epoch();
}

// The following functions are from PPSSPP
// Copyright (c) 2012- PPSSPP Project.

void __RtcPspTimeToTm(tm *val, const SceDateTime *pt) {
    val->tm_year = pt->year - 1900;
    val->tm_mon = pt->month - 1;
    val->tm_mday = pt->day;
    val->tm_wday = -1;
    val->tm_yday = -1;
    val->tm_hour = pt->hour;
    val->tm_min = pt->minute;
    val->tm_sec = pt->second;
    val->tm_isdst = 0;
}

// Based on http://howardhinnant.github.io/date_algorithms.html
static std::int64_t days_from_civil(std::int64_t y, std::uint32_t m, std::uint32_t d) {
	y -= m <= 2;
	const std::int64_t era = (y >= 0 ? y : y-399) / 400;
	const std::uint32_t yoe = static_cast<std::uint32_t>(y - era * 400);      // [0, 399]
	const std::uint32_t doy = (153*(m > 2 ? m-3 : m+9) + 2)/5 + d-1;// [0, 365]
	const std::uint32_t doe = yoe * 365 + yoe/4 - yoe/100 + doy;    // [0, 146096]
	return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

static void civil_from_days(std::int64_t z, std::int64_t &out_y, std::uint32_t &out_m, std::uint32_t &out_d) {
	z += 719468;
	const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const std::uint32_t doe = static_cast<std::uint32_t>(z - era * 146097);              // [0, 146096]
	const std::uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365; // [0, 399]
	const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
	const std::uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);               // [0, 365]
	const std::uint32_t mp = (5*doy + 2)/153;                                  // [0, 11]
	out_d = doy - (153*mp+2)/5 + 1;                                  // [1, 31]
	out_m = mp < 10 ? mp+3 : mp-9;                                   // [1, 12]
	out_y = y + (out_m <= 2);
}

void __RtcTicksToPspTime(SceDateTime *t, std::uint64_t ticks) {
	std::uint64_t Day = 24ull * 60ull * 60ull * 1000000ull;
	t->microsecond = ticks % 1000000ull;
	t->second = ticks / 1000000ull % 60ull;
	t->minute = ticks / 1000000ull / 60ull % 60ull;
	t->hour   = ticks / 1000000ull / 60ull / 60ull % 24ull;
	std::int64_t z = std::int64_t(ticks / Day) - std::int64_t(RTC_OFFSET / Day);
        std::int64_t y;
	std::uint32_t m, d;
	civil_from_days(z, y, m, d);
	t->day   = d;
	t->month = m;
	t->year  = y;
}

std::uint64_t __RtcPspTimeToTicks(const SceDateTime *pt) {
	std::int64_t z = days_from_civil(std::int64_t(pt->year), pt->month, pt->day);
	return RTC_OFFSET +
		pt->microsecond + 
		1000000ull * (pt->second +
		60ull * (pt->minute + 
		60ull * (pt->hour + 
		24ull * std::uint64_t(z))));
}
