const fs = require('fs');
const { JSDOM } = require('jsdom');

const ROOT = require('path').join(__dirname, '..');
const web = fs.readFileSync(ROOT + '/ChoreStation/web_server.h', 'utf8');
const storage = fs.readFileSync(ROOT + '/ChoreStation/storage_manager.h', 'utf8');
const seedRoutines = JSON.parse(fs.readFileSync(ROOT + '/SDCard_Contents/config/routines.json', 'utf8'));
const seedTasks = JSON.parse(fs.readFileSync(ROOT + '/SDCard_Contents/tasks/recurring.json', 'utf8'));

// Extract the embedded HTML page between R"HTML( ... )HTML"
const m = web.match(/R"HTML\(([\s\S]*?)\)HTML"/);
if (!m) { console.error('FAIL: could not extract PAGE_HTML'); process.exit(1); }
const html = m[1];

let pass = 0, fail = 0;
function ok(name, cond) { (cond ? (pass++, console.log('  PASS ' + name)) : (fail++, console.log('  FAIL ' + name))); }

const family = { members: [
  { id: 0, name: 'Mom', role: 'parent' },
  { id: 1, name: 'Dad', role: 'parent' },
  { id: 2, name: 'Ada', role: 'child' },
  { id: 3, name: 'Bea', role: 'child' },
]};

let posted = {};   // captured POST bodies by path

function mockFetch(url, opts) {
  let body;
  if (opts && opts.method === 'POST') {
    posted[url] = JSON.parse(opts.body);
    body = { ok: true };
  } else if (url === '/api/family') body = family;
  else if (url === '/api/routines') body = seedRoutines;
  else if (url === '/api/tasks') body = seedTasks;
  else body = {};
  return Promise.resolve({ json: () => Promise.resolve(body) });
}

const dom = new JSDOM(html, {
  runScripts: 'dangerously',
  url: 'http://192.168.1.50/',
  beforeParse(window) { window.fetch = mockFetch; },
});
const w = dom.window;

// Let the on-load Promise.all resolve.
setTimeout(() => {
  console.log('\n[render]');
  const rCards = w.document.querySelectorAll('#listR .card');
  const cCards = w.document.querySelectorAll('#listC .card');
  ok('routine cards rendered = seed count', rCards.length === seedRoutines.routines.length);
  ok('chore cards rendered = seed count', cCards.length === seedTasks.tasks.length);

  // Assignee dropdown should list children + Anyone/All children
  const firstAssign = w.document.querySelector('#listR .card .f-assigned');
  ok('routine assignee has All children option', !!firstAssign && /All children/.test(firstAssign.innerHTML));
  ok('routine assignee lists child names', !!firstAssign && /Ada/.test(firstAssign.innerHTML) && /Bea/.test(firstAssign.innerHTML));

  console.log('\n[routines round-trip]');
  w.show('R');
  const rOut = w.collect();
  const bed = rOut.find(x => x.name === 'Bedtime');
  const morn = rOut.find(x => x.name === 'School Morning');
  ok('Bedtime present', !!bed);
  ok('Bedtime recurDays preserved (127)', bed && bed.recurDays === 127);
  ok('Bedtime steps preserved', bed && bed.steps.length === 5 && bed.steps[0] === 'Put on pajamas');
  ok('Bedtime points preserved', bed && bed.points === 5);
  ok('Bedtime window preserved', bed && bed.startHour === 19 && bed.endHour === 22);
  ok('School Morning recurDays preserved (62 = Mon-Fri)', morn && morn.recurDays === 62);
  ok('School Morning assigned=254 (all children)', morn && morn.assigned === 254);

  console.log('\n[chores round-trip]');
  w.show('C');
  const cOut = w.collect();
  const brush = cOut.find(x => x.name === 'Brush teeth (AM)');
  ok('Brush teeth AM present', !!brush);
  ok('  cat=1', brush && brush.cat === 1);
  ok('  size=0', brush && brush.size === 0);
  ok('  assigned=2', brush && brush.assigned === 2);
  ok('  minTime=120', brush && brush.minTime === 120);
  ok('  timeout=1800', brush && brush.timeout === 1800);
  ok('  recurDays=127', brush && brush.recurDays === 127);
  ok('  recurHour=7', brush && brush.recurHour === 7);
  const hw = cOut.find(x => x.name === 'Homework time');
  ok('Homework approval=true preserved', hw && hw.approval === true);
  ok('Homework recurDays=62 preserved', hw && hw.recurDays === 62);

  console.log('\n[add + save POST]');
  w.show('R');
  const before = w.document.querySelectorAll('#listR .card').length;
  w.add();
  ok('add() appends a routine card', w.document.querySelectorAll('#listR .card').length === before + 1);
  w.save();

  setTimeout(() => {
    ok('POST /api/routines captured', !!posted['/api/routines']);
    const pr = posted['/api/routines'];
    ok('posted wraps {routines:[...]}', pr && Array.isArray(pr.routines));
    ok('posted includes the added (blank) routine', pr && pr.routines.length === before + 1);

    console.log('\n[key contract vs storage_manager.h]');
    // Keys the firmware reads for routines:
    const routineKeysRead = ['id','name','assigned','points','startHour','endHour','recurDays','steps'];
    const rObj = pr.routines[0];
    routineKeysRead.forEach(k => ok('routine JSON has "' + k + '"', k in rObj));

    // Keys the firmware reads for tasks (from loadTasks):
    w.show('C'); w.save();
    setTimeout(() => {
      const pt = posted['/api/tasks'];
      ok('POST /api/tasks captured', !!pt && Array.isArray(pt.tasks));
      const taskKeysRead = ['id','name','cat','size','type','state','assigned','creator',
        'minTime','timeout','approval','ownMess','childClaimed','parentOverride',
        'noRemApproved','noRemToggle','recurDays','recurHour','recurMin'];
      const tObj = pt.tasks[0];
      taskKeysRead.forEach(k => ok('task JSON has "' + k + '"', k in tObj));

      // Cross-check: every key the loader reads via t["..."] should be produced.
      const loaderTaskKeys = [...storage.matchAll(/t\["(\w+)"\]/g)].map(x => x[1]);
      const missing = [...new Set(loaderTaskKeys)].filter(k => !(k in tObj));
      ok('no task key read by loader is missing from POST (' + missing.join(',') + ')', missing.length === 0);

      const loaderRoutineKeys = [...storage.matchAll(/r\["(\w+)"\]/g)].map(x => x[1])
        .filter(k => k !== 'steps' || true);
      const rMissing = [...new Set(loaderRoutineKeys)].filter(k => !(k in rObj));
      ok('no routine key read by loader is missing from POST (' + rMissing.join(',') + ')', rMissing.length === 0);

      console.log('\n' + pass + ' passed, ' + fail + ' failed');
      process.exit(fail ? 1 : 0);
    }, 20);
  }, 20);
}, 60);
