// =============================================================================
// pins.h — Hardware Pin Definitions for E32R40T + RC522
// =============================================================================
// This file is the SINGLE SOURCE OF TRUTH for all GPIO assignments.
// The E32R40T has two SPI buses:
//   HSPI (bus 1): LCD + XPT2046 touch — pins 12/13/14, CS 15/33
//   VSPI (bus 2): SD card + RC522      — pins 18/19/23, CS 5/21
//
// Pin budget (ESP32-D0WD-V3 in ESP32-WROOM-32E):
//   GPIO 0       BOOT button (usable as input after boot)
//   GPIO 1,3     UART0 TX/RX (USB serial — do not reassign)
//   GPIO 2       LCD DC (data/command select)
//   GPIO 4       Audio enable (active LOW)
//   GPIO 5       SD card CS
//   GPIO 6-11    Internal flash — NEVER USE
//   GPIO 12      HSPI MISO  (LCD + touch)
//   GPIO 13      HSPI MOSI  (LCD + touch)
//   GPIO 14      HSPI CLK   (LCD + touch)
//   GPIO 15      LCD CS
//   GPIO 16      Green LED  (active LOW, common anode)
//   GPIO 17      Blue LED   (active LOW, common anode)
//   GPIO 18      VSPI CLK   (SD + SPI peripheral)
//   GPIO 19      VSPI MISO  (SD + SPI peripheral)
//   GPIO 21      SPI peripheral CS → RC522 SDA/SS
//   GPIO 22      Red LED    (active LOW, common anode)
//   GPIO 23      VSPI MOSI  (SD + SPI peripheral)
//   GPIO 25      I2C SCL connector → repurposed as RC522 RST
//   GPIO 26      Audio DAC output
//   GPIO 27      LCD backlight (HIGH = on)
//   GPIO 32      I2C SDA connector → available (RC522 IRQ or future use)
//   GPIO 33      Touch CS (XPT2046)
//   GPIO 34      Battery voltage ADC (input only)
//   GPIO 35      Expansion input (input only)
//   GPIO 36      Touch interrupt (input only)
//   GPIO 39      Expansion input (input only)
//   EN           Shared reset: ESP32 + LCD
// =============================================================================

#pragma once

// ─── LCD (ST7796 on HSPI) ────────────────────────────────────────────────────
#define PIN_LCD_CS          15
#define PIN_LCD_DC           2
#define PIN_LCD_BL          27    // HIGH = backlight on
#define PIN_LCD_RST         -1    // Tied to EN (hardware reset only)

// ─── Touch (XPT2046 on HSPI, shared CLK/MOSI/MISO with LCD) ─────────────────
#define PIN_TOUCH_CS        33
#define PIN_TOUCH_IRQ       36    // LOW when touch detected (input only)

// ─── HSPI bus (LCD + Touch) ──────────────────────────────────────────────────
#define PIN_HSPI_CLK        14
#define PIN_HSPI_MOSI       13
#define PIN_HSPI_MISO       12

// ─── VSPI bus (SD Card + RC522) ──────────────────────────────────────────────
#define PIN_VSPI_CLK        18
#define PIN_VSPI_MOSI       23
#define PIN_VSPI_MISO       19

// ─── SD Card (on VSPI) ──────────────────────────────────────────────────────
#define PIN_SD_CS            5

// ─── RFID RC522 (on VSPI, shared CLK/MOSI/MISO with SD) ─────────────────────
#define PIN_RFID_CS         21    // SPI peripheral connector CS
#define PIN_RFID_RST        25    // Repurposed from I2C SCL connector
#define PIN_RFID_IRQ        32    // Optional — repurposed from I2C SDA connector

// ─── RGB LEDs (onboard, active LOW) ──────────────────────────────────────────
// Not used by the chore station software — all feedback is on-screen.
// These GPIOs are FREE for future use (buzzer, extra sensor, etc.)
// If you want to drive them anyway, they are common-anode (LOW = on).
#define PIN_LED_RED         22    // Available
#define PIN_LED_GREEN       16    // Available
#define PIN_LED_BLUE        17    // Available

// ─── Audio ───────────────────────────────────────────────────────────────────
#define PIN_AUDIO_ENABLE     4    // LOW = amplifier enabled
#define PIN_AUDIO_DAC       26    // DAC channel 2

// ─── Buttons ─────────────────────────────────────────────────────────────────
#define PIN_BOOT_BUTTON      0    // Can use as general input after boot

// ─── Battery ─────────────────────────────────────────────────────────────────
#define PIN_BATTERY_ADC     34    // Input only, no pull-up

// ─── Expansion (input only) ──────────────────────────────────────────────────
#define PIN_EXPANSION_1     35
#define PIN_EXPANSION_2     39
