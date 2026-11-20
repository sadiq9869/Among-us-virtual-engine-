// fnprobe.cpp v2 - READ-ONLY "state object" finder (no calls, no hooks, no writes)
// Path: Bcore/src/main/cpp/carprobe/fnprobe.cpp   (replaces v1)
// Idea: the game's state constructor (orig RVA 0x1b2adc0) stores a vtable
// pointer = *(base+0x2848188)+0x10 at object+0x100, and similar ones at
// +0x60 +0x90 +0xc0 +0xf0 (slots 0x2848160..0x2848188). This probe reads those
// slot values, scans the heap for that signature and dumps the objects.
// Output lines start with "ST ". Log tag CARPROBE1. 64-bit only.
#if defined(__LP64__)
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#define TAG "CARPROBE1"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace {

const uint64_t kSlots[6] = {0x2848160, 0x2848168, 0x2848170, 0x2848178, 0x2848180, 0x2848188};
const uint64_t kSigOff[5] = {0x60, 0x90, 0xc0, 0xf0, 0x100};  // vptr-like words
const size_t kObjSize = 0x240;

struct Lib { uint64_t base; uint64_t end; char path[200]; };

int phdrCb(struct dl_phdr_info* info, size_t, void* data) {
    Lib* f = (Lib*)data;
    if (info->dlpi_name && strstr(info->dlpi_name, "libgame-CARROM")) {
        f->base = (uint64_t)info->dlpi_addr;
        uint64_t hi = 0;
        for (int i = 0; i < info->dlpi_phnum; i++)
            if (info->dlpi_phdr[i].p_type == PT_LOAD) {
                uint64_t e = info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz;
                if (e > hi) hi = e;
            }
        f->end = f->base + hi;
        strncpy(f->path, info->dlpi_name, sizeof(f->path) - 1);
        return 1;
    }
    return 0;
}

bool rd(int fd, uint64_t addr, void* buf, size_t n) {
    return pread(fd, buf, n, (off_t)addr) == (ssize_t)n;
}

bool skipRegion(const char* name) {
    static const char* bad[] = {"dalvik", "gfx", "ashmem", "dmabuf", "kgsl", "mali", "stack",
                                "art", "memfd", "/dev/", ".so", ".apk", ".oat", ".vdex", ".art", nullptr};
    for (int i = 0; bad[i]; i++) if (strstr(name, bad[i])) return true;
    return false;
}

std::vector<uint64_t> findHits(int fd, uint64_t target, const void* ownBuf, size_t ownLen) {
    std::vector<uint64_t> hits;
    FILE* m = fopen("/proc/self/maps", "r");
    if (!m) return hits;
    static char line[512];
    static uint64_t chunk[1 << 17];  // 1 MB
    while (fgets(line, sizeof(line), m)) {
        unsigned long long s, e; char perm[8]; unsigned long long off; char dev[16]; unsigned long ino;
        char name[400]; name[0] = 0;
        int n = sscanf(line, "%llx-%llx %7s %llx %15s %lu %399[^\n]", &s, &e, perm, &off, dev, &ino, name);
        if (n < 6) continue;
        if (perm[0] != 'r' || perm[1] != 'w' || perm[3] != 'p') continue;
        const char* nm = name; while (*nm == ' ') nm++;
        if (*nm && strcmp(nm, "[heap]") != 0 && strncmp(nm, "[anon:", 6) != 0) continue;
        if (*nm && skipRegion(nm)) continue;
        if (e - s > (900ull << 20)) continue;
        uint64_t own0 = (uint64_t)(uintptr_t)ownBuf, own1 = own0 + ownLen;
        if (own0 >= s && own0 < e) { /* region holds our own buffers: still scanned, hits inside are dropped */ }
        for (uint64_t a = s; a < e; a += sizeof(chunk)) {
            size_t len = (size_t)((e - a) < sizeof(chunk) ? (e - a) : sizeof(chunk));
            ssize_t r = pread(fd, chunk, len, (off_t)a);
            if (r <= 0) continue;
            size_t nw = (size_t)r / 8;
            for (size_t i = 0; i < nw; i++)
                if (chunk[i] == target) {
                    uint64_t at = a + i * 8;
                    if (at >= (uint64_t)(uintptr_t)chunk && at < (uint64_t)(uintptr_t)chunk + sizeof(chunk)) continue;
                    if (at >= own0 && at < own1) continue;
                    hits.push_back(at);
                }
        }
    }
    fclose(m);
    return hits;
}

void fmt(const Lib& L, int fd, uint64_t w, char* out, size_t n) {
    if (w == 0) { snprintf(out, n, "0"); return; }
    if (w >= L.base && w < L.end) { snprintf(out, n, "L+%llx", (unsigned long long)(w - L.base)); return; }
    double d; memcpy(&d, &w, 8);
    double ad = d < 0 ? -d : d;
    if (ad >= 1e-6 && ad <= 1e7) { snprintf(out, n, "d%.9g", d); return; }
    if (w > 0x7000000000ull && w < 0x8000000000ull) { snprintf(out, n, "H%llx", (unsigned long long)w); return; }
    if (w < 0x100000000ull) {
        uint32_t lo = (uint32_t)w; float f; memcpy(&f, &lo, 4);
        snprintf(out, n, "i%u", lo); return;
    }
    uint32_t a = (uint32_t)w, b = (uint32_t)(w >> 32); float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
    float afa = fa < 0 ? -fa : fa, afb = fb < 0 ? -fb : fb;
    if (afa >= 1e-4f && afa <= 1e6f && afb >= 1e-4f && afb <= 1e6f) { snprintf(out, n, "f%g,%g", fa, fb); return; }
    snprintf(out, n, "x%llx", (unsigned long long)w);
}

void dumpObj(const Lib& L, int fd, uint64_t B, int idx) {
    uint64_t buf[kObjSize / 8];
    if (!rd(fd, B, buf, sizeof(buf))) { LOGI("ST obj%d @%llx READFAIL", idx, (unsigned long long)B); return; }
    char t[3][40];
    for (size_t i = 0; i < kObjSize / 8; i += 3) {
        for (int k = 0; k < 3; k++) {
            if (i + k < kObjSize / 8) fmt(L, fd, buf[i + k], t[k], sizeof(t[k])); else t[k][0] = 0;
        }
        LOGI("ST obj%d +%03zx: %s | %s | %s", idx, i * 8, t[0], t[1], t[2]);
    }
}

void scanOnce(int n) {
    Lib L; memset(&L, 0, sizeof(L));
    dl_iterate_phdr(phdrCb, &L);
    if (!L.base) { LOGI("ST scan %d: game lib not found", n); return; }
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { LOGI("ST cannot open /proc/self/mem"); return; }
    uint64_t V[6]; bool ok = true;
    for (int i = 0; i < 6; i++) {
        uint64_t s = 0;
        if (!rd(fd, L.base + kSlots[i], &s, 8) || s == 0) { ok = false; }
        V[i] = s + 0x10;
    }
    LOGI("ST scan %d: base=0x%llx slots=%s V=%llx %llx %llx %llx %llx %llx", n, (unsigned long long)L.base,
         ok ? "ok" : "BAD", (unsigned long long)V[0], (unsigned long long)V[1], (unsigned long long)V[2],
         (unsigned long long)V[3], (unsigned long long)V[4], (unsigned long long)V[5]);
    if (!ok) { close(fd); return; }
    std::vector<uint64_t> hits = findHits(fd, V[5], nullptr, 0);
    LOGI("ST scan %d: %zu words equal V5 (object+0x100)", n, hits.size());
    struct Cand { uint64_t B; int score; };
    std::vector<Cand> c;
    for (uint64_t h : hits) {
        if (h < 0x100) continue;
        uint64_t B = h - 0x100; int score = 1;
        for (int k = 0; k < 4; k++) {
            uint64_t w = 0;
            if (!rd(fd, B + kSigOff[k], &w, 8)) continue;
            for (int j = 0; j < 6; j++) if (w == V[j]) { score++; break; }
        }
        c.push_back({B, score});
    }
    int shown = 0;
    for (int want = 5; want >= 3 && shown < 4; want--)
        for (auto& x : c) {
            if (x.score != want || shown >= 4) continue;
            LOGI("ST scan %d: object @%llx score=%d/5", n, (unsigned long long)x.B, x.score);
            dumpObj(L, fd, x.B, shown);
            shown++;
        }
    LOGI("ST scan %d: done, candidates=%zu shown=%d", n, c.size(), shown);
    close(fd);
}

void* worker(void*) {
    sleep(50);
    for (int n = 1; n <= 5; n++) { scanOnce(n); sleep(40); }
    return nullptr;
}

__attribute__((constructor)) void fnprobeInit() {
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
}

}  // namespace
#endif
