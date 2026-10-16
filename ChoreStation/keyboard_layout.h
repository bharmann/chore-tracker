// =============================================================================
// keyboard_layout.h — On-screen keyboard geometry and key content
// =============================================================================
// Pure data + math, no Arduino/TFT dependencies, so the layout can be unit
// tested on a PC (test/keyboard_layout_test.cpp): every printable ASCII
// character reachable, keys inside the screen, no overlaps, hit-testing exact.
//
// Portrait 320x480, five rows at the bottom of the screen. Widths are in
// tenths of a standard key; a row is 100 units wide (ten keys) and narrower
// rows are centred. Three layers: lower, upper (shift / caps lock), symbols.
// Letter layers carry a digit row so passwords rarely need a layer switch.
// =============================================================================

#pragma once
#include <stdint.h>
#include <string.h>

namespace kb {

// Key codes below 32 are actions; 32..126 type that character.
enum Code : uint8_t {
    K_NONE  = 0,   // spacer (no key)
    K_SHIFT = 1,
    K_BKSP  = 2,
    K_TOSYM = 3,   // letters → symbols
    K_TOABC = 4,   // symbols → letters
    K_DONE  = 5,
};

enum Layer : uint8_t { L_LOWER = 0, L_UPPER = 1, L_SYM = 2, L_COUNT = 3 };

static const int16_t SCREEN_W = 320;
static const int16_t MARGIN   = 2;     // left/right screen margin
static const int16_t GAP      = 3;     // space between keys
static const int16_t ROW_H    = 44;
static const uint8_t ROWS     = 5;
static const int16_t TOP      = 476 - ROWS * ROW_H;   // first row's y (256)

struct Key  { uint8_t code; uint8_t w; };
struct Rect { int16_t x, y, w, h; };

// A row is: optional left action, a run of character keys, optional right
// action. Space is just the ' ' character with a wide key.
struct RowDef {
    uint8_t lCode, lW;
    const char* chars;
    uint8_t cW;
    uint8_t rCode, rW;
};

static const RowDef ROW_DEFS[L_COUNT][ROWS] = {
    {   // lower
        { K_NONE, 0,   "1234567890", 10, K_NONE, 0 },
        { K_NONE, 0,   "qwertyuiop", 10, K_NONE, 0 },
        { K_NONE, 0,   "asdfghjkl",  10, K_NONE, 0 },
        { K_SHIFT, 15, "zxcvbnm",    10, K_BKSP, 15 },
        { K_TOSYM, 25, " ",          50, K_DONE, 25 },
    },
    {   // upper
        { K_NONE, 0,   "1234567890", 10, K_NONE, 0 },
        { K_NONE, 0,   "QWERTYUIOP", 10, K_NONE, 0 },
        { K_NONE, 0,   "ASDFGHJKL",  10, K_NONE, 0 },
        { K_SHIFT, 15, "ZXCVBNM",    10, K_BKSP, 15 },
        { K_TOSYM, 25, " ",          50, K_DONE, 25 },
    },
    {   // symbols — all 32 ASCII punctuation characters, each exactly once
        { K_NONE, 0,   "[]{}<>^`~|",   10, K_NONE, 0 },
        { K_NONE, 0,   "-/:;()$&@\"",  10, K_NONE, 0 },
        { K_NONE, 0,   ".,?!'#%*+",    10, K_NONE, 0 },
        { K_NONE, 15,  "=_\\",         23, K_BKSP, 15 },
        { K_TOABC, 25, " ",            50, K_DONE, 25 },
    },
};

inline uint8_t keysInRow(Layer l, uint8_t row) {
    const RowDef& d = ROW_DEFS[l][row];
    return (d.lW ? 1 : 0) + (uint8_t)strlen(d.chars) + (d.rW ? 1 : 0);
}

inline Key keyAtIndex(Layer l, uint8_t row, uint8_t i) {
    const RowDef& d = ROW_DEFS[l][row];
    uint8_t n = (uint8_t)strlen(d.chars);
    if (d.lW) {
        if (i == 0) return { d.lCode, d.lW };
        i--;
    }
    if (i < n) return { (uint8_t)d.chars[i], d.cW };
    return { d.rCode, d.rW };
}

inline uint8_t rowUnits(Layer l, uint8_t row) {
    const RowDef& d = ROW_DEFS[l][row];
    return d.lW + (uint8_t)strlen(d.chars) * d.cW + d.rW;
}

// Screen x of a position measured in key units (row is 100 units wide).
inline int16_t unitX(int16_t units) {
    return MARGIN + (int16_t)((int32_t)units * (SCREEN_W - 2 * MARGIN) / 100);
}

inline Rect keyRect(Layer l, uint8_t row, uint8_t i) {
    int16_t u = (100 - rowUnits(l, row)) / 2;          // centre short rows
    for (uint8_t k = 0; k < i; k++) u += keyAtIndex(l, row, k).w;
    int16_t x0 = unitX(u), x1 = unitX(u + keyAtIndex(l, row, i).w);
    int16_t y0 = TOP + row * ROW_H;
    return { (int16_t)(x0 + GAP / 2), (int16_t)(y0 + GAP / 2),
             (int16_t)(x1 - x0 - GAP), (int16_t)(ROW_H - GAP) };
}

// Which key is under (x, y)? A touch in the gap between keys counts for the
// nearer key's slot (each key owns its full unit span), so small misses still
// land. Spacers and the area outside the rows return false.
inline bool keyAt(Layer l, int16_t x, int16_t y, uint8_t& row, uint8_t& idx) {
    if (y < TOP || y >= TOP + ROWS * ROW_H) return false;
    row = (uint8_t)((y - TOP) / ROW_H);
    int16_t u = (100 - rowUnits(l, row)) / 2;
    uint8_t n = keysInRow(l, row);
    for (uint8_t i = 0; i < n; i++) {
        Key k = keyAtIndex(l, row, i);
        if (x >= unitX(u) && x < unitX(u + k.w)) {
            if (k.code == K_NONE) return false;
            idx = i;
            return true;
        }
        u += k.w;
    }
    return false;
}

// Short label for action keys (character keys draw their own character).
inline const char* actionLabel(uint8_t code, Layer l) {
    switch (code) {
        case K_SHIFT: return l == L_UPPER ? "SHIFT" : "shift";
        case K_BKSP:  return "<del";
        case K_TOSYM: return "#+=";
        case K_TOABC: return "abc";
        case K_DONE:  return "Done";
        case ' ':     return "space";
        default:      return "";
    }
}

} // namespace kb
