// =============================================================================
// ota_update.h — Firmware updates from GitHub releases (parent-triggered)
// =============================================================================
// A release (tag vX.Y.Z) carries two files, built by the release workflow:
//   version.json      {"version":"v1.4.0","size":1234567,"sha256":"..."}
//   ChoreStation.bin  the app image for the OTA slot
// fetched from github.com/<repo>/releases/latest/download/... over HTTPS
// (certificates checked against the core's built-in CA bundle; GitHub
// redirects to its download host, which is followed).
//
// check() runs now and then while the station is idle; it only finds out
// whether a newer version exists. install() is run by a parent (Admin ->
// System Info, or the web editor): it downloads into the spare OTA slot,
// checks the size and SHA-256, and only then switches. The new firmware boots
// "pending verify": healthCheck() marks it good once it has run cleanly for a
// minute; if it crashes or can't see the SD card first, the bootloader goes
// back to the previous firmware (rollback is enabled in the core's bootloader).
// =============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#include "ota_logic.h"
#include "version.h"

// The core's built-in CA certificate bundle (linked from libmbedtls).
extern const uint8_t ota_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t ota_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

class OtaUpdater {
public:
    static constexpr const char* RELEASES = "https://github.com/bharmann/chore-tracker/releases/latest/download/";
    static const uint32_t CHECK_EVERY_MS = 6UL * 3600 * 1000;   // 6 h
    static const uint32_t HEALTHY_AFTER_MS = 60000;             // new firmware proves itself in 1 min

    enum class State : uint8_t { NOT_CHECKED, CHECKING, UP_TO_DATE, AVAILABLE, FAILED };

    State    state = State::NOT_CHECKED;
    char     latest[24] = "";          // newest release tag seen
    uint32_t latestSize = 0;
    char     latestSha[65] = "";
    char     lastError[48] = "";
    uint32_t lastCheckMs = 0;
    bool     checkedOnce = false;

    const char* current() const { return FW_VERSION; }
    bool updateAvailable() const { return state == State::AVAILABLE; }

    // Time for a background check? (WiFi up, not checked recently)
    bool checkDue() const {
        if (WiFi.status() != WL_CONNECTED) return false;
        return !checkedOnce || millis() - lastCheckMs >= CHECK_EVERY_MS;
    }

    // Blocking (~1-3 s). Returns true if a newer release exists.
    bool check() {
        checkedOnce = true;
        lastCheckMs = millis();
        if (WiFi.status() != WL_CONNECTED) { _fail("No WiFi"); return false; }
        state = State::CHECKING;

        NetworkClientSecure client;
        _secure(client);
        HTTPClient http;
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(10000);
        String url = String(RELEASES) + "version.json";
        if (!http.begin(client, url)) { _fail("Can't reach GitHub"); return false; }
        int code = http.GET();
        if (code != 200) {
            char e[32]; snprintf(e, sizeof(e), code == 404 ? "No release yet" : "GitHub said %d", code);
            http.end(); _fail(e); return false;
        }
        String body = http.getString();
        http.end();

        JsonDocument doc;
        if (deserializeJson(doc, body)) { _fail("Bad version.json"); return false; }
        const char* v = doc["version"] | "";
        const char* sha = doc["sha256"] | "";
        uint32_t size = doc["size"] | 0;
        if (!parseVersionOk(v) || !isHexSha256(sha) || size < 100000) { _fail("Bad version.json"); return false; }

        strlcpy(latest, v, sizeof(latest));
        strlcpy(latestSha, sha, sizeof(latestSha));
        latestSize = size;
        lastError[0] = '\0';
        state = compareVersions(latest, FW_VERSION) > 0 ? State::AVAILABLE : State::UP_TO_DATE;
        Serial.printf("[OTA] Running %s, latest release %s -> %s\n", FW_VERSION, latest,
                      state == State::AVAILABLE ? "update available" : "up to date");
        return state == State::AVAILABLE;
    }

    // Blocking download + flash of the release found by check(). progress(done,
    // total) is called as it goes. Returns true when the new firmware is in
    // place (caller restarts); false with lastError set otherwise, and the
    // running firmware is untouched.
    bool install(void (*progress)(uint32_t done, uint32_t total)) {
        if (state != State::AVAILABLE) { _fail("No update to install"); return false; }

        NetworkClientSecure client;
        _secure(client);
        HTTPClient http;
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(20000);
        String url = String(RELEASES) + "ChoreStation.bin";
        if (!http.begin(client, url)) { _fail("Can't reach GitHub"); return false; }
        int code = http.GET();
        if (code != 200) {
            char e[32]; snprintf(e, sizeof(e), "Download failed (%d)", code);
            http.end(); _fail(e); return false;
        }
        int len = http.getSize();
        if (len <= 0 || (uint32_t)len != latestSize) {
            http.end(); _fail("Size doesn't match the release"); return false;
        }
        if (!Update.begin(len, U_FLASH)) {
            http.end(); _fail("Not enough room for it"); return false;
        }

        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        NetworkClient* stream = http.getStreamPtr();
        static uint8_t buf[2048];
        uint32_t done = 0, lastData = millis();
        while (done < (uint32_t)len) {
            size_t avail = stream->available();
            if (!avail) {
                if (!http.connected() || millis() - lastData > 20000) break;
                delay(2);
                continue;
            }
            size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
            if (want > (uint32_t)len - done) want = (uint32_t)len - done;
            int n = stream->readBytes(buf, want);
            if (n <= 0) continue;
            if (Update.write(buf, n) != (size_t)n) break;
            mbedtls_sha256_update(&sha, buf, n);
            done += n;
            lastData = millis();
            if (progress) progress(done, len);
        }
        http.end();

        uint8_t digest[32];
        mbedtls_sha256_finish(&sha, digest);
        mbedtls_sha256_free(&sha);
        if (done != (uint32_t)len) { Update.abort(); _fail("Download stopped early"); return false; }
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        if (strcmp(hex, latestSha) != 0) { Update.abort(); _fail("File check failed (SHA-256)"); return false; }
        if (!Update.end(true)) { _fail(Update.errorString()); return false; }
        Serial.printf("[OTA] %s installed (%u bytes) - restarting.\n", latest, (unsigned)len);
        return true;
    }

    // Call every loop. A freshly installed firmware is marked good after a
    // minute of normal running with the SD card readable; if the card can't be
    // read for that long, go back to the previous firmware.
    void healthCheck(bool sdOk) {
        if (_healthDone || millis() < HEALTHY_AFTER_MS) return;
        _healthDone = true;
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t st;
        if (!running || esp_ota_get_state_partition(running, &st) != ESP_OK) return;   // USB flash: nothing to do
        if (st != ESP_OTA_IMG_PENDING_VERIFY) return;
        if (sdOk) {
            esp_ota_mark_app_valid_cancel_rollback();
            Serial.println("[OTA] New firmware running fine - kept.");
        } else {
            Serial.println("[OTA] New firmware can't read the SD card - going back.");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }

private:
    bool _healthDone = false;

    static bool parseVersionOk(const char* v) { int t[3]; return parseVersion(v, t); }

    void _secure(NetworkClientSecure& c) {
        c.setCACertBundle(ota_crt_bundle_start, ota_crt_bundle_end - ota_crt_bundle_start);
        c.setTimeout(15);   // seconds
    }

    void _fail(const char* why) {
        strlcpy(lastError, why, sizeof(lastError));
        if (state == State::CHECKING || state == State::NOT_CHECKED) state = State::FAILED;
        Serial.printf("[OTA] %s\n", why);
    }
};
