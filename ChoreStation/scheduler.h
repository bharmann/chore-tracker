// =============================================================================
// scheduler.h — Recurring Task Generation & Daily Reset
// =============================================================================
// Two responsibilities:
//
// 1. RECURRING TASK GENERATION
//    Reads recurring task templates from /tasks/recurring.json.
//    At boot and periodically, checks which templates should produce
//    active task instances today based on recurDays bitmask + current
//    day of week. Avoids creating duplicates if the task is already
//    in the active list (matched by template ID).
//
// 2. DAILY RESET
//    Triggered when TimeManager detects a day rollover:
//    - Evaluate streaks for each child
//    - Reset dailyTasksCompleted, remindedToday, remindersToday
//    - Clear completed/timed-out tasks from active list
//    - Generate today's recurring tasks
//    - Create daily backup
//    - Archive old logs
//
// =============================================================================

#pragma once
#include <Arduino.h>
#include "data_model.h"
#include "time_manager.h"
#include "storage_manager.h"
#include "backup_manager.h"
#include "routine_manager.h"

extern TimeManager  timeMan;
extern StorageManager storage;
extern BackupManager  backups;
extern RoutineManager routines;
extern FamilyMember   family[];
extern uint8_t        familyCount;
extern Task           activeTasks[];
extern uint8_t        activeTaskCount;

class Scheduler {
public:

    void begin() {
        _lastScheduleCheckMs = 0;
        _recurringLoaded = false;
        _recurringCount = 0;

        // Load recurring templates
        _recurringLoaded = storage.loadTasks(
            _recurringTemplates, _recurringCount, "/tasks/recurring.json");

        if (_recurringLoaded) {
            Serial.printf("[SCHED] Loaded %d recurring templates.\n", _recurringCount);
        } else {
            Serial.println("[SCHED] No recurring templates found.");
        }
    }

    // ─── Call from main loop ─────────────────────────────────────────────
    // Checks for day rollover and periodic task generation.
    // Returns true if active tasks were modified (caller should redraw).

    bool update() {
        bool changed = false;

        // Catch a new day even without a rollover event (e.g. booted after
        // midnight, or NTP synced late). Idempotent; only acts on real change.
        routines.syncDayIfNeeded();

        // Day rollover
        if (timeMan.hasDayRolledOver()) {
            changed = performDailyReset();
        }

        // Periodic scheduling check (every 5 minutes)
        if (millis() - _lastScheduleCheckMs > 300000) {
            _lastScheduleCheckMs = millis();
            if (_recurringLoaded) {
                changed |= generateRecurringTasks();
            }
        }

        return changed;
    }

    // ─── Generate recurring tasks for today ──────────────────────────────
    // Checks each template's recurDays bitmask against current day.
    // Only creates if no active task with the same template ID exists.

    bool generateRecurringTasks() {
        bool synced = timeMan.isTimeSynced();
        uint8_t dowBit = synced ? (1 << timeMan.currentDayOfWeek()) : 0;
        uint8_t hour = timeMan.currentHour();
        bool added = false;

        for (uint8_t i = 0; i < _recurringCount; i++) {
            const Task& tmpl = _recurringTemplates[i];

            if (synced) {
                // Check if this template runs today
                if (!(tmpl.recurDays & dowBit)) continue;
                // Check if it's past the scheduled hour (don't generate too early)
                if (hour < tmpl.recurHour) continue;
            } else {
                // No NTP: day-of-week is unknown, so only generate
                // everyday (127) templates. Day-specific ones wait for sync.
                if (tmpl.recurDays != 127) continue;
            }

            // "All children" (254) templates expand to one instance per child,
            // all sharing the template id; everything else is one instance.
            for (uint8_t m = 0; m < familyCount || (tmpl.assignedTo != 254 && m == 0); m++) {
                uint8_t who = tmpl.assignedTo;
                if (tmpl.assignedTo == 254) {
                    if (family[m].role != Role::CHILD) continue;
                    who = m;
                    if (_isAlreadyActive(tmpl.id, who)) continue;
                } else if (_isAlreadyActive(tmpl.id)) {
                    break;
                }

                if (activeTaskCount >= MAX_ACTIVE_TASKS) {
                    Serial.println("[SCHED] Active task list full — skipping.");
                    break;
                }

                // Create instance from template
                Task& newTask = activeTasks[activeTaskCount];
                memcpy(&newTask, &tmpl, sizeof(Task));
                newTask.assignedTo = who;
                newTask.state = TaskState::AVAILABLE;
                newTask.startedAtMs = 0;
                newTask.completedAtMs = 0;
                newTask.startedEpoch = 0;
                newTask.noReminderToggled = false;
                newTask.childClaimedNoReminder = false;
                newTask.parentOverrodeReminder = false;
                newTask.noReminderApproved = false;
                activeTaskCount++;
                added = true;

                Serial.printf("[SCHED] Generated: '%s' (id=%d) for %s\n",
                              newTask.name, newTask.id,
                              newTask.assignedTo < familyCount
                                  ? family[newTask.assignedTo].name : "anyone");
                if (tmpl.assignedTo != 254) break;
            }
        }

        if (added) {
            backups.markTasksDirty();
        }
        return added;
    }

    // ─── Daily Reset ─────────────────────────────────────────────────────
    // Called once when the day rolls over.

    bool performDailyReset() {
        Serial.println("[SCHED] ═══ DAILY RESET ═══");

        // 1. Evaluate streaks for each child
        _evaluateStreaks();

        // 2. Reset daily counters
        for (uint8_t i = 0; i < familyCount; i++) {
            family[i].dailyTasksCompleted = 0;
            family[i].remindedToday = false;
            family[i].remindersToday = 0;
            Serial.printf("[SCHED] Reset daily counters for %s\n", family[i].name);
        }

        // 2b. Reset routine completion masks for the new day
        routines.resetDaily();

        // 3. Clear completed and timed-out tasks from active list
        uint8_t newCount = 0;
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            if (activeTasks[i].state == TaskState::COMPLETED ||
                activeTasks[i].state == TaskState::TIMED_OUT) {
                // Skip — these are done
                continue;
            }
            // Keep pending/in-progress/available tasks that carried over
            if (i != newCount) {
                memcpy(&activeTasks[newCount], &activeTasks[i], sizeof(Task));
            }
            newCount++;
        }
        uint8_t removed = activeTaskCount - newCount;
        activeTaskCount = newCount;
        Serial.printf("[SCHED] Cleared %d finished tasks.\n", removed);

        // 4. Generate today's recurring tasks
        generateRecurringTasks();

        // 5. Save everything
        backups.markTasksDirty();
        backups.markFamilyDirty();

        // 6. Create daily backup
        char dateStr[11];
        timeMan.currentDateStr(dateStr, sizeof(dateStr));
        backups.createDailyBackup(dateStr);

        // 7. Archive old logs
        backups.archiveOldLogs(dateStr);

        Serial.println("[SCHED] ═══ DAILY RESET COMPLETE ═══\n");
        return true;
    }

private:
    Task     _recurringTemplates[MAX_ACTIVE_TASKS];
    uint8_t  _recurringCount;
    bool     _recurringLoaded;
    uint32_t _lastScheduleCheckMs;

    // An instance of this template already exists today. assignee 255 = any
    // (a claimed "Anyone" task keeps the template id but changes assignee).
    bool _isAlreadyActive(uint16_t templateId, uint8_t assignee = 255) {
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            if (activeTasks[i].id != templateId) continue;
            if (assignee == 255 || activeTasks[i].assignedTo == assignee) return true;
        }
        return false;
    }

    // ─── Streak Evaluation ───────────────────────────────────────────────
    // Called at end of day (before resetting counters).
    // Rules:
    //   - Child must have completed at least 1 task today
    //   - No parent overrides today (remindedToday == false)
    //   - Met their daily goal
    //   → streak increments
    //   Otherwise → streak resets to 0
    //
    // Streak bonus points are awarded at thresholds (5, 10, 30 days)
    // and added directly to the member's totalPoints.

    void _evaluateStreaks() {
        for (uint8_t i = 0; i < familyCount; i++) {
            FamilyMember& m = family[i];
            if (m.role != Role::CHILD) continue;

            bool qualified =
                m.dailyTasksCompleted > 0 &&     // Did at least 1 task
                !m.remindedToday &&                // No overrides
                m.dailyTasksCompleted >= m.dailyGoal;  // Met goal

            if (qualified) {
                m.currentStreak++;
                Serial.printf("[STREAK] %s: streak → %d days\n",
                              m.name, m.currentStreak);

                // Check for streak bonus thresholds
                uint8_t bonus = 0;
                if (m.currentStreak == 5)  bonus = PointTable::STREAK_BONUS_5_DAYS;
                if (m.currentStreak == 10) bonus = PointTable::STREAK_BONUS_10_DAYS;
                if (m.currentStreak == 30) bonus = PointTable::STREAK_BONUS_30_DAYS;

                if (bonus > 0) {
                    m.totalPoints += bonus;
                    Serial.printf("[STREAK] %s earned %d-day streak bonus: +%d pts!\n",
                                  m.name, m.currentStreak, bonus);
                }
            } else {
                if (m.currentStreak > 0) {
                    Serial.printf("[STREAK] %s: streak reset (was %d days). "
                                  "Tasks=%d/%d, overrides=%s\n",
                                  m.name, m.currentStreak,
                                  m.dailyTasksCompleted, m.dailyGoal,
                                  m.remindedToday ? "yes" : "no");
                }
                m.currentStreak = 0;
            }
        }
    }
};
