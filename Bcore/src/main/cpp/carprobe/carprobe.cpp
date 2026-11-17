// carprobe.cpp  v5 = STAGE 2: SOLID PREDICTION LINE (simulation + JNI for the Kotlin overlay) on top of the v4.2 probe.
//   NEW in v5: table/root transform read from the node parent chain at runtime, shot simulation (striker path, first coin hit,
//   coin path with wall bounces, short striker deflection), JNI object top.niunaijun.blackboxa.menu.NativeCarrom
//   (setEnabled / setOptions / lines / statusText). Still READ-ONLY: no hooks, no code patches, no game-memory writes
//   (the old kTestMaxGuides test write stays disabled). The probe no longer stops after 30 min (the overlay needs it).
//   Coin-coin collisions and pockets are NOT simulated (not measured yet). Physics numbers below are MEASURED from build 19.6.1
//   (Freestyle Offline only) and UNCALIBRATED. NOT built / NOT tested when written.
// (older header) v4.2 = STAGE 1 RECON (burst lines now list only objects that MOVED) for the solid-line feature: parent chains, PocketNode positions, shot recording at 16 ms.
//   (v3 base: adds ONE test write: mMaxGuides of the LOCAL player's guide node; v2.1 + garbage filter for freed objects, _controlsLogic link, shows every valid object)  -  READ-ONLY probe for Carrom Pool (com.miniclip.carrom).
// NO hooks and NO code patching. It READS memory (through a pipe, so a bad address gives an error code
// instead of a crash) and prints values to logcat, tag CARPROBE1.
// v3 TEST ONLY: it also writes ONE int (mMaxGuides = kTestMaxGuides) into the guide node that belongs to the
// local player (its _controlsLogic points to our ControlsLogicLocal object). Set kTestMaxGuides = 0 to disable.
//
// How it works (everything by NAME, no hardcoded offsets or addresses):
//  1. finds the game library in memory by the name prefix "libgame-CARROM" (the "-Module-1492" part is ignored);
//  2. finds each class by its name string -> pointer to that string -> class struct
//     (layout confirmed offline from game v19.6.1 / versionCode 1492: class+8 super, +16 name, +32 info,
//      +40 instance_size, +48 ivar list; ivar list = [u32 count][name,type,offset]*24 bytes from list+8);
//  3. gets every ivar offset by NAME from that class (and its super classes);
//  4. scans the process heap for objects whose first word (isa) equals the class address;
//  5. prints angle / power / position values of those objects while you play.
// Build-dependent values: NONE hardcoded. Only the names below (class + ivar names, from v19.6.1).
// NOT built / NOT tested when written.
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <jni.h>

#define TAG "CARPROBE1"
#define LOG(...) do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); usleep(1200); } while (0)

// ------------------------------------------------------------------ config
static const char* kLibPrefix   = "libgame-CARROM";   // name prefix of the game's own library
static const long  kStartDelayMs = 20000;              // wait after the library appears
static const long  kLifeMs       = 0;                  // v5: 0 = the worker never stops (the overlay needs it); old value was 30 min
static const long  kTickMs       = 500;                // value read interval
static const long  kScanWaitMs   = 6000;               // heap rescan interval while objects are missing
static const long  kScanSlowMs   = 25000;              // heap rescan interval when everything is found
static const int   kMaxPerClass  = 200;                // max heap hits kept per class
static const size_t CH = 16384;                        // read chunk
static const int   kTestMaxGuides = 0;                 // TEST: value written to mMaxGuides of the LOCAL guide (0 = no write at all)

typedef uint64_t u64;
typedef uint32_t u32;

static long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

// one static scratch buffer for all scans (excluded from the heap-scan hits, so we never count our own copies)
static uint8_t g_scanbuf[16384 + 256];

// ------------------------------------------------------------------ safe read (never crashes)
static int g_pipe[2] = {-1, -1};

static bool rd(u64 a, void* out, size_t n) {
    uint8_t* o = (uint8_t*)out;
    while (n) {
        size_t c = n > CH ? CH : n;
        ssize_t w = write(g_pipe[1], (const void*)(uintptr_t)a, c);
        if (w <= 0) return false;
        size_t got = 0;
        while (got < (size_t)w) {
            ssize_t r = read(g_pipe[0], o + got, (size_t)w - got);
            if (r <= 0) return false;
            got += (size_t)r;
        }
        if ((size_t)w != c) return false;
        a += c; o += c; n -= c;
    }
    return true;
}

static bool rd_str(u64 a, char* out, size_t max) {
    size_t i = 0;
    while (i + 1 < max) {
        size_t pg = 4096 - (size_t)((a + i) & 4095);
        size_t n = std::min(pg, max - 1 - i);
        if (!rd(a + i, out + i, n)) {
            if (i == 0) return false;
            break;
        }
        void* z = memchr(out + i, 0, n);
        if (z) { out[(char*)z - out] = 0; return true; }
        i += n;
    }
    out[i] = 0;
    return i > 0;
}

// safe write into our own process: data goes through the pipe, read() copies it to the target address.
// A bad / read-only address makes read() return an error code, it does not crash.
static bool wr(u64 a, const void* data, size_t n) {
    if (n == 0 || n > 4096) return false;
    if (write(g_pipe[1], data, n) != (ssize_t)n) return false;
    ssize_t r = read(g_pipe[0], (void*)(uintptr_t)a, n);
    if (r != (ssize_t)n) {
        char tmp[256];
        while (read(g_pipe[0], tmp, sizeof(tmp)) > 0) {}      // drain whatever is left in the pipe
        return false;
    }
    return true;
}

static bool is_ident(const char* s) {
    size_t n = strlen(s);
    if (n < 3 || n > 80) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// ------------------------------------------------------------------ game library segments
struct Seg { u64 start; size_t size; int flags; };
static std::vector<Seg> g_segs;
static u64 g_bias = 0;
static char g_libname[256] = "";

static int phdr_cb(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name && strstr(info->dlpi_name, kLibPrefix)) {
        g_bias = (u64)info->dlpi_addr;
        snprintf(g_libname, sizeof(g_libname), "%s", info->dlpi_name);
        g_segs.clear();
        for (int i = 0; i < info->dlpi_phnum; i++) {
            const ElfW(Phdr)& p = info->dlpi_phdr[i];
            if (p.p_type == PT_LOAD) {
                Seg s; s.start = (u64)info->dlpi_addr + p.p_vaddr; s.size = p.p_memsz; s.flags = (int)p.p_flags;
                g_segs.push_back(s);
            }
        }
        return 1;
    }
    return 0;
}

static bool find_game_lib() {
    g_segs.clear();
    dl_iterate_phdr(phdr_cb, nullptr);
    return !g_segs.empty();
}

// ------------------------------------------------------------------ class lookup by name
static std::vector<u64> find_cstr(const char* name) {
    std::string pat; pat.push_back('\0'); pat += name; pat.push_back('\0');
    std::vector<u64> out;
    if (pat.size() > 200) return out;
    uint8_t* bufp = g_scanbuf;
    for (const Seg& s : g_segs) {
        if (!(s.flags & PF_R) || (s.flags & PF_W)) continue;     // strings live in read-only data
        for (size_t off = 0; off < s.size; off += CH) {
            size_t n = std::min(CH + pat.size(), s.size - off);
            if (n < pat.size() || !rd(s.start + off, bufp, n)) continue;
            size_t pos = 0;
            while (pos + pat.size() <= n) {
                void* p = memmem(bufp + pos, n - pos, pat.data(), pat.size());
                if (!p) break;
                size_t idx = (uint8_t*)p - bufp;
                if (idx < CH) out.push_back(s.start + off + idx + 1);
                pos = idx + 1;
            }
        }
    }
    return out;
}

static std::vector<u64> find_ptrs_to(const std::vector<u64>& targets) {
    std::vector<u64> out;
    uint8_t* bufp = g_scanbuf;
    for (const Seg& s : g_segs) {
        if (!(s.flags & PF_R) || (s.flags & PF_X)) continue;       // class data is not in code
        u64 a0 = (s.start + 7) & ~(u64)7;
        u64 end = s.start + s.size;
        for (u64 a = a0; a + 8 <= end; a += CH) {
            size_t n = (size_t)std::min<u64>(CH, end - a);
            n &= ~(size_t)7;
            if (!n || !rd(a, bufp, n)) continue;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                u64 w; memcpy(&w, bufp + i, 8);
                for (u64 t : targets) if (w == t) { out.push_back(a + i); break; }
            }
        }
    }
    return out;
}

static std::map<std::string, u64> g_classCache;

static u64 find_class(const std::string& name) {
    auto it = g_classCache.find(name);
    if (it != g_classCache.end()) return it->second;
    u64 result = 0;
    std::vector<u64> strs = find_cstr(name.c_str());
    if (!strs.empty()) {
        std::vector<u64> locs = find_ptrs_to(strs);
        for (u64 loc : locs) {
            u64 c = loc - 16;
            u64 w[8];
            if (!rd(c, w, sizeof(w))) continue;
            u64 isa = w[0], info = w[4], isz = w[5];
            if (isa == 0 || info == 0 || (info & 2) || !(info & 1) || info > 65535) continue;
            if (isz < 8 || isz > 100000) continue;
            result = c;
            break;
        }
    }
    g_classCache[name] = result;
    return result;
}

static bool class_name_at(u64 c, char* out, size_t max) {
    u64 np;
    if (!rd(c + 16, &np, 8)) return false;
    return rd_str(np, out, max) && is_ident(out);
}

// super class: after the runtime resolves classes it is a Class pointer, before it is a name string
static u64 super_of(u64 c) {
    u64 s;
    if (!rd(c + 8, &s, 8) || !s) return 0;
    char nm[96];
    u64 info;
    if (class_name_at(s, nm, sizeof(nm)) && rd(s + 32, &info, 8) && info != 0 && info < 65536) return s;
    if (rd_str(s, nm, sizeof(nm)) && is_ident(nm)) return find_class(nm);
    return 0;
}

static int ivar_off(u64 c, const char* iv, int depth = 0) {
    if (!c || depth > 8) return -1;
    u64 list;
    if (rd(c + 48, &list, 8) && list) {
        u32 cnt;
        if (rd(list, &cnt, 4) && cnt < 1000) {
            for (u32 i = 0; i < cnt; i++) {
                u64 e = list + 8 + 24ull * i;
                u64 np; u32 off; char nm[96];
                if (!rd(e, &np, 8) || !rd(e + 16, &off, 4)) continue;
                if (rd_str(np, nm, sizeof(nm)) && strcmp(nm, iv) == 0) return (int)off;
            }
        }
    }
    return ivar_off(super_of(c), iv, depth + 1);
}

// ------------------------------------------------------------------ classes + ivars we care about
enum { C_LOCAL, C_REMOTE, C_FG, C_STRIKER, C_PUCK, C_POCKET, C_COUNT };
static const char* kClassNames[C_COUNT] = {
    "ControlsLogicLocal", "ControlsLogicRemote", "ControlsForegroundNode", "StrikerNode", "PuckNode", "PocketNode"
};
static u64 g_cls[C_COUNT];

struct Iv { const char* name; int off; };
// local aim
static Iv L_angle = {"_angle", -1}, L_dist = {"_distanceToCenter", -1}, L_active = {"_active", -1},
          L_enabled = {"_enabled", -1}, L_vrot = {"_visualRotation", -1}, L_center = {"_center", -1},
          L_enough = {"_hasHadEnoughPower", -1}, L_sid = {"_strikerId", -1};
// remote aim
static Iv R_angle = {"_angle", -1}, R_power = {"_power", -1}, R_spos = {"_strikerPosition", -1}, R_sid = {"_strikerId", -1};
// aim guide node
static Iv F_ctl = {"_controlsLogic", -1};
static Iv F_maxg = {"mMaxGuides", -1}, F_scale = {"_tableScale", -1}, F_vrot = {"_visualRotation", -1}, F_safe = {"_safeAreaOffset", -1};
// CCNode parts (found through StrikerNode / PuckNode / ControlsForegroundNode super chain)
static Iv N_pos = {"_position", -1}, N_vis = {"_visible", -1}, N_run = {"_isRunning", -1}, N_size = {"_contentSize", -1};
static Iv P_radius = {"_radius", -1};
static Iv N_ignore = {"_ignoreAnchorPointForPosition", -1};   // optional: if the game has no ivar of this name it is treated as false
static Iv N_parent = {"_parent", -1}, N_anchor = {"_anchorPoint", -1}, N_scx = {"_scaleX", -1}, N_scy = {"_scaleY", -1}, N_rot = {"_rotationX", -1};

static void resolve_offsets() {
    for (int i = 0; i < C_COUNT; i++) {
        g_cls[i] = find_class(kClassNames[i]);
        LOG("class %-24s -> %s%lx", kClassNames[i], g_cls[i] ? "0x" : "NOT FOUND ", (unsigned long)g_cls[i]);
    }
    struct { Iv* iv; int cls; } t[] = {
        {&L_angle, C_LOCAL}, {&L_dist, C_LOCAL}, {&L_active, C_LOCAL}, {&L_enabled, C_LOCAL}, {&L_vrot, C_LOCAL},
        {&L_center, C_LOCAL}, {&L_enough, C_LOCAL}, {&L_sid, C_LOCAL},
        {&R_angle, C_REMOTE}, {&R_power, C_REMOTE}, {&R_spos, C_REMOTE}, {&R_sid, C_REMOTE},
        {&F_ctl, C_FG}, {&F_maxg, C_FG}, {&F_scale, C_FG}, {&F_vrot, C_FG}, {&F_safe, C_FG},
        {&N_pos, C_STRIKER}, {&N_vis, C_STRIKER}, {&N_run, C_STRIKER}, {&N_size, C_STRIKER},
        {&P_radius, C_PUCK},
        {&N_ignore, C_STRIKER}, {&N_parent, C_STRIKER}, {&N_anchor, C_STRIKER}, {&N_scx, C_STRIKER}, {&N_scy, C_STRIKER}, {&N_rot, C_STRIKER},
    };
    for (auto& e : t) {
        e.iv->off = g_cls[e.cls] ? ivar_off(g_cls[e.cls], e.iv->name) : -1;
        LOG("ivar  %-24s %-20s offset %d", kClassNames[e.cls], e.iv->name, e.iv->off);
    }
    // sanity: dump raw class words when something is missing (helps the next fix round)
    for (int i = 0; i < C_COUNT; i++) {
        if (!g_cls[i]) continue;
        u64 w[8];
        if (rd(g_cls[i], w, sizeof(w)))
            LOG("raw %-24s %lx %lx %lx %lx %lx %lx %lx %lx", kClassNames[i],
                (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
                (unsigned long)w[4], (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7]);
    }
}

// ------------------------------------------------------------------ heap scan for instances
static const u64 kObf = 0x5a5a1234a5a54321ULL;      // class addresses are stored XOR-ed so our own memory does not look like an object

struct Reg { u64 s, e; };

static std::vector<Reg> heap_regions() {
    std::vector<Reg> v;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return v;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long s, e; char perms[8]; int n = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &s, &e, perms, &n) < 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p') continue;
        const char* path = line + n;
        while (*path == ' ') path++;
        bool ok = (*path == '\n' || *path == 0) || strstr(path, "[heap]") || strstr(path, "scudo") ||
                  strstr(path, "libc_malloc") || strstr(path, "jemalloc");
        if (!ok) continue;
        if (e - s > (1ull << 30)) continue;
        Reg r; r.s = s; r.e = e;
        v.push_back(r);
    }
    fclose(f);
    return v;
}

static std::vector<u64> g_objs[C_COUNT];

static void scan_heap() {
    u64 want[C_COUNT]; int nw = 0; int idx[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) if (g_cls[i]) { want[nw] = g_cls[i] ^ kObf; idx[nw] = i; nw++; }
    for (int i = 0; i < C_COUNT; i++) g_objs[i].clear();
    if (!nw) return;
    std::vector<Reg> regs = heap_regions();
    u64 bufLo = (u64)(uintptr_t)g_scanbuf, bufHi = bufLo + sizeof(g_scanbuf);
    long chunks = 0;
    for (const Reg& r : regs) {
        for (u64 a = r.s; a < r.e; a += CH) {
            size_t n = (size_t)std::min<u64>(CH, r.e - a);
            if (!rd(a, g_scanbuf, n)) continue;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                u64 w; memcpy(&w, g_scanbuf + i, 8);
                u64 x = w ^ kObf;
                for (int k = 0; k < nw; k++) {
                    if (x == want[k]) {
                        u64 addr = a + i;
                        if (addr >= bufLo && addr < bufHi) continue;
                        if ((int)g_objs[idx[k]].size() < kMaxPerClass) g_objs[idx[k]].push_back(addr);
                    }
                }
            }
            if ((++chunks & 511) == 0) usleep(1500);        // be gentle with the game thread
        }
    }
}

// ------------------------------------------------------------------ value helpers
static bool getd(u64 o, int off, double& v) { return off >= 0 && rd(o + off, &v, 8) && isfinite(v); }
static bool getf(u64 o, int off, float& v) { return off >= 0 && rd(o + off, &v, 4) && isfinite(v); }
static bool geti(u64 o, int off, int& v) { return off >= 0 && rd(o + off, &v, 4); }
static bool getb(u64 o, int off, int& v) { uint8_t b; if (off < 0 || !rd(o + off, &b, 1)) return false; v = b; return true; }
static bool getpt(u64 o, int off, double& x, double& y) { return getd(o, off, x) && getd(o, off + 8, y); }

static bool still_valid(int cls, u64 o) {
    u64 isa;
    if (!rd(o, &isa, 8) || isa != g_cls[cls]) return false;
    return true;
}

// ------------------------------------------------------------------ TEST write (only the local player's guide)
static int g_patchLogs = 0;

static void apply_guide_test() {
    if (kTestMaxGuides <= 0 || F_maxg.off < 0 || F_ctl.off < 0 || N_run.off < 0) return;
    for (u64 g : g_objs[C_FG]) {
        int run, mg; u64 ctl;
        if (!getb(g, N_run.off, run) || run != 1) continue;
        if (!geti(g, F_maxg.off, mg) || mg < 1 || mg > 64) continue;
        if (!rd(g + F_ctl.off, &ctl, 8) || !ctl) continue;
        bool mine = false;
        for (u64 l : g_objs[C_LOCAL]) if (l == ctl && still_valid(C_LOCAL, l)) { mine = true; break; }
        if (!mine || mg == kTestMaxGuides) continue;
        int v = kTestMaxGuides;
        bool ok = wr(g + F_maxg.off, &v, 4);
        int back = -1; geti(g, F_maxg.off, back);
        if (g_patchLogs < 40) {
            LOG("PATCH guide@%lx maxGuides %d -> %d  write=%s  readback=%d", (unsigned long)g, mg, v, ok ? "ok" : "FAILED", back);
            g_patchLogs++;
        }
    }
}

// ------------------------------------------------------------------ snapshot
// Freed objects keep their old isa word, so the heap scan also finds dead memory. Every object
// below is therefore checked for plausible values (flags are 0/1, numbers in a sane range).
static bool sane(double v, double lim) { return isfinite(v) && fabs(v) < lim; }

static std::string snapshot() {
    char b[320];
    std::string s;
    int shown;

    shown = 0;
    for (u64 o : g_objs[C_LOCAL]) {
        if (shown >= 6) break;
        double ang, dist, cx, cy; float vr; int act, en, enough, sid;
        if (!getd(o, L_angle.off, ang) || !getd(o, L_dist.off, dist)) continue;
        if (!getb(o, L_active.off, act) || !getb(o, L_enabled.off, en) || !getb(o, L_enough.off, enough)) continue;
        if (!geti(o, L_sid.off, sid)) continue;
        if (!sane(ang, 7.0) || !sane(dist, 5000.0) || act > 1 || en > 1 || enough > 1 || sid < 0 || sid > 16) continue;
        getf(o, L_vrot.off, vr) || (vr = NAN);
        if (!getpt(o, L_center.off, cx, cy) || !sane(cx, 5000.0) || !sane(cy, 5000.0)) { cx = cy = NAN; }
        snprintf(b, sizeof(b), "LOCAL@%lx ang=%.4f(%.1fdeg) dist=%.2f act=%d en=%d power_ok=%d sid=%d vrot=%.3f ctr=%.1f,%.1f\n",
                 (unsigned long)o, ang, ang * 180.0 / M_PI, dist, act, en, enough, sid, vr, cx, cy);
        s += b; shown++;
    }
    shown = 0;
    for (u64 o : g_objs[C_REMOTE]) {
        if (shown >= 6) break;
        double ang, pw, sx, sy; int sid;
        if (!getd(o, R_angle.off, ang) || !getd(o, R_power.off, pw) || !geti(o, R_sid.off, sid)) continue;
        if (!sane(ang, 7.0) || !sane(pw, 10000.0) || sid < 0 || sid > 16) continue;
        if (!getpt(o, R_spos.off, sx, sy) || !sane(sx, 5000.0) || !sane(sy, 5000.0)) continue;
        snprintf(b, sizeof(b), "REMOTE@%lx ang=%.4f(%.1fdeg) power=%.2f sid=%d spos=%.1f,%.1f\n",
                 (unsigned long)o, ang, ang * 180.0 / M_PI, pw, sid, sx, sy);
        s += b; shown++;
    }
    shown = 0;
    for (u64 o : g_objs[C_FG]) {
        if (shown >= 6) break;
        int mg; float sc, vr; double px, py; int run; u64 ctl = 0;
        if (!geti(o, F_maxg.off, mg) || mg < 0 || mg > 64) continue;
        if (!getb(o, N_run.off, run) || run > 1) continue;
        getf(o, F_scale.off, sc) || (sc = NAN);
        getf(o, F_vrot.off, vr) || (vr = NAN);
        getpt(o, N_pos.off, px, py) || (px = py = NAN);
        if (F_ctl.off >= 0) rd(o + F_ctl.off, &ctl, 8);
        snprintf(b, sizeof(b), "GUIDE@%lx maxGuides=%d tableScale=%.3f vrot=%.3f pos=%.1f,%.1f run=%d logic=%lx\n",
                 (unsigned long)o, mg, sc, vr, px, py, run, (unsigned long)ctl);
        s += b; shown++;
    }
    shown = 0;
    for (u64 o : g_objs[C_STRIKER]) {
        if (shown >= 6) break;
        double px, py; int vis, run;
        if (!getpt(o, N_pos.off, px, py) || !sane(px, 5000.0) || !sane(py, 5000.0)) continue;
        if (!getb(o, N_vis.off, vis) || !getb(o, N_run.off, run) || vis > 1 || run != 1) continue;
        snprintf(b, sizeof(b), "STRIKER@%lx pos=%.1f,%.1f vis=%d\n", (unsigned long)o, px, py, vis);
        s += b; shown++;
    }
    std::string pk;
    int pn = 0;
    for (u64 o : g_objs[C_PUCK]) {
        if (pn >= 24) break;
        double px, py; float r; int vis, run;
        if (!getpt(o, N_pos.off, px, py) || !sane(px, 5000.0) || !sane(py, 5000.0)) continue;
        if (!getb(o, N_run.off, run) || !getb(o, N_vis.off, vis) || run != 1 || vis > 1) continue;
        if (!getf(o, P_radius.off, r) || r < 1.0f || r > 200.0f) continue;
        snprintf(b, sizeof(b), " (%.0f,%.0f r%.1f v%d)", px, py, r, vis);
        pk += b; pn++;
    }
    snprintf(b, sizeof(b), "PUCKS n=%d:", pn);
    s += b; s += pk; s += "\n";
    std::string pc; int pcn = 0;
    for (u64 o : g_objs[C_POCKET]) {
        if (pcn >= 12) break;
        double px, py; int vis, run;
        if (!getpt(o, N_pos.off, px, py) || !sane(px, 5000.0) || !sane(py, 5000.0)) continue;
        if (!getb(o, N_vis.off, vis) || !getb(o, N_run.off, run) || vis > 1 || run > 1) continue;
        snprintf(b, sizeof(b), " @%lx(%.1f,%.1f v%d r%d)", (unsigned long)o, px, py, vis, run);
        pc += b; pcn++;
    }
    snprintf(b, sizeof(b), "POCKETS n=%d:", pcn);
    s += b; s += pc; s += "\n";
    return s;
}

// ------------------------------------------------------------------ main thread
static void log_lines(const std::string& s) {
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        LOG("%s", s.substr(p, e - p).c_str());
        p = e + 1;
    }
}

static int count_valid(int cls) { return (int)g_objs[cls].size(); }

// ------------------------------------------------------------------ stage 1 helpers
static bool obj_class_name(u64 o, char* out, size_t max) {
    u64 isa;
    if (!rd(o, &isa, 8) || !isa) return false;
    return class_name_at(isa, out, max);
}

// walks from a node up through its parents and prints position / size / scale of every level
static void dump_chain(const char* tag, u64 o) {
    for (int d = 0; d < 14 && o; d++) {
        char nm[96] = "?";
        obj_class_name(o, nm, sizeof(nm));
        double px = NAN, py = NAN, cw = NAN, ch = NAN, ax = NAN, ay = NAN;
        float sx = NAN, sy = NAN, rot = NAN;
        getpt(o, N_pos.off, px, py);
        getpt(o, N_size.off, cw, ch);
        getpt(o, N_anchor.off, ax, ay);
        getf(o, N_scx.off, sx); getf(o, N_scy.off, sy); getf(o, N_rot.off, rot);
        LOG("CHAIN %s d=%d %s@%lx pos=%.2f,%.2f size=%.2f,%.2f anchor=%.2f,%.2f scale=%.3f,%.3f rot=%.2f",
            tag, d, nm, (unsigned long)o, px, py, cw, ch, ax, ay, sx, sy, rot);
        u64 par = 0;
        if (N_parent.off < 0 || !rd(o + N_parent.off, &par, 8) || par == o) break;
        o = par;
    }
}

struct Aim { bool ok; double ang, dist; int act, en, enough; bool hasCenter; double cx, cy; u64 obj; };

// picks the LOCAL aim object: finger down + enabled beats the rest; an object that a running guide node points to gets a bonus (ghost filter)
static Aim read_aim() {
    Aim a; a.ok = false; a.ang = a.dist = 0; a.act = a.en = a.enough = 0; a.hasCenter = false; a.cx = a.cy = 0; a.obj = 0;
    int bestScore = -1;
    for (u64 o : g_objs[C_LOCAL]) {
        double ang, dist; int act, en, enough, sid;
        if (!getd(o, L_angle.off, ang) || !getd(o, L_dist.off, dist)) continue;
        if (!getb(o, L_active.off, act) || !getb(o, L_enabled.off, en) || !getb(o, L_enough.off, enough)) continue;
        if (!geti(o, L_sid.off, sid)) continue;
        if (!sane(ang, 7.0) || !sane(dist, 5000.0) || act > 1 || en > 1 || enough > 1 || sid < 0 || sid > 16) continue;
        int score = (act == 1 ? 2 : 0) + (en == 1 ? 1 : 0);
        for (u64 g : g_objs[C_FG]) {
            u64 ctl; int run;
            if (F_ctl.off >= 0 && N_run.off >= 0 && rd(g + F_ctl.off, &ctl, 8) && ctl == o && getb(g, N_run.off, run) && run == 1) { score += 4; break; }
        }
        if (score > bestScore) {
            bestScore = score;
            a.ok = true; a.ang = ang; a.dist = dist; a.act = act; a.en = en; a.enough = enough; a.obj = o;
            double cx, cy;
            a.hasCenter = getpt(o, L_center.off, cx, cy) && sane(cx, 5000.0) && sane(cy, 5000.0);
            if (a.hasCenter) { a.cx = cx; a.cy = cy; }
        }
    }
    return a;
}

struct Item { char kind; u64 o; };

static bool item_valid(char kind, u64 o) {
    double x, y; int vis, run;
    if (!getpt(o, N_pos.off, x, y) || !sane(x, 5000.0) || !sane(y, 5000.0)) return false;
    if (!getb(o, N_run.off, run) || !getb(o, N_vis.off, vis) || run != 1 || vis > 1) return false;
    if (kind == 'P') { float r; if (!getf(o, P_radius.off, r) || r < 1.0f || r > 200.0f) return false; }
    return true;
}

static std::vector<Item> build_items() {
    std::vector<Item> v;
    for (u64 o : g_objs[C_STRIKER]) if (item_valid('S', o)) { Item it; it.kind = 'S'; it.o = o; v.push_back(it); }
    for (u64 o : g_objs[C_PUCK])    if (item_valid('P', o)) { Item it; it.kind = 'P'; it.o = o; v.push_back(it); }
    return v;
}

static void read_xy(const std::vector<Item>& it, std::vector<double>& xy) {
    xy.assign(it.size() * 2, NAN);
    for (size_t i = 0; i < it.size(); i++) {
        double x, y;
        if (getpt(it[i].o, N_pos.off, x, y)) { xy[2 * i] = x; xy[2 * i + 1] = y; }
    }
}

static bool pucks_moved(const std::vector<Item>& it, const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < it.size(); i++) {
        if (it[i].kind != 'P') continue;
        double dx = a[2 * i] - b[2 * i], dy = a[2 * i + 1] - b[2 * i + 1];
        if (isfinite(dx) && isfinite(dy) && (fabs(dx) > 0.05 || fabs(dy) > 0.05)) return true;
    }
    return false;
}

static bool any_moved(const std::vector<Item>& it, const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < it.size(); i++) {
        double dx = a[2 * i] - b[2 * i], dy = a[2 * i + 1] - b[2 * i + 1];
        if (isfinite(dx) && isfinite(dy) && (fabs(dx) > 0.05 || fabs(dy) > 0.05)) return true;
    }
    return false;
}

static bool same_items(const std::vector<Item>& a, const std::vector<Item>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (a[i].o != b[i].o) return false;
    return true;
}

// ================================================================== STAGE 2: PREDICTION (simulation, no game writes)
// ---- settings (set from Kotlin through JNI)
static std::atomic<int> g_enabled{0};       // prediction on/off (the Kotlin button)
static std::atomic<int> g_fricPct{100};     // friction multiplier in percent (tuning slider)
static std::atomic<int> g_maxBounces{3};    // wall bounces drawn per path
static std::atomic<int> g_deflect{1};       // draw the short striker remainder after the hit
static std::atomic<int> g_recon{1};         // 1 = keep the old shot-recording logs (BURST lines) for tuning

// ---- PHYSICS NUMBERS. Source: game 19.6.1 (versionCode 1492), [MEASURED] from the user's CARPROBE1 logs, Freestyle Offline only.
// All UNCALIBRATED. Units = the game's table points, time = 1 frame (60 Hz).
static const double kMinDrag       = 30.3;     // no shot at or below this drag length            (report 6.3)
static const double kV0Slope       = 0.198;    // launch speed per frame = slope * dist + offset   (report 6.3)
static const double kV0Offset      = -5.99;
static const double kV0Cap         = 17.4;     // launch speed cap per frame                       (report 6.3)
static const double kFricHigh      = 0.17;     // speed lost per frame when speed > kFricSwitch    (report 6.4)
static const double kFricSwitch    = 4.75;     // regime switch ("~4.5 .. ~5")                     (report 6.4)
static const double kFricLowBase   = 0.064;    // low-speed loss = base + slope * speed            (report 6.4)
static const double kFricLowSlope  = 0.0065;
static const double kStrikerRadius = 17.4;     // +-1                                              (report 6.2)
static const double kBoardHalf     = 254.1;    // centre limit = kBoardHalf - radius: striker 236.7, coin 241.0 (report 6.2)
static const double kStrikerWallE  = 0.98;     // speed kept after a wall bounce (striker)         (report 6.5)
static const double kCoinWallE     = 0.73;     // normal speed kept after a wall bounce (coin)     (report 6.5; tangential part kept = ASSUMPTION)
static const double kRestitution   = 1.0;      // striker - coin                                   (report 6.6)
static const double kMassRatio     = 1.7;      // striker mass / coin mass                         (report 6.6)
static const double kAimTol        = 3.0;      // LOCAL._center must be this close to a table striker
static const int    kMaxFrames     = 700;
static const double kStopSpeed     = 0.05;
static const double kDeflectMaxLen = 120.0;    // the short striker remainder is cut at this length
static const double kBoardBound    = 260.0;    // coins outside +-this are ignored (garbage / flying in)

struct V2 { double x, y; };
struct Xf { double a, b, c, d, tx, ty; };                 // x' = a x + c y + tx ; y' = b x + d y + ty   (Cocos: y points up)
static Xf xf_id() { Xf m; m.a = 1; m.b = 0; m.c = 0; m.d = 1; m.tx = 0; m.ty = 0; return m; }
static V2 xf_apply(const Xf& m, double x, double y) { V2 r; r.x = m.a * x + m.c * y + m.tx; r.y = m.b * x + m.d * y + m.ty; return r; }
static Xf xf_compose(const Xf& o, const Xf& i) {            // o after i
    Xf r;
    r.a = o.a * i.a + o.c * i.b;   r.b = o.b * i.a + o.d * i.b;
    r.c = o.a * i.c + o.c * i.d;   r.d = o.b * i.c + o.d * i.d;
    r.tx = o.a * i.tx + o.c * i.ty + o.tx;
    r.ty = o.b * i.tx + o.d * i.ty + o.ty;
    return r;
}
static bool xf_close(const Xf& p, const Xf& q) {
    return fabs(p.a - q.a) < 1e-4 && fabs(p.b - q.b) < 1e-4 && fabs(p.c - q.c) < 1e-4 && fabs(p.d - q.d) < 1e-4 &&
           fabs(p.tx - q.tx) < 0.02 && fabs(p.ty - q.ty) < 0.02;
}

// ---- class name of an object, cached by class pointer
static std::map<u64, std::string> g_nameCache;
static std::string obj_cname(u64 o) {
    u64 isa;
    if (!rd(o, &isa, 8) || !isa) return std::string();
    auto it = g_nameCache.find(isa);
    if (it != g_nameCache.end()) return it->second;
    char nm[96];
    std::string r;
    if (class_name_at(isa, nm, sizeof(nm))) r = nm;
    g_nameCache[isa] = r;
    return r;
}

// Cocos nodeToParent transform of one node (position, anchor, content size, scale, rotation; skew ignored).
static bool node_to_parent(u64 n, Xf& out) {
    double px, py, ax, ay, w, h; float sx, sy, rot; int ig = 0;
    if (!getpt(n, N_pos.off, px, py) || !getpt(n, N_anchor.off, ax, ay) || !getpt(n, N_size.off, w, h)) return false;
    if (!getf(n, N_scx.off, sx) || !getf(n, N_scy.off, sy) || !getf(n, N_rot.off, rot)) return false;
    if (!sane(px, 1e5) || !sane(py, 1e5) || !sane(ax, 100.0) || !sane(ay, 100.0) || !sane(w, 1e5) || !sane(h, 1e5)) return false;
    if (!(fabs(sx) < 1000.0f) || !(fabs(sy) < 1000.0f) || !(fabs(rot) < 100000.0f)) return false;
    if (N_ignore.off >= 0) { if (!getb(n, N_ignore.off, ig) || ig > 1) ig = 0; }
    double apx = ax * w, apy = ay * h;                          // anchorPointInPoints
    double r = -(double)rot * M_PI / 180.0, c = cos(r), s = sin(r);
    out.a = c * sx;  out.b = s * sx;  out.c = -s * sy;  out.d = c * sy;
    double x0 = px + (ig ? apx : 0.0), y0 = py + (ig ? apy : 0.0);
    out.tx = x0 - (out.a * apx + out.c * apy);
    out.ty = y0 - (out.b * apx + out.d * apy);
    return true;
}

struct Chain { bool ok; u64 table; Xf toRoot; double rootW, rootH; };

// Walks from obj's parent up to the node named "RootNode" (its own transform is NOT applied: its local space = design space).
// toRoot maps a point given in obj's _position space (= the space of obj's parent) into design points (y up).
static Chain walk_chain(u64 obj) {
    Chain c; c.ok = false; c.table = 0; c.toRoot = xf_id(); c.rootW = c.rootH = 0;
    u64 n;
    if (N_parent.off < 0 || !rd(obj + N_parent.off, &n, 8) || !n) return c;
    Xf m = xf_id();
    for (int d = 0; d < 24 && n; d++) {
        std::string nm = obj_cname(n);
        if (nm.empty()) return c;
        if (nm == "RootNode") {
            double w, h;
            if (!getpt(n, N_size.off, w, h) || !(w > 1.0) || !(h > 1.0) || w > 1e5 || h > 1e5) return c;
            c.rootW = w; c.rootH = h; c.toRoot = m; c.ok = (c.table != 0);
            return c;
        }
        if (nm == "TableNode" && !c.table) c.table = n;
        Xf t;
        if (!node_to_parent(n, t)) return c;
        m = xf_compose(t, m);
        u64 par;
        if (!rd(n + N_parent.off, &par, 8) || par == n) return c;
        n = par;
    }
    return c;
}

// ---- the world the prediction works on (cached, refreshed every ~400 ms while aiming)
struct Coin { u64 o; double x, y, r; };
struct World {
    bool ok; u64 localObj; u64 striker; u64 table; Xf toRoot; double rootW, rootH; std::vector<Coin> coins; long at; std::string why;
};
static World g_world;

static bool read_pos(u64 o, double& x, double& y) { return getpt(o, N_pos.off, x, y) && sane(x, 5000.0) && sane(y, 5000.0); }

static void refresh_world(u64 localObj, double cx, double cy, long t) {
    World w; w.ok = false; w.localObj = localObj; w.striker = 0; w.table = 0; w.toRoot = xf_id(); w.rootW = w.rootH = 0; w.at = t;
    if (N_pos.off < 0 || N_parent.off < 0 || N_anchor.off < 0 || N_size.off < 0 || N_scx.off < 0 || N_scy.off < 0 || N_rot.off < 0) {
        w.why = "CCNode ivar missing (pos/parent/anchor/size/scale/rot)"; g_world = w; return;
    }
    // 1. the striker that belongs to the aim: a StrikerNode under a TableNode whose position equals LOCAL._center
    double best = 1e18; Chain bc; bc.ok = false; bc.table = 0;
    for (u64 o : g_objs[C_STRIKER]) {
        if (!item_valid('S', o)) continue;
        double x, y;
        if (!read_pos(o, x, y)) continue;
        Chain ch = walk_chain(o);
        if (!ch.ok) continue;
        double d = hypot(x - cx, y - cy);
        if (d < best) { best = d; w.striker = o; bc = ch; }
    }
    if (!w.striker) { w.why = "no StrikerNode under a TableNode"; g_world = w; return; }
    if (best > kAimTol) { char b[96]; snprintf(b, sizeof(b), "aim centre is %.1f away from the nearest table striker", best); w.why = b; g_world = w; return; }
    w.table = bc.table; w.toRoot = bc.toRoot; w.rootW = bc.rootW; w.rootH = bc.rootH;
    // 2. coins: live PuckNodes in the same table and the same coordinate space as the striker
    int rejChain = 0;
    for (u64 o : g_objs[C_PUCK]) {
        if (!item_valid('P', o)) continue;
        int vis; if (!getb(o, N_vis.off, vis) || vis != 1) continue;
        double x, y; float r;
        if (!read_pos(o, x, y) || fabs(x) > kBoardBound || fabs(y) > kBoardBound) continue;
        if (!getf(o, P_radius.off, r) || r < 1.0f || r > 60.0f) continue;
        Chain ch = walk_chain(o);
        if (!ch.ok || ch.table != w.table || !xf_close(ch.toRoot, w.toRoot)) { rejChain++; continue; }
        Coin c; c.o = o; c.x = x; c.y = y; c.r = r;
        w.coins.push_back(c);
    }
    w.ok = true;
    char b[64]; snprintf(b, sizeof(b), "ok (rejected pucks: %d)", rejChain); w.why = b;
    g_world = w;
}

// ---- simulation
struct Body { double x, y, vx, vy, r; bool striker; };

static double friction_loss(double v, double scale) {
    return ((v > kFricSwitch) ? kFricHigh : (kFricLowBase + kFricLowSlope * v)) * scale;
}

// Moves one body frame by frame. Stops at: speed 0, wall bounce number maxBounces+1, length limit, or (if targets) the first contact with a coin.
// Returns the index of the coin hit or -1. pts = polyline (start, every wall contact, end). b ends at the stop / contact position.
static int sim_path(Body& b, const std::vector<Coin>* targets, int maxBounces, double fricScale, double maxLen,
                    std::vector<V2>& pts, int& bounces) {
    pts.clear(); pts.push_back(V2{b.x, b.y});
    bounces = 0;
    const double lim = kBoardHalf - b.r;
    const double e = b.striker ? kStrikerWallE : kCoinWallE;
    double len = 0;
    for (int f = 0; f < kMaxFrames; f++) {
        if (hypot(b.vx, b.vy) < kStopSpeed) break;
        double rem = 1.0;
        for (int ev = 0; ev < 8 && rem > 1e-9; ev++) {
            double dx = b.vx * rem, dy = b.vy * rem;
            double tBest = 1.0; int kind = 0, ci = -1;
            if (dx > 1e-12 && b.x + dx > lim)        { double t = (lim - b.x) / dx;  if (t < tBest) { tBest = t; kind = 1; } }
            else if (dx < -1e-12 && b.x + dx < -lim) { double t = (-lim - b.x) / dx; if (t < tBest) { tBest = t; kind = 1; } }
            if (dy > 1e-12 && b.y + dy > lim)        { double t = (lim - b.y) / dy;  if (t < tBest) { tBest = t; kind = 2; } }
            else if (dy < -1e-12 && b.y + dy < -lim) { double t = (-lim - b.y) / dy; if (t < tBest) { tBest = t; kind = 2; } }
            if (targets) {
                double aa = dx * dx + dy * dy;
                for (size_t i = 0; i < targets->size() && aa > 1e-18; i++) {
                    const Coin& c = (*targets)[i];
                    double px = b.x - c.x, py = b.y - c.y, R = b.r + c.r;
                    double bb = px * dx + py * dy, cc = px * px + py * py - R * R;
                    double t;
                    if (cc <= 0) { if (bb >= 0) continue; t = 0; }               // already touching: counts only if moving closer
                    else {
                        double disc = bb * bb - aa * cc;
                        if (disc < 0) continue;
                        t = (-bb - sqrt(disc)) / aa;
                        if (t < 0) continue;
                    }
                    if (t <= tBest) { tBest = t; kind = 3; ci = (int)i; }
                }
            }
            b.x += dx * tBest; b.y += dy * tBest; len += hypot(dx, dy) * tBest;
            rem *= (1.0 - tBest);
            if (kind == 3) { pts.push_back(V2{b.x, b.y}); return ci; }
            if (kind == 1 || kind == 2) {
                if (kind == 1) { b.x = (b.vx > 0) ? lim : -lim; } else { b.y = (b.vy > 0) ? lim : -lim; }
                pts.push_back(V2{b.x, b.y});
                if (kind == 1) b.vx = -b.vx; else b.vy = -b.vy;
                if (b.striker) { b.vx *= e; b.vy *= e; }
                else if (kind == 1) b.vx *= e; else b.vy *= e;
                bounces++;
                if (bounces > maxBounces) return -1;
            }
            if (kind == 0) break;
        }
        if (len > maxLen) break;
        double sp = hypot(b.vx, b.vy);
        double ns = sp - friction_loss(sp, fricScale);
        if (ns <= 0) break;
        b.vx *= ns / sp; b.vy *= ns / sp;
    }
    pts.push_back(V2{b.x, b.y});
    return -1;
}

// ---- prediction output (read by JNI): [designW, designH, nPaths, {type, nPoints, x0,y0,x1,y1...}...]  design points, y up
// type 0 = striker path (white), 1 = hit coin path (red), 2 = striker remainder after the hit (thin white)
static std::mutex g_predMx;
static std::vector<float> g_predData;
static char g_predStatus[400] = "Prediction: waiting for the game";
static char g_predLastLog[300] = "";

static void pred_set(const std::vector<float>& d, const char* st) {
    std::lock_guard<std::mutex> lk(g_predMx);
    g_predData = d;
    snprintf(g_predStatus, sizeof(g_predStatus), "%s", st);
}

static void put_path(std::vector<float>& out, int type, const std::vector<V2>& pts, const Xf& m) {
    out.push_back((float)type); out.push_back((float)pts.size());
    for (const V2& p : pts) { V2 q = xf_apply(m, p.x, p.y); out.push_back((float)q.x); out.push_back((float)q.y); }
}

static double launch_speed(double dist) {
    double v = kV0Slope * dist + kV0Offset;
    if (v > kV0Cap) v = kV0Cap;
    return v;
}

static long g_lastPredAt = 0;
static bool g_wasAiming = false;

static void pred_tick(const Aim& a, long t) {
    if (!g_enabled.load()) { if (g_wasAiming) { std::vector<float> e; pred_set(e, "Prediction: OFF"); g_wasAiming = false; } return; }
    if (!a.ok || a.act != 1) {
        if (g_wasAiming) { std::vector<float> e; pred_set(e, a.ok ? "Prediction: waiting for aim" : "Prediction: aim objects not found yet"); g_wasAiming = false; }
        else if (!a.ok) { std::vector<float> e; pred_set(e, "Prediction: aim objects not found yet (heap scan every few seconds)"); }
        return;
    }
    if (t - g_lastPredAt < 25) return;
    g_lastPredAt = t;
    char st[400];
    bool newAim = !g_wasAiming;
    g_wasAiming = true;
    if (!a.hasCenter) { std::vector<float> e; pred_set(e, "Prediction: LOCAL._center unreadable"); return; }
    if (newAim || !g_world.ok || g_world.localObj != a.obj || t - g_world.at > 400) refresh_world(a.obj, a.cx, a.cy, t);
    const World& w = g_world;
    if (!w.ok) { std::vector<float> e; snprintf(st, sizeof(st), "Prediction: %s", w.why.c_str()); pred_set(e, st); return; }
    double sx, sy;
    if (!read_pos(w.striker, sx, sy)) { std::vector<float> e; pred_set(e, "Prediction: striker position unreadable"); return; }
    if (a.dist <= kMinDrag) {
        std::vector<float> e; snprintf(st, sizeof(st), "Prediction: drag %.1f too short (needs > %.1f)", a.dist, kMinDrag); pred_set(e, st); return;
    }
    double v0 = launch_speed(a.dist);
    double fs = g_fricPct.load() / 100.0;
    int mb = g_maxBounces.load();
    Body s; s.x = sx; s.y = sy; s.vx = cos(a.ang) * v0; s.vy = sin(a.ang) * v0; s.r = kStrikerRadius; s.striker = true;
    std::vector<V2> sp, cp, dp; int sb = 0, cb = 0, db = 0;
    int hit = sim_path(s, &w.coins, mb, fs, 1e9, sp, sb);
    std::vector<float> out;
    out.push_back((float)w.rootW); out.push_back((float)w.rootH);
    int nPaths = 1;
    size_t idxN = out.size(); out.push_back(0.f);
    put_path(out, 0, sp, w.toRoot);
    char hitTxt[120] = "no coin hit";
    if (hit >= 0) {
        const Coin& c = w.coins[hit];
        double nx = c.x - s.x, ny = c.y - s.y, nl = hypot(nx, ny);
        if (nl > 1e-9) {
            nx /= nl; ny /= nl;
            double vn = s.vx * nx + s.vy * ny;
            double tx = s.vx - vn * nx, ty = s.vy - vn * ny;
            double ms = kMassRatio, mc = 1.0, e = kRestitution;
            double svn = (ms - e * mc) / (ms + mc) * vn;
            double cvn = (1.0 + e) * ms / (ms + mc) * vn;
            if (vn > 0) {
                Body cb2; cb2.x = c.x; cb2.y = c.y; cb2.vx = cvn * nx; cb2.vy = cvn * ny; cb2.r = c.r; cb2.striker = false;
                sim_path(cb2, nullptr, mb, fs, 1e9, cp, cb);
                put_path(out, 1, cp, w.toRoot); nPaths++;
                if (g_deflect.load()) {
                    Body s2; s2.x = s.x; s2.y = s.y; s2.vx = tx + svn * nx; s2.vy = ty + svn * ny; s2.r = kStrikerRadius; s2.striker = true;
                    sim_path(s2, nullptr, mb, fs, kDeflectMaxLen, dp, db);
                    if (dp.size() >= 2) { put_path(out, 2, dp, w.toRoot); nPaths++; }
                }
                snprintf(hitTxt, sizeof(hitTxt), "hit coin %d at (%.1f,%.1f) coin v0 %.2f stops (%.1f,%.1f)", hit, c.x, c.y, cvn, cb2.x, cb2.y);
            } else snprintf(hitTxt, sizeof(hitTxt), "grazing contact, no push");
        }
    }
    out[idxN] = (float)nPaths;
    V2 origin = xf_apply(w.toRoot, 0, 0);
    snprintf(st, sizeof(st), "Prediction ON | ang %.1f deg  drag %.1f  v0 %.2f/frame | coins %d | striker bounces %d | %s | table origin in design (%.1f,%.1f) root %.0fx%.0f",
             a.ang * 180.0 / M_PI, a.dist, v0, (int)w.coins.size(), sb, hitTxt, origin.x, origin.y, w.rootW, w.rootH);
    pred_set(out, st);
    {
        std::lock_guard<std::mutex> lk(g_predMx);
        snprintf(g_predLastLog, sizeof(g_predLastLog), "ang=%.4f dist=%.2f v0=%.3f start=%.2f,%.2f strikerEnd=%.1f,%.1f %s", a.ang, a.dist, v0, sx, sy,
                 sp.empty() ? 0.0 : sp.back().x, sp.empty() ? 0.0 : sp.back().y, hitTxt);
    }
}

static void* worker(void*) {
    if (pipe2(g_pipe, O_NONBLOCK | O_CLOEXEC) != 0) { LOG("pipe2 failed"); return nullptr; }
    long t0 = now_ms();
    while (!find_game_lib()) {
        if (now_ms() - t0 > 10 * 60 * 1000L) { LOG("game library (%s*) not found, giving up", kLibPrefix); return nullptr; }
        sleep(1);
    }
    LOG("game lib: %s  bias=0x%lx  segments=%d", g_libname, (unsigned long)g_bias, (int)g_segs.size());
    usleep(kStartDelayMs * 1000);
    find_game_lib();
    resolve_offsets();

    long start = now_ms(), lastScan = 0, lastBeat = 0, lastSnap = 0, lastChain = 0;
    int chainDumps = 0, bursts = 0;
    std::string last;
    bool aimActive = false; Aim lastAim; lastAim.ok = false; lastAim.ang = lastAim.dist = 0; lastAim.en = lastAim.enough = 0;
    std::vector<Item> prevItems, bItems; std::vector<double> prevXY, xy, bPrev, bLogged;
    bool burst = false; long burstStart = 0, lastMove = 0; int burstLines = 0;

    while (kLifeMs == 0 || now_ms() - start < kLifeMs) {
        long t = now_ms();
        if (!burst) {
            bool haveCore = !g_objs[C_LOCAL].empty() && !g_objs[C_FG].empty();
            long wait = haveCore ? kScanSlowMs : kScanWaitMs;
            if (!aimActive && (g_enabled.load() || g_recon.load()) && t - lastScan > wait) {
                long s0 = now_ms();
                scan_heap();
                lastScan = now_ms();
                LOG("heap scan %ld ms: LOCAL=%d REMOTE=%d GUIDE=%d STRIKER=%d PUCK=%d POCKET=%d", lastScan - s0,
                    count_valid(C_LOCAL), count_valid(C_REMOTE), count_valid(C_FG), count_valid(C_STRIKER), count_valid(C_PUCK), count_valid(C_POCKET));
                LOG("state cls L=%lx R=%lx G=%lx S=%lx P=%lx K=%lx | off L.ang=%d L.dist=%d N.pos=%d N.parent=%d N.csize=%d N.scx=%d P.rad=%d",
                    (unsigned long)g_cls[C_LOCAL], (unsigned long)g_cls[C_REMOTE], (unsigned long)g_cls[C_FG],
                    (unsigned long)g_cls[C_STRIKER], (unsigned long)g_cls[C_PUCK], (unsigned long)g_cls[C_POCKET],
                    L_angle.off, L_dist.off, N_pos.off, N_parent.off, N_size.off, N_scx.off, P_radius.off);
            }
            for (int c = 0; c < C_COUNT; c++) {
                std::vector<u64> keep;
                for (u64 o : g_objs[c]) if (still_valid(c, o)) keep.push_back(o);
                g_objs[c].swap(keep);
            }
            apply_guide_test();
        }

        // --- aim tracking: while the finger is down we poll fast, the release starts a shot recording
        Aim a = read_aim();
        bool release = false;
        if (a.ok) {
            if (a.act == 1) { lastAim = a; aimActive = true; }
            else if (aimActive) {
                aimActive = false; release = true;
                LOG("AIM released: last ang=%.5f(%.2fdeg) dist=%.3f en=%d power_ok=%d",
                    lastAim.ang, lastAim.ang * 180.0 / M_PI, lastAim.dist, lastAim.en, lastAim.enough);
                if (g_enabled.load()) {
                    char pl[300];
                    { std::lock_guard<std::mutex> lk(g_predMx); snprintf(pl, sizeof(pl), "%s", g_predLastLog); }
                    LOG("PRED last: %s", pl);
                }
            }
        }

        pred_tick(a, t);

        // --- coins moving by themselves (opponent shot) also start a recording
        bool puckMoved = false;
        std::vector<Item> items;
        if (!burst && g_recon.load()) {
            items = build_items();
            read_xy(items, xy);
            if (same_items(items, prevItems)) puckMoved = pucks_moved(items, xy, prevXY);
            prevItems = items; prevXY = xy;
        }
        if (!burst && g_recon.load() && (release || puckMoved) && bursts < 24 && !items.empty()) {
            burst = true; bursts++; burstStart = t; lastMove = t; burstLines = 0;
            bItems = items; bPrev = xy; bLogged = xy;
            LOG("BURST %d start (%s) lastAim ang=%.5f dist=%.3f items=%d", bursts, release ? "aim release" : "coins moved",
                lastAim.ang, lastAim.dist, (int)bItems.size());
            for (size_t i = 0; i < bItems.size(); i++) {
                float r = NAN; if (bItems[i].kind == 'P') getf(bItems[i].o, P_radius.off, r);
                LOG("BURST obj %d %c@%lx r=%.1f start=%.2f,%.2f", (int)i, bItems[i].kind, (unsigned long)bItems[i].o, r, xy[2 * i], xy[2 * i + 1]);
            }
        }
        if (burst) {
            read_xy(bItems, xy);
            if (any_moved(bItems, xy, bPrev)) lastMove = t;
            bPrev = xy;
            // delta log: only objects whose position changed since the last logged value (all others stay as in "BURST obj ... start=")
            std::string line; char b[64];
            for (size_t i = 0; i < bItems.size(); i++) {
                double x = xy[2 * i], y = xy[2 * i + 1], lx = bLogged[2 * i], ly = bLogged[2 * i + 1];
                bool fin = isfinite(x) && isfinite(y);
                bool changed = fin ? (!isfinite(lx) || fabs(x - lx) > 0.004 || fabs(y - ly) > 0.004) : isfinite(lx);
                if (!changed) continue;
                if (fin) snprintf(b, sizeof(b), " %d=%.2f,%.2f", (int)i, x, y); else snprintf(b, sizeof(b), " %d=-", (int)i);
                line += b;
                bLogged[2 * i] = x; bLogged[2 * i + 1] = y;
            }
            if (!line.empty()) {
                LOG("B %ld%s", t - burstStart, line.c_str());
            }
            burstLines++;
            if ((t - burstStart > 800 && t - lastMove > 1500) || t - burstStart > 12000 || burstLines >= 700) {
                burst = false;
                LOG("BURST end lines=%d", burstLines);
            }
        } else if (!aimActive && g_recon.load() && t - lastSnap >= 500) {
            lastSnap = t;
            std::string s = snapshot();
            if (s != last || t - lastBeat > 10000) {
                log_lines(s);
                last = s; lastBeat = t;
            }
            // parent chains (to find the board node and the point -> pixel scale), a few times only
            if (chainDumps < 3 && t - lastChain > 90000) {
                std::vector<Item> it = build_items();
                u64 st = 0, pk = 0, pc = 0;
                for (auto& x : it) { if (x.kind == 'S' && !st) st = x.o; if (x.kind == 'P' && !pk) pk = x.o; }
                for (u64 o : g_objs[C_POCKET]) { double px, py; int vis, run;
                    if (getpt(o, N_pos.off, px, py) && sane(px, 5000.0) && sane(py, 5000.0) && getb(o, N_vis.off, vis) && getb(o, N_run.off, run) && vis <= 1 && run <= 1) { pc = o; break; } }
                if (st && (pk || pc)) {
                    LOG("CHAIN dump %d", chainDumps + 1);
                    dump_chain("STRIKER", st);
                    if (pk) dump_chain("PUCK", pk);
                    if (pc) dump_chain("POCKET", pc);
                    chainDumps++; lastChain = t;
                }
            }
        }
        usleep(burst ? 16000 : (aimActive ? 10000 : 100000));
    }
    LOG("probe finished");
    return nullptr;
}

__attribute__((constructor)) static void init() {
    LOG("carprobe v5 loaded (stage 2 prediction; test write kTestMaxGuides=%d)", kTestMaxGuides);
    pthread_t th;
    pthread_create(&th, nullptr, worker, nullptr);
    pthread_detach(th);
}

// ================================================================== JNI (Kotlin object top.niunaijun.blackboxa.menu.NativeCarrom)
// The function names must match that object's package, name and method names exactly.
extern "C" {

JNIEXPORT void JNICALL Java_top_niunaijun_blackboxa_menu_NativeCarrom_setEnabled(JNIEnv*, jobject, jint on) {
    g_enabled.store(on != 0 ? 1 : 0);
    if (!on) { std::vector<float> e; pred_set(e, "Prediction: OFF"); }
}

// frictionPercent 50..150, maxBounces 0..5, deflection 0/1, recon 0/1 (values outside the range are clamped)
JNIEXPORT void JNICALL Java_top_niunaijun_blackboxa_menu_NativeCarrom_setOptions(JNIEnv*, jobject, jint frictionPercent, jint maxBounces, jint deflection, jint recon) {
    int f = frictionPercent < 50 ? 50 : (frictionPercent > 150 ? 150 : frictionPercent);
    int b = maxBounces < 0 ? 0 : (maxBounces > 5 ? 5 : maxBounces);
    g_fricPct.store(f);
    g_maxBounces.store(b);
    g_deflect.store(deflection != 0 ? 1 : 0);
    g_recon.store(recon != 0 ? 1 : 0);
}

// [designW, designH, nPaths, {type, nPoints, x,y,...}...] in design points (y up); empty array = nothing to draw
JNIEXPORT jfloatArray JNICALL Java_top_niunaijun_blackboxa_menu_NativeCarrom_lines(JNIEnv* env, jobject) {
    std::vector<float> copy;
    { std::lock_guard<std::mutex> lk(g_predMx); copy = g_predData; }
    if (copy.size() > 100000) copy.clear();
    jfloatArray arr = env->NewFloatArray((jsize)copy.size());
    if (!arr) return nullptr;
    if (!copy.empty()) env->SetFloatArrayRegion(arr, 0, (jsize)copy.size(), copy.data());
    return arr;
}

JNIEXPORT jstring JNICALL Java_top_niunaijun_blackboxa_menu_NativeCarrom_statusText(JNIEnv* env, jobject) {
    char b[400];
    { std::lock_guard<std::mutex> lk(g_predMx); snprintf(b, sizeof(b), "%s", g_predStatus); }
    return env->NewStringUTF(b);
}

}  // extern "C"
