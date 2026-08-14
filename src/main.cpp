// SPDX-License-Identifier: MIT
// cpp-slab-watch — Linux kernel slab allocator monitor (/proc/slabinfo)
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o slab-watch
// Usage: ./slab-watch [--top N] [--watch N] [--json] [--demo] [--no-color] [--selftest]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Mutable ANSI palette (--no-color must actually clear it — see skill pitfall)
// ─────────────────────────────────────────────────────────────────────────────
static std::string C_RESET  = "\x1b[0m";
static std::string C_BOLD   = "\x1b[1m";
static std::string C_DIM    = "\x1b[2m";
static std::string C_RED    = "\x1b[31m";
static std::string C_GREEN  = "\x1b[32m";
static std::string C_YELLOW = "\x1b[33m";
static std::string C_BLUE   = "\x1b[34m";
static std::string C_CYAN   = "\x1b[36m";

static void disable_color() {
  C_RESET = C_BOLD = C_DIM = C_RED = C_GREEN = C_YELLOW = C_BLUE = C_CYAN = "";
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::optional<unsigned long long> parse_ull(std::string_view s) {
  // std::from_chars does NOT skip leading whitespace — trim first
  auto start = s.find_first_not_of(" \t");
  if (start == std::string_view::npos) return std::nullopt;
  s.remove_prefix(start);
  unsigned long long v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc()) return std::nullopt;
  if (ptr != s.data() + s.size()) return std::nullopt;  // trailing garbage
  return v;
}

static std::vector<std::string> split_ws(std::string_view s) {
  std::vector<std::string> out;
  std::istringstream ss{std::string(s)};
  std::string tok;
  while (ss >> tok) out.push_back(tok);
  return out;
}

static std::string fmt_num(unsigned long long v) {
  char buf[24];
  if (v >= 1000000000ULL) {
    std::snprintf(buf, sizeof(buf), "%.1fG", static_cast<double>(v) / 1e9);
  } else if (v >= 1000000ULL) {
    std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(v) / 1e6);
  } else if (v >= 1000ULL) {
    std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(v) / 1e3);
  } else {
    std::snprintf(buf, sizeof(buf), "%llu", v);
  }
  return buf;
}

static std::string fmt_rate(double v) {
  char buf[24];
  if (v >= 1e9)      std::snprintf(buf, sizeof(buf), "%.1fG/s", v / 1e9);
  else if (v >= 1e6) std::snprintf(buf, sizeof(buf), "%.1fM/s", v / 1e6);
  else if (v >= 1e3) std::snprintf(buf, sizeof(buf), "%.1fk/s", v / 1e3);
  else               std::snprintf(buf, sizeof(buf), "%.1f/s", v);
  return buf;
}

static std::string fmt_bytes(unsigned long long b) {
  char buf[24];
  if (b >= 1073741824ULL)      std::snprintf(buf, sizeof(buf), "%.1fG", static_cast<double>(b) / 1073741824.0);
  else if (b >= 1048576ULL)    std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(b) / 1048576.0);
  else if (b >= 1024ULL)       std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(b) / 1024.0);
  else                         std::snprintf(buf, sizeof(buf), "%llu", b);
  return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Data model — one cache line of /proc/slabinfo
//   name <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>
//   : tunables <limit> <batchcount> <sharedfactor>
//   : slabdata <active_slabs> <num_slabs> <sharedavail>
// ─────────────────────────────────────────────────────────────────────────────
struct SlabCache {
  std::string name;
  unsigned long long active_objs = 0;   // objects currently allocated
  unsigned long long num_objs = 0;      // total objects in the cache
  unsigned long long objsize = 0;       // bytes per object
  unsigned long long objperslab = 0;    // objects per slab
  unsigned long long pagesperslab = 0;  // pages per slab
  unsigned long long limit = 0, batchcount = 0, sharedfactor = 0;  // tunables
  unsigned long long active_slabs = 0, num_slabs = 0, sharedavail = 0;  // slabdata

  unsigned long long total_mem(unsigned long long page_size) const {
    return num_slabs * pagesperslab * page_size;
  }
  unsigned long long active_mem(unsigned long long page_size) const {
    return active_slabs * pagesperslab * page_size;
  }
  double utilization() const {
    return num_objs ? 100.0 * static_cast<double>(active_objs) / static_cast<double>(num_objs) : 0.0;
  }
};

struct Snapshot {
  std::vector<SlabCache> caches;
  unsigned long long page_size = 4096;

  unsigned long long total_objs() const {
    unsigned long long s = 0;
    for (auto& c : caches) s += c.num_objs;
    return s;
  }
  unsigned long long active_objs() const {
    unsigned long long s = 0;
    for (auto& c : caches) s += c.active_objs;
    return s;
  }
  unsigned long long total_mem() const {
    unsigned long long s = 0;
    for (auto& c : caches) s += c.total_mem(page_size);
    return s;
  }
  unsigned long long active_mem() const {
    unsigned long long s = 0;
    for (auto& c : caches) s += c.active_mem(page_size);
    return s;
  }
  double utilization() const {
    auto t = total_objs();
    return t ? 100.0 * static_cast<double>(active_objs()) / static_cast<double>(t) : 0.0;
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Parsing (pure — used by selftest with synthetic data)
// ─────────────────────────────────────────────────────────────────────────────
static std::optional<Snapshot> parse_slabinfo(const std::string& text,
                                              unsigned long long page_size = 4096) {
  Snapshot snap;
  snap.page_size = page_size;
  std::istringstream in{text};
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line.starts_with("slabinfo - version")) continue;  // version banner
    if (line.starts_with('#')) continue;                   // header comment lines
    auto toks = split_ws(line);
    if (toks.size() < 6) continue;  // name + 5 numeric fields minimum

    SlabCache c;
    c.name = toks[0];
    // Core 5 numeric fields: active_objs num_objs objsize objperslab pagesperslab
    for (int i = 0; i < 5; ++i) {
      auto v = parse_ull(toks[1 + i]);
      if (!v) { c.name.clear(); break; }  // malformed line → skip
      switch (i) {
        case 0: c.active_objs = *v; break;
        case 1: c.num_objs = *v; break;
        case 2: c.objsize = *v; break;
        case 3: c.objperslab = *v; break;
        case 4: c.pagesperslab = *v; break;
      }
    }
    if (c.name.empty()) continue;
    // Optional sections: ": tunables <l> <b> <s>" and ": slabdata <a> <n> <s>"
    for (size_t i = 6; i < toks.size(); ++i) {
      if (toks[i] == "tunables" && i + 3 < toks.size()) {
        auto l = parse_ull(toks[i + 1]);
        auto b = parse_ull(toks[i + 2]);
        auto s = parse_ull(toks[i + 3]);
        if (l && b && s) { c.limit = *l; c.batchcount = *b; c.sharedfactor = *s; }
        i += 3;
      } else if (toks[i] == "slabdata" && i + 3 < toks.size()) {
        auto a = parse_ull(toks[i + 1]);
        auto n = parse_ull(toks[i + 2]);
        auto s = parse_ull(toks[i + 3]);
        if (a && n && s) { c.active_slabs = *a; c.num_slabs = *n; c.sharedavail = *s; }
        i += 3;
      }
    }
    snap.caches.push_back(std::move(c));
  }
  if (snap.caches.empty()) return std::nullopt;
  return snap;
}

// ─────────────────────────────────────────────────────────────────────────────
// Real /proc/slabinfo (may be masked in containers → nullopt)
// ─────────────────────────────────────────────────────────────────────────────
static std::optional<std::string> read_real_slabinfo() {
  std::ifstream f("/proc/slabinfo");
  if (!f) return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  if (ss.str().empty()) return std::nullopt;
  return ss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Demo data — synthetic /proc/slabinfo, counters grow per step (LCG-ish)
// ─────────────────────────────────────────────────────────────────────────────
struct DemoCache {
  const char* name;
  unsigned long long base_active, base_num;
  unsigned long long objsize, objperslab, pagesperslab;
  double growth;  // active-objs growth factor per step
};

static const DemoCache kDemoCaches[] = {
  {"kmalloc-8k",                0,      0,   8192,   4,   8, 0.0},
  {"kmalloc-4k",              128,    128,   4096,   8,   8, 0.0},
  {"kmalloc-2k",             1024,   1024,   2048,  16,   8, 0.0},
  {"kmalloc-1k",             2048,   2048,   1024,  32,   8, 0.0},
  {"kmalloc-512",            5120,   6144,    512,  64,   8, 0.5},
  {"kmalloc-256",           12800,  16384,    256, 128,   8, 1.0},
  {"kmalloc-192",            2048,   4096,    192, 160,   8, 0.3},
  {"kmalloc-128",           24576,  32768,    128, 256,   8, 1.5},
  {"kmalloc-96",            12288,  16384,     96, 320,   8, 0.8},
  {"kmalloc-64",            65536,  65536,     64, 512,   8, 2.0},
  {"kmalloc-32",            32768,  32768,     32,1024,   8, 1.2},
  {"kmalloc-16",            65536,  65536,     16,2048,   8, 2.5},
  {"kmalloc-8",             32768,  65536,      8,4096,   8, 1.0},
  {"kmalloc-rcl-128",        4096,   8192,    128, 256,   8, 0.4},
  {"kmalloc-rcl-64",        16384,  32768,     64, 512,   8, 1.0},
  {"dentry",                84512,  98304,    192, 160,   8, 3.0},
  {"filp",                  51200,  65536,    256, 128,   8, 1.8},
  {"inode_cache",           76432,  81920,    640,  51,   8, 2.2},
  {"buffer_head",           16384,  32768,    104, 315,   8, 0.6},
  {"task_struct",             512,    768,   4544,   3,   4, 0.1},
  {"mm_struct",               256,    512,   1088,  15,   4, 0.05},
  {"vm_area_struct",        20480,  32768,    200, 163,   8, 1.4},
  {"anon_vma",               8192,  16384,     88, 372,   8, 0.7},
  {"anon_vma_chain",        20480,  32768,     64, 512,   8, 1.1},
  {"radix_tree_node",       12288,  16384,    576,  28,   4, 0.5},
  {"signal_cache",            512,    768,    960,  17,   4, 0.05},
  {"files_cache",             256,    512,    704,  23,   4, 0.02},
  {"fs_cache",                512,    768,     64, 512,   8, 0.01},
  {"sock_inode_cache",       2048,   4096,    704,  23,   4, 0.3},
  {"TCP",                    4096,   8192,   2112,   7,   4, 0.9},
  {"UDP",                    1024,   2048,   1152,  14,   4, 0.2},
  {"page->ptl",              5120,   8192,     48, 682,   8, 0.4},
};

static std::string demo_slabinfo_text(unsigned long long step) {
  std::ostringstream out;
  out << "slabinfo - version: 2.1\n";
  out << "# name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>"
         " : tunables <limit> <batchcount> <sharedfactor>"
         " : slabdata <active_slabs> <num_slabs> <sharedavail>\n";
  for (auto& d : kDemoCaches) {
    // Deterministic growth: objs grow with step, num_objs only when a new slab
    // fills up; slab counts derived from ceil(objs / objperslab).
    double growth = d.growth * static_cast<double>(step % 60);
    unsigned long long active = d.base_active +
        static_cast<unsigned long long>(static_cast<double>(d.base_active) * growth / 100.0);
    if (d.base_num == 0) { active = 0; }
    unsigned long long num = std::max(active, d.base_num);
    if (d.base_num > 0) {
      // num_objs grows in objperslab steps as slabs fill
      unsigned long long extra = active > d.base_active
          ? ((active - d.base_active) / d.objperslab + 1) * d.objperslab : 0;
      num = d.base_num + extra;
      num = std::max(num, active);
    }
    unsigned long long num_slabs = num ? (num + d.objperslab - 1) / d.objperslab : 0;
    unsigned long long active_slabs = active
        ? (active + d.objperslab - 1) / d.objperslab : 0;
    out << std::left << std::setw(20) << d.name
        << std::right << std::setw(10) << active
        << std::setw(10) << num
        << std::setw(9) << d.objsize
        << std::setw(10) << d.objperslab
        << std::setw(10) << d.pagesperslab
        << " : tunables    0    0    0 : slabdata "
        << std::setw(8) << active_slabs << ' '
        << std::setw(8) << num_slabs << "      0\n";
  }
  return out.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Rendering
// ─────────────────────────────────────────────────────────────────────────────
static std::string pct_color(double pct) {
  if (pct > 90.0) return C_RED;
  if (pct > 70.0) return C_YELLOW;
  return C_GREEN;
}

static std::string trunc_name(const std::string& s, size_t max) {
  if (s.size() <= max) return s;
  return s.substr(0, max - 3) + "...";
}

static void render_snapshot(const Snapshot& snap, int top_n) {
  auto sorted = snap.caches;
  std::stable_sort(sorted.begin(), sorted.end(),
    [&](const SlabCache& a, const SlabCache& b) {
      return a.total_mem(snap.page_size) > b.total_mem(snap.page_size);
    });
  if (top_n <= 0 || top_n > static_cast<int>(sorted.size())) top_n = static_cast<int>(sorted.size());

  std::println(std::cout, "{}SLAB WATCH{} — Linux kernel slab allocator ({}proc/slabinfo{})",
               C_BOLD, C_RESET, C_DIM, C_RESET);
  std::println(std::cout, "  {}------------------------------------------------------------------------------{}", C_DIM, C_RESET);
  std::println(std::cout, "  {}TOP CACHES BY MEMORY{} ({} of {})",
               C_BOLD, C_RESET, top_n, sorted.size());
  std::println(std::cout, "  {:<20} {:>16} {:>8} {:>6} {:>8} {:>7}", "NAME", "ACTIVE/TOTAL", "OBJSIZE", "SLABS", "MEMORY", "UTIL");

  for (int i = 0; i < top_n; ++i) {
    auto& c = sorted[static_cast<size_t>(i)];
    double util = c.utilization();
    std::ostringstream row;
    row << "  " << std::left << std::setw(20) << trunc_name(c.name, 20)
        << std::right << std::setw(8) << fmt_num(c.active_objs) << '/'
        << std::left << std::setw(7) << fmt_num(c.num_objs)
        << std::right << std::setw(8) << fmt_num(c.objsize)
        << std::setw(6) << c.num_slabs
        << std::setw(8) << fmt_bytes(c.total_mem(snap.page_size)) << ' '
        << pct_color(util) << std::fixed << std::setprecision(1) << std::setw(6) << util << "%" << C_RESET;
    std::cout << row.str() << '\n';
  }

  std::println(std::cout, "  {}------------------------------------------------------------------------------{}", C_DIM, C_RESET);
  std::ostringstream sum;
  sum << "  SUMMARY  caches " << snap.caches.size()
      << "   objects " << fmt_num(snap.total_objs())
      << "   slab memory " << C_CYAN << fmt_bytes(snap.total_mem()) << C_RESET
      << " (active " << fmt_bytes(snap.active_mem()) << ")"
      << "   util " << pct_color(snap.utilization())
      << std::fixed << std::setprecision(1) << snap.utilization() << "%" << C_RESET
      << "   page " << snap.page_size << 'B';
  std::cout << sum.str() << '\n';
}

static void render_json(const Snapshot& snap) {
  std::ostringstream out;
  out << "{\"caches\":[";
  bool first = true;
  for (auto& c : snap.caches) {
    if (!first) out << ',';
    first = false;
    out << "{\"name\":\"" << c.name << "\","
        << "\"active_objs\":" << c.active_objs << ','
        << "\"num_objs\":" << c.num_objs << ','
        << "\"objsize\":" << c.objsize << ','
        << "\"objperslab\":" << c.objperslab << ','
        << "\"pagesperslab\":" << c.pagesperslab << ','
        << "\"active_slabs\":" << c.active_slabs << ','
        << "\"num_slabs\":" << c.num_slabs << ','
        << "\"active_mem\":" << c.active_mem(snap.page_size) << ','
        << "\"total_mem\":" << c.total_mem(snap.page_size) << ','
        << "\"utilization\":" << c.utilization() << '}';
  }
  out << "],\"summary\":{"
      << "\"page_size\":" << snap.page_size << ','
      << "\"caches\":" << snap.caches.size() << ','
      << "\"total_objs\":" << snap.total_objs() << ','
      << "\"active_objs\":" << snap.active_objs() << ','
      << "\"total_mem\":" << snap.total_mem() << ','
      << "\"active_mem\":" << snap.active_mem() << ','
      << "\"utilization\":" << snap.utilization() << "}}\n";
  std::cout << out.str();
  std::cout.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// Watch mode — deltas between snapshots
// ─────────────────────────────────────────────────────────────────────────────
struct CacheRate {
  std::string name;
  double objs_per_s = 0.0;
  double mem_per_s = 0.0;  // bytes/s
  unsigned long long total_mem = 0;
  double utilization = 0.0;
};

static std::vector<CacheRate> compute_rates(const Snapshot& cur, const Snapshot& prev,
                                            double dt, unsigned long long page_size) {
  std::vector<CacheRate> rates;
  for (auto& c : cur.caches) {
    CacheRate r;
    r.name = c.name;
    r.total_mem = c.total_mem(page_size);
    r.utilization = c.utilization();
    for (auto& p : prev.caches) {
      if (p.name == c.name) {
        r.objs_per_s = dt > 0 ? static_cast<double>(c.active_objs - p.active_objs) / dt : 0.0;
        r.mem_per_s = dt > 0
            ? static_cast<double>(c.total_mem(page_size) - p.total_mem(page_size)) / dt : 0.0;
        break;
      }
    }
    rates.push_back(std::move(r));
  }
  std::stable_sort(rates.begin(), rates.end(),
    [](const CacheRate& a, const CacheRate& b) { return a.objs_per_s > b.objs_per_s; });
  return rates;
}

static void render_watch(const Snapshot& snap, const std::vector<CacheRate>& rates,
                         int top_n, unsigned long long elapsed_s) {
  std::println(std::cout, "{}SLAB WATCH{} — /proc/slabinfo — live ({}top {}{}, {}elapsed {}s{})",
               C_BOLD, C_RESET, C_DIM, top_n, C_RESET, C_DIM, elapsed_s, C_RESET);
  std::println(std::cout, "  {}------------------------------------------------------------------------------{}", C_DIM, C_RESET);
  std::println(std::cout, "  {:<20} {:>14} {:>12} {:>9} {:>7}", "NAME", "OBJS/s", "MEM/s", "MEMORY", "UTIL");
  int shown = 0;
  for (auto& r : rates) {
    if (shown++ >= top_n) break;
    std::ostringstream row;
    row << "  " << std::left << std::setw(20) << trunc_name(r.name, 20)
        << std::right << std::setw(12) << fmt_rate(r.objs_per_s)
        << std::setw(12) << fmt_rate(r.mem_per_s)
        << std::setw(9) << fmt_bytes(r.total_mem) << ' '
        << pct_color(r.utilization) << std::fixed << std::setprecision(1)
        << std::setw(6) << r.utilization << "%" << C_RESET;
    std::cout << row.str() << '\n';
  }
  std::println(std::cout, "  {}------------------------------------------------------------------------------{}", C_DIM, C_RESET);
  double total_rate = 0.0;
  for (auto& r : rates) total_rate += r.objs_per_s;
  std::ostringstream sum;
  sum << "  SUMMARY  caches " << snap.caches.size()
      << "   objects " << fmt_num(snap.total_objs())
      << "   slab memory " << C_CYAN << fmt_bytes(snap.total_mem()) << C_RESET
      << "   alloc " << C_GREEN << fmt_rate(total_rate) << C_RESET
      << "   util " << pct_color(snap.utilization())
      << std::fixed << std::setprecision(1) << snap.utilization() << "%" << C_RESET;
  std::cout << sum.str() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Selftest
// ─────────────────────────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

static void check(bool ok, const std::string& name) {
  if (ok) { ++g_pass; std::println(std::cout, "  {}PASS{} {}", C_GREEN, C_RESET, name); }
  else    { ++g_fail; std::println(std::cout, "  {}FAIL{} {}", C_RED, C_RESET, name); }
}

static int run_selftest() {
  std::println(std::cout, "{}SLAB WATCH selftest{}", C_BOLD, C_RESET);

  // 1. Full modern format with tunables + slabdata
  const std::string modern =
    "slabinfo - version: 2.1\n"
    "# name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> : tunables <limit> <batchcount> <sharedfactor> : slabdata <active_slabs> <num_slabs> <sharedavail>\n"
    "dentry             84512  98304    192  160    8 : tunables    0    0    0 : slabdata    528    768      0\n"
    "kmalloc-64         65536  65536     64  512    8 : tunables    0    0    0 : slabdata     64     64      0\n";
  auto s1 = parse_slabinfo(modern);
  check(s1.has_value() && s1->caches.size() == 2, "parse: formato moderno (tunables+slabdata)");
  check(s1.has_value() && s1->caches[0].name == "dentry", "parse: nombre primer cache");
  check(s1.has_value() && s1->caches[0].active_objs == 84512, "parse: active_objs");
  check(s1.has_value() && s1->caches[0].num_objs == 98304, "parse: num_objs");
  check(s1.has_value() && s1->caches[0].objsize == 192, "parse: objsize");
  check(s1.has_value() && s1->caches[0].objperslab == 160, "parse: objperslab");
  check(s1.has_value() && s1->caches[0].pagesperslab == 8, "parse: pagesperslab");
  check(s1.has_value() && s1->caches[0].num_slabs == 768, "parse: slabdata num_slabs");
  check(s1.has_value() && s1->caches[0].active_slabs == 528, "parse: slabdata active_slabs");

  // 2. Legacy 6-field format (no tunables/slabdata)
  const std::string legacy =
    "slabinfo - version: 2.1\n"
    "size-32 4096 4096 32 1024 8\n"
    "dentry 100 200 192 160 8\n";
  auto s2 = parse_slabinfo(legacy);
  check(s2.has_value() && s2->caches.size() == 2, "parse: formato legacy 6 campos");
  check(s2.has_value() && s2->caches[0].num_slabs == 0, "parse: legacy sin slabdata → slabs=0");

  // 3. Robustness: empty, only comments, malformed numeric line
  check(!parse_slabinfo("").has_value(), "parse: texto vacío → nullopt");
  check(!parse_slabinfo("# solo comentarios\nslabinfo - version: 2.1\n").has_value(),
        "parse: solo header/comentarios → nullopt");
  const std::string malformed =
    "slabinfo - version: 2.1\n"
    "dentry 84512 NOT_A_NUM 192 160 8 : slabdata 528 768 0\n"
    "filp 51200 65536 256 128 8 : slabdata 400 512 0\n";
  auto s3 = parse_slabinfo(malformed);
  check(s3.has_value() && s3->caches.size() == 1 && s3->caches[0].name == "filp",
        "parse: línea malformada se salta sin romper el resto");

  // 4. Memory math
  // dentry: num_slabs=768, pagesperslab=8, page 4096 → 768*8*4096 = 25,165,824
  check(s1.has_value() && s1->caches[0].total_mem(4096) == 768ULL * 8 * 4096,
        "mem: total_mem = num_slabs * pagesperslab * page_size");
  // active: 528*8*4096 = 17,301,504
  check(s1.has_value() && s1->caches[0].active_mem(4096) == 528ULL * 8 * 4096,
        "mem: active_mem = active_slabs * pagesperslab * page_size");
  // utilization 84512/98304 = 85.97...%
  check(s1.has_value() && std::abs(s1->caches[0].utilization() - 85.97) < 0.01,
        "mem: utilization = active_objs/num_objs");

  // 5. Summary aggregation
  check(s1.has_value() && s1->total_objs() == 98304 + 65536, "sum: total_objs agrega todos los caches");
  check(s1.has_value() && s1->caches.size() == 2, "sum: caches contados");
  check(s1.has_value() && s1->utilization() > 0.0 && s1->utilization() < 100.0,
        "sum: utilization global en rango");

  // 6. fmt_num / fmt_bytes
  check(fmt_num(999) == "999", "fmt: 999 → 999");
  check(fmt_num(12345) == "12.3k", "fmt: 12345 → 12.3k");
  check(fmt_num(9876543) == "9.9M", "fmt: 9876543 → 9.9M");
  check(fmt_num(1234567890ULL) == "1.2G", "fmt: 1234567890 → 1.2G");
  check(fmt_bytes(4096) == "4.0K", "fmt: bytes 4096 → 4.0K");
  check(fmt_bytes(25165824) == "24.0M", "fmt: bytes 25165824 → 24.0M");

  // 7. Top-N ordering by memory
  auto s4 = parse_slabinfo(
    "slabinfo - version: 2.1\n"
    "small 0 10 10 1 1 : slabdata 1 1 0\n"        // 1*1*4096 = 4K
    "big 0 10 10 1 8 : slabdata 8 8 0\n"          // 8*8*4096 = 256K
    "medium 0 10 10 1 4 : slabdata 4 4 0\n");     // 4*4*4096 = 64K
  check(s4.has_value() && s4->caches[0].name == "small" && s4->caches[1].name == "big"
        && s4->caches[2].name == "medium",
        "top: parse preserva orden de archivo (ordenación se hace en render)");

  // 8. Demo data integrity
  auto demo = demo_slabinfo_text(0);
  auto sd = parse_slabinfo(demo);
  check(sd.has_value() && sd->caches.size() > 20, "demo: genera >20 caches parseables");
  check(sd.has_value() && sd->total_mem() > 0, "demo: memoria total > 0");

  // 9. Demo growth (step 30 > step 0 for growing caches)
  auto sd30 = parse_slabinfo(demo_slabinfo_text(30));
  check(sd.has_value() && sd30.has_value()
        && sd30->active_objs() > sd->active_objs(),
        "demo: active_objs crece con el paso (para tasas en watch)");

  // 10. Rates
  auto prev = parse_slabinfo(demo_slabinfo_text(0));
  auto cur = parse_slabinfo(demo_slabinfo_text(10));
  check(prev.has_value() && cur.has_value(), "rates: snapshots de demo válidos");
  if (prev && cur) {
    auto rates = compute_rates(*cur, *prev, 1.0, 4096);
    check(rates.size() == cur->caches.size(), "rates: un rate por cache");
    double max_rate = 0.0;
    for (auto& r : rates) max_rate = std::max(max_rate, r.objs_per_s);
    check(max_rate > 0.0, "rates: hay crecimiento > 0 objs/s");
    check(std::is_sorted(rates.begin(), rates.end(),
          [](const CacheRate& a, const CacheRate& b) { return a.objs_per_s > b.objs_per_s; }),
          "rates: ordenados por objs/s desc (ties permitidos)");
  }

  // 11. Real /proc smoke test (conditional — masked in containers)
  auto real = read_real_slabinfo();
  if (real) {
    auto sr = parse_slabinfo(*real);
    check(sr.has_value() && sr->caches.size() > 0, "real: /proc/slabinfo legible y parseado");
    std::println(std::cout, "  {}SKIP{} real: /proc/slabinfo no accesible (contenedor) — se reporta, no falla", C_DIM, C_RESET);
  } else {
    std::println(std::cout, "  {}SKIP{} real: /proc/slabinfo no accesible (contenedor) — se reporta, no falla", C_DIM, C_RESET);
  }

  std::println(std::cout, "{}Result:{} {} passed, {} failed", C_BOLD, C_RESET, g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  int top_n = 10;
  int watch_secs = 0;
  bool json_mode = false, demo_mode = false, selftest = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    if (a == "--top" && i + 1 < argc) {
      ++i;
      auto v = parse_ull(argv[i]);
      if (v && *v > 0) top_n = static_cast<int>(*v);
    } else if (a == "--watch" && i + 1 < argc) {
      ++i;
      auto v = parse_ull(argv[i]);
      if (v) watch_secs = static_cast<int>(*v);
    } else if (a == "--json") {
      json_mode = true;
    } else if (a == "--demo") {
      demo_mode = true;
    } else if (a == "--selftest") {
      selftest = true;
    } else if (a == "-c" || a == "--no-color") {
      disable_color();
    } else if (a == "-h" || a == "--help") {
      std::println(std::cout, "Usage: slab-watch [--top N] [--watch N] [--json] [--demo] [--no-color] [--selftest]");
      std::println(std::cout, "  Monitor del slab allocator del kernel Linux via /proc/slabinfo.");
      std::println(std::cout, "    --top N      top N caches por memoria (default 10)");
      std::println(std::cout, "    --watch N    refresco en vivo cada N segundos con tasas delta");
      std::println(std::cout, "    --json       salida JSON para scripting");
      std::println(std::cout, "    --demo       datos sinteticos (contenedores/VMs con /proc enmascarado)");
      std::println(std::cout, "    --no-color   sin ANSI (pipes/logs)");
      std::println(std::cout, "    --selftest   auto-verificacion");
      return 0;
    } else {
      std::println(std::cerr, "Opción desconocida: {}", a);
      return 2;
    }
  }

  if (selftest) return run_selftest();

  if (json_mode && watch_secs > 0) {
    std::println(std::cerr, "--json y --watch son mutuamente excluyentes");
    return 2;
  }

  // Source of data: --demo or real /proc/slabinfo
  auto source_text = [&](unsigned long long step) -> std::optional<std::string> {
    if (demo_mode) return demo_slabinfo_text(step);
    return read_real_slabinfo();
  };

  auto page_size = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
  if (page_size == 0) page_size = 4096;

  if (watch_secs > 0) {
    auto prev = parse_slabinfo(*source_text(0), page_size);
    if (!prev) {
      std::println(std::cerr, "No se pudo leer /proc/slabinfo — prueba con --demo");
      return 1;
    }
    auto t_start = std::chrono::steady_clock::now();
    auto t_frame = t_start;
    unsigned long long step = 0;
    for (;;) {
      ++step;
      auto text = source_text(step);
      if (!text) break;
      auto cur = parse_slabinfo(*text, page_size);
      if (!cur) break;
      auto now = std::chrono::steady_clock::now();
      double dt = std::chrono::duration<double>(now - t_frame).count();
      if (step == 1) dt = static_cast<double>(watch_secs);  // first frame: nominal interval
      double elapsed = std::chrono::duration<double>(now - t_start).count();
      std::cout << "\x1b[2J\x1b[H";  // clear screen + home
      auto rates = compute_rates(*cur, *prev, dt, page_size);
      render_watch(*cur, rates, top_n, static_cast<unsigned long long>(elapsed));
      std::cout << std::flush;  // critical: flush each frame (pipes/timeout)
      prev = std::move(cur);
      t_frame = now;
      std::this_thread::sleep_for(std::chrono::seconds(watch_secs));
    }
    return 0;
  }

  auto text = source_text(0);
  if (!text) {
    std::println(std::cerr, "No se pudo leer /proc/slabinfo — prueba con --demo");
    return 1;
  }
  auto snap = parse_slabinfo(*text, page_size);
  if (!snap) {
    std::println(std::cerr, "No se pudo parsear /proc/slabinfo");
    return 1;
  }

  if (json_mode) {
    render_json(*snap);
  } else {
    render_snapshot(*snap, top_n);
  }
  return 0;
}
