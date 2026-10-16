// =============================================================================
// task_engine.h — Task Lifecycle State Machine & Point Calculator
// =============================================================================
//
// STATE TRANSITIONS:
//   AVAILABLE → IN_PROGRESS          (child taps START on task detail)
//   IN_PROGRESS → PENDING_REVIEW     (child taps I'M DONE, claim or approval needed)
//   IN_PROGRESS → COMPLETED          (child taps I'M DONE, no claim or approval)
//   PENDING_REVIEW → COMPLETED       (parent approves from parent dashboard)
//   PENDING_REVIEW → AVAILABLE       (parent rejects, task returns to queue)
//   IN_PROGRESS → AVAILABLE          (time limit hit, or Put it back)
//
// NO-REMINDER BONUS FLOW:
//   1. Child toggles "I did this on my own" on task detail (while IN_PROGRESS)
//   2. Child taps I'M DONE → toggle state frozen as the claim
//   3. If toggled ON: task goes to PENDING_REVIEW regardless of requiresApproval
//      Parent sees the claim and can APPROVE, OVERRIDE (remove claim), or REJECT
//   4. If toggled OFF: normal flow (approval if required, or direct complete)
//   5. Self-initiated tasks auto-set claim to true (initiative subsumes no-reminder)
//
// No sounds here: the UI plays exactly one cue per outcome (it knows whether
// a badge, an approval or a redo happened). Tones used to play twice.
//
// =============================================================================

#pragma once
#include <Arduino.h>
#include "data_model.h"
#include "milestones.h"
#include "audio_feedback.h"
#include "storage_manager.h"
#include "time_manager.h"

extern AudioFeedback audio;
extern StorageManager storage;
extern TimeManager timeMan;
extern FamilyMember family[];
extern uint8_t familyCount;

// ─── Result struct returned after a state transition ─────────────────────────

struct TransitionResult {
    bool        success;
    const char* message;
    TaskState   newState;
    uint8_t     pointsEarned;
    int8_t      newMilestone;       // -1 if none
};

class TaskEngine {
public:

    // ─── Start a task ────────────────────────────────────────────────────

    static TransitionResult startTask(Task& task, uint8_t userId) {
        TransitionResult r = { false, "", task.state, 0, -1 };

        if (task.state != TaskState::AVAILABLE) {
            r.message = "Task not available";
            return r;
        }

        if (task.assignedTo != userId && task.assignedTo != 255) {
            r.message = "Not your task";
            return r;
        }

        task.state = TaskState::IN_PROGRESS;
        task.startedAtMs = millis();
        task.startedEpoch = timeMan.currentEpoch();   // 0 if NTP not synced
        task.noReminderToggled = false;
        task.childClaimedNoReminder = false;
        task.parentOverrodeReminder = false;
        task.noReminderApproved = false;

        if (task.assignedTo == 255) {
            task.assignedTo = userId;
            task.claimedFromAnyone = true;
        }

        r.success = true;
        r.newState = TaskState::IN_PROGRESS;
        r.message = "Task started!";

        Serial.printf("[TASK] '%s' started by member %d\n", task.name, userId);
        return r;
    }

    // ─── Put a started task back (started by mistake / changed their mind) ─
    // Back to the list as if never started; an "Anyone" task is free again.

    static TransitionResult putBack(Task& task, uint8_t userId) {
        TransitionResult r = { false, "", task.state, 0, -1 };
        if (task.state != TaskState::IN_PROGRESS || task.assignedTo != userId) {
            r.message = "Not your task";
            return r;
        }
        task.state = TaskState::AVAILABLE;
        task.startedAtMs = 0;
        task.startedEpoch = 0;
        task.noReminderToggled = false;
        if (task.claimedFromAnyone) { task.assignedTo = 255; task.claimedFromAnyone = false; }
        r.success = true;
        r.newState = TaskState::AVAILABLE;
        r.message = "Put back";
        Serial.printf("[TASK] '%s' put back by member %d\n", task.name, userId);
        return r;
    }

    // ─── Complete a task (IN_PROGRESS → time checked) ───────────────────
    // Validates minimum time. Does NOT finalize — caller must then call
    // submitClaim() with the child's toggle state to resolve the task.

    static TransitionResult completeTask(Task& task, uint8_t userId,
                                          uint8_t taskArrayIdx) {
        TransitionResult r = { false, "", task.state, 0, -1 };

        if (task.state != TaskState::IN_PROGRESS) {
            r.message = "Task not in progress";
            return r;
        }

        if (task.assignedTo != userId) {
            r.message = "Not your task";
            return r;
        }

        // Check minimum time
        if (task.minimumTimeSec > 0) {
            uint32_t elapsed = (millis() - task.startedAtMs) / 1000;
            if (elapsed < task.minimumTimeSec) {
                r.message = "Not enough time";
                Serial.printf("[TASK] '%s' needs %ds more\n", task.name,
                              task.minimumTimeSec - (uint16_t)elapsed);
                return r;
            }
        }

        task.completedAtMs = millis();
        r.success = true;
        r.message = "Time validated";

        Serial.printf("[TASK] '%s' time validated — awaiting claim submission\n", task.name);
        return r;
    }

    // ─── Submit the child's claim and resolve task state ────────────────
    // claimsNoReminder = the toggle state from the task detail screen.
    // For SELF_INIT tasks, pass true (auto-granted).

    static TransitionResult submitClaim(Task& task, FamilyMember& member,
                                         bool claimsNoReminder) {
        TransitionResult r = { false, "", task.state, 0, -1 };

        // Self-initiated tasks: auto-grant claim, initiative bonus applies instead
        if (task.type == TaskType::SELF_INIT) {
            claimsNoReminder = true;  // Always true for self-init
        }

        task.childClaimedNoReminder = claimsNoReminder;

        if (claimsNoReminder) {
            // Claim made → ALWAYS requires parent review, even if the task
            // normally wouldn't need approval. The parent must verify the claim.
            task.state = TaskState::PENDING_REVIEW;
            r.success = true;
            r.newState = TaskState::PENDING_REVIEW;
            r.message = "Claim submitted! Ask parent to review.";
            Serial.printf("[CLAIM] '%s' — %s claims no reminder, pending parent review\n",
                          task.name, member.name);
        } else {
            // No claim → normal flow
            if (task.requiresApproval) {
                task.state = TaskState::PENDING_REVIEW;
                r.success = true;
                r.newState = TaskState::PENDING_REVIEW;
                r.message = "Waiting for approval";
            } else {
                // No claim, no approval needed → finalize directly
                return _finalizeCompletion(task, member);
            }
        }

        return r;
    }

    // ─── Parent approves task (accepts claim as-is) ──────────────────────

    static TransitionResult approveTask(Task& task, FamilyMember& assignee) {
        TransitionResult r = { false, "", task.state, 0, -1 };

        if (task.state != TaskState::PENDING_REVIEW) {
            r.message = "Nothing to approve";
            return r;
        }

        // Accept the child's claim as submitted
        task.noReminderApproved = task.childClaimedNoReminder;
        task.parentOverrodeReminder = false;

        Serial.printf("[APPROVE] '%s' — approved (claim=%s)\n",
                      task.name,
                      task.childClaimedNoReminder ? "no-reminder" : "standard");

        return _finalizeCompletion(task, assignee);
    }

    // ─── Parent approves task BUT overrides the no-reminder claim ────────
    // Task work is accepted, but parent knows they actually reminded.

    static TransitionResult approveWithOverride(Task& task, FamilyMember& assignee) {
        TransitionResult r = { false, "", task.state, 0, -1 };

        if (task.state != TaskState::PENDING_REVIEW) {
            r.message = "Nothing to approve";
            return r;
        }

        if (!task.childClaimedNoReminder) {
            // Child didn't claim it — nothing to override, just approve normally
            return approveTask(task, assignee);
        }

        // Override: accept the task, reject the claim
        task.parentOverrodeReminder = true;
        task.noReminderApproved = false;

        Serial.printf("[OVERRIDE] '%s' — parent overrode no-reminder claim for %s\n",
                      task.name, assignee.name);

        return _finalizeCompletion(task, assignee);
    }

    // ─── Parent rejects task entirely ────────────────────────────────────

    static TransitionResult rejectTask(Task& task) {
        TransitionResult r = { false, "", task.state, 0, -1 };

        if (task.state != TaskState::PENDING_REVIEW) {
            r.message = "Nothing to reject";
            return r;
        }

        task.state = TaskState::AVAILABLE;  // Go back to queue
        task.startedAtMs = 0;
        task.startedEpoch = 0;
        task.completedAtMs = 0;
        task.childClaimedNoReminder = false;
        task.parentOverrodeReminder = false;
        task.noReminderApproved = false;

        r.success = true;
        r.newState = TaskState::AVAILABLE;
        r.message = "Try again!";
        Serial.printf("[REJECT] '%s' rejected — returned to queue\n", task.name);

        return r;
    }

    // ─── Timeout ─────────────────────────────────────────────────────────
    // True when a running task has hit its time limit. The caller puts it
    // back on the list and tells the child (TIMED_OUT is no longer entered;
    // the state stays in the enum for old saved files, cleared at reset).

    static bool checkTimeout(const Task& task) {
        if (task.state != TaskState::IN_PROGRESS) return false;
        if (task.timeoutTimeSec == 0) return false;
        uint32_t elapsed = (millis() - task.startedAtMs) / 1000;
        return elapsed >= task.timeoutTimeSec;
    }

    // ─── Utility ─────────────────────────────────────────────────────────

    static void formatElapsed(uint32_t startMs, char* buf, size_t bufLen) {
        uint32_t elapsed = (millis() - startMs) / 1000;
        if (elapsed < 60) {
            snprintf(buf, bufLen, "%lus", (unsigned long)elapsed);
        } else if (elapsed < 3600) {
            snprintf(buf, bufLen, "%lum %lus", (unsigned long)(elapsed / 60), (unsigned long)(elapsed % 60));
        } else {
            snprintf(buf, bufLen, "%luh %lum", (unsigned long)(elapsed / 3600), (unsigned long)((elapsed % 3600) / 60));
        }
    }

    static int32_t remainingMinTime(const Task& task) {
        if (task.minimumTimeSec == 0) return 0;
        if (task.state != TaskState::IN_PROGRESS) return 0;
        uint32_t elapsed = (millis() - task.startedAtMs) / 1000;
        return (int32_t)task.minimumTimeSec - (int32_t)elapsed;
    }

private:

    // ─── Finalize: calculate points, check milestones, log ───────────────

    static TransitionResult _finalizeCompletion(Task& task, FamilyMember& member) {
        TransitionResult r = { true, "Completed!", TaskState::COMPLETED, 0, -1 };

        task.state = TaskState::COMPLETED;

        // ── Base points ──
        uint8_t basePoints = task.isOwnMess
            ? PointTable::ownMess(task.size)
            : PointTable::base(task.size);

        uint8_t bonusPoints = 0;

        // ── Initiative bonus (self-proposed tasks) ──
        // This is the HIGHER bonus and does NOT stack with no-reminder.
        bool wasInitiative = (task.type == TaskType::SELF_INIT);
        if (wasInitiative) {
            bonusPoints += PointTable::INITIATIVE_BONUS;
            // No-reminder is implicitly true for self-init, but we don't
            // add the separate bonus — initiative subsumes it.
            task.noReminderApproved = true;  // For logging accuracy
        }

        // ── No-reminder bonus (child claimed + parent approved) ──
        // Only applies to non-self-init tasks where the claim was approved.
        if (!wasInitiative && task.noReminderApproved) {
            bonusPoints += PointTable::NO_REMINDER_BONUS;
        }

        uint8_t totalPoints = basePoints + bonusPoints;

        // ── Update member stats ──
        uint32_t prevPoints = member.totalPoints;
        member.totalPoints += totalPoints;
        member.dailyTasksCompleted++;

        // Track whether this child had any overrides today (affects streak)
        // A child who never gets overridden = fully honest day = streak eligible
        if (task.parentOverrodeReminder) {
            member.remindedToday = true;
            member.remindersToday++;
        }

        r.pointsEarned = totalPoints;

        // ── Check for new milestones ──
        for (uint8_t i = 0; i < MILESTONE_COUNT; i++) {
            if (prevPoints < MILESTONES[i].pointThreshold &&
                member.totalPoints >= MILESTONES[i].pointThreshold) {
                r.newMilestone = i;
                Serial.printf("[MILESTONE] %s earned '%s' (%d pts)!\n",
                              member.name, MILESTONES[i].name,
                              MILESTONES[i].pointThreshold);
                break;
            }
        }

        // ── Log to SD with full audit trail ──
        CompletionLog log;
        log.taskId = task.id;
        log.memberId = member.id;
        log.basePoints = basePoints;
        log.bonusPoints = bonusPoints;
        log.completedEpoch = timeMan.currentEpoch();
        log.wasInitiative = wasInitiative;
        log.childClaimedNoReminder = task.childClaimedNoReminder;
        log.noReminderApproved = task.noReminderApproved;
        log.parentOverrode = task.parentOverrodeReminder;
        log.durationSec = (task.startedAtMs && task.completedAtMs)
            ? (task.completedAtMs - task.startedAtMs) / 1000 : 0;

        char dateStr[11];
        timeMan.currentDateStr(dateStr, sizeof(dateStr));
        storage.appendLog(log, dateStr);

        // ── Result message ──
        static char msgBuf[48];
        if (bonusPoints > 0) {
            snprintf(msgBuf, sizeof(msgBuf), "+%d pts (+%d bonus!)", totalPoints, bonusPoints);
        } else {
            snprintf(msgBuf, sizeof(msgBuf), "+%d pts", totalPoints);
        }
        r.message = msgBuf;

        Serial.printf("[TASK] '%s' completed: %d base + %d bonus = %d total "
                      "(claim=%d, approved=%d, override=%d)\n",
                      task.name, basePoints, bonusPoints, totalPoints,
                      task.childClaimedNoReminder,
                      task.noReminderApproved,
                      task.parentOverrodeReminder);

        return r;
    }
};

