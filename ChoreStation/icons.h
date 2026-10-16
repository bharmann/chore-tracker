// =============================================================================
// icons.h — On-Screen Icon System (replaces physical LEDs)
// =============================================================================
// All icons drawn with TFT_eSPI primitives — no bitmaps needed.
// Designed for the 320×480 ST7796 at viewable sizes (16px–48px).
//
// Icon families:
//   Category icons  — one per task category (broom, heart, hands, book, paw, tree)
//   State icons     — task lifecycle (circle, play, hourglass, check, X)
//   Trajectory icon — arrow up / right / down with color
//   Bonus icons     — initiative star, silent bell, fire streak
//   Milestone icons — collectible badges earned at point thresholds
//   UI icons        — RFID tap, back arrow, gear, plus
// =============================================================================

#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "data_model.h"

// Forward-declare the global tft so icons can draw directly
extern TFT_eSPI tft;

namespace Icons {

// ─── Sizing ──────────────────────────────────────────────────────────────────
// Icons are drawn at a given (x,y) top-left with a size parameter.
// "sm" = 16px, "md" = 24px, "lg" = 32px, "xl" = 48px

// ─── Category Icons ──────────────────────────────────────────────────────────

inline void drawCategoryIcon(int16_t x, int16_t y, TaskCategory cat, uint16_t color, uint8_t size = 24) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 2;

    switch (cat) {
        case TaskCategory::CLEANING_TIDYING:
            // Broom / sparkle — diamond with rays
            tft.drawLine(cx, cy - r, cx, cy + r, color);         // Vertical shaft
            tft.drawLine(cx - r/2, cy + r/3, cx + r/2, cy + r/3, color); // Cross bar
            tft.drawLine(cx - r/3, cy + r/3, cx - r/2, cy + r, color);   // Left bristle
            tft.drawLine(cx + r/3, cy + r/3, cx + r/2, cy + r, color);   // Right bristle
            tft.drawLine(cx, cy + r/3, cx, cy + r, color);               // Center bristle
            // Sparkle
            tft.fillCircle(cx + r/2 + 2, cy - r/2, 2, color);
            break;

        case TaskCategory::HEALTH_HYGIENE:
            // Heart
            tft.fillCircle(cx - r/3, cy - r/4, r/3, color);
            tft.fillCircle(cx + r/3, cy - r/4, r/3, color);
            tft.fillTriangle(cx - r + 2, cy - r/6, cx + r - 2, cy - r/6,
                             cx, cy + r - 1, color);
            break;

        case TaskCategory::FAMILY_SUPPORT:
            // Two people / handshake — simplified as two overlapping circles + base
            tft.fillCircle(cx - r/3, cy - r/3, r/4, color);  // Head 1
            tft.fillCircle(cx + r/3, cy - r/3, r/4, color);  // Head 2
            tft.fillRoundRect(cx - r + 2, cy, r * 2 - 4, r/2, 3, color); // Shared body
            break;

        case TaskCategory::ACADEMIC_LEARNING:
            // Open book
            tft.drawLine(cx, cy - r/2, cx, cy + r/2, color);  // Spine
            // Left page
            tft.drawLine(cx, cy - r/2, cx - r + 2, cy - r/3, color);
            tft.drawLine(cx - r + 2, cy - r/3, cx - r + 2, cy + r/3, color);
            tft.drawLine(cx - r + 2, cy + r/3, cx, cy + r/2, color);
            // Right page
            tft.drawLine(cx, cy - r/2, cx + r - 2, cy - r/3, color);
            tft.drawLine(cx + r - 2, cy - r/3, cx + r - 2, cy + r/3, color);
            tft.drawLine(cx + r - 2, cy + r/3, cx, cy + r/2, color);
            break;

        case TaskCategory::PET_CARE:
            // Paw print
            tft.fillCircle(cx, cy + r/4, r/3, color);           // Pad
            tft.fillCircle(cx - r/3, cy - r/4, r/5, color);     // Toe 1
            tft.fillCircle(cx + r/3, cy - r/4, r/5, color);     // Toe 2
            tft.fillCircle(cx - r/6, cy - r/2, r/5, color);     // Toe 3
            tft.fillCircle(cx + r/6, cy - r/2, r/5, color);     // Toe 4
            break;

        case TaskCategory::OUTDOOR_MAINTENANCE:
            // Tree
            tft.fillTriangle(cx, cy - r, cx - r/2, cy + r/4, cx + r/2, cy + r/4, color);
            tft.fillRect(cx - r/6, cy + r/4, r/3, r/2, color);  // Trunk
            break;

        default:
            tft.drawRoundRect(x + 2, y + 2, size - 4, size - 4, 4, color);
            break;
    }
}

// ─── Task State Icons ────────────────────────────────────────────────────────

inline void drawStateIcon(int16_t x, int16_t y, TaskState state, uint8_t size = 20) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 2;

    switch (state) {
        case TaskState::AVAILABLE:
            // Open circle (ready)
            tft.drawCircle(cx, cy, r, 0x4A69);  // Gray circle
            break;

        case TaskState::IN_PROGRESS:
            // Play triangle (active, pulsing amber)
            tft.fillTriangle(cx - r/3, cy - r/2, cx - r/3, cy + r/2,
                             cx + r/2, cy, 0xFDA0);
            break;

        case TaskState::PENDING_REVIEW:
            // Hourglass / eye (needs parent)
            tft.drawCircle(cx, cy, r, 0x2C9F);
            tft.fillCircle(cx, cy, r/3, 0x2C9F);
            break;

        case TaskState::COMPLETED:
            // Checkmark in circle
            tft.fillCircle(cx, cy, r, 0x2E8B);
            tft.drawLine(cx - r/3, cy, cx - r/8, cy + r/3, 0xFFFF);
            tft.drawLine(cx - r/8, cy + r/3, cx + r/3, cy - r/3, 0xFFFF);
            break;

        case TaskState::TIMED_OUT:
            // X in circle
            tft.fillCircle(cx, cy, r, 0xF800);
            tft.drawLine(cx - r/3, cy - r/3, cx + r/3, cy + r/3, 0xFFFF);
            tft.drawLine(cx + r/3, cy - r/3, cx - r/3, cy + r/3, 0xFFFF);
            break;
    }
}

// ─── Trajectory Icon ─────────────────────────────────────────────────────────

inline void drawTrajectoryIcon(int16_t x, int16_t y, Trajectory traj, uint8_t size = 32) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 2;

    uint16_t color;
    switch (traj) {
        case Trajectory::AHEAD:    color = 0x2E8B; break; // Green
        case Trajectory::ON_TRACK: color = 0x2C9F; break; // Teal
        case Trajectory::BEHIND:   color = 0xFDA0; break; // Amber
    }

    // Background circle
    tft.fillCircle(cx, cy, r, color);

    // Arrow direction
    uint16_t white = 0xFFFF;
    switch (traj) {
        case Trajectory::AHEAD:
            // Up arrow
            tft.fillTriangle(cx, cy - r/2, cx - r/3, cy, cx + r/3, cy, white);
            tft.fillRect(cx - r/6, cy, r/3, r/3, white);
            break;
        case Trajectory::ON_TRACK:
            // Right arrow (steady)
            tft.fillTriangle(cx + r/2, cy, cx, cy - r/3, cx, cy + r/3, white);
            tft.fillRect(cx - r/3, cy - r/6, r/2, r/3, white);
            break;
        case Trajectory::BEHIND:
            // Down arrow
            tft.fillTriangle(cx, cy + r/2, cx - r/3, cy, cx + r/3, cy, white);
            tft.fillRect(cx - r/6, cy - r/3, r/3, r/3, white);
            break;
    }
}

// ─── Bonus Icons ─────────────────────────────────────────────────────────────

inline void drawInitiativeStar(int16_t x, int16_t y, uint8_t size = 16) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 1;
    uint16_t gold = 0xFE60;

    // 5-point star approximation with triangles
    tft.fillTriangle(cx, cy - r, cx - r/3, cy + r/3, cx + r/3, cy + r/3, gold);
    tft.fillTriangle(cx - r, cy - r/4, cx + r, cy - r/4, cx, cy + r, gold);
}

inline void drawNoReminderBell(int16_t x, int16_t y, uint8_t size = 16) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 1;
    uint16_t silver = 0xBDF7;

    // Bell shape
    tft.fillTriangle(cx - r/2, cy + r/4, cx + r/2, cy + r/4, cx, cy - r, silver);
    tft.fillRect(cx - r/2, cy + r/4, r, r/4, silver);
    tft.fillCircle(cx, cy + r/2, r/5, silver);
    // Slash through it (silent)
    tft.drawLine(x + 2, y + size - 2, x + size - 2, y + 2, 0xF800);
}

inline void drawStreakFire(int16_t x, int16_t y, uint16_t streakDays, uint8_t size = 16) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 1;

    // Flame color intensifies with streak
    uint16_t outerColor = (streakDays >= 10) ? 0xF800 : 0xFDA0;  // Red or amber
    uint16_t innerColor = 0xFE60;  // Gold core

    // Outer flame
    tft.fillTriangle(cx, cy - r, cx - r/2, cy + r, cx + r/2, cy + r, outerColor);
    // Inner flame
    tft.fillTriangle(cx, cy - r/3, cx - r/4, cy + r, cx + r/4, cy + r, innerColor);
}

// ─── UI Icons ────────────────────────────────────────────────────────────────

inline void drawRfidIcon(int16_t x, int16_t y, uint16_t color, uint8_t size = 48) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 2;

    // Card outline
    tft.drawRoundRect(cx - r/2, cy - r * 2/3, r, r * 4/3, 4, color);

    // Signal arcs
    for (int i = 1; i <= 3; i++) {
        int16_t arcR = r/3 + i * r/4;
        // Draw arc segments (approximated with short lines)
        for (int a = -45; a <= 45; a += 8) {
            float rad = a * PI / 180.0f;
            int16_t px = cx + r/2 + (int16_t)(cos(rad) * arcR);
            int16_t py = cy + (int16_t)(sin(rad) * arcR);
            tft.drawPixel(px, py, color);
            tft.drawPixel(px + 1, py, color);
        }
    }
}

inline void drawBackArrow(int16_t x, int16_t y, uint16_t color, uint8_t size = 24) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 3;

    tft.fillTriangle(cx - r, cy, cx, cy - r/2, cx, cy + r/2, color);
    tft.fillRect(cx, cy - r/4, r, r/2, color);
}

inline void drawGearIcon(int16_t x, int16_t y, uint16_t color, uint8_t size = 24) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 2;

    tft.drawCircle(cx, cy, r * 2/3, color);
    tft.fillCircle(cx, cy, r/3, color);
    // Gear teeth (N/S/E/W + diagonals)
    for (int a = 0; a < 360; a += 45) {
        float rad = a * PI / 180.0f;
        int16_t px = cx + (int16_t)(cos(rad) * r);
        int16_t py = cy + (int16_t)(sin(rad) * r);
        tft.fillCircle(px, py, 2, color);
    }
}

inline void drawPlusIcon(int16_t x, int16_t y, uint16_t color, uint8_t size = 24) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 3;

    tft.fillRect(cx - r, cy - 2, r * 2, 4, color);
    tft.fillRect(cx - 2, cy - r, 4, r * 2, color);
}

inline void drawCheckIcon(int16_t x, int16_t y, uint16_t color, uint8_t size = 24) {
    int16_t cx = x + size / 2;
    int16_t cy = y + size / 2;
    int16_t r  = size / 2 - 3;

    tft.drawLine(cx - r/2, cy, cx - r/6, cy + r/2, color);
    tft.drawLine(cx - r/6, cy + r/2, cx + r/2, cy - r/2, color);
    // Thicken
    tft.drawLine(cx - r/2, cy + 1, cx - r/6, cy + r/2 + 1, color);
    tft.drawLine(cx - r/6, cy + r/2 + 1, cx + r/2, cy - r/2 + 1, color);
}

// ─── Progress Bar (replaces LED strip) ───────────────────────────────────────

inline void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint8_t completed, uint8_t total,
                            uint16_t fillColor, uint16_t bgColor = 0x1082) {
    // Background track
    tft.fillRoundRect(x, y, w, h, h / 2, bgColor);
    tft.drawRoundRect(x, y, w, h, h / 2, 0x2945);

    if (total == 0) return;

    // Filled portion
    int16_t fillW = (int16_t)((float)completed / total * (w - 4));
    if (fillW < h) fillW = h;  // Minimum visible fill
    if (completed > 0) {
        tft.fillRoundRect(x + 2, y + 2, fillW, h - 4, (h - 4) / 2, fillColor);
    }

    // Tick marks for each task slot
    for (uint8_t i = 1; i < total; i++) {
        int16_t tx = x + 2 + (int16_t)((float)i / total * (w - 4));
        tft.drawFastVLine(tx, y + 2, h - 4, 0x1082);
    }
}

} // namespace Icons
