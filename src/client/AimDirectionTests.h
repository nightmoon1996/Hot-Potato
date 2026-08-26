#pragma once

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "AimDirection.h"

inline void RunAimDirectionSmokeTests() {
    // Straight right
    {
        Vector2 dir = ComputeAimDirection(Vector2{0.0f, 0.0f}, Vector2{10.0f, 0.0f});
        if (std::fabs(dir.x - 1.0f) > 0.001f || std::fabs(dir.y - 0.0f) > 0.001f) {
            printf("FAIL: expected direction (1,0), got (%f,%f)\n", dir.x, dir.y);
            exit(1);
        }
        printf("PASS: aim direction straight right normalizes correctly\n");
    }

    // Diagonal
    {
        Vector2 dir = ComputeAimDirection(Vector2{0.0f, 0.0f}, Vector2{3.0f, 4.0f});
        float expectedX = 3.0f / 5.0f, expectedY = 4.0f / 5.0f; // 3-4-5 triangle
        if (std::fabs(dir.x - expectedX) > 0.001f || std::fabs(dir.y - expectedY) > 0.001f) {
            printf("FAIL: expected direction (%f,%f), got (%f,%f)\n", expectedX, expectedY, dir.x, dir.y);
            exit(1);
        }
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (std::fabs(len - 1.0f) > 0.001f) {
            printf("FAIL: expected unit length, got %f\n", len);
            exit(1);
        }
        printf("PASS: diagonal aim direction normalizes to correct unit vector\n");
    }

    // Degenerate: same point returns a default direction, not NaN/zero
    {
        Vector2 dir = ComputeAimDirection(Vector2{50.0f, 50.0f}, Vector2{50.0f, 50.0f});
        if (std::fabs(dir.x - 1.0f) > 0.001f || std::fabs(dir.y - 0.0f) > 0.001f) {
            printf("FAIL: expected degenerate default (1,0), got (%f,%f)\n", dir.x, dir.y);
            exit(1);
        }
        printf("PASS: degenerate same-point aim returns default direction, not NaN\n");
    }

    printf("All AimDirection smoke tests passed.\n");
}
