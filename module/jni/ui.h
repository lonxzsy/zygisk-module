#pragma once
#include <mutex>

// Backend-independent ImGui menu + touch handling (shared by the GLES and Vulkan paths).
namespace ui {

enum class Backend { None = 0, GLES = 1, Vulkan = 2 };

// Acquire exclusive ownership of the global ImGui context for a rendering backend,
// preventing crashes when a game creates both an EGL context and a Vulkan swapchain.
bool acquire_backend(Backend b);
std::mutex &render_mutex();

// Call once after ImGui::CreateContext().
void init_style();
// Call before ImGui::NewFrame(): sets display size / delta time and applies queued touches.
void begin_frame(int w, int h);
// Builds the menu widgets (between NewFrame and Render).
void draw();

// Touch entry point (thread-safe, called from the game's UI/input thread).
// action: android MotionEvent action masked (0 down, 1 up, 2 move, 3 cancel).
// Returns true if the event was consumed by the overlay and must not reach the game.
bool touch(float x, float y, int action);

}  // namespace ui
