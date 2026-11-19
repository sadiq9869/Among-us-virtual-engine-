// cfgscan.cpp  v2 (v1 matched pointers read as tiny numbers; v2 rejects them and no longer needs 'nice' decimals)  (NEW FILE, separate from carprobe.cpp - it does not touch the prediction code)
//
// WHAT IT DOES: reads the game's OWN physics configuration out of the game's memory (READ-ONLY: no hooks, no writes, no patches).
// WHY: the game does not keep friction / restitution / mass in its code. Its config loader (build 19.6.1, versionCode 1492,
//      found by disassembling libgame-CARROM-...-1492.so) parses a config (key -> double) into structs in the heap:
//        gameplayGeneralConfig  : 7 doubles in a row
//             +0x00 strikerInputBarWidth  +0x08 puckMaxPickingDistance  +0x10 controlsMinDistanceShot  +0x18 controlsMaxDistanceShot
//             +0x20 physicsSpeedFactor    +0x28 maxPower                +0x30 minPower
//        table properties       : 6 doubles in a row (starting at struct +0x20)
//             friction, restitution, pocketRadius, ballInPocketSpeedFactor, puckInPocketSpeedFactor, pocketLogicalRadiusFactor
//        ballPropertiesList     : one record per ball id, int id at struct +0x18, then 5 doubles from +0x20:
//             radius, friction, restitution, mass, spinFriction      (mass / spinFriction order = order of the key reads in the loader)
//      This file looks for exactly those patterns in the writable heap and prints what it finds.
// HOW: pread() on /proc/self/mem (a bad page gives an error, not a crash). A candidate must be "nice" numbers (a double that is
//      a short decimal like 0.17 or 13.1, or the double of such a float) and inside plausible ranges, so random memory almost never matches.
// OUTPUT: logcat, tag CARPROBE1, lines starting with "CFG". The summary lines are "CFG sum ...".
//      adb logcat -d -v raw -s CARPROBE1:V | grep "^CFG sum" | head -80
// NOT built for Android by an AI, NOT run in the game. Host test (g++ on a PC) passed, see the report.
// These struct layouts are valid for build 1492 ONLY (they come from that build's loader code); after a game update re-check.

#include <android/log.h>
#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

namespace cfgscan {

static const char* kTag = "CARPROBE1";
static const long  kFirstDelayMs = 40000;     // first scan this long after the process starts
static const long  kPeriodMs     = 60000;     // then every minute
static const int   kMaxScans     = 12;        // 12 scans, then the thread ends
static const size_t CH           = 1u << 20;  // read chunk 1 MB
static const size_t kMaxRegion   = 900u << 20; // skip regions bigger than this
static const int   kMaxSumLines  = 60;

static void L(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void L(const char* fmt, ...) {
    char b[900];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", b);
}

static long now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }

// "nice" = a short decimal number (<= 5 decimals) or the double value of the float closest to such a number
[[maybe_unused]] static bool nice(double d) {
    if (d == 0.0) return true;
    if (!std::isfinite(d)) return false;
    static const double sc[] = {1e2, 1e3, 1e4, 1e5};
    for (double s : sc) {
        double a = std::nearbyint(d * s) / s;
        if (std::fabs(d - a) < 1e-9) return true;
        if (std::fabs(d - (double)(float)a) < 1e-12) return true;
    }
    return false;
}
static inline bool in(double v, double lo, double hi) { return v >= lo && v <= hi; }

struct Region { uint64_t s, e; };

static std::vector<Region> regions() {
    std::vector<Region> out;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return out;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        unsigned long long s = 0, e = 0, off = 0;
        char perms[8] = "";
        int n = 0;
        if (sscanf(line, "%llx-%llx %7s %llx %*s %*s %n", &s, &e, perms, &off, &n) < 4) continue;
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p') continue;
        const char* p = line + n;
        while (*p == ' ') p++;
        std::string path(p);
        while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
        bool ok = path.empty() || path == "[heap]" || path.compare(0, 6, "[anon:") == 0;
        if (!ok) continue;
        static const char* bad[] = {"dalvik", "gfx", "ashmem", "dmabuf", "kgsl", "mali", "stack", "vvar", "memfd", "art", "linker_alloc", nullptr};
        for (int i = 0; bad[i]; i++) if (path.find(bad[i]) != std::string::npos) ok = false;
        if (!ok) continue;
        if (e - s < 4096 || e - s > kMaxRegion) continue;
        out.push_back(Region{(uint64_t)s, (uint64_t)e});
    }
    fclose(f);
    return out;
}

struct Hit { uint64_t addr; std::string key; };
static std::map<std::string, int> g_cnt[3];            // summary per type: key text -> number of copies
static std::map<std::string, uint64_t> g_first[3];     // first address of each key

static void add_hit(int type, uint64_t addr, const char* key) {
    std::string k(key);
    g_cnt[type][k]++;
    if (!g_first[type].count(k)) g_first[type][k] = addr;
}

static int g_ctxLeft[3] = {0, 0, 0};
static void ctx_print(int type, uint64_t addr, const uint8_t* p, bool ok) {
    if (!ok || g_ctxLeft[type] <= 0) return;
    g_ctxLeft[type]--;
    static const char* nm[3] = {"ball", "table", "general"};
    char b[800]; int n = snprintf(b, sizeof b, "CFG ctx %s @%llx doubles from -8 to +11 (index 0 = record start):", nm[type], (unsigned long long)addr);
    for (int k = -8; k < 12 && n < 760; k++) { double v; memcpy(&v, p + k * 8, 8); n += snprintf(b + n, sizeof b - n, " %.9g", v); }
    L("%s", b);
}
// a value that is either exactly 0 or a normal-sized number (rejects denormals = pointers read as doubles)
static inline bool z_or(double v, double hi) { return v == 0.0 || (v >= 1e-3 && v <= hi); }

static void check(uint64_t addr, const double* d, int32_t idBefore, const uint8_t* p, bool ctxOk) {
    // ball: radius friction restitution mass spinFriction, int id before
    {
        double r = d[0], fr = d[1], re = d[2], m = d[3], sp = d[4];
        if (in(r, 5, 40) && z_or(fr, 5) && z_or(re, 3) && in(m, 0.01, 1000) && z_or(sp, 10) && idBefore >= 0 && idBefore <= 10000) {
            int nz = (fr != 0) + (re != 0) + (sp != 0);
            if (nz >= 2) {
                char k[300];
                snprintf(k, sizeof k, "id=%d radius=%.10g friction=%.10g restitution=%.10g mass=%.10g spinFriction=%.10g", idBefore, r, fr, re, m, sp);
                add_hit(0, addr, k); ctx_print(0, addr, p, ctxOk);
            }
        }
    }
    // table: friction restitution pocketRadius ballInPocketSpeedFactor puckInPocketSpeedFactor pocketLogicalRadiusFactor
    {
        double fr = d[0], re = d[1], pr = d[2], bi = d[3], pi = d[4], lf = d[5];
        if (z_or(fr, 5) && z_or(re, 3) && in(pr, 5, 100) && z_or(bi, 50) && z_or(pi, 50) && z_or(lf, 10)) {
            int nz = (re != 0) + (bi != 0) + (pi != 0) + (lf != 0);
            if (nz >= 3) {
                char k[300];
                snprintf(k, sizeof k, "friction=%.10g restitution=%.10g pocketRadius=%.10g ballInPocketSpeedFactor=%.10g puckInPocketSpeedFactor=%.10g pocketLogicalRadiusFactor=%.10g", fr, re, pr, bi, pi, lf);
                add_hit(1, addr, k); ctx_print(1, addr, p, ctxOk);
            }
        }
    }
    // general: strikerInputBarWidth puckMaxPickingDistance controlsMinDistanceShot controlsMaxDistanceShot physicsSpeedFactor maxPower minPower
    {
        double bw = d[0], pk = d[1], mn = d[2], mx = d[3], ps = d[4], mxp = d[5], mnp = d[6];
        if (in(bw, 1, 2000) && in(pk, 1, 500) && in(mn, 1, 300) && in(mx, 1, 2000) && mx > mn && in(ps, 0.001, 100) && in(mxp, 0.1, 1000) && z_or(mnp, 1000)) {
            char k[300];
            snprintf(k, sizeof k, "strikerInputBarWidth=%.10g puckMaxPickingDistance=%.10g controlsMinDistanceShot=%.10g controlsMaxDistanceShot=%.10g physicsSpeedFactor=%.10g maxPower=%.10g minPower=%.10g", bw, pk, mn, mx, ps, mxp, mnp);
            add_hit(2, addr, k); ctx_print(2, addr, p, ctxOk);
        }
    }
}

static int g_scanNo = 0;

static void scan_once(int memfd) {
    long t0 = now_ms();
    for (int t = 0; t < 3; t++) { g_cnt[t].clear(); g_first[t].clear(); g_ctxLeft[t] = 2; }
    std::vector<Region> R = regions();
    uint64_t bytes = 0;
    static uint8_t* buf = nullptr;
    if (!buf) buf = (uint8_t*)malloc(CH + 256);
    if (!buf) return;
    for (const Region& rg : R) {
        for (uint64_t off = rg.s; off < rg.e; off += CH) {
            uint64_t base = off > rg.s ? off - 8 : off;        // 8 bytes of overlap so the int id before a record is in the buffer
            size_t want = (size_t)std::min<uint64_t>(CH + 8 + 64, rg.e - base);
            ssize_t got = pread(memfd, buf, want, (off_t)base);
            if (got <= 0) continue;
            bytes += got;
            size_t lim = (size_t)got >= 64 ? (size_t)got - 64 : 0;
            size_t endPos = std::min<size_t>(lim, CH + 8);
            for (size_t i = 8; i <= endPos && i + 56 <= (size_t)got; i += 8) {
                double a; memcpy(&a, buf + i, 8);
                // cheap prefilter on the first double of every pattern (radius 3..60 / friction 0..5 / barWidth 1..2000)
                if (!(a >= 0 && a <= 2000)) continue;
                double d[7];
                memcpy(d, buf + i, 56);
                int32_t idb; memcpy(&idb, buf + i - 8, 4);
                check(base + i, d, idb, buf + i, i >= 72 && i + 96 <= (size_t)got);
            }
            if ((off >> 20) % 16 == 0) usleep(1000);
        }
    }
    long ms = now_ms() - t0;
    L("CFG scan %d: regions=%zu MB=%llu ms=%ld  ball=%zu kinds table=%zu kinds general=%zu kinds", g_scanNo, R.size(), (unsigned long long)(bytes >> 20), ms,
      g_cnt[0].size(), g_cnt[1].size(), g_cnt[2].size());
    static const char* nm[3] = {"ball", "table", "general"};
    for (int t = 0; t < 3; t++) {
        int n = 0;
        for (auto& kv : g_cnt[t]) {
            if (n++ >= kMaxSumLines) break;
            L("CFG sum %s x%d @%llx %s", nm[t], kv.second, (unsigned long long)g_first[t][kv.first], kv.first.c_str());
        }
    }
}

static void* worker(void*) {
    usleep((useconds_t)kFirstDelayMs * 1000);
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { L("CFG cannot open /proc/self/mem"); return nullptr; }
    L("CFG cfgscan v2 started (read-only), %d scans planned", kMaxScans);
    for (g_scanNo = 1; g_scanNo <= kMaxScans; g_scanNo++) {
        scan_once(fd);
        usleep((useconds_t)kPeriodMs * 1000);
    }
    close(fd);
    return nullptr;
}

} // namespace cfgscan

#ifndef CFG_HOST_TEST
__attribute__((constructor)) static void cfgscan_start() {
    pthread_t th;
    if (pthread_create(&th, nullptr, cfgscan::worker, nullptr) == 0) pthread_detach(th);
}
#endif
