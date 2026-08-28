#pragma once

#include <raylib-cpp.hpp>
#include "../shared/Protocol.h"

// Draws the local player's own 4-slot hotbar along the bottom of the screen: one box per
// slot, showing the held item's count (blank if empty) and highlighting whichever slot is
// currently selected. Screen-space (not affected by the world camera), drawn OUTSIDE
// BeginMode2D/EndMode2D in main.cpp, same as other pure-HUD elements in this codebase.
inline void DrawHotbar(const PlayerSnapshot& mySnapshot, int selectedSlot, int screenWidth, int screenHeight) {
    const int kSlotSize = 48;
    const int kSlotGap = 8;
    const int kSlotCount = 4;
    int totalWidth = kSlotCount * kSlotSize + (kSlotCount - 1) * kSlotGap;
    int startX = (screenWidth - totalWidth) / 2;
    int y = screenHeight - kSlotSize - 16;

    for (int i = 0; i < kSlotCount; i++) {
        int x = startX + i * (kSlotSize + kSlotGap);
        Rectangle box{ (float)x, (float)y, (float)kSlotSize, (float)kSlotSize };

        bool selected = (i == selectedSlot);
        DrawRectangleRec(box, selected ? Fade(SKYBLUE, 0.35f) : Fade(LIGHTGRAY, 0.25f));
        DrawRectangleLinesEx(box, selected ? 3 : 1, selected ? SKYBLUE : DARKGRAY);

        const HotbarSlotSnapshot& slot = mySnapshot.slots[i];
        DrawText(TextFormat("%d", i + 1), x + 4, y - 18, 14, BLACK); // key label above the box

        if (slot.count > 0) {
            // Only one item type exists today (RevivePotion) — draw a simple filled circle as
            // its icon rather than a real sprite/texture, matching this project's existing
            // placeholder-shape visual style (e.g. the gold circle used for world items).
            DrawCircleV(Vector2{ (float)(x + kSlotSize / 2), (float)(y + kSlotSize / 2) }, 14, GOLD);
            DrawText(TextFormat("%d", slot.count), x + kSlotSize - 16, y + kSlotSize - 18, 14, BLACK);
        }
    }
}
