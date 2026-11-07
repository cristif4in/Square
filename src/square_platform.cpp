/* =====================================================================
 *  Square Browser Engine — square_platform.cpp
 *  Engine instance, load pipeline, threading, QuickJS + DOM/Web API,
 *  event loop, painting orchestration.
 *  (public C API + input handling live in square_platform_api.cpp)
 * ===================================================================== */
#include "square_platform_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdarg>
#include <deque>
#include <unordered_set>

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

/* optional disk cache (SQRL_CACHE env): avoids re-hitting servers while testing */
static bool cache_lookup(const std::string& url, std::vector<uint8_t>* out) {
  const char* dir = getenv("SQRL_CACHE");
  if (!dir || url.rfind("http", 0) != 0) return false;
  uint64_t h = 1469598103934665603ull;
  for (char c : url) { h ^= (uint8_t)c; h *= 1099511628211ull; }
  char path[512];
  snprintf(path, sizeof(path), "%s/%016llx.cache", dir, (unsigned long long)h);
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out->resize(n);
  size_t rd = fread(out->data(), 1, n, f);
  fclose(f);
  return rd == (size_t)n && n > 0;
}
static void cache_store(const std::string& url, const std::vector<uint8_t>& body) {
  const char* dir = getenv("SQRL_CACHE");
  if (!dir || url.rfind("http", 0) != 0 || body.empty()) return;
  uint64_t h = 1469598103934665603ull;
  for (char c : url) { h ^= (uint8_t)c; h *= 1099511628211ull; }
  char path[512];
  snprintf(path, sizeof(path), "%s/%016llx.cache", dir, (unsigned long long)h);
  FILE* f = fopen(path, "wb");
  if (f) {
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
  }
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
  std::vector<uint8_t> cached;
  if (cache_lookup(url, &cached)) {
    HttpResult r;
    r.ok = true;
    r.status = 200;
    r.body = std::move(cached);
    r.final_url = url;
    return r;
  }
  std::vector<HttpHeader> hdrs;
  hdrs.push_back({"User-Agent", inst->ua});
  std::string dest_s = dest ? dest : "document";
  bool is_doc = dest_s == "document";
  bool is_img = dest_s == "image";
  bool is_style = dest_s == "style";
  bool is_script = dest_s == "script";
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
  if (r.ok && r.status == 200 && !looks_blocked(r)) {
    cache_store(url, r.body);
  }
  return r;
}

/* ============================ DOM binding (C level) ============================ */
JSClassID node_class_id = 0;

typedef struct {
  Node* node;
} NodeSlot;

void node_finalizer(JSRuntime* rt, JSValue val) {
  (void)rt;
  NodeSlot* slot = (NodeSlot*)JS_GetOpaque(val, node_class_id);
  if (slot) {
    /* C side owns nodes; slot just references */
    delete slot;
  }
}

std::unordered_map<int64_t, Node*> g_node_registry;
std::mutex g_reg_mtx;
int64_t g_next_node_id = 1;
Node* g_document_node = nullptr; /* "__document__" sentinel */

std::unordered_map<Node*, int64_t> g_node_ids; /* stable id per node (listener keys) */

int64_t node_id_for(Node* n) {
  std::lock_guard<std::mutex> lk(g_reg_mtx);
  auto it = g_node_ids.find(n);
  if (it != g_node_ids.end()) return it->second;
  int64_t id = g_next_node_id++;
  g_node_ids[n] = id;
  g_node_registry[id] = n;
  return id;
}

JSValue wrap_node(JSContext* ctx, Node* n) {
  if (!n) return JS_NULL;
  JSValue obj = JS_NewObjectClass(ctx, node_class_id);
  if (JS_IsException(obj)) return obj;
  NodeSlot* slot = new NodeSlot{n};
  JS_SetOpaque(obj, slot);
  int64_t id;
  {
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    /* one stable identity per node so JS wrappers share listener keys */
    auto it = g_node_ids.find(n);
    if (it != g_node_ids.end()) {
      id = it->second;
    } else {
      id = g_next_node_id++;
      g_node_ids[n] = id;
      g_node_registry[id] = n;
    }
  }
  JS_SetPropertyStr(ctx, obj, "_ptr", JS_NewInt64(ctx, id));
  return obj;
}

Node* unwrap_node_ctx(JSContext* ctx, JSValueConst v) {
  NodeSlot* slot = (NodeSlot*)JS_GetOpaque(v, node_class_id);
  if (slot) return slot->node;
  if (JS_IsNumber(v)) {
    int64_t id = 0;
    JS_ToInt64(ctx, &id, v);
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    auto it = g_node_registry.find(id);
    return it != g_node_registry.end() ? it->second : nullptr;
  }
  if (JS_IsString(v)) {
    const char* s = JS_ToCString(ctx, v);
    bool is_doc = s && strcmp(s, "__document__") == 0;
    if (s) JS_FreeCString(ctx, s);
    if (is_doc) return g_document_node;
  }
  return nullptr;
}
#define unwrap_node(v) unwrap_node_ctx(ctx, (v))

void mark_dirty(Instance* inst) {
  inst->dirty = true;
}

static JSValue js_create_element(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* tag = argc > 0 ? JS_ToCString(ctx, argv[0]) : "div";
  const char* ns = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
  if (!tag) return JS_ThrowTypeError(ctx, "bad tag");
  Node* n = new Node();
  n->type = N_ELEMENT;
  n->tag = plat_lc(tag);
  if (ns && strstr(ns, "svg")) {
    n->ns = "svg";
    n->in_svg = true;
  }
  JS_FreeCString(ctx, tag);
  if (ns) JS_FreeCString(ctx, ns);
  mark_dirty(inst);
  return wrap_node(ctx, n);
}

static JSValue js_create_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* data = argc > 0 ? JS_ToCString(ctx, argv[0]) : "";
  Node* n = new Node();
  n->type = N_TEXT;
  n->data = data ? data : "";
  if (data) JS_FreeCString(ctx, data);
  mark_dirty(inst);
  return wrap_node(ctx, n);
}

static JSValue js_create_comment(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* data = argc > 0 ? JS_ToCString(ctx, argv[0]) : "";
  Node* n = new Node();
  n->type = N_COMMENT;
  n->data = data ? data : "";
  if (data) JS_FreeCString(ctx, data);
  mark_dirty(inst);
  return wrap_node(ctx, n);
}

static JSValue js_append_child(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  if (argc < 2) return JS_ThrowTypeError(ctx, "appendChild(parent, child)");
  Node* parent = unwrap_node(argv[0]);
  Node* child = unwrap_node(argv[1]);
  if (!parent || !child) return JS_ThrowTypeError(ctx, "bad node");
  if (child->parent) {
    auto& sibs = child->parent->children;
    sibs.erase(std::remove(sibs.begin(), sibs.end(), child), sibs.end());
  }
  child->parent = parent;
  parent->children.push_back(child);
  mark_dirty(inst);
  return JS_DupValue(ctx, argv[1]);
}

static JSValue js_insert_before(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  if (argc < 2) return JS_ThrowTypeError(ctx, "insertBefore(parent, child, ref?)");
  Node* parent = unwrap_node(argv[0]);
  Node* child = unwrap_node(argv[1]);
  Node* ref = argc > 2 && !JS_IsNull(argv[2]) ? unwrap_node(argv[2]) : nullptr;
  if (!parent || !child) return JS_ThrowTypeError(ctx, "bad node");
  if (child->parent) {
    auto& sibs = child->parent->children;
    sibs.erase(std::remove(sibs.begin(), sibs.end(), child), sibs.end());
  }
  child->parent = parent;
  auto it = std::find(parent->children.begin(), parent->children.end(), ref);
  if (ref && it != parent->children.end())
    parent->children.insert(it, child);
  else
    parent->children.push_back(child);
  mark_dirty(inst);
  return JS_DupValue(ctx, argv[1]);
}

static JSValue js_remove_child(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  if (argc < 2) return JS_ThrowTypeError(ctx, "removeChild(parent, child)");
  Node* parent = unwrap_node(argv[0]);
  Node* child = unwrap_node(argv[1]);
  if (!parent || !child) return JS_ThrowTypeError(ctx, "bad node");
  auto& sibs = parent->children;
  sibs.erase(std::remove(sibs.begin(), sibs.end(), child), sibs.end());
  child->parent = nullptr;
  mark_dirty(inst);
  return JS_DupValue(ctx, argv[1]);
}

static JSValue js_child_index(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_NewInt32(ctx, -1);
  Node* n = unwrap_node(argv[0]);
  if (!n) return JS_NewInt32(ctx, -1);
  return JS_NewInt32(ctx, n->child_index());
}

static JSValue js_child_nodes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "childNodes(node)");
  Node* n = unwrap_node(argv[0]);
  if (!n) return JS_NewArray(ctx);
  JSValue arr = JS_NewArray(ctx);
  int i = 0;
  for (Node* c : n->children) {
    JS_SetPropertyUint32(ctx, arr, i++, wrap_node(ctx, c));
  }
  return arr;
}

static JSValue js_relative(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int which) {
  if (argc < 1) return JS_NULL;
  Node* n = unwrap_node(argv[0]);
  if (!n) return JS_NULL;
  switch (which) {
    case 0: return wrap_node(ctx, n->parent);
    case 1: {
      for (Node* c : n->children)
        if (c->type == N_ELEMENT || c->type == N_TEXT) return wrap_node(ctx, c);
      return JS_NULL;
    }
    case 2: {
      for (int i = (int)n->children.size() - 1; i >= 0; i--) {
        Node* c = n->children[i];
        if (c->type == N_ELEMENT || c->type == N_TEXT) return wrap_node(ctx, c);
      }
      return JS_NULL;
    }
    case 3: {
      if (!n->parent) return JS_NULL;
      int idx = n->child_index();
      if (idx < 0) return JS_NULL;
      for (int i = idx + 1; i < (int)n->parent->children.size(); i++) {
        Node* c = n->parent->children[i];
        if (c->type == N_ELEMENT || c->type == N_TEXT) return wrap_node(ctx, c);
      }
      return JS_NULL;
    }
    case 4: {
      if (!n->parent) return JS_NULL;
      int idx = n->child_index();
      for (int i = idx - 1; i >= 0; i--) {
        Node* c = n->parent->children[i];
        if (c->type == N_ELEMENT || c->type == N_TEXT) return wrap_node(ctx, c);
      }
      return JS_NULL;
    }
  }
  return JS_NULL;
}

static JSValue js_parent(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 0); }
static JSValue js_first(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 1); }
static JSValue js_last(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 2); }
static JSValue js_next(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 3); }
static JSValue js_prev(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 4); }

static JSValue js_node_type(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  if (!n) return JS_NewInt32(ctx, 0);
  switch (n->type) {
    case N_ELEMENT: return JS_NewInt32(ctx, 1);
    case N_TEXT: return JS_NewInt32(ctx, 3);
    case N_COMMENT: return JS_NewInt32(ctx, 8);
    case N_DOCUMENT: return JS_NewInt32(ctx, 9);
    case N_DOCTYPE: return JS_NewInt32(ctx, 10);
  }
  return JS_NewInt32(ctx, 0);
}

static JSValue js_tag(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  if (!n) return JS_NewString(ctx, "");
  if (n->type == N_TEXT) return JS_NewString(ctx, "#text");
  if (n->type == N_COMMENT) return JS_NewString(ctx, "#comment");
  if (n->type == N_DOCUMENT) return JS_NewString(ctx, "#document");
  std::string t = n->tag;
  if (n->ns == "svg") {
    /* svg tags keep camelCase for some elements */
    static const char* camel[] = {"lineargradient", "radialgradient", "clippath", "textpath",
                                  "feblend", "fecolormatrix", "fecomposite", "feconvolvematrix",
                                  "fediffuselighting", "fedisplacementmap", "fedistantlight",
                                  "fedropshadow", "feflood", "fefunca", "fefuncb", "fefuncg",
                                  "fefuncr", "fegaussianblur", "femerge", "femergenode",
                                  "femorphology", "feoffset", "fepointlight", "fespecularlighting",
                                  "fespotlight", "fetile", "feturbulence", "foreignobject", nullptr};
    for (int i = 0; camel[i]; i++)
      if (t == camel[i]) {
        /* camelcase it */
        std::string out;
        bool up = false;
        for (char ch : t) {
          if (ch == '-') { up = true; continue; }
          out += up ? (char)toupper((unsigned char)ch) : ch;
          up = false;
        }
        return JS_NewStringLen(ctx, out.c_str(), out.size());
      }
  }
  return JS_NewStringLen(ctx, t.c_str(), t.size());
}

static JSValue js_get_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  const char* name = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
  if (!n || !name) {
    if (name) JS_FreeCString(ctx, name);
    return JS_NULL;
  }
  const std::string* v = n->attr(name);
  JSValue r = v ? JS_NewStringLen(ctx, v->c_str(), v->size()) : JS_NULL;
  JS_FreeCString(ctx, name);
  return r;
}

static JSValue js_set_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  const char* name = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
  const char* val = argc > 2 ? JS_ToCString(ctx, argv[2]) : "";
  if (!n || !name) {
    if (name) JS_FreeCString(ctx, name);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
  }
#ifdef LAYOUT_DEBUG
  if (getenv("SQRL_PROBE")) fprintf(stderr, "[probe] set_attr node=%p name=%s\n", (void*)n, name);
#endif
  n->set_attr(plat_lc(name), val ? val : "");
  JS_FreeCString(ctx, name);
  if (val) JS_FreeCString(ctx, val);
  mark_dirty(inst);
  return JS_UNDEFINED;
}

static JSValue js_remove_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  const char* name = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
  if (n && name) {
    n->remove_attr(plat_lc(name));
    mark_dirty(inst);
  }
  if (name) JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

static JSValue js_has_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  const char* name = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
  bool has = n && name && n->attr(name) != nullptr;
  if (name) JS_FreeCString(ctx, name);
  return JS_NewBool(ctx, has);
}

static JSValue js_set_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  const char* val = argc > 1 ? JS_ToCString(ctx, argv[1]) : "";
  if (n) {
    if (n->type == N_TEXT) {
      n->data = val ? val : "";
    } else {
      /* replace children with single text node (graveyard, not freed) */
      for (Node* c : n->children) { c->parent = nullptr; inst->graveyard.push_back(c); }
      n->children.clear();
      if (val && *val) {
        Node* t = new Node();
        t->type = N_TEXT;
        t->data = val;
        t->parent = n;
        n->children.push_back(t);
      }
    }
    mark_dirty(inst);
  }
  if (val) JS_FreeCString(ctx, val);
  return JS_UNDEFINED;
}

static JSValue js_get_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  if (!n) return JS_NewString(ctx, "");
  std::string out;
  std::function<void(Node*)> walk = [&](Node* x) {
    if (x->type == N_TEXT) out += x->data;
    for (Node* c : x->children) walk(c);
  };
  if (n->type == N_TEXT) out = n->data;
  else walk(n);
  return JS_NewStringLen(ctx, out.c_str(), out.size());
}

static JSValue js_set_inner_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  const char* val = argc > 1 ? JS_ToCString(ctx, argv[1]) : "";
  if (n) {
    for (Node* c : n->children) { c->parent = nullptr; inst->graveyard.push_back(c); }
    n->children.clear();
    if (val && *val) {
      std::vector<Node*> frag = html_parse_fragment(val);
      for (Node* c : frag) {
        c->parent = n;
        n->children.push_back(c);
      }
    }
    mark_dirty(inst);
  }
  if (val) JS_FreeCString(ctx, val);
  return JS_UNDEFINED;
}

static JSValue js_get_inner_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  if (!n) return JS_NewString(ctx, "");
  std::string out = serialize_html(n);
  return JS_NewStringLen(ctx, out.c_str(), out.size());
}

static JSValue js_get_by_id(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* id = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
  if (!id) return JS_NULL;
  Node* found = nullptr;
  if (inst->doc && inst->doc->root) {
    std::function<void(Node*)> walk = [&](Node* x) {
      if (found) return;
      if (x->type == N_ELEMENT) {
        const std::string* v = x->attr("id");
        if (v && *v == id) {
          found = x;
          return;
        }
      }
      for (Node* c : x->children) walk(c);
    };
    walk(inst->doc->root);
  }
  JS_FreeCString(ctx, id);
  return found ? wrap_node(ctx, found) : JS_NULL;
}

static JSValue js_query_selector_all(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  Node* scope = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  const char* sel = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
  JSValue arr = JS_NewArray(ctx);
  if (!sel) return arr;
  Selector parsed;
  if (!css_parse_selector_public(sel, &parsed)) {
    JS_FreeCString(ctx, sel);
    return arr;
  }
  Node* start = scope ? scope : (inst->doc ? inst->doc->root : nullptr);
  if (start) {
    bool scope_is_elem = scope != nullptr;
    int i = 0;
    std::function<void(Node*)> walk = [&](Node* x) {
      if (x->type == N_ELEMENT && (!scope_is_elem || x != scope) && selector_matches(x, parsed)) {
        JS_SetPropertyUint32(ctx, arr, i++, wrap_node(ctx, x));
      }
      for (Node* c : x->children) walk(c);
    };
    walk(start);
  }
  JS_FreeCString(ctx, sel);
  return arr;
}

static JSValue js_mark_dirty(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  mark_dirty(inst);
  return JS_UNDEFINED;
}

static JSValue timer_set(JSContext* ctx, int argc, JSValueConst* argv, bool is_interval) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
  double delay = 0;
  if (argc > 1) JS_ToFloat64(ctx, &delay, argv[1]);
  std::lock_guard<std::mutex> lk(inst->js_mtx);
  Timer t;
  t.id = inst->next_timer_id++;
  t.when = now_ms() + std::max(0.0, delay);
  t.interval = is_interval ? std::max(0.0, delay) : 0;
  t.fn = JS_DupValue(ctx, argv[0]);
  inst->timers.push_back(t);
#ifdef LAYOUT_DEBUG
  engine_log(1, "[timer] set id=%d delay=%.1f interval=%d", t.id, delay, t.interval);
#endif
  return JS_NewInt32(ctx, t.id);
}

static JSValue js_set_timeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return timer_set(ctx, argc, argv, false);
}

static JSValue js_set_interval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return timer_set(ctx, argc, argv, true);
}

static JSValue js_clear_timeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  int32_t id = 0;
  if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
  std::lock_guard<std::mutex> lk(inst->js_mtx);
  for (auto it = inst->timers.begin(); it != inst->timers.end(); ++it) {
    if (it->id == id) {
      JS_FreeValue(ctx, it->fn);
      inst->timers.erase(it);
      break;
    }
  }
  return JS_UNDEFINED;
}

static JSValue js_request_animation_frame(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
  std::lock_guard<std::mutex> lk(inst->js_mtx);
  int id = inst->raf_id++;
  inst->raf_map[id] = JS_DupValue(ctx, argv[0]);
  return JS_NewInt32(ctx, id);
}

static JSValue js_cancel_animation_frame(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  int32_t id = 0;
  if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
  std::lock_guard<std::mutex> lk(inst->js_mtx);
  auto it = inst->raf_map.find(id);
  if (it != inst->raf_map.end()) {
    JS_FreeValue(ctx, it->second);
    inst->raf_map.erase(it);
  }
  return JS_UNDEFINED;
}

static JSValue js_local_storage_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* k = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
  if (!k) return JS_NULL;
  auto it = inst->local_storage.find(k);
  JSValue r = it != inst->local_storage.end() ? JS_NewStringLen(ctx, it->second.c_str(), it->second.size()) : JS_NULL;
  JS_FreeCString(ctx, k);
  return r;
}

static JSValue js_local_storage_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* k = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
  const char* v = argc > 1 ? JS_ToCString(ctx, argv[1]) : "";
  if (k) {
    inst->local_storage[k] = v ? v : "";
    JS_FreeCString(ctx, k);
  }
  if (v) JS_FreeCString(ctx, v);
  return JS_UNDEFINED;
}

static JSValue js_local_storage_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* k = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
  if (k) {
    inst->local_storage.erase(k);
    JS_FreeCString(ctx, k);
  }
  return JS_UNDEFINED;
}

} /* namespace sqrl */
/* =====================================================================
 *  Square Browser Engine — square_platform.cpp (part 2: JS runtime,
 *  prelude, event loop)
 * ===================================================================== */

namespace sqrl {

/* ============================ module loader ============================ */

static char* module_normalize(JSContext* ctx, const char* base_name, const char* name, void* opaque) {
  Instance* inst = (Instance*)opaque;
  std::string base = base_name;
  std::string rel = name;
  std::string resolved;
  if (rel.rfind("http", 0) == 0 || rel.rfind("file:", 0) == 0 || rel.rfind("data:", 0) == 0)
    resolved = rel;
  else {
    std::string base_dir = base;
    size_t slash = base.rfind('/');
    if (slash != std::string::npos) base_dir = base.substr(0, slash + 1);
    resolved = url_resolve(base_dir, rel);
  }
  (void)inst;
  return js_strdup(ctx, resolved.c_str());
}

static std::unordered_map<std::string, std::string> g_module_cache;
static std::mutex g_module_mtx;

static JSModuleDef* module_loader(JSContext* ctx, const char* module_name, void* opaque) {
  Instance* inst = (Instance*)opaque;
  std::string url = module_name;
  std::string code;
  {
    std::lock_guard<std::mutex> lk(g_module_mtx);
    auto it = g_module_cache.find(url);
    if (it != g_module_cache.end()) code = it->second;
  }
  if (code.empty()) {
    HttpResult r = fetch_sync(inst, url);
    if (!r.ok || r.status >= 400) {
      JS_ThrowReferenceError(ctx, "could not load module %s: %s", url.c_str(), r.err.c_str());
      return nullptr;
    }
    code.assign(r.body.begin(), r.body.end());
    {
      std::string ctype = r.header("Content-Type");
      std::string head = code.substr(0, 40);
      if (ctype.find("text/html") != std::string::npos ||
          head.rfind("<!DOCTYPE", 0) == 0 || head.rfind("<!doctype", 0) == 0 ||
          head.rfind("<html", 0) == 0) {
        JS_ThrowReferenceError(ctx, "module %s returned HTML (blocked or 404 page)", url.c_str());
        return nullptr;
      }
    }
    std::lock_guard<std::mutex> lk(g_module_mtx);
    g_module_cache[url] = code;
#ifdef LAYOUT_DEBUG
    engine_log(1, "[module] loaded %s (%d bytes)", url.c_str(), (int)code.size());
#endif
  }
  JSValue func = JS_Eval(ctx, code.c_str(), code.size(), url.c_str(),
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(func)) return nullptr;
  JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(func);
  JS_FreeValue(ctx, func);
  return m;
}

/* ============================ JS prelude (DOM API in JS) ============================ */
static const char* ctx_doc_key = "__document__";

static const char* k_js_prelude = R"PRELUDE(
/* window/self aliases: the engine exposes globals on globalThis */
if (typeof window === 'undefined') {
  globalThis.window = globalThis;
  globalThis.self = globalThis;
  globalThis.top = globalThis;
  globalThis.parent = globalThis;
  globalThis.frames = globalThis;
}
(function() {
"use strict";
// ---- Node handles are opaque objects with .ptr = unique id; C primitives global ----
const C = globalThis.__sq;

let nodeCounter = 1;
const nodeMap = new Map(); // ptr -> JS wrapper

function wrap(ptr) {
  if (ptr === null || ptr === undefined) return null;
  let w = nodeMap.get(ptr);
  if (!w) { w = makeWrapper(ptr); nodeMap.set(ptr, w); }
  return w;
}

function makeWrapper(ptr) {
  const handler = {
    get(target, prop) { return Reflect.get(target, prop); }
  };
  const w = new SQNode(ptr);
  return w;
}

class SQStyle {
  constructor(node) {
    this._node = node;
    this._props = {};
    this._load();
  }
  _load() {
    const css = C.attr(this._node._ptr, 'style');
    this._props = {};
    if (css) {
      for (const decl of css.split(';')) {
        const i = decl.indexOf(':');
        if (i < 0) continue;
        const name = decl.slice(0, i).trim();
        const val = decl.slice(i + 1).trim();
        if (name) this._props[name] = val;
      }
    }
  }
  _save() {
    const parts = [];
    for (const k in this._props) parts.push(k + ': ' + this._props[k]);
    C.setAttr(this._node._ptr, 'style', parts.join('; '));
  }
  setProperty(name, val, prio) {
    name = String(name);
    if (val === null || val === undefined || val === '') delete this._props[name];
    else this._props[name] = String(val);
    this._save();
  }
  removeProperty(name) { delete this._props[name]; this._save(); }
  getPropertyValue(name) { return this._props[name] || ''; }
  item(i) { return Object.keys(this._props)[i] || ''; }
  get length() { return Object.keys(this._props).length; }
  get cssText() {
    const parts = [];
    for (const k in this._props) parts.push(k + ': ' + this._props[k]);
    return parts.join('; ');
  }
  set cssText(v) {
    this._props = {};
    if (v) {
      for (const decl of String(v).split(';')) {
        const i = decl.indexOf(':');
        if (i < 0) continue;
        this._props[decl.slice(0, i).trim()] = decl.slice(i + 1).trim();
      }
    }
    this._save();
  }
}

class SQClassList {
  constructor(node) { this._node = node; }
  _arr() {
    const c = C.attr(this._node._ptr, 'class');
    return c ? c.split(/\s+/).filter(x => x) : [];
  }
  _save(a) { C.setAttr(this._node._ptr, 'class', a.join(' ')); }
  add(...names) { const a = this._arr(); for (const n of names) if (!a.includes(n)) a.push(n); this._save(a); }
  remove(...names) { this._save(this._arr().filter(n => !names.includes(n))); }
  toggle(n, force) {
    const a = this._arr();
    const has = a.includes(n);
    if (force === undefined ? !has : force) { if (!has) a.push(n); this._save(a); return true; }
    this._save(a.filter(x => x !== n)); return false;
  }
  contains(n) { return this._arr().includes(n); }
  get length() { return this._arr().length; }
  item(i) { return this._arr()[i] || null; }
  toString() { return this._arr().join(' '); }
}

class SQNode {
  constructor(ptr) {
    // C bindings pass either an id (number/string) or the raw handle object
    while (ptr && typeof ptr === 'object' && ptr._ptr !== undefined && !(ptr instanceof SQNode === false && ptr.nodeType === undefined)) {
      if (typeof ptr._ptr === 'object' && ptr._ptr !== null && ptr._ptr._ptr !== undefined) { ptr = ptr._ptr; continue; }
      if (typeof ptr._ptr === 'number' || typeof ptr._ptr === 'string' || ptr._ptr === null) { ptr = ptr._ptr; }
      break;
    }
    this._ptr = ptr;
  }
  get nodeType() { return C.nodeType(this._ptr); }
  get tagName() { const t = C.tag(this._ptr); return typeof t === 'string' ? t.toUpperCase() : t; }
  get nodeName() { return C.tag(this._ptr); }
  get namespaceURI() { return C.nsUri(this._ptr); }
  get parentNode() { return wrap(C.parent(this._ptr)); }
  get parentElement() { return wrap(C.parent(this._ptr)); }
  get firstChild() { return wrap(C.firstChild(this._ptr)); }
  get lastChild() { return wrap(C.lastChild(this._ptr)); }
  get nextSibling() { return wrap(C.nextSibling(this._ptr)); }
  get previousSibling() { return wrap(C.prevSibling(this._ptr)); }
  get ownerDocument() { return document; }
  get isConnected() { return true; }
  get childNodes() {
    const arr = C.childNodes(this._ptr);
    const out = [];
    for (let i = 0; i < arr.length; i++) out.push(wrap(arr[i]));
    out.item = (i) => out[i] || null;
    return out;
  }
  get children() {
    const out = [];
    for (const c of this.childNodes) if (c.nodeType === 1) out.push(c);
    out.item = (i) => out[i] || null;
    return out;
  }
  get childElementCount() { return this.children.length; }
  get firstElementChild() { const c = this.children; return c[0] || null; }
  get lastElementChild() { const c = this.children; return c[c.length - 1] || null; }
  get textContent() { return C.getText(this._ptr); }
  set textContent(v) {
    if (this.nodeType === 3) C.setText(this._ptr, String(v));
    else {
      C.removeAll(this._ptr);
      if (v !== '' && v !== null && v !== undefined) this.appendChild(document.createTextNode(String(v)));
    }
  }
  get innerHTML() { return C.getHtml(this._ptr); }
  set innerHTML(v) { C.setHtml(this._ptr, String(v)); }
  get id() { return C.attr(this._ptr, 'id') || ''; }
  set id(v) { C.setAttr(this._ptr, 'id', String(v)); }
  get className() { return C.attr(this._ptr, 'class') || ''; }
  set className(v) { C.setAttr(this._ptr, 'class', String(v)); }
  get style() {
    if (!this._style) this._style = new SQStyle(this);
    return this._style;
  }
  get classList() {
    if (!this._classList) this._classList = new SQClassList(this);
    return this._classList;
  }
  get dataset() {
    if (!this._dataset) {
      const self = this;
      this._dataset = new Proxy({}, {
        get(t, k) {
          const v = C.attr(self._ptr, 'data-' + kebab(k));
          return v === null ? undefined : v;
        },
        set(t, k, v) { C.setAttr(self._ptr, 'data-' + kebab(k), String(v)); return true; },
        deleteProperty(t, k) { C.removeAttr(self._ptr, 'data-' + kebab(k)); return true; },
        has(t, k) { return C.attr(self._ptr, 'data-' + kebab(k)) !== null; }
      });
    }
    return this._dataset;
  }
  getAttribute(name) { return C.attr(this._ptr, String(name)); }
  setAttribute(name, val) { C.setAttr(this._ptr, String(name), val === null || val === undefined ? '' : String(val)); }
  removeAttribute(name) { C.removeAttr(this._ptr, String(name)); }
  hasAttribute(name) { return C.attr(this._ptr, String(name)) !== null; }
  hasAttributes() { return C.attrs(this._ptr).length > 0; }
  getAttributeNames() { return C.attrs(this._ptr); }
  appendChild(child) {
    if (child && child._fragment) {
      for (const c of child._fragment) this.appendChild(c);
      child._fragment = [];
      return child;
    }
    C.append(this._ptr, child._ptr);
    return child;
  }
  insertBefore(child, ref) {
    if (child && child._fragment) {
      for (const c of child._fragment) this.insertBefore(c, ref);
      child._fragment = [];
      return child;
    }
    if (ref && ref._ptr !== undefined && C.parent(ref._ptr) !== null && ptrEq(C.parent(ref._ptr), this._ptr)) {
      C.insertBefore(this._ptr, child._ptr, ref._ptr);
    } else {
      C.append(this._ptr, child._ptr);
    }
    return child;
  }
  removeChild(child) { C.remove(this._ptr, child._ptr); return child; }
  insertAdjacentHTML(position, html) {
    const p = C.parent(this._ptr);
    if ((position === 'beforebegin' || position === 'afterend') && p === null) return;
    /* parse into a detached temp element, then move the nodes into place */
    const tmp = document.createElement('div');
    C.setHtml(tmp._ptr, String(html == null ? '' : html));
    const arr = C.childNodes(tmp._ptr);
    for (let i = 0; i < arr.length; i++) {
      const k = new SQNode(arr[i]);
      if (position === 'beforeend') {
        C.append(this._ptr, k._ptr);
      } else if (position === 'afterbegin') {
        C.insertBefore(this._ptr, k._ptr, C.firstChild(this._ptr));
      } else if (position === 'afterend') {
        C.insertBefore(p, k._ptr, C.nextSibling(this._ptr));
      } else if (position === 'beforebegin') {
        C.insertBefore(p, k._ptr, this._ptr);
      }
    }
  }
  insertAdjacentElement(position, el) {
    const p = C.parent(this._ptr);
    if ((position === 'beforebegin' || position === 'afterend') && p === null) return null;
    if (!el || el._ptr === undefined) return null;
    if (position === 'beforeend') C.append(this._ptr, el._ptr);
    else if (position === 'afterbegin') C.insertBefore(this._ptr, el._ptr, C.firstChild(this._ptr));
    else if (position === 'afterend') C.insertBefore(p, el._ptr, C.nextSibling(this._ptr));
    else if (position === 'beforebegin') C.insertBefore(p, el._ptr, this._ptr);
    return el;
  }
  insertAdjacentText(position, text) {
    const t = document.createTextNode(String(text == null ? '' : text));
    this.insertAdjacentElement(position, t);
    return t;
  }
  append(...nodes) { for (const n of nodes) this.appendChild(typeof n === 'object' ? n : document.createTextNode(String(n))); }
  prepend(...nodes) { for (const n of nodes.reverse()) this.insertBefore(typeof n === 'object' ? n : document.createTextNode(String(n)), this.firstChild); }
  remove() { const p = C.parent(this._ptr); if (p !== null) C.remove(p, this._ptr); }
  replaceChild(nu, old) { this.insertBefore(nu, old); this.removeChild(old); return old; }
  replaceWith(...nodes) {
    const p = C.parent(this._ptr);
    if (p === null) return;
    for (const n of nodes) {
      const real = typeof n === 'object' ? n : document.createTextNode(String(n));
      p.insertBefore(real, this);
    }
    p.removeChild(this);
  }
  contains(other) {
    let cur = other;
    while (cur) {
      if (cur === this) return true;
      cur = cur.parentNode;
    }
    return false;
  }
  closest(sel) {
    let cur = this;
    while (cur) {
      if (cur.matches && cur.matches(sel)) return cur;
      cur = cur.parentElement;
    }
    return null;
  }
  matches(sel) {
    return C.matches(this._ptr, String(sel));
  }
  querySelector(sel) {
    const all = C.querySelectorAll(this._ptr, String(sel));
    return all.length ? wrap(all[0]) : null;
  }
  querySelectorAll(sel) {
    const all = C.querySelectorAll(this._ptr, String(sel));
    const out = [];
    for (const p of all) out.push(wrap(p));
    out.item = (i) => out[i] || null;
    return out;
  }
  addEventListener(type, fn, opts) { addEventListenerImpl(this, type, fn, opts); }
  removeEventListener(type, fn, opts) { removeEventListenerImpl(this, type, fn, opts); }
  dispatchEvent(ev) { return dispatchImpl(this, ev); }
  getBoundingClientRect() {
    const r = C.rect(this._ptr);
    return { x: r[0], y: r[1], width: r[2], height: r[3], top: r[1], left: r[0], right: r[0] + r[2], bottom: r[1] + r[3], toJSON(){} };
  }
  get offsetWidth() { return C.rect(this._ptr)[2]; }
  get offsetHeight() { return C.rect(this._ptr)[3]; }
  focus() {}
  blur() {}
  click() {}
  scrollIntoView() {}
  get value() { return this.getAttribute('value') || ''; }
  set value(v) { this.setAttribute('value', v); }
  get checked() { return this.hasAttribute('checked'); }
  set checked(v) { if (v) this.setAttribute('checked', ''); else this.removeAttribute('checked'); }
  get disabled() { return this.hasAttribute('disabled'); }
  set disabled(v) { if (v) this.setAttribute('disabled', ''); else this.removeAttribute('disabled'); }
  get hidden() { return this.hasAttribute('hidden'); }
  set hidden(v) { if (v) this.setAttribute('hidden', ''); else this.removeAttribute('hidden'); }
}

function kebab(k) {
  return String(k).replace(/[A-Z]/g, m => '-' + m.toLowerCase());
}
function ptrEq(a, b) {
  return a === b || (a && b && a.ptr === b.ptr);
}

// fragment
class SQFragment extends SQNode {
  constructor() { super('__fragment__'); this._fragment = []; this._ptr = null; this.nodeTypeValue = 11; }
  get nodeType() { return 11; }
}

// ---- events ----
const eventListeners = new Map(); // key: nodeptr + type -> Set<fn>

function listenerKey(node, type) {
  let p = (node && node._ptr !== undefined) ? node._ptr : node;
  if (p !== null && typeof p === 'object' && p._ptr !== undefined) p = p._ptr;
  let k;
  try { k = String(p); } catch (e) { k = 'obj#' + (node && node.__uid ? node.__uid : Math.random()); }
  return k + '\u0000' + type;
}
function addEventListenerImpl(node, type, fn, opts) {
  const k = listenerKey(node, type);
  if (!eventListeners.has(k)) eventListeners.set(k, new Set());
  eventListeners.get(k).add(fn);
}
function removeEventListenerImpl(node, type, fn) {
  const k = listenerKey(node, type);
  const s = eventListeners.get(k);
  if (s) s.delete(fn);
}
function dispatchImpl(node, ev) {
  const k = listenerKey(node, ev.type);
  const s = eventListeners.get(k);
  if (s) for (const fn of [...s]) fn.call(node, ev);
  return true;
}

class SQEvent {
  constructor(type, init) {
    this.type = type;
    this.detail = init && init.detail;
    this.bubbles = !!(init && init.bubbles);
    this.cancelable = !!(init && init.cancelable);
    this.defaultPrevented = false;
    this.target = null;
    this.currentTarget = null;
  }
  preventDefault() { this.defaultPrevented = true; }
  stopPropagation() {}
  stopImmediatePropagation() {}
}
class SQCustomEvent extends SQEvent {
  constructor(type, init) { super(type, init); }
}

// ---- document ----
class SQDocument extends SQNode {
  constructor() { super(globalThis.__sq_doc_ptr); }
  get nodeType() { return 9; }
  get documentElement() { return wrap(C.documentElement()); }
  get body() { return wrap(C.bodyElement()); }
  get head() { return wrap(C.headElement()); }
  get title() { return C.title(); }
  set title(v) { C.setTitle(String(v)); }
  get location() { return globalThis.location; }
  get URL() { return globalThis.location.href; }
  get cookie() { return globalThis.__sq_cookie_get ? globalThis.__sq_cookie_get() : ''; }
  set cookie(v) { if (globalThis.__sq_cookie_set) globalThis.__sq_cookie_set(String(v)); }
  get documentURI() { return globalThis.location.href; }
  createElement(tag) { return wrap(C.createElement(String(tag))); }
  createElementNS(ns, tag) { return wrap(C.createElement(String(tag), String(ns))); }
  createTextNode(t) { return wrap(C.createTextNode(String(t))); }
  createComment(t) { return wrap(C.createComment(String(t || ''))); }
  createDocumentFragment() { return new SQFragment(); }
  createEvent(type) { return new SQEvent(type.toLowerCase().replace('events', '')); }
  getElementById(id) { return wrap(C.getElementById(String(id))); }
  getElementsByTagName(tag) {
    const out = [];
    for (const p of C.querySelectorAll(null, String(tag).toLowerCase())) out.push(wrap(p));
    out.item = (i) => out[i] || null;
    return out;
  }
  getElementsByClassName(cls) {
    const out = [];
    for (const p of C.querySelectorAll(null, '.' + String(cls))) out.push(wrap(p));
    out.item = (i) => out[i] || null;
    return out;
  }
  querySelector(sel) {
    const all = C.querySelectorAll(null, String(sel));
    return all.length ? wrap(all[0]) : null;
  }
  querySelectorAll(sel) {
    const out = [];
    for (const p of C.querySelectorAll(null, String(sel))) out.push(wrap(p));
    out.item = (i) => out[i] || null;
    return out;
  }
  addEventListener(type, fn, opts) { addEventListenerImpl(this, type, fn, opts); }
  removeEventListener(type, fn) { removeEventListenerImpl(this, type, fn); }
  dispatchEvent(ev) { return dispatchImpl(this, ev); }
}

// ---- window / globals ----
class SQLocation {
  constructor(href) {
    const m = /^([a-z]+):\/\/([^\/?#]+)([^?#]*)(\?[^#]*)?(#.*)?$/.exec(href) || [null, 'https:', '', '/', '', ''];
    /* internal _href — assigning this.href would recurse into the setter */
    this._href = href;
    this.protocol = m[1] === 'file' ? 'file:' : m[1] + ':';
    this.host = m[2];
    this.hostname = m[2].split(':')[0];
    this.port = m[2].includes(':') ? m[2].split(':')[1] : '';
    this.pathname = m[3] || '/';
    this.search = m[4] || '';
    this.hash = m[5] || '';
    this.origin = this.protocol + '//' + m[2];
  }
  assign(u) { globalThis.__sq_navigate(u); }
  replace(u) { globalThis.__sq_navigate(u); }
  reload() { globalThis.__sq_navigate(this._href); }
  get href() { return this._href; }
  set href(u) {
    const cur = this._href;
    const next = String(u);
    if (next === cur) return; /* same-URL href assignment is a no-op */
    globalThis.__sq_navigate(next);
  }
  toString() { return this._href; }
}

class SQStorage {
  constructor(getter, setter, remover) { this._g = getter; this._s = setter; this._r = remover; }
  getItem(k) { return this._g(String(k)); }
  setItem(k, v) { this._s(String(k), String(v)); }
  removeItem(k) { this._r(String(k)); }
  clear() {}
  key(i) { return null; }
  get length() { return 0; }
}

class SQMediaQueryList {
  constructor(matches) { this.matches = matches; }
  addListener() {}
  removeListener() {}
  addEventListener() {}
  removeEventListener() {}
}

const listeners = new Map();
function winListeners() { return listeners; }

function addEventListenerImpl2(type, fn) {
  if (!listeners.has(type)) listeners.set(type, new Set());
  listeners.get(type).add(fn);
}

// navigator
const navigator = {
  userAgent: globalThis.__sq_ua,
  language: 'en-US',
  languages: ['en-US', 'en'],
  platform: 'Win32',
  hardwareConcurrency: globalThis.__sq_threads,
  maxTouchPoints: 0,
  vendor: 'Square',
  appVersion: '5.0',
  onLine: true,
  clipboard: { writeText() {}, readText() {} }
};

// MessageChannel: ports deliver via setTimeout 0
class SQMessagePort {
  constructor() { this.onmessage = null; this._other = null; this.started = false; }
  postMessage(data) {
    const other = this._other;
    setTimeout(() => {
      if (other && other.onmessage) other.onmessage({ data: structuredCloneShim(data), ports: [] });
    }, 0);
  }
  start() { this.started = true; }
  close() {}
}
function structuredCloneShim(v) {
  if (v === null || typeof v !== 'object') return v;
  try { return JSON.parse(JSON.stringify(v)); } catch (e) { return v; }
}
class SQMessageChannel {
  constructor() {
    this.port1 = new SQMessagePort();
    this.port2 = new SQMessagePort();
    this.port1._other = this.port2;
    this.port2._other = this.port1;
  }
}

// MutationObserver stub
class MutationObserver {
  constructor(cb) { this._cb = cb; }
  observe() {}
  disconnect() {}
  takeRecords() { return []; }
}
class IntersectionObserver {
  constructor(cb) { this._cb = cb; }
  observe() {}
  unobserve() {}
  disconnect() {}
  takeRecords() { return []; }
}
class ResizeObserver {
  constructor(cb) { this._cb = cb; }
  observe() {}
  unobserve() {}
  disconnect() {}
}
class PerformanceObserver {
  constructor(cb) { this._cb = cb; }
  observe() {}
  disconnect() {}
  takeRecords() { return []; }
}

class AbortController {
  constructor() {
    this.signal = {
      aborted: false,
      reason: undefined,
      onabort: null,
      addEventListener() {},
      removeEventListener() {},
      throwIfAborted() { if (this.aborted) throw new Error('AbortError'); }
    };
  }
  abort(reason) { this.signal.aborted = true; this.signal.reason = reason; }
}

class URLSearchParamsShim {
  constructor(init) {
    this._m = new Map();
    if (typeof init === 'string') {
      const q = init[0] === '?' ? init.slice(1) : init;
      for (const pair of q.split('&')) {
        if (!pair) continue;
        const i = pair.indexOf('=');
        const k = i < 0 ? pair : pair.slice(0, i);
        const v = i < 0 ? '' : pair.slice(i + 1);
        this._m.set(decodeURIComponent(k.replace(/\+/g, ' ')), decodeURIComponent(v.replace(/\+/g, ' ')));
      }
    }
  }
  get(k) { return this._m.has(k) ? this._m.get(k) : null; }
  set(k, v) { this._m.set(k, String(v)); }
  has(k) { return this._m.has(k); }
  append(k, v) { if (!this._m.has(k)) this._m.set(k, String(v)); }
  delete(k) { this._m.delete(k); }
  forEach(fn) { for (const [k, v] of this._m) fn(v, k); }
  toString() {
    const parts = [];
    for (const [k, v] of this._m) parts.push(encodeURIComponent(k) + '=' + encodeURIComponent(v));
    return parts.join('&');
  }
  *entries() { yield* this._m.entries(); }
  *[Symbol.iterator]() { yield* this._m.entries(); }
}

class URLShim {
  constructor(url, base) {
    const u = base ? new URLShim(base).resolve(url) : url;
    const m = /^([a-z]+):\/\/([^\/?#]+)([^?#]*)(\?[^#]*)?(#.*)?$/.exec(u) || [null, 'https:', '', '/', '', ''];
    this.href = u;
    this.protocol = m[1] + ':';
    this.host = m[2];
    this.hostname = m[2].split(':')[0];
    this.port = m[2].includes(':') ? m[2].split(':')[1] : '';
    this.pathname = m[3] || '/';
    this.search = m[4] || '';
    this.hash = m[5] || '';
    this.origin = this.protocol + '//' + m[2];
  }
  resolve(rel) {
    if (/^[a-z]+:\/\//.test(rel) || rel.startsWith('data:')) return rel;
    if (rel.startsWith('//')) return this.protocol + rel;
    if (rel.startsWith('/')) return this.origin + rel;
    const dir = this.pathname.slice(0, this.pathname.lastIndexOf('/') + 1);
    return this.origin + dir + rel;
  }
  toString() { return this.href; }
  static createObjectURL() { return ''; }
  static revokeObjectURL() {}
}

// install globals
const document = new SQDocument();
globalThis.document = document;
globalThis.Node = SQNode;
globalThis.Element = SQNode;
globalThis.HTMLElement = SQNode;
globalThis.HTMLIFrameElement = SQNode;
globalThis.Document = SQDocument;
globalThis.DocumentFragment = SQFragment;
globalThis.Event = SQEvent;
globalThis.CustomEvent = SQCustomEvent;
globalThis.MessageChannel = SQMessageChannel;
globalThis.MutationObserver = MutationObserver;
globalThis.IntersectionObserver = IntersectionObserver;
globalThis.ResizeObserver = ResizeObserver;
globalThis.PerformanceObserver = PerformanceObserver;
globalThis.AbortController = AbortController;
globalThis.AbortSignal = { abort() {} };
globalThis.URL = URLShim;
globalThis.URLSearchParams = URLSearchParamsShim;
globalThis.navigator = navigator;
globalThis.location = new SQLocation(globalThis.__sq_url);
globalThis.localStorage = new SQStorage(
  (k) => globalThis.__sq_ls_get(k),
  (k, v) => globalThis.__sq_ls_set(k, v),
  (k) => globalThis.__sq_ls_remove(k));
globalThis.sessionStorage = globalThis.localStorage;
globalThis.matchMedia = (q) => new SQMediaQueryList(String(q).includes('prefers-color-scheme: dark') ? false : globalThis.innerWidth >= 0);
globalThis.getComputedStyle = (el, pseudo) => el && el.style ? el.style : { getPropertyValue: () => '' };
globalThis.performance = {
  now: () => globalThis.__sq_perf_now(),
  timeOrigin: 0,
  mark(){}, measure(){},
  getEntries(){ return []; },
  getEntriesByType(){ return []; },
  getEntriesByName(){ return []; },
  timing: { navigationStart: 0, loadEventEnd: 0, domContentLoadedEventEnd: 0, fetchStart: 0, responseEnd: 0 },
  navigation: { type: 0, redirectCount: 0 }
};
globalThis.EventTarget = class { addEventListener(t, f) { addEventListenerImpl2(t, f); } removeEventListener() {} dispatchEvent() { return true; } };
globalThis.addEventListener = (t, f) => addEventListenerImpl2(t, f);
globalThis.removeEventListener = () => {};
globalThis.dispatchEvent = () => true;
globalThis.innerWidth = globalThis.__sq_vw;
globalThis.innerHeight = globalThis.__sq_vh;
globalThis.outerWidth = globalThis.__sq_vw;
globalThis.outerHeight = globalThis.__sq_vh;
globalThis.devicePixelRatio = 1;
globalThis.scrollTo = () => {};
globalThis.scrollBy = () => {};
globalThis.scroll = () => {};
globalThis.alert = () => {};
globalThis.confirm = () => false;
globalThis.prompt = () => null;
globalThis.history = {
  state: null,
  length: 1,
  scrollRestoration: 'auto',
  pushState(s) { this.state = s; },
  replaceState(s) { this.state = s; },
  back() {}, forward() {}, go() {}
};
globalThis.window = globalThis;
globalThis.self = globalThis;
globalThis.top = globalThis;
globalThis.parent = globalThis;
globalThis.frames = globalThis;
globalThis.crypto = {
  getRandomValues(arr) {
    for (let i = 0; i < arr.length; i++) arr[i] = (Math.random() * 256) | 0;
    return arr;
  },
  randomUUID() {
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
      const r = (Math.random() * 16) | 0;
      return (c === 'x' ? r : (r & 3) | 8).toString(16);
    });
  }
};
globalThis.btoa = (s) => { /* base64 encode */
  const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  let out = '';
  const bytes = [];
  for (let i = 0; i < s.length; i++) {
    let c = s.charCodeAt(i);
    if (c > 255) { bytes.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63)); }
    else bytes.push(c);
  }
  for (let i = 0; i < bytes.length; i += 3) {
    const b = [bytes[i] || 0, bytes[i + 1] || 0, bytes[i + 2] || 0];
    out += chars[b[0] >> 2] + chars[((b[0] & 3) << 4) | (b[1] >> 4)];
    out += i + 1 < bytes.length ? chars[((b[1] & 15) << 2) | (b[2] >> 6)] : '=';
    out += i + 2 < bytes.length ? chars[b[2] & 63] : '=';
  }
  return out;
};
globalThis.atob = (s) => {
  const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  let out = '';
  let val = 0, bits = 0;
  for (const c of String(s)) {
    const i = chars.indexOf(c);
    if (i < 0) continue;
    val = (val << 6) | i;
    bits += 6;
    if (bits >= 8) { bits -= 8; out += String.fromCharCode((val >> bits) & 255); }
  }
  return out;
};
globalThis.TextEncoder = class {
  encode(s) {
    const utf8 = unescape(encodeURIComponent(String(s)));
    const arr = new Uint8Array(utf8.length);
    for (let i = 0; i < utf8.length; i++) arr[i] = utf8.charCodeAt(i);
    return arr;
  }
};
globalThis.TextDecoder = class {
  decode(arr) {
    let s = '';
    const u8 = arr instanceof Uint8Array ? arr : new Uint8Array(arr || []);
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    try { return decodeURIComponent(escape(s)); } catch (e) { return s; }
  }
};
/* setInterval is a native C binding now; clearInterval aliases clearTimeout */
globalThis.clearInterval = (id) => globalThis.clearTimeout(id);
globalThis.requestIdleCallback = (fn) => setTimeout(() => fn({ didTimeout: false, timeRemaining: () => 50 }), 1);
globalThis.cancelIdleCallback = (id) => clearTimeout(id);
globalThis.queueMicrotask = globalThis.queueMicrotask || ((fn) => Promise.resolve().then(fn));
globalThis.structuredClone = structuredCloneShim;
globalThis.SQDocument = SQDocument;
globalThis.__sq_wrap = wrap;
globalThis.__sq_event_listeners = listeners;

// ---- Intl polyfill (QuickJS lacks it; enough for i18n libraries) ----
if (typeof globalThis.Intl === 'undefined') {
  class NumberFormatPolyfill {
    constructor(locale, opts) { this._locale = (locale && String(locale)) || 'en-US'; this._opts = opts || {}; }
    format(n) {
      const num = Number(n);
      if (isNaN(num)) return 'NaN';
      const min = this._opts.minimumFractionDigits;
      const max = this._opts.maximumFractionDigits;
      let s;
      if (min !== undefined || max !== undefined) {
        const mn = min === undefined ? 0 : min;
        const mx = max === undefined ? 3 : max;
        s = num.toLocaleString('en-US', { minimumFractionDigits: mn, maximumFractionDigits: mx });
      } else s = num.toLocaleString('en-US');
      const cur = this._opts.currency;
      if (this._opts.style === 'currency' && cur) {
        const sym = { USD: '$', EUR: '\u20ac', GBP: '\u00a3', RON: 'lei' }[cur] || cur + ' ';
        return sym + s;
      }
      return s;
    }
    formatToParts(n) { return [{ type: 'integer', value: this.format(n) }]; }
    resolvedOptions() { return { locale: this._locale, ...this._opts }; }
    static supportedLocalesOf() { return []; }
  }
  class DateTimeFormatPolyfill {
    constructor(locale, opts) { this._opts = opts || {}; }
    format(d) {
      try { return new Date(d).toLocaleString('en-US', this._opts); } catch (e) { return String(d); }
    }
    resolvedOptions() { return { locale: 'en-US', ...this._opts }; }
    static supportedLocalesOf() { return []; }
  }
  class PluralRulesPolyfill {
    constructor(locale, opts) { this._opts = opts || {}; }
    select(n) {
      const num = Number(n);
      if (this._opts.type === 'ordinal') {
        const v = num % 100;
        if (v === 11 || v === 12 || v === 13) return 'other';
        const d = num % 10;
        if (d === 1) return 'one';
        if (d === 2) return 'two';
        if (d === 3) return 'few';
        return 'other';
      }
      if (num === 1) return 'one';
      return 'other';
    }
    resolvedOptions() { return { locale: 'en-US', ...this._opts }; }
    static supportedLocalesOf() { return []; }
  }
  class CollatorPolyfill {
    compare(a, b) { a = String(a); b = String(b); return a < b ? -1 : a > b ? 1 : 0; }
    resolvedOptions() { return { locale: 'en-US' }; }
    static supportedLocalesOf() { return []; }
  }
  class LocalePolyfill {
    constructor(tag, opts) { this._tag = String(tag); this.baseName = this._tag; this.language = this._tag.split('-')[0]; }
    toString() { return this._tag; }
    maximize() { return new LocalePolyfill(this._tag); }
    minimize() { return new LocalePolyfill(this._tag); }
  }
  globalThis.Intl = {
    NumberFormat: NumberFormatPolyfill,
    DateTimeFormat: DateTimeFormatPolyfill,
    PluralRules: PluralRulesPolyfill,
    Collator: CollatorPolyfill,
    Locale: LocalePolyfill,
    getCanonicalLocales: (l) => Array.isArray(l) ? l.map(String) : [String(l)]
  };
}

// ---- document readiness lifecycle ----
globalThis.__sq_ready_state = 'loading';
try {
  Object.defineProperty(document, 'readyState', {
    get: () => globalThis.__sq_ready_state,
    configurable: true
  });
} catch (e) {}
globalThis.__sq_dom_content_loaded = function() {
  globalThis.__sq_ready_state = 'interactive';
  try {
    const ev = new SQEvent('DOMContentLoaded');
    dispatchImpl(document, ev);
    const ls = listeners.get('DOMContentLoaded');
    if (ls) for (const fn of [...ls]) { try { fn.call(globalThis, ev); } catch (e) { console.error(e && e.stack || e); } }
    const dls = eventListeners.get('__document__' + '\u0000DOMContentLoaded');
    if (dls) for (const fn of [...dls]) { try { fn.call(document, ev); } catch (e) { console.error(e && e.stack || e); } }
  } catch (e) { console.error(e && e.stack || e); }
};
globalThis.__sq_window_load = function() {
  globalThis.__sq_ready_state = 'complete';
  try {
    const ev = new SQEvent('load');
    const ls = listeners.get('load');
    if (ls) for (const fn of [...ls]) { try { fn.call(globalThis, ev); } catch (e) { console.error(e && e.stack || e); } }
  } catch (e) { console.error(e && e.stack || e); }
};
/* expose internals for the C click dispatcher */
globalThis.__sqDispatch = dispatchImpl;

/* ---- additional standard globals (YouTube / Polymer / analytics) ---- */
globalThis.NodeFilter = {
  SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 0x1, SHOW_ATTRIBUTE: 0x2, SHOW_TEXT: 0x4,
  SHOW_CDATA_SECTION: 0x8, SHOW_COMMENT: 0x80, SHOW_DOCUMENT: 0x100,
  SHOW_DOCUMENT_TYPE: 0x200, SHOW_DOCUMENT_FRAGMENT: 0x400,
  FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3
};
const __NodeC = { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4,
  ENTITY_REFERENCE_NODE: 5, ENTITY_NODE: 6, PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8,
  DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11, NOTATION_NODE: 12,
  DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2,
  DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8,
  DOCUMENT_POSITION_CONTAINED_BY: 16, DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC: 32 };
try { Object.assign(document.constructor.prototype, __NodeC); } catch (e) {}
try { Object.assign(Object.getPrototypeOf(document.body) || {}, __NodeC); } catch (e) {}
globalThis.Node = globalThis.Node || __NodeC;
try { for (const k in __NodeC) { if (globalThis.Node[k] === undefined) globalThis.Node[k] = __NodeC[k]; } } catch (e) {}

/* custom elements registry (ShadyDOM polyfill expects it) */
globalThis.customElements = globalThis.customElements || (function () {
  const defs = new Map();
  return {
    define(name, ctor) { try { defs.set(String(name).toLowerCase(), ctor); } catch (e) {} },
    get(name) { return defs.get(String(name).toLowerCase()); },
    upgrade() {}, whenDefined(name) {
      return defs.has(String(name).toLowerCase()) ? Promise.resolve() : Promise.resolve();
    }
  };
})();

/* Web Animations API stub: element.animate(keyframes, options) */
function __sqAnimation() {
  const a = {
    currentTime: 0, effect: null, finished: new Promise(function (r) { a._finish = r; }),
    id: '', pending: false, playState: 'finished', playbackRate: 1, replaceState: 'active',
    startTime: 0, timeline: null,
    cancel() {}, commitStyles() {}, finish() { if (a._finish) a._finish(); },
    pause() {}, play() {}, persist() {}, reverse() {}, updatePlaybackRate() {},
    addEventListener() {}, removeEventListener() {}, onfinish: null, oncancel: null
  };
  setTimeout(function () { if (a._finish) a._finish(); }, 0);
  return a;
}
try {
  const __elProto = Object.getPrototypeOf(document.body);
  if (__elProto && !__elProto.animate) {
    __elProto.animate = function () { return __sqAnimation(); };
  }
} catch (e) {}

/* XMLHttpRequest over fetch (async; enough for analytics/beacons/SPF) */
globalThis.XMLHttpRequest = function () {
  this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = '';
  this.response = ''; this.responseType = ''; this.responseURL = '';
  this.onload = null; this.onerror = null; this.onabort = null; this.onreadystatechange = null;
  this.withCredentials = false; this._headers = {}; this._method = 'GET'; this._url = '';
  this._async = true; this.timeout = 0; this.upload = { addEventListener() {} };
};
globalThis.XMLHttpRequest.prototype = {
  UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4,
  open(method, url, async) {
    this._method = String(method || 'GET').toUpperCase();
    this._url = String(url || '');
    this._async = async !== false;
    this._setReady(1);
  },
  setRequestHeader(k, v) { this._headers[String(k)] = String(v); },
  _setReady(n) {
    this.readyState = n;
    try { if (this.onreadystatechange) this.onreadystatechange({ target: this, type: 'readystatechange' }); } catch (e) {}
  },
  abort() { this._setReady(4); if (this.onabort) this.onabort({ target: this, type: 'abort' }); },
  getAllResponseHeaders() { return ''; },
  getResponseHeader() { return null; },
  overrideMimeType() {},
  addEventListener(type, fn) {
    if (type === 'load') this.onload = fn;
    else if (type === 'error') this.onerror = fn;
    else if (type === 'abort') this.onabort = fn;
    else if (type === 'readystatechange') this.onreadystatechange = fn;
  },
  removeEventListener() {},
  send(body) {
    const self = this;
    const go = typeof fetch === 'function' ? fetch : null;
    if (!go) { self.status = 0; self._setReady(4); if (self.onerror) self.onerror({ target: self }); return; }
    go(self._url, { method: self._method, headers: self._headers, body: self._method === 'GET' || self._method === 'HEAD' ? undefined : (body === undefined ? null : body) })
      .then(function (res) {
        self.status = res.status; self.statusText = res.statusText || '';
        self.responseURL = self._url;
        return res.text().then(function (t) {
          self.responseText = t; self.response = t;
          self._setReady(4);
          if (self.onload) self.onload({ target: self, type: 'load' });
        });
      })
      .catch(function (err) {
        self.status = 0; self._setReady(4);
        if (self.onerror) self.onerror({ target: self, type: 'error' });
      });
  }
};

/* AbortController / AbortSignal (fetch signals, throwIfAborted) */
globalThis.AbortSignal = globalThis.AbortSignal || function AbortSignal() {
  this.aborted = false; this.reason = undefined; this.onabort = null;
  this.addEventListener = function () {}; this.removeEventListener = function () {};
  this.throwIfAborted = function () {
    if (this.aborted) throw (this.reason || new Error('AbortError'));
  };
};
AbortSignal.timeout = function (ms) { return new AbortSignal(); };
AbortSignal.abort = function (reason) {
  const sig = new AbortSignal();
  sig.aborted = true;
  sig.reason = reason || new Error('AbortError');
  return sig;
};
globalThis.AbortController = globalThis.AbortController || function AbortController() {
  const sig = new AbortSignal();
  this.signal = sig;
  this.abort = function (reason) {
    if (sig.aborted) return;
    sig.aborted = true;
    sig.reason = reason || new Error('AbortError');
    if (sig.onabort) try { sig.onabort({ target: sig, type: 'abort' }); } catch (e) {}
  };
};

/* Image constructor */
globalThis.Image = function (w, h) {
  const img = document.createElement('img');
  if (w) img.width = w;
  if (h) img.height = h;
  return img;
};

/* Selection/Range stubs */
globalThis.getSelection = globalThis.getSelection || function () {
  return { addRange() {}, removeAllRanges() {}, getRangeAt() { return null; }, toString() { return ''; } };
};
globalThis.Range = globalThis.Range || function () {
  this.collapsed = true; this.commonAncestorContainer = document.body;
  this.setStart = function () {}; this.setEnd = function () {};
  this.collapse = function () {}; this.selectNode = function () {};
  this.selectNodeContents = function () {}; this.deleteContents = function () {};
  this.cloneContents = function () { return document.createDocumentFragment(); };
  this.getBoundingClientRect = function () { return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 }; };
  this.getClientRects = function () { return { length: 0, item: function () { return null; } }; };
};
globalThis.__sqListenersKey = listenerKey;
})();
)PRELUDE";

/* ============================ JS console / error reporting ============================ */
JSValue js_console_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  std::string out;
  for (int i = 0; i < argc; i++) {
    if (i) out += " ";
    /* errors: include stack */
    JSValue stack = JS_GetPropertyStr(ctx, argv[i], "stack");
    if (!JS_IsUndefined(stack) && !JS_IsException(stack)) {
      const char* st = JS_ToCString(ctx, stack);
      if (st) {
        out += st;
        JS_FreeCString(ctx, st);
        JS_FreeValue(ctx, stack);
        continue;
      }
    }
    JS_FreeValue(ctx, stack);
    const char* s = JS_ToCString(ctx, argv[i]);
    if (s) {
      out += s;
      JS_FreeCString(ctx, s);
    } else {
      JSValue j = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
      if (!JS_IsException(j)) {
        const char* s2 = JS_ToCString(ctx, j);
        if (s2) out += s2;
        JS_FreeCString(ctx, s2);
      }
      JS_FreeValue(ctx, j);
    }
  }
  if (inst->console_log.size() < 400) inst->console_log.push_back(out);
  engine_log(1, "[js] %s", out.c_str());
  return JS_UNDEFINED;
}

void report_js_exception(Instance* inst) {
  JSContext* ctx = inst->jctx;
  JSValue exc = JS_GetException(ctx);
  const char* msg = JS_ToCString(ctx, exc);
  std::string m = msg ? msg : "(exception)";
  if (msg) JS_FreeCString(ctx, msg);
  JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
  const char* st = JS_ToCString(ctx, stack);
  engine_log(3, "[js error] %s%s", m.c_str(), st ? st : "");
  if (st) JS_FreeCString(ctx, st);
  JS_FreeValue(ctx, stack);
  JS_FreeValue(ctx, exc);
}

/* ============================ timers / loop helpers ============================ */
static void fire_timer(Instance* inst, Timer& t) {
  JSContext* ctx = inst->jctx;
  if (!JS_IsFunction(ctx, t.fn)) return;
  JSValue r = JS_Call(ctx, t.fn, JS_UNDEFINED, 0, nullptr);
  if (JS_IsException(r)) report_js_exception(inst);
  JS_FreeValue(ctx, r);
}

static void pump_fetches(Instance* inst) {
  JSContext* ctx = inst->jctx;
  std::vector<PendingFetch*> done;
  {
    std::lock_guard<std::mutex> lk(inst->js_mtx);
    for (auto it = inst->fetches.begin(); it != inst->fetches.end();) {
      if ((*it)->done) {
        done.push_back(*it);
        it = inst->fetches.erase(it);
      } else ++it;
    }
  }
  for (PendingFetch* t : done) {
    JSValue args[3];
    args[0] = JS_NewInt32(ctx, t->status);
    std::string body(t->body.begin(), t->body.end());
    args[1] = JS_NewStringLen(ctx, body.c_str(), body.size());
    args[2] = JS_NewStringLen(ctx, t->err.c_str(), t->err.size());
    if (JS_IsFunction(ctx, t->callback)) {
      JSValue r = JS_Call(ctx, t->callback, JS_UNDEFINED, 3, args);
      if (JS_IsException(r)) report_js_exception(inst);
      JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    JS_FreeValue(ctx, t->callback);
    delete t;
  }
}

static void pump_raf(Instance* inst) {
  JSContext* ctx = inst->jctx;
  std::unordered_map<int, JSValue> fire;
  {
    std::lock_guard<std::mutex> lk(inst->js_mtx);
    fire.swap(inst->raf_map);
  }
  for (auto& [id, fn] : fire) {
    if (JS_IsFunction(ctx, fn)) {
      JSValue ts = JS_NewFloat64(ctx, now_ms() - inst->js_epoch);
      JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &ts);
      if (JS_IsException(r)) report_js_exception(inst);
      JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
  }
}

static void fire_timers(Instance* inst) {
  std::vector<int> due;
  double now = now_ms();
  {
    std::lock_guard<std::mutex> lk(inst->js_mtx);
    for (auto& t : inst->timers)
      if (t.when <= now) due.push_back(t.id);
  }
#ifdef LAYOUT_DEBUG
  if (!due.empty()) engine_log(1, "[timer] firing %d timers", (int)due.size());
#endif
  for (int id : due) {
    if (inst->cancel) break;
    JSValue fn = JS_UNDEFINED;
    int interval = 0;
    {
      std::lock_guard<std::mutex> lk(inst->js_mtx);
      for (auto it = inst->timers.begin(); it != inst->timers.end(); ++it) {
        if (it->id == id) {
          interval = it->interval;
          if (interval > 0) {
            it->when = now_ms() + interval;
            fn = JS_DupValue(inst->jctx, it->fn);
          } else {
            fn = it->fn;              /* take ownership */
            inst->timers.erase(it);
          }
          break;
        }
      }
    }
    Timer tmp;
    tmp.fn = fn;
    if (JS_IsFunction(inst->jctx, fn)) fire_timer(inst, tmp);
    JS_FreeValue(inst->jctx, fn);
  }
}

/* ============================ style/layout/paint refresh ============================ */
void refresh_page(Instance* inst) {
  std::lock_guard<std::mutex> lk(inst->mtx);
  if (!inst->doc || !inst->doc->body) return;
  int vw = inst->vw, vh = inst->vh;
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
  /* paint viewport-sized frame at the current scroll offset */
  {
    std::lock_guard<std::mutex> plk(inst->paint_mtx);
    if (inst->latest.w != vw || inst->latest.h != vh) {
      inst->latest.free_pix();
      inst->latest.alloc(vw, vh);
    }
    if (!getenv("SQRL_NO_PAINT"))
      paint_document(inst->doc, inst->root_box, &inst->latest, &inst->images, (float)inst->scroll_y);
  }
  inst->dirty = false;
}

/* ============================ page thread ============================ */
static void set_state(Instance* inst, int st, float progress, const char* info) {
  inst->state = st;
  if (inst->state_fn) inst->state_fn(inst->state_ud, st, progress, info ? info : "");
}

void fire_queued_clicks(Instance* inst); /* defined in square_platform_api.cpp (input) */
void page_main(Instance* inst, std::string url) {
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
      }
    }
    /* running CSS animations: repaint periodically so keyframes advance.
     * Light path: re-evaluate keyframe values + layout + paint, skipping the
     * selector cascade (that's what made animated pages lag). */
    if (inst->doc && inst->doc->sheet.has_animations && now_ms() - last_anim_frame >= 33) {
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
                         (float)inst->scroll_y);
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
