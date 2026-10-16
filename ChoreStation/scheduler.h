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
    // Called when a streak bonus is awarded at the daily reset (member, points),
    // so the UI can tell the child next time they scan. Set in setup().
    void (*onStreakBonus)(uint8_t memberId, uint8_t points) = nullptr;

    // ─── Today's counts, derived from what's saved ───────────────────────
    // Worked out from the task list (active.json) and routine masks rather
    // than a counter, so a reboot mid-day doesn't wipe a child's progress.

    // Chores finished and approved today, plus routines finished today.
    uint8_t doneToday(uint8_t m) const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            const Task& t = activeTasks[i];
            if (t.assignedTo == m && t.state == TaskState::COMPLETED && !t.doneEarlierDay) n++;
        }
        for (uint8_t r = 0; r < routines.count(); r++)
            if (routines.isDoneToday(r, m)) n++;
        return n;
    }

    // Finished today but waiting for a parent. Counts toward the goal and the
    // streak (approval speed shouldn't cost a streak), except jobs a child
    // made up for themselves, which only count once a parent has seen them.
    uint8_t pendingToday(uint8_t m) const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            const Task& t = activeTasks[i];
            if (t.assignedTo != m || t.state != TaskState::PENDING_REVIEW || t.doneEarlierDay) continue;
            bool selfMade = t.type == TaskType::SELF_INIT ||
                            (t.type == TaskType::ADHOC && t.createdBy == t.assignedTo);
            if (!selfMade) n++;
        }
        return n;
    }

    // A parent took back a "did it on my own" claim today (breaks the streak).
    bool overriddenToday(uint8_t m) const {
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            const Task& t = activeTasks[i];
            if (t.assignedTo == m && t.state == TaskState::COMPLETED && !t.doneEarlierDay &&
                t.parentOverrodeReminder) return true;
        }
        return false;
    }

    void begin() {
        _lastScheduleCheckMs = 0;
        _recurringLoaded = false;
        _recurringCount = 0;

        // Load recurring templates
        _recurringLoaded = storage.loadTasks(
            _recurringTemplates, _recurringCount, "/tasks/recurring.json");

        if (_recurringLoaded) {
            Serial.printf("[SCHED] Loaded %d recurring templates.\n", _recurringCount);
            if (_syncActiveWithTemplates()) backups.markTasksDirty();
        } else {
            Serial.println("[SCHED] No recurring templates found.");
        }
    }

    // ─── Call from main loop ─────────────────────────────────────────────
    // Checks for day rollover and periodic task generation.
    // Returns true if active tasks were modified (caller should redraw).

    bool update() {
        bool changed = false;

        // Day rollover. With the clock set, compare today's date with the
        // saved date of the last reset: this also catches a station that was
        // off over midnight (it resets once when it comes back). Checked
        // before routines.syncDayIfNeeded() so the streak still sees
        // yesterday's finished routines. Offline: uptime-based rollover.
        if (timeMan.isTimeSynced()) {
            char today[11];
            timeMan.currentDateStr(today, sizeof(today));
            if (today[0] != '0') {
                if (!_stampLoaded) { storage.loadDayStamp(_resetStamp, sizeof(_resetStamp)); _stampLoaded = true; }
                bool firstCheck = !_stampChecked;
                _stampChecked = true;
                if (_resetStamp[0] == '\0') {                  // first run: today is day one
                    strlcpy(_resetStamp, today, sizeof(_resetStamp));
                    storage.saveDayStamp(_resetStamp);
                } else if (strcmp(_resetStamp, today) != 0) {
                    // At midnight, or right after boot (station was off over
                    // midnight): reset. But if the clock only became known
                    // long after boot (WiFi came up late), today's chores may
                    // already be done on the old date: adopt today instead of
                    // wiping them.
                    if (firstCheck && millis() > LATE_SYNC_MS) {
                        Serial.println("[SCHED] Clock set late - keeping today's progress, no catch-up reset.");
                        strlcpy(_resetStamp, today, sizeof(_resetStamp));
                        storage.saveDayStamp(_resetStamp);
                    } else {
                        changed = performDailyReset();
                    }
                }
            }
        } else if (timeMan.hasDayRolledOver()) {
            changed = performDailyReset();
        }

        // Catch a new day for routine masks too (idempotent).
        routines.syncDayIfNeeded();

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
                newTask.doneEarlierDay = false;
                newTask.claimedFromAnyone = false;
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

    // ─── Parent rejected a chore finished on an earlier day ──────────────
    // It has to be redone. If today's copy is already on the list, that copy
    // is the redo, so this one is removed and nobody sees the chore twice;
    // otherwise it becomes today's copy. Returns true if it was removed.
    bool settleRejected(uint8_t idx) {
        if (idx >= activeTaskCount) return false;
        Task& t = activeTasks[idx];
        if (!t.doneEarlierDay) return false;
        t.doneEarlierDay = false;
        if (t.type != TaskType::RECURRING) return false;

        const Task* tmpl = _findTemplate(t.id);
        bool perChild = tmpl && tmpl->assignedTo == 254;
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            const Task& o = activeTasks[i];
            if (i == idx || o.id != t.id || o.type != TaskType::RECURRING || o.doneEarlierDay) continue;
            if (perChild && o.assignedTo != t.assignedTo) continue;
            Serial.printf("[SCHED] Rejected '%s' dropped: today's copy is the redo\n", t.name);
            for (uint8_t k = idx; k + 1 < activeTaskCount; k++)
                memcpy(&activeTasks[k], &activeTasks[k + 1], sizeof(Task));
            activeTaskCount--;
            backups.markTasksDirty();
            return true;
        }
        return false;
    }

    // ─── Daily Reset ─────────────────────────────────────────────────────
    // Called once when the day rolls over.

    bool performDailyReset() {
        Serial.println("[SCHED] ═══ DAILY RESET ═══");

        // 1. Evaluate streaks for each child
        _evaluateStreaks();

        // 2. Reset daily counters (legacy fields; today's counts are derived)
        for (uint8_t i = 0; i < familyCount; i++) {
            family[i].dailyTasksCompleted = 0;
            family[i].remindedToday = false;
            family[i].remindersToday = 0;
            Serial.printf("[SCHED] Reset daily counters for %s\n", family[i].name);
        }

        // 2b. Reset routine completion masks for the new day
        routines.resetDaily();

        // 3. Clear completed and timed-out tasks from active list, and
        //    unfinished copies of chores that expire daily
        uint8_t newCount = 0;
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            Task& t = activeTasks[i];
            if (t.state == TaskState::COMPLETED ||
                t.state == TaskState::TIMED_OUT) {
                // Skip — these are done
                continue;
            }
            bool recurring = t.type == TaskType::RECURRING;
            if (recurring && t.expiresDaily &&
                (t.state == TaskState::AVAILABLE || t.state == TaskState::IN_PROGRESS)) {
                Serial.printf("[SCHED] '%s' expired (not done)\n", t.name);
                continue;
            }
            // Work already done keeps waiting for a parent, but today's copy
            // of the chore still appears alongside it, and it already counted
            // toward yesterday (as pending), so it doesn't count again today.
            // (A child's self-made job isn't counted while pending, so it
            // counts on the day a parent approves it instead.)
            bool selfMade = t.type == TaskType::SELF_INIT ||
                            (t.type == TaskType::ADHOC && t.createdBy == t.assignedTo);
            if (t.state == TaskState::PENDING_REVIEW && !selfMade) t.doneEarlierDay = true;
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
        if (dateStr[0] != '0') {                              // remember today's reset
            strlcpy(_resetStamp, dateStr, sizeof(_resetStamp));
            _stampLoaded = true;
            storage.saveDayStamp(_resetStamp);
        }
        backups.createDailyBackup(dateStr);

        // 7. Archive old logs
        backups.archiveOldLogs(dateStr);

        Serial.println("[SCHED] ═══ DAILY RESET COMPLETE ═══\n");
        return true;
    }

private:
    // Make today's not-yet-started recurring tasks match the (possibly just
    // edited) templates: drop ones whose template was deleted, refresh the
    // rest. Started, pending and finished tasks are left alone, as are
    // one-off and kid-added tasks. Returns true if anything changed.
    bool _syncActiveWithTemplates() {
        bool changed = false;
        uint8_t keep = 0;
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            Task& t = activeTasks[i];
            bool drop = false;
            if (t.type == TaskType::RECURRING && t.state == TaskState::AVAILABLE) {
                const Task* tmpl = _findTemplate(t.id);
                if (!tmpl) {
                    drop = true;
                    Serial.printf("[SCHED] '%s' removed (template deleted)\n", t.name);
                } else if (tmpl->assignedTo == 254 &&
                           (t.assignedTo >= familyCount || family[t.assignedTo].role != Role::CHILD)) {
                    // Now "Per child": today's shared (first-taker) copy goes;
                    // generateRecurringTasks() makes each child's own copy.
                    drop = true;
                    Serial.printf("[SCHED] '%s' is per child now - shared copy replaced\n", t.name);
                } else {
                    uint8_t who = t.assignedTo;
                    if (tmpl->assignedTo != 254) who = tmpl->assignedTo;   // per-child copies keep theirs
                    if (strcmp(t.name, tmpl->name) || t.category != tmpl->category ||
                        t.size != tmpl->size || t.minimumTimeSec != tmpl->minimumTimeSec ||
                        t.timeoutTimeSec != tmpl->timeoutTimeSec ||
                        t.requiresApproval != tmpl->requiresApproval ||
                        t.isOwnMess != tmpl->isOwnMess || t.expiresDaily != tmpl->expiresDaily ||
                        t.assignedTo != who) {
                        strlcpy(t.name, tmpl->name, sizeof(t.name));
                        t.category = tmpl->category;
                        t.size = tmpl->size;
                        t.minimumTimeSec = tmpl->minimumTimeSec;
                        t.timeoutTimeSec = tmpl->timeoutTimeSec;
                        t.requiresApproval = tmpl->requiresApproval;
                        t.isOwnMess = tmpl->isOwnMess;
                        t.expiresDaily = tmpl->expiresDaily;
                        t.recurDays = tmpl->recurDays;
                        t.recurHour = tmpl->recurHour;
                        t.recurMinute = tmpl->recurMinute;
                        t.assignedTo = who;
                        changed = true;
                    }
                    // A template no longer shared per child keeps one copy
                    if (tmpl->assignedTo != 254) {
                        for (uint8_t k = 0; k < keep && !drop; k++)
                            drop = activeTasks[k].id == t.id &&
                                   activeTasks[k].type == TaskType::RECURRING &&
                                   activeTasks[k].state == TaskState::AVAILABLE;
                    }
                }
            }
            if (drop) { changed = true; continue; }
            if (keep != i) memcpy(&activeTasks[keep], &t, sizeof(Task));
            keep++;
        }
        activeTaskCount = keep;
        return changed;
    }

    const Task* _findTemplate(uint16_t id) const {
        for (uint8_t i = 0; i < _recurringCount; i++)
            if (_recurringTemplates[i].id == id) return &_recurringTemplates[i];
        return nullptr;
    }

    char     _resetStamp[11] = "";   // date of the last daily reset (/stats/day.json)
    bool     _stampLoaded = false;
    bool     _stampChecked = false;              // first dated check since boot done
    static const uint32_t LATE_SYNC_MS = 600000; // clock known >10 min after boot = "late"
    Task     _recurringTemplates[MAX_ACTIVE_TASKS];
    uint8_t  _recurringCount;
    bool     _recurringLoaded;
    uint32_t _lastScheduleCheckMs;

    // An instance of this template already exists today. assignee 255 = any
    // (a claimed "Anyone" task keeps the template id but changes assignee).
    bool _isAlreadyActive(uint16_t templateId, uint8_t assignee = 255) {
        for (uint8_t i = 0; i < activeTaskCount; i++) {
            if (activeTasks[i].id != templateId) continue;
            if (activeTasks[i].doneEarlierDay) continue;   // an earlier day's, awaiting approval
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

            // Finished + waiting for a parent, so a slow approval can't cost
            // the streak. (Runs before step 3 clears the finished tasks.)
            uint8_t did = doneToday(i) + pendingToday(i);
            bool reminded = overriddenToday(i);
            bool qualified =
                did > 0 &&                  // Did at least 1 task
                !reminded &&                // No overrides
                did >= m.dailyGoal;         // Met goal

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
                    if (onStreakBonus) onStreakBonus(i, bonus);
                    Serial.printf("[STREAK] %s earned %d-day streak bonus: +%d pts!\n",
                                  m.name, m.currentStreak, bonus);
                }
            } else {
                if (m.currentStreak > 0) {
                    Serial.printf("[STREAK] %s: streak reset (was %d days). "
                                  "Tasks=%d/%d, overrides=%s\n",
                                  m.name, m.currentStreak,
                                  did, m.dailyGoal, reminded ? "yes" : "no");
                }
                m.currentStreak = 0;
            }
        }
    }
};
