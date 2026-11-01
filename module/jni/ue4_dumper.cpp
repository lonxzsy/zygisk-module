#include "ue4_dumper.h"
#include "il2cpp_dumper.h"

#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ue4 {

struct MemRange {
    uintptr_t start;
    uintptr_t end;
    bool writable;
    bool executable;
    bool is_ue4;
};

static std::vector<MemRange> g_ranges;
static std::vector<MemRange> g_ue4_rw;
static uintptr_t g_lib_base = 0;
static size_t g_lib_size = 0;
static bool g_manual_gnames = false;
static bool g_manual_guobj = false;

// Config values are RVAs from libUE4.so base in most dumps, but people often
// paste absolute in-memory VAs - accept both. 64-bit VAs are far above any
// realistic module RVA, so the size of the value disambiguates reliably.
static uintptr_t resolve_manual_addr(uint64_t v) {
    if (!v) return 0;
    uintptr_t a = (uintptr_t)v;
    if (!g_lib_base) return a;
    if (a >= g_lib_base && a < g_lib_base + g_lib_size) return a;  // matches module VA
#if defined(__aarch64__)
    if (a >= 0x100000000ULL) return a;                             // 64-bit absolute VA
#endif
    return g_lib_base + a;                                         // RVA
}

static void refresh_maps() {
    g_ranges.clear();
    g_ue4_rw.clear();
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long s = 0, e = 0;
        char perms[8] = {};
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        if (strstr(line, "/dev/") || strstr(line, "[vvar") || strstr(line, "[vsyscall")) continue;
        bool w = (perms[1] == 'w');
        bool x = (perms[2] == 'x');
        bool is_ue4 = (strstr(line, "libUE4.so") != nullptr || strstr(line, "libUnreal.so") != nullptr ||
                       (g_lib_base && s >= g_lib_base && e <= g_lib_base + g_lib_size));
        MemRange r{(uintptr_t)s, (uintptr_t)e, w, x, is_ue4};
        if (!g_ranges.empty() && g_ranges.back().end == r.start &&
            g_ranges.back().writable == r.writable && g_ranges.back().is_ue4 == r.is_ue4) {
            g_ranges.back().end = r.end;
        } else {
            g_ranges.push_back(r);
        }
        if (is_ue4 && w) {
            if (!g_ue4_rw.empty() && g_ue4_rw.back().end == r.start) {
                g_ue4_rw.back().end = r.end;
            } else {
                g_ue4_rw.push_back(r);
            }
        }
    }
    fclose(f);
}

static bool is_readable(uintptr_t addr, size_t len = sizeof(uintptr_t)) {
#if defined(__aarch64__)
    addr &= 0x00FFFFFFFFFFFFFFULL;
#endif
    if (addr < 0x10000 || addr + len < addr) return false;
    uintptr_t end = addr + len;
    int lo = 0, hi = (int)g_ranges.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (g_ranges[mid].end <= addr) lo = mid + 1;
        else if (g_ranges[mid].start > addr) hi = mid - 1;
        else return end <= g_ranges[mid].end;
    }
    return false;
}

static bool is_ue4_code_ptr(uintptr_t addr) {
#if defined(__aarch64__)
    addr &= 0x00FFFFFFFFFFFFFFULL;
#endif
    if (!g_lib_base || !g_lib_size) return false;
    return addr >= g_lib_base && addr < g_lib_base + g_lib_size;
}

template <typename T>
static bool read_val(uintptr_t addr, T &out) {
    if (!is_readable(addr, sizeof(T))) return false;
    return dumper::safe_read(addr, &out, sizeof(T));
}

static bool is_valid_ascii_name(const char *s, int max_len = 64) {
    if (!s || !s[0]) return false;
    for (int i = 0; i < max_len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0) return i > 0;
        if (c < 0x20 || c > 0x7E) return false;
    }
    return false;
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

// ------------------------------------------------------------------ GNames Structures
enum class GNamesMode { None, FNamePool, TNameEntryArray };

struct GNamesProfile {
    GNamesMode mode = GNamesMode::None;
    uintptr_t gnames_va = 0;        // Address in .bss or heap
    uintptr_t blocks_array_va = 0;  // Address of Blocks[] or Chunks[] array
    uint32_t num_blocks = 0;

    // For FNamePool (UE 4.23+ / UE5)
    uint32_t pool_stride = 2;       // Alignment stride (usually 2 or 4)
    uint32_t header_offset = 0;     // Offset of 2-byte header before string
    uint32_t string_offset = 2;     // Offset of string inside FNameEntry
    uint32_t len_shift = 6;         // header >> len_shift gives length

    // For TNameEntryArray (UE 4.11 - 4.22)
    uint32_t chunk_elements = 16384; // 0x4000
    uint32_t entry_str_off = 16;     // Offset of AnsiName inside FNameEntry
};

static GNamesProfile g_names;

// Check if a memory block looks like FNamePool Blocks[0] starting with "None" and "ByteProperty"
static bool probe_fnamepool_block0(uintptr_t block0, GNamesProfile &prof) {
    if (!is_readable(block0, 128)) return false;
    uint8_t buf[128] = {};
    if (!dumper::safe_read(block0, buf, sizeof buf))
        return false;

    // Look for "None" within first 16 bytes
    int none_off = -1;
    for (int i = 2; i <= 12; i += 2) {
        if (memcmp(buf + i, "None", 4) == 0 && buf[i + 4] != 'a') {
            none_off = i;
            break;
        }
    }
    if (none_off < 2) return false;

    // Look for "ByteProperty" right after "None" (within 16 bytes)
    int byte_off = -1;
    for (int i = none_off + 4; i <= none_off + 20; i += 2) {
        if (memcmp(buf + i, "ByteProperty", 12) == 0) {
            byte_off = i;
            break;
        }
    }
    if (byte_off < 0) return false;

    // Look for "IntProperty" right after "ByteProperty"
    int int_off = -1;
    for (int i = byte_off + 12; i <= byte_off + 28; i += 2) {
        if (memcmp(buf + i, "IntProperty", 11) == 0) {
            int_off = i;
            break;
        }
    }
    if (int_off < 0) return false;

    // Calibrate header encoding from "None" (len 4) and "ByteProperty" (len 12)
    uint16_t hdr_none = *reinterpret_cast<uint16_t *>(buf + none_off - 2);
    uint16_t hdr_byte = *reinterpret_cast<uint16_t *>(buf + byte_off - 2);

    uint32_t shift = 6;
    for (uint32_t s : {6u, 1u, 5u, 4u, 0u}) {
        if (((hdr_none >> s) & 0x3FF) == 4 && ((hdr_byte >> s) & 0x3FF) == 12) {
            shift = s;
            break;
        }
    }

    uint32_t entry0_start = none_off - 2;
    uint32_t entry1_start = byte_off - 2;
    uint32_t diff = entry1_start - entry0_start; // For "None" (2 hdr + 4 chars = 6 bytes; if aligned to 4 -> 8)
    uint32_t stride = (diff == 8) ? 4 : 2;

    prof.mode = GNamesMode::FNamePool;
    prof.header_offset = entry0_start; // initial offset in block0 (usually 0)
    prof.string_offset = 2;
    prof.len_shift = shift;
    prof.pool_stride = stride;
    return true;
}

// Probe an FNamePool-like object. The Blocks[] pointer array sits after
// Lock/CurrentBlock/CurrentByteCursor, but builds differ slightly, so try the
// usual offsets.
static bool probe_pool_with_layout_offsets(uintptr_t obj_va, GNamesProfile &prof) {
    const uintptr_t offs[] = {0x10, 0x08, 0x18, 0x20, 0x28};
    for (uintptr_t off : offs) {
        uintptr_t block0 = 0;
        if (!read_val(obj_va + off, block0) || !is_readable(block0, 128)) continue;
        GNamesProfile p;
        if (!probe_fnamepool_block0(block0, p)) continue;
        p.gnames_va = obj_va;
        p.blocks_array_va = obj_va + off;
        prof = p;
        return true;
    }
    return false;
}

// Probe a Blocks[] array given directly (GNames points past the pool header)
static bool probe_blocks_array(uintptr_t blocks_arr, GNamesProfile &prof) {
    if (blocks_arr < 0x10) return false;
    uintptr_t block0 = 0;
    if (!read_val(blocks_arr, block0) || !is_readable(block0, 128)) return false;
    if (!probe_fnamepool_block0(block0, prof)) return false;
    prof.gnames_va = blocks_arr - 0x10;
    prof.blocks_array_va = blocks_arr;
    return true;
}

static uint32_t count_name_blocks(uintptr_t blocks_array_va) {
    uint32_t nb = 0;
    for (uint32_t b = 0; b < 8192; ++b) {
        uintptr_t bp = 0;
        if (!read_val(blocks_array_va + b * sizeof(uintptr_t), bp) || !is_readable(bp, 64)) break;
        nb++;
    }
    return nb;
}

// Check if a pointer array looks like TNameEntryArray::Chunks[]
static bool probe_tnameentry_chunks(uintptr_t chunks_va, GNamesProfile &prof) {
    uintptr_t chunk0 = 0;
    if (!read_val(chunks_va, chunk0) || !is_readable(chunk0, sizeof(uintptr_t) * 8)) return false;

    uintptr_t e0 = 0, e1 = 0, e2 = 0;
    if (!read_val(chunk0 + 0 * sizeof(uintptr_t), e0) ||
        !read_val(chunk0 + 1 * sizeof(uintptr_t), e1) ||
        !read_val(chunk0 + 2 * sizeof(uintptr_t), e2)) return false;

    if (!is_readable(e0, 48) || !is_readable(e1, 48) || !is_readable(e2, 48)) return false;

    char b0[48] = {}, b1[48] = {}, b2[48] = {};
    if (!dumper::safe_read(e0, b0, sizeof b0) ||
        !dumper::safe_read(e1, b1, sizeof b1) ||
        !dumper::safe_read(e2, b2, sizeof b2)) return false;

    for (uint32_t off = sizeof(uintptr_t); off <= 28; off += 4) {
        if (strcmp(b0 + off, "None") == 0 &&
            strcmp(b1 + off, "ByteProperty") == 0 &&
            strcmp(b2 + off, "IntProperty") == 0) {
            prof.mode = GNamesMode::TNameEntryArray;
            prof.blocks_array_va = chunks_va;
            prof.entry_str_off = off;
            prof.chunk_elements = 16384;
            uint32_t num_chunks = 0;
            for (uint32_t c = 0; c < 128; ++c) {
                uintptr_t cp = 0;
                if (!read_val(chunks_va + c * sizeof(uintptr_t), cp) || !is_readable(cp, 64)) break;
                num_chunks++;
            }
            prof.num_blocks = num_chunks;
            return true;
        }
    }
    return false;
}

static bool detect_gnames() {
    // 1. Scan libUE4.so / libUnreal.so writable (.data / .bss) segments
    for (const auto &rw : g_ue4_rw) {
        for (uintptr_t p = rw.start; p + sizeof(uintptr_t) <= rw.end; p += sizeof(uintptr_t)) {
            uintptr_t val = 0;
            if (!read_val(p, val) || !is_readable(val, 64)) continue;

            // Check if 'val' is Blocks[0] of an inline FNamePool in .bss
            GNamesProfile prof;
            if (probe_fnamepool_block0(val, prof)) {
                prof.gnames_va = p - 16; // Lock (8) + CurrentBlock (4) + CurrentByteCursor (4)
                prof.blocks_array_va = p;
                prof.num_blocks = count_name_blocks(p);
                g_names = prof;
                dumper::log("[UE4] Detected GNames structure: FNamePool (UE 4.23+ / UE5)");
                dumper::log("[UE4] FNamePool Blocks VA: 0x%llx (RVA: 0x%llx), Blocks: %u, shift: %u, stride: %u",
                            (unsigned long long)p,
                            (unsigned long long)(p - g_lib_base),
                            prof.num_blocks, prof.len_shift, prof.pool_stride);
                return true;
            }

            // Check if 'val' is a pointer to TNameEntryArray (GNames pointer in .bss)
            if (probe_tnameentry_chunks(val, prof)) {
                prof.gnames_va = p;
                g_names = prof;
                dumper::log("[UE4] Detected GNames structure: TNameEntryArray* (UE 4.11-4.22)");
                dumper::log("[UE4] GNames Ptr VA: 0x%llx (RVA: 0x%llx) -> 0x%llx, Chunks: %u, StrOff: 0x%x",
                            (unsigned long long)p,
                            (unsigned long long)(p - g_lib_base),
                            (unsigned long long)val,
                            prof.num_blocks, prof.entry_str_off);
                return true;
            }

            // Also check if 'p' itself is Chunks[0] of an inline TNameEntryArray
            if (probe_tnameentry_chunks(p, prof)) {
                prof.gnames_va = p;
                g_names = prof;
                dumper::log("[UE4] Detected GNames structure: Inline TNameEntryArray");
                dumper::log("[UE4] GNames VA: 0x%llx (RVA: 0x%llx), Chunks: %u, StrOff: 0x%x",
                            (unsigned long long)p,
                            (unsigned long long)(p - g_lib_base),
                            prof.num_blocks, prof.entry_str_off);
                return true;
            }
        }
    }
    return false;
}

// Apply the GNames address from config.txt. Returns false when it does not
// validate, in which case the auto-scan fallback still runs.
static bool setup_manual_gnames(const Config &cfg) {
    uintptr_t va = resolve_manual_addr(cfg.ue4_gnames);
    if (!va) return false;

    GNamesProfile prof;
    bool ok = false;

    auto try_fnamepool = [&](uintptr_t cand) {
        if (probe_pool_with_layout_offsets(cand, prof)) return true;
        uintptr_t deref = 0;
        if (read_val(cand, deref) && deref) {
            if (probe_pool_with_layout_offsets(deref, prof)) return true;
            if (probe_blocks_array(deref, prof)) {
                prof.gnames_va = cand;
                return true;
            }
        }
        if (probe_blocks_array(cand, prof)) {
            prof.gnames_va = cand;
            return true;
        }
        return false;
    };
    // UE 4.11-4.22: GNames may be a pointer to TNameEntryArray, the array
    // itself, or its Chunks.Data pointer - walk the dereference chain.
    auto try_tnameentry = [&](uintptr_t cand) {
        uintptr_t cur = cand;
        for (int depth = 0; depth < 4; ++depth) {
            GNamesProfile p;
            if (probe_tnameentry_chunks(cur, p)) {
                p.gnames_va = cand;
                prof = p;
                return true;
            }
            uintptr_t next = 0;
            if (!read_val(cur, next) || !next) break;
            cur = next;
        }
        return false;
    };

    if (cfg.ue4_gnames_mode == 2) ok = try_tnameentry(va);
    else if (cfg.ue4_gnames_mode == 1) ok = try_fnamepool(va);
    else ok = try_fnamepool(va) || try_tnameentry(va);

    // Signature not recognized, but encoding hints were supplied: trust them.
    if (!ok && cfg.ue4_gnames_mode != 2 && cfg.ue4_fnamepool_stride && cfg.ue4_fnamepool_shift) {
        uintptr_t pool = va;
        uintptr_t deref = 0;
        if (read_val(va, deref) && deref && is_readable(deref + 0x10, 8)) pool = deref;
        uintptr_t blocks_arr = pool + 0x10;
        uintptr_t block0 = 0;
        if (read_val(blocks_arr, block0) && is_readable(block0, 64)) {
            prof = GNamesProfile{};
            prof.mode = GNamesMode::FNamePool;
            prof.gnames_va = pool;
            prof.blocks_array_va = blocks_arr;
            prof.string_offset = 2;
            prof.pool_stride = cfg.ue4_fnamepool_stride;
            prof.len_shift = cfg.ue4_fnamepool_shift;
            ok = true;
            dumper::log("[UE4] Manual GNames: block0 signature not found, using stride/shift from config.txt");
        }
    }

    if (!ok) {
        dumper::log("[UE4] Manual GNames 0x%llx (RVA 0x%llx): no FNamePool/TNameEntryArray signature found",
                    (unsigned long long)va, (unsigned long long)(g_lib_base ? va - g_lib_base : va));
        return false;
    }

    if (prof.mode == GNamesMode::FNamePool) {
        if (cfg.ue4_fnamepool_stride) prof.pool_stride = cfg.ue4_fnamepool_stride;
        if (cfg.ue4_fnamepool_shift) prof.len_shift = cfg.ue4_fnamepool_shift;
    }
    prof.num_blocks = count_name_blocks(prof.blocks_array_va);
    if (!prof.num_blocks) return false;

    g_names = prof;
    g_manual_gnames = true;
    dumper::log("[UE4] Using manual GNames from config.txt: %s @ 0x%llx (RVA: 0x%llx), Blocks: %u, stride: %u, shift: %u",
                prof.mode == GNamesMode::FNamePool ? "FNamePool" : "TNameEntryArray",
                (unsigned long long)prof.gnames_va,
                (unsigned long long)(prof.gnames_va - g_lib_base),
                prof.num_blocks, prof.pool_stride, prof.len_shift);
    return true;
}

static std::string get_name_by_index(uint32_t index) {
    if (g_names.mode == GNamesMode::FNamePool) {
        uint32_t block_idx = index >> 16;
        uint32_t offset_idx = index & 0xFFFF;
        if (block_idx >= g_names.num_blocks) return "";
        uintptr_t block_ptr = 0;
        if (!read_val(g_names.blocks_array_va + block_idx * sizeof(uintptr_t), block_ptr) || !block_ptr)
            return "";
        uintptr_t entry_va = block_ptr + (size_t)offset_idx * g_names.pool_stride;
        if (block_idx == 0 && offset_idx == 0) entry_va += g_names.header_offset;
        uint16_t header = 0;
        if (!read_val(entry_va, header)) return "";
        bool is_wide = (header & 1) != 0;
        uint32_t len = (header >> g_names.len_shift) & 0x3FF;
        if (len == 0 || len > 256 || is_wide) return "";
        uintptr_t str_va = entry_va + g_names.string_offset;
        if (!is_readable(str_va, len)) return "";
        char buf[260] = {};
        if (!dumper::safe_read(str_va, buf, len))
            return "";
        buf[len] = '\0';
        if (!is_valid_ascii_name(buf, (int)len + 1)) return "";
        return std::string(buf, len);
    } else if (g_names.mode == GNamesMode::TNameEntryArray) {
        uint32_t chunk_idx = index / g_names.chunk_elements;
        uint32_t within_idx = index % g_names.chunk_elements;
        if (chunk_idx >= g_names.num_blocks) return "";
        uintptr_t chunk_ptr = 0;
        if (!read_val(g_names.blocks_array_va + chunk_idx * sizeof(uintptr_t), chunk_ptr) || !chunk_ptr)
            return "";
        uintptr_t entry_ptr = 0;
        if (!read_val(chunk_ptr + within_idx * sizeof(uintptr_t), entry_ptr) || !entry_ptr)
            return "";
        uintptr_t str_va = entry_ptr + g_names.entry_str_off;
        if (!is_readable(str_va, 16)) return "";
        char buf[256] = {};
        if (!dumper::safe_read(str_va, buf, 128))
            return "";
        buf[255] = '\0';
        if (!is_valid_ascii_name(buf, 255)) return "";
        return std::string(buf);
    }
    return "";
}

// ------------------------------------------------------------------ GUObjectArray & UObject Layout
enum class UObjectArrayMode { None, Chunked, Fixed };

struct UObjectProfile {
    UObjectArrayMode mode = UObjectArrayMode::None;
    uintptr_t guobjectarray_va = 0;
    uintptr_t objects_ptr = 0;
    int32_t max_elements = 0;
    int32_t num_elements = 0;
    int32_t num_chunks = 0;
    uint32_t item_size = sizeof(uintptr_t) == 8 ? 24 : 16; // sizeof(FUObjectItem)
    uint32_t elements_per_chunk = 65536;                   // 64 * 1024

    // Calibrated UObject member offsets
    uint32_t off_internal_index = sizeof(uintptr_t) == 8 ? 0x0C : 0x08;
    uint32_t off_class_private  = sizeof(uintptr_t) == 8 ? 0x10 : 0x0C;
    uint32_t off_name_private   = sizeof(uintptr_t) == 8 ? 0x18 : 0x10;
    uint32_t off_outer_private  = sizeof(uintptr_t) == 8 ? 0x20 : 0x18;
    bool fname_has_number       = true;  // false when UE_FNAME_OUTLINE_NUMBER=1 (UE5 / UE4.26+)

    // Calibrated UStruct / Property member offsets
    bool struct_calibrated = false;
    uint32_t off_super_struct    = sizeof(uintptr_t) == 8 ? 0x40 : 0x28;
    uint32_t off_children        = sizeof(uintptr_t) == 8 ? 0x48 : 0x2C; // UField* Children
    uint32_t off_child_props     = sizeof(uintptr_t) == 8 ? 0x50 : 0x30; // FField* ChildProperties (UE 4.25+)
    uint32_t off_properties_size = sizeof(uintptr_t) == 8 ? 0x58 : 0x34;

    // UField / UProperty vs FField / FProperty
    bool use_ffield = false;
    uint32_t off_ufield_next     = sizeof(uintptr_t) == 8 ? 0x28 : 0x1C;
    uint32_t off_uprop_offset    = sizeof(uintptr_t) == 8 ? 0x4C : 0x34;

    uint32_t off_ffield_class    = sizeof(uintptr_t) == 8 ? 0x08 : 0x04;
    uint32_t off_ffield_next     = sizeof(uintptr_t) == 8 ? 0x20 : 0x10;
    uint32_t off_ffield_name     = sizeof(uintptr_t) == 8 ? 0x28 : 0x14;
    uint32_t off_fprop_offset    = sizeof(uintptr_t) == 8 ? 0x4C : 0x30;

    // Calibrated UFunction::Func (native function pointer)
    uint32_t off_ufunc_ptr       = 0;
};

static UObjectProfile g_uobj;

static uintptr_t get_uobject_by_index(int32_t index) {
    if (index < 0 || index >= g_uobj.num_elements) return 0;
    if (g_uobj.mode == UObjectArrayMode::Chunked) {
        uint32_t chunk_idx = (uint32_t)index / g_uobj.elements_per_chunk;
        uint32_t within_idx = (uint32_t)index % g_uobj.elements_per_chunk;
        if ((int32_t)chunk_idx >= g_uobj.num_chunks) return 0;
        uintptr_t chunk_base = 0;
        if (!read_val(g_uobj.objects_ptr + chunk_idx * sizeof(uintptr_t), chunk_base) || !chunk_base)
            return 0;
        uintptr_t obj = 0;
        if (!read_val(chunk_base + (size_t)within_idx * g_uobj.item_size, obj)) return 0;
        return obj;
    } else if (g_uobj.mode == UObjectArrayMode::Fixed) {
        uintptr_t obj = 0;
        if (!read_val(g_uobj.objects_ptr + (size_t)index * g_uobj.item_size, obj)) return 0;
        return obj;
    }
    return 0;
}

static bool verify_uobject_array_items(uintptr_t first_chunk, UObjectProfile &prof) {
    const uint32_t item_sizes[] = {24, 16, 32, 20};
    const uint32_t idx_offs[] = {0x0C, 0x08, 0x10};

    for (uint32_t isz : item_sizes) {
        uintptr_t obj0 = 0, obj1 = 0, obj2 = 0, obj3 = 0;
        if (!read_val(first_chunk + 0 * isz, obj0) ||
            !read_val(first_chunk + 1 * isz, obj1) ||
            !read_val(first_chunk + 2 * isz, obj2) ||
            !read_val(first_chunk + 3 * isz, obj3)) continue;

        if (!is_readable(obj0, 64) || !is_readable(obj1, 64) ||
            !is_readable(obj2, 64) || !is_readable(obj3, 64)) continue;

        // Check VFTable points into readable memory
        uintptr_t vft0 = 0, vft1 = 0;
        if (!read_val(obj0, vft0) || !read_val(obj1, vft1) ||
            !is_readable(vft0, 8) || !is_readable(vft1, 8)) continue;

        for (uint32_t ioff : idx_offs) {
            int32_t idx0 = -1, idx1 = -1, idx2 = -1, idx3 = -1;
            if (!read_val(obj0 + ioff, idx0) || !read_val(obj1 + ioff, idx1) ||
                !read_val(obj2 + ioff, idx2) || !read_val(obj3 + ioff, idx3)) continue;

            if (idx0 == 0 && idx1 == 1 && idx2 == 2 && idx3 == 3) {
                prof.item_size = isz;
                prof.off_internal_index = ioff;
                prof.off_class_private = (ioff + 4 + (sizeof(uintptr_t) - 1)) & ~(sizeof(uintptr_t) - 1);
                prof.off_name_private = prof.off_class_private + sizeof(uintptr_t);
                prof.off_outer_private = prof.off_name_private + 8;

                // Double check FName of obj0 resolves to a valid ASCII name
                uint32_t name_idx0 = 0;
                if (read_val(obj0 + prof.off_name_private, name_idx0)) {
                    std::string n0 = get_name_by_index(name_idx0);
                    if (n0.empty() && read_val(obj0 + prof.off_name_private + 8, name_idx0)) {
                        // In case FName has CasePreservingIndex or Class/Name swapped
                        std::string n_alt = get_name_by_index(name_idx0);
                        if (!n_alt.empty()) {
                            prof.off_name_private += 8;
                            prof.off_outer_private += 8;
                        }
                    }
                }

                // Check if FName has an inline Number (8 bytes) or outline number (4 bytes in UE5):
                // Core engine objects obj0..obj3 have Number == 0 when inline Number is present.
                int32_t num0 = 0, num1 = 0;
                read_val(obj0 + prof.off_name_private + 4, num0);
                read_val(obj1 + prof.off_name_private + 4, num1);
                if (num0 != 0 || num1 != 0) {
                    prof.fname_has_number = false;
                    // On 32-bit, OuterPrivate follows a 4-byte FName at +4; on 64-bit, aligned to +8
                    if (sizeof(uintptr_t) == 4) {
                        prof.off_outer_private = prof.off_name_private + 4;
                    }
                } else {
                    prof.fname_has_number = true;
                }
                return true;
            }
        }
    }
    return false;
}

// Validate a TUObjectArray / FFixedUObjectArray located at arr_va.
// mode_hint: 0 auto, 1 chunked only, 2 fixed only.
static bool probe_uobject_array_at(uintptr_t arr_va, UObjectProfile &prof, int mode_hint = 0) {
    uintptr_t objects_ptr = 0;
    if (!read_val(arr_va, objects_ptr) || !is_readable(objects_ptr, 64)) return false;

    // Chunked TUObjectArray (Objects*, PreAllocatedObjects*, MaxElements, NumElements, MaxChunks, NumChunks)
    if (mode_hint != 2) {
        int32_t max_el = 0, num_el = 0, max_ch = 0, num_ch = 0;
        if (read_val(arr_va + 2 * sizeof(uintptr_t), max_el) &&
            read_val(arr_va + 2 * sizeof(uintptr_t) + 4, num_el) &&
            read_val(arr_va + 2 * sizeof(uintptr_t) + 8, max_ch) &&
            read_val(arr_va + 2 * sizeof(uintptr_t) + 12, num_ch)) {
            if (max_el >= 10000 && max_el <= 20000000 &&
                num_el >= 100 && num_el <= max_el &&
                max_ch >= 1 && max_ch <= 1024 &&
                num_ch >= 1 && num_ch <= max_ch) {
                uintptr_t chunk0 = 0;
                if (read_val(objects_ptr, chunk0) && is_readable(chunk0, 128)) {
                    UObjectProfile p;
                    if (verify_uobject_array_items(chunk0, p)) {
                        p.mode = UObjectArrayMode::Chunked;
                        p.guobjectarray_va = arr_va;
                        p.objects_ptr = objects_ptr;
                        p.max_elements = max_el;
                        p.num_elements = num_el;
                        p.num_chunks = num_ch;
                        prof = p;
                        return true;
                    }
                }
            }
        }
    }

    // Fixed TUObjectArray (Objects*, MaxElements, NumElements)
    if (mode_hint != 1) {
        int32_t max_el = 0, num_el = 0;
        if (read_val(arr_va + sizeof(uintptr_t), max_el) &&
            read_val(arr_va + sizeof(uintptr_t) + 4, num_el)) {
            if (max_el >= 10000 && max_el <= 20000000 &&
                num_el >= 100 && num_el <= max_el) {
                UObjectProfile p;
                if (verify_uobject_array_items(objects_ptr, p)) {
                    p.mode = UObjectArrayMode::Fixed;
                    p.guobjectarray_va = arr_va;
                    p.objects_ptr = objects_ptr;
                    p.max_elements = max_el;
                    p.num_elements = num_el;
                    p.num_chunks = 1;
                    prof = p;
                    return true;
                }
            }
        }
    }
    return false;
}

static void log_guobj_profile() {
    dumper::log("[UE4] GUObjectArray VA: 0x%llx (RVA: 0x%llx), Mode: %s, NumElements: %d, Chunks: %d, ItemSize: %u",
                (unsigned long long)g_uobj.guobjectarray_va,
                (unsigned long long)(g_uobj.guobjectarray_va - g_lib_base),
                g_uobj.mode == UObjectArrayMode::Chunked ? "Chunked" : "Fixed",
                g_uobj.num_elements, g_uobj.num_chunks, g_uobj.item_size);
}

static bool detect_guobjectarray() {
    for (const auto &rw : g_ue4_rw) {
        for (uintptr_t p = rw.start; p + 32 <= rw.end; p += sizeof(uintptr_t)) {
            UObjectProfile prof;
            if (!probe_uobject_array_at(p, prof)) continue;
            g_uobj = prof;
            dumper::log("[UE4] Detected GUObjectArray: %s",
                        prof.mode == UObjectArrayMode::Chunked ? "Chunked (TUObjectArray)"
                                                               : "Fixed (FFixedUObjectArray)");
            log_guobj_profile();
            return true;
        }
    }
    return false;
}

static void apply_guobj_hints(const Config &cfg, UObjectProfile &prof) {
    if (cfg.ue4_item_size) prof.item_size = cfg.ue4_item_size;
    if (cfg.ue4_elements_per_chunk) prof.elements_per_chunk = cfg.ue4_elements_per_chunk;
    if (cfg.ue4_num_elements) {
        int32_t n = (int32_t)std::min<uint32_t>(cfg.ue4_num_elements, 20000000u);
        prof.num_elements = n;
        if (n > prof.max_elements) prof.max_elements = n;
        if (prof.mode == UObjectArrayMode::Chunked)
            prof.num_chunks = (n + (int32_t)prof.elements_per_chunk - 1) / (int32_t)prof.elements_per_chunk;
        else
            prof.num_chunks = 1;
    }
}

// Build a profile from user-supplied element count when the array header
// layout is not recognized by the probes.
static bool build_guobj_from_hints(uintptr_t arr_va, const Config &cfg, UObjectProfile &prof) {
    if (!cfg.ue4_num_elements) return false;
    uintptr_t objects_ptr = 0;
    if (!read_val(arr_va, objects_ptr) || !is_readable(objects_ptr, 64)) return false;

    UObjectProfile p;
    p.guobjectarray_va = arr_va;
    p.objects_ptr = objects_ptr;
    p.num_elements = (int32_t)std::min<uint32_t>(cfg.ue4_num_elements, 20000000u);
    p.max_elements = p.num_elements;
    p.item_size = cfg.ue4_item_size ? cfg.ue4_item_size
                                    : (sizeof(uintptr_t) == 8 ? 24u : 16u);
    p.elements_per_chunk = cfg.ue4_elements_per_chunk ? cfg.ue4_elements_per_chunk : 65536u;

    if (cfg.ue4_guobj_mode == 2) {
        p.mode = UObjectArrayMode::Fixed;
        p.num_chunks = 1;
    } else {
        uintptr_t chunk0 = 0;
        if (!read_val(objects_ptr, chunk0) || !is_readable(chunk0, 128)) return false;
        p.mode = UObjectArrayMode::Chunked;
        p.num_chunks = (p.num_elements + (int32_t)p.elements_per_chunk - 1) / (int32_t)p.elements_per_chunk;
    }
    prof = p;
    return true;
}

// Apply the GUObjectArray address from config.txt. Returns false when it does
// not validate, in which case the auto-scan fallback still runs.
static bool setup_manual_guobject(const Config &cfg) {
    static bool logged_invalid = false;
    uintptr_t va = resolve_manual_addr(cfg.ue4_guobjectarray);
    if (!va) return false;

    // FUObjectArray carries GC bookkeeping fields before ObjObjects, so the
    // array itself usually starts a few words in. Try the plausible spots.
    const uintptr_t offs[] = {0x0, 0x08, 0x10, 0x18, 0x20, 0x28};
    UObjectProfile prof;
    uintptr_t used = 0;

    auto probe_variants = [&](uintptr_t base) -> uintptr_t {
        for (uintptr_t off : offs) {
            if (probe_uobject_array_at(base + off, prof, cfg.ue4_guobj_mode)) return base + off;
        }
        return 0;
    };

    used = probe_variants(va);
    if (!used) {
        uintptr_t deref = 0;
        if (read_val(va, deref) && deref) used = probe_variants(deref);
    }
    if (!used) {
        for (uintptr_t off : offs) {
            if (!build_guobj_from_hints(va + off, cfg, prof)) continue;
            used = va + off;
            static bool logged_hint = false;
            if (!logged_hint) {
                dumper::log("[UE4] GUObjectArray header not recognized; using ue4_num_elements/ue4_item_size from config.txt");
                logged_hint = true;
            }
            break;
        }
    }
    if (!used) {
        if (!logged_invalid)
            dumper::log("[UE4] Manual GUObjectArray 0x%llx (RVA 0x%llx): no TUObjectArray layout validated "
                        "(tried array/TUObjectArray/FUObjectArray+0x0..0x28)",
                        (unsigned long long)va, (unsigned long long)(g_lib_base ? va - g_lib_base : va));
        logged_invalid = true;
        return false;
    }

    apply_guobj_hints(cfg, prof);
    g_uobj = prof;
    g_manual_guobj = true;
    dumper::log("[UE4] Using manual GUObjectArray from config.txt @ 0x%llx (RVA: 0x%llx)",
                (unsigned long long)used, (unsigned long long)(used - g_lib_base));
    log_guobj_profile();
    return true;
}

static void apply_manual_uobject_offsets(const Config &cfg) {
    int n = 0;
    if (cfg.ue4_off_internal_index) { g_uobj.off_internal_index = cfg.ue4_off_internal_index; n++; }
    if (cfg.ue4_off_class) { g_uobj.off_class_private = cfg.ue4_off_class; n++; }
    if (cfg.ue4_off_name) { g_uobj.off_name_private = cfg.ue4_off_name; n++; }
    if (cfg.ue4_off_outer) { g_uobj.off_outer_private = cfg.ue4_off_outer; n++; }
    if (cfg.ue4_off_super) { g_uobj.off_super_struct = cfg.ue4_off_super; n++; }
    if (cfg.ue4_off_children) { g_uobj.off_children = cfg.ue4_off_children; n++; }
    if (cfg.ue4_off_child_props) { g_uobj.off_child_props = cfg.ue4_off_child_props; n++; }
    if (cfg.ue4_off_properties_size) { g_uobj.off_properties_size = cfg.ue4_off_properties_size; n++; }
    if (cfg.ue4_off_prop_offset) { g_uobj.off_fprop_offset = cfg.ue4_off_prop_offset; g_uobj.off_uprop_offset = cfg.ue4_off_prop_offset; n++; }
    if (cfg.ue4_off_func) { g_uobj.off_ufunc_ptr = cfg.ue4_off_func; n++; }
    if (cfg.ue4_use_ffield >= 0) { g_uobj.use_ffield = cfg.ue4_use_ffield != 0; n++; }
    if (n) dumper::log("[UE4] Applied %d manual UObject/UStruct offset(s) from config.txt", n);
}

// ------------------------------------------------------------------ Object Helpers & Calibration
// Base name without _Number suffix (safe for comparing against "Class", "ScriptStruct", "Function", etc.)
static std::string get_object_base_name(uintptr_t obj) {
    if (!obj) return "";
    uint32_t name_idx = 0;
    if (!read_val(obj + g_uobj.off_name_private, name_idx)) return "";
    return get_name_by_index(name_idx);
}

static std::string get_object_name(uintptr_t obj) {
    if (!obj) return "";
    uint32_t name_idx = 0;
    if (!read_val(obj + g_uobj.off_name_private, name_idx)) return "";
    std::string base = get_name_by_index(name_idx);
    if (base.empty()) return "";
    if (g_uobj.fname_has_number) {
        int32_t number = 0;
        if (read_val(obj + g_uobj.off_name_private + 4, number) && number > 0 && number < 100000) {
            base += "_" + std::to_string(number - 1);
        }
    }
    return base;
}

static uintptr_t get_object_class(uintptr_t obj) {
    uintptr_t cls = 0;
    if (!obj || !read_val(obj + g_uobj.off_class_private, cls)) return 0;
    return cls;
}

static uintptr_t get_object_outer(uintptr_t obj) {
    uintptr_t outer = 0;
    if (!obj || !read_val(obj + g_uobj.off_outer_private, outer)) return 0;
    return outer;
}

static std::string get_full_name(uintptr_t obj) {
    if (!obj) return "None";
    std::string name = get_object_name(obj);
    uintptr_t outer = get_object_outer(obj);
    int depth = 0;
    while (outer && depth < 8) {
        std::string oname = get_object_name(outer);
        if (oname.empty()) break;
        name = oname + "." + name;
        outer = get_object_outer(outer);
        depth++;
    }
    return name;
}

static void calibrate_ustruct_offsets() {
    uintptr_t uclass_obj = 0, ustruct_obj = 0, ufield_obj = 0, uobject_obj = 0;
    uintptr_t vec_struct = 0, color_struct = 0;
    std::vector<uintptr_t> sample_funcs;

    int32_t limit = std::min<int32_t>(g_uobj.num_elements, 30000);
    for (int32_t i = 0; i < limit; ++i) {
        uintptr_t obj = get_uobject_by_index(i);
        if (!obj) continue;
        std::string name = get_object_base_name(obj);
        std::string cls_name = get_object_base_name(get_object_class(obj));
        if (cls_name == "Class") {
            if (name == "Object" && !uobject_obj) uobject_obj = obj;
            else if (name == "Field" && !ufield_obj) ufield_obj = obj;
            else if (name == "Struct" && !ustruct_obj) ustruct_obj = obj;
            else if (name == "Class" && !uclass_obj) uclass_obj = obj;
        } else if (cls_name == "ScriptStruct") {
            if (name == "Vector" && !vec_struct) vec_struct = obj;
            else if (name == "Color" && !color_struct) color_struct = obj;
        } else if (cls_name == "Function" && sample_funcs.size() < 8) {
            sample_funcs.push_back(obj);
        }
        if (uclass_obj && ustruct_obj && ufield_obj && uobject_obj &&
            vec_struct && color_struct && sample_funcs.size() >= 4)
            break;
    }

    // 1. Calibrate UStruct::SuperStruct using Class -> Struct -> Field -> Object chain
    if (uclass_obj && ustruct_obj && ufield_obj) {
        for (uint32_t off = g_uobj.off_outer_private + sizeof(uintptr_t); off <= 0x80; off += sizeof(uintptr_t)) {
            uintptr_t s1 = 0, s2 = 0;
            if (read_val(uclass_obj + off, s1) && s1 == ustruct_obj &&
                read_val(ustruct_obj + off, s2) && s2 == ufield_obj) {
                g_uobj.off_super_struct = off;
                g_uobj.struct_calibrated = true;
                dumper::log("[UE4] Calibrated UStruct::SuperStruct offset = 0x%x", off);
                break;
            }
        }
    }

    // 2. Calibrate UStruct::PropertiesSize using Vector (12 or 24 bytes in UE5 double-precision) and Color (4 bytes)
    if (vec_struct && color_struct) {
        for (uint32_t off = g_uobj.off_super_struct + sizeof(uintptr_t); off <= 0xB0; off += 4) {
            int32_t vsz = 0, csz = 0;
            if (read_val(vec_struct + off, vsz) && (vsz == 12 || vsz == 24) &&
                read_val(color_struct + off, csz) && csz == 4) {
                g_uobj.off_properties_size = off;
                dumper::log("[UE4] Calibrated UStruct::PropertiesSize offset = 0x%x (Vector=%d, Color=%d)",
                            off, vsz, csz);
                break;
            }
        }
    }

    // 3. Calibrate Children / ChildProperties (FField vs UField) on Vector (fields X, Y, Z)
    if (vec_struct) {
        for (uint32_t off = g_uobj.off_super_struct + sizeof(uintptr_t); off < g_uobj.off_properties_size; off += sizeof(uintptr_t)) {
            uintptr_t child = 0;
            if (!read_val(vec_struct + off, child) || !is_readable(child, 64)) continue;

            // Check if 'child' is a UObject (UField / UProperty in UE 4.11 - 4.24)
            std::string u_name = get_object_base_name(child);
            if (u_name == "X" || u_name == "Y" || u_name == "Z") {
                g_uobj.use_ffield = false;
                g_uobj.off_children = off;
                dumper::log("[UE4] Calibrated UStruct::Children (UField) offset = 0x%x", off);
                break;
            }

            // Check if 'child' is an FField (UE 4.25+ / UE5) by looking for FName "X" inside child
            for (uint32_t f_name_off = 0x10; f_name_off <= 0x38; f_name_off += 4) {
                uint32_t f_idx = 0;
                if (read_val(child + f_name_off, f_idx)) {
                    std::string fn = get_name_by_index(f_idx);
                    if (fn == "X") {
                        g_uobj.use_ffield = true;
                        g_uobj.off_child_props = off;
                        g_uobj.off_children = off - sizeof(uintptr_t);
                        g_uobj.off_ffield_name = f_name_off;
                        g_uobj.off_ffield_next = f_name_off - sizeof(uintptr_t);
                        // Find Offset_Internal in second field ("Y" -> offset 4 for float, or 8 for UE5 double)
                        uintptr_t next_field = 0;
                        if (read_val(child + g_uobj.off_ffield_next, next_field) && is_readable(next_field, 96)) {
                            for (uint32_t poff = f_name_off + 8; poff <= 0x68; poff += 4) {
                                int32_t x_off = -1, y_off = -1;
                                if (read_val(child + poff, x_off) && x_off == 0 &&
                                    read_val(next_field + poff, y_off) && (y_off == 4 || y_off == 8)) {
                                    g_uobj.off_fprop_offset = poff;
                                    break;
                                }
                            }
                        }
                        dumper::log("[UE4] Calibrated UStruct::ChildProperties (FField) offset = 0x%x, NameOff = 0x%x, PropOff = 0x%x",
                                    off, g_uobj.off_ffield_name, g_uobj.off_fprop_offset);
                        break;
                    }
                }
            }
            if (g_uobj.use_ffield) break;
        }
    }

    // 4. Calibrate UFunction::Func (native code pointer into libUE4.so / libUnreal.so)
    if (!sample_funcs.empty() && g_lib_base && g_lib_size) {
        uint32_t start_off = g_uobj.off_properties_size + 0x18;
        for (uint32_t off = start_off; off <= 0x110; off += sizeof(uintptr_t)) {
            int hits = 0;
            for (uintptr_t fn_obj : sample_funcs) {
                uintptr_t code_ptr = 0;
                if (read_val(fn_obj + off, code_ptr) && is_ue4_code_ptr(code_ptr)) {
                    hits++;
                }
            }
            if (hits >= (int)std::min<size_t>(2, sample_funcs.size())) {
                g_uobj.off_ufunc_ptr = off;
                dumper::log("[UE4] Calibrated UFunction::Func native pointer offset = 0x%x", off);
                break;
            }
        }
    }
}

// ------------------------------------------------------------------ Dump Routines
static void dump_offsets_info(const std::string &out_dir) {
    FILE *f = fopen((out_dir + "/Offsets_Info.txt").c_str(), "w");
    if (!f) return;

    fprintf(f, "========================================================\n");
    fprintf(f, "  Universal Unreal Engine 4 / 5 Dump Info\n");
    fprintf(f, "========================================================\n");
    fprintf(f, "Engine Lib Base : 0x%llx\n", (unsigned long long)g_lib_base);
    fprintf(f, "Engine Lib Size : 0x%zx\n\n", g_lib_size);

    fprintf(f, "[GNames]\n");
    fprintf(f, "  Source       : %s\n", g_manual_gnames ? "config.txt (manual)" : "auto-detected");
    fprintf(f, "  Structure    : %s\n",
            g_names.mode == GNamesMode::FNamePool ? "FNamePool (UE 4.23+ / UE5)" :
            g_names.mode == GNamesMode::TNameEntryArray ? "TNameEntryArray (UE 4.11-4.22)" : "Unknown");
    fprintf(f, "  GNames VA    : 0x%llx\n", (unsigned long long)g_names.gnames_va);
    fprintf(f, "  GNames RVA   : 0x%llx\n", (unsigned long long)(g_names.gnames_va - g_lib_base));
    fprintf(f, "  Blocks VA    : 0x%llx (RVA: 0x%llx)\n",
            (unsigned long long)g_names.blocks_array_va,
            (unsigned long long)(g_names.blocks_array_va - g_lib_base));
    fprintf(f, "  NumBlocks    : %u\n\n", g_names.num_blocks);

    fprintf(f, "[GUObjectArray]\n");
    fprintf(f, "  Source       : %s\n", g_manual_guobj ? "config.txt (manual)" : "auto-detected");
    fprintf(f, "  Structure    : %s\n",
            g_uobj.mode == UObjectArrayMode::Chunked ? "Chunked (FChunkedFixedUObjectArray)" :
            g_uobj.mode == UObjectArrayMode::Fixed ? "Fixed (FFixedUObjectArray)" : "Unknown");
    fprintf(f, "  GUObject VA  : 0x%llx\n", (unsigned long long)g_uobj.guobjectarray_va);
    fprintf(f, "  GUObject RVA : 0x%llx\n", (unsigned long long)(g_uobj.guobjectarray_va - g_lib_base));
    fprintf(f, "  NumElements  : %d / %d\n", g_uobj.num_elements, g_uobj.max_elements);
    fprintf(f, "  FUObjectItem : 0x%x bytes\n\n", g_uobj.item_size);

    fprintf(f, "[Calibrated UObject & UStruct Offsets]\n");
    fprintf(f, "  UObject::InternalIndex    : 0x%x\n", g_uobj.off_internal_index);
    fprintf(f, "  UObject::ClassPrivate     : 0x%x\n", g_uobj.off_class_private);
    fprintf(f, "  UObject::NamePrivate      : 0x%x (%s)\n", g_uobj.off_name_private,
            g_uobj.fname_has_number ? "8-byte FName" : "4-byte FName / Outline Number");
    fprintf(f, "  UObject::OuterPrivate     : 0x%x\n", g_uobj.off_outer_private);
    fprintf(f, "  UStruct::SuperStruct      : 0x%x\n", g_uobj.off_super_struct);
    fprintf(f, "  UStruct::Children         : 0x%x\n", g_uobj.off_children);
    fprintf(f, "  UStruct::PropertiesSize   : 0x%x\n", g_uobj.off_properties_size);
    if (g_uobj.use_ffield) {
        fprintf(f, "  UStruct::ChildProperties  : 0x%x (FField mode)\n", g_uobj.off_child_props);
        fprintf(f, "  FField::Next              : 0x%x\n", g_uobj.off_ffield_next);
        fprintf(f, "  FField::Name              : 0x%x\n", g_uobj.off_ffield_name);
        fprintf(f, "  FProperty::Offset         : 0x%x\n", g_uobj.off_fprop_offset);
    }
    if (g_uobj.off_ufunc_ptr) {
        fprintf(f, "  UFunction::Func           : 0x%x\n", g_uobj.off_ufunc_ptr);
    }
    fclose(f);
}

static size_t dump_names(const std::string &out_dir) {
    FILE *f = fopen((out_dir + "/NamesDump.txt").c_str(), "w");
    if (!f) return 0;
    size_t count = 0;

    if (g_names.mode == GNamesMode::FNamePool) {
        for (uint32_t b = 0; b < g_names.num_blocks; ++b) {
            dumper::set_status_detail("UE4: names block %u/%u (%zu names)", b + 1, g_names.num_blocks, count);
            uintptr_t block_ptr = 0;
            if (!read_val(g_names.blocks_array_va + b * sizeof(uintptr_t), block_ptr) || !block_ptr)
                continue;
            uint32_t off_idx = 0;
            while (off_idx < 65536) {
                uint32_t index = (b << 16) | off_idx;
                uintptr_t entry_va = block_ptr + (size_t)off_idx * g_names.pool_stride;
                if (b == 0 && off_idx == 0) entry_va += g_names.header_offset;
                uint16_t header = 0;
                if (!read_val(entry_va, header)) break;
                uint32_t len = (header >> g_names.len_shift) & 0x3FF;
                if (len == 0 || len > 256) break;
                bool is_wide = (header & 1) != 0;
                if (!is_wide) {
                    std::string s = get_name_by_index(index);
                    if (!s.empty()) {
                        fprintf(f, "[%06u] %s\n", index, s.c_str());
                        count++;
                    }
                }
                size_t total_bytes = g_names.string_offset + (is_wide ? len * 2 : len);
                size_t aligned_bytes = (total_bytes + g_names.pool_stride - 1) & ~((size_t)g_names.pool_stride - 1);
                uint32_t step = (uint32_t)(aligned_bytes / g_names.pool_stride);
                if (step == 0) step = 1;
                off_idx += step;
            }
        }
    } else if (g_names.mode == GNamesMode::TNameEntryArray) {
        uint32_t max_idx = g_names.num_blocks * g_names.chunk_elements;
        for (uint32_t i = 0; i < max_idx; ++i) {
            if ((i & 0xFFFF) == 0)
                dumper::set_status_detail("UE4: names %u/%u (%zu found)", i, max_idx, count);
            std::string s = get_name_by_index(i);
            if (!s.empty()) {
                fprintf(f, "[%06u] %s\n", i, s.c_str());
                count++;
            }
        }
    }

    fclose(f);
    return count;
}

static size_t dump_objects_and_sdk(const Config &cfg, const std::string &out_dir,
                                   size_t &out_classes, size_t &out_funcs) {
    FILE *fobj = fopen((out_dir + "/ObjectsDump.txt").c_str(), "w");
    FILE *fsdk = cfg.dump_cs ? fopen((out_dir + "/SDK_Dump.hpp").c_str(), "w") : nullptr;
    FILE *ffn  = fopen((out_dir + "/FunctionsDump.txt").c_str(), "w");
    FILE *fjs  = cfg.dump_script_json ? fopen((out_dir + "/script.json").c_str(), "w") : nullptr;
    bool first_js = true;
    if (fjs) fprintf(fjs, "{\"ScriptMethod\":[\n");

    size_t obj_count = 0;
    out_classes = 0;
    out_funcs = 0;

    if (fsdk) {
        fprintf(fsdk, "// Auto-Generated Unreal Engine (UE4/UE5) SDK Dump\n");
        fprintf(fsdk, "// Engine lib base: 0x%llx\n\n", (unsigned long long)g_lib_base);
    }

    int32_t total = std::min<int32_t>(g_uobj.num_elements, 2000000);
    for (int32_t i = 0; i < total; ++i) {
        if ((i % 50000) == 0) {
            dumper::set_status_detail("UE4: objects %d/%d (%zu dumped, %zu classes, %zu funcs)",
                                      i, total, obj_count, out_classes, out_funcs);
            if (i) dumper::log("[UE4] objects progress: %d/%d (%zu dumped)", i, total, obj_count);
        }
        uintptr_t obj = get_uobject_by_index(i);
        if (!obj || !is_readable(obj, 64)) continue;

        uintptr_t cls = get_object_class(obj);
        std::string cls_base = get_object_base_name(cls);
        std::string cls_name = get_object_name(cls);
        std::string full_name = get_full_name(obj);
        if (cls_name.empty() && full_name.empty()) continue;

        if (fobj) {
            fprintf(fobj, "[%06d] %-24s %-60s 0x%llx\n",
                    i, cls_name.c_str(), full_name.c_str(), (unsigned long long)obj);
        }
        obj_count++;

        // Record standalone UFunction RVA into FunctionsDump.txt and script.json
        if (cls_base == "Function") {
            uintptr_t fn_va = 0;
            if (g_uobj.off_ufunc_ptr) read_val(obj + g_uobj.off_ufunc_ptr, fn_va);
#if defined(__aarch64__)
            fn_va &= 0x00FFFFFFFFFFFFFFULL;
#endif
            bool valid_code = is_ue4_code_ptr(fn_va);
            uintptr_t rva = valid_code ? (fn_va - g_lib_base) : 0;
            if (ffn) {
                fprintf(ffn, "[%06d] RVA: 0x%08llx  VA: 0x%llx  %s\n",
                        i, (unsigned long long)rva, (unsigned long long)fn_va, full_name.c_str());
            }
            if (fjs && valid_code) {
                fprintf(fjs, "%s{\"Address\":%llu,\"Name\":\"%s\",\"Signature\":\"void %s()\"}",
                        first_js ? "" : ",\n",
                        (unsigned long long)rva,
                        json_escape(full_name).c_str(),
                        json_escape(full_name).c_str());
                first_js = false;
            }
            out_funcs++;
            continue;
        }

        // If this object is a Class or ScriptStruct, emit SDK layout
        if (fsdk && (cls_base == "Class" || cls_base == "ScriptStruct")) {
            std::string sname = get_object_name(obj);
            uintptr_t super = 0;
            read_val(obj + g_uobj.off_super_struct, super);
            std::string super_name = get_object_name(super);
            int32_t prop_sz = 0;
            read_val(obj + g_uobj.off_properties_size, prop_sz);

            fprintf(fsdk, "// %s %s (Size: 0x%x)\n", cls_base.c_str(), full_name.c_str(), prop_sz);
            if (!super_name.empty())
                fprintf(fsdk, "struct %s : public %s {\n", sname.c_str(), super_name.c_str());
            else
                fprintf(fsdk, "struct %s {\n", sname.c_str());

            if (g_uobj.use_ffield) {
                uintptr_t field = 0;
                read_val(obj + g_uobj.off_child_props, field);
                int safety = 0;
                while (field && is_readable(field, 80) && safety++ < 512) {
                    uint32_t fname_idx = 0;
                    read_val(field + g_uobj.off_ffield_name, fname_idx);
                    std::string fname = get_name_by_index(fname_idx);
                    int32_t foff = 0;
                    read_val(field + g_uobj.off_fprop_offset, foff);

                    std::string ftype = "Property";
                    uintptr_t fcls = 0;
                    if (read_val(field + g_uobj.off_ffield_class, fcls) && is_readable(fcls, 16)) {
                        uint32_t fc_idx = 0;
                        if (read_val(fcls, fc_idx)) {
                            std::string fcn = get_name_by_index(fc_idx);
                            if (!fcn.empty()) ftype = fcn;
                        }
                    }
                    if (!fname.empty()) {
                        fprintf(fsdk, "\t%-24s %-32s; // Offset: 0x%x\n",
                                ftype.c_str(), fname.c_str(), foff);
                    }
                    uintptr_t next = 0;
                    read_val(field + g_uobj.off_ffield_next, next);
                    field = next;
                }

                // Also walk UField* Children for UFunctions on this class
                uintptr_t uchild = 0;
                read_val(obj + g_uobj.off_children, uchild);
                int fn_safety = 0;
                bool any_fn = false;
                while (uchild && is_readable(uchild, 64) && fn_safety++ < 512) {
                    std::string ctype = get_object_base_name(get_object_class(uchild));
                    std::string cname = get_object_name(uchild);
                    if (ctype == "Function" && !cname.empty()) {
                        if (!any_fn) { fprintf(fsdk, "\n\t// Functions\n"); any_fn = true; }
                        uintptr_t fn_va = 0;
                        if (g_uobj.off_ufunc_ptr) read_val(uchild + g_uobj.off_ufunc_ptr, fn_va);
#if defined(__aarch64__)
                        fn_va &= 0x00FFFFFFFFFFFFFFULL;
#endif
                        uintptr_t rva = is_ue4_code_ptr(fn_va) ? (fn_va - g_lib_base) : 0;
                        fprintf(fsdk, "\tvoid %s(); // RVA: 0x%llx VA: 0x%llx\n",
                                cname.c_str(), (unsigned long long)rva, (unsigned long long)fn_va);
                    }
                    uintptr_t next = 0;
                    read_val(uchild + g_uobj.off_ufield_next, next);
                    uchild = next;
                }
            } else {
                uintptr_t field = 0;
                read_val(obj + g_uobj.off_children, field);
                int safety = 0;
                while (field && is_readable(field, 80) && safety++ < 512) {
                    std::string fname = get_object_name(field);
                    std::string ftype = get_object_base_name(get_object_class(field));
                    int32_t foff = 0;
                    read_val(field + g_uobj.off_uprop_offset, foff);
                    if (!fname.empty()) {
                        if (ftype == "Function") {
                            uintptr_t fn_va = 0;
                            if (g_uobj.off_ufunc_ptr) read_val(field + g_uobj.off_ufunc_ptr, fn_va);
#if defined(__aarch64__)
                            fn_va &= 0x00FFFFFFFFFFFFFFULL;
#endif
                            uintptr_t rva = is_ue4_code_ptr(fn_va) ? (fn_va - g_lib_base) : 0;
                            fprintf(fsdk, "\tvoid %s(); // Function RVA: 0x%llx VA: 0x%llx\n",
                                    fname.c_str(), (unsigned long long)rva, (unsigned long long)fn_va);
                        } else {
                            fprintf(fsdk, "\t%-24s %-32s; // Offset: 0x%x\n",
                                    ftype.c_str(), fname.c_str(), foff);
                        }
                    }
                    uintptr_t next = 0;
                    read_val(field + g_uobj.off_ufield_next, next);
                    field = next;
                }
            }
            fprintf(fsdk, "};\n\n");
            out_classes++;
        }
    }

    if (fobj) fclose(fobj);
    if (fsdk) fclose(fsdk);
    if (ffn) fclose(ffn);
    if (fjs) {
        fprintf(fjs, "\n]}\n");
        fclose(fjs);
    }
    return obj_count;
}

static void dump_ue4_so_memory(const std::string &out_dir) {
    std::string path = out_dir + "/libUE4_mem.so";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    dumper::log("[UE4] Dumping engine .so memory (%zu MB)...", g_lib_size / (1024 * 1024));
    dumper::set_status_detail("UE4: dumping engine memory (0/%zu MB)", g_lib_size / (1024 * 1024));
    std::vector<char> page(4096);
    size_t zero_pages = 0;
    for (size_t off = 0; off < g_lib_size; off += 4096) {
        if ((off & 0x1FFFFFF) == 0)  // every 32 MB
            dumper::set_status_detail("UE4: dumping engine memory (%zu/%zu MB)", off / (1024 * 1024),
                                      g_lib_size / (1024 * 1024));
        size_t n = std::min<size_t>(4096, g_lib_size - off);
        bool ok = dumper::safe_read(g_lib_base + off, page.data(), n);
        if (!ok) {
            memset(page.data(), 0, n);
            zero_pages++;
        }
        fwrite(page.data(), 1, n, f);
    }
    fclose(f);
    dumper::log("[UE4] libUE4_mem.so dumped (%zu bytes, %zu unreadable pages)", g_lib_size, zero_pages);
}

static size_t rw_bytes() {
    size_t total = 0;
    for (const auto &rw : g_ue4_rw) total += rw.end - rw.start;
    return total;
}

static bool wait_for_gnames(int passes) {
    using namespace std::chrono_literals;
    for (int pass = 0; pass < passes; ++pass) {
        refresh_maps();
        size_t mb = rw_bytes() / (1024 * 1024);
        dumper::set_status_detail("UE4: auto-searching GNames (pass %d/%d, %zu MB)...", pass + 1, passes, mb);
        dumper::log("[UE4] GNames search pass %d/%d over %zu MB of writable memory...", pass + 1, passes, mb);
        auto t0 = std::chrono::steady_clock::now();
        if (detect_gnames()) {
            long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            dumper::log("[UE4] GNames found on pass %d (%ld ms)", pass + 1, ms);
            return true;
        }
        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        dumper::log("[UE4] GNames not found on pass %d (%ld ms)", pass + 1, ms);
        std::this_thread::sleep_for(1s);
    }
    return false;
}

static bool wait_for_guobject(const Config &cfg, int passes) {
    using namespace std::chrono_literals;
    for (int pass = 0; pass < passes; ++pass) {
        refresh_maps();
        size_t mb = rw_bytes() / (1024 * 1024);
        dumper::set_status_detail("UE4: auto-searching GUObjectArray (pass %d/%d, %zu MB)...", pass + 1, passes, mb);
        dumper::log("[UE4] GUObjectArray search pass %d/%d over %zu MB of writable memory...", pass + 1, passes, mb);
        auto t0 = std::chrono::steady_clock::now();
        bool ok = setup_manual_guobject(cfg) || detect_guobjectarray();
        if (ok) {
            long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            dumper::log("[UE4] GUObjectArray found on pass %d (%ld ms)", pass + 1, ms);
            return true;
        }
        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        dumper::log("[UE4] GUObjectArray not found on pass %d (%ld ms)", pass + 1, ms);
        std::this_thread::sleep_for(1s);
    }
    return false;
}

bool dump_ue4(const Config &cfg, const std::string &package, uintptr_t lib_base, size_t lib_size) {
    g_lib_base = lib_base;
    g_lib_size = lib_size;
    (void)package;

    std::string out_dir = dumper::output_dir();
    refresh_maps();
    dumper::log("[UE4] Engine lib base=0x%llx size=0x%zx, writable memory=%zu MB",
                (unsigned long long)g_lib_base, g_lib_size, rw_bytes() / (1024 * 1024));
    if (!g_lib_base)
        dumper::log("[UE4] WARNING: Engine lib base is 0 - RVAs from config.txt cannot be resolved");

    // Manual GNames from config.txt: dump names right away instead of waiting.
    dumper::set_status_detail("UE4: applying GNames offset...");
    bool gnames_ok = setup_manual_gnames(cfg);
    if (!gnames_ok) gnames_ok = wait_for_gnames(60);
    if (!gnames_ok) {
        dumper::log("[UE4] GNames not found. Set ue4_gnames=<RVA or VA> in config.txt");
        dumper::set_status_detail("UE4 failed: GNames not found (see dump.log)");
        if (cfg.dump_so && g_lib_base && g_lib_size) dump_ue4_so_memory(out_dir);
        return false;
    }

    dumper::set_status_detail("UE4: dumping GNames table...");
    if (cfg.dump_metadata || cfg.dump_cs) {
        dumper::log("[UE4] Dumping GNames table...");
        size_t n_names = dump_names(out_dir);
        dumper::log("[UE4] Dumped %zu FNames -> NamesDump.txt", n_names);
    }

    // GUObjectArray is only needed for the object/SDK dump - try it after the
    // names are already on disk.
    dumper::set_status_detail("UE4: applying GUObjectArray offset...");
    bool guobj_ok = setup_manual_guobject(cfg);
    if (!guobj_ok) {
        guobj_ok = detect_guobjectarray();
        if (!guobj_ok) guobj_ok = wait_for_guobject(cfg, 60);
    }

    if (guobj_ok) {
        dumper::set_status_detail("UE4: calibrating UStruct & UFunction offsets...");
        calibrate_ustruct_offsets();
        apply_manual_uobject_offsets(cfg);
        if (g_names.mode == GNamesMode::TNameEntryArray) {
            dumper::set_engine_version("UE 4.11-4.22 (TNameEntryArray)");
        } else if (g_uobj.use_ffield) {
            dumper::set_engine_version(g_uobj.fname_has_number ? "UE 4.25+ / UE5 (FField)"
                                                               : "UE5 (Outline FName + FField)");
        } else {
            dumper::set_engine_version("UE 4.23-4.24 (FNamePool + UField)");
        }
    } else {
        dumper::log("[UE4] GUObjectArray not found - ObjectsDump/SDK skipped. Set ue4_guobjectarray=<RVA or VA> in config.txt");
    }

    dumper::log("[UE4] Offsets source: GNames=%s, GUObjectArray=%s",
                g_manual_gnames ? "config.txt" : "auto",
                g_manual_guobj ? "config.txt" : "auto");

    dump_offsets_info(out_dir);

    if (guobj_ok && (cfg.dump_cs || cfg.dump_script_json)) {
        dumper::set_status_detail("UE4: dumping GUObjectArray, Functions & SDK...");
        dumper::log("[UE4] Dumping GUObjectArray, Functions & SDK (%d objects)...", g_uobj.num_elements);
        size_t n_classes = 0, n_funcs = 0;
        size_t n_objs = dump_objects_and_sdk(cfg, out_dir, n_classes, n_funcs);
        dumper::log("[UE4] Dumped %zu UObjects -> ObjectsDump.txt", n_objs);
        dumper::log("[UE4] Dumped %zu UFunctions -> FunctionsDump.txt / script.json", n_funcs);
        dumper::log("[UE4] Dumped %zu SDK Classes/Structs -> SDK_Dump.hpp", n_classes);
    }

    if (cfg.dump_so && g_lib_base && g_lib_size) {
        dump_ue4_so_memory(out_dir);
    }

    dumper::set_status_detail("UE4: done -> %s", out_dir.c_str());
    return true;
}

}  // namespace ue4
