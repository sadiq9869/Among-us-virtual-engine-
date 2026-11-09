// MENUTEST v6: diagnostic only (read-only). No hooks, no patches, nothing is changed in the game.
// Change vs v5: NO il2cpp call at all until kStartDelayMs after the watcher starts. The v5 log showed that
// il2cpp_domain_get returns non-null while Unity is still inside il2cpp_init, and that the game then aborted
// inside il2cpp_init. A non-null domain is therefore NOT a readiness signal.
// Change vs v4: every il2cpp call runs inside a SIGSEGV/SIGBUS guard (sigsetjmp/siglongjmp), so a call
// made before il2cpp is initialised does not kill the game; it is logged and retried.
// Steps: wait for libil2cpp.so -> resolve il2cpp API with xdl -> probe il2cpp domain (guarded, retry)
// -> find Assembly-CSharp (guarded) -> attach thread -> look up key classes BY NAME (guarded).
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include "xdl.h"

#define TAG "MENUTEST"
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); usleep(1500); } while (0)

static long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// ---------------- crash guard ----------------
static sigjmp_buf g_jmp;
static volatile pid_t g_guardTid = 0;
static struct sigaction g_oldSegv, g_oldBus;
static const char *volatile g_step = "start";

static pid_t myTid() { return (pid_t)syscall(SYS_gettid); }

static void guardHandler(int sig, siginfo_t *info, void *ctx) {
    if (g_guardTid != 0 && g_guardTid == myTid()) {
        siglongjmp(g_jmp, 1);
    }
    struct sigaction *old = (sig == SIGSEGV) ? &g_oldSegv : &g_oldBus;
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
        old->sa_sigaction(sig, info, ctx);
    } else if (old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN) {
        sigaction(sig, old, nullptr); // returning re-faults into the previous (default) action
    } else {
        old->sa_handler(sig);
    }
}

// Runs fn(arg). Returns true if it finished, false if it faulted (SIGSEGV/SIGBUS on this thread).
// The guard is installed right before the call and removed right after, so we are the top handler only
// for the duration of the call; faults on other threads are chained to the previous handler.
static bool guarded(void (*fn)(void *), void *arg) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = guardHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_oldSegv);
    sigaction(SIGBUS, &sa, &g_oldBus);
    volatile bool ok = false;
    g_guardTid = myTid();
    if (sigsetjmp(g_jmp, 1) == 0) {
        fn(arg);
        ok = true;
    }
    g_guardTid = 0;
    sigaction(SIGSEGV, &g_oldSegv, nullptr);
    sigaction(SIGBUS, &g_oldBus, nullptr);
    return ok;
}

static bool findInMaps(const char *needle, unsigned long *base, char *path, size_t pathLen) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, needle)) continue;
        unsigned long start = 0;
        sscanf(line, "%lx-", &start);
        char *slash = strchr(line, '/');
        if (slash) {
            size_t n = strlen(slash);
            while (n > 0 && (slash[n - 1] == '\n' || slash[n - 1] == ' ')) slash[--n] = 0;
            strncpy(path, slash, pathLen - 1);
            path[pathLen - 1] = 0;
        } else {
            path[0] = 0;
        }
        *base = start;
        found = true;
        break;
    }
    fclose(f);
    return found;
}

// ---- il2cpp API (names from the public il2cpp C API; resolved at runtime) ----
typedef void *(*fn_domain_get_t)();
typedef void **(*fn_domain_get_assemblies_t)(void *domain, size_t *size);
typedef void *(*fn_assembly_get_image_t)(void *assembly);
typedef const char *(*fn_image_get_name_t)(void *image);
typedef size_t (*fn_image_get_class_count_t)(void *image);
typedef void *(*fn_class_from_name_t)(void *image, const char *ns, const char *name);
typedef void *(*fn_class_get_methods_t)(void *klass, void **iter);
typedef const char *(*fn_method_get_name_t)(void *method);
typedef uint32_t (*fn_method_get_param_count_t)(void *method);
typedef uint32_t (*fn_method_get_flags_t)(void *method, uint32_t *iflags);
typedef void *(*fn_class_get_fields_t)(void *klass, void **iter);
typedef const char *(*fn_field_get_name_t)(void *field);
typedef size_t (*fn_field_get_offset_t)(void *field);
typedef int (*fn_field_get_flags_t)(void *field);
typedef void *(*fn_thread_attach_t)(void *domain);
typedef void (*fn_thread_detach_t)(void *thread);

static fn_domain_get_t p_domain_get;
static fn_domain_get_assemblies_t p_domain_get_assemblies;
static fn_assembly_get_image_t p_assembly_get_image;
static fn_image_get_name_t p_image_get_name;
static fn_image_get_class_count_t p_image_get_class_count;
static fn_class_from_name_t p_class_from_name;
static fn_class_get_methods_t p_class_get_methods;
static fn_method_get_name_t p_method_get_name;
static fn_method_get_param_count_t p_method_get_param_count;
static fn_method_get_flags_t p_method_get_flags;
static fn_class_get_fields_t p_class_get_fields;
static fn_field_get_name_t p_field_get_name;
static fn_field_get_offset_t p_field_get_offset;
static fn_field_get_flags_t p_field_get_flags;
static fn_thread_attach_t p_thread_attach;
static fn_thread_detach_t p_thread_detach;

static unsigned long g_il2cppBase = 0;

static bool resolveAll(void *h) {
    struct Entry { const char *name; void **slot; bool required; };
    Entry entries[] = {
        {"il2cpp_domain_get", (void **)&p_domain_get, true},
        {"il2cpp_domain_get_assemblies", (void **)&p_domain_get_assemblies, true},
        {"il2cpp_assembly_get_image", (void **)&p_assembly_get_image, true},
        {"il2cpp_image_get_name", (void **)&p_image_get_name, true},
        {"il2cpp_image_get_class_count", (void **)&p_image_get_class_count, false},
        {"il2cpp_class_from_name", (void **)&p_class_from_name, true},
        {"il2cpp_class_get_methods", (void **)&p_class_get_methods, false},
        {"il2cpp_method_get_name", (void **)&p_method_get_name, false},
        {"il2cpp_method_get_param_count", (void **)&p_method_get_param_count, false},
        {"il2cpp_method_get_flags", (void **)&p_method_get_flags, false},
        {"il2cpp_class_get_fields", (void **)&p_class_get_fields, false},
        {"il2cpp_field_get_name", (void **)&p_field_get_name, false},
        {"il2cpp_field_get_offset", (void **)&p_field_get_offset, false},
        {"il2cpp_field_get_flags", (void **)&p_field_get_flags, false},
        {"il2cpp_thread_attach", (void **)&p_thread_attach, true},
        {"il2cpp_thread_detach", (void **)&p_thread_detach, false},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        *entries[i].slot = xdl_sym(h, entries[i].name, nullptr);
        if (!*entries[i].slot) {
            LOGI("MENUTEST resolve: %s MISSING%s", entries[i].name, entries[i].required ? " (required)" : " (optional)");
            if (entries[i].required) ok = false;
        }
    }
    LOGI("MENUTEST resolveAll done ok=%d", ok ? 1 : 0);
    return ok;
}

static void dumpClass(void *image, const char *name, bool listMembers) {
    g_step = name;
    void *klass = p_class_from_name(image, "", name);
    if (!klass) {
        LOGI("MENUTEST class %s: NOT FOUND (namespace \"\")", name);
        return;
    }
    int methodCount = 0, fieldCount = 0;
    if (p_class_get_methods && p_method_get_name) {
        void *iter = nullptr;
        void *m;
        while ((m = p_class_get_methods(klass, &iter)) != nullptr) methodCount++;
    }
    if (p_class_get_fields && p_field_get_name) {
        void *iter = nullptr;
        void *f;
        while ((f = p_class_get_fields(klass, &iter)) != nullptr) fieldCount++;
    }
    LOGI("MENUTEST class %s: FOUND klass=%p methods=%d fields=%d", name, klass, methodCount, fieldCount);
    if (!listMembers) return;

    if (p_class_get_methods && p_method_get_name) {
        void *iter = nullptr;
        void *m;
        int n = 0;
        while ((m = p_class_get_methods(klass, &iter)) != nullptr && n < 150) {
            const char *mn = p_method_get_name(m);
            uint32_t pc = p_method_get_param_count ? p_method_get_param_count(m) : 0;
            uint32_t iflags = 0;
            uint32_t fl = p_method_get_flags ? p_method_get_flags(m, &iflags) : 0;
            // first pointer-sized field of MethodInfo is assumed to be the native code pointer [unverified]
            unsigned long ptr = m ? *(unsigned long *)m : 0;
            long rva = (ptr && g_il2cppBase && ptr >= g_il2cppBase) ? (long)(ptr - g_il2cppBase) : -1;
            LOGI("MENUTEST   %s::%s params=%u static=%d ptr?=0x%lx rva?=0x%lx", name, mn ? mn : "?", pc, (fl & 0x10) ? 1 : 0, ptr, rva);
            n++;
        }
    }
    if (p_class_get_fields && p_field_get_name) {
        void *iter = nullptr;
        void *f;
        int n = 0;
        while ((f = p_class_get_fields(klass, &iter)) != nullptr && n < 150) {
            const char *fn = p_field_get_name(f);
            size_t off = p_field_get_offset ? p_field_get_offset(f) : 0;
            int fl = p_field_get_flags ? p_field_get_flags(f) : 0;
            LOGI("MENUTEST   %s.%s offset=0x%zx static=%d", name, fn ? fn : "?", off, (fl & 0x10) ? 1 : 0);
            n++;
        }
    }
}

// ---- guarded work items ----
struct DomainArg { void *domain; };
static void domainFn(void *a) { ((DomainArg *)a)->domain = p_domain_get(); }

struct AsmArg { void *domain; void *image; size_t count; bool listed; };
static void asmFn(void *a) {
    AsmArg *x = (AsmArg *)a;
    x->count = 0;
    g_step = "domain_get_assemblies";
    void **asms = p_domain_get_assemblies(x->domain, &x->count);
    if (!asms || x->count == 0) return;
    if (!x->listed) LOGI("MENUTEST assemblies=%zu", x->count);
    for (size_t i = 0; i < x->count; i++) {
        g_step = "assembly_get_image";
        void *img = p_assembly_get_image(asms[i]);
        g_step = "image_get_name";
        const char *nm = img ? p_image_get_name(img) : nullptr;
        if (!x->listed) LOGI("MENUTEST   assembly[%zu]=%s", i, nm ? nm : "?");
        if (nm && strstr(nm, "Assembly-CSharp") && !strstr(nm, "firstpass")) x->image = img;
    }
    x->listed = true;
}

struct DumpArg { void *domain; void *image; };
static void dumpFn(void *a) {
    DumpArg *x = (DumpArg *)a;
    g_step = "thread_attach";
    void *thr = p_thread_attach(x->domain);
    LOGI("MENUTEST thread attached=%p", thr);

    if (p_image_get_class_count) {
        g_step = "image_get_class_count";
        LOGI("MENUTEST Assembly-CSharp class count=%zu", p_image_get_class_count(x->image));
    }

    const char *brief[] = {"GameData", "RoleBehaviour", "LobbyBehaviour", "MeetingHud",
                           "AmongUsClient", "HudManager", "KillButton", "Vent", "DeadBody", "ShipStatus"};
    for (size_t i = 0; i < sizeof(brief) / sizeof(brief[0]); i++) dumpClass(x->image, brief[i], false);

    const char *detail[] = {"PlayerControl", "PlayerPhysics", "NetworkedPlayerInfo"};
    for (size_t i = 0; i < sizeof(detail) / sizeof(detail[0]); i++) dumpClass(x->image, detail[i], true);

    if (thr && p_thread_detach) p_thread_detach(thr);
}

static void *watcher(void *) {
    const long t0 = nowMs();
    char path[512];
    unsigned long base = 0;

    while (nowMs() - t0 < 90000) {
        if (findInMaps("libil2cpp.so", &base, path, sizeof(path))) break;
        usleep(100 * 1000);
    }
    if (base == 0) {
        LOGI("MENUTEST libil2cpp.so NOT seen within 90 s");
        return nullptr;
    }
    g_il2cppBase = base;
    LOGI("MENUTEST libil2cpp.so seen after %ld ms, base=0x%lx", nowMs() - t0, base);

    void *h = xdl_open("libil2cpp.so", XDL_DEFAULT);
    if (!h) h = xdl_open(path, XDL_DEFAULT);
    LOGI("MENUTEST xdl_open handle=%p", h);
    if (!h) return nullptr;
    if (!resolveAll(h)) {
        LOGI("MENUTEST required il2cpp function missing, stopping");
        return nullptr;
    }

    // probe il2cpp_domain_get: v4 crashed here-ish (null deref inside libil2cpp, 3 frames deep).
    // Now guarded: a fault means "il2cpp not ready yet", we wait and retry.
    const long kStartDelayMs = 30000; // UNCALIBRATED guess: how long Unity needs to finish il2cpp_init
    LOGI("MENUTEST waiting until %ld ms before the first il2cpp call (no il2cpp calls until then)", kStartDelayMs);
    while (nowMs() - t0 < kStartDelayMs) usleep(250 * 1000);
    LOGI("MENUTEST start delay over at %ld ms", nowMs() - t0);
    LOGI("MENUTEST calling il2cpp_domain_get (guarded)");
    void *domain = nullptr;
    int tries = 0, faults = 0;
    while (nowMs() - t0 < 90000 && faults < 40) {
        DomainArg da = {nullptr};
        g_step = "il2cpp_domain_get";
        bool ok = guarded(domainFn, &da);
        tries++;
        if (!ok) {
            faults++;
            if (faults <= 3 || faults % 10 == 0)
                LOGI("MENUTEST domain_get FAULT #%d at %ld ms (il2cpp not ready?)", faults, nowMs() - t0);
        } else if (da.domain) {
            domain = da.domain;
            break;
        } else if (tries <= 3) {
            LOGI("MENUTEST domain_get returned null (try %d)", tries);
        }
        usleep(300 * 1000);
    }
    if (!domain) {
        LOGI("MENUTEST il2cpp domain not obtained: tries=%d faults=%d", tries, faults);
        return nullptr;
    }
    LOGI("MENUTEST il2cpp domain=%p after %ld ms (tries=%d faults=%d)", domain, nowMs() - t0, tries, faults);
    usleep(1500 * 1000); // let il2cpp finish init

    // wait for Assembly-CSharp
    AsmArg aa = {domain, nullptr, 0, false};
    int asmFaults = 0, asmTries = 0;
    while (nowMs() - t0 < 90000 && !aa.image && asmFaults < 10) {
        bool ok = guarded(asmFn, &aa);
        if (!ok) {
            asmFaults++;
            LOGI("MENUTEST assembly scan FAULT #%d, step=%s", asmFaults, g_step);
        }
        if (!aa.image) {
            asmTries++;
            if (asmTries <= 2 || asmTries % 5 == 0)
                LOGI("MENUTEST Assembly-CSharp not yet visible (try %d, asmCount=%zu)", asmTries, aa.count);
            usleep(1000 * 1000);
        }
    }
    if (!aa.image) {
        LOGI("MENUTEST Assembly-CSharp not found (faults=%d)", asmFaults);
        return nullptr;
    }
    LOGI("MENUTEST Assembly-CSharp image=%p after %ld ms", aa.image, nowMs() - t0);

    DumpArg da2 = {domain, aa.image};
    if (!guarded(dumpFn, &da2)) {
        LOGI("MENUTEST dump FAULT, last step=%s", g_step);
        return nullptr;
    }
    LOGI("MENUTEST v6 watcher done after %ld ms", nowMs() - t0);
    return nullptr;
}

__attribute__((constructor))
static void menutest_ctor() {
    LOGI("MENUTEST v6 loaded in pid=%d uid=%d", getpid(), getuid());
    pthread_t th;
    if (pthread_create(&th, nullptr, watcher, nullptr) == 0) {
        pthread_detach(th);
    } else {
        LOGI("MENUTEST pthread_create failed");
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    LOGI("MENUTEST JNI_OnLoad called, pid=%d", getpid());
    return JNI_VERSION_1_6;
}
