// carprobe.cpp  v2 (v1 + garbage filter for freed objects, _controlsLogic link, shows every valid object)  -  READ-ONLY probe for Carrom Pool (com.miniclip.carrom).
// NO hooks, NO patching, NO writes into the game. It only READS memory (through a pipe, so a bad
// address gives an error code instead of a crash) and prints values to logcat, tag CARPROBE1.
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

#define TAG "CARPROBE1"
#define LOG(...) do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); usleep(1200); } while (0)

// ------------------------------------------------------------------ config
static const char* kLibPrefix   = "libgame-CARROM";   // name prefix of the game's own library
static const long  kStartDelayMs = 20000;              // wait after the library appears
static const long  kLifeMs       = 30L * 60 * 1000;    // probe stops itself after 30 min
static const long  kTickMs       = 500;                // value read interval
static const long  kScanWaitMs   = 6000;               // heap rescan interval while objects are missing
static const long  kScanSlowMs   = 25000;              // heap rescan interval when everything is found
static const int   kMaxPerClass  = 200;                // max heap hits kept per class
static const size_t CH = 16384;                        // read chunk

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
enum { C_LOCAL, C_REMOTE, C_FG, C_STRIKER, C_PUCK, C_COUNT };
static const char* kClassNames[C_COUNT] = {
    "ControlsLogicLocal", "ControlsLogicRemote", "ControlsForegroundNode", "StrikerNode", "PuckNode"
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

    long start = now_ms(), lastScan = 0, lastBeat = 0;
    std::string last;
    while (now_ms() - start < kLifeMs) {
        long t = now_ms();
        bool haveCore = !g_objs[C_LOCAL].empty() && !g_objs[C_FG].empty();
        long wait = haveCore ? kScanSlowMs : kScanWaitMs;
        if (t - lastScan > wait) {
            long s0 = now_ms();
            scan_heap();
            lastScan = now_ms();
            LOG("heap scan %ld ms: LOCAL=%d REMOTE=%d GUIDE=%d STRIKER=%d PUCK=%d", lastScan - s0,
                count_valid(C_LOCAL), count_valid(C_REMOTE), count_valid(C_FG), count_valid(C_STRIKER), count_valid(C_PUCK));
        }
        // drop freed objects
        for (int c = 0; c < C_COUNT; c++) {
            std::vector<u64> keep;
            for (u64 o : g_objs[c]) if (still_valid(c, o)) keep.push_back(o);
            g_objs[c].swap(keep);
        }
        std::string s = snapshot();
        if (s != last || t - lastBeat > 10000) {
            log_lines(s);
            last = s; lastBeat = t;
        }
        usleep(kTickMs * 1000);
    }
    LOG("probe finished (30 min)");
    return nullptr;
}

__attribute__((constructor)) static void init() {
    LOG("carprobe v2 loaded");
    pthread_t th;
    pthread_create(&th, nullptr, worker, nullptr);
    pthread_detach(th);
}
