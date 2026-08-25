#include <cassert>
#include <raylib-cpp.hpp>
#include "Item.h"
#include "Player.h"
#include "Hazard.h"
#include "Combat.h"

void SmokeTestItems() {
    Inventory inv;
    assert(inv.Count(ItemType::RevivePotion) == 0);

    assert(inv.Add(ItemType::RevivePotion) == true);
    assert(inv.Count(ItemType::RevivePotion) == 1);

    assert(inv.Add(ItemType::RevivePotion) == true);
    assert(inv.Count(ItemType::RevivePotion) == 2);

    assert(inv.Remove(ItemType::RevivePotion, 1) == true);
    assert(inv.Count(ItemType::RevivePotion) == 1);

    assert(inv.Remove(ItemType::RevivePotion, 5) == false);
    assert(inv.Count(ItemType::RevivePotion) == 1);

    assert(inv.Remove(ItemType::RevivePotion, 1) == true);
    assert(inv.Count(ItemType::RevivePotion) == 0);
    assert(inv.Slots().size() == 0);

    // Pickup logic
    WorldItem item{ {100.0f, 100.0f}, ItemType::RevivePotion, true };
    Inventory pickupInv;

    // Too far away: no pickup
    bool pickedFar = TryPickup(item, Vector2{500.0f, 500.0f}, pickupInv, 24.0f);
    assert(pickedFar == false);
    assert(item.active == true);
    assert(pickupInv.Count(ItemType::RevivePotion) == 0);

    // Within radius: picks up
    bool pickedNear = TryPickup(item, Vector2{105.0f, 100.0f}, pickupInv, 24.0f);
    assert(pickedNear == true);
    assert(item.active == false);
    assert(pickupInv.Count(ItemType::RevivePotion) == 1);

    // Already inactive: no second pickup
    bool pickedAgain = TryPickup(item, Vector2{105.0f, 100.0f}, pickupInv, 24.0f);
    assert(pickedAgain == false);
    assert(pickupInv.Count(ItemType::RevivePotion) == 1);
}

void SmokeTestPlayerStateMachine() {
    Player p(Vector2{0.0f, 0.0f});
    assert(p.state == PlayerState::Alive);
    assert(p.hp == Player::kMaxHp);

    // Damage that doesn't kill
    p.TakeDamage(30);
    assert(p.state == PlayerState::Alive);
    assert(p.hp == 70);

    // Damage that downs
    p.TakeDamage(100);
    assert(p.state == PlayerState::Downed);
    assert(p.hp == 0);
    assert(p.downedTimer == Player::kDownedDuration);

    // Damage while downed has no effect
    p.TakeDamage(10);
    assert(p.state == PlayerState::Downed);

    // Downed timer expiring kills
    p.UpdateTimers(Player::kDownedDuration + 1.0f);
    assert(p.state == PlayerState::Dead);
    assert(p.deathRespawnTimer == Player::kDeathRespawnDelay);

    // Death respawn timer expiring respawns
    p.UpdateTimers(Player::kDeathRespawnDelay + 1.0f);
    assert(p.state == PlayerState::Alive);
    assert(p.hp == Player::kRespawnHp);
    assert(p.position.x == p.spawnPoint.x && p.position.y == p.spawnPoint.y);

    // Revive from downed
    Player p2(Vector2{0.0f, 0.0f});
    p2.ForceDown();
    assert(p2.state == PlayerState::Downed);
    p2.ReviveFromDowned();
    assert(p2.state == PlayerState::Alive);
    assert(p2.hp == Player::kReviveHp);
}

void SmokeTestHazard() {
    HazardZone zone{ Rectangle{0.0f, 0.0f, 100.0f, 100.0f} };
    Player p(Vector2{50.0f, 50.0f}); // inside the zone
    float carry = 0.0f;

    // 1 second at 5 HP/sec should deal 5 damage total, spread across calls
    for (int i = 0; i < 60; i++) {
        ApplyHazardDamage(zone, p, 1.0f / 60.0f, carry);
    }
    assert(p.hp <= Player::kMaxHp - 4);

    // Player outside the zone takes no damage
    Player p2(Vector2{500.0f, 500.0f});
    float carry2 = 0.0f;
    ApplyHazardDamage(zone, p2, 1.0f, carry2);
    assert(p2.hp == Player::kMaxHp);
}

void SmokeTestCombatAndRevive() {
    // Attack: in range, off cooldown -> succeeds
    Player attacker(Vector2{0.0f, 0.0f});
    Player target(Vector2{10.0f, 0.0f});
    bool hit = TryAttack(attacker, target);
    assert(hit == true);
    assert(target.hp == Player::kMaxHp - Player::kAttackDamage);
    assert(attacker.attackCooldownTimer == Player::kAttackCooldown);

    // Immediately again: on cooldown -> fails
    bool hit2 = TryAttack(attacker, target);
    assert(hit2 == false);
    assert(target.hp == Player::kMaxHp - Player::kAttackDamage); // unchanged

    // Out of range -> fails
    Player far(Vector2{1000.0f, 1000.0f});
    Player attacker2(Vector2{0.0f, 0.0f});
    bool hit3 = TryAttack(attacker2, far);
    assert(hit3 == false);

    // Revive: full sequence
    Player reviver(Vector2{0.0f, 0.0f});
    Player downed(Vector2{5.0f, 0.0f});
    downed.ForceDown();
    reviver.inventory.Add(ItemType::RevivePotion);

    // Not enough time yet: still downed
    bool completed1 = UpdateRevive(reviver, downed, true, 1.0f, 32.0f);
    assert(completed1 == false);
    assert(downed.state == PlayerState::Downed);

    // Cross the threshold
    bool completed2 = UpdateRevive(reviver, downed, true, 1.5f, 32.0f);
    assert(completed2 == true);
    assert(downed.state == PlayerState::Alive);
    assert(downed.hp == Player::kReviveHp);
    assert(reviver.inventory.Count(ItemType::RevivePotion) == 0);

    // Releasing the key resets progress
    Player reviver2(Vector2{0.0f, 0.0f});
    Player downed2(Vector2{5.0f, 0.0f});
    downed2.ForceDown();
    reviver2.inventory.Add(ItemType::RevivePotion);
    UpdateRevive(reviver2, downed2, true, 1.0f, 32.0f);
    assert(reviver2.channelTimer > 0.0f);
    UpdateRevive(reviver2, downed2, false, 0.0f, 32.0f); // key released
    assert(reviver2.channelTimer == 0.0f);
}

int main()
{
    SmokeTestItems();
    TraceLog(LOG_INFO, "SmokeTestItems passed");

    SmokeTestPlayerStateMachine();
    TraceLog(LOG_INFO, "SmokeTestPlayerStateMachine passed");

    SmokeTestHazard();
    TraceLog(LOG_INFO, "SmokeTestHazard passed");

    SmokeTestCombatAndRevive();
    TraceLog(LOG_INFO, "SmokeTestCombatAndRevive passed");

    TraceLog(LOG_INFO, "All smoke tests passed");

    int screenWidth = 1000;
    int screenHeight = 600;
    raylib::Window w(screenWidth, screenHeight, "Maxion Test - Pickup/Inventory/Revive");
    SetTargetFPS(60);

    Player p1(Vector2{150.0f, 300.0f});
    Player p2(Vector2{850.0f, 300.0f});
    float p1HazardCarry = 0.0f;
    float p2HazardCarry = 0.0f;

    HazardZone hazard{ Rectangle{450.0f, 200.0f, 100.0f, 200.0f} };

    std::vector<WorldItem> worldItems = {
        WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
        WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
    };

    const float moveSpeed = 200.0f;
    const float pickupRadius = 24.0f;
    const float reviveRange = 32.0f;

    while (!w.ShouldClose())
    {
        float dt = GetFrameTime();

        // --- Input: movement (only when Alive) ---
        if (p1.state == PlayerState::Alive) {
            Vector2 move{0, 0};
            if (IsKeyDown(KEY_W)) move.y -= 1;
            if (IsKeyDown(KEY_S)) move.y += 1;
            if (IsKeyDown(KEY_A)) move.x -= 1;
            if (IsKeyDown(KEY_D)) move.x += 1;
            p1.position.x += move.x * moveSpeed * dt;
            p1.position.y += move.y * moveSpeed * dt;
        }
        if (p2.state == PlayerState::Alive) {
            Vector2 move{0, 0};
            if (IsKeyDown(KEY_UP)) move.y -= 1;
            if (IsKeyDown(KEY_DOWN)) move.y += 1;
            if (IsKeyDown(KEY_LEFT)) move.x -= 1;
            if (IsKeyDown(KEY_RIGHT)) move.x += 1;
            p2.position.x += move.x * moveSpeed * dt;
            p2.position.y += move.y * moveSpeed * dt;
        }

        // --- Input: pickup (interact key, only when Alive) ---
        if (p1.state == PlayerState::Alive && IsKeyPressed(KEY_E)) {
            for (auto& item : worldItems) {
                if (TryPickup(item, p1.position, p1.inventory, pickupRadius)) break;
            }
        }
        if (p2.state == PlayerState::Alive && IsKeyPressed(KEY_RIGHT_CONTROL)) {
            for (auto& item : worldItems) {
                if (TryPickup(item, p2.position, p2.inventory, pickupRadius)) break;
            }
        }

        // --- Input: attack ---
        if (IsKeyPressed(KEY_Q)) {
            TryAttack(p1, p2);
        }
        if (IsKeyPressed(KEY_RIGHT_SHIFT)) {
            TryAttack(p2, p1);
        }
        UpdateAttackCooldown(p1, dt);
        UpdateAttackCooldown(p2, dt);

        // --- Input + logic: channel revive (interact key held) ---
        bool p1InteractHeld = IsKeyDown(KEY_E);
        bool p2InteractHeld = IsKeyDown(KEY_RIGHT_CONTROL);
        UpdateRevive(p1, p2, p1InteractHeld, dt, reviveRange);
        UpdateRevive(p2, p1, p2InteractHeld, dt, reviveRange);

        // --- Hazard damage ---
        ApplyHazardDamage(hazard, p1, dt, p1HazardCarry);
        ApplyHazardDamage(hazard, p2, dt, p2HazardCarry);

        // --- State timers (downed -> dead -> respawn) ---
        p1.UpdateTimers(dt);
        p2.UpdateTimers(dt);

        // --- Draw ---
        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawRectangleRec(hazard.bounds, Fade(RED, 0.3f));

        for (const auto& item : worldItems) {
            if (item.active) {
                DrawCircleV(item.position, 8, GOLD);
            }
        }

        auto drawPlayer = [](const Player& p, Color color, const char* label) {
            Color drawColor = color;
            if (p.state == PlayerState::Downed) drawColor = GRAY;
            if (p.state == PlayerState::Dead) drawColor = Fade(GRAY, 0.3f);

            DrawCircleV(p.position, 16, drawColor);
            DrawText(label, (int)p.position.x - 10, (int)p.position.y - 34, 14, BLACK);

            int barWidth = 40;
            DrawRectangle((int)p.position.x - barWidth / 2, (int)p.position.y - 26, barWidth, 5, DARKGRAY);
            int hpWidth = (int)(barWidth * ((float)p.hp / Player::kMaxHp));
            DrawRectangle((int)p.position.x - barWidth / 2, (int)p.position.y - 26, hpWidth, 5, GREEN);

            DrawText(TextFormat("Potions: %d", p.inventory.Count(ItemType::RevivePotion)),
                      (int)p.position.x - 30, (int)p.position.y + 20, 12, DARKBLUE);

            if (p.channelTimer > 0.0f) {
                float ratio = p.channelTimer / Player::kChannelDuration;
                DrawCircleSector(p.position, 24, -90, -90 + 360 * ratio, 32, Fade(SKYBLUE, 0.6f));
            }
        };

        drawPlayer(p1, BLUE, "P1");
        drawPlayer(p2, MAROON, "P2");

        DrawText("P1: WASD move, E pickup/revive, Q attack", 10, 10, 16, BLACK);
        DrawText("P2: Arrows move, RCtrl pickup/revive, RShift attack", 10, 30, 16, BLACK);

        EndDrawing();
    }

    return 0;
}
