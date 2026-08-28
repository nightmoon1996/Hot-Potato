#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <string>
#include <cmath>
#include "../shared/Geometry.h" // only for Vector2/Rectangle math types, no window/raylib linking

#include "../shared/Protocol.h"
#include "../shared/Serialize.h"
#include "../shared/ReliableChannel.h"
#include "../shared/Socket.h"
#include "SessionManager.h"
#include "Item.h"
#include "Hazard.h"
#include "Combat.h"
#include "DebugActions.h"
#include "HotPotato.h"
#include "MatchState.h"
#include "Dash.h"

// Defined above main(); forward-declared here so the smoke tests can exercise it.
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, bool* usePressed, float dt,
                                bool* activeOut, HotPotato& potato, float* chargeTimer,
                                InputMsg* latestInputs, Rectangle courtBounds, MatchState& match,
                                GameMode mode);
static void StartNewMatch(Session& session, MatchState& match, HotPotato& potato, float* chargeTimer,
                          GameMode mode);

#include "SmokeTests.h"

struct ClientLocation {
    bool found;
    std::string sessionName;
    int slotIndex;
};

static ClientLocation FindClientByAddress(SessionManager& manager, const std::vector<std::string>& sessionNames,
                                           const std::string& ip, uint16_t port) {
    for (const auto& name : sessionNames) {
        Session* session = manager.GetSession(name);
        if (!session) continue;
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (session->slots[i].clientIp == ip && session->slots[i].clientPort == port &&
                session->slots[i].state == SlotState::Connected) {
                return { true, name, i };
            }
        }
    }
    return { false, "", -1 };
}

static const uint16_t kServerPort = 7777;
static const float kMoveSpeed = 200.0f;
static const float kPickupRadius = 24.0f;
static const float kReviveRange = 32.0f;
static constexpr int kSelfHealAmount = 30;

static double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void SendReliable(UdpSocket& socket, PlayerSlot& slot, MessageType type, const uint8_t* payload, size_t payloadLen, double now) {
    std::vector<uint8_t> buffer;
    buffer.push_back((uint8_t)type);
    buffer.insert(buffer.end(), payload, payload + payloadLen);

    uint32_t seq = slot.reliableSender.NextSeq();
    slot.reliableSender.TrackUnacked(seq, buffer, now);

    PacketHeader header{ 1, seq, (uint16_t)buffer.size() };
    std::vector<uint8_t> packet;
    SerializeStruct(header, packet);
    packet.insert(packet.end(), buffer.begin(), buffer.end());
    socket.SendTo(slot.clientIp, slot.clientPort, packet.data(), packet.size());
}

static void SendUnreliable(UdpSocket& socket, const std::string& ip, uint16_t port, uint32_t seq, MessageType type, const uint8_t* payload, size_t payloadLen) {
    std::vector<uint8_t> buffer;
    buffer.push_back((uint8_t)type);
    buffer.insert(buffer.end(), payload, payload + payloadLen);

    PacketHeader header{ 0, seq, (uint16_t)buffer.size() };
    std::vector<uint8_t> packet;
    SerializeStruct(header, packet);
    packet.insert(packet.end(), buffer.begin(), buffer.end());
    socket.SendTo(ip, port, packet.data(), packet.size());
}

// Starts a fresh Hot Potato round: hands the potato to the first Alive, active player in
// ascending slot order and clears every player's accumulated charge.
static void ResetPotatoForNewRound(HotPotato& potato, Session& session, const bool* active, float* chargeTimer) {
    HotPotato reset{};
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (active[i] && session.slots[i].player.state == PlayerState::Alive) {
            reset.held = true;
            reset.holderSlot = i;
            reset.position = session.slots[i].player.position;
            reset.explodeTimer = ComputeExplodeTimerForCatch(0);
            break;
        }
    }
    // If no Alive player exists, `reset` stays held=false/holderSlot=-1 — the potato
    // idles rather than pinning to a corpse; it'll pick up a holder once someone respawns.
    potato = reset;

    // Clear every player's accumulated charge: a player who charged but never released
    // before this round ended must not carry stale charge into the next round (it would
    // produce an inflated-force "free" throw on their very first tap next time they hold).
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        chargeTimer[i] = 0.0f;
    }
}

// Starts a brand-new match for a session: clears all scores/round state (so matchOver
// goes back to false) and respawns the potato exactly like a normal round reset.
// Triggered out-of-band by the debug menu's "New Match" button (DebugAction::NewMatch);
// session-scoped, so it deliberately lives here rather than in ApplyDebugAction.
static void StartNewMatch(Session& session, MatchState& match, HotPotato& potato, float* chargeTimer,
                          GameMode mode) {
    match = MatchState{};
    bool active[kMaxPlayersPerSession];
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        active[i] = session.slots[i].state == SlotState::Connected;
    }
    if (!HotPotatoGameplayEnabled(mode, active)) {
        // Under-populated 2v2 (or RevivePotionTest, which never calls this): leave the
        // potato inert rather than handing it to someone in a session that isn't allowed
        // to run gameplay yet. The tick seeds it once the population gate opens.
        potato = HotPotato{};
        for (int i = 0; i < kMaxPlayersPerSession; i++) chargeTimer[i] = 0.0f;
        return;
    }
    ResetPotatoForNewRound(potato, session, active, chargeTimer);
}

// Pure gameplay-state mutation for one session's tick: pickup, hot potato, revive
// and timers. Deliberately free of socket/serialization concerns so it can be unit
// tested directly (see SmokeTestSimulationTick). `active` is filled in for the caller,
// which needs it to build the snapshot.
static void SimulateSessionTick(Session& session, std::vector<WorldItem>& items, HazardZone& hazard,
                                float* hazardCarry, bool* attack, bool* interact, bool* usePressed, float dt,
                                bool* activeOut, HotPotato& potato, float* chargeTimer,
                                InputMsg* latestInputs, Rectangle courtBounds, MatchState& match,
                                GameMode mode) {
    // Only players occupying a genuinely Connected slot are simulated.
    // Empty and DisconnectedPending slots are frozen: no movement, pickup,
    // attack, revive, hazard damage, or timer updates.
    bool active[kMaxPlayersPerSession];
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        active[i] = session.slots[i].state == SlotState::Connected;
        if (activeOut) activeOut[i] = active[i];
    }

    // Selected hotbar slot: clamp before storing, since SlotAt indexes a fixed-size array
    // with no bounds checking of its own — a malformed/malicious client packet must never
    // be allowed to set an out-of-range selectedSlot.
    if (latestInputs) {
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (!active[i]) continue;
            int sel = latestInputs[i].selectedSlot;
            if (sel < 0) sel = 0;
            if (sel >= Inventory::kCapacity) sel = Inventory::kCapacity - 1;
            session.slots[i].player.selectedSlot = sel;
        }
    }

    // Pickup: each active, Alive player who isn't currently a valid revive
    // channel target for anyone else may pick up an item. (A player who could
    // instead be revived should channel-revive, not pick up items, mirroring
    // the original 2-player behavior's "!canRevive" gate.)
    //
    // pickedUpThisTick[i] records whether player i's press just picked something up. This
    // matters because interactHeld (which drives pickup) and usePressed (which drives the
    // self-heal loop below) both fire from the same physical E key on the client — without
    // this flag, picking up a potion while below full HP would immediately self-heal it away
    // on the very same press, so the item would never actually land in the hotbar.
    bool pickedUpThisTick[kMaxPlayersPerSession] = {};
    if (interact) {
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (!active[i] || session.slots[i].player.state != PlayerState::Alive || !interact[i]) continue;
            bool isRevivable = false;
            for (int j = 0; j < kMaxPlayersPerSession; j++) {
                if (i == j || !active[j]) continue;
                if (session.slots[j].player.state == PlayerState::Downed &&
                    DistanceBetween(session.slots[i].player.position, session.slots[j].player.position) <= kReviveRange) {
                    isRevivable = true;
                    break;
                }
            }
            if (isRevivable) continue;
            for (auto& item : items) {
                if (TryPickup(item, session.slots[i].player.position, session.slots[i].player.inventory, kPickupRadius)) {
                    pickedUpThisTick[i] = true;
                    break;
                }
            }
        }
    }

    // Hazard zone: only active in RevivePotionTest, which uses the ORIGINAL Classic HP-loss
    // mechanic (not Hot Potato's instant-Down) as its down-trigger, since it has no potato.
    if (mode == GameMode::RevivePotionTest) {
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (active[i]) ApplyHazardDamage(hazard, session.slots[i].player, dt, hazardCarry[i]);
        }
    }

    // 2v2 needs all four slots filled before any potato action or scoring may happen —
    // see HotPotatoGameplayEnabled in MatchState.h for why. While under-populated, the
    // potato is forced inert so no stale held/in-flight state (e.g. the held potato seeded
    // at room creation) is broadcast as if a live round were running; the client renders a
    // "Waiting for 4 players..." state off exactly that condition. FFA and RevivePotionTest
    // behavior is untouched.
    const bool potatoEnabled = HotPotatoGameplayEnabled(mode, active);
    if (mode == GameMode::TwoVTwo && !potatoEnabled) {
        potato.held = false;
        potato.inFlight = false;
        potato.holderSlot = -1;
        potato.velocity = Vector2{ 0.0f, 0.0f };
        potato.justThrown = false;
        for (int i = 0; i < kMaxPlayersPerSession; i++) chargeTimer[i] = 0.0f;
    }

    // The gate just opened (a 4th player joined a 2v2 room that was waiting): the potato
    // was left inert above, so seed a fresh round now. 2v2-only; FFA never reaches here.
    if (potatoEnabled && mode == GameMode::TwoVTwo && !match.matchOver &&
        !potato.held && !potato.inFlight) {
        ResetPotatoForNewRound(potato, session, active, chargeTimer);
    }

    if (potatoEnabled) {
    // --- Hot Potato: charge tracking, throw, flight, catch, explosion ---
    bool soloMode = false;
    {
        int activeCount = 0;
        for (int i = 0; i < kMaxPlayersPerSession; i++) if (active[i]) activeCount++;
        soloMode = (activeCount == 1);
    }

    if (match.matchOver) {
        // Match decided: freeze the potato in whatever state it's in (typically unheld,
        // since the winning round-end already reset it) — no further charge/throw/catch/
        // explosion simulation runs. A new match starts via the debug menu's "New Match"
        // action (see DebugAction::NewMatch), which resets this session's MatchState and
        // respawns the potato via StartNewMatch.
    } else {
    // Charge tracking: accumulate while held, cap at kMaxChargeDuration, reset on release.
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (!active[i]) continue;
        if (potato.held && potato.holderSlot == i && latestInputs[i].chargingThrow) {
            chargeTimer[i] += dt;
            if (chargeTimer[i] > kMaxChargeDuration) chargeTimer[i] = kMaxChargeDuration;
        }
    }

    // Release: only the current holder's release matters.
    if (potato.held && potato.holderSlot >= 0 && active[potato.holderSlot] &&
        latestInputs[potato.holderSlot].releaseThrow) {
        int holder = potato.holderSlot;
        float force = ComputeThrowForce(chargeTimer[holder]);
        Vector2 aim{ latestInputs[holder].aimDirX, latestInputs[holder].aimDirY };
        float aimLen = std::sqrt(aim.x * aim.x + aim.y * aim.y);
        if (aimLen < 0.0001f) { aim = Vector2{1.0f, 0.0f}; aimLen = 1.0f; } // degenerate aim: default to +X
        aim.x /= aimLen;
        aim.y /= aimLen;

        potato.velocity = Vector2{ aim.x * force, aim.y * force };
        potato.held = false;
        potato.inFlight = true;
        potato.lastThrowerSlot = holder;
        potato.holderSlot = -1;
        potato.justThrown = true; // grace flag: exclude the thrower from catch checks while the potato is still within kCatchRadius of them post-release (see the flight block below)
        chargeTimer[holder] = 0.0f;
    }

    // Flight: integrate position, apply drag, check catch, check bounds.
    if (potato.inFlight) {
        potato.position.x += potato.velocity.x * dt;
        potato.position.y += potato.velocity.y * dt;
        ApplyPotatoDrag(potato, dt);

        Vector2 positions[kMaxPlayersPerSession];
        for (int i = 0; i < kMaxPlayersPerSession; i++) positions[i] = session.slots[i].player.position;

        // Same-tick/near-tick self-catch fallback: measured that even a full-force throw
        // takes ~3 ticks at 60Hz to clear kCatchRadius(20) of a stationary thrower (8.3,
        // 16.6, 24.7 units at ticks 0/1/2 post-release), so a single-tick grace flag is
        // not enough — the thrower would still instantly re-catch their own throw one or
        // two ticks later. Instead, exclude the thrower from the catch check for as long
        // as the potato remains within kCatchRadius of the thrower's OWN position (i.e.
        // hasn't actually left their immediate vicinity yet); once it clears that radius
        // even once, clear the flag permanently so the thrower CAN catch their own throw
        // later (e.g. after a solo-mode wall bounce).
        int excludeSlot = -1;
        if (potato.justThrown && potato.lastThrowerSlot != -1 && active[potato.lastThrowerSlot]) {
            float distFromThrower = DistanceBetween2(potato.position, positions[potato.lastThrowerSlot]);
            if (distFromThrower <= kCatchRadius) {
                excludeSlot = potato.lastThrowerSlot;
            } else {
                potato.justThrown = false;
            }
        } else {
            potato.justThrown = false;
        }
        // Only active, Alive players can catch: a Downed or Dead player standing in the
        // flight path must not become the new holder (they can't throw it, which would
        // strand the potato). Filter here rather than changing FindCatchTarget's signature.
        bool catchEligible[kMaxPlayersPerSession];
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            catchEligible[i] = active[i] && session.slots[i].player.state == PlayerState::Alive;
        }
        int catcher = FindCatchTarget(potato.position, positions, catchEligible, kMaxPlayersPerSession, excludeSlot);
        if (catcher != -1) {
            ResolveCatch(potato, catcher, positions[catcher]);
        } else {
            bool outOfBounds = potato.position.x < courtBounds.x || potato.position.x > courtBounds.x + courtBounds.width ||
                                potato.position.y < courtBounds.y || potato.position.y > courtBounds.y + courtBounds.height;
            if (outOfBounds) {
                if (soloMode) {
                    // Reflect off whichever boundary was crossed, clamp back inside.
                    if (potato.position.x < courtBounds.x) { potato.position.x = courtBounds.x; potato.velocity.x = -potato.velocity.x; }
                    if (potato.position.x > courtBounds.x + courtBounds.width) { potato.position.x = courtBounds.x + courtBounds.width; potato.velocity.x = -potato.velocity.x; }
                    if (potato.position.y < courtBounds.y) { potato.position.y = courtBounds.y; potato.velocity.y = -potato.velocity.y; }
                    if (potato.position.y > courtBounds.y + courtBounds.height) { potato.position.y = courtBounds.y + courtBounds.height; potato.velocity.y = -potato.velocity.y; }
                } else if (potato.lastThrowerSlot != -1 && active[potato.lastThrowerSlot]) {
                    // Multiplayer: leaving the court downs the last thrower, scores the
                    // round, and starts a new round (or ends the match).
                    session.slots[potato.lastThrowerSlot].player.ForceDown();
                    if (mode == GameMode::TwoVTwo) {
                        ScoreRoundEndTeam(match, active, potato.lastThrowerSlot);
                        AdvanceRoundOrEndMatchTeam(match);
                    } else {
                        ScoreRoundEnd(match, active, potato.lastThrowerSlot);
                        AdvanceRoundOrEndMatch(match, active);
                    }
                    // Respawn held by the first Alive active player (deterministic slot
                    // order), via the shared ResetPotatoForNewRound helper.
                    ResetPotatoForNewRound(potato, session, active, chargeTimer);
                }
            }
        }
    }

    // Holder vanished (disconnected, downed, or dead): reclaim the potato immediately.
    // Must run BEFORE the explosion countdown, which also gates on active[holderSlot] —
    // otherwise a held potato whose holder went away would freeze forever, its timer
    // never ticking down and never expiring.
    if (potato.held && (potato.holderSlot < 0 || !active[potato.holderSlot] ||
                        session.slots[potato.holderSlot].player.state != PlayerState::Alive)) {
        ResetPotatoForNewRound(potato, session, active, chargeTimer);
    }

    // Explosion: timer expires while held.
    if (potato.held && potato.holderSlot >= 0 && active[potato.holderSlot]) {
        potato.explodeTimer -= dt;
        if (potato.explodeTimer <= 0.0f) {
            int exploderSlot = potato.holderSlot;
            session.slots[exploderSlot].player.ForceDown();
            if (mode == GameMode::TwoVTwo) {
                ScoreRoundEndTeam(match, active, exploderSlot);
                AdvanceRoundOrEndMatchTeam(match);
            } else {
                ScoreRoundEnd(match, active, exploderSlot);
                AdvanceRoundOrEndMatch(match, active);
            }
            ResetPotatoForNewRound(potato, session, active, chargeTimer);
        }
    }
    }

    // Held potato tracks its holder's position each tick.
    if (potato.held && potato.holderSlot >= 0 && active[potato.holderSlot]) {
        potato.position = session.slots[potato.holderSlot].player.position;
    }
    }

    // Revive: each active reviver channels against AT MOST ONE target per tick — the
    // lowest-slot-indexed Downed candidate that UpdateRevive actually makes progress
    // against. channelTimer lives on the reviver, not on the (reviver, target) pair, so
    // calling UpdateRevive again for the same reviver against a different, non-valid
    // target would reset the timer to 0 and undo that progress within the same tick
    // (harmless with exactly 2 active players, fatal with 3+). Breaking on progress
    // prevents that; the explicit reset below preserves the original "no valid target
    // resets the timer" behavior, which is UpdateRevive's only observable effect when
    // canProgress is false.
    //
    // revivedThisTick[i] records whether UpdateRevive returned true for reviver i this
    // tick — i.e. the channel actually COMPLETED. This matters because UpdateRevive zeroes
    // channelTimer on BOTH of its exits ("no progress possible" and "revive completed"), so
    // channelTimer == 0.0f alone cannot tell those two cases apart. The self-heal loop below
    // needs that distinction; the return value is the only thing that carries it.
    bool revivedThisTick[kMaxPlayersPerSession] = {};
    if (interact) {
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (!active[i]) continue;
            bool channeling = false;
            for (int j = 0; j < kMaxPlayersPerSession; j++) {
                if (i == j || !active[j]) continue;
                if (session.slots[j].player.state != PlayerState::Downed) continue;
                bool potionSelected = session.slots[i].player.inventory.SlotAt(session.slots[i].player.selectedSlot).count > 0 &&
                                      session.slots[i].player.inventory.SlotAt(session.slots[i].player.selectedSlot).type == ItemType::RevivePotion;
                if (UpdateRevive(session.slots[i].player, session.slots[j].player, interact[i] && potionSelected, dt, kReviveRange)) {
                    revivedThisTick[i] = true;
                    // The revive completed, which consumed the channel: stop scanning further
                    // targets for this reviver so a later non-valid target's canProgress==false
                    // path can't clobber state for a channel that already resolved.
                    break;
                }
                if (session.slots[i].player.channelTimer > 0.0f) { channeling = true; break; }
            }
            if (!channeling && !revivedThisTick[i]) session.slots[i].player.channelTimer = 0.0f;
        }
    }

    // Instant self-heal: fires once per FRESH press of use (usePressed is edge-triggered
    // client-side, see InputMsg), only when the selected slot holds RevivePotion AND the
    // player did NOT make revive-channel progress this tick (i.e., no one was revivable in
    // range — if someone WAS revivable, the press should have gone toward the channel, not
    // a self-heal; checking channelTimer > 0.0f after the revive loop tells us which case we
    // are in, since UpdateRevive resets it to 0 whenever it made no progress against anyone
    // this tick, per its own existing logic already exercised above) — EXCEPT for the tick a
    // revive actually completes, which also leaves channelTimer at 0; revivedThisTick[i]
    // covers that case, so one E-press can't pay for both a completed revive and a self-heal.
    //
    // usePressed[] is the server's latch of the last-received InputMsg's edge-triggered flag.
    // It is DRAINED here (set back to false) the moment it is observed true, so a single
    // physical press fires at most one self-heal even when several server ticks elapse before
    // the next input packet arrives (routine under unreliable UDP + framerate drift). Without
    // the drain, one press would re-fire every tick until a fresh packet overwrote the latch.
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (!usePressed || !usePressed[i]) continue;
        usePressed[i] = false; // consume the edge: one press, one chance to act
        if (!active[i]) continue;
        Player& p = session.slots[i].player;
        if (p.state != PlayerState::Alive) continue;
        if (p.channelTimer > 0.0f) continue; // was channeling a revive this tick instead
        if (revivedThisTick[i]) continue;    // a revive completed this tick; press is spent
        if (pickedUpThisTick[i]) continue;   // this press just picked up an item; don't also spend it on a heal
        const InventorySlot& sel = p.inventory.SlotAt(p.selectedSlot);
        if (sel.count > 0 && sel.type == ItemType::RevivePotion) {
            if (p.inventory.Remove(ItemType::RevivePotion, 1)) {
                p.TryHeal(kSelfHealAmount);
            }
        }
    }

    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (active[i]) session.slots[i].player.UpdateTimers(dt);
    }

    // Dash cooldown decays unconditionally for every active player, independent of
    // match/potato state — it's a player-movement mechanic, not something that should
    // freeze when a match concludes.
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (active[i] && session.slots[i].player.dashCooldownTimer > 0.0f) {
            session.slots[i].player.dashCooldownTimer -= dt;
            if (session.slots[i].player.dashCooldownTimer < 0.0f) session.slots[i].player.dashCooldownTimer = 0.0f;
        }
    }

    // Advance any in-progress dash by this tick's dt, sliding position from dashStartPos
    // toward dashTargetPos instead of having already snapped there (see Player::AdvanceDash /
    // Dash.h's TryApplyDash). Also unconditional, same reasoning as the cooldown decay above.
    //
    // Swept dash-vs-potato catch: this used to be a single sweep of the whole ~150px dash
    // distance, done once at the moment the dash was applied (when it was still an instant
    // teleport). Now that the dash is spread across several ticks, the equivalent check is
    // this tick's much shorter sub-segment (from before AdvanceDash to after it) — still
    // swept rather than point-sampled, so a fast-moving dash segment still can't tunnel past
    // the potato between two point-samples, it just does so in smaller steps now.
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (!active[i]) continue;
        Player& p = session.slots[i].player;
        if (!p.IsDashing()) continue;

        auto [subSegStart, subSegEnd] = p.AdvanceDash(dt);
        if (mode == GameMode::RevivePotionTest) continue; // no potato in this mode

        // Thrower grace: same rule as the tick loop's own catch check — the thrower can't
        // re-catch while the potato hasn't yet cleared kCatchRadius of them post-release.
        bool graced = potato.justThrown && potato.lastThrowerSlot == i &&
                      DistanceBetween2(potato.position, subSegStart) <= kCatchRadius;
        if (potato.inFlight && !graced &&
            DashSegmentCatchesPotato(subSegStart, subSegEnd, potato.position)) {
            ResolveCatch(potato, i, subSegEnd);
        }
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--test") {
        RunAllSmokeTests();
        return 0;
    }
    std::srand((unsigned int)std::time(nullptr));

    UdpSocket socket;
    if (!socket.Bind(kServerPort)) {
        std::printf("Failed to bind UDP port %d\n", (int)kServerPort);
        return 1;
    }
    std::printf("Server listening on UDP port %d\n", (int)kServerPort);

    SessionManager sessionManager;
    // Shared by every session; damage is applied per-mode inside SimulateSessionTick —
    // only RevivePotionTest sessions take hazard damage (it uses the Classic HP-loss
    // down-trigger). FFA and 2v2 sessions are still SENT the zone for rendering, but
    // standing in it costs them nothing.
    HazardZone hazard{ Rectangle{450.0f, 200.0f, 100.0f, 200.0f} };
    Rectangle courtBounds{ 0.0f, 0.0f, 1000.0f, 600.0f };

    // Track world items and unreliable-send sequence counters per session name,
    // since Session itself only holds player slots.
    std::map<std::string, std::vector<WorldItem>> sessionWorldItems;
    std::map<std::string, float[kMaxPlayersPerSession]> sessionHazardCarry;
    std::map<std::string, uint32_t> sessionSnapshotSeq;
    std::map<std::string, bool[kMaxPlayersPerSession]> pendingAttack;
    std::map<std::string, bool[kMaxPlayersPerSession]> pendingInteract;
    std::map<std::string, bool[kMaxPlayersPerSession]> pendingUse;
    std::map<std::string, HotPotato> sessionPotato;
    std::map<std::string, float[kMaxPlayersPerSession]> sessionChargeTimer;
    std::map<std::string, MatchState> sessionMatch;
    std::map<std::string, InputMsg[kMaxPlayersPerSession]> sessionLatestInput;
    std::map<std::string, GameMode> sessionGameMode;

    const double tickInterval = 1.0 / 60.0;
    double lastTick = NowSeconds();

    uint8_t recvBuffer[1024];

    while (true) {
        double now = NowSeconds();

        // --- Receive and dispatch incoming packets ---
        // Drain every packet waiting in the OS socket buffer each pass, not just one:
        // with the 1ms sleep below, a single-packet-per-pass loop falls behind under
        // the input rate of two connected clients, growing input lag over time.
        std::string fromIp;
        uint16_t fromPort;
        int received;
        while ((received = socket.ReceiveFrom(recvBuffer, sizeof(recvBuffer), fromIp, fromPort)) > (int)sizeof(PacketHeader)) {
            PacketHeader header{};
            DeserializeStruct(recvBuffer, received, header);
            const uint8_t* payload = recvBuffer + sizeof(PacketHeader);
            size_t payloadLen = (size_t)received - sizeof(PacketHeader);

            if (payloadLen >= 1) {
                MessageType type = (MessageType)payload[0];
                const uint8_t* body = payload + 1;
                size_t bodyLen = payloadLen - 1;

                if (header.channel == 1 && type == MessageType::ConnectRequest) {
                    ConnectRequestMsg msg{};
                    if (DeserializeStruct(body, bodyLen, msg)) {
                        std::string sessionName(msg.sessionName);
                        ConnectOutcome outcome = sessionManager.HandleConnect(sessionName, msg.reconnectToken, fromIp, fromPort, now);

                        if (outcome.result == ConnectResult::Rejected) {
                            RejectedMsg reject{ outcome.rejectReason };
                            std::vector<uint8_t> rejectBytes;
                            SerializeStruct(reject, rejectBytes);
                            std::vector<uint8_t> full;
                            full.push_back((uint8_t)MessageType::Rejected);
                            full.insert(full.end(), rejectBytes.begin(), rejectBytes.end());
                            PacketHeader rejectHeader{ 1, 0, (uint16_t)full.size() };
                            std::vector<uint8_t> packet;
                            SerializeStruct(rejectHeader, packet);
                            packet.insert(packet.end(), full.begin(), full.end());
                            socket.SendTo(fromIp, fromPort, packet.data(), packet.size());
                        } else {
                            Session* session = sessionManager.GetSession(outcome.roomCode);
                            if (sessionWorldItems.find(outcome.roomCode) == sessionWorldItems.end()) {
                                if (msg.requestedMode == GameMode::TwoVTwo) {
                                    // 2v2: revive items spawn OUTSIDE the court, so fetching one
                                    // costs a teammate real time away from the potato action.
                                    sessionWorldItems[outcome.roomCode] = {
                                        WorldItem{ Vector2{-50.0f, 300.0f}, ItemType::RevivePotion, true },  // just left of the court
                                        WorldItem{ Vector2{1050.0f, 300.0f}, ItemType::RevivePotion, true }, // just right of the court
                                    };
                                } else {
                                    // FFA and RevivePotionTest: original in-arena positions.
                                    sessionWorldItems[outcome.roomCode] = {
                                        WorldItem{ Vector2{300.0f, 500.0f}, ItemType::RevivePotion, true },
                                        WorldItem{ Vector2{700.0f, 100.0f}, ItemType::RevivePotion, true },
                                    };
                                }
                                for (int i = 0; i < kMaxPlayersPerSession; i++) {
                                    sessionHazardCarry[outcome.roomCode][i] = 0.0f;
                                    sessionChargeTimer[outcome.roomCode][i] = 0.0f;
                                    sessionLatestInput[outcome.roomCode][i] = InputMsg{};
                                }
                                sessionSnapshotSeq[outcome.roomCode] = 1;
                                HotPotato freshPotato{};
                                if (msg.requestedMode == GameMode::FFA) {
                                    freshPotato.held = true;
                                    freshPotato.holderSlot = 0; // slot 0 (the room creator) always starts holding; simplest deterministic choice for this phase
                                    freshPotato.position = session->slots[0].player.position;
                                    freshPotato.explodeTimer = ComputeExplodeTimerForCatch(0);
                                } else {
                                    // TwoVTwo and RevivePotionTest start with an INERT potato
                                    // (held=false, inFlight=false, holderSlot=-1, the HotPotato{}
                                    // defaults). RevivePotionTest has no potato at all, and 2v2
                                    // must not look like a live round until four players are
                                    // connected — the tick seeds 2v2's real potato once the
                                    // population gate opens. Snapshots therefore never claim a
                                    // held potato exists in a mode/state where one doesn't.
                                }
                                sessionPotato[outcome.roomCode] = freshPotato;
                                sessionMatch[outcome.roomCode] = MatchState{};
                                sessionGameMode[outcome.roomCode] = msg.requestedMode;
                            }

                            WelcomeMsg welcome{
                                (uint8_t)outcome.slotIndex, outcome.sessionToken,
                                Player::kMaxHp, Player::kDownedDuration, Player::kDeathRespawnDelay,
                                Player::kReviveHp, Player::kRespawnHp, Player::kAttackCooldown,
                                Player::kAttackRange, Player::kAttackDamage, Player::kChannelDuration,
                                kHazardDamagePerSecond, Inventory::kCapacity, {},
                                // Explicit defensive fallback, matching the tick loop's
                                // `.count(...) ? ... : GameMode::FFA` idiom: the creation block
                                // above always populates this entry first, so a miss should be
                                // impossible — but read it the same explicit way in both places
                                // rather than letting operator[] silently insert a default.
                                sessionGameMode.count(outcome.roomCode)
                                    ? sessionGameMode[outcome.roomCode] : GameMode::FFA
                            };
                            std::strncpy(welcome.roomCode, outcome.roomCode.c_str(), sizeof(welcome.roomCode) - 1);
                            PlayerSlot& slot = session->slots[outcome.slotIndex];
                            std::vector<uint8_t> welcomeBytes;
                            SerializeStruct(welcome, welcomeBytes);
                            SendReliable(socket, slot, MessageType::Welcome, welcomeBytes.data(), welcomeBytes.size(), now);
                        }
                    }
                } else {
                    std::vector<std::string> sessionNames = sessionManager.GetSessionNames();
                    ClientLocation loc = FindClientByAddress(sessionManager, sessionNames, fromIp, fromPort);
                    if (loc.found) {
                        Session* session = sessionManager.GetSession(loc.sessionName);
                        PlayerSlot& slot = session->slots[loc.slotIndex];
                        slot.lastPacketAtSeconds = now;

                        if (header.channel == 0 && type == MessageType::Input) {
                            InputMsg input{};
                            if (DeserializeStruct(body, bodyLen, input)) {
                                if (slot.player.state == PlayerState::Alive) {
                                    slot.player.position.x += input.moveX * kMoveSpeed * (float)tickInterval;
                                    slot.player.position.y += input.moveY * kMoveSpeed * (float)tickInterval;
                                    // Ordinary movement is clamped to the arena too, so dash
                                    // (which has always clamped) isn't an unintended
                                    // "return to arena" button for an out-of-bounds walker.
                                    slot.player.position = ClampToCourtBounds(slot.player.position, courtBounds);

                                    // Facing-direction tracking, cooldown gating and the dash
                                    // destination computation all live in TryApplyDash (Dash.h) —
                                    // one implementation shared by production and smoke tests. The
                                    // actual position change is NOT applied here: TryApplyDash calls
                                    // Player::StartDash, which spreads the travel over
                                    // Player::kDashDuration, advanced tick-by-tick in
                                    // SimulateSessionTick (see the dash-cooldown/AdvanceDash loop
                                    // there) so the dash reads as a fast slide instead of a
                                    // teleport. The swept dash-vs-potato catch check that used to
                                    // live here (immediately after a one-step teleport) now lives
                                    // in that same per-tick loop, since that's where the dash's
                                    // sub-segment of travel for each tick actually happens.
                                    TryApplyDash(slot.player, Vector2{ input.moveX, input.moveY },
                                                 input.dashPressed, courtBounds);
                                }
                                pendingAttack[loc.sessionName][loc.slotIndex] = input.attackPressed;
                                pendingInteract[loc.sessionName][loc.slotIndex] = input.interactHeld;
                                pendingUse[loc.sessionName][loc.slotIndex] = input.usePressed;
                                sessionLatestInput[loc.sessionName][loc.slotIndex] = input;
                            }
                        } else if (header.channel == 1 && type == MessageType::Ack) {
                            AckMsg ack{};
                            if (DeserializeStruct(body, bodyLen, ack)) {
                                slot.reliableSender.OnAckReceived(ack.ackedSeq);
                            }
                        } else if (header.channel == 1 && type == MessageType::DebugActionRequest) {
                            DebugActionRequestMsg req{};
                            if (DeserializeStruct(body, bodyLen, req)) {
                                std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ready;
                                slot.reliableReceiver.TryDeliverInOrder(header.seq, std::vector<uint8_t>(body, body + bodyLen), ready);
                                for (auto& msg : ready) {
                                    DebugActionRequestMsg deliveredReq{};
                                    if (DeserializeStruct(msg.second.data(), msg.second.size(), deliveredReq)) {
                                        if (deliveredReq.action == DebugAction::NewMatch) {
                                            // Session-scoped action: resets this room's MatchState and
                                            // respawns the potato. targetSlot is ignored by design.
                                            // No-op for RevivePotionTest, which has no match, round
                                            // or potato concept — "start a new match" is meaningless
                                            // there and would only seed a bogus held potato.
                                            GameMode sessionMode = sessionGameMode.count(loc.sessionName)
                                                ? sessionGameMode[loc.sessionName] : GameMode::FFA;
                                            if (sessionMode != GameMode::RevivePotionTest) {
                                                StartNewMatch(*session, sessionMatch[loc.sessionName],
                                                              sessionPotato[loc.sessionName],
                                                              sessionChargeTimer[loc.sessionName],
                                                              sessionMode);
                                            }
                                        } else if (deliveredReq.targetSlot < kMaxPlayersPerSession) {
                                            ApplyDebugAction(session->slots[deliveredReq.targetSlot].player, deliveredReq.action);
                                        }
                                    }
                                }
                                AckMsg ackReply{ header.seq };
                                std::vector<uint8_t> ackBytes;
                                SerializeStruct(ackReply, ackBytes);
                                std::vector<uint8_t> ackFull;
                                ackFull.push_back((uint8_t)MessageType::Ack);
                                ackFull.insert(ackFull.end(), ackBytes.begin(), ackBytes.end());
                                PacketHeader ackHeader{ 1, 0, (uint16_t)ackFull.size() };
                                std::vector<uint8_t> ackPacket;
                                SerializeStruct(ackHeader, ackPacket);
                                ackPacket.insert(ackPacket.end(), ackFull.begin(), ackFull.end());
                                socket.SendTo(slot.clientIp, slot.clientPort, ackPacket.data(), ackPacket.size());
                            }
                        }
                    }
                }
            }
        }

        // --- Fixed 60Hz simulation tick ---
        now = NowSeconds();
        if (now - lastTick >= tickInterval) {
            float dt = (float)(now - lastTick);
            lastTick = now;
            sessionManager.CheckAllTimeouts(now);

            for (auto& sessionEntry : sessionManager.GetSessionNames()) {
                Session* session = sessionManager.GetSession(sessionEntry);
                if (!session) continue;

                std::vector<WorldItem>& items = sessionWorldItems[sessionEntry];
                float* hazardCarry = sessionHazardCarry[sessionEntry];
                bool* attack = pendingAttack.count(sessionEntry) ? pendingAttack[sessionEntry] : nullptr;
                bool* interact = pendingInteract.count(sessionEntry) ? pendingInteract[sessionEntry] : nullptr;
                bool* usePressed = pendingUse.count(sessionEntry) ? pendingUse[sessionEntry] : nullptr;

                bool active[kMaxPlayersPerSession];
                HotPotato& potato = sessionPotato[sessionEntry];
                float* chargeTimer = sessionChargeTimer[sessionEntry];
                InputMsg* latestInputs = sessionLatestInput.count(sessionEntry) ? sessionLatestInput[sessionEntry] : nullptr;
                if (!latestInputs) continue; // session exists but no input map yet (shouldn't happen once creation-time init runs, but guards a null deref)
                MatchState& match = sessionMatch[sessionEntry];
                GameMode mode = sessionGameMode.count(sessionEntry) ? sessionGameMode[sessionEntry] : GameMode::FFA;
                SimulateSessionTick(*session, items, hazard, hazardCarry, attack, interact, usePressed, dt, active, potato, chargeTimer, latestInputs, courtBounds, match, mode);

                // state value 3 = "absent" (slot not Connected): not a real PlayerState,
                // repurposed on the wire so an inactive slot renders as not-present
                // instead of a fully-visible phantom player, without changing the
                // PlayerSnapshot layout.
                static constexpr uint8_t kSnapshotStateAbsent = 3;

                SnapshotMsg snap{};
                for (int i = 0; i < kMaxPlayersPerSession; i++) {
                    Player& p = session->slots[i].player;
                    PlayerSnapshot ps{ p.position.x, p.position.y, p.hp, active[i] ? (uint8_t)p.state : kSnapshotStateAbsent, {}, p.channelTimer };
                    for (int s = 0; s < Inventory::kCapacity; s++) {
                        const InventorySlot& slot = p.inventory.SlotAt(s);
                        ps.slots[s] = HotbarSlotSnapshot{ (uint8_t)slot.type, slot.count };
                    }
                    snap.players[i] = ps;
                }
                for (int i = 0; i < 2 && i < (int)items.size(); i++) {
                    snap.items[i] = WorldItemSnapshot{ items[i].position.x, items[i].position.y, items[i].active };
                }
                snap.hazardX = hazard.bounds.x;
                snap.hazardY = hazard.bounds.y;
                snap.hazardW = hazard.bounds.width;
                snap.hazardH = hazard.bounds.height;
                snap.potato = PotatoSnapshot{ potato.position.x, potato.position.y, potato.held, potato.inFlight, potato.holderSlot, potato.explodeTimer };

                MatchSnapshot matchSnap{};
                matchSnap.roundNumber = match.roundNumber;
                for (int i = 0; i < kMaxPlayersPerSession; i++) matchSnap.roundScore[i] = match.roundScore[i];
                matchSnap.matchOver = match.matchOver;
                matchSnap.winnerSlot = match.winnerSlot;
                matchSnap.inTiebreak = match.inTiebreak;
                matchSnap.teamScore[0] = match.teamScore[0];
                matchSnap.teamScore[1] = match.teamScore[1];
                snap.match = matchSnap;

                std::vector<uint8_t> snapBytes;
                SerializeStruct(snap, snapBytes);
                uint32_t seq = sessionSnapshotSeq[sessionEntry]++;
                for (int i = 0; i < kMaxPlayersPerSession; i++) {
                    if (session->slots[i].state == SlotState::Connected) {
                        SendUnreliable(socket, session->slots[i].clientIp, session->slots[i].clientPort, seq, MessageType::Snapshot, snapBytes.data(), snapBytes.size());
                    }
                }

                // Retransmit any unacked reliable messages for this session's slots
                for (int i = 0; i < kMaxPlayersPerSession; i++) {
                    if (session->slots[i].state != SlotState::Connected) continue;
                    auto toResend = session->slots[i].reliableSender.GetMessagesToRetransmit(now, 0.1);
                    for (auto& msg : toResend) {
                        PacketHeader resendHeader{ 1, msg.first, (uint16_t)msg.second.size() };
                        std::vector<uint8_t> packet;
                        SerializeStruct(resendHeader, packet);
                        packet.insert(packet.end(), msg.second.begin(), msg.second.end());
                        socket.SendTo(session->slots[i].clientIp, session->slots[i].clientPort, packet.data(), packet.size());
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
