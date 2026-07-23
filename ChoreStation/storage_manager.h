// =============================================================================
// storage_manager.h — SD Card Persistence Layer
// =============================================================================
// File structure on SD card:
//   /config/
//     family.json      — Family member definitions (names, UIDs, roles)
//     settings.json    — System settings (daily goals, wake/bed hours)
//   /tasks/
//     recurring.json   — Recurring task templates
//     active.json      — Currently active/available tasks
//   /logs/
//     YYYY-MM-DD.json  — Daily completion logs (one file per day)
//   /stats/
//     stats.json       — Accumulated points, streaks
//
// Design notes:
// - We read family + recurring tasks into RAM on boot
// - Active tasks stay in RAM and are flushed to SD on changes
// - Completion logs are append-only (written after each completion)
// - Stats are updated and saved daily or on significant events
// =============================================================================

#pragma once
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include "pins.h"
#include "data_model.h"

class StorageManager {
public:
    bool begin() {
        // SD card is on VSPI — ensure RC522 CS is deselected
        pinMode(PIN_RFID_CS, OUTPUT);
        digitalWrite(PIN_RFID_CS, HIGH);

        // Initialize SD card
        // Note: SPI.begin() for VSPI should be called once before this
        if (!SD.begin(PIN_SD_CS)) {
            Serial.println("[SD] Card mount failed!");
            return false;
        }

        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("[SD] Card mounted. Size: %lluMB\n", cardSize);

        // Ensure directory structure exists
        _ensureDir("/config");
        _ensureDir("/tasks");
        _ensureDir("/logs");
        _ensureDir("/stats");

        return true;
    }

    // ─── Family Members ──────────────────────────────────────────────────

    bool loadFamily(FamilyMember* members, uint8_t& count) {
        File f = SD.open("/config/family.json", FILE_READ);
        if (!f) {
            Serial.println("[SD] No family.json found — first run?");
            count = 0;
            return false;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err) {
            Serial.printf("[SD] family.json parse error: %s\n", err.c_str());
            return false;
        }

        JsonArray arr = doc["members"].as<JsonArray>();
        count = 0;
        for (JsonObject m : arr) {
            if (count >= MAX_FAMILY_MEMBERS) break;
            FamilyMember& fm = members[count];
            fm.id = count;
            strlcpy(fm.name, m["name"] | "Unknown", NAME_LENGTH);
            fm.role = (m["role"].as<String>() == "parent") ? Role::PARENT : Role::CHILD;
            fm.dailyGoal = m["dailyGoal"] | 5;
            fm.totalPoints = m["totalPoints"] | 0;
            fm.currentStreak = m["streak"] | 0;
            fm.dailyTasksCompleted = 0;
            fm.remindedToday = false;

            // Parse RFID UID from hex string like "2B:B8:59:B1"
            String uidStr = m["uid"].as<String>();
            fm.rfidUidLength = _parseHexUid(uidStr, fm.rfidUid);

            count++;
        }

        Serial.printf("[SD] Loaded %d family members.\n", count);
        return true;
    }

    bool saveFamily(const FamilyMember* members, uint8_t count) {
        JsonDocument doc;
        JsonArray arr = doc["members"].to<JsonArray>();

        for (uint8_t i = 0; i < count; i++) {
            JsonObject m = arr.add<JsonObject>();
            m["name"] = members[i].name;
            m["role"] = (members[i].role == Role::PARENT) ? "parent" : "child";
            m["dailyGoal"] = members[i].dailyGoal;
            m["totalPoints"] = members[i].totalPoints;
            m["streak"] = members[i].currentStreak;

            // Serialize UID to hex string
            char uidBuf[32];
            _uidToHex(members[i].rfidUid, members[i].rfidUidLength, uidBuf);
            m["uid"] = uidBuf;
        }

        return _writeJson("/config/family.json", doc);
    }

    // ─── Tasks ───────────────────────────────────────────────────────────

    bool loadTasks(Task* tasks, uint8_t& count, const char* path) {
        File f = SD.open(path, FILE_READ);
        if (!f) {
            count = 0;
            return false;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err) {
            Serial.printf("[SD] Task parse error: %s\n", err.c_str());
            return false;
        }

        JsonArray arr = doc["tasks"].as<JsonArray>();
        count = 0;
        for (JsonObject t : arr) {
            if (count >= MAX_ACTIVE_TASKS) break;
            Task& task = tasks[count];
            task.id = t["id"] | 0;
            strlcpy(task.name, t["name"] | "", sizeof(task.name));
            task.category = static_cast<TaskCategory>(t["cat"].as<int>());
            task.size = static_cast<TaskSize>(t["size"].as<int>());
            task.type = static_cast<TaskType>(t["type"].as<int>());
            task.state = static_cast<TaskState>(t["state"].as<int>());
            task.assignedTo = t["assigned"] | 255;
            task.createdBy = t["creator"] | 0;
            task.minimumTimeSec = t["minTime"] | 0;
            task.timeoutTimeSec = t["timeout"] | 0;
            task.requiresApproval = t["approval"] | false;
            task.isOwnMess = t["ownMess"] | false;
            task.childClaimedNoReminder = t["childClaimed"] | false;
            task.parentOverrodeReminder = t["parentOverride"] | false;
            task.noReminderApproved = t["noRemApproved"] | false;
            task.noReminderToggled = t["noRemToggle"] | false;
            task.recurDays = t["recurDays"] | 0;
            task.recurHour = t["recurHour"] | 0;
            task.recurMinute = t["recurMin"] | 0;
            task.startedAtMs = 0;
            task.completedAtMs = 0;
            count++;
        }

        Serial.printf("[SD] Loaded %d tasks from %s\n", count, path);
        return true;
    }

    bool saveTasks(const Task* tasks, uint8_t count, const char* path) {
        JsonDocument doc;
        JsonArray arr = doc["tasks"].to<JsonArray>();

        for (uint8_t i = 0; i < count; i++) {
            const Task& t = tasks[i];
            JsonObject obj = arr.add<JsonObject>();
            obj["id"] = t.id;
            obj["name"] = t.name;
            obj["cat"] = static_cast<int>(t.category);
            obj["size"] = static_cast<int>(t.size);
            obj["type"] = static_cast<int>(t.type);
            obj["state"] = static_cast<int>(t.state);
            obj["assigned"] = t.assignedTo;
            obj["creator"] = t.createdBy;
            obj["minTime"] = t.minimumTimeSec;
            obj["timeout"] = t.timeoutTimeSec;
            obj["approval"] = t.requiresApproval;
            obj["ownMess"] = t.isOwnMess;
            obj["childClaimed"] = t.childClaimedNoReminder;
            obj["parentOverride"] = t.parentOverrodeReminder;
            obj["noRemApproved"] = t.noReminderApproved;
            obj["noRemToggle"] = t.noReminderToggled;
            obj["recurDays"] = t.recurDays;
            obj["recurHour"] = t.recurHour;
            obj["recurMin"] = t.recurMinute;
        }

        return _writeJson(path, doc);
    }

    // ─── Routines ────────────────────────────────────────────────────────

    bool loadRoutines(Routine* routines, uint8_t& count) {
        File f = SD.open("/config/routines.json", FILE_READ);
        if (!f) { count = 0; return false; }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            Serial.printf("[SD] routines.json parse error: %s\n", err.c_str());
            count = 0;
            return false;
        }

        JsonArray arr = doc["routines"].as<JsonArray>();
        count = 0;
        for (JsonObject r : arr) {
            if (count >= MAX_ROUTINES) break;
            Routine& rt = routines[count];
            rt.id           = r["id"] | (count + 1);
            strlcpy(rt.name, r["name"] | "Routine", sizeof(rt.name));
            rt.assignedTo   = r["assigned"] | 254;   // default: all children
            rt.bundlePoints = r["points"] | 3;
            rt.startHour    = r["startHour"] | 0;
            rt.endHour      = r["endHour"] | 23;
            rt.recurDays    = r["recurDays"] | 127;
            rt.doneMask     = 0;

            JsonArray steps = r["steps"].as<JsonArray>();
            rt.stepCount = 0;
            for (JsonVariant s : steps) {
                if (rt.stepCount >= MAX_ROUTINE_STEPS) break;
                strlcpy(rt.steps[rt.stepCount], s.as<const char*>() ? s.as<const char*>() : "",
                        ROUTINE_STEP_LEN);
                rt.stepCount++;
            }
            count++;
        }
        Serial.printf("[SD] Loaded %d routines.\n", count);
        return true;
    }

    // Persist today's per-routine completion mask (survives reboot).
    bool saveRoutineState(const Routine* routines, uint8_t count, const char* dateStr) {
        JsonDocument doc;
        doc["date"] = dateStr;
        JsonArray arr = doc["done"].to<JsonArray>();
        for (uint8_t i = 0; i < count; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["id"]   = routines[i].id;
            o["mask"] = routines[i].doneMask;
        }
        return _writeJson("/stats/routines.json", doc);
    }

    // Load today's completion mask. Returns the stored date via dateOut so the
    // caller can decide whether it is stale (a new day → reset).
    bool loadRoutineState(Routine* routines, uint8_t count,
                          char* dateOut, size_t dateLen) {
        if (dateLen) dateOut[0] = '\0';
        File f = SD.open("/stats/routines.json", FILE_READ);
        if (!f) return false;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return false;

        strlcpy(dateOut, doc["date"] | "", dateLen);
        for (JsonObject o : doc["done"].as<JsonArray>()) {
            uint8_t id = o["id"] | 0;
            uint8_t mask = o["mask"] | 0;
            for (uint8_t i = 0; i < count; i++) {
                if (routines[i].id == id) { routines[i].doneMask = mask; break; }
            }
        }
        return true;
    }

    // ─── Completion Log ──────────────────────────────────────────────────

    bool appendLog(const CompletionLog& log, const char* dateStr) {
        char path[40];
        snprintf(path, sizeof(path), "/logs/%s.json", dateStr);

        // Read existing log or start fresh
        JsonDocument doc;
        File f = SD.open(path, FILE_READ);
        if (f) {
            deserializeJson(doc, f);
            f.close();
        }

        JsonArray arr;
        if (doc["entries"].is<JsonArray>()) {
            arr = doc["entries"].as<JsonArray>();
        } else {
            arr = doc["entries"].to<JsonArray>();
        }

        JsonObject entry = arr.add<JsonObject>();
        entry["task"] = log.taskId;
        entry["member"] = log.memberId;
        entry["base"] = log.basePoints;
        entry["bonus"] = log.bonusPoints;
        entry["epoch"] = log.completedEpoch;
        entry["initiative"] = log.wasInitiative;
        entry["childClaimed"] = log.childClaimedNoReminder;
        entry["noRemApproved"] = log.noReminderApproved;
        entry["parentOverride"] = log.parentOverrode;

        return _writeJson(path, doc);
    }

    // ─── First-Run Setup Detection ───────────────────────────────────────

    bool isFirstRun() {
        return !SD.exists("/config/family.json");
    }

private:
    void _ensureDir(const char* path) {
        if (!SD.exists(path)) {
            SD.mkdir(path);
        }
    }

    bool _writeJson(const char* path, JsonDocument& doc) {
        File f = SD.open(path, FILE_WRITE);
        if (!f) {
            Serial.printf("[SD] Failed to open %s for writing\n", path);
            return false;
        }
        serializeJsonPretty(doc, f);
        f.close();
        return true;
    }

    uint8_t _parseHexUid(const String& hex, uint8_t* out) {
        uint8_t len = 0;
        int start = 0;
        while (start < (int)hex.length() && len < UID_LENGTH) {
            int colonPos = hex.indexOf(':', start);
            String byteStr;
            if (colonPos == -1) {
                byteStr = hex.substring(start);
                start = hex.length();
            } else {
                byteStr = hex.substring(start, colonPos);
                start = colonPos + 1;
            }
            out[len++] = (uint8_t)strtoul(byteStr.c_str(), NULL, 16);
        }
        return len;
    }

    void _uidToHex(const uint8_t* uid, uint8_t len, char* out) {
        out[0] = '\0';
        for (uint8_t i = 0; i < len; i++) {
            char seg[4];
            if (i > 0) strcat(out, ":");
            snprintf(seg, sizeof(seg), "%02X", uid[i]);
            strcat(out, seg);
        }
    }
};
