// =============================================================================
// rfid_manager.h — RC522 RFID Reader on VSPI Bus
// =============================================================================
// The RC522 shares the VSPI bus with the SD card. Both use separate CS pins
// so only one device talks at a time. The MFRC522 library uses the default
// SPI instance, so we need to initialize SPI with VSPI pins before calling
// PCD_Init(), and manage CS lines carefully.
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

        _rfid.PCD_Init();
        delay(50);

        // Verify RC522 is responding
        byte version = _rfid.PCD_ReadRegister(MFRC522::VersionReg);
        if (version == 0x00 || version == 0xFF) {
            Serial.println("[RFID] ERROR: RC522 not detected! Check wiring.");
            _online = false;
        } else {
            Serial.printf("[RFID] RC522 firmware version: 0x%02X\n", version);
            Serial.println("[RFID] Reader online and ready.");
            _online = true;
        }
    }

    // Call in loop(). Returns true if a new (debounced) tag was read.
    // Populates uid[] and uidLength on success.
    bool poll(uint8_t* uid, uint8_t& uidLength) {
        if (!_online) return false;

        // Deselect SD card to be safe
        digitalWrite(PIN_SD_CS, HIGH);

        // Check for new card
        if (!_rfid.PICC_IsNewCardPresent()) return false;
        if (!_rfid.PICC_ReadCardSerial())   return false;

        // Debounce: ignore if same UID read within debounce window
        uint32_t now = millis();
        if (_isSameUid(_rfid.uid.uidByte, _rfid.uid.size) &&
            (now - _lastReadMs) < _debounceMs) {
            _rfid.PICC_HaltA();
            _rfid.PCD_StopCrypto1();
            return false;
        }

        // New valid read
        uidLength = _rfid.uid.size;
        memcpy(uid, _rfid.uid.uidByte, uidLength);

        // Update debounce state
        memcpy(_lastUid, _rfid.uid.uidByte, uidLength);
        _lastUidLength = uidLength;
        _lastReadMs = now;

        // Halt card
        _rfid.PICC_HaltA();
        _rfid.PCD_StopCrypto1();

        Serial.printf("[RFID] Tag detected: ");
        for (uint8_t i = 0; i < uidLength; i++) {
            Serial.printf("%02X ", uid[i]);
        }
        Serial.println();

        return true;
    }

    bool isOnline() const { return _online; }

    // Set debounce time (ms) — how long to ignore re-reads of same tag
    void setDebounceMs(uint32_t ms) { _debounceMs = ms; }

    // Match a scanned UID against a family member's stored UID
    static bool matchUid(const uint8_t* scanned, uint8_t scannedLen,
                         const FamilyMember& member) {
        if (scannedLen != member.rfidUidLength) return false;
        return memcmp(scanned, member.rfidUid, scannedLen) == 0;
    }

private:
    MFRC522  _rfid;
    bool     _online = false;
    uint8_t  _lastUid[UID_LENGTH];
    uint8_t  _lastUidLength = 0;
    uint32_t _lastReadMs;
    uint32_t _debounceMs;

    bool _isSameUid(const uint8_t* uid, uint8_t len) const {
        if (len != _lastUidLength) return false;
        return memcmp(uid, _lastUid, len) == 0;
    }
};
