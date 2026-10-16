// =============================================================================
// audio_feedback.h — Short feedback cues via the DAC + FM8002E amplifier
// =============================================================================
// The E32R40T routes DAC channel 2 (GPIO26) through an FM8002E amplifier to
// the speaker connector. GPIO4 enables the amplifier (active LOW).
//
// Cues play in the background: the ESP32's hardware cosine generator makes
// the tone on GPIO26 and a one-shot esp_timer steps through the notes, so
// playing a sound never freezes the screen (the old square wave busy-waited
// 0.3-1.5 s per sound). esp_timer runs in its own task, so cues finish even
// inside blocking modals (keyboard, menus) where loop() doesn't run.
//
// A new cue replaces whatever is playing. Note lists live in input_logic.h
// (host-tested). Fallback: #define AUDIO_DAC_BLOCKING 1 before including this
// file to get the old blocking square wave.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "pins.h"
#include "input_logic.h"

#ifndef AUDIO_DAC_BLOCKING
#define AUDIO_DAC_BLOCKING 0
#endif

#if !AUDIO_DAC_BLOCKING
#include "driver/dac_cosine.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

class AudioFeedback {
public:
    void begin() {
        pinMode(PIN_AUDIO_ENABLE, OUTPUT);
        digitalWrite(PIN_AUDIO_ENABLE, HIGH);  // Amp OFF by default (save power)
#if !AUDIO_DAC_BLOCKING
        _lock = xSemaphoreCreateMutex();
        esp_timer_create_args_t args = {};
        args.callback = &AudioFeedback::_onTimer;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "cue";
        if (esp_timer_create(&args, &_timer) != ESP_OK) _timer = nullptr;
#endif
    }

    // Names kept from the old API; each plays one cue.
    void touchClick()       { _play(cues::CLICK,  CUE_LEN(cues::CLICK)); }
    void scanBeep()         { _play(cues::SCAN,   CUE_LEN(cues::SCAN)); }
    void taskStartTone()    { _play(cues::START,  CUE_LEN(cues::START)); }
    void taskCompleteTone() { _play(cues::POINTS, CUE_LEN(cues::POINTS)); }   // points earned
    void errorTone()        { _play(cues::ERROR,  CUE_LEN(cues::ERROR)); }
    void sentTone()         { _play(cues::SENT,   CUE_LEN(cues::SENT)); }     // waiting for a parent
    void badgeTone()        { _play(cues::BADGE,  CUE_LEN(cues::BADGE)); }    // new badge
    void redoTone()         { _play(cues::REDO,   CUE_LEN(cues::REDO)); }     // sent back to redo

private:
#if AUDIO_DAC_BLOCKING
    // ── Fallback: the original blocking square wave, one cue at a time ──
    void _play(const ToneNote* notes, uint8_t count) {
        for (uint8_t i = 0; i < count; i++) {
            if (notes[i].hz == 0) { delay(notes[i].ms); continue; }
            _square(notes[i].hz, notes[i].ms);
        }
    }
    void _square(uint16_t freqHz, uint16_t durationMs) {
        digitalWrite(PIN_AUDIO_ENABLE, LOW);
        delay(5);
        uint32_t halfPeriodUs = 500000 / freqHz;
        uint32_t cycles = (uint32_t)freqHz * durationMs / 1000;
        for (uint32_t i = 0; i < cycles; i++) {
            dacWrite(PIN_AUDIO_DAC, 200); delayMicroseconds(halfPeriodUs);
            dacWrite(PIN_AUDIO_DAC, 55);  delayMicroseconds(halfPeriodUs);
        }
        dacWrite(PIN_AUDIO_DAC, 128);
        delay(5);
        digitalWrite(PIN_AUDIO_ENABLE, HIGH);
    }
#else
    // ── Background cues: cosine generator + one-shot timer ──
    esp_timer_handle_t     _timer = nullptr;
    SemaphoreHandle_t      _lock  = nullptr;
    dac_cosine_handle_t    _chan  = nullptr;
    ToneSeq                _seq;
    int64_t                _dueUs = 0;   // when the armed timer should fire

    void _play(const ToneNote* notes, uint8_t count) {
        if (!_timer || !_lock) return;
        xSemaphoreTake(_lock, portMAX_DELAY);
        esp_timer_stop(_timer);              // fine if it isn't running
        _seq.start(notes, count);
        _soundCurrent();
        xSemaphoreGive(_lock);
    }

    static void _onTimer(void* arg) { static_cast<AudioFeedback*>(arg)->_next(); }

    void _next() {
        xSemaphoreTake(_lock, portMAX_DELAY);
        // A callback already waiting on the lock when a new cue started is
        // stale (the new cue re-armed the timer): it must not skip a note.
        if (esp_timer_get_time() + 2000 >= _dueUs) {
            if (_seq.advance()) _soundCurrent();
            else                _silence();
        }
        xSemaphoreGive(_lock);
    }

    // Caller holds _lock. The amp stays on from note to note (switching it
    // per note pops); it's off during rests and after the last note.
    void _soundCurrent() {
        ToneNote n{};
        if (!_seq.current(n)) { _silence(); return; }
        if (n.hz == 0) _silence();                    // rest: amp off, wave stopped
        else           _stopWave();                   // next note: just swap the wave
        if (n.hz) {
            dac_cosine_config_t cfg = {};
            cfg.chan_id = DAC_CHAN_1;                 // GPIO26
            cfg.freq_hz = n.hz;
            cfg.clk_src = DAC_COSINE_CLK_SRC_DEFAULT;
            cfg.atten   = static_cast<dac_cosine_atten_t>(n.vol > 3 ? 3 : n.vol);
            cfg.phase   = DAC_COSINE_PHASE_0;
            cfg.offset  = 0;
            cfg.flags.force_set_freq = true;
            if (dac_cosine_new_channel(&cfg, &_chan) == ESP_OK) {
                dac_cosine_start(_chan);
                digitalWrite(PIN_AUDIO_ENABLE, LOW);  // amp on after the wave runs
            } else {
                _chan = nullptr;
            }
        }
        _dueUs = esp_timer_get_time() + (int64_t)n.ms * 1000;
        esp_timer_start_once(_timer, (uint64_t)n.ms * 1000);
    }

    // Caller holds _lock. Amp off first so stopping the wave doesn't pop.
    void _silence() {
        digitalWrite(PIN_AUDIO_ENABLE, HIGH);
        _stopWave();
    }

    void _stopWave() {
        if (_chan) {
            dac_cosine_stop(_chan);
            dac_cosine_del_channel(_chan);
            _chan = nullptr;
        }
    }
#endif
};
