#pragma once

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "RoomMenu.h"

inline void RunRoomMenuSmokeTests() {
    // Test 1: digits are appended
    {
        std::string buffer;
        RoomMenu::AppendDigitInput(buffer, '1');
        RoomMenu::AppendDigitInput(buffer, '2');
        RoomMenu::AppendDigitInput(buffer, '3');
        if (buffer != "123") {
            printf("FAIL: expected buffer '123', got '%s'\n", buffer.c_str());
            exit(1);
        }
        printf("PASS: digits are appended in order\n");
    }

    // Test 2: non-digit characters are ignored
    {
        std::string buffer;
        RoomMenu::AppendDigitInput(buffer, 'a');
        RoomMenu::AppendDigitInput(buffer, '5');
        RoomMenu::AppendDigitInput(buffer, '!');
        if (buffer != "5") {
            printf("FAIL: expected buffer '5' (non-digits ignored), got '%s'\n", buffer.c_str());
            exit(1);
        }
        printf("PASS: non-digit characters are ignored\n");
    }

    // Test 3: input is capped at 6 characters
    {
        std::string buffer;
        for (int i = 0; i < 10; i++) {
            RoomMenu::AppendDigitInput(buffer, '0' + (i % 10));
        }
        if (buffer.size() != 6) {
            printf("FAIL: expected buffer capped at 6 chars, got size %zu\n", buffer.size());
            exit(1);
        }
        if (buffer != "012345") {
            printf("FAIL: expected first 6 digits '012345', got '%s'\n", buffer.c_str());
            exit(1);
        }
        printf("PASS: input is capped at 6 characters, extras dropped\n");
    }

    // Test 4: mode cycling: FFA -> 2v2 -> RevivePotionTest -> FFA (wraps around)
    {
        assert(RoomMenu::NextMode(GameMode::FFA) == GameMode::TwoVTwo);
        assert(RoomMenu::NextMode(GameMode::TwoVTwo) == GameMode::RevivePotionTest);
        assert(RoomMenu::NextMode(GameMode::RevivePotionTest) == GameMode::FFA);
        printf("PASS: mode cycling wraps FFA -> 2v2 -> RevivePotionTest -> FFA\n");
    }

    printf("All RoomMenu smoke tests passed.\n");
}
