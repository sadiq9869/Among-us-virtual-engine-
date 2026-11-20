// physprobe.cpp  v3  (separate file, READ-ONLY, no hooks, no writes, no calls into the game)
//
// Goal: find the game's live physics objects by their C++ RTTI names and print
// what is inside them (words, doubles, floats, pointers that lead to other known objects).
//
// How it works (all by NAME at runtime, nothing hardcoded):
//  1. dl_iterate_phdr -> segments of the library whose path contains "libgame-CARROM".
//  2. In those segments find Itanium type-name strings like "18TablePhysicsEngine"
//     (only names with the keywords below).
//  3. Find typeinfo objects (word == address of that string at typeinfo+8),
//     then vtables (word == typeinfo address at vtable[-1]); the vptr = that slot + 8.
//  4. Every 60 s: scan the writable heap for 8-byte words equal to one of those vptrs
//     = live objects. Print count per class and a raw dump of the first objects.
//
// Log tag CARPROBE1, every line starts with "PH ".
// Build: add carprobe/physprobe.cpp to LOCAL_SRC_FILES in Android.mk. Nothing else changes.

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef PH_LIBNAME
#define PH_LIBNAME "libgame-CARROM"
#endif

#if defined(__LP64__)

namespace {

struct Seg { uint64_t a; uint64_t n; };
struct Region { uint64_t a, b; };
struct Hit { uint64_t addr; std::string name; };

std::vector<Seg> g_segs;
uint64_t g_libLo = 0, g_libHi = 0;
int g_fd = -1;

bool g_disc = false;                              // class discovery done
std::unordered_map<uint64_t, std::string> g_vptr; // vptr -> class name
uint64_t g_vmin = ~0ull, g_vmax = 0;
std::vector<Region> g_regions;                    // writable regions of the last scan

void L(const char* fmt, ...) {
  char b[900];
  va_list ap; va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  __android_log_print(ANDROID_LOG_INFO, "CARPROBE1", "%s", b);
}

bool rd(uint64_t addr, void* buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = pread(g_fd, (char*)buf + got, n - got, (off_t)(addr + got));
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

int cb(struct dl_phdr_info* i, size_t, void*) {
  if (!i->dlpi_name || !strstr(i->dlpi_name, PH_LIBNAME)) return 0;
  for (int k = 0; k < i->dlpi_phnum; k++) {
    const ElfW(Phdr)& p = i->dlpi_phdr[k];
    if (p.p_type != PT_LOAD || !(p.p_flags & PF_R)) continue;
    Seg s{(uint64_t)i->dlpi_addr + p.p_vaddr, (uint64_t)p.p_memsz};
    g_segs.push_back(s);
  }
  return 0;
}

bool find_lib() {
  g_segs.clear();
  dl_iterate_phdr(cb, nullptr);
  if (g_segs.empty()) return false;
  g_libLo = ~0ull; g_libHi = 0;
  for (auto& s : g_segs) { g_libLo = std::min(g_libLo, s.a); g_libHi = std::max(g_libHi, s.a + s.n); }
  return true;
}

const char* kKeys[] = {"Physics", "TableLogic", "PuckModel", "StrikerModel", "TableShape",
                       "TablePolygon", "TableState", "MatchControllerCarrom", "World", "Engine",
                       "Body", "Simulation", "Collision", "Disc"};

bool okname(const std::string& s) {
  // simple Itanium name: <len><identifier>  where len == strlen(identifier)
  size_t i = 0;
  while (i < s.size() && isdigit((unsigned char)s[i])) i++;
  if (i > 0 && i < s.size() && i <= 3) {
    int len = atoi(s.substr(0, i).c_str());
    if ((size_t)len == s.size() - i) {
      for (size_t k = i; k < s.size(); k++)
        if (!(isalnum((unsigned char)s[k]) || s[k] == '_')) return false;
      return true;
    }
  }
  // nested / template names: N...E
  if (s.size() > 4 && s[0] == 'N' && s.back() == 'E') {
    for (char c : s) if (!(isalnum((unsigned char)c) || c == '_')) return false;
    return true;
  }
  return false;
}

void discover() {
  std::vector<std::vector<uint8_t>> bufs(g_segs.size());
  for (size_t si = 0; si < g_segs.size(); si++) {
    bufs[si].resize(g_segs[si].n);
    // read in 1 MB pieces; unreadable pieces stay zero
    for (uint64_t o = 0; o < g_segs[si].n; o += (1u << 20)) {
      size_t n = (size_t)std::min<uint64_t>(1u << 20, g_segs[si].n - o);
      if (!rd(g_segs[si].a + o, bufs[si].data() + o, n)) memset(bufs[si].data() + o, 0, n);
    }
  }
  // pass 1: type-name strings
  std::unordered_map<uint64_t, std::string> strs;
  for (size_t si = 0; si < g_segs.size(); si++) {
    const uint8_t* b = bufs[si].data();
    size_t n = bufs[si].size();
    for (const char* key : kKeys) {
      size_t kl = strlen(key);
      const uint8_t* p = b;
      const uint8_t* end = b + n;
      while (p < end && (p = (const uint8_t*)memmem(p, (size_t)(end - p), key, kl))) {
        size_t pos = (size_t)(p - b);
        size_t s = pos;
        while (s > 0 && b[s - 1] != 0 && pos - s < 100) s--;
        if (s > 0 && b[s - 1] != 0) { p += kl; continue; }
        size_t e = pos;
        while (e < n && b[e] != 0 && e - pos < 100) e++;
        std::string name((const char*)b + s, e - s);
        if (okname(name)) strs[g_segs[si].a + s] = name;
        p += kl;
        if (strs.size() > 3000) break;
      }
    }
  }
  // pass 2: typeinfo = word == string address at T+8
  std::unordered_map<uint64_t, std::string> ti;
  for (size_t si = 0; si < g_segs.size(); si++) {
    const uint64_t* w = (const uint64_t*)bufs[si].data();
    size_t cnt = bufs[si].size() / 8;
    for (size_t i = 1; i < cnt; i++) {
      uint64_t v = w[i];
      if (v < g_libLo || v >= g_libHi) continue;
      auto it = strs.find(v);
      if (it != strs.end()) ti[g_segs[si].a + (i - 1) * 8] = it->second;
    }
  }
  // pass 3: vptr = (slot holding typeinfo address) + 8
  for (size_t si = 0; si < g_segs.size(); si++) {
    const uint64_t* w = (const uint64_t*)bufs[si].data();
    size_t cnt = bufs[si].size() / 8;
    for (size_t i = 0; i < cnt; i++) {
      uint64_t v = w[i];
      if (v < g_libLo || v >= g_libHi) continue;
      auto it = ti.find(v);
      if (it != ti.end()) {
        uint64_t vp = g_segs[si].a + i * 8 + 8;
        g_vptr[vp] = it->second;
        g_vmin = std::min(g_vmin, vp); g_vmax = std::max(g_vmax, vp);
      }
    }
  }
  std::map<std::string, int> per;
  for (auto& kv : g_vptr) per[kv.second]++;
  L("PH discover: strings=%zu typeinfo=%zu vtables=%zu classes=%zu", strs.size(), ti.size(),
    g_vptr.size(), per.size());
  std::string line;
  for (auto& kv : per) {
    char t[160];
    snprintf(t, sizeof t, "%s x%d; ", kv.first.c_str(), kv.second);
    if (line.size() + strlen(t) > 700) { L("PH classes: %s", line.c_str()); line.clear(); }
    line += t;
  }
  if (!line.empty()) L("PH classes: %s", line.c_str());
  g_disc = true;
}

bool skip_region(const char* path) {
  if (!path[0]) return false;
  if (strcmp(path, "[heap]") == 0) return false;
  if (strncmp(path, "[anon:", 6) == 0) {
    static const char* bad[] = {"dalvik", "gfx", "ashmem", "dmabuf", "kgsl", "mali", "stack", "art", "memfd"};
    for (const char* b : bad) if (strstr(path, b)) return true;
    return false;
  }
  return true;
}

bool in_region(uint64_t v) {
  size_t lo = 0, hi = g_regions.size();
  while (lo < hi) {
    size_t m = (lo + hi) / 2;
    if (v < g_regions[m].a) hi = m;
    else if (v >= g_regions[m].b) lo = m + 1;
    else return true;
  }
  return false;
}

std::string fmtword(uint64_t w) {
  char t[160];
  if (w == 0) return "0";
  if (w >= g_libLo && w < g_libHi) {
    snprintf(t, sizeof t, "L+%llx", (unsigned long long)(w - g_libLo));
    auto it = g_vptr.find(w);
    if (it != g_vptr.end()) return std::string(t) + "=vptr:" + it->second;
    return t;
  }
  if (w > 0x10000 && (w & 7) == 0 && in_region(w)) {
    uint64_t first = 0;
    if (rd(w, &first, 8)) {
      auto it = g_vptr.find(first);
      if (it != g_vptr.end()) {
        snprintf(t, sizeof t, "H%llx>%s", (unsigned long long)w, it->second.c_str());
        return t;
      }
    }
    snprintf(t, sizeof t, "H%llx", (unsigned long long)w);
    return t;
  }
  double d; memcpy(&d, &w, 8);
  if (std::isfinite(d) && fabs(d) >= 1e-6 && fabs(d) <= 1e7) {
    snprintf(t, sizeof t, "d%.9g", d);
    return t;
  }
  if (w < 1000000000ull) { snprintf(t, sizeof t, "i%llu", (unsigned long long)w); return t; }
  float f[2]; memcpy(f, &w, 8);
  bool a = std::isfinite(f[0]) && (f[0] == 0 || (fabsf(f[0]) >= 1e-4f && fabsf(f[0]) <= 1e6f));
  bool b = std::isfinite(f[1]) && (f[1] == 0 || (fabsf(f[1]) >= 1e-4f && fabsf(f[1]) <= 1e6f));
  if (a && b) { snprintf(t, sizeof t, "f%.7g,%.7g", f[0], f[1]); return t; }
  snprintf(t, sizeof t, "x%llx", (unsigned long long)w);
  return t;
}

bool interesting(const std::string& n) {
  static const char* k[] = {"Physics", "TableLogic", "Engine", "World", "Body", "PuckModel",
                            "StrikerModel", "TableShape", "Simulation"};
  for (const char* s : k) if (strstr(n.c_str(), s)) return true;
  return false;
}

const double kFit[] = {0.17, 0.0715, 0.73, 0.98, 1.7, 13.1, 17.4, 18.08, 30.3, 0.198, 4.7, 14.0};

// one-line summary of a pointee: sane doubles and values near the old fits
void summarize(const char* tag, uint64_t addr, uint64_t via, const uint8_t* b, size_t n, int& printed) {
  const uint64_t* w = (const uint64_t*)b;
  int sane = 0, fits = 0;
  std::string fl;
  for (size_t i = 0; i < n / 8; i++) {
    double d; memcpy(&d, &w[i], 8);
    if (std::isfinite(d) && fabs(d) >= 1e-3 && fabs(d) <= 1e4) {
      sane++;
      for (double t : kFit)
        if (fabs(d - t) <= 0.005 * t) {
          fits++;
          char t2[48]; snprintf(t2, sizeof t2, "d%.6g@+%zu ", d, i * 8);
          if (fl.size() < 220) fl += t2;
          break;
        }
    }
    float f[2]; memcpy(f, &w[i], 8);
    for (int h = 0; h < 2; h++)
      if (std::isfinite(f[h]) && f[h] != 0)
        for (double t : kFit)
          if (fabs(f[h] - t) <= 0.005 * t) {
            fits++;
            char t2[48]; snprintf(t2, sizeof t2, "f%.6g@+%zu ", f[h], i * 8 + h * 4);
            if (fl.size() < 220) fl += t2;
            break;
          }
  }
  auto it = g_vptr.find(w[0]);
  if (it != g_vptr.end() || fits > 0) {
    if (printed < 90) {
      L("PH %s @%llx via %lld first=%s sane=%d fits=%d %s", tag, (unsigned long long)addr,
        (long long)(int64_t)via, fmtword(w[0]).c_str(), sane, fits, fl.c_str());
      printed++;
    }
  }
}

void walk(const std::vector<uint64_t>& objs, int scanNo) {
  // A hit can be a secondary vptr inside the object, so read a window around it.
  const uint64_t BACK = 0x200;
  const size_t SZ = 0x800;
  struct Cand { uint64_t base; int ptrs; int vps; };
  std::vector<Cand> cands;
  size_t unreadable = 0;
  for (uint64_t base : objs) {
    std::vector<uint8_t> ob(SZ);
    if (!rd(base - BACK, ob.data(), SZ)) { unreadable++; continue; }
    const uint64_t* w = (const uint64_t*)ob.data();
    int ptrs = 0, vps = 0;
    for (size_t i = 0; i < SZ / 8; i++) {
      uint64_t v = w[i];
      if (v > 0x10000 && (v & 7) == 0 && in_region(v)) {
        ptrs++;
        uint64_t first = 0;
        if (rd(v, &first, 8) && g_vptr.count(first)) vps++;
      }
    }
    cands.push_back({base, ptrs, vps});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.ptrs > y.ptrs; });
  std::string cl;
  for (size_t i = 0; i < cands.size() && i < 20; i++) {
    char t[64]; snprintf(t, sizeof t, "%llx:%d/%d ", (unsigned long long)cands[i].base, cands[i].ptrs, cands[i].vps);
    cl += t;
  }
  L("PH mc scan=%d hits=%zu unreadable=%zu (addr:heapptrs/vptrptrs) %s", scanNo, objs.size(), unreadable, cl.c_str());

  for (size_t ci = 0; ci < cands.size() && ci < 3; ci++) {
    uint64_t base = cands[ci].base;
    std::vector<uint8_t> ob(SZ);
    if (!rd(base - BACK, ob.data(), SZ)) continue;
    const uint64_t* w = (const uint64_t*)ob.data();
    std::string hint;
    for (uint64_t off = 0x558; off < 0x580; off += 8) {
      uint64_t v = 0;
      if (rd(base + off, &v, 8)) { hint += fmtword(v); hint += " | "; }
    }
    L("PH mc @%llx ptrs=%d hint +0x558..: %s", (unsigned long long)base, cands[ci].ptrs, hint.c_str());
    int printed = 0;
    std::vector<uint64_t> seen;
    seen.push_back(base);
    std::vector<std::pair<uint64_t, int64_t>> l1;
    for (size_t i = 0; i < SZ / 8 && l1.size() < 120; i++) {
      uint64_t v = w[i];
      if (v > 0x10000 && (v & 7) == 0 && in_region(v) &&
          std::find(seen.begin(), seen.end(), v) == seen.end()) {
        seen.push_back(v);
        l1.push_back({v, (int64_t)(i * 8) - (int64_t)BACK});
      }
    }
    std::vector<std::pair<uint64_t, int64_t>> l2;
    for (auto& pr : l1) {
      std::vector<uint8_t> pb(0x400);
      if (!rd(pr.first, pb.data(), pb.size())) continue;
      summarize("L1", pr.first, (uint64_t)pr.second, pb.data(), pb.size(), printed);
      const uint64_t* pw = (const uint64_t*)pb.data();
      for (size_t i = 1; i < pb.size() / 8 && l2.size() < 600; i++) {
        uint64_t v = pw[i];
        if (v > 0x10000 && (v & 7) == 0 && in_region(v) &&
            std::find(seen.begin(), seen.end(), v) == seen.end()) {
          seen.push_back(v);
          l2.push_back({v, (int64_t)pr.first});
        }
      }
    }
    for (auto& pr : l2) {
      std::vector<uint8_t> pb(0x400);
      if (!rd(pr.first, pb.data(), pb.size())) continue;
      summarize("L2", pr.first, (uint64_t)pr.second, pb.data(), pb.size(), printed);
    }
    L("PH mc @%llx walked L1=%zu L2=%zu printed=%d", (unsigned long long)base, l1.size(), l2.size(), printed);
  }
}

void scan_heap(int scanNo) {
  std::vector<Region> regs;
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) { L("PH scan %d: cannot open maps", scanNo); return; }
  char line[600];
  while (fgets(line, sizeof line, f)) {
    unsigned long long a, b; char perm[8]; unsigned long long off; char dev[16]; unsigned long long ino;
    int np = 0;
    if (sscanf(line, "%llx-%llx %7s %llx %15s %llu %n", &a, &b, perm, &off, dev, &ino, &np) < 6) continue;
    if (perm[0] != 'r' || perm[1] != 'w' || perm[3] != 'p') continue;
    char* path = line + np;
    size_t pl = strlen(path);
    while (pl && (path[pl - 1] == '\n' || path[pl - 1] == ' ')) path[--pl] = 0;
    while (*path == ' ') path++;
    if (skip_region(path)) continue;
    if (b - a > (900ull << 20)) continue;
    regs.push_back({a, b});
  }
  fclose(f);
  g_regions = regs;
  std::sort(g_regions.begin(), g_regions.end(), [](const Region& x, const Region& y) { return x.a < y.a; });

  std::vector<Hit> hits;
  std::vector<uint8_t> buf(4u << 20);
  uint64_t mb = 0;
  for (auto& r : g_regions) {
    mb += (r.b - r.a) >> 20;
    for (uint64_t o = r.a; o < r.b; o += buf.size()) {
      size_t n = (size_t)std::min<uint64_t>(buf.size(), r.b - o);
      if (!rd(o, buf.data(), n)) continue;
      const uint64_t* w = (const uint64_t*)buf.data();
      for (size_t i = 0; i < n / 8; i++) {
        uint64_t v = w[i];
        if (v < g_vmin || v > g_vmax) continue;
        auto it = g_vptr.find(v);
        uint64_t ha = o + i * 8;
        // ignore our own read buffer (it holds copies of heap words)
        if (ha >= (uint64_t)(uintptr_t)buf.data() && ha < (uint64_t)(uintptr_t)buf.data() + buf.size()) continue;
        if (it != g_vptr.end() && hits.size() < 5000) hits.push_back({ha, it->second});
      }
    }
  }
  std::map<std::string, std::vector<uint64_t>> by;
  for (auto& h : hits) by[h.name].push_back(h.addr);
  L("PH scan %d: regions=%zu MB=%llu hits=%zu classes=%zu", scanNo, g_regions.size(),
    (unsigned long long)mb, hits.size(), by.size());
  std::string sum;
  for (auto& kv : by) {
    char t[200];
    snprintf(t, sizeof t, "%s x%zu; ", kv.first.c_str(), kv.second.size());
    if (sum.size() + strlen(t) > 700) { L("PH live: %s", sum.c_str()); sum.clear(); }
    sum += t;
  }
  if (!sum.empty()) L("PH live: %s", sum.c_str());

  if (scanNo >= 3 && scanNo <= 5) {
    for (auto& kv : by)
      if (kv.first.find("MatchControllerCarrom") != std::string::npos) walk(kv.second, scanNo);
  }
}

void* worker(void*) {
  sleep(45);
  g_fd = open("/proc/self/mem", O_RDONLY);
  if (g_fd < 0) { L("PH cannot open /proc/self/mem"); return nullptr; }
  L("PH physprobe v3 started (read-only)");
  for (int sc = 1; sc <= 6; sc++) {
    if (!g_disc) {
      if (!find_lib()) L("PH lib '%s' not found yet", PH_LIBNAME);
      else discover();
    }
    if (g_disc && !g_vptr.empty()) scan_heap(sc);
    else if (g_disc) L("PH no RTTI classes found with the keywords");
    sleep(60);
  }
  close(g_fd);
  return nullptr;
}

}  // namespace

#ifdef PH_TEST
void ph_test_run() {
  g_fd = open("/proc/self/mem", O_RDONLY);
  if (!find_lib()) { L("PH TEST lib not found"); return; }
  discover();
  scan_heap(3);
}
#else
__attribute__((constructor)) static void ph_ctor() {
  pthread_t t;
  if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
}
#endif

#endif  // __LP64__
