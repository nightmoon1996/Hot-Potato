#include <raylib-cpp.hpp>
#include <string>
#include <cmath>
#include "NetClient.h"
#include "DebugMenu.h"
#include "Juice.h"
#include "JuiceTests.h"
#include "RoomMenuTests.h"

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--test") {
            RunJuiceSmokeTests();
            RunRoomMenuSmokeTests();
            return 0;
        }
    }

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
    ClientEffectsState effects;
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
        if (netClient.HasReceivedSnapshot()) {
            effects.Update(snap, mySlot, dt);
        }

        BeginDrawing();
        ClearBackground(RAYWHITE);

        Camera2D camera{};
        camera.offset = Vector2{ (float)screenWidth / 2.0f + effects.GetShakeOffsetX(), (float)screenHeight / 2.0f + effects.GetShakeOffsetY() };
        camera.target = Vector2{ (float)screenWidth / 2.0f, (float)screenHeight / 2.0f };
        camera.rotation = 0.0f;
        camera.zoom = 1.0f;

        BeginMode2D(camera);

        DrawRectangle((int)snap.hazardX, (int)snap.hazardY, (int)snap.hazardW, (int)snap.hazardH, Fade(RED, 0.3f));

        for (int i = 0; i < 2; i++) {
            if (snap.items[i].active) {
                DrawCircleV(Vector2{ snap.items[i].posX, snap.items[i].posY }, 8, GOLD);
            }
        }

        auto drawPlayer = [&](int slot, const PlayerSnapshot& p, Color color, const char* label) {
            if (p.state == 3) return; // absent: slot not occupied by a connected player

            Color drawColor = color;
            if (p.state == 1) drawColor = GRAY;   // Downed
            if (p.state == 2) drawColor = Fade(GRAY, 0.3f); // Dead

            float flashRatio = effects.GetHitFlashRatio(slot);
            if (flashRatio > 0.0f) {
                drawColor = ColorLerp(drawColor, WHITE, flashRatio);
            }

            Vector2 pos{ p.posX, p.posY };
            DrawCircleV(pos, 16, drawColor);
            DrawText(label, (int)pos.x - 10, (int)pos.y - 34, 14, BLACK);

            int barWidth = 40;
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, barWidth, 5, DARKGRAY);
            float displayedHp = effects.GetDisplayedHp(slot);
            int hpWidth = (int)(barWidth * (displayedHp / constants.maxHp));
            if (hpWidth < 0) hpWidth = 0;
            DrawRectangle((int)pos.x - barWidth / 2, (int)pos.y - 26, hpWidth, 5, GREEN);

            DrawText(TextFormat("Potions: %d", p.potionCount), (int)pos.x - 30, (int)pos.y + 20, 12, DARKBLUE);

            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / constants.channelDuration;
                int filledDots = (int)(kReviveRingDotCount * ratio);
                if (filledDots > kReviveRingDotCount) filledDots = kReviveRingDotCount;
                float ringRadius = kReviveRingRadius;
                for (int dot = 0; dot < kReviveRingDotCount; dot++) {
                    float angle = (-90.0f + 360.0f * (float)dot / (float)kReviveRingDotCount) * (3.14159265f / 180.0f);
                    Vector2 dotPos{ pos.x + cosf(angle) * ringRadius, pos.y + sinf(angle) * ringRadius };
                    if (dot < filledDots) {
                        DrawCircleV(dotPos, kReviveRingDotRadius, Fade(SKYBLUE, 0.9f));
                    } else {
                        DrawCircleLines((int)dotPos.x, (int)dotPos.y, (int)kReviveRingDotRadius, Fade(SKYBLUE, 0.4f));
                    }
                }
            }
        };

        drawPlayer(0, snap.players[0], BLUE, "P1");
        drawPlayer(1, snap.players[1], MAROON, "P2");

        for (int i = 0; i < effects.GetParticleCount(); i++) {
            const Particle& particle = effects.GetParticles()[i];
            if (!particle.active) continue;
            float alpha = 1.0f - (particle.age / particle.lifetime);
            if (alpha < 0.0f) alpha = 0.0f;
            DrawCircleV(particle.pos, 3.0f, Fade(particle.color, alpha));
        }

        for (int i = 0; i < effects.GetDamageNumberCount(); i++) {
            const DamageNumber& dmgNum = effects.GetDamageNumbers()[i];
            if (!dmgNum.active) continue;
            float alpha = 1.0f - (dmgNum.age / dmgNum.lifetime);
            if (alpha < 0.0f) alpha = 0.0f;
            const char* text = TextFormat("-%d", dmgNum.value);
            Color textColor = Fade(RED, alpha);
            DrawText(text, (int)dmgNum.pos.x - 6, (int)dmgNum.pos.y - 10, 16, textColor);
        }

        EndMode2D();

        DrawText(TextFormat("You are slot %d. WASD move, E pickup/revive, Q attack.", (int)mySlot), 10, 10, 16, BLACK);
        DrawText("F1: Debug Menu", 10, 30, 16, DARKGRAY);

        debugMenu.DrawAndHandle(netClient);

        EndDrawing();
    }

    return 0;
}
