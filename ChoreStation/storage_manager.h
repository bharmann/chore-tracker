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
        JsonDocument doc;
        if (!_loadJson("/config/family.json", doc)) {
            Serial.println("[SD] No readable family.json — first run?");
            count = 0;
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
            fm.rfidUidLength = parseHexUid(m["uid"] | "", fm.rfidUid);

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
        JsonDocument doc;
        if (!_loadJson(path, doc)) {
            count = 0;
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
            task.startedEpoch = t["startEpoch"] | 0;
            task.expiresDaily = t["expires"] | false;
            task.doneEarlierDay = t["doneEarlier"] | false;
            task.claimedFromAnyone = t["fromAnyone"] | false;
            // Until NTP lets restoreTaskTimers() rebuild it from startedEpoch,
            // an in-progress task counts from boot rather than from 0.
            task.startedAtMs = (task.state == TaskState::IN_PROGRESS) ? millis() : 0;
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
            obj["startEpoch"] = t.startedEpoch;
            obj["expires"] = t.expiresDaily;
            obj["doneEarlier"] = t.doneEarlierDay;
            obj["fromAnyone"] = t.claimedFromAnyone;
        }

        return _writeJson(path, doc);
    }

    // ─── Routines ────────────────────────────────────────────────────────

    bool loadRoutines(Routine* routines, uint8_t& count) {
        JsonDocument doc;
        if (!_loadJson("/config/routines.json", doc)) { count = 0; return false; }

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
            memset(rt.stepMask, 0, sizeof(rt.stepMask));

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
    // Date of the last daily reset ("YYYY-MM-DD"), so a station that was off
    // over midnight still runs the reset once when it comes back.
    bool loadDayStamp(char* out, size_t len) {
        if (len) out[0] = '\0';
        File f = SD.open("/stats/day.json", FILE_READ);
        if (!f) return false;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return false;
        strlcpy(out, doc["lastReset"] | "", len);
        return out[0] != '\0';
    }
    bool saveDayStamp(const char* date) {
        JsonDocument doc;
        doc["lastReset"] = date;
        return _writeJson("/stats/day.json", doc);
    }

    bool saveRoutineState(const Routine* routines, uint8_t count, const char* dateStr) {
        JsonDocument doc;
        doc["date"] = dateStr;
        JsonArray arr = doc["done"].to<JsonArray>();
        for (uint8_t i = 0; i < count; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["id"]   = routines[i].id;
            o["mask"] = routines[i].doneMask;
            JsonArray st = o["steps"].to<JsonArray>();   // checked steps, per member
            for (uint8_t m = 0; m < MAX_FAMILY_MEMBERS; m++) st.add(routines[i].stepMask[m]);
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
                if (routines[i].id == id) {
                    routines[i].doneMask = mask;
                    JsonArray st = o["steps"].as<JsonArray>();
                    uint8_t m = 0;
                    for (JsonVariant v : st) {
                        if (m >= MAX_FAMILY_MEMBERS) break;
                        routines[i].stepMask[m++] = v.as<uint16_t>();
                    }
                    break;
                }
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
        if (log.durationSec) entry["secs"] = log.durationSec;

        return _writeJson(path, doc);
    }

    // ─── Raw JSON write (used by the web UI) ─────────────────────────────
    // Validates that the body parses as JSON and has the expected top-level
    // array (so a stray {} can't wipe the file), keeps the previous version as
    // <path>.bak (loads fall back to it), then overwrites.
    bool writeRawJson(const char* path, const String& body, const char* arrayKey) {
        JsonDocument doc;
        if (deserializeJson(doc, body)) {
            Serial.printf("[SD] writeRawJson: invalid JSON for %s\n", path);
            return false;
        }
        if (!doc[arrayKey].is<JsonArray>()) {
            Serial.printf("[SD] writeRawJson: %s needs a \"%s\" array\n", path, arrayKey);
            return false;
        }
        copyFile(path, _bakPath(path).c_str());
        if (SD.exists(path)) SD.remove(path);
        File f = SD.open(path, FILE_WRITE);
        if (!f) {
            Serial.printf("[SD] writeRawJson: cannot open %s\n", path);
            return false;
        }
        size_t n = f.print(body);
        f.close();
        return n == body.length();
    }

    bool fileExists(const char* path) { return SD.exists(path); }

    // ─── First-Run Setup Detection ───────────────────────────────────────

    bool isFirstRun() {
        return !SD.exists("/config/family.json") && !SD.exists("/config/family.json.bak");
    }

    // Parse "04:A2:3B:C1" (also tolerates spaces/dashes or no separators).
    // Returns the byte count; 0 for an empty or malformed string.
    static uint8_t parseHexUid(const char* hex, uint8_t* out) {
        uint8_t len = 0;
        int nibble = -1;
        for (const char* p = hex; *p; p++) {
            char c = *p;
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else if (c == ':' || c == ' ' || c == '-') {
                if (nibble >= 0) {               // single-digit byte like "4:"
                    if (len >= UID_LENGTH) return 0;
                    out[len++] = (uint8_t)nibble; nibble = -1;
                }
                continue;
            }
            else return 0;
            if (nibble < 0) nibble = v;
            else {
                if (len >= UID_LENGTH) return 0;
                out[len++] = (uint8_t)((nibble << 4) | v);
                nibble = -1;
            }
        }
        if (nibble >= 0) {
            if (len >= UID_LENGTH) return 0;
            out[len++] = (uint8_t)nibble;
        }
        return len;
    }

    // ─── WiFi credentials (on-device WiFi setup) ─────────────────────────
    // Updates ssid/password in /config/wifi.json, keeping every other key
    // (timezone, webPin). Previous version kept as wifi.json.bak.
    bool saveWifiCredentials(const char* ssid, const char* password) {
        JsonDocument doc;
        if (!_loadJson("/config/wifi.json", doc) || !doc.is<JsonObject>()) {
            doc.clear();
            doc["gmtOffset"] = -28800;   // README default (Pacific)
            doc["dstOffset"] = 3600;
        }
        doc["ssid"] = ssid;
        doc["password"] = password;
        copyFile("/config/wifi.json", "/config/wifi.json.bak");
        return _writeJson("/config/wifi.json", doc);
    }

    // ─── Touch calibration (5 values from TFT_eSPI::calibrateTouch) ──────

    bool loadTouchCal(uint16_t* cal) {
        JsonDocument doc;
        if (!_loadJson("/config/touch.json", doc)) return false;
        JsonArray a = doc["cal"].as<JsonArray>();
        if (a.size() != 5) return false;
        for (uint8_t i = 0; i < 5; i++) cal[i] = a[i] | 0;
        return true;
    }

    bool saveTouchCal(const uint16_t* cal) {
        JsonDocument doc;
        JsonArray a = doc["cal"].to<JsonArray>();
        for (uint8_t i = 0; i < 5; i++) a.add(cal[i]);
        return _writeJson("/config/touch.json", doc);
    }

    bool copyFile(const char* src, const char* dst) {
        File in = SD.open(src, FILE_READ);
        if (!in) return false;
        File out = SD.open(dst, FILE_WRITE);
        if (!out) { in.close(); return false; }
        uint8_t buf[512];
        while (in.available()) {
            size_t n = in.read(buf, sizeof(buf));
            out.write(buf, n);
        }
        out.close(); in.close();
        return true;
    }

private:
    void _ensureDir(const char* path) {
        if (!SD.exists(path)) {
            SD.mkdir(path);
        }
    }

    static String _bakPath(const char* path) { return String(path) + ".bak"; }

    // Parse a JSON file, falling back to the <path>.bak shadow copy that
    // BackupManager / writeRawJson keep, e.g. after a power cut mid-write.
    bool _loadJson(const char* path, JsonDocument& doc) {
        File f = SD.open(path, FILE_READ);
        if (f) {
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (!err) return true;
            Serial.printf("[SD] %s parse error: %s — trying .bak\n", path, err.c_str());
        }
        String bak = _bakPath(path);
        f = SD.open(bak.c_str(), FILE_READ);
        if (!f) return false;
        doc.clear();
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return false;
        Serial.printf("[SD] Recovered %s from %s\n", path, bak.c_str());
        return true;
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

    void _uidToHex(const uint8_t* uid, uint8_t len, char* out) {
        out[0] = '\0';
        for (uint8_t i = 0; i < len && i < UID_LENGTH; i++) {
            char seg[4];
            if (i > 0) strcat(out, ":");
            snprintf(seg, sizeof(seg), "%02X", uid[i]);
            strcat(out, seg);
        }
    }
};
