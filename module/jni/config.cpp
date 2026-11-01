#include "config.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

static std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static bool parse_u64(const std::string &v, uint64_t &out) {
    if (v.empty()) return false;
    // Strip optional '_' separators (e.g. 0x4B7_C3C0)
    std::string clean;
    clean.reserve(v.size());
    for (char c : v) {
        if (c != '_') clean += c;
    }
    if (clean.empty()) return false;
    const char *s = clean.c_str();
    char *end = nullptr;
    bool hex_prefix = clean.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    unsigned long long n = strtoull(s, &end, hex_prefix ? 16 : 10);
    if (end != s && *end == '\0') {
        out = n;
        return true;
    }
    // Bare hex without the 0x prefix, e.g. "4B7C3C0" as pasted from UE4 dumps.
    n = strtoull(s, &end, 16);
    if (end == s || *end != '\0') return false;
    out = n;
    return true;
}

static bool parse_u32(const std::string &v, uint32_t &out) {
    uint64_t n = 0;
    if (!parse_u64(v, n) || n > 0xFFFFFFFFull) return false;
    out = static_cast<uint32_t>(n);
    return true;
}

static std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

Config parse_config(const std::string &text) {
    Config c;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = lower(trim(line.substr(0, eq)));
        std::string v = trim(line.substr(eq + 1));
        auto hash = v.find('#');
        if (hash != std::string::npos) v = trim(v.substr(0, hash));
        std::string lv = lower(v);
        bool on = lv != "0" && lv != "false" && lv != "no" && lv != "off";

        // Accepted aliases (key names are case-insensitive).
        if (k == "gnames" || k == "ue4_gnames_rva" || k == "ue4_gnames_offset") k = "ue4_gnames";
        else if (k == "gobjects" || k == "guobjectarray" || k == "ue4_gobjects" ||
                 k == "ue4_guobject_array" || k == "ue4_guobjectarray_rva" ||
                 k == "ue4_guobjectarray_offset") k = "ue4_guobjectarray";
        else if (k == "lib" || k == "target_lib") k = "custom_lib";
        else if (k == "delay_ms" || k == "wait_ms") k = "init_delay_ms";

        auto num64 = [&](uint64_t &dst) {
            if (parse_u64(v, dst)) return;
            c.notes.push_back("invalid number for " + k + ": '" + v + "'");
        };
        auto num32 = [&](uint32_t &dst) {
            if (parse_u32(v, dst)) return;
            c.notes.push_back("invalid number for " + k + ": '" + v + "'");
        };

        if ((k == "package" || k == "pkg" || k == "target") && !v.empty()) c.packages.push_back(v);
        else if (k == "output" && !v.empty()) c.output = v;
        else if (k == "custom_lib" && !v.empty()) c.custom_lib = v;
        else if (k == "auto_dump") c.auto_dump = on;
        else if (k == "overlay") c.overlay = on;
        else if (k == "init_delay_ms") num32(c.init_delay_ms);
        else if (k == "engine") {
            if (lv == "auto" || lv == "0") c.engine_override = 0;
            else if (lv == "unity" || lv == "il2cpp" || lv == "1") c.engine_override = 1;
            else if (lv == "ue4" || lv == "ue5" || lv == "unreal" || lv == "2") c.engine_override = 2;
            else c.notes.push_back("invalid engine: '" + v + "' (auto|unity|ue4)");
        }
        else if (k == "dump_cs") c.dump_cs = on;
        else if (k == "dump_metadata") c.dump_metadata = on;
        else if (k == "dump_so") c.dump_so = on;
        else if (k == "dump_script_json") c.dump_script_json = on;
        else if (k == "ue4_gnames") num64(c.ue4_gnames);
        else if (k == "ue4_guobjectarray") num64(c.ue4_guobjectarray);
        else if (k == "ue4_gnames_mode") {
            if (lv == "auto" || lv == "0") c.ue4_gnames_mode = 0;
            else if (lv == "fnamepool") c.ue4_gnames_mode = 1;
            else if (lv == "tnameentryarray" || lv == "nameentryarray") c.ue4_gnames_mode = 2;
            else c.notes.push_back("invalid ue4_gnames_mode: '" + v + "' (fnamepool|tnameentryarray)");
        } else if (k == "ue4_guobj_mode") {
            if (lv == "auto" || lv == "0") c.ue4_guobj_mode = 0;
            else if (lv == "chunked") c.ue4_guobj_mode = 1;
            else if (lv == "fixed") c.ue4_guobj_mode = 2;
            else c.notes.push_back("invalid ue4_guobj_mode: '" + v + "' (chunked|fixed)");
        } else if (k == "ue4_fnamepool_stride") num32(c.ue4_fnamepool_stride);
        else if (k == "ue4_fnamepool_shift") num32(c.ue4_fnamepool_shift);
        else if (k == "ue4_item_size") num32(c.ue4_item_size);
        else if (k == "ue4_elements_per_chunk") num32(c.ue4_elements_per_chunk);
        else if (k == "ue4_num_elements") num32(c.ue4_num_elements);
        else if (k == "ue4_off_internal_index") num32(c.ue4_off_internal_index);
        else if (k == "ue4_off_class") num32(c.ue4_off_class);
        else if (k == "ue4_off_name") num32(c.ue4_off_name);
        else if (k == "ue4_off_outer") num32(c.ue4_off_outer);
        else if (k == "ue4_off_super") num32(c.ue4_off_super);
        else if (k == "ue4_off_children") num32(c.ue4_off_children);
        else if (k == "ue4_off_child_props") num32(c.ue4_off_child_props);
        else if (k == "ue4_off_properties_size") num32(c.ue4_off_properties_size);
        else if (k == "ue4_off_prop_offset") num32(c.ue4_off_prop_offset);
        else if (k == "ue4_off_func") num32(c.ue4_off_func);
        else if (k == "ue4_use_ffield") {
            if (lv == "auto" || lv == "-1") c.ue4_use_ffield = -1;
            else if (lv == "1" || lv == "true" || lv == "ffield") c.ue4_use_ffield = 1;
            else if (lv == "0" || lv == "false" || lv == "ufield") c.ue4_use_ffield = 0;
            else c.notes.push_back("invalid ue4_use_ffield: '" + v + "' (0|1)");
        } else {
            c.notes.push_back("unknown key: " + k);
        }
    }
    return c;
}

static bool is_auxiliary_subprocess(const std::string &sub) {
    // Skip non-game helper processes like :push, :webview, :crashpad, :sandboxed_process*
    static const char *const kIgnoredPrefixes[] = {
        "push", "webview", "sandboxed_", "privileged_", "crash", "bugly",
        "metrics", "analytics", "daemon", "service", "ipc", "download", "work"
    };
    std::string ls = lower(sub);
    for (const char *p : kIgnoredPrefixes) {
        if (ls.rfind(p, 0) == 0) return true;
    }
    return false;
}

bool Config::matches(const std::string &proc) const {
    if (proc.empty()) return false;
    for (const auto &p : packages) {
        if (p == "*" || proc == p) return true;
        // If user didn't specify a subprocess in config, match game subprocesses
        // (e.g. com.game:main, com.game:unity) while ignoring auxiliary helpers.
        if (p.find(':') == std::string::npos && proc.rfind(p + ":", 0) == 0) {
            std::string sub = proc.substr(p.size() + 1);
            if (!is_auxiliary_subprocess(sub)) return true;
        }
    }
    return false;
}
