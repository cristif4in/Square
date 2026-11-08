/* =====================================================================
 *  Square Browser Engine — square_platform.cpp
 *  Engine instance, load pipeline, threading, QuickJS + DOM/Web API,
 *  event loop, painting orchestration.
 *  (public C API + input handling live in square_platform_api.cpp)
 * ===================================================================== */
#include "square_platform_internal.h"
#include "square_layout_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <unordered_set>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sqrl {

std::string plat_lc(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

/* ============================ engine_log ============================ */
std::mutex g_log_mtx;
square_log_fn g_log_fn = nullptr;
void* g_log_ud = nullptr;

void engine_log(int level, const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> lk(g_log_mtx);
  if (g_log_fn) g_log_fn(g_log_ud, level, buf);
}

double now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/* ============================ file/http helpers ============================ */
static bool read_file_bytes(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out->resize(n);
  size_t rd = fread(out->data(), 1, n, f);
  fclose(f);
  return rd == (size_t)n;
}

static const int64_t kCacheMaxAgeSec = 2 * 24 * 3600;

static std::string cache_dir() {
  const char* env = getenv("SQRL_CACHE");
  return (env && *env) ? std::string(env) : std::string("square_cache");
}

static void cache_mkdir(const std::string& dir) {
#ifdef _WIN32
  CreateDirectoryA(dir.c_str(), nullptr);
#else
  mkdir(dir.c_str(), 0755);
#endif
}

static void cache_remove(const std::string& path) {
#ifdef _WIN32
  DeleteFileA(path.c_str());
#else
  unlink(path.c_str());
#endif
}

static uint64_t cache_hash(const std::string& url) {
  uint64_t h = 1469598103934665603ull;
  for (char c : url) { h ^= (uint8_t)c; h *= 1099511628211ull; }
  return h;
}

static std::string cache_path(const std::string& dir, const std::string& url) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%016llx.cache", dir.c_str(), (unsigned long long)cache_hash(url));
  return path;
}

static bool parse_cache_header(const std::string& head, int64_t* ts_out) {
  if (head.rfind("SQRL1 ", 0) != 0) return false;
  size_t e = head.find('\n');
  if (e == std::string::npos || e < 7) return false;
  *ts_out = (int64_t)atoll(head.substr(6, e - 6).c_str());
  return *ts_out > 0;
}

static bool cache_entry_expired(int64_t ts) {
  return (int64_t)time(nullptr) - ts > kCacheMaxAgeSec;
}

static bool cache_is_expired_file(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char head[64] = {0};
  size_t n = fread(head, 1, sizeof(head) - 1, f);
  fclose(f);
  int64_t ts = 0;
  std::string hs(head, n);
  return !parse_cache_header(hs, &ts) || cache_entry_expired(ts);
}

static void cache_enforce_size_cap(const std::string& dir) {
  struct Ent {
    std::string path;
    int64_t ts, size;
  };
  std::vector<Ent> ents;
  int64_t total = 0;
#ifdef _WIN32
  std::string pat = dir + "\\*.cache";
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::string path = dir + "\\" + fd.cFileName;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) continue;
    char head[64] = {0};
    size_t n = fread(head, 1, sizeof(head) - 1, f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    int64_t ts = 0;
    std::string hs(head, n);
    if (parse_cache_header(hs, &ts)) {
      ents.push_back({path, ts, sz});
      total += sz;
    } else {
      cache_remove(path);
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  struct dirent* e;
  while ((e = readdir(d))) {
    std::string name = e->d_name;
    if (name.size() < 6 || name.compare(name.size() - 6, 6, ".cache") != 0) continue;
    std::string path = dir + "/" + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) continue;
    char head[64] = {0};
    size_t n = fread(head, 1, sizeof(head) - 1, f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    int64_t ts = 0;
    std::string hs(head, n);
    if (parse_cache_header(hs, &ts)) {
      ents.push_back({path, ts, sz});
      total += sz;
    } else {
      cache_remove(path);
    }
  }
  closedir(d);
#endif
  const int64_t kCacheCapBytes = 256ll * 1024 * 1024;
  if (total <= kCacheCapBytes) return;
  std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.ts < b.ts; });
  for (auto& en : ents) {
    if (total <= kCacheCapBytes) break;
    cache_remove(en.path);
    total -= en.size;
  }
}

static void cache_sweep(const std::string& dir) {
#ifdef _WIN32
  std::string pat = dir + "\\*.cache";
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::string path = dir + "\\" + fd.cFileName;
    if (cache_is_expired_file(path)) cache_remove(path);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  cache_enforce_size_cap(dir);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  struct dirent* e;
  while ((e = readdir(d))) {
    std::string name = e->d_name;
    if (name.size() < 6 || name.compare(name.size() - 6, 6, ".cache") != 0) continue;
    std::string path = dir + "/" + name;
    if (cache_is_expired_file(path)) cache_remove(path);
  }
  closedir(d);
  cache_enforce_size_cap(dir);
#endif
}

struct CacheEntry {
  std::vector<uint8_t> body;
  int64_t ts = 0;
  int max_age = 0;
  std::string etag, last_mod;
};

static bool cache_lookup(const std::string& url, CacheEntry* out) {
  if (url.rfind("http", 0) != 0) return false;
  std::string path = cache_path(cache_dir(), url);
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string all;
  all.resize(n > 0 ? (size_t)n : 0);
  size_t rd = n > 0 ? fread(&all[0], 1, (size_t)n, f) : 0;
  fclose(f);
  int64_t ts = 0;
  if (rd == 0 || !parse_cache_header(all, &ts) || cache_entry_expired(ts)) {
    cache_remove(path);
    return false;
  }
  size_t eol = all.find('\n');
  size_t body_at = eol + 1;
  if (eol == std::string::npos || body_at >= all.size()) {
    cache_remove(path);
    return false;
  }
  out->ts = ts;
  out->max_age = kCacheMaxAgeSec;
  out->etag.clear();
  out->last_mod.clear();
  std::string head = all.substr(6, eol - 6);
  if (!head.empty()) {
    auto unflat = [](std::string v) {
      std::string rep;
      for (char c : v) rep += (c == '\x01' ? ' ' : c);
      return rep;
    };
    size_t sp1 = head.find(' ');
    if (sp1 != std::string::npos) {
      out->max_age = atoi(head.substr(sp1 + 1).c_str());
      size_t sp2 = head.find(' ', sp1 + 1);
      if (sp2 != std::string::npos) {
        size_t sp3 = head.find(' ', sp2 + 1);
        out->etag = unflat(sp3 == std::string::npos ? head.substr(sp2 + 1)
                                                    : head.substr(sp2 + 1, sp3 - sp2 - 1));
        if (sp3 != std::string::npos) out->last_mod = unflat(head.substr(sp3 + 1));
      }
    }
  }
  out->body.assign(all.begin() + body_at, all.end());
  return true;
}

static void cache_store(const std::string& url, const std::vector<uint8_t>& body, int max_age,
                        const std::string& etag, const std::string& last_mod) {
  if (url.rfind("http", 0) != 0 || body.empty()) return;
  static bool swept = false;
  std::string dir = cache_dir();
  if (!swept) {
    swept = true;
    cache_mkdir(dir);
    cache_sweep(dir);
  }
  auto flat = [](std::string v) {
    for (auto& c : v)
      if (c == ' ' || c == '\n' || c == '\r') c = '\x01';
    return v;
  };
  std::string path = cache_path(dir, url);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) {
    cache_mkdir(dir);
    f = fopen(path.c_str(), "wb");
  }
  if (!f) return;
  fprintf(f, "SQRL2 %lld %d %s %s\n", (long long)time(nullptr), max_age, flat(etag).c_str(),
          flat(last_mod).c_str());
  fwrite(body.data(), 1, body.size(), f);
  fclose(f);
}

HttpResult fetch_sync(Instance* inst, const std::string& url, const char* dest);
HttpResult fetch_sync(Instance* inst, const std::string& url) { return fetch_sync(inst, url, "document"); }
HttpResult fetch_sync(Instance* inst, const std::string& url, const char* dest) {
  if (url.rfind("file://", 0) == 0) {
    HttpResult r;
    std::string path = url.substr(7);
    if (path.size() > 2 && path[0] == '/' && path[2] == ':') path = path.substr(1); /* /C:/ */
    std::vector<uint8_t> bytes;
    if (read_file_bytes(path, &bytes)) {
      r.ok = true;
      r.status = 200;
      r.body = std::move(bytes);
      r.final_url = url;
    } else {
      r.err = "file not found: " + path;
    }
    return r;
  }
  if (url.rfind("data:", 0) == 0) {
    HttpResult r;
    std::string mime;
    std::vector<uint8_t> bytes;
    if (data_url_parse(url, &mime, &bytes)) {
      r.ok = true;
      r.status = 200;
      r.body = std::move(bytes);
      r.final_url = url;
    } else r.err = "bad data url";
    return r;
  }
  std::string dest_s = dest ? dest : "document";
  bool is_doc = dest_s == "document";
  bool is_img = dest_s == "image";
  bool is_style = dest_s == "style";
  bool is_script = dest_s == "script";
  if (is_img) {
    CacheEntry ce;
    if (cache_lookup(url, &ce)) {
      int64_t age = (int64_t)time(nullptr) - ce.ts;
      bool fresh = age <= ce.max_age;
      bool revalidatable = !ce.etag.empty() || !ce.last_mod.empty();
      auto cached_result = [&]() {
        HttpResult r;
        r.ok = true;
        r.status = 200;
        r.body = ce.body;
        r.final_url = url;
        return r;
      };
      if (!fresh && revalidatable) {
        std::vector<HttpHeader> vh;
        vh.push_back({"User-Agent", inst->ua});
        if (!ce.etag.empty()) vh.push_back({"If-None-Match", ce.etag});
        if (!ce.last_mod.empty()) vh.push_back({"If-Modified-Since", ce.last_mod});
        HttpResult rr = net_http_get(url, vh, "GET", 15000, &inst->jar, &inst->cancel);
        if (rr.status == 304) {
          cache_store(url, ce.body, ce.max_age, ce.etag, ce.last_mod);
          return cached_result();
        }
        if (rr.ok && rr.status == 200 && !rr.body.empty()) {
          int ma = 86400;
          std::string etag = rr.header("ETag");
          std::string lm = rr.header("Last-Modified");
          std::string cc = plat_lc(rr.header("Cache-Control"));
          size_t mp = cc.find("max-age=");
          if (mp != std::string::npos) ma = atoi(cc.c_str() + mp + 8);
          cache_store(url, rr.body, ma, etag, lm);
          rr.final_url = url;
          return rr;
        }
        return cached_result();
      }
      if (fresh) return cached_result();
    }
  }
  std::vector<HttpHeader> hdrs;
  hdrs.push_back({"User-Agent", inst->ua});
  if (is_doc) {
    hdrs.push_back({"Accept",
                    "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,"
                    "image/png,image/svg+xml,*/*;q=0.8"});
  } else if (is_img) {
    hdrs.push_back({"Accept",
                    "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8"});
  } else if (is_style) {
    hdrs.push_back({"Accept", "text/css,*/*;q=0.1"});
  } else if (is_script) {
    hdrs.push_back({"Accept", "*/*"});
  } else {
    hdrs.push_back({"Accept", "*/*"});
  }
  hdrs.push_back({"Accept-Language", "en-US,en;q=0.9"});
  hdrs.push_back({"Sec-Fetch-Dest", dest_s});
  hdrs.push_back({"Sec-Fetch-Mode", is_doc ? "navigate" : "no-cors"});
  /* Sec-Fetch-Site: same-origin only when host matches the document host */
  {
    auto host_of = [](const std::string& u) -> std::string {
      size_t p = u.find("://");
      if (p == std::string::npos) return "";
      p += 3;
      size_t e = u.find('/', p);
      std::string h = u.substr(p, e == std::string::npos ? std::string::npos : e - p);
      size_t at = h.rfind('@');
      if (at != std::string::npos) h = h.substr(at + 1);
      if (!h.empty() && h.front() == '[') { /* ipv6 */
        size_t e6 = h.find(']');
        if (e6 != std::string::npos) h = h.substr(0, e6 + 1);
      } else {
        size_t c = h.rfind(':');
        if (c != std::string::npos && h.find_first_not_of("0123456789", c + 1) == std::string::npos)
          h = h.substr(0, c);
      }
      return h;
    };
    std::string uhost = host_of(url);
    std::string bhost = host_of(inst->final_url.empty() ? url : inst->final_url);
    hdrs.push_back({"Sec-Fetch-Site",
                    uhost.empty() || bhost.empty() ? "none" : (uhost == bhost ? "same-origin" : "cross-site")});
  }
  hdrs.push_back({"Upgrade-Insecure-Requests", "1"});

  /* fetch with retry: CDN rate-limit block pages are transient.
   * NOTE: do NOT treat "<!doctype html>" as a block signature — every real
   * HTML document starts with one and that caused 4x retries on success. */
  auto looks_blocked = [](const HttpResult& rr) {
    if (!rr.ok) return false;
    if (rr.status == 403 || rr.status == 429 || rr.status == 503) return true;
    if (rr.body.size() < 12) return false;
    std::string body(rr.body.begin(), rr.body.begin() + std::min(rr.body.size(), (size_t)4000));
    std::string blc = plat_lc(body);
    if (blc.find("cf-browser-verification") != std::string::npos ||
        blc.find("cf-chl-bypass") != std::string::npos ||
        blc.find("just a moment") != std::string::npos ||
        blc.find("you have been blocked") != std::string::npos ||
        blc.find("attention required") != std::string::npos)
      return true;
    /* "ray id" only counts when the page is tiny (real block pages are bare) */
    if (rr.body.size() < 2500 && blc.find("ray id") != std::string::npos) return true;
    return false;
  };
  HttpResult r;
  for (int attempt = 0; attempt < 4; attempt++) {
    r = net_http_get(url, hdrs, "GET", 20000, &inst->jar, &inst->cancel);
    if (inst->cancel) return r;
    if (!looks_blocked(r)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(600 * (attempt + 1)));
    engine_log(2, "[retry] %s (attempt %d, status %d)", url.c_str(), attempt + 1, r.status);
  }
  if (is_img && r.ok && r.status == 200 && !looks_blocked(r)) {
    std::string cc = plat_lc(r.header("Cache-Control"));
    if (cc.find("no-store") == std::string::npos) {
      int ma = 86400;
      size_t mp = cc.find("max-age=");
      if (mp != std::string::npos)
        ma = atoi(cc.c_str() + mp + 8);
      else if (cc.find("no-cache") != std::string::npos)
        ma = 0;
      cache_store(url, r.body, ma, r.header("ETag"), r.header("Last-Modified"));
    }
  }
  return r;
}
/* ============================ style/layout/paint refresh ============================ */
static void paint_current_frame(Instance* inst, int vw, int vh) {
  inst->page_h = inst->root_box ? (int)layout_page_height(inst->root_box) : vh;
  int scroll_max = inst->page_h - vh;
  if (scroll_max < 0) scroll_max = 0;
  if (inst->scroll_y > scroll_max) inst->scroll_y = scroll_max;
  {
    std::lock_guard<std::mutex> plk(inst->paint_mtx);
    if (inst->latest.w != vw || inst->latest.h != vh) {
      inst->latest.free_pix();
      inst->latest.alloc(vw, vh);
    }
    if (!getenv("SQRL_NO_PAINT"))
      paint_document(inst->doc, inst->root_box, &inst->latest, &inst->images, (float)inst->scroll_y,
                     &inst->sel);
  }
}

static bool relayout_dirty(Instance* inst, const std::vector<Node*>& dirty, int vw, int vh) {
  Document* doc = inst->doc;
  if (!doc || !inst->root_box) return false;
  std::vector<Node*> roots;
  for (Node* n : dirty) {
    if (!n || n->type != N_ELEMENT || !n->cs) return false;
    bool in_doc = false;
    for (Node* a = n; a; a = a->parent)
      if (a == doc->body || a == doc->html) { in_doc = true; break; }
    if (!in_doc) continue;
    bool covered = false;
    for (Node* a = n->parent; a; a = a->parent) {
      if (std::find(dirty.begin(), dirty.end(), a) != dirty.end()) { covered = true; break; }
    }
    if (!covered) roots.push_back(n);
  }
  if (roots.empty()) return true;
  for (Node* r : roots) {
    std::vector<Node*> chain;
    for (Node* a = r->parent; a && a->type == N_ELEMENT && a->cs; a = a->parent) chain.push_back(a);
    for (size_t i = chain.size(); i-- > 0;) style_node_single(doc, chain[i], vw, vh);
  }
  for (Node* r : roots) style_subtree(doc, r, vw, vh);
  for (Node* r : roots) {
    if (!r->box) return false;
    std::vector<Box*> chain;
    for (Box* bx = r->box; bx; bx = bx->parent) chain.push_back(bx);
    if (chain.empty() || chain.back() != inst->root_box) return false;
    LayoutCtx lc;
    lc.doc = doc;
    lc.vw = vw;
    lc.vh = vh;
    lc.rem = doc->root_font_size;
    lc.images = &inst->images;
    lc.root_box = inst->root_box;
    for (size_t i = 0; i < chain.size(); i++) {
      Box* bx = chain[i];
      if (!bx->node) return false;
      float ow = bx->w, oh = bx->h, ox = bx->x, oy = bx->y;
      layout_element(bx, bx->node, lc);
      if (bx->w == ow && bx->h == oh && bx->x == ox && bx->y == oy) break;
    }
  }
  paint_current_frame(inst, vw, vh);
  return true;
}

void refresh_page(Instance* inst) {
  std::lock_guard<std::mutex> lk(inst->mtx);
  if (!inst->doc || !inst->doc->body) return;
  int vw = inst->vw, vh = inst->vh;
  std::vector<Node*> dirty;
  {
    std::lock_guard<std::mutex> dlk(inst->dirty_mtx);
    dirty.swap(inst->dirty_nodes);
  }
  if (!dirty.empty() && !getenv("SQRL_NO_INCR") && !inst->doc->sheet.has_animations) {
    if (relayout_dirty(inst, dirty, vw, vh)) {
      inst->dirty = false;
      return;
    }
  }
  style_tree(inst->doc, vw, vh);
  if (getenv("SQRL_NO_LAYOUT")) { inst->dirty = false; return; }
#ifdef LAYOUT_DEBUG
  {
    int els = 0;
    std::function<void(Node*)> cnt = [&](Node* n) { if (n->type == N_ELEMENT) els++; for (auto* c : n->children) cnt(c); };
    cnt(inst->doc->body);
    Node* rootdiv = nullptr;
    std::function<void(Node*)> find = [&](Node* n) {
      if (n->type == N_ELEMENT && n->attr("id") && *n->attr("id") == "root") rootdiv = n;
      for (auto* c : n->children) find(c);
    };
    find(inst->doc->body);
    engine_log(1, "[layout] body els=%d rootdiv=%s rootdiv_kids=%zu", els,
               rootdiv ? "yes" : "no", rootdiv ? rootdiv->children.size() : 0);
    if (rootdiv && rootdiv->children.size()) {
      Node* c0 = rootdiv->children[0];
      engine_log(1, "[layout] root child0 tag=%s display=%d", c0->tag.c_str(), c0->cs ? (int)c0->cs->display : -1);
    }
  }
#endif
  if (getenv("SQRL_NO_LAYOUT")) return;
  if (inst->root_box) free_box_tree(inst->root_box);
  inst->root_box = nullptr;
  inst->root_box = layout_document(inst->doc, vw, vh, &inst->images);
  inst->page_h = inst->root_box ? (int)layout_page_height(inst->root_box) : vh;
  /* keep the scroll position valid after relayout (resize / DOM change) */
  int scroll_max = inst->page_h - vh;
  if (scroll_max < 0) scroll_max = 0;
  if (inst->scroll_y > scroll_max) inst->scroll_y = scroll_max;
  paint_current_frame(inst, vw, vh);
  inst->dirty = false;
}

/* ============================ page thread ============================ */
static void set_state(Instance* inst, int st, float progress, const char* info) {
  inst->state = st;
  if (inst->state_fn) inst->state_fn(inst->state_ud, st, progress, info ? info : "");
}

void fire_queued_clicks(Instance* inst); /* defined in square_platform_api.cpp (input) */
void page_main(Instance* inst, std::string url) {
  transitions_reset();
  js_module_cache_clear();
  inst->images.mtx.lock();
  inst->images.map.clear();
  inst->images.requested.clear();
  inst->images.mtx.unlock();
  inst->sel = TextSelection{};
  inst->sel_dragging = false;
  inst->sel_dirty = false;
  inst->press_valid = false;
  {
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    g_node_registry.clear();
    g_node_ids.clear();
    g_next_node_id = 1;
  }
  /* --- fetch document --- */
  set_state(inst, SQ_STATE_CONNECTING, 0.05f, url.c_str());
  HttpResult res = fetch_sync(inst, url);
  if (!res.ok) {
    engine_log(3, "load failed: %s", res.err.c_str());
    set_state(inst, SQ_STATE_ERROR, 0, res.err.c_str());
    return;
  }
  inst->final_url = res.final_url.empty() ? url : res.final_url;
  std::string html(res.body.begin(), res.body.end());
  set_state(inst, SQ_STATE_DOCUMENT, 0.15f, "parsing");

  /* data:text/html shortcut */
  inst->doc = html_parse_document(html);
  g_document_node = inst->doc ? inst->doc->root : nullptr;
  inst->doc->base_url = inst->final_url;
  inst->images.base_url = inst->final_url;
  /* lazy image loading: paint/layout requests missing urls on demand */
  inst->images.request_fetch = [inst](const std::string& u) {
    inst->loading++;
    inst->pool.submit([inst, u]() {
      HttpResult r = fetch_sync(inst, u, "image");
      if (r.ok && r.status < 400) {
        ImageInfo info;
        info.url = u;
        if (image_decode(r.body, &info.gfx_id, &info.w, &info.h)) {
          info.decoded = true;
          inst->images.put(u, info);
          inst->dirty = true;
        } else {
          ImageInfo bad;
          bad.failed = true;
          bad.url = u;
          inst->images.put(u, bad);
        }
      } else {
        ImageInfo bad;
        bad.failed = true;
        bad.url = u;
        inst->images.put(u, bad);
      }
      inst->resources_loaded++;
      inst->loading--;
    });
  };
  inst->page_title = inst->doc->title;
  engine_log(1, "parsed: %d bytes, title='%s'", (int)html.size(), inst->doc->title.c_str());

  /* --- stylesheets --- */
  set_state(inst, SQ_STATE_STYLES, 0.3f, "styles");
  std::vector<std::string> css_urls;
  std::vector<std::string> inline_css;
  std::function<void(Node*)> scan_styles = [&](Node* n) {
    if (n->type == N_ELEMENT) {
      if (n->tag == "link") {
        const std::string* rel = n->attr("rel");
        const std::string* href = n->attr("href");
        if (rel && href && rel->find("stylesheet") != std::string::npos)
          css_urls.push_back(url_resolve(inst->final_url, *href));
      } else if (n->tag == "style") {
        for (Node* c : n->children)
          if (c->type == N_TEXT) inline_css.push_back(c->data);
      }
    }
    for (Node* c : n->children) scan_styles(c);
  };
  scan_styles(inst->doc->root);

  std::vector<std::string> css_bodies;
  std::mutex css_mtx;
  inst->loading = 1;
  for (auto& u : css_urls) {
    inst->loading++;
    std::string uu = u;
    inst->pool.submit([inst, uu, &css_bodies, &css_mtx]() {
      HttpResult r = fetch_sync(inst, uu, "style");
      std::string body;
      if (r.ok && r.status < 400) body.assign(r.body.begin(), r.body.end());
      std::lock_guard<std::mutex> lk(css_mtx);
      css_bodies.push_back(std::move(body));
      inst->loading--;
    });
  }
  /* wait for css */
  while (inst->loading > 1 && !inst->cancel) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  inst->loading = 0;

  {
    int vw = inst->vw, vh = inst->vh;
    for (auto& body : css_bodies) {
      if (body.empty()) continue;
      css_parse_stylesheet(body, &inst->doc->sheet, vw, vh);
    }
    for (auto& body : inline_css) css_parse_stylesheet(body, &inst->doc->sheet, vw, vh);
    /* @imports (one level) */
    std::vector<std::string> imports;
    for (auto& u : inst->doc->sheet.imports) imports.push_back(url_resolve(inst->final_url, u));
    for (auto& iu : imports) {
      HttpResult r = fetch_sync(inst, iu, "style");
      if (r.ok && r.status < 400) {
        std::string body(r.body.begin(), r.body.end());
        css_parse_stylesheet(body, &inst->doc->sheet, vw, vh);
      }
    }
    engine_log(1, "css: %d rules, %d font-faces", (int)inst->doc->sheet.rules.size(),
               (int)inst->doc->sheet.fontfaces.size());
  }

  /* initial layout so something renders even without JS */
  { std::lock_guard<std::mutex> lk(inst->doc_mtx); refresh_page(inst); }

  /* --- images (async) --- */
  {
    std::unordered_set<std::string> img_urls;
    std::function<void(Node*)> scan_imgs = [&](Node* n) {
      if (n->type == N_ELEMENT) {
        if (n->tag == "img" || n->tag == "source" || n->tag == "input") {
          const std::string* src = n->attr("src");
          if (!src && n->tag == "img") src = n->attr("data-src");
          if (src && !src->empty() && src->rfind("data:", 0) != 0)
            img_urls.insert(inst->images.resolve(*src));
        }
        /* inline style background-image url() (React sets these via style attr) */
        const std::string* style = n->attr("style");
        if (style) {
          size_t p2 = 0;
          while ((p2 = style->find("url(", p2)) != std::string::npos) {
            size_t e = style->find(')', p2);
            if (e == std::string::npos) break;
            std::string u = style->substr(p2 + 4, e - p2 - 4);
            if (!u.empty() && (u.front() == '"' || u.front() == '\'')) u = u.substr(1, u.size() - 2);
            if (!u.empty() && u.rfind("data:", 0) != 0)
              img_urls.insert(inst->images.resolve(u));
            p2 = e;
          }
        }
      }
      for (Node* c : n->children) scan_imgs(c);
    };
    scan_imgs(inst->doc->root);
    /* background images from computed styles later; scan CSS url()s that are images */
    for (auto& rule : inst->doc->sheet.rules) {
      for (auto& d : rule.decls) {
        if (d.prop == "background" || d.prop == "background-image") {
          size_t p = 0;
          while ((p = d.value.find("url(", p)) != std::string::npos) {
            size_t e = d.value.find(')', p);
            if (e == std::string::npos) break;
            std::string u = d.value.substr(p + 4, e - p - 4);
            if (!u.empty() && (u.front() == '"' || u.front() == '\'')) u = u.substr(1, u.size() - 2);
            if (u.rfind("data:", 0) != 0) img_urls.insert(inst->images.resolve(u));
            p = e;
          }
        }
      }
    }
    inst->loading = (int)img_urls.size();
    engine_log(1, "images to load: %d", (int)img_urls.size());
    for (const auto& iu : img_urls) {
      inst->pool.submit([inst, iu]() {
        engine_log(1, "[img-fetch] GET %.70s", iu.c_str());
        HttpResult r = fetch_sync(inst, iu, "image");
        engine_log(1, "[img-fetch] ok=%d status=%d size=%zu err=%.60s", (int)r.ok, r.status,
                   r.body.size(), r.err.c_str());
        if (r.ok && r.status < 400) {
          ImageInfo info;
          info.url = iu;
          if (image_decode(r.body, &info.gfx_id, &info.w, &info.h)) {
            info.decoded = true;
            inst->images.put(iu, info);
            inst->dirty = true;
          } else {
            ImageInfo bad;
            bad.failed = true;
            bad.url = iu;
            inst->images.put(iu, bad);
          }
        } else {
          ImageInfo bad;
          bad.failed = true;
          bad.url = iu;
          inst->images.put(iu, bad);
        }
        inst->resources_loaded++;
        inst->loading--;
      });
    }
  }

  /* --- javascript --- */
  set_state(inst, SQ_STATE_SCRIPT, 0.6f, "javascript");
  if (getenv("SQRL_NO_JS")) {
    while (!inst->cancel) {
      if (inst->state != SQ_STATE_READY && inst->loading <= 0) {
        set_state(inst, SQ_STATE_READY, 1.f, "ready");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return;
  }
  inst->js_epoch = now_ms();
  inst->rt = JS_NewRuntime();
  if (inst->rt) {
    JS_SetMemoryLimit(inst->rt, 512 * 1024 * 1024);
    JS_SetMaxStackSize(inst->rt, 2 * 1024 * 1024);
    inst->jctx = JS_NewContext(inst->rt);
    JS_SetContextOpaque(inst->jctx, inst);
    if (!node_class_id) JS_NewClassID(&node_class_id);
    JSClassDef cdef = {"NodeHandle", node_finalizer, nullptr, nullptr, nullptr};
    JS_NewClass(inst->rt, node_class_id, &cdef);
    JS_SetModuleLoaderFunc(inst->rt, module_normalize, module_loader, inst);
    JS_SetHostPromiseRejectionTracker(
        inst->rt,
        [](JSContext* ctx, JSValue promise, JSValue reason, int is_handled, void* opaque) -> void {
          const char* msg = JS_ToCString(ctx, reason);
          engine_log(2, "[promise rejection] %s", msg ? msg : "(unknown)");
          if (msg) JS_FreeCString(ctx, msg);
          JSValue stack = JS_GetPropertyStr(ctx, reason, "stack");
          if (!JS_IsUndefined(stack) && !JS_IsException(stack)) {
            const char* st = JS_ToCString(ctx, stack);
            engine_log(2, "[promise rejection stack] %s", st ? st : "");
            if (st) JS_FreeCString(ctx, st);
          }
          JS_FreeValue(ctx, stack);
        },
        inst);

    /* install C primitives */
    JSValue glob = JS_GetGlobalObject(inst->jctx);
    auto setfn = [&](const char* name, JSCFunction* fn, int len) {
      JS_SetPropertyStr(inst->jctx, glob, name, JS_NewCFunction(inst->jctx, fn, name, len));
    };
    setfn("createElement", js_create_element, 2);
    setfn("createTextNode", js_create_text, 1);
    setfn("createComment", js_create_comment, 1);
    setfn("append", js_append_child, 2);
    setfn("insertBefore", js_insert_before, 3);
    setfn("remove", js_remove_child, 2);
    setfn("childNodes", js_child_nodes, 1);
    setfn("childIndex", js_child_index, 1);
    setfn("parent", js_parent, 1);
    setfn("firstChild", js_first, 1);
    setfn("lastChild", js_last, 1);
    setfn("nextSibling", js_next, 1);
    setfn("prevSibling", js_prev, 1);
    setfn("nodeType", js_node_type, 1);
    setfn("tag", js_tag, 1);
    setfn("attr", js_get_attr, 2);
    setfn("setAttr", js_set_attr, 3);
    setfn("removeAttr", js_remove_attr, 2);
    setfn("hasAttr", js_has_attr, 2);
    setfn("attrs", js_attr_names, 1);
    setfn("setText", js_set_text, 2);
    setfn("getText", js_get_text, 1);
    setfn("setHtml", js_set_inner_html, 2);
    setfn("getHtml", js_get_inner_html, 1);
    setfn("removeAll", js_remove_all_children, 1);
    setfn("getElementById", js_get_by_id, 1);
    setfn("querySelectorAll", js_query_selector_all, 2);
    setfn("matches", js_matches, 2);
    setfn("markDirty", js_mark_dirty, 0);
    setfn("setTimeout", js_set_timeout, 2);
    setfn("setInterval", js_set_interval, 2);
    setfn("clearTimeout", js_clear_timeout, 1);
    setfn("requestAnimationFrame", js_request_animation_frame, 1);
    setfn("cancelAnimationFrame", js_cancel_animation_frame, 1);
    setfn("__sq_ls_get", js_local_storage_get, 1);
    setfn("__sq_ls_set", js_local_storage_set, 2);
    setfn("__sq_ls_remove", js_local_storage_remove, 1);
    setfn("__sq_fetch_start", js_fetch_start, 2);
    if (getenv("SQRL_NO_FETCH")) {
      JSValue stub = JS_Eval(inst->jctx, "globalThis.fetch = function(){ return Promise.reject(new Error('fetch disabled')); };", 90, "<nofetch>", JS_EVAL_TYPE_GLOBAL);
      JS_FreeValue(inst->jctx, stub);
    }
    setfn("nsUri", js_ns_uri, 1);
    setfn("docPtr", js_doc_ptr, 0);
    setfn("documentElement", js_html_element, 0);
    setfn("bodyElement", js_body_element, 0);
    setfn("headElement", js_head_element, 0);
    setfn("title", js_get_title_fn, 0);
    setfn("setTitle", js_set_title_fn, 1);
    setfn("rect", js_rect_of, 1);
    setfn("__sq_perf_now", js_perf_now, 0);
    setfn("__sq_navigate", js_navigate_to, 1);
    setfn("__sq_cookie_get", js_cookie_get, 0);
    setfn("__sq_cookie_set", js_cookie_set, 1);
    /* build the __sq namespace object the prelude expects */
    JSValue sqobj = JS_NewObject(inst->jctx);
    const char* sq_names[] = {
      "createElement", "createTextNode", "createComment", "append", "insertBefore",
      "remove", "childNodes", "childIndex", "parent", "firstChild", "lastChild",
      "nextSibling", "prevSibling", "nodeType", "tag", "nsUri", "attr", "setAttr",
      "removeAttr", "hasAttr", "attrs", "setText", "getText", "setHtml", "getHtml",
      "removeAll", "getElementById", "querySelectorAll", "matches", "markDirty",
      "documentElement", "bodyElement", "headElement", "title", "setTitle", "rect",
      nullptr};
    for (int sn = 0; sq_names[sn]; sn++) {
      JSValue v = JS_GetPropertyStr(inst->jctx, glob, sq_names[sn]);
      JS_SetPropertyStr(inst->jctx, sqobj, sq_names[sn], v);
    }
    JS_SetPropertyStr(inst->jctx, glob, "__sq", sqobj);
    JS_SetPropertyStr(inst->jctx, glob, "console", make_console(inst));
    /* keep glob referenced by the context only */
    JS_FreeValue(inst->jctx, glob);
    glob = JS_GetGlobalObject(inst->jctx); /* reacquire for the remaining property sets */
    JS_SetPropertyStr(inst->jctx, glob, "__sq_doc_ptr", JS_NewString(inst->jctx, ctx_doc_key));
    JS_SetPropertyStr(inst->jctx, glob, "__sq_url", JS_NewString(inst->jctx, inst->final_url.c_str()));
    JS_SetPropertyStr(inst->jctx, glob, "__sq_ua", JS_NewString(inst->jctx, inst->ua.c_str()));
    JS_SetPropertyStr(inst->jctx, glob, "__sq_threads", JS_NewInt32(inst->jctx, inst->pool.thread_count()));
    JS_SetPropertyStr(inst->jctx, glob, "__sq_vw", JS_NewInt32(inst->jctx, inst->vw));
    JS_SetPropertyStr(inst->jctx, glob, "__sq_vh", JS_NewInt32(inst->jctx, inst->vh));
    JS_FreeValue(inst->jctx, glob);
    /* fetch(): JS promise wrapper over the C callback bridge */
    static const char* fetch_patch =
      "(function(){"
      "globalThis.fetch=function(u,o){"
      "  const url=typeof u==='string'?u:(u&&u.url)||'';"
      "  return new Promise(function(resolve,reject){"
      "    try { globalThis.__sq_fetch_start(url, function(status, body, err){"
      "      if (err) { reject(new TypeError('fetch failed: '+err)); return; }"
      "      resolve({ ok: status>=200&&status<300, status: status, statusText: '',"
      "        _body: body,"
      "        text: function(){ return Promise.resolve(this._body||''); },"
      "        json: function(){ try { return Promise.resolve(JSON.parse(this._body||'null')); } catch(e) { return Promise.reject(new SyntaxError('bad json')); } },"
      "        headers: { get: function(){ return null; } }"
      "      });"
      "    }); } catch(e) { reject(e); }"
      "  });"
      "};"
      "})();";
    /* prelude */
    if (getenv("SQRL_NO_PRELUDE")) {
      inst->js_ready = true;
      goto js_done;
    }
    JSValue pv = JS_Eval(inst->jctx, k_js_prelude, strlen(k_js_prelude), "<prelude>",
                         JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(pv)) report_js_exception(inst);
    JS_FreeValue(inst->jctx, pv);
    {
      const char* dbg = "globalThis.__sq_dbg = [typeof document, typeof globalThis.__sq, typeof globalThis.__sq && typeof globalThis.__sq.documentElement].join('|');";
      JSValue dv = JS_Eval(inst->jctx, dbg, strlen(dbg), "<dbg>", JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(dv)) report_js_exception(inst);
      JS_FreeValue(inst->jctx, dv);
      JSValue dvv = JS_GetGlobalObject(inst->jctx);
      JSValue dbgv = JS_GetPropertyStr(inst->jctx, dvv, "__sq_dbg");
      const char* s = JS_ToCString(inst->jctx, dbgv);
      engine_log(1, "[dbg] %s", s ? s : "?");
      if (s) JS_FreeCString(inst->jctx, s);
      JS_FreeValue(inst->jctx, dbgv);
      JS_FreeValue(inst->jctx, dvv);
    }
    if (!getenv("SQRL_NO_FETCHPATCH")) {
      JSValue fp = JS_Eval(inst->jctx, fetch_patch, strlen(fetch_patch), "<fetchpatch>",
                           JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(fp)) report_js_exception(inst);
      JS_FreeValue(inst->jctx, fp);
    }

    /* scripts in DOCUMENT ORDER (inline and external interleaved) —
     * order-dependent pages (e.g. slowAES bot checks) require this */
    struct ScriptEntry {
      bool external;
      std::string payload;  /* url for external (with \x01type suffix), code for inline */
    };
    std::vector<ScriptEntry> script_queue;
    std::function<void(Node*)> scan_scripts = [&](Node* n) {
      if (n->type == N_ELEMENT && n->tag == "script") {
        const std::string* type = n->attr("type");
        const std::string* src = n->attr("src");
        std::string t = type ? *type : "";
        /* rocket-loader mangles types: "...-module" / "...-text/javascript" */
        bool is_module = t.rfind("module") == t.size() - 6 && !t.empty();
        bool is_js = t.empty() || t.find("javascript") != std::string::npos ||
                     (is_module);
        bool is_json = t.find("ld+json") != std::string::npos || t == "application/json";
        if (!is_json && is_js) {
          if (src && !src->empty()) {
            /* Cloudflare Rocket Loader only re-orders script execution; we
             * already execute in document order, and its fallback reload-loops
             * on DOMs it can't instrument — skip it. */
            if (src->find("rocket-loader") != std::string::npos) {
              engine_log(1, "[script] skipped (rocket loader): %s", src->c_str());
              return;
            }
            std::string u = url_resolve(inst->final_url, *src);
            script_queue.push_back({true, u + std::string("\x01") + (is_module ? "module" : "classic")});
          } else {
            for (Node* c : n->children)
              if (c->type == N_TEXT && !c->data.empty())
                script_queue.push_back({false, c->data});
          }
        }
      }
      for (Node* c : n->children) scan_scripts(c);
    };
    scan_scripts(inst->doc->root);
    {
      int ext = 0, inl = 0;
      for (auto& s : script_queue) (s.external ? ext : inl)++;
      engine_log(1, "scripts: %d external, %d inline", ext, inl);
    }
    for (auto& se : script_queue) {
      std::string code, u, ctype;
      bool is_module = false;
      if (se.external) {
        size_t sep = se.payload.rfind(std::string("\x01"));
        u = se.payload.substr(0, sep);
        is_module = se.payload.substr(sep + 1) == "module";
        if (getenv("SQRL_NO_SCRIPTS")) continue;
        HttpResult r = fetch_sync(inst, u, "script");
        if (!r.ok) {
          engine_log(3, "script fetch failed: %s (%s)", u.c_str(), r.err.c_str());
          continue;
        }
        code.assign(r.body.begin(), r.body.end());
        /* refuse HTML served as script (challenge/block pages). Only trust the
         * leading bytes / Content-Type — big bundles legitimately contain
         * "<body" inside template strings, so never scan the whole body. */
        ctype = r.header("Content-Type");
        bool looks_html = false;
        if (ctype.find("text/html") != std::string::npos) looks_html = true;
        else if (ctype.find("javascript") != std::string::npos ||
                 ctype.find("ecmascript") != std::string::npos)
          looks_html = false;
        else {
          std::string lead = code.substr(0, 200);
          size_t nz = lead.find_first_not_of(" \t\r\n");
          if (nz != std::string::npos &&
              (lead.compare(nz, 9, "<!DOCTYPE") == 0 || lead.compare(nz, 9, "<!doctype") == 0 ||
               lead.compare(nz, 5, "<html") == 0))
            looks_html = true;
        }
        if (looks_html) {
          engine_log(2, "[script] skipped (HTML served as script, ctype='%s', lead='%.20s'): %s",
                     ctype.c_str(), code.c_str(), u.c_str());
          continue;
        }
        inst->final_script_url = u;
      } else {
        if (getenv("SQRL_NO_INLINE")) continue;
        code = se.payload;
        u = "<inline>";
      }
      engine_log(1, "[script] eval %s (%d bytes, %s)", u.c_str(), (int)code.size(),
                 se.external ? (is_module ? "module" : "classic") : "inline");
      JSValue jv = JS_Eval(inst->jctx, code.c_str(), code.size(), u.c_str(),
                           se.external && is_module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(jv)) report_js_exception(inst);
      else engine_log(1, "[script] ok %s", u.c_str());
      JS_FreeValue(inst->jctx, jv);
      JSContext* job_ctx2 = nullptr;
      while (JS_ExecutePendingJob(inst->rt, &job_ctx2) > 0) {
      }
    }
    /* DOM lifecycle: content loaded + window load (runs app boots) */
    if (inst->jctx && !getenv("SQRL_NO_DCL")) {
      const char* domready = "globalThis.__sq_dom_content_loaded && globalThis.__sq_dom_content_loaded();";
      JSValue r1 = JS_Eval(inst->jctx, domready, strlen(domready), "<domcontentloaded>", JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(r1)) report_js_exception(inst);
      JS_FreeValue(inst->jctx, r1);
      JSContext* jcx = nullptr;
      while (JS_ExecutePendingJob(inst->rt, &jcx) > 0) {}
      const char* winload = "globalThis.__sq_window_load && globalThis.__sq_window_load();";
      JSValue r2 = JS_Eval(inst->jctx, winload, strlen(winload), "<windowload>", JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(r2)) report_js_exception(inst);
      JS_FreeValue(inst->jctx, r2);
      while (JS_ExecutePendingJob(inst->rt, &jcx) > 0) {}
    }
    inst->js_ready = true;
  }
js_done:;

  /* --- event loop: run until cancelled; repaint on dirty --- */
  set_state(inst, SQ_STATE_ASSETS, 0.75f, "assets");
  double last_frame = now_ms();
  double ready_since = -1;
  double last_activity = now_ms();
  double last_anim_frame = 0;
  while (!inst->cancel) {
    bool activity = false;
    {
      std::lock_guard<std::mutex> doclk(inst->doc_mtx);
      pump_fetches(inst);
      fire_timers(inst);
      int jobs;
      do {
        JSContext* c1 = nullptr;
        jobs = JS_ExecutePendingJob(inst->rt, &c1);
        if (jobs > 0) activity = true;
      } while (jobs > 0);
      pump_raf(inst);
      /* queued clicks dispatch on the JS thread */
      {
        std::lock_guard<std::mutex> lk(inst->js_mtx);
        if (!inst->click_queue.empty()) {
          fire_queued_clicks(inst);
          activity = true;
        }
      }

      if (inst->dirty) {
        refresh_page(inst);
        activity = true;
      } else if (inst->sel_dirty) {
        inst->sel_dirty = false;
        if (inst->root_box) {
          std::lock_guard<std::mutex> plk(inst->paint_mtx);
          if (!getenv("SQRL_NO_PAINT"))
            paint_document(inst->doc, inst->root_box, &inst->latest, &inst->images,
                           (float)inst->scroll_y, &inst->sel);
        }
        activity = true;
      }
    }
    /* running CSS animations: repaint periodically so keyframes advance.
     * Light path: re-evaluate keyframe values + layout + paint, skipping the
     * selector cascade (that's what made animated pages lag). */
    if (inst->doc && (inst->doc->sheet.has_animations || css_has_transitions()) &&
        now_ms() - last_anim_frame >= 33) {
      last_anim_frame = now_ms();
      std::lock_guard<std::mutex> doclk(inst->doc_mtx);
      if (!inst->dirty && !inst->cancel && inst->doc->body) {
        style_reanimate(inst->doc, inst->vw, inst->vh);
        int vw = inst->vw, vh = inst->vh;
        if (inst->root_box) free_box_tree(inst->root_box);
        inst->root_box = nullptr;
        inst->root_box = layout_document(inst->doc, vw, vh, &inst->images);
        inst->page_h = inst->root_box ? (int)layout_page_height(inst->root_box) : vh;
        int scroll_max = inst->page_h - vh;
        if (scroll_max < 0) scroll_max = 0;
        if (inst->scroll_y > scroll_max) inst->scroll_y = scroll_max;
        std::lock_guard<std::mutex> plk(inst->paint_mtx);
        if (inst->latest.w != vw || inst->latest.h != vh) {
          inst->latest.free_pix();
          inst->latest.alloc(vw, vh);
        }
        if (!getenv("SQRL_NO_PAINT"))
          paint_document(inst->doc, inst->root_box, &inst->latest, &inst->images,
                         (float)inst->scroll_y, &inst->sel);
      } else {
        inst->dirty = true; /* other changes pending: full refresh */
      }
    }
    /* navigation request? (link click) → leave this page; the request stays
     * queued for page_thread_entry to pick up */
    {
      std::lock_guard<std::mutex> nlk(inst->nav_mtx);
      if (inst->nav_pending) break;
    }
    /* frame pacing for rAF */
    double now = now_ms();
    if (now - last_frame >= 16) {
      /* deliver a rAF tick only if there are queued callbacks or dirty js */
      last_frame = now;
    }
    if (activity) last_activity = now;

    /* ready when assets settled + js idle */
    if (inst->state != SQ_STATE_READY) {
      bool assets_done = inst->loading <= 0;
      bool js_idle = inst->timers.empty() && inst->fetches.empty() && inst->raf_map.empty();
      if (assets_done && (js_idle || !inst->js_ready) && now - last_activity > 250) {
        std::lock_guard<std::mutex> doclk2(inst->doc_mtx);
        refresh_page(inst);
        set_state(inst, SQ_STATE_READY, 1.f, "ready");
      }
    } else {
      /* stay responsive: keep repainting on dirty, timers already handled */
      if (now - last_frame > 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

} /* namespace sqrl */
