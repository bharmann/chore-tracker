// =============================================================================
// web_server.h — Parent Web UI for editing Chores, Routines & Family
// =============================================================================
// The touchscreen is fine for kids scanning and tapping, but painful for a
// parent authoring tasks (preset names only, no keyboard, resistive touch).
// This module runs a small HTTP server on the ESP32 (only reachable once WiFi
// is up) so a parent can open the station's IP in a phone browser and edit
// recurring chores, routines and family members with real forms and a keyboard.
//
// Data contract: the API reads and writes the SAME SD JSON files the firmware
// already uses — /tasks/recurring.json and /config/routines.json — so there is
// one source of truth. On a successful save the module raises a reload flag;
// the main loop reloads the affected module (scheduler / routines) so changes
// take effect without a reboot. Family edits go straight into the in-RAM
// family[] (names, goals, tag UIDs) and are saved immediately.
//
// Endpoints:
//   GET  /               → the single-page editor (HTML/JS, served from PROGMEM)
//   GET  /api/family     → members (id, name, role, dailyGoal, uid, points)
//   POST /api/family     → update names / goals / tag UIDs, or add members
//   GET  /api/lastscan   → the most recent tag that matched nobody
//   GET  /api/routines   → current /config/routines.json
//   POST /api/routines   → replace /config/routines.json (validated)
//   GET  /api/tasks      → current /tasks/recurring.json
//   POST /api/tasks      → replace /tasks/recurring.json (validated)
//
// Auth: optional. If /config/wifi.json has "webPin", every request needs HTTP
// Basic auth (user "parent", password = the PIN). Without it the editor is
// open to anyone on the LAN — fine for a trusted home network, but the kids
// are on that network too. WiFi credentials are never served.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <ArduinoJson.h>
#include "data_model.h"
#include "storage_manager.h"
#include "backup_manager.h"
#include "rfid_manager.h"
#include "ota_update.h"

extern StorageManager storage;
extern BackupManager  backups;
extern FamilyMember   family[];
extern uint8_t        familyCount;
extern uint8_t        lastUnknownUid[];
extern uint8_t        lastUnknownLen;
extern uint32_t       lastUnknownMs;

extern OtaUpdater ota;

class WebUI {
public:
    // Public reload flags — the main loop consumes these after a save so the
    // running firmware picks up edited templates/routines without a reboot.
    bool pendingTaskReload    = false;
    bool pendingRoutineReload = false;
    bool familyChanged        = false;   // main loop redraws name-bearing screens
    bool pendingOtaInstall    = false;   // main loop runs the install (shows progress on the station)

    void begin(const char* pin) {
        strlcpy(_pin, pin ? pin : "", sizeof(_pin));
        _server.on("/", HTTP_GET, [this]() { if (_auth()) _handleRoot(); });
        _server.on("/api/family",   HTTP_GET,  [this]() { if (_auth()) _handleFamily(); });
        _server.on("/api/family",   HTTP_POST, [this]() { if (_auth()) _handleFamilySave(); });
        _server.on("/api/lastscan", HTTP_GET,  [this]() { if (_auth()) _handleLastScan(); });
        _server.on("/api/routines", HTTP_GET,  [this]() { if (_auth()) _streamFile("/config/routines.json", "{\"routines\":[]}"); });
        _server.on("/api/routines", HTTP_POST, [this]() { if (_auth()) _handleSave("/config/routines.json", "routines", pendingRoutineReload); });
        _server.on("/api/tasks",    HTTP_GET,  [this]() { if (_auth()) _streamFile("/tasks/recurring.json", "{\"tasks\":[]}"); });
        _server.on("/api/tasks",    HTTP_POST, [this]() { if (_auth()) _handleSave("/tasks/recurring.json", "tasks", pendingTaskReload); });
        _server.on("/api/firmware",         HTTP_GET,  [this]() { if (_auth()) _handleFirmware(false); });
        _server.on("/api/firmware/check",   HTTP_POST, [this]() { if (_auth()) _handleFirmware(true); });
        _server.on("/api/firmware/install", HTTP_POST, [this]() { if (_auth()) _handleFirmwareInstall(); });
        _server.onNotFound([this]() { _server.send(404, "text/plain", "Not found"); });
        _server.begin();
        _started = true;
        Serial.printf("[WEB] Server started on port 80 (%s).\n", _pin[0] ? "PIN required" : "no PIN");
    }

    // Call every loop; cheap when idle.
    void handle() {
        if (_started) _server.handleClient();
    }

    bool isStarted() const { return _started; }

private:
    WebServer _server{80};
    bool      _started = false;
    char      _pin[33] = "";

    bool _auth() {
        if (!_pin[0] || _server.authenticate("parent", _pin)) return true;
        _server.requestAuthentication(BASIC_AUTH, "Chore Station", "PIN required");
        return false;
    }

    void _sendJson(int code, JsonDocument& doc) {
        String out;
        serializeJson(doc, out);
        _server.send(code, "application/json", out);
    }

    // Firmware: what's running, the newest release, and whether it's newer.
    void _handleFirmware(bool checkNow) {
        if (checkNow) ota.check();
        JsonDocument doc;
        doc["current"] = ota.current();
        doc["latest"] = ota.latest;
        doc["available"] = ota.updateAvailable();
        doc["checked"] = ota.checkedOnce;
        doc["error"] = ota.lastError;
        _sendJson(200, doc);
    }

    // The station does the install itself (it shows progress and restarts).
    void _handleFirmwareInstall() {
        if (!ota.updateAvailable()) { _sendError(400, "No update available - check first"); return; }
        pendingOtaInstall = true;
        JsonDocument doc;
        doc["ok"] = true;
        doc["installing"] = ota.latest;
        _sendJson(200, doc);
    }

    void _sendError(int code, const char* msg) {
        JsonDocument doc;
        doc["error"] = msg;
        _sendJson(code, doc);
    }

    void _streamFile(const char* path, const char* fallback) {
        File f = SD.open(path, FILE_READ);
        if (!f) { _server.send(200, "application/json", fallback); return; }
        _server.streamFile(f, "application/json");
        f.close();
    }

    void _handleSave(const char* path, const char* arrayKey, bool& reloadFlag) {
        if (!_server.hasArg("plain")) { _sendError(400, "empty body"); return; }
        String body = _server.arg("plain");
        if (storage.writeRawJson(path, body, arrayKey)) {
            reloadFlag = true;
            _server.send(200, "application/json", "{\"ok\":true}");
            Serial.printf("[WEB] Saved %s (%u bytes)\n", path, (unsigned)body.length());
        } else {
            _sendError(400, "invalid JSON");
        }
    }

    void _handleFamily() {
        JsonDocument doc;
        JsonArray arr = doc["members"].to<JsonArray>();
        for (uint8_t i = 0; i < familyCount; i++) {
            JsonObject m = arr.add<JsonObject>();
            m["id"]        = family[i].id;
            m["name"]      = family[i].name;
            m["role"]      = (family[i].role == Role::PARENT) ? "parent" : "child";
            m["dailyGoal"] = family[i].dailyGoal;
            m["points"]    = family[i].totalPoints;
            char hex[UID_LENGTH * 3 + 1];
            RFIDManager::uidToHex(family[i].rfidUid, family[i].rfidUidLength, hex, sizeof(hex));
            m["uid"] = hex;
        }
        doc["max"] = MAX_FAMILY_MEMBERS;
        _sendJson(200, doc);
    }

    void _handleLastScan() {
        JsonDocument doc;
        char hex[UID_LENGTH * 3 + 1] = "";
        if (lastUnknownLen) RFIDManager::uidToHex(lastUnknownUid, lastUnknownLen, hex, sizeof(hex));
        doc["uid"] = hex;
        doc["ageSec"] = lastUnknownLen ? (millis() - lastUnknownMs) / 1000 : 0;
        _sendJson(200, doc);
    }

    // Edit names / goals / tags of existing members, or append new ones.
    // Members can't be deleted here: ids are array positions and every task
    // assignment refers to them.
    void _handleFamilySave() {
        if (!_server.hasArg("plain")) { _sendError(400, "empty body"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, _server.arg("plain")) || !doc["members"].is<JsonArray>()) {
            _sendError(400, "invalid JSON"); return;
        }

        // Validate everything into a copy first; apply only if all of it is good.
        FamilyMember next[MAX_FAMILY_MEMBERS];
        memcpy(next, family, sizeof(next));
        uint8_t nextCount = familyCount;

        for (JsonObject m : doc["members"].as<JsonArray>()) {
            int id = m["id"] | -1;
            FamilyMember* fm;
            if (id >= 0 && id < familyCount) {
                fm = &next[id];
            } else {
                if (nextCount >= MAX_FAMILY_MEMBERS) { _sendError(400, "family is full"); return; }
                fm = &next[nextCount];
                memset(fm, 0, sizeof(FamilyMember));
                fm->id = nextCount;
                fm->role = (strcmp(m["role"] | "child", "parent") == 0) ? Role::PARENT : Role::CHILD;
                fm->dailyGoal = (fm->role == Role::CHILD) ? 5 : 3;
                nextCount++;
            }
            const char* name = m["name"] | "";
            if (name[0]) strlcpy(fm->name, name, NAME_LENGTH);
            if (!fm->name[0]) { _sendError(400, "every member needs a name"); return; }
            int goal = m["dailyGoal"] | (int)fm->dailyGoal;
            fm->dailyGoal = constrain(goal, 1, 20);
            const char* uid = m["uid"] | "";
            uint8_t parsed[UID_LENGTH];
            uint8_t len = StorageManager::parseHexUid(uid, parsed);
            if (uid[0] && len == 0) { _sendError(400, "tag must look like 04:A2:3B:C1"); return; }
            memcpy(fm->rfidUid, parsed, len);
            fm->rfidUidLength = len;
        }

        for (uint8_t i = 0; i < nextCount; i++)
            for (uint8_t j = i + 1; j < nextCount; j++)
                if (next[i].rfidUidLength && RFIDManager::matchUid(next[i].rfidUid, next[i].rfidUidLength, next[j])) {
                    _sendError(400, "the same tag is assigned twice"); return;
                }

        memcpy(family, next, sizeof(next));
        familyCount = nextCount;
        backups._shadowCopy("/config/family.json");
        if (!storage.saveFamily(family, familyCount)) { _sendError(500, "SD write failed"); return; }
        familyChanged = true;
        _server.send(200, "application/json", "{\"ok\":true}");
        Serial.printf("[WEB] Family saved (%u members)\n", familyCount);
    }

    void _handleRoot() {
        _server.send_P(200, "text/html", PAGE_HTML);
    }

    static const char PAGE_HTML[] PROGMEM;
};

// =============================================================================
// Single-page editor. Vanilla JS, no external assets. Loads family + both
// files, renders friendly forms, and POSTs each whole file back on save.
// =============================================================================
const char WebUI::PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Chore Station</title>
<style>
:root{--bg:#12161c;--card:#1e2530;--pri:#2f9bd6;--ok:#2fb06a;--warn:#e0a63c;--err:#e04b4b;--txt:#eef2f7;--dim:#8a97a6}
*{box-sizing:border-box;font-family:-apple-system,system-ui,sans-serif}
body{margin:0;background:var(--bg);color:var(--txt);padding:12px;max-width:720px;margin:0 auto}
h1{font-size:20px;margin:6px 0 2px}
.sub{color:var(--dim);font-size:13px;margin-bottom:12px}
.tabs{display:flex;gap:8px;margin-bottom:12px}
.tab{flex:1;padding:10px;text-align:center;background:var(--card);border-radius:8px;cursor:pointer;font-weight:600}
.tab.on{background:var(--pri)}
.card{background:var(--card);border-radius:10px;padding:12px;margin-bottom:10px}
label{display:block;font-size:12px;color:var(--dim);margin:8px 0 3px}
input,select,textarea{width:100%;padding:8px;border-radius:6px;border:1px solid #2c3644;background:#0e1218;color:var(--txt);font-size:15px}
textarea{min-height:96px;resize:vertical}
.row{display:flex;gap:8px}.row>div{flex:1}
.days{display:flex;gap:4px}.days button{flex:1;padding:8px 0;border:none;border-radius:6px;background:#0e1218;color:var(--dim);font-weight:700;cursor:pointer}
.days button.on{background:var(--pri);color:#fff}
.del{background:var(--err);border:none;color:#fff;padding:6px 10px;border-radius:6px;cursor:pointer;float:right}
.use{background:#2c3644;border:none;color:#fff;padding:8px 10px;border-radius:6px;cursor:pointer;white-space:nowrap}
.scan{background:#0e1218;border:1px dashed var(--warn);border-radius:8px;padding:10px;margin-bottom:10px;font-size:14px}
.role{float:right;font-size:12px;color:var(--dim);text-transform:uppercase}
.bar{position:sticky;bottom:0;background:var(--bg);padding:10px 0;display:flex;gap:8px}
.btn{flex:1;padding:12px;border:none;border-radius:8px;font-size:16px;font-weight:700;cursor:pointer;color:#fff}
.add{background:#2c3644}.save{background:var(--ok)}
.toast{position:fixed;left:50%;transform:translateX(-50%);bottom:80px;background:#000a;padding:10px 16px;border-radius:8px;opacity:0;transition:.2s}
.toast.on{opacity:1}
</style></head><body>
<h1>Chore Station</h1>
<div class="sub" id="ip"></div>
<div class="tabs">
  <div class="tab on" id="tabR" onclick="show('R')">Routines</div>
  <div class="tab" id="tabC" onclick="show('C')">Chores</div>
  <div class="tab" id="tabF" onclick="show('F')">Family</div>
</div>
<div id="listR"></div>
<div id="exC" class="scan" style="display:none">Starting fresh? <button type="button" class="use" onclick="loadExamples()">Replace all chores with the examples</button></div>
<div id="listC" style="display:none"></div>
<div id="paneF" style="display:none">
  <div class="scan" id="scan">Scan a tag on the station to see its ID here.</div>
  <div id="listF"></div>
  <div class="scan" id="fw">Firmware: ...</div>
</div>
<div class="bar">
  <button class="btn add" onclick="add()">+ Add</button>
  <button class="btn save" onclick="save()">Save changes</button>
</div>
<div class="toast" id="toast"></div>
<script>
var fam=[], cur='R', lastUid='';
var DAYS=['Su','Mo','Tu','We','Th','Fr','Sa'];
var CATS=['Cleaning','Health','Family','Academic','Pet Care','Outdoor'];
var SIZES=['Small','Medium','Large'];
// Same as SDCard_Contents/tasks/recurring.json (the jsdom test keeps them in sync)
var EXAMPLE_CHORES=[{"id":1101,"name":"Clean Playroom","cat":0,"size":0,"type":0,"state":0,"assigned":255,"creator":0,"minTime":0,"timeout":0,"approval":true,"ownMess":false,"childClaimed":false,"parentOverride":false,"noRemApproved":false,"noRemToggle":false,"recurDays":127,"recurHour":0,"recurMin":0,"startEpoch":0,"expires":false},{"id":1102,"name":"Brush Teeth - AM","cat":1,"size":0,"type":0,"state":0,"assigned":254,"creator":0,"minTime":120,"timeout":0,"approval":false,"ownMess":false,"childClaimed":false,"parentOverride":false,"noRemApproved":false,"noRemToggle":false,"recurDays":127,"recurHour":6,"recurMin":0,"startEpoch":0,"expires":true},{"id":1103,"name":"Brush Teeth - PM","cat":1,"size":0,"type":0,"state":0,"assigned":254,"creator":0,"minTime":120,"timeout":0,"approval":false,"ownMess":false,"childClaimed":false,"parentOverride":false,"noRemApproved":false,"noRemToggle":false,"recurDays":127,"recurHour":18,"recurMin":0,"startEpoch":0,"expires":true},{"id":1104,"name":"Set the table","cat":2,"size":0,"type":0,"state":0,"assigned":255,"creator":0,"minTime":0,"timeout":0,"approval":false,"ownMess":false,"childClaimed":false,"parentOverride":false,"noRemApproved":false,"noRemToggle":false,"recurDays":127,"recurHour":17,"recurMin":0,"startEpoch":0,"expires":true},{"id":1105,"name":"Clear the table","cat":2,"size":0,"type":0,"state":0,"assigned":255,"creator":0,"minTime":0,"timeout":0,"approval":false,"ownMess":false,"childClaimed":false,"parentOverride":false,"noRemApproved":false,"noRemToggle":false,"recurDays":127,"recurHour":17,"recurMin":0,"startEpoch":0,"expires":true},{"id":1106,"name":"Empty Dishwasher - Top","cat":0,"size":0,"type":0,"state":0,"assigned":255,"creator":0,"minTime":0,"timeout":0,"approval":false,"ownMess":false,"childClaimed":false,"parentOverride":false,"noRemApproved":false,"noRemToggle":false,"recurDays":127,"recurHour":9,"recurMin":0,"startEpoch":0,"expires":false},{"id":1107,"name":"Empty Dishwasher - Bottom","cat":0,"size":0,"type":0,"state":0,"assigned":255,"creator":0,"minTime":0,"timeout":0,"approval":false,"ownMess":false,"childClaimed":false,"parentOverride":false,"noRemApproved":false,"noRemToggle":false,"recurDays":127,"recurHour":9,"recurMin":0,"startEpoch":0,"expires":false},{"id":2001,"name":"Yardwork","cat":5,"size":1,"type":0,"state":0,"assigned":255,"creator":0,"minTime":1800,"timeout":0,"approval":true,"ownMess":false,"childClaimed":false,"parentOverride":false,"noRemApproved":false,"noRemToggle":false,"recurDays":65,"recurHour":9,"recurMin":0,"startEpoch":0,"expires":false},{"id":2002,"name":"Put clothes away","cat":0,"size":0,"type":0,"state":0,"assigned":254,"creator":0,"minTime":0,"timeout":0,"approval":true,"ownMess":false,"childClaimed":false,"parentOverride":false,"noRemApproved":false,"noRemToggle":false,"recurDays":65,"recurHour":12,"recurMin":0,"startEpoch":0,"expires":false}];

function esc(s){return String(s==null?'':s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function toast(m){var t=document.getElementById('toast');t.textContent=m;t.className='toast on';setTimeout(function(){t.className='toast'},1800)}
function show(t){cur=t;['R','C','F'].forEach(function(k){document.getElementById('tab'+k).className='tab'+(t==k?' on':'')});document.getElementById('listR').style.display=t=='R'?'':'none';document.getElementById('listC').style.display=t=='C'?'':'none';document.getElementById('exC').style.display=t=='C'?'':'none';document.getElementById('paneF').style.display=t=='F'?'':'none'}

// 254 = each child gets their own copy; 255 = one copy: for a chore the first
// to start it takes it, a routine is shown to everyone (parents too).
function assignOpts(v,withAll,routine){
  var o='<option value="255"'+(v==255?' selected':'')+'>'+(routine?'Everyone':'First taker')+'</option>';
  if(withAll)o+='<option value="254"'+(v==254?' selected':'')+'>Per child</option>';
  fam.forEach(function(m){if(m.role=='child')o+='<option value="'+m.id+'"'+(v==m.id?' selected':'')+'>'+esc(m.name)+'</option>'});
  return o;
}
function daysRow(mask){
  var h='<div class="days" data-mask="'+mask+'">';
  for(var i=0;i<7;i++)h+='<button type="button" class="'+((mask>>i)&1?'on':'')+'" onclick="tday(this,'+i+')">'+DAYS[i]+'</button>';
  return h+'</div>';
}
// Times are stored in seconds but edited in minutes
function mins(sec){return Number(((+sec||0)/60).toFixed(2))}
function secs(min){return Math.max(0,Math.round((+min||0)*60))}
function tday(b,i){var d=b.parentNode,m=+d.dataset.mask;m^=(1<<i);d.dataset.mask=m;b.className=((m>>i)&1)?'on':''}

function routineCard(r){
  r=r||{name:'',assigned:254,points:5,startHour:19,endHour:22,recurDays:127,steps:[]};
  return '<div class="card" data-k="R" data-id="'+(r.id||0)+'">'
   +'<button class="del" onclick="this.parentNode.remove()">Delete</button>'
   +'<label>Name</label><input class="f-name" value="'+esc(r.name)+'">'
   +'<div class="row"><div><label>Who</label><select class="f-assigned">'+assignOpts(r.assigned,true,true)+'</select></div>'
   +'<div><label>Points</label><input class="f-points" type="number" value="'+(r.points||5)+'"></div></div>'
   +'<div class="row"><div><label>Start hour</label><input class="f-startHour" type="number" min="0" max="23" value="'+(r.startHour||0)+'"></div>'
   +'<div><label>End hour</label><input class="f-endHour" type="number" min="0" max="23" value="'+(r.endHour==null?23:r.endHour)+'"></div></div>'
   +'<label>Days</label>'+daysRow(r.recurDays==null?127:r.recurDays)
   +'<label>Steps (one per line, up to 10 steps of 39 characters)</label><textarea class="f-steps">'+esc((r.steps||[]).join('\n'))+'</textarea>'
   +'</div>';
}
function choreCard(t){
  t=t||{name:'',cat:0,size:0,assigned:255,minTime:0,timeout:0,approval:false,ownMess:false,expires:false,recurDays:127,recurHour:7,recurMin:0};
  var co='',so='';
  CATS.forEach(function(c,i){co+='<option value="'+i+'"'+(t.cat==i?' selected':'')+'>'+c+'</option>'});
  SIZES.forEach(function(s,i){so+='<option value="'+i+'"'+(t.size==i?' selected':'')+'>'+s+'</option>'});
  return '<div class="card" data-k="C" data-id="'+(t.id||0)+'">'
   +'<button class="del" onclick="this.parentNode.remove()">Delete</button>'
   +'<label>Name</label><input class="f-name" value="'+esc(t.name)+'">'
   +'<div class="row"><div><label>Category</label><select class="f-cat">'+co+'</select></div>'
   +'<div><label>Size</label><select class="f-size">'+so+'</select></div></div>'
   +'<div class="row"><div><label>Who</label><select class="f-assigned">'+assignOpts(t.assigned,true)+'</select></div>'
   +'<div><label>Approval?</label><select class="f-approval"><option value="0"'+(!t.approval?' selected':'')+'>No</option><option value="1"'+(t.approval?' selected':'')+'>Yes</option></select></div></div>'
   +'<label>Own mess? (reduced points)</label><select class="f-ownMess"><option value="0"'+(!t.ownMess?' selected':'')+'>No</option><option value="1"'+(t.ownMess?' selected':'')+'>Yes</option></select>'
   +'<label>If not done by midnight</label><select class="f-expires"><option value="0"'+(!t.expires?' selected':'')+'>Carries over to the next day</option><option value="1"'+(t.expires?' selected':'')+'>Disappears (next day starts fresh)</option></select>'
   +'<div class="row"><div><label>Min. time (minutes, 0 = none)</label><input class="f-minTime" type="number" min="0" step="any" value="'+mins(t.minTime)+'"></div>'
   +'<div><label>Time limit (minutes, 0 = none)</label><input class="f-timeout" type="number" min="0" step="any" value="'+mins(t.timeout)+'"></div></div>'
   +'<label>Days</label>'+daysRow(t.recurDays==null?127:t.recurDays)
   +'<div class="row"><div><label>Hour</label><input class="f-recurHour" type="number" min="0" max="23" value="'+(t.recurHour||0)+'"></div>'
   +'<div><label>Minute</label><input class="f-recurMin" type="number" min="0" max="59" value="'+(t.recurMin||0)+'"></div></div>'
   +'</div>';
}
function memberCard(m){
  var isNew=!m; m=m||{id:-1,name:'',role:'child',dailyGoal:5,uid:'',points:0};
  return '<div class="card" data-k="F" data-id="'+m.id+'">'
   +(isNew?'<select class="f-role" style="width:auto;float:right"><option value="child">Child</option><option value="parent">Parent</option></select>'
          :'<span class="role">'+esc(m.role)+' &middot; '+(m.points||0)+' pts</span>')
   +'<label>Name</label><input class="f-name" maxlength="15" value="'+esc(m.name)+'">'
   +'<div class="row"><div><label>Daily goal</label><input class="f-goal" type="number" min="1" max="20" value="'+(m.dailyGoal||5)+'"></div>'
   +'<div style="flex:2"><label>Tag ID</label><div class="row"><input class="f-uid" placeholder="04:A2:3B:C1" value="'+esc(m.uid)+'">'
   +'<button type="button" class="use" onclick="useScan(this)">Use last scan</button></div></div></div>'
   +'</div>';
}

function add(){
  if(cur=='F'){document.getElementById('listF').insertAdjacentHTML('beforeend',memberCard());return}
  var l=document.getElementById('list'+cur);l.insertAdjacentHTML('beforeend',cur=='R'?routineCard():choreCard());
}
function useScan(b){
  if(!lastUid){toast('Scan an unregistered tag on the station first');return}
  b.parentNode.querySelector('.f-uid').value=lastUid;toast('Tag filled in - press Save');
}

// Keep existing ids (the station tracks today's progress by id); give new
// items the next free id.
function assignIds(out,cards,base){
  var used=out.map(function(o,i){return +cards[i].dataset.id||0}).filter(Boolean);
  var next=Math.max.apply(null,[base].concat(used));
  out.forEach(function(o,i){var id=+cards[i].dataset.id||0;o.id=id||++next});
}

var cut=[];   // routine steps shortened on the last collect()
function collect(){
  var cards=document.querySelectorAll('#list'+cur+' .card'),out=[];cut=[];
  cards.forEach(function(c){
    var g=function(s){return c.querySelector('.'+s)};
    var mask=+c.querySelector('.days').dataset.mask;
    if(cur=='R'){
      var steps=g('f-steps').value.split('\n').map(function(s){return s.trim()}).filter(Boolean);
      // The station keeps 10 steps of up to 39 characters: say so instead of cutting silently
      steps.forEach(function(st,i){if(st.length>39){steps[i]=st.slice(0,39);cut.push((g('f-name').value||'Routine')+' step '+(i+1))}});
      if(steps.length>10){cut.push((g('f-name').value||'Routine')+': only the first 10 steps');steps=steps.slice(0,10)}
      out.push({id:0,name:g('f-name').value,assigned:+g('f-assigned').value,points:+g('f-points').value,
        startHour:+g('f-startHour').value,endHour:+g('f-endHour').value,recurDays:mask,steps:steps});
    }else{
      out.push({id:0,name:g('f-name').value,cat:+g('f-cat').value,size:+g('f-size').value,type:0,state:0,
        assigned:+g('f-assigned').value,creator:0,minTime:secs(g('f-minTime').value),timeout:secs(g('f-timeout').value),
        approval:g('f-approval').value=='1',ownMess:g('f-ownMess').value=='1',childClaimed:false,parentOverride:false,
        noRemApproved:false,noRemToggle:false,recurDays:mask,recurHour:+g('f-recurHour').value,recurMin:+g('f-recurMin').value,
        startEpoch:0,expires:g('f-expires').value=='1',doneEarlier:false,fromAnyone:false});
    }
  });
  assignIds(out,cards,cur=='R'?0:2000);
  return out;
}
function collectFamily(){
  var out=[];
  document.querySelectorAll('#listF .card').forEach(function(c){
    var g=function(s){return c.querySelector('.'+s)};
    var m={id:+c.dataset.id,name:g('f-name').value.trim(),dailyGoal:+g('f-goal').value,uid:g('f-uid').value.trim()};
    if(g('f-role'))m.role=g('f-role').value;
    out.push(m);
  });
  return out;
}

function post(url,body,after){
  fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(function(r){return r.json()}).then(function(j){toast(j.ok?(cut.length?'Saved - shortened: '+cut.join(', '):'Saved!'):'Error: '+(j.error||'?'));if(j.ok&&after)after()})
    .catch(function(){toast('Network error')});
}
function save(){cut=[];
  if(cur=='F'){post('/api/family',{members:collectFamily()},loadFamily);return}
  var items=collect();
  post(cur=='R'?'/api/routines':'/api/tasks',(cur=='R')?{routines:items}:{tasks:items});
}

// Wipe the chore list and save the examples in its place. The station drops
// today's copies of chores that no longer exist and adds the new ones.
function loadExamples(){
  if(!confirm('Replace ALL chores with the '+EXAMPLE_CHORES.length+' example chores? Routines and family are not touched.'))return;
  render('listC',EXAMPLE_CHORES,choreCard);
  show('C');save();
}
// Firmware updates come from GitHub releases; the station installs and restarts.
function fwShow(j){
  var el=document.getElementById('fw');if(!el||!j)return;
  var h='Firmware <b>'+esc(j.current||'?')+'</b>';
  if(j.available)h+=' - <b>'+esc(j.latest)+'</b> is available. <button type="button" class="use" onclick="fwInstall()">Install '+esc(j.latest)+'</button>';
  else h+=(j.checked?(j.error?' - '+esc(j.error):' - up to date'):'')+' <button type="button" class="use" onclick="fwCheck()">Check for update</button>';
  el.innerHTML=h;
}
function fwLoad(){fetch('/api/firmware').then(function(r){return r.json()}).then(fwShow).catch(function(){})}
function fwCheck(){toast('Checking GitHub...');fetch('/api/firmware/check',{method:'POST'}).then(function(r){return r.json()}).then(fwShow).catch(function(){toast('Network error')})}
function fwInstall(){
  if(!confirm('Install the update now? The station shows progress and restarts (about a minute). Chores and points are kept.'))return;
  fetch('/api/firmware/install',{method:'POST'}).then(function(r){return r.json()}).then(function(j){toast(j.ok?'Installing on the station...':'Error: '+(j.error||'?'))}).catch(function(){toast('Network error')});
}
function render(id,arr,fn){document.getElementById(id).innerHTML=arr.map(fn).join('')}
function loadFamily(){
  return fetch('/api/family').then(function(r){return r.json()}).then(function(j){fam=j.members||[];render('listF',fam,memberCard)});
}
function pollScan(){
  fetch('/api/lastscan').then(function(r){return r.json()}).then(function(j){
    if(!j||!j.uid)return;
    lastUid=j.uid;
    document.getElementById('scan').innerHTML='Last unregistered tag: <b>'+esc(j.uid)+'</b> ('+(j.ageSec||0)+'s ago). Tap "Use last scan" on a person, then Save.';
  }).catch(function(){});
}

Promise.all([
  fetch('/api/family').then(function(r){return r.json()}),
  fetch('/api/routines').then(function(r){return r.json()}),
  fetch('/api/tasks').then(function(r){return r.json()})
]).then(function(res){
  fam=res[0].members||[];
  render('listR',(res[1].routines||[]),routineCard);
  render('listC',(res[2].tasks||[]),choreCard);
  render('listF',fam,memberCard);
  document.getElementById('ip').textContent='Editing on '+location.host;
}).catch(function(){toast('Load failed')});
pollScan();setInterval(pollScan,3000);fwLoad();
</script>
</body></html>
)HTML";
