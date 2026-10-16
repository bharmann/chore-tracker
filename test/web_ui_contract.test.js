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
  { id: 0, name: 'Mom', role: 'parent', dailyGoal: 3, uid: '04:A2:3B:C1', points: 12 },
  { id: 1, name: 'Dad', role: 'parent', dailyGoal: 3, uid: '', points: 0 },
  { id: 2, name: 'Ada', role: 'child',  dailyGoal: 5, uid: 'DE:AD:BE:EF', points: 40 },
  { id: 3, name: 'Bea <b>', role: 'child', dailyGoal: 5, uid: 'CA:FE:BA:BE', points: 33 },
]};
const lastScan = { uid: '04:11:22:33:44:55:66', ageSec: 4 };

let posted = {};   // captured POST bodies by path

function mockFetch(url, opts) {
  let body;
  if (opts && opts.method === 'POST') {
    posted[url] = JSON.parse(opts.body);
    body = { ok: true };
  } else if (url === '/api/family') body = family;
  else if (url === '/api/routines') body = seedRoutines;
  else if (url === '/api/tasks') body = seedTasks;
  else if (url === '/api/lastscan') body = lastScan;
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
  ok('routine assignee has Per child option', !!firstAssign && /Per child/.test(firstAssign.innerHTML));
  ok('routine assignee lists child names', !!firstAssign && /Ada/.test(firstAssign.innerHTML) && /Bea/.test(firstAssign.innerHTML));
  ok('member names are HTML-escaped', !!firstAssign && /Bea &lt;b&gt;/.test(firstAssign.innerHTML));
  const firstChoreAssign = w.document.querySelector('#listC .card .f-assigned');
  ok('chore assignee offers All children (scheduler expands 254)', !!firstChoreAssign && /value="254"/.test(firstChoreAssign.innerHTML));

  console.log('\n[routines round-trip]');
  w.show('R');
  const rOut = w.collect();
  const bed = rOut.find(x => x.name === 'Bedtime');
  const morn = rOut.find(x => x.name === 'School Morning');
  ok('Bedtime present', !!bed);
  ok('Bedtime recurDays preserved (127)', bed && bed.recurDays === 127);
  ok('Bedtime steps preserved', bed && bed.steps.length === 4 && bed.steps[0] === 'Put on pajamas');
  ok('Bedtime points preserved', bed && bed.points === 2);
  ok('Bedtime window preserved', bed && bed.startHour === 19 && bed.endHour === 22);
  ok('long routine steps survive (up to 39 chars)', rOut.some(r => r.steps.includes('Check backpack: forms & notices')));
  {
    const card = w.document.querySelector('#listR .card');
    const ta = card.querySelector('.f-steps');
    const before = ta.value;
    ta.value = 'x'.repeat(45) + '\n' + Array.from({length: 11}, (_, i) => 'step ' + i).join('\n');
    const capped = w.collect().find(r => r.id === +card.dataset.id);
    ok('web caps steps at 39 chars', capped && capped.steps[0].length === 39);
    ok('web keeps at most 10 steps', capped && capped.steps.length === 10);
    ok('web reports what it shortened', w.eval('cut.length') >= 2);
    ta.value = before;
    w.collect();
  }
  const stepLen = +(fs.readFileSync(ROOT + '/ChoreStation/data_model.h', 'utf8').match(/ROUTINE_STEP_LEN\s*=\s*(\d+)/) || [])[1];
  ok('firmware step buffer fits every seed step (ROUTINE_STEP_LEN ' + stepLen + ')',
     seedRoutines.routines.every(r => r.steps.every(st => st.length < stepLen)));
  ok('School Morning recurDays preserved (62 = Mon-Fri)', morn && morn.recurDays === 62);
  ok('School Morning assigned=254 (all children)', morn && morn.assigned === 254);

  console.log('\n[chores round-trip]');
  w.show('C');
  const cOut = w.collect();
  // Every seed chore must come back with every editable field intact
  const choreFields = ['id', 'name', 'cat', 'size', 'assigned', 'minTime', 'timeout',
                       'approval', 'ownMess', 'expires', 'recurDays', 'recurHour', 'recurMin'];
  seedTasks.tasks.forEach(t => {
    const o = cOut.find(x => x.id === t.id);
    const bad = o ? choreFields.filter(f => o[f] !== t[f]) : ['missing'];
    ok('chore "' + t.name + '" round-trips' + (bad.length ? ' (' + bad.join(',') + ')' : ''), bad.length === 0);
  });
  ok('seed exercises all-children (254) and anyone (255)',
     seedTasks.tasks.some(t => t.assigned === 254) && seedTasks.tasks.some(t => t.assigned === 255));
  ok('seed exercises a timed (min time) and an approval chore',
     seedTasks.tasks.some(t => t.minTime > 0) && seedTasks.tasks.some(t => t.approval));
  ok('seed exercises an expiring and a carry-over chore',
     seedTasks.tasks.some(t => t.expires === true) && seedTasks.tasks.some(t => t.expires === false));

  // Times are edited in minutes but stored in seconds; odd values survive
  const odd = { id: 4242, name: 'Odd times', cat: 0, size: 0, assigned: 255, minTime: 90,
                timeout: 5430, approval: false, ownMess: true, recurDays: 127, recurHour: 8, recurMin: 15 };
  w.document.getElementById('listC').insertAdjacentHTML('beforeend', w.choreCard(odd));
  const oddOut = w.collect().find(x => x.id === 4242);
  ok('time editor shows minutes', /value="1.5"/.test(w.choreCard(odd)));
  ok('90 s min time round-trips', oddOut && oddOut.minTime === 90);
  ok('5430 s timeout round-trips', oddOut && oddOut.timeout === 5430);
  ok('own mess set on the station survives a web save', oddOut && oddOut.ownMess === true);
  w.document.getElementById('listC').innerHTML = seedTasks.tasks.map(w.choreCard).join('');

  console.log('\n[stable ids]');
  // The station tracks today's instances and routine progress by id, so a
  // save must not renumber existing items.
  ok('chore ids preserved on save', seedTasks.tasks.every(t => cOut.some(o => o.id === t.id && o.name === t.name)));
  ok('routine ids preserved on save', seedRoutines.routines.every(r => rOut.some(o => o.id === r.id && o.name === r.name)));
  w.show('C');
  w.document.querySelector('#listC .card .del').click();   // delete the first chore
  w.add();
  const cOut2 = w.collect();
  const maxSeed = Math.max(...seedTasks.tasks.map(t => t.id));
  ok('deleting a chore leaves the others\' ids alone', seedTasks.tasks.slice(1).every(t => cOut2.some(o => o.id === t.id && o.name === t.name)));
  ok('new chore gets a fresh id above all existing', cOut2[cOut2.length - 1].id === Math.max(2000, maxSeed) + 1);
  ok('no duplicate chore ids', new Set(cOut2.map(o => o.id)).size === cOut2.length);
  // put the chore list back for the contract checks below
  w.document.getElementById('listC').innerHTML = seedTasks.tasks.map(w.choreCard).join('');

  console.log('\n[replace with examples]');
  ok('built-in examples match SDCard_Contents/tasks/recurring.json',
     JSON.stringify(w.EXAMPLE_CHORES) === JSON.stringify(seedTasks.tasks));
  w.confirm = () => false;
  delete posted['/api/tasks'];
  w.loadExamples();
  ok('cancelling the confirm changes nothing', !posted['/api/tasks']);
  w.document.getElementById('listC').innerHTML = '';   // pretend the list was something else
  w.confirm = () => true;
  w.loadExamples();
  const ex = posted['/api/tasks'];
  ok('confirmed: saves exactly the example chores',
     ex && ex.tasks.length === seedTasks.tasks.length &&
     seedTasks.tasks.every(t => ex.tasks.some(o => o.id === t.id && o.name === t.name &&
       o.assigned === t.assigned && o.minTime === t.minTime && o.timeout === t.timeout)));

  console.log('\n[family tab]');
  w.show('F');
  const fCards = w.document.querySelectorAll('#listF .card');
  ok('family cards rendered', fCards.length === family.members.length);
  ok('last unknown scan shown', /04:11:22:33:44:55:66/.test(w.document.getElementById('scan').innerHTML));
  fCards[1].querySelector('.use').click();
  const fam = w.collectFamily();
  ok('"Use last scan" fills the tag field', fam[1].uid === lastScan.uid);
  ok('existing tags round-trip', fam[0].uid === '04:A2:3B:C1' && fam[2].uid === 'DE:AD:BE:EF');
  ok('family ids are positions', fam.map(f => f.id).join() === '0,1,2,3');
  ok('family goals round-trip', fam[2].dailyGoal === 5 && fam[0].dailyGoal === 3);
  w.add();
  const famNew = w.collectFamily();
  const added = famNew[famNew.length - 1];
  ok('added member has id -1 and a role', added.id === -1 && added.role === 'child');

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
        'noRemApproved','noRemToggle','recurDays','recurHour','recurMin','expires','doneEarlier'];
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

      console.log('\n[family save]');
      w.show('F'); w.save();
      setTimeout(() => {
        const pf = posted['/api/family'];
        ok('POST /api/family captured', !!pf && Array.isArray(pf.members));
        const keys = ['id', 'name', 'dailyGoal', 'uid'];
        // _handleFamilySave reads these keys (m["..."])
        const webSrc = web.slice(0, web.indexOf('PAGE_HTML[] PROGMEM = R'));
        const serverKeys = [...new Set([...webSrc.matchAll(/m\["(\w+)"\]/g)].map(x => x[1]))]
          .filter(k => !['role', 'points'].includes(k));   // role only for new members; points is output-only
        ok('family POST has every key the server reads (' + serverKeys.join(',') + ')',
           pf && serverKeys.every(k => k in pf.members[0]) && keys.every(k => k in pf.members[0]));

        console.log('\n' + pass + ' passed, ' + fail + ' failed');
        process.exit(fail ? 1 : 0);
      }, 20);
    }, 20);
  }, 20);
}, 60);
