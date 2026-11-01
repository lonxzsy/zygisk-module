#include "il2cpp_dumper.h"
#include "ue4_dumper.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "shadowhook.h"

#define TAG "Il2CppDumperZ"

namespace dumper {

// ------------------------------------------------------------------ globals
static Config g_cfg;
static std::string g_pkg;
static std::string g_out;
static std::mutex g_log_mtx;
static std::string g_log;
static std::string g_detail;
static std::string g_engine_ver;
static FILE *g_log_file = nullptr;
static void *g_handle = nullptr;
static std::string g_target_lib_name;
static Status g_status;

Status &status() { return g_status; }

const char *engine_name(Engine e) {
    switch (e) {
        case Engine::Detecting: return "Auto-Detecting...";
        case Engine::Unity: return "Unity (IL2CPP)";
        case Engine::UE4: return "Unreal Engine (UE4/UE5)";
    }
    return "?";
}

const char *state_name(State s) {
    switch (s) {
        case State::WaitingForLib: return "Waiting for Engine Lib";
        case State::WaitingForRuntime: return "Waiting for Runtime Init";
        case State::Ready: return "Ready";
        case State::Dumping: return "Dumping...";
        case State::Done: return "Done";
        case State::Failed: return "Failed";
    }
    return "?";
}

void log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    std::lock_guard<std::mutex> l(g_log_mtx);
    g_log += buf;
    g_log += '\n';
    if (g_log.size() > 24576) g_log.erase(0, g_log.size() - 16384);
    if (!g_out.empty()) {
        if (!g_log_file) {
            // First log after the output dir is known: seed the file with the
            // whole buffered history so nothing from startup is lost.
            g_log_file = fopen((g_out + "/dump.log").c_str(), "w");
            if (g_log_file) fwrite(g_log.data(), 1, g_log.size(), g_log_file);
        } else {
            fputs(buf, g_log_file);
            fputc('\n', g_log_file);
        }
        if (g_log_file) fflush(g_log_file);
    }
}

std::string log_snapshot() {
    std::lock_guard<std::mutex> l(g_log_mtx);
    return g_log;
}

void clear_log() {
    std::lock_guard<std::mutex> l(g_log_mtx);
    g_log.clear();
}

void set_status_detail(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(g_log_mtx);
    g_detail = buf;
}

std::string status_detail() {
    std::lock_guard<std::mutex> l(g_log_mtx);
    return g_detail;
}

std::string engine_version() {
    std::lock_guard<std::mutex> l(g_log_mtx);
    return g_engine_ver;
}

void set_engine_version(const std::string &v) {
    std::lock_guard<std::mutex> l(g_log_mtx);
    g_engine_ver = v;
}

std::string output_dir() {
    std::lock_guard<std::mutex> l(g_log_mtx);
    return g_out;
}

void set_output_dir(const std::string &dir) {
    std::lock_guard<std::mutex> l(g_log_mtx);
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
    g_out = dir;
}

void request_dump() { g_status.request = true; }

struct ThreadPipe {
    int fd[2] = {-1, -1};
    bool ensure() {
        if (fd[0] >= 0) return true;
        return pipe2(fd, O_CLOEXEC | O_NONBLOCK) == 0;
    }
    void reset() {
        if (fd[0] >= 0) { close(fd[0]); fd[0] = -1; }
        if (fd[1] >= 0) { close(fd[1]); fd[1] = -1; }
    }
    ~ThreadPipe() { reset(); }
};

bool safe_read(uintptr_t addr, void *buf, size_t len) {
    if (!buf || len == 0) return false;
#if defined(__aarch64__)
    addr &= 0x00FFFFFFFFFFFFFFULL;
    if (addr < 0x10000ULL || addr > 0x0000FFFFFFFFFFFFULL) return false;
#else
    if (addr < 0x10000UL) return false;
#endif
    struct iovec local_iov = {buf, len};
    struct iovec remote_iov = {reinterpret_cast<void *>(addr), len};
    ssize_t n = syscall(__NR_process_vm_readv, getpid(), &local_iov, 1, &remote_iov, 1, 0);
    if (n == (ssize_t)len) return true;
    if (errno == EFAULT || errno == EINVAL) return false;

    static thread_local ThreadPipe t_pipe;
    if (!t_pipe.ensure()) return false;
    size_t done = 0;
    while (done < len) {
        size_t chunk = std::min<size_t>(len - done, 4096);
        ssize_t w = write(t_pipe.fd[1], reinterpret_cast<const char *>(addr) + done, chunk);
        if (w <= 0) return false;
        ssize_t r = read(t_pipe.fd[0], static_cast<char *>(buf) + done, (size_t)w);
        if (r != w) {
            t_pipe.reset();
            return false;
        }
        done += (size_t)w;
    }
    return true;
}

// ------------------------------------------------------------ crash guard
// Reading arbitrary game memory / calling into the runtime can fault; recover
// instead of killing the game.
static thread_local sigjmp_buf *t_jmp = nullptr;
static struct sigaction g_old_segv, g_old_bus;

static void fault_handler(int sig, siginfo_t *si, void *ctx) {
    if (t_jmp) siglongjmp(*t_jmp, 1);
    struct sigaction &old = sig == SIGSEGV ? g_old_segv : g_old_bus;
    if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(sig, si, ctx);
    else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) old.sa_handler(sig);
    else { signal(sig, SIG_DFL); }
}

static void install_fault_handler() {
    struct sigaction sa {};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
}

static void restore_fault_handler() {
    sigaction(SIGSEGV, &g_old_segv, nullptr);
    sigaction(SIGBUS, &g_old_bus, nullptr);
}

// Run fn(); returns false if it faulted.
template <typename F>
static bool guarded(F &&fn) {
    sigjmp_buf jb;
    sigjmp_buf *prev = t_jmp;
    if (sigsetjmp(jb, 1) != 0) {
        t_jmp = prev;
        return false;
    }
    t_jmp = &jb;
    fn();
    t_jmp = prev;
    return true;
}

// -------------------------------------------------------------- IL2CPP API
using P = void *;
#define API(ret, name, args) static ret(*name) args = nullptr;
API(P, il2cpp_domain_get, ())
API(P *, il2cpp_domain_get_assemblies, (P, size_t *))
API(P, il2cpp_assembly_get_image, (P))
API(const char *, il2cpp_image_get_name, (P))
API(size_t, il2cpp_image_get_class_count, (P))
API(P, il2cpp_image_get_class, (P, size_t))
API(P, il2cpp_thread_attach, (P))
API(const char *, il2cpp_class_get_name, (P))
API(const char *, il2cpp_class_get_namespace, (P))
API(P, il2cpp_class_get_parent, (P))
API(P, il2cpp_class_get_declaring_type, (P))
API(P, il2cpp_class_get_interfaces, (P, void **))
API(P, il2cpp_class_get_fields, (P, void **))
API(P, il2cpp_class_get_methods, (P, void **))
API(P, il2cpp_class_get_properties, (P, void **))
API(int, il2cpp_class_get_flags, (P))
API(bool, il2cpp_class_is_valuetype, (P))
API(bool, il2cpp_class_is_enum, (P))
API(P, il2cpp_class_get_type, (P))
API(const char *, il2cpp_field_get_name, (P))
API(P, il2cpp_field_get_type, (P))
API(size_t, il2cpp_field_get_offset, (P))
API(int, il2cpp_field_get_flags, (P))
API(void, il2cpp_field_static_get_value, (P, void *))
API(const char *, il2cpp_property_get_name, (P))
API(P, il2cpp_property_get_get_method, (P))
API(P, il2cpp_property_get_set_method, (P))
API(uint32_t, il2cpp_property_get_flags, (P))
API(const char *, il2cpp_method_get_name, (P))
API(P, il2cpp_method_get_return_type, (P))
API(uint32_t, il2cpp_method_get_param_count, (P))
API(P, il2cpp_method_get_param, (P, uint32_t))
API(const char *, il2cpp_method_get_param_name, (P, uint32_t))
API(uint32_t, il2cpp_method_get_flags, (P, uint32_t *))
API(char *, il2cpp_type_get_name, (P))
API(void, il2cpp_free, (void *))
#undef API

static bool resolve_api(const char *lib_name) {
    bool ok = true;
    void *sh_handle = lib_name ? shadowhook_dlopen(lib_name) : nullptr;
    auto sym = [&](const char *n, void *&out, bool required = true) {
        out = g_handle ? dlsym(g_handle, n) : nullptr;
        if (!out && sh_handle) {
            // Fallback to xdl (.dynsym + .symtab search)
            out = shadowhook_dlsym(sh_handle, n);
        }
        if (!out && required) {
            log("missing export: %s", n);
            ok = false;
        }
    };
#define R(name) sym(#name, reinterpret_cast<void *&>(name))
#define RO(name) sym(#name, reinterpret_cast<void *&>(name), false)
    R(il2cpp_domain_get); R(il2cpp_domain_get_assemblies); R(il2cpp_assembly_get_image);
    R(il2cpp_image_get_name); R(il2cpp_image_get_class_count); R(il2cpp_image_get_class);
    R(il2cpp_thread_attach); R(il2cpp_class_get_name); R(il2cpp_class_get_namespace);
    R(il2cpp_class_get_parent); R(il2cpp_class_get_interfaces); R(il2cpp_class_get_fields);
    R(il2cpp_class_get_methods); R(il2cpp_class_get_flags); R(il2cpp_class_is_valuetype);
    R(il2cpp_class_is_enum); R(il2cpp_field_get_name); R(il2cpp_field_get_type);
    R(il2cpp_field_get_offset); R(il2cpp_field_get_flags); R(il2cpp_method_get_name);
    R(il2cpp_method_get_return_type); R(il2cpp_method_get_param_count);
    R(il2cpp_method_get_param); R(il2cpp_method_get_param_name); R(il2cpp_method_get_flags);
    R(il2cpp_type_get_name);
    RO(il2cpp_free); RO(il2cpp_class_get_declaring_type); RO(il2cpp_class_get_type);
    RO(il2cpp_field_static_get_value);
    RO(il2cpp_class_get_properties); RO(il2cpp_property_get_name);
    RO(il2cpp_property_get_get_method); RO(il2cpp_property_get_set_method);
    RO(il2cpp_property_get_flags);
#undef R
#undef RO
    if (sh_handle) shadowhook_dlclose(sh_handle);
    return ok;
}

// ------------------------------------------------------------ lib location
struct LibInfo {
    uintptr_t base = 0;
    size_t size = 0;
};

static LibInfo find_lib(const char *name) {
    if (!name || !name[0]) return {};
    struct Ctx { const char *name; LibInfo li; } ctx{name, {}};
    dl_iterate_phdr(
        [](dl_phdr_info *i, size_t, void *d) -> int {
            auto *c = static_cast<Ctx *>(d);
            if (!i->dlpi_name || !strstr(i->dlpi_name, c->name)) return 0;
            uintptr_t min_vaddr = UINTPTR_MAX;
            uintptr_t max_end = 0;
            for (int k = 0; k < i->dlpi_phnum; k++) {
                auto &ph = i->dlpi_phdr[k];
                if (ph.p_type == PT_LOAD) {
                    if (ph.p_vaddr < min_vaddr) min_vaddr = ph.p_vaddr;
                    uintptr_t e = ph.p_vaddr + ph.p_memsz;
                    if (e > max_end) max_end = e;
                }
            }
            if (min_vaddr == UINTPTR_MAX) min_vaddr = 0;
            c->li.base = i->dlpi_addr + min_vaddr;
            c->li.size = max_end > min_vaddr ? (max_end - min_vaddr) : 0;
            return 1;
        },
        &ctx);

    if (ctx.li.base && ctx.li.size) return ctx.li;

    // Fallback: parse /proc/self/maps if dl_iterate_phdr didn't list it
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return ctx.li;
    char line[512];
    uintptr_t min_start = UINTPTR_MAX, max_end = 0;
    while (fgets(line, sizeof line, maps)) {
        if (!strstr(line, name)) continue;
        unsigned long long s = 0, e = 0;
        if (sscanf(line, "%llx-%llx", &s, &e) == 2) {
            if ((uintptr_t)s < min_start) min_start = (uintptr_t)s;
            if ((uintptr_t)e > max_end) max_end = (uintptr_t)e;
        }
    }
    fclose(maps);
    if (min_start != UINTPTR_MAX && max_end > min_start) {
        ctx.li.base = min_start;
        ctx.li.size = max_end - min_start;
    }
    return ctx.li;
}

// Scan loaded libraries for IL2CPP exports in case libil2cpp.so was renamed
static std::string detect_renamed_il2cpp_lib() {
    struct Ctx { std::string found; } ctx;
    dl_iterate_phdr(
        [](dl_phdr_info *i, size_t, void *d) -> int {
            if (!i->dlpi_name || !i->dlpi_name[0]) return 0;
            if (strstr(i->dlpi_name, "/system/") || strstr(i->dlpi_name, "/apex/") ||
                strstr(i->dlpi_name, "/vendor/")) return 0;
            void *h = dlopen(i->dlpi_name, RTLD_NOW | RTLD_NOLOAD);
            if (!h) return 0;
            bool is_il2cpp = (dlsym(h, "il2cpp_init") != nullptr ||
                              dlsym(h, "il2cpp_domain_get") != nullptr);
            if (is_il2cpp) {
                auto *c = static_cast<Ctx *>(d);
                const char *slash = strrchr(i->dlpi_name, '/');
                c->found = slash ? (slash + 1) : i->dlpi_name;
                return 1;
            }
            return 0;
        },
        &ctx);
    return ctx.found;
}

// ------------------------------------------------------------- file helpers
static bool mkdirs(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if ((path[i] == '/' || i + 1 == path.size()) && cur.size() > 1)
            mkdir(cur.c_str(), 0771);
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool prepare_output() {
    std::vector<std::string> candidates;
    if (g_cfg.output != "auto" && !g_cfg.output.empty())
        candidates.push_back(g_cfg.output + "/" + g_pkg);
    candidates.push_back("/sdcard/Android/data/" + g_pkg + "/files/il2cpp_dump");
    candidates.push_back("/data/data/" + g_pkg + "/files/il2cpp_dump");
    candidates.push_back("/data/user/0/" + g_pkg + "/files/il2cpp_dump");
    for (auto &c : candidates) {
        if (!mkdirs(c)) continue;
        std::string probe = c + "/.probe";
        FILE *f = fopen(probe.c_str(), "w");
        if (!f) continue;
        fclose(f);
        unlink(probe.c_str());
        set_output_dir(c);
        return true;
    }
    return false;
}

// -------------------------------------------------------------- type naming
static std::string type_name(P type) {
    if (!type) return "?";
    std::string r = "?";
    guarded([&] {
        char *n = il2cpp_type_get_name(type);
        if (n) {
            r = n;
            if (il2cpp_free) il2cpp_free(n);
        }
    });
    return r;
}

static std::string full_class_name(P klass) {
    if (!klass) return "?";
    std::string name = il2cpp_class_get_name(klass) ?: "?";
    if (il2cpp_class_get_declaring_type) {
        P decl = il2cpp_class_get_declaring_type(klass);
        int depth = 0;
        while (decl && depth++ < 8) {
            const char *dn = il2cpp_class_get_name(decl);
            if (!dn || !dn[0]) break;
            name = std::string(dn) + "." + name;
            decl = il2cpp_class_get_declaring_type(decl);
        }
    }
    return name;
}

static std::string access_str(int flags) {
    switch (flags & 7) {
        case 1: return "private ";
        case 2: return "private protected ";
        case 3: return "internal ";
        case 4: return "protected ";
        case 5: return "protected internal ";
        case 6: return "public ";
        default: return "";
    }
}

static std::string hex(uint64_t v) {
    char b[32];
    snprintf(b, sizeof b, "0x%llX", (unsigned long long)v);
    return b;
}

static std::string json_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

// -------------------------------------------------------------- the dump
struct Writers {
    FILE *cs = nullptr;
    FILE *js = nullptr;
    bool first_js = true;
    uintptr_t base = 0;
    size_t size = 0;
};

static void dump_class(P klass, size_t type_idx, Writers &w) {
    std::string ns = il2cpp_class_get_namespace(klass) ?: "";
    std::string name = full_class_name(klass);
    int cflags = il2cpp_class_get_flags(klass);
    bool is_iface = cflags & 0x20;
    bool is_enum = il2cpp_class_is_enum(klass);
    bool is_vt = il2cpp_class_is_valuetype(klass);

    std::string head = "\n// Namespace: " + ns + "\n";
    int vis = cflags & 7;
    head += (vis == 1 || vis == 2) ? "public " : "internal ";
    if (!is_iface && !is_enum && !is_vt) {
        if ((cflags & 0x80) && (cflags & 0x100)) head += "static ";
        else if (cflags & 0x80) head += "abstract ";
        else if (cflags & 0x100) head += "sealed ";
    }
    head += is_iface ? "interface " : is_enum ? "enum " : is_vt ? "struct " : "class ";
    head += name;

    std::vector<std::string> bases;
    if (!is_enum && !is_vt) {
        P parent = il2cpp_class_get_parent(klass);
        if (parent && !is_iface) {
            std::string pn = il2cpp_class_get_name(parent) ?: "";
            if (pn != "Object" && !pn.empty()) bases.push_back(pn);
        }
    }
    if (!is_enum) {
        void *it = nullptr;
        while (P iface = il2cpp_class_get_interfaces(klass, &it))
            bases.push_back(il2cpp_class_get_name(iface) ?: "?");
    }
    if (!bases.empty()) {
        head += " : ";
        for (size_t i = 0; i < bases.size(); i++) head += (i ? ", " : "") + bases[i];
    }
    fprintf(w.cs, "%s // TypeDefIndex: %zu\n{\n", head.c_str(), type_idx);

    // Fields
    void *it = nullptr;
    bool any = false;
    while (P f = il2cpp_class_get_fields(klass, &it)) {
        if (!any) { fprintf(w.cs, "\t// Fields\n"); any = true; }
        int fl = il2cpp_field_get_flags(f);
        std::string line = "\t";
        if (fl & 0x40) {
            line += access_str(fl) + "const ";
        } else {
            line += access_str(fl);
            if (fl & 0x10) line += "static ";
            if (fl & 0x20) line += "readonly ";
        }
        std::string ftype = type_name(il2cpp_field_get_type(f));
        std::string fname = il2cpp_field_get_name(f) ?: "?";
        line += ftype + " " + fname;
        bool is_static = (fl & 0x10) != 0;
        if (fl & 0x40) {
            // Literal / enum constant: try reading value via il2cpp_field_static_get_value
            int64_t val = 0;
            bool got_val = false;
            if (il2cpp_field_static_get_value && is_enum) {
                guarded([&] {
                    il2cpp_field_static_get_value(f, &val);
                    got_val = true;
                });
            }
            if (got_val) {
                line += " = " + std::to_string((int32_t)val) + ";";
            } else {
                line += ";";
            }
        } else {
            line += "; // " + hex(il2cpp_field_get_offset(f)) + (is_static ? " (static)" : "");
        }
        fprintf(w.cs, "%s\n", line.c_str());
    }

    // Properties
    if (il2cpp_class_get_properties && il2cpp_property_get_name) {
        it = nullptr;
        any = false;
        while (P prop = il2cpp_class_get_properties(klass, &it)) {
            if (!any) { fprintf(w.cs, "\n\t// Properties\n"); any = true; }
            const char *pname = il2cpp_property_get_name(prop) ?: "?";
            P getter = il2cpp_property_get_get_method ? il2cpp_property_get_get_method(prop) : nullptr;
            P setter = il2cpp_property_get_set_method ? il2cpp_property_get_set_method(prop) : nullptr;
            std::string ptype = "?";
            int mflags = 0;
            if (getter) {
                ptype = type_name(il2cpp_method_get_return_type(getter));
                uint32_t ifl = 0;
                mflags = (int)il2cpp_method_get_flags(getter, &ifl);
            } else if (setter && il2cpp_method_get_param_count(setter) > 0) {
                ptype = type_name(il2cpp_method_get_param(setter, 0));
                uint32_t ifl = 0;
                mflags = (int)il2cpp_method_get_flags(setter, &ifl);
            }
            std::string pline = "\t" + access_str(mflags);
            if (mflags & 0x10) pline += "static ";
            pline += ptype + " " + pname + " { ";
            if (getter) pline += "get; ";
            if (setter) pline += "set; ";
            pline += "}";
            fprintf(w.cs, "%s\n", pline.c_str());
        }
    }

    // Methods
    it = nullptr;
    any = false;
    while (P m = il2cpp_class_get_methods(klass, &it)) {
        if (!any) { fprintf(w.cs, "\n\t// Methods\n"); any = true; }
        uint32_t iflags = 0;
        int fl = (int)il2cpp_method_get_flags(m, &iflags);
        void *ptr = *reinterpret_cast<void **>(m);  // MethodInfo::methodPointer (first field)
        uintptr_t va = reinterpret_cast<uintptr_t>(ptr);
#if defined(__aarch64__)
        va &= 0x00FFFFFFFFFFFFFFULL;
#endif
        bool valid_va = (va >= w.base && (w.size == 0 || va < w.base + w.size));
        uintptr_t rva = valid_va ? (va - w.base) : 0;

        std::string line = "\t";
        line += access_str(fl);
        if (fl & 0x10) line += "static ";
        if (fl & 0x400) line += "abstract ";
        else if ((fl & 0x40) && (fl & 0x20)) line += "sealed override ";
        else if (fl & 0x40) line += "virtual ";

        std::string ret = type_name(il2cpp_method_get_return_type(m));
        std::string mname = il2cpp_method_get_name(m) ?: "?";
        std::string params;
        uint32_t pc = il2cpp_method_get_param_count(m);
        for (uint32_t i = 0; i < pc; i++) {
            std::string t = type_name(il2cpp_method_get_param(m, i));
            std::string pn = il2cpp_method_get_param_name(m, i) ?: ("arg" + std::to_string(i));
            params += (i ? ", " : "") + t + " " + pn;
        }
        line += ret + " " + mname + "(" + params + ") { }";
        if (valid_va) line += " // RVA: " + hex(rva) + " VA: " + hex(va);
        else line += " // RVA: -1 Offset: -1";
        fprintf(w.cs, "%s\n", line.c_str());

        if (w.js && valid_va) {
            std::string full = (ns.empty() ? "" : ns + ".") + name + "$$" + mname;
            fprintf(w.js, "%s{\"Address\":%llu,\"Name\":\"%s\",\"Signature\":\"%s\"}",
                    w.first_js ? "" : ",\n", (unsigned long long)rva,
                    json_escape(full).c_str(),
                    json_escape(ret + " " + full + "(" + params + ")").c_str());
            w.first_js = false;
        }
    }
    fprintf(w.cs, "}\n");
}

static bool dump_cs_and_json(uintptr_t base, size_t size) {
    Writers w;
    w.base = base;
    w.size = size;
    if (g_cfg.dump_cs) w.cs = fopen((g_out + "/dump.cs").c_str(), "w");
    else w.cs = fopen("/dev/null", "w");
    if (g_cfg.dump_script_json) {
        w.js = fopen((g_out + "/script.json").c_str(), "w");
        if (w.js) fprintf(w.js, "{\"ScriptMethod\":[\n");
    }
    if (!w.cs) { log("cannot open dump.cs for writing"); return false; }

    P domain = il2cpp_domain_get();
    il2cpp_thread_attach(domain);
    size_t asm_count = 0;
    P *assemblies = il2cpp_domain_get_assemblies(domain, &asm_count);
    log("assemblies: %zu", asm_count);

    // First write image list header (compatible with Il2CppDumper format)
    for (size_t a = 0; a < asm_count; a++) {
        P image = il2cpp_assembly_get_image(assemblies[a]);
        const char *iname = image ? il2cpp_image_get_name(image) : "?";
        size_t cc = image ? il2cpp_image_get_class_count(image) : 0;
        fprintf(w.cs, "// Image %zu: %s - %zu classes\n", a, iname ?: "?", cc);
    }

    size_t classes_ok = 0, classes_fail = 0, global_type_idx = 0;
    for (size_t a = 0; a < asm_count; a++) {
        P image = il2cpp_assembly_get_image(assemblies[a]);
        if (!image) continue;
        const char *iname = il2cpp_image_get_name(image);
        size_t cc = il2cpp_image_get_class_count(image);
        set_status_detail("Unity: dumping image %zu/%zu (%s, %zu classes)...",
                          a + 1, asm_count, iname ?: "?", cc);
        for (size_t c = 0; c < cc; c++, global_type_idx++) {
            P k = il2cpp_image_get_class(image, c);
            if (!k) continue;
            bool ok = guarded([&] { dump_class(k, global_type_idx, w); });
            if (ok) classes_ok++;
            else { classes_fail++; fprintf(w.cs, "\n// <class %zu in image %zu faulted>\n", c, a); }
        }
        fflush(w.cs);
    }
    log("classes dumped: %zu (faulted: %zu)", classes_ok, classes_fail);

    fclose(w.cs);
    if (w.js) {
        fprintf(w.js, "\n]}\n");
        fclose(w.js);
    }
    return true;
}

// Raw memory dump of the mapped libil2cpp.so (unreadable pages zero-filled).
static bool dump_so_memory(const LibInfo &li) {
    std::string path = g_out + "/libil2cpp_mem.so";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    std::vector<char> page(4096);
    size_t zero_pages = 0;
    for (uintptr_t off = 0; off < li.size; off += 4096) {
        size_t n = std::min<size_t>(4096, li.size - off);
        bool ok = safe_read(li.base + off, page.data(), n);
        if (!ok) { memset(page.data(), 0, n); zero_pages++; }
        fwrite(page.data(), 1, n, f);
    }
    fclose(f);
    log("libil2cpp memory dump: %zu bytes (base 0x%llx), %zu unreadable pages",
        li.size, (unsigned long long)li.base, zero_pages);
    return true;
}

// Check if address p holds a valid Il2CppGlobalMetadataHeader (magic 0xFAB11BAF or structural match
// when the game zeroes/corrupts the magic bytes in memory).
static bool try_extract_metadata_at(uintptr_t p, bool allow_structural = false) {
    int32_t hdr[80] = {};
    if (!safe_read(p, hdr, sizeof hdr)) return false;

    uint32_t magic = static_cast<uint32_t>(hdr[0]);
    int version = hdr[1];
    bool magic_ok = (magic == 0xFAB11BAF);

    if (!magic_ok) {
        if (!allow_structural) return false;
        // Structural check: hdr[2] is stringLiteralOffset (typically 0x100..0x200),
        // followed by contiguous/aligned (offset, size) pairs.
        if (hdr[2] < 0xC0 || hdr[2] > 0x280 || (hdr[2] & 3) != 0) return false;
        int valid_pairs = 0;
        int32_t prev_end = hdr[2];
        for (int i = 2; i + 1 < 56; i += 2) {
            int32_t off = hdr[i], sz = hdr[i + 1];
            if (off < hdr[2] || sz < 0 || off > 0x20000000 || sz > 0x20000000) break;
            // In Il2CppGlobalMetadataHeader, each section offset starts right at or aligned after prev_end
            if (off < prev_end || off > prev_end + 64) break;
            prev_end = off + sz;
            valid_pairs++;
        }
        if (valid_pairs < 20) return false;
        if (version < 16 || version > 45) {
            // Infer metadata version from header size (hdr[2])
            if (hdr[2] == 0x100) version = 24;
            else if (hdr[2] == 0x108 || hdr[2] == 0x110) version = 27;
            else version = 29;
        }
    } else {
        if (version < 16 || version > 45) return false;
    }

    int64_t total = 0;
    for (int i = 2; i + 1 < 80; i += 2) {
        int32_t off = hdr[i], sz = hdr[i + 1];
        if (off < 0x80 || sz < 0 || off > 0x30000000) break;
        if ((int64_t)off + sz > total) total = (int64_t)off + sz;
    }
    if (total <= 0x1000 || total > 0x30000000) return false;

    log("[Unity] Detected global-metadata v%d at %p, size %lld bytes (%s)",
        version, (void *)p, (long long)total,
        magic_ok ? "magic 0xFAB11BAF" : "reconstructed header");
    set_engine_version("metadata v" + std::to_string(version));

    FILE *f = fopen((g_out + "/global-metadata.dat").c_str(), "wb");
    if (!f) return false;
    std::vector<char> buf(1 << 16);
    for (int64_t off = 0; off < total; off += (int64_t)buf.size()) {
        size_t n = (size_t)std::min<int64_t>(buf.size(), total - off);
        if (!safe_read(p + off, buf.data(), n))
            memset(buf.data(), 0, n);
        if (off == 0 && !magic_ok && n >= 8) {
            // Restore standard magic and version in the dumped file
            uint32_t fixed_magic = 0xFAB11BAF;
            int32_t fixed_ver = version;
            memcpy(buf.data(), &fixed_magic, 4);
            memcpy(buf.data() + 4, &fixed_ver, 4);
        }
        fwrite(buf.data(), 1, n, f);
    }
    fclose(f);
    return true;
}

// Locate decrypted global-metadata.dat in memory via .bss pointer scan + Scudo/page scan
static bool dump_metadata(const std::string &il2cpp_lib_name) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return false;
    struct Region { uintptr_t s, e; bool is_il2cpp_rw; };
    std::vector<Region> regs;
    char line[512];
    while (fgets(line, sizeof line, maps)) {
        unsigned long long s = 0, e = 0;
        char perms[8] = {};
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        if (strstr(line, "/dev/") || strstr(line, "[vvar") || strstr(line, "[vsyscall")) continue;
        bool il2cpp_rw = (perms[1] == 'w' &&
                          (strstr(line, "libil2cpp.so") != nullptr ||
                           (!il2cpp_lib_name.empty() && strstr(line, il2cpp_lib_name.c_str()) != nullptr)));
        regs.push_back({(uintptr_t)s, (uintptr_t)e, il2cpp_rw});
    }
    fclose(maps);

    auto is_in_readable = [&](uintptr_t addr) {
#if defined(__aarch64__)
        addr &= 0x00FFFFFFFFFFFFFFULL;
#endif
        if (addr < 0x10000) return false;
        for (const auto &r : regs) {
            if (addr >= r.s && addr + 256 <= r.e) return true;
        }
        return false;
    };

    // Pass 1: .bss / .data pointers in libil2cpp.so (exact magic or structural match)
    for (bool structural : {false, true}) {
        for (const auto &r : regs) {
            if (!r.is_il2cpp_rw) continue;
            for (uintptr_t p = r.s; p + sizeof(uintptr_t) <= r.e; p += sizeof(uintptr_t)) {
                uintptr_t target = 0;
                if (!safe_read(p, &target, sizeof target)) continue;
#if defined(__aarch64__)
                target &= 0x00FFFFFFFFFFFFFFULL;
#endif
                if (is_in_readable(target) && try_extract_metadata_at(target, structural)) {
                    return true;
                }
            }
        }
    }

    // Pass 2: Page / Scudo allocation scan across all large mappings
    const uintptr_t scudo_offs[] = {0x0, 0x10, 0x20, 0x30, 0x40};
    for (bool structural : {false, true}) {
        for (const auto &r : regs) {
            if (r.e - r.s < 0x10000) continue;
            for (uintptr_t page = r.s; page + 4096 <= r.e; page += 4096) {
                for (uintptr_t off : scudo_offs) {
                    if (try_extract_metadata_at(page + off, structural)) return true;
                }
            }
        }
    }
    log("[Unity] global-metadata not found in memory");
    return false;
}

// Auto-scan for Il2CppCodeRegistration in .data/.bss when exports are stripped
static void scan_il2cpp_registrations(const LibInfo &li) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    struct Seg { uintptr_t s, e; };
    std::vector<Seg> rw_segs;
    char line[512];
    while (fgets(line, sizeof line, maps)) {
        unsigned long long s = 0, e = 0;
        char perms[8] = {};
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perms) != 3) continue;
        if (perms[0] == 'r' && perms[1] == 'w' &&
            (strstr(line, "libil2cpp.so") || (s >= li.base && e <= li.base + li.size))) {
            rw_segs.push_back({(uintptr_t)s, (uintptr_t)e});
        }
    }
    fclose(maps);

    for (const auto &seg : rw_segs) {
        for (uintptr_t p = seg.s; p + 16 * sizeof(uintptr_t) <= seg.e; p += sizeof(uintptr_t)) {
            uintptr_t pair[2] = {};
            if (!safe_read(p, pair, sizeof pair)) continue;
            uintptr_t count = pair[0], arr = pair[1];
            if (count >= 500 && count <= 500000 && arr >= li.base && arr < li.base + li.size) {
                uintptr_t fns[8] = {};
                if (!safe_read(arr, fns, sizeof fns)) continue;
                bool valid_fn_array = true;
                for (int k = 0; k < 8; ++k) {
                    if (fns[k] < li.base || fns[k] >= li.base + li.size) {
                        valid_fn_array = false;
                        break;
                    }
                }
                if (valid_fn_array) {
                    log("[Unity] Detected CodeRegistration table candidate at VA 0x%llx (RVA 0x%llx, %zu methods)",
                        (unsigned long long)p, (unsigned long long)(p - li.base), (size_t)count);
                    break;
                }
            }
        }
    }
}

static bool g_api_resolved = false;

static void do_dump() {
    g_status.state = State::Dumping;
    set_status_detail("Preparing output directory...");
    log("--- dump started (%s) ---", engine_name(g_status.engine.load()));
    if (!prepare_output()) {
        log("no writable output directory");
        set_status_detail("Failed: no writable output directory");
        g_status.state = State::Failed;
        return;
    }
    log("output: %s", g_out.c_str());
    set_status_detail("Output: %s", g_out.c_str());

    if (g_status.engine.load() == Engine::UE4) {
        const char *lib_to_find = !g_target_lib_name.empty() ? g_target_lib_name.c_str() : "libUE4.so";
        LibInfo li = find_lib(lib_to_find);
        if (!li.base) li = find_lib("libUnreal.so");
        bool ok = ue4::dump_ue4(g_cfg, g_pkg, li.base, li.size);
        log("--- UE4/UE5 dump finished: %s ---", g_out.c_str());
        set_status_detail(ok ? "UE4/UE5 dump finished: %s" : "UE4/UE5 dump failed - see dump.log", g_out.c_str());
        g_status.state = ok ? State::Done : State::Failed;
        return;
    }

    // Unity (IL2CPP) Dump Pipeline
    install_fault_handler();
    struct Restore { ~Restore() { restore_fault_handler(); } } restore_guard;

    const char *lib_to_find = !g_target_lib_name.empty() ? g_target_lib_name.c_str() : "libil2cpp.so";
    LibInfo li = find_lib(lib_to_find);
    if (g_cfg.dump_metadata) {
        set_status_detail("Unity: searching global-metadata.dat in memory...");
        dump_metadata(lib_to_find);
    }
    if (g_cfg.dump_so && li.base) {
        set_status_detail("Unity: dumping %s memory...", lib_to_find);
        dump_so_memory(li);
    }

    if (!g_api_resolved) {
        set_status_detail("Unity: scanning registration structures...");
        log("[Unity] Exports stripped: scanning for registration structures...");
        scan_il2cpp_registrations(li);
        log("--- dump finished (memory + metadata mode): %s ---", g_out.c_str());
        set_status_detail("Unity dump finished (memory + metadata): %s", g_out.c_str());
        g_status.state = State::Done;
        return;
    }

    {
        P domain = nullptr;
        size_t n = 0;
        guarded([&] {
            domain = il2cpp_domain_get();
            if (domain) il2cpp_domain_get_assemblies(domain, &n);
        });
        if (!domain || n == 0) {
            log("IL2CPP runtime is not initialised yet - press Dump Now in a few seconds");
            set_status_detail("IL2CPP runtime not initialised - press Dump Now to retry");
            g_status.state = State::Ready;
            return;
        }
    }

    set_status_detail("Unity: dumping classes...");
    bool ok = guarded([&] {
        if (g_cfg.dump_cs || g_cfg.dump_script_json) dump_cs_and_json(li.base, li.size);
    });
    if (!ok) log("fault during dump.cs generation");
    log("--- Unity dump finished: %s ---", g_out.c_str());
    set_status_detail(ok ? "Unity dump finished: %s" : "Unity dump failed - see dump.log", g_out.c_str());
    g_status.state = ok ? State::Done : State::Failed;
}

// ---------------------------------------------------------------- worker
static void worker() {
    using namespace std::chrono_literals;
    log("Auto-detecting game engine (Unity IL2CPP / Unreal Engine 4 & 5)...");
    set_status_detail("Waiting for engine library (libil2cpp.so / libUE4.so / libUnreal.so)...");

    for (int iter = 0; ; ++iter) {
        if (!g_cfg.custom_lib.empty()) {
            if ((g_handle = dlopen(g_cfg.custom_lib.c_str(), RTLD_NOW | RTLD_NOLOAD)) ||
                find_lib(g_cfg.custom_lib.c_str()).base != 0) {
                g_target_lib_name = g_cfg.custom_lib;
                if (g_cfg.engine_override == 2) g_status.engine = Engine::UE4;
                else if (g_cfg.engine_override == 1) g_status.engine = Engine::Unity;
                else {
                    bool has_il2cpp = (g_handle && (dlsym(g_handle, "il2cpp_init") || dlsym(g_handle, "il2cpp_domain_get")));
                    g_status.engine = has_il2cpp ? Engine::Unity : Engine::UE4;
                }
                break;
            }
        }
        if (g_cfg.engine_override != 2) {
            if ((g_handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD)) ||
                find_lib("libil2cpp.so").base != 0) {
                g_status.engine = Engine::Unity;
                g_target_lib_name = "libil2cpp.so";
                break;
            }
        }
        if (g_cfg.engine_override != 1) {
            if ((g_handle = dlopen("libUE4.so", RTLD_NOW | RTLD_NOLOAD)) ||
                find_lib("libUE4.so").base != 0) {
                g_status.engine = Engine::UE4;
                g_target_lib_name = "libUE4.so";
                break;
            }
            if ((g_handle = dlopen("libUnreal.so", RTLD_NOW | RTLD_NOLOAD)) ||
                find_lib("libUnreal.so").base != 0) {
                g_status.engine = Engine::UE4;
                g_target_lib_name = "libUnreal.so";
                break;
            }
        }
        // Every 2 seconds, scan loaded libraries in case libil2cpp.so was renamed
        if (g_cfg.engine_override != 2 && iter > 0 && (iter % 20) == 0) {
            std::string renamed = detect_renamed_il2cpp_lib();
            if (!renamed.empty()) {
                g_handle = dlopen(renamed.c_str(), RTLD_NOW | RTLD_NOLOAD);
                g_target_lib_name = renamed;
                g_status.engine = Engine::Unity;
                log("[Unity] Detected renamed IL2CPP library: %s", renamed.c_str());
                break;
            }
        }
        std::this_thread::sleep_for(100ms);
    }

    uint32_t wait_ms = g_cfg.init_delay_ms;

    if (g_status.engine.load() == Engine::UE4) {
        LibInfo li = find_lib(g_target_lib_name.c_str());
        g_status.il2cpp_base = li.base;
        log("[UE4] Detected %s @ %p size 0x%zx", g_target_lib_name.c_str(), (void *)li.base, li.size);
        if (g_cfg.auto_dump) {
            g_status.state = State::WaitingForRuntime;
            set_status_detail("UE4/UE5 detected, waiting %u ms for runtime init...", wait_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
            do_dump();
        } else {
            g_status.state = State::Ready;
            set_status_detail("UE4/UE5 ready - press [Dump Now] in overlay");
        }
    } else {
        LibInfo li = find_lib(g_target_lib_name.c_str());
        g_status.il2cpp_base = li.base;
        log("[Unity] Detected %s @ %p size 0x%zx", g_target_lib_name.c_str(), (void *)li.base, li.size);

        g_status.state = State::WaitingForRuntime;
        g_api_resolved = resolve_api(g_target_lib_name.c_str());
        if (!g_cfg.auto_dump) {
            g_status.state = State::Ready;
            set_status_detail("Unity ready - press [Dump Now] in overlay");
        } else if (!g_api_resolved) {
            log("[Unity] Standard IL2CPP exports are stripped/hidden; switching to structure-scan & memory dump mode");
            set_status_detail("Unity: IL2CPP exports stripped, waiting %u ms...", wait_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
            do_dump();
        } else {
            log("[Unity] Waiting for IL2CPP runtime & assemblies...");
            set_status_detail("Unity: waiting for IL2CPP runtime & assemblies...");
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));

            bool ready = false;
            for (int retry = 0; retry < 60; ++retry) {
                P domain = nullptr;
                size_t n = 0;
                guarded([&] {
                    domain = il2cpp_domain_get();
                    if (domain) il2cpp_domain_get_assemblies(domain, &n);
                });
                if (domain && n > 0) {
                    log("[Unity] IL2CPP runtime ready (%zu assemblies detected). Starting automatic dump...", n);
                    ready = true;
                    break;
                }
                std::this_thread::sleep_for(500ms);
            }

            if (!ready) {
                log("[Unity] Timeout waiting for assemblies, attempting dump anyway...");
            }
            do_dump();
        }
    }

    for (;;) {
        std::this_thread::sleep_for(500ms);
        // Fallback trigger for subsequent dumps: create <app files>/dump_now
        if (!g_status.request) {
            std::string t1 = "/sdcard/Android/data/" + g_pkg + "/files/dump_now";
            if (access(t1.c_str(), F_OK) == 0) {
                unlink(t1.c_str());
                g_status.request = true;
            }
        }
        if (g_status.request.exchange(false)) do_dump();
    }
}

void start(const Config &cfg, const std::string &package) {
    g_cfg = cfg;
    g_pkg = package;

    log("[MOD] target process: %s", package.c_str());
    std::string pkgs;
    for (auto &p : cfg.packages) pkgs += (pkgs.empty() ? "" : ", ") + p;
    log("[MOD] config packages: %s", pkgs.empty() ? "(none)" : pkgs.c_str());
    log("[MOD] options: auto_dump=%d overlay=%d delay=%ums engine=%d custom_lib=%s",
        (int)cfg.auto_dump, (int)cfg.overlay, cfg.init_delay_ms, cfg.engine_override,
        cfg.custom_lib.empty() ? "(auto)" : cfg.custom_lib.c_str());
    log("[MOD] dump flags: cs=%d metadata=%d so=%d script_json=%d",
        (int)cfg.dump_cs, (int)cfg.dump_metadata, (int)cfg.dump_so, (int)cfg.dump_script_json);

    if (cfg.ue4_gnames || cfg.ue4_guobjectarray || cfg.ue4_gnames_mode || cfg.ue4_guobj_mode ||
        cfg.ue4_fnamepool_stride || cfg.ue4_fnamepool_shift || cfg.ue4_item_size ||
        cfg.ue4_elements_per_chunk || cfg.ue4_num_elements || cfg.ue4_off_internal_index ||
        cfg.ue4_off_class || cfg.ue4_off_name || cfg.ue4_off_outer || cfg.ue4_off_super ||
        cfg.ue4_off_children || cfg.ue4_off_child_props || cfg.ue4_off_properties_size ||
        cfg.ue4_off_prop_offset || cfg.ue4_off_func || cfg.ue4_use_ffield >= 0) {
        log("[MOD] UE4 config: GNames=0x%llx GUObjectArray=0x%llx mode=%d/%d stride=%u shift=%u item=%u epc=%u num=%u ffield=%d",
            (unsigned long long)cfg.ue4_gnames, (unsigned long long)cfg.ue4_guobjectarray,
            cfg.ue4_gnames_mode, cfg.ue4_guobj_mode, cfg.ue4_fnamepool_stride, cfg.ue4_fnamepool_shift,
            cfg.ue4_item_size, cfg.ue4_elements_per_chunk, cfg.ue4_num_elements, cfg.ue4_use_ffield);
    } else {
        log("[MOD] UE4 config: no manual offsets, auto-detect will be used");
    }
    for (auto &n : cfg.notes) log("[MOD] config warning: %s", n.c_str());

    std::thread(worker).detach();
}

}  // namespace dumper
