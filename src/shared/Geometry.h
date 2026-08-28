#pragma once

#include "Vector2.hpp"
#include "Rectangle.hpp"

inline bool PointInRect(Vector2 point, Rectangle rect) {
    return point.x >= rect.x && point.x <= (rect.x + rect.width) &&
           point.y >= rect.y && point.y <= (rect.y + rect.height);
}
