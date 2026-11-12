// MENUTEST v16 = v15 (Always Imposter swapped DURING SelectRoles via RpcSetRole hook) +
//   * "Swap now" removed completely (button, JNI function, old after-match swap code)
//   * player text is "Name(Colour)" with exactly ONE bracket pair (the game's colour text is stripped of its own brackets first)
//   * RPCSETROLE / V15 REDIRECT / SAFETY / ROLES log lines show player names (pointer->name table filled outside the hooks)
//   * 4 visual tasks: Submit Scan, Clear Asteroids, Empty Trash, Prime Shields (works for crew AND impostor)
// Build for game 2026.11.3 (Unity 2022.3.62f3, il2cpp). NOT built / NOT tested when written.
// First feature (still here): "Complete my tasks".
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
#include <random>
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
typedef void *(*fn_method_get_return_type_t)(void *method);

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
static fn_method_get_return_type_t p_method_get_return_type;

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
        {"il2cpp_method_get_return_type", (void **)&p_method_get_return_type, false},
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
static void *m_get_ColorName = nullptr;    // NetworkedPlayerInfo.get_ColorName() (colour name text, e.g. "Cyan")
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
        m_get_ColorName = findMethod(k_NetInfo, "get_ColorName", 0);
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
    unsigned rt = 0;
    if (!data || g_roleEnumCount == 0 || !readU16(data, f_RoleType, &rt)) return false;
    *known = true;
    // v12: only RoleType is used. RoleWhenAlive is not a plain enum value (v11 misread it and blocked crew players as "impostor").
    // A dead impostor shows RoleType=ImpostorGhost, which also contains "Impostor".
    return nameIsImpostorTeam(roleName(rt));
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

// v16: the game's colour text may already come wrapped in brackets (seen by the user as "Name ((Cyan))").
// Strip any brackets/spaces around it, so the result has NO brackets, then fmtNameColour adds exactly ONE pair.
static void stripBrackets(const char *in, char *out, size_t cap) {
    out[0] = 0;
    if (!in || cap < 2) return;
    size_t s = 0, e = strlen(in);
    while (s < e && (in[s] == '(' || in[s] == '[' || in[s] == ' ' || in[s] == '\t')) s++;
    while (e > s && (in[e - 1] == ')' || in[e - 1] == ']' || in[e - 1] == ' ' || in[e - 1] == '\t')) e--;
    size_t n = e - s;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, in + s, n);
    out[n] = 0;
}
// "Name(Colour)" with exactly one bracket pair; just "Name" if the colour is empty
static void fmtNameColour(char *out, size_t cap, const char *name, const char *colourPlain) {
    if (colourPlain && colourPlain[0]) snprintf(out, cap, "%s(%s)", name, colourPlain);
    else snprintf(out, cap, "%s", name);
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
        unsigned rt = 0;
        bool haveRole = readU16(data, f_RoleType, &rt);
        char role[80];
        if (!haveRole) snprintf(role, sizeof(role), "role unknown");
        else if (roleName(rt)) snprintf(role, sizeof(role), "%s", roleName(rt));
        else snprintf(role, sizeof(role), "role#%u", rt);
        char col[48];
        col[0] = 0;
        if (m_get_ColorName) {
            bool okc = false;
            void *cs = invokeRaw(m_get_ColorName, data, nullptr, "get_ColorName", &okc);
            char tmp[40];
            tmp[0] = 0;
            if (okc) il2str(cs, tmp, sizeof(tmp));
            stripBrackets(tmp, col, sizeof(col));      // v16: no double brackets
        }
        char shown[170];
        fmtNameColour(shown, sizeof(shown), name, col);
        if (haveDead) { if (isDead) dead++; else alive++; }
        lo += snprintf(lines + lo, sizeof(lines) - lo, "%s%s | %s | %s%s\n", shown, (pc == local) ? " (you)" : "", role,
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
        // v10: resolveExtraFn is already done by runRequest() BEFORE workFn. Do not call guarded() here:
        // workFn already runs inside guarded() and its mutex is not recursive (v9 deadlocked the game here).
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
    if (!guarded(resolveExtraFn, nullptr)) {
        setStatus("Fault while resolving player/role classes (see logcat MENUTEST).");
        return;
    }
    if (!guarded(workFn, nullptr)) {
        setStatus("Crash guard caught a fault while completing tasks (see logcat MENUTEST).");
        LOGI("MENUTEST workFn FAULT");
    }
}

// ---------------------------------------------------------------- v11: host tools (Always Imposter / role swap)
// Everything is found BY NAME at run time. Nothing here is verified against the real game yet; every name that is
// not found disables the feature with a message instead of guessing.
static void *k_AmongUsClient = nullptr, *k_ShipStatus = nullptr;
static void *f_ACInst = nullptr, *m_ACInst = nullptr, *m_AmHost = nullptr;     // AmongUsClient.Instance / get_AmHost
static void *f_ShipInst = nullptr, *m_ShipInst = nullptr;                      // ShipStatus.Instance (exists only in a match)
static void *f_roleAssigned = nullptr;                                         // PlayerControl.roleAssigned
static void *m_RMSelectRoles = nullptr;                                          // v14: RoleManager.SelectRoles/0 (log-only hook)
static void *m_RpcSetRole = nullptr, *m_CoSetRole = nullptr;                   // looked up in resolveGameFn
static void *k_RoleManager = nullptr, *f_RMInst = nullptr, *m_RMInst = nullptr, *m_RMSetRole = nullptr;   // RoleManager.Instance.SetRole(player, role)
static bool g_hostDone = false;
static std::atomic<int> g_alwaysImp{0};
static std::atomic<int> g_wantIdx{0};                 // 0 = random impostor role (take the swapped impostor's role)
static std::atomic<bool> g_reqHostInfo{false};
static const char *kWantNames[] = {"Random", "Impostor", "Shapeshifter", "Phantom", "Viper"};
static const int kWantCount = 5;
static std::mutex g_hostMx;
static char g_hostInfo[1400] = "Host tools: not read yet.";
static char g_swapMsg[500] = "No swap done yet.";
static std::atomic<long> g_hostInfoAt{0};
static void setSwapMsg(const char *fmt, ...) {
    char buf[500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_hostMx);
        changed = strcmp(buf, g_swapMsg) != 0;
        strncpy(g_swapMsg, buf, sizeof(g_swapMsg) - 1);
        g_swapMsg[sizeof(g_swapMsg) - 1] = 0;
    }
    if (changed) LOGI("MENUTEST SWAP: %s", buf);
}
static void setHostInfo(const char *txt) {
    std::lock_guard<std::mutex> lk(g_hostMx);
    strncpy(g_hostInfo, txt, sizeof(g_hostInfo) - 1);
    g_hostInfo[sizeof(g_hostInfo) - 1] = 0;
    g_hostInfoAt.store(nowMs());
}

static void resolveHostFn(void *) {
    if (g_hostDone) return;
    g_hostDone = true;
    k_AmongUsClient = p_class_from_name(g_image, "", "AmongUsClient");
    k_ShipStatus = p_class_from_name(g_image, "", "ShipStatus");
    if (k_AmongUsClient) {
        f_ACInst = findField(k_AmongUsClient, "Instance");
        if (!f_ACInst) f_ACInst = findField(k_AmongUsClient, "_instance");
        m_ACInst = findMethod(k_AmongUsClient, "get_Instance", 0);
        m_AmHost = findMethod(k_AmongUsClient, "get_AmHost", 0);
    }
    if (k_ShipStatus) {
        f_ShipInst = findField(k_ShipStatus, "Instance");
        if (!f_ShipInst) f_ShipInst = findField(k_ShipStatus, "_instance");
        m_ShipInst = findMethod(k_ShipStatus, "get_Instance", 0);
    }
    k_RoleManager = p_class_from_name(g_image, "", "RoleManager");
    if (k_RoleManager) {
        f_RMInst = findField(k_RoleManager, "Instance");
        if (!f_RMInst) f_RMInst = findField(k_RoleManager, "_instance");
        m_RMInst = findMethod(k_RoleManager, "get_Instance", 0);
        m_RMSetRole = findMethod(k_RoleManager, "SetRole", 2);
        m_RMSelectRoles = findMethod(k_RoleManager, "SelectRoles", 0);
        if (p_class_get_methods && p_method_get_name) {
            void *iter = nullptr, *m;
            int cnt = 0;
            while ((m = p_class_get_methods(k_RoleManager, &iter)) != nullptr && cnt++ < 60)
                LOGI("MENUTEST   RoleManager method %s", p_method_get_name(m));
        }
    }
    LOGI("MENUTEST RoleManager=%p inst(field=%p,getter=%p) SetRole/2=%p", k_RoleManager, f_RMInst, m_RMInst, m_RMSetRole);
    f_roleAssigned = findField(k_PlayerControl, "roleAssigned");
    LOGI("MENUTEST host: AmongUsClient=%p inst(field=%p,getter=%p) get_AmHost=%p | ShipStatus=%p inst(field=%p,getter=%p) | roleAssigned=%p | RpcSetRole=%p CoSetRole=%p",
         k_AmongUsClient, f_ACInst, m_ACInst, m_AmHost, k_ShipStatus, f_ShipInst, m_ShipInst, f_roleAssigned, m_RpcSetRole, m_CoSetRole);
}

static void *staticObj(void *field, void *getter) {
    void *o = nullptr;
    if (field) { p_field_static_get_value(field, &o); if (o) return o; }
    if (getter) { bool ok = false; void *r = invokeRaw(getter, nullptr, nullptr, "get_Instance", &ok); if (ok) return r; }
    return nullptr;
}
// 1 = you are host, 0 = not host, -1 = could not tell
static int hostState() {
    void *ac = staticObj(f_ACInst, m_ACInst);
    if (!ac || !m_AmHost) return -1;
    bool h = false;
    if (!invokeBool(m_AmHost, ac, "get_AmHost", &h)) return -1;
    return h ? 1 : 0;
}
// 1 = in a match (ShipStatus exists), 0 = lobby/menu, -1 = could not tell
static int inMatchState() {
    if (!f_ShipInst && !m_ShipInst) return -1;
    return staticObj(f_ShipInst, m_ShipInst) ? 1 : 0;
}
static const char *rn(unsigned v) { const char *n = roleName(v); return n ? n : "?"; }
struct PInfo { void *pc; void *data; unsigned role; bool haveRole, dead, disc, assigned; char name[96]; };   // name = "Name(Colour)"

// reads every player (host sees everyone's role). returns count or -1
static int collectPlayers(PInfo *out, int cap, void **localOut) {
    void *local = nullptr;
    if (f_LocalPlayer) p_field_static_get_value(f_LocalPlayer, &local);
    *localOut = local;
    void *list = nullptr;
    if (f_AllPlayers) p_field_static_get_value(f_AllPlayers, &list);
    if (!list) return -1;
    void *lk = p_object_get_class(list);
    void *mCount = findMethod(lk, "get_Count", 0);
    void *mItem = findMethod(lk, "get_Item", 1);
    int count = 0;
    if (!mCount || !mItem || !invokeInt(mCount, list, nullptr, "get_Count", &count)) return -1;
    if (count < 0 || count > kMaxPlayers) return -1;
    int n = 0;
    for (int i = 0; i < count && n < cap; i++) {
        int idx = i;
        void *args[1] = {&idx};
        bool ok = false;
        void *pc = invokeRaw(mItem, list, args, "get_Item", &ok);
        void *data = ok ? dataOf(pc) : nullptr;
        if (!data) continue;
        PInfo &p = out[n];
        memset(&p, 0, sizeof(p));
        p.pc = pc;
        p.data = data;
        p.haveRole = readU16(data, f_RoleType, &p.role);
        readBoolField(data, f_IsDead, &p.dead);
        readBoolField(data, f_Disconnected, &p.disc);
        p.assigned = true;
        if (f_roleAssigned) readBoolField(pc, f_roleAssigned, &p.assigned);
        char nm[48];
        nm[0] = 0;
        if (m_get_PlayerName) {
            bool okn = false;
            void *s = invokeRaw(m_get_PlayerName, data, nullptr, "get_PlayerName", &okn);
            if (okn) il2str(s, nm, sizeof(nm));
        }
        if (!nm[0]) snprintf(nm, sizeof(nm), "player#%d", i);
        char col[40];
        col[0] = 0;
        if (m_get_ColorName) {
            bool okc = false;
            void *cs = invokeRaw(m_get_ColorName, data, nullptr, "get_ColorName", &okc);
            char tmp[40];
            tmp[0] = 0;
            if (okc) il2str(cs, tmp, sizeof(tmp));
            stripBrackets(tmp, col, sizeof(col));
        }
        fmtNameColour(p.name, sizeof(p.name), nm, col);   // v16: "Name(Colour)", one bracket pair
        n++;
    }
    return n;
}

typedef void (*fn_rpcsetrole_t)(void *self, uint64_t role, uint64_t canOverride, const void *method);
static fn_rpcsetrole_t orig_rpcsetrole = nullptr;
typedef void (*fn_selectroles_t)(void *self, const void *method);
static fn_selectroles_t orig_selectroles = nullptr;
// v15 state of the swap-while-assigning (touched only by the Unity thread inside SelectRoles)
static std::atomic<int> g_inSelect{0};
static void *g_displaced = nullptr;            // the impostor who lost his role to me
static bool g_localImpDone = false, g_haveLocalCrew = false, g_crewGiven = false;
static unsigned g_localCrewRole = 0;
static const void *g_lastMethod = nullptr;
static const unsigned kWantVals[] = {0, 1, 5, 9, 18};   // Random, Impostor, Shapeshifter, Phantom, Viper (values from the game dump)
static inline bool isImpRoleVal(unsigned v) { return v == 1 || v == 5 || v == 9 || v == 18; }
static std::atomic<long> g_assignLogAt{0};   // v14: after SelectRoles returns, log all roles with pointers once
// v13: one log line with every player's role as THIS (host) phone sees it. Compare with what the other phone's screen shows.
// Log tag example: "MENUTEST ROLES[after-1.5s]: me=Viper | JC=Scientist | Bob=Phantom(dead)"
static void logRoles(const char *tag) {
    PInfo ps[kMaxPlayers];
    void *local = nullptr;
    int n = collectPlayers(ps, kMaxPlayers, &local);
    if (n <= 0) { LOGI("MENUTEST ROLES[%s]: could not read players", tag); return; }
    char buf[1800];
    int lo = 0;
    for (int i = 0; i < n && lo < (int)sizeof(buf) - 160; i++) {
        lo += snprintf(buf + lo, sizeof(buf) - lo, "%s%s%s@%p=%s%s%s", i ? " | " : "",
                       ps[i].name, ps[i].pc == local ? "[me]" : "", ps[i].pc, ps[i].haveRole ? rn(ps[i].role) : "?",
                       ps[i].dead ? "(dead)" : "", ps[i].disc ? "(left)" : "");
    }
    LOGI("MENUTEST ROLES[%s]: %s", tag, buf);
}
static void assignLogFn(void *) { logRoles("after-SelectRoles"); }
static void hostInfoFn(void *) {
    int h = hostState(), m = inMatchState();
    char mine[80] = "unknown";
    void *local = nullptr;
    if (f_LocalPlayer) p_field_static_get_value(f_LocalPlayer, &local);
    if (local) {
        void *d = dataOf(local);
        unsigned r = 0;
        if (d && readU16(d, f_RoleType, &r)) snprintf(mine, sizeof(mine), "%s", rn(r));
    }
    char swapmsg[500];
    {
        std::lock_guard<std::mutex> lk(g_hostMx);
        strncpy(swapmsg, g_swapMsg, sizeof(swapmsg) - 1);
        swapmsg[sizeof(swapmsg) - 1] = 0;
    }
    int wi = g_wantIdx.load();
    if (wi < 0 || wi >= kWantCount) wi = 0;
    char buf[1400];
    snprintf(buf, sizeof(buf), "Host: %s\nIn match: %s\nYour role: %s\nAlways Imposter: %s\nImposter role: %s\nLast: %s",
             h == 1 ? "YES" : (h == 0 ? "NO" : "unknown"), m == 1 ? "YES" : (m == 0 ? "NO (lobby/menu)" : "unknown"), mine,
             g_alwaysImp.load() ? "ON" : "OFF", kWantNames[wi], swapmsg);
    setHostInfo(buf);
}

static void runHostInfo() {
    if (!guarded(resolveExtraFn, nullptr) || !guarded(resolveHostFn, nullptr)) { setHostInfo("Host: unknown\nFault while resolving host classes (see logcat MENUTEST)."); return; }
    if (!guarded(hostInfoFn, nullptr)) setHostInfo("Host: unknown\nCrash guard caught a fault while reading host info (see logcat MENUTEST).");
}

// after SelectRoles returns, one ROLES line is logged ~2.5 s later (names + roles of everybody)
static void logTick() {
    long al = g_assignLogAt.load();
    if (al && nowMs() >= al && g_assignLogAt.exchange(0) == al) {
        if (!guarded(assignLogFn, nullptr)) LOGI("MENUTEST assignLogFn FAULT");
    }
}

// ---------------------------------------------------------------- v16: pointer -> "Name(Colour)" table (for the log lines)
// The RpcSetRole / SelectRoles hooks must NOT call il2cpp (names are not always ready inside SelectRoles), so the table is
// filled earlier, on the Unity thread, every ~1.5 s while players exist (lobby!) and once more right before SelectRoles.
// The hooks only read plain memory from this table. A pointer that is not in the table is printed as "name-unknown".
struct NameEnt { void *pc; char name[100]; };
static NameEnt g_names[kMaxPlayers];
static int g_nameCount = 0;
static std::atomic<int> g_namesBusy{0};
static long g_namesAt = 0;

static void refreshNamesFn(void *) {
    PInfo ps[kMaxPlayers];
    void *local = nullptr;
    int n = collectPlayers(ps, kMaxPlayers, &local);
    if (n <= 0) return;
    g_namesBusy.store(1);
    if (n > kMaxPlayers) n = kMaxPlayers;
    for (int i = 0; i < n; i++) {
        g_names[i].pc = ps[i].pc;
        snprintf(g_names[i].name, sizeof(g_names[i].name), "%s%s", ps[i].name, ps[i].pc == local ? "[me]" : "");
    }
    g_nameCount = n;
    g_namesBusy.store(0);
}
static const char *nameOf(void *pc) {
    if (!pc || g_namesBusy.load()) return nullptr;
    for (int i = 0; i < g_nameCount; i++) if (g_names[i].pc == pc) return g_names[i].name;
    return nullptr;
}
static const char *pname(void *pc) { const char *n = nameOf(pc); return n ? n : "name-unknown"; }
// refresh the table if it is older than maxAgeMs (Unity thread only)
static void nameTick(long maxAgeMs) {
    long now = nowMs();
    if (now - g_namesAt < maxAgeMs) return;
    g_namesAt = now;
    if (!f_AllPlayers) {
        if (!guarded(resolveExtraFn, nullptr)) return;
        if (!f_AllPlayers) return;
    }
    if (!f_roleAssigned && !guarded(resolveHostFn, nullptr)) return;
    if (!guarded(refreshNamesFn, nullptr)) LOGI("MENUTEST refreshNamesFn FAULT");
}

// ---------------------------------------------------------------- v16: visual tasks (Submit Scan, Clear Asteroids, Empty Trash, Prime Shields)
// Source: dump.cs of 2026.11.3. NOT tested when written. No role check on purpose (works as crew and as impostor).
//  * Asteroids / Trash / Shields: PlayerControl.RpcPlayAnimation(byte animType) with animType = TaskTypes value
//    (ClearAsteroids=6, EmptyGarbage=10, PrimeShields=1). [GUESS: the game uses the TaskTypes number as animType]
//  * Submit Scan: the hologram is driven by MedScanSystem (SystemTypes.MedBay=10): ShipStatus.RpcUpdateSystem(MedBay, 128|playerId)
//    = "I use the scanner", and 64|playerId = "I am done" (Request=128, Release=64, NumMask=31 are constants in dump.cs).
//  * Whether the lobby setting "Visual Tasks: OFF" blocks the animation on the OTHER phones is [UNKNOWN]; the test shows it.
static const unsigned kAnimShields = 1, kAnimAsteroids = 6, kAnimGarbage = 10;
static const unsigned kSysMedBay = 10;
enum { VIS_SCAN = 1, VIS_ASTEROIDS = 2, VIS_TRASH = 3, VIS_SHIELDS = 4 };
static const int  kAsteroidShots = 20;     // the real task needs 20 destroyed asteroids; one laser per shot
static const long kAsteroidGapMs = 450;    // + random 0..349 ms between shots (a guess at a human rhythm)
static const long kScanMs = 10000;         // the real scan takes about 10 s

static std::atomic<int> g_reqVisual{0};
static std::atomic<long> g_reqVisualAt{0};
static std::mutex g_visMx;
static char g_visMsg[500] = "Visual tasks: not used yet.";
static void setVisMsg(const char *fmt, ...) {
    char buf[500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lk(g_visMx);
        strncpy(g_visMsg, buf, sizeof(g_visMsg) - 1);
        g_visMsg[sizeof(g_visMsg) - 1] = 0;
    }
    LOGI("MENUTEST VISUAL: %s", buf);
}
static unsigned visRnd(unsigned n) {
    static std::mt19937 g((unsigned)nowMs());
    return n ? (unsigned)(g() % n) : 0;
}

static void *m_RpcPlayAnim = nullptr, *m_RpcUpdateSysByte = nullptr;
static void *f_ShipWeapons = nullptr, *f_ShipShields = nullptr, *f_ShipHatch = nullptr, *f_ShipMedScanner = nullptr, *f_PlayerId = nullptr;
static bool g_visResolved = false;

static void resolveVisualFn(void *) {
    if (g_visResolved) return;
    g_visResolved = true;
    m_RpcPlayAnim = findMethod(k_PlayerControl, "RpcPlayAnimation", 1);
    if (m_RpcPlayAnim) {
        void *pt = p_method_get_param(m_RpcPlayAnim, 0);
        if (!pt || !isIntegerType(p_type_get_type(pt))) {
            LOGI("MENUTEST VISUAL: RpcPlayAnimation parameter is not an integer type, disabled");
            m_RpcPlayAnim = nullptr;
        }
    }
    f_PlayerId = findField(k_PlayerControl, "PlayerId");
    if (k_ShipStatus) {
        f_ShipWeapons = findField(k_ShipStatus, "WeaponsImage");
        f_ShipShields = findField(k_ShipStatus, "ShieldsImages");
        f_ShipHatch = findField(k_ShipStatus, "Hatch");
        f_ShipMedScanner = findField(k_ShipStatus, "MedScanner");
        // ShipStatus has two RpcUpdateSystem overloads with 2 parameters: (SystemTypes, byte) and (SystemTypes, MessageWriter).
        // Take the one whose 2nd parameter is an integer type (the byte one).
        if (p_class_get_methods && p_method_get_name) {
            void *iter = nullptr, *m;
            int guardN = 0;
            while ((m = p_class_get_methods(k_ShipStatus, &iter)) != nullptr && guardN++ < 600) {
                const char *mn = p_method_get_name(m);
                if (!mn || strcmp(mn, "RpcUpdateSystem") != 0) continue;
                void *p0 = p_method_get_param(m, 0), *p1 = p_method_get_param(m, 1);
                if (p0 && p1 && isIntegerType(p_type_get_type(p1))) { m_RpcUpdateSysByte = m; break; }
            }
        }
    }
    LOGI("MENUTEST VISUAL resolve: RpcPlayAnimation=%p RpcUpdateSystem(byte)=%p PlayerId=%p | ShipStatus fields: WeaponsImage=%p ShieldsImages=%p Hatch=%p MedScanner=%p",
         m_RpcPlayAnim, m_RpcUpdateSysByte, f_PlayerId, f_ShipWeapons, f_ShipShields, f_ShipHatch, f_ShipMedScanner);
}

// a ShipStatus object field that is null on this map means "this map has no such visual" (heuristic, logged)
static bool shipHas(void *ship, void *field) {
    if (!field) return true;                     // field name unknown in this build: do not block
    void *v = nullptr;
    p_field_get_value(ship, field, &v);
    return v != nullptr;
}
static bool sendAnim(void *local, unsigned anim) {
    if (!m_RpcPlayAnim) return false;
    uint64_t a = anim & 0xff;
    void *args[1] = {&a};
    bool ok = false;
    invokeRaw(m_RpcPlayAnim, local, args, "RpcPlayAnimation", &ok);
    return ok;
}
static bool sendScan(void *ship, unsigned pid, bool request) {
    if (!m_RpcUpdateSysByte) return false;
    uint64_t st = kSysMedBay;
    uint64_t amt = (request ? 128u : 64u) | (pid & 31u);
    void *args[2] = {&st, &amt};
    bool ok = false;
    invokeRaw(m_RpcUpdateSysByte, ship, args, "RpcUpdateSystem(MedBay)", &ok);
    return ok;
}

struct VisSeq { bool active; int kind; int left; long nextAt; unsigned pid; };
static VisSeq g_vs = {false, 0, 0, 0, 0};     // Unity thread only
static int g_visWhich = 0;

static void visualReqFn(void *) {
    const int which = g_visWhich;
    void *local = nullptr;
    if (f_LocalPlayer) p_field_static_get_value(f_LocalPlayer, &local);
    if (!local) { setVisMsg("No local player yet. Join a lobby or match first."); return; }
    void *ship = staticObj(f_ShipInst, m_ShipInst);
    if (!ship) { setVisMsg("No map is loaded (ShipStatus missing). Start a match or Freeplay first; the lobby has no tasks."); return; }

    // pressing the button of a running task stops it
    if (g_vs.active && g_vs.kind == which) {
        g_vs.active = false;
        if (which == VIS_SCAN) { sendScan(ship, g_vs.pid, false); setVisMsg("Submit Scan stopped (release sent)."); }
        else setVisMsg("Stopped.");
        return;
    }
    if (g_vs.active) { setVisMsg("Another visual task is still running. Press its button again to stop it first."); return; }

    long now = nowMs();
    if (which == VIS_SCAN) {
        if (!m_RpcUpdateSysByte) { setVisMsg("Submit Scan: ShipStatus.RpcUpdateSystem(byte) not found in this game build. Nothing sent."); return; }
        if (!shipHas(ship, f_ShipMedScanner)) { setVisMsg("Submit Scan: this map has no scanner (MedScanner is empty). Nothing sent."); return; }
        unsigned pid = 255;
        if (f_PlayerId) { uint64_t b = 0; p_field_get_value(local, f_PlayerId, &b); pid = (unsigned)(b & 0xff); }
        if (pid > 31) { setVisMsg("Submit Scan: your player id (%u) cannot be sent (scanner uses 0..31). Nothing sent.", pid); return; }
        if (!sendScan(ship, pid, true)) { setVisMsg("Submit Scan: the request threw an exception. Nothing else sent."); return; }
        g_vs.active = true; g_vs.kind = VIS_SCAN; g_vs.pid = pid; g_vs.left = 1; g_vs.nextAt = now + kScanMs;
        setVisMsg("Submit Scan: request sent (player id %u). Stand on the MedBay scanner pad. It ends by itself after %ld s; press again to stop.", pid, kScanMs / 1000);
        return;
    }
    if (!m_RpcPlayAnim) { setVisMsg("RpcPlayAnimation not found in this game build (or its parameter is not a byte). Nothing sent."); return; }
    if (which == VIS_ASTEROIDS) {
        if (!shipHas(ship, f_ShipWeapons)) { setVisMsg("Clear Asteroids: this map has no weapon turret (WeaponsImage is empty). Nothing sent."); return; }
        if (!sendAnim(local, kAnimAsteroids)) { setVisMsg("Clear Asteroids: RpcPlayAnimation threw an exception. Nothing else sent."); return; }
        g_vs.active = true; g_vs.kind = VIS_ASTEROIDS; g_vs.left = kAsteroidShots - 1;
        g_vs.nextAt = now + kAsteroidGapMs + (long)visRnd(350);
        setVisMsg("Clear Asteroids: shot 1/%d sent. Press again to stop.", kAsteroidShots);
        return;
    }
    if (which == VIS_TRASH) {
        if (!shipHas(ship, f_ShipHatch)) { setVisMsg("Empty Trash: this map has no garbage hatch (Hatch is empty). Nothing sent."); return; }
        if (!sendAnim(local, kAnimGarbage)) { setVisMsg("Empty Trash: RpcPlayAnimation threw an exception."); return; }
        setVisMsg("Empty Trash: animation sent (TaskTypes.EmptyGarbage=%u).", kAnimGarbage);
        return;
    }
    if (which == VIS_SHIELDS) {
        if (!shipHas(ship, f_ShipShields)) { setVisMsg("Prime Shields: this map has no shield lights (ShieldsImages is empty). Nothing sent."); return; }
        if (!sendAnim(local, kAnimShields)) { setVisMsg("Prime Shields: RpcPlayAnimation threw an exception."); return; }
        setVisMsg("Prime Shields: animation sent (TaskTypes.PrimeShields=%u).", kAnimShields);
        return;
    }
    setVisMsg("Unknown visual task %d.", which);
}

static void visualStepFn(void *) {
    if (!g_vs.active) return;
    void *local = nullptr;
    if (f_LocalPlayer) p_field_static_get_value(f_LocalPlayer, &local);
    void *ship = staticObj(f_ShipInst, m_ShipInst);
    if (!local || !ship) { g_vs.active = false; setVisMsg("Visual task stopped: the match/map is gone."); return; }
    long now = nowMs();
    if (now < g_vs.nextAt) return;
    if (g_vs.kind == VIS_ASTEROIDS) {
        if (!sendAnim(local, kAnimAsteroids)) { g_vs.active = false; setVisMsg("Clear Asteroids: RpcPlayAnimation threw an exception. Stopped."); return; }
        g_vs.left--;
        if (g_vs.left <= 0) { g_vs.active = false; setVisMsg("Clear Asteroids: all %d shots sent.", kAsteroidShots); return; }
        g_vs.nextAt = now + kAsteroidGapMs + (long)visRnd(350);
        setVisMsg("Clear Asteroids: shot %d/%d sent.", kAsteroidShots - g_vs.left, kAsteroidShots);
    } else if (g_vs.kind == VIS_SCAN) {
        bool ok = sendScan(ship, g_vs.pid, false);
        g_vs.active = false;
        setVisMsg(ok ? "Submit Scan: finished (release sent)." : "Submit Scan: the release threw an exception.");
    } else {
        g_vs.active = false;
    }
}
static void visualTick() {
    if (!g_vs.active || nowMs() < g_vs.nextAt) return;
    if (!guarded(visualStepFn, nullptr)) {
        g_vs.active = false;
        setVisMsg("Crash guard caught a fault in the visual task; stopped (see logcat MENUTEST).");
        LOGI("MENUTEST visualStepFn FAULT");
    }
}
static void runVisual() {
    int w = g_reqVisual.exchange(0);
    if (w <= 0) return;
    if (nowMs() - g_reqVisualAt.load() > kReqTtlMs) { setVisMsg("Press expired: no player object was active within %ld s. Join a match and press again.", kReqTtlMs / 1000); return; }
    if (!guarded(resolveExtraFn, nullptr) || !guarded(resolveHostFn, nullptr) || !guarded(resolveVisualFn, nullptr)) {
        setVisMsg("Fault while resolving the visual task functions (see logcat MENUTEST).");
        return;
    }
    g_visWhich = w;
    if (!guarded(visualReqFn, nullptr)) {
        g_vs.active = false;
        setVisMsg("Crash guard caught a fault while starting the visual task (see logcat MENUTEST).");
        LOGI("MENUTEST visualReqFn FAULT");
    }
}

// ---------------------------------------------------------------- main-thread dispatch: hook on PlayerControl::FixedUpdate
typedef void (*fn_fixedupdate_t)(void *self, const void *method);
static fn_fixedupdate_t orig_fixed = nullptr;

static void hk_fixed(void *self, const void *method) {
    if (orig_fixed) orig_fixed(self, method);
    if (g_req.load(std::memory_order_relaxed) && g_req.exchange(false)) runRequest();
    if (g_reqPlayers.load(std::memory_order_relaxed) && g_reqPlayers.exchange(false)) runPlayers();
    if (g_reqHostInfo.load(std::memory_order_relaxed) && g_reqHostInfo.exchange(false)) runHostInfo();
    if (g_reqVisual.load(std::memory_order_relaxed) != 0) runVisual();
    visualTick();
    nameTick(1500);
    logTick();
}

// v11: observer hook on PlayerControl::CoSetRole(role, canOverride). It only LOGS (no il2cpp calls, no guarded()), so it cannot
// deadlock or disturb the swap. Shows on a non-host phone which role messages arrive and when.
typedef void (*fn_cosetrole_t)(void *self, uint64_t role, uint64_t canOverride, const void *method);
static fn_cosetrole_t orig_cosetrole = nullptr;
static std::atomic<int> g_coSetRoleLogged{0};
static void hk_cosetrole(void *self, uint64_t role, uint64_t canOverride, const void *method) {
    if (orig_cosetrole) orig_cosetrole(self, role, canOverride, method);
    if (g_coSetRoleLogged.fetch_add(1) < 300) {
        void *local = nullptr;
        if (f_LocalPlayer) p_field_static_get_value(f_LocalPlayer, &local);
        unsigned rv = (unsigned)(role & 0xffff);
        const char *nm = roleName(rv);
        LOGI("MENUTEST CoSetRole self=%p isLocal=%d role=%u (%s) canOverride=%d", self, self == local ? 1 : 0, rv, nm ? nm : "?", (int)(canOverride & 0xff));
    }
}

// v14: LOG-ONLY hooks (they never change an argument or a result). They answer: does the game call RpcSetRole while it assigns
// roles, in which order (impostors first?), with which canOverride, and when does RoleManager.SelectRoles start/end.
// No il2cpp calls inside the hooks (only pointers/numbers are logged); names come from the "ROLES[after-SelectRoles]" line.
static std::atomic<int> g_rpcLogged{0};
static void hk_rpcsetrole(void *self, uint64_t role, uint64_t canOverride, const void *method) {
    int seq = g_rpcLogged.fetch_add(1);
    void *local = nullptr;
    if (f_LocalPlayer) p_field_static_get_value(f_LocalPlayer, &local);
    unsigned rv = (unsigned)(role & 0xffff);
    const bool inSel = g_inSelect.load() != 0;
    if (seq < 400) {
        const char *nm = roleName(rv);
        // v16: player=Name(Colour) comes from the table filled before SelectRoles (no il2cpp call in here)
        LOGI("MENUTEST RPCSETROLE #%d t=%ld self=%p player=%s isLocal=%d role=%u (%s) canOverride=%d by=GAME%s", seq, nowMs() % 1000000, self,
             pname(self), self == local ? 1 : 0, rv, nm ? nm : "?", (int)(canOverride & 0xff), inSel ? " [in SelectRoles]" : "");
    }
    g_lastMethod = method;
    // ---- v15: swap while the game assigns roles (host only: SelectRoles runs only on the host)
    if (inSel && local && orig_rpcsetrole && g_alwaysImp.load()) {
        if (self == local) {
            if (isImpRoleVal(rv)) {
                g_localImpDone = true;                       // the game itself made me an impostor: nothing to swap
            } else if (g_displaced) {
                LOGI("MENUTEST V15 REDIRECT: my crew role %u goes to the displaced impostor %p (%s)", rv, g_displaced, pname(g_displaced));
                orig_rpcsetrole(g_displaced, role, canOverride, method);
                g_crewGiven = true;
                setSwapMsg("Always Imposter: the impostor %s got your crew role (%u), you took his impostor role.", pname(g_displaced), rv);
                return;
            } else {
                g_haveLocalCrew = true; g_localCrewRole = rv; // crew role arrived before any impostor role (unexpected order); remembered
            }
        } else if (isImpRoleVal(rv) && !g_localImpDone && !g_displaced) {
            int wi = g_wantIdx.load();
            unsigned give = (wi > 0 && wi < kWantCount) ? kWantVals[wi] : rv;
            LOGI("MENUTEST V15 REDIRECT: impostor role %u of %p (%s) goes to ME as %u", rv, self, pname(self), give);
            g_displaced = self;
            g_localImpDone = true;
            if (g_haveLocalCrew) {                           // I already got a crew role: override it, and hand that role to the displaced one
                orig_rpcsetrole(local, (uint64_t)give, 1, method);
                orig_rpcsetrole(self, (uint64_t)g_localCrewRole, 1, method);
                g_crewGiven = true;
            } else {
                orig_rpcsetrole(local, (uint64_t)give, canOverride, method);
            }
            return;
        }
    }
    if (orig_rpcsetrole) orig_rpcsetrole(self, role, canOverride, method);
}
static void hk_selectroles(void *self, const void *method) {
    nameTick(800);   // v16: make sure the pointer->name table is fresh BEFORE the roles are assigned (names are read here, not in the hooks)
    LOGI("MENUTEST SELECTROLES ENTER t=%ld alwaysImp=%d names=%d", nowMs() % 1000000, g_alwaysImp.load() ? 1 : 0, g_nameCount);
    g_displaced = nullptr; g_localImpDone = false; g_haveLocalCrew = false; g_crewGiven = false; g_localCrewRole = 0;
    g_inSelect.store(1);
    if (orig_selectroles) orig_selectroles(self, method);
    g_inSelect.store(0);
    if (g_displaced && !g_crewGiven && orig_rpcsetrole) {   // safety net: the displaced impostor must never stay without a role
        LOGI("MENUTEST V15 SAFETY: displaced %p (%s) got no crew role, giving Crewmate", g_displaced, pname(g_displaced));
        orig_rpcsetrole(g_displaced, 0, 1, g_lastMethod);
    }
    LOGI("MENUTEST SELECTROLES EXIT t=%ld swapped=%d", nowMs() % 1000000, g_displaced ? 1 : 0);
    g_assignLogAt.store(nowMs() + 2500);
    g_rpcLogged.store(0);
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
    m_RpcSetRole = findMethod(k_PlayerControl, "RpcSetRole", 2);
    m_CoSetRole = findMethod(k_PlayerControl, "CoSetRole", 2);
    {   // v14: RoleManager.SelectRoles must be known at hook-install time (resolveHostFn only runs later)
        void *krm = p_class_from_name(g_image, "", "RoleManager");
        if (krm) m_RMSelectRoles = findMethod(krm, "SelectRoles", 0);
    }
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

    // v11: observer hook on CoSetRole, only if it is a void method with code inside libil2cpp
    if (m_CoSetRole && p_method_get_return_type) {
        void *rt = p_method_get_return_type(m_CoSetRole);
        void *tgt = *(void **)m_CoSetRole;
        if (rt && p_type_get_type(rt) == 0x01 && tgt && inIl2cppText((unsigned long)tgt)) {
            int r = DobbyHook(tgt, (void *)hk_cosetrole, (void **)&orig_cosetrole);
            LOGI("MENUTEST CoSetRole observer hook %s (r=%d target=%p)", (r == 0 && orig_cosetrole) ? "installed" : "NOT installed", r, tgt);
            if (r != 0) orig_cosetrole = nullptr;
        } else {
            LOGI("MENUTEST CoSetRole observer hook skipped: return type=0x%x (void is 0x1), target=%p, inside libil2cpp=%d",
                 rt ? p_type_get_type(rt) : -1, tgt, (tgt && inIl2cppText((unsigned long)tgt)) ? 1 : 0);
        }
    } else {
        LOGI("MENUTEST CoSetRole observer hook skipped (method or return-type API missing)");
    }

    // v14: log-only hooks on RpcSetRole and RoleManager.SelectRoles (both must be void methods with code inside libil2cpp)
    if (m_RpcSetRole && p_method_get_return_type) {
        void *rt = p_method_get_return_type(m_RpcSetRole);
        void *tgt = *(void **)m_RpcSetRole;
        if (rt && p_type_get_type(rt) == 0x01 && tgt && inIl2cppText((unsigned long)tgt)) {
            int r = DobbyHook(tgt, (void *)hk_rpcsetrole, (void **)&orig_rpcsetrole);
            LOGI("MENUTEST RpcSetRole log hook %s (r=%d target=%p)", (r == 0 && orig_rpcsetrole) ? "installed" : "NOT installed", r, tgt);
            if (r != 0) orig_rpcsetrole = nullptr;
        } else {
            LOGI("MENUTEST RpcSetRole log hook skipped: return type=0x%x, target=%p", rt ? p_type_get_type(rt) : -1, tgt);
        }
    }
    if (m_RMSelectRoles && p_method_get_return_type) {
        void *rt = p_method_get_return_type(m_RMSelectRoles);
        void *tgt = *(void **)m_RMSelectRoles;
        if (rt && p_type_get_type(rt) == 0x01 && tgt && inIl2cppText((unsigned long)tgt)) {
            int r = DobbyHook(tgt, (void *)hk_selectroles, (void **)&orig_selectroles);
            LOGI("MENUTEST SelectRoles log hook %s (r=%d target=%p)", (r == 0 && orig_selectroles) ? "installed" : "NOT installed", r, tgt);
            if (r != 0) orig_selectroles = nullptr;
        } else {
            LOGI("MENUTEST SelectRoles log hook skipped: return type=0x%x, target=%p", rt ? p_type_get_type(rt) : -1, tgt);
        }
    } else {
        LOGI("MENUTEST SelectRoles log hook skipped (method not found)");
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
        if (g_reqHostInfo.exchange(false)) runHostInfo();
        if (g_reqVisual.load() != 0) runVisual();
        visualTick();
        nameTick(1500);
        logTick();
    }
    return nullptr;
}

__attribute__((constructor))
static void menutest_ctor() {
    LOGI("MENUTEST v16 loaded in pid=%d uid=%d", getpid(), getuid());
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

// v11: host tools. Options are plain atomics; the work happens on the Unity thread.
extern "C" JNIEXPORT void JNICALL
Java_top_niunaijun_blackboxa_menu_NativeMenu_setHostOptions(JNIEnv *env, jobject thiz, jint on, jint roleIdx) {
    g_alwaysImp.store(on ? 1 : 0);
    g_wantIdx.store((roleIdx >= 0 && roleIdx < kWantCount) ? roleIdx : 0);
    LOGI("MENUTEST host options: alwaysImpostor=%d wantedRole=%s", on ? 1 : 0, kWantNames[(roleIdx >= 0 && roleIdx < kWantCount) ? roleIdx : 0]);
}

extern "C" JNIEXPORT jint JNICALL
Java_top_niunaijun_blackboxa_menu_NativeMenu_requestHostInfo(JNIEnv *env, jobject thiz) {
    if (g_state.load() != ST_READY) return 0;
    g_reqHostInfo.store(true);
    return 1;
}

// v16: visual tasks. which: 1 = Submit Scan, 2 = Clear Asteroids, 3 = Empty Trash, 4 = Prime Shields. The work runs on the Unity thread.
extern "C" JNIEXPORT jint JNICALL
Java_top_niunaijun_blackboxa_menu_NativeMenu_requestVisual(JNIEnv *env, jobject thiz, jint which) {
    if (g_state.load() != ST_READY) return 0;
    if (which < VIS_SCAN || which > VIS_SHIELDS) return 0;
    g_reqVisualAt.store(nowMs());
    g_reqVisual.store((int)which);
    return 1;
}

extern "C" JNIEXPORT jstring JNICALL
Java_top_niunaijun_blackboxa_menu_NativeMenu_visualText(JNIEnv *env, jobject thiz) {
    if (g_state.load() != ST_READY) return env->NewStringUTF("Menu not ready yet (wait for 'Menu ready').");
    char buf[520];
    {
        std::lock_guard<std::mutex> lk(g_visMx);
        strncpy(buf, g_visMsg, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
    }
    return env->NewStringUTF(buf);
}

extern "C" JNIEXPORT jstring JNICALL
Java_top_niunaijun_blackboxa_menu_NativeMenu_hostInfoText(JNIEnv *env, jobject thiz) {
    if (g_state.load() != ST_READY) return env->NewStringUTF("Host: unknown\nMenu not ready yet (wait for 'Menu ready').");
    char buf[1500];
    long at = g_hostInfoAt.load();
    {
        std::lock_guard<std::mutex> lk(g_hostMx);
        const char *pre = (at == 0 || nowMs() - at > 3000) ? "(no fresh update: no player object is active, join a lobby or match)\n" : "";
        snprintf(buf, sizeof(buf), "%s%s", pre, g_hostInfo);
    }
    return env->NewStringUTF(buf);
}
