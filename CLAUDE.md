# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

ESP32 firmware for an RFID chore station. Hardware: **E32R40T** (ESP32-WROOM-32E + ST7796 4" TFT + XPT2046 touch) plus an **RC522** RFID reader. ~5,700 lines of C++ across `ChoreStation/*.h` and one `.ino`. Family members scan RFID tags to see tasks, start/complete chores, earn points, and claim a no-reminder bonus that parents approve.

## Build & Flash

This project **migrated off PlatformIO to the Arduino IDE** for easier maintenance — the flat `ChoreStation/*.h` + `.ino` layout is the Arduino sketch structure, and there is intentionally **no `platformio.ini`**. Do not reintroduce PlatformIO layout (`src/`, `platformio.ini`, `lib_deps`). Build is the **Arduino IDE** flow (`ARDUINO_IDE_SETUP.md`):

- Board: **ESP32 Dev Module**, 240MHz, 4MB flash, Default 4MB w/ spiffs, upload 921600.
- Libraries: **TFT_eSPI** (Bodmer 2.5.x), **MFRC522** (miguelbalboa 1.4.x), **ArduinoJson** (7.x). SD, WiFi, and **WebServer** (the parent web UI) are all in the ESP32 core — no extra library for the web server (deliberately not AsyncWebServer, to avoid new deps).
- **Critical:** copy this repo's `User_Setup.h` over the one inside the installed TFT_eSPI library folder. Pin/driver defines live there, NOT in the sketch. White screen after flash = this step was skipped.
- Serial Monitor at **115200** for boot logs. RC522 firmware version prints at boot (0x92 good; 0x00/0xFF = wiring wrong).
- CLI equivalent: `arduino-cli compile --fqbn esp32:esp32:esp32 ChoreStation` then `... upload -p <port>`. No CI, no tests — verification is on-device.

## Architecture

Header-only modules. `ChoreStation.ino` includes every `.h` and owns all globals + UI. Each `.h` is a self-contained class (`RFIDManager`, `StorageManager`, `TaskEngine`, `Scheduler`, `BackupManager`, `TimeManager`, `DisplayPower`, `SetupWizard`, `ParentAdmin`, `AudioFeedback`, `RoutineManager`, `WebUI`). No `.cpp` files.

- **Web UI** (`web_server.h`) runs an ESP32-core `WebServer` on port 80, reachable once WiFi is up (the home screen prints the URL). It serves a one-page editor (HTML/JS in PROGMEM) plus a JSON API that reads/writes the SAME SD files the firmware uses — `GET/POST /api/routines` ↔ `/config/routines.json`, `GET/POST /api/tasks` ↔ `/tasks/recurring.json`, `GET /api/family` for assignee names. Saves are validated by `StorageManager::writeRawJson` (parse-check then remove-and-overwrite) and raise `web.pendingTaskReload` / `web.pendingRoutineReload`; the main loop consumes those and calls `scheduler.begin()` / `routines.begin()` so edits apply without a reboot. No auth — trusted-LAN tool, same threat model as the SD card; WiFi creds are never served. The page's field↔JSON contract is regression-tested host-side with jsdom (see scratchpad test), asserting emitted keys match the storage loaders.

- **Routines** (`routine_manager.h`) are ordered checklists done under ONE scan (bedtime, get-ready-for-school). Scan → dashboard → tap the routine card → check off each step on-screen → flat bundle of points on full completion. No per-step RFID scan. Templates in `/config/routines.json`; per-day completion is a `doneMask` (bit per member) persisted to `/stats/routines.json` and cleared at the day rollover by `Scheduler::performDailyReset`. Surfaced as dashboard cards for children (highlighted gold during their `[startHour,endHour]` window, which may wrap midnight). Offline (no NTP), only daily (`recurDays==127`) routines surface. Dashboard rows (routines + tasks) are built by one shared `buildDashRows()` used by both draw and touch so taps always match drawn cards.

- **`pins.h` is the single source of truth for GPIO.** Two SPI buses: HSPI = LCD + touch (12/13/14); VSPI = SD card + RC522 (18/19/23), sharing the bus with separate CS pins (SD=5, RFID=21). Never touch GPIO 6-11 (flash).
- **`data_model.h` holds every struct/enum**, kept flat for ESP32 SRAM. `PointTable` encodes the whole point economy (base points by size, own-mess reduction, initiative/no-reminder/streak bonuses). `computeTrajectory()` derives ahead/on-track/behind from time-of-day, never stored.
- **Main loop (`loop()` in the .ino)** is a non-blocking pump: poll RFID → poll touch → `display.update()` power state → per-task timeout checks → message-overlay expiry → auto-return/idle timers → background services (`backups.update`, `timeMan.update`, `scheduler.update`). Ends with `delay(10)`. No blocking waits anywhere.
- **Screen dispatch** is a `Screen` enum + `switch` in `handleTouch()`; each screen has a `draw*` and `touch*` function. Redraws are explicit calls, not a render loop.

## Invariants (violating these breaks the UX contract — see the .ino header)

- **Scan = Dashboard, always.** Any tag scan from any screen jumps to that person's dashboard. No conditional interpretation.
- **Task state is self-contained on the `Task` struct** (timer, `noReminderToggled`, claim, approval) — never global UI vars. Multiple tasks run in-progress at once.
- **No-reminder is opt-in by the child** (default off) and always forces parent review. Initiative (+2) subsumes no-reminder (+1); they don't stack.
- **Parents approve only from the parent dashboard.** Child task-detail never shows approval buttons. Parent override accepts the work but rejects the bonus claim, logged for audit.
- **All feedback is on-screen + DAC audio.** Onboard RGB LEDs (GPIO 16/17/22) are deliberately unused and free.

## Persistence (SD card, FAT32)

`StorageManager` reads/writes JSON; `BackupManager` sits in front of it with **write coalescing** — call `markTasksDirty()` / `markFamilyDirty()`, and `backups.update()` flushes on an interval or when dirty data ages out (never per-change). It writes via **shadow copies** (`_shadowCopy`) so power loss mid-write can't corrupt files, and `loadWithFallback` recovers from the shadow. Daily backups + monthly archives are auto-created.

SD layout (`SDCard_Contents/` is the seed): `/config/wifi.json` (WiFi + timezone, hand-edited before first boot), `/config/family.json` (written by setup wizard), `/config/routines.json` (routine checklists, hand-edited), `/tasks/recurring.json` (task templates), plus auto-created `/logs /stats /backups /archive`. `/stats/routines.json` holds today's routine completion masks.

`routines.json` fields: `assigned` (member id, 254=all children, 255=anyone), `points` (flat bundle), `startHour`/`endHour` (surfacing window, wraps midnight if start>end), `recurDays` (same bitmask as tasks), `steps` (array of ≤10 short strings).

`recurring.json` fields: `recurDays` bitmask (bit0=Sun..bit6=Sat; 127=daily, 62=Mon-Fri), `minTime` (min seconds start→complete), `timeout` (auto-abort seconds, 0=off), `assigned` (member id, 255=unassigned). WiFi is optional — without it everything works except NTP time sync.

## Not Yet Built

Touch calibration (per-unit XPT2046 values), on-device custom/free-text task names (the web UI now covers custom names from a phone), voice input, native mobile companion app (the on-device web UI covers LAN editing), collaborative tasks, on-device WiFi picker.
