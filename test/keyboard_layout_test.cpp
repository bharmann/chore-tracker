// Host-side checks for ChoreStation/keyboard_layout.h (no device needed).
//   g++ -std=c++17 -Wall -Wextra -Werror -I ChoreStation test/keyboard_layout_test.cpp -o /tmp/kbt && /tmp/kbt
#include <cstdio>
#include <set>
#include "keyboard_layout.h"

using namespace kb;

static int pass = 0, fail = 0;
static void ok(const char* name, bool cond) {
    if (cond) pass++;
    else { fail++; std::printf("  FAIL %s\n", name); }
}

int main() {
    std::set<char> reachable;

    for (uint8_t l = 0; l < L_COUNT; l++) {
        Layer L = (Layer)l;
        bool hasDone = false, hasBksp = false, hasSwitch = false;
        for (uint8_t r = 0; r < ROWS; r++) {
            char nm[96];
            std::snprintf(nm, sizeof nm, "layer %d row %d fits in 100 units", l, r);
            ok(nm, rowUnits(L, r) <= 100);

            int16_t prevRight = -1;
            for (uint8_t i = 0; i < keysInRow(L, r); i++) {
                Key k = keyAtIndex(L, r, i);
                Rect rc = keyRect(L, r, i);
                if (k.code == K_NONE) continue;
                if (k.code >= 32) reachable.insert((char)k.code);
                hasDone   |= k.code == K_DONE;
                hasBksp   |= k.code == K_BKSP;
                hasSwitch |= k.code == K_TOSYM || k.code == K_TOABC;

                std::snprintf(nm, sizeof nm, "L%d r%d k%d inside the screen", l, r, i);
                ok(nm, rc.x >= 0 && rc.x + rc.w <= SCREEN_W && rc.y >= TOP && rc.y + rc.h <= 480);
                std::snprintf(nm, sizeof nm, "L%d r%d k%d does not overlap its neighbour", l, r, i);
                ok(nm, rc.x > prevRight);
                prevRight = rc.x + rc.w - 1;
                // Resistive touch + a child's finger: keep targets >= 28 px wide, 40 px tall
                std::snprintf(nm, sizeof nm, "L%d r%d k%d is at least 28x40 px", l, r, i);
                ok(nm, rc.w >= 28 && rc.h >= 40);

                // Centre and all four inner corners hit this exact key
                int16_t pts[5][2] = {
                    { (int16_t)(rc.x + rc.w / 2), (int16_t)(rc.y + rc.h / 2) },
                    { rc.x, rc.y }, { (int16_t)(rc.x + rc.w - 1), rc.y },
                    { rc.x, (int16_t)(rc.y + rc.h - 1) },
                    { (int16_t)(rc.x + rc.w - 1), (int16_t)(rc.y + rc.h - 1) },
                };
                for (auto& p : pts) {
                    uint8_t hr = 255, hi = 255;
                    bool hit = keyAt(L, p[0], p[1], hr, hi);
                    std::snprintf(nm, sizeof nm, "L%d r%d k%d hit-test at (%d,%d)", l, r, i, p[0], p[1]);
                    ok(nm, hit && hr == r && hi == i);
                }
            }
        }
        char nm[64];
        std::snprintf(nm, sizeof nm, "layer %d has Done, Delete and a layer switch", l);
        ok(nm, hasDone && hasBksp && hasSwitch);
    }

    for (int c = 32; c <= 126; c++) {
        char nm[48];
        std::snprintf(nm, sizeof nm, "character '%c' (%d) can be typed", (char)c, c);
        ok(nm, reachable.count((char)c) == 1);
    }

    // Outside the keyboard area, and the symbol layer's spacer, are not keys
    uint8_t r, i;
    ok("touch above the keyboard is not a key", !keyAt(L_LOWER, 160, TOP - 1, r, i));
    ok("touch below the last row is not a key", !keyAt(L_LOWER, 160, TOP + ROWS * ROW_H, r, i));
    ok("symbol-layer spacer is not a key", !keyAt(L_SYM, unitX(5), TOP + 3 * ROW_H + ROW_H / 2, r, i));
    // A touch in the gap between two keys still lands on one of them
    Rect a = keyRect(L_LOWER, 1, 0);
    ok("gap between q and w lands on a key", keyAt(L_LOWER, a.x + a.w + 1, a.y + 10, r, i) && r == 1 && i <= 1);

    std::printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
