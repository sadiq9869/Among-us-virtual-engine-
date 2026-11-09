// MENUTEST v2: diagnostic only. Waits for libil2cpp.so to appear in this process,
// then logs when it appeared and whether common il2cpp export functions can be resolved.
// It does not hook, patch or change anything in the game.
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define TAG "MENUTEST"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Finds the first maps line containing `needle`. Returns true and fills base + path.
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

static void *watcher(void *) {
    const long t0 = nowMs();
    bool seenUnity = false, seenMain = false;
    char path[512];
    unsigned long base = 0;

    while (nowMs() - t0 < 90000) {
        char p2[512];
        unsigned long b2 = 0;
        if (!seenMain && findInMaps("libmain.so", &b2, p2, sizeof(p2))) {
            seenMain = true;
            LOGI("MENUTEST libmain.so seen after %ld ms", nowMs() - t0);
        }
        if (!seenUnity && findInMaps("libunity.so", &b2, p2, sizeof(p2))) {
            seenUnity = true;
            LOGI("MENUTEST libunity.so seen after %ld ms", nowMs() - t0);
        }
        if (findInMaps("libil2cpp.so", &base, path, sizeof(path))) break;
        usleep(100 * 1000);
    }

    if (base == 0) {
        LOGI("MENUTEST libil2cpp.so NOT seen within 90 s");
        return nullptr;
    }
    LOGI("MENUTEST libil2cpp.so seen after %ld ms, base=0x%lx path=%s", nowMs() - t0, base, path);

    void *h = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        const char *e = dlerror();
        LOGI("MENUTEST dlopen(NOLOAD) failed: %s", e ? e : "(null)");
        h = RTLD_DEFAULT;
        LOGI("MENUTEST falling back to RTLD_DEFAULT for dlsym");
    } else {
        LOGI("MENUTEST dlopen(NOLOAD) OK handle=%p", h);
    }

    const char *names[] = {
        "il2cpp_domain_get",
        "il2cpp_domain_get_assemblies",
        "il2cpp_domain_assembly_open",
        "il2cpp_assembly_get_image",
        "il2cpp_image_get_name",
        "il2cpp_class_from_name",
        "il2cpp_class_get_method_from_name",
        "il2cpp_class_get_field_from_name",
        "il2cpp_thread_attach",
        "il2cpp_runtime_invoke",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        void *sym = dlsym(h, names[i]);
        LOGI("MENUTEST sym %s = %p", names[i], sym);
    }
    LOGI("MENUTEST watcher done");
    return nullptr;
}

__attribute__((constructor))
static void menutest_ctor() {
    LOGI("MENUTEST v2 loaded in pid=%d uid=%d", getpid(), getuid());
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
