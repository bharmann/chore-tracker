// =============================================================================
// web_server.h — Parent Web UI for editing Chores & Routines
// =============================================================================
// The touchscreen is fine for kids scanning and tapping, but painful for a
// parent authoring tasks (preset names only, no keyboard, resistive touch).
// This module runs a small HTTP server on the ESP32 (only reachable once WiFi
// is up) so a parent can open the station's IP in a phone browser and edit
// recurring chores and routines with real forms and a keyboard.
//
// Data contract: the API reads and writes the SAME SD JSON files the firmware
// already uses — /tasks/recurring.json and /config/routines.json — so there is
// one source of truth. On a successful save the module raises a reload flag;
// the main loop reloads the affected module (scheduler / routines) so changes
// take effect without a reboot.
//
// Endpoints:
//   GET  /               → the single-page editor (HTML/JS, served from PROGMEM)
//   GET  /api/family     → members (id, name, role) for assignee dropdowns
//   GET  /api/routines   → current /config/routines.json
//   POST /api/routines   → replace /config/routines.json (validated)
//   GET  /api/tasks      → current /tasks/recurring.json
//   POST /api/tasks      → replace /tasks/recurring.json (validated)
//
// Scope note: no authentication — this is a trusted-home-LAN tool, same threat
// model as the SD card itself. WiFi credentials are never served. If exposed
// beyond the LAN this would need auth; documented as a known limitation.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include "data_model.h"
#include "storage_manager.h"

extern StorageManager storage;
extern FamilyMember   family[];
extern uint8_t        familyCount;

class WebUI {
public:
    // Public reload flags — the main loop consumes these after a save so the
    // running firmware picks up edited templates/routines without a reboot.
    bool pendingTaskReload    = false;
    bool pendingRoutineReload = false;

    void begin() {
        _server.on("/", HTTP_GET, [this]() { _handleRoot(); });
        _server.on("/api/family",   HTTP_GET,  [this]() { _handleFamily(); });
        _server.on("/api/routines", HTTP_GET,  [this]() { _streamFile("/config/routines.json", "{\"routines\":[]}"); });
        _server.on("/api/routines", HTTP_POST, [this]() { _handleSave("/config/routines.json", pendingRoutineReload); });
        _server.on("/api/tasks",    HTTP_GET,  [this]() { _streamFile("/tasks/recurring.json", "{\"tasks\":[]}"); });
        _server.on("/api/tasks",    HTTP_POST, [this]() { _handleSave("/tasks/recurring.json", pendingTaskReload); });
        _server.onNotFound([this]() { _server.send(404, "text/plain", "Not found"); });
        _server.begin();
        _started = true;
        Serial.println("[WEB] Server started on port 80.");
    }

    // Call every loop; cheap when idle.
    void handle() {
        if (_started) _server.handleClient();
    }

    bool isStarted() const { return _started; }

private:
    WebServer _server{80};
    bool      _started = false;

    void _streamFile(const char* path, const char* fallback) {
        File f = SD.open(path, FILE_READ);
        if (!f) { _server.send(200, "application/json", fallback); return; }
        _server.streamFile(f, "application/json");
        f.close();
    }

    void _handleSave(const char* path, bool& reloadFlag) {
        if (!_server.hasArg("plain")) {
            _server.send(400, "application/json", "{\"error\":\"empty body\"}");
            return;
        }
        String body = _server.arg("plain");
        if (storage.writeRawJson(path, body)) {
            reloadFlag = true;
            _server.send(200, "application/json", "{\"ok\":true}");
            Serial.printf("[WEB] Saved %s (%u bytes)\n", path, (unsigned)body.length());
        } else {
            _server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        }
    }

    void _handleFamily() {
        String out = "{\"members\":[";
        for (uint8_t i = 0; i < familyCount; i++) {
            if (i) out += ",";
            out += "{\"id\":";
            out += (int)family[i].id;
            out += ",\"name\":\"";
            out += family[i].name;
            out += "\",\"role\":\"";
            out += (family[i].role == Role::PARENT) ? "parent" : "child";
            out += "\"}";
        }
        out += "]}";
        _server.send(200, "application/json", out);
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
</div>
<div id="listR"></div>
<div id="listC" style="display:none"></div>
<div class="bar">
  <button class="btn add" onclick="add()">+ Add</button>
  <button class="btn save" onclick="save()">Save changes</button>
</div>
<div class="toast" id="toast"></div>
<script>
var fam=[], cur='R';
var DAYS=['Su','Mo','Tu','We','Th','Fr','Sa'];
var CATS=['Cleaning','Health','Family','Academic','Pet Care','Outdoor'];
var SIZES=['Small','Medium','Large'];

function esc(s){return (s||'').replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function toast(m){var t=document.getElementById('toast');t.textContent=m;t.className='toast on';setTimeout(function(){t.className='toast'},1800)}
function show(t){cur=t;document.getElementById('tabR').className='tab'+(t=='R'?' on':'');document.getElementById('tabC').className='tab'+(t=='C'?' on':'');document.getElementById('listR').style.display=t=='R'?'':'none';document.getElementById('listC').style.display=t=='C'?'':'none'}

function assignOpts(v,withAll){
  var o='<option value="255"'+(v==255?' selected':'')+'>Anyone</option>';
  if(withAll)o+='<option value="254"'+(v==254?' selected':'')+'>All children</option>';
  fam.forEach(function(m){if(m.role=='child')o+='<option value="'+m.id+'"'+(v==m.id?' selected':'')+'>'+esc(m.name)+'</option>'});
  return o;
}
function daysRow(mask){
  var h='<div class="days" data-mask="'+mask+'">';
  for(var i=0;i<7;i++)h+='<button type="button" class="'+((mask>>i)&1?'on':'')+'" onclick="tday(this,'+i+')">'+DAYS[i]+'</button>';
  return h+'</div>';
}
function tday(b,i){var d=b.parentNode,m=+d.dataset.mask;m^=(1<<i);d.dataset.mask=m;b.className=((m>>i)&1)?'on':''}

function routineCard(r){
  r=r||{name:'',assigned:254,points:5,startHour:19,endHour:22,recurDays:127,steps:[]};
  return '<div class="card" data-k="R">'
   +'<button class="del" onclick="this.parentNode.remove()">Delete</button>'
   +'<label>Name</label><input class="f-name" value="'+esc(r.name)+'">'
   +'<div class="row"><div><label>Who</label><select class="f-assigned">'+assignOpts(r.assigned,true)+'</select></div>'
   +'<div><label>Points</label><input class="f-points" type="number" value="'+(r.points||5)+'"></div></div>'
   +'<div class="row"><div><label>Start hour</label><input class="f-startHour" type="number" min="0" max="23" value="'+(r.startHour||0)+'"></div>'
   +'<div><label>End hour</label><input class="f-endHour" type="number" min="0" max="23" value="'+(r.endHour==null?23:r.endHour)+'"></div></div>'
   +'<label>Days</label>'+daysRow(r.recurDays==null?127:r.recurDays)
   +'<label>Steps (one per line)</label><textarea class="f-steps">'+esc((r.steps||[]).join('\n'))+'</textarea>'
   +'</div>';
}
function choreCard(t){
  t=t||{name:'',cat:0,size:0,assigned:255,minTime:0,timeout:0,approval:false,recurDays:127,recurHour:7,recurMin:0};
  var co='',so='';
  CATS.forEach(function(c,i){co+='<option value="'+i+'"'+(t.cat==i?' selected':'')+'>'+c+'</option>'});
  SIZES.forEach(function(s,i){so+='<option value="'+i+'"'+(t.size==i?' selected':'')+'>'+s+'</option>'});
  return '<div class="card" data-k="C">'
   +'<button class="del" onclick="this.parentNode.remove()">Delete</button>'
   +'<label>Name</label><input class="f-name" value="'+esc(t.name)+'">'
   +'<div class="row"><div><label>Category</label><select class="f-cat">'+co+'</select></div>'
   +'<div><label>Size</label><select class="f-size">'+so+'</select></div></div>'
   +'<div class="row"><div><label>Who</label><select class="f-assigned">'+assignOpts(t.assigned,false)+'</select></div>'
   +'<div><label>Approval?</label><select class="f-approval"><option value="0"'+(!t.approval?' selected':'')+'>No</option><option value="1"'+(t.approval?' selected':'')+'>Yes</option></select></div></div>'
   +'<div class="row"><div><label>Min sec</label><input class="f-minTime" type="number" value="'+(t.minTime||0)+'"></div>'
   +'<div><label>Timeout sec</label><input class="f-timeout" type="number" value="'+(t.timeout||0)+'"></div></div>'
   +'<label>Days</label>'+daysRow(t.recurDays==null?127:t.recurDays)
   +'<div class="row"><div><label>Hour</label><input class="f-recurHour" type="number" min="0" max="23" value="'+(t.recurHour||0)+'"></div>'
   +'<div><label>Minute</label><input class="f-recurMin" type="number" min="0" max="59" value="'+(t.recurMin||0)+'"></div></div>'
   +'</div>';
}

function add(){var l=document.getElementById('list'+cur);l.insertAdjacentHTML('beforeend',cur=='R'?routineCard():choreCard())}

function collect(){
  var cards=document.querySelectorAll('#list'+cur+' .card'),out=[];
  cards.forEach(function(c){
    var g=function(s){return c.querySelector('.'+s)};
    var mask=+c.querySelector('.days').dataset.mask;
    if(cur=='R'){
      var steps=g('f-steps').value.split('\n').map(function(s){return s.trim()}).filter(Boolean);
      out.push({id:0,name:g('f-name').value,assigned:+g('f-assigned').value,points:+g('f-points').value,
        startHour:+g('f-startHour').value,endHour:+g('f-endHour').value,recurDays:mask,steps:steps});
    }else{
      out.push({id:0,name:g('f-name').value,cat:+g('f-cat').value,size:+g('f-size').value,type:0,state:0,
        assigned:+g('f-assigned').value,creator:0,minTime:+g('f-minTime').value,timeout:+g('f-timeout').value,
        approval:g('f-approval').value=='1',ownMess:false,childClaimed:false,parentOverride:false,
        noRemApproved:false,noRemToggle:false,recurDays:mask,recurHour:+g('f-recurHour').value,recurMin:+g('f-recurMin').value});
    }
  });
  // Assign stable-ish ids
  out.forEach(function(o,i){o.id=(cur=='R')?(i+1):(2000+i)});
  return out;
}

function save(){
  var items=collect();
  var body=(cur=='R')?{routines:items}:{tasks:items};
  fetch(cur=='R'?'/api/routines':'/api/tasks',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(function(r){return r.json()}).then(function(j){toast(j.ok?'Saved!':'Error: '+(j.error||'?'))})
    .catch(function(){toast('Network error')});
}

function render(id,arr,fn){document.getElementById(id).innerHTML=arr.map(fn).join('')}

Promise.all([
  fetch('/api/family').then(function(r){return r.json()}),
  fetch('/api/routines').then(function(r){return r.json()}),
  fetch('/api/tasks').then(function(r){return r.json()})
]).then(function(res){
  fam=res[0].members||[];
  render('listR',(res[1].routines||[]),routineCard);
  render('listC',(res[2].tasks||[]),choreCard);
  document.getElementById('ip').textContent='Editing on '+location.host;
}).catch(function(){toast('Load failed')});
</script>
</body></html>
)HTML";
