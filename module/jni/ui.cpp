#include "ui.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "il2cpp_dumper.h"
#include "imgui.h"

namespace ui {

struct TouchEv {
    float x, y;
    int action;
};

static std::mutex g_mtx;
static std::mutex g_render_mtx;
static std::atomic<int> g_active_backend{0};
static std::deque<TouchEv> g_queue;
static float g_win_rect[4] = {};  // x, y, w, h of current active window/pill
static bool g_minimized = false;
static bool g_auto_scroll = true;
static bool g_capturing_touch = false;
static float g_scale = 2.4f;

bool acquire_backend(Backend b) {
    int cur = g_active_backend.load(std::memory_order_relaxed);
    int target = static_cast<int>(b);
    if (cur == target) return true;
    // Prefer Vulkan if game upgrades from an initial dummy EGL context to Vulkan,
    // or claim if None.
    if (cur == 0) {
        return g_active_backend.compare_exchange_strong(cur, target);
    }
    return false;
}

std::mutex &render_mutex() {
    return g_render_mtx;
}

static bool point_in_rect(float x, float y, const float r[4]) {
    if (r[2] <= 0.0f || r[3] <= 0.0f) return false;
    const float pad = 12.0f;
    return x >= (r[0] - pad) && x <= (r[0] + r[2] + pad) &&
           y >= (r[1] - pad) && y <= (r[1] + r[3] + pad);
}

bool touch(float x, float y, int action) {
    std::lock_guard<std::mutex> l(g_mtx);
    bool is_down = (action == 0 || action == 5);
    bool is_up = (action == 1 || action == 3 || action == 6);

    if (is_down) {
        g_capturing_touch = point_in_rect(x, y, g_win_rect);
    }

    if (g_capturing_touch) {
        if (g_queue.size() < 64) {
            g_queue.push_back({x, y, action});
        }
        if (is_up) {
            g_capturing_touch = false;
        }
        return true;  // Consume touch so the game doesn't click behind the ImGui window
    }
    return false;
}

void init_style() {
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.FrameRounding = 6.0f;
    st.GrabRounding = 6.0f;
    st.ScaleAllSizes(g_scale);
    st.FontScaleMain = g_scale;
    st.TouchExtraPadding = ImVec2(8.0f, 8.0f);
}

void begin_frame(int w, int h) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    if (dt > 0.1f) dt = 0.016f;
    io.DeltaTime = dt < 1e-4f ? 1e-4f : dt;
    last = now;

    std::lock_guard<std::mutex> l(g_mtx);
    bool saw_down_this_frame = false;
    while (!g_queue.empty()) {
        TouchEv e = g_queue.front();
        bool is_down = (e.action == 0 || e.action == 5);
        bool is_up = (e.action == 1 || e.action == 3 || e.action == 6);

        // If both DOWN and UP arrived in the same frame (fast tap), process DOWN
        // this frame and leave UP for the next frame so ImGui registers the click.
        if (is_up && saw_down_this_frame) {
            break;
        }

        g_queue.pop_front();
        io.AddMousePosEvent(e.x, e.y);
        if (is_down) {
            io.AddMouseButtonEvent(0, true);
            saw_down_this_frame = true;
        } else if (is_up) {
            io.AddMouseButtonEvent(0, false);
        }
    }
}

void draw() {
    ImGuiIO &io = ImGui::GetIO();
    auto &st = dumper::status();
    auto eng = st.engine.load();
    auto s = st.state.load();

    float rect[4] = {};

    if (g_minimized) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.02f, io.DisplaySize.y * 0.05f),
                                ImGuiCond_FirstUseEver);
        ImGuiWindowFlags pill_flags = ImGuiWindowFlags_NoTitleBar |
                                      ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##DumperPill", nullptr, pill_flags)) {
            std::string label = std::string("[+] Dumper: ") + dumper::state_name(s);
            if (ImGui::Button(label.c_str())) {
                g_minimized = false;
            }
            ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
            rect[0] = p.x; rect[1] = p.y; rect[2] = sz.x; rect[3] = sz.y;
        }
        ImGui::End();
    } else {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.04f, io.DisplaySize.y * 0.04f),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.58f, io.DisplaySize.y * 0.62f),
                                 ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Universal Dumper (Unity IL2CPP / UE4 / UE5)", nullptr,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            if (ImGui::Button("[-] Minimize")) {
                g_minimized = true;
            }
            ImGui::SameLine();

            bool can_dump = (s != dumper::State::Dumping && s != dumper::State::WaitingForLib);
            if (!can_dump) ImGui::BeginDisabled();
            if (ImGui::Button(s == dumper::State::Done ? "[ Re-Dump Now ]" : "[ Dump Now ]")) {
                dumper::request_dump();
            }
            if (!can_dump) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Clear Log")) {
                dumper::clear_log();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &g_auto_scroll);

            ImGui::Separator();
            std::string ver = dumper::engine_version();
            if (!ver.empty()) {
                ImGui::Text("Engine: %s (%s) | State: %s",
                            dumper::engine_name(eng), ver.c_str(), dumper::state_name(s));
            } else {
                ImGui::Text("Engine: %s | State: %s",
                            dumper::engine_name(eng), dumper::state_name(s));
            }

            std::string detail = dumper::status_detail();
            if (!detail.empty()) {
                ImGui::TextWrapped("Stage: %s", detail.c_str());
            }
            if (st.il2cpp_base.load()) {
                ImGui::Text("Target Lib Base: 0x%llx", (unsigned long long)st.il2cpp_base.load());
            }
            std::string out = dumper::output_dir();
            if (!out.empty()) {
                ImGui::TextWrapped("Output: %s", out.c_str());
            }

            ImGui::Separator();
            if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                std::string l = dumper::log_snapshot();
                ImGui::TextUnformatted(l.c_str());
                if (g_auto_scroll) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();

            ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
            rect[0] = p.x; rect[1] = p.y; rect[2] = sz.x; rect[3] = sz.y;
        }
        ImGui::End();
    }

    std::lock_guard<std::mutex> l(g_mtx);
    memcpy(g_win_rect, rect, sizeof rect);
}

}  // namespace ui
