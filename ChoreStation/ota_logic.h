// =============================================================================
// ota_logic.h — Firmware version compare + checks (pure, host-tested)
// =============================================================================
// Release tags look like "v1.4.0" (also "1.4" or "v2"). A build made in the
// Arduino IDE is "dev" and counts as older than any release, so a station
// flashed by USB can always take the latest release.
// test/ota_logic_test.cpp checks this with plain g++.
// =============================================================================

#pragma once
#include <stdint.h>
#include <string.h>

// "v1.4.0" -> {1,4,0}. Returns false for anything that isn't a version.
static inline bool parseVersion(const char* s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    if (*s < '0' || *s > '9') return false;
    for (int part = 0; part < 3; part++) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (v > 99999) return false;
            s++; digits++;
        }
        if (!digits) return false;
        out[part] = v;
        if (*s == '.') { s++; continue; }
        break;
    }
    return *s == '\0' || *s == '-' || *s == '+';   // "1.4.0-beta" compares as 1.4.0
}

// <0: a is older than b, 0: same, >0: a is newer. Unparseable ("dev") is oldest.
static inline int compareVersions(const char* a, const char* b) {
    int va[3], vb[3];
    bool okA = parseVersion(a, va), okB = parseVersion(b, vb);
    if (!okA || !okB) return (okA ? 1 : 0) - (okB ? 1 : 0);
    for (int i = 0; i < 3; i++)
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return 0;
}

static inline bool isHexSha256(const char* s) {
    if (!s || strlen(s) != 64) return false;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}
