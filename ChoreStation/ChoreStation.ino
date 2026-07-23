// =============================================================================
// main.cpp — Family Chore Station v5
// =============================================================================
// CORE RULES:
//   1. SCAN = DASHBOARD. Any tag scan from any screen goes to that
//      person's dashboard. No conditional interpretation. Period.
//   2. DASHBOARD IS HOME BASE. All actions bounce back here.
//   3. TASK STATE IS SELF-CONTAINED. Timer, toggle, claim — all on Task.
//   4. PARENT APPROVES FROM PARENT DASHBOARD. Pending tasks shown at top.
//      Child's task detail NEVER shows approval buttons.
//   5. NON-BLOCKING MESSAGES. Brief overlays clear themselves.
//   6. CHILDREN CAN SELF-INITIATE. "+" button on child dashboard.
// =============================================================================

#include <Arduino.h>
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
#include "parent_admin.h"
#include "preset_names.h"

// ─── Globals ─────────────────────────────────────────────────────────────────

TFT_eSPI       tft;
RFIDManager    rfid;
AudioFeedback  audio;
StorageManager storage;
SetupWizard    wizard;
DisplayPower   display;
BackupManager  backups;
TimeManager    timeMan;
RoutineManager routines;
Scheduler      scheduler;
ParentAdmin    admin;

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
char     msgText[48]    = "";
uint16_t msgColor       = 0;

// Self-init wizard state (child proposing a task)
bool     selfInitActive = false;
uint8_t  selfInitStep   = 0;   // 0=category, 1=name, 2=size, 3=confirm
Task     selfInitDraft;
uint8_t  selfInitNameIdx = 0;

// Quick-add state (parent fast path)
bool     quickAddActive = false;
uint8_t  quickAddStep   = 0;   // 0=category+name, 1=assign, 2=confirm

static const uint8_t  TASKS_PER_PAGE      = 5;
static const uint32_t IDLE_TIMEOUT_MS     = 30000;
static const uint32_t DETAIL_TIMEOUT_MS   = 8000;  // Auto-return from detail
static const uint32_t DETAIL_REVIEW_MS    = 30000; // Longer timeout for parent review
uint32_t detailShownAtMs = 0;

enum class Screen : uint8_t {
    HOME, DASHBOARD, TASK_DETAIL, SELF_INIT, QUICK_ADD,
    PARENT_ADMIN, MILESTONES, ROUTINE, SETUP
};
Screen currentScreen = Screen::HOME;

// Routine session state (one routine walked at a time)
int8_t viewingRoutineIdx = -1;
bool   routineStepDone[MAX_ROUTINE_STEPS] = { false };

// A dashboard list row is either a task or a routine. Both draw and touch build
// the SAME ordered list via buildDashRows() so a tapped card always matches the
// card that was drawn (this also fixes parent pending-first tap alignment).
struct DashRow { uint8_t kind; uint8_t idx; };  // kind: 0 = task, 1 = routine

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
void drawQuickAdd();

void goHome();
void goDashboard(uint8_t userId);
void goTaskDetail(uint8_t taskIdx);
void goRoutine(uint8_t routineIdx);
void drawRoutine();
void touchRoutine(uint16_t x, uint16_t y);
void finishRoutine(uint8_t routineIdx);
uint8_t buildDashRows(DashRow* rows, uint8_t maxRows);

void handleScan(uint8_t* uid, uint8_t len);
void handleTouch(uint16_t x, uint16_t y);
void touchDashboard(uint16_t x, uint16_t y);
void touchTaskDetail(uint16_t x, uint16_t y);
void touchSelfInit(uint16_t x, uint16_t y);
void touchQuickAdd(uint16_t x, uint16_t y);

void showMsg(const char* msg, uint16_t color, uint16_t durationMs = 1200);
void drawMsgOverlay();

bool isVisibleTask(uint8_t idx);
uint8_t totalPages();
uint8_t countPending();

// Preset names imported from preset_names.h as presetTaskName()

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200); delay(500);
    Serial.println("\n══ Family Chore Station v5 ══\n");

    audio.begin();
    SPI.begin(PIN_VSPI_CLK, PIN_VSPI_MISO, PIN_VSPI_MOSI);
    bool sdOk = storage.begin();
    rfid.begin();
    tft.init(); tft.setRotation(0); tft.fillScreen(C_BG);
    display.begin();

    tft.setTextColor(C_PRI); tft.setTextSize(2);
    tft.setCursor(80, 220); tft.print("Starting...");

    if (sdOk) timeMan.begin();
    backups.begin();

    if (!sdOk || storage.isFirstRun()) {
        inSetupMode = true; currentScreen = Screen::SETUP; wizard.begin();
    } else {
        storage.loadFamily(family, familyCount);
        storage.loadTasks(activeTasks, activeTaskCount, "/tasks/active.json");
        routines.begin();
        scheduler.begin();
        if (activeTaskCount == 0) scheduler.generateRecurringTasks();
        goHome();
    }
    audio.scanBeep(); lastActivityMs = millis();
}

// ═════════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
    if (inSetupMode) {
        if (wizard.update()) {
            inSetupMode = false;
            memcpy(family, wizard.getMembers(), sizeof(FamilyMember)*wizard.getMemberCount());
            familyCount = wizard.getMemberCount();
            goHome();
        }
        return;
    }

    // RFID — always polls, always goes to dashboard
    uint8_t uid[UID_LENGTH]; uint8_t uidLen = 0;
    if (rfid.poll(uid, uidLen)) {
        display.wake();
        handleScan(uid, uidLen);
    }

    // Touch
    uint16_t tx, ty;
    if (tft.getTouch(&tx, &ty)) {
        display.wake();
        if (display.isActive()) {
            handleTouch(tx, ty);
            lastActivityMs = millis();
        }
        delay(180);
    }

    // Display power
    display.update(lastActivityMs, timeMan.currentHour());
    if (display.needsRedraw()) {
        if (currentScreen == Screen::HOME) drawHome();
        else if (currentScreen == Screen::DASHBOARD) drawDashboard();
    }

    if (!display.isSleeping()) {
        // Task timeouts
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            if (TaskEngine::checkTimeout(activeTasks[i])) {
                backups.markTasksDirty();
                if (currentScreen == Screen::DASHBOARD) drawDashboard();
                else if (currentScreen == Screen::TASK_DETAIL && viewingTaskIdx == i)
                    drawTaskDetail();
            }
        }

        // Non-blocking message expiry
        if (msgActive && millis() >= msgExpireMs) {
            msgActive = false;
            // Redraw current screen to clear overlay
            if (currentScreen == Screen::DASHBOARD) drawDashboard();
            else if (currentScreen == Screen::HOME) drawHome();
        }

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
        if (currentScreen == Screen::HOME && display.shouldPixelShift()) drawHome();

        // Idle → home
        if (currentScreen == Screen::DASHBOARD || currentScreen == Screen::MILESTONES ||
            currentScreen == Screen::ROUTINE) {
            if (millis() - lastActivityMs > IDLE_TIMEOUT_MS) goHome();
        }
    }

    // Background services
    backups.update(activeTasks, activeTaskCount, family, familyCount,
        [](const Task* t, uint8_t c, const char* p){ storage.saveTasks(t, c, p); },
        [](const FamilyMember* f, uint8_t c){ storage.saveFamily(f, c); });
    timeMan.update();
    if (scheduler.update()) {
        if (currentScreen == Screen::DASHBOARD) drawDashboard();
    }

    delay(10);
}

// ═════════════════════════════════════════════════════════════════════════════
// NAVIGATION
// ═════════════════════════════════════════════════════════════════════════════

void goHome() {
    currentUserId = 255; viewingTaskIdx = -1; viewingRoutineIdx = -1; taskPage = 0;
    selfInitActive = false; quickAddActive = false; msgActive = false;
    currentScreen = Screen::HOME; drawHome();
}

void goDashboard(uint8_t userId) {
    currentUserId = userId; viewingTaskIdx = -1; viewingRoutineIdx = -1; taskPage = 0;
    selfInitActive = false; quickAddActive = false;
    currentScreen = Screen::DASHBOARD; lastActivityMs = millis();
    drawDashboard();
    // Draw message overlay on top if active
    if (msgActive) drawMsgOverlay();
}

void goTaskDetail(uint8_t taskIdx) {
    viewingTaskIdx = taskIdx;
    currentScreen = Screen::TASK_DETAIL;
    detailShownAtMs = millis(); lastActivityMs = millis();
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
        audio.errorTone();
        showMsg("Unknown tag", C_ERR);
        return;
    }

    audio.scanBeep();

    // Are we on the owner's task detail with a scan-confirmable action?
    if (currentScreen == Screen::TASK_DETAIL && viewingTaskIdx >= 0 && mid == currentUserId) {
        Task& task = activeTasks[viewingTaskIdx];

        if (mid == task.assignedTo && task.state == TaskState::AVAILABLE) {
            // Start task, then go to dashboard
            TransitionResult r = TaskEngine::startTask(task, mid);
            if (r.success) {
                backups.markTasksDirty();
                audio.taskStartTone();
                showMsg("Task started!", C_OK, 800);
            }
            goDashboard(mid);
            return;
        }

        if (mid == task.assignedTo && task.state == TaskState::IN_PROGRESS) {
            // Complete task
            TransitionResult r = TaskEngine::completeTask(task, mid, viewingTaskIdx);
            if (r.success) {
                bool claim = task.noReminderToggled;
                TransitionResult r2 = TaskEngine::submitClaim(task, family[mid], claim);
                backups.markTasksDirty(); backups.markFamilyDirty();
                if (r2.newState == TaskState::COMPLETED) {
                    audio.taskCompleteTone();
                    showMsg(r2.message, C_GOLD, 1500);
                    // Milestone check (brief non-blocking notification)
                    if (r2.newMilestone >= 0) {
                        goDashboard(mid);
                        MilestoneIcons::drawNewBadgeNotification(r2.newMilestone);
                        msgActive = true; msgExpireMs = millis() + 3000;
                        return;
                    }
                } else {
                    audio.scanBeep();
                    showMsg("Sent for approval!", C_PRI, 1000);
                }
            } else {
                showMsg(r.message, C_WARN, 1500);
                drawTaskDetail(); // Refresh timer display
                return;
            }
            goDashboard(mid);
            return;
        }
    }

    // Default: go to this person's dashboard
    goDashboard(mid);
}

// ═════════════════════════════════════════════════════════════════════════════
// TOUCH ROUTER
// ═════════════════════════════════════════════════════════════════════════════

void handleTouch(uint16_t x, uint16_t y) {
    audio.touchClick();

    // Dismiss message overlay on any touch
    if (msgActive) { msgActive = false; drawDashboard(); return; }

    switch (currentScreen) {
        case Screen::HOME: break;
        case Screen::DASHBOARD:   touchDashboard(x, y); break;
        case Screen::TASK_DETAIL: touchTaskDetail(x, y); break;
        case Screen::ROUTINE:     touchRoutine(x, y); break;
        case Screen::SELF_INIT:   touchSelfInit(x, y); break;
        case Screen::QUICK_ADD:   touchQuickAdd(x, y); break;
        case Screen::PARENT_ADMIN:
            if (admin.handleTouch(x, y)) goDashboard(currentUserId);
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

    // Top-right: admin/gear (parent) or badges (child)
    if (x >= 240 && y <= 55) {
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

    // "+" button for child self-init (bottom-right corner)
    if (!isParent && x >= 260 && y >= 440) {
        selfInitActive = true; selfInitStep = 0;
        memset(&selfInitDraft, 0, sizeof(Task));
        selfInitDraft.type = TaskType::SELF_INIT;
        selfInitDraft.assignedTo = currentUserId;
        selfInitDraft.createdBy = currentUserId;
        selfInitDraft.requiresApproval = true;
        selfInitDraft.size = TaskSize::SMALL;
        selfInitNameIdx = 0;
        currentScreen = Screen::SELF_INIT;
        drawSelfInit();
        return;
    }

    // Quick-add button for parent (bottom-right corner)
    if (isParent && x >= 260 && y >= 440) {
        quickAddActive = true; quickAddStep = 0;
        memset(&selfInitDraft, 0, sizeof(Task)); // Reuse draft
        selfInitDraft.type = TaskType::ADHOC;
        selfInitDraft.size = TaskSize::SMALL;
        selfInitDraft.assignedTo = 255;
        selfInitDraft.createdBy = currentUserId;
        selfInitDraft.requiresApproval = false;
        selfInitNameIdx = 0;
        currentScreen = Screen::QUICK_ADD;
        drawQuickAdd();
        return;
    }

    // Page navigation (bottom area, left/right halves)
    uint8_t tp = totalPages();
    if (y >= 455 && tp > 1) {
        if (x >= 160 && taskPage < tp - 1) { taskPage++; drawDashboard(); }
        else if (x < 160 && taskPage > 0) { taskPage--; drawDashboard(); }
        return;
    }

    // Card taps (routines + tasks) — same ordering as drawDashboard.
    int16_t listStartY = 142;
    if (y >= listStartY && y < listStartY + TASKS_PER_PAGE * 48) {
        uint8_t slot = (y - listStartY) / 48;
        uint8_t target = taskPage * TASKS_PER_PAGE + slot;
        DashRow rows[MAX_ACTIVE_TASKS + MAX_ROUTINES];
        uint8_t rowCount = buildDashRows(rows, MAX_ACTIVE_TASKS + MAX_ROUTINES);
        if (target < rowCount) {
            if (rows[target].kind == 1) goRoutine(rows[target].idx);
            else                        goTaskDetail(rows[target].idx);
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

    // ── Owner actions ──
    if (isOwner && task.state == TaskState::IN_PROGRESS && task.type != TaskType::SELF_INIT) {
        // No-reminder toggle: y=260..304
        if (y >= 260 && y <= 304) {
            task.noReminderToggled = !task.noReminderToggled;
            drawTaskDetail();
            return;
        }
    }

    // ── Parent approval actions (only from parent's own dashboard context) ──
    if (isParent && task.state == TaskState::PENDING_REVIEW) {
        // Approve: y=380..415
        if (y >= 380 && y <= 415) {
            FamilyMember& assignee = family[task.assignedTo];
            TransitionResult r = TaskEngine::approveTask(task, assignee);
            if (r.success) {
                backups.markTasksDirty(); backups.markFamilyDirty();
                audio.taskCompleteTone();
                showMsg(r.message, C_GOLD, 1500);
                if (r.newMilestone >= 0) {
                    goDashboard(currentUserId);
                    MilestoneIcons::drawNewBadgeNotification(r.newMilestone);
                    msgActive = true; msgExpireMs = millis() + 3000;
                    return;
                }
            }
            goDashboard(currentUserId);
            return;
        }
        // Override: y=420..445 (only if child claimed)
        if (task.childClaimedNoReminder && y >= 420 && y <= 445) {
            FamilyMember& assignee = family[task.assignedTo];
            TaskEngine::approveWithOverride(task, assignee);
            backups.markTasksDirty(); backups.markFamilyDirty();
            audio.taskCompleteTone();
            showMsg("Approved (claim removed)", C_WARN, 1500);
            goDashboard(currentUserId);
            return;
        }
        // Reject
        int16_t rejectY = task.childClaimedNoReminder ? 450 : 420;
        if (y >= rejectY && y <= rejectY + 30) {
            TaskEngine::rejectTask(task);
            backups.markTasksDirty();
            audio.errorTone();
            showMsg("Try again!", C_ERR, 1000);
            goDashboard(currentUserId);
            return;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ROUTINE — one scan got the child here; tap each step, earn a flat bundle
// ═════════════════════════════════════════════════════════════════════════════

void goRoutine(uint8_t idx) {
    viewingRoutineIdx = idx;
    for (uint8_t i = 0; i < MAX_ROUTINE_STEPS; i++) routineStepDone[i] = false;
    currentScreen = Screen::ROUTINE;
    detailShownAtMs = millis(); lastActivityMs = millis();
    drawRoutine();
}

void finishRoutine(uint8_t idx) {
    if (idx >= routines.count() || currentUserId >= familyCount) { goDashboard(currentUserId); return; }
    Routine& rt = routines.get(idx);
    FamilyMember& m = family[currentUserId];

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
    char d[11]; timeMan.currentDateStr(d, sizeof(d));
    storage.appendLog(log, d);

    backups.markFamilyDirty();
    audio.taskCompleteTone();

    char buf[40];
    snprintf(buf, sizeof(buf), "%s done! +%d", rt.name, rt.bundlePoints);
    showMsg(buf, C_GOLD, 1500);
    Serial.printf("[ROUTINE] %s completed '%s' (+%d)\n", m.name, rt.name, rt.bundlePoints);

    if (ms >= 0) {
        goDashboard(currentUserId);
        MilestoneIcons::drawNewBadgeNotification(ms);
        msgActive = true; msgExpireMs = millis() + 3000;
        return;
    }
    goDashboard(currentUserId);
}

void drawRoutine() {
    if (viewingRoutineIdx < 0 || viewingRoutineIdx >= routines.count()) { goDashboard(currentUserId); return; }
    const Routine& rt = routines.get(viewingRoutineIdx);
    bool done = routines.isDoneToday(viewingRoutineIdx, currentUserId);

    uint8_t checked = 0;
    for (uint8_t i = 0; i < rt.stepCount; i++) if (routineStepDone[i]) checked++;

    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 320, 48, C_CARD);
    Icons::drawBackArrow(8, 13, C_TXT2, 22);
    tft.setTextColor(C_TXT); tft.setTextSize(2);
    tft.setCursor(38, 6); tft.print(rt.name);
    tft.setTextColor(C_GOLD); tft.setTextSize(1);
    tft.setCursor(38, 30);
    tft.printf("+%d points   %d/%d steps", rt.bundlePoints,
               done ? rt.stepCount : checked, rt.stepCount);

    for (uint8_t i = 0; i < rt.stepCount; i++) {
        int16_t y = 60 + i * 34;
        bool on = done || routineStepDone[i];
        tft.fillRoundRect(10, y, 300, 30, 6, C_CARD);
        tft.drawRoundRect(18, y + 5, 20, 20, 4, on ? C_OK : C_TXT3);
        if (on) {
            tft.fillRoundRect(20, y + 7, 16, 16, 3, C_OK);
            Icons::drawCheckIcon(21, y + 8, C_BG, 14);
        }
        tft.setTextColor(on ? C_TXT3 : C_TXT); tft.setTextSize(1);
        tft.setCursor(48, y + 11); tft.print(rt.steps[i]);
    }

    if (done) {
        tft.setTextColor(C_OK); tft.setTextSize(2);
        tft.setCursor(70, 445); tft.print("All done today!");
    } else {
        bool all = (checked == rt.stepCount && rt.stepCount > 0);
        tft.fillRoundRect(10, 438, 300, 34, 8, all ? C_OK : C_DIM);
        tft.setTextColor(all ? C_TXT : C_TXT3); tft.setTextSize(2);
        tft.setCursor(95, 445); tft.printf("FINISH +%d", rt.bundlePoints);
    }
}

void touchRoutine(uint16_t x, uint16_t y) {
    lastActivityMs = millis();
    if (viewingRoutineIdx < 0 || viewingRoutineIdx >= routines.count()) { goDashboard(currentUserId); return; }
    if (x < 60 && y < 48) { goDashboard(currentUserId); return; }

    const Routine& rt = routines.get(viewingRoutineIdx);
    if (routines.isDoneToday(viewingRoutineIdx, currentUserId)) return;  // read-only once done

    // Toggle a step
    for (uint8_t i = 0; i < rt.stepCount; i++) {
        int16_t sy = 60 + i * 34;
        if (y >= sy && y < sy + 30) {
            routineStepDone[i] = !routineStepDone[i];
            drawRoutine();
            return;
        }
    }

    // Finish — only fires when every step is checked (button is dim otherwise)
    if (y >= 438) {
        uint8_t checked = 0;
        for (uint8_t i = 0; i < rt.stepCount; i++) if (routineStepDone[i]) checked++;
        if (checked == rt.stepCount && rt.stepCount > 0) finishRoutine(viewingRoutineIdx);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// CHILD SELF-INIT (3 steps: category+name, size, confirm)
// ═════════════════════════════════════════════════════════════════════════════

void touchSelfInit(uint16_t x, uint16_t y) {
    lastActivityMs = millis();
    if (x < 60 && y < 48) { goDashboard(currentUserId); return; } // Back

    switch (selfInitStep) {
        case 0: // Category + name
            // Category row taps
            for (uint8_t i = 0; i < 6; i++) {
                if (y >= 55 + i * 34 && y < 55 + i * 34 + 30) {
                    selfInitDraft.category = static_cast<TaskCategory>(i);
                    selfInitNameIdx = 0;
                    drawSelfInit(); return;
                }
            }
            // Name row taps (below categories)
            for (uint8_t i = 0; i < 6; i++) {
                if (y >= 270 + i * 28 && y < 270 + i * 28 + 24) {
                    selfInitNameIdx = i;
                    drawSelfInit(); return;
                }
            }
            // Next
            if (x >= 210 && y >= 450) { selfInitStep = 1; drawSelfInit(); }
            break;

        case 1: // Size
            for (uint8_t i = 0; i < 3; i++) {
                if (y >= 80 + i * 65 && y < 80 + i * 65 + 55) {
                    selfInitDraft.size = static_cast<TaskSize>(i);
                    drawSelfInit(); return;
                }
            }
            if (x >= 210 && y >= 450) { selfInitStep = 2; drawSelfInit(); }
            break;

        case 2: // Confirm
            if (x >= 180 && y >= 440) { // SAVE
                // Create the self-init task
                if (activeTaskCount < MAX_ACTIVE_TASKS) {
                    uint8_t ci = static_cast<uint8_t>(selfInitDraft.category);
                    strlcpy(selfInitDraft.name, presetTaskName(ci, selfInitNameIdx),
                            sizeof(selfInitDraft.name));
                    selfInitDraft.id = millis() & 0xFFFF;
                    selfInitDraft.state = TaskState::AVAILABLE;
                    selfInitDraft.requiresApproval = true;
                    memcpy(&activeTasks[activeTaskCount], &selfInitDraft, sizeof(Task));
                    activeTaskCount++;
                    backups.markTasksDirty();
                    audio.taskCompleteTone();
                    showMsg("Task proposed!", C_GOLD, 1000);
                    Serial.printf("[SELF-INIT] %s proposed '%s'\n",
                                  family[currentUserId].name, selfInitDraft.name);
                }
                goDashboard(currentUserId);
            }
            if (x < 150 && y >= 440) { selfInitStep = 1; drawSelfInit(); } // Back
            break;
    }
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

    switch (selfInitStep) {
        case 0: { // Category + name
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(15, 55); tft.print("What kind of task?");
            // Compact category buttons
            for (uint8_t i = 0; i < 6; i++) {
                int16_t y = 55 + i * 34;
                bool sel = (ci == i);
                tft.fillRoundRect(15, y, 290, 30, 5, sel ? catC(static_cast<TaskCategory>(i)) : C_CARD);
                tft.setTextColor(sel ? C_TXT : C_TXT2); tft.setTextSize(1);
                tft.setCursor(25, y + 9); tft.print(CAT_NAMES[i]);
            }
            // Name list for selected category
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(15, 262); tft.print("What is it?");
            for (uint8_t i = 0; i < 6; i++) {
                int16_t y = 275 + i * 28;
                bool sel = (selfInitNameIdx == i);
                tft.fillRoundRect(15, y, 290, 24, 4, sel ? C_PRI : C_CARD);
                tft.setTextColor(sel ? C_TXT : C_TXT2); tft.setTextSize(1);
                tft.setCursor(25, y + 6); tft.print(presetTaskName(ci, i));
            }
            tft.fillRoundRect(210, 450, 100, 26, 5, C_PRI);
            tft.setTextColor(C_TXT); tft.setTextSize(1);
            tft.setCursor(235, 456); tft.print("NEXT >");
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
                tft.setCursor(240, y + 16); tft.printf("+%d", pts + PointTable::INITIATIVE_BONUS);
                tft.setTextColor(sel ? C_TXT : C_TXT3); tft.setTextSize(1);
                tft.setCursor(25, y + 35); tft.printf("(%d base + %d initiative)", pts, PointTable::INITIATIVE_BONUS);
            }
            tft.fillRoundRect(210, 450, 100, 26, 5, C_PRI);
            tft.setTextColor(C_TXT); tft.setTextSize(1);
            tft.setCursor(235, 456); tft.print("NEXT >");
            break;
        }

        case 2: { // Confirm
            tft.setTextColor(C_TXT); tft.setTextSize(2);
            tft.setCursor(15, 65); tft.print("Ready to propose:");

            tft.fillRoundRect(15, 100, 290, 70, 8, C_CARD);
            Icons::drawCategoryIcon(22, 108, selfInitDraft.category, catC(selfInitDraft.category), 24);
            tft.setTextColor(C_TXT); tft.setTextSize(2);
            tft.setCursor(54, 105); tft.print(presetTaskName(ci, selfInitNameIdx));
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(54, 130); tft.printf("%s  |  %s  |  +%d pts",
                CAT_NAMES[ci], SIZE_NAMES[(int)selfInitDraft.size],
                PointTable::base(selfInitDraft.size) + PointTable::INITIATIVE_BONUS);
            tft.setCursor(54, 145);
            tft.setTextColor(C_GOLD); tft.print("Initiative bonus included!");

            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(15, 200);
            tft.print("A parent will review and approve");
            tft.setCursor(15, 215);
            tft.print("both the task and your work.");

            tft.fillRoundRect(15, 440, 120, 30, 5, C_DIM);
            tft.setTextColor(C_TXT); tft.setTextSize(1);
            tft.setCursor(45, 449); tft.print("< BACK");
            tft.fillRoundRect(180, 440, 120, 30, 5, C_OK);
            tft.setTextColor(C_TXT); tft.setTextSize(1);
            tft.setCursor(200, 449); tft.print("PROPOSE!");
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

    uint8_t ci = static_cast<uint8_t>(selfInitDraft.category);

    switch (quickAddStep) {
        case 0: // Category + name + assign
            // Categories (compact)
            for (uint8_t i = 0; i < 6; i++) {
                if (y >= 55 + i * 30 && y < 55 + i * 30 + 26) {
                    selfInitDraft.category = static_cast<TaskCategory>(i);
                    selfInitNameIdx = 0; drawQuickAdd(); return;
                }
            }
            // Names
            for (uint8_t i = 0; i < 6; i++) {
                if (y >= 246 + i * 26 && y < 246 + i * 26 + 22) {
                    selfInitNameIdx = i; drawQuickAdd(); return;
                }
            }
            // Assign buttons (bottom area)
            {
                int16_t ay = 410;
                uint8_t childIdx = 0;
                for (uint8_t i = 0; i < familyCount; i++) {
                    if (family[i].role != Role::CHILD) continue;
                    int16_t bx = 15 + childIdx * 100;
                    if (x >= bx && x <= bx + 90 && y >= ay && y <= ay + 28) {
                        selfInitDraft.assignedTo = i; drawQuickAdd(); return;
                    }
                    childIdx++;
                }
                // "Both" button
                int16_t bothX = 15 + childIdx * 100;
                if (x >= bothX && x <= bothX + 90 && y >= ay && y <= ay + 28) {
                    selfInitDraft.assignedTo = 254; // Special: 254 = all children
                    drawQuickAdd(); return;
                }
            }
            // Create
            if (x >= 210 && y >= 450) {
                // Save task(s)
                strlcpy(selfInitDraft.name, presetTaskName(ci, selfInitNameIdx),
                        sizeof(selfInitDraft.name));
                selfInitDraft.state = TaskState::AVAILABLE;

                if (selfInitDraft.assignedTo == 254) {
                    // Create one per child
                    for (uint8_t i = 0; i < familyCount && activeTaskCount < MAX_ACTIVE_TASKS; i++) {
                        if (family[i].role != Role::CHILD) continue;
                        Task& t = activeTasks[activeTaskCount];
                        memcpy(&t, &selfInitDraft, sizeof(Task));
                        t.assignedTo = i;
                        t.id = (millis() & 0xFFF0) | (i & 0x0F);
                        activeTaskCount++;
                    }
                } else {
                    if (activeTaskCount < MAX_ACTIVE_TASKS) {
                        Task& t = activeTasks[activeTaskCount];
                        memcpy(&t, &selfInitDraft, sizeof(Task));
                        t.id = millis() & 0xFFFF;
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
    tft.setTextColor(C_TXT); tft.setTextSize(2);
    tft.setCursor(38, 14); tft.print("Quick Add");

    uint8_t ci = static_cast<uint8_t>(selfInitDraft.category);

    // Categories (compact, single column)
    for (uint8_t i = 0; i < 6; i++) {
        int16_t y = 55 + i * 30;
        bool sel = (ci == i);
        tft.fillRoundRect(15, y, 290, 26, 4, sel ? catC(static_cast<TaskCategory>(i)) : C_CARD);
        tft.setTextColor(sel ? C_TXT : C_TXT2); tft.setTextSize(1);
        tft.setCursor(25, y + 7); tft.print(CAT_NAMES[i]);
    }

    // Names for selected category
    tft.setTextColor(C_TXT3); tft.setTextSize(1);
    tft.setCursor(15, 238); tft.print("Task:");
    for (uint8_t i = 0; i < 6; i++) {
        int16_t y = 248 + i * 26;
        bool sel = (selfInitNameIdx == i);
        tft.fillRoundRect(15, y, 290, 22, 3, sel ? C_PRI : C_CARD);
        tft.setTextColor(sel ? C_TXT : C_TXT2); tft.setTextSize(1);
        tft.setCursor(25, y + 5); tft.print(presetTaskName(ci, i));
    }

    // Assign row
    tft.setTextColor(C_TXT3); tft.setTextSize(1);
    tft.setCursor(15, 398); tft.print("Assign to:");
    int16_t ay = 410;
    uint8_t childIdx = 0;
    for (uint8_t i = 0; i < familyCount; i++) {
        if (family[i].role != Role::CHILD) continue;
        int16_t bx = 15 + childIdx * 100;
        bool sel = (selfInitDraft.assignedTo == i);
        tft.fillRoundRect(bx, ay, 90, 28, 5, sel ? C_PRI : C_CARD);
        tft.setTextColor(sel ? C_TXT : C_TXT2); tft.setTextSize(1);
        tft.setCursor(bx + 10, ay + 8); tft.print(family[i].name);
        childIdx++;
    }
    // "Both" button
    int16_t bothX = 15 + childIdx * 100;
    bool bothSel = (selfInitDraft.assignedTo == 254);
    tft.fillRoundRect(bothX, ay, 90, 28, 5, bothSel ? C_PRI : C_CARD);
    tft.setTextColor(bothSel ? C_TXT : C_TXT2); tft.setTextSize(1);
    tft.setCursor(bothX + 15, ay + 8); tft.print("Both");

    // Create button
    tft.fillRoundRect(210, 450, 100, 26, 5, C_OK);
    tft.setTextColor(C_TXT); tft.setTextSize(1);
    tft.setCursor(225, 456); tft.print("CREATE");
}

// ═════════════════════════════════════════════════════════════════════════════
// NON-BLOCKING MESSAGE OVERLAY
// ═════════════════════════════════════════════════════════════════════════════

void showMsg(const char* msg, uint16_t color, uint16_t durationMs) {
    msgActive = true;
    msgExpireMs = millis() + durationMs;
    strlcpy(msgText, msg, sizeof(msgText));
    msgColor = color;
    drawMsgOverlay();
}

void drawMsgOverlay() {
    if (!msgActive) return;
    tft.fillRoundRect(20, 210, 280, 40, 8, msgColor);
    tft.setTextColor(C_TXT); tft.setTextSize(2);
    int16_t tw = strlen(msgText) * 12;
    tft.setCursor(max(30, 160 - tw/2), 218);
    tft.print(msgText);
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
    return t.assignedTo == currentUserId;
}

// Build the ordered dashboard list shared by draw + touch.
//  - Child: their routines (scheduled today) first, then their tasks.
//  - Parent: pending-review tasks first, then the rest (no routines).
uint8_t buildDashRows(DashRow* rows, uint8_t maxRows) {
    uint8_t n = 0;
    if (currentUserId >= familyCount) return 0;
    bool isParent = (family[currentUserId].role == Role::PARENT);

    if (isParent) {
        for (uint8_t i = 0; i < activeTaskCount && n < maxRows; i++)
            if (isVisibleTask(i) && activeTasks[i].state == TaskState::PENDING_REVIEW)
                rows[n++] = { 0, i };
        for (uint8_t i = 0; i < activeTaskCount && n < maxRows; i++)
            if (isVisibleTask(i) && activeTasks[i].state != TaskState::PENDING_REVIEW)
                rows[n++] = { 0, i };
    } else {
        for (uint8_t r = 0; r < routines.count() && n < maxRows; r++)
            if (routines.visibleFor(r, currentUserId) && routines.activeToday(r))
                rows[n++] = { 1, r };
        for (uint8_t i = 0; i < activeTaskCount && n < maxRows; i++)
            if (isVisibleTask(i)) rows[n++] = { 0, i };
    }
    return n;
}

uint8_t totalPages() {
    DashRow rows[MAX_ACTIVE_TASKS + MAX_ROUTINES];
    uint8_t n = buildDashRows(rows, MAX_ACTIVE_TASKS + MAX_ROUTINES);
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

    tft.setTextColor(C_TXT2); tft.setTextSize(2);
    tft.setCursor(55+sx, 200+sy); tft.print("Scan your tag");

    Icons::drawRfidIcon(136+sx, 260+sy, C_PRI, 48);

    // In-progress indicator
    uint8_t ip = 0;
    for (uint8_t i = 0; i < activeTaskCount; i++)
        if (activeTasks[i].state == TaskState::IN_PROGRESS) ip++;
    if (ip > 0) {
        tft.setTextColor(C_WARN); tft.setTextSize(1);
        tft.setCursor(80+sx, 370+sy);
        tft.printf("%d task%s in progress", ip, ip > 1 ? "s" : "");
    }

    // Pending approvals indicator
    uint8_t pa = countPending();
    if (pa > 0) {
        tft.setTextColor(C_PRI); tft.setTextSize(1);
        tft.setCursor(80+sx, 388+sy);
        tft.printf("%d awaiting approval", pa);
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

void drawDashboard() {
    if (currentUserId >= familyCount) { goHome(); return; }
    const FamilyMember& user = family[currentUserId];
    bool isParent = (user.role == Role::PARENT);

    tft.fillScreen(C_BG);

    // Header
    tft.fillRect(0, 0, 320, 55, C_CARD);
    tft.setTextColor(C_TXT); tft.setTextSize(3);
    tft.setCursor(15, 12); tft.print(user.name);
    tft.setTextColor(C_DIM); tft.setTextSize(1);
    tft.setCursor(15, 40); tft.print(isParent ? "PARENT" : "CHILD");

    // Top-right
    if (isParent) {
        Icons::drawGearIcon(275, 8, C_TXT3, 24);
        uint8_t pa = countPending();
        if (pa > 0) {
            tft.fillCircle(295, 10, 9, C_ERR);
            tft.setTextColor(C_TXT); tft.setTextSize(1);
            tft.setCursor(pa < 10 ? 292 : 289, 6); tft.printf("%d", pa);
        }
        tft.setTextColor(C_DIM); tft.setTextSize(1);
        tft.setCursor(262, 36); tft.print("Admin");
    } else {
        uint8_t badges = 0;
        for (uint8_t i = 0; i < MILESTONE_COUNT; i++)
            if (user.totalPoints >= MILESTONES[i].pointThreshold) badges++;
        tft.setTextColor(C_GOLD); tft.setTextSize(2);
        tft.setCursor(258, 10); tft.printf("%d", badges);
        tft.setTextColor(C_DIM); tft.setTextSize(1);
        tft.setCursor(255, 33); tft.print("badges >");
    }

    // ── Info card ──
    if (!isParent) {
        // Child trajectory
        Trajectory traj = computeTrajectory(user.dailyTasksCompleted, user.dailyGoal,
                                            timeMan.currentHour(), 7, 21);
        uint16_t tc; const char* tl;
        switch (traj) {
            case Trajectory::AHEAD:    tc=C_OK;   tl="Ahead of schedule!"; break;
            case Trajectory::ON_TRACK: tc=C_PRI;  tl="On track"; break;
            case Trajectory::BEHIND:   tc=C_WARN; tl="A bit behind"; break;
        }
        tft.fillRoundRect(10, 62, 300, 50, 8, C_CARD);
        Icons::drawTrajectoryIcon(18, 67, traj, 26);
        tft.setTextColor(tc); tft.setTextSize(2);
        tft.setCursor(50, 66); tft.print(tl);
        Icons::drawProgressBar(50, 89, 180, 10, user.dailyTasksCompleted, user.dailyGoal, tc, C_BG);
        tft.setTextColor(C_TXT2); tft.setTextSize(1);
        tft.setCursor(50, 102); tft.printf("%d / %d today", user.dailyTasksCompleted, user.dailyGoal);
        if (user.currentStreak > 0) {
            Icons::drawStreakFire(260, 85, user.currentStreak, 14);
            tft.setTextColor(C_GOLD); tft.setTextSize(1);
            tft.setCursor(276, 88); tft.printf("%d", user.currentStreak);
        }
    } else {
        // Parent: family overview
        tft.fillRoundRect(10, 62, 300, 50, 8, C_CARD);
        tft.setTextColor(C_TXT3); tft.setTextSize(1);
        tft.setCursor(18, 66); tft.print("FAMILY");
        int16_t fx = 18;
        for (uint8_t i = 0; i < familyCount; i++) {
            if (family[i].role != Role::CHILD) continue;
            tft.setTextColor(C_TXT2); tft.setTextSize(1);
            tft.setCursor(fx, 80); tft.print(family[i].name);
            tft.setTextColor(C_TXT3);
            tft.setCursor(fx, 92);
            tft.printf("%d/%d done", family[i].dailyTasksCompleted, family[i].dailyGoal);
            fx += 100;
        }
        uint8_t pa = countPending();
        if (pa > 0) {
            tft.setTextColor(C_WARN); tft.setCursor(240, 80);
            tft.printf("%d", pa);
            tft.setCursor(230, 92); tft.print("pending");
        }
    }

    // ── Section label ──
    tft.setTextColor(C_DIM); tft.setTextSize(1);
    tft.setCursor(15, 118);

    // For parents, show pending first, then the rest
    if (isParent) {
        uint8_t pa = countPending();
        tft.print(pa > 0 ? "PENDING APPROVALS + ALL TASKS" : "ALL TASKS");
    } else {
        tft.print("YOUR TASKS");
    }

    uint8_t tp = totalPages();
    if (tp > 1) { tft.setCursor(230, 118); tft.printf("Pg %d/%d", taskPage+1, tp); }
    tft.drawFastHLine(15, 130, 290, C_CARD);

    // ── Cards (paged): routines + tasks, order from buildDashRows ──
    DashRow rows[MAX_ACTIVE_TASKS + MAX_ROUTINES];
    uint8_t rowCount = buildDashRows(rows, MAX_ACTIVE_TASKS + MAX_ROUTINES);

    uint16_t yPos = 135;
    uint8_t startAt = taskPage * TASKS_PER_PAGE;
    uint8_t shown = 0;

    for (uint8_t s = startAt; s < rowCount && shown < TASKS_PER_PAGE; s++) {
        if (rows[s].kind == 1) {
            // ── Routine card ──
            const Routine& rt = routines.get(rows[s].idx);
            bool done = routines.isDoneToday(rows[s].idx, currentUserId);
            bool now  = routines.inWindow(rows[s].idx);
            uint16_t accent = done ? C_OK : (now ? C_GOLD : C_PRI);

            tft.fillRoundRect(10, yPos, 300, 44, 6, C_CARD);
            tft.fillRect(10, yPos, 4, 44, accent);
            if (done) Icons::drawCheckIcon(16, yPos + 8, C_OK, 18);
            else      Icons::drawStreakFire(18, yPos + 6, now ? 1 : 0, 16);

            tft.setTextColor(done ? C_TXT3 : C_TXT); tft.setTextSize(1);
            tft.setCursor(40, yPos + 5); tft.print(rt.name);
            tft.setTextColor(C_TXT3);
            tft.setCursor(40, yPos + 20);
            tft.printf("Routine  %d steps  +%d", rt.stepCount, rt.bundlePoints);
            if (done) {
                tft.setTextColor(C_OK); tft.setCursor(40, yPos + 33); tft.print("Done today");
            } else if (now) {
                tft.setTextColor(C_GOLD); tft.setCursor(40, yPos + 33); tft.print("Time for this now!");
            }
            tft.setTextColor(accent); tft.setTextSize(1);
            tft.setCursor(286, yPos + 16); tft.print(done ? "" : ">");

            yPos += 48;
            shown++;
            continue;
        }

        uint8_t i = rows[s].idx;
        const Task& t = activeTasks[i];
        uint16_t cc = catC(t.category);

        tft.fillRoundRect(10, yPos, 300, 44, 6, C_CARD);
        tft.fillRect(10, yPos, 4, 44, cc);
        Icons::drawCategoryIcon(16, yPos + 6, t.category, cc, 16);

        tft.setTextColor(C_TXT); tft.setTextSize(1);
        tft.setCursor(38, yPos + 5); tft.print(t.name);

        tft.setTextColor(C_TXT3); tft.setTextSize(1);
        tft.setCursor(38, yPos + 20);
        const char* sz[] = {"S","M","L"};
        uint8_t pts = t.isOwnMess ? PointTable::ownMess(t.size) : PointTable::base(t.size);
        tft.printf("%s %dpt", sz[(int)t.size], pts);

        // Assignee name (parent view)
        if (isParent && t.assignedTo < familyCount) {
            tft.setCursor(110, yPos + 20); tft.print(family[t.assignedTo].name);
        }

        // State indicator
        Icons::drawStateIcon(280, yPos + 12, t.state, 18);

        // In-progress elapsed time
        if (t.state == TaskState::IN_PROGRESS) {
            char el[12]; TaskEngine::formatElapsed(t.startedAtMs, el, sizeof(el));
            tft.setTextColor(C_WARN); tft.setTextSize(1);
            tft.setCursor(38, yPos + 33); tft.print(el);
        }

        // Pending label
        if (t.state == TaskState::PENDING_REVIEW) {
            tft.setTextColor(C_PRI); tft.setTextSize(1);
            tft.setCursor(38, yPos + 33);
            tft.print(t.childClaimedNoReminder ? "Review (claimed bonus)" : "Needs approval");
        }

        // Self-init indicator
        if (t.type == TaskType::SELF_INIT) {
            Icons::drawInitiativeStar(262, yPos + 4, 10);
        }

        yPos += 48;
        shown++;
    }

    if (shown == 0 && taskPage == 0) {
        tft.setTextColor(C_TXT2); tft.setTextSize(2);
        tft.setCursor(50, 220); tft.print("All caught up!");
        Icons::drawCheckIcon(140, 260, C_OK, 36);
    }

    // Page dots
    if (tp > 1) {
        int16_t dx = (320 - tp * 14) / 2;
        for (uint8_t p = 0; p < tp; p++)
            tft.fillCircle(dx + p * 14, 456, 4, p == taskPage ? C_PRI : C_DIM);
    }

    // ── Bottom action button ──
    // "+" for child self-init or parent quick-add
    tft.fillRoundRect(268, 443, 44, 28, 6, isParent ? C_PRI : C_GOLD);
    Icons::drawPlusIcon(278, 446, C_TXT, 22);

    tft.setTextColor(C_DIM); tft.setTextSize(1);
    tft.setCursor(15, 470);
    tft.print(isParent ? "Tap task to review" : "Tap task for details");
}

// ═════════════════════════════════════════════════════════════════════════════
// DRAWING — TASK DETAIL (child sees info + actions, parent sees approval)
// ═════════════════════════════════════════════════════════════════════════════

void drawTaskDetail() {
    if (viewingTaskIdx < 0 || viewingTaskIdx >= activeTaskCount) { goDashboard(currentUserId); return; }
    const Task& task = activeTasks[viewingTaskIdx];
    bool isOwner = (currentUserId == task.assignedTo);
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
    tft.setTextColor(C_TXT); tft.setTextSize(2);
    tft.setCursor(50, 58); tft.print(task.name);
    tft.setTextColor(cc); tft.setTextSize(1);
    tft.setCursor(50, 80); tft.print(CAT_NAMES[(int)task.category]);

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
            tft.setCursor(20, 162); tft.printf("Min: %ds left", rem);
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

    // ── No-reminder toggle (owner, IN_PROGRESS, not self-init) ──
    if (isOwner && task.state == TaskState::IN_PROGRESS && task.type != TaskType::SELF_INIT) {
        bool on = task.noReminderToggled;
        tft.fillRoundRect(10, 245, 300, 44, 8, C_CARD);
        int16_t tx = 20, ty = 255;
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

    if (task.state == TaskState::AVAILABLE && isOwner) {
        tft.setTextColor(C_OK); tft.setTextSize(1);
        tft.setCursor(55, ay); tft.print("Scan your tag to start");
        Icons::drawRfidIcon(140, ay + 18, C_PRI, 36);
    }
    else if (task.state == TaskState::IN_PROGRESS && isOwner) {
        int32_t rem = TaskEngine::remainingMinTime(task);
        if (rem > 0) {
            tft.setTextColor(C_TXT3); tft.setTextSize(1);
            tft.setCursor(40, ay); tft.print("Keep going — not enough time yet");
        } else {
            tft.setTextColor(C_OK); tft.setTextSize(1);
            tft.setCursor(55, ay); tft.print("Scan your tag to complete");
            Icons::drawRfidIcon(140, ay + 18, C_OK, 36);
        }
    }
    else if (task.state == TaskState::PENDING_REVIEW && !isParent) {
        tft.setTextColor(C_PRI); tft.setTextSize(1);
        tft.setCursor(35, ay); tft.print("Waiting for a parent to review");
    }
    else if (task.state == TaskState::PENDING_REVIEW && isParent) {
        // PARENT APPROVAL BUTTONS
        tft.fillRoundRect(10, 380, 300, 32, 6, C_OK);
        tft.setTextColor(C_TXT); tft.setTextSize(2);
        tft.setCursor(105, 385); tft.print("APPROVE");

        if (task.childClaimedNoReminder) {
            tft.fillRoundRect(10, 420, 300, 22, 5, C_WARN);
            tft.setTextColor(C_BG); tft.setTextSize(1);
            tft.setCursor(48, 425); tft.print("APPROVE, REMOVE BONUS CLAIM");

            tft.fillRoundRect(10, 450, 300, 22, 5, C_ERR);
            tft.setTextColor(C_TXT); tft.setTextSize(1);
            tft.setCursor(130, 455); tft.print("REJECT");
        } else {
            tft.fillRoundRect(10, 420, 300, 22, 5, C_ERR);
            tft.setTextColor(C_TXT); tft.setTextSize(1);
            tft.setCursor(130, 425); tft.print("REJECT");
        }
    }
    else if (task.state == TaskState::TIMED_OUT) {
        tft.setTextColor(C_ERR); tft.setTextSize(1);
        tft.setCursor(70, ay); tft.print("This task timed out.");
    }
}
