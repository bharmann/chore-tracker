// =============================================================================
// User_Setup.h — TFT_eSPI Configuration for E32R40T
// =============================================================================
//
// INSTALLATION:
//   1. Find your TFT_eSPI library folder:
//      - Windows: C:\Users\<you>\Documents\Arduino\libraries\TFT_eSPI\
//      - Mac: ~/Documents/Arduino/libraries/TFT_eSPI/
//      - Linux: ~/Arduino/libraries/TFT_eSPI/
//   2. RENAME the existing User_Setup.h to User_Setup.h.bak (backup)
//   3. COPY this file into the TFT_eSPI folder as User_Setup.h
//   4. Restart Arduino IDE
//
// This configures TFT_eSPI for the E32R40T module:
//   - ESP32-WROOM-32E processor
//   - ST7796 4" TFT (320x480) on HSPI bus
//   - XPT2046 resistive touch on same HSPI bus
//   - Backlight on GPIO27 (active HIGH)
// =============================================================================

// ─── Display Driver ──────────────────────────────────────────────────────────
#define ST7796_DRIVER

#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// ─── SPI Port ────────────────────────────────────────────────────────────────
// REQUIRED. Without this, TFT_eSPI creates its own SPIClass on the VSPI
// peripheral — the bus the RC522 and SD card use. tft.init() then re-routes
// VSPI's MISO input to GPIO12, so after boot the RC522 reads garbage (no tags
// ever detected) and SD reads fail, even though the display looks fine.
// ChoreStation.ino refuses to compile if this is missing.
#define USE_HSPI_PORT

// ─── SPI Pins (HSPI bus — dedicated to LCD + touch) ─────────────────────────
#define TFT_MISO  12
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_CS    15    // LCD chip select
#define TFT_DC     2    // Data/command select
#define TFT_RST   -1    // RST tied to ESP32 EN pin (hardware reset only)

// ─── Touch (XPT2046 on same HSPI bus) ────────────────────────────────────────
#define TOUCH_CS  33

// ─── Backlight ───────────────────────────────────────────────────────────────
#define TFT_BL    27
#define TFT_BACKLIGHT_ON HIGH

// ─── SPI Frequencies ─────────────────────────────────────────────────────────
#define SPI_FREQUENCY       40000000   // 40 MHz — max for ST7796 over SPI
#define SPI_READ_FREQUENCY  20000000   // 20 MHz for read operations
#define SPI_TOUCH_FREQUENCY  2500000   // 2.5 MHz for XPT2046 touch

// ─── Font Loading ────────────────────────────────────────────────────────────
#define LOAD_GLCD    // Original Adafruit 8 pixel font (needed for basic text)
#define LOAD_FONT2   // Small 16 pixel high font
#define LOAD_FONT4   // Medium 26 pixel high font
#define LOAD_FONT6   // Large 48 pixel (digits only)
#define LOAD_FONT7   // 7 segment 48 pixel (digits only)
#define LOAD_FONT8   // Large 75 pixel font (digits only)
#define LOAD_GFXFF   // FreeFonts
#define SMOOTH_FONT
