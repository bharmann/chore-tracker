// =============================================================================
// preset_names.h — Shared Task Name Presets
// =============================================================================
// Single source of truth for the 6 preset names per category.
// Used by main.cpp (child self-init, quick-add) and parent_admin.h (wizard).
// =============================================================================

#pragma once

static const uint8_t NAMES_PER_CAT = 6;

inline const char* presetTaskName(uint8_t cat, uint8_t idx) {
    static const char* const NAMES[6][6] = {
        {"Tidy room", "Empty dishwasher", "Wipe counters", "Vacuum floor", "Clean bathroom", "Take out trash"},
        {"Brush teeth", "Shower/Bath", "Wash hands", "Pack lunch", "Lay out clothes", "Drink water"},
        {"Set table", "Clear table", "Help cook", "Help sibling", "Fold laundry", "Carry groceries"},
        {"Homework", "Read 20 min", "Practice music", "Study spelling", "Math worksheet", "Science project"},
        {"Feed pet", "Walk dog", "Clean litter", "Fill water bowl", "Brush pet", "Play with pet"},
        {"Mow lawn", "Rake leaves", "Water plants", "Sweep porch", "Pull weeds", "Wash car"}
    };
    if (cat >= 6 || idx >= 6) return "Task";
    return NAMES[cat][idx];
}
