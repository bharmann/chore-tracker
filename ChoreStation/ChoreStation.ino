// =============================================================================
// main.cpp — Family Chore Station v5
// =============================================================================
// CORE RULES:
//   1. SCAN = DASHBOARD. Any tag scan from any screen goes to that
//      person's dashboard. No conditional interpretation. Period.
//      (If they have a chore running, a "Done with it?" prompt opens on
//      top; Not yet = the dashboard.) That scan is the only one needed:
//      START and I'M DONE are on-screen taps.
//   2. DASHBOARD IS HOME BASE. All actions bounce back here.
//   3. TASK STATE IS SELF-CONTAINED. Timer, toggle, claim — all on Task.
//   4. PARENT APPROVES FROM PARENT DASHBOARD. Pending tasks shown at top.
//      Child's task detail NEVER shows approval buttons.
//   5. NON-BLOCKING MESSAGES. Brief overlays clear themselves.
//   6. CHILDREN CAN SELF-INITIATE. "+" button on child dashboard.
// =============================================================================

#include <Arduino.h>

// The E32R40T is a classic ESP32 (ESP32-WROOM-32E). Building for another chip
// (e.g. an ESP32-S3 left selected from another project) fails later with
// confusing errors like "'dacWrite' was not declared", so say it plainly.
#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Wrong board selected: use Tools > Board > esp32 > ESP32 Dev Module (the E32R40T is a classic ESP32). Then re-check Partition Scheme = Minimal SPIFFS (1.9MB APP with OTA) and Upload Speed = 460800."
#endif

#include <SPI.h>
#include <TFT_eSPI.h>

#include "pins.h"
#include "data_model.h"
#include "icons.h"
#include "milestones.h"
#include "rfid_manager.h"
#include "audio_feedback.h"
#include "storage_manager.h"
#include "task_engine.h"
#include "setup_wizard.h"
#include "display_power.h"
#include "backup_manager.h"
#include "time_manager.h"
#include "routine_manager.h"
#include "scheduler.h"
#include "ota_update.h"
#include "parent_admin.h"
#include "web_server.h"
#include "keyboard.h"
#include "wifi_setup.h"
#include "button_menu.h"
#include "preset_names.h"

// TFT_eSPI reads its pin config from the User_Setup.h inside the LIBRARY folder,
// not this sketch. A stale copy there builds fine but breaks the hardware, so
// refuse to compile instead.
#if !defined(USE_HSPI_PORT)
#error "TFT_eSPI would share the RC522/SD bus (RFID goes deaf after boot). Copy this repo's User_Setup.h into the TFT_eSPI library folder - see ARDUINO_IDE_SETUP.md."
#endif
#if !defined(TOUCH_CS) || !defined(ST7796_DRIVER)
#error "TFT_eSPI is not configured for the E32R40T. Copy this repo's User_Setup.h into the TFT_eSPI library folder - see ARDUINO_IDE_SETUP.md."
#endif

// ─── Globals ─────────────────────────────────────────────────────────────────

TFT_eSPI       tft;
RFIDManager    rfid;
AudioFeedback  audio;
TouchGate      touchGate;       // one action per finger press (input_logic.h)
StorageManager storage;
SetupWizard    wizard;
DisplayPower   display;
BackupManager  backups;
TimeManager    timeMan;
RoutineManager routines;
Scheduler      scheduler;
ParentAdmin    admin;
WebUI          web;
OtaUpdater     ota;             // firmware updates from GitHub releases

// A firmware installed over WiFi boots "pending verify": don't let the core
// mark it good at startup; ota.healthCheck() does after a minute of normal
// running (otherwise the bootloader goes back to the previous firmware).
extern "C" bool verifyRollbackLater() { return true; }

FamilyMember  family[MAX_FAMILY_MEMBERS];
uint8_t       familyCount = 0;
Task          activeTasks[MAX_ACTIVE_TASKS];
uint8_t       activeTaskCount = 0;

// ─── UI State ────────────────────────────────────────────────────────────────

uint8_t  currentUserId  = 255;
int8_t   viewingTaskIdx = -1;
uint8_t  taskPage       = 0;
uint32_t lastActivityMs = 0;
bool     inSetupMode    = false;

// Non-blocking message overlay
bool     msgActive      = false;
uint32_t msgExpireMs    = 0;
static const int16_t MSG_BOX_Y = 210;   // message overlay top
int16_t  msgBoxY0 = 0, msgBoxY1 = 0;    // where the overlay was drawn (a tap there only dismisses it)
bool     msgBlocking    = false;
// Points celebration (an overlay like the messages): big "+4", the new total
// and the badge bar filling up in a few steps, animated from loop().
bool     celebrate      = false;
uint8_t  celPts         = 0;
uint32_t celFrom = 0, celTo = 0;
uint8_t  celStep        = 0;
uint32_t celStepMs      = 0;
static const int16_t CEL_Y = 150, CEL_H = 176, CEL_BAR_Y = 274;  // badge popup: a tap only dismisses it (others pass through)
char     msgText[48]    = "";
char     msgSub[40]     = "";   // optional small second line
uint16_t msgColor       = 0;

// Last tag that matched nobody — shown on screen and offered by the web UI's
// Family tab so a parent can assign it without the setup wizard.
uint8_t  lastUnknownUid[UID_LENGTH];
uint8_t  lastUnknownLen = 0;
uint32_t lastUnknownMs  = 0;

bool     lastRfidOnline = false;   // redraw status when the reader comes/goes
bool     timersNeedRestore = false; // in-progress timers loaded from SD

// Tag pairing (re-assign tags to existing members, keeps names + points)
uint8_t  pairIdx = 0;
bool     bootHeld = false;          // BOOT button held at power-up → recovery
uint32_t lastSdRetryMs = 0;

// Self-init wizard state (child proposing a task)
bool     selfInitActive = false;
uint8_t  selfInitStep   = 0;   // 0=category, 1=name, 2=size, 3=confirm
Task     selfInitDraft;
uint8_t  selfInitNameIdx = 0;
char     selfInitCustom[41] = "";   // typed on the keyboard; overrides the preset
bool     selfInitInitiative = false; // child opted into the +2 "my own idea" bonus

// Quick-add state (parent fast path)
bool     quickAddActive = false;
uint8_t  quickAddStep   = 0;   // 0=category+name, 1=assign, 2=confirm

static const uint8_t  TASKS_PER_PAGE      = 5;
static const uint32_t IDLE_TIMEOUT_MS     = 60000;
static const uint32_t ROUTINE_IDLE_MS     = 180000; // checklists are done away from the screen
static const uint32_t SETUP_IDLE_MS       = 90000; // admin / add-job / tag pairing left open
static const uint32_t DETAIL_TIMEOUT_MS   = 20000; // Auto-return from detail
static const uint32_t DETAIL_REVIEW_MS    = 30000; // Longer timeout for parent review
uint32_t detailShownAtMs = 0;
// When the task screen opened. Its buttons ignore taps for a moment so the
// tap (or a double-tap) that opened it can't also press START/DONE/APPROVE.
uint32_t detailOpenedMs = 0;
static const uint32_t TAP_GUARD_MS = 600;
// Card buttons ignore presses for a moment after the list under the finger
// changes (a chore finished, a page turned, a new person): a double-tap must
// not finish the next chore that slid into place.
uint32_t chipGuardUntil = 0;
uint32_t lastListSig = 0;
static const uint32_t CHIP_GUARD_MS = 1000;
static const uint32_t ARRIVE_GUARD_MS = 400;
uint32_t promptOpenedMs = 0;        // "Done with...?" ignores taps for TAP_GUARD_MS
uint32_t lastSigCheckMs = 0;
enum CardChip : uint8_t { CHIP_NONE, CHIP_DONE, CHIP_START, CHIP_FINISH, CHIP_APPROVE };
static const int16_t CHIP_X = 248;
static const int16_t JOB_BTN_X = 226;                // "+ Job" button left edge (draw + touch)

enum class Screen : uint8_t {
    HOME, DASHBOARD, TASK_DETAIL, SELF_INIT, QUICK_ADD,
    PARENT_ADMIN, MILESTONES, ROUTINE, SETUP,
    NO_SD, PAIR_TAGS, DONE_PROMPT, NEWS
};
Screen currentScreen = Screen::HOME;

// Routine session state (one routine walked at a time)
int8_t viewingRoutineIdx = -1;

// "While you were away" — what happened to a person's chores since they last
// scanned: approvals (points, a badge), streak bonus, redos, time-outs. Shown
// on their next scan, then cleared. RAM only (a reboot forgets it).
struct AwayNews {
    uint16_t points;            // points from parent approvals
    uint8_t  approved;          // how many chores a parent checked
    int8_t   badge;             // highest badge earned while away (-1 none)
    uint8_t  streakBonus;       // points from a streak bonus
    uint8_t  redos;             // chores sent back to redo
    char     redoName[24];
    uint8_t  timedOut;          // chores whose time ran out (back on the list)
    char     timedOutName[24];
};
AwayNews news[MAX_FAMILY_MEMBERS];
uint32_t newsShownMs = 0;
static const uint32_t NEWS_AUTO_MS = 6000;

// "Done with this chore?" — shown right after a scan when that person has a
// chore running, since finishing it is almost always why they came back.
int8_t promptTaskIdx = -1;

// Layout shared by draw + touch for the task screen and the done prompt
static const int16_t DETAIL_BTN_Y = 310, BIG_BTN_H = 56, TOGGLE_Y = 245;
static const int16_t PUTBACK_Y = 384, PUTBACK_H = 40;
// Parent review: APPROVE (big, DETAIL_BTN_Y), "Approve, no bonus" if claimed, Redo
static const int16_t REVIEW_NB_Y = 374, REVIEW_BTN_H = 44;
int8_t   redoArmedIdx = -1;        // Redo needs a second tap within REDO_CONFIRM_MS
uint32_t redoArmedMs  = 0;
static const uint32_t REDO_CONFIRM_MS = 3000;
static const int16_t NEWS_OK_Y = 410;
// I Found a Job, step 1 (draw and touch share these)
static const int16_t SI_CAT_X = 15, SI_CAT_Y = 70, SI_CAT_W = 92, SI_CAT_H = 36;
static const int16_t SI_CAT_PITCH_X = 99, SI_CAT_PITCH_Y = 42;
static const int16_t SI_NAME_Y = 172, SI_NAME_H = 34, SI_NAME_PITCH = 40;
static const int16_t SI_DIDIT_Y = 300, SI_LATER_Y = 366;     // step 3 buttons
// Quick Add rows (draw + touch)
static const int16_t QA_CAT_Y = 54, QA_CAT_H = 30, QA_CAT_PITCH = 34;
static const int16_t QA_NAME_Y = 124, QA_NAME_H = 26, QA_NAME_PITCH = 30;
static const int16_t QA_ROW_H = 30, QA_SIZE_Y = 308, QA_SIZE_X = 62, QA_SIZE_W = 77;
static const int16_t QA_WHO_Y = 344, QA_WHO2_Y = 380;
// Routine screen rows + auto-finish (the member and routine are captured when
// the pause starts, so a scan by someone else can't take the points)
static const int16_t ROUT_TOP = 56, ROUT_BOTTOM = 436, ROUT_MAX_ROW = 62;
static const uint32_t ROUT_FINISH_PAUSE_MS = 600;
uint32_t routineFinishAt = 0;
int8_t   routineFinishIdx = -1;
uint8_t  routineFinishMember = 255;
// Dashboard list + pager (draw and touch share these)
static const int16_t LIST_Y = 135, ROW_H = 48;
static const int16_t PAGER_Y = LIST_Y + TASKS_PER_PAGE * ROW_H + 6, PAGER_H = 44;
static const int16_t PROMPT_TOGGLE_Y = 200, PROMPT_YES_Y = 300, PROMPT_NO_Y = 380, PROMPT_NO_H = 48;

// A dashboard list row is either a task or a routine. Both draw and touch build
// the SAME ordered list via buildDashRows() so a tapped card always matches the
// card that was drawn (this also fixes parent pending-first tap alignment).
struct DashRow { uint8_t kind; uint8_t idx; };  // kind: 0 = task, 1 = routine, 2 = "done today" summary
static const uint8_t DASH_ROWS_MAX = MAX_ACTIVE_TASKS + MAX_ROUTINES + 1;

// ─── Colors ──────────────────────────────────────────────────────────────────

#define C_BG   0x1082
#define C_CARD 0x2945
#define C_PRI  0x2C9F
#define C_OK   0x2E8B
#define C_WARN 0xFDA0
#define C_ERR  0xF800
#define C_GOLD 0xFE60
#define C_TXT  0xFFFF
#define C_TXT2 0xB596
#define C_TXT3 0x7BCF
#define C_DIM  0x4A69

static const uint16_t CAT_COL[]={0x2C9F,0x2E8B,0xFDA0,0x7A1F,0xFBE0,0x3666};
inline uint16_t catC(TaskCategory c){return CAT_COL[min((uint8_t)c,(uint8_t)5)];}
static const char* CAT_NAMES[]={"Cleaning","Health","Family","Academic","Pet Care","Outdoor"};
static const char* SIZE_NAMES[]={"Small","Medium","Large"};

// ─── Forward declarations ────────────────────────────────────────────────────

void drawHome();
void drawDashboard();
void drawTaskDetail();
void drawSelfInit();
int8_t addSelfInitJob();
void drawQuickAdd();

void goHome();
void goDashboard(uint8_t userId);
void goTaskDetail(uint8_t taskIdx);
void goRoutine(uint8_t routineIdx);
void drawRoutine();
int16_t routineRowH(uint8_t steps);
void touchRoutine(uint16_t x, uint16_t y);
void finishRoutine(uint8_t routineIdx, uint8_t member);
uint8_t buildDashRows(DashRow* rows, uint8_t maxRows);

void handleScan(uint8_t* uid, uint8_t len);
void startTaskFor(uint8_t taskIdx, uint8_t memberId);
void finishTaskFor(uint8_t taskIdx, uint8_t memberId);
int8_t runningTaskFor(uint8_t memberId);
void drawDonePrompt();
void drawNews();
void touchNews(uint16_t x, uint16_t y);
void continueAfterNews();
bool hasNews(uint8_t m);
void clearNews(uint8_t m);
void onStreakBonus(uint8_t m, uint8_t pts);
void enterPerson(uint8_t mid);
void parentApprove(uint8_t taskIdx, bool removeClaim);
void parentRedo(uint8_t taskIdx);
bool promptTaskValid();
void drawBigButton(int16_t y, const char* label, uint16_t color);
void touchDonePrompt(uint16_t x, uint16_t y);
void handleTouch(uint16_t x, uint16_t y);
void touchDashboard(uint16_t x, uint16_t y);
void touchTaskDetail(uint16_t x, uint16_t y);
void touchSelfInit(uint16_t x, uint16_t y);
void touchQuickAdd(uint16_t x, uint16_t y);

void showMsg(const char* msg, uint16_t color, uint16_t durationMs = 1200);
bool runOtaInstall();
void otaProgress(uint32_t done, uint32_t total);
void showMsg2(const char* msg, const char* sub, uint16_t color, uint16_t durationMs);
void drawMsgOverlay();
void showCelebration(uint8_t pts, uint32_t totalAfter);
void drawCelebration();
void drawCelebrationBar();
void showBadgePopup(int8_t badge);
void redrawCurrent();

void drawNoSd();
void goPairTags();
void drawPairTags();
void handlePairScan(uint8_t* uid, uint8_t len);
void touchPairTags(uint16_t x, uint16_t y);
void finishPairing();
void runRecovery();
bool confirmStartOver();
void startSetupOver();
bool touchCalPlausible(const uint16_t* cal);
uint8_t waitTapInBox(int16_t bx, int16_t by, int16_t bw, int16_t bh);
void runTouchCalibration();
void restoreTaskTimers();
bool canStartTask(const Task& t, uint8_t memberId);
int16_t txt(const char* s, int16_t x, int16_t y, uint8_t font, uint16_t color, uint8_t datum = TL_DATUM);
int16_t txtFit(const char* s, int16_t x, int16_t y, int16_t maxW, uint8_t font, uint16_t color);
uint8_t taskPoints(const Task& t);
void drawPointsTag(uint8_t pts, int16_t xr, int16_t y, uint16_t color);
int8_t nextBadge(uint32_t points, uint32_t& prevThreshold);
uint8_t badgesEarned(uint32_t points);
void drawBadgeBar(uint32_t points, int16_t x, int16_t y, int16_t w, int16_t h);
void drawChildHeader(const FamilyMember& user);
void drawParentHeader(const FamilyMember& user);
void drawRoutineCard(uint8_t r, int16_t y);
void drawTaskCard(uint8_t i, int16_t y, bool isParent);
uint8_t doneTodayChores(uint8_t m, uint16_t& pts);
void drawDoneTodayRow(int16_t y);
bool isTimed(const Task& t);
uint8_t cardChip(const Task& t, uint8_t viewer);
void doneNowFor(uint8_t taskIdx, uint8_t memberId);

bool isVisibleTask(uint8_t idx);
uint8_t totalPages();
uint32_t dashSignature();
uint8_t countPending();

// Preset names imported from preset_names.h as presetTaskName()

// Name a child's proposed job will get: typed, or the chosen preset.
const char* selfInitName() {
    if (selfInitCustom[0]) return selfInitCustom;
    return presetTaskName(static_cast<uint8_t>(selfInitDraft.category), selfInitNameIdx);
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200); delay(500);
    Serial.println("\n══ Family Chore Station v5 ══\n");

    // BOOT (GPIO0) pressed while "Starting..." shows opens the recovery screen
    // (re-pair tags, recalibrate touch). NOT held during power-on: GPIO0 low at
    // reset puts the ESP32 into its flashing bootloader instead.
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

    audio.begin();
    for (uint8_t m = 0; m < MAX_FAMILY_MEMBERS; m++) clearNews(m);
    scheduler.onStreakBonus = onStreakBonus;
    SPI.begin(PIN_VSPI_CLK, PIN_VSPI_MISO, PIN_VSPI_MOSI);
    bool sdOk = storage.begin();
    rfid.begin();
    lastRfidOnline = rfid.isOnline();
    tft.init(); tft.setRotation(0); tft.fillScreen(C_BG);
    display.begin();

    if (!sdOk) {
        // Nothing works without the card (config, family, tasks). Say so
        // plainly and keep retrying instead of dropping into the setup wizard,
        // which can't save without it anyway.
        currentScreen = Screen::NO_SD;
        drawNoSd();
        lastActivityMs = millis();
        return;
    }

    // Touch calibration is per-unit; run it once (or on request) and keep the
    // result on the SD card. Every tap target depends on it.
    uint16_t cal[5];
    if (storage.loadTouchCal(cal) && touchCalPlausible(cal)) {
        tft.setTouch(cal);
        Serial.printf("[TOUCH] Calibration loaded: %u %u %u %u %u\n", cal[0], cal[1], cal[2], cal[3], cal[4]);
    } else {
        runTouchCalibration();   // missing or implausible touch.json
    }

    tft.fillScreen(C_BG);
    tft.setTextColor(C_PRI); tft.setTextSize(2);
    tft.setCursor(80, 220); tft.print("Starting...");
    tft.setTextColor(C_TXT3); tft.setTextSize(1);
    tft.setCursor(55, 260); tft.print("Hold BOOT now for recovery mode");
    uint32_t splashMs = millis();
    while (millis() - splashMs < 1500 && !bootHeld) {
        bootHeld = (digitalRead(PIN_BOOT_BUTTON) == LOW);
        delay(20);
    }

    timeMan.begin();   // may take up to 15 s while WiFi connects
    if (digitalRead(PIN_BOOT_BUTTON) == LOW) bootHeld = true;
    backups.begin();

    if (storage.isFirstRun()) {
        inSetupMode = true; currentScreen = Screen::SETUP; wizard.begin();
    } else {
        storage.loadFamily(family, familyCount);
        storage.loadTasks(activeTasks, activeTaskCount, "/tasks/active.json");
        timersNeedRestore = true;
        routines.begin();
        scheduler.begin();
        if (activeTaskCount == 0) scheduler.generateRecurringTasks();
        web.begin(timeMan.webPin());   // parent web editor (reachable once WiFi is up)
        if (bootHeld) runRecovery();
        else          goHome();
    }
    audio.scanBeep(); lastActivityMs = millis();
    touchGate.requireRelease();   // a finger still on the glass from recovery/calibration
}

// Streak bonuses are awarded at the midnight reset: tell the child next scan.
void onStreakBonus(uint8_t m, uint8_t pts) {
    if (m < MAX_FAMILY_MEMBERS) news[m].streakBonus += pts;
}

// ═════════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
    if (currentScreen == Screen::NO_SD) {
        ota.healthCheck(false);    // new firmware that can't read the card goes back
        // Retry the mount; a clean restart is the simplest way to bring every
        // module up in the right order once the card is in.
        if (millis() - lastSdRetryMs > 2000) {
            lastSdRetryMs = millis();
            if (storage.begin()) {
                Serial.println("[SD] Card found — restarting.");
                delay(200);
                ESP.restart();
            }
        }
        delay(10);
        return;
    }

    if (inSetupMode) {
        ota.healthCheck(true);
        if (wizard.update()) {
            inSetupMode = false;
            memcpy(family, wizard.getMembers(), sizeof(FamilyMember)*wizard.getMemberCount());
            familyCount = wizard.getMemberCount();
            touchGate.requireRelease();
            goHome();
        }
        return;
    }

    ota.healthCheck(true);

    // RFID — always polls, always goes to dashboard (pairing mode excepted)
    uint8_t uid[UID_LENGTH]; uint8_t uidLen = 0;
    if (rfid.poll(uid, uidLen)) {
        display.wake();
        if (currentScreen == Screen::PAIR_TAGS) handlePairScan(uid, uidLen);
        else                                    handleScan(uid, uidLen);
    }

    // Reader came online / dropped out: refresh screens that show its status
    if (rfid.isOnline() != lastRfidOnline) {
        lastRfidOnline = rfid.isOnline();
        if (currentScreen == Screen::HOME || currentScreen == Screen::PAIR_TAGS) redrawCurrent();
    }

    if (timersNeedRestore && timeMan.isTimeSynced()) restoreTaskTimers();

    // Touch: act once per press (a held finger never repeats). A press on a
    // dark screen only wakes it; it still counts as activity so the screen
    // doesn't drop straight back to sleep once it has faded in.
    uint16_t tx = 0, ty = 0;
    bool touched = tft.getTouch(&tx, &ty);
    if (touchGate.poll(touched, tx, ty, millis())) {
        bool visible = display.isVisible();
        display.wake();
        lastActivityMs = millis();
        if (visible) handleTouch(touchGate.x, touchGate.y);
    }

    // Display power
    display.update(lastActivityMs, timeMan.currentHour());
    if (display.needsRedraw()) {
        if (currentScreen == Screen::HOME) drawHome();
        else if (currentScreen == Screen::DASHBOARD) drawDashboard();
    }

    // Timers below run whether or not the screen is lit: a time limit must
    // expire and an open screen must lock even while the backlight is off.
    {
        // Task timeouts
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            if (TaskEngine::checkTimeout(activeTasks[i])) {
                // Time ran out: back on the list (a first-taker chore goes back
                // to everyone) and the child is told on their next scan.
                Task& t = activeTasks[i];
                uint8_t who = t.assignedTo;              // before putBack resets it
                if (who < MAX_FAMILY_MEMBERS) {
                    AwayNews& n = news[who];
                    if (n.timedOut++ == 0) strlcpy(n.timedOutName, t.name, sizeof(n.timedOutName));
                }
                TaskEngine::putBack(t, who);
                Serial.printf("[TASK] '%s' ran out of time - back on the list\n", t.name);
                backups.markTasksDirty();
                if (currentScreen == Screen::DASHBOARD) drawDashboard();
                else if (currentScreen == Screen::TASK_DETAIL && viewingTaskIdx == i)
                    drawTaskDetail();
                else if (currentScreen == Screen::DONE_PROMPT && promptTaskIdx == i)
                    drawDonePrompt();   // no longer running: goes to the dashboard
            }
        }

        // Non-blocking message expiry
        if (msgActive && millis() >= msgExpireMs) {
            msgActive = false;
            redrawCurrent();   // clear the overlay
        }

        // Dashboard shows something stale (a card button appeared/disappeared)?
        if (currentScreen == Screen::DASHBOARD && !msgActive && millis() - lastSigCheckMs >= 1000) {
            lastSigCheckMs = millis();
            if (dashSignature() != lastListSig) drawDashboard();
        }

        // Routine auto-finish after the last tick
        if (routineFinishAt && (int32_t)(millis() - routineFinishAt) >= 0) {
            routineFinishAt = 0;
            int8_t ri = routineFinishIdx; uint8_t rm = routineFinishMember;
            if (currentScreen == Screen::ROUTINE && viewingRoutineIdx == ri && currentUserId == rm &&
                ri >= 0 && ri < routines.count() && !routines.isDoneToday(ri, rm) &&
                routines.stepsDone(ri, rm) == routines.get(ri).stepCount)
                finishRoutine(ri, rm);
        }

        // Redo confirm window over: show plain "Redo" again
        if (redoArmedIdx >= 0 && millis() - redoArmedMs >= REDO_CONFIRM_MS) {
            redoArmedIdx = -1;
            if (currentScreen == Screen::TASK_DETAIL) drawTaskDetail();
        }

        // Celebration bar fills in a few steps
        if (msgActive && celebrate && celStep < 3 && millis() - celStepMs >= 150) {
            celStep++; celStepMs = millis();
            drawCelebrationBar();
        }

        // News card moves on by itself
        if (currentScreen == Screen::NEWS && millis() - newsShownMs > NEWS_AUTO_MS) continueAfterNews();

        // Auto-return from task detail
        if (currentScreen == Screen::TASK_DETAIL && viewingTaskIdx >= 0) {
            // Use longer timeout if parent is reviewing pending tasks
            bool isReview = (currentUserId < familyCount &&
                             family[currentUserId].role == Role::PARENT &&
                             activeTasks[viewingTaskIdx].state == TaskState::PENDING_REVIEW);
            uint32_t timeout = isReview ? DETAIL_REVIEW_MS : DETAIL_TIMEOUT_MS;
            if (millis() - detailShownAtMs > timeout) {
                goDashboard(currentUserId);
            }
        }

        // Pixel shift on home
        if (!display.isSleeping() && currentScreen == Screen::HOME && display.shouldPixelShift()) drawHome();

        // Idle → home (locks the station so the next person must scan).
        // Screens that change things (admin, adding jobs, pairing tags) get
        // a longer timeout but are never left open indefinitely.
        uint32_t idle = millis() - lastActivityMs;
        bool quickLock = currentScreen == Screen::DASHBOARD || currentScreen == Screen::MILESTONES ||
                         currentScreen == Screen::DONE_PROMPT || currentScreen == Screen::NEWS;
        bool routineLock = currentScreen == Screen::ROUTINE;
        bool slowLock  = currentScreen == Screen::PARENT_ADMIN || currentScreen == Screen::SELF_INIT ||
                         currentScreen == Screen::QUICK_ADD || currentScreen == Screen::PAIR_TAGS;
        if ((quickLock && idle > IDLE_TIMEOUT_MS) || (routineLock && idle > ROUTINE_IDLE_MS) ||
            (slowLock && idle > SETUP_IDLE_MS)) goHome();
    }

    // Background services
    backups.update(activeTasks, activeTaskCount, family, familyCount,
        [](const Task* t, uint8_t c, const char* p){ storage.saveTasks(t, c, p); },
        [](const FamilyMember* f, uint8_t c){ storage.saveFamily(f, c); });
    timeMan.update();
    if (scheduler.update()) {
        // The task list may have been compacted: screens holding a task
        // index go back to the dashboard rather than act on the wrong chore.
        if (currentScreen == Screen::TASK_DETAIL || currentScreen == Screen::DONE_PROMPT) goDashboard(currentUserId);
        else if (currentScreen == Screen::DASHBOARD) drawDashboard();
    }

    // Firmware: look for a newer release now and then, only while nobody is
    // using the station (the check blocks for a second or two).
    if (currentScreen == Screen::HOME && millis() - lastActivityMs > 60000 && ota.checkDue()) {
        ota.check();
        if (currentScreen == Screen::HOME) drawHome();
    }

    // Web editor: serve requests, then apply any saves without a reboot.
    web.handle();
    if (web.pendingOtaInstall) {
        web.pendingOtaInstall = false;
        if (ota.updateAvailable()) runOtaInstall();   // restarts on success
        redrawCurrent();
    }
    if (web.pendingRoutineReload) {
        web.pendingRoutineReload = false;
        routines.begin();
        if (currentScreen == Screen::DASHBOARD) drawDashboard();
        Serial.println("[WEB] Routines reloaded from edit.");
    }
    if (web.pendingTaskReload) {
        web.pendingTaskReload = false;
        scheduler.begin();                    // reload templates, sync today's list
        scheduler.generateRecurringTasks();   // new chores show up right away
        Serial.println("[WEB] Recurring templates reloaded from edit.");
        if (currentScreen == Screen::TASK_DETAIL || currentScreen == Screen::DONE_PROMPT)
            goDashboard(currentUserId);   // index may have moved
        else if (currentScreen == Screen::DASHBOARD || currentScreen == Screen::HOME) redrawCurrent();
    }
    if (web.familyChanged) {
        web.familyChanged = false;
        if (currentScreen == Screen::HOME || currentScreen == Screen::DASHBOARD) redrawCurrent();
    }

    delay(10);
}

// ═════════════════════════════════════════════════════════════════════════════
// NAVIGATION
// ═════════════════════════════════════════════════════════════════════════════

void goHome() {
    routineFinishAt = 0;
    currentUserId = 255; viewingTaskIdx = -1; viewingRoutineIdx = -1; taskPage = 0;
    selfInitActive = false; quickAddActive = false; msgActive = false;
    currentScreen = Screen::HOME; drawHome();
}

void goDashboard(uint8_t userId) {
    currentUserId = userId; viewingTaskIdx = -1; viewingRoutineIdx = -1; taskPage = 0;
    selfInitActive = false; quickAddActive = false; redoArmedIdx = -1;
    currentScreen = Screen::DASHBOARD; lastActivityMs = millis();
    drawDashboard();   // also redraws any message on top
    // Arriving: card buttons wait a moment (the tap that got us here)
    if ((int32_t)(chipGuardUntil - (millis() + ARRIVE_GUARD_MS)) < 0) chipGuardUntil = millis() + ARRIVE_GUARD_MS;
}

void goTaskDetail(uint8_t taskIdx) {
    viewingTaskIdx = taskIdx; redoArmedIdx = -1;
    currentScreen = Screen::TASK_DETAIL;
    detailShownAtMs = millis(); lastActivityMs = millis(); detailOpenedMs = millis();
    // A bonus toggle left on by someone who didn't finish doesn't carry over
    if (activeTasks[taskIdx].state == TaskState::AVAILABLE) activeTasks[taskIdx].noReminderToggled = false;
    drawTaskDetail();
}

// ═════════════════════════════════════════════════════════════════════════════
// SCAN — ALWAYS goes to that person's dashboard. No exceptions.
// ═════════════════════════════════════════════════════════════════════════════

void handleScan(uint8_t* uid, uint8_t len) {
    lastActivityMs = millis();

    int8_t mid = -1;
    for (uint8_t i = 0; i < familyCount; i++)
        if (RFIDManager::matchUid(uid, len, family[i])) { mid = i; break; }

    if (mid < 0) {
        // Remember it so a parent can assign it from the web UI (Family tab),
        // and show the UID so it can also be typed into family.json.
        memcpy(lastUnknownUid, uid, len);
        lastUnknownLen = len;
        lastUnknownMs = millis();
        char hex[UID_LENGTH * 3 + 1];
        RFIDManager::uidToHex(uid, len, hex, sizeof(hex));
        audio.errorTone();
        showMsg2("Unknown tag", hex, C_ERR, 2500);
        return;
    }

    audio.scanBeep();
    msgActive = false; celebrate = false;   // nothing from the last person carries over
    routineFinishAt = 0;

    // Scan = this person's dashboard. If they have a chore running, ask
    // straight away whether they're done with it (Not yet = the dashboard).
    if (hasNews(mid)) {
        currentUserId = mid; viewingTaskIdx = -1; viewingRoutineIdx = -1; taskPage = 0;
        selfInitActive = false; quickAddActive = false; msgActive = false;
        currentScreen = Screen::NEWS; lastActivityMs = millis(); newsShownMs = millis();
        drawNews();
        const AwayNews& n = news[mid];
        if (n.badge >= 0)                     audio.badgeTone();
        else if (n.approved || n.streakBonus) audio.taskCompleteTone();
        else if (n.redos)                     audio.redoTone();
        return;
    }
    enterPerson(mid);
}

// After the scan (and any news): a running chore asks "Done with it?",
// otherwise the dashboard.
void enterPerson(uint8_t mid) {
    int8_t run = runningTaskFor(mid);
    if (run < 0) { goDashboard(mid); return; }
    currentUserId = mid; viewingTaskIdx = -1; viewingRoutineIdx = -1; taskPage = 0;
    selfInitActive = false; quickAddActive = false; msgActive = false;
    promptTaskIdx = run;
    currentScreen = Screen::DONE_PROMPT; lastActivityMs = millis(); promptOpenedMs = millis();
    drawDonePrompt();
}

bool hasNews(uint8_t m) {
    if (m >= MAX_FAMILY_MEMBERS) return false;
    const AwayNews& n = news[m];
    return n.approved || n.badge >= 0 || n.streakBonus || n.redos || n.timedOut;
}

void clearNews(uint8_t m) {
    if (m >= MAX_FAMILY_MEMBERS) return;
    memset(&news[m], 0, sizeof(AwayNews));
    news[m].badge = -1;
}

// ── Parent decisions (the child hears about them on their next scan) ──

void parentApprove(uint8_t taskIdx, bool removeClaim) {
    Task& task = activeTasks[taskIdx];
    uint8_t who = task.assignedTo;
    if (who >= familyCount) return;
    TransitionResult r = removeClaim ? TaskEngine::approveWithOverride(task, family[who])
                                     : TaskEngine::approveTask(task, family[who]);
    if (!r.success) { showMsg(r.message, C_WARN, 1200); return; }
    backups.markTasksDirty(); backups.markFamilyDirty();
    AwayNews& n = news[who];
    n.points += r.pointsEarned;
    n.approved++;
    if (r.newMilestone > n.badge) n.badge = r.newMilestone;
    audio.taskCompleteTone();
    char msg[32], sub[40];
    snprintf(msg, sizeof(msg), "Approved: +%d", r.pointsEarned);
    snprintf(sub, sizeof(sub), removeClaim ? "for %s (bonus removed)" : "for %s", family[who].name);
    showMsg2(msg, sub, removeClaim ? C_WARN : C_OK, 1300);
}

void parentRedo(uint8_t taskIdx) {
    Task& task = activeTasks[taskIdx];
    uint8_t who = task.assignedTo;
    if (who < MAX_FAMILY_MEMBERS) {
        AwayNews& n = news[who];
        if (n.redos++ == 0) strlcpy(n.redoName, task.name, sizeof(n.redoName));
    }
    TaskEngine::rejectTask(task);
    scheduler.settleRejected(taskIdx);   // may remove it (today's copy is the redo)
    backups.markTasksDirty();
    audio.redoTone();
    showMsg("Sent back to redo", C_ERR, 1000);
}

// The chore this person started longest ago that's still running, or -1.
int8_t runningTaskFor(uint8_t memberId) {
    int8_t best = -1;
    for (uint8_t i = 0; i < activeTaskCount; i++) {
        const Task& t = activeTasks[i];
        if (t.state != TaskState::IN_PROGRESS || t.assignedTo != memberId) continue;
        if (best < 0 || (int32_t)(t.startedAtMs - activeTasks[best].startedAtMs) < 0) best = i;
    }
    return best;
}

// Start: the scan that opened the dashboard already said who this is. Back to
// the scan screen afterwards, since they're off to do the chore.
void startTaskFor(uint8_t taskIdx, uint8_t memberId) {
    Task& task = activeTasks[taskIdx];
    TransitionResult r = TaskEngine::startTask(task, memberId);   // claims "Anyone" tasks
    if (!r.success) { showMsg(r.message, C_WARN, 1500); return; }
    backups.markTasksDirty();
    audio.taskStartTone();
    goHome();
    showMsg2("Task started!", "Scan your tag when you're done", C_OK, 2000);
}

// One-tap chore: start and finish together, keeping the bonus toggle the
// child set on the task screen (startTask clears it).
void doneNowFor(uint8_t taskIdx, uint8_t memberId) {
    Task& task = activeTasks[taskIdx];
    bool claim = task.noReminderToggled;
    TransitionResult r = TaskEngine::startTask(task, memberId);   // claims "Anyone" tasks
    if (!r.success) { showMsg(r.message, C_WARN, 1500); return; }
    task.noReminderToggled = claim;
    finishTaskFor(taskIdx, memberId);
}

void finishTaskFor(uint8_t taskIdx, uint8_t memberId) {
    Task& task = activeTasks[taskIdx];
    TransitionResult r = TaskEngine::completeTask(task, memberId, taskIdx);
    if (!r.success) { audio.errorTone(); showMsg(r.message, C_WARN, 1500); return; }   // e.g. not enough time yet

    bool claim = task.noReminderToggled;
    TransitionResult r2 = TaskEngine::submitClaim(task, family[memberId], claim);
    backups.markTasksDirty(); backups.markFamilyDirty();
    if (r2.newState == TaskState::COMPLETED) {
        if (r2.newMilestone >= 0) {
            goDashboard(memberId);
            showBadgePopup(r2.newMilestone);
            return;
        }
        audio.taskCompleteTone();
        msgActive = false;
        goDashboard(memberId);
        showCelebration(r2.pointsEarned, family[memberId].totalPoints);
        return;
    }
    audio.sentTone();
    char sub[32];
    snprintf(sub, sizeof(sub), "+%d when a parent checks", taskPoints(task) + (claim ? PointTable::NO_REMINDER_BONUS : 0));
    showMsg2("Sent to a parent", sub, C_PRI, 1500);
    goDashboard(memberId);
}

// ═════════════════════════════════════════════════════════════════════════════
// TOUCH ROUTER
// ═════════════════════════════════════════════════════════════════════════════

void handleTouch(uint16_t x, uint16_t y) {
    Serial.printf("[TOUCH] %u,%u\n", x, y);   // where taps land: calibration diagnostics
    audio.touchClick();

    // A tap clears a message and, unless it landed on the message itself,
    // still does what it was aimed at, so the next action isn't lost. The
    // badge celebration always swallows the tap.
    if (msgActive) {
        bool onBox = y >= msgBoxY0 && y < msgBoxY1;
        bool swallow = msgBlocking || onBox;
        msgActive = false; msgBlocking = false;
        redrawCurrent();
        if (swallow) return;
    }

    switch (currentScreen) {
        case Screen::HOME: break;
        case Screen::DASHBOARD:   touchDashboard(x, y); break;
        case Screen::TASK_DETAIL: touchTaskDetail(x, y); break;
        case Screen::ROUTINE:     touchRoutine(x, y); break;
        case Screen::SELF_INIT:   touchSelfInit(x, y); break;
        case Screen::QUICK_ADD:   touchQuickAdd(x, y); break;
        case Screen::PAIR_TAGS:   touchPairTags(x, y); break;
        case Screen::DONE_PROMPT: touchDonePrompt(x, y); break;
        case Screen::NEWS:        touchNews(x, y); break;
        case Screen::PARENT_ADMIN:
            if (admin.handleTouch(x, y)) goDashboard(currentUserId);
            else if (admin.wantsPairTags) { admin.wantsPairTags = false; goPairTags(); }
            break;
        case Screen::MILESTONES:
            if (x < 50 && y < 55) goDashboard(currentUserId);
            break;
        default: break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// DASHBOARD TOUCH
// ═════════════════════════════════════════════════════════════════════════════

void touchDashboard(uint16_t x, uint16_t y) {
    if (currentUserId >= familyCount) return;
    bool isParent = (family[currentUserId].role == Role::PARENT);

    // Top-right gear = Admin (parent); a child's header + badge row = badges
    if ((isParent && x >= 240 && y <= 55) || (!isParent && y < 84)) {
        if (isParent) {
            admin.begin();
            currentScreen = Screen::PARENT_ADMIN;
            admin.drawCurrentScreen();
        } else {
            currentScreen = Screen::MILESTONES;
            MilestoneIcons::drawCollection(family[currentUserId]);
        }
        return;
    }

    // "+ Job" (bottom-right): I Found a Job for a child, Quick Add for a parent
    if (!isParent && x >= JOB_BTN_X && y >= 436) {
        selfInitActive = true; selfInitStep = 0;
        memset(&selfInitDraft, 0, sizeof(Task));
        selfInitDraft.type = TaskType::SELF_INIT;
        selfInitDraft.assignedTo = currentUserId;
        selfInitDraft.createdBy = currentUserId;
        selfInitDraft.requiresApproval = true;
        selfInitDraft.size = TaskSize::SMALL;
        selfInitNameIdx = 0;
        selfInitCustom[0] = '\0';
        selfInitInitiative = false;
        currentScreen = Screen::SELF_INIT;
        drawSelfInit();
        return;
    }

    if (isParent && x >= JOB_BTN_X && y >= 436) {
        quickAddActive = true; quickAddStep = 0;
        memset(&selfInitDraft, 0, sizeof(Task)); // Reuse draft
        selfInitDraft.type = TaskType::ADHOC;
        selfInitDraft.size = TaskSize::SMALL;
        selfInitDraft.assignedTo = 255;
        selfInitDraft.createdBy = currentUserId;
        selfInitDraft.requiresApproval = false;
        selfInitNameIdx = 0;
        selfInitCustom[0] = '\0';
        currentScreen = Screen::QUICK_ADD;
        drawQuickAdd();
        return;
    }

    // Bye (padlock): back to the scan screen so nobody else can use this dashboard
    if (x < 100 && y >= 436) { goHome(); return; }

    // Pager (Back / More, right under the cards)
    uint8_t tp = totalPages();
    if (tp > 1 && y >= PAGER_Y && y < PAGER_Y + PAGER_H) {
        if (x >= 200 && taskPage < tp - 1) { taskPage++; drawDashboard(); }
        else if (x < 120 && taskPage > 0) { taskPage--; drawDashboard(); }
        return;
    }

    // Card taps (routines + tasks) — same ordering and rows as drawDashboard.
    if (y >= LIST_Y && y < LIST_Y + TASKS_PER_PAGE * ROW_H) {
        uint8_t slot = (y - LIST_Y) / ROW_H;
        uint8_t target = taskPage * TASKS_PER_PAGE + slot;
        DashRow rows[DASH_ROWS_MAX];
        uint8_t rowCount = buildDashRows(rows, DASH_ROWS_MAX);
        if (target < rowCount) {
            if (rows[target].kind == 1) { goRoutine(rows[target].idx); return; }
            if (rows[target].kind != 0) return;                 // "done today" summary
            uint8_t i = rows[target].idx;
            uint8_t chip = cardChip(activeTasks[i], currentUserId);
            if (chip && x >= CHIP_X - 4) {
                if ((int32_t)(millis() - chipGuardUntil) < 0) return;   // list just changed
                uint8_t me = currentUserId;
                switch (chip) {
                    case CHIP_DONE:                       // no bonus claim from a quick tap
                        activeTasks[i].noReminderToggled = false;
                        doneNowFor(i, me); break;
                    case CHIP_START:   startTaskFor(i, me); break;
                    case CHIP_FINISH:  finishTaskFor(i, me); break;
                    case CHIP_APPROVE: parentApprove(i, false); drawDashboard(); break;
                }
                chipGuardUntil = millis() + CHIP_GUARD_MS;
                return;
            }
            goTaskDetail(i);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// TASK DETAIL TOUCH
// ═════════════════════════════════════════════════════════════════════════════

void touchTaskDetail(uint16_t x, uint16_t y) {
    lastActivityMs = millis();
    detailShownAtMs = millis();

    if (viewingTaskIdx < 0) return;
    Task& task = activeTasks[viewingTaskIdx];
    bool isOwner = (currentUserId == task.assignedTo);
    bool isParent = (currentUserId < familyCount && family[currentUserId].role == Role::PARENT);

    // Back
    if (x < 60 && y < 48) { goDashboard(currentUserId); return; }

    if (millis() - detailOpenedMs < TAP_GUARD_MS) return;   // the tap that opened this screen

    bool startable = canStartTask(task, currentUserId);
    bool running   = isOwner && task.state == TaskState::IN_PROGRESS;

    // ── START (timed) / DONE (one tap) / I'M DONE: the dashboard scan is enough ──
    if (y >= DETAIL_BTN_Y && y < DETAIL_BTN_Y + BIG_BTN_H) {
        if (startable) {
            if (isTimed(task)) startTaskFor(viewingTaskIdx, currentUserId);
            else               doneNowFor(viewingTaskIdx, currentUserId);
            return;
        }
        if (running && TaskEngine::remainingMinTime(task) <= 0) {
            finishTaskFor(viewingTaskIdx, currentUserId);
            return;
        }
    }

    // ── Put it back: started by mistake, or doing something else first ──
    if (running && y >= PUTBACK_Y && y < PUTBACK_Y + PUTBACK_H) {
        if (TaskEngine::putBack(task, currentUserId).success) {
            backups.markTasksDirty();
            showMsg("Put back on the list", C_PRI, 1200);
        }
        goDashboard(currentUserId);
        return;
    }

    // ── No-reminder toggle (while running, or before a one-tap DONE) ──
    bool toggleShown = task.type != TaskType::SELF_INIT && (running || (startable && !isTimed(task)));
    if (toggleShown && y >= TOGGLE_Y && y < TOGGLE_Y + 44) {
        task.noReminderToggled = !task.noReminderToggled;
        drawTaskDetail();
        return;
    }

    // ── Parent approval actions (only from parent's own dashboard context) ──
    if (isParent && task.state == TaskState::PENDING_REVIEW) {
        if (y >= DETAIL_BTN_Y && y < DETAIL_BTN_Y + BIG_BTN_H) {
            parentApprove(viewingTaskIdx, false);   // a new badge is the child's news, not shown here
            goDashboard(currentUserId);
            return;
        }
        int16_t redoY = REVIEW_NB_Y;
        if (task.childClaimedNoReminder) {
            if (y >= REVIEW_NB_Y && y < REVIEW_NB_Y + REVIEW_BTN_H) {
                parentApprove(viewingTaskIdx, true);
                goDashboard(currentUserId);
                return;
            }
            redoY = REVIEW_NB_Y + REVIEW_BTN_H + 8;
        }
        if (y >= redoY && y < redoY + REVIEW_BTN_H) {
            bool armed = redoArmedIdx == viewingTaskIdx && millis() - redoArmedMs < REDO_CONFIRM_MS;
            if (armed) {
                redoArmedIdx = -1;
                parentRedo(viewingTaskIdx);
                goDashboard(currentUserId);
            } else {
                redoArmedIdx = viewingTaskIdx; redoArmedMs = millis();
                drawTaskDetail();
            }
            return;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ROUTINE — one scan got the child here; tap each step, earn a flat bundle
// ═════════════════════════════════════════════════════════════════════════════

void goRoutine(uint8_t idx) {
    viewingRoutineIdx = idx;
    // Every step already ticked (left during the pause, an edit removed the
    // last open step, old saved state): finish it now instead of leaving it
    // stuck at "All done".
    if (idx < routines.count() && currentUserId < familyCount && routines.get(idx).stepCount > 0 &&
        !routines.isDoneToday(idx, currentUserId) &&
        routines.stepsDone(idx, currentUserId) >= routines.get(idx).stepCount) {
        routineFinishAt = millis() + ROUT_FINISH_PAUSE_MS;
        routineFinishIdx = idx; routineFinishMember = currentUserId;
    }
    currentScreen = Screen::ROUTINE;
    detailShownAtMs = millis(); lastActivityMs = millis();
    drawRoutine();
}

void finishRoutine(uint8_t idx, uint8_t member) {
    if (idx >= routines.count() || member >= familyCount) { goDashboard(currentUserId); return; }
    Routine& rt = routines.get(idx);
    FamilyMember& m = family[member];

    uint32_t prev = m.totalPoints;
    m.totalPoints += rt.bundlePoints;
    m.dailyTasksCompleted++;
    routines.markDone(idx, m.id);

    // Milestone check (same rule as task completion)
    int8_t ms = -1;
    for (uint8_t i = 0; i < MILESTONE_COUNT; i++) {
        if (prev < MILESTONES[i].pointThreshold && m.totalPoints >= MILESTONES[i].pointThreshold) {
            ms = i; break;
        }
    }

    // Audit log — routine ids offset by 60000 so they can't collide with tasks
    CompletionLog log;
    log.taskId = 60000 + rt.id;
    log.memberId = m.id;
    log.basePoints = rt.bundlePoints;
    log.bonusPoints = 0;
    log.completedEpoch = timeMan.currentEpoch();
    log.wasInitiative = false;
    log.childClaimedNoReminder = false;
    log.noReminderApproved = false;
    log.parentOverrode = false;
    log.durationSec = 0;
    char d[11]; timeMan.currentDateStr(d, sizeof(d));
    storage.appendLog(log, d);

    backups.markFamilyDirty();
    Serial.printf("[ROUTINE] %s completed '%s' (+%d)\n", m.name, rt.name, rt.bundlePoints);

    if (ms >= 0) {
        goDashboard(member);
        showBadgePopup(ms);
        return;
    }
    audio.taskCompleteTone();
    msgActive = false;
    goDashboard(member);
    showCelebration(rt.bundlePoints, m.totalPoints);
}

void drawRoutine() {
    if (viewingRoutineIdx < 0 || viewingRoutineIdx >= routines.count()) { goDashboard(currentUserId); return; }
    const Routine& rt = routines.get(viewingRoutineIdx);
    bool done = routines.isDoneToday(viewingRoutineIdx, currentUserId);

    uint8_t checked = routines.stepsDone(viewingRoutineIdx, currentUserId);

    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 320, 48, C_CARD);
    Icons::drawBackArrow(8, 13, C_TXT2, 22);
    txtFit(rt.name, 38, 4, 200, 4, C_TXT);
    drawPointsTag(rt.bundlePoints, 306, 6, C_GOLD);
    char prog[24]; snprintf(prog, sizeof(prog), "%d of %d steps", done ? rt.stepCount : checked, rt.stepCount);
    txt(prog, 306, 28, 2, C_TXT2, TR_DATUM);

    // Rows fill the screen: big targets for small fingers
    int16_t rowH = routineRowH(rt.stepCount);
    for (uint8_t i = 0; i < rt.stepCount; i++) {
        int16_t y = ROUT_TOP + i * rowH;
        bool on = done || routines.stepDone(viewingRoutineIdx, currentUserId, i);
        int16_t h = rowH - 6, cb = h < 34 ? h - 6 : 28;
        tft.fillRoundRect(10, y, 300, h, 8, C_CARD);
        tft.drawRoundRect(18, y + (h - cb) / 2, cb, cb, 5, on ? C_OK : C_TXT2);
        if (on) {
            tft.fillRoundRect(20, y + (h - cb) / 2 + 2, cb - 4, cb - 4, 4, C_OK);
            Icons::drawCheckIcon(22, y + (h - cb) / 2 + 3, C_BG, cb - 8);
        }
        txtFit(rt.steps[i], 18 + cb + 12, y + (h - 16) / 2, 300 - cb - 24, 2, on ? C_TXT3 : C_TXT);
    }

    if (done) {
        txt("All done today!", 160, 446, 4, C_OK, TC_DATUM);
    } else if (checked == rt.stepCount && rt.stepCount > 0) {
        txt("All done - nice!", 160, 446, 4, C_GOLD, TC_DATUM);   // finishing in a moment
    } else {
        char hint[32]; snprintf(hint, sizeof(hint), "+%d when every step is checked", rt.bundlePoints);
        txt(hint, 160, 450, 2, C_TXT2, TC_DATUM);
    }
}

// Row height so the steps fill the area between the header and the hint.
int16_t routineRowH(uint8_t steps) {
    if (steps == 0) return ROUT_MAX_ROW;
    int16_t h = (ROUT_BOTTOM - ROUT_TOP) / steps;
    return h > ROUT_MAX_ROW ? ROUT_MAX_ROW : h;
}

void touchRoutine(uint16_t x, uint16_t y) {
    lastActivityMs = millis();
    if (viewingRoutineIdx < 0 || viewingRoutineIdx >= routines.count()) { goDashboard(currentUserId); return; }
    if (x < 60 && y < 48) {
        // Back during the finishing pause: finish now, don't drop it
        if (routineFinishAt && routineFinishIdx == viewingRoutineIdx && routineFinishMember == currentUserId) {
            routineFinishAt = 0;
            finishRoutine(viewingRoutineIdx, currentUserId);
            return;
        }
        goDashboard(currentUserId);
        return;
    }

    const Routine& rt = routines.get(viewingRoutineIdx);
    if (routines.isDoneToday(viewingRoutineIdx, currentUserId)) return;  // read-only once done

    // Toggle a step. Checking the last one finishes the routine after a
    // short pause that shows every box ticked (unticking cancels it).
    int16_t rowH = routineRowH(rt.stepCount);
    for (uint8_t i = 0; i < rt.stepCount; i++) {
        int16_t sy = ROUT_TOP + i * rowH;
        if (y >= sy && y < sy + rowH) {
            routines.toggleStep(viewingRoutineIdx, currentUserId, i);
            bool all = routines.stepsDone(viewingRoutineIdx, currentUserId) == rt.stepCount;
            if (all) {
                routineFinishAt = millis() + ROUT_FINISH_PAUSE_MS;
                routineFinishIdx = viewingRoutineIdx; routineFinishMember = currentUserId;
            } else {
                routineFinishAt = 0;
            }
            drawRoutine();
            return;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// CHILD SELF-INIT (3 steps: category + name, size, confirm)
// A child proposes a job; a parent always checks it. The +2 initiative bonus
// ("this was my own idea") is an opt-in toggle on the confirm step, off by
// default. Without it the job is a regular one-off (ADHOC) task for the child.
// ═════════════════════════════════════════════════════════════════════════════

// Id for a one-off task: unique among active tasks and outside the ranges
// recurring templates use (seed 1000s, web editor 2000+, admin 3000-22999),
// so it can never be mistaken for today's instance of a template.
uint16_t newAdhocTaskId() {
    uint16_t id = 30000 + (millis() % 20000);
    for (uint16_t tries = 0; tries < 20000; tries++, id++) {
        if (id >= 50000) id = 30000;
        bool used = false;
        for (uint8_t i = 0; i < activeTaskCount && !used; i++) used = activeTasks[i].id == id;
        if (!used) return id;
    }
    return id;
}

// "Type my own" button shared by the kid proposal and parent quick-add screens
void drawTypeOwnButton(int16_t y) {
    bool custom = selfInitCustom[0] != '\0';
    tft.fillRoundRect(15, y, 185, 26, 5, custom ? C_GOLD : C_CARD);
    tft.setTextColor(custom ? C_BG : C_TXT2); tft.setTextSize(1);
    tft.setCursor(25, y + 9);
    if (custom) {
        char shown[27];
        strlcpy(shown, selfInitCustom, sizeof(shown));   // fits the button
        tft.print(shown);
    } else {
        tft.print("Type my own...");
    }
}

void typeOwnTaskName(const char* title) {
    char buf[sizeof(selfInitCustom)];
    strlcpy(buf, selfInitName(), sizeof(buf));
    if (OnScreenKeyboard::run(title, buf, sizeof(buf), sizeof(buf) - 1, true) && buf[0])
        strlcpy(selfInitCustom, buf, sizeof(selfInitCustom));
}

void touchSelfInit(uint16_t x, uint16_t y) {
    lastActivityMs = millis();
    if (x < 60 && y < 48) { goDashboard(currentUserId); return; } // Back

    switch (selfInitStep) {
        case 0: // Category + name
            // Category taps (a typed name is kept: pick any category for it)
            for (uint8_t i = 0; i < 6; i++) {
                int16_t bx = SI_CAT_X + (i % 3) * SI_CAT_PITCH_X, by = SI_CAT_Y + (i / 3) * SI_CAT_PITCH_Y;
                if (x >= bx && x < bx + SI_CAT_W && y >= by && y < by + SI_CAT_H) {
                    selfInitDraft.category = static_cast<TaskCategory>(i);
                    selfInitNameIdx = 0;
                    drawSelfInit(); return;
                }
            }
            // Preset name taps (below categories) replace a typed name
            for (uint8_t i = 0; i < 6; i++) {
                int16_t ny = SI_NAME_Y + i * SI_NAME_PITCH;
                if (y >= ny && y < ny + SI_NAME_H) {
                    selfInitNameIdx = i;
                    selfInitCustom[0] = '\0';
                    drawSelfInit(); return;
                }
            }
            if (y >= 446 && x < 205) { typeOwnTaskName("What's the job?"); drawSelfInit(); return; }
            if (x >= 210 && y >= 446) { selfInitStep = 1; drawSelfInit(); }
            break;

        case 1: // Size
            for (uint8_t i = 0; i < 3; i++) {
                if (y >= 80 + i * 65 && y < 80 + i * 65 + 55) {
                    selfInitDraft.size = static_cast<TaskSize>(i);
                    drawSelfInit(); return;
                }
            }
            if (x < 150 && y >= 446) { selfInitStep = 0; drawSelfInit(); return; }  // Back
            if (x >= 210 && y >= 446) { selfInitStep = 2; drawSelfInit(); }
            break;

        case 2: // Confirm
            if (y >= 190 && y <= 234) {                     // "my own idea" toggle
                selfInitInitiative = !selfInitInitiative;
                drawSelfInit();
                return;
            }
            // I DID IT: most found jobs are already done, so add it and send
            // it straight to a parent. Do it later: just add it to the list.
            if (y >= SI_DIDIT_Y && y < SI_DIDIT_Y + BIG_BTN_H) {
                int8_t idx = addSelfInitJob();
                if (idx >= 0) doneNowFor(idx, currentUserId);   // -> waiting for a parent
                else          goDashboard(currentUserId);
                return;
            }
            if (y >= SI_LATER_Y && y < SI_LATER_Y + 44) {
                if (addSelfInitJob() >= 0) { audio.taskStartTone(); showMsg("Job added!", C_GOLD, 1000); }
                goDashboard(currentUserId);
                return;
            }
            if (x < 150 && y >= 440) { selfInitStep = 1; drawSelfInit(); } // Back
            break;
    }
}

// Add the job the child described; returns its index, or -1 if the list is full.
int8_t addSelfInitJob() {
    if (activeTaskCount >= MAX_ACTIVE_TASKS) { showMsg("Too many jobs today", C_WARN, 1500); return -1; }
    strlcpy(selfInitDraft.name, selfInitName(), sizeof(selfInitDraft.name));
    selfInitDraft.type = selfInitInitiative ? TaskType::SELF_INIT : TaskType::ADHOC;
    selfInitDraft.id = newAdhocTaskId();
    selfInitDraft.state = TaskState::AVAILABLE;
    selfInitDraft.requiresApproval = true;   // a parent checks kid-made jobs
    selfInitDraft.noReminderToggled = false;
    uint8_t idx = activeTaskCount;
    memcpy(&activeTasks[idx], &selfInitDraft, sizeof(Task));
    activeTaskCount++;
    backups.markTasksDirty();
    Serial.printf("[SELF-INIT] %s added '%s'%s\n", family[currentUserId].name,
                  selfInitDraft.name, selfInitInitiative ? " (own idea)" : "");
    return (int8_t)idx;
}

void drawSelfInit() {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 320, 48, C_CARD);
    Icons::drawBackArrow(8, 13, C_TXT2, 22);
    tft.setTextColor(C_TXT); tft.setTextSize(2);
    tft.setCursor(38, 14); tft.print("I Found a Job!");

    // Step dots
    for (uint8_t i = 0; i < 3; i++) {
        tft.fillCircle(140 + i * 16, 38, (i == selfInitStep) ? 4 : 3,
                        (i <= selfInitStep) ? C_PRI : C_DIM);
    }

    uint8_t ci = static_cast<uint8_t>(selfInitDraft.category);
    bool custom = selfInitCustom[0] != '\0';

    switch (selfInitStep) {
        case 0: { // Category + name
            tft.setTextColor(C_TXT2); tft.setTextSize(1);
            tft.setCursor(15, 57); tft.print("What kind of job?");
            // Categories: 3 x 2 grid (geometry shared with touchSelfInit)
            for (uint8_t i = 0; i < 6; i++) {
                int16_t bx = SI_CAT_X + (i % 3) * SI_CAT_PITCH_X, by = SI_CAT_Y + (i / 3) * SI_CAT_PITCH_Y;
                bool sel = (ci == i);
                tft.fillRoundRect(bx, by, SI_CAT_W, SI_CAT_H, 5, sel ? catC(static_cast<TaskCategory>(i)) : C_CARD);
                tft.setTextColor(sel ? C_TXT : C_TXT2); tft.setTextSize(1);
                tft.setCursor(bx + 8, by + 14); tft.print(CAT_NAMES[i]);
            }
            // Name list for selected category (or a typed name)
            tft.setTextColor(C_TXT2); tft.setTextSize(1);
            tft.setCursor(15, SI_NAME_Y - 14); tft.print("What is it? Pick one, or type your own below.");
            for (uint8_t i = 0; i < 6; i++) {
                int16_t y = SI_NAME_Y + i * SI_NAME_PITCH;
                bool sel = !custom && (selfInitNameIdx == i);
                tft.fillRoundRect(15, y, 290, SI_NAME_H, 4, sel ? C_PRI : C_CARD);
                tft.setTextColor(sel ? C_TXT : C_TXT2); tft.setTextSize(1);
                tft.setCursor(25, y + 13); tft.print(presetTaskName(ci, i));
            }
            drawTypeOwnButton(450);
            tft.fillRoundRect(210, 450, 100, 26, 5, C_PRI);
            tft.setTextColor(C_TXT); tft.setTextSize(1);
            tft.setCursor(235, 459); tft.print("NEXT >");
            break;
        }

        case 1: { // Size
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(15, 60); tft.print("How big is this job?");
            for (uint8_t i = 0; i < 3; i++) {
                int16_t y = 80 + i * 65;
                bool sel = (static_cast<uint8_t>(selfInitDraft.size) == i);
                tft.fillRoundRect(15, y, 290, 55, 8, sel ? C_PRI : C_CARD);
                tft.setTextColor(sel ? C_TXT : C_TXT2); tft.setTextSize(2);
                tft.setCursor(25, y + 8); tft.print(SIZE_NAMES[i]);
                uint8_t pts = PointTable::base(static_cast<TaskSize>(i));
                tft.setTextColor(sel ? C_TXT : C_GOLD); tft.setTextSize(2);
                tft.setCursor(240, y + 16); tft.printf("+%d", pts);
                tft.setTextColor(sel ? C_TXT : C_TXT3); tft.setTextSize(1);
                tft.setCursor(25, y + 35);
                static const char* SIZE_HINT[] = { "Quick - a few minutes", "Takes a while", "A big job" };
                tft.print(SIZE_HINT[i]);
            }
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(15, 290); tft.print("Your own idea? You can add a bonus next.");
            tft.fillRoundRect(15, 450, 120, 26, 5, C_DIM);
            tft.setTextColor(C_TXT); tft.setTextSize(1);
            tft.setCursor(45, 459); tft.print("< BACK");
            tft.fillRoundRect(210, 450, 100, 26, 5, C_PRI);
            tft.setCursor(235, 459); tft.print("NEXT >");
            break;
        }

        case 2: { // Confirm
            tft.setTextColor(C_TXT); tft.setTextSize(2);
            tft.setCursor(15, 65); tft.print("Ready to add:");

            uint8_t pts = PointTable::base(selfInitDraft.size) +
                          (selfInitInitiative ? PointTable::INITIATIVE_BONUS : 0);
            tft.fillRoundRect(15, 100, 290, 70, 8, C_CARD);
            Icons::drawCategoryIcon(22, 108, selfInitDraft.category, catC(selfInitDraft.category), 24);
            tft.setTextColor(C_TXT); tft.setTextSize(strlen(selfInitName()) > 20 ? 1 : 2);
            tft.setCursor(54, 105); tft.print(selfInitName());
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(54, 130); tft.printf("%s  |  %s  |  +%d pts",
                CAT_NAMES[ci], SIZE_NAMES[(int)selfInitDraft.size], pts);

            // Opt-in initiative bonus (off by default)
            bool on = selfInitInitiative;
            tft.fillRoundRect(15, 190, 290, 44, 8, C_CARD);
            tft.fillRoundRect(25, 200, 44, 24, 12, on ? C_OK : C_DIM);
            tft.fillCircle(on ? 59 : 35, 212, 9, on ? C_TXT : C_TXT2);
            tft.setTextColor(on ? C_GOLD : C_TXT2); tft.setTextSize(1);
            tft.setCursor(80, 202); tft.print("This was my own idea");
            tft.setTextColor(on ? C_GOLD : C_TXT3);
            tft.setCursor(80, 216);
            tft.printf("+%d bonus - a parent checks it", PointTable::INITIATIVE_BONUS);

            txt("A parent will check your work.", 15, 248, 2, C_TXT2);

            drawBigButton(SI_DIDIT_Y, "I DID IT!", C_OK);
            tft.fillRoundRect(10, SI_LATER_Y, 300, 44, 8, C_CARD);
            txt("Do it later", 160, SI_LATER_Y + 22, 2, C_TXT, MC_DATUM);

            tft.fillRoundRect(15, 440, 120, 30, 5, C_DIM);
            txt("< BACK", 75, 455, 2, C_TXT, MC_DATUM);
            break;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// PARENT QUICK-ADD (2 steps: category+name+assign, confirm)
// ═════════════════════════════════════════════════════════════════════════════

void touchQuickAdd(uint16_t x, uint16_t y) {
    lastActivityMs = millis();
    if (x < 60 && y < 48) { goDashboard(currentUserId); return; }

    switch (quickAddStep) {
        case 0: // Category + name + assign
            // Categories (3 x 2 grid)
            for (uint8_t i = 0; i < 6; i++) {
                int16_t bx = SI_CAT_X + (i % 3) * SI_CAT_PITCH_X, by = QA_CAT_Y + (i / 3) * QA_CAT_PITCH;
                if (x >= bx && x < bx + SI_CAT_W && y >= by && y < by + QA_CAT_H) {
                    selfInitDraft.category = static_cast<TaskCategory>(i);
                    selfInitNameIdx = 0; drawQuickAdd(); return;
                }
            }
            // Names (a preset replaces a typed name)
            for (uint8_t i = 0; i < 6; i++) {
                int16_t ny = QA_NAME_Y + i * QA_NAME_PITCH;
                if (y >= ny && y < ny + QA_NAME_H) {
                    selfInitNameIdx = i; selfInitCustom[0] = '\0'; drawQuickAdd(); return;
                }
            }
            // Size
            if (y >= QA_SIZE_Y && y < QA_SIZE_Y + QA_ROW_H) {
                for (uint8_t i = 0; i < 3; i++) {
                    int16_t bx = QA_SIZE_X + i * (QA_SIZE_W + 6);
                    if (x >= bx && x < bx + QA_SIZE_W) { selfInitDraft.size = (TaskSize)i; drawQuickAdd(); return; }
                }
            }
            // Who: children
            if (y >= QA_WHO_Y && y < QA_WHO_Y + QA_ROW_H) {
                uint8_t kids = 0;
                for (uint8_t i = 0; i < familyCount; i++) if (family[i].role == Role::CHILD) kids++;
                int16_t kw = kids ? (290 - (kids - 1) * 6) / kids : 290, k = 0;
                for (uint8_t i = 0; i < familyCount; i++) {
                    if (family[i].role != Role::CHILD) continue;
                    int16_t bx = 15 + k * (kw + 6);
                    if (x >= bx && x < bx + kw) { selfInitDraft.assignedTo = i; drawQuickAdd(); return; }
                    k++;
                }
            }
            // Who: Per child / First taker
            if (y >= QA_WHO2_Y && y < QA_WHO2_Y + QA_ROW_H) {
                selfInitDraft.assignedTo = x < 160 ? 254 : 255;
                drawQuickAdd(); return;
            }
            if (y >= 446 && x < 205) { typeOwnTaskName("Task name"); drawQuickAdd(); return; }
            // Create
            if (x >= 210 && y >= 446) {
                // Save task(s)
                strlcpy(selfInitDraft.name, selfInitName(), sizeof(selfInitDraft.name));
                selfInitDraft.state = TaskState::AVAILABLE;

                if (selfInitDraft.assignedTo == 254) {
                    // Create one per child
                    for (uint8_t i = 0; i < familyCount && activeTaskCount < MAX_ACTIVE_TASKS; i++) {
                        if (family[i].role != Role::CHILD) continue;
                        Task& t = activeTasks[activeTaskCount];
                        memcpy(&t, &selfInitDraft, sizeof(Task));
                        t.assignedTo = i;
                        t.id = newAdhocTaskId();
                        activeTaskCount++;
                    }
                } else {
                    if (activeTaskCount < MAX_ACTIVE_TASKS) {
                        Task& t = activeTasks[activeTaskCount];
                        memcpy(&t, &selfInitDraft, sizeof(Task));
                        t.id = newAdhocTaskId();
                        if (t.assignedTo == 255 || t.assignedTo == 254) t.assignedTo = 255;
                        activeTaskCount++;
                    }
                }
                backups.markTasksDirty();
                audio.taskCompleteTone();
                showMsg("Task created!", C_OK, 1000);
                goDashboard(currentUserId);
            }
            break;
    }
}

void drawQuickAdd() {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 320, 48, C_CARD);
    Icons::drawBackArrow(8, 13, C_TXT2, 22);
    txt("Quick Add", 38, 11, 4, C_TXT);

    uint8_t ci = static_cast<uint8_t>(selfInitDraft.category);

    // Categories: 3 x 2 grid
    for (uint8_t i = 0; i < 6; i++) {
        int16_t bx = SI_CAT_X + (i % 3) * SI_CAT_PITCH_X, by = QA_CAT_Y + (i / 3) * QA_CAT_PITCH;
        bool sel = (ci == i);
        tft.fillRoundRect(bx, by, SI_CAT_W, QA_CAT_H, 5, sel ? catC(static_cast<TaskCategory>(i)) : C_CARD);
        txt(CAT_NAMES[i], bx + SI_CAT_W / 2, by + QA_CAT_H / 2, 1, sel ? C_TXT : C_TXT2, MC_DATUM);
    }

    // Names for the category (a typed name shows on the "Type my own" button)
    for (uint8_t i = 0; i < 6; i++) {
        int16_t y = QA_NAME_Y + i * QA_NAME_PITCH;
        bool sel = !selfInitCustom[0] && (selfInitNameIdx == i);
        tft.fillRoundRect(15, y, 290, QA_NAME_H, 4, sel ? C_PRI : C_CARD);
        txt(presetTaskName(ci, i), 25, y + QA_NAME_H / 2, 1, sel ? C_TXT : C_TXT2, ML_DATUM);
    }

    // Size
    txt("Size", 15, QA_SIZE_Y + QA_ROW_H / 2, 2, C_TXT2, ML_DATUM);
    static const char* SZ[] = { "Small", "Medium", "Large" };
    for (uint8_t i = 0; i < 3; i++) {
        int16_t bx = QA_SIZE_X + i * (QA_SIZE_W + 6);
        bool sel = (uint8_t)selfInitDraft.size == i;
        tft.fillRoundRect(bx, QA_SIZE_Y, QA_SIZE_W, QA_ROW_H, 5, sel ? C_PRI : C_CARD);
        char lbl[16]; snprintf(lbl, sizeof(lbl), "%s +%d", SZ[i], PointTable::base((TaskSize)i));
        txt(lbl, bx + QA_SIZE_W / 2, QA_SIZE_Y + QA_ROW_H / 2, 1, sel ? C_TXT : C_TXT2, MC_DATUM);
    }

    // Who: each child, then Per child / First taker (First taker is the default)
    uint8_t kids = 0;
    for (uint8_t i = 0; i < familyCount; i++) if (family[i].role == Role::CHILD) kids++;
    int16_t kw = kids ? (290 - (kids - 1) * 6) / kids : 290, k = 0;
    for (uint8_t i = 0; i < familyCount; i++) {
        if (family[i].role != Role::CHILD) continue;
        int16_t bx = 15 + k * (kw + 6);
        bool sel = selfInitDraft.assignedTo == i;
        tft.fillRoundRect(bx, QA_WHO_Y, kw, QA_ROW_H, 5, sel ? C_PRI : C_CARD);
        txtFit(family[i].name, bx + 6, QA_WHO_Y + 6, kw - 12, 2, sel ? C_TXT : C_TXT2);
        k++;
    }
    const uint8_t whoVal[2] = { 254, 255 };
    const char*   whoLbl[2] = { "Per child", "First taker" };
    for (uint8_t j = 0; j < 2; j++) {
        int16_t bx = 15 + j * 148;
        bool sel = selfInitDraft.assignedTo == whoVal[j];
        tft.fillRoundRect(bx, QA_WHO2_Y, 142, QA_ROW_H, 5, sel ? C_PRI : C_CARD);
        txt(whoLbl[j], bx + 71, QA_WHO2_Y + QA_ROW_H / 2, 2, sel ? C_TXT : C_TXT2, MC_DATUM);
    }

    drawTypeOwnButton(450);

    // Create button
    tft.fillRoundRect(210, 446, 100, 30, 5, C_OK);
    txt("CREATE", 260, 461, 2, C_TXT, MC_DATUM);
}

// ═════════════════════════════════════════════════════════════════════════════
// NON-BLOCKING MESSAGE OVERLAY
// ═════════════════════════════════════════════════════════════════════════════

void showMsg(const char* msg, uint16_t color, uint16_t durationMs) {
    showMsg2(msg, "", color, durationMs);
}

void showMsg2(const char* msg, const char* sub, uint16_t color, uint16_t durationMs) {
    msgActive = true; msgBlocking = false; celebrate = false;
    msgExpireMs = millis() + durationMs;
    strlcpy(msgText, msg, sizeof(msgText));
    strlcpy(msgSub, sub, sizeof(msgSub));
    msgColor = color;
    drawMsgOverlay();
}

void drawMsgOverlay() {
    if (!msgActive) return;
    if (celebrate) { drawCelebration(); return; }
    bool hasSub = msgSub[0] != '\0';
    msgBoxY0 = MSG_BOX_Y; msgBoxY1 = MSG_BOX_Y + (hasSub ? 60 : 44);
    tft.fillRoundRect(20, MSG_BOX_Y, 280, msgBoxY1 - msgBoxY0, 8, msgColor);
    txtFit(msgText, 160 - min((int16_t)130, (int16_t)(tft.textWidth(msgText, 4) / 2)), MSG_BOX_Y + 8, 260, 4, C_TXT);
    if (hasSub) txt(msgSub, 160, MSG_BOX_Y + 38, 2, C_TXT, TC_DATUM);
}

// ── Points celebration ──
void showCelebration(uint8_t pts, uint32_t totalAfter) {
    celebrate = true; celPts = pts;
    celTo = totalAfter; celFrom = totalAfter >= pts ? totalAfter - pts : 0;
    celStep = 0; celStepMs = millis();
    msgActive = true; msgBlocking = false; msgExpireMs = millis() + 1800;
    msgText[0] = '\0'; msgSub[0] = '\0';
    drawCelebration();
}

void drawCelebration() {
    msgBoxY0 = CEL_Y; msgBoxY1 = CEL_Y + CEL_H;
    tft.fillRoundRect(24, CEL_Y, 272, CEL_H, 12, 0x18E6);
    tft.drawRoundRect(24, CEL_Y, 272, CEL_H, 12, C_GOLD);
    tft.drawRoundRect(25, CEL_Y + 1, 270, CEL_H - 2, 11, C_GOLD);

    // Big "+N": a drawn plus (Font 7 has digits only) and 7-segment digits
    char num[6]; snprintf(num, sizeof(num), "%d", celPts);
    int16_t nw = tft.textWidth(num, 7), total = 34 + 8 + nw, sx = 160 - total / 2;
    tft.fillRect(sx, CEL_Y + 37, 34, 9, C_GOLD);
    tft.fillRect(sx + 12, CEL_Y + 25, 9, 33, C_GOLD);
    txt(num, sx + 42, CEL_Y + 18, 7, C_GOLD, TL_DATUM);

    char tot[24]; snprintf(tot, sizeof(tot), "%lu total", (unsigned long)celTo);
    int16_t tw = tft.textWidth(tot, 2);
    Icons::drawInitiativeStar(160 - tw / 2 - 18, CEL_Y + 82, 14);
    txt(tot, 160 - tw / 2, CEL_Y + 80, 2, C_TXT, TL_DATUM);

    drawCelebrationBar();
}

// Bar fills from the old total to the new one over 3 steps (loop calls this).
void drawCelebrationBar() {
    uint32_t p = celFrom + (celTo - celFrom) * celStep / 3;
    drawBadgeBar(p, 44, CEL_BAR_Y, 232, 10);
    uint32_t lo; int8_t nb = nextBadge(celTo, lo);
    char line[40];
    if (nb >= 0) snprintf(line, sizeof(line), "%lu to %s",
                          (unsigned long)(MILESTONES[nb].pointThreshold - celTo), MILESTONES[nb].name);
    else         snprintf(line, sizeof(line), "All badges earned!");
    tft.fillRect(30, CEL_BAR_Y + 16, 260, 18, 0x18E6);
    txtFit(line, 160 - min((int16_t)128, (int16_t)(tft.textWidth(line, 2) / 2)), CEL_BAR_Y + 16, 256, 2, C_TXT2);
}

// The new-badge card (looked at, not tapped through).
void showBadgePopup(int8_t badge) {
    celebrate = false;
    MilestoneIcons::drawNewBadgeNotification(badge);
    msgActive = true; msgBlocking = true; msgExpireMs = millis() + 3000;
    msgBoxY0 = 0; msgBoxY1 = 480;
    audio.badgeTone();
}

// Redraw whatever screen is showing (used to clear overlays / refresh status).
void redrawCurrent() {
    switch (currentScreen) {
        case Screen::HOME:         drawHome(); break;
        case Screen::DASHBOARD:    drawDashboard(); break;
        case Screen::TASK_DETAIL:  drawTaskDetail(); break;
        case Screen::ROUTINE:      drawRoutine(); break;
        case Screen::SELF_INIT:    drawSelfInit(); break;
        case Screen::QUICK_ADD:    drawQuickAdd(); break;
        case Screen::PARENT_ADMIN: admin.drawCurrentScreen(); break;
        case Screen::MILESTONES:
            if (currentUserId < familyCount) MilestoneIcons::drawCollection(family[currentUserId]);
            else goHome();
            break;
        case Screen::PAIR_TAGS:    drawPairTags(); break;
        case Screen::DONE_PROMPT:  drawDonePrompt(); break;
        case Screen::NEWS:         drawNews(); break;
        case Screen::NO_SD:        drawNoSd(); break;
        default: break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// UTILITY
// ═════════════════════════════════════════════════════════════════════════════

bool isVisibleTask(uint8_t idx) {
    const Task& t = activeTasks[idx];
    if (t.state == TaskState::COMPLETED || t.state == TaskState::TIMED_OUT) return false;
    if (currentUserId < familyCount && family[currentUserId].role == Role::PARENT) {
        return true; // Parent sees all active tasks
    }
    // Children see their own tasks plus unclaimed "Anyone" tasks (the first
    // child to start one claims it).
    if (t.assignedTo == 255) return t.state == TaskState::AVAILABLE;
    return t.assignedTo == currentUserId;
}

// May this member scan-to-start this task? Own tasks, or unclaimed "Anyone".
bool canStartTask(const Task& t, uint8_t memberId) {
    if (t.state != TaskState::AVAILABLE) return false;
    return t.assignedTo == memberId || t.assignedTo == 255;
}

// Quick-action button on the right of a card (x CHIP_X..310). One tap does
// the obvious next thing; the rest of the card opens the task screen.
uint8_t cardChip(const Task& t, uint8_t viewer) {
    if (viewer >= familyCount) return CHIP_NONE;
    if (t.state == TaskState::AVAILABLE && canStartTask(t, viewer))
        return isTimed(t) ? CHIP_START : CHIP_DONE;
    if (t.state == TaskState::IN_PROGRESS && t.assignedTo == viewer && TaskEngine::remainingMinTime(t) <= 0)
        return CHIP_FINISH;
    // Approve in one tap only when there's nothing to judge: no bonus claim,
    // and not timed (the review screen shows how long it took).
    if (t.state == TaskState::PENDING_REVIEW && family[viewer].role == Role::PARENT &&
        !t.childClaimedNoReminder && !isTimed(t))
        return CHIP_APPROVE;
    return CHIP_NONE;
}

// Timed chores (a minimum time or a time limit) are started, then finished,
// so the time is checked and recorded. Everything else is one tap: DONE.
bool isTimed(const Task& t) { return t.minimumTimeSec > 0 || t.timeoutTimeSec > 0; }

// Build the ordered dashboard list shared by draw + touch. Whatever needs
// action from the person looking comes first:
//  - Child:  their in-progress tasks (tap to finish), routines due now,
//            tasks to start, later routines, tasks waiting on a parent,
//            routines already done today.
//  - Parent: tasks waiting for approval, tasks in progress, the rest.
uint8_t buildDashRows(DashRow* rows, uint8_t maxRows) {
    uint8_t n = 0;
    if (currentUserId >= familyCount) return 0;
    bool isParent = (family[currentUserId].role == Role::PARENT);

    auto addTasks = [&](TaskState st) {
        for (uint8_t i = 0; i < activeTaskCount && n < maxRows; i++)
            if (isVisibleTask(i) && activeTasks[i].state == st) rows[n++] = { 0, i };
    };
    // which: 0 = due now (not done), 1 = later (not done), 2 = done today
    auto addRoutines = [&](uint8_t which) {
        for (uint8_t r = 0; r < routines.count() && n < maxRows; r++) {
            if (!routines.visibleFor(r, currentUserId) || !routines.activeToday(r)) continue;
            bool done = routines.isDoneToday(r, currentUserId);
            uint8_t kind = done ? 2 : (routines.inWindow(r) ? 0 : 1);
            if (kind == which) rows[n++] = { 1, r };
        }
    };

    if (isParent) {
        addTasks(TaskState::PENDING_REVIEW);
        addTasks(TaskState::IN_PROGRESS);
        addTasks(TaskState::AVAILABLE);
    } else {
        addTasks(TaskState::IN_PROGRESS);
        addRoutines(0);
        addTasks(TaskState::AVAILABLE);
        addRoutines(1);
        addTasks(TaskState::PENDING_REVIEW);
        addRoutines(2);
        uint16_t pts;
        if (n < maxRows && doneTodayChores(currentUserId, pts) > 0) rows[n++] = { 2, 0 };
    }
    return n;
}

// Fingerprint of what the visible page shows: person, page, rows, states and
// card buttons. drawDashboard() stores it; loop() redraws when it changes
// (e.g. a timed chore's minimum passes and its check button appears), so a
// card button only ever acts on what is on screen.
uint32_t dashSignature() {
    DashRow rows[DASH_ROWS_MAX];
    uint8_t rowCount = buildDashRows(rows, DASH_ROWS_MAX);
    uint8_t startAt = taskPage * TASKS_PER_PAGE;
    uint32_t sig = 2166136261u ^ currentUserId ^ ((uint32_t)taskPage << 8) ^ ((uint32_t)rowCount << 16);
    for (uint8_t s = startAt; s < rowCount && s < startAt + TASKS_PER_PAGE; s++) {
        uint8_t st = 9, chip = 0;
        if (rows[s].kind == 0) {
            st = (uint8_t)activeTasks[rows[s].idx].state;
            chip = cardChip(activeTasks[rows[s].idx], currentUserId);
        }
        sig = (sig ^ ((uint32_t)(rows[s].kind << 12) | (rows[s].idx << 4) | st | ((uint32_t)chip << 20))) * 16777619u;
    }
    return sig;
}

uint8_t totalPages() {
    DashRow rows[DASH_ROWS_MAX];
    uint8_t n = buildDashRows(rows, DASH_ROWS_MAX);
    return max((uint8_t)1, (uint8_t)((n + TASKS_PER_PAGE - 1) / TASKS_PER_PAGE));
}

uint8_t countPending() {
    uint8_t c = 0;
    for (uint8_t i = 0; i < activeTaskCount; i++)
        if (activeTasks[i].state == TaskState::PENDING_REVIEW) c++;
    return c;
}

// ═════════════════════════════════════════════════════════════════════════════
// DRAWING — HOME
// ═════════════════════════════════════════════════════════════════════════════

void drawHome() {
    tft.fillScreen(C_BG);
    int8_t sx = display.shiftX(), sy = display.shiftY();

    tft.setTextColor(C_TXT); tft.setTextSize(3);
    tft.setCursor(55+sx, 80+sy); tft.print("CHORE");
    tft.setTextColor(C_PRI);
    tft.setCursor(55+sx, 115+sy); tft.print("STATION");

    txt("Scan your tag", 160+sx, 198+sy, 4, C_TXT2, TC_DATUM);

    Icons::drawRfidIcon(136+sx, 260+sy, rfid.isOnline() ? C_PRI : C_ERR, 48);

    // Reader problems used to show only as a 3px dot — make them obvious.
    if (!rfid.isOnline()) {
        tft.fillRoundRect(15, 318+sy, 290, 40, 6, C_ERR);
        tft.setTextColor(C_TXT); tft.setTextSize(2);
        tft.setCursor(25+sx, 323+sy); tft.print("Tag reader offline");
        tft.setTextSize(1);
        tft.setCursor(25+sx, 343+sy); tft.print("Check RC522 wiring (3.3V, GND, RST)");
    }

    // Waiting for a parent: a bar a parent can't miss, with who it's for
    uint8_t pa = countPending();
    if (pa > 0) {
        char line[64]; int16_t n = snprintf(line, sizeof(line), "%d to check:", pa);
        for (uint8_t m = 0; m < familyCount && n < (int16_t)sizeof(line) - 12; m++) {
            uint8_t c = 0;
            for (uint8_t i = 0; i < activeTaskCount; i++)
                if (activeTasks[i].state == TaskState::PENDING_REVIEW && activeTasks[i].assignedTo == m) c++;
            if (c) n += snprintf(line + n, sizeof(line) - n, " %s %d", family[m].name, c);
        }
        tft.fillRoundRect(15, 376+sy, 290, 36, 8, C_WARN);
        txtFit(line, 25+sx, 386+sy, 270, 2, C_BG);
    }

    // Chores running right now
    uint8_t ip = 0;
    for (uint8_t i = 0; i < activeTaskCount; i++)
        if (activeTasks[i].state == TaskState::IN_PROGRESS) ip++;
    if (ip > 0) {
        char line[32]; snprintf(line, sizeof(line), "%d chore%s in progress", ip, ip > 1 ? "s" : "");
        txt(line, 160+sx, 418+sy, 2, C_WARN, TC_DATUM);
    }

    // WiFi / web-editor address (parents point a phone browser here)
    if (WiFi.status() == WL_CONNECTED) {
        char url[48]; snprintf(url, sizeof(url), "Edit chores: http://%s", WiFi.localIP().toString().c_str());
        txt(url, 160+sx, 440+sy, 1, C_TXT2, TC_DATUM);
    }

    tft.drawFastHLine(0, 455, 320, C_CARD);
    tft.setTextColor(C_DIM); tft.setTextSize(1);
    tft.setCursor(15, 462);
    tft.fillCircle(200, 465, 3, rfid.isOnline() ? C_OK : C_ERR);
    tft.fillCircle(240, 465, 3, timeMan.isTimeSynced() ? C_OK : C_WARN);
    tft.setCursor(250, 462); tft.print(timeMan.isTimeSynced() ? "NTP" : "---");
}

// ═════════════════════════════════════════════════════════════════════════════
// DRAWING — DASHBOARD
// ═════════════════════════════════════════════════════════════════════════════

// ─── Text helpers ─────────────────────────────────────────────────────────────
// TFT_eSPI fonts: 1 = 6x8 GLCD, 2 = 16 px, 4 = 26 px, 7 = 7-segment digits.
// The font is passed per call and never left set: the admin, keyboard and
// other screens assume the GLCD font with setTextSize().
int16_t txt(const char* s, int16_t x, int16_t y, uint8_t font, uint16_t color, uint8_t datum) {
    tft.setTextSize(1); tft.setTextColor(color); tft.setTextDatum(datum);
    int16_t w = tft.drawString(s, x, y, font);
    tft.setTextDatum(TL_DATUM);
    return w;
}

// Draw in `font` if it fits in maxW; Font 4 falls back to Font 2; otherwise
// the small GLCD font, shortened with "..". y is the top of the text line.
int16_t txtFit(const char* s, int16_t x, int16_t y, int16_t maxW, uint8_t font, uint16_t color) {
    if (tft.textWidth(s, font) <= maxW) return txt(s, x, y, font, color, TL_DATUM);
    if (font == 4 && tft.textWidth(s, 2) <= maxW) return txt(s, x, y + 5, 2, color, TL_DATUM);
    if (font == 2 || font == 4) {
        char buf[64]; strlcpy(buf, s, sizeof(buf));
        int16_t maxChars = maxW / 6;
        if ((int16_t)strlen(buf) > maxChars && maxChars > 3) {
            buf[maxChars - 2] = '.'; buf[maxChars - 1] = '.'; buf[maxChars] = '\0';
        }
        return txt(buf, x, y + (font == 4 ? 9 : 4), 1, color, TL_DATUM);
    }
    return txt(s, x, y, font, color, TL_DATUM);
}

// Points a task is worth to whoever finishes it (initiative included).
uint8_t taskPoints(const Task& t) {
    uint8_t p = t.isOwnMess ? PointTable::ownMess(t.size) : PointTable::base(t.size);
    if (t.type == TaskType::SELF_INIT) p += PointTable::INITIATIVE_BONUS;
    return p;
}

// Gold "+4" with a star, right-aligned at xr.
void drawPointsTag(uint8_t pts, int16_t xr, int16_t y, uint16_t color) {
    char p[8]; snprintf(p, sizeof(p), "+%d", pts);
    int16_t w = tft.textWidth(p, 2);
    txt(p, xr, y, 2, color, TR_DATUM);
    Icons::drawInitiativeStar(xr - w - 14, y + 2, 12);
}

// Next badge above `points` (-1 = all earned), and the threshold before it.
int8_t nextBadge(uint32_t points, uint32_t& prevThreshold) {
    prevThreshold = 0;
    for (uint8_t i = 0; i < MILESTONE_COUNT; i++) {
        if (points < MILESTONES[i].pointThreshold) return i;
        prevThreshold = MILESTONES[i].pointThreshold;
    }
    return -1;
}

uint8_t badgesEarned(uint32_t points) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MILESTONE_COUNT; i++) if (points >= MILESTONES[i].pointThreshold) n++;
    return n;
}

// Bar under the badge line: how far from the last badge to the next.
void drawBadgeBar(uint32_t points, int16_t x, int16_t y, int16_t w, int16_t h) {
    uint32_t lo; int8_t nb = nextBadge(points, lo);
    tft.fillRoundRect(x, y, w, h, h / 2, C_BG);
    int16_t fw = w;
    if (nb >= 0) {
        uint32_t hi = MILESTONES[nb].pointThreshold;
        fw = (int16_t)((uint32_t)w * (points - lo) / (hi - lo));
    }
    if (fw > 0) tft.fillRoundRect(x, y, fw < h ? h : fw, h, h / 2, C_GOLD);
}

// ── Child header: name + points, badge progress, today + streak ──
void drawChildHeader(const FamilyMember& user) {
    uint8_t me = currentUserId;

    // Row 1 (0-44): name, and the point total in gold
    tft.fillRect(0, 0, 320, 44, C_CARD);
    char pts[12]; snprintf(pts, sizeof(pts), "%lu", (unsigned long)user.totalPoints);
    int16_t pw = tft.textWidth(pts, 4);
    txt(pts, 308, 10, 4, C_GOLD, TR_DATUM);
    Icons::drawInitiativeStar(308 - pw - 26, 12, 22);
    txtFit(user.name, 12, 10, 308 - pw - 34 - 12, 4, C_TXT);

    // Row 2 (48-82): badges earned + progress to the next one (tap = badges)
    tft.fillRoundRect(6, 48, 308, 34, 6, C_CARD);
    uint8_t earned = badgesEarned(user.totalPoints);
    tft.fillCircle(26, 65, 12, earned ? C_GOLD : C_DIM);
    char ec[4]; snprintf(ec, sizeof(ec), "%d", earned);
    txt(ec, 26, 66, 2, C_BG, MC_DATUM);
    uint32_t lo; int8_t nb = nextBadge(user.totalPoints, lo);
    if (nb >= 0) {
        char line[40];
        snprintf(line, sizeof(line), "%lu to %s",
                 (unsigned long)(MILESTONES[nb].pointThreshold - user.totalPoints), MILESTONES[nb].name);
        txtFit(line, 46, 51, 236, 2, C_TXT);
        drawBadgeBar(user.totalPoints, 46, 71, 236, 6);
    } else {
        txt("All badges earned!", 46, 57, 2, C_GOLD, TL_DATUM);
    }
    txt(">", 302, 57, 2, C_TXT2, TR_DATUM);

    // Row 3 (86-128): today's goal as segments (filled = done, outlined =
    // waiting for a parent), how it's going, and the streak
    uint8_t done = scheduler.doneToday(me), pend = scheduler.pendingToday(me);
    uint8_t goal = user.dailyGoal ? user.dailyGoal : 1;
    int16_t segGap = 3, segW = (166 - (goal - 1) * segGap) / goal;
    if (segW < 3) segW = 3;
    for (uint8_t k = 0; k < goal; k++) {
        int16_t sx = 12 + k * (segW + segGap);
        if (k < done)             tft.fillRoundRect(sx, 90, segW, 14, 3, C_OK);
        else if (k < done + pend) { tft.fillRoundRect(sx, 90, segW, 14, 3, C_BG);
                                    tft.drawRoundRect(sx, 90, segW, 14, 3, C_OK);
                                    tft.drawRoundRect(sx + 1, 91, segW - 2, 12, 2, C_OK); }
        else                      tft.fillRoundRect(sx, 90, segW, 14, 3, C_CARD);
    }
    Trajectory traj = computeTrajectory(done + pend, user.dailyGoal, timeMan.currentHour(), 7, 21);
    uint16_t tc; const char* tl;
    switch (traj) {
        case Trajectory::AHEAD:    tc = C_OK;   tl = "Ahead!";       break;
        case Trajectory::ON_TRACK: tc = C_PRI;  tl = "On track";     break;
        default:                   tc = C_WARN; tl = "A bit behind"; break;
    }
    char today[24]; snprintf(today, sizeof(today), "%d of %d today", done + pend, user.dailyGoal);
    int16_t w = txt(today, 12, 108, 2, C_TXT, TL_DATUM);
    txt(tl, 12 + w + 8, 108, 2, tc, TL_DATUM);

    // Streak: days in a row, and how far to the next bonus
    if (user.currentStreak > 0) {
        Icons::drawStreakFire(190, 88, user.currentStreak, 20);
        char d[16]; snprintf(d, sizeof(d), "%d day%s", user.currentStreak, user.currentStreak == 1 ? "" : "s");
        txt(d, 214, 88, 2, C_GOLD, TL_DATUM);
        uint8_t nextAt = user.currentStreak < 5 ? 5 : user.currentStreak < 10 ? 10 : user.currentStreak < 30 ? 30 : 0;
        if (nextAt) {
            uint8_t bonus = nextAt == 5 ? PointTable::STREAK_BONUS_5_DAYS
                          : nextAt == 10 ? PointTable::STREAK_BONUS_10_DAYS : PointTable::STREAK_BONUS_30_DAYS;
            uint8_t left = nextAt - user.currentStreak;
            char b[24]; snprintf(b, sizeof(b), "%d day%s to +%d", left, left == 1 ? "" : "s", bonus);
            txt(b, 214, 108, 2, C_TXT2, TL_DATUM);
        }
    } else {
        Icons::drawStreakFire(190, 88, 0, 20);
        txt("Start a streak!", 214, 96, 2, C_TXT2, TL_DATUM);
    }
}

// ── Parent header: name, Admin, each child's day ──
void drawParentHeader(const FamilyMember& user) {
    tft.fillRect(0, 0, 320, 55, C_CARD);
    txtFit(user.name, 12, 14, 220, 4, C_TXT);
    Icons::drawGearIcon(275, 8, C_TXT3, 24);
    if (ota.updateAvailable()) txt("Update!", 287, 36, 1, C_WARN, TC_DATUM);   // Admin > System Info
    else                       txt("Admin", 287, 36, 1, C_TXT2, TC_DATUM);

    tft.fillRoundRect(10, 62, 300, 50, 8, C_CARD);
    txt("FAMILY TODAY", 18, 66, 1, C_TXT2, TL_DATUM);
    uint8_t kids = 0;
    for (uint8_t i = 0; i < familyCount; i++) if (family[i].role == Role::CHILD) kids++;
    int16_t colW = kids > 3 ? 290 / kids : 100;
    int16_t fx = 18;
    for (uint8_t i = 0; i < familyCount; i++) {
        if (family[i].role != Role::CHILD) continue;
        txtFit(family[i].name, fx, 78, colW - 8, 2, C_TXT);
        char d[16]; snprintf(d, sizeof(d), "%d/%d done", scheduler.doneToday(i) + scheduler.pendingToday(i), family[i].dailyGoal);
        txt(d, fx, 96, 1, C_TXT2, TL_DATUM);
        fx += colW;
    }
    uint8_t pa = countPending();
    if (pa > 0) {
        char c[24]; snprintf(c, sizeof(c), "%d TO CHECK", pa);
        txt(c, 15, 114, 2, C_WARN, TL_DATUM);
    } else {
        txt("ALL CHORES", 15, 117, 1, C_TXT2, TL_DATUM);
    }
    tft.drawFastHLine(15, 130, 290, C_CARD);
}

// ── Cards (44 px tall; title Font 2 on top, one small line under it) ──
void drawRoutineCard(uint8_t r, int16_t y) {
    const Routine& rt = routines.get(r);
    bool done = routines.isDoneToday(r, currentUserId);
    bool now  = routines.inWindow(r);
    uint16_t accent = done ? C_OK : (now ? C_GOLD : C_PRI);

    tft.fillRoundRect(10, y, 300, 44, 6, C_CARD);
    tft.fillRect(10, y, 4, 44, accent);
    if (done) Icons::drawCheckIcon(16, y + 12, C_OK, 18);
    else      Icons::drawStreakFire(18, y + 12, now ? 1 : 0, 16);

    txtFit(rt.name, 40, y + 4, 196, 2, done ? C_TXT3 : C_TXT);
    drawPointsTag(rt.bundlePoints, 304, y + 4, done ? C_TXT3 : C_GOLD);

    char line[40];
    uint8_t partial = done ? 0 : routines.stepsDone(r, currentUserId);
    if (done)         { txt("Done today", 40, y + 28, 1, C_OK, TL_DATUM); return; }
    if (partial)      snprintf(line, sizeof(line), "Routine - %d of %d steps", partial, rt.stepCount);
    else              snprintf(line, sizeof(line), "Routine - %d steps", rt.stepCount);
    int16_t w = txt(line, 40, y + 28, 1, C_TXT2, TL_DATUM);
    if (now) txt("Time for this now!", 40 + w + 8, y + 28, 1, C_GOLD, TL_DATUM);
}

void drawTaskCard(uint8_t i, int16_t y, bool isParent) {
    const Task& t = activeTasks[i];
    uint16_t cc = catC(t.category);

    // Cards that need the viewer to act stand out: a child's running
    // task (finish it), a parent's pending approval (review it).
    bool actRun     = !isParent && t.state == TaskState::IN_PROGRESS;
    bool actApprove =  isParent && t.state == TaskState::PENDING_REVIEW;
    uint16_t bg = actRun ? 0x4140 : actApprove ? 0x10A8 : C_CARD;
    tft.fillRoundRect(10, y, 300, 44, 6, bg);
    if (actRun || actApprove) {
        uint16_t edge = actRun ? C_WARN : C_PRI;
        tft.drawRoundRect(10, y, 300, 44, 6, edge);
        tft.drawRoundRect(11, y + 1, 298, 42, 5, edge);
    }
    tft.fillRect(10, y, 4, 44, cc);
    Icons::drawCategoryIcon(16, y + 6, t.category, cc, 16);

    uint8_t chip = cardChip(t, currentUserId);
    int16_t right = chip ? CHIP_X - 6 : 304;              // points tag sits left of a chip
    char ptxt[8]; snprintf(ptxt, sizeof(ptxt), "+%d", taskPoints(t));
    txtFit(t.name, 38, y + 4, right - tft.textWidth(ptxt, 2) - 18 - 38, 2, C_TXT);
    drawPointsTag(taskPoints(t), right, y + 4, C_GOLD);
    if (chip) {
        uint16_t cbg = chip == CHIP_APPROVE ? C_WARN : C_OK;
        tft.fillRoundRect(CHIP_X, y + 4, 304 - CHIP_X, 36, 6, cbg);
        if (chip == CHIP_START)        txt("START", (CHIP_X + 304) / 2, y + 22, 2, C_TXT, MC_DATUM);
        else if (chip == CHIP_APPROVE) txt("OK", (CHIP_X + 304) / 2, y + 22, 4, C_BG, MC_DATUM);
        else                           Icons::drawCheckIcon((CHIP_X + 304) / 2 - 12, y + 10, C_TXT, 24);
    }

    // One small line: what to do next, or who it's for
    char line[48] = "";
    uint16_t lc = C_TXT2;
    const char* who = t.assignedTo < familyCount ? family[t.assignedTo].name : "First taker";
    if (t.state == TaskState::IN_PROGRESS) {
        char el[12]; TaskEngine::formatElapsed(t.startedAtMs, el, sizeof(el));
        int32_t rem = TaskEngine::remainingMinTime(t);
        lc = C_WARN;
        if (!actRun)      snprintf(line, sizeof(line), "%s - in progress %s", who, el);
        else if (rem > 0) snprintf(line, sizeof(line), "%s - keep going, %ldm %lds left", el, (long)(rem / 60), (long)(rem % 60));
        else              snprintf(line, sizeof(line), "%s - done? tap the check", el);
    } else if (t.state == TaskState::PENDING_REVIEW) {
        if (isParent) { lc = C_TXT; snprintf(line, sizeof(line), "%s - %s", who,
                            t.childClaimedNoReminder ? "TAP to review (bonus claim)" : "TAP to approve"); }
        else          snprintf(line, sizeof(line), "Waiting for a parent to check");
    } else if (isParent || t.assignedTo == 255) {
        snprintf(line, sizeof(line), "%s", who);
    } else {
        snprintf(line, sizeof(line), isTimed(t) ? "Timed - START, then scan back" : "Did it? Tap the check");
    }
    txt(line, 38, y + 28, 1, lc, TL_DATUM);
}

// Today's finished chores, as one summary row at the end of a child's list.
uint8_t doneTodayChores(uint8_t m, uint16_t& pts) {
    uint8_t n = 0; pts = 0;
    for (uint8_t i = 0; i < activeTaskCount; i++) {
        const Task& t = activeTasks[i];
        if (t.assignedTo != m || t.state != TaskState::COMPLETED || t.doneEarlierDay) continue;
        n++;
        pts += taskPoints(t);
        if (t.type != TaskType::SELF_INIT && t.noReminderApproved) pts += PointTable::NO_REMINDER_BONUS;
    }
    return n;
}

void drawDoneTodayRow(int16_t y) {
    uint16_t pts; uint8_t n = doneTodayChores(currentUserId, pts);
    tft.fillRoundRect(10, y, 300, 44, 6, C_BG);
    tft.drawRoundRect(10, y, 300, 44, 6, C_CARD);
    Icons::drawCheckIcon(16, y + 12, C_OK, 18);
    char line[32]; snprintf(line, sizeof(line), "Done today: %d chore%s", n, n == 1 ? "" : "s");
    txt(line, 40, y + 13, 2, C_TXT2, TL_DATUM);
    drawPointsTag((uint8_t)(pts > 255 ? 255 : pts), 304, y + 13, C_OK);
}

void drawDashboard() {
    if (currentUserId >= familyCount) { goHome(); return; }
    const FamilyMember& user = family[currentUserId];
    bool isParent = (user.role == Role::PARENT);

    tft.fillScreen(C_BG);

    if (isParent) drawParentHeader(user);
    else          drawChildHeader(user);

    uint8_t tp = totalPages();

    // ── Cards (paged): routines + tasks (+ "done today"), order from buildDashRows ──
    DashRow rows[DASH_ROWS_MAX];
    uint8_t rowCount = buildDashRows(rows, DASH_ROWS_MAX);

    int16_t yPos = LIST_Y;
    uint8_t startAt = taskPage * TASKS_PER_PAGE;
    uint8_t shown = 0;

    // A different list under the finger (other person, page, a chore finished,
    // or a card button appearing) arms the card-button guard.
    uint32_t sig = dashSignature();
    if (sig != lastListSig) { lastListSig = sig; chipGuardUntil = millis() + CHIP_GUARD_MS; }

    for (uint8_t s = startAt; s < rowCount && shown < TASKS_PER_PAGE; s++) {
        if (rows[s].kind == 1)      drawRoutineCard(rows[s].idx, yPos);
        else if (rows[s].kind == 2) drawDoneTodayRow(yPos);
        else                        drawTaskCard(rows[s].idx, yPos, isParent);
        yPos += ROW_H;
        shown++;
    }

    if (shown == 0 && taskPage == 0) {
        txt("All caught up!", 160, 210, 4, C_TXT2, TC_DATUM);
        Icons::drawCheckIcon(142, 250, C_OK, 36);
    }

    // Pager, right under the cards: big Back / More buttons and "Page 1 of 3"
    if (tp > 1) {
        bool hasPrev = taskPage > 0, hasNext = taskPage < tp - 1;
        tft.fillRoundRect(10, PAGER_Y, 100, PAGER_H, 8, hasPrev ? C_PRI : C_CARD);
        tft.setTextColor(hasPrev ? C_TXT : C_DIM); tft.setTextSize(2);
        tft.setCursor(24, PAGER_Y + 14); tft.print("< Back");
        tft.fillRoundRect(210, PAGER_Y, 100, PAGER_H, 8, hasNext ? C_PRI : C_CARD);
        tft.setTextColor(hasNext ? C_TXT : C_DIM);
        tft.setCursor(224, PAGER_Y + 14); tft.print("More >");

        char pg[16]; snprintf(pg, sizeof(pg), "Page %d of %d", taskPage + 1, tp);
        tft.setTextColor(C_TXT2); tft.setTextSize(1);
        tft.setCursor(160 - (int16_t)strlen(pg) * 3, PAGER_Y + 8); tft.print(pg);
        int16_t dx = 160 - (tp - 1) * 7;
        for (uint8_t p = 0; p < tp; p++)
            tft.fillCircle(dx + p * 14, PAGER_Y + 30, 4, p == taskPage ? C_PRI : C_DIM);
    }

    // ── Bottom row ──
    // "+ Job": a child adds a job they found, a parent adds a quick one
    tft.fillRoundRect(JOB_BTN_X, 440, 310 - JOB_BTN_X, 32, 6, isParent ? C_PRI : C_GOLD);
    txt("+ Job", (JOB_BTN_X + 310) / 2, 456, 2, isParent ? C_TXT : C_BG, MC_DATUM);

    // Bye (padlock, bottom-left): ends this person's session
    tft.fillRoundRect(10, 440, 88, 32, 6, C_CARD);
    tft.drawRoundRect(10, 440, 88, 32, 6, C_TXT3);
    tft.drawRoundRect(22, 447, 9, 12, 4, C_TXT2);        // padlock shackle
    tft.drawRoundRect(23, 448, 7, 10, 3, C_TXT2);
    tft.fillRoundRect(20, 455, 13, 10, 2, C_TXT2);       // padlock body (covers shackle ends)
    txt("Bye", 62, 456, 4, C_TXT, MC_DATUM);

    if (msgActive) drawMsgOverlay();   // a redraw under a message keeps it visible
}

// ═════════════════════════════════════════════════════════════════════════════
// DRAWING — TASK DETAIL (child sees info + actions, parent sees approval)
// ═════════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════════
// DONE PROMPT — after a scan, offer to finish the chore that's running
// ═════════════════════════════════════════════════════════════════════════════

bool promptTaskValid() {
    return promptTaskIdx >= 0 && promptTaskIdx < activeTaskCount &&
           activeTasks[promptTaskIdx].state == TaskState::IN_PROGRESS &&
           activeTasks[promptTaskIdx].assignedTo == currentUserId;
}

void drawDonePrompt() {
    if (!promptTaskValid() || currentUserId >= familyCount) { goDashboard(currentUserId); return; }
    const Task& task = activeTasks[promptTaskIdx];

    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 320, 48, C_CARD);
    txtFit(family[currentUserId].name, 15, 11, 290, 4, C_TXT);

    txt("Done with", 15, 62, 2, C_TXT2);
    Icons::drawCategoryIcon(15, 94, task.category, catC(task.category), 28);
    char q[52]; snprintf(q, sizeof(q), "%s?", task.name);
    txtFit(q, 52, 95, 258, 4, C_TXT);

    char el[16]; TaskEngine::formatElapsed(task.startedAtMs, el, sizeof(el));
    char st[32]; snprintf(st, sizeof(st), "Started %s ago", el);
    txt(st, 15, 150, 2, C_TXT2);

    int32_t rem = TaskEngine::remainingMinTime(task);
    if (rem > 0) {
        txt("Keep going!", 15, 196, 4, C_WARN);
        char left[48];
        snprintf(left, sizeof(left), "%ldm %lds left before you can finish.", (long)(rem / 60), (long)(rem % 60));
        txt(left, 15, 230, 2, C_TXT2);
        tft.fillRoundRect(10, PROMPT_NO_Y, 300, PROMPT_NO_H, 8, C_CARD);
        txt("OK", 160, PROMPT_NO_Y + PROMPT_NO_H / 2, 4, C_TXT, MC_DATUM);
        return;
    }

    if (task.type != TaskType::SELF_INIT) {      // initiative already includes this bonus
        bool on = task.noReminderToggled;
        tft.fillRoundRect(10, PROMPT_TOGGLE_Y, 300, 44, 8, C_CARD);
        int16_t tx = 20, ty = PROMPT_TOGGLE_Y + 10;
        tft.fillRoundRect(tx, ty, 44, 24, 12, on ? C_OK : C_DIM);
        tft.fillCircle(on ? tx + 34 : tx + 10, ty + 12, 9, on ? C_TXT : C_TXT2);
        txt("I did this on my own", 74, PROMPT_TOGGLE_Y + 5, 2, on ? C_OK : C_TXT);
        txt(on ? "+1 bonus (a parent checks)" : "Nobody reminded me? Tap for +1", 74, PROMPT_TOGGLE_Y + 25, 1, on ? C_GOLD : C_TXT2);
    }

    drawBigButton(PROMPT_YES_Y, "YES, DONE!", C_OK);
    tft.fillRoundRect(10, PROMPT_NO_Y, 300, PROMPT_NO_H, 8, C_CARD);
    txt("Not yet", 160, PROMPT_NO_Y + PROMPT_NO_H / 2, 4, C_TXT2, MC_DATUM);
}

void touchDonePrompt(uint16_t x, uint16_t y) {
    lastActivityMs = millis();
    if (millis() - promptOpenedMs < TAP_GUARD_MS) return;   // the tap that opened it (e.g. on the news card)
    if (!promptTaskValid()) { goDashboard(currentUserId); return; }
    Task& task = activeTasks[promptTaskIdx];

    if (y >= PROMPT_NO_Y && y < PROMPT_NO_Y + PROMPT_NO_H) { goDashboard(currentUserId); return; }
    if (TaskEngine::remainingMinTime(task) > 0) return;

    if (task.type != TaskType::SELF_INIT && y >= PROMPT_TOGGLE_Y && y < PROMPT_TOGGLE_Y + 44) {
        task.noReminderToggled = !task.noReminderToggled;
        drawDonePrompt();
        return;
    }
    if (y >= PROMPT_YES_Y && y < PROMPT_YES_Y + BIG_BTN_H) finishTaskFor(promptTaskIdx, currentUserId);
}

// ═════════════════════════════════════════════════════════════════════════════
// NEWS — "While you were away": one card on the next scan, then cleared
// ═════════════════════════════════════════════════════════════════════════════

void drawNews() {
    if (currentUserId >= familyCount) { goHome(); return; }
    const AwayNews& n = news[currentUserId];

    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 320, 48, C_CARD);
    txtFit(family[currentUserId].name, 15, 11, 290, 4, C_TXT);

    txt("While you were away", 15, 62, 4, C_TXT2);

    int16_t y = 104;
    auto line = [&](uint16_t color, const char* text, const char* sub) {
        tft.fillRoundRect(10, y, 300, sub[0] ? 52 : 36, 8, C_CARD);
        tft.fillRect(10, y, 5, sub[0] ? 52 : 36, color);
        txt(text, 24, y + 6, 2, color);
        if (sub[0]) txtFit(sub, 24, y + 28, 280, 2, C_TXT);
        y += (sub[0] ? 52 : 36) + 8;
    };
    char a[32], b[48];
    if (n.approved) {
        snprintf(a, sizeof(a), "+%d points!", n.points);
        snprintf(b, sizeof(b), "A parent checked %d job%s", n.approved, n.approved > 1 ? "s" : "");
        line(C_GOLD, a, b);
    }
    if (n.badge >= 0 && n.badge < (int8_t)MILESTONE_COUNT) {
        snprintf(b, sizeof(b), "%s", MILESTONES[n.badge].name);
        line(C_GOLD, "New badge!", b);
    }
    if (n.streakBonus) {
        snprintf(a, sizeof(a), "Streak bonus +%d", n.streakBonus);
        line(C_WARN, a, "");
    }
    if (n.redos) {
        if (n.redos > 1) snprintf(b, sizeof(b), "%s and %d more", n.redoName, n.redos - 1);
        else             snprintf(b, sizeof(b), "%s", n.redoName);
        line(C_ERR, "Redo:", b);
    }
    if (n.timedOut) {
        if (n.timedOut > 1) snprintf(b, sizeof(b), "%s and %d more - back on your list", n.timedOutName, n.timedOut - 1);
        else                snprintf(b, sizeof(b), "%s - back on your list", n.timedOutName);
        line(C_WARN, "Time ran out", b);
    }

    drawBigButton(NEWS_OK_Y, "OK", C_PRI);
}

// Any tap moves on: it's a card to look at, not a menu.
void touchNews(uint16_t, uint16_t) { continueAfterNews(); }

void continueAfterNews() {
    uint8_t m = currentUserId;
    clearNews(m);
    if (m >= familyCount) { goHome(); return; }
    enterPerson(m);
}

// Full-width action button (START / I'M DONE): big enough to hit first try.
void drawBigButton(int16_t y, const char* label, uint16_t color) {
    tft.fillRoundRect(10, y, 300, BIG_BTN_H, 10, color);
    txt(label, 160, y + BIG_BTN_H / 2, 4, C_TXT, MC_DATUM);
}

void drawTaskDetail() {
    if (viewingTaskIdx < 0 || viewingTaskIdx >= activeTaskCount) { goDashboard(currentUserId); return; }
    const Task& task = activeTasks[viewingTaskIdx];
    bool isParent = (currentUserId < familyCount && family[currentUserId].role == Role::PARENT);
    uint16_t cc = catC(task.category);

    tft.fillScreen(C_BG);

    // Header
    tft.fillRect(0, 0, 320, 48, C_CARD);
    Icons::drawBackArrow(8, 13, C_TXT2, 22);
    tft.setTextColor(C_TXT); tft.setTextSize(1);
    tft.setCursor(38, 6); tft.print("TASK");
    if (task.assignedTo < familyCount) {
        tft.setTextColor(C_TXT3); tft.setCursor(38, 22);
        tft.print(family[task.assignedTo].name);
    }

    // Task identity
    Icons::drawCategoryIcon(15, 58, task.category, cc, 28);
    // Big title; long names step down to smaller text instead of wrapping
    txtFit(task.name, 50, 54, 260, 4, C_TXT);
    txt(CAT_NAMES[(int)task.category], 50, 84, 1, cc);

    // State + timing
    tft.fillRoundRect(10, 100, 300, 80, 8, C_CARD);
    Icons::drawStateIcon(20, 108, task.state, 24);
    const char* sn[]={"Ready","In Progress","Needs Approval","Completed","Timed Out"};
    uint16_t sc[]={C_DIM,C_WARN,C_PRI,C_OK,C_ERR};
    tft.setTextColor(sc[(int)task.state]); tft.setTextSize(2);
    tft.setCursor(52, 108); tft.print(sn[(int)task.state]);

    if (task.state == TaskState::IN_PROGRESS) {
        char el[16]; TaskEngine::formatElapsed(task.startedAtMs, el, sizeof(el));
        tft.setTextColor(C_TXT3); tft.setTextSize(1);
        tft.setCursor(20, 140); tft.print("Elapsed:");
        tft.setTextColor(C_WARN); tft.setTextSize(2);
        tft.setCursor(90, 137); tft.print(el);
        int32_t rem = TaskEngine::remainingMinTime(task);
        if (rem > 0) {
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(20, 162); tft.printf("Min: %ldm %lds left", (long)(rem / 60), (long)(rem % 60));
        } else if (task.minimumTimeSec > 0) {
            tft.setTextColor(C_OK); tft.setTextSize(1);
            tft.setCursor(20, 162); tft.print("Ready to complete!");
        }
    }

    // Points
    tft.fillRoundRect(10, 190, 300, 45, 8, C_CARD);
    uint8_t pts = task.isOwnMess ? PointTable::ownMess(task.size) : PointTable::base(task.size);
    tft.setTextColor(C_TXT3); tft.setTextSize(1);
    tft.setCursor(20, 197); tft.print("POINTS");
    tft.setTextColor(C_TXT); tft.setTextSize(2);
    tft.setCursor(80, 194); tft.printf("%d", pts);
    tft.setTextColor(C_TXT3); tft.setTextSize(1);
    tft.setCursor(105, 199); tft.print("base");
    if (task.type == TaskType::SELF_INIT) {
        tft.setTextColor(C_GOLD); tft.setCursor(20, 218);
        tft.printf("+%d initiative", PointTable::INITIATIVE_BONUS);
    } else if (task.noReminderToggled || task.noReminderApproved) {
        tft.setTextColor(C_OK); tft.setCursor(20, 218);
        tft.printf("+%d no-reminder", PointTable::NO_REMINDER_BONUS);
    }

    bool startable = canStartTask(task, currentUserId);
    bool running   = currentUserId == task.assignedTo && task.state == TaskState::IN_PROGRESS;

    // Timed chore waiting on a parent: how long it took
    if (task.state == TaskState::PENDING_REVIEW && isTimed(task) &&
        task.startedAtMs && task.completedAtMs) {
        char took[16];
        uint32_t secs = (task.completedAtMs - task.startedAtMs) / 1000;
        if (secs < 60)        snprintf(took, sizeof(took), "%lus", (unsigned long)secs);
        else if (secs < 3600) snprintf(took, sizeof(took), "%lum %lus", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        else                  snprintf(took, sizeof(took), "%luh %lum", (unsigned long)(secs / 3600), (unsigned long)((secs % 3600) / 60));
        tft.setTextColor(C_TXT3); tft.setTextSize(1);
        tft.setCursor(20, 140); tft.printf("Took %s", took);
    }

    // ── No-reminder toggle (while running, or before a one-tap DONE) ──
    if (task.type != TaskType::SELF_INIT && (running || (startable && !isTimed(task)))) {
        bool on = task.noReminderToggled;
        tft.fillRoundRect(10, TOGGLE_Y, 300, 44, 8, C_CARD);
        int16_t tx = 20, ty = TOGGLE_Y + 10;
        tft.fillRoundRect(tx, ty, 44, 24, 12, on ? C_OK : C_DIM);
        tft.fillCircle(on ? tx+34 : tx+10, ty+12, 9, on ? C_TXT : C_TXT2);
        tft.setTextColor(on ? C_OK : C_TXT3); tft.setTextSize(1);
        tft.setCursor(74, 257); tft.print("I did this on my own");
        tft.setTextColor(on ? C_GOLD : C_DIM);
        tft.setCursor(74, 271); tft.print(on ? "+1 bonus (parent verifies)" : "Toggle for bonus");
    }

    // ── Claim display (PENDING_REVIEW) ──
    if (task.state == TaskState::PENDING_REVIEW && task.childClaimedNoReminder) {
        tft.fillRoundRect(10, 245, 300, 30, 8, C_CARD);
        Icons::drawInitiativeStar(18, 252, 14);
        tft.setTextColor(C_GOLD); tft.setTextSize(1);
        tft.setCursor(38, 255); tft.print("Claims: did this on my own");
    }

    // ── Context-sensitive bottom area ──
    int16_t ay = 310;

    if (startable) {
        if (isTimed(task)) drawBigButton(DETAIL_BTN_Y, "START", C_OK);
        else               drawBigButton(DETAIL_BTN_Y, "DONE!", C_OK);
    }
    else if (running) {
        int32_t rem = TaskEngine::remainingMinTime(task);
        if (rem > 0) {
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(40, ay); tft.print("Keep going - not enough time yet");
        } else {
            drawBigButton(DETAIL_BTN_Y, "I'M DONE", C_OK);
        }
        tft.fillRoundRect(10, PUTBACK_Y, 300, PUTBACK_H, 8, C_CARD);
        tft.setTextColor(C_TXT2); tft.setTextSize(1);
        tft.setCursor(70, PUTBACK_Y + 16); tft.print("Stop - put it back on the list");
    }
    else if (task.state == TaskState::PENDING_REVIEW && !isParent) {
        tft.setTextColor(C_PRI); tft.setTextSize(1);
        tft.setCursor(35, ay); tft.print("Waiting for a parent to review");
    }
    else if (task.state == TaskState::PENDING_REVIEW && isParent) {
        // PARENT REVIEW: big APPROVE, then (if claimed) approve without the
        // bonus, then Redo, which asks for a second tap.
        drawBigButton(DETAIL_BTN_Y, "APPROVE", C_OK);
        int16_t redoY = REVIEW_NB_Y;
        if (task.childClaimedNoReminder) {
            tft.fillRoundRect(10, REVIEW_NB_Y, 300, REVIEW_BTN_H, 8, C_WARN);
            txt("Approve, no bonus", 160, REVIEW_NB_Y + REVIEW_BTN_H / 2, 2, C_BG, MC_DATUM);
            redoY = REVIEW_NB_Y + REVIEW_BTN_H + 8;
        }
        bool armed = redoArmedIdx == viewingTaskIdx && millis() - redoArmedMs < REDO_CONFIRM_MS;
        tft.fillRoundRect(10, redoY, 300, REVIEW_BTN_H, 8, armed ? C_ERR : C_CARD);
        tft.drawRoundRect(10, redoY, 300, REVIEW_BTN_H, 8, C_ERR);
        txt(armed ? "Tap again to send back" : "Redo", 160, redoY + REVIEW_BTN_H / 2, 2,
            armed ? C_TXT : C_ERR, MC_DATUM);
    }
    else if (task.state == TaskState::TIMED_OUT) {
        tft.setTextColor(C_ERR); tft.setTextSize(1);
        tft.setCursor(70, ay); tft.print("This task timed out.");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// NO SD CARD — loop() retries the mount and restarts once the card is in
// ═════════════════════════════════════════════════════════════════════════════

void drawNoSd() {
    tft.fillScreen(C_BG);
    tft.setTextColor(C_ERR); tft.setTextSize(3);
    tft.setCursor(40, 120); tft.print("No SD card");
    tft.setTextColor(C_TXT2); tft.setTextSize(2);
    tft.setCursor(20, 190); tft.print("Insert the CHORES");
    tft.setCursor(20, 215); tft.print("card (FAT32) and");
    tft.setCursor(20, 240); tft.print("wait a moment.");
    tft.setTextColor(C_TXT3); tft.setTextSize(1);
    tft.setCursor(20, 300); tft.print("The station restarts by itself once");
    tft.setCursor(20, 314); tft.print("the card is readable.");
    tft.setCursor(20, 340);
    tft.printf("Tag reader: %s", rfid.isOnline() ? "OK" : "NOT DETECTED");
}

// ═════════════════════════════════════════════════════════════════════════════
// TAG PAIRING — assign a fresh tag to each existing member. Keeps names,
// points and streaks (unlike the first-run wizard). Reached from the recovery
// screen (hold BOOT at power-up) or Admin > Family Settings.
// ═════════════════════════════════════════════════════════════════════════════

void goPairTags() {
    pairIdx = 0;
    currentScreen = Screen::PAIR_TAGS;
    lastActivityMs = millis();
    drawPairTags();
}

void drawPairTags() {
    if (pairIdx >= familyCount) { finishPairing(); return; }
    const FamilyMember& m = family[pairIdx];

    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 320, 48, C_CARD);
    tft.setTextColor(C_TXT); tft.setTextSize(2);
    tft.setCursor(12, 14); tft.print("Pair Family Tags");
    tft.setTextColor(C_TXT3); tft.setTextSize(1);
    tft.setCursor(230, 20); tft.printf("%d of %d", pairIdx + 1, familyCount);

    tft.setTextColor(C_TXT2); tft.setTextSize(2);
    tft.setCursor(20, 80); tft.print("Scan the tag for:");
    tft.setTextColor(m.role == Role::PARENT ? C_PRI : C_GOLD); tft.setTextSize(4);
    tft.setCursor(20, 115); tft.print(m.name);
    tft.setTextColor(C_TXT3); tft.setTextSize(1);
    tft.setCursor(20, 160); tft.print(m.role == Role::PARENT ? "PARENT" : "CHILD");

    char hex[UID_LENGTH * 3 + 1];
    RFIDManager::uidToHex(m.rfidUid, m.rfidUidLength, hex, sizeof(hex));
    tft.setCursor(20, 180); tft.printf("Current tag: %s", m.rfidUidLength ? hex : "(none)");

    Icons::drawRfidIcon(136, 220, rfid.isOnline() ? C_PRI : C_ERR, 48);
    if (!rfid.isOnline()) {
        tft.setTextColor(C_ERR); tft.setTextSize(1);
        tft.setCursor(20, 300); tft.print("Tag reader offline - check RC522 wiring.");
    }

    tft.fillRoundRect(15, 380, 290, 36, 6, C_CARD);
    tft.setTextColor(C_TXT2); tft.setTextSize(2);
    tft.setCursor(70, 390); tft.print("SKIP (keep tag)");
    tft.fillRoundRect(15, 430, 290, 36, 6, C_OK);
    tft.setTextColor(C_TXT); tft.setTextSize(2);
    tft.setCursor(100, 440); tft.print("DONE / SAVE");
}

void handlePairScan(uint8_t* uid, uint8_t len) {
    lastActivityMs = millis();
    if (pairIdx >= familyCount) return;

    // A tag can only belong to one person
    for (uint8_t i = 0; i < familyCount; i++) {
        if (i != pairIdx && RFIDManager::matchUid(uid, len, family[i])) {
            audio.errorTone();
            char buf[40]; snprintf(buf, sizeof(buf), "That's %s's tag", family[i].name);
            showMsg(buf, C_ERR, 1500);
            return;
        }
    }

    memcpy(family[pairIdx].rfidUid, uid, len);
    family[pairIdx].rfidUidLength = len;
    backups.markFamilyDirty();
    audio.scanBeep();
    Serial.printf("[PAIR] %s paired to new tag\n", family[pairIdx].name);
    pairIdx++;
    if (pairIdx >= familyCount) finishPairing();
    else drawPairTags();
}

void touchPairTags(uint16_t x, uint16_t y) {
    lastActivityMs = millis();
    if (y >= 380 && y <= 416) { pairIdx++; drawPairTags(); return; }   // skip
    if (y >= 430 && y <= 466) { finishPairing(); return; }            // done
}

void finishPairing() {
    backups.flushNow(activeTasks, activeTaskCount, family, familyCount,
        [](const Task* t, uint8_t c, const char* p){ storage.saveTasks(t, c, p); },
        [](const FamilyMember* f, uint8_t c){ storage.saveFamily(f, c); });
    audio.taskCompleteTone();
    goHome();
    showMsg("Tags saved!", C_OK, 1500);
}

// ═════════════════════════════════════════════════════════════════════════════
// RECOVERY — press BOOT during the "Starting..." splash. For when tags, touch
// or WiFi don't work, so the normal admin path (parent tag + taps) is out.
// Every option works from the BOOT button alone (ButtonMenu): touch may be
// exactly what's broken.
// ═════════════════════════════════════════════════════════════════════════════

void runRecovery() {
    uint8_t sel = 0;
    while (true) {
        char foot1[48], foot2[64];
        snprintf(foot1, sizeof(foot1), "Tag reader: %s (0x%02X)",
                 rfid.isOnline() ? "online" : "OFFLINE", rfid.version());
        if (WiFi.status() == WL_CONNECTED)
            snprintf(foot2, sizeof(foot2), "WiFi: %s  http://%s",
                     WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        else
            snprintf(foot2, sizeof(foot2), "WiFi: not connected");

        static const MenuItem items[] = {
            { "Re-pair family tags", "New tags; keeps names, points, streaks", C_PRI },
            { "WiFi setup",          "Pick a network, type the password",     C_CARD },
            { "Calibrate touch",     "When taps land in the wrong place",     C_CARD },
            { "Start setup over",    "Erase family, run the setup wizard",    0x8000 },
            { "Exit",                "",                                      C_DIM },
        };
        sel = ButtonMenu::run("Recovery", items, 5, sel, foot1, foot2);
        switch (sel) {
            case 0: goPairTags(); return;
            case 1: WifiSetup::run(); break;
            case 2: runTouchCalibration(); break;
            case 3:
                if (confirmStartOver()) startSetupOver();   // restarts
                break;
            default: goHome(); return;
        }
    }
}

bool confirmStartOver() {
    static const MenuItem items[] = {
        { "Cancel",            "Keep everything as it is",               C_DIM },
        { "Erase and restart", "Names, tags, points, streaks are reset", 0x8000 },
    };
    return ButtonMenu::run("Start setup over?", items, 2, 0,
                           "Chores and routines are kept. They're assigned",
                           "by position: register people in the same order.") == 1;
}

// Moves family.json aside (kept as family.json.old) and restarts; with no
// family file (or its .bak) the station boots into the setup wizard.
void startSetupOver() {
    backups.flushNow(activeTasks, activeTaskCount, family, familyCount,
        [](const Task* t, uint8_t c, const char* p){ storage.saveTasks(t, c, p); },
        [](const FamilyMember* f, uint8_t c){ storage.saveFamily(f, c); });
    if (SD.exists("/config/family.json.old")) SD.remove("/config/family.json.old");
    if (SD.exists("/config/family.json")) SD.rename("/config/family.json", "/config/family.json.old");
    if (SD.exists("/config/family.json.bak")) SD.remove("/config/family.json.bak");
    Serial.println("[SETUP] Family erased (saved as family.json.old) - restarting into the wizard.");

    tft.fillScreen(C_BG);
    tft.setTextColor(C_TXT); tft.setTextSize(2);
    tft.setCursor(30, 220); tft.print("Restarting setup...");
    delay(1500);
    ESP.restart();
}

// Plausible output of TFT_eSPI::calibrateTouch: [x0, xRange, y0, yRange,
// flags] on the XPT2046's 12-bit (0..4095) scale. A bad file would otherwise
// make taps vanish: getTouch() drops anything that maps off-screen.
bool touchCalPlausible(const uint16_t* cal) {
    return cal[1] >= 1000 && cal[3] >= 1000 &&
           cal[0] < 3000 && cal[2] < 3000 &&
           (uint32_t)cal[0] + cal[1] <= 4300 && (uint32_t)cal[2] + cal[3] <= 4300 &&
           cal[4] <= 7;
}

// Wait for one tap. Returns 1 hit inside the box, 0 miss / gave up,
// 2 = BOOT held (accept the calibration anyway), 3 = BOOT pressed (redo).
uint8_t waitTapInBox(int16_t bx, int16_t by, int16_t bw, int16_t bh) {
    const int16_t SLOP = 15;
    uint32_t start = millis(), pressedMs = 0, bootDownMs = 0;
    bool bootArmed = digitalRead(PIN_BOOT_BUTTON) == HIGH, bootDown = false;
    while (millis() - start < 20000) {
        bool down = digitalRead(PIN_BOOT_BUTTON) == LOW;
        if (!down) {
            if (bootDown && bootArmed && millis() - bootDownMs >= 30) return 3;
            bootDown = false; bootArmed = true;
        } else if (bootArmed) {
            if (!bootDown) { bootDown = true; bootDownMs = millis(); }
            else if (millis() - bootDownMs >= ButtonMenu::HOLD_MS) return 2;
        }

        uint16_t x, y;
        if (tft.getTouch(&x, &y)) {
            Serial.printf("[TOUCH] test tap at %u,%u\n", x, y);
            bool hit = x >= bx - SLOP && x <= bx + bw + SLOP && y >= by - SLOP && y <= by + bh + SLOP;
            tft.fillCircle(x, y, 5, hit ? C_OK : C_ERR);    // show where it landed
            uint32_t t = millis();                           // wait for release
            while (tft.getTouchRawZ() > 300 && millis() - t < 3000) delay(15);
            delay(300);
            return hit ? 1 : 0;
        }
        // Pressed, but the calibration maps it off the screen: a miss
        if (tft.getTouchRawZ() > 600) {
            if (!pressedMs) pressedMs = millis();
            else if (millis() - pressedMs > 250) {
                Serial.println("[TOUCH] test tap mapped off-screen");
                return 0;
            }
        } else pressedMs = 0;
        delay(15);
    }
    return 0;
}

// TFT_eSPI's corner-arrow routine reads raw touch values, so it works no matter
// how far off the current calibration is. The result is then checked with two
// test taps and only saved if they land where they should.
void runTouchCalibration() {
    // Let go of BOOT first so the press that got us here isn't misread
    uint32_t t0 = millis();
    while (digitalRead(PIN_BOOT_BUTTON) == LOW && millis() - t0 < 5000) delay(10);

    while (true) {
        tft.fillScreen(C_BG);
        tft.setTextColor(C_TXT); tft.setTextSize(2);
        tft.setCursor(40, 190); tft.print("Touch calibration");
        tft.setTextColor(C_TXT2); tft.setTextSize(1);
        tft.setCursor(40, 220); tft.print("Touch each corner arrow as it");
        tft.setCursor(40, 234); tft.print("appears (a stylus works best).");

        uint16_t cal[5];
        tft.calibrateTouch(cal, C_GOLD, C_BG, 15);
        tft.setTouch(cal);
        Serial.printf("[TOUCH] Calibration values: %u %u %u %u %u\n", cal[0], cal[1], cal[2], cal[3], cal[4]);

        // Two test targets in opposite quarters check both axes
        static const int16_t BOX[2][2] = { { 40, 80 }, { 200, 330 } };
        uint8_t result = touchCalPlausible(cal) ? 1 : 0;
        for (uint8_t i = 0; i < 2 && result == 1; i++) {
            tft.fillScreen(C_BG);
            tft.setTextColor(C_TXT); tft.setTextSize(2);
            tft.setCursor(20, 20); tft.print("Check: tap the box");
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(20, 445); tft.print("BOOT: press = redo calibration");
            tft.setCursor(20, 459); tft.print("      hold 1 second = keep it anyway");
            tft.fillRoundRect(BOX[i][0], BOX[i][1], 80, 80, 8, C_PRI);
            tft.drawRoundRect(BOX[i][0] - 2, BOX[i][1] - 2, 84, 84, 9, C_TXT);
            result = waitTapInBox(BOX[i][0], BOX[i][1], 80, 80);
        }

        if (result == 1 || result == 2) {
            bool saved = storage.saveTouchCal(cal);
            Serial.printf("[TOUCH] Calibration %s (%s)\n", result == 1 ? "verified" : "kept by BOOT",
                          saved ? "saved" : "NOT saved");
            tft.fillScreen(C_BG);
            tft.setTextColor(C_OK); tft.setTextSize(2);
            tft.setCursor(60, 220); tft.print("Touch calibrated");
            delay(800);
            return;
        }

        tft.fillScreen(C_BG);
        tft.setTextColor(C_WARN); tft.setTextSize(2);
        tft.setCursor(20, 200); tft.print("Taps didn't line up.");
        tft.setTextColor(C_TXT2);
        tft.setCursor(20, 230); tft.print("Let's redo it.");
        delay(1500);
    }
}

// In-progress timers survive a reboot: tasks store the wall-clock start time,
// and once NTP is up we rebuild the millis()-based start from it.
void restoreTaskTimers() {
    timersNeedRestore = false;
    uint32_t now = timeMan.currentEpoch();
    for (uint8_t i = 0; i < activeTaskCount; i++) {
        Task& t = activeTasks[i];
        if (t.state != TaskState::IN_PROGRESS || t.startedEpoch == 0 || t.startedEpoch > now) continue;
        uint32_t elapsedS = now - t.startedEpoch;
        t.startedAtMs = millis() - elapsedS * 1000UL;   // unsigned wrap is fine
        Serial.printf("[TASK] Restored timer for '%s' (%lus elapsed)\n", t.name, (unsigned long)elapsedS);
    }
}

// ── Firmware install (blocking, parent-started): progress on screen, then restart ──
void otaProgress(uint32_t done, uint32_t total) {
    static uint8_t lastPct = 255;
    uint8_t pct = total ? (uint8_t)((uint64_t)done * 100 / total) : 0;
    if (pct == lastPct) return;
    lastPct = pct;
    tft.fillRoundRect(30, 250, 260, 20, 6, C_CARD);
    tft.fillRoundRect(30, 250, (int16_t)(260 * pct / 100) < 12 ? 12 : (int16_t)(260 * pct / 100), 20, 6, C_OK);
    tft.fillRect(100, 280, 120, 20, C_BG);
    char p[8]; snprintf(p, sizeof(p), "%d%%", pct);
    txt(p, 160, 282, 2, C_TXT, TC_DATUM);
}

bool runOtaInstall() {
    if (!ota.updateAvailable()) return false;
    // Everything on the card first: the restart mustn't lose a chore or a point
    backups.flushNow(activeTasks, activeTaskCount, family, familyCount,
        [](const Task* t, uint8_t c, const char* p) { storage.saveTasks(t, c, p); },
        [](const FamilyMember* f, uint8_t c) { storage.saveFamily(f, c); });

    tft.fillScreen(C_BG);
    txt("Updating", 160, 120, 4, C_TXT, TC_DATUM);
    char v[48]; snprintf(v, sizeof(v), "%s -> %s", ota.current(), ota.latest);
    txt(v, 160, 160, 2, C_TXT2, TC_DATUM);
    txt("Please don't unplug the station", 160, 200, 2, C_WARN, TC_DATUM);
    otaProgress(0, 1);

    if (ota.install(otaProgress)) {
        txt("Done - restarting...", 160, 330, 2, C_OK, TC_DATUM);
        delay(1200);
        ESP.restart();
        return true;
    }
    txt("Update didn't install:", 160, 330, 2, C_ERR, TC_DATUM);
    txt(ota.lastError, 160, 352, 2, C_TXT, TC_DATUM);
    txt("The station keeps running as before.", 160, 380, 2, C_TXT2, TC_DATUM);
    audio.errorTone();
    delay(3500);
    touchGate.requireRelease();
    lastActivityMs = millis();
    return false;
}

void reloadRecurringTemplates() {
    scheduler.begin();
}

void generateTodaysRecurring() {
    scheduler.generateRecurringTasks();
}
