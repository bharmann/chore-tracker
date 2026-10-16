// =============================================================================
// milestones.h — Collectible Achievement Badge System
// =============================================================================
// Milestones are earned at point thresholds and displayed as collectible
// icons on a dedicated screen. Each milestone has a unique icon, name,
// and threshold. Kids can scroll through their collection — earned badges
// are full color, unearned are grayed outlines.
//
// Design philosophy:
//   - Badges are PERSONAL — no cross-sibling comparison
//   - Progress shown as "X badges earned, working toward next"
//   - No raw point totals displayed (trajectory card handles pacing)
//   - Badge art uses simple geometric shapes (TFT primitives)
//   - Themed tiers: Starter → Explorer → Champion → Legend → Master
// =============================================================================

#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "data_model.h"

extern TFT_eSPI tft;

// ─── Milestone definitions ───────────────────────────────────────────────────

struct MilestoneDef {
    uint16_t    pointThreshold;   // Points needed to earn this badge
    const char* name;             // Display name
    const char* tier;             // Tier label
    uint16_t    iconColor;        // Primary color when earned
};

// Total milestone count
static const uint8_t MILESTONE_COUNT = 16;

// Milestone table — ordered by threshold ascending
static const MilestoneDef MILESTONES[MILESTONE_COUNT] = {
    // ── Starter Tier (getting started) ──
    {    10, "First Steps",      "Starter",   0x2C9F },  // Teal
    {    25, "Helping Hand",     "Starter",   0x2C9F },
    {    50, "Rising Star",      "Starter",   0x2C9F },

    // ── Explorer Tier (building habits) ──
    {   100, "Trailblazer",      "Explorer",  0x2E8B },  // Green
    {   175, "Go-Getter",        "Explorer",  0x2E8B },
    {   250, "Pathfinder",       "Explorer",  0x2E8B },

    // ── Champion Tier (consistent effort) ──
    {   400, "Task Champion",    "Champion",  0xFE60 },  // Gold
    {   600, "Daily Hero",       "Champion",  0xFE60 },
    {   850, "Responsibility Pro","Champion", 0xFE60 },

    // ── Legend Tier (long-term commitment) ──
    {  1200, "Household Legend",  "Legend",   0xF81F },  // Magenta
    {  1600, "Initiative Master", "Legend",   0xF81F },
    {  2100, "Dependable",        "Legend",   0xF81F },

    // ── Master Tier (extraordinary) ──
    {  2800, "Iron Streak",      "Master",    0xFFE0 },  // Bright yellow
    {  3600, "Family MVP",       "Master",    0xFFE0 },
    {  4500, "Grand Master",     "Master",    0xFFE0 },
    {  6000, "Ultimate Helper",  "Master",    0xFFE0 },
};

// ─── Milestone badge shapes ──────────────────────────────────────────────────
// Each tier has a distinct badge frame shape

namespace MilestoneIcons {

    // Get the tier index (0-4) from the milestone index
    inline uint8_t tierIndex(uint8_t milestoneIdx) {
        if (milestoneIdx < 3) return 0;       // Starter
        if (milestoneIdx < 6) return 1;       // Explorer
        if (milestoneIdx < 9) return 2;       // Champion
        if (milestoneIdx < 12) return 3;      // Legend
        return 4;                              // Master
    }

    // Draw a single milestone badge
    // earned = true → full color; false → dim outline
    inline void drawBadge(int16_t x, int16_t y, uint8_t milestoneIdx, bool earned,
                          uint8_t size = 44) {
        const MilestoneDef& m = MILESTONES[milestoneIdx];
        int16_t cx = x + size / 2;
        int16_t cy = y + size / 2;
        int16_t r  = size / 2 - 2;

        uint16_t color = earned ? m.iconColor : 0x2945;   // Full color or dim gray
        uint16_t fill  = earned ? 0x1082 : 0x0841;         // Dark fill or darker

        uint8_t tier = tierIndex(milestoneIdx);

        switch (tier) {
            case 0: // Starter — Circle badge
                tft.fillCircle(cx, cy, r, fill);
                tft.drawCircle(cx, cy, r, color);
                tft.drawCircle(cx, cy, r - 2, color);
                break;

            case 1: // Explorer — Hexagon badge
            {
                // Approximate hexagon with 6 vertices
                int16_t hex[12];
                for (int i = 0; i < 6; i++) {
                    float angle = (60 * i - 30) * PI / 180.0f;
                    hex[i * 2]     = cx + (int16_t)(cos(angle) * r);
                    hex[i * 2 + 1] = cy + (int16_t)(sin(angle) * r);
                }
                // Draw as triangles from center
                for (int i = 0; i < 6; i++) {
                    int j = (i + 1) % 6;
                    tft.fillTriangle(cx, cy, hex[i*2], hex[i*2+1],
                                     hex[j*2], hex[j*2+1], fill);
                    tft.drawLine(hex[i*2], hex[i*2+1], hex[j*2], hex[j*2+1], color);
                }
                break;
            }

            case 2: // Champion — Shield badge
                tft.fillRoundRect(x + 2, y + 2, size - 4, size * 2/3, 4, fill);
                tft.fillTriangle(x + 2, y + size * 2/3 - 2,
                                 x + size - 2, y + size * 2/3 - 2,
                                 cx, y + size - 2, fill);
                tft.drawRoundRect(x + 2, y + 2, size - 4, size * 2/3, 4, color);
                tft.drawLine(x + 2, y + size * 2/3 - 2, cx, y + size - 2, color);
                tft.drawLine(x + size - 2, y + size * 2/3 - 2, cx, y + size - 2, color);
                break;

            case 3: // Legend — Diamond badge
                tft.fillTriangle(cx, y + 2, x + 2, cy, cx, y + size - 2, fill);
                tft.fillTriangle(cx, y + 2, x + size - 2, cy, cx, y + size - 2, fill);
                tft.drawLine(cx, y + 2, x + 2, cy, color);
                tft.drawLine(x + 2, cy, cx, y + size - 2, color);
                tft.drawLine(cx, y + 2, x + size - 2, cy, color);
                tft.drawLine(x + size - 2, cy, cx, y + size - 2, color);
                break;

            case 4: // Master — Star badge
            {
                // 5-point star
                uint16_t starColor = earned ? m.iconColor : 0x2945;
                // Outer star
                tft.fillTriangle(cx, y + 2,
                                 cx - r * 2/3, y + size - 4,
                                 cx + r * 2/3, y + size - 4, fill);
                tft.fillTriangle(x + 2, cy - r/4,
                                 x + size - 2, cy - r/4,
                                 cx, y + size - 6, fill);
                // Outline
                tft.drawLine(cx, y + 2, cx - r/3, cy - 2, starColor);
                tft.drawLine(cx - r/3, cy - 2, x + 2, cy - r/4, starColor);
                tft.drawLine(x + 2, cy - r/4, cx - r/5, cy + r/3, starColor);
                tft.drawLine(cx - r/5, cy + r/3, cx - r * 2/3, y + size - 4, starColor);
                // Right side
                tft.drawLine(cx, y + 2, cx + r/3, cy - 2, starColor);
                tft.drawLine(cx + r/3, cy - 2, x + size - 2, cy - r/4, starColor);
                tft.drawLine(x + size - 2, cy - r/4, cx + r/5, cy + r/3, starColor);
                tft.drawLine(cx + r/5, cy + r/3, cx + r * 2/3, y + size - 4, starColor);
                break;
            }
        }

        // Inner emblem — different per milestone within tier
        if (earned) {
            uint8_t inTier = milestoneIdx % 3;
            int16_t ir = r / 3;
            switch (inTier) {
                case 0: // First in tier — single dot
                    tft.fillCircle(cx, cy, ir, m.iconColor);
                    break;
                case 1: // Second — double dot
                    tft.fillCircle(cx - ir, cy, ir - 1, m.iconColor);
                    tft.fillCircle(cx + ir, cy, ir - 1, m.iconColor);
                    break;
                case 2: // Third — triple / triangle dots
                    tft.fillCircle(cx, cy - ir, ir - 1, m.iconColor);
                    tft.fillCircle(cx - ir, cy + ir/2, ir - 1, m.iconColor);
                    tft.fillCircle(cx + ir, cy + ir/2, ir - 1, m.iconColor);
                    break;
            }
        }
    }

    // ─── Milestone Collection Screen ─────────────────────────────────────────

    // Draw the full milestone collection for a user
    // Returns the number of earned milestones
    // Centered name under a badge cell; wraps at the last space that fits,
    // second line truncated to the cell. 6 px per character at text size 1.
    inline void _drawWrappedName(const char* name, int16_t cellX, int16_t cellW, int16_t y) {
        uint8_t maxChars = (cellW - 4) / 6;
        uint8_t len = strlen(name);
        char line1[24], line2[24];
        if (len <= maxChars) {
            strlcpy(line1, name, sizeof(line1));
            line2[0] = '\0';
        } else {
            int split = -1;
            for (int c = 0; c < len && c <= maxChars; c++) if (name[c] == ' ') split = c;
            if (split < 0) split = maxChars;                 // one long word
            uint8_t l1 = split < (int)sizeof(line1) ? split : sizeof(line1) - 1;
            memcpy(line1, name, l1); line1[l1] = '\0';
            const char* rest = name + split + (name[split] == ' ' ? 1 : 0);
            uint8_t l2 = strlen(rest);
            if (l2 > maxChars) l2 = maxChars;
            if (l2 > sizeof(line2) - 1) l2 = sizeof(line2) - 1;
            memcpy(line2, rest, l2); line2[l2] = '\0';
        }
        tft.setTextSize(1);
        tft.setCursor(cellX + (cellW - (int16_t)strlen(line1) * 6) / 2, y);
        tft.print(line1);
        if (line2[0]) {
            tft.setCursor(cellX + (cellW - (int16_t)strlen(line2) * 6) / 2, y + 9);
            tft.print(line2);
        }
    }

    inline uint8_t drawCollection(const FamilyMember& user, uint8_t scrollOffset = 0) {
        tft.fillScreen(0x1082);

        // ── Header ──
        tft.fillRect(0, 0, 320, 55, 0x2945);
        Icons::drawBackArrow(8, 16, 0xB596, 24);

        tft.setTextColor(0xFFFF);
        tft.setTextSize(2);
        tft.setCursor(40, 10);
        tft.print(user.name);
        tft.setTextSize(1);
        tft.setCursor(40, 32);
        tft.setTextColor(0x7BCF);
        tft.print("BADGE COLLECTION");

        // Count earned
        uint8_t earned = 0;
        for (uint8_t i = 0; i < MILESTONE_COUNT; i++) {
            if (user.totalPoints >= MILESTONES[i].pointThreshold) earned++;
        }

        // Summary line
        tft.setTextColor(0xFE60);
        tft.setCursor(250, 20);                 // right of a 15-char name
        tft.setTextSize(2);
        tft.printf("%d", earned);
        tft.setTextSize(1);
        tft.setCursor(276, 26);
        tft.setTextColor(0x7BCF);
        tft.printf("/ %d", MILESTONE_COUNT);

        // ── Badge rows: one row per tier ──
        // Each tier gets its own row, its cells sized to the number of badges
        // in it (3 or 4), so badges never collide and long names have room.
        // Names wrap onto two lines at a space.
        const int16_t BADGE = 40, ROW_H = 74;
        int16_t y = 60;
        uint8_t i = scrollOffset;
        while (i < MILESTONE_COUNT && y + ROW_H <= 452) {
            const char* tier = MILESTONES[i].tier;
            uint8_t n = 0;
            while (i + n < MILESTONE_COUNT && strcmp(MILESTONES[i + n].tier, tier) == 0) n++;

            tft.setTextSize(1);
            tft.setTextColor(MILESTONES[i].iconColor);
            tft.setCursor(8, y);
            tft.print(tier);

            int16_t cellW = 320 / n;
            for (uint8_t k = 0; k < n; k++) {
                uint8_t m = i + k;
                int16_t cx = k * cellW;
                bool isEarned = (user.totalPoints >= MILESTONES[m].pointThreshold);
                drawBadge(cx + (cellW - BADGE) / 2, y + 11, m, isEarned, BADGE);
                tft.setTextColor(isEarned ? 0xB596 : 0x4A69);
                _drawWrappedName(MILESTONES[m].name, cx, cellW, y + 11 + BADGE + 3);
            }
            i += n;
            y += ROW_H;
        }

        // ── Next milestone hint ──
        if (earned < MILESTONE_COUNT) {
            uint16_t nextThreshold = MILESTONES[earned].pointThreshold;
            uint16_t remaining = nextThreshold - user.totalPoints;
            tft.fillRect(0, 455, 320, 25, 0x2945);
            tft.setTextColor(0x7BCF);
            tft.setTextSize(1);
            tft.setCursor(15, 462);
            tft.printf("Next: %s - %d pts to go", MILESTONES[earned].name, remaining);
        }

        return earned;
    }

    // ─── Milestone notification (shown briefly on task complete) ──────────

    inline void drawNewBadgeNotification(uint8_t milestoneIdx) {
        const MilestoneDef& m = MILESTONES[milestoneIdx];

        // Overlay card in center of screen
        int16_t cardW = 240;
        int16_t cardH = 160;
        int16_t cardX = (320 - cardW) / 2;
        int16_t cardY = (480 - cardH) / 2 - 20;

        // Shadow
        tft.fillRoundRect(cardX + 3, cardY + 3, cardW, cardH, 12, 0x0000);
        // Card
        tft.fillRoundRect(cardX, cardY, cardW, cardH, 12, 0x18E6);   // dark slate (RGB565)
        tft.drawRoundRect(cardX, cardY, cardW, cardH, 12, m.iconColor);
        tft.drawRoundRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2, 11, m.iconColor);

        // "NEW BADGE!" header
        tft.setTextColor(m.iconColor);
        tft.setTextSize(2);
        tft.setCursor(cardX + 55, cardY + 12);
        tft.print("NEW BADGE!");

        // Draw the badge large and centered
        drawBadge(cardX + cardW/2 - 28, cardY + 40, milestoneIdx, true, 56);

        // Badge name
        tft.setTextColor(0xFFFF);
        tft.setTextSize(2);
        int16_t nameW = strlen(m.name) * 12;
        tft.setCursor(cardX + (cardW - nameW) / 2, cardY + 105);
        tft.print(m.name);

        // Tier label
        tft.setTextColor(0x7BCF);
        tft.setTextSize(1);
        int16_t tierW = strlen(m.tier) * 6;
        tft.setCursor(cardX + (cardW - tierW) / 2, cardY + 130);
        tft.print(m.tier);
        tft.print(" Tier");
    }

} // namespace MilestoneIcons
