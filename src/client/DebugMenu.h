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
        DrawRectangle(150, 150, 700, 260, RAYWHITE);
        DrawText("DEBUG MENU (F1 to close)", 170, 160, 16, BLACK);

        // Session-wide action, deliberately outside the per-player columns: resets the
        // match (matchOver -> false, scores cleared) and respawns the potato. The server
        // ignores targetSlot for DebugAction::NewMatch, so the 0 below is a placeholder.
        {
            Rectangle bounds{ 690.0f, 155.0f, 140.0f, 26.0f };
            DrawRectangleRec(bounds, LIGHTGRAY);
            DrawRectangleLinesEx(bounds, 1, DARKGRAY);
            DrawText("New Match", (int)bounds.x + 8, (int)bounds.y + 6, 14, BLACK);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                CheckCollisionPointRec(GetMousePosition(), bounds)) {
                netClient.SendDebugAction(DebugAction::NewMatch, 0);
            }
        }

        const char* labels[kMaxPlayersPerSession] = { "P1 (slot 0)", "P2 (slot 1)", "P3 (slot 2)", "P4 (slot 3)" };
        Color colors[kMaxPlayersPerSession] = { BLUE, MAROON, GREEN, PURPLE };
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            int x = 170 + i * 170;
            DrawText(labels[i], x, 190, 14, colors[i]);
            HandleColumn(netClient, (uint8_t)i, x, 210);
        }
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
