# Arduino IDE Setup Guide — Family Chore Station

## 1. Install Arduino IDE

Download from https://www.arduino.cc/en/software (version 2.x recommended).

## 2. Add ESP32 Board Support

1. Open Arduino IDE
2. Go to **File → Preferences**
3. In "Additional boards manager URLs" add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
4. Go to **Tools → Board → Boards Manager**
5. Search for "esp32" and install **esp32 by Espressif Systems** (version 2.x or 3.x; tested on 3.3.10)

## 3. Install Libraries

Go to **Sketch → Include Library → Manage Libraries** and install:

| Library | Author | Version |
|---------|--------|---------|
| TFT_eSPI | Bodmer | 2.5.x (tested 2.5.43) |
| MFRC522 | miguelbalboa | 1.4.x (tested 1.4.12) |
| ArduinoJson | Benoit Blanchon | 7.x (tested 7.4.3) |

The SD library is built into the ESP32 core — no separate install needed.

## 4. Configure TFT_eSPI (CRITICAL STEP)

The TFT_eSPI library needs pin definitions set in its own config file, not in the sketch.

1. Find your TFT_eSPI library folder:
   - **Windows:** `C:\Users\<your-username>\Documents\Arduino\libraries\TFT_eSPI\`
   - **Mac:** `~/Documents/Arduino/libraries/TFT_eSPI/`
   - **Linux:** `~/Arduino/libraries/TFT_eSPI/`

2. In that folder, find `User_Setup.h`

3. **Rename** the existing `User_Setup.h` to `User_Setup.h.bak` (keep as backup)

4. **Copy** the `User_Setup.h` file from this project into the TFT_eSPI folder

5. Restart Arduino IDE

If the display shows white after flashing, this step was likely missed or the file wasn't copied to the right location.

**Re-copy it whenever this repo's `User_Setup.h` changes.** It now sets `USE_HSPI_PORT`, which keeps the display on its own SPI bus. Without it the display library grabs the bus the RC522 and SD card use, and the reader goes deaf right after boot: the display works, but tags are never detected. The sketch refuses to compile (with an `#error` pointing here) if the library copy is missing that line, so a stale copy can't sneak through again.

## 5. Open the Sketch

1. Open Arduino IDE
2. **File → Open** → navigate to `ChoreStation/ChoreStation.ino`
3. All `.h` files in the same folder will be automatically included

## 6. Select Board Settings

Go to **Tools** and set:

| Setting | Value |
|---------|-------|
| Board | ESP32 Dev Module |
| Upload Speed | 460800 (921600 fails with "chip stopped responding" on some USB adapters) |
| CPU Frequency | 240MHz (WiFi/BT) |
| Flash Frequency | 80MHz |
| Flash Mode | DIO |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | Default 4MB with spiffs |
| Port | (your USB serial port) |

## 7. Prepare SD Card

Format a MicroSD card as **FAT32** and copy the contents of the `SDCard_Contents` folder onto it:

```
SD Card Root/
├── config/
│   ├── wifi.json      ← EDIT THIS with your WiFi credentials
│   ├── routines.json  ← Example routines (edit here or in the web editor)
│   └── family.example.json ← Format reference only (see below)
├── tasks/
│   └── recurring.json ← Example tasks (edit as needed)
├── logs/              ← Created automatically
├── stats/             ← Created automatically
├── backups/           ← Created automatically
└── archive/           ← Created automatically
```

**Edit `config/wifi.json` before inserting the card:**
```json
{
  "ssid": "YourNetworkName",
  "password": "YourPassword",
  "gmtOffset": -28800,
  "dstOffset": 3600,
  "webPin": ""
}
```

`webPin` is optional: set it (e.g. `"4821"`) and the phone web editor asks for a login (user `parent`, password = the PIN). Leave it empty for no login.

Don't copy `family.example.json` as `family.json`: its tag IDs are placeholders, so no real tag would match and the setup wizard (which registers your real tags) would never run. `family.json` is created by the wizard.

PST (California) = -28800. See README.md for other timezone values.

## 8. Wire the RC522

Connect the RFID reader to the E32R40T board:

| RC522 Pin | ESP32 GPIO | Board Connector |
|-----------|-----------|-----------------|
| SDA (SS)  | GPIO21    | SPI 4-pin       |
| SCK       | GPIO18    | SPI 4-pin       |
| MOSI      | GPIO23    | SPI 4-pin       |
| MISO      | GPIO19    | SPI 4-pin       |
| RST       | GPIO25    | I2C 4-pin       |
| IRQ       | GPIO32    | I2C 4-pin (optional, unused by firmware) |
| VCC       | **3.3V**  | Any power pin (**not** 5V)  |
| GND       | GND       | Any ground (**required**)   |

Things that commonly go wrong:
- The **SPI 4-pin socket carries only the four signals** (IO21/18/19/23). It has no power or ground pins, so VCC and GND must come from another header. Without a shared ground nothing works.
- The sockets are 1.25 mm pitch. Dupont pins pushed into them make poor, intermittent contact; use a matching 1.25 mm (MX1.25 / PicoBlade-style) cable.
- **RST must be connected.** If it floats, the RC522 can sit in power-down and read as "not detected".
- The RC522 header pins usually ship unsoldered; a cold joint on MISO is the classic failure.

## 9. Upload

1. Insert the SD card into the E32R40T's card slot
2. Connect the E32R40T via USB-C
3. Click the **Upload** button (→) in Arduino IDE
4. Wait for compilation and upload to complete
5. Open **Serial Monitor** at 115200 baud to see boot messages

## 10. First Boot

1. **Touch calibration** runs once: touch each corner arrow as it appears (a stylus works best). The result is saved to `config/touch.json`.
2. The **setup wizard** appears: scan each family member's tag and pick a name. Names, goals and tags can be changed later from the phone web editor (Family tab).
3. The station restarts into normal mode. The home screen shows the web editor's address once WiFi connects.

## Recovery mode

Power the station on, then **press and hold BOOT while "Starting..." is on screen** (don't hold it while plugging in: that puts the ESP32 into its flashing mode instead). The recovery screen offers:
- **Re-pair family tags**: scan a new tag for each person, keeping names, points and streaks.
- **Calibrate touch**: press BOOT again for this if the touch screen is too far off to hit the button.

## Troubleshooting

**White screen:** The TFT_eSPI User_Setup.h wasn't copied correctly. Double-check step 4.

**Compile error "TFT_eSPI would share the RC522/SD bus":** The `User_Setup.h` inside the TFT_eSPI library folder is an old copy. Repeat step 4.

**Touches land on the wrong button:** Recalibrate from recovery mode (above), or delete `config/touch.json` from the SD card to recalibrate on the next boot.

**SD card not detected:** The station shows "No SD card" and restarts by itself once a FAT32 card is readable. The SD card shares the VSPI bus with the RC522 — both CS pins must be correctly wired.

**RFID: "Tag reader offline" on the home screen.** Open Serial Monitor at 115200; boot prints the RC522 version byte:
- `0x91` / `0x92`: genuine chip, wiring good. `0x88`, `0xB2`, `0x12`: common clones, also fine.
- `0x00`: no power, no shared GND, RST not connected, or MISO stuck low.
- `0xFF`: MISO floating (disconnected).
The firmware re-checks every 5 seconds, so it comes online by itself once wiring is fixed (no reboot needed). If it stays offline, try once with the SD card removed to rule out the shared bus.

**RFID: reader online, but nothing happens when you scan.**
- The RC522 only reads **13.56 MHz** tags (MIFARE Classic/Ultralight, NTAG21x stickers, the white card and blue fob that ship with it). It can't see **125 kHz** fobs (EM4100/HID Prox, a very common cheap "RFID key fob") or phones.
- Hold the tag flat within 1–2 cm of the antenna. Metal behind the reader (or the display PCB) kills range.
- Serial Monitor prints `[RFID] Tag detected: 04:A2:...` for every read; if that appears, the reader works.

**RFID: "Unknown tag" with an ID underneath.** The reader works but the tag isn't registered. Either open the web editor → Family → "Use last scan" on the right person → Save, or use recovery mode → Re-pair family tags.

**WiFi won't connect:** Verify SSID and password in wifi.json are exact (case-sensitive). The station still works without WiFi — you just lose accurate time sync. It retries in the background without freezing the screen.
