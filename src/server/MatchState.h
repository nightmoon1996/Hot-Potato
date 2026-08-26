#pragma once

#include "../shared/Protocol.h"

struct MatchState {
    int roundNumber = 1; // 1-indexed; 1..kRoundsPerMatch during normal play, stays at kRoundsPerMatch+something conceptually once in tiebreak (tiebreak rounds don't increment past the cap the same way — see AdvanceRoundOrEndMatch)
    int roundScore[kMaxPlayersPerSession] = {0, 0, 0, 0};
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
