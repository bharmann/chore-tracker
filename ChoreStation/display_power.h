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
    static const uint8_t BRIGHT_NIGHT   = 30;   // Night mode (barely visible)
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
        // Configure LEDC PWM for backlight
        // Channel 0, 5000 Hz, 8-bit resolution
        ledcSetup(_pwmChannel, 5000, 8);
        ledcAttachPin(PIN_LCD_BL, _pwmChannel);

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
        bool isNightTime = _isNightHour(currentHour);
        uint8_t fullBright = isNightTime ? BRIGHT_NIGHT : BRIGHT_FULL;
        uint8_t dimBright  = isNightTime ? BRIGHT_NIGHT : BRIGHT_DIM;

        switch (_state) {
            case State::ACTIVE:
                if (idle >= sleepAfterMs) {
                    _enterSleep();
                } else if (idle >= dimAfterMs) {
                    _enterDim(dimBright);
                }
                break;

            case State::DIM:
                if (idle >= sleepAfterMs) {
                    _enterSleep();
                } else if (idle < dimAfterMs) {
                    // User became active again
                    _enterActive(fullBright);
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
            bool night = false; // Night mode resolved on next update() call
            _enterActive(night ? BRIGHT_NIGHT : BRIGHT_FULL);
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
    uint8_t currentBrightness() const { return _brightness; }

    // Direct brightness control (for settings UI)
    void setBrightness(uint8_t level) {
        _brightness = level;
        ledcWrite(_pwmChannel, level);
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
