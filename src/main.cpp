#include <raylib-cpp.hpp>

int main()
{

    // Initialization
    int screenWidth = 800;
    int screenHeight = 450;

    raylib::Color textColor(LIGHTGRAY);
    raylib::Window w(screenWidth, screenHeight, "Maxion Test");

    std::string textDraw = "Hello World";

    SetTargetFPS(60);

    // Main game loop
    while (!w.ShouldClose()) // Detect window close button or ESC key
    {
        // Update

        // TODO: Update your variables here

        // Draw
        BeginDrawing();
        ClearBackground(RAYWHITE);
        textColor.DrawText(textDraw, 190, 200, 20);

        // TraceLog(LOG_INFO, "test");
        EndDrawing();
    }

    return 0;
}