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
5. Search for "esp32" and install **esp32 by Espressif Systems** (version 2.x or 3.x)

## 3. Install Libraries

Go to **Sketch → Include Library → Manage Libraries** and install:

| Library | Author | Version |
|---------|--------|---------|
| TFT_eSPI | Bodmer | 2.5.x |
| MFRC522 | miguelbalboa | 1.4.x |
| ArduinoJson | Benoit Blanchon | 7.x |

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

## 5. Open the Sketch

1. Open Arduino IDE
2. **File → Open** → navigate to `ChoreStation/ChoreStation.ino`
3. All `.h` files in the same folder will be automatically included

## 6. Select Board Settings

Go to **Tools** and set:

| Setting | Value |
|---------|-------|
| Board | ESP32 Dev Module |
| Upload Speed | 921600 |
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
│   └── family.json    ← Created by setup wizard on first boot
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
  "dstOffset": 3600
}
```

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
| IRQ       | GPIO32    | I2C 4-pin       |
| VCC       | 3.3V      | Any power       |
| GND       | GND       | Any ground      |

## 9. Upload

1. Insert the SD card into the E32R40T's card slot
2. Connect the E32R40T via USB-C
3. Click the **Upload** button (→) in Arduino IDE
4. Wait for compilation and upload to complete
5. Open **Serial Monitor** at 115200 baud to see boot messages

## 10. First Boot

The setup wizard will appear on first boot — scan RFID tags to register family members.

## Troubleshooting

**White screen:** The TFT_eSPI User_Setup.h wasn't copied correctly. Double-check step 4.

**Touch not responding:** The XPT2046 may need calibration. Run the TFT_eSPI library's `Touch_calibrate` example first, then note the calibration values.

**SD card not detected:** Check that it's FAT32 formatted and fully inserted. The SD card shares the VSPI bus with the RC522 — both CS pins must be correctly wired.

**RFID not detected:** Check wiring. Open Serial Monitor — it should print the RC522 firmware version at boot (typically 0x92). If it shows 0x00 or 0xFF, wiring is wrong.

**WiFi won't connect:** Verify SSID and password in wifi.json are exact (case-sensitive). The station still works without WiFi — you just lose accurate time sync.
