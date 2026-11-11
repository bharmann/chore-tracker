// Host test for allowance_logic.h (dates, weeks, money).
//   g++ -std=c++17 -Wall -Wextra -Werror -I ChoreStation test/allowance_logic_test.cpp -o alt && ./alt
#include "allowance_logic.h"
#include <cstdio>

static int pass = 0, fail = 0;
#define CHECK(cond) do { if (cond) pass++; else { fail++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
    // Epoch + known dates
    CHECK(alDaysFromCivil(1970, 1, 1) == 0);
    CHECK(alDow(0) == 4);                                  // Thursday
    int32_t d;
    CHECK(alParseDate("2026-11-15", d));
    CHECK(alDow(d) == 0);                                  // Sunday
    CHECK(alWeekStart(d) == d);
    int32_t wed;
    CHECK(alParseDate("2026-11-11", wed));
    CHECK(alDow(wed) == 3);
    CHECK(alWeekStart(wed) == d - 7);                      // Sun Nov 8
    CHECK(alFirstWeek(wed) == d);                          // tracking starts Sun Nov 15
    CHECK(alFirstWeek(d) == d);
    int32_t sat;
    CHECK(alParseDate("2026-11-21", sat));
    CHECK(alWeekStart(sat) == d);
    CHECK(alWeekStart(sat + 1) == d + 7);

    // Round trip across leap years and month/year ends
    for (int32_t x = alDaysFromCivil(2024, 1, 1); x < alDaysFromCivil(2031, 1, 1); x++) {
        int32_t y; uint32_t m, dd;
        alCivilFromDays(x, y, m, dd);
        if (alDaysFromCivil(y, m, dd) != x) { fail++; printf("FAIL round trip %d\n", (int)x); break; }
        char buf[12]; alFormatDate(x, buf, sizeof(buf));
        int32_t back;
        if (!alParseDate(buf, back) || back != x) { fail++; printf("FAIL parse %s\n", buf); break; }
    }
    pass++;
    CHECK(alParseDate("2028-02-29", d));
    char buf[24];
    alFormatDate(d + 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "2028-03-01") == 0);
    alFormatShort(alDaysFromCivil(2026, 12, 31), buf, sizeof(buf));
    CHECK(strcmp(buf, "Dec 31") == 0);

    // Bad dates
    CHECK(!alParseDate("0000-00-00", d));
    CHECK(!alParseDate("", d));
    CHECK(!alParseDate(nullptr, d));
    CHECK(!alParseDate("2026/11/15", d));
    CHECK(!alParseDate("2026-13-01", d));

    // Money
    alFormatCents(500, buf, sizeof(buf));    CHECK(strcmp(buf, "$5.00") == 0);
    alFormatCents(1250, buf, sizeof(buf));   CHECK(strcmp(buf, "$12.50") == 0);
    alFormatCents(-305, buf, sizeof(buf));   CHECK(strcmp(buf, "-$3.05") == 0);
    alFormatCents(0, buf, sizeof(buf));      CHECK(strcmp(buf, "$0.00") == 0);
    alFormatCents(7, buf, sizeof(buf));      CHECK(strcmp(buf, "$0.07") == 0);

    // Recurrence
    int32_t sun = alDaysFromCivil(2026, 11, 15);
    CHECK(alRunsOn(127, sun));
    CHECK(!alRunsOn(62, sun));                 // Mon-Fri
    CHECK(alRunsOn(62, sun + 1));
    CHECK(!alRunsOn(62, sun + 6));             // Saturday
    CHECK(alBitCount(127) == 7);
    CHECK(alBitCount(0) == 0);
    CHECK(alBitCount(0x55) == 4);

    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
