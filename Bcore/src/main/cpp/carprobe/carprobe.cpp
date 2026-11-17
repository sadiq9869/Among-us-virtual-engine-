// carprobe.cpp  v5.2 = v5.1 prediction (round-2 physics, coin-coin, pockets, foul) + READ-ONLY DUMP MODE.
//   NEW in v5.2: DUMP MODE. Once per game start, the first time a match is found (coins + aim objects on the heap), the probe
//   (1) finds ALL Objective-C classes of the game library by scanning its data segments for class structs (no runtime API needed),
//   (2) lists every ivar whose name looks physics-related (friction, restitution, mass, velocity, damping, angular, impulse ...),
//   (3) dumps ALL ivars (name, type, offset) of TableLogic, PuckModel, StrikerModel, TableShape, TablePolygon, PuckLogic, PuckNode,
//       StrikerNode, TableNode, ControlsLogic* and of every class that owns an ivar called friction / spinFriction / mass /
//       restitution / physicsSpeedFactor,
//   (4) best-effort method list of those classes (method entry layout is NOT confirmed: names may print as "?"),
//   (5) reads the live values of all ivars of the found instances (idle), and after each of the first 3 shots prints only the
//       numeric ivars that CHANGED (60 / 250 / 600 ms after release) = velocity / spin / state fields.
//   All log lines of this mode start with "DUMP". Nothing is written to game memory, no hooks.
//   NOT built here by an AI beyond a host syntax check; NOT run in the game.
// (older header v5.1) round-2 physics, multi-body simulation with coin-coin collisions, pockets, foul marker, "PRED status" diagnostics.
// (older header v5) STAGE 2: SOLID PREDICTION LINE (simulation + JNI for the Kotlin overlay) on top of the v4.2 probe.
//   table/root transform read from the node parent chain at runtime, JNI object top.niunaijun.blackboxa.menu.NativeCarrom
//   (setEnabled / setOptions / lines / statusText). READ-ONLY: no hooks, no code patches, no game-memory writes
//   (the old kTestMaxGuides test write stays disabled). The probe never stops (the overlay needs it).
//   Physics numbers below are MEASURED from build 19.6.1 (Freestyle Offline only) and UNCALIBRATED.
// (older header) v4.2 = STAGE 1 RECON (burst lines list only objects that MOVED): parent chains, PocketNode positions, shot recording at 16 ms.
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
#include <set>
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
static const int   kDumpEnabled  = 1;                  // v5.2: 1 = run the one-time DUMP mode when the first match is found
static const int   kDumpMaxObjsPerClass = 24;          // live instances printed per dumped class
static const int   kDumpMaxObjsTotal    = 150;
static const int   kDumpMaxHits         = 300;         // physics-looking ivar names printed
static const int   kDumpMaxMethods      = 200;         // per class

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
// Round 2 (v5.1): re-fitted on three recorded shots (BURST 10, 11, 12). Units = the game's table points, time = 1 physics step (60 Hz).
static const double kMinDrag       = 30.3;     // no shot at or below this drag length            (round 1)
static const double kV0Slope       = 0.198;    // first-step speed = slope * dist + offset         (round 2: 3 shots, residual < 0.05; round 1 offset was -5.99, ~0.65 too low)
static const double kV0Offset      = -5.34;
static const double kV0Cap         = 18.08;    // first-step speed cap, reached at dist ~118       (round 2: BURST 11)
static const double kFricHigh      = 0.17;     // speed lost per step while speed > kFricSwitch    (round 2: striker + coin, many samples)
static const double kFricSwitch    = 4.0;      // regime switch, +-0.15                            (round 2: BURST 10 striker slide)
static const double kFricLow       = 0.0715;   // speed lost per step at low speed, constant       (round 2: BURST 10 striker slide 4.0 -> 0)
static const double kStrikerRadius = 17.4;     // +-1                                              (round 1)
static const double kBoardHalf     = 254.1;    // centre limit = kBoardHalf - radius: striker 236.7, coin 241.0 (round 1; coin bounce re-confirmed round 2)
static const double kStrikerWallE  = 0.98;     // speed kept after a wall bounce (striker)         (round 1; re-confirmed round 2)
static const double kCoinWallE     = 0.73;     // normal speed kept after a wall bounce, tangential part kept (round 2 BURST 10: 237.29 predicted 237.26)
static const double kRestStrikerCoin = 1.0;    // striker - coin restitution                       (round 1)
static const double kRestCoinCoin    = 1.0;    // coin - coin restitution, equal masses            (round 2: BURST 10, normal part 12.8 -> 12.65 measured)
static const double kStrikerMass   = 1.7;      // striker mass / coin mass                         (round 1; re-checked round 2: coin 1.259 x, striker 0.259 x)
static const double kCoinMass      = 1.0;
// Pockets (round 2, ONLY 3 events: 2 coins, 1 striker). A body is captured when the position at the START of a step is closer to the
// pocket centre than this. Real interval: coin 11.0 .. 17.4, striker 3.7 .. 5.8 (the mid points are used). UNCERTAIN, tune with more events.
static const double kPocketEnterCoin    = 14.0;
static const double kPocketEnterStriker = 4.7;
static const double kPocketRing         = 22.0;   // drawn ring radius around a pocket that catches something (display only)
static const double kAimTol        = 3.0;      // LOCAL._center must be this close to a table striker
static const int    kMaxFrames     = 700;
static const double kStopSpeed     = 0.05;
static const double kDeflectMaxLen = 120.0;    // the short striker remainder after the hit is cut at this length
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
    bool ok; u64 localObj; u64 striker; u64 table; Xf toRoot; double rootW, rootH; std::vector<Coin> coins; std::vector<V2> pockets; long at; std::string why;
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
    // 3. pockets: PocketNodes in the same table and coordinate space (duplicates within 1 unit are dropped)
    for (u64 o : g_objs[C_POCKET]) {
        int vis, run; double x, y;
        if (!getb(o, N_vis.off, vis) || !getb(o, N_run.off, run) || vis != 1 || run != 1) continue;
        if (!read_pos(o, x, y) || fabs(x) > kBoardBound || fabs(y) > kBoardBound) continue;
        Chain ch = walk_chain(o);
        if (!ch.ok || ch.table != w.table || !xf_close(ch.toRoot, w.toRoot)) continue;
        bool dup = false;
        for (const V2& p : w.pockets) if (hypot(p.x - x, p.y - y) < 1.0) dup = true;
        if (!dup) w.pockets.push_back(V2{x, y});
    }
    w.ok = true;
    char b[96]; snprintf(b, sizeof(b), "ok (rejected pucks: %d, pockets: %d)", rejChain, (int)w.pockets.size()); w.why = b;
    g_world = w;
}

// ---- simulation: every body on the table (striker + all coins) frame by frame, exact time of impact inside a frame
struct SBody {
    double x, y, vx, vy, r, m;
    bool striker, alive, draw, moved, hitByStriker;
    int pocket, bounces, coinIdx;      // coinIdx = index in World::coins, -1 for the striker
    size_t hitIdx;                     // striker: index in pts of its first collision point (0 = none)
    double afterHitLen;
    std::vector<V2> pts;               // polyline: start, wall contacts, collisions, end
};

static SBody mk_body(double x, double y, double r, double m, bool striker, int coinIdx) {
    SBody b;
    b.x = x; b.y = y; b.vx = 0; b.vy = 0; b.r = r; b.m = m;
    b.striker = striker; b.alive = true; b.draw = true; b.moved = false; b.hitByStriker = false;
    b.pocket = -1; b.bounces = 0; b.coinIdx = coinIdx; b.hitIdx = 0; b.afterHitLen = 0;
    b.pts.push_back(V2{x, y});
    return b;
}

static double speed_of(const SBody& b) { return hypot(b.vx, b.vy); }
static double friction_loss(double v, double scale) { return ((v > kFricSwitch) ? kFricHigh : kFricLow) * scale; }
static void sb_point(SBody& b) { if (b.draw && b.pts.size() < 400) b.pts.push_back(V2{b.x, b.y}); }

// B[0] is the striker with its launch velocity set, B[1..] are the coins at rest. Returns the coinIdx the striker touched first (-1 = none).
static int simulate(std::vector<SBody>& B, const std::vector<V2>& pockets, int maxBounces, double fs, bool deflect) {
    int firstHit = -1;
    for (int f = 0; f < kMaxFrames; f++) {
        // pocket capture is decided from the position at the START of the step (measured: the drag begins on the step that starts inside)
        for (SBody& a : B) {
            if (!a.alive) continue;
            double R = a.striker ? kPocketEnterStriker : kPocketEnterCoin;
            for (size_t p = 0; p < pockets.size(); p++) {
                if (hypot(a.x - pockets[p].x, a.y - pockets[p].y) < R) {
                    a.alive = false; a.pocket = (int)p; a.vx = a.vy = 0; sb_point(a);
                    break;
                }
            }
        }
        bool any = false;
        for (const SBody& a : B) if (a.alive && speed_of(a) > 1e-9) { any = true; break; }
        if (!any) break;

        double rem = 1.0;
        for (int ev = 0; ev < 64 && rem > 1e-9; ev++) {
            double tBest = 1.0; int kind = 0, ei = -1, ej = -1;
            for (size_t i = 0; i < B.size(); i++) {
                const SBody& a = B[i];
                if (!a.alive) continue;
                double dx = a.vx * rem, dy = a.vy * rem;
                if (fabs(dx) < 1e-12 && fabs(dy) < 1e-12) continue;
                double lim = kBoardHalf - a.r;
                if (dx > 1e-12 && a.x + dx > lim)        { double t = std::max(0.0, (lim - a.x) / dx);  if (t < tBest) { tBest = t; kind = 1; ei = (int)i; } }
                else if (dx < -1e-12 && a.x + dx < -lim) { double t = std::max(0.0, (-lim - a.x) / dx); if (t < tBest) { tBest = t; kind = 1; ei = (int)i; } }
                if (dy > 1e-12 && a.y + dy > lim)        { double t = std::max(0.0, (lim - a.y) / dy);  if (t < tBest) { tBest = t; kind = 2; ei = (int)i; } }
                else if (dy < -1e-12 && a.y + dy < -lim) { double t = std::max(0.0, (-lim - a.y) / dy); if (t < tBest) { tBest = t; kind = 2; ei = (int)i; } }
                for (size_t j = 0; j < B.size(); j++) {
                    if (j == i) continue;
                    const SBody& c = B[j];
                    if (!c.alive) continue;
                    bool cMoving = (fabs(c.vx) + fabs(c.vy)) > 1e-12;
                    if (cMoving && j < i) continue;                 // that pair is handled from c's side
                    double ex = dx - c.vx * rem, ey = dy - c.vy * rem;
                    double px = a.x - c.x, py = a.y - c.y, R = a.r + c.r;
                    double aa = ex * ex + ey * ey, bb = px * ex + py * ey, cc = px * px + py * py - R * R;
                    if (aa < 1e-18) continue;
                    double t;
                    if (cc <= 0) { if (bb >= 0) continue; t = 0; }  // already touching: counts only if moving closer
                    else {
                        if (bb >= 0) continue;
                        double disc = bb * bb - aa * cc;
                        if (disc < 0) continue;
                        t = (-bb - sqrt(disc)) / aa;
                        if (t < 0) continue;
                    }
                    if (t < tBest) { tBest = t; kind = 3; ei = (int)i; ej = (int)j; }
                }
            }
            for (SBody& a : B) {
                if (!a.alive) continue;
                double mx = a.vx * rem * tBest, my = a.vy * rem * tBest;
                a.x += mx; a.y += my;
                if (mx != 0.0 || my != 0.0) a.moved = true;
                if (a.striker && a.hitIdx) a.afterHitLen += hypot(mx, my);
            }
            rem *= (1.0 - tBest);
            if (B[0].alive && B[0].hitIdx && B[0].draw && B[0].afterHitLen > kDeflectMaxLen) { sb_point(B[0]); B[0].draw = false; }
            if (kind == 0) break;

            if (kind == 1 || kind == 2) {
                SBody& a = B[ei];
                double lim = kBoardHalf - a.r;
                if (kind == 1) { a.x = (a.vx > 0) ? lim : -lim; a.vx = -a.vx; }
                else           { a.y = (a.vy > 0) ? lim : -lim; a.vy = -a.vy; }
                if (a.striker) { a.vx *= kStrikerWallE; a.vy *= kStrikerWallE; }
                else if (kind == 1) a.vx *= kCoinWallE;
                else a.vy *= kCoinWallE;
                a.bounces++;
                sb_point(a);
                if (a.bounces > maxBounces) a.draw = false;
            } else {
                SBody& a = B[ei];
                SBody& c = B[ej];
                double nx = c.x - a.x, ny = c.y - a.y, nl = hypot(nx, ny);
                if (nl < 1e-9) break;
                nx /= nl; ny /= nl;
                double vn = (a.vx - c.vx) * nx + (a.vy - c.vy) * ny;
                if (vn > 0) {
                    double e = (a.striker || c.striker) ? kRestStrikerCoin : kRestCoinCoin;
                    double j = (1.0 + e) * vn / (1.0 / a.m + 1.0 / c.m);
                    a.vx -= j / a.m * nx; a.vy -= j / a.m * ny;
                    c.vx += j / c.m * nx; c.vy += j / c.m * ny;
                    sb_point(a); sb_point(c);
                    SBody* st = a.striker ? &a : (c.striker ? &c : nullptr);
                    if (st && !st->hitIdx && st->draw) {
                        st->hitIdx = st->pts.size() - 1;
                        if (!deflect) st->draw = false;
                    }
                    if (st) {
                        SBody& co = a.striker ? c : a;
                        if (firstHit < 0) firstHit = co.coinIdx;
                        co.hitByStriker = true;
                    }
                }
            }
        }
        // friction at the end of the step
        for (SBody& a : B) {
            if (!a.alive) continue;
            double sp = speed_of(a);
            if (sp < 1e-9) continue;
            double ns = sp - friction_loss(sp, fs);
            if (ns < kStopSpeed) { a.vx = a.vy = 0; }
            else { a.vx *= ns / sp; a.vy *= ns / sp; }
        }
    }
    for (SBody& a : B) if (a.alive && a.moved) sb_point(a);
    return firstHit;
}

// ---- prediction output (read by JNI): [designW, designH, nPaths, {type, nPoints, x0,y0,x1,y1...}...]  design points, y up
// type 0 = striker path (white), 1 = coin hit by the striker (red), 2 = striker remainder after the hit (thin white),
// 3 = ring marker "coin potted" (2 points: pocket centre, point on the ring), 4 = ring marker "striker potted = FOUL", 5 = coin moved by another coin (orange)
static std::mutex g_predMx;
static std::vector<float> g_predData;
static char g_predStatus[400] = "Prediction: waiting for the game";
static char g_predLastLog[900] = "";

// every status that is not the running "Prediction ON ..." line is also written to logcat once per change (tag CARPROBE1, "PRED status"),
// so a missing line can be diagnosed from adb without opening the panel
static void pred_set(const std::vector<float>& d, const char* st) {
    static char lastLogged[400] = "";
    bool doLog = false;
    {
        std::lock_guard<std::mutex> lk(g_predMx);
        g_predData = d;
        snprintf(g_predStatus, sizeof(g_predStatus), "%s", st);
        if (strncmp(st, "Prediction ON", 13) != 0) {
            if (strcmp(st, lastLogged) != 0) { snprintf(lastLogged, sizeof(lastLogged), "%s", st); doLog = true; }
        } else if (lastLogged[0]) {
            lastLogged[0] = 0;
            doLog = true;
        }
    }
    if (doLog) LOG("PRED status: %s", st);
}

static void put_path(std::vector<float>& out, int type, const std::vector<V2>& pts, const Xf& m) {
    out.push_back((float)type); out.push_back((float)pts.size());
    for (const V2& p : pts) { V2 q = xf_apply(m, p.x, p.y); out.push_back((float)q.x); out.push_back((float)q.y); }
}

static void put_ring(std::vector<float>& out, int type, const V2& c, double rad, const Xf& m) {
    out.push_back((float)type); out.push_back(2.f);
    V2 p = xf_apply(m, c.x, c.y), q = xf_apply(m, c.x + rad, c.y);
    out.push_back((float)p.x); out.push_back((float)p.y); out.push_back((float)q.x); out.push_back((float)q.y);
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
    std::vector<SBody> B;
    B.reserve(w.coins.size() + 1);
    B.push_back(mk_body(sx, sy, kStrikerRadius, kStrikerMass, true, -1));
    B[0].vx = cos(a.ang) * v0; B[0].vy = sin(a.ang) * v0;
    for (size_t i = 0; i < w.coins.size(); i++) B.push_back(mk_body(w.coins[i].x, w.coins[i].y, w.coins[i].r, kCoinMass, false, (int)i));
    int first = simulate(B, w.pockets, mb, fs, g_deflect.load() != 0);

    std::vector<float> out;
    out.push_back((float)w.rootW); out.push_back((float)w.rootH);
    size_t idxN = out.size(); out.push_back(0.f);
    int nPaths = 0;
    const SBody& s0 = B[0];
    size_t hi = s0.hitIdx;
    { std::vector<V2> mainPts(s0.pts.begin(), s0.pts.begin() + (hi ? hi + 1 : s0.pts.size())); put_path(out, 0, mainPts, w.toRoot); nPaths++; }
    if (hi && s0.pts.size() - hi >= 2) { std::vector<V2> d(s0.pts.begin() + hi, s0.pts.end()); put_path(out, 2, d, w.toRoot); nPaths++; }
    int movedCoins = 0, potted = 0;
    char coinLog[640]; coinLog[0] = 0;
    for (size_t i = 1; i < B.size(); i++) {
        const SBody& c = B[i];
        if (c.moved && c.pts.size() >= 2) {
            put_path(out, c.hitByStriker ? 1 : 5, c.pts, w.toRoot); nPaths++;
            movedCoins++;
            size_t l = strlen(coinLog);
            if (l < 500) snprintf(coinLog + l, sizeof(coinLog) - l, " c%d(%.1f,%.1f)->(%.1f,%.1f)%s", c.coinIdx, c.pts.front().x, c.pts.front().y, c.x, c.y, c.pocket >= 0 ? "[POT]" : "");
        }
        if (c.pocket >= 0) { put_ring(out, 3, w.pockets[c.pocket], kPocketRing, w.toRoot); nPaths++; potted++; }
    }
    bool foul = (s0.pocket >= 0);
    if (foul) { put_ring(out, 4, w.pockets[s0.pocket], kPocketRing, w.toRoot); nPaths++; }
    out[idxN] = (float)nPaths;

    char hitTxt[120] = "striker hits no coin";
    if (first >= 0) snprintf(hitTxt, sizeof(hitTxt), "first hit coin %d at (%.1f,%.1f)", first, w.coins[first].x, w.coins[first].y);
    V2 origin = xf_apply(w.toRoot, 0, 0);
    snprintf(st, sizeof(st), "Prediction ON | ang %.1f deg  drag %.1f  v0 %.2f/step | coins %d pockets %d | %s | coins moved %d potted %d%s | table origin in design (%.1f,%.1f) root %.0fx%.0f",
             a.ang * 180.0 / M_PI, a.dist, v0, (int)w.coins.size(), (int)w.pockets.size(), hitTxt, movedCoins, potted, foul ? " | FOUL: striker potted" : "",
             origin.x, origin.y, w.rootW, w.rootH);
    pred_set(out, st);
    {
        std::lock_guard<std::mutex> lk(g_predMx);
        snprintf(g_predLastLog, sizeof(g_predLastLog), "ang=%.4f dist=%.2f v0=%.3f start=%.2f,%.2f striker_end=%.1f,%.1f%s | %s |%s", a.ang, a.dist, v0, sx, sy,
                 s0.x, s0.y, foul ? "[POT=FOUL]" : "", hitTxt, coinLog);
    }
}

// ================================================================== v5.2 DUMP MODE (read-only)
// Goal: find out where the game keeps friction / spinFriction / mass / restitution / physicsSpeedFactor (if they are ObjC ivars at all)
// and which ivars of the model objects change while a shot is running. Everything is read through rd(); nothing is written.
// All class addresses that live in HEAP memory are stored XOR-ed with kObf, otherwise the heap scan would find our own copies.

struct IvInfo { std::string cls, name, type; int off; };
struct DumpObj { u64 o; std::vector<std::string> idle; };            // idle = numeric value text per ivar ("" = not numeric)
struct DumpCls { std::string name; u64 clsX; std::vector<IvInfo> ivs; std::vector<DumpObj> objs; };
static std::vector<DumpCls> g_dc;
static bool g_dumpDone = false;
static bool g_dumpReady = false;

static const char* kDumpClasses[] = {
    "TableLogic", "PuckModel", "StrikerModel", "TableShape", "TablePolygon", "PuckLogic",
    "PuckNode", "StrikerNode", "TableNode", "ControlsLogic", "ControlsLogicLocal", "ControlsForegroundNode",
    "GameplayMultiPlayerView", "GameplayMultiPlayerFreestyleOfflineView"
};
// substrings (lower case) of ivar names that are listed as "DUMP hit"
static const char* kInterest[] = {
    "fric", "restit", "mass", "veloc", "damp", "angular", "impuls", "bounce", "elastic", "speedfactor", "physic", "spinrate", "spinspeed"
};
// exact (lower case) ivar names whose owner class is dumped completely
static const char* kExact[] = {
    "_friction", "friction", "spinfriction", "_spinfriction", "physicsspeedfactor", "_physicsspeedfactor",
    "_mass", "mass", "_restitution", "restitution"
};

static std::string lower_s(const std::string& s) {
    std::string r = s;
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}

static bool in_lib(u64 p) {
    for (const Seg& s : g_segs) if (p >= s.start && p < s.start + s.size) return true;
    return false;
}

static std::string class_name_s(u64 c) {
    char nm[96];
    if (class_name_at(c, nm, sizeof(nm))) return std::string(nm);
    return std::string("?");
}

// Finds every ObjC class struct of the game library by pattern (name pointer inside the library, info flags = class (not meta), plausible instance size).
static std::vector<u64> enum_classes() {
    std::vector<u64> out;
    std::set<u64> seen;
    for (const Seg& s : g_segs) {
        if (!(s.flags & PF_R) || (s.flags & PF_X)) continue;
        u64 a0 = (s.start + 7) & ~(u64)7;
        u64 end = s.start + s.size;
        for (u64 a = a0; a + 64 <= end; a += CH) {
            size_t n = (size_t)std::min<u64>(CH + 64, end - a);
            n &= ~(size_t)7;
            if (n < 64 || !rd(a, g_scanbuf, n)) continue;
            size_t lim = std::min<size_t>(CH, n - 64);
            for (size_t i = 0; i <= lim; i += 8) {
                u64 w[8];
                memcpy(w, g_scanbuf + i, 64);
                u64 info = w[4], isz = w[5];
                if (!w[0] || !(info & 1) || (info & 2) || info >= 65536) continue;
                if (isz < 8 || isz > 100000) continue;
                if (!in_lib(w[2])) continue;
                u64 c = a + i;
                if (seen.count(c)) continue;
                char nm[96];
                if (!rd_str(w[2], nm, sizeof(nm)) || !is_ident(nm)) continue;
                seen.insert(c);
                out.push_back(c);
            }
        }
    }
    return out;
}

// ivars declared by this class only (no super classes)
static std::vector<IvInfo> class_ivars_own(u64 c, const std::string& cname) {
    std::vector<IvInfo> v;
    u64 list;
    if (!rd(c + 48, &list, 8) || !list) return v;
    u32 cnt;
    if (!rd(list, &cnt, 4) || cnt == 0 || cnt > 400) return v;
    std::vector<uint8_t> raw(8 + 24ull * cnt);
    if (!rd(list, raw.data(), raw.size())) return v;
    for (u32 i = 0; i < cnt; i++) {
        const uint8_t* e = raw.data() + 8 + 24ull * i;
        u64 np, tp; u32 off;
        memcpy(&np, e, 8); memcpy(&tp, e + 8, 8); memcpy(&off, e + 16, 4);
        char nm[96], ty[100];
        if (!rd_str(np, nm, sizeof(nm))) continue;
        if (!rd_str(tp, ty, sizeof(ty))) ty[0] = 0;
        IvInfo iv; iv.cls = cname; iv.name = nm; iv.type = ty; iv.off = (int)off;
        v.push_back(iv);
    }
    return v;
}

// ivars of the class and its super classes (stops at CC* / NS* base classes: cocos nodes are not interesting here)
static std::vector<IvInfo> all_ivars(u64 c) {
    std::vector<IvInfo> v;
    for (int d = 0; d < 6 && c; d++) {
        std::string nm = class_name_s(c);
        if (d > 0 && (nm == "?" || nm.compare(0, 2, "CC") == 0 || nm.compare(0, 2, "NS") == 0)) break;
        std::vector<IvInfo> own = class_ivars_own(c, nm);
        v.insert(v.end(), own.begin(), own.end());
        c = super_of(c);
    }
    return v;
}

static int prim_size(char c) {
    switch (c) {
        case 'f': case 'i': case 'I': case 'l': case 'L': return 4;
        case 'd': case 'q': case 'Q': return 8;
        case 's': case 'S': return 2;
        case 'c': case 'C': case 'B': return 1;
        default: return 0;
    }
}

static void fmt_prim(const uint8_t* p, char c, char* o, size_t n) {
    switch (c) {
        case 'f': { float v; memcpy(&v, p, 4); snprintf(o, n, "%.5g", (double)v); break; }
        case 'd': { double v; memcpy(&v, p, 8); snprintf(o, n, "%.7g", v); break; }
        case 'i': case 'l': { int32_t v; memcpy(&v, p, 4); snprintf(o, n, "%d", (int)v); break; }
        case 'I': case 'L': { uint32_t v; memcpy(&v, p, 4); snprintf(o, n, "%u", (unsigned)v); break; }
        case 's': { int16_t v; memcpy(&v, p, 2); snprintf(o, n, "%d", (int)v); break; }
        case 'S': { uint16_t v; memcpy(&v, p, 2); snprintf(o, n, "%u", (unsigned)v); break; }
        case 'c': { int8_t v = (int8_t)p[0]; snprintf(o, n, "%d", (int)v); break; }
        case 'C': case 'B': snprintf(o, n, "%u", (unsigned)p[0]); break;
        case 'q': { int64_t v; memcpy(&v, p, 8); snprintf(o, n, "%lld", (long long)v); break; }
        case 'Q': { uint64_t v; memcpy(&v, p, 8); snprintf(o, n, "%llu", (unsigned long long)v); break; }
        default: snprintf(o, n, "?"); break;
    }
}

// Formats one ivar of a live object by its type encoding. numericOnly = only numbers and flat structs of numbers (for the idle/changed comparison).
static bool fmt_value(u64 obj, const IvInfo& iv, bool numericOnly, std::string& out) {
    if (iv.off < 0) return false;
    const char* t = iv.type.c_str();
    while (*t && strchr("rnNoORV", *t)) t++;
    char c = *t;
    char b[200];
    int ps = prim_size(c);
    if (ps) {
        uint8_t buf[8];
        if (!rd(obj + (u64)iv.off, buf, (size_t)ps)) return false;
        fmt_prim(buf, c, b, sizeof(b));
        out = b;
        return true;
    }
    if (c == '@' || c == '^' || c == '*' || c == ':' || c == '#') {
        if (numericOnly) return false;
        u64 p;
        if (!rd(obj + (u64)iv.off, &p, 8)) return false;
        if (c == '@' && p) {
            std::string cn = obj_cname(p);
            snprintf(b, sizeof(b), "0x%lx(%s)", (unsigned long)p, cn.empty() ? "?" : cn.c_str());
        } else snprintf(b, sizeof(b), "0x%lx", (unsigned long)p);
        out = b;
        return true;
    }
    if (c == '{') {
        const char* q = strchr(t, '=');
        char kinds[16]; int offs[16]; int nf = 0, cur = 0;
        bool ok = (q != nullptr);
        if (ok) q++;
        while (ok && *q && *q != '}') {
            if (*q == '"') { q++; while (*q && *q != '"') q++; if (*q) q++; continue; }
            int s = prim_size(*q);
            if (!s || nf >= 16) { ok = false; break; }
            cur = (cur + s - 1) / s * s;
            kinds[nf] = *q; offs[nf] = cur; nf++; cur += s; q++;
        }
        if (ok && *q == '}' && nf > 0 && cur <= 64) {
            uint8_t buf[64];
            if (!rd(obj + (u64)iv.off, buf, (size_t)cur)) return false;
            std::string s = "(";
            for (int k = 0; k < nf; k++) {
                fmt_prim(buf + offs[k], kinds[k], b, sizeof(b));
                if (k) s += ",";
                s += b;
            }
            s += ")";
            out = s;
            return true;
        }
    }
    if (numericOnly) return false;
    uint8_t raw[16];
    if (!rd(obj + (u64)iv.off, raw, 16)) return false;
    std::string s = "raw=";
    for (int k = 0; k < 16; k++) { snprintf(b, sizeof(b), "%02x", raw[k]); s += b; }
    out = s;
    return true;
}

static void log_chunked(const std::string& head, const std::vector<std::string>& items) {
    std::string line = head;
    for (const std::string& it : items) {
        if (line.size() + it.size() > 700 && line.size() > head.size()) {
            LOG("%s", line.c_str());
            line = head + " (cont)";
        }
        line += " ";
        line += it;
    }
    if (line.size() > head.size()) LOG("%s", line.c_str());
}

static bool is_sel(const char* s) {
    size_t n = strlen(s);
    if (n < 1 || n > 119) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x21 || (unsigned char)s[i] > 0x7e) return false;
    return true;
}

// BEST EFFORT, layout of a method entry is not confirmed: word0 is tried as a name string, then as a pointer to one; word2 is printed as RVA (probably the IMP).
static void dump_methods(u64 c, const std::string& cn) {
    u64 ml;
    if (!rd(c + 56, &ml, 8)) return;
    int n = 0;
    for (int li = 0; ml && li < 6; li++) {
        u64 next; u32 cnt;
        if (!rd(ml, &next, 8) || !rd(ml + 8, &cnt, 4) || cnt > 2000) break;
        for (u32 i = 0; i < cnt && n < kDumpMaxMethods; i++) {
            u64 e[3];
            if (!rd(ml + 16 + 24ull * i, e, 24)) break;
            char nm[120]; nm[0] = 0;
            bool ok = rd_str(e[0], nm, sizeof(nm)) && is_sel(nm);
            if (!ok) {
                u64 p2;
                ok = rd(e[0], &p2, 8) && rd_str(p2, nm, sizeof(nm)) && is_sel(nm);
            }
            LOG("DUMP m %s.%s w2_rva=0x%lx", cn.c_str(), ok ? nm : "?", (unsigned long)(e[2] - g_bias));
            n++;
        }
        ml = next;
    }
}

// heap scan for instances of several classes (wantX = class addresses XOR kObf). Hits inside our scan buffer and inside this thread's stack are dropped.
static void scan_heap_x(const std::vector<u64>& wantX, std::vector<std::vector<u64>>& out) {
    out.assign(wantX.size(), std::vector<u64>());
    u64 stLo = 0, stHi = 0;
    pthread_attr_t at;
    if (pthread_getattr_np(pthread_self(), &at) == 0) {
        void* sa = nullptr; size_t ss = 0;
        if (pthread_attr_getstack(&at, &sa, &ss) == 0) { stLo = (u64)(uintptr_t)sa; stHi = stLo + ss; }
        pthread_attr_destroy(&at);
    }
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
                for (size_t k = 0; k < wantX.size(); k++) {
                    if (w != (wantX[k] ^ kObf)) continue;
                    u64 addr = a + i;
                    if (addr >= bufLo && addr < bufHi) continue;
                    if (stHi && addr >= stLo && addr < stHi) continue;
                    if ((int)out[k].size() < kDumpMaxObjsPerClass) out[k].push_back(addr);
                }
            }
            if ((++chunks & 511) == 0) usleep(1500);
        }
    }
}

static void run_dump() {
    long t0 = now_ms();
    std::map<std::string, u64> byNameX;            // class name -> class address XOR kObf
    std::set<std::string> extra;
    {
        std::vector<u64> all = enum_classes();
        LOG("DUMP begin: %d classes found by structure scan in %ld ms (lib bias 0x%lx)", (int)all.size(), now_ms() - t0, (unsigned long)g_bias);
        if (all.empty()) { LOG("DUMP end: no class structs found (layout assumption failed)"); return; }
        int hits = 0;
        for (u64 c : all) {
            std::string cn = class_name_s(c);
            if (cn == "?") continue;
            byNameX[cn] = c ^ kObf;
            std::vector<IvInfo> iv = class_ivars_own(c, cn);
            for (const IvInfo& x : iv) {
                std::string l = lower_s(x.name);
                bool hit = false;
                for (const char* tk : kInterest) if (l.find(tk) != std::string::npos) { hit = true; break; }
                for (const char* ex : kExact) if (l == ex) { if (extra.size() < 12) extra.insert(cn); hit = true; }
                if (!hit) continue;
                if (hits < kDumpMaxHits) LOG("DUMP hit %s.%s type=%s off=%d", cn.c_str(), x.name.c_str(), x.type.c_str(), x.off);
                hits++;
            }
        }
        LOG("DUMP hits total %d (printed up to %d); classes owning friction/mass/restitution/speedFactor ivars: %d", hits, kDumpMaxHits, (int)extra.size());
        for (const std::string& e : extra) LOG("DUMP owner %s", e.c_str());
    }
    // list of classes to dump completely
    std::vector<std::string> names;
    for (const char* n : kDumpClasses) names.push_back(n);
    for (const std::string& e : extra) if (std::find(names.begin(), names.end(), e) == names.end()) names.push_back(e);
    g_dc.clear();
    for (const std::string& nm : names) {
        auto it = byNameX.find(nm);
        if (it == byNameX.end()) { LOG("DUMP class %s NOT FOUND (not an ObjC class of this library, or a C++ class)", nm.c_str()); continue; }
        u64 c = it->second ^ kObf;
        u64 isz = 0, sup = 0;
        rd(c + 40, &isz, 8);
        sup = super_of(c);
        LOG("DUMP class %s size=%lu super=%s", nm.c_str(), (unsigned long)isz, sup ? class_name_s(sup).c_str() : "none");
        DumpCls d; d.name = nm; d.clsX = c ^ kObf; d.ivs = all_ivars(c);
        for (const IvInfo& iv : d.ivs) LOG("DUMP iv %s.%s type=%s off=%d", iv.cls.c_str(), iv.name.c_str(), iv.type.c_str(), iv.off);
        dump_methods(c, nm);
        g_dc.push_back(d);
    }
    byNameX.clear();
    // live instances
    std::vector<u64> wantX;
    for (const DumpCls& d : g_dc) wantX.push_back(d.clsX);
    std::vector<std::vector<u64>> hitsPer;
    long s0 = now_ms();
    scan_heap_x(wantX, hitsPer);
    LOG("DUMP heap scan %ld ms", now_ms() - s0);
    int total = 0;
    for (size_t k = 0; k < g_dc.size(); k++) {
        DumpCls& d = g_dc[k];
        int n = 0;
        for (u64 o : hitsPer[k]) {
            if (total >= kDumpMaxObjsTotal) break;
            DumpObj ob; ob.o = o;
            std::vector<std::string> items;
            for (const IvInfo& iv : d.ivs) {
                std::string full, num;
                if (fmt_value(o, iv, false, full)) items.push_back(iv.name + "=" + full);
                if (!fmt_value(o, iv, true, num)) num.clear();
                ob.idle.push_back(num);
            }
            char head[96];
            snprintf(head, sizeof(head), "DUMP obj %s@%lx", d.name.c_str(), (unsigned long)o);
            log_chunked(head, items);
            d.objs.push_back(ob);
            n++; total++;
        }
        LOG("DUMP class %s: %d live instances shown", d.name.c_str(), n);
    }
    g_dumpReady = true;
    LOG("DUMP end: %ld ms, %d objects; changed numeric ivars follow after the next shots (DUMP dyn)", now_ms() - t0, total);
}

// prints only numeric ivars whose value differs from the idle snapshot
static void dump_dyn(long dt, int shot) {
    int lines = 0;
    LOG("DUMP dyn shot=%d t=+%ldms", shot, dt);
    for (DumpCls& d : g_dc) {
        for (DumpObj& ob : d.objs) {
            if (lines >= 120) return;
            u64 isa;
            if (!rd(ob.o, &isa, 8) || (isa ^ kObf) != d.clsX) continue;       // object is gone
            std::vector<std::string> items;
            for (size_t i = 0; i < d.ivs.size() && i < ob.idle.size(); i++) {
                if (ob.idle[i].empty()) continue;
                std::string now;
                if (!fmt_value(ob.o, d.ivs[i], true, now)) continue;
                if (now != ob.idle[i]) items.push_back(d.ivs[i].name + "=" + ob.idle[i] + "->" + now);
            }
            if (items.empty()) continue;
            char head[112];
            snprintf(head, sizeof(head), "DUMP dyn s%d +%ld %s@%lx", shot, dt, d.name.c_str(), (unsigned long)ob.o);
            log_chunked(head, items);
            lines++;
        }
    }
}

static const long kDynDelay[3] = {60, 250, 600};   // ms after the release of the shot

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
    int dynShots = 0, dynStep = -1; long dynT0 = 0;

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
                // v5.2: one-time dump the first time a match is on the heap (coins + aim objects)
                if (kDumpEnabled && !g_dumpDone && count_valid(C_PUCK) > 0 && count_valid(C_LOCAL) > 0) {
                    g_dumpDone = true;
                    run_dump();
                }
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
                if (g_dumpReady && dynShots < 3) { dynShots++; dynStep = 0; dynT0 = now_ms(); }
                if (g_enabled.load()) {
                    char pl[900];
                    { std::lock_guard<std::mutex> lk(g_predMx); snprintf(pl, sizeof(pl), "%s", g_predLastLog); }
                    LOG("PRED last: %s", pl);
                }
            }
        }

        pred_tick(a, t);

        // v5.2: changed ivars of the dumped model objects at +60 / +250 / +600 ms after each of the first 3 shots
        if (dynStep >= 0) {
            long dt = now_ms() - dynT0;
            if (dt >= kDynDelay[dynStep]) {
                dump_dyn(dt, dynShots);
                dynStep++;
                if (dynStep >= 3) dynStep = -1;
            }
        }

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
    LOG("carprobe v5.2 loaded (prediction + dump mode %s; test write kTestMaxGuides=%d)", kDumpEnabled ? "ON" : "off", kTestMaxGuides);
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
