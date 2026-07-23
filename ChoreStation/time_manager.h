// =============================================================================
// time_manager.h — WiFi Connection + NTP Time Synchronization
// =============================================================================
// The chore station needs accurate time for:
//   - Trajectory calculation (what hour is it?)
//   - Recurring task scheduling (what day of week? what time?)
//   - Daily reset (detect day rollover)
//   - Backup date stamps
//   - Completion log timestamps
//
// WiFi credentials are stored on SD card at /config/wifi.json.
// NTP syncs on boot and every 6 hours thereafter.
// Falls back to millis()-based estimation if WiFi is unavailable.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <SD.h>
#include <ArduinoJson.h>

class TimeManager {
public:
    // NTP config
    static constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
    static constexpr const char* NTP_SERVER_2 = "time.nist.gov";
    static constexpr long GMT_OFFSET_SEC      = -8 * 3600;  // PST (adjust per user)
    static constexpr int  DST_OFFSET_SEC      = 3600;       // 1hr DST

    static const uint32_t NTP_SYNC_INTERVAL_MS = 6UL * 3600 * 1000;  // 6 hours
    static const uint32_t WIFI_RETRY_INTERVAL_MS = 300000;            // 5 min between retries
    static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;            // 15s connect timeout
    static const uint32_t UPTIME_DAY_MS = 24UL * 3600 * 1000;         // 24h fallback rollover

    void begin() {
        _timeSynced = false;
        _lastSyncMs = 0;
        _lastWifiAttemptMs = 0;
        _wifiConnected = false;
        _lastDay = 0;
        _lastUptimeRollMs = millis();

        // Try loading WiFi credentials from SD
        _loadWifiConfig();

        if (strlen(_ssid) > 0) {
            Serial.printf("[WIFI] Connecting to '%s'...\n", _ssid);
            _connectWifi();
        } else {
            Serial.println("[WIFI] No credentials found at /config/wifi.json");
            Serial.println("[TIME] Running without NTP — time will be approximate.");
        }
    }

    // Call from main loop — handles reconnection and periodic re-sync
    void update() {
        // If WiFi is connected but time not synced, sync now
        if (_wifiConnected && !_timeSynced) {
            _syncNtp();
        }

        // Periodic NTP re-sync
        if (_timeSynced && millis() - _lastSyncMs > NTP_SYNC_INTERVAL_MS) {
            _syncNtp();
        }

        // Retry WiFi if disconnected
        if (!_wifiConnected && strlen(_ssid) > 0) {
            if (millis() - _lastWifiAttemptMs > WIFI_RETRY_INTERVAL_MS) {
                _connectWifi();
            }
        }

        // Check for WiFi disconnect
        if (_wifiConnected && WiFi.status() != WL_CONNECTED) {
            _wifiConnected = false;
            Serial.println("[WIFI] Connection lost.");
        }
    }

    // ─── Time Queries ────────────────────────────────────────────────────

    bool isTimeSynced() const { return _timeSynced; }
    bool isWifiConnected() const { return _wifiConnected; }

    // Current hour (0-23). Returns best estimate even without NTP.
    uint8_t currentHour() const {
        if (!_timeSynced) return 12;  // Default to noon if no time
        struct tm t;
        if (!getLocalTime(&t, 100)) return 12;
        return t.tm_hour;
    }

    uint8_t currentMinute() const {
        if (!_timeSynced) return 0;
        struct tm t;
        if (!getLocalTime(&t, 100)) return 0;
        return t.tm_min;
    }

    // Day of week: 0=Sunday, 1=Monday, ..., 6=Saturday
    uint8_t currentDayOfWeek() const {
        if (!_timeSynced) return 1;  // Default Monday
        struct tm t;
        if (!getLocalTime(&t, 100)) return 1;
        return t.tm_wday;
    }

    // Date string as "YYYY-MM-DD"
    void currentDateStr(char* buf, size_t len) const {
        if (!_timeSynced) {
            strlcpy(buf, "0000-00-00", len);
            return;
        }
        struct tm t;
        if (!getLocalTime(&t, 100)) {
            strlcpy(buf, "0000-00-00", len);
            return;
        }
        snprintf(buf, len, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    }

    // Unix epoch timestamp
    uint32_t currentEpoch() const {
        if (!_timeSynced) return 0;
        time_t now;
        time(&now);
        return (uint32_t)now;
    }

    // Day-of-month (1-31) — used for day-rollover detection
    uint8_t currentDayOfMonth() const {
        if (!_timeSynced) return 0;
        struct tm t;
        if (!getLocalTime(&t, 100)) return 0;
        return t.tm_mday;
    }

    // ─── Day Rollover Detection ──────────────────────────────────────────
    // Returns true ONCE when the day changes. Call from main loop.

    bool hasDayRolledOver() {
        if (!_timeSynced) {
            // No NTP: fall back to uptime-based rollover so offline units
            // still reset daily counters and regenerate recurring tasks.
            // Unsigned subtraction is millis()-overflow safe.
            if (millis() - _lastUptimeRollMs >= UPTIME_DAY_MS) {
                _lastUptimeRollMs = millis();
                Serial.println("[TIME] Uptime-based day rollover (no NTP).");
                return true;
            }
            return false;
        }
        uint8_t today = currentDayOfMonth();
        if (today == 0) return false;

        if (_lastDay == 0) {
            // First check after boot — not a rollover
            _lastDay = today;
            return false;
        }

        if (today != _lastDay) {
            _lastDay = today;
            Serial.printf("[TIME] Day rollover detected! New day: %d\n", today);
            return true;
        }
        return false;
    }

    // ─── Timezone configuration ──────────────────────────────────────────

    void setTimezone(long gmtOffsetSec, int dstOffsetSec) {
        configTime(gmtOffsetSec, dstOffsetSec, NTP_SERVER_1, NTP_SERVER_2);
        _syncNtp();
    }

private:
    char _ssid[33];
    char _password[65];
    bool _timeSynced;
    bool _wifiConnected;
    uint32_t _lastSyncMs;
    uint32_t _lastWifiAttemptMs;
    uint8_t  _lastDay;
    uint32_t _lastUptimeRollMs;

    bool _loadWifiConfig() {
        _ssid[0] = '\0';
        _password[0] = '\0';

        File f = SD.open("/config/wifi.json", FILE_READ);
        if (!f) return false;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err) {
            Serial.printf("[WIFI] wifi.json parse error: %s\n", err.c_str());
            return false;
        }

        strlcpy(_ssid, doc["ssid"] | "", sizeof(_ssid));
        strlcpy(_password, doc["password"] | "", sizeof(_password));

        // Optional timezone override
        if (doc["gmtOffset"].is<long>()) {
            long gmt = doc["gmtOffset"];
            int dst = doc["dstOffset"] | 3600;
            configTime(gmt, dst, NTP_SERVER_1, NTP_SERVER_2);
        } else {
            configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
        }

        return strlen(_ssid) > 0;
    }

    void _connectWifi() {
        _lastWifiAttemptMs = millis();
        WiFi.mode(WIFI_STA);
        WiFi.begin(_ssid, _password);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED &&
               millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
            delay(250);
        }

        if (WiFi.status() == WL_CONNECTED) {
            _wifiConnected = true;
            Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        } else {
            _wifiConnected = false;
            Serial.println("[WIFI] Connection failed. Will retry.");
            WiFi.disconnect(true);
        }
    }

    void _syncNtp() {
        if (!_wifiConnected) return;

        Serial.println("[NTP] Syncing time...");
        struct tm t;
        if (getLocalTime(&t, 5000)) {
            _timeSynced = true;
            _lastSyncMs = millis();
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
            Serial.printf("[NTP] Time synced: %s\n", buf);
        } else {
            Serial.println("[NTP] Sync failed — will retry.");
        }
    }
};
