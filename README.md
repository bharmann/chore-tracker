# Family Chore Station

**Hardware:** E32R40T (ESP32-32E + ST7796 4" TFT) + RFID-RC522  
**Framework:** Arduino via PlatformIO  
**Project Size:** ~4,660 lines across 16 source files

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
| VCC       | 3.3V      | Any power pin   |
| GND       | GND       | Any ground      |

The RC522 shares the VSPI bus with the SD card. Both have separate CS pins so they coexist without conflict.

## SD Card Setup

Format a MicroSD card as FAT32 and create this structure:

```
/config/
  wifi.json       ← Your WiFi credentials + timezone
  family.json     ← Created by setup wizard on first boot
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
  "dstOffset": 3600
}
```

**Fields:**

- `ssid` — Your WiFi network name (case-sensitive, exact match)
- `password` — Your WiFi password (supports WPA2-PSK)
- `gmtOffset` — Seconds from UTC for your timezone (see table below)
- `dstOffset` — Usually 3600 (1 hour) if your area uses daylight saving, 0 if not

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

See `data/tasks/recurring.json` for a complete example. Key fields:

- `recurDays`: Bitmask for days of week. bit0=Sun, bit1=Mon, ..., bit6=Sat. 127=every day, 62=Mon-Fri, 64=Sat only.
- `minTime`: Minimum seconds between start and complete scan (e.g., 120 for 2-minute tooth brushing).
- `timeout`: Auto-abort after this many seconds (0 = no timeout).
- `assigned`: Family member ID (0-based, matches registration order). 255 = unassigned.

## First Boot

1. Insert SD card with `wifi.json` and `recurring.json`
2. Connect E32R40T via USB-C
3. Flash via Arduino IDE: open `ChoreStation/ChoreStation.ino` and click Upload (see `ARDUINO_IDE_SETUP.md`)
4. The setup wizard appears:
   - Scan parent tag(s) → pick names
   - Scan child tag(s) → pick names
   - Review and save
   - Device reboots into normal mode

## Daily Operation

### For Kids
1. Walk up to station, scan your tag → see your dashboard with today's tasks
2. Tap a task to see details → scan your tag to start (timer begins)
3. If you did the task without being reminded, toggle "I did this on my own" ON
4. Scan your tag to complete → bounces back to dashboard
5. If you claimed the bonus, the task goes to parent review automatically
6. Check your milestone badges by tapping the badge count in the header
7. Noticed something that needs doing? Tap the gold "+" to propose a new task
8. **Routines** (bedtime, get-ready-for-school): tap the routine card on your dashboard, check off each step, then tap FINISH for a bundle of points — one scan, no per-step scanning. Routines light up gold during their time of day. Edit them in `/config/routines.json` on the SD card.

### For Parents
1. Scan your tag → see family overview with both kids' progress and pending approvals
2. Pending tasks appear at the top of your task list with a red badge count
3. Tap a pending task → APPROVE, OVERRIDE (remove bonus claim), or REJECT
4. Tap the teal "+" for quick ad-hoc task creation (3 taps: category, name, assign)
5. Tap the gear icon → full admin (create recurring tasks, manage tasks, adjust goals)

### Core Rule
Any tag scan from any screen goes to that person's dashboard. No exceptions. This means twins can alternate freely — scan, do your thing, walk away. The other twin scans and sees her own dashboard instantly.

## Architecture

| File | Lines | Purpose |
|------|-------|---------|
| `ChoreStation.ino` | 1,246 | Boot, main loop, all screens, scan/touch routing, actions |
| `parent_admin.h` | 920 | Task creation wizard, task management, settings, system info |
| `backup_manager.h` | 548 | Write coalescing, shadow copies, daily backups, log archiving |
| `setup_wizard.h` | 497 | First-run family registration flow |
| `task_engine.h` | 378 | Task state machine, point calculator, completion logging |
| `milestones.h` | 328 | 16 collectible badge definitions + collection screen |
| `icons.h` | 314 | On-screen icons drawn via TFT primitives |
| `storage_manager.h` | 290 | SD card JSON persistence layer |
| `scheduler.h` | 253 | Recurring task generation + daily reset + streak evaluation |
| `time_manager.h` | 243 | WiFi + NTP time sync |
| `display_power.h` | 210 | PWM backlight dimming, sleep, anti-burn-in pixel shift |
| `data_model.h` | 181 | All structs and enums |
| `rfid_manager.h` | 119 | RC522 RFID reader with debounce |
| `pins.h` | 89 | GPIO pin assignments (single source of truth) |
| `audio_feedback.h` | 78 | DAC tone generation for scan/complete/error |
| `User_Setup.h` | — | TFT_eSPI pin/driver config (copy into the library folder) |
| `preset_names.h` | 23 | Shared task name presets (6 per category) |

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

- **Touch calibration** — XPT2046 needs per-unit calibration values (run TFT_eSPI calibration example)
- **Custom task names** — preset lists cover common tasks; free-text would need on-screen keyboard
- **Voice input** — would need an I2S MEMS microphone module (INMP441)
- **Mobile companion app** — for remote task creation, notifications, dashboard
- **Collaborative tasks** — joint tasks where both children work together on one item
- **On-device WiFi picker** — credentials edited in wifi.json on SD card before first boot
