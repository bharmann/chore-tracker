// Host-side checks for ChoreStation/ota_logic.h (no device needed).
//   g++ -std=c++17 -Wall -Wextra -Werror -I ChoreStation test/ota_logic_test.cpp -o /tmp/olt && /tmp/olt
#include <cstdio>
#include "ota_logic.h"

static int pass = 0, fail = 0;
static void ok(const char* name, bool cond) {
    if (cond) pass++;
    else { fail++; std::printf("  FAIL %s\n", name); }
}

int main() {
    int v[3];
    ok("parse v1.4.0", parseVersion("v1.4.0", v) && v[0] == 1 && v[1] == 4 && v[2] == 0);
    ok("parse 2.10", parseVersion("2.10", v) && v[0] == 2 && v[1] == 10 && v[2] == 0);
    ok("parse V3", parseVersion("V3", v) && v[0] == 3 && v[1] == 0);
    ok("parse 1.4.0-beta", parseVersion("1.4.0-beta", v) && v[2] == 0);
    ok("reject dev", !parseVersion("dev", v));
    ok("reject empty", !parseVersion("", v));
    ok("reject null", !parseVersion(nullptr, v));
    ok("reject 1..2", !parseVersion("1..2", v));
    ok("reject 1.x", !parseVersion("1.x", v));

    ok("newer patch", compareVersions("v1.4.1", "v1.4.0") > 0);
    ok("older minor", compareVersions("v1.3.9", "v1.4.0") < 0);
    ok("numeric, not text: 1.10 > 1.9", compareVersions("1.10", "1.9") > 0);
    ok("same", compareVersions("v1.4", "1.4.0") == 0);
    ok("dev is older than any release", compareVersions("dev", "v0.0.1") < 0);
    ok("release is newer than dev", compareVersions("v0.0.1", "dev") > 0);
    ok("dev vs dev is same", compareVersions("dev", "dev") == 0);
    ok("garbage release isn't newer", compareVersions("v1.4.0", "oops") > 0);

    ok("sha ok", isHexSha256("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    ok("sha too short", !isHexSha256("0123"));
    ok("sha uppercase rejected", !isHexSha256("0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"));
    ok("sha null", !isHexSha256(nullptr));

    std::printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
