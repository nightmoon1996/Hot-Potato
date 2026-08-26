#pragma once

#include <raylib-cpp.hpp>
#include <string>
#include <cstdint>
#include "NetClient.h"

class RoomMenu {
public:
    bool IsDone() const { return done; }

    void Draw() {
        DrawText("MAXION TEST", 380, 100, 32, BLACK);

        DrawRectangleRec(modeButton, LIGHTGRAY);
        DrawRectangleLinesEx(modeButton, 1, DARKGRAY);
        DrawText(ModeLabel(selectedMode), (int)modeButton.x + 5, (int)modeButton.y + 8, 12, BLACK);

        DrawRectangleRec(createButton, LIGHTGRAY);
        DrawRectangleLinesEx(createButton, 1, DARKGRAY);
        DrawText("Create Room", (int)createButton.x + 20, (int)createButton.y + 12, 18, BLACK);

        DrawText("Enter 6-digit code:", 380, 300, 16, BLACK);
        DrawRectangleRec(codeBox, RAYWHITE);
        DrawRectangleLinesEx(codeBox, 1, DARKGRAY);
        DrawText(codeInput.c_str(), (int)codeBox.x + 10, (int)codeBox.y + 8, 20, BLACK);

        DrawRectangleRec(joinButton, LIGHTGRAY);
        DrawRectangleLinesEx(joinButton, 1, DARKGRAY);
        DrawText("Join Room", (int)joinButton.x + 20, (int)joinButton.y + 12, 18, BLACK);

        if (!errorMessage.empty()) {
            DrawText(errorMessage.c_str(), 380, 420, 16, RED);
        }
    }

    void HandleInput(NetClient& netClient, const std::string& serverIp, uint16_t serverPort) {
        // Digit-only text entry into codeInput, capped at 6 characters
        int key = GetCharPressed();
        while (key > 0) {
            AppendDigitInput(codeInput, key);
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !codeInput.empty()) {
            codeInput.pop_back();
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), modeButton)) {
            selectedMode = NextMode(selectedMode);
        }

        bool createClicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), createButton);
        bool joinClicked = (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), joinButton)) ||
                            (IsKeyPressed(KEY_ENTER) && codeInput.size() == 6);

        if (createClicked) {
            errorMessage.clear();
            if (netClient.CreateRoom(serverIp, serverPort, selectedMode)) {
                done = true;
            } else {
                errorMessage = RejectReasonToMessage(netClient.GetLastRejectReason());
            }
        } else if (joinClicked && codeInput.size() == 6) {
            errorMessage.clear();
            if (netClient.JoinRoom(serverIp, serverPort, codeInput)) {
                done = true;
            } else {
                errorMessage = RejectReasonToMessage(netClient.GetLastRejectReason());
            }
        }
    }

    // Pure digit-filtering logic, extracted so it's testable without a raylib context.
    static void AppendDigitInput(std::string& buffer, int charCode) {
        if (charCode >= '0' && charCode <= '9' && buffer.size() < 6) {
            buffer += (char)charCode;
        }
    }

    // Pure mode-cycling logic, extracted so it's testable without a raylib context
    // (mirrors AppendDigitInput's public-static-for-testability pattern above).
    static const char* ModeLabel(GameMode mode) {
        switch (mode) {
            case GameMode::FFA: return "Mode: FFA (click to cycle)";
            case GameMode::TwoVTwo: return "Mode: 2v2 (click to cycle)";
            case GameMode::RevivePotionTest: return "Mode: Revive Test (click to cycle)";
            default: return "Mode: ?";
        }
    }

    static GameMode NextMode(GameMode mode) {
        switch (mode) {
            case GameMode::FFA: return GameMode::TwoVTwo;
            case GameMode::TwoVTwo: return GameMode::RevivePotionTest;
            case GameMode::RevivePotionTest: return GameMode::FFA;
            default: return GameMode::FFA;
        }
    }

private:
    std::string codeInput;
    std::string errorMessage;
    bool done = false;
    GameMode selectedMode = GameMode::FFA;

    Rectangle modeButton{ 400.0f, 140.0f, 200.0f, 30.0f };
    Rectangle createButton{ 400.0f, 190.0f, 200.0f, 40.0f };
    Rectangle codeBox{ 400.0f, 330.0f, 200.0f, 30.0f };
    Rectangle joinButton{ 400.0f, 370.0f, 200.0f, 40.0f };

    static std::string RejectReasonToMessage(RejectReason reason) {
        switch (reason) {
            case RejectReason::RoomNotFound: return "Room not found";
            case RejectReason::SessionFull: return "Room is full";
            case RejectReason::InvalidToken: return "Connection failed";
            default: return "Connection failed";
        }
    }
};
