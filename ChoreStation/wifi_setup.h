// =============================================================================
// wifi_setup.h — On-device WiFi setup (blocking modal)
// =============================================================================
// Scan → tap a network → type the password on the on-screen keyboard →
// connect. Credentials are saved to /config/wifi.json (other keys kept) only
// once the connection works; on failure the previous network is restored.
// Reached from the recovery screen, Admin → System Info, and the first-run
// wizard's welcome screen — so a new house never needs the SD card pulled.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include "keyboard.h"
#include "storage_manager.h"
#include "time_manager.h"
#include "audio_feedback.h"

extern TFT_eSPI       tft;
extern StorageManager storage;
extern TimeManager    timeMan;
extern AudioFeedback  audio;

class WifiSetup {
public:
    // Returns true if the station ended up connected to a newly chosen network.
    static bool run() {
        WifiSetup w;
        return w._loop();
    }

private:
    static const uint16_t WS_BG = 0x1082, WS_CARD = 0x2945, WS_DIM = 0x4A69,
                          WS_PRI = 0x2C9F, WS_OK = 0x2E8B, WS_WARN = 0xFDA0,
                          WS_ERR = 0xF800, WS_TXT = 0xFFFF, WS_TXT2 = 0xB596,
                          WS_TXT3 = 0x7BCF;
    static const uint8_t  MAX_NETS = 8;
    static const int16_t  LIST_Y = 72, ROW_H = 40, BTN_Y = 420;
    static const uint32_t IDLE_TIMEOUT_MS = 120000;

    struct Net { char ssid[33]; int8_t rssi; bool open; };
    Net     _nets[MAX_NETS];
    uint8_t _count = 0;
    int     _scanResult = 0;

    bool _loop() {
        _scan();
        uint16_t tx, ty;                       // ignore the tap that opened us
        uint32_t waitStart = millis();
        for (uint8_t up = 0; up < 3 && millis() - waitStart < 3000; ) {
            up = tft.getTouch(&tx, &ty) ? 0 : up + 1;
            delay(15);
        }
        uint32_t lastTouchMs = millis();
        while (millis() - lastTouchMs < IDLE_TIMEOUT_MS) {
            uint16_t x, y;
            if (!_tap(x, y)) { delay(15); continue; }
            lastTouchMs = millis();

            if (y >= BTN_Y) {
                if (x < 107) { _scan(); continue; }                        // Rescan
                if (x < 213) {                                              // Other...
                    char ssid[33] = "";
                    if (OnScreenKeyboard::run("Network name", ssid, sizeof(ssid), 32, false) && ssid[0]) {
                        if (_choose(ssid, false)) return true;
                    }
                    _drawList();
                    continue;
                }
                return false;                                               // Cancel
            }
            if (y >= LIST_Y && y < LIST_Y + MAX_NETS * ROW_H) {
                uint8_t i = (y - LIST_Y) / ROW_H;
                if (i < _count) {
                    audio.touchClick();
                    if (_choose(_nets[i].ssid, _nets[i].open)) return true;
                    _drawList();
                }
            }
        }
        return false;
    }

    // Wait for a full tap (press then release); returns where it was pressed.
    bool _tap(uint16_t& x, uint16_t& y) {
        if (!tft.getTouch(&x, &y)) return false;
        uint16_t tx, ty;
        uint8_t up = 0;
        uint32_t start = millis();
        while (up < 3 && millis() - start < 3000) {
            up = tft.getTouch(&tx, &ty) ? 0 : up + 1;
            delay(15);
        }
        return true;
    }

    void _scan() {
        _drawHeader();
        tft.setTextColor(WS_TXT2); tft.setTextSize(2);
        tft.setCursor(20, 200); tft.print("Scanning...");

        if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
        // A connection attempt in progress makes scans fail; drop it (the
        // TimeManager retries on its own later).
        if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();

        _scanResult = WiFi.scanNetworks();
        _count = 0;
        for (int i = 0; i < _scanResult && _count < MAX_NETS; i++) {
            String s = WiFi.SSID(i);
            if (!s.length()) continue;                     // hidden network
            bool dup = false;
            for (uint8_t j = 0; j < _count && !dup; j++) dup = (s == _nets[j].ssid);
            if (dup) continue;                             // mesh/extender repeats
            strlcpy(_nets[_count].ssid, s.c_str(), sizeof(_nets[_count].ssid));
            _nets[_count].rssi = (int8_t)WiFi.RSSI(i);
            _nets[_count].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
            _count++;
        }
        WiFi.scanDelete();
        Serial.printf("[WIFI] Scan: %d found, %u listed\n", _scanResult, _count);
        _drawList();
    }

    // Try a network. Returns true when connected (and saved).
    bool _choose(const char* ssid, bool open) {
        char pass[65] = "";
        if (!open) {
            if (strcmp(ssid, timeMan.ssid()) == 0) strlcpy(pass, timeMan.password(), sizeof(pass));
            if (!OnScreenKeyboard::run("Password", pass, sizeof(pass), 63, false)) return false;
        }

        char oldSsid[33], oldPass[65];
        strlcpy(oldSsid, timeMan.ssid(), sizeof(oldSsid));
        strlcpy(oldPass, timeMan.password(), sizeof(oldPass));

        _drawHeader();
        tft.setTextColor(WS_TXT2); tft.setTextSize(2);
        tft.setCursor(20, 180); tft.print("Connecting to");
        tft.setTextColor(WS_TXT);
        tft.setCursor(20, 205); tft.print(ssid);

        timeMan.useNetwork(ssid, pass);
        uint32_t start = millis();
        while (!timeMan.isWifiConnected() && millis() - start < 20000) {
            timeMan.update();
            delay(100);
        }

        tft.fillRect(0, 170, 320, 160, WS_BG);
        tft.setTextSize(2);
        if (timeMan.isWifiConnected()) {
            bool saved = storage.saveWifiCredentials(ssid, pass);
            audio.taskCompleteTone();
            tft.setTextColor(WS_OK);
            tft.setCursor(20, 180); tft.print("Connected!");
            tft.setTextColor(WS_TXT2);
            tft.setCursor(20, 210); tft.print(WiFi.localIP().toString());
            tft.setTextSize(1);
            tft.setTextColor(saved ? WS_TXT3 : WS_ERR);
            tft.setCursor(20, 245);
            tft.print(saved ? "Saved to the SD card." : "Connected, but NOT saved - check the SD card.");
            Serial.printf("[WIFI] Setup: connected to '%s' (%s)\n", ssid, saved ? "saved" : "save FAILED");
            delay(2500);
            return true;
        }

        audio.errorTone();
        tft.setTextColor(WS_ERR);
        tft.setCursor(20, 180); tft.print("Couldn't connect.");
        tft.setTextColor(WS_TXT2); tft.setTextSize(1);
        tft.setCursor(20, 210); tft.print("Check the password (it is case-sensitive)");
        tft.setCursor(20, 224); tft.print("and that the network is 2.4 GHz.");
        Serial.printf("[WIFI] Setup: could not connect to '%s'\n", ssid);
        timeMan.useNetwork(oldSsid, oldPass);   // put the old network back
        delay(2500);
        return false;
    }

    // ─── Drawing ─────────────────────────────────────────────────────────

    void _drawHeader() {
        tft.fillScreen(WS_BG);
        tft.fillRect(0, 0, 320, 48, WS_CARD);
        tft.setTextColor(WS_TXT); tft.setTextSize(2);
        tft.setCursor(12, 16); tft.print("WiFi setup");
        tft.setTextSize(1);
        tft.setCursor(12, 54);
        if (WiFi.status() == WL_CONNECTED) {
            tft.setTextColor(WS_OK);
            tft.printf("Connected to %s", WiFi.SSID().c_str());
        } else {
            tft.setTextColor(WS_WARN);
            tft.print(timeMan.ssid()[0] ? "Not connected" : "No network set up yet");
        }
    }

    void _drawList() {
        _drawHeader();
        if (_count == 0) {
            tft.setTextColor(WS_TXT2); tft.setTextSize(2);
            tft.setCursor(20, 180);
            tft.print(_scanResult < 0 ? "Scan failed." : "No networks found.");
            tft.setTextSize(1);
            tft.setCursor(20, 210); tft.print("Tap Rescan, or Other to type a name.");
        }
        for (uint8_t i = 0; i < _count; i++) {
            int16_t y = LIST_Y + i * ROW_H;
            tft.fillRoundRect(8, y, 304, ROW_H - 4, 6, WS_CARD);
            _drawBars(16, y + 8, _nets[i].rssi);
            tft.setTextColor(WS_TXT); tft.setTextSize(2);
            tft.setCursor(48, y + 10);
            char name[20];
            strlcpy(name, _nets[i].ssid, sizeof(name));       // 19 chars fit
            tft.print(name);
            tft.setTextSize(1);
            tft.setTextColor(_nets[i].open ? WS_WARN : WS_TXT3);
            tft.setCursor(282, y + 14);
            tft.print(_nets[i].open ? "open" : "");
        }
        _btn(8,   BTN_Y, 96, 44, "Rescan", WS_PRI);
        _btn(112, BTN_Y, 96, 44, "Other...", WS_DIM);
        _btn(216, BTN_Y, 96, 44, "Cancel", WS_DIM);
    }

    void _drawBars(int16_t x, int16_t y, int8_t rssi) {
        uint8_t bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
        for (uint8_t b = 0; b < 4; b++) {
            int16_t h = 5 + b * 5;
            tft.fillRect(x + b * 6, y + 20 - h, 4, h, b < bars ? WS_OK : WS_DIM);
        }
    }

    void _btn(int16_t x, int16_t y, int16_t w, int16_t h, const char* lbl, uint16_t bg) {
        tft.fillRoundRect(x, y, w, h, 6, bg);
        tft.setTextColor(WS_TXT); tft.setTextSize(2);
        tft.setCursor(x + (w - (int16_t)strlen(lbl) * 12) / 2, y + (h - 16) / 2);
        tft.print(lbl);
    }
};
