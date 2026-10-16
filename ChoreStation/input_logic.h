// =============================================================================
// input_logic.h — Touch press detection + tone sequencing (pure, host-tested)
// =============================================================================
// No Arduino or TFT dependencies, so test/input_logic_test.cpp checks it with
// plain g++ (like keyboard_layout.h). The firmware feeds it touch polls and
// timer ticks; everything that decides *when* something happens lives here.
// =============================================================================

#pragma once
#include <stdint.h>

// ─── TouchGate: one action per finger press ───────────────────────────────────
// Feed every touch poll. poll() returns true exactly once per press, when the
// press has lasted DOWN_MS (and DOWN_POLLS polls) — a held finger never
// repeats. Resistive panels drop out for a poll or two while pressed, so the
// finger only counts as lifted after UP_MS (and UP_POLLS polls) of no touch.
// Times, not poll counts, set the pace: loop time varies (SD writes, web).
class TouchGate {
public:
    static const uint32_t DOWN_MS = 25, UP_MS = 60;
    static const uint8_t  DOWN_POLLS = 2, UP_POLLS = 3;

    bool poll(bool touched, uint16_t px, uint16_t py, uint32_t nowMs) {
        switch (_s) {
            case UP:
                if (touched) { _s = MAYBE_DOWN; _since = nowMs; _polls = 1; }
                return false;
            case MAYBE_DOWN:
                if (!touched) { _s = UP; return false; }            // a blip
                _polls++;
                if (nowMs - _since >= DOWN_MS && _polls >= DOWN_POLLS) {
                    _s = DOWN; x = px; y = py;                       // confirming poll's point
                    return true;
                }
                return false;
            case DOWN:
                if (!touched) { _s = MAYBE_UP; _since = nowMs; _polls = 1; }
                return false;
            case MAYBE_UP:
                if (touched) { _s = DOWN; return false; }           // dropout, still pressed
                _polls++;
                if (nowMs - _since >= UP_MS && _polls >= UP_POLLS) _s = UP;
                return false;
        }
        return false;
    }

    // After a modal screen (keyboard, menus) consumed the touch, the finger may
    // still be down: don't let its release or hold press whatever is underneath.
    void requireRelease() { _s = DOWN; }

    bool pressed() const { return _s == DOWN || _s == MAYBE_UP; }

    uint16_t x = 0, y = 0;   // where the last press was confirmed

private:
    enum State : uint8_t { UP, MAYBE_DOWN, DOWN, MAYBE_UP };
    State    _s = UP;
    uint32_t _since = 0;
    uint8_t  _polls = 0;
};

// ─── Tones ────────────────────────────────────────────────────────────────────
// A cue is a short list of notes. hz == 0 is a rest. vol: 0 = loudest .. 3 =
// quietest (maps to the DAC cosine generator's 0/6/12/18 dB attenuation).

struct ToneNote {
    uint16_t hz;
    uint16_t ms;
    uint8_t  vol;
};

// The ESP32 DAC cosine generator steps in ~130 Hz increments (8.5 MHz RTC
// clock / 65536), so cue notes are picked on that grid: 520/650/780/1040 Hz
// is a clean C-E-G-C major triad, 390 Hz a low tone.
static inline uint16_t snapCosineHz(uint16_t hz) {
    if (hz == 0) return 0;
    uint16_t steps = (uint16_t)((hz + 65) / 130);
    if (steps == 0) steps = 1;
    return (uint16_t)(steps * 130);
}

// Plays one cue at a time. start() replaces whatever was playing (cues never
// queue up behind each other), current() is the note to sound now, advance()
// moves on when its time is up and returns false at the end.
class ToneSeq {
public:
    void start(const ToneNote* notes, uint8_t count) {
        _notes = notes; _count = count; _idx = 0;
        _on = (notes != nullptr && count > 0);
    }
    bool playing() const { return _on; }
    bool current(ToneNote& out) const {
        if (!_on) return false;
        out = _notes[_idx];
        return true;
    }
    bool advance() {
        if (!_on) return false;
        if (++_idx >= _count) { _on = false; return false; }
        return true;
    }
    void stop() { _on = false; }

private:
    const ToneNote* _notes = nullptr;
    uint8_t _count = 0, _idx = 0;
    bool    _on = false;
};

// Named cues (frequencies already on the cosine grid)
namespace cues {
    static const ToneNote CLICK[]  = { {1300, 20, 2} };   // long enough for the amp to wake
    static const ToneNote SCAN[]   = { {1040, 70, 1} };
    static const ToneNote START[]  = { {520, 90, 1}, {0, 25, 1}, {780, 110, 1} };
    static const ToneNote POINTS[] = { {520, 70, 1}, {650, 70, 1}, {780, 70, 1}, {1040, 170, 1} };
    static const ToneNote SENT[]   = { {650, 90, 2}, {0, 50, 2}, {650, 90, 2} };
    static const ToneNote BADGE[]  = { {520, 90, 1}, {650, 90, 1}, {780, 90, 1}, {1040, 130, 1},
                                       {0, 70, 1}, {780, 80, 1}, {1040, 280, 1} };
    static const ToneNote REDO[]   = { {520, 120, 1}, {0, 30, 1}, {390, 220, 1} };
    static const ToneNote ERROR[]  = { {520, 110, 1}, {0, 30, 1}, {390, 190, 1} };
}
#define CUE_LEN(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))
