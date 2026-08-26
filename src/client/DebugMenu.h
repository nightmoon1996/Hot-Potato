#pragma once

#include <raylib-cpp.hpp>
#include "NetClient.h"

class DebugMenu {
public:
    bool visible = false;

    void Toggle() {
        visible = !visible;
    }

    void DrawAndHandle(NetClient& netClient) {
        if (!visible) {
            return;
        }

        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.5f));
        DrawRectangle(300, 150, 400, 260, RAYWHITE);
        DrawText("DEBUG MENU (F1 to close)", 320, 160, 16, BLACK);

        DrawText("P1 (slot 0)", 340, 190, 16, BLUE);
        DrawText("P2 (slot 1)", 540, 190, 16, MAROON);

        HandleColumn(netClient, 0, 320, 210);
        HandleColumn(netClient, 1, 520, 210);
    }

private:
    void HandleColumn(NetClient& netClient, uint8_t targetSlot, int x, int startY) {
        const char* labels[5] = { "Kill", "Revive", "Heal Full", "Give Potion", "Force Down" };
        DebugAction actions[5] = { DebugAction::Kill, DebugAction::Revive, DebugAction::HealFull, DebugAction::GivePotion, DebugAction::ForceDown };
        for (int i = 0; i < 5; i++) {
            Rectangle bounds{ (float)x, (float)(startY + i * 40), 150.0f, 30.0f };
            DrawRectangleRec(bounds, LIGHTGRAY);
            DrawRectangleLinesEx(bounds, 1, DARKGRAY);
            DrawText(labels[i], (int)bounds.x + 8, (int)bounds.y + 7, 14, BLACK);

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                CheckCollisionPointRec(GetMousePosition(), bounds)) {
                netClient.SendDebugAction(actions[i], targetSlot);
            }
        }
    }
};
