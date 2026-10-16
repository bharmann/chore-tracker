// =============================================================================
// setup_wizard.h — First-Run Family Setup Wizard
// =============================================================================
// Guides the user through registering family members by scanning RFID tags.
// Flow:
//   1. Welcome screen — "Scan a parent tag to begin"
//   2. Scan parent 1 → enter name via touch keyboard
//   3. Scan parent 2 (or skip for single-parent)
//   4. Scan child 1 → enter name
//   5. Scan child 2 → enter name (or skip)
//   6. Review & confirm
//   7. Save to SD → reboot into normal mode
//
// The touch keyboard is simplified: preset name selection from a list,
// since the resistive touchscreen + stylus makes a full QWERTY impractical
// for initial setup. Names can be changed later via parent admin.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "pins.h"
#include "data_model.h"
#include "icons.h"
#include "rfid_manager.h"
#include "audio_feedback.h"
#include "storage_manager.h"
#include "keyboard.h"
#include "wifi_setup.h"

extern TFT_eSPI      tft;
extern RFIDManager   rfid;
extern AudioFeedback audio;
extern StorageManager storage;

class SetupWizard {
public:
    enum class Step : uint8_t {
        WELCOME,
        SCAN_PARENT,
        NAME_PARENT,
        SCAN_CHILD,
        NAME_CHILD,
        REVIEW,
        COMPLETE
    };

    void begin() {
        _step = Step::WELCOME;
        _memberCount = 0;
        _currentScanRole = Role::PARENT;
        _parentsDone = 0;
        _childrenDone = 0;
        _selectedNameIdx = 0;
        memset(_members, 0, sizeof(_members));
        drawCurrentStep();
    }

    // Call from main loop — returns true when setup is complete
    bool update() {
        if (_step == Step::COMPLETE) return true;

        // Poll RFID
        uint8_t uid[UID_LENGTH];
        uint8_t uidLen = 0;
        if (rfid.poll(uid, uidLen)) {
            handleScan(uid, uidLen);
        }

        // Reader status changed (e.g. just wired up) → refresh the prompt
        if (rfid.isOnline() != _readerOnline) {
            _readerOnline = rfid.isOnline();
            if (_step == Step::SCAN_PARENT || _step == Step::SCAN_CHILD) drawCurrentStep();
        }

        // Poll touch
        uint16_t tx, ty;
        if (tft.getTouch(&tx, &ty)) {
            handleTouch(tx, ty);
            delay(200);  // Basic debounce for resistive touch
        }

        return false;
    }

    // Get the configured family members
    FamilyMember* getMembers() { return _members; }
    uint8_t getMemberCount() const { return _memberCount; }

private:
    Step          _step;
    FamilyMember  _members[MAX_FAMILY_MEMBERS];
    uint8_t       _memberCount;
    Role          _currentScanRole;
    uint8_t       _parentsDone;
    uint8_t       _childrenDone;
    uint8_t       _selectedNameIdx;     // CUSTOM_NAME = typed on the keyboard
    char          _customName[NAME_LENGTH] = "";
    bool          _readerOnline = false;

    static const uint8_t CUSTOM_NAME = 255;
    static const int16_t TYPE_BTN_Y = 385;   // "Type a name" button

    // Temporary storage for the tag just scanned
    uint8_t       _pendingUid[UID_LENGTH];
    uint8_t       _pendingUidLen;

    // Preset name lists (can be customized later via admin)
    static const uint8_t PARENT_NAME_COUNT = 6;
    static const uint8_t CHILD_NAME_COUNT = 12;

    const char* _parentNames[PARENT_NAME_COUNT] = {
        "Dad", "Mom", "Papa", "Mama", "Parent 1", "Parent 2"
    };

    const char* _childNames[CHILD_NAME_COUNT] = {
        "Alex", "Jordan", "Sam", "Riley", "Taylor", "Casey",
        "Morgan", "Drew", "Quinn", "Avery", "Child 1", "Child 2"
    };

    // ─── Drawing ─────────────────────────────────────────────────────────

    void drawCurrentStep() {
        tft.fillScreen(0x1082);

        // Step indicator at top
        _drawStepIndicator();

        switch (_step) {
            case Step::WELCOME:     _drawWelcome(); break;
            case Step::SCAN_PARENT: _drawScanPrompt("parent"); break;
            case Step::NAME_PARENT: _drawNamePicker(_parentNames, PARENT_NAME_COUNT); break;
            case Step::SCAN_CHILD:  _drawScanPrompt("child"); break;
            case Step::NAME_CHILD:  _drawNamePicker(_childNames, CHILD_NAME_COUNT); break;
            case Step::REVIEW:      _drawReview(); break;
            case Step::COMPLETE:    break;
        }
    }

    void _drawStepIndicator() {
        // Simple dot indicator showing progress
        uint8_t totalSteps = 5;
        uint8_t currentStep;
        switch (_step) {
            case Step::WELCOME:     currentStep = 0; break;
            case Step::SCAN_PARENT:
            case Step::NAME_PARENT: currentStep = 1; break;
            case Step::SCAN_CHILD:
            case Step::NAME_CHILD:  currentStep = 2 + _parentsDone; break;
            case Step::REVIEW:      currentStep = 4; break;
            default:                currentStep = 0; break;
        }

        int16_t startX = (320 - totalSteps * 20) / 2;
        for (uint8_t i = 0; i < totalSteps; i++) {
            int16_t dx = startX + i * 20;
            if (i < currentStep) {
                tft.fillCircle(dx + 6, 10, 4, 0x2C9F);   // Completed
            } else if (i == currentStep) {
                tft.fillCircle(dx + 6, 10, 5, 0xFFFF);    // Current
            } else {
                tft.drawCircle(dx + 6, 10, 4, 0x4A69);    // Future
            }
        }
    }

    void _drawWelcome() {
        tft.setTextColor(0xFFFF);
        tft.setTextSize(3);
        tft.setCursor(45, 80);
        tft.print("Welcome!");

        Icons::drawRfidIcon(136, 160, 0x2C9F, 48);

        tft.setTextColor(0xB596);
        tft.setTextSize(2);
        tft.setCursor(30, 250);
        tft.print("Let's set up your");
        tft.setCursor(30, 275);
        tft.print("family chore station");

        tft.setTextColor(0x7BCF);
        tft.setTextSize(1);
        tft.setCursor(30, 330);
        tft.print("You'll register each family member");
        tft.setCursor(30, 345);
        tft.print("by scanning their RFID tag.");
        tft.setCursor(30, 360);
        tft.print("Names can be changed later in the web editor.");

        // "Begin" button
        _drawButton(85, 400, 150, 45, "BEGIN", 0x2C9F);

        // WiFi first, so the station has the time and the web editor from day one
        tft.fillRoundRect(85, 452, 150, 24, 6, 0x2945);
        tft.setTextColor(WiFi.status() == WL_CONNECTED ? 0x2E8B : 0xB596);
        tft.setTextSize(1);
        tft.setCursor(WiFi.status() == WL_CONNECTED ? 91 : 130, 460);
        tft.print(WiFi.status() == WL_CONNECTED ? "WiFi connected - change" : "WiFi setup");
    }

    void _drawScanPrompt(const char* roleLabel) {
        tft.setTextColor(0xFFFF);
        tft.setTextSize(2);
        tft.setCursor(30, 60);
        tft.printf("Scan %s tag", roleLabel);

        uint8_t num = (_currentScanRole == Role::PARENT)
                      ? _parentsDone + 1
                      : _childrenDone + 1;
        tft.setTextColor(0x7BCF);
        tft.setTextSize(1);
        tft.setCursor(30, 90);
        tft.printf("%s #%d - hold tag near reader", roleLabel, num);

        Icons::drawRfidIcon(136, 160, rfid.isOnline() ? 0x2C9F : 0xF800, 48);
        if (!rfid.isOnline()) {
            tft.setTextColor(0xF800);
            tft.setTextSize(1);
            tft.setCursor(30, 265);
            tft.print("Tag reader offline - check RC522 wiring");
            tft.setCursor(30, 279);
            tft.print("(3.3V, GND, RST). Retrying every 5 s.");
        }

        // Scanning animation hint
        tft.setTextColor(0x4A69);
        tft.setTextSize(1);
        tft.setCursor(90, 240);
        tft.print("Waiting for scan...");

        // Skip button (for optional 2nd parent / 2nd child)
        if ((_currentScanRole == Role::PARENT && _parentsDone >= 1) ||
            (_currentScanRole == Role::CHILD  && _childrenDone >= 1)) {
            _drawButton(85, 420, 150, 35, "SKIP", 0x4A69);
        }
    }

    void _drawNamePicker(const char** names, uint8_t count) {
        tft.setTextColor(0xFFFF);
        tft.setTextSize(2);
        tft.setCursor(30, 50);
        tft.print("Choose a name:");

        tft.setTextColor(0x7BCF);
        tft.setTextSize(1);
        tft.setCursor(30, 75);
        tft.print("Tap to select, then tap CONFIRM");

        // Name grid (2 columns)
        uint8_t cols = 2;
        int16_t btnW = 140;
        int16_t btnH = 38;
        int16_t padX = 10;
        int16_t padY = 6;
        int16_t startY = 100;

        for (uint8_t i = 0; i < count; i++) {
            uint8_t col = i % cols;
            uint8_t row = i / cols;
            int16_t bx = padX + col * (btnW + padX);
            int16_t by = startY + row * (btnH + padY);

            if (by + btnH > TYPE_BTN_Y) break;  // Don't overflow

            bool selected = (i == _selectedNameIdx);
            uint16_t bgColor = selected ? 0x2C9F : 0x2945;
            uint16_t txtColor = selected ? 0xFFFF : 0xB596;

            tft.fillRoundRect(bx, by, btnW, btnH, 6, bgColor);
            tft.setTextColor(txtColor);
            tft.setTextSize(2);
            // Center text in button
            int16_t textW = strlen(names[i]) * 12;
            tft.setCursor(bx + (btnW - textW) / 2, by + 10);
            tft.print(names[i]);
        }

        // Type a real name on the on-screen keyboard
        bool custom = (_selectedNameIdx == CUSTOM_NAME);
        tft.fillRoundRect(10, TYPE_BTN_Y, 300, 36, 6, custom ? 0x2C9F : 0x2945);
        tft.setTextColor(custom ? 0xFFFF : 0xB596); tft.setTextSize(2);
        tft.setCursor(22, TYPE_BTN_Y + 10);
        if (custom) tft.printf("Name: %s", _customName);
        else        tft.print("Type a name...");

        // Confirm button
        _drawButton(85, 430, 150, 40, "CONFIRM", 0x2E8B);
    }

    void _drawReview() {
        tft.setTextColor(0xFFFF);
        tft.setTextSize(2);
        tft.setCursor(30, 50);
        tft.print("Review Family:");

        int16_t yPos = 90;
        for (uint8_t i = 0; i < _memberCount; i++) {
            tft.fillRoundRect(20, yPos, 280, 45, 6, 0x2945);

            // Role icon
            tft.setTextColor(_members[i].role == Role::PARENT ? 0x2C9F : 0xFE60);
            tft.setTextSize(1);
            tft.setCursor(30, yPos + 5);
            tft.print(_members[i].role == Role::PARENT ? "PARENT" : "CHILD");

            // Name
            tft.setTextColor(0xFFFF);
            tft.setTextSize(2);
            tft.setCursor(30, yPos + 20);
            tft.print(_members[i].name);

            // UID preview
            tft.setTextColor(0x4A69);
            tft.setTextSize(1);
            char uidStr[24];
            _uidToStr(_members[i].rfidUid, _members[i].rfidUidLength, uidStr);
            tft.setCursor(200, yPos + 25);
            tft.print(uidStr);

            yPos += 55;
        }

        // Buttons
        _drawButton(20,  420, 130, 40, "RESTART", 0xF800);
        _drawButton(170, 420, 130, 40, "SAVE", 0x2E8B);
    }

    void _drawButton(int16_t x, int16_t y, int16_t w, int16_t h,
                     const char* label, uint16_t color) {
        tft.fillRoundRect(x, y, w, h, 8, color);
        tft.setTextColor(0xFFFF);
        tft.setTextSize(2);
        int16_t textW = strlen(label) * 12;
        tft.setCursor(x + (w - textW) / 2, y + (h - 16) / 2);
        tft.print(label);
    }

    // ─── Event handlers ──────────────────────────────────────────────────

    void handleScan(uint8_t* uid, uint8_t uidLen) {
        if (_step == Step::SCAN_PARENT || _step == Step::SCAN_CHILD) {
            // Check for duplicate
            for (uint8_t i = 0; i < _memberCount; i++) {
                if (RFIDManager::matchUid(uid, uidLen, _members[i])) {
                    audio.errorTone();
                    tft.setTextColor(0xF800);
                    tft.setTextSize(1);
                    tft.setCursor(60, 300);
                    tft.print("Tag already registered!");
                    return;
                }
            }

            // Store pending UID
            memcpy(_pendingUid, uid, uidLen);
            _pendingUidLen = uidLen;
            _selectedNameIdx = 0;
            _customName[0] = '\0';

            audio.scanBeep();

            // Advance to name picker
            if (_step == Step::SCAN_PARENT) {
                _step = Step::NAME_PARENT;
            } else {
                _step = Step::NAME_CHILD;
            }
            drawCurrentStep();
        }
    }

    void handleTouch(uint16_t tx, uint16_t ty) {
        switch (_step) {
            case Step::WELCOME:
                // WiFi button: y=452..476
                if (ty >= 450 && tx >= 85 && tx <= 235) {
                    WifiSetup::run();
                    drawCurrentStep();
                    break;
                }
                // BEGIN button: y=400..445, x=85..235
                if (ty >= 400 && ty <= 445 && tx >= 85 && tx <= 235) {
                    audio.touchClick();
                    _step = Step::SCAN_PARENT;
                    _currentScanRole = Role::PARENT;
                    drawCurrentStep();
                }
                break;

            case Step::SCAN_PARENT:
            case Step::SCAN_CHILD:
                // SKIP button: y=420..455, x=85..235
                if (ty >= 420 && ty <= 455 && tx >= 85 && tx <= 235) {
                    audio.touchClick();
                    _advanceAfterSkip();
                }
                break;

            case Step::NAME_PARENT:
            case Step::NAME_CHILD:
            {
                // Name grid selection
                uint8_t cols = 2;
                int16_t btnW = 140, btnH = 38, padX = 10, padY = 6;
                int16_t startY = 100;
                uint8_t count = (_step == Step::NAME_PARENT)
                                ? PARENT_NAME_COUNT : CHILD_NAME_COUNT;

                for (uint8_t i = 0; i < count; i++) {
                    uint8_t col = i % cols;
                    uint8_t row = i / cols;
                    int16_t bx = padX + col * (btnW + padX);
                    int16_t by = startY + row * (btnH + padY);

                    if (tx >= bx && tx <= bx + btnW && ty >= by && ty <= by + btnH) {
                        audio.touchClick();
                        _selectedNameIdx = i;
                        drawCurrentStep();
                        return;
                    }
                }

                // Type a name
                if (ty >= TYPE_BTN_Y && ty <= TYPE_BTN_Y + 36) {
                    audio.touchClick();
                    char buf[NAME_LENGTH];
                    strlcpy(buf, _customName, sizeof(buf));
                    if (OnScreenKeyboard::run("Name", buf, sizeof(buf), NAME_LENGTH - 1, true) && buf[0]) {
                        strlcpy(_customName, buf, sizeof(_customName));
                        _selectedNameIdx = CUSTOM_NAME;
                    }
                    drawCurrentStep();
                    return;
                }

                // CONFIRM button: y=430..470, x=85..235
                if (ty >= 430 && ty <= 470 && tx >= 85 && tx <= 235) {
                    audio.taskCompleteTone();
                    _registerMember();
                }
                break;
            }

            case Step::REVIEW:
                // RESTART: y=420..460, x=20..150
                if (ty >= 420 && ty <= 460 && tx >= 20 && tx <= 150) {
                    audio.touchClick();
                    begin();  // Full restart
                    return;
                }
                // SAVE: y=420..460, x=170..300
                if (ty >= 420 && ty <= 460 && tx >= 170 && tx <= 300) {
                    audio.taskCompleteTone();
                    _saveAndFinish();
                }
                break;

            default:
                break;
        }
    }

    void _registerMember() {
        FamilyMember& m = _members[_memberCount];
        m.id = _memberCount;
        m.role = _currentScanRole;
        m.dailyGoal = (_currentScanRole == Role::CHILD) ? 5 : 3;
        m.totalPoints = 0;
        m.currentStreak = 0;
        m.dailyTasksCompleted = 0;
        m.remindedToday = false;

        memcpy(m.rfidUid, _pendingUid, _pendingUidLen);
        m.rfidUidLength = _pendingUidLen;

        const char** names = (_currentScanRole == Role::PARENT)
                             ? _parentNames : _childNames;
        strlcpy(m.name, _selectedNameIdx == CUSTOM_NAME ? _customName : names[_selectedNameIdx],
                NAME_LENGTH);

        _memberCount++;
        Serial.printf("[SETUP] Registered: %s (%s) UID length=%d\n",
                      m.name,
                      m.role == Role::PARENT ? "parent" : "child",
                      m.rfidUidLength);

        _advanceStep();
    }

    void _advanceStep() {
        if (_currentScanRole == Role::PARENT) {
            _parentsDone++;
            if (_parentsDone < 2) {
                _step = Step::SCAN_PARENT;  // Scan 2nd parent
            } else {
                _currentScanRole = Role::CHILD;
                _step = Step::SCAN_CHILD;   // Move to children
            }
        } else {
            _childrenDone++;
            if (_childrenDone < 2) {
                _step = Step::SCAN_CHILD;   // Scan 2nd child
            } else {
                _step = Step::REVIEW;       // All done
            }
        }
        drawCurrentStep();
    }

    void _advanceAfterSkip() {
        if (_currentScanRole == Role::PARENT) {
            _currentScanRole = Role::CHILD;
            _step = Step::SCAN_CHILD;
        } else {
            _step = Step::REVIEW;
        }
        drawCurrentStep();
    }

    void _saveAndFinish() {
        // Set default daily goals
        for (uint8_t i = 0; i < _memberCount; i++) {
            _members[i].dailyGoal = (_members[i].role == Role::CHILD) ? 5 : 3;
        }

        if (!storage.saveFamily(_members, _memberCount)) {
            // Restarting now would just loop back into the wizard
            audio.errorTone();
            tft.fillRect(0, 380, 320, 30, 0x1082);
            tft.setTextColor(0xF800); tft.setTextSize(1);
            tft.setCursor(20, 390); tft.print("Could not save to the SD card - check it, then SAVE.");
            return;
        }
        Serial.printf("[SETUP] Saved %d family members to SD.\n", _memberCount);

        _step = Step::COMPLETE;

        // Show completion screen briefly
        tft.fillScreen(0x1082);
        tft.setTextColor(0x2E8B);
        tft.setTextSize(3);
        tft.setCursor(50, 180);
        tft.print("All set!");
        tft.setTextColor(0xB596);
        tft.setTextSize(2);
        tft.setCursor(40, 240);
        tft.print("Restarting...");

        delay(2000);
        ESP.restart();
    }

    void _uidToStr(const uint8_t* uid, uint8_t len, char* out) {
        out[0] = '\0';
        for (uint8_t i = 0; i < len && i < 4; i++) {
            char seg[4];
            if (i > 0) strcat(out, ":");
            snprintf(seg, sizeof(seg), "%02X", uid[i]);
            strcat(out, seg);
        }
    }
};
