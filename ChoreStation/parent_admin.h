// =============================================================================
// parent_admin.h — Parent Administration & Task Configuration
// =============================================================================
// Accessed when a parent taps the gear icon on their dashboard.
//
// TASK CREATION WIZARD (step-by-step, large tap targets):
//   1. Type:     Recurring / One-time
//   2. Category: 6 category buttons with icons
//   3. Name:     Preset names per category (+ custom via letter picker)
//   4. Size:     Small / Medium / Large with point preview
//   5. Assign:   Multi-select members, "All Children", or "Anyone"
//   6. Schedule: Day picker + time (recurring only)
//   7. Options:  Min time, timeout, approval, own mess
//   8. Review:   Summary + save
//
// On save, if multiple assignees are selected, the system creates one
// independent task instance per assignee (each gets their own timer,
// completion state, and claim tracking).
//
// Also provides:
//   - Task list management (edit/delete)
//   - Family settings (daily goals per member)
//   - System info (SD health, WiFi, backups)
// =============================================================================

#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "data_model.h"
#include "icons.h"
#include "audio_feedback.h"
#include "storage_manager.h"
#include "backup_manager.h"
#include "time_manager.h"
#include "preset_names.h"
#include "keyboard.h"
#include "wifi_setup.h"

extern TFT_eSPI       tft;
extern AudioFeedback  audio;
extern StorageManager storage;
extern BackupManager  backups;
extern TimeManager    timeMan;
extern FamilyMember   family[];
extern uint8_t        familyCount;
extern Task           activeTasks[];
extern uint8_t        activeTaskCount;

// Color constants (must match main.cpp palette)
#define CA_BG     0x1082
#define CA_CARD   0x2945
#define CA_PRI    0x2C9F
#define CA_OK     0x2E8B
#define CA_WARN   0xFDA0
#define CA_ERR    0xF800
#define CA_GOLD   0xFE60
#define CA_TXT    0xFFFF
#define CA_TXT2   0xB596
#define CA_TXT3   0x7BCF
#define CA_DIM    0x4A69

// Defined in ChoreStation.ino: re-reads /tasks/recurring.json into the
// scheduler so template edits apply today, not after the next reboot.
void reloadRecurringTemplates();

static const uint16_t CA_CAT_COLORS[] = {
    0x2C9F, 0x2E8B, 0xFDA0, 0x7A1F, 0xFBE0, 0x3666
};

class ParentAdmin {
public:
    // Set when the parent taps "Re-pair tags"; the main sketch owns that
    // screen (it needs the RFID poll loop), so it checks and clears this.
    bool wantsPairTags = false;

    enum class Menu : uint8_t { MAIN, CREATE_TASK, MANAGE_TASKS, FAMILY_SETTINGS, SYSTEM_INFO };
    enum class WizStep : uint8_t { TYPE, CATEGORY, NAME, SIZE, ASSIGN, SCHEDULE, OPTIONS, REVIEW };

    void begin() {
        _menu = Menu::MAIN;
        _wizStep = WizStep::TYPE;
        _resetDraft();
        _editIdx = -1;
        _scroll = 0;
        _confirmDel = -1;
    }

    void drawCurrentScreen() {
        switch (_menu) {
            case Menu::MAIN:            _drawMain(); break;
            case Menu::CREATE_TASK:     _drawWizard(); break;
            case Menu::MANAGE_TASKS:    _drawTaskList(); break;
            case Menu::FAMILY_SETTINGS: _drawSettings(); break;
            case Menu::SYSTEM_INFO:     _drawSysInfo(); break;
        }
    }

    // Returns true if admin should close (back to dashboard)
    bool handleTouch(uint16_t x, uint16_t y) {
        switch (_menu) {
            case Menu::MAIN:            return _touchMain(x, y);
            case Menu::CREATE_TASK:     return _touchWizard(x, y);
            case Menu::MANAGE_TASKS:    return _touchTaskList(x, y);
            case Menu::FAMILY_SETTINGS: return _touchSettings(x, y);
            case Menu::SYSTEM_INFO:     return _touchSysInfo(x, y);
        }
        return false;
    }

private:
    Menu     _menu;
    WizStep  _wizStep;
    Task     _draft;
    int8_t   _editIdx;        // -1 = new, >=0 = editing
    uint8_t  _selName;        // Preset name index (NAMES_PER_CAT = keep _origName)
    uint8_t  _scroll;         // List scroll offset
    int8_t   _confirmDel;     // Task index awaiting a second tap to delete
    char     _origName[48];   // Name of the task being edited (may be custom)
    bool     _hasOrigName;
    int16_t  _sysBtnY;        // System Info button rows (set when drawn)

    // ── Multi-assign state ───────────────────────────────────────────────
    // Bitmask: bit N = family member N is selected.
    // Special values checked separately:
    //   _assignAll  = true → create for every child
    //   _assignAny  = true → unassigned (255), anyone can claim
    uint8_t  _assignMask;     // Per-member selection bitmask
    bool     _assignAll;      // "All Children" shortcut
    bool     _assignAny;      // "Anyone" (unassigned)

    // ── Time picker ──────────────────────────────────────────────────────
    uint8_t  _pickHour;
    uint8_t  _pickMin;

    // ── Preset task names per category ───────────────────────────────────

    static constexpr uint8_t NAMES_PER_CAT = 6;

    // Preset names imported from preset_names.h as presetTaskName()

    // ── Helpers ──────────────────────────────────────────────────────────

    void _resetDraft() {
        memset(&_draft, 0, sizeof(Task));
        _draft.type = TaskType::RECURRING;
        _draft.category = TaskCategory::CLEANING_TIDYING;
        _draft.size = TaskSize::SMALL;
        _draft.state = TaskState::AVAILABLE;
        _draft.assignedTo = 255;
        _draft.recurDays = 127;
        _draft.recurHour = 8;
        _selName = 0;
        _assignMask = 0;
        _assignAll = false;
        _assignAny = false;
        _pickHour = 8;
        _pickMin = 0;
        _origName[0] = '\0';
        _hasOrigName = false;
    }

    // Name the draft will be saved with: a preset, or the edited task's own
    // (possibly custom, web-entered) name.
    const char* _draftName() {
        if (_hasOrigName && _selName >= NAMES_PER_CAT) return _origName;
        return presetTaskName(static_cast<uint8_t>(_draft.category), _selName);
    }

    // Task ids must be unique across active tasks AND recurring templates, or
    // the scheduler can't tell an instance from its template and duplicates it.
    uint16_t _newTaskId(const Task* recur, uint8_t recurCount) {
        uint16_t id = 3000 + (millis() % 20000);
        for (uint16_t tries = 0; tries < 60000; tries++, id++) {
            if (id >= 60000) id = 3000;   // 60000+ is reserved for routine logs
            bool used = false;
            for (uint8_t i = 0; i < activeTaskCount && !used; i++) used = activeTasks[i].id == id;
            for (uint8_t i = 0; i < recurCount && !used; i++) used = recur[i].id == id;
            if (!used) return id;
        }
        return id;
    }

    void _hdr(const char* t, bool back = true) {
        tft.fillRect(0, 0, 320, 48, CA_CARD);
        if (back) Icons::drawBackArrow(8, 13, CA_TXT2, 22);
        tft.setTextColor(CA_TXT); tft.setTextSize(2);
        tft.setCursor(back ? 38 : 12, 14); tft.print(t);
    }

    void _btn(int16_t x, int16_t y, int16_t w, int16_t h,
              const char* lbl, uint16_t bg, uint16_t fg = CA_TXT, uint8_t sz = 2) {
        tft.fillRoundRect(x, y, w, h, 6, bg);
        tft.setTextColor(fg); tft.setTextSize(sz);
        int16_t tw = strlen(lbl) * (sz == 2 ? 12 : 6);
        tft.setCursor(x + (w - tw) / 2, y + (h - (sz == 2 ? 16 : 8)) / 2);
        tft.print(lbl);
    }

    bool _hit(uint16_t tx, uint16_t ty, int16_t x, int16_t y, int16_t w, int16_t h) {
        return tx >= x && tx <= x + w && ty >= y && ty <= y + h;
    }

    void _toggle(int16_t x, int16_t y, const char* lbl, bool on) {
        tft.fillRoundRect(x, y, 290, 38, 6, CA_CARD);
        int16_t tx = x + 10, ty = y + 7;
        tft.fillRoundRect(tx, ty, 44, 24, 12, on ? CA_OK : CA_DIM);
        tft.fillCircle(on ? tx + 34 : tx + 10, ty + 12, 9, on ? CA_TXT : CA_TXT2);
        tft.setTextColor(on ? CA_OK : CA_TXT3); tft.setTextSize(1);
        tft.setCursor(x + 65, y + 14); tft.print(lbl);
    }

    // Count selected assignees from mask
    uint8_t _countSelected() {
        if (_assignAny) return 1;
        if (_assignAll) {
            uint8_t c = 0;
            for (uint8_t i = 0; i < familyCount; i++)
                if (family[i].role == Role::CHILD) c++;
            return c;
        }
        uint8_t c = 0;
        for (uint8_t i = 0; i < familyCount; i++)
            if (_assignMask & (1 << i)) c++;
        return c;
    }

    // ═════════════════════════════════════════════════════════════════════
    // MAIN MENU
    // ═════════════════════════════════════════════════════════════════════

    void _drawMain() {
        tft.fillScreen(CA_BG);
        _hdr("Admin", true);
        int16_t y = 62, h = 52, g = 8;
        _btn(15, y, 290, h, "Create New Task", CA_PRI);     y += h + g;
        _btn(15, y, 290, h, "Manage Tasks", CA_CARD);       y += h + g;
        _btn(15, y, 290, h, "Family Settings", CA_CARD);    y += h + g;
        _btn(15, y, 290, h, "System Info", CA_CARD);

        tft.setTextColor(CA_DIM); tft.setTextSize(1);
        tft.setCursor(15, 450);
        tft.printf("%d active tasks  |  %d members", activeTaskCount, familyCount);
    }

    bool _touchMain(uint16_t x, uint16_t y) {
        audio.touchClick();
        if (x < 50 && y < 48) return true;  // Back = exit admin
        int16_t by = 62, h = 52, g = 8;
        if (_hit(x,y,15,by,290,h)) { _menu=Menu::CREATE_TASK; _wizStep=WizStep::TYPE; _resetDraft(); _editIdx=-1; _drawWizard(); return false; } by+=h+g;
        if (_hit(x,y,15,by,290,h)) { _menu=Menu::MANAGE_TASKS; _scroll=0; _drawTaskList(); return false; } by+=h+g;
        if (_hit(x,y,15,by,290,h)) { _menu=Menu::FAMILY_SETTINGS; _drawSettings(); return false; } by+=h+g;
        if (_hit(x,y,15,by,290,h)) { _menu=Menu::SYSTEM_INFO; _drawSysInfo(); return false; }
        return false;
    }

    // ═════════════════════════════════════════════════════════════════════
    // TASK WIZARD
    // ═════════════════════════════════════════════════════════════════════

    void _drawWizard() {
        tft.fillScreen(CA_BG);
        // Step dots
        uint8_t total = (_draft.type == TaskType::RECURRING) ? 8 : 7;
        uint8_t cur = static_cast<uint8_t>(_wizStep);
        int16_t dx = (320 - total * 16) / 2;
        for (uint8_t i = 0; i < total; i++) {
            uint16_t c = (i < cur) ? CA_PRI : (i == cur) ? CA_TXT : CA_DIM;
            tft.fillCircle(dx + i * 16, 10, (i == cur) ? 4 : 3, c);
        }
        switch (_wizStep) {
            case WizStep::TYPE:     _wType(); break;
            case WizStep::CATEGORY: _wCat(); break;
            case WizStep::NAME:     _wName(); break;
            case WizStep::SIZE:     _wSize(); break;
            case WizStep::ASSIGN:   _wAssign(); break;
            case WizStep::SCHEDULE: _wSched(); break;
            case WizStep::OPTIONS:  _wOpts(); break;
            case WizStep::REVIEW:   _wReview(); break;
        }
    }

    void _wType() {
        _hdr("Task Type");
        bool isRec = (_draft.type == TaskType::RECURRING);
        tft.fillRoundRect(15, 80, 290, 60, 8, isRec ? CA_PRI : CA_CARD);
        tft.setTextColor(CA_TXT); tft.setTextSize(2);
        tft.setCursor(25, 90); tft.print("Recurring");
        tft.setTextColor(isRec ? CA_TXT : CA_TXT3); tft.setTextSize(1);
        tft.setCursor(25, 118); tft.print("Repeats on selected days");

        tft.fillRoundRect(15, 155, 290, 60, 8, !isRec ? CA_PRI : CA_CARD);
        tft.setTextColor(CA_TXT); tft.setTextSize(2);
        tft.setCursor(25, 165); tft.print("One-time");
        tft.setTextColor(!isRec ? CA_TXT : CA_TXT3); tft.setTextSize(1);
        tft.setCursor(25, 193); tft.print("Do once, then done");
    }

    void _wCat() {
        _hdr("Category");
        const char* cn[] = {"Cleaning","Health","Family","Academic","Pet Care","Outdoor"};
        for (uint8_t i = 0; i < 6; i++) {
            int16_t y = 55 + i * 46;
            bool sel = (static_cast<uint8_t>(_draft.category) == i);
            tft.fillRoundRect(15, y, 290, 40, 6, sel ? CA_CAT_COLORS[i] : CA_CARD);
            Icons::drawCategoryIcon(22, y + 4, static_cast<TaskCategory>(i),
                                    sel ? CA_TXT : CA_CAT_COLORS[i], 20);
            tft.setTextColor(sel ? CA_TXT : CA_TXT2); tft.setTextSize(2);
            tft.setCursor(50, y + 10); tft.print(cn[i]);
        }
        _btn(210, 445, 100, 28, "NEXT >", CA_PRI, CA_TXT, 1);
    }

    void _wName() {
        _hdr("Task Name");
        uint8_t ci = static_cast<uint8_t>(_draft.category);
        for (uint8_t i = 0; i < NAMES_PER_CAT; i++) {
            int16_t y = 55 + i * 42;
            bool sel = (i == _selName);
            _btn(15, y, 290, 36, presetTaskName(ci, i), sel ? CA_PRI : CA_CARD);
        }
        // A custom name (typed here, or the edited task's own web-entered name)
        if (_hasOrigName) {
            int16_t y = 55 + NAMES_PER_CAT * 42;
            bool sel = (_selName >= NAMES_PER_CAT);
            _btn(15, y, 290, 36, _origName, sel ? CA_GOLD : CA_CARD, sel ? CA_BG : CA_TXT, 1);
        }
        _btn(15, 55 + (NAMES_PER_CAT + 1) * 42, 290, 36, "Type a custom name...", CA_DIM, CA_TXT, 1);
        _btn(210, 445, 100, 28, "NEXT >", CA_PRI, CA_TXT, 1);
    }

    void _wSize() {
        _hdr("Task Size");
        const char* sn[] = {"Small","Medium","Large"};
        const char* sd[] = {"5-15 min, quick","15-45 min, moderate","45+ min, big job"};
        uint8_t pts[] = {PointTable::base(TaskSize::SMALL),
                         PointTable::base(TaskSize::MEDIUM),
                         PointTable::base(TaskSize::LARGE)};
        for (uint8_t i = 0; i < 3; i++) {
            int16_t y = 58 + i * 72;
            bool sel = (static_cast<uint8_t>(_draft.size) == i);
            tft.fillRoundRect(15, y, 290, 64, 8, sel ? CA_PRI : CA_CARD);
            tft.setTextColor(sel ? CA_TXT : CA_TXT2); tft.setTextSize(3);
            tft.setCursor(25, y + 6); tft.print(sn[i]);
            tft.setTextColor(sel ? CA_TXT : CA_TXT3); tft.setTextSize(1);
            tft.setCursor(25, y + 40); tft.print(sd[i]);
            tft.setTextColor(sel ? CA_TXT : CA_GOLD); tft.setTextSize(2);
            tft.setCursor(255, y + 18); tft.printf("%dpt", pts[i]);
        }
        _btn(210, 445, 100, 28, "NEXT >", CA_PRI, CA_TXT, 1);
    }

    // ── Step 5: Multi-Assign ──

    void _wAssign() {
        _hdr("Assign To");
        tft.setTextColor(CA_TXT3); tft.setTextSize(1);
        tft.setCursor(15, 55); tft.print("Select one or more. Task duplicated per person.");

        int16_t y = 72;

        // "All Children" shortcut
        bool allSel = _assignAll;
        tft.fillRoundRect(15, y, 290, 42, 6, allSel ? CA_PRI : CA_CARD);
        _drawCheckbox(22, y + 11, allSel);
        tft.setTextColor(allSel ? CA_TXT : CA_TXT2); tft.setTextSize(2);
        tft.setCursor(48, y + 10); tft.print("All Children");
        tft.setTextColor(allSel ? CA_TXT : CA_TXT3); tft.setTextSize(1);
        uint8_t kidCount = 0;
        for (uint8_t i = 0; i < familyCount; i++) if (family[i].role == Role::CHILD) kidCount++;
        tft.setCursor(48, y + 30);
        tft.printf("Creates %d tasks (one per child)", kidCount);
        y += 50;

        // Individual members
        for (uint8_t i = 0; i < familyCount; i++) {
            bool sel = (_assignMask & (1 << i)) && !_assignAll && !_assignAny;
            bool dimmed = _assignAll || _assignAny;  // Dim when a shortcut is active
            tft.fillRoundRect(15, y, 290, 38, 6, sel ? CA_PRI : CA_CARD);
            _drawCheckbox(22, y + 9, sel || (_assignAll && family[i].role == Role::CHILD));
            tft.setTextColor(dimmed && !sel ? CA_DIM : (sel ? CA_TXT : CA_TXT2));
            tft.setTextSize(2);
            tft.setCursor(48, y + 9); tft.print(family[i].name);
            tft.setTextColor(dimmed ? CA_DIM : CA_TXT3); tft.setTextSize(1);
            tft.setCursor(200, y + 14);
            tft.print(family[i].role == Role::PARENT ? "Parent" : "Child");
            y += 42;
        }

        // "Anyone" option
        y += 4;
        bool anySel = _assignAny;
        tft.fillRoundRect(15, y, 290, 38, 6, anySel ? CA_PRI : CA_CARD);
        _drawCheckbox(22, y + 9, anySel);
        tft.setTextColor(anySel ? CA_TXT : CA_TXT2); tft.setTextSize(2);
        tft.setCursor(48, y + 9); tft.print("Anyone");
        tft.setTextColor(anySel ? CA_TXT : CA_TXT3); tft.setTextSize(1);
        // (no subtext needed, keeps layout clean)

        // Selection summary
        uint8_t cnt = _countSelected();
        tft.setTextColor(cnt > 0 ? CA_OK : CA_WARN); tft.setTextSize(1);
        tft.setCursor(15, 440);
        if (cnt == 0) tft.print("Select at least one assignee");
        else tft.printf("%d task%s will be created", cnt, cnt > 1 ? "s" : "");

        if (cnt > 0) _btn(210, 445, 100, 28, "NEXT >", CA_PRI, CA_TXT, 1);
    }

    void _drawCheckbox(int16_t x, int16_t y, bool checked) {
        tft.drawRoundRect(x, y, 18, 18, 3, checked ? CA_OK : CA_DIM);
        if (checked) {
            tft.drawLine(x + 4, y + 9, x + 7, y + 13, CA_TXT);
            tft.drawLine(x + 7, y + 13, x + 14, y + 5, CA_TXT);
            tft.drawLine(x + 4, y + 10, x + 7, y + 14, CA_TXT);
            tft.drawLine(x + 7, y + 14, x + 14, y + 6, CA_TXT);
        }
    }

    void _wSched() {
        _hdr("Schedule");
        tft.setTextColor(CA_TXT3); tft.setTextSize(1);
        tft.setCursor(15, 55); tft.print("Which days?");

        const char* dl[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
        for (uint8_t i = 0; i < 7; i++) {
            int16_t dx = 11 + i * 43;
            bool on = _draft.recurDays & (1 << i);
            tft.fillRoundRect(dx, 72, 38, 38, 6, on ? CA_PRI : CA_CARD);
            tft.setTextColor(on ? CA_TXT : CA_TXT3); tft.setTextSize(2);
            tft.setCursor(dx + 5, 79); tft.print(dl[i]);
        }

        // Presets
        _btn(15, 120, 90, 26, "Every day", CA_BG, CA_TXT3, 1);
        _btn(115, 120, 90, 26, "Weekdays", CA_BG, CA_TXT3, 1);
        _btn(215, 120, 90, 26, "Weekend", CA_BG, CA_TXT3, 1);

        // Time picker
        tft.setTextColor(CA_TXT3); tft.setTextSize(1);
        tft.setCursor(15, 165); tft.print("Task appears at:");

        // Hour column
        _btn(30, 185, 50, 28, "+", CA_CARD, CA_TXT3, 1);
        tft.fillRoundRect(30, 218, 50, 35, 4, CA_CARD);
        tft.setTextColor(CA_TXT); tft.setTextSize(3);
        tft.setCursor(35, 222); tft.printf("%02d", _pickHour);
        _btn(30, 258, 50, 28, "-", CA_CARD, CA_TXT3, 1);

        tft.setTextColor(CA_TXT); tft.setTextSize(3);
        tft.setCursor(88, 222); tft.print(":");

        // Minute column
        _btn(105, 185, 50, 28, "+", CA_CARD, CA_TXT3, 1);
        tft.fillRoundRect(105, 218, 50, 35, 4, CA_CARD);
        tft.setTextColor(CA_TXT); tft.setTextSize(3);
        tft.setCursor(110, 222); tft.printf("%02d", _pickMin);
        _btn(105, 258, 50, 28, "-", CA_CARD, CA_TXT3, 1);

        tft.setTextColor(CA_TXT3); tft.setTextSize(2);
        tft.setCursor(175, 228); tft.print(_pickHour < 12 ? "AM" : "PM");

        _btn(210, 445, 100, 28, "NEXT >", CA_PRI, CA_TXT, 1);
    }

    void _wOpts() {
        _hdr("Options");
        int16_t y = 55;

        // Min time
        tft.setTextColor(CA_TXT3); tft.setTextSize(1);
        tft.setCursor(15, y); tft.print("MINIMUM TIME (0 = none)"); y += 14;
        tft.fillRoundRect(15, y, 290, 40, 6, CA_CARD);
        _btn(20, y + 5, 45, 28, "-30s", CA_BG, CA_TXT3, 1);
        tft.setTextColor(CA_TXT); tft.setTextSize(2);
        char mtBuf[16];
        if (_draft.minimumTimeSec >= 60)
            snprintf(mtBuf, sizeof(mtBuf), "%dm %ds", _draft.minimumTimeSec / 60, _draft.minimumTimeSec % 60);
        else
            snprintf(mtBuf, sizeof(mtBuf), "%ds", _draft.minimumTimeSec);
        tft.setCursor(110, y + 10); tft.print(mtBuf);
        _btn(250, y + 5, 45, 28, "+30s", CA_BG, CA_TXT3, 1);
        y += 50;

        // Timeout
        tft.setTextColor(CA_TXT3); tft.setTextSize(1);
        tft.setCursor(15, y); tft.print("TIMEOUT (0 = none)"); y += 14;
        tft.fillRoundRect(15, y, 290, 40, 6, CA_CARD);
        _btn(20, y + 5, 45, 28, "-5m", CA_BG, CA_TXT3, 1);
        tft.setTextColor(CA_TXT); tft.setTextSize(2);
        char toBuf[16];
        if (_draft.timeoutTimeSec >= 3600)
            snprintf(toBuf, sizeof(toBuf), "%dh %dm", _draft.timeoutTimeSec / 3600, (_draft.timeoutTimeSec % 3600) / 60);
        else if (_draft.timeoutTimeSec >= 60)
            snprintf(toBuf, sizeof(toBuf), "%dm", _draft.timeoutTimeSec / 60);
        else
            snprintf(toBuf, sizeof(toBuf), "%ds", _draft.timeoutTimeSec);
        tft.setCursor(110, y + 10); tft.print(toBuf);
        _btn(250, y + 5, 45, 28, "+5m", CA_BG, CA_TXT3, 1);
        y += 55;

        // Toggles
        _toggle(15, y, "Requires parent approval", _draft.requiresApproval); y += 46;
        _toggle(15, y, "Own mess (reduced points)", _draft.isOwnMess);

        _btn(210, 445, 100, 28, "NEXT >", CA_PRI, CA_TXT, 1);
    }

    void _wReview() {
        _hdr(_editIdx >= 0 ? "Review Edit" : "Review & Create");
        uint8_t ci = static_cast<uint8_t>(_draft.category);
        const char* cn[] = {"Cleaning","Health","Family","Academic","Pet Care","Outdoor"};
        const char* sn[] = {"Small","Medium","Large"};

        int16_t y = 52;

        // Name
        tft.setTextColor(CA_TXT); tft.setTextSize(2);
        tft.setCursor(15, y); tft.print(_draftName()); y += 24;

        // Info rows
        auto row = [&](const char* label, const char* val, uint16_t vc = CA_TXT2) {
            tft.fillRoundRect(15, y, 290, 24, 3, CA_CARD);
            tft.setTextColor(CA_TXT3); tft.setTextSize(1);
            tft.setCursor(22, y + 7); tft.print(label);
            tft.setTextColor(vc);
            tft.setCursor(140, y + 7); tft.print(val);
            y += 27;
        };

        row("Category", cn[ci], CA_CAT_COLORS[ci]);
        row("Size", sn[static_cast<uint8_t>(_draft.size)]);
        uint8_t pts = _draft.isOwnMess ? PointTable::ownMess(_draft.size) : PointTable::base(_draft.size);
        char pb[8]; snprintf(pb, sizeof(pb), "%d pts", pts);
        row("Points", pb, CA_GOLD);
        row("Type", _draft.type == TaskType::RECURRING ? "Recurring" : "One-time");

        // Assignees
        char assignBuf[64] = "";
        uint8_t taskCount = 0;
        if (_assignAny) {
            strlcpy(assignBuf, "Anyone", sizeof(assignBuf));
            taskCount = 1;
        } else if (_assignAll) {
            strlcpy(assignBuf, "All children", sizeof(assignBuf));
            for (uint8_t i = 0; i < familyCount; i++)
                if (family[i].role == Role::CHILD) taskCount++;
        } else {
            for (uint8_t i = 0; i < familyCount; i++) {
                if (_assignMask & (1 << i)) {
                    if (strlen(assignBuf) > 0) strcat(assignBuf, ", ");
                    strcat(assignBuf, family[i].name);
                    taskCount++;
                }
            }
        }
        row("Assign", assignBuf);
        if (taskCount > 1) {
            char tb[16]; snprintf(tb, sizeof(tb), "%d copies", taskCount);
            row("Creates", tb, CA_WARN);
        }

        if (_draft.type == TaskType::RECURRING) {
            char sb[40] = "";
            const char* dn[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
            for (uint8_t i = 0; i < 7; i++) {
                if (_draft.recurDays & (1 << i)) {
                    if (strlen(sb) > 0) strcat(sb, " ");
                    strcat(sb, dn[i]);
                }
            }
            char tb[8]; snprintf(tb, sizeof(tb), " %d:%02d", _draft.recurHour, _draft.recurMinute);
            strcat(sb, tb);
            row("Schedule", sb);
        }

        if (_draft.minimumTimeSec > 0) {
            char mb[12]; snprintf(mb, sizeof(mb), "%ds", _draft.minimumTimeSec);
            row("Min time", mb);
        }
        row("Approval", _draft.requiresApproval ? "Yes" : "No");

        _btn(15, 440, 120, 32, "< BACK", CA_DIM);
        _btn(185, 440, 120, 32, "SAVE", CA_OK);
    }

    // ── Wizard touch ─────────────────────────────────────────────────────

    bool _touchWizard(uint16_t x, uint16_t y) {
        audio.touchClick();

        // Back (top-left, all steps)
        if (x < 50 && y < 48) {
            if (_wizStep == WizStep::TYPE) { _menu = Menu::MAIN; _drawMain(); }
            else {
                _wizStep = static_cast<WizStep>(static_cast<uint8_t>(_wizStep) - 1);
                if (_wizStep == WizStep::SCHEDULE && _draft.type != TaskType::RECURRING)
                    _wizStep = WizStep::ASSIGN;
                _drawWizard();
            }
            return false;
        }

        bool next = _hit(x, y, 210, 445, 100, 28);

        switch (_wizStep) {
        case WizStep::TYPE:
            if (_hit(x,y,15,80,290,60))  { _draft.type = TaskType::RECURRING; _wizStep = WizStep::CATEGORY; }
            if (_hit(x,y,15,155,290,60)) { _draft.type = TaskType::ADHOC; _wizStep = WizStep::CATEGORY; }
            _drawWizard(); break;

        case WizStep::CATEGORY:
            for (uint8_t i=0;i<6;i++)
                if (_hit(x,y,15,55+i*46,290,40)) { _draft.category=static_cast<TaskCategory>(i); _selName=0; _drawWizard(); return false; }
            if (next) { _wizStep=WizStep::NAME; _drawWizard(); } break;

        case WizStep::NAME:
            for (uint8_t i=0;i<NAMES_PER_CAT;i++)
                if (_hit(x,y,15,55+i*42,290,36)) { _selName=i; _drawWizard(); return false; }
            if (_hasOrigName && _hit(x,y,15,55+NAMES_PER_CAT*42,290,36)) { _selName=NAMES_PER_CAT; _drawWizard(); return false; }
            if (_hit(x,y,15,55+(NAMES_PER_CAT+1)*42,290,36)) {
                char buf[sizeof(_origName)];
                strlcpy(buf, _draftName(), sizeof(buf));
                if (OnScreenKeyboard::run("Task name", buf, sizeof(buf), 40, true) && buf[0]) {
                    strlcpy(_origName, buf, sizeof(_origName));
                    _hasOrigName = true;
                    _selName = NAMES_PER_CAT;
                }
                _drawWizard();
                return false;
            }
            if (next) { _wizStep=WizStep::SIZE; _drawWizard(); } break;

        case WizStep::SIZE:
            for (uint8_t i=0;i<3;i++)
                if (_hit(x,y,15,58+i*72,290,64)) { _draft.size=static_cast<TaskSize>(i); _drawWizard(); return false; }
            if (next) {
                _wizStep=WizStep::ASSIGN;
                if (_editIdx < 0) { _assignMask=0; _assignAll=false; _assignAny=false; }  // edits keep their assignee
                _drawWizard();
            } break;

        case WizStep::ASSIGN:
        {
            int16_t ay = 72;
            // "All Children"
            if (_hit(x,y,15,ay,290,42)) {
                _assignAll = !_assignAll;
                if (_assignAll) { _assignAny = false; _assignMask = 0; }
                _drawWizard(); return false;
            }
            ay += 50;
            // Individual members
            for (uint8_t i = 0; i < familyCount; i++) {
                if (_hit(x,y,15,ay,290,38)) {
                    _assignMask ^= (1 << i);
                    _assignAll = false; _assignAny = false;
                    _drawWizard(); return false;
                }
                ay += 42;
            }
            ay += 4;
            // "Anyone"
            if (_hit(x,y,15,ay,290,38)) {
                _assignAny = !_assignAny;
                if (_assignAny) { _assignAll = false; _assignMask = 0; }
                _drawWizard(); return false;
            }
            if (next && _countSelected() > 0) {
                _wizStep = (_draft.type == TaskType::RECURRING) ? WizStep::SCHEDULE : WizStep::OPTIONS;
                _drawWizard();
            }
            break;
        }

        case WizStep::SCHEDULE:
            // Day toggles
            for (uint8_t i=0;i<7;i++)
                if (_hit(x,y,11+i*43,72,38,38)) { _draft.recurDays^=(1<<i); _drawWizard(); return false; }
            // Presets
            if (_hit(x,y,15,120,90,26))  { _draft.recurDays=127; _drawWizard(); return false; }
            if (_hit(x,y,115,120,90,26)) { _draft.recurDays=62; _drawWizard(); return false; }
            if (_hit(x,y,215,120,90,26)) { _draft.recurDays=65; _drawWizard(); return false; }
            // Hour +/-
            if (_hit(x,y,30,185,50,28)) { _pickHour=(_pickHour+1)%24; _drawWizard(); return false; }
            if (_hit(x,y,30,258,50,28)) { _pickHour=(_pickHour+23)%24; _drawWizard(); return false; }
            // Min +/-
            if (_hit(x,y,105,185,50,28)) { _pickMin=(_pickMin+15)%60; _drawWizard(); return false; }
            if (_hit(x,y,105,258,50,28)) { _pickMin=(_pickMin+45)%60; _drawWizard(); return false; }
            if (next) { _draft.recurHour=_pickHour; _draft.recurMinute=_pickMin; _wizStep=WizStep::OPTIONS; _drawWizard(); }
            break;

        case WizStep::OPTIONS:
            // Min time -/+
            if (_hit(x,y,20,74,45,28) && _draft.minimumTimeSec>=30) { _draft.minimumTimeSec-=30; _drawWizard(); return false; }
            if (_hit(x,y,250,74,45,28)) { _draft.minimumTimeSec+=30; _drawWizard(); return false; }
            // Timeout -/+
            if (_hit(x,y,20,138,45,28) && _draft.timeoutTimeSec>=300) { _draft.timeoutTimeSec-=300; _drawWizard(); return false; }
            if (_hit(x,y,250,138,45,28)) { _draft.timeoutTimeSec+=300; _drawWizard(); return false; }
            // Toggles
            if (_hit(x,y,15,198,290,38)) { _draft.requiresApproval=!_draft.requiresApproval; _drawWizard(); return false; }
            if (_hit(x,y,15,244,290,38)) { _draft.isOwnMess=!_draft.isOwnMess; _drawWizard(); return false; }
            if (next) { _wizStep=WizStep::REVIEW; _drawWizard(); }
            break;

        case WizStep::REVIEW:
            if (_hit(x,y,15,440,120,32)) { _wizStep=WizStep::OPTIONS; _drawWizard(); return false; }
            if (_hit(x,y,185,440,120,32)) { _saveTask(); _menu=Menu::MAIN; _drawMain(); }
            break;
        }
        return false;
    }

    // ── Save: creates one task instance per selected assignee ─────────

    void _saveTask() {
        strlcpy(_draft.name, _draftName(), sizeof(_draft.name));

        // ── Editing an existing task: update in place, do NOT create a
        //    duplicate. Preserve the task's id and live state (an in-progress
        //    or pending task stays in progress/pending with its timer). ──
        if (_editIdx >= 0 && _editIdx < activeTaskCount) {
            Task& t = activeTasks[_editIdx];
            Task live = t;
            memcpy(&t, &_draft, sizeof(Task));
            t.id                     = live.id;
            t.state                  = live.state;
            t.startedAtMs            = live.startedAtMs;
            t.completedAtMs          = live.completedAtMs;
            t.startedEpoch           = live.startedEpoch;
            t.noReminderToggled      = live.noReminderToggled;
            t.childClaimedNoReminder = live.childClaimedNoReminder;
            t.parentOverrodeReminder = live.parentOverrodeReminder;
            t.noReminderApproved     = live.noReminderApproved;

            // Resolve to a single assignee from the selection.
            if (_assignAny) {
                t.assignedTo = 255;
            } else {
                for (uint8_t i = 0; i < familyCount; i++) {
                    if (_assignMask & (1 << i)) { t.assignedTo = i; break; }
                }
            }

            // Recurring task: also update the persisted template (match by id)
            // so the edit survives tomorrow's regeneration. A template shared
            // by all children (254) stays shared.
            if (t.type == TaskType::RECURRING) {
                Task recurTasks[MAX_ACTIVE_TASKS];
                uint8_t recurCount = 0;
                storage.loadTasks(recurTasks, recurCount, "/tasks/recurring.json");
                for (uint8_t i = 0; i < recurCount; i++) {
                    if (recurTasks[i].id == t.id) {
                        uint8_t keepAssign = recurTasks[i].assignedTo;
                        memcpy(&recurTasks[i], &t, sizeof(Task));
                        recurTasks[i].state = TaskState::AVAILABLE;
                        recurTasks[i].startedEpoch = 0;
                        recurTasks[i].noReminderToggled = false;
                        recurTasks[i].childClaimedNoReminder = false;
                        recurTasks[i].parentOverrodeReminder = false;
                        recurTasks[i].noReminderApproved = false;
                        if (keepAssign == 254) recurTasks[i].assignedTo = 254;
                        break;
                    }
                }
                storage.saveTasks(recurTasks, recurCount, "/tasks/recurring.json");
                reloadRecurringTemplates();
            }

            backups.markTasksDirty();
            audio.taskCompleteTone();
            Serial.printf("[ADMIN] Edited task '%s'\n", t.name);
            _editIdx = -1;
            return;
        }

        _draft.state = TaskState::AVAILABLE;
        _draft.noReminderToggled = false;
        _draft.childClaimedNoReminder = false;
        _draft.parentOverrodeReminder = false;
        _draft.noReminderApproved = false;
        _draft.startedEpoch = 0;

        // Build list of assignee IDs
        uint8_t assignees[MAX_FAMILY_MEMBERS];
        uint8_t assigneeCount = 0;

        if (_assignAny) {
            assignees[0] = 255;
            assigneeCount = 1;
        } else if (_assignAll) {
            for (uint8_t i = 0; i < familyCount; i++) {
                if (family[i].role == Role::CHILD)
                    assignees[assigneeCount++] = i;
            }
        } else {
            for (uint8_t i = 0; i < familyCount; i++) {
                if (_assignMask & (1 << i))
                    assignees[assigneeCount++] = i;
            }
        }

        if (assigneeCount == 0) {
            assignees[0] = 255;
            assigneeCount = 1;
        }

        bool recurring = (_draft.type == TaskType::RECURRING);
        Task recurTasks[MAX_ACTIVE_TASKS];
        uint8_t recurCount = 0;
        if (recurring) storage.loadTasks(recurTasks, recurCount, "/tasks/recurring.json");

        // Recurring "All children" = ONE template (254) the scheduler expands
        // per child; its instances share the template id.
        uint16_t sharedId = (recurring && _assignAll) ? _newTaskId(recurTasks, recurCount) : 0;

        uint8_t created = 0;
        for (uint8_t a = 0; a < assigneeCount; a++) {
            if (activeTaskCount >= MAX_ACTIVE_TASKS) {
                Serial.println("[ADMIN] Task list full!");
                break;
            }

            // Instance and its template MUST share an id, or the scheduler
            // won't recognise the instance and generates a duplicate.
            uint16_t id = sharedId ? sharedId : _newTaskId(recurTasks, recurCount);

            Task& newTask = activeTasks[activeTaskCount];
            memcpy(&newTask, &_draft, sizeof(Task));
            newTask.assignedTo = assignees[a];
            newTask.id = id;
            newTask.startedAtMs = 0;
            newTask.completedAtMs = 0;
            activeTaskCount++;
            created++;

            if (recurring && !sharedId && recurCount < MAX_ACTIVE_TASKS) {
                memcpy(&recurTasks[recurCount], &newTask, sizeof(Task));
                recurCount++;
            }

            Serial.printf("[ADMIN] Created '%s' for %s\n", newTask.name,
                          assignees[a] < familyCount ? family[assignees[a]].name : "anyone");
        }

        if (recurring && sharedId && recurCount < MAX_ACTIVE_TASKS) {
            memcpy(&recurTasks[recurCount], &_draft, sizeof(Task));
            recurTasks[recurCount].id = sharedId;
            recurTasks[recurCount].assignedTo = 254;
            recurCount++;
        }
        if (recurring) {
            storage.saveTasks(recurTasks, recurCount, "/tasks/recurring.json");
            reloadRecurringTemplates();
        }

        backups.markTasksDirty();
        audio.taskCompleteTone();
        Serial.printf("[ADMIN] Created %d task instance%s.\n", created, created > 1 ? "s" : "");
    }

    // ═════════════════════════════════════════════════════════════════════
    // TASK LIST MANAGEMENT
    // ═════════════════════════════════════════════════════════════════════

    void _drawTaskList() {
        tft.fillScreen(CA_BG);
        _hdr("Manage Tasks");
        if (activeTaskCount == 0) {
            tft.setTextColor(CA_TXT3); tft.setTextSize(2);
            tft.setCursor(60, 200); tft.print("No tasks yet.");
            return;
        }
        int16_t y = 52;
        for (uint8_t i = _scroll; i < activeTaskCount && (y + 40) < 455; i++) {
            const Task& t = activeTasks[i];
            tft.fillRoundRect(10, y, 300, 36, 5, CA_CARD);
            tft.fillRect(10, y, 4, 36, CA_CAT_COLORS[static_cast<uint8_t>(t.category)]);
            tft.setTextColor(CA_TXT2); tft.setTextSize(1);
            tft.setCursor(20, y + 5); tft.print(t.name);
            tft.setTextColor(CA_DIM); tft.setCursor(20, y + 20);
            if (t.assignedTo < familyCount) tft.print(family[t.assignedTo].name);
            else tft.print("Anyone");
            // Delete X — first tap arms it, second tap deletes
            if (_confirmDel == (int8_t)i) {
                tft.fillRoundRect(215, y + 4, 88, 28, 4, CA_ERR);
                tft.setTextColor(CA_TXT); tft.setTextSize(1);
                tft.setCursor(222, y + 8);
                tft.print(t.type == TaskType::RECURRING ? "Delete +" : "Delete?");
                if (t.type == TaskType::RECURRING) { tft.setCursor(222, y + 19); tft.print("stop repeat"); }
            } else {
                tft.fillRoundRect(275, y + 4, 28, 28, 4, CA_ERR);
                tft.setTextColor(CA_TXT); tft.setTextSize(1);
                tft.setCursor(285, y + 12); tft.print("X");
            }
            y += 40;
        }
        if (_scroll > 0) { _btn(250, 10, 60, 28, "^ up", CA_BG, CA_TXT3, 1); }
        if (y >= 455) { tft.setTextColor(CA_TXT3); tft.setTextSize(1); tft.setCursor(140, 458); tft.print("v more"); }
    }

    bool _touchTaskList(uint16_t x, uint16_t y) {
        audio.touchClick();
        if (x < 50 && y < 48) { _menu = Menu::MAIN; _confirmDel = -1; _drawMain(); return false; }
        if (y >= 455 && _scroll + 10 < activeTaskCount) { _scroll += 5; _confirmDel = -1; _drawTaskList(); return false; }
        if (x >= 250 && y < 48 && _scroll > 0) { _scroll = (_scroll >= 5) ? _scroll - 5 : 0; _confirmDel = -1; _drawTaskList(); return false; }

        int16_t ry = 52;
        for (uint8_t i = _scroll; i < activeTaskCount && (ry + 40) < 455; i++) {
            if (y >= ry && y < ry + 36) {
                bool armed = (_confirmDel == (int8_t)i);
                if (armed && x >= 215) {          // second tap: delete
                    _deleteTask(i);
                    _confirmDel = -1;
                    _drawTaskList();
                } else if (x >= 275) {            // first tap: arm
                    _confirmDel = i;
                    _drawTaskList();
                } else {                          // Edit
                    _confirmDel = -1;
                    _beginEdit(i);
                }
                return false;
            }
            ry += 40;
        }
        if (_confirmDel >= 0) { _confirmDel = -1; _drawTaskList(); }
        return false;
    }

    void _beginEdit(uint8_t i) {
        _resetDraft();
        _editIdx = i;
        memcpy(&_draft, &activeTasks[i], sizeof(Task));
        uint8_t a = _draft.assignedTo;
        _assignMask = (a < familyCount) ? (1 << a) : 0;
        _assignAll = false;
        _assignAny = (a == 255);
        if (!_assignAny && _assignMask == 0) _assignAny = true;   // unknown id
        _pickHour = _draft.recurHour;
        _pickMin  = _draft.recurMinute;

        // Keep the existing name selectable even when it isn't a preset
        strlcpy(_origName, _draft.name, sizeof(_origName));
        _hasOrigName = true;
        _selName = NAMES_PER_CAT;
        uint8_t ci = static_cast<uint8_t>(_draft.category);
        for (uint8_t n = 0; n < NAMES_PER_CAT; n++)
            if (strcmp(presetTaskName(ci, n), _draft.name) == 0) { _selName = n; break; }

        _menu = Menu::CREATE_TASK; _wizStep = WizStep::CATEGORY; _drawWizard();
    }

    // Deleting a recurring instance also removes its template; otherwise the
    // scheduler regenerates it within five minutes and delete looks broken.
    void _deleteTask(uint8_t i) {
        Task gone = activeTasks[i];
        for (uint8_t j = i; j + 1 < activeTaskCount; j++)
            memcpy(&activeTasks[j], &activeTasks[j + 1], sizeof(Task));
        activeTaskCount--;

        if (gone.type == TaskType::RECURRING) {
            // Other children's instances of a shared (254) template go too
            for (uint8_t j = 0; j < activeTaskCount; ) {
                if (activeTasks[j].id == gone.id && activeTasks[j].type == TaskType::RECURRING) {
                    for (uint8_t k = j; k + 1 < activeTaskCount; k++)
                        memcpy(&activeTasks[k], &activeTasks[k + 1], sizeof(Task));
                    activeTaskCount--;
                } else j++;
            }
            Task recurTasks[MAX_ACTIVE_TASKS];
            uint8_t recurCount = 0;
            storage.loadTasks(recurTasks, recurCount, "/tasks/recurring.json");
            uint8_t keep = 0;
            for (uint8_t j = 0; j < recurCount; j++)
                if (recurTasks[j].id != gone.id) recurTasks[keep++] = recurTasks[j];
            if (keep != recurCount) storage.saveTasks(recurTasks, keep, "/tasks/recurring.json");
            reloadRecurringTemplates();
        }
        backups.markTasksDirty();
        Serial.printf("[ADMIN] Deleted '%s'\n", gone.name);
    }

    // ═════════════════════════════════════════════════════════════════════
    // FAMILY SETTINGS
    // ═════════════════════════════════════════════════════════════════════

    void _drawSettings() {
        tft.fillScreen(CA_BG);
        _hdr("Family Settings");

        tft.setTextColor(CA_TXT3); tft.setTextSize(1);
        tft.setCursor(15, 55); tft.print("TAP A NAME TO RENAME     DAILY GOAL (tasks/day)");

        int16_t y = 72;
        for (uint8_t i = 0; i < familyCount; i++) {
            tft.fillRoundRect(15, y, 290, 42, 6, CA_CARD);
            tft.setTextColor(CA_TXT2); tft.setTextSize(2);
            tft.setCursor(25, y + 5); tft.print(family[i].name);
            tft.setTextColor(CA_TXT3); tft.setTextSize(1);
            tft.setCursor(25, y + 26);
            tft.print(family[i].role == Role::PARENT ? "Parent" : "Child");

            _btn(205, y + 7, 28, 28, "-", CA_BG, CA_TXT3, 2);
            tft.setTextColor(CA_TXT); tft.setTextSize(2);
            tft.setCursor(243, y + 11); tft.printf("%d", family[i].dailyGoal);
            _btn(270, y + 7, 28, 28, "+", CA_BG, CA_TXT3, 2);
            y += 48;
        }

        _btn(15, 392, 290, 36, "Re-pair tags (scan new tags)", CA_PRI, CA_TXT, 1);

        _btn(15, 440, 120, 32, "< BACK", CA_DIM);
        _btn(185, 440, 120, 32, "SAVE", CA_OK);
    }

    bool _touchSettings(uint16_t x, uint16_t y) {
        audio.touchClick();
        if (x < 50 && y < 48) { _menu = Menu::MAIN; _drawMain(); return false; }

        for (uint8_t i = 0; i < familyCount; i++) {
            int16_t ry = 72 + i * 48;
            if (_hit(x, y, 15, ry, 180, 42)) {                 // rename
                char buf[NAME_LENGTH];
                strlcpy(buf, family[i].name, sizeof(buf));
                if (OnScreenKeyboard::run("Rename", buf, sizeof(buf), NAME_LENGTH - 1, true) && buf[0]) {
                    strlcpy(family[i].name, buf, NAME_LENGTH);
                    backups.markFamilyDirty();
                }
                _drawSettings();
                return false;
            }
            if (_hit(x, y, 205, ry + 7, 28, 28) && family[i].dailyGoal > 1) {
                family[i].dailyGoal--; backups.markFamilyDirty(); _drawSettings(); return false;
            }
            if (_hit(x, y, 270, ry + 7, 28, 28) && family[i].dailyGoal < 20) {
                family[i].dailyGoal++; backups.markFamilyDirty(); _drawSettings(); return false;
            }
        }

        if (_hit(x, y, 15, 392, 290, 36)) { wantsPairTags = true; return false; }
        if (_hit(x, y, 15, 440, 120, 32)) { _menu = Menu::MAIN; _drawMain(); return false; }
        if (_hit(x, y, 185, 440, 120, 32)) {
            storage.saveFamily(family, familyCount);
            audio.taskCompleteTone();
            _menu = Menu::MAIN; _drawMain();
        }
        return false;
    }

    // ═════════════════════════════════════════════════════════════════════
    // SYSTEM INFO
    // ═════════════════════════════════════════════════════════════════════

    void _drawSysInfo() {
        tft.fillScreen(CA_BG);
        _hdr("System Info");

        auto health = backups.checkHealth();
        int16_t y = 55;

        auto info = [&](const char* label, const char* val, uint16_t vc = CA_TXT2) {
            tft.setTextColor(CA_TXT3); tft.setTextSize(1);
            tft.setCursor(15, y); tft.print(label);
            tft.setTextColor(vc);
            tft.setCursor(160, y); tft.print(val);
            y += 18;
        };

        info("SD Card", health.mounted ? "Mounted" : "NOT FOUND", health.mounted ? CA_OK : CA_ERR);
        char sizeBuf[16]; snprintf(sizeBuf, sizeof(sizeBuf), "%lluMB", health.totalBytes / (1024*1024));
        info("SD Size", sizeBuf);
        char usedBuf[16]; snprintf(usedBuf, sizeof(usedBuf), "%lluMB", health.usedBytes / (1024*1024));
        info("SD Used", usedBuf);

        y += 5;
        info("WiFi", timeMan.isWifiConnected() ? "Connected" : "Disconnected",
             timeMan.isWifiConnected() ? CA_OK : CA_WARN);
        info("NTP Time", timeMan.isTimeSynced() ? "Synced" : "Not synced",
             timeMan.isTimeSynced() ? CA_OK : CA_WARN);

        if (timeMan.isTimeSynced()) {
            char tb[20]; timeMan.currentDateStr(tb, sizeof(tb));
            char full[32]; snprintf(full, sizeof(full), "%s %02d:%02d", tb, timeMan.currentHour(), timeMan.currentMinute());
            info("Current Time", full);
        }

        y += 5;
        char bkBuf[8]; snprintf(bkBuf, sizeof(bkBuf), "%d", health.backupCount);
        info("Backups", bkBuf);
        char lgBuf[8]; snprintf(lgBuf, sizeof(lgBuf), "%d", health.logFileCount);
        info("Log Files", lgBuf);
        char tkBuf[8]; snprintf(tkBuf, sizeof(tkBuf), "%d / %d", activeTaskCount, MAX_ACTIVE_TASKS);
        info("Active Tasks", tkBuf);
        char fmBuf[8]; snprintf(fmBuf, sizeof(fmBuf), "%d / %d", familyCount, MAX_FAMILY_MEMBERS);
        info("Members", fmBuf);

        y += 10;
        _sysBtnY = y;   // touch handler uses the same rows
        _btn(15, y, 290, 36, "Create Backup Now", CA_PRI);
        y += 44;
        _btn(15, y, 290, 36, "Force Save All Data", CA_CARD);
        y += 44;
        _btn(15, y, 290, 36, "WiFi Setup", CA_CARD);
    }

    bool _touchSysInfo(uint16_t x, uint16_t y) {
        audio.touchClick();
        if (x < 50 && y < 48) { _menu = Menu::MAIN; _drawMain(); return false; }

        if (_hit(x, y, 15, _sysBtnY, 290, 36)) {
            char dateStr[11]; timeMan.currentDateStr(dateStr, sizeof(dateStr));
            backups.createDailyBackup(dateStr);
            audio.taskCompleteTone();
            _drawSysInfo(); // Refresh to show updated count
            return false;
        }
        if (_hit(x, y, 15, _sysBtnY + 88, 290, 36)) {
            WifiSetup::run();
            _drawSysInfo();
            return false;
        }
        if (_hit(x, y, 15, _sysBtnY + 44, 290, 36)) {
            backups.flushNow(
                activeTasks, activeTaskCount, family, familyCount,
                [](const Task* t, uint8_t c, const char* p) { storage.saveTasks(t, c, p); },
                [](const FamilyMember* f, uint8_t c) { storage.saveFamily(f, c); }
            );
            audio.taskCompleteTone();
            return false;
        }
        return false;
    }
};
