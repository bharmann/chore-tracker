// Host-side checks for ChoreStation/input_logic.h (no device needed).
//   g++ -std=c++17 -Wall -Wextra -Werror -I ChoreStation test/input_logic_test.cpp -o /tmp/ilt && /tmp/ilt
#include <cstdio>
#include "input_logic.h"

static int pass = 0, fail = 0;
static void ok(const char* name, bool cond) {
    if (cond) pass++;
    else { fail++; std::printf("  FAIL %s\n", name); }
}

// Feed a touch pattern: one character per poll, 'X' = touched, '.' = not,
// polls every stepMs. Returns how many presses fired.
static int presses(TouchGate& g, const char* pattern, uint32_t stepMs, uint32_t& t) {
    int n = 0;
    for (const char* p = pattern; *p; p++) {
        if (g.poll(*p == 'X', 100, 200, t)) n++;
        t += stepMs;
    }
    return n;
}

int main() {
    // ── TouchGate ──
    {
        TouchGate g; uint32_t t = 0;
        ok("held finger fires once", presses(g, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", 15, t) == 1);
        ok("still pressed while held", g.pressed());
    }
    {
        TouchGate g; uint32_t t = 0;
        // press, short dropouts (2 polls at 15 ms = 30 ms < UP_MS) while held
        ok("dropouts while held don't refire", presses(g, "XXXX..XXXX.XXXX..XXXXX", 15, t) == 1);
    }
    {
        TouchGate g; uint32_t t = 0;
        ok("press, lift, press = two", presses(g, "XXXX......XXXX......", 15, t) == 2);
    }
    {
        TouchGate g; uint32_t t = 0;
        ok("single-poll blip doesn't fire", presses(g, ".X......", 15, t) == 0);
    }
    {
        TouchGate g; uint32_t t = 0;
        // DOWN_MS 25 ms: with a fast 5 ms loop it takes 6 polls, not 2
        ok("fast loop: needs DOWN_MS, not just 2 polls", presses(g, "XXX", 5, t) == 0);
        ok("fast loop: fires once DOWN_MS reached", presses(g, "XXXX", 5, t) == 1);
    }
    {
        TouchGate g; uint32_t t = 0;
        // slow loop (SD write): 2 polls 100 ms apart is a press
        ok("slow loop: 2 polls is enough", presses(g, "XX", 100, t) == 1);
        // a single 100 ms gap with UP_POLLS 3 isn't a release
        ok("slow loop: one long gap isn't a release", presses(g, ".X", 100, t) == 0);
    }
    {
        TouchGate g; uint32_t t = 0;
        g.requireRelease();
        ok("after a modal: finger still down doesn't fire", presses(g, "XXXXXXXX", 15, t) == 0);
        ok("after a modal: lift then tap fires", presses(g, "......XXXX", 15, t) == 1);
    }
    {
        TouchGate g; uint32_t t = 0xFFFFFF00u;   // millis() wrap
        ok("works across millis() wrap", presses(g, "XXXXXX......XXXXXX", 15, t) == 2);
    }
    {
        TouchGate g; uint32_t t = 0;
        g.poll(true, 10, 10, t); t += 30;
        bool fired = g.poll(true, 123, 321, t);
        ok("press point = confirming poll", fired && g.x == 123 && g.y == 321);
    }

    // ── Tones ──
    ok("snap 523 -> 520", snapCosineHz(523) == 520);
    ok("snap 659 -> 650", snapCosineHz(659) == 650);
    ok("snap 784 -> 780", snapCosineHz(784) == 780);
    ok("snap 0 stays a rest", snapCosineHz(0) == 0);
    ok("snap tiny -> 130 minimum", snapCosineHz(20) == 130);

    const ToneNote* all[] = { cues::CLICK, cues::SCAN, cues::START, cues::POINTS, cues::SENT,
                              cues::BADGE, cues::REDO, cues::ERROR };
    const uint8_t lens[] = { CUE_LEN(cues::CLICK), CUE_LEN(cues::SCAN), CUE_LEN(cues::START),
                             CUE_LEN(cues::POINTS), CUE_LEN(cues::SENT), CUE_LEN(cues::BADGE),
                             CUE_LEN(cues::REDO), CUE_LEN(cues::ERROR) };
    bool onGrid = true, sane = true;
    for (unsigned c = 0; c < sizeof(all) / sizeof(all[0]); c++) {
        uint32_t total = 0;
        for (uint8_t i = 0; i < lens[c]; i++) {
            const ToneNote& n = all[c][i];
            if (n.hz != snapCosineHz(n.hz)) onGrid = false;
            if (n.ms == 0 || n.vol > 3) sane = false;
            total += n.ms;
        }
        if (total > 1200) sane = false;          // no cue longer than 1.2 s
        if (all[c][0].hz == 0) sane = false;      // cues start with a sound
    }
    ok("every cue note is on the cosine grid", onGrid);
    ok("every cue is short and well-formed", sane);

    {
        ToneSeq s; ToneNote n{};
        s.start(cues::START, CUE_LEN(cues::START));
        ok("seq: first note", s.current(n) && n.hz == 520);
        ok("seq: rest", s.advance() && s.current(n) && n.hz == 0);
        ok("seq: last note", s.advance() && s.current(n) && n.hz == 780);
        ok("seq: ends", !s.advance() && !s.playing() && !s.current(n));
        s.start(cues::POINTS, CUE_LEN(cues::POINTS));
        s.advance();
        s.start(cues::CLICK, CUE_LEN(cues::CLICK));   // a new cue replaces the old one
        ok("seq: new cue replaces old", s.current(n) && n.hz == 1300 && !s.advance());
        s.start(nullptr, 0);
        ok("seq: empty cue is silent", !s.playing());
    }

    std::printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
