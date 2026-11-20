// fnprobe.cpp v1 - READ-ONLY address check (no calls, no hooks, no writes)
// Path: Bcore/src/main/cpp/carprobe/fnprobe.cpp
// Question it answers: at (game lib base + RVA) is the SAME code that the
// original libgame-CARROM-...-1492.so has? Output lines start with "FN ".
// Log tag: CARPROBE1. 64-bit only.
#if defined(__LP64__)
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TAG "CARPROBE1"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace {

struct Fn { uint64_t rva; uint32_t w[8]; };

// RVAs and first 8 instruction words taken from the ORIGINAL 1492 lib.
const Fn kFns[] = {
    {0x1b2bebc, {0xd100c3ffu, 0xa9017bfdu, 0xf90013f3u, 0x910043fdu, 0xd53bd053u, 0xf9401668u, 0xf90007e8u, 0xf940b808u}},
    {0x1b2d764, {0xfc1c0feau, 0x6d00a3e9u, 0xa901fbfdu, 0xf90017f5u, 0xa9034ff4u, 0x910063fdu, 0x1e604028u, 0x1e604009u}},
    {0x1b2adc0, {0xa9bc7bfdu, 0xa9015ff8u, 0xa90257f6u, 0xa9034ff4u, 0x910003fdu, 0xd00068e8u, 0xd00068e9u, 0xaa0003f4u}},
    {0x18bf3ac, {0xa9bc7bfdu, 0xa9015ff8u, 0xa90257f6u, 0xa9034ff4u, 0x910003fdu, 0xa9402029u, 0xaa0103f4u, 0xaa0003f3u}},
    {0x1b34930, {0x6dbe23e9u, 0xa9017bfdu, 0x910043fdu, 0xf941f800u, 0x2a0203e1u, 0x1e604028u, 0x1e604009u, 0x97ffdd5cu}},
    {0x1b35a54, {0xd10143ffu, 0xa9027bfdu, 0xf9001bf5u, 0xa9044ff4u, 0x910083fdu, 0xd53bd054u, 0x91152000u, 0xd10033a1u}},
    {0x2265840, {0xd10443ffu, 0xfd006be8u, 0xa90dfbfdu, 0xf90077fcu, 0xa90f57f6u, 0xa9104ff4u, 0x910363fdu, 0xad0007e0u}},
    {0x1b2ded4, {0xd102c3ffu, 0x6d023befu, 0x6d0333edu, 0x6d042bebu, 0x6d0523e9u, 0xa9067bfdu, 0xa90767fau, 0xa9085ff8u}},
    {0x1b2e548, {0xd10503ffu, 0x6d0a3befu, 0x6d0b33edu, 0x6d0c2bebu, 0x6d0d23e9u, 0xa90e7bfdu, 0xa90f6ffcu, 0xa91067fau}},
    {0x1b2ebf0, {0xf94003f6u, 0x1e60418au, 0xb5fffcc0u, 0x17ffffe7u, 0xf94003e8u, 0xf9401508u, 0xf85b03a9u, 0xeb09011fu}},
    {0x1b2d868, {0xd102c3ffu, 0xfd0033e8u, 0xa9077bfdu, 0xa9085ff8u, 0xa90957f6u, 0xa90a4ff4u, 0x9101c3fdu, 0xd53bd056u}},
};

struct Found { uint64_t base; char path[256]; };

int phdrCb(struct dl_phdr_info* info, size_t, void* data) {
    Found* f = (Found*)data;
    if (info->dlpi_name && strstr(info->dlpi_name, "libgame-CARROM")) {
        f->base = (uint64_t)info->dlpi_addr;
        strncpy(f->path, info->dlpi_name, sizeof(f->path) - 1);
        return 1;
    }
    return 0;
}

void scanOnce(int n) {
    Found f; memset(&f, 0, sizeof(f));
    dl_iterate_phdr(phdrCb, &f);
    if (!f.base) { LOGI("FN scan %d: game lib not found", n); return; }
    LOGI("FN scan %d: lib=%s base=0x%llx", n, f.path, (unsigned long long)f.base);
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { LOGI("FN cannot open /proc/self/mem"); return; }
    int match = 0, total = 0;
    for (const Fn& fn : kFns) {
        uint32_t got[8]; memset(got, 0, sizeof(got));
        ssize_t r = pread(fd, got, sizeof(got), (off_t)(f.base + fn.rva));
        total++;
        if (r != (ssize_t)sizeof(got)) { LOGI("FN rva=0x%llx READFAIL", (unsigned long long)fn.rva); continue; }
        int same = 0;
        for (int i = 0; i < 8; i++) if (got[i] == fn.w[i]) same++;
        if (same == 8) match++;
        LOGI("FN rva=0x%llx %s same=%d/8 got=%08x %08x %08x %08x", (unsigned long long)fn.rva,
             same == 8 ? "MATCH" : "DIFF", same, got[0], got[1], got[2], got[3]);
    }
    LOGI("FN summary scan %d: %d of %d functions match", n, match, total);
    close(fd);
}

void* worker(void*) {
    sleep(50);
    for (int n = 1; n <= 4; n++) { scanOnce(n); sleep(60); }
    return nullptr;
}

__attribute__((constructor)) void fnprobeInit() {
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
}

}  // namespace
#endif
