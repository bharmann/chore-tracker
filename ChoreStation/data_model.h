// =============================================================================
// data_model.h — Task, User, and Point Economy Data Structures
// =============================================================================
// Designed for storage on SD card as JSON files.
// Kept as flat C structs for memory efficiency on ESP32 (520KB SRAM).
// =============================================================================

#pragma once
#include <Arduino.h>

// ─── Constants ───────────────────────────────────────────────────────────────

static const uint8_t MAX_FAMILY_MEMBERS = 6;
static const uint8_t MAX_ACTIVE_TASKS   = 32;   // Tasks loaded in RAM at once
static const uint8_t UID_LENGTH         = 7;     // RFID UID max bytes (MIFARE)
static const uint8_t NAME_LENGTH        = 16;    // Display name max chars
static const uint8_t MAX_ROUTINES       = 4;     // Routine templates in RAM
static const uint8_t MAX_ROUTINE_STEPS  = 10;    // Steps per routine
static const uint8_t ROUTINE_STEP_LEN   = 40;    // Step label: 39 chars + NUL (one line at text size 1)

// ─── Enums ───────────────────────────────────────────────────────────────────

enum class Role : uint8_t {
    PARENT = 0,
    CHILD  = 1
};

enum class TaskCategory : uint8_t {
    CLEANING_TIDYING    = 0,   // Household organization
    HEALTH_HYGIENE      = 1,   // Personal care
    FAMILY_SUPPORT      = 2,   // Helping others, collaborative work
    ACADEMIC_LEARNING   = 3,   // Educational activities
    PET_CARE            = 4,   // Animal responsibilities
    OUTDOOR_MAINTENANCE = 5,   // Yard work, exterior
    CATEGORY_COUNT      = 6
};

enum class TaskSize : uint8_t {
    SMALL  = 0,   // 1-2 points, ~5-15 min
    MEDIUM = 1,   // 3-5 points, ~15-45 min
    LARGE  = 2    // 6-8 points, ~45+ min
};

enum class TaskType : uint8_t {
    RECURRING   = 0,   // Scheduled, auto-generated
    ADHOC       = 1,   // Created on-the-fly by parent
    SELF_INIT   = 2    // Proposed by child
};

enum class TaskState : uint8_t {
    AVAILABLE      = 0,   // Ready to be started
    IN_PROGRESS    = 1,   // Someone started it
    PENDING_REVIEW = 2,   // Completed, awaiting parent approval
    COMPLETED      = 3,   // Done and approved
    TIMED_OUT      = 4    // Auto-aborted
};

// ─── Point values per size ───────────────────────────────────────────────────
// These are the BASE point values. Bonuses stack on top.

struct PointTable {
    static constexpr uint8_t base(TaskSize size) {
        switch (size) {
            case TaskSize::SMALL:  return 1;
            case TaskSize::MEDIUM: return 4;
            case TaskSize::LARGE:  return 7;
            default:               return 0;
        }
    }

    // "Own mess" modifier: cleaning up after yourself = reduced points
    static constexpr uint8_t ownMess(TaskSize size) {
        switch (size) {
            case TaskSize::SMALL:  return 1;   // Still 1 (floor)
            case TaskSize::MEDIUM: return 2;   // Reduced from 4
            case TaskSize::LARGE:  return 4;   // Reduced from 7
            default:               return 0;
        }
    }

    // Bonus points
    static const uint8_t INITIATIVE_BONUS       = 2;  // Self-proposed task
    static const uint8_t NO_REMINDER_BONUS      = 1;  // Completed without nagging
    static const uint8_t STREAK_BONUS_5_DAYS    = 3;  // 5 consecutive days
    static const uint8_t STREAK_BONUS_10_DAYS   = 5;  // 10 consecutive days
    static const uint8_t STREAK_BONUS_30_DAYS   = 10; // 30 consecutive days
};

// ─── Family Member ───────────────────────────────────────────────────────────

struct FamilyMember {
    uint8_t  id;                      // 0-based index
    char     name[NAME_LENGTH];       // Display name
    uint8_t  rfidUid[UID_LENGTH];     // RFID tag UID
    uint8_t  rfidUidLength;           // Actual UID length (4 or 7)
    Role     role;                    // PARENT or CHILD

    // Running stats (persisted to SD daily)
    uint32_t totalPoints;
    uint16_t currentStreak;           // Consecutive days of independent completion
    uint8_t  dailyTasksCompleted;     // Legacy: today's counts are derived from the task list (Scheduler::doneToday)
    uint8_t  dailyGoal;              // Target tasks per day (set by parent)
    bool     remindedToday;          // True if parent had to remind today
    uint8_t  remindersToday;         // Count of reminders issued today (for streak eval)
};

// ─── Task Definition ─────────────────────────────────────────────────────────

struct Task {
    uint16_t    id;                   // Unique task ID
    char        name[48];             // Short description (expanded for natural phrasing)
    TaskCategory category;
    TaskSize    size;
    TaskType    type;
    TaskState   state;

    uint8_t     assignedTo;           // FamilyMember.id (255 = unassigned)
    uint8_t     createdBy;            // FamilyMember.id (who created/proposed)

    uint32_t    startedAtMs;          // millis() when the task was started
    uint32_t    completedAtMs;        // millis() when the task was finished
    uint32_t    startedEpoch;         // wall-clock start (0 = unknown); persisted so
                                      // in-progress timers survive a reboot
    uint16_t    minimumTimeSec;       // Minimum seconds between start and complete
    uint16_t    timeoutTimeSec;       // Auto-abort after this many seconds (0=none)

    bool        requiresApproval;     // Parent must approve completion
    bool        isOwnMess;            // Reduced points (cleaning up own mess)

    // ── No-Reminder Claim (self-contained per task) ──────────────────────
    // The child toggles this ON via the task detail screen before scanning
    // to complete. It lives on the task, not as a global UI variable.
    // If toggled ON, completion ALWAYS requires parent approval.
    // Self-initiated tasks auto-set this to true (initiative subsumes it).

    bool        noReminderToggled;        // Child toggled "I did this on my own"
    bool        childClaimedNoReminder;   // Frozen at completion time from toggle
    bool        parentOverrodeReminder;   // Parent changed claim to false
    bool        noReminderApproved;       // Final resolved value after approval

    // Scheduling (for RECURRING type)
    uint8_t     recurDays;            // Bitmask: bit0=Sun, bit1=Mon, ..., bit6=Sat
    uint8_t     recurHour;            // Hour of day to generate (0-23)
    uint8_t     recurMinute;          // Minute (0-59)

    // Missed-chore handling (RECURRING). Unfinished copies normally carry
    // over to the next day; an expiring chore's are dropped at the daily
    // reset instead (feed the dog, make bed: yesterday's no longer matters).
    bool        expiresDaily;
    // Finished on an earlier day and still waiting for approval. Set at the
    // daily reset; such a copy doesn't block today's copy of the chore.
    bool        doneEarlierDay;
    // Was an "Anyone" (255) task until this person started it; "Put it back"
    // returns it to everyone.
    bool        claimedFromAnyone;
};

// ─── Routine (ordered checklist done in one sitting) ─────────────────────────
// A Routine bundles several quick steps (e.g. bedtime: pajamas, brush teeth,
// pack bag) under ONE scan. The child scans to reach their dashboard, taps the
// routine, checks off each step on-screen, and earns a flat bundle of points
// on full completion — no per-step RFID scan. Templates live in
// /config/routines.json; per-day completion is tracked in doneMask and reset
// each morning. Surfaced on the dashboard, highlighted during its time window.

struct Routine {
    uint8_t id;
    char    name[20];
    uint8_t assignedTo;       // member id, 254 = all children, 255 = anyone
    uint8_t bundlePoints;     // flat award on full completion
    uint8_t startHour;        // surfacing window start (0-23)
    uint8_t endHour;          // surfacing window end (0-23, may wrap past midnight)
    uint8_t recurDays;        // bitmask bit0=Sun..bit6=Sat (127 = daily)
    uint8_t stepCount;
    char    steps[MAX_ROUTINE_STEPS][ROUTINE_STEP_LEN];

    uint8_t doneMask;         // runtime: bit N = member N finished it today
    uint16_t stepMask[MAX_FAMILY_MEMBERS];  // runtime: steps checked today, per member
};

// ─── Daily Log Entry (written to SD) ─────────────────────────────────────────

struct CompletionLog {
    uint16_t taskId;
    uint8_t  memberId;
    uint8_t  basePoints;
    uint8_t  bonusPoints;           // Sum of all bonuses earned
    uint32_t completedEpoch;        // Unix timestamp
    bool     wasInitiative;         // Self-proposed task
    bool     childClaimedNoReminder; // What the child claimed
    bool     noReminderApproved;    // What was actually awarded
    bool     parentOverrode;        // True if parent changed the child's claim
    uint32_t durationSec;           // Start to finish (0 = one-tap chore or unknown)
};

// ─── Trajectory Status (computed, not stored) ────────────────────────────────

enum class Trajectory : uint8_t {
    AHEAD    = 0,   // More tasks done than expected at this hour
    ON_TRACK = 1,   // Within ±1 of expected pace
    BEHIND   = 2    // Fewer tasks done than expected
};

// Helper to compute trajectory based on time of day and tasks completed
inline Trajectory computeTrajectory(uint8_t completed, uint8_t dailyGoal,
                                     uint8_t currentHour, uint8_t wakeHour = 7,
                                     uint8_t bedHour = 21) {
    if (dailyGoal == 0) return Trajectory::ON_TRACK;
    if (currentHour < wakeHour) return Trajectory::ON_TRACK;
    if (currentHour >= bedHour) {
        return (completed >= dailyGoal) ? Trajectory::AHEAD : Trajectory::BEHIND;
    }

    // Linear interpolation: how many tasks should be done by now?
    float progress = (float)(currentHour - wakeHour) / (float)(bedHour - wakeHour);
    float expected = progress * dailyGoal;

    if ((float)completed >= expected + 1.0f) return Trajectory::AHEAD;
    if ((float)completed <= expected - 1.0f) return Trajectory::BEHIND;
    return Trajectory::ON_TRACK;
}
