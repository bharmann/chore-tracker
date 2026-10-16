// =============================================================================
// routine_manager.h — Named Routines (bedtime / get-ready-for-school)
// =============================================================================
// A Routine is an ordered checklist done in one sitting under a SINGLE scan.
// The child scans to reach their dashboard, taps the routine, checks off each
// step on-screen, and earns a flat bundle of points on full completion. There
// is no per-step RFID scan — the whole point is to make multi-step daily
// routines seamless and quick.
//
// Templates load from /config/routines.json (hand-editable, like recurring
// tasks). Per-day completion is tracked in each routine's doneMask (bit N =
// member N finished today) and persisted to /stats/routines.json so a reboot
// mid-day doesn't let a child re-earn the bundle. resetDaily() clears the masks
// at the day rollover, wired into the Scheduler like task/counter resets.
// Checked-off steps are kept the same way (stepMask, per member), so leaving
// a routine halfway and coming back keeps the checkmarks.
//
// Surfacing: a routine is "active today" when its recurDays bitmask matches the
// current day (or, offline with no NTP, when it recurs daily). It is
// "in window" when the current hour falls in [startHour, endHour], with wrap
// support so a bedtime window like 19->23 (or 21->1) works.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "data_model.h"
#include "storage_manager.h"
#include "time_manager.h"

extern StorageManager storage;
extern TimeManager    timeMan;
extern FamilyMember   family[];
extern uint8_t        familyCount;

class RoutineManager {
public:
    void begin() {
        _count = 0;
        _loaded = storage.loadRoutines(_routines, _count);
        if (!_loaded) {
            Serial.println("[ROUTINE] No routines.json found.");
            return;
        }

        // Restore today's completion masks.
        char savedDate[11] = "";
        if (storage.loadRoutineState(_routines, _count, savedDate, sizeof(savedDate))) {
            strlcpy(_stateDate, savedDate, sizeof(_stateDate));
        }
        // A new day is NOT wiped here: Scheduler::update() first runs the
        // daily reset (whose streak check needs yesterday's finished
        // routines), then calls syncDayIfNeeded().
        Serial.printf("[ROUTINE] Ready (%d routines).\n", _count);
    }

    // Detect a new calendar day even when no rollover event fired (e.g. booted
    // after midnight). Safe to call every loop; only acts on an actual change.
    void syncDayIfNeeded() {
        if (!timeMan.isTimeSynced()) return;
        char today[11];
        timeMan.currentDateStr(today, sizeof(today));
        if (today[0] == '0') return;                 // date not really known yet
        if (strcmp(_stateDate, today) != 0) {
            _clearMasks();
            strlcpy(_stateDate, today, sizeof(_stateDate));
            _persist();
            Serial.println("[ROUTINE] New day — routine masks reset.");
        }
    }

    uint8_t count() const { return _count; }
    Routine& get(uint8_t idx) { return _routines[idx]; }
    const Routine& get(uint8_t idx) const { return _routines[idx]; }

    // Is this routine scheduled for today? Offline (no NTP) we can't know the
    // day of week, so only daily routines (recurDays == 127) surface.
    bool activeToday(uint8_t idx) const {
        if (idx >= _count) return false;
        if (!timeMan.isTimeSynced()) return _routines[idx].recurDays == 127;
        uint8_t dowBit = 1 << timeMan.currentDayOfWeek();
        return (_routines[idx].recurDays & dowBit) != 0;
    }

    // Is the current hour within the routine's surfacing window? Supports
    // windows that wrap past midnight (startHour > endHour).
    bool inWindow(uint8_t idx) const {
        if (idx >= _count) return false;
        uint8_t h = timeMan.currentHour();
        uint8_t s = _routines[idx].startHour;
        uint8_t e = _routines[idx].endHour;
        if (s <= e) return h >= s && h <= e;
        return h >= s || h <= e;   // wraps midnight
    }

    // Is this routine assigned to (or available to) this member?
    bool visibleFor(uint8_t idx, uint8_t memberId) const {
        if (idx >= _count || memberId >= familyCount) return false;
        uint8_t a = _routines[idx].assignedTo;
        if (a == 255) return true;                       // anyone
        if (a == 254) return family[memberId].role == Role::CHILD;  // all children
        return a == memberId;
    }

    bool isDoneToday(uint8_t idx, uint8_t memberId) const {
        if (idx >= _count || memberId >= 8) return false;
        return (_routines[idx].doneMask & (1 << memberId)) != 0;
    }

    // Mark a routine complete for a member and persist the state.
    void markDone(uint8_t idx, uint8_t memberId) {
        if (idx >= _count || memberId >= 8) return;
        _routines[idx].doneMask |= (1 << memberId);
        _persist();
    }

    // Steps checked off today. Kept (and saved to the SD card) so a child who
    // leaves partway through comes back to the same checkmarks.
    bool stepDone(uint8_t idx, uint8_t memberId, uint8_t step) const {
        if (idx >= _count || memberId >= MAX_FAMILY_MEMBERS || step >= MAX_ROUTINE_STEPS) return false;
        return (_routines[idx].stepMask[memberId] & (1u << step)) != 0;
    }
    uint8_t stepsDone(uint8_t idx, uint8_t memberId) const {
        uint8_t n = 0;
        if (idx >= _count) return 0;
        for (uint8_t s = 0; s < _routines[idx].stepCount; s++) n += stepDone(idx, memberId, s);
        return n;
    }
    void toggleStep(uint8_t idx, uint8_t memberId, uint8_t step) {
        if (idx >= _count || memberId >= MAX_FAMILY_MEMBERS || step >= MAX_ROUTINE_STEPS) return;
        _routines[idx].stepMask[memberId] ^= (1u << step);
        _persist();
    }

    // Clear all masks (called at day rollover) and persist.
    void resetDaily() {
        _clearMasks();
        _persist();
        Serial.println("[ROUTINE] Daily completion reset.");
    }

private:
    Routine  _routines[MAX_ROUTINES];
    uint8_t  _count = 0;
    bool     _loaded = false;
    char     _stateDate[11] = "";   // date the current masks belong to

    void _clearMasks() {
        for (uint8_t i = 0; i < _count; i++) {
            _routines[i].doneMask = 0;
            memset(_routines[i].stepMask, 0, sizeof(_routines[i].stepMask));
        }
    }

    void _persist() {
        char today[11];
        timeMan.currentDateStr(today, sizeof(today));
        strlcpy(_stateDate, today, sizeof(_stateDate));
        storage.saveRoutineState(_routines, _count, today);
    }
};
