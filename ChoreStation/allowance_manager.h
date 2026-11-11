// =============================================================================
// allowance_manager.h — Weekly allowance earned by doing the required chores
// =============================================================================
// A parent picks which routines / chores are REQUIRED (web editor, Allowance
// tab). A child's day counts when every required item that applies to them
// that day is done. A week (Sunday..Saturday) with at least `need` good days
// (default 7 = every day) pays `cents` (default $5.00), credited when the next
// week starts. Parents can fix a missed item on a day of the current week,
// override a finished week either way (paid <-> missed), set each child's
// starting savings, and record money spent or added. Balance = starting
// savings + every ledger entry.
//
// What "done" means for a day comes from two places, merged:
//   * live: finishing a routine / chore records it for today (noteDone),
//     a parent's Redo takes it back (noteUndone);
//   * snapshot: just before the midnight reset, Scheduler calls closeDay()
//     with yesterday's date and everything finished that day is recorded
//     (catches any path that didn't call noteDone).
// Plus a parent's manual marks. Today's status shown on screen also counts
// what's finished right now.
//
// Keys: a chore is its template id; a routine is 60000 + its id (the same
// convention as the completion log).
//
// Everything lives in /stats/allowance.json (previous version kept as .bak):
// {
//   "cents": 500, "need": 7, "req": [60002, 60003, 1103],
//   "week": 20407,                      // day number of the current week's Sunday
//   "kids": [ { "id": 2, "start": 2500, "carry": 0,
//               "days": [[60002,1103], [], ...7],         // done keys per day
//               "log": [ {"k":"w","day":20400,"ok":127,"paid":true,"amt":500,"a":500},
//                        {"k":"m","day":20405,"a":-300,"n":"Toy"} ] } ]
// }
// Log kinds: "w" = a finished week (ok = bitmask of good days, ovr = a parent
// changed it), "m" = money in/out recorded by a parent (a = +/- cents).
// Needs the clock (NTP): without the date nothing is recorded or paid.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "data_model.h"
#include "allowance_logic.h"
#include "storage_manager.h"
#include "time_manager.h"
#include "routine_manager.h"
#include "scheduler.h"

extern StorageManager storage;
extern TimeManager    timeMan;
extern RoutineManager routines;
extern Scheduler      scheduler;
extern FamilyMember   family[];
extern uint8_t        familyCount;
extern Task           activeTasks[];
extern uint8_t        activeTaskCount;

class AllowanceManager {
public:
    static constexpr const char* PATH = "/stats/allowance.json";
    static const uint16_t ROUTINE_KEY = 60000;
    static const uint16_t MAX_LOG = 150;          // older entries fold into "carry"
    static const uint8_t  MAX_REQ = 12;

    // Day states for the week strip
    enum DayState : uint8_t { DAY_FUTURE, DAY_OK, DAY_MISSED, DAY_TODAY, DAY_NOT_TRACKED };

    void begin() {
        _doc.clear();
        if (!storage.loadJsonFile(PATH, _doc) || !_doc.is<JsonObject>()) {
            _doc.clear();
            _doc["cents"] = 500;
            _doc["need"]  = 7;
            _doc["req"].to<JsonArray>();
            _pickDefaultRequired();
            _dirty = true;
            Serial.println("[ALLOW] New allowance file.");
        }
        if (!_doc["kids"].is<JsonArray>()) _doc["kids"].to<JsonArray>();
        if (!_doc["req"].is<JsonArray>())  _doc["req"].to<JsonArray>();
        _today = -1;
        _lastUpdateMs = 0;
        update();
        Serial.printf("[ALLOW] Ready: %u required item(s), $%ld.%02ld/week, %d good days needed.\n",
                      (unsigned)_doc["req"].size(), (long)(cents() / 100), (long)(cents() % 100), need());
    }

    // Call every loop (cheap): learns today's date, starts tracking on the
    // first Sunday, pays/closes finished weeks.
    bool update() {
        if (_lastUpdateMs && millis() - _lastUpdateMs < 5000) return false;
        _lastUpdateMs = millis();
        int32_t today;
        if (!_todayFromClock(today)) { _flush(); return false; }
        _today = today;
        if (!(_doc["week"].is<int32_t>())) {
            _doc["week"] = alFirstWeek(today);
            _dirty = true;
            char d[12]; alFormatDate(alFirstWeek(today), d, sizeof(d));
            Serial.printf("[ALLOW] Tracking starts Sunday %s.\n", d);
        }
        bool rolled = _rollTo(today);
        _flush();
        return rolled;
    }

    // ─── Recording ───────────────────────────────────────────────────────
    void noteDone(uint8_t m, uint16_t key) {
        int8_t i = _todayIdx();
        if (i < 0 || !_isChild(m)) return;
        if (_addKey(_kid(m)["days"][i].as<JsonArray>(), key)) { _dirty = true; _flush(); }
    }

    void noteUndone(uint8_t m, uint16_t key) {
        int8_t i = _todayIdx();
        if (i < 0 || !_isChild(m)) return;
        if (_removeKey(_kid(m)["days"][i].as<JsonArray>(), key)) { _dirty = true; _flush(); }
    }

    // Scheduler: the day `date` is over (called before its progress is wiped).
    void closeDay(const char* date) {
        int32_t day;
        if (!alParseDate(date, day) || !_doc["week"].is<int32_t>()) return;
        if (day >= week() + 7) _rollTo(day);       // station was off across a week end
        if (day < week() || day >= week() + 7) { _flush(); return; }
        uint8_t idx = (uint8_t)(day - week());
        for (uint8_t m = 0; m < familyCount; m++) {
            if (!_isChild(m)) continue;
            uint16_t keys[MAX_ROUTINES + MAX_ACTIVE_TASKS];
            uint8_t n = _derivedDone(m, keys, sizeof(keys) / sizeof(keys[0]));
            JsonArray d = _kid(m)["days"][idx].as<JsonArray>();
            for (uint8_t k = 0; k < n; k++) _dirty |= _addKey(d, keys[k]);
        }
        Serial.printf("[ALLOW] Day %s recorded.\n", date);
        _flush();
    }

    // ─── Queries (station screen) ────────────────────────────────────────
    int32_t cents() const { return _doc["cents"] | 500; }
    uint8_t need() const { return _doc["need"] | 7; }
    int32_t week() const { return _doc["week"] | (int32_t)0; }
    int32_t today() const { return _today; }
    bool    knowsDate() const { return _today >= 0; }
    bool    started() const { return knowsDate() && _doc["week"].is<int32_t>() && _today >= week(); }
    uint8_t requiredCount() const { return _doc["req"].size(); }

    int32_t balance(uint8_t m) {
        JsonObject k = _kid(m);
        int32_t b = (k["start"] | (int32_t)0) + (k["carry"] | (int32_t)0);
        for (JsonObject e : k["log"].as<JsonArray>()) b += e["a"] | (int32_t)0;
        return b;
    }

    DayState dayState(uint8_t m, uint8_t i) {
        if (!started()) return DAY_NOT_TRACKED;
        int32_t day = week() + i;
        if (day > _today) return DAY_FUTURE;
        bool ok = dayOk(m, i);
        if (day == _today) return ok ? DAY_OK : DAY_TODAY;
        return ok ? DAY_OK : DAY_MISSED;
    }

    uint8_t goodDays(uint8_t m) {
        uint8_t n = 0;
        for (uint8_t i = 0; i < 7; i++) if (dayState(m, i) == DAY_OK) n++;
        return n;
    }

    // Days left this week (today included when it isn't finished yet)
    uint8_t daysLeft(uint8_t m) {
        uint8_t n = 0;
        for (uint8_t i = 0; i < 7; i++) {
            DayState s = dayState(m, i);
            if (s == DAY_FUTURE || s == DAY_TODAY) n++;
        }
        return n;
    }

    // Today's required items for this child: keys + whether done. Returns count.
    uint8_t todayItems(uint8_t m, uint16_t* keys, bool* done, uint8_t max) {
        int8_t i = _todayIdx();
        if (i < 0) return 0;
        uint8_t n = 0;
        for (JsonVariant v : _doc["req"].as<JsonArray>()) {
            uint16_t key = v.as<uint16_t>();
            if (n >= max || !applies(key, m, _today)) continue;
            keys[n] = key;
            done[n] = satisfied(key, m, (uint8_t)i);
            n++;
        }
        return n;
    }

    // Name of a required item (routine or chore), "" if it no longer exists.
    const char* itemName(uint16_t key) const {
        if (key >= ROUTINE_KEY) {
            int8_t r = _routineIdx(key - ROUTINE_KEY);
            return r >= 0 ? routines.get(r).name : "";
        }
        const Task* t = scheduler.findTemplate(key);
        return t ? t->name : "";
    }

    // Ledger, newest first: returns false past the end.
    bool logEntry(uint8_t m, uint16_t newestFirst, JsonObject& out) {
        JsonArray log = _kid(m)["log"];
        if (newestFirst >= log.size()) return false;
        out = log[log.size() - 1 - newestFirst];
        return true;
    }

    // ─── Rules ───────────────────────────────────────────────────────────
    bool applies(uint16_t key, uint8_t m, int32_t day) const {
        if (key >= ROUTINE_KEY) {
            int8_t r = _routineIdx(key - ROUTINE_KEY);
            return r >= 0 && alRunsOn(routines.get(r).recurDays, day) && routines.visibleFor(r, m);
        }
        const Task* t = scheduler.findTemplate(key);
        if (!t || !alRunsOn(t->recurDays, day)) return false;
        if (t->assignedTo == 254 || t->assignedTo == 255) return _isChild(m);
        return t->assignedTo == m;
    }

    // Done on day i of the current week. A "first taker" chore counts for
    // every child once anyone has done it.
    bool satisfied(uint16_t key, uint8_t m, uint8_t i) {
        if (_doneBy(key, m, i)) return true;
        if (key < ROUTINE_KEY) {
            const Task* t = scheduler.findTemplate(key);
            if (t && t->assignedTo == 255)
                for (uint8_t o = 0; o < familyCount; o++)
                    if (o != m && _isChild(o) && _doneBy(key, o, i)) return true;
        }
        return false;
    }

    bool dayOk(uint8_t m, uint8_t i) {
        if (requiredCount() == 0) return false;          // not set up: nothing earns
        int32_t day = week() + i;
        for (JsonVariant v : _doc["req"].as<JsonArray>()) {
            uint16_t key = v.as<uint16_t>();
            if (applies(key, m, day) && !satisfied(key, m, i)) return false;
        }
        return true;
    }

    // ─── Web editor ──────────────────────────────────────────────────────
    void toJson(JsonDocument& out) {
        update();
        out["cents"] = cents();
        out["need"] = need();
        out["synced"] = knowsDate();
        out["started"] = started();
        char d[12];
        if (_doc["week"].is<int32_t>()) { alFormatDate(week(), d, sizeof(d)); out["week"] = d; }
        if (knowsDate()) { alFormatDate(_today, d, sizeof(d)); out["today"] = d; }
        out["todayIdx"] = _todayIdx();

        // Everything that could be required, and whether it is
        JsonArray opts = out["options"].to<JsonArray>();
        for (uint8_t r = 0; r < routines.count(); r++) {
            JsonObject o = opts.add<JsonObject>();
            o["key"] = ROUTINE_KEY + routines.get(r).id;
            o["name"] = routines.get(r).name;
            o["kind"] = "Routine";
            o["days"] = routines.get(r).recurDays;
            o["required"] = _hasKey(_doc["req"].as<JsonArrayConst>(), ROUTINE_KEY + routines.get(r).id);
        }
        for (uint8_t t = 0; t < scheduler.templateCount(); t++) {
            const Task& tm = scheduler.templateAt(t);
            JsonObject o = opts.add<JsonObject>();
            o["key"] = tm.id;
            o["name"] = tm.name;
            o["kind"] = "Chore";
            o["days"] = tm.recurDays;
            o["required"] = _hasKey(_doc["req"].as<JsonArrayConst>(), tm.id);
        }

        JsonArray kids = out["kids"].to<JsonArray>();
        for (uint8_t m = 0; m < familyCount; m++) {
            if (!_isChild(m)) continue;
            JsonObject k = kids.add<JsonObject>();
            JsonObject src = _kid(m);
            k["id"] = m;
            k["name"] = family[m].name;
            k["balance"] = balance(m);
            k["start"] = src["start"] | (int32_t)0;
            k["good"] = goodDays(m);
            JsonArray days = k["days"].to<JsonArray>();
            for (uint8_t i = 0; i < 7; i++) {
                JsonObject dy = days.add<JsonObject>();
                int32_t day = week() + i;
                alFormatDate(day, d, sizeof(d));
                dy["date"] = d;
                dy["state"] = (uint8_t)dayState(m, i);
                JsonArray items = dy["items"].to<JsonArray>();
                for (JsonVariant v : _doc["req"].as<JsonArray>()) {
                    uint16_t key = v.as<uint16_t>();
                    if (!applies(key, m, day)) continue;
                    JsonObject it = items.add<JsonObject>();
                    it["key"] = key;
                    it["done"] = satisfied(key, m, i);
                }
            }
            JsonArray log = k["log"].to<JsonArray>();
            JsonArray srcLog = src["log"];
            for (size_t e = 0; e < srcLog.size(); e++) {
                JsonObject le = log.add<JsonObject>();
                le.set(srcLog[e].as<JsonObject>());
                le["idx"] = e;
                alFormatDate(srcLog[e]["day"] | (int32_t)0, d, sizeof(d));
                le["date"] = d;
            }
        }
    }

    // Apply one parent action from the web editor. Returns nullptr on
    // success, otherwise a short reason.
    const char* apply(JsonDocument& in) {
        const char* op = in["op"] | "";
        if (!strcmp(op, "settings")) {
            int32_t c = in["cents"] | cents();
            int n = in["need"] | (int)need();
            if (c < 0 || c > 100000) return "allowance must be $0 - $1000";
            if (n < 1 || n > 7) return "days needed must be 1 - 7";
            _doc["cents"] = c;
            _doc["need"] = n;
            if (in["req"].is<JsonArray>()) {
                JsonArray req = _doc["req"].to<JsonArray>();
                for (JsonVariant v : in["req"].as<JsonArray>())
                    if (req.size() < MAX_REQ) req.add(v.as<uint16_t>());
            }
            return _save();
        }

        int m = in["member"] | -1;
        if (m < 0 || m >= familyCount || !_isChild((uint8_t)m)) return "pick a child";
        JsonObject kid = _kid((uint8_t)m);

        if (!strcmp(op, "start")) {
            kid["start"] = in["cents"] | (int32_t)0;
            return _save();
        }
        if (!strcmp(op, "mark")) {
            int i = in["day"] | -1;
            uint16_t key = in["key"] | 0;
            if (!started()) return "tracking hasn't started yet";
            if (i < 0 || i > 6 || week() + i > _today) return "only days of this week up to today";
            if (in["done"] | false) _addKey(kid["days"][i].as<JsonArray>(), key);
            else                    _removeKey(kid["days"][i].as<JsonArray>(), key);
            return _save();
        }
        if (!strcmp(op, "override")) {
            JsonArray log = kid["log"];
            int idx = in["idx"] | -1;
            if (idx < 0 || idx >= (int)log.size() || strcmp(log[idx]["k"] | "", "w")) return "no such week";
            JsonObject e = log[idx];
            bool paid = in["paid"] | false;
            e["paid"] = paid;
            e["a"] = paid ? (e["amt"] | cents()) : 0;
            e["ovr"] = true;
            return _save();
        }
        if (!strcmp(op, "money")) {
            int32_t c = in["cents"] | (int32_t)0;
            if (c == 0 || c > 1000000 || c < -1000000) return "enter an amount";
            JsonObject e = _addLog(kid);
            e["k"] = "m";
            e["day"] = knowsDate() ? _today : (int32_t)0;
            e["a"] = c;
            const char* note = in["note"] | "";
            char n[40]; strlcpy(n, note, sizeof(n));
            e["n"] = n;
            return _save();
        }
        if (!strcmp(op, "delete")) {
            JsonArray log = kid["log"];
            int idx = in["idx"] | -1;
            if (idx < 0 || idx >= (int)log.size() || strcmp(log[idx]["k"] | "", "m")) return "only money entries can be deleted";
            log.remove(idx);
            return _save();
        }
        return "unknown action";
    }

private:
    JsonDocument _doc;
    int32_t  _today = -1;
    uint32_t _lastUpdateMs = 0;
    bool     _dirty = false;

    static bool _isChild(uint8_t m) { return m < familyCount && family[m].role == Role::CHILD; }

    bool _todayFromClock(int32_t& day) const {
        if (!timeMan.isTimeSynced()) return false;
        char d[11];
        timeMan.currentDateStr(d, sizeof(d));
        return alParseDate(d, day);
    }

    int8_t _todayIdx() const {
        if (!started() || _today >= week() + 7) return -1;
        return (int8_t)(_today - week());
    }

    static int8_t _routineIdx(uint16_t id) {
        for (uint8_t r = 0; r < routines.count(); r++)
            if (routines.get(r).id == id) return r;
        return -1;
    }

    JsonObject _kid(uint8_t m) {
        JsonArray kids = _doc["kids"];
        for (JsonObject k : kids) if ((k["id"] | -1) == m) return k;
        JsonObject k = kids.add<JsonObject>();
        k["id"] = m;
        k["start"] = 0;
        k["carry"] = 0;
        JsonArray days = k["days"].to<JsonArray>();
        for (uint8_t i = 0; i < 7; i++) days.add<JsonArray>();
        k["log"].to<JsonArray>();
        _dirty = true;
        return k;
    }

    static bool _hasKey(JsonArrayConst a, uint16_t key) {
        for (JsonVariantConst v : a) if (v.as<uint16_t>() == key) return true;
        return false;
    }
    static bool _addKey(JsonArray a, uint16_t key) {
        if (_hasKey(a, key)) return false;
        a.add(key);
        return true;
    }
    static bool _removeKey(JsonArray a, uint16_t key) {
        for (size_t i = 0; i < a.size(); i++)
            if (a[i].as<uint16_t>() == key) { a.remove(i); return true; }
        return false;
    }

    // Recorded for day i, or (for today) finished right now.
    bool _doneBy(uint16_t key, uint8_t m, uint8_t i) {
        if (_hasKey(_kid(m)["days"][i].as<JsonArrayConst>(), key)) return true;
        if ((int8_t)i != _todayIdx()) return false;
        uint16_t keys[MAX_ROUTINES + MAX_ACTIVE_TASKS];
        uint8_t n = _derivedDone(m, keys, sizeof(keys) / sizeof(keys[0]));
        for (uint8_t k = 0; k < n; k++) if (keys[k] == key) return true;
        return false;
    }

    // What this child has finished today, from the live task list + routine masks.
    uint8_t _derivedDone(uint8_t m, uint16_t* keys, uint8_t max) const {
        uint8_t n = 0;
        for (uint8_t r = 0; r < routines.count() && n < max; r++)
            if (routines.isDoneToday(r, m)) keys[n++] = ROUTINE_KEY + routines.get(r).id;
        for (uint8_t i = 0; i < activeTaskCount && n < max; i++) {
            const Task& t = activeTasks[i];
            if (t.type != TaskType::RECURRING || t.assignedTo != m || t.doneEarlierDay) continue;
            if (t.state == TaskState::COMPLETED || t.state == TaskState::PENDING_REVIEW) keys[n++] = t.id;
        }
        return n;
    }

    JsonObject _addLog(JsonObject kid) {
        JsonArray log = kid["log"];
        while (log.size() >= MAX_LOG) {
            kid["carry"] = (kid["carry"] | (int32_t)0) + (log[0]["a"] | (int32_t)0);
            log.remove(0);
        }
        return log.add<JsonObject>();
    }

    // Close (and pay) every week that ended before `day`.
    bool _rollTo(int32_t day) {
        bool rolled = false;
        while (_doc["week"].is<int32_t>() && day >= week() + 7) {
            _finishWeek();
            _doc["week"] = week() + 7;
            rolled = true;
        }
        return rolled;
    }

    void _finishWeek() {
        char d[12]; alFormatDate(week(), d, sizeof(d));
        for (uint8_t m = 0; m < familyCount; m++) {
            if (!_isChild(m)) continue;
            uint8_t mask = 0;
            for (uint8_t i = 0; i < 7; i++) if (dayOk(m, i)) mask |= 1 << i;
            bool paid = alBitCount(mask) >= need() && requiredCount() > 0;
            JsonObject kid = _kid(m);
            JsonObject e = _addLog(kid);
            e["k"] = "w";
            e["day"] = week();
            e["ok"] = mask;
            e["paid"] = paid;
            e["amt"] = cents();
            e["a"] = paid ? cents() : 0;
            JsonArray days = kid["days"];
            for (uint8_t i = 0; i < 7; i++) days[i].to<JsonArray>();
            Serial.printf("[ALLOW] Week of %s: %s %d/7 good days -> %s\n", d, family[m].name,
                          alBitCount(mask), paid ? "PAID" : "missed");
        }
        _dirty = true;
    }

    // First run: the chores the family asked for (after school, bedtime,
    // evening teeth). A parent can change the list on the web Allowance tab.
    void _pickDefaultRequired() {
        JsonArray req = _doc["req"];
        for (uint8_t r = 0; r < routines.count(); r++) {
            String n = routines.get(r).name; n.toLowerCase();
            if (n.indexOf("after school") >= 0 || n.indexOf("bedtime") >= 0 || n.indexOf("bed time") >= 0)
                req.add(ROUTINE_KEY + routines.get(r).id);
        }
        for (uint8_t t = 0; t < scheduler.templateCount(); t++) {
            const Task& tm = scheduler.templateAt(t);
            String n = tm.name; n.toLowerCase();
            if (n.indexOf("brush") >= 0 &&
                (n.indexOf("pm") >= 0 || n.indexOf("evening") >= 0 || n.indexOf("night") >= 0))
                req.add(tm.id);
        }
        for (JsonVariant v : req) Serial.printf("[ALLOW] Required by default: %s\n", itemName(v.as<uint16_t>()));
    }

    const char* _save() {
        _dirty = true;
        return _flush(true) ? nullptr : "SD card write failed";
    }

    bool _flush(bool force = false) {
        if (!_dirty && !force) return false;
        _dirty = false;
        if (!storage.saveJsonFile(PATH, _doc)) { _dirty = true; return false; }
        return true;
    }
};
