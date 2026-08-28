#pragma once

#include <cstdio>
#include <cstdlib>
#include "Juice.h"

inline void RunJuiceSmokeTests() {
    // Test 1: HP drop triggers damage number + particles, HP rise does not
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 100, 100, 100, kSnapshotStateAlive, {}, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        snap.items[0].active = false;
        snap.items[1].active = false;
        state.Update(snap, 0, 0.016f); // first frame, establishes baseline

        snap.players[0].hp = 70;
        state.Update(snap, 0, 0.016f); // hp dropped 100 -> 70

        int activeDamageNumbers = 0;
        for (int i = 0; i < state.GetDamageNumberCount(); i++) {
            if (state.GetDamageNumbers()[i].active) activeDamageNumbers++;
        }
        if (activeDamageNumbers != 1) {
            printf("FAIL: expected 1 active damage number after hp drop, got %d\n", activeDamageNumbers);
            exit(1);
        }

        int activeParticles = 0;
        for (int i = 0; i < state.GetParticleCount(); i++) {
            if (state.GetParticles()[i].active) activeParticles++;
        }
        if (activeParticles != kParticlesPerBurst) {
            printf("FAIL: expected %d active particles after hit, got %d\n", kParticlesPerBurst, activeParticles);
            exit(1);
        }

        // HP rise should not spawn a new damage number
        snap.players[0].hp = 90;
        state.Update(snap, 0, 0.016f);
        activeDamageNumbers = 0;
        for (int i = 0; i < state.GetDamageNumberCount(); i++) {
            if (state.GetDamageNumbers()[i].active) activeDamageNumbers++;
        }
        if (activeDamageNumbers != 1) {
            printf("FAIL: hp rise should not spawn a new damage number, active count changed to %d\n", activeDamageNumbers);
            exit(1);
        }
        printf("PASS: hit detection spawns damage number + particles, heal does not\n");
    }

    // Test 2: Downed -> Alive triggers a revive burst
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 50, 50, 50, kSnapshotStateDowned, {}, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        state.Update(snap, 1, 0.016f);

        snap.players[0].state = kSnapshotStateAlive;
        snap.players[0].hp = 50; // ReviveFromDowned sets hp, no drop, so no hit-flash path triggers
        state.Update(snap, 1, 0.016f);

        int activeParticles = 0;
        for (int i = 0; i < state.GetParticleCount(); i++) {
            if (state.GetParticles()[i].active) activeParticles++;
        }
        if (activeParticles != kParticlesPerBurst) {
            printf("FAIL: expected %d active particles after revive, got %d\n", kParticlesPerBurst, activeParticles);
            exit(1);
        }
        printf("PASS: downed->alive transition spawns revive burst\n");
    }

    // Test 3: item active->inactive triggers a pickup burst
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0].state = kSnapshotStateAbsent;
        snap.players[1].state = kSnapshotStateAbsent;
        snap.items[0] = WorldItemSnapshot{ 200.0f, 200.0f, true };
        state.Update(snap, 0, 0.016f);

        snap.items[0].active = false;
        state.Update(snap, 0, 0.016f);

        int activeParticles = 0;
        for (int i = 0; i < state.GetParticleCount(); i++) {
            if (state.GetParticles()[i].active) activeParticles++;
        }
        if (activeParticles != kParticlesPerBurst) {
            printf("FAIL: expected %d active particles after pickup, got %d\n", kParticlesPerBurst, activeParticles);
            exit(1);
        }
        printf("PASS: item active->inactive transition spawns pickup burst\n");
    }

    // Test 4: displayedHp converges toward real hp over repeated updates
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 0, 0, 100, kSnapshotStateAlive, {}, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        state.Update(snap, 1, 0.016f);

        snap.players[0].hp = 20;
        for (int i = 0; i < 200; i++) {
            state.Update(snap, 1, 0.016f);
        }
        float displayed = state.GetDisplayedHp(0);
        if (displayed > 20.5f || displayed < 19.5f) {
            printf("FAIL: expected displayedHp to converge to ~20 after 200 updates, got %f\n", displayed);
            exit(1);
        }
        printf("PASS: displayedHp converges toward real hp\n");
    }

    // Test 5: shakeTrauma decays to 0 with no further hits
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 0, 0, 100, kSnapshotStateAlive, {}, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        state.Update(snap, 0, 0.016f);

        snap.players[0].hp = 50; // local player (slot 0) hit -> adds trauma
        state.Update(snap, 0, 0.016f);

        for (int i = 0; i < 200; i++) {
            state.Update(snap, 0, 0.016f); // no further hp change, trauma should decay
        }

        float offsetMagnitudeSquaredProxy = state.GetShakeOffsetX(); // near 0 once trauma is 0
        // GetShakeOffsetX is randomized; instead verify indirectly via repeated calls bounding near 0
        bool allNearZero = true;
        for (int i = 0; i < 20; i++) {
            if (state.GetShakeOffsetX() > 0.01f || state.GetShakeOffsetX() < -0.01f) {
                allNearZero = false;
                break;
            }
        }
        if (!allNearZero) {
            printf("FAIL: expected shakeTrauma (and thus shake offset) to have decayed to ~0\n");
            exit(1);
        }
        printf("PASS: shakeTrauma decays to 0 over time with no further hits\n");
    }

    // Test 6: particle/damage-number pools reuse expired slots and don't exceed capacity
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 0, 0, 100, kSnapshotStateAlive, {}, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        state.Update(snap, 1, 0.016f);

        // Trigger far more hits than pool capacity allows across many frames
        for (int hitNum = 0; hitNum < 20; hitNum++) {
            snap.players[0].hp = snap.players[0].hp > 5 ? snap.players[0].hp - 5 : 100;
            state.Update(snap, 1, 0.016f);
        }

        int activeParticles = 0;
        for (int i = 0; i < state.GetParticleCount(); i++) {
            if (state.GetParticles()[i].active) activeParticles++;
        }
        if (activeParticles > kMaxParticles) {
            printf("FAIL: active particle count %d exceeds pool capacity %d\n", activeParticles, kMaxParticles);
            exit(1);
        }
        printf("PASS: particle pool never exceeds capacity across repeated spawns\n");
    }

    // Test 7: present -> absent -> present (reconnect) does not treat stale hp as a diff source
    {
        ClientEffectsState state;
        SnapshotMsg snap{};
        snap.players[0] = PlayerSnapshot{ 0, 0, 100, kSnapshotStateAlive, {}, 0.0f };
        snap.players[1].state = kSnapshotStateAbsent;
        state.Update(snap, 1, 0.016f); // player 0 present, alive, hp=100

        snap.players[0].state = kSnapshotStateAbsent;
        state.Update(snap, 1, 0.016f); // player 0 goes absent

        snap.players[0] = PlayerSnapshot{ 0, 0, 40, kSnapshotStateAlive, {}, 0.0f };
        state.Update(snap, 1, 0.016f); // player 0 reappears at hp=40

        int activeDamageNumbers = 0;
        for (int i = 0; i < state.GetDamageNumberCount(); i++) {
            if (state.GetDamageNumbers()[i].active) activeDamageNumbers++;
        }
        if (activeDamageNumbers != 0) {
            printf("FAIL: reconnect cycle should not spawn a damage number, got %d active\n", activeDamageNumbers);
            exit(1);
        }

        int activeParticles = 0;
        for (int i = 0; i < state.GetParticleCount(); i++) {
            if (state.GetParticles()[i].active) activeParticles++;
        }
        if (activeParticles != 0) {
            printf("FAIL: reconnect cycle should not spawn particles, got %d active\n", activeParticles);
            exit(1);
        }

        float displayed = state.GetDisplayedHp(0);
        if (displayed != 40.0f) {
            printf("FAIL: expected displayedHp to snap to 40 on reappearance, got %f\n", displayed);
            exit(1);
        }
        printf("PASS: present->absent->present cycle snaps displayedHp with no spurious diff effects\n");
    }

    printf("All Juice smoke tests passed.\n");
}
