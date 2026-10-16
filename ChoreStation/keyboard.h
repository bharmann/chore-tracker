// =============================================================================
// keyboard.h — On-screen keyboard (blocking modal)
// =============================================================================
// Used for names, custom task names and WiFi passwords. Layout lives in
// keyboard_layout.h (unit-tested on a PC).
//
// Slide-to-select: touching shows the key under your finger, magnified above
// the keyboard; it types when you LIFT. On a 4" resistive screen with ~28 px
// keys this is far more accurate than type-on-press, and a wrong key can be
// corrected by sliding before letting go.
//
// The call blocks (RFID, web and background services pause) until Done,
// Cancel, or two minutes without a touch. That's fine for the occasional
// parent entry it's used for, and keeps every caller a one-liner.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "keyboard_layout.h"
#include "audio_feedback.h"

extern TFT_eSPI      tft;
extern AudioFeedback audio;

class OnScreenKeyboard {
public:
    // Edits buf in place (initial text = current contents). Returns true when
    // the user pressed Done; buf is untouched on cancel/timeout.
    // autoCap: start in upper case while the text is empty (names).
    static bool run(const char* title, char* buf, size_t bufSize,
                    uint8_t maxLen, bool autoCap) {
        OnScreenKeyboard k(title, buf, bufSize, maxLen, autoCap);
        return k._loop();
    }

private:
    static const uint16_t KB_BG = 0x1082, KB_CARD = 0x2945, KB_ACT = 0x4A69,
                          KB_PRI = 0x2C9F, KB_OK = 0x2E8B, KB_TXT = 0xFFFF,
                          KB_TXT3 = 0x7BCF, KB_ERR = 0xF800;
    static const uint32_t IDLE_TIMEOUT_MS = 120000;
    static const int16_t  BUBBLE_Y = 130, BUBBLE_H = 110;

    const char* _title;
    char*       _out;
    size_t      _outSize;
    uint8_t     _maxLen;
    bool        _autoCap;
    char        _text[65];
    kb::Layer   _layer;
    bool        _capsLock = false;
    uint32_t    _lastShiftMs = 0;
    bool        _hl = false;          // a key is highlighted (finger down on it)
    uint8_t     _hlRow = 0, _hlIdx = 0;

    OnScreenKeyboard(const char* title, char* buf, size_t bufSize, uint8_t maxLen, bool autoCap)
        : _title(title), _out(buf), _outSize(bufSize), _autoCap(autoCap) {
        if (maxLen > sizeof(_text) - 1) maxLen = sizeof(_text) - 1;
        if (bufSize && maxLen > bufSize - 1) maxLen = bufSize - 1;
        _maxLen = maxLen;
        strlcpy(_text, buf, sizeof(_text));
        if (strlen(_text) > _maxLen) _text[_maxLen] = '\0';
        _layer = (_autoCap && !_text[0]) ? kb::L_UPPER : kb::L_LOWER;
    }

    bool _loop() {
        _drawAll();
        _waitRelease();   // the tap that opened us must not type anything

        uint32_t lastTouchMs = millis();
        uint8_t  upPolls = 0;
        bool     cancelDown = false;
        while (true) {
            uint16_t x, y;
            if (tft.getTouch(&x, &y)) {
                lastTouchMs = millis();
                upPolls = 0;
                cancelDown = (y < 48 && x >= 220);
                uint8_t r, i;
                bool on = kb::keyAt(_layer, x, y, r, i);
                if (!on) { _unhighlight(); }
                else if (!_hl || r != _hlRow || i != _hlIdx) {
                    _unhighlight();
                    _hl = true; _hlRow = r; _hlIdx = i;
                    _drawKey(r, i, true);
                    _drawBubble(kb::keyAtIndex(_layer, r, i).code);
                }
            } else if (++upPolls == 3) {          // ~45 ms of no touch = lifted
                if (cancelDown) return false;
                if (_hl) {
                    uint8_t code = kb::keyAtIndex(_layer, _hlRow, _hlIdx).code;
                    _unhighlight();
                    if (_press(code)) return true;
                }
            }
            if (millis() - lastTouchMs > IDLE_TIMEOUT_MS) return false;
            delay(15);
        }
    }

    // Returns true when Done was pressed.
    bool _press(uint8_t code) {
        size_t len = strlen(_text);
        switch (code) {
            case kb::K_DONE:
                strlcpy(_out, _text, _outSize);
                audio.taskCompleteTone();
                return true;
            case kb::K_BKSP:
                if (len) _text[len - 1] = '\0';
                break;
            case kb::K_SHIFT: {
                uint32_t now = millis();
                if (_layer == kb::L_UPPER && !_capsLock && now - _lastShiftMs < 400) {
                    _capsLock = true;                       // double-tap = caps lock
                } else if (_layer == kb::L_UPPER) {
                    _layer = kb::L_LOWER; _capsLock = false;
                } else {
                    _layer = kb::L_UPPER;
                }
                _lastShiftMs = now;
                _drawKeys();
                break;
            }
            case kb::K_TOSYM: _layer = kb::L_SYM;   _capsLock = false; _drawKeys(); break;
            case kb::K_TOABC: _layer = kb::L_LOWER; _drawKeys(); break;
            default:
                if (code < 32 || code > 126) break;
                if (len >= _maxLen) { audio.errorTone(); return false; }
                _text[len] = (char)code; _text[len + 1] = '\0';
                if (_layer == kb::L_UPPER && !_capsLock) { _layer = kb::L_LOWER; _drawKeys(); }
                break;
        }
        audio.touchClick();
        _drawText();
        return false;
    }

    void _waitRelease() {
        uint16_t x, y;
        uint32_t start = millis();
        uint8_t up = 0;
        while (up < 3 && millis() - start < 3000) {
            up = tft.getTouch(&x, &y) ? 0 : up + 1;
            delay(15);
        }
    }

    // ─── Drawing ─────────────────────────────────────────────────────────

    void _drawAll() {
        tft.fillScreen(KB_BG);
        tft.fillRect(0, 0, 320, 48, KB_CARD);
        tft.setTextColor(KB_TXT); tft.setTextSize(2);
        tft.setCursor(10, 16); tft.print(_title);
        tft.fillRoundRect(228, 8, 84, 32, 6, KB_ACT);
        tft.setCursor(234, 16); tft.print("Cancel");
        _drawText();
        _drawBubble(0);
        _drawKeys();
    }

    void _drawText() {
        tft.fillRoundRect(8, 58, 304, 40, 6, 0x0841);
        tft.drawRoundRect(8, 58, 304, 40, 6, KB_PRI);
        tft.setTextSize(2); tft.setTextColor(KB_TXT);
        const uint8_t VISIBLE = 23;               // 12 px per char, room for cursor
        size_t len = strlen(_text);
        const char* shown = len > VISIBLE ? _text + (len - VISIBLE) : _text;
        tft.setCursor(16, 70);
        tft.print(shown);
        tft.print("_");
        tft.setTextSize(1); tft.setTextColor(len >= _maxLen ? KB_ERR : KB_TXT3);
        tft.fillRect(8, 102, 304, 12, KB_BG);
        tft.setCursor(12, 104);
        tft.printf("%u/%u   Slide to a key, lift to type", (unsigned)len, (unsigned)_maxLen);
    }

    // Big preview of the key under the finger (the finger hides the key itself)
    void _drawBubble(uint8_t code) {
        tft.fillRect(0, BUBBLE_Y, 320, BUBBLE_H, KB_BG);
        if (!code) return;
        tft.fillRoundRect(100, BUBBLE_Y + 5, 120, BUBBLE_H - 10, 10, KB_PRI);
        tft.setTextColor(KB_TXT);
        if (code > ' ' && code <= 126) {
            tft.setTextSize(8);                      // 48x64 px glyph
            tft.setCursor(160 - 24, BUBBLE_Y + 23);
            tft.print((char)code);
        } else {
            const char* lbl = kb::actionLabel(code, _layer);
            tft.setTextSize(2);
            tft.setCursor(160 - (int16_t)strlen(lbl) * 6, BUBBLE_Y + 47);
            tft.print(lbl);
        }
    }

    void _drawKeys() {
        tft.fillRect(0, kb::TOP, 320, 480 - kb::TOP, KB_BG);
        for (uint8_t r = 0; r < kb::ROWS; r++)
            for (uint8_t i = 0; i < kb::keysInRow(_layer, r); i++)
                _drawKey(r, i, false);
    }

    void _drawKey(uint8_t r, uint8_t i, bool pressed) {
        kb::Key k = kb::keyAtIndex(_layer, r, i);
        if (k.code == kb::K_NONE) return;
        kb::Rect rc = kb::keyRect(_layer, r, i);
        uint16_t bg = pressed ? KB_PRI
                    : k.code == kb::K_DONE ? KB_OK
                    : (k.code < 32 || k.code == ' ') ? KB_ACT : KB_CARD;
        if (k.code == kb::K_SHIFT && _layer == kb::L_UPPER && !pressed) bg = _capsLock ? KB_OK : KB_PRI;
        tft.fillRoundRect(rc.x, rc.y, rc.w, rc.h, 5, bg);
        tft.setTextColor(KB_TXT);
        if (k.code > ' ' && k.code <= 126) {
            tft.setTextSize(2);
            tft.setCursor(rc.x + rc.w / 2 - 6, rc.y + rc.h / 2 - 8);
            tft.print((char)k.code);
        } else {
            const char* lbl = kb::actionLabel(k.code, _layer);
            tft.setTextSize(1);
            tft.setCursor(rc.x + rc.w / 2 - (int16_t)strlen(lbl) * 3, rc.y + rc.h / 2 - 4);
            tft.print(lbl);
        }
    }

    void _unhighlight() {
        if (!_hl) return;
        _hl = false;
        _drawKey(_hlRow, _hlIdx, false);
        _drawBubble(0);
    }
};
