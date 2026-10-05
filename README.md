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

See `SDCard_Contents/tasks/recurring.json` for a complete example (or just use the web editor). Key fields:

- `recurDays`: Bitmask for days of week. bit0=Sun, bit1=Mon, ..., bit6=Sat. 127=every day, 62=Mon-Fri, 64=Sat only.
- `minTime`: Minimum seconds between start and complete scan (e.g., 120 for 2-minute tooth brushing).
- `timeout`: Auto-abort after this many seconds (0 = no timeout).
- `assigned`: Family member ID (0-based, matches registration order). 255 = anyone (first child to start it claims it), 254 = every child gets their own copy.

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
1. Walk up to station, scan your tag → see your dashboard with today's tasks
2. Tap a task to see details → scan your tag to start (timer begins)
3. If you did the task without being reminded, toggle "I did this on my own" ON
4. Scan your tag to complete → bounces back to dashboard
5. If you claimed the bonus, the task goes to parent review automatically
6. Check your milestone badges by tapping the badge count in the header
7. Noticed something that needs doing? Tap the gold "+" to propose a new task (pick one from the list, or "Type it myself...")
8. **Routines** (bedtime, get-ready-for-school): tap the routine card on your dashboard, check off each step, then tap FINISH for a bundle of points — one scan, no per-step scanning. Routines light up gold during their time of day. Edit them in `/config/routines.json` on the SD card.

### For Parents
1. Scan your tag → see family overview with both kids' progress and pending approvals
2. Pending tasks appear at the top of your task list with a red badge count
3. Tap a pending task → APPROVE, OVERRIDE (remove bonus claim), or REJECT
4. Tap the teal "+" for quick ad-hoc task creation (3 taps: category, name, assign)
5. Tap the gear icon → full admin (create recurring tasks with preset or typed names, manage tasks, rename people and adjust goals, WiFi setup)

### Web editor (from your phone)
When the station is on WiFi, the home screen shows a URL like `http://192.168.1.50`. Open it in any browser on the same network to edit:
- **Chores** and **routines**, with a real keyboard: custom names, schedules, steps, points, assignees.
- **Family**: names, daily goals, and tag IDs. "Use last scan" fills in the most recent unregistered tag.

Saves write straight to the SD card and apply live (no reboot). Chores and routines are the same files you could hand-edit: `/tasks/recurring.json` and `/config/routines.json`. Existing items keep their ids when you save, so today's progress isn't disturbed.

> Security: set `webPin` in `wifi.json` to require a login; without it anyone on your WiFi (kids included) can edit chores. WiFi credentials are never shown or served. Don't port-forward the station to the internet.

### Core Rule
Any tag scan from any screen goes to that person's dashboard. No exceptions. This means twins can alternate freely — scan, do your thing, walk away. The other twin scans and sees her own dashboard instantly.

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
