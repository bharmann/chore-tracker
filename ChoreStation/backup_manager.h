// =============================================================================
// backup_manager.h — Data Backup, Restore & Write Protection
// =============================================================================
// Three concerns addressed here:
//
// 1. WRITE WEAR REDUCTION
//    SD card flash cells have limited write cycles (~10,000-100,000).
//    The old code wrote to active.json on EVERY task state change.
//    Fix: Coalesce writes using a dirty flag + periodic flush interval.
//    Only write when dirty AND enough time has elapsed since last write.
//
// 2. PERIODIC BACKUP
//    Creates timestamped snapshots of all config/state files into /backups/.
//    Keeps the last N backups and auto-prunes older ones.
//    Runs once daily (or on-demand via parent admin).
//
// 3. CORRUPTION RECOVERY
//    Before writing any file, creates a .bak copy first.
//    On boot, if a primary file fails to parse, falls back to .bak.
//    Validates JSON parse before accepting any loaded file.
//
// 4. LOG ARCHIVING
//    Daily log files in /logs/ accumulate over time. After 30 days,
//    older logs are summarized into monthly aggregates in /archive/
//    and the daily files are deleted to save space.
//
// SD card file structure (updated):
//   /config/
//     family.json          — Active config
//     family.json.bak      — Shadow copy (last known good)
//     settings.json
//     settings.json.bak
//   /tasks/
//     recurring.json
//     recurring.json.bak
//     active.json
//     active.json.bak
//   /logs/
//     2026-04-11.json      — Daily completion logs
//   /stats/
//     stats.json
//     stats.json.bak
//   /backups/
//     2026-04-11/           — Full daily snapshot
//       family.json
//       recurring.json
//       active.json
//       stats.json
//   /archive/
//     2026-03.json          — Monthly aggregate summary
// =============================================================================

#pragma once
#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>
#include "data_model.h"

class BackupManager {
public:
    // ─── Write Coalescing ────────────────────────────────────────────────
    // Instead of writing on every change, mark dirty and flush periodically.

    static const uint32_t MIN_WRITE_INTERVAL_MS = 10000;  // 10 seconds
    static const uint32_t MAX_DIRTY_AGE_MS      = 30000;  // Force flush after 30s dirty
    static const uint8_t  MAX_BACKUPS           = 7;       // Keep last 7 daily backups
    static const uint8_t  LOG_RETENTION_DAYS    = 30;      // Archive logs older than this

    void begin() {
        _tasksDirty = false;
        _familyDirty = false;
        _lastTaskWriteMs = millis();
        _lastFamilyWriteMs = millis();
        _dirtyAtMs = 0;
        _lastBackupDay = 0;

        _ensureDir("/backups");
        _ensureDir("/archive");

        Serial.println("[BACKUP] Manager initialized.");
    }

    // Mark data as needing to be saved (call instead of immediate write)
    void markTasksDirty()  {
        if (!_tasksDirty) {
            _tasksDirty = true;
            _dirtyAtMs = millis();
        }
    }

    void markFamilyDirty() {
        if (!_familyDirty) {
            _familyDirty = true;
            _dirtyAtMs = millis();
        }
    }

    // Call from main loop — checks if it's time to flush dirty data
    // Returns true if a write actually happened
    bool update(
        const Task* tasks, uint8_t taskCount,
        const FamilyMember* family, uint8_t familyCount,
        void (*saveTasksFn)(const Task*, uint8_t, const char*),
        void (*saveFamilyFn)(const FamilyMember*, uint8_t)
    ) {
        bool wrote = false;
        uint32_t now = millis();

        // Tasks
        if (_tasksDirty) {
            bool elapsed = (now - _lastTaskWriteMs) >= MIN_WRITE_INTERVAL_MS;
            bool overdue = (now - _dirtyAtMs) >= MAX_DIRTY_AGE_MS;

            if (elapsed || overdue) {
                // Create shadow backup first
                _shadowCopy("/tasks/active.json");

                saveTasksFn(tasks, taskCount, "/tasks/active.json");
                _tasksDirty = false;
                _lastTaskWriteMs = now;
                wrote = true;
                Serial.println("[BACKUP] Tasks flushed to SD.");
            }
        }

        // Family
        if (_familyDirty) {
            bool elapsed = (now - _lastFamilyWriteMs) >= MIN_WRITE_INTERVAL_MS;
            bool overdue = (now - _dirtyAtMs) >= MAX_DIRTY_AGE_MS;

            if (elapsed || overdue) {
                _shadowCopy("/config/family.json");
                saveFamilyFn(family, familyCount);
                _familyDirty = false;
                _lastFamilyWriteMs = now;
                wrote = true;
                Serial.println("[BACKUP] Family data flushed to SD.");
            }
        }

        return wrote;
    }

    // Force immediate flush of all dirty data (call before power-off or on demand)
    void flushNow(
        const Task* tasks, uint8_t taskCount,
        const FamilyMember* family, uint8_t familyCount,
        void (*saveTasksFn)(const Task*, uint8_t, const char*),
        void (*saveFamilyFn)(const FamilyMember*, uint8_t)
    ) {
        if (_tasksDirty) {
            _shadowCopy("/tasks/active.json");
            saveTasksFn(tasks, taskCount, "/tasks/active.json");
            _tasksDirty = false;
            Serial.println("[BACKUP] Tasks force-flushed.");
        }
        if (_familyDirty) {
            _shadowCopy("/config/family.json");
            saveFamilyFn(family, familyCount);
            _familyDirty = false;
            Serial.println("[BACKUP] Family force-flushed.");
        }
    }

    bool hasDirtyData() const { return _tasksDirty || _familyDirty; }

    // ─── Shadow Copy (write-ahead safety) ────────────────────────────────
    // Copy current file to .bak BEFORE overwriting with new data.
    // If the write fails or power is lost mid-write, .bak is intact.

    bool _shadowCopy(const char* path) {
        char bakPath[64];
        snprintf(bakPath, sizeof(bakPath), "%s.bak", path);

        if (!SD.exists(path)) return true;  // Nothing to back up

        // Delete old backup
        if (SD.exists(bakPath)) SD.remove(bakPath);

        // Copy
        File src = SD.open(path, FILE_READ);
        if (!src) return false;

        File dst = SD.open(bakPath, FILE_WRITE);
        if (!dst) { src.close(); return false; }

        uint8_t buf[512];
        while (src.available()) {
            size_t n = src.read(buf, sizeof(buf));
            dst.write(buf, n);
        }

        dst.close();
        src.close();
        return true;
    }

    // ─── Corruption Recovery ─────────────────────────────────────────────
    // Try loading primary file; if it fails, fall back to .bak

    bool loadWithFallback(const char* path, JsonDocument& doc) {
        // Try primary
        if (_tryLoadJson(path, doc)) {
            return true;
        }

        // Try backup
        char bakPath[64];
        snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
        Serial.printf("[BACKUP] Primary '%s' failed, trying backup...\n", path);

        if (_tryLoadJson(bakPath, doc)) {
            Serial.printf("[BACKUP] Recovered from '%s'!\n", bakPath);

            // Restore primary from backup
            _fileCopy(bakPath, path);
            return true;
        }

        Serial.printf("[BACKUP] Both primary and backup failed for '%s'\n", path);
        return false;
    }

    // ─── Daily Full Backup ───────────────────────────────────────────────
    // Creates a timestamped snapshot of all critical files.
    // Call once per day (e.g., at midnight or on first boot of the day).

    bool createDailyBackup(const char* dateStr) {
        char dir[32];
        snprintf(dir, sizeof(dir), "/backups/%s", dateStr);

        if (SD.exists(dir)) {
            Serial.printf("[BACKUP] Daily backup '%s' already exists.\n", dateStr);
            return true;  // Already done today
        }

        SD.mkdir(dir);

        // Copy critical files
        char dst[64];
        snprintf(dst, sizeof(dst), "%s/family.json", dir);
        _fileCopy("/config/family.json", dst);

        snprintf(dst, sizeof(dst), "%s/recurring.json", dir);
        _fileCopy("/tasks/recurring.json", dst);

        snprintf(dst, sizeof(dst), "%s/active.json", dir);
        _fileCopy("/tasks/active.json", dst);

        snprintf(dst, sizeof(dst), "%s/stats.json", dir);
        _fileCopy("/stats/stats.json", dst);

        Serial.printf("[BACKUP] Daily backup created: %s\n", dir);

        _pruneOldBackups();
        return true;
    }

    // ─── Restore from Backup ─────────────────────────────────────────────
    // Overwrites current config/tasks with a specific dated backup.
    // Returns false if the backup doesn't exist.

    bool restoreFromBackup(const char* dateStr) {
        char dir[32];
        snprintf(dir, sizeof(dir), "/backups/%s", dateStr);

        if (!SD.exists(dir)) {
            Serial.printf("[BACKUP] Backup '%s' not found.\n", dateStr);
            return false;
        }

        char src[64];

        snprintf(src, sizeof(src), "%s/family.json", dir);
        if (SD.exists(src)) _fileCopy(src, "/config/family.json");

        snprintf(src, sizeof(src), "%s/recurring.json", dir);
        if (SD.exists(src)) _fileCopy(src, "/tasks/recurring.json");

        snprintf(src, sizeof(src), "%s/active.json", dir);
        if (SD.exists(src)) _fileCopy(src, "/tasks/active.json");

        snprintf(src, sizeof(src), "%s/stats.json", dir);
        if (SD.exists(src)) _fileCopy(src, "/stats/stats.json");

        Serial.printf("[BACKUP] Restored from backup: %s\n", dateStr);
        return true;
    }

    // ─── List Available Backups ──────────────────────────────────────────
    // Populates an array of date strings. Returns count.

    uint8_t listBackups(char dates[][11], uint8_t maxCount) {
        File dir = SD.open("/backups");
        if (!dir || !dir.isDirectory()) return 0;

        uint8_t count = 0;
        File entry;
        while ((entry = dir.openNextFile()) && count < maxCount) {
            if (entry.isDirectory()) {
                const char* name = entry.name();
                // Extract just the folder name (date part)
                const char* slash = strrchr(name, '/');
                const char* datePart = slash ? slash + 1 : name;
                if (strlen(datePart) == 10) {  // YYYY-MM-DD
                    strlcpy(dates[count], datePart, 11);
                    count++;
                }
            }
            entry.close();
        }
        dir.close();

        return count;
    }

    // ─── Log Archiving ───────────────────────────────────────────────────
    // Summarize daily logs older than retention period into monthly files.
    // Called periodically (e.g., daily).

    bool archiveOldLogs(const char* currentDateStr) {
        // Parse current date to get comparison point
        // currentDateStr format: "YYYY-MM-DD"
        int curYear, curMonth, curDay;
        if (sscanf(currentDateStr, "%d-%d-%d", &curYear, &curMonth, &curDay) != 3) {
            return false;
        }

        File dir = SD.open("/logs");
        if (!dir || !dir.isDirectory()) return false;

        uint8_t archived = 0;
        File entry;
        while ((entry = dir.openNextFile())) {
            if (entry.isDirectory()) { entry.close(); continue; }

            // ESP32 core 2.x+: name() is the bare filename; path() is the
            // absolute path that SD.open()/remove() need.
            String fullPath = entry.path();
            const char* fname = fullPath.c_str();
            const char* slash = strrchr(fname, '/');
            const char* name = slash ? slash + 1 : fname;

            // Parse log filename: YYYY-MM-DD.json
            int logYear, logMonth, logDay;
            if (sscanf(name, "%d-%d-%d.json", &logYear, &logMonth, &logDay) != 3) {
                entry.close();
                continue;
            }

            // Calculate approximate age in days
            int ageDays = (curYear - logYear) * 365 + (curMonth - logMonth) * 30 + (curDay - logDay);

            if (ageDays > LOG_RETENTION_DAYS) {
                // Read the log, add its entries to the monthly archive
                char archivePath[32];
                snprintf(archivePath, sizeof(archivePath), "/archive/%04d-%02d.json",
                         logYear, logMonth);

                _appendToArchive(fname, archivePath);

                // Delete the daily log
                entry.close();
                SD.remove(fname);
                archived++;
                Serial.printf("[ARCHIVE] Archived and removed: %s\n", name);
                continue;
            }

            entry.close();
        }
        dir.close();

        if (archived > 0) {
            Serial.printf("[ARCHIVE] Archived %d old log files.\n", archived);
        }
        return true;
    }

    // ─── SD Card Health Check ────────────────────────────────────────────

    struct SDHealth {
        bool     mounted;
        uint64_t totalBytes;
        uint64_t usedBytes;
        uint8_t  backupCount;
        uint8_t  logFileCount;
    };

    SDHealth checkHealth() {
        SDHealth h;
        h.mounted = (SD.cardType() != CARD_NONE);
        h.totalBytes = SD.totalBytes();
        h.usedBytes = SD.usedBytes();

        // Count backups
        h.backupCount = 0;
        File bdir = SD.open("/backups");
        if (bdir && bdir.isDirectory()) {
            File e;
            while ((e = bdir.openNextFile())) {
                if (e.isDirectory()) h.backupCount++;
                e.close();
            }
            bdir.close();
        }

        // Count log files
        h.logFileCount = 0;
        File ldir = SD.open("/logs");
        if (ldir && ldir.isDirectory()) {
            File e;
            while ((e = ldir.openNextFile())) {
                if (!e.isDirectory()) h.logFileCount++;
                e.close();
            }
            ldir.close();
        }

        return h;
    }

private:
    bool     _tasksDirty;
    bool     _familyDirty;
    uint32_t _lastTaskWriteMs;
    uint32_t _lastFamilyWriteMs;
    uint32_t _dirtyAtMs;
    uint8_t  _lastBackupDay;

    void _ensureDir(const char* path) {
        if (!SD.exists(path)) SD.mkdir(path);
    }

    bool _tryLoadJson(const char* path, JsonDocument& doc) {
        File f = SD.open(path, FILE_READ);
        if (!f) return false;

        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err) {
            Serial.printf("[BACKUP] JSON parse error in '%s': %s\n", path, err.c_str());
            return false;
        }
        return true;
    }

    bool _fileCopy(const char* srcPath, const char* dstPath) {
        File src = SD.open(srcPath, FILE_READ);
        if (!src) return false;

        File dst = SD.open(dstPath, FILE_WRITE);
        if (!dst) { src.close(); return false; }

        uint8_t buf[512];
        while (src.available()) {
            size_t n = src.read(buf, sizeof(buf));
            dst.write(buf, n);
        }

        dst.close();
        src.close();
        return true;
    }

    void _pruneOldBackups() {
        // List all backup directories and remove oldest if over MAX_BACKUPS
        char dates[MAX_BACKUPS + 4][11];
        uint8_t count = listBackups(dates, MAX_BACKUPS + 4);

        if (count <= MAX_BACKUPS) return;

        // Sort dates (they're YYYY-MM-DD format, so lexicographic = chronological)
        for (uint8_t i = 0; i < count - 1; i++) {
            for (uint8_t j = i + 1; j < count; j++) {
                if (strcmp(dates[i], dates[j]) > 0) {
                    char tmp[11];
                    strlcpy(tmp, dates[i], 11);
                    strlcpy(dates[i], dates[j], 11);
                    strlcpy(dates[j], tmp, 11);
                }
            }
        }

        // Remove oldest backups
        while (count > MAX_BACKUPS) {
            char dir[32];
            snprintf(dir, sizeof(dir), "/backups/%s", dates[count - MAX_BACKUPS - 1]);

            // Remove files in the backup directory
            _removeDir(dir);
            Serial.printf("[BACKUP] Pruned old backup: %s\n", dates[count - MAX_BACKUPS - 1]);
            count--;
        }
    }

    void _removeDir(const char* path) {
        File dir = SD.open(path);
        if (!dir || !dir.isDirectory()) return;

        File entry;
        while ((entry = dir.openNextFile())) {
            char fullPath[80];
            snprintf(fullPath, sizeof(fullPath), "%s/%s",
                     path, entry.name());
            entry.close();
            SD.remove(fullPath);
        }
        dir.close();
        SD.rmdir(path);
    }

    void _appendToArchive(const char* logPath, const char* archivePath) {
        // Read daily log
        JsonDocument logDoc;
        File logFile = SD.open(logPath, FILE_READ);
        if (!logFile) return;
        deserializeJson(logDoc, logFile);
        logFile.close();

        // Read or create archive
        JsonDocument archDoc;
        File archFile = SD.open(archivePath, FILE_READ);
        if (archFile) {
            deserializeJson(archDoc, archFile);
            archFile.close();
        }

        // Merge: append log entries into archive's "entries" array
        JsonArray archEntries;
        if (archDoc["entries"].is<JsonArray>()) {
            archEntries = archDoc["entries"].as<JsonArray>();
        } else {
            archEntries = archDoc["entries"].to<JsonArray>();
        }

        if (logDoc["entries"].is<JsonArray>()) {
            for (JsonObject e : logDoc["entries"].as<JsonArray>()) {
                archEntries.add(e);
            }
        }

        // Write archive back
        File out = SD.open(archivePath, FILE_WRITE);
        if (out) {
            serializeJson(archDoc, out);  // Compact for archives (saves space)
            out.close();
        }
    }
};
