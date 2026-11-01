#include "overlay.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "il2cpp_dumper.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "input_hook.h"
#include "shadowhook.h"
#include "ui.h"
#include "vk_overlay.h"

namespace overlay {

static bool g_init = false;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static uint64_t g_swap_count = 0;
static thread_local bool t_in_swap = false;

static void on_frame(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE) return;

    GLint vp[4] = {};
    glGetIntegerv(GL_VIEWPORT, vp);
    EGLint w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0) {
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    }

    if (w < 100 || h < 100) return;

    // Prevent GLES and Vulkan from racing on the global ImGui context
    if (!ui::acquire_backend(ui::Backend::GLES)) return;
    std::lock_guard<std::mutex> render_lock(ui::render_mutex());

    if (g_init && ctx != g_ctx) {
        dumper::log("EGL: context changed (%p -> %p), reinitializing", g_ctx, ctx);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_init = false;
    }

    if (!g_init) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ui::init_style();
        EGLint client_ver = 3;
        eglQueryContext(dpy, ctx, EGL_CONTEXT_CLIENT_VERSION, &client_ver);
        const char *glsl_ver = (client_ver >= 3) ? "#version 300 es" : "#version 100";
        ImGui_ImplOpenGL3_Init(glsl_ver);
        g_ctx = ctx;
        g_init = true;
        dumper::log("EGL: ImGui overlay initialized for ctx %p (GLES %d, %dx%d)",
                    ctx, (int)client_ver, w, h);
    }

    g_swap_count++;
    if (g_swap_count <= 5 || g_swap_count % 300 == 1) {
        GLint cur_fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur_fbo);
        dumper::log("EGL present heartbeat: frame %llu, %dx%d, ctx %p, fbo %d",
                    (unsigned long long)g_swap_count, w, h, ctx, cur_fbo);
    }

    // Save OpenGL states
    GLint last_draw_fbo = 0, last_read_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &last_draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &last_read_fbo);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean last_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean last_color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, last_color_mask);
    GLint last_viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, last_viewport);

    // Apply overlay state
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    ImGui_ImplOpenGL3_NewFrame();
    ui::begin_frame(w, h);
    ImGui::NewFrame();
    ui::draw();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Restore OpenGL states
    glColorMask(last_color_mask[0], last_color_mask[1], last_color_mask[2], last_color_mask[3]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, last_draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, last_read_fbo);
    glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
    if (last_scissor) glEnable(GL_SCISSOR_TEST);
    if (last_cull) glEnable(GL_CULL_FACE);
    if (last_depth) glEnable(GL_DEPTH_TEST);
    if (last_stencil) glEnable(GL_STENCIL_TEST);

    // Clear any OpenGL errors so game loop won't see them
    while (glGetError() != GL_NO_ERROR) {}
}

// -----------------------------------------------------------------------------
// ShadowHook proxies (using SHADOWHOOK_STACK_SCOPE and SHADOWHOOK_CALL_PREV)
// -----------------------------------------------------------------------------
static EGLBoolean hook_swap_sh(EGLDisplay dpy, EGLSurface surf) {
    SHADOWHOOK_STACK_SCOPE();
    if (!t_in_swap) {
        t_in_swap = true;
        on_frame(dpy, surf);
        t_in_swap = false;
    }
    return SHADOWHOOK_CALL_PREV(hook_swap_sh, dpy, surf);
}

static EGLBoolean hook_swap_damage_sh(EGLDisplay dpy, EGLSurface surf, const EGLint *rects, EGLint n_rects) {
    SHADOWHOOK_STACK_SCOPE();
    if (!t_in_swap) {
        t_in_swap = true;
        on_frame(dpy, surf);
        t_in_swap = false;
    }
    return SHADOWHOOK_CALL_PREV(hook_swap_damage_sh, dpy, surf, rects, n_rects);
}

// -----------------------------------------------------------------------------
// eglGetProcAddress wrapper
// -----------------------------------------------------------------------------
static EGLBoolean (*real_swap_from_gpa)(EGLDisplay, EGLSurface) = nullptr;
static EGLBoolean (*real_swap_damage_from_gpa)(EGLDisplay, EGLSurface, const EGLint *, EGLint) = nullptr;

static EGLBoolean hook_swap_gpa(EGLDisplay dpy, EGLSurface surf) {
    if (!t_in_swap) {
        t_in_swap = true;
        on_frame(dpy, surf);
        t_in_swap = false;
    }
    if (real_swap_from_gpa) return real_swap_from_gpa(dpy, surf);
    return eglSwapBuffers(dpy, surf);
}

static EGLBoolean hook_swap_damage_gpa(EGLDisplay dpy, EGLSurface surf, const EGLint *rects, EGLint n_rects) {
    if (!t_in_swap) {
        t_in_swap = true;
        on_frame(dpy, surf);
        t_in_swap = false;
    }
    if (real_swap_damage_from_gpa) return real_swap_damage_from_gpa(dpy, surf, rects, n_rects);
    return hook_swap_gpa(dpy, surf);
}

static __eglMustCastToProperFunctionPointerType hook_eglGetProcAddress(const char *procname) {
    SHADOWHOOK_STACK_SCOPE();
    if (!procname) return nullptr;

    auto real_func = SHADOWHOOK_CALL_PREV(hook_eglGetProcAddress, procname);

    if (!strcmp(procname, "eglSwapBuffers") || !strcmp(procname, "IMGeglSwapBuffers")) {
        if (real_func && !real_swap_from_gpa) {
            real_swap_from_gpa = reinterpret_cast<EGLBoolean(*)(EGLDisplay, EGLSurface)>(real_func);
        }
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_swap_gpa);
    }
    if (!strcmp(procname, "eglSwapBuffersWithDamageKHR") ||
        !strcmp(procname, "eglSwapBuffersWithDamageEXT") ||
        !strcmp(procname, "IMGeglSwapBuffersWithDamageKHR")) {
        if (real_func && !real_swap_damage_from_gpa) {
            real_swap_damage_from_gpa = reinterpret_cast<EGLBoolean(*)(EGLDisplay, EGLSurface, const EGLint *, EGLint)>(real_func);
        }
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_swap_damage_gpa);
    }
    return real_func;
}

// -----------------------------------------------------------------------------
// Dynamic library monitor & hook installer
// -----------------------------------------------------------------------------
struct LoadedLib {
    std::string name;
    std::string path;
    uintptr_t base = 0;
};

static std::vector<LoadedLib> get_loaded_libraries() {
    std::vector<LoadedLib> libs;
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) return libs;

    std::string line;
    std::set<std::string> seen;
    while (std::getline(maps, line)) {
        auto slash = line.find('/');
        if (slash == std::string::npos) continue;
        std::string path = line.substr(slash);
        while (!path.empty() && (path.back() == ' ' || path.back() == '\r' || path.back() == '\n')) {
            path.pop_back();
        }
        if (path.length() >= 3 && path.substr(path.length() - 3) == ".so") {
            auto bslash = path.rfind('/');
            std::string base_name = (bslash != std::string::npos) ? path.substr(bslash + 1) : path;
            if (seen.insert(base_name).second) {
                uintptr_t start = 0, end = 0;
                char dash;
                std::istringstream iss(line);
                if (iss >> std::hex >> start >> dash >> end) {
                    libs.push_back({base_name, path, start});
                } else {
                    libs.push_back({base_name, path, 0});
                }
            }
        }
    }
    return libs;
}

static bool is_graphics_library(const std::string &base) {
    std::string lower = base;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("egl") != std::string::npos) return true;
    if (lower.find("gles") != std::string::npos) return true;
    if (lower.find("powervr") != std::string::npos) return true;
    if (lower.find("adreno") != std::string::npos) return true;
    if (lower.find("mali") != std::string::npos) return true;
    if (lower.find("unity") != std::string::npos) return true;
    if (lower.find("ue4") != std::string::npos) return true;
    if (lower.find("unreal") != std::string::npos) return true;
    return false;
}

static void try_hook(std::set<std::pair<std::string, std::string>> &hooked,
                     const std::string &lib, const char *sym, void *new_fn) {
    auto key = std::make_pair(lib, std::string(sym));
    if (hooked.find(key) != hooked.end()) return;

    void *stub = shadowhook_hook_sym_name(lib.c_str(), sym, new_fn, nullptr);
    if (stub != nullptr) {
        hooked.insert(key);
        dumper::log("EGL hook: %s -> %s installed successfully", lib.c_str(), sym);
    } else {
        int err = shadowhook_get_errno();
        if (err != SHADOWHOOK_ERRNO_PENDING && err != SHADOWHOOK_ERRNO_NOT_FOUND) {
            hooked.insert(key);  // Don't spam retries on fatal errors
        }
    }
}

void install(JNIEnv *env) {
    shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
    input::install(env);
    vkov::install();

    std::thread([] {
        using namespace std::chrono_literals;

        // Hook eglGetProcAddress first in libEGL.so
        shadowhook_hook_sym_name("libEGL.so", "eglGetProcAddress",
                                 reinterpret_cast<void *>(hook_eglGetProcAddress), nullptr);

        std::set<std::pair<std::string, std::string>> hooked;

        // Static known candidates to try immediately
        const char *known_libs[] = {
            "libEGL.so",
            "libIMGegl.so",
            "libEGL_powervr.so",
            "libGLESv2_powervr.so",
            "libEGL_adreno.so",
            "libq3dtools_adreno.so",
            "libGLES_mali.so",
            "libmali.so",
            "libunity.so",
            "libUE4.so",
            "libUnreal.so"
        };

        // Continuous monitoring loop:
        // Checks every 200ms for first 30s, then every 1.5s indefinitely.
        for (int iter = 0; ; ++iter) {
            for (const char *kl : known_libs) {
                try_hook(hooked, kl, "eglSwapBuffers", reinterpret_cast<void *>(hook_swap_sh));
                try_hook(hooked, kl, "IMGeglSwapBuffers", reinterpret_cast<void *>(hook_swap_sh));
                try_hook(hooked, kl, "eglSwapBuffersWithDamageKHR", reinterpret_cast<void *>(hook_swap_damage_sh));
                try_hook(hooked, kl, "eglSwapBuffersWithDamageEXT", reinterpret_cast<void *>(hook_swap_damage_sh));
                try_hook(hooked, kl, "IMGeglSwapBuffersWithDamageKHR", reinterpret_cast<void *>(hook_swap_damage_sh));
            }

            // Also inspect all dynamically loaded libraries via /proc/self/maps
            std::vector<LoadedLib> loaded = get_loaded_libraries();
            for (const auto &lib : loaded) {
                if (lib.name == "libunity.so") {
                    input::hook_unity();
                }
                if (!is_graphics_library(lib.name)) continue;
                try_hook(hooked, lib.name, "eglSwapBuffers", reinterpret_cast<void *>(hook_swap_sh));
                try_hook(hooked, lib.name, "IMGeglSwapBuffers", reinterpret_cast<void *>(hook_swap_sh));
                try_hook(hooked, lib.name, "eglSwapBuffersWithDamageKHR", reinterpret_cast<void *>(hook_swap_damage_sh));
                try_hook(hooked, lib.name, "eglSwapBuffersWithDamageEXT", reinterpret_cast<void *>(hook_swap_damage_sh));
                try_hook(hooked, lib.name, "IMGeglSwapBuffersWithDamageKHR", reinterpret_cast<void *>(hook_swap_damage_sh));
            }

            std::this_thread::sleep_for(iter < 150 ? 200ms : 1500ms);
        }
    }).detach();
}

}  // namespace overlay
