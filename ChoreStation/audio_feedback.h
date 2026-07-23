// =============================================================================
// audio_feedback.h — Simple Tone Feedback via DAC + FM8002E Amplifier
// =============================================================================
// The E32R40T has a DAC output on GPIO26 routed through an FM8002E amplifier
// to the speaker connector. GPIO4 controls amplifier enable (active LOW).
//
// For the chore station we keep audio simple: short tones and melodies
// generated via the DAC. No MP3/WAV decoding needed for feedback sounds.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "pins.h"

class AudioFeedback {
public:
    void begin() {
        pinMode(PIN_AUDIO_ENABLE, OUTPUT);
        digitalWrite(PIN_AUDIO_ENABLE, HIGH);  // Amp OFF by default (save power)
    }

    // Play a quick "beep" confirmation tone
    void scanBeep() {
        _playTone(880, 80);   // A5, 80ms
    }

    // Ascending two-tone: task started
    void taskStartTone() {
        _playTone(523, 100);  // C5
        delay(30);
        _playTone(659, 100);  // E5
    }

    // Triumphant three-tone: task completed
    void taskCompleteTone() {
        _playTone(523, 80);   // C5
        delay(20);
        _playTone(659, 80);   // E5
        delay(20);
        _playTone(784, 150);  // G5
    }

    // Descending tone: error or rejection
    void errorTone() {
        _playTone(440, 150);  // A4
        delay(50);
        _playTone(330, 200);  // E4
    }

    // Quick click for touch feedback
    void touchClick() {
        _playTone(1000, 15);
    }

private:
    void _playTone(uint16_t freqHz, uint16_t durationMs) {
        // Enable amplifier
        digitalWrite(PIN_AUDIO_ENABLE, LOW);
        delay(5);  // Let amp stabilize

        // Generate square wave via DAC
        // DAC is 8-bit (0-255), we toggle between low and high
        uint32_t halfPeriodUs = 500000 / freqHz;
        uint32_t cycles = (uint32_t)freqHz * durationMs / 1000;

        for (uint32_t i = 0; i < cycles; i++) {
            dacWrite(PIN_AUDIO_DAC, 200);
            delayMicroseconds(halfPeriodUs);
            dacWrite(PIN_AUDIO_DAC, 55);
            delayMicroseconds(halfPeriodUs);
        }

        // Silence and disable amp
        dacWrite(PIN_AUDIO_DAC, 128);  // Mid-point (silence)
        delay(5);
        digitalWrite(PIN_AUDIO_ENABLE, HIGH);  // Amp OFF
    }
};
