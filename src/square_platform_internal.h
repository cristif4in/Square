/* =====================================================================
 *  Square Browser Engine — internal header shared by the platform module:
 *    square_platform.cpp       engine instance, QuickJS bindings, JS
 *                              prelude, page load pipeline + event loop
 *    square_platform_api.cpp   remaining bindings, public C API, input
 *  Not installed publicly — engine-internal only.
 * ===================================================================== */
#pragma once

#include "square_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "../thirdparty/quickjs/quickjs.h"

#include <unordered_set>

namespace sqrl {

/* ============================ image cache ============================ */
struct ImageCache : ImageCacheView {
  std::mutex mtx;
  std::unordered_map<std::string, ImageInfo> map;
  std::unordered_set<std::string> requested;
  std::string base_url;
  std::function<void(const std::string&)> request_fetch;   /* lazy load hook */

  ImageInfo find(const std::string& raw_url) override {
    std::string key = resolve(raw_url);
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lk(mtx);
      auto it = map.find(key);
      if (it != map.end()) return it->second;
      fresh = requested.insert(key).second;
    }
    if (fresh && request_fetch) request_fetch(key);
    return ImageInfo{};
  }
  std::string resolve(const std::string& raw) {
    if (raw.rfind("data:", 0) == 0 || raw.rfind("http", 0) == 0 || raw.rfind("file:", 0) == 0)
      return raw;
    return url_resolve(base_url, raw);
  }
  void put(const std::string& resolved, ImageInfo info) {
    std::lock_guard<std::mutex> lk(mtx);
    map[resolved] = info;
  }
};

/* ============================ instance ============================ */
struct Timer {
  int id = 0;
  double when = 0;      /* ms epoch from engine start */
  int interval = 0;     /* 0 = one-shot */
  JSValue fn;
};

struct PendingFetch {
  JSValue callback;        /* JS function(status:int, body:string, err:string) */
  std::atomic<bool> done{false};
  int status = 0;
  std::string err;
  std::vector<uint8_t> body;
};

struct Instance {
  std::mutex mtx;                 /* protects doc/root_box/state */
  std::mutex doc_mtx;             /* serializes DOM tree access (JS thread vs layout) */
  std::atomic<int> state{SQ_STATE_IDLE};
  std::atomic<bool> cancel{false};
  std::atomic<bool> dirty{false}; /* DOM/style changed → relayout */
  std::atomic<int> loading{0};    /* pending resource fetches */
  int vw = 1280, vh = 800;
  int scroll_y = 0;               /* current scroll offset (page coords) */
  std::string ua = "Square/1.0 (cristif4in)";
  square_state_fn state_fn = nullptr;
  void* state_ud = nullptr;

  ThreadPool pool;
  CookieJar jar;
  ImageCache images;

  Document* doc = nullptr;
  Box* root_box = nullptr;
  std::string page_title;
  std::string final_url;
  std::string final_script_url;
  std::string pending_html;      /* for load_html */

  Surface latest;                 /* latest painted frame (viewport-sized or full) */
  std::mutex paint_mtx;
  int page_h = 0;

  std::thread page_thread;
  bool page_running = false;

  /* graveyard: DOM nodes removed/replaced stay alive until page teardown
     (old layout boxes/frags may still reference them) */
  std::vector<Node*> graveyard;

  /* JS */
  JSRuntime* rt = nullptr;
  JSContext* jctx = nullptr;
  std::mutex js_mtx;
  std::vector<Timer> timers;
  int next_timer_id = 1;
  std::vector<PendingFetch*> fetches;
  double js_epoch = 0;
  bool js_ready = false;
  std::vector<JSValue> raf_queue;
  int raf_id = 1;
  std::unordered_map<int, JSValue> raf_map;
  std::unordered_map<std::string, std::string> local_storage;
  std::vector<std::string> console_log;

  /* stats */
  int resources_loaded = 0;

  /* input / navigation queue */
  std::mutex nav_mtx;
  std::string nav_request;
  bool nav_pending = false;
  struct QueuedClick {
    int node_id;
    float x, y;
  };
  std::vector<QueuedClick> click_queue;   /* guarded by js_mtx */
  float mouse_x = -1, mouse_y = -1;
  bool mouse_valid = false;
  int same_url_hops = 0;               /* reload-loop guard */
  std::atomic<bool> nav_busy{false};   /* a navigation join/start is in flight */

  Instance() : pool(std::max(2, (int)std::thread::hardware_concurrency())) {}
};

/* ============================ shared globals ============================ */
extern std::unordered_map<int64_t, Node*> g_node_registry;
extern std::mutex g_reg_mtx;
extern std::unordered_map<Node*, int64_t> g_node_ids;
extern int64_t g_next_node_id;
extern Node* g_document_node;      /* "__document__" sentinel */
extern JSClassID node_class_id;

/* ============================ shared functions ============================ */
std::string plat_lc(std::string s);
extern std::mutex g_log_mtx;
extern square_log_fn g_log_fn;
extern void* g_log_ud;
HttpResult fetch_sync(Instance* inst, const std::string& url, const char* dest);
HttpResult fetch_sync(Instance* inst, const std::string& url);
double now_ms();
void node_finalizer(JSRuntime* rt, JSValue val);
int64_t node_id_for(Node* n);
JSValue wrap_node(JSContext* ctx, Node* n);
Node* unwrap_node_ctx(JSContext* ctx, JSValueConst v);
void mark_dirty(Instance* inst);
void report_js_exception(Instance* inst);
JSValue js_console_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue make_console(Instance* inst);
void refresh_page(Instance* inst);
void page_main(Instance* inst, std::string url);
void fire_queued_clicks(Instance* inst);
void set_state_public(Instance* inst, int st, float progress, const char* info);

JSValue js_create_element(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_create_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_create_comment(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_append_child(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_insert_before(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_remove_child(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_child_index(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_child_nodes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_parent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_first(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_last(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_next(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_prev(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_node_type(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_tag(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_get_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_set_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_remove_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_has_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_set_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_get_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_set_inner_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_get_inner_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_get_by_id(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_query_selector_all(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_mark_dirty(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_set_timeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_set_interval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_clear_timeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_request_animation_frame(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_cancel_animation_frame(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_local_storage_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_local_storage_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_local_storage_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);

extern const char* k_js_prelude;
extern const char* ctx_doc_key;
char* module_normalize(JSContext* ctx, const char* base_name, const char* name, void* opaque);
JSModuleDef* module_loader(JSContext* ctx, const char* module_name, void* opaque);
void pump_fetches(Instance* inst);
void pump_raf(Instance* inst);
void fire_timers(Instance* inst);

/* ---- bindings defined in square_platform_api.cpp, used by the runtime setup ---- */
JSValue js_fetch_start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);
JSValue js_attr_names(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_remove_all_children(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_matches(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_ns_uri(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_doc_ptr(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_html_element(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_body_element(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_head_element(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_get_title_fn(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_set_title_fn(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_rect_of(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_perf_now(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_navigate_to(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_cookie_get(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);
JSValue js_cookie_set(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv);

} /* namespace sqrl */
