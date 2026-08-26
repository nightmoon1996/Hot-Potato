#pragma once

#include "raylib-cpp.hpp"
#include <cmath>

// Normalizes the direction from `from` to `to`; returns {1,0} if the points coincide
// (degenerate case — avoids a zero-length aim vector reaching the server).
inline Vector2 ComputeAimDirection(Vector2 from, Vector2 to) {
    Vector2 delta{ to.x - from.x, to.y - from.y };
    float len = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (len < 0.0001f) {
        return Vector2{1.0f, 0.0f};
    }
    return Vector2{ delta.x / len, delta.y / len };
}
