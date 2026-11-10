// MENUTEST v9 - first real feature: "Complete my tasks".
// Build for game 2026.11.3 (Unity 2022.3.62f3, il2cpp). NOT built / NOT tested when written.
//
// What it does:
//  * waits for libil2cpp.so, resolves il2cpp API BY NAME (xdl), then waits kStartDelayMs before
//    touching il2cpp at all (v4/v5 crashed the game by calling il2cpp too early; v6 with the delay worked);
//  * finds Assembly-CSharp, PlayerControl, its static field LocalPlayer, its field myTasks and the
//    methods RpcCompleteTask / FixedUpdate, all BY NAME (no hardcoded offsets or addresses);
//  * on a button press (Kotlin -> JNI) it runs on the Unity main thread (Dobby hook on
//    PlayerControl::FixedUpdate; if that hook cannot be installed it falls back to the helper thread):
//      LocalPlayer.myTasks -> for every task: skip if already complete, else call
//      LocalPlayer.RpcCompleteTask(task.Id); if that is not possible/throws, call task.Complete().
//    Task class, Id field, parameter type, List layout are all discovered through il2cpp reflection at run
//    time; whatever is not found is reported in the status text and in logcat (tag MENUTEST).
// Unknown until the first real run (nobody has seen them): the task class names, the type of Id, whether
// the host accepts the completion. Everything is logged so the next step can be exact.
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <atomic>
#include <mutex>
#include "xdl.h"
#include "Dobby/dobby.h"

#define TAG "MENUTEST"
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); usleep(1500); } while (0)

// ---------------------------------------------------------------- config
static const long kStartDelayMs = 30000;   // UNCALIBRATED: no il2cpp call before this (v6 worked with it)
static const long kGiveUpMs     = 180000;  // stop waiting for il2cpp/Assembly-CSharp after this
static const int  kMaxTasks     = 128;     // sanity bound for the task list
static const long kReqTtlMs     = 5000;    // a press is only served within 5 s (no surprise run later)

// ---------------------------------------------------------------- state / status text
enum { ST_LOADING = 0, ST_READY = 1, ST_ERROR = 2 };
static std::atomic<int> g_state{ST_LOADING};
static std::mutex g_statusMx;
static char g_status[600] = "Menu: loading...";
static std::atomic<bool> g_req{false};     // set by the button, consumed on the Unity thread
static std::atomic<bool> g_hooked{false};
static std::atomic<long> g_reqAt{0};       // when the button was pressed (ms), requests older than kReqTtlMs expire
static long g_t0 = 0;

static void setStatus(const char *fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        strncpy(g_status, buf, sizeof(g_status) - 1);
        g_status[sizeof(g_status) - 1] = 0;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "STATUS: %s", buf);
}

static long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// ---------------------------------------------------------------- crash guard (SIGSEGV/SIGBUS on the calling thread)
static __thread sigjmp_buf t_jmp;
static __thread int t_guard = 0;
static struct sigaction g_oldSegv, g_oldBus;
static std::mutex g_guardMx;

static void guardHandler(int sig, siginfo_t *info, void *ctx) {
    if (t_guard) siglongjmp(t_jmp, 1);
    struct sigaction *old = (sig == SIGSEGV) ? &g_oldSegv : &g_oldBus;
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
        old->sa_sigaction(sig, info, ctx);
    } else if (old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN) {
        sigaction(sig, old, nullptr); // returning re-faults into the previous (default) action
    } else {
        old->sa_handler(sig);
    }
}

// Runs fn(arg); true if it finished, false if it faulted. Handler is installed only for the call.
static bool guarded(void (*fn)(void *), void *arg) {
    g_guardMx.lock();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = guardHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_oldSegv);
    sigaction(SIGBUS, &sa, &g_oldBus);
    volatile bool ok = false;
    t_guard = 1;
    if (sigsetjmp(t_jmp, 1) == 0) {
        fn(arg);
        ok = true;
    }
    t_guard = 0;
    sigaction(SIGSEGV, &g_oldSegv, nullptr);
    sigaction(SIGBUS, &g_oldBus, nullptr);
    g_guardMx.unlock();
    return ok;
}

// ---------------------------------------------------------------- maps helpers
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

// true if addr lies in an executable mapping of libil2cpp.so
static bool inIl2cppText(unsigned long addr) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[1024];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "libil2cpp.so")) continue;
        unsigned long s = 0, e = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3 && perms[2] == 'x' && addr >= s && addr < e) {
            ok = true;
            break;
        }
    }
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------- il2cpp API (public C API names, resolved at run time)
typedef void *(*fn_domain_get_t)();
typedef void **(*fn_domain_get_assemblies_t)(void *domain, size_t *size);
typedef void *(*fn_assembly_get_image_t)(void *assembly);
typedef const char *(*fn_image_get_name_t)(void *image);
typedef void *(*fn_class_from_name_t)(void *image, const char *ns, const char *name);
typedef void *(*fn_class_get_methods_t)(void *klass, void **iter);
typedef void *(*fn_class_get_fields_t)(void *klass, void **iter);
typedef void *(*fn_class_get_parent_t)(void *klass);
typedef void *(*fn_class_get_method_from_name_t)(void *klass, const char *name, int argc);
typedef void *(*fn_class_get_field_from_name_t)(void *klass, const char *name);
typedef const char *(*fn_class_get_name_t)(void *klass);
typedef const char *(*fn_method_get_name_t)(void *method);
typedef void *(*fn_method_get_param_t)(void *method, uint32_t index);
typedef const char *(*fn_field_get_name_t)(void *field);
typedef void *(*fn_field_get_type_t)(void *field);
typedef void (*fn_field_get_value_t)(void *obj, void *field, void *out);
typedef void (*fn_field_static_get_value_t)(void *field, void *out);
typedef int (*fn_type_get_type_t)(void *type);
typedef void *(*fn_runtime_invoke_t)(void *method, void *obj, void **params, void **exc);
typedef void *(*fn_object_unbox_t)(void *obj);
typedef void *(*fn_object_get_class_t)(void *obj);
typedef void *(*fn_thread_attach_t)(void *domain);
typedef void (*fn_thread_detach_t)(void *thread);
typedef void *(*fn_class_from_type_t)(void *type);
typedef int (*fn_field_get_flags_t)(void *field);
typedef uint16_t *(*fn_string_chars_t)(void *str);
typedef int32_t (*fn_string_length_t)(void *str);

static fn_domain_get_t p_domain_get;
static fn_domain_get_assemblies_t p_domain_get_assemblies;
static fn_assembly_get_image_t p_assembly_get_image;
static fn_image_get_name_t p_image_get_name;
static fn_class_from_name_t p_class_from_name;
static fn_class_get_methods_t p_class_get_methods;
static fn_class_get_fields_t p_class_get_fields;
static fn_class_get_parent_t p_class_get_parent;
static fn_class_get_method_from_name_t p_class_get_method_from_name;
static fn_class_get_field_from_name_t p_class_get_field_from_name;
static fn_class_get_name_t p_class_get_name;
static fn_method_get_name_t p_method_get_name;
static fn_method_get_param_t p_method_get_param;
static fn_field_get_name_t p_field_get_name;
static fn_field_get_type_t p_field_get_type;
static fn_field_get_value_t p_field_get_value;
static fn_field_static_get_value_t p_field_static_get_value;
static fn_type_get_type_t p_type_get_type;
static fn_runtime_invoke_t p_runtime_invoke;
static fn_object_unbox_t p_object_unbox;
static fn_object_get_class_t p_object_get_class;
static fn_thread_attach_t p_thread_attach;
static fn_thread_detach_t p_thread_detach;
static fn_class_from_type_t p_class_from_type;
static fn_field_get_flags_t p_field_get_flags;
static fn_string_chars_t p_string_chars;
static fn_string_length_t p_string_length;

static bool resolveAll(void *h) {
    struct Entry { const char *name; void **slot; bool required; };
    Entry entries[] = {
        {"il2cpp_domain_get", (void **)&p_domain_get, true},
        {"il2cpp_domain_get_assemblies", (void **)&p_domain_get_assemblies, true},
        {"il2cpp_assembly_get_image", (void **)&p_assembly_get_image, true},
        {"il2cpp_image_get_name", (void **)&p_image_get_name, true},
        {"il2cpp_class_from_name", (void **)&p_class_from_name, true},
        {"il2cpp_class_get_methods", (void **)&p_class_get_methods, false},
        {"il2cpp_class_get_fields", (void **)&p_class_get_fields, false},
        {"il2cpp_class_get_parent", (void **)&p_class_get_parent, false},
        {"il2cpp_class_get_method_from_name", (void **)&p_class_get_method_from_name, true},
        {"il2cpp_class_get_field_from_name", (void **)&p_class_get_field_from_name, true},
        {"il2cpp_class_get_name", (void **)&p_class_get_name, true},
        {"il2cpp_method_get_name", (void **)&p_method_get_name, false},
        {"il2cpp_method_get_param", (void **)&p_method_get_param, true},
        {"il2cpp_field_get_name", (void **)&p_field_get_name, false},
        {"il2cpp_field_get_type", (void **)&p_field_get_type, false},
        {"il2cpp_field_get_value", (void **)&p_field_get_value, true},
        {"il2cpp_field_static_get_value", (void **)&p_field_static_get_value, true},
        {"il2cpp_type_get_type", (void **)&p_type_get_type, true},
        {"il2cpp_runtime_invoke", (void **)&p_runtime_invoke, true},
        {"il2cpp_object_unbox", (void **)&p_object_unbox, true},
        {"il2cpp_object_get_class", (void **)&p_object_get_class, true},
        {"il2cpp_thread_attach", (void **)&p_thread_attach, true},
        {"il2cpp_thread_detach", (void **)&p_thread_detach, false},
        {"il2cpp_class_from_type", (void **)&p_class_from_type, false},
        {"il2cpp_field_get_flags", (void **)&p_field_get_flags, false},
        {"il2cpp_string_chars", (void **)&p_string_chars, false},
        {"il2cpp_string_length", (void **)&p_string_length, false},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        *entries[i].slot = xdl_sym(h, entries[i].name, nullptr);
        if (!*entries[i].slot) {
            LOGI("MENUTEST resolve: %s MISSING%s", entries[i].name, entries[i].required ? " (required)" : " (optional)");
            if (entries[i].required) {
                ok = false;
                setStatus("Menu error: il2cpp function %s not found in this game build", entries[i].name);
            }
        }
    }
    LOGI("MENUTEST resolveAll done ok=%d", ok ? 1 : 0);
    return ok;
}

// look a field/method up in the class and then in its parents (explicit, in case the API does not walk parents)
static void *findField(void *k, const char *name) {
    for (int guard = 0; k && guard < 16; guard++) {
        void *f = p_class_get_field_from_name(k, name);
        if (f) return f;
        k = p_class_get_parent ? p_class_get_parent(k) : nullptr;
    }
    return nullptr;
}
static void *findMethod(void *k, const char *name, int argc) {
    for (int guard = 0; k && guard < 16; guard++) {
        void *m = p_class_get_method_from_name(k, name, argc);
        if (m) return m;
        k = p_class_get_parent ? p_class_get_parent(k) : nullptr;
    }
    return nullptr;
}

// ---------------------------------------------------------------- resolved game objects (all found BY NAME at run time)
static void *g_domain = nullptr;
static void *g_image = nullptr;
static void *k_PlayerControl = nullptr;
static void *f_LocalPlayer = nullptr;   // static field PlayerControl.LocalPlayer
static void *f_myTasks = nullptr;       // instance field PlayerControl.myTasks (a List<...>)
static void *m_RpcCompleteTask = nullptr; // PlayerControl.RpcCompleteTask(1 param), optional
static void *m_FixedUpdate = nullptr;   // PlayerControl.FixedUpdate(0 params), used for the main-thread hook

// ---------------------------------------------------------------- invoke helpers
static void *invokeRaw(void *m, void *obj, void **args, const char *what, bool *ok) {
    void *exc = nullptr;
    void *r = p_runtime_invoke(m, obj, args, &exc);
    if (exc) {
        void *ek = p_object_get_class(exc);
        LOGI("MENUTEST   exception in %s: %s", what, ek ? p_class_get_name(ek) : "?");
        *ok = false;
        return nullptr;
    }
    *ok = true;
    return r;
}
static bool invokeInt(void *m, void *obj, void **args, const char *what, int *out) {
    bool ok = false;
    void *r = invokeRaw(m, obj, args, what, &ok);
    if (!ok || !r) return false;
    void *p = p_object_unbox(r);
    if (!p) return false;
    *out = *(int *)p;
    return true;
}
static bool invokeBool(void *m, void *obj, const char *what, bool *out) {
    bool ok = false;
    void *r = invokeRaw(m, obj, nullptr, what, &ok);
    if (!ok || !r) return false;
    void *p = p_object_unbox(r);
    if (!p) return false;
    *out = (*(uint8_t *)p) != 0;
    return true;
}

// one-time discovery log of a class (names only) so a failure can be diagnosed from logcat
static void logClassNames(void *k, const char *label) {
    if (!k) return;
    LOGI("MENUTEST discovery %s: class %s", label, p_class_get_name(k));
    for (int depth = 0; k && depth < 4; depth++) {
        if (p_class_get_methods && p_method_get_name) {
            void *iter = nullptr, *m;
            int n = 0;
            while ((m = p_class_get_methods(k, &iter)) != nullptr && n < 60) {
                LOGI("MENUTEST   %s method %s::%s", label, p_class_get_name(k), p_method_get_name(m));
                n++;
            }
        }
        if (p_class_get_fields && p_field_get_name) {
            void *iter = nullptr, *f;
            int n = 0;
            while ((f = p_class_get_fields(k, &iter)) != nullptr && n < 60) {
                LOGI("MENUTEST   %s field %s.%s", label, p_class_get_name(k), p_field_get_name(f));
                n++;
            }
        }
        k = p_class_get_parent ? p_class_get_parent(k) : nullptr;
    }
}

// ---------------------------------------------------------------- v9: extra lookups (players, roles) - all BY NAME
static const int kMaxPlayers = 40;
static void *k_NetInfo = nullptr;          // class NetworkedPlayerInfo
static void *f_AllPlayers = nullptr;       // static field PlayerControl.AllPlayerControls (a List<PlayerControl>)
static void *m_get_Data = nullptr;         // PlayerControl.get_Data()
static void *m_get_PlayerName = nullptr;   // NetworkedPlayerInfo.get_PlayerName()
static void *f_IsDead = nullptr, *f_Disconnected = nullptr, *f_RoleType = nullptr, *f_RoleWhenAlive = nullptr;
struct EnumVal { char name[40]; unsigned val; };
static EnumVal g_roleEnum[48];             // RoleType enum names/values read from the live game
static int g_roleEnumCount = 0;
static bool g_extraDone = false;

static void resolveExtraFn(void *) {
    if (g_extraDone) return;
    g_extraDone = true;
    f_AllPlayers = findField(k_PlayerControl, "AllPlayerControls");
    m_get_Data = findMethod(k_PlayerControl, "get_Data", 0);
    k_NetInfo = p_class_from_name(g_image, "", "NetworkedPlayerInfo");
    if (k_NetInfo) {
        m_get_PlayerName = findMethod(k_NetInfo, "get_PlayerName", 0);
        f_IsDead = findField(k_NetInfo, "IsDead");
        f_Disconnected = findField(k_NetInfo, "Disconnected");
        f_RoleType = findField(k_NetInfo, "RoleType");
        f_RoleWhenAlive = findField(k_NetInfo, "RoleWhenAlive");
    }
    if (f_RoleType && p_field_get_type && p_class_from_type && p_class_get_fields && p_field_get_name) {
        void *ty = p_field_get_type(f_RoleType);
        void *ek = ty ? p_class_from_type(ty) : nullptr;
        if (ek) {
            LOGI("MENUTEST role enum class: %s", p_class_get_name(ek));
            void *iter = nullptr, *f;
            while ((f = p_class_get_fields(ek, &iter)) != nullptr && g_roleEnumCount < 48) {
                const char *fn = p_field_get_name(f);
                if (!fn || !strcmp(fn, "value__")) continue;
                if (p_field_get_flags && !(p_field_get_flags(f) & 0x10)) continue; // 0x10 = static
                uint64_t buf = 0;
                p_field_static_get_value(f, &buf);
                EnumVal &e = g_roleEnum[g_roleEnumCount++];
                strncpy(e.name, fn, sizeof(e.name) - 1);
                e.name[sizeof(e.name) - 1] = 0;
                e.val = (unsigned)(buf & 0xffff);
                LOGI("MENUTEST   role enum %s = %u", e.name, e.val);
            }
        }
    }
    LOGI("MENUTEST extra: AllPlayerControls=%p get_Data=%p NetInfo=%p get_PlayerName=%p IsDead=%p Disconnected=%p RoleType=%p RoleWhenAlive=%p enumNames=%d",
         f_AllPlayers, m_get_Data, k_NetInfo, m_get_PlayerName, f_IsDead, f_Disconnected, f_RoleType, f_RoleWhenAlive, g_roleEnumCount);
}

static const char *roleName(unsigned v) {
    for (int i = 0; i < g_roleEnumCount; i++) if (g_roleEnum[i].val == v) return g_roleEnum[i].name;
    return nullptr;
}
static bool readU16(void *obj, void *field, unsigned *out) {
    if (!obj || !field) return false;
    uint64_t b = 0;
    p_field_get_value(obj, field, &b);
    *out = (unsigned)(b & 0xffff);
    return true;
}
static bool readBoolField(void *obj, void *field, bool *out) {
    if (!obj || !field) return false;
    uint64_t b = 0;
    p_field_get_value(obj, field, &b);
    *out = (b & 0xff) != 0;
    return true;
}
static void *dataOf(void *pc) {
    if (!pc || !m_get_Data) return nullptr;
    bool ok = false;
    void *r = invokeRaw(m_get_Data, pc, nullptr, "get_Data", &ok);
    return ok ? r : nullptr;
}
// by NAME of the role (read from the game's own enum): impostor team = names containing "Impostor", Shapeshifter, Phantom, Viper
// (a name-based assumption about the game, not a confirmed fact)
static bool nameIsImpostorTeam(const char *n) {
    return n && (strstr(n, "Impostor") || !strcmp(n, "Shapeshifter") || !strcmp(n, "Phantom") || !strcmp(n, "Viper"));
}
// known=false if the role could not be read (then the answer must not be trusted)
static bool isImpostorData(void *data, bool *known) {
    *known = false;
    unsigned rt = 0, rw = 0;
    if (!data || g_roleEnumCount == 0 || !readU16(data, f_RoleType, &rt)) return false;
    *known = true;
    if (nameIsImpostorTeam(roleName(rt))) return true;
    if (f_RoleWhenAlive && readU16(data, f_RoleWhenAlive, &rw) && nameIsImpostorTeam(roleName(rw))) return true;
    return false;
}
static void il2str(void *s, char *out, size_t cap) {
    out[0] = 0;
    if (!s || !p_string_chars || !p_string_length || cap < 8) return;
    int n = p_string_length(s);
    const uint16_t *c = p_string_chars(s);
    if (!c || n <= 0) return;
    if (n > 32) n = 32;
    size_t o = 0;
    for (int i = 0; i < n && o + 4 < cap; i++) {
        unsigned ch = c[i];
        if (ch < 0x80) out[o++] = (char)ch;
        else if (ch >= 0xD800 && ch <= 0xDFFF) out[o++] = '?';
        else if (ch < 0x800) { out[o++] = (char)(0xC0 | (ch >> 6)); out[o++] = (char)(0x80 | (ch & 0x3F)); }
        else { out[o++] = (char)(0xE0 | (ch >> 12)); out[o++] = (char)(0x80 | ((ch >> 6) & 0x3F)); out[o++] = (char)(0x80 | (ch & 0x3F)); }
    }
    out[o] = 0;
}

// ---------------------------------------------------------------- v9: player info (name / role / alive-dead)
static std::mutex g_playersMx;
static char g_players[4096] = "Players: not requested yet.";
static std::atomic<long> g_playersAt{0};
static std::atomic<bool> g_reqPlayers{false};

static void setPlayers(const char *txt) {
    std::lock_guard<std::mutex> lk(g_playersMx);
    strncpy(g_players, txt, sizeof(g_players) - 1);
    g_players[sizeof(g_players) - 1] = 0;
    g_playersAt.store(nowMs());
}

static void playersFn(void *) {
    void *local = nullptr;
    if (f_LocalPlayer) p_field_static_get_value(f_LocalPlayer, &local);
    void *list = nullptr;
    if (f_AllPlayers) p_field_static_get_value(f_AllPlayers, &list);
    if (!list) { setPlayers("No players yet (join a lobby or match)."); return; }
    void *lk = p_object_get_class(list);
    void *mCount = findMethod(lk, "get_Count", 0);
    void *mItem = findMethod(lk, "get_Item", 1);
    int count = 0;
    if (!mCount || !mItem || !invokeInt(mCount, list, nullptr, "get_Count", &count)) { setPlayers("Could not read the player list."); return; }
    if (count < 0 || count > kMaxPlayers) { setPlayers("Player count is outside the sane range, not reading."); return; }
    if (count == 0) { setPlayers("Player list is empty."); return; }

    char out[4096];
    size_t o = 0;
    int alive = 0, dead = 0;
    char lines[3800];
    size_t lo = 0;
    lines[0] = 0;
    for (int i = 0; i < count; i++) {
        int idx = i;
        void *args[1] = {&idx};
        bool ok = false;
        void *pc = invokeRaw(mItem, list, args, "get_Item", &ok);
        void *data = ok ? dataOf(pc) : nullptr;
        if (!data) { lo += snprintf(lines + lo, sizeof(lines) - lo, "#%d | (no data yet)\n", i); if (lo >= sizeof(lines)) break; continue; }
        char name[120];
        name[0] = 0;
        if (m_get_PlayerName) {
            bool okn = false;
            void *s = invokeRaw(m_get_PlayerName, data, nullptr, "get_PlayerName", &okn);
            if (okn) il2str(s, name, sizeof(name));
        }
        if (!name[0]) snprintf(name, sizeof(name), "player#%d", i);
        bool isDead = false, disc = false;
        bool haveDead = readBoolField(data, f_IsDead, &isDead);
        readBoolField(data, f_Disconnected, &disc);
        unsigned rt = 0, rw = 0;
        bool haveRole = readU16(data, f_RoleType, &rt);
        char role[80];
        if (!haveRole) snprintf(role, sizeof(role), "role unknown");
        else if (roleName(rt)) snprintf(role, sizeof(role), "%s", roleName(rt));
        else snprintf(role, sizeof(role), "role#%u", rt);
        if (haveRole && isDead && f_RoleWhenAlive && readU16(data, f_RoleWhenAlive, &rw) && rw != rt) {
            size_t rl = strlen(role);
            if (roleName(rw)) snprintf(role + rl, sizeof(role) - rl, " (was %s)", roleName(rw));
        }
        if (haveDead) { if (isDead) dead++; else alive++; }
        lo += snprintf(lines + lo, sizeof(lines) - lo, "%s%s | %s | %s%s\n", name, (pc == local) ? " (you)" : "", role,
                       haveDead ? (isDead ? "DEAD" : "Alive") : "?", disc ? " | Disconnected" : "");
        if (lo >= sizeof(lines)) { lo = sizeof(lines) - 1; break; }
    }
    o += snprintf(out + o, sizeof(out) - o, "Players: %d (alive %d, dead %d)\n%s", count, alive, dead, lines);
    if (o < sizeof(out) - 120)
        snprintf(out + o, sizeof(out) - o, "Note: the server may hide other players' roles; this shows only what this client knows.");
    setPlayers(out);
}

static void runPlayers() {
    if (!guarded(resolveExtraFn, nullptr)) { setPlayers("Fault while resolving player classes (see logcat MENUTEST)."); return; }
    if (!guarded(playersFn, nullptr)) {
        setPlayers("Crash guard caught a fault while reading players (see logcat MENUTEST).");
        LOGI("MENUTEST playersFn FAULT");
    }
}

// ---------------------------------------------------------------- the feature: complete all my tasks
static std::atomic<int> g_discoveryLogged{0};

static bool isIntegerType(int tt) { return tt >= 0x04 && tt <= 0x0b; } // I1,U1,I2,U2,I4,U4,I8,U8 (ECMA element types)

static void workFn(void *) {
    void *local = nullptr;
    p_field_static_get_value(f_LocalPlayer, &local);
    if (!local) {
        setStatus("No local player yet. Join a lobby or start a match, then press again.");
        return;
    }
    // v9: an impostor's task list is fake. Sending RpcCompleteTask for it got the player kicked (v8 test), so do not send it.
    {
        guarded(resolveExtraFn, nullptr);
        bool known = false;
        bool imp = isImpostorData(dataOf(local), &known);
        if (!known) {
            setStatus("Cannot read your role (RoleType / get_Data / enum names missing, see logcat MENUTEST). Not sending anything.");
            return;
        }
        if (imp) {
            setStatus("You are on the Impostor team: your tasks are fake and are not sent to the host (sending them got you kicked). Nothing done.");
            LOGI("MENUTEST local player is impostor-team; task completion blocked");
            return;
        }
    }
    void *list = nullptr;
    p_field_get_value(local, f_myTasks, &list);
    if (!list) {
        setStatus("myTasks is empty/null (no tasks assigned yet).");
        return;
    }
    void *lk = p_object_get_class(list);
    void *mCount = findMethod(lk, "get_Count", 0);
    void *mItem = findMethod(lk, "get_Item", 1);
    if (!mCount || !mItem) {
        logClassNames(lk, "tasklist");
        setStatus("Task list class %s has no get_Count/get_Item (see logcat MENUTEST discovery).", lk ? p_class_get_name(lk) : "?");
        return;
    }
    int count = 0;
    if (!invokeInt(mCount, list, nullptr, "get_Count", &count)) {
        setStatus("Could not read the number of tasks (exception, see logcat).");
        return;
    }
    if (count < 0 || count > kMaxTasks) {
        setStatus("Task count %d is outside 0..%d, refusing to continue.", count, kMaxTasks);
        return;
    }
    if (count == 0) {
        setStatus("You have 0 tasks right now.");
        return;
    }

    int done = 0, skippedDone = 0, skippedSystem = 0, viaRpc = 0, viaComplete = 0, failed = 0;
    for (int i = 0; i < count; i++) {
        int idx = i;
        void *args[1] = {&idx};
        bool ok = false;
        void *task = invokeRaw(mItem, list, args, "get_Item", &ok);
        if (!ok || !task) { failed++; continue; }

        void *tk = p_object_get_class(task);
        const char *tname = tk ? p_class_get_name(tk) : "?";
        if (g_discoveryLogged.fetch_add(1) < 1) logClassNames(tk, "task");

        // heuristic by NAME: sabotage/system fix-it tasks are not "my tasks" (logged, easy to remove)
        if (strstr(tname, "Sabotage")) { skippedSystem++; LOGI("MENUTEST task[%d] %s skipped (system task by name)", i, tname); continue; }

        void *mIsComplete = findMethod(tk, "get_IsComplete", 0);
        bool already = false;
        if (mIsComplete && invokeBool(mIsComplete, task, "get_IsComplete", &already) && already) {
            skippedDone++;
            continue;
        }

        bool completed = false;
        // route 1: LocalPlayer.RpcCompleteTask(task.Id)  (the game's own networked completion path)
        // v8: the Id field is an auto-property, so its real name is "<Id>k__BackingField" (seen in the v7 log);
        // try that, then "Id", then the getter get_Id().
        void *fId = findField(tk, "<Id>k__BackingField");
        if (!fId) fId = findField(tk, "Id");
        void *mGetId = fId ? nullptr : findMethod(tk, "get_Id", 0);
        if (m_RpcCompleteTask && (fId || mGetId)) {
            void *pt = p_method_get_param(m_RpcCompleteTask, 0);
            int ptype = pt ? p_type_get_type(pt) : -1;
            if (isIntegerType(ptype)) {
                unsigned char idbuf[16];
                memset(idbuf, 0, sizeof(idbuf));
                bool haveId = false;
                if (fId) {
                    p_field_get_value(task, fId, idbuf);
                    haveId = true;
                } else {
                    bool okg = false;
                    void *boxed = invokeRaw(mGetId, task, nullptr, "get_Id", &okg);
                    void *raw = (okg && boxed) ? p_object_unbox(boxed) : nullptr;
                    if (raw) { memcpy(idbuf, raw, 4); haveId = true; }
                }
                if (haveId && *(unsigned int *)idbuf == 255u) {
                    // v9: Id 255 was seen on ReactorTask (a sabotage fix task) in the v8 log; treated as a system task (observation-based guess)
                    skippedSystem++;
                    LOGI("MENUTEST task[%d] %s skipped (Id 255 = system/sabotage task)", i, tname);
                    continue;
                }
                if (haveId) {
                    LOGI("MENUTEST task[%d] %s Id=%u (via %s), calling RpcCompleteTask", i, tname, *(unsigned int *)idbuf, fId ? "field" : "get_Id");
                    void *a2[1] = {idbuf};
                    bool ok2 = false;
                    invokeRaw(m_RpcCompleteTask, local, a2, "RpcCompleteTask", &ok2);
                    if (ok2) { completed = true; viaRpc++; LOGI("MENUTEST task[%d] %s completed via RpcCompleteTask(Id)", i, tname); }
                } else {
                    LOGI("MENUTEST task[%d] %s could not read Id; route 1 skipped", i, tname);
                }
            } else {
                LOGI("MENUTEST task[%d] RpcCompleteTask param type=0x%x is not an integer; route 1 skipped", i, ptype);
            }
        } else {
            LOGI("MENUTEST task[%d] %s route 1 unavailable (RpcCompleteTask=%p IdField=%p get_Id=%p)", i, tname, m_RpcCompleteTask, fId, mGetId);
        }
        // route 2: the task's own Complete()
        if (!completed) {
            void *mComplete = findMethod(tk, "Complete", 0);
            if (mComplete) {
                bool ok3 = false;
                invokeRaw(mComplete, task, nullptr, "task.Complete", &ok3);
                if (ok3) { completed = true; viaComplete++; LOGI("MENUTEST task[%d] %s completed via Complete()", i, tname); }
            } else {
                LOGI("MENUTEST task[%d] %s route 2 unavailable (no Complete())", i, tname);
            }
        }
        if (completed) done++; else failed++;
    }

    // re-read what the game says now (only if the game exposes get_IsComplete)
    int nowComplete = -1;
    {
        int c = 0, known = 0;
        for (int i = 0; i < count; i++) {
            int idx = i;
            void *args[1] = {&idx};
            bool ok = false;
            void *task = invokeRaw(mItem, list, args, "get_Item", &ok);
            if (!ok || !task) continue;
            void *tk = p_object_get_class(task);
            void *mIsComplete = tk ? findMethod(tk, "get_IsComplete", 0) : nullptr;
            bool v = false;
            if (mIsComplete && invokeBool(mIsComplete, task, "get_IsComplete", &v)) { known++; if (v) c++; }
        }
        if (known == count) nowComplete = c;
    }
    if (nowComplete >= 0) {
        setStatus("Tasks: %d total. Called: %d (Rpc %d, Complete %d). Already done: %d. System skipped: %d. Failed: %d. Game now reports %d/%d complete.",
                  count, done, viaRpc, viaComplete, skippedDone, skippedSystem, failed, nowComplete, count);
    } else {
        setStatus("Tasks: %d total. Called: %d (Rpc %d, Complete %d). Already done: %d. System skipped: %d. Failed: %d. (Game gave no completion flag to re-check.)",
                  count, done, viaRpc, viaComplete, skippedDone, skippedSystem, failed);
    }
}

static void runRequest() {
    if (nowMs() - g_reqAt.load() > kReqTtlMs) {
        setStatus("Press expired: no player object was active within %ld s. Join a match and press again.", kReqTtlMs / 1000);
        return;
    }
    if (!guarded(workFn, nullptr)) {
        setStatus("Crash guard caught a fault while completing tasks (see logcat MENUTEST).");
        LOGI("MENUTEST workFn FAULT");
    }
}

// ---------------------------------------------------------------- main-thread dispatch: hook on PlayerControl::FixedUpdate
typedef void (*fn_fixedupdate_t)(void *self, const void *method);
static fn_fixedupdate_t orig_fixed = nullptr;

static void hk_fixed(void *self, const void *method) {
    if (orig_fixed) orig_fixed(self, method);
    if (g_req.load(std::memory_order_relaxed) && g_req.exchange(false)) runRequest();
    if (g_reqPlayers.load(std::memory_order_relaxed) && g_reqPlayers.exchange(false)) runPlayers();
}

// ---------------------------------------------------------------- watcher thread: waits, resolves, hooks
struct DomainArg { void *domain; };
static void domainFn(void *a) { ((DomainArg *)a)->domain = p_domain_get(); }

struct AsmArg { void *domain; void *image; size_t count; };
static void asmFn(void *a) {
    AsmArg *x = (AsmArg *)a;
    x->count = 0;
    void **asms = p_domain_get_assemblies(x->domain, &x->count);
    if (!asms || x->count == 0 || x->count > 4096) return;
    for (size_t i = 0; i < x->count; i++) {
        void *img = p_assembly_get_image(asms[i]);
        const char *nm = img ? p_image_get_name(img) : nullptr;
        if (nm && strstr(nm, "Assembly-CSharp") && !strstr(nm, "firstpass")) { x->image = img; return; }
    }
}

struct ResolveArg { bool ok; };
static void resolveGameFn(void *a) {
    ResolveArg *r = (ResolveArg *)a;
    r->ok = false;
    k_PlayerControl = p_class_from_name(g_image, "", "PlayerControl");
    LOGI("MENUTEST class PlayerControl: %s (%p)", k_PlayerControl ? "FOUND" : "NOT FOUND", k_PlayerControl);
    if (!k_PlayerControl) return;
    f_LocalPlayer = findField(k_PlayerControl, "LocalPlayer");
    f_myTasks = findField(k_PlayerControl, "myTasks");
    m_RpcCompleteTask = findMethod(k_PlayerControl, "RpcCompleteTask", 1);
    m_FixedUpdate = findMethod(k_PlayerControl, "FixedUpdate", 0);
    LOGI("MENUTEST field LocalPlayer=%p myTasks=%p, method RpcCompleteTask=%p FixedUpdate=%p",
         f_LocalPlayer, f_myTasks, m_RpcCompleteTask, m_FixedUpdate);
    r->ok = (f_LocalPlayer != nullptr && f_myTasks != nullptr);
}

static void *watcher(void *) {
    const long t0 = g_t0 = nowMs();
    char path[512];
    unsigned long base = 0;

    while (nowMs() - t0 < kGiveUpMs) {
        if (findInMaps("libil2cpp.so", &base, path, sizeof(path))) break;
        usleep(100 * 1000);
    }
    if (base == 0) {
        g_state.store(ST_ERROR);
        setStatus("Menu error: libil2cpp.so never appeared.");
        return nullptr;
    }
    LOGI("MENUTEST libil2cpp.so seen after %ld ms, base=0x%lx", nowMs() - t0, base);

    void *h = xdl_open("libil2cpp.so", XDL_DEFAULT);
    if (!h) h = xdl_open(path, XDL_DEFAULT);
    LOGI("MENUTEST xdl_open handle=%p", h);
    if (!h || !resolveAll(h)) {
        g_state.store(ST_ERROR);
        if (!h) setStatus("Menu error: could not open libil2cpp.so symbols.");
        return nullptr;
    }

    // NO il2cpp call before the delay (calling it early crashed the game in v4/v5)
    while (nowMs() - t0 < kStartDelayMs) {
        setStatus("Menu loading: waiting for the game (%ld/%ld s)...", (nowMs() - t0) / 1000, kStartDelayMs / 1000);
        usleep(1000 * 1000);
    }
    LOGI("MENUTEST start delay over at %ld ms", nowMs() - t0);

    void *domain = nullptr;
    int tries = 0, faults = 0;
    while (nowMs() - t0 < kGiveUpMs && faults < 40) {
        DomainArg da = {nullptr};
        bool ok = guarded(domainFn, &da);
        tries++;
        if (!ok) { faults++; LOGI("MENUTEST domain_get FAULT #%d at %ld ms", faults, nowMs() - t0); }
        else if (da.domain) { domain = da.domain; break; }
        usleep(500 * 1000);
    }
    if (!domain) {
        g_state.store(ST_ERROR);
        setStatus("Menu error: il2cpp domain not available (tries=%d faults=%d).", tries, faults);
        return nullptr;
    }
    g_domain = domain;
    LOGI("MENUTEST il2cpp domain=%p after %ld ms", domain, nowMs() - t0);
    usleep(1500 * 1000);

    AsmArg aa = {domain, nullptr, 0};
    int asmFaults = 0;
    while (nowMs() - t0 < kGiveUpMs && !aa.image && asmFaults < 10) {
        if (!guarded(asmFn, &aa)) { asmFaults++; LOGI("MENUTEST assembly scan FAULT #%d", asmFaults); }
        if (!aa.image) usleep(1000 * 1000);
    }
    if (!aa.image) {
        g_state.store(ST_ERROR);
        setStatus("Menu error: Assembly-CSharp not found (faults=%d).", asmFaults);
        return nullptr;
    }
    g_image = aa.image;
    LOGI("MENUTEST Assembly-CSharp image=%p after %ld ms", g_image, nowMs() - t0);

    void *thr = nullptr;
    {
        // attach this helper thread to the il2cpp runtime (needed for il2cpp calls from it)
        struct A { void *thr; } a = {nullptr};
        auto fn = [](void *p) { ((A *)p)->thr = p_thread_attach(g_domain); };
        if (!guarded(fn, &a)) { g_state.store(ST_ERROR); setStatus("Menu error: il2cpp_thread_attach faulted."); return nullptr; }
        thr = a.thr;
    }
    LOGI("MENUTEST thread attached=%p", thr);

    ResolveArg ra = {false};
    if (!guarded(resolveGameFn, &ra) || !ra.ok) {
        g_state.store(ST_ERROR);
        setStatus("Menu error: PlayerControl / LocalPlayer / myTasks not found in this game version.");
        return nullptr;
    }

    // main-thread dispatch via hook; verify the target really is code inside libil2cpp first
    if (m_FixedUpdate) {
        void *target = *(void **)m_FixedUpdate; // first member of MethodInfo = native code pointer [il2cpp layout, verified here by range]
        if (target && inIl2cppText((unsigned long)target)) {
            int r = DobbyHook(target, (void *)hk_fixed, (void **)&orig_fixed);
            if (r == 0 && orig_fixed) {
                g_hooked.store(true);
                LOGI("MENUTEST hook on PlayerControl::FixedUpdate installed (target=%p)", target);
            } else {
                LOGI("MENUTEST DobbyHook failed r=%d; using helper-thread fallback", r);
            }
        } else {
            LOGI("MENUTEST FixedUpdate pointer %p is not inside libil2cpp text; using helper-thread fallback", target);
        }
    } else {
        LOGI("MENUTEST PlayerControl::FixedUpdate not found; using helper-thread fallback");
    }

    g_state.store(ST_READY);
    setStatus("Menu ready (%s). Join a match and press 'Complete my tasks'.",
              g_hooked.load() ? "main-thread mode" : "helper-thread mode, less safe");

    if (g_hooked.load()) {
        // requests are served on the Unity thread by the hook; this thread is done
        if (thr && p_thread_detach) p_thread_detach(thr);
        LOGI("MENUTEST watcher done (hook mode) after %ld ms", nowMs() - t0);
        return nullptr;
    }
    // fallback: serve requests here (stays attached)
    for (;;) {
        usleep(200 * 1000);
        if (g_req.exchange(false)) runRequest();
        if (g_reqPlayers.exchange(false)) runPlayers();
    }
    return nullptr;
}

__attribute__((constructor))
static void menutest_ctor() {
    LOGI("MENUTEST v9 loaded in pid=%d uid=%d", getpid(), getuid());
    pthread_t th;
    if (pthread_create(&th, nullptr, watcher, nullptr) == 0) {
        pthread_detach(th);
    } else {
        LOGI("MENUTEST pthread_create failed");
        g_state.store(ST_ERROR);
        setStatus("Menu error: could not start the helper thread.");
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    LOGI("MENUTEST JNI_OnLoad called, pid=%d", getpid());
    return JNI_VERSION_1_6;
}

// ---------------------------------------------------------------- JNI bridge (class top.niunaijun.blackboxa.menu.NativeMenu)
// returns: 1 = request queued, 0 = not ready yet / error (see statusText)
extern "C" JNIEXPORT jint JNICALL
Java_top_niunaijun_blackboxa_menu_NativeMenu_requestCompleteTasks(JNIEnv *env, jobject thiz) {
    int st = g_state.load();
    if (st != ST_READY) {
        LOGI("MENUTEST button pressed but menu state=%d", st);
        return 0;
    }
    setStatus("Working on your tasks...");
    g_reqAt.store(nowMs());
    g_req.store(true);
    return 1;
}

extern "C" JNIEXPORT jstring JNICALL
Java_top_niunaijun_blackboxa_menu_NativeMenu_statusText(JNIEnv *env, jobject thiz) {
    char buf[600];
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        strncpy(buf, g_status, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
    }
    return env->NewStringUTF(buf);
}

// v9: player info. requestPlayers queues a read on the Unity thread; playersText returns the last result.
extern "C" JNIEXPORT jint JNICALL
Java_top_niunaijun_blackboxa_menu_NativeMenu_requestPlayers(JNIEnv *env, jobject thiz) {
    if (g_state.load() != ST_READY) return 0;
    g_reqPlayers.store(true);
    return 1;
}

extern "C" JNIEXPORT jstring JNICALL
Java_top_niunaijun_blackboxa_menu_NativeMenu_playersText(JNIEnv *env, jobject thiz) {
    if (g_state.load() != ST_READY) return env->NewStringUTF("Menu not ready yet (wait for 'Menu ready').");
    char buf[4200];
    long at = g_playersAt.load();
    {
        std::lock_guard<std::mutex> lk(g_playersMx);
        const char *pre = (at == 0 || nowMs() - at > 3000) ? "(no fresh update: no player object is active, join a lobby or match)\n" : "";
        snprintf(buf, sizeof(buf), "%s%s", pre, g_players);
    }
    return env->NewStringUTF(buf);
}
