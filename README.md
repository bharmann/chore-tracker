# Family Chore Station

**Hardware:** E32R40T (ESP32-32E + ST7796 4" TFT) + RFID-RC522  
**Framework:** Arduino (Arduino IDE / arduino-cli; migrated off PlatformIO)  
**Project Size:** ~8,200 lines across 21 source files

## What It Does

An RFID-based household task management system that teaches children responsibility and initiative through structured chore tracking, a point economy, milestone badges, and a no-reminder bonus system.

Family members scan personal RFID tags to identify themselves, view their assigned tasks, start/complete chores, and claim bonuses. Parents review completions, approve or override bonus claims, and manage tasks.

## Hardware Wiring

### RC522 → E32R40T Connections

| RC522 Pin | ESP32 GPIO | Board Connector |
|-----------|-----------|-----------------|
| SDA (SS)  | GPIO21    | SPI 4-pin       |
| SCK       | GPIO18    | SPI 4-pin       |
| MOSI      | GPIO23    | SPI 4-pin       |
| MISO      | GPIO19    | SPI 4-pin       |
| RST       | GPIO25    | I2C 4-pin       |
| IRQ       | GPIO32    | I2C 4-pin (optional) |
| VCC       | **3.3V**  | Any power pin (**not** 5V) |
| GND       | GND       | Any ground (**required**)  |

The RC522 shares the VSPI bus with the SD card. Both have separate CS pins so they coexist without conflict. The display and touch have their own bus (HSPI); `User_Setup.h` must keep `USE_HSPI_PORT` or the display library takes over the RC522's bus and tags stop being detected after boot.

The SPI 4-pin socket has **no power or ground pins**, so take 3.3V and GND from another header. The RC522 reads **13.56 MHz** tags only (MIFARE / NTAG); 125 kHz fobs won't register. See the RFID troubleshooting in `ARDUINO_IDE_SETUP.md`.

## SD Card Setup

Format a MicroSD card as FAT32 and create this structure:

```
/config/
  wifi.json       ← Your WiFi credentials + timezone (+ optional webPin)
  family.json     ← Created by setup wizard on first boot (don't pre-copy one)
  routines.json   ← Routine checklists (also editable from the web editor)
  touch.json      ← Auto-created by touch calibration
/tasks/
  recurring.json  ← Your recurring task templates
/logs/            ← Auto-created, daily completion logs
/stats/           ← Auto-created
/backups/         ← Auto-created, daily snapshots
/archive/         ← Auto-created, monthly log summaries
```

### wifi.json (edit BEFORE inserting SD card)

Open `/config/wifi.json` in any text editor and replace the placeholder values with your actual WiFi network name and password:

```json
{
  "ssid": "YourWiFiNetwork",
  "password": "YourWiFiPassword",
  "gmtOffset": -28800,
  "dstOffset": 3600,
  "webPin": ""
}
```

**Fields:**

- `ssid` — Your WiFi network name (case-sensitive, exact match)
- `password` — Your WiFi password (supports WPA2-PSK)
- `gmtOffset` — Seconds from UTC for your timezone (see table below)
- `dstOffset` — Usually 3600 (1 hour) if your area uses daylight saving, 0 if not
- `webPin` — Optional. If set, the phone web editor asks for a login (user `parent`, password = this PIN)

**Common timezone offsets:**

| Timezone | gmtOffset | Region |
|----------|-----------|--------|
| PST (UTC-8) | -28800 | California, Pacific NW |
| MST (UTC-7) | -25200 | Arizona, Mountain |
| CST (UTC-6) | -21600 | Texas, Midwest |
| EST (UTC-5) | -18000 | New York, East Coast |
| UTC | 0 | UK (winter) |
| CET (UTC+1) | 3600 | Central Europe |
| JST (UTC+9) | 32400 | Japan |

If your WiFi password or network changes later, pull the SD card, edit the file, and reinsert. The station picks up the new credentials on next boot.

**If WiFi is unavailable:** The station still works — tasks, scanning, points, and milestones all function normally. You lose NTP time sync (trajectory defaults to a noon estimate) and daily reset will be based on uptime rather than actual midnight. WiFi is purely for time accuracy.

### recurring.json (your task templates)

See `SDCard_Contents/tasks/recurring.json` for a complete example (or just use the web editor, which shows times in minutes). The examples are the family's own list: shared chores (`assigned` 254 = each child, 255 = whoever starts it), all Small except yard work, with brushing teeth (AM/PM, 2-minute minimum, one per child) and yard work (30-minute minimum, weekends) as the timed chores. Teeth-brushing is a chore, so the example routines leave it out. To swap them onto a card that already has older chores, no card reader needed: web editor → Chores → **Replace all chores with the examples**. (Or copy the new `recurring.json` to `/tasks/` by hand.) Key fields:

- `recurDays`: Bitmask for days of week. bit0=Sun, bit1=Mon, ..., bit6=Sat. 127=every day, 62=Mon-Fri, 64=Sat only.
- `minTime`: Minimum seconds between start and complete scan (e.g., 120 for 2-minute tooth brushing).
- `timeout`: Auto-abort after this many seconds (0 = no timeout).
- `assigned`: Family member ID (0-based, matches registration order). 255 = **First taker** (one copy; whoever starts it takes it), 254 = **Per child** (every child gets their own copy). Routines use the same numbers: 254 = Per child, 255 = Everyone (parents too).
- `recurHour`: The chore appears once this hour starts (checked every 5 minutes). `recurMin` is stored but not used yet.
- `expires`: What happens to an unfinished copy at midnight. `false` (default) = it carries over until done, and the next day's copy waits for it. `true` = it disappears and the next day starts fresh (the examples use this for brushing teeth and setting/clearing the table). Either way, a chore that was finished but is still waiting for approval stays for the parent, and the next day's copy still appears.

## First Boot

1. Insert SD card with `wifi.json` and `recurring.json`
2. Connect E32R40T via USB-C
3. Flash via Arduino IDE: open `ChoreStation/ChoreStation.ino` and click Upload (see `ARDUINO_IDE_SETUP.md`)
4. Touch calibration (once): tap each corner arrow as it appears
5. The setup wizard appears:
   - **WiFi setup** (optional): pick your network, type the password on the on-screen keyboard
   - Scan parent tag(s) → pick or type names
   - Scan child tag(s) → pick or type names
   - Review and save
   - Device reboots into normal mode

**Lost or new tags, new WiFi, touch way off, or want to start over?** Press and hold the BOOT button while "Starting..." shows to open **recovery mode** (works with just the BOOT button: press = next, hold = select): re-pair tags (keeps names and points), WiFi setup, recalibrate touch, or start setup over. Admin → Family Settings → "Re-pair tags" does the same from a parent's dashboard. An unregistered tag shows "Unknown tag" plus its ID, and the web editor's Family tab can assign it with one tap.

## Daily Operation

### For Kids
1. Walk up to station, scan your tag. If a parent checked your chores (or a chore was sent back, or ran out of time) since last time, a **While you were away** card shows that first. Then your dashboard:
   - top: your name and **point total**; the badge row ("12 to Rising Star" with a bar — tap it for the badge collection); today's goal as segments (filled = done, outlined = waiting for a parent) and your streak ("2 days to +3")
   - the list: whatever needs you comes first (a chore you've started, routines due now, chores to do), then chores waiting for a parent, finished routines and a "Done today" summary. Each card shows what it's worth ("+4")
2. **One-tap chores:** tap the green check on the card. (Want the "I did this on my own" +1? Tap the card instead, turn it on, then **DONE!**.)
3. **Timed chores** (a minimum time or a time limit, like brushing teeth or yard work): tap **START** on the card, go do it, then scan again — the station asks **Done with …?** straight away. Tap **YES, DONE!** (or **Not yet**). How long it took is shown to the parent. Started the wrong one? Tap it and **Stop - put it back**. If time runs out it goes back on the list and you're told next scan.
4. Earning points shows a big "+4", your new total and the badge bar filling up; a new badge gets its own card and fanfare
5. More chores than fit? **More >** / **< Back** right under the list
6. Noticed something that needed doing? Tap **+ Job**: pick a category, a name (or "Type my own..."), the size. On the last screen, "This was my own idea" adds the +2 initiative bonus (off by default). **I DID IT!** sends it to a parent right away; **Do it later** just adds it to your list
7. **Routines** (bedtime, get-ready-for-school): tap the routine card and check off each step — checking the last one finishes it. One scan, no per-step scanning. Checked steps are remembered for the day. Routines light up gold during their time of day. Edit them in the web editor (or `/config/routines.json`)
8. Done? Tap **Bye** (bottom-left).

### For Parents
1. The home screen shows an amber **"2 to check: Ava 1 Mia 1"** bar when chores are waiting for you
2. Scan your tag → each child's day, then the list headed **"2 TO CHECK"**: waiting chores first, then in progress, then the rest
3. A waiting chore with no bonus claim (and not timed) has an amber **OK** on its card — one tap approves it. Others: tap the card → **APPROVE**, **Approve, no bonus** (if they claimed "did it on my own" but you reminded them) or **Redo** (tap twice; the child is told next scan)
4. **+ Job** (bottom-right) = Quick Add: category, name or "Type my own...", size, and who (a child, **Per child** = one each, or **First taker**)
5. Tap the gear icon → full admin (create recurring tasks with preset or typed names, manage tasks, rename people and adjust goals, WiFi setup)

### Web editor (from your phone)
When the station is on WiFi, the home screen shows a URL like `http://192.168.1.50`. Open it in any browser on the same network to edit:
- **Chores** and **routines**, with a real keyboard: custom names, schedules, steps, points, assignees.
- **Family**: names, daily goals, and tag IDs. "Use last scan" fills in the most recent unregistered tag.

Saves write straight to the SD card and apply live (no reboot): today's not-yet-started copies of a chore are updated to match its edit, or removed if the chore was deleted; started and waiting-for-approval chores are left alone. Chores and routines are the same files you could hand-edit: `/tasks/recurring.json` and `/config/routines.json`. Existing items keep their ids when you save, so today's progress isn't disturbed.

> Security: set `webPin` in `wifi.json` to require a login; without it anyone on your WiFi (kids included) can edit chores. WiFi credentials are never shown or served. Don't port-forward the station to the internet.

### Locking
Every dashboard has a **Bye** button (padlock, bottom-left) that goes straight back to the "Scan your tag" screen, so nobody else can use a dashboard that was left open. It also locks by itself: dashboards and badges after 60 seconds without a touch; routines after 3 minutes (steps are done away from the screen); admin, adding a job and tag pairing after 90 seconds.

### Core Rule
Any tag scan from any screen goes to that person's dashboard. No exceptions. This means twins can alternate freely — scan, do your thing, walk away. The other twin scans and sees her own dashboard instantly.

## Firmware Updates (over WiFi, from GitHub releases)

Once a station runs firmware with update support (flash that one by USB), later versions install over WiFi:

1. **Publish:** on GitHub (the mobile app works too) open Actions → *Release* → **Run workflow** and type the version, e.g. `v1.4.0`; it releases the latest `main`. Or push a tag: `git tag v1.4.0 && git push origin v1.4.0`. The *Release* workflow builds the firmware and creates a GitHub release with `ChoreStation.bin`, `version.json` (version, size, SHA-256) and `ChoreStation-full.bin` (for a USB flash at 0x0). Use a higher number every time (v1.4.0 → v1.4.1 → v1.5.0).
2. **Notice:** the station checks the latest release every 6 hours while idle (and at Admin → System Info → *Check for update*). When a newer one exists the parent dashboard's gear says **Update!** and System Info shows "(v1.4.1 ready)".
3. **Install (a parent):** Admin → System Info → **Install update** → tap again to confirm, or the web editor's Family tab → *Install*. The station saves everything, shows progress, checks the download's size and SHA-256, and restarts. Chores, points and settings (all on the SD card) are kept.
4. **Safety net:** the new firmware has to run normally for a minute (SD card readable) before it's kept. If it crashes, is unplugged in that minute, or can't read the card, the station goes back to the previous firmware by itself. A failed download leaves the running firmware untouched.

Builds from the Arduino IDE report version `dev`, which counts as older than any release.

## Architecture

| File | Lines | Purpose |
|------|-------|---------|
| `ChoreStation.ino` | 1,871 | Boot, main loop, all screens, scan/touch routing, tag pairing, recovery |
| `parent_admin.h` | 1,133 | Task creation wizard, task management, settings, system info |
| `setup_wizard.h` | 563 | First-run family registration flow |
| `backup_manager.h` | 551 | Write coalescing, shadow copies, daily backups, log archiving |
| `storage_manager.h` | 460 | SD card JSON persistence (with .bak fallback), touch calibration, WiFi credentials |
| `web_server.h` | 420 | Phone web editor: chores, routines, family + tags |
| `task_engine.h` | 380 | Task state machine, point calculator, completion logging |
| `milestones.h` | 328 | 16 collectible badge definitions + collection screen |
| `icons.h` | 314 | On-screen icons drawn via TFT primitives |
| `time_manager.h` | 304 | WiFi + NTP time sync (non-blocking after boot) |
| `scheduler.h` | 280 | Recurring task generation + daily reset + streak evaluation |
| `wifi_setup.h` | 241 | On-device WiFi setup: scan, pick, type password |
| `keyboard.h` | 232 | On-screen keyboard (slide-to-select, types on lift) |
| `display_power.h` | 220 | PWM backlight dimming, sleep, anti-burn-in pixel shift |
| `data_model.h` | 208 | All structs and enums |
| `rfid_manager.h` | 175 | RC522 reader: debounce, health check + auto-recovery |
| `keyboard_layout.h` | 143 | Keyboard geometry/content, unit-tested on a PC |
| `routine_manager.h` | 139 | Routine checklists (bedtime / school mornings) |
| `pins.h` | 89 | GPIO pin assignments (single source of truth) |
| `audio_feedback.h` | 78 | DAC tone generation for scan/complete/error |
| `preset_names.h` | 23 | Shared task name presets (6 per category) |
| `User_Setup.h` | — | TFT_eSPI pin/driver config (copy into the library folder) |

Host-side tests: `test/web_ui_contract.test.js` (jsdom) checks the web editor against the firmware's JSON loaders, and `test/keyboard_layout_test.cpp` checks the on-screen keyboard layout (every character typeable, keys on screen, no overlaps, exact hit-testing). CI (`.github/workflows/build.yml`) compiles the firmware and runs both on every push.

## Key Design Decisions

- **Scan = Dashboard, always.** Any tag scan from any screen immediately shows that person's dashboard. No conditional interpretation. Twins alternate freely.
- **Task state is self-contained.** Timer, no-reminder toggle, claim status, approval — all live on the Task struct, not as global UI variables. Multiple tasks can be in-progress simultaneously.
- **No-reminder is opt-in by the child** via a toggle on the task detail screen. Default is OFF. Claiming the bonus always forces parent review.
- **Initiative and no-reminder bonuses don't stack** on self-initiated tasks. Initiative (+2) subsumes no-reminder (+1).
- **Parent override** accepts the task work but rejects the bonus claim. Logged for audit trail.
- **Parent approves from their own dashboard** — pending tasks sorted to top with badge count.
- **Trajectory shows ahead/on-track/behind** rather than raw point totals to avoid sibling competition.
- **Screen-only feedback** — onboard RGB LEDs freed up (GPIO16/17/22 available).
- **Non-blocking messages** — all action feedback uses timed overlays, never blocking delays.
- **Backlight dims after 15s, sleeps after 2min**, with pixel shifting to prevent image persistence.
- **Write coalescing** batches SD card writes. Shadow copies protect against power-loss corruption.
- **Paged task lists** — page dots at bottom, tap to navigate. No scrolling.

## What's Not Built Yet

- **Voice input** — would need an I2S MEMS microphone module (INMP441)
- **Notifications / remote dashboard** — the web editor covers LAN editing, but nothing pushes "task waiting for approval" to a phone yet
- **Collaborative tasks** — joint tasks where both children work together on one item
