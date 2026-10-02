// =============================================================================
// rfid_manager.h — RC522 RFID Reader on VSPI Bus
// =============================================================================
// The RC522 shares the VSPI bus with the SD card. Both use separate CS pins
// so only one device talks at a time. The MFRC522 library uses the default
// SPI instance, so we need to initialize SPI with VSPI pins before calling
// PCD_Init(), and manage CS lines carefully.
//
// The display MUST be on its own bus (USE_HSPI_PORT in User_Setup.h) or
// tft.init() steals VSPI's MISO line and the reader goes deaf after boot.
//
// Health check: the reader is re-probed every PROBE_INTERVAL_MS. A reader that
// was missing at boot (wired while powered, slow power-up, loose connector)
// comes online without a reboot, and one that brownout-resets (which turns its
// antenna off) is re-initialised automatically.
//
// The RC522 only reads 13.56 MHz ISO14443A tags (MIFARE Classic/Ultralight,
// NTAG21x). 125 kHz fobs (EM4100/HID Prox) are invisible to it.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include "pins.h"
#include "data_model.h"

class RFIDManager {
public:
    RFIDManager()
        : _rfid(PIN_RFID_CS, PIN_RFID_RST)
        , _lastReadMs(0)
        , _debounceMs(1500)   // Ignore re-reads within 1.5 seconds
    {
        memset(_lastUid, 0, sizeof(_lastUid));
    }

    // Call once in setup() AFTER SD card init (both share VSPI)
    void begin() {
        // Ensure SD CS is HIGH (deselected) before we talk to RC522
        pinMode(PIN_SD_CS, OUTPUT);
        digitalWrite(PIN_SD_CS, HIGH);

        // The VSPI bus should already be initialized by SD card setup.
        // If SD isn't used, uncomment:
        // SPI.begin(PIN_VSPI_CLK, PIN_VSPI_MISO, PIN_VSPI_MOSI, PIN_RFID_CS);

        _init();
        _lastProbeMs = millis();
    }

    // Call in loop(). Returns true if a new (debounced) tag was read.
    // Populates uid[] and uidLength on success.
    bool poll(uint8_t* uid, uint8_t& uidLength) {
        uint32_t now = millis();
        if (now - _lastProbeMs >= PROBE_INTERVAL_MS) {
            _lastProbeMs = now;
            _probe();
        }
        if (!_online) return false;

        // Deselect SD card to be safe
        digitalWrite(PIN_SD_CS, HIGH);

        // Check for new card
        if (!_rfid.PICC_IsNewCardPresent()) return false;
        if (!_rfid.PICC_ReadCardSerial())   return false;

        // Triple-size (10-byte) UIDs exist; never copy more than our buffers hold
        uint8_t len = _rfid.uid.size;
        if (len > UID_LENGTH) len = UID_LENGTH;

        // Debounce: ignore if same UID read within debounce window
        if (_isSameUid(_rfid.uid.uidByte, len) &&
            (now - _lastReadMs) < _debounceMs) {
            _rfid.PICC_HaltA();
            _rfid.PCD_StopCrypto1();
            return false;
        }

        // New valid read
        uidLength = len;
        memcpy(uid, _rfid.uid.uidByte, len);

        // Update debounce state
        memcpy(_lastUid, _rfid.uid.uidByte, len);
        _lastUidLength = len;
        _lastReadMs = now;

        // Halt card
        _rfid.PICC_HaltA();
        _rfid.PCD_StopCrypto1();

        // Colon format matches family.json, so it can be pasted straight in
        char hex[UID_LENGTH * 3 + 1];
        uidToHex(uid, uidLength, hex, sizeof(hex));
        Serial.printf("[RFID] Tag detected: %s (SAK 0x%02X)\n", hex, _rfid.uid.sak);

        return true;
    }

    bool isOnline() const { return _online; }
    uint8_t version() const { return _version; }

    // Set debounce time (ms) — how long to ignore re-reads of same tag
    void setDebounceMs(uint32_t ms) { _debounceMs = ms; }

    // Match a scanned UID against a family member's stored UID
    static bool matchUid(const uint8_t* scanned, uint8_t scannedLen,
                         const FamilyMember& member) {
        if (scannedLen != member.rfidUidLength) return false;
        return memcmp(scanned, member.rfidUid, scannedLen) == 0;
    }

    // "04:A2:3B:C1" — the format family.json and the web UI use
    static void uidToHex(const uint8_t* uid, uint8_t len, char* out, size_t outLen) {
        if (!outLen) return;
        out[0] = '\0';
        for (uint8_t i = 0; i < len; i++) {
            char seg[4];
            snprintf(seg, sizeof(seg), i ? ":%02X" : "%02X", uid[i]);
            strlcat(out, seg, outLen);
        }
    }

private:
    static const uint32_t PROBE_INTERVAL_MS = 5000;

    MFRC522  _rfid;
    bool     _online = false;
    uint8_t  _version = 0;
    uint8_t  _lastUid[UID_LENGTH];
    uint8_t  _lastUidLength = 0;
    uint32_t _lastReadMs;
    uint32_t _debounceMs;
    uint32_t _lastProbeMs = 0;

    static bool _validVersion(uint8_t v) { return v != 0x00 && v != 0xFF; }

    void _init() {
        _rfid.PCD_Init();
        delay(50);
        _version = _rfid.PCD_ReadRegister(MFRC522::VersionReg);
        _online = _validVersion(_version);
        if (_online) {
            _rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
            Serial.printf("[RFID] RC522 firmware version: 0x%02X\n", _version);
            Serial.println("[RFID] Reader online and ready.");
        } else {
            Serial.printf("[RFID] ERROR: RC522 not detected (version 0x%02X). "
                          "0x00 = no power/GND/RST or MISO stuck low, "
                          "0xFF = MISO floating. Will keep retrying.\n", _version);
        }
    }

    // Cheap periodic health check. Re-initialises when the chip (re)appears or
    // has lost its antenna configuration after a brownout reset.
    void _probe() {
        digitalWrite(PIN_SD_CS, HIGH);
        uint8_t v = _rfid.PCD_ReadRegister(MFRC522::VersionReg);
        if (!_validVersion(v)) {
            if (_online) Serial.println("[RFID] Reader lost — will keep retrying.");
            _online = false;
            _version = v;
            return;
        }
        bool antennaOn = (_rfid.PCD_ReadRegister(MFRC522::TxControlReg) & 0x03) == 0x03;
        if (!_online || !antennaOn) _init();
    }

    bool _isSameUid(const uint8_t* uid, uint8_t len) const {
        if (len != _lastUidLength) return false;
        return memcmp(uid, _lastUid, len) == 0;
    }
};
