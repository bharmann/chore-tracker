// =============================================================================
// button_menu.h — Full-width option list usable WITHOUT the touch screen
// =============================================================================
// Recovery exists partly to fix a broken touch calibration, so every option
// must also work from the BOOT button:
//   - tap an option, or
//   - BOOT short press = move to the next option, BOOT hold (1 s) = choose it.
// Blocking modal; returns the chosen index.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "pins.h"
#include "audio_feedback.h"

extern TFT_eSPI      tft;
extern AudioFeedback audio;

struct MenuItem {
    const char* label;
    const char* sub;      // small second line, may be ""
    uint16_t    color;
};

class ButtonMenu {
public:
    static const uint32_t HOLD_MS = 1000;

    static uint8_t run(const char* title, const MenuItem* items, uint8_t n,
                       uint8_t selected, const char* foot1 = "", const char* foot2 = "") {
        if (selected >= n) selected = 0;
        _draw(title, items, n, selected, foot1, foot2);

        // The press that opened us (BOOT or a tap) must be released first
        bool bootArmed = digitalRead(PIN_BOOT_BUTTON) == HIGH;
        bool bootDown = false;
        uint32_t bootDownMs = 0;
        int8_t touchIdx = -1;
        bool touchArmed = false;   // set once the screen has been released
        uint8_t upPolls = 0;

        while (true) {
            // ── BOOT button ──
            bool down = digitalRead(PIN_BOOT_BUTTON) == LOW;
            if (!down) {
                if (bootDown && bootArmed && millis() - bootDownMs >= 30) {  // short press
                    selected = (selected + 1) % n;
                    audio.touchClick();
                    _drawItems(items, n, selected);
                }
                bootDown = false;
                bootArmed = true;
            } else if (bootArmed) {
                if (!bootDown) { bootDown = true; bootDownMs = millis(); }
                else if (millis() - bootDownMs >= HOLD_MS) {
                    audio.taskCompleteTone();
                    return selected;          // caller's next screen waits for release
                }
            }

            // ── Touch: choose on release, if still on the same option ──
            uint16_t x, y;
            if (tft.getTouch(&x, &y)) {
                if (upPolls >= 3) Serial.printf("[TOUCH] %u,%u\n", x, y);   // once per press
                upPolls = 0;
                if (touchArmed) touchIdx = _itemAt(n, y);
            } else if (upPolls < 3 && ++upPolls == 3) {      // just released
                if (!touchArmed) touchArmed = true;
                else if (touchIdx >= 0) {
                    audio.touchClick();
                    return (uint8_t)touchIdx;
                }
            }
            delay(15);
        }
    }

private:
    static const int16_t TOP = 62, BOTTOM = 438, GAP = 8;

    static int16_t _itemH(uint8_t n) {
        int16_t h = (BOTTOM - TOP) / n - GAP;
        return h > 80 ? 80 : h;
    }

    static int8_t _itemAt(uint8_t n, uint16_t y) {
        int16_t h = _itemH(n);
        for (uint8_t i = 0; i < n; i++) {
            int16_t iy = TOP + i * (h + GAP);
            if ((int16_t)y >= iy && (int16_t)y < iy + h) return i;
        }
        return -1;
    }

    static void _draw(const char* title, const MenuItem* items, uint8_t n, uint8_t sel,
                      const char* foot1, const char* foot2) {
        tft.fillScreen(0x1082);
        tft.setTextColor(0xFFFF); tft.setTextSize(2);
        tft.setCursor(15, 12); tft.print(title);
        tft.setTextColor(0x7BCF); tft.setTextSize(1);
        tft.setCursor(15, 38); tft.print("Tap an option - or BOOT button:");
        tft.setCursor(15, 50); tft.print("press = next, hold 1 second = select");
        _drawItems(items, n, sel);
        tft.setTextColor(0x7BCF); tft.setTextSize(1);
        tft.setCursor(15, 448); tft.print(foot1);
        tft.setCursor(15, 462); tft.print(foot2);
    }

    static void _drawItems(const MenuItem* items, uint8_t n, uint8_t sel) {
        int16_t h = _itemH(n);
        for (uint8_t i = 0; i < n; i++) {
            int16_t y = TOP + i * (h + GAP);
            tft.fillRoundRect(10, y - 3, 300, h + 6, 10, 0x1082);   // clear old outline
            tft.fillRoundRect(15, y, 290, h, 8, items[i].color);
            if (i == sel) {
                tft.drawRoundRect(12, y - 3, 296, h + 6, 10, 0xFFFF);
                tft.drawRoundRect(13, y - 2, 294, h + 4, 9, 0xFFFF);
            }
            bool hasSub = items[i].sub && items[i].sub[0];
            tft.setTextColor(0xFFFF); tft.setTextSize(2);
            tft.setCursor(30, y + (hasSub ? h / 2 - 14 : h / 2 - 8));
            tft.print(items[i].label);
            if (hasSub) {
                tft.setTextSize(1); tft.setTextColor(0xD69A);
                tft.setCursor(30, y + h / 2 + 6);
                tft.print(items[i].sub);
            }
        }
    }
};
