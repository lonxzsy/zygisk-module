#include "input_hook.h"

#include <android/input.h>
#include <android/log.h>
#include <jni.h>
#include <link.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "il2cpp_dumper.h"
#include "shadowhook.h"
#include "ui.h"

namespace input {

// Unity 2018-2021: nativeInjectEvent(JNIEnv*, jobject, jobject)
// Unity 2022+:     nativeInjectEvent(JNIEnv*, jobject, jobject, jint)
// Forwarding the 4th register argument works for both 3-arg and 4-arg ABI on ARM/ARM64.
using InjectFn = jboolean (*)(JNIEnv *, jobject, jobject, jint);
static InjectFn orig_inject = nullptr;
static std::atomic<bool> g_unity_hook_attempted{false};

static jclass g_motion_cls = nullptr;
static jmethodID g_get_action = nullptr, g_get_x = nullptr, g_get_y = nullptr;

static jboolean my_inject(JNIEnv *env, jobject thiz, jobject ev, jint source) {
    SHADOWHOOK_STACK_SCOPE();
    if (env && ev) {
        if (!g_motion_cls) {
            jclass c = env->FindClass("android/view/MotionEvent");
            if (c) {
                g_motion_cls = static_cast<jclass>(env->NewGlobalRef(c));
                g_get_action = env->GetMethodID(c, "getActionMasked", "()I");
                g_get_x = env->GetMethodID(c, "getX", "()F");
                g_get_y = env->GetMethodID(c, "getY", "()F");
                env->DeleteLocalRef(c);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (g_motion_cls && g_get_action && g_get_x && g_get_y &&
            env->IsInstanceOf(ev, g_motion_cls)) {
            int action = env->CallIntMethod(ev, g_get_action);
            float x = env->CallFloatMethod(ev, g_get_x);
            float y = env->CallFloatMethod(ev, g_get_y);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            } else {
                bool hit = ui::touch(x, y, action);
                if (hit) return JNI_TRUE;
            }
        }
    }
    return orig_inject ? orig_inject(env, thiz, ev, source) : JNI_FALSE;
}

// -----------------------------------------------------------------------------
// Universal AInputQueue hook for Unreal Engine and native games
// -----------------------------------------------------------------------------
static int32_t (*orig_getEvent)(AInputQueue *, AInputEvent **) = nullptr;

static int32_t hook_getEvent(AInputQueue *q, AInputEvent **out) {
    SHADOWHOOK_STACK_SCOPE();
    int32_t res = orig_getEvent ? orig_getEvent(q, out) : -1;
    if (res >= 0 && out && *out) {
        int32_t type = AInputEvent_getType(*out);
        if (type == AINPUT_EVENT_TYPE_MOTION) {
            int32_t action = AMotionEvent_getAction(*out) & AMOTION_EVENT_ACTION_MASK;
            float x = AMotionEvent_getX(*out, 0);
            float y = AMotionEvent_getY(*out, 0);
            if (ui::touch(x, y, action)) {
                AInputQueue_finishEvent(q, *out, 1);
                return -1;
            }
        }
    }
    return res;
}

struct SegRange {
    uintptr_t start;
    uintptr_t end;
    bool executable;
};

static uintptr_t untag_ptr(uintptr_t p) {
#if defined(__aarch64__)
    return p & 0x00FFFFFFFFFFFFFFULL;
#else
    return p;
#endif
}

// Locate UnityPlayer.nativeInjectEvent directly in mapped memory of libunity.so.
// Works even when libunity.so is loaded straight from split_config.*.apk without extraction,
// and supports both 32-bit (armeabi-v7a) and 64-bit (arm64-v8a).
static uintptr_t find_native_inject_in_memory() {
    struct Ctx {
        std::vector<SegRange> segs;
    } ctx;

    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *data) -> int {
            if (!info->dlpi_name || !strstr(info->dlpi_name, "libunity.so")) return 0;
            auto *c = static_cast<Ctx *>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto &ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_R) || ph.p_memsz == 0) continue;
                uintptr_t s = info->dlpi_addr + ph.p_vaddr;
                uintptr_t e = s + ph.p_memsz;
                bool exec = (ph.p_flags & PF_X) != 0;
                c->segs.push_back({s, e, exec});
            }
            return 1;
        },
        &ctx);

    if (ctx.segs.empty()) return 0;

    auto in_segs = [&](uintptr_t addr, bool require_exec) -> bool {
        addr = untag_ptr(addr);
        for (const auto &seg : ctx.segs) {
            if (require_exec && !seg.executable) continue;
            if (addr >= seg.start && addr < seg.end) return true;
        }
        return false;
    };

    const char target[] = "nativeInjectEvent";
    std::vector<uintptr_t> str_vas;
    for (const auto &seg : ctx.segs) {
        if (seg.end <= seg.start + sizeof(target)) continue;
        const auto *p = reinterpret_cast<const uint8_t *>(seg.start);
        size_t len = seg.end - seg.start;
        for (size_t i = 0; i + sizeof(target) <= len; ++i) {
            if (p[i] == 'n' && memcmp(p + i, target, sizeof(target)) == 0) {
                str_vas.push_back(seg.start + i);
            }
        }
    }
    if (str_vas.empty()) return 0;

    // Scan for JNINativeMethod { const char* name; const char* signature; void* fnPtr; }
    for (const auto &seg : ctx.segs) {
        if (seg.executable) continue;  // JNINativeMethod lives in .rodata / .data.rel.ro / .data
        for (uintptr_t cur = seg.start; cur + 3 * sizeof(uintptr_t) <= seg.end; cur += sizeof(uintptr_t)) {
            const auto *words = reinterpret_cast<const uintptr_t *>(cur);
            uintptr_t w0 = untag_ptr(words[0]);
            bool match_name = false;
            for (uintptr_t sva : str_vas) {
                if (w0 == untag_ptr(sva)) {
                    match_name = true;
                    break;
                }
            }
            if (!match_name) continue;

            uintptr_t w1 = untag_ptr(words[1]);
            uintptr_t w2 = untag_ptr(words[2]);
            if (!in_segs(w1, false) || !in_segs(w2, true)) continue;

            const auto *sig = reinterpret_cast<const char *>(w1);
            if (sig[0] == '(' && strstr(sig, "InputEvent")) {
                dumper::log("Found Unity nativeInjectEvent in memory at %p (sig: %s)",
                            reinterpret_cast<void *>(w2), sig);
                return w2;
            }
        }
    }
    return 0;
}

void hook_unity() {
    if (orig_inject || g_unity_hook_attempted.load()) return;

    uintptr_t fn_va = find_native_inject_in_memory();
    if (!fn_va) return;

    g_unity_hook_attempted = true;
    void *fn_addr = reinterpret_cast<void *>(fn_va);
    void *stub = shadowhook_hook_func_addr(fn_addr,
                                           reinterpret_cast<void *>(my_inject),
                                           reinterpret_cast<void **>(&orig_inject));
    dumper::log("Unity nativeInjectEvent hook: addr=%p, stub=%p, err=%d",
                fn_addr, stub, shadowhook_get_errno());
}

void install(JNIEnv *) {
    shadowhook_hook_sym_name("libandroid.so", "AInputQueue_getEvent",
                             reinterpret_cast<void *>(hook_getEvent),
                             reinterpret_cast<void **>(&orig_getEvent));
}

}  // namespace input
