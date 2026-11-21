// evline.cpp v2 - READ-ONLY finder for a "polyline of (x,y) doubles" in the heap
// Path: Bcore/src/main/cpp/carprobe/evline.cpp
// Why: the other trajectory mod running in the same game process keeps the line it
// draws as a list of (x, y) double pairs (table coordinates). This probe only READS
// memory (pread on /proc/self/mem), finds such lists, logs their key numbers and
// dumps the best ones. It does not call, hook or write anything. It never looks at
// any key / login / protection code.
// Output lines contain "EVL". Log tag CARPROBE1. 64-bit only.
#if defined(__LP64__)
#include <android/log.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#define TAG "CARPROBE1"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace {

const int    kMinPts   = 8;        // shortest list that counts
const double kMaxAbs   = 40.0;     // the lists are in cm (board half = 37), so stay inside this
const double kMinStep  = 0.02;     // distance between two neighbouring points
const double kMaxStep  = 4.0;       // a ball moves at most ~2.8 cm per step
const double kMinLen   = 10.0;     // whole polyline (cm)
const double kMinBox   = 5.0;      // spread of the polyline (cm)
const int    kScans    = 80;
const int    kPeriodS  = 8;
const int    kDumpMax  = 400;      // points per dump

uint64_t gStackLo = 0, gStackHi = 0;
uint64_t chunkBuf[1 << 17];        // 1 MB read buffer (hits inside it are ignored)
double   dumpBuf[kDumpMax * 2];    // dump copy lives here, not on the heap (hits inside it are ignored)

struct Run { uint64_t addr; int n; double len, x0, x1, y0, y1; double p0x, p0y, p1x, p1y, pnx, pny; };

inline bool okv(double v) {
    if (!(v == v)) return false;
    double a = v < 0 ? -v : v;
    if (a > kMaxAbs) return false;
    return a == 0.0 || a >= 1e-3;
}

long nowMs() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }

bool skipRegion(const char* name) {
    static const char* bad[] = {"dalvik", "gfx", "ashmem", "dmabuf", "kgsl", "mali", "stack",
                                "art", "memfd", "/dev/", ".so", ".apk", ".oat", ".vdex", ".art", nullptr};
    for (int i = 0; bad[i]; i++) if (strstr(name, bad[i])) return true;
    return false;
}

void closeRun(std::map<uint64_t, Run>& out, uint64_t startAddr, int n, double len, double x0, double x1, double y0, double y1,
              double p0x, double p0y, double p1x, double p1y, double pnx, double pny) {
    if (n < kMinPts || len < kMinLen) return;
    if ((x1 - x0) < kMinBox && (y1 - y0) < kMinBox) return;
    if (startAddr >= (uint64_t)(uintptr_t)chunkBuf && startAddr < (uint64_t)(uintptr_t)chunkBuf + sizeof(chunkBuf)) return;
    if (startAddr >= (uint64_t)(uintptr_t)dumpBuf && startAddr < (uint64_t)(uintptr_t)dumpBuf + sizeof(dumpBuf)) return;
    if (startAddr >= gStackLo && startAddr < gStackHi) return;
    Run r; r.addr = startAddr; r.n = n; r.len = len; r.x0 = x0; r.x1 = x1; r.y0 = y0; r.y1 = y1;
    r.p0x = p0x; r.p0y = p0y; r.p1x = p1x; r.p1y = p1y; r.pnx = pnx; r.pny = pny;
    auto it = out.find(startAddr);
    if (it == out.end() || it->second.n < n) out[startAddr] = r;
}

// one pass over a buffer of doubles, pairs start at word index 'parity'
void scanWords(const double* w, size_t nw, uint64_t baseAddr, int parity, std::map<uint64_t, Run>& out) {
    bool in = false;
    int n = 0; uint64_t start = 0; double len = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0, px = 0, py = 0;
    double p0x = 0, p0y = 0, p1x = 0, p1y = 0;
    for (size_t j = (size_t)parity; j + 1 < nw; j += 2) {
        double x = w[j], y = w[j + 1];
        bool v = okv(x) && okv(y);
        if (v && in) {
            double dx = x - px, dy = y - py, d = sqrt(dx * dx + dy * dy);
            if (d < kMinStep || d > kMaxStep) v = false;
            else {
                n++; len += d;
                if (x < x0) { x0 = x; }
                if (x > x1) { x1 = x; }
                if (y < y0) { y0 = y; }
                if (y > y1) { y1 = y; }
                if (n == 2) { p1x = x; p1y = y; }
                px = x; py = y;
                continue;
            }
        }
        if (in) { closeRun(out, start, n, len, x0, x1, y0, y1, p0x, p0y, p1x, p1y, px, py); in = false; }
        if (v) {  // start a new run here
            in = true; n = 1; len = 0; start = baseAddr + j * 8;
            x0 = x1 = x; y0 = y1 = y; px = x; py = y; p0x = x; p0y = y; p1x = x; p1y = y;
        }
    }
    if (in) closeRun(out, start, n, len, x0, x1, y0, y1, p0x, p0y, p1x, p1y, px, py);
}

void scanHeap(int fd, std::map<uint64_t, Run>& out, size_t* regions, uint64_t* mb) {
    FILE* m = fopen("/proc/self/maps", "r");
    if (!m) return;
    static char line[512];
    const size_t kOverlap = 4096;
    while (fgets(line, sizeof(line), m)) {
        unsigned long long s, e; char perm[8]; unsigned long long off; char dev[16]; unsigned long ino; char name[400]; name[0] = 0;
        int n = sscanf(line, "%llx-%llx %7s %llx %15s %lu %399[^\n]", &s, &e, perm, &off, dev, &ino, name);
        if (n < 6) continue;
        if (perm[0] != 'r' || perm[1] != 'w' || perm[3] != 'p') continue;
        const char* nm = name; while (*nm == ' ') nm++;
        if (*nm && strcmp(nm, "[heap]") != 0 && strncmp(nm, "[anon:", 6) != 0) continue;
        if (*nm && skipRegion(nm)) continue;
        if (e - s > (900ull << 20)) continue;
        (*regions)++; *mb += (e - s) >> 20;
        for (uint64_t a = s; a < e; a += sizeof(chunkBuf) - kOverlap) {
            size_t len = (size_t)((e - a) < sizeof(chunkBuf) ? (e - a) : sizeof(chunkBuf));
            ssize_t r = pread(fd, chunkBuf, len, (off_t)a);
            if (r < 64) continue;
            size_t nw = (size_t)r / 8;
            const double* w = (const double*)chunkBuf;
            scanWords(w, nw, a, 0, out);
            scanWords(w, nw, a, 1, out);
            if (a + len >= e) break;
        }
    }
    fclose(m);
}

bool run_more(const Run& a, const Run& b) { return a.n > b.n; }

std::set<std::string> gSeen;   // first 3 points of every list already printed

void dumpRun(int fd, const Run& r, int idx) {
    int n = r.n < kDumpMax ? r.n : kDumpMax;
    double* buf = dumpBuf;
    if (pread(fd, buf, (size_t)n * 16, (off_t)r.addr) != (ssize_t)((size_t)n * 16)) { LOGI("EVL dump%d @%llx READFAIL", idx, (unsigned long long)r.addr); return; }
    std::string cur; char t[64]; int part = 0;
    for (int i = 0; i < n; i++) {
        snprintf(t, sizeof(t), " %.3f,%.3f", buf[(size_t)i * 2], buf[(size_t)i * 2 + 1]);
        cur += t;
        if (cur.size() > 700 || i == n - 1) { LOGI("EVL dump%d @%llx part%d:%s", idx, (unsigned long long)r.addr, part++, cur.c_str()); cur.clear(); }
    }
}

void scanOnce(int n) {
    {
        pthread_attr_t at;
        if (pthread_getattr_np(pthread_self(), &at) == 0) {
            void* sa = nullptr; size_t ss = 0;
            if (pthread_attr_getstack(&at, &sa, &ss) == 0) { gStackLo = (uint64_t)(uintptr_t)sa; gStackHi = gStackLo + ss; }
            pthread_attr_destroy(&at);
        }
    }
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { LOGI("EVL cannot open /proc/self/mem"); return; }
    long t0 = nowMs(); size_t regions = 0; uint64_t mb = 0;
    std::map<uint64_t, Run> runs;
    scanHeap(fd, runs, &regions, &mb);
    std::vector<Run> v;
    for (auto& kv : runs) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), run_more);
    // drop a list when a neighbour list 8 bytes away (the other pairing) is clearly longer
    std::vector<Run> keep;
    for (auto& r : v) {
        bool shadow = false;
        auto a1 = runs.find(r.addr - 8), a2 = runs.find(r.addr + 8);
        if (a1 != runs.end() && a1->second.n > r.n + 1) shadow = true;
        if (a2 != runs.end() && a2->second.n > r.n + 1) shadow = true;
        if (!shadow) keep.push_back(r);
    }
    int fresh = 0;
    std::vector<Run> todo;
    for (auto& r : keep) {
        double buf3[6];
        if (pread(fd, buf3, sizeof(buf3), (off_t)r.addr) != (ssize_t)sizeof(buf3)) continue;
        char sg[160]; snprintf(sg, sizeof(sg), "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", buf3[0], buf3[1], buf3[2], buf3[3], buf3[4], buf3[5]);
        if (gSeen.insert(sg).second) { fresh++; if (todo.size() < 8) todo.push_back(r); }
    }
    LOGI("EVL scan %d: regions=%zu MB=%llu ms=%ld lists=%zu kept=%zu new=%d", n, regions, (unsigned long long)mb, nowMs() - t0, v.size(), keep.size(), fresh);
    for (size_t i = 0; i < todo.size(); i++) {
        const Run& r = todo[i];
        LOGI("EVL new%zu @%llx n=%d len=%.1f p0=(%.3f,%.3f) p1=(%.3f,%.3f) last=(%.3f,%.3f)", i, (unsigned long long)r.addr, r.n, r.len,
             r.p0x, r.p0y, r.p1x, r.p1y, r.pnx, r.pny);
        dumpRun(fd, r, (int)i);
    }
    close(fd);
}

void* worker(void*) {
    sleep(45);
    for (int n = 1; n <= kScans; n++) { scanOnce(n); sleep(kPeriodS); }
    LOGI("EVL finished");
    return nullptr;
}

__attribute__((constructor)) void evlineInit() {
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
}

}  // namespace
#endif
