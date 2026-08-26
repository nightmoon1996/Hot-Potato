#pragma once

#include <raylib-cpp.hpp>
#include "Player.h"

struct DebugButton {
    Rectangle bounds;
    const char* label;
};

class DebugMenu {
public:
    bool visible = false;

    void Toggle() {
        visible = !visible;
    }

    void DrawAndHandle(Player& p1, Player& p2) {
        if (!visible) {
            return;
        }

        DrawRectangle(0, 0, 1000, 600, Fade(BLACK, 0.5f));
        DrawRectangle(300, 150, 400, 260, RAYWHITE);
        DrawText("DEBUG MENU (F1 to close)", 320, 160, 16, BLACK);

        DrawText("P1", 340, 190, 16, BLUE);
        DrawText("P2", 540, 190, 16, MAROON);

        HandleColumn(p1, 320, 210);
        HandleColumn(p2, 520, 210);
    }

private:
    void HandleColumn(Player& p, int x, int startY) {
        const char* labels[5] = { "Kill", "Revive", "Heal Full", "Give Potion", "Force Down" };
        for (int i = 0; i < 5; i++) {
            Rectangle bounds{ (float)x, (float)(startY + i * 40), 150.0f, 30.0f };
            DrawRectangleRec(bounds, LIGHTGRAY);
            DrawRectangleLinesEx(bounds, 1, DARKGRAY);
            DrawText(labels[i], (int)bounds.x + 8, (int)bounds.y + 7, 14, BLACK);

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                CheckCollisionPointRec(GetMousePosition(), bounds)) {
                ApplyAction(p, i);
            }
        }
    }

    void ApplyAction(Player& p, int index) {
        switch (index) {
            case 0: p.Kill(); break;
            case 1:
                if (p.state == PlayerState::Downed) p.ReviveFromDowned();
                else if (p.state == PlayerState::Dead) p.RespawnFull();
                break;
            case 2:
                if (p.state == PlayerState::Alive) p.hp = Player::kMaxHp;
                break;
            case 3: p.inventory.Add(ItemType::RevivePotion); break;
            case 4: p.ForceDown(); break;
        }
    }
};
