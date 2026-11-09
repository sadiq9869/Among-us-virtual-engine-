// MENUTEST v4: diagnostic only (read-only). No hooks, no patches, nothing is changed in the game.
// Steps: wait for libil2cpp.so -> resolve il2cpp API with xdl -> wait for il2cpp domain ->
// find Assembly-CSharp -> attach thread -> look up key classes BY NAME -> list their methods/fields.
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "xdl.h"

#define TAG "MENUTEST"
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); usleep(1500); } while (0)

static long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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
    return ok;
}

static void dumpClass(void *image, const char *name, bool listMembers) {
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

    // wait for the il2cpp domain to exist
    void *domain = nullptr;
    while (nowMs() - t0 < 90000) {
        domain = p_domain_get();
        if (domain) break;
        usleep(200 * 1000);
    }
    if (!domain) {
        LOGI("MENUTEST il2cpp domain still null after 90 s");
        return nullptr;
    }
    LOGI("MENUTEST il2cpp domain=%p after %ld ms", domain, nowMs() - t0);
    usleep(1500 * 1000); // let il2cpp finish init

    // wait for Assembly-CSharp
    void *image = nullptr;
    size_t asmCount = 0;
    bool listed = false;
    while (nowMs() - t0 < 90000 && !image) {
        asmCount = 0;
        void **asms = p_domain_get_assemblies(domain, &asmCount);
        if (asms && asmCount > 0) {
            if (!listed) {
                LOGI("MENUTEST assemblies=%zu", asmCount);
            }
            for (size_t i = 0; i < asmCount; i++) {
                void *img = p_assembly_get_image(asms[i]);
                const char *nm = img ? p_image_get_name(img) : nullptr;
                if (!listed) LOGI("MENUTEST   assembly[%zu]=%s", i, nm ? nm : "?");
                if (nm && strstr(nm, "Assembly-CSharp") && !strstr(nm, "firstpass")) image = img;
            }
            listed = true;
        }
        if (!image) usleep(1000 * 1000);
    }
    if (!image) {
        LOGI("MENUTEST Assembly-CSharp not found");
        return nullptr;
    }
    LOGI("MENUTEST Assembly-CSharp image=%p after %ld ms", image, nowMs() - t0);

    void *thr = p_thread_attach(domain);
    LOGI("MENUTEST thread attached=%p", thr);

    if (p_image_get_class_count) {
        LOGI("MENUTEST Assembly-CSharp class count=%zu", p_image_get_class_count(image));
    }

    const char *brief[] = {"GameData", "RoleBehaviour", "LobbyBehaviour", "MeetingHud",
                           "AmongUsClient", "HudManager", "KillButton", "Vent", "DeadBody", "ShipStatus"};
    for (size_t i = 0; i < sizeof(brief) / sizeof(brief[0]); i++) dumpClass(image, brief[i], false);

    const char *detail[] = {"PlayerControl", "PlayerPhysics", "NetworkedPlayerInfo"};
    for (size_t i = 0; i < sizeof(detail) / sizeof(detail[0]); i++) dumpClass(image, detail[i], true);

    if (thr && p_thread_detach) p_thread_detach(thr);
    LOGI("MENUTEST v4 watcher done after %ld ms", nowMs() - t0);
    return nullptr;
}

__attribute__((constructor))
static void menutest_ctor() {
    LOGI("MENUTEST v4 loaded in pid=%d uid=%d", getpid(), getuid());
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
