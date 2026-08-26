#include <raylib-cpp.hpp>
#include <string>
#include "NetClient.h"
#include "DebugMenu.h"

int main(int argc, char** argv)
{
    std::string sessionName = (argc > 1) ? argv[1] : "default";

    NetClient netClient;
    if (!netClient.Connect("127.0.0.1", 7777, sessionName)) {
        TraceLog(LOG_ERROR, "Failed to connect to server");
        return 1;
    }
    TraceLog(LOG_INFO, "Connected as player slot %d", (int)netClient.GetPlayerSlot());

    int screenWidth = 1000;
    int screenHeight = 600;
    raylib::Window w(screenWidth, screenHeight, "Maxion Test - Client");
    SetTargetFPS(60);

    DebugMenu debugMenu;
    const WelcomeMsg& constants = netClient.GetGameConstants();
    uint8_t mySlot = netClient.GetPlayerSlot();

    while (!w.ShouldClose())
    {
        double now = GetTime();
        float dt = GetFrameTime();

        if (IsKeyPressed(KEY_F1)) {
            debugMenu.Toggle();
        }

        Vector2 move{0, 0};
        if (IsKeyDown(KEY_W)) move.y -= 1;
        if (IsKeyDown(KEY_S)) move.y += 1;
        if (IsKeyDown(KEY_A)) move.x -= 1;
        if (IsKeyDown(KEY_D)) move.x += 1;
        bool interactHeld = IsKeyDown(KEY_E);
        bool attackPressed = IsKeyPressed(KEY_Q);

        netClient.SendInput(move.x, move.y, interactHeld, attackPressed);
        netClient.PollNetwork(now);

        const SnapshotMsg& snap = netClient.GetLatestSnapshot();

        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawRectangle((int)snap.hazardX, (int)snap.hazardY, (int)snap.hazardW, (int)snap.hazardH, Fade(RED, 0.3f));

        for (int i = 0; i < 2; i++) {
            if (snap.items[i].active) {
                DrawCircleV(Vector2{ snap.items[i].posX, snap.items[i].posY }, 8, GOLD);
            }
        }

        auto drawPlayer = [&](const PlayerSnapshot& p, Color color, const char* label) {
            if (p.state == 3) return; // absent: slot not occupied by a connected player

            Color drawColor = color;
            if (p.state == 1) drawColor = GRAY;   // Downed
            if (p.state == 2) drawColor = Fade(GRAY, 0.3f); // Dead

            Vector2 pos{ p.posX, p.posY };
            DrawCircleV(pos, 16, drawColor);
            DrawText(label, (int)pos.x - 10, (int)pos.y - 34, 14, BLACK);

            int barWidth = 40;
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, barWidth, 5, DARKGRAY);
            int hpWidth = (int)(barWidth * ((float)p.hp / constants.maxHp));
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, hpWidth, 5, GREEN);

            DrawText(TextFormat("Potions: %d", p.potionCount), (int)pos.x - 30, (int)pos.y + 20, 12, DARKBLUE);

            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / constants.channelDuration;
                DrawCircleSector(pos, 24, -90, -90 + 360 * ratio, 32, Fade(SKYBLUE, 0.6f));
            }
        };

        drawPlayer(snap.players[0], BLUE, "P1");
        drawPlayer(snap.players[1], MAROON, "P2");

        DrawText(TextFormat("You are slot %d. WASD move, E pickup/revive, Q attack.", (int)mySlot), 10, 10, 16, BLACK);
        DrawText("F1: Debug Menu", 10, 30, 16, DARKGRAY);

        debugMenu.DrawAndHandle(netClient);

        EndDrawing();
    }

    return 0;
}
