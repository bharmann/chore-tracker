// =============================================================================
// allowance_logic.h — Dates, weeks and money for the allowance (pure, host-tested)
// =============================================================================
// Days are counted as "day numbers" (days since 1970-01-01, local calendar
// date), so week math is plain integer math. A week runs Sunday..Saturday and
// is named by its Sunday's day number. Money is kept in cents.
// test/allowance_logic_test.cpp checks this with plain g++. Keep it
// Arduino-free.
// =============================================================================

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Howard Hinnant's days_from_civil: proleptic Gregorian date -> day number.
static inline int32_t alDaysFromCivil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static inline void alCivilFromDays(int32_t z, int32_t& y, uint32_t& m, uint32_t& d) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int32_t)yoe + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

// 0 = Sunday .. 6 = Saturday (1970-01-01 was a Thursday)
static inline uint8_t alDow(int32_t day) { return (uint8_t)(((day + 4) % 7 + 7) % 7); }

// The Sunday that starts the week containing `day`
static inline int32_t alWeekStart(int32_t day) { return day - alDow(day); }

// Tracking starts on a Sunday: today if it is one, otherwise the next one.
static inline int32_t alFirstWeek(int32_t today) {
    uint8_t w = alDow(today);
    return w == 0 ? today : today + (7 - w);
}

// "2026-11-15" -> day number. False for anything else (incl. "0000-00-00").
static inline bool alParseDate(const char* s, int32_t& out) {
    if (!s || strlen(s) < 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    int32_t y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    uint32_t m = (uint32_t)((s[5]-'0')*10 + (s[6]-'0'));
    uint32_t d = (uint32_t)((s[8]-'0')*10 + (s[9]-'0'));
    if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    out = alDaysFromCivil(y, m, d);
    return true;
}

// day number -> "2026-11-15"
static inline void alFormatDate(int32_t day, char* buf, size_t len) {
    int32_t y; uint32_t m, d;
    alCivilFromDays(day, y, m, d);
    snprintf(buf, len, "%04d-%02u-%02u", (int)y, (unsigned)m, (unsigned)d);
}

// day number -> "Nov 15" (short, for the station screen)
static inline void alFormatShort(int32_t day, char* buf, size_t len) {
    static const char* MON[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int32_t y; uint32_t m, d;
    alCivilFromDays(day, y, m, d);
    snprintf(buf, len, "%s %u", MON[(m - 1) % 12], (unsigned)d);
}

// 1250 -> "$12.50", -300 -> "-$3.00"
static inline void alFormatCents(int32_t cents, char* buf, size_t len) {
    bool neg = cents < 0;
    uint32_t a = (uint32_t)(neg ? -(int64_t)cents : cents);
    snprintf(buf, len, "%s$%lu.%02lu", neg ? "-" : "", (unsigned long)(a / 100), (unsigned long)(a % 100));
}

static inline uint8_t alBitCount(uint8_t mask) {
    uint8_t n = 0;
    while (mask) { n += mask & 1; mask >>= 1; }
    return n;
}

// Does a chore/routine that repeats on `recurDays` (bit0 = Sun .. bit6 = Sat)
// happen on `day`?
static inline bool alRunsOn(uint8_t recurDays, int32_t day) {
    return (recurDays >> alDow(day)) & 1;
}
