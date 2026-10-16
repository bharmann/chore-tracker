// =============================================================================
// display_power.h — Screen Longevity & Power Management
// =============================================================================
// TN TFT lifespan concerns and mitigations:
//
// 1. BACKLIGHT WEAR — The 8 white LEDs are the primary wear component.
//    Running 24/7 at full brightness = ~20,000-50,000 hr rated life.
//    At 12hr/day that's 4.5-11 years, but dimming extends it significantly.
//    Strategy: PWM dim after inactivity, full off after extended idle.
//
// 2. IMAGE PERSISTENCE — TN panels can develop ghosting from static images
//    displayed for weeks/months. Not permanent burn-in like OLED, but
//    annoying. Strategy: pixel-shift the home screen, vary elements,
//    blank screen when sleeping.
//
// 3. POWER DRAW — The display draws ~230mA when active. Sleep mode
//    reduces to ~40mA (ESP32 still running, RFID still polling).
//    Relevant for battery operation.
//
// Backlight control: GPIO27 supports PWM via LEDC peripheral.
// We use 8-bit PWM (0-255) for smooth dimming.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "pins.h"

class DisplayPower {
public:
    // Brightness levels (0-255 PWM duty)
    static const uint8_t BRIGHT_FULL    = 255;  // Active use
    static const uint8_t BRIGHT_DIM     = 80;   // Idle dim (readable but low power)
    static const uint8_t BRIGHT_NIGHT   = 30;   // Night mode, idle (barely visible)
    static const uint8_t BRIGHT_NIGHT_ACTIVE = 160;  // Night mode while in use (bedtime routines)
    static const uint8_t BRIGHT_OFF     = 0;    // Sleep (screen blank)

    // Timing thresholds (milliseconds)
    uint32_t dimAfterMs     = 15000;    // Dim after 15s idle
    uint32_t sleepAfterMs   = 120000;   // Sleep after 2min idle
    uint32_t shiftIntervalMs = 300000;  // Pixel-shift every 5min on home screen

    // Night mode hours (24h format, configurable by parent)
    uint8_t nightStartHour = 21;  // 9 PM
    uint8_t nightEndHour   = 7;   // 7 AM

    enum class State : uint8_t {
        ACTIVE,   // Full brightness, screen content visible
        DIM,      // Reduced brightness, screen content visible
        SLEEPING, // Backlight off, screen blank — RFID still polling
        WAKING    // Transitioning from sleep to active (fade in)
    };

    void begin() {
        // Configure LEDC PWM for backlight — 5000 Hz, 8-bit resolution.
        // esp32 core 3.x replaced ledcSetup+ledcAttachPin with a single
        // ledcAttach(pin, freq, res) and made ledcWrite address the PIN, not
        // the channel. Guard so the sketch builds on both 2.x and 3.x cores.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcAttach(PIN_LCD_BL, 5000, 8);
#else
        ledcSetup(_pwmChannel, 5000, 8);
        ledcAttachPin(PIN_LCD_BL, _pwmChannel);
#endif

        setBrightness(BRIGHT_FULL);
        _state = State::ACTIVE;
        _lastActivityMs = millis();
        _lastShiftMs = millis();

        Serial.println("[DISPLAY] Power manager initialized.");
    }

    // Call from main loop with the timestamp of last user activity
    void update(uint32_t lastActivityMs, uint8_t currentHour = 12) {
        _lastActivityMs = lastActivityMs;
        uint32_t idle = millis() - _lastActivityMs;

        // Determine target brightness
        // Night mode only dims an idle screen: someone using it at bedtime
        // or before school needs to read it.
        _night = _isNightHour(currentHour);
        uint8_t fullBright = _night ? BRIGHT_NIGHT_ACTIVE : BRIGHT_FULL;
        uint8_t dimBright  = _night ? BRIGHT_NIGHT : BRIGHT_DIM;

        switch (_state) {
            case State::ACTIVE:
                if (idle >= sleepAfterMs) {
                    _enterSleep();
                } else if (idle >= dimAfterMs) {
                    _enterDim(dimBright);
                } else if (_brightness != fullBright) {
                    setBrightness(fullBright);       // night mode started/ended
                }
                break;

            case State::DIM:
                if (idle >= sleepAfterMs) {
                    _enterSleep();
                } else if (idle < dimAfterMs) {
                    // User became active again
                    _enterActive(fullBright);
                } else if (_brightness != dimBright) {
                    setBrightness(dimBright);
                }
                break;

            case State::SLEEPING:
                // Stay asleep — wake is triggered by wake() call
                break;

            case State::WAKING:
                // Fade in animation
                if (_fadeBrightness < fullBright) {
                    _fadeBrightness += 5;
                    if (_fadeBrightness > fullBright) _fadeBrightness = fullBright;
                    setBrightness(_fadeBrightness);
                } else {
                    _state = State::ACTIVE;
                }
                break;
        }
    }

    // Call when RFID scan or touch detected — wake from any state
    void wake() {
        if (_state == State::SLEEPING) {
            _state = State::WAKING;
            _fadeBrightness = 0;
            _screenNeedsRedraw = true;
            Serial.println("[DISPLAY] Waking up.");
        } else if (_state == State::DIM) {
            _enterActive(_night ? BRIGHT_NIGHT_ACTIVE : BRIGHT_FULL);
        }
        // ACTIVE and WAKING: no change needed
    }

    // Does the caller need to redraw the screen? (After wake from sleep)
    bool needsRedraw() {
        if (_screenNeedsRedraw) {
            _screenNeedsRedraw = false;
            return true;
        }
        return false;
    }

    // Should the home screen apply pixel shift? (anti-burn-in)
    bool shouldPixelShift() {
        if (millis() - _lastShiftMs >= shiftIntervalMs) {
            _lastShiftMs = millis();
            _shiftOffset = (_shiftOffset + 1) % 4;  // Cycle through 4 positions
            return true;
        }
        return false;
    }

    // Get current pixel shift offset (0-3, maps to ±2px in x and y)
    // Apply to all static text positions on home screen
    int8_t shiftX() const {
        static const int8_t dx[] = { 0, 2, 0, -2 };
        return dx[_shiftOffset];
    }

    int8_t shiftY() const {
        static const int8_t dy[] = { 0, 0, 2, 2 };
        return dy[_shiftOffset];
    }

    State getState() const { return _state; }
    bool isSleeping() const { return _state == State::SLEEPING; }
    bool isActive() const { return _state == State::ACTIVE; }
    // Is anything readable on screen right now? (A tap on a dark screen only wakes it.)
    bool isVisible() const { return _state == State::ACTIVE || _state == State::DIM; }
    uint8_t currentBrightness() const { return _brightness; }

    // Direct brightness control (for settings UI)
    void setBrightness(uint8_t level) {
        _brightness = level;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcWrite(PIN_LCD_BL, level);   // 3.x: write by pin
#else
        ledcWrite(_pwmChannel, level);  // 2.x: write by channel
#endif
    }

    // Configure night mode hours
    void setNightHours(uint8_t startHour, uint8_t endHour) {
        nightStartHour = startHour;
        nightEndHour = endHour;
    }

private:
    static const uint8_t _pwmChannel = 0;

    State    _state = State::ACTIVE;
    uint8_t  _brightness = BRIGHT_FULL;
    uint32_t _lastActivityMs = 0;
    uint32_t _lastShiftMs = 0;
    uint8_t  _shiftOffset = 0;
    uint8_t  _fadeBrightness = 0;
    bool     _screenNeedsRedraw = false;
    bool     _night = false;   // last night-mode result from update()

    bool _isNightHour(uint8_t hour) const {
        if (nightStartHour > nightEndHour) {
            // Wraps midnight: e.g., 21:00 → 07:00
            return (hour >= nightStartHour || hour < nightEndHour);
        } else {
            return (hour >= nightStartHour && hour < nightEndHour);
        }
    }

    void _enterActive(uint8_t brightness) {
        _state = State::ACTIVE;
        setBrightness(brightness);
        Serial.println("[DISPLAY] Active.");
    }

    void _enterDim(uint8_t brightness) {
        _state = State::DIM;
        setBrightness(brightness);
        Serial.println("[DISPLAY] Dimmed.");
    }

    void _enterSleep() {
        _state = State::SLEEPING;
        setBrightness(BRIGHT_OFF);
        Serial.println("[DISPLAY] Sleeping (backlight off, RFID still active).");
    }
};
