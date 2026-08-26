#pragma once

#include "../shared/Protocol.h"

// Fixed team assignment for 2v2: slots 0+1 = Team A (0), slots 2+3 = Team B (1).
inline int TeamForSlot(int slot) {
    return (slot < 2) ? 0 : 1;
}

struct MatchState {
    int roundNumber = 1; // 1-indexed; 1..kRoundsPerMatch during normal play, stays at kRoundsPerMatch+something conceptually once in tiebreak (tiebreak rounds don't increment past the cap the same way — see AdvanceRoundOrEndMatch)
    int roundScore[kMaxPlayersPerSession] = {0, 0, 0, 0};
    int teamScore[2] = {0, 0}; // 2v2 only: teamScore[0] = Team A (slots 0-1), teamScore[1] = Team B (slots 2-3)
    bool matchOver = false;
    int winnerSlot = -1; // -1 until matchOver is true and exactly one winner is determined
    bool inTiebreak = false;
    bool tiebreakEligible[kMaxPlayersPerSession] = {false, false, false, false}; // which slots are still contending in the current tiebreak; irrelevant when inTiebreak is false
};

// Credits every active player except `excludedSlot` (the round-end's cause: the exploded
// holder, or the out-of-bounds last-thrower) with +1 round score. If a tiebreak is active,
// only slots marked `tiebreakEligible` are credited or excluded from — non-eligible slots
// (already-eliminated from contention, or never-tied players) never have their score
// touched during a tiebreak, since tiebreak scoring only needs to resolve who among the
// originally-tied set pulls ahead first.
inline void ScoreRoundEnd(MatchState& match, const bool* active, int excludedSlot) {
    for (int i = 0; i < kMaxPlayersPerSession; i++) {
        if (!active[i] || i == excludedSlot) continue;
        if (match.inTiebreak && !match.tiebreakEligible[i]) continue;
        match.roundScore[i] += 1;
    }
}

// Called once per round-end, AFTER ScoreRoundEnd, BEFORE the next round's potato spawns.
// Advances the round counter and, once kRoundsPerMatch normal rounds are complete (or a
// tiebreak round has just resolved), determines whether the match is over.
inline void AdvanceRoundOrEndMatch(MatchState& match, const bool* active) {
    if (match.matchOver) return; // no-op once decided; guards against a stray extra call

    if (match.inTiebreak) {
        // Resolve the tiebreak: among tiebreakEligible slots, find the strict max of
        // roundScore. If exactly one holds it, the match ends. If 2+ still tie, stay in
        // tiebreak, narrow tiebreakEligible to just the still-tied slots, and play another
        // tiebreak round (roundNumber is not meaningfully incremented further during
        // tiebreak rounds; kept fixed for HUD display purposes at kRoundsPerMatch + 1).
        int maxScore = -1;
        int maxCount = 0;
        int maxSlot = -1;
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (!active[i] || !match.tiebreakEligible[i]) continue;
            if (match.roundScore[i] > maxScore) {
                maxScore = match.roundScore[i];
                maxCount = 1;
                maxSlot = i;
            } else if (match.roundScore[i] == maxScore) {
                maxCount++;
            }
        }
        if (maxCount == 1) {
            match.matchOver = true;
            match.winnerSlot = maxSlot;
        } else if (maxCount == 0) {
            // All tiebreak-eligible players vanished (e.g. every one of them disconnected)
            // while some other non-eligible active player kept triggering scored round-ends.
            // Nobody is left to determine a winner among, so end the match rather than
            // wedging inTiebreak forever with no resolution path.
            match.matchOver = true;
            // winnerSlot stays -1 (its default) -- the client already renders this as "no winner."
        } else {
            // Still tied among 2+: narrow eligibility to just the tied slots and continue.
            for (int i = 0; i < kMaxPlayersPerSession; i++) {
                match.tiebreakEligible[i] = active[i] && match.roundScore[i] == maxScore;
            }
        }
        return;
    }

    match.roundNumber += 1;
    if (match.roundNumber > kRoundsPerMatch) {
        // Normal rounds complete: find the strict max among all active players.
        int maxScore = -1;
        int maxCount = 0;
        int maxSlot = -1;
        for (int i = 0; i < kMaxPlayersPerSession; i++) {
            if (!active[i]) continue;
            if (match.roundScore[i] > maxScore) {
                maxScore = match.roundScore[i];
                maxCount = 1;
                maxSlot = i;
            } else if (match.roundScore[i] == maxScore) {
                maxCount++;
            }
        }
        if (maxCount == 1) {
            match.matchOver = true;
            match.winnerSlot = maxSlot;
        } else if (maxCount >= 2) {
            match.inTiebreak = true;
            for (int i = 0; i < kMaxPlayersPerSession; i++) {
                match.tiebreakEligible[i] = active[i] && match.roundScore[i] == maxScore;
            }
        }
        // maxCount == 0 (no active players at all): match simply doesn't resolve; leave
        // matchOver false. This is an edge case with no active players to declare a winner
        // among — not expected in practice but must not crash.
    }
}

// 2v2 variant of ScoreRoundEnd: credits the NON-losing team's teamScore, not individual
// players. `excludedSlot` is the round's cause (exploded holder / out-of-bounds thrower);
// the team OPPOSITE that player's team gets +1. Tiebreak eligibility (when inTiebreak) is
// tracked per-team here rather than per-slot — reuses the same tiebreakEligible[4] array,
// but only ever needs indices 0/1 meaningfully populated for a 2-team tiebreak (2v2 never
// has more than 2 contending parties, so the richer per-slot narrowing FFA's tiebreak needs
// is unnecessary complexity here; a direct 2-team comparison suffices).
inline void ScoreRoundEndTeam(MatchState& match, const bool* active, int excludedSlot) {
    if (excludedSlot < 0 || excludedSlot >= kMaxPlayersPerSession) return;
    int losingTeam = TeamForSlot(excludedSlot);
    int winningTeam = 1 - losingTeam;
    match.teamScore[winningTeam] += 1;
}

// 2v2 variant of AdvanceRoundOrEndMatch: exactly 2 teams, so a tie is always a straight
// 2-way tie — no eligibility-narrowing tiebreak loop is needed (that complexity in the FFA
// path exists to handle 3+ tied parties, which cannot happen with exactly 2 teams). The
// very next round-end's scoring (which team gets credited) IS the tiebreak resolution.
inline void AdvanceRoundOrEndMatchTeam(MatchState& match) {
    if (match.matchOver) return;

    if (match.inTiebreak) {
        // Any single round-end during a tiebreak immediately decides it: whichever team
        // just scored (i.e. now leads) wins outright, since with 2 teams "still tied" after
        // a scoring round-end is impossible (only one team's score can change per round-end).
        if (match.teamScore[0] != match.teamScore[1]) {
            match.matchOver = true;
            match.winnerSlot = (match.teamScore[0] > match.teamScore[1]) ? 0 : 1; // stores the WINNING TEAM index, not a player slot, for 2v2's HUD to interpret
        }
        return;
    }

    match.roundNumber += 1;
    if (match.roundNumber > kRoundsPerMatch) {
        if (match.teamScore[0] != match.teamScore[1]) {
            match.matchOver = true;
            match.winnerSlot = (match.teamScore[0] > match.teamScore[1]) ? 0 : 1;
        } else {
            match.inTiebreak = true;
        }
    }
}
