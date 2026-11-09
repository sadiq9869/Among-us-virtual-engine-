// MENUTEST: diagnostic only. Writes log lines when loaded. Does nothing else.
#include <android/log.h>
#include <jni.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define TAG "MENUTEST"

static int countInMaps(const char *needle) {
    int n = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) n++;
    }
    fclose(f);
    return n;
}

__attribute__((constructor))
static void menutest_ctor() {
    __android_log_print(ANDROID_LOG_INFO, TAG,
                        "MENUTEST loaded in pid=%d uid=%d | maps: libil2cpp=%d libunity=%d libmain=%d",
                        getpid(), getuid(),
                        countInMaps("libil2cpp.so"),
                        countInMaps("libunity.so"),
                        countInMaps("libmain.so"));
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "MENUTEST JNI_OnLoad called, pid=%d", getpid());
    return JNI_VERSION_1_6;
}
