/* =====================================================================
 *  Square Browser Engine — square_platform_api.cpp
 *  Remaining JS bindings, the public C API (square_create / load / save /
 *  input & hit testing) and the page thread. Split from square_platform.cpp.
 * ===================================================================== */
#include "square_platform_internal.h"

#include <algorithm>
#include <chrono>
#include <cctype>

namespace sqrl {

void set_state_public(Instance* inst, int st, float progress, const char* info) {
  inst->state = st;
  if (inst->state_fn) inst->state_fn(inst->state_ud, st, progress, info ? info : "");
}

JSValue js_attr_names(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node_ctx(ctx, argv[0]) : nullptr;
  JSValue arr = JS_NewArray(ctx);
  if (!n) return arr;
  int i = 0;
  for (auto& a : n->attrs) JS_SetPropertyUint32(ctx, arr, i++, JS_NewStringLen(ctx, a.name.c_str(), a.name.size()));
  return arr;
}

JSValue js_remove_all_children(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  Node* n = argc > 0 ? unwrap_node_ctx(ctx, argv[0]) : nullptr;
  if (n) {
    for (Node* c : n->children) { c->parent = nullptr; inst->graveyard.push_back(c); }
    n->children.clear();
    mark_dirty(inst);
  }
  return JS_UNDEFINED;
}

static bool selector_matches_public(Node* n, const char* sel) {
  Selector parsed;
  if (!css_parse_selector_public(sel, &parsed)) return false;
  return selector_matches(n, parsed);
}

JSValue js_matches(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node_ctx(ctx, argv[0]) : nullptr;
  const char* sel = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
  if (!n || !sel) {
    if (sel) JS_FreeCString(ctx, sel);
    return JS_FALSE;
  }
  bool r = selector_matches_public(n, sel);
  JS_FreeCString(ctx, sel);
  return JS_NewBool(ctx, r);
}

JSValue js_ns_uri(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node_ctx(ctx, argv[0]) : nullptr;
  if (n && n->ns == "svg") return JS_NewString(ctx, "http://www.w3.org/2000/svg");
  if (n && n->type == N_ELEMENT) return JS_NewString(ctx, "http://www.w3.org/1999/xhtml");
  return JS_NewString(ctx, "");
}

JSValue js_doc_ptr(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  return wrap_node(inst->jctx, inst->doc ? inst->doc->root : nullptr);
}

JSValue js_html_element(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  return wrap_node(inst->jctx, inst->doc ? inst->doc->html : nullptr);
}
JSValue js_body_element(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  return wrap_node(inst->jctx, inst->doc ? inst->doc->body : nullptr);
}
JSValue js_head_element(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  return wrap_node(inst->jctx, inst->doc ? inst->doc->head : nullptr);
}
JSValue js_get_title_fn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  return JS_NewStringLen(ctx, inst->page_title.c_str(), inst->page_title.size());
}
JSValue js_set_title_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* t = argc > 0 ? JS_ToCString(ctx, argv[0]) : "";
  if (t) {
    inst->page_title = t;
    JS_FreeCString(ctx, t);
  }
  return JS_UNDEFINED;
}
JSValue js_perf_now(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  return JS_NewFloat64(ctx, now_ms() - inst->js_epoch);
}
JSValue js_rect_of(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  Node* n = argc > 0 ? unwrap_node_ctx(ctx, argv[0]) : nullptr;
  JSValue arr = JS_NewArray(ctx);
  float x = 0, y = 0, w = 0, h = 0;
  if (n && n->box) {
    x = n->box->x;
    y = n->box->y;
    w = n->box->w;
    h = n->box->h;
  }
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, x));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, y));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, w));
  JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, h));
  return arr;
}
JSValue js_cookie_get(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  if (!inst) return JS_NewString(ctx, "");
  /* host of the document (cookie jar matches by host) */
  std::string sch, host, port, path;
  url_split(inst->final_url, &sch, &host, &port, &path);
  std::string v = inst->jar.get(host);
  return JS_NewStringLen(ctx, v.c_str(), v.size());
}

JSValue js_cookie_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  if (!inst || argc < 1) return JS_UNDEFINED;
  const char* v = JS_ToCString(ctx, argv[0]);
  if (v && *v) {
    std::string sch, host, port, path;
    url_split(inst->final_url, &sch, &host, &port, &path);
    inst->jar.store(host, v);
  }
  if (v) JS_FreeCString(ctx, v);
  return JS_UNDEFINED;
}

JSValue js_navigate_to(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* u = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
  if (inst && u && *u) {
    std::string target = url_resolve(inst->final_url, u);
    JSValue st = JS_Eval(ctx, "new Error().stack", 17, "<navstack>", JS_EVAL_TYPE_GLOBAL);
    const char* stk = JS_IsException(st) ? "(?)" : JS_ToCString(ctx, st);
    engine_log(1, "[js] navigation to: %s\nstack: %s", target.c_str(), stk ? stk : "?");
    if (!JS_IsException(st)) JS_FreeCString(ctx, stk);
    JS_FreeValue(ctx, st);
    std::lock_guard<std::mutex> nlk(inst->nav_mtx);
    inst->nav_request = target;
    inst->nav_pending = true;
  }
  if (u) JS_FreeCString(ctx, u);
  return JS_UNDEFINED;
}

JSValue make_console(Instance* inst) {
  JSContext* ctx = inst->jctx;
  JSValue con = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, con, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
  JS_SetPropertyStr(ctx, con, "info", JS_NewCFunction(ctx, js_console_log, "info", 1));
  JS_SetPropertyStr(ctx, con, "warn", JS_NewCFunction(ctx, js_console_log, "warn", 1));
  JS_SetPropertyStr(ctx, con, "error", JS_NewCFunction(ctx, js_console_log, "error", 1));
  JS_SetPropertyStr(ctx, con, "debug", JS_NewCFunction(ctx, js_console_log, "debug", 1));
  JS_SetPropertyStr(ctx, con, "trace", JS_NewCFunction(ctx, js_console_log, "trace", 1));
  JS_SetPropertyStr(ctx, con, "table", JS_NewCFunction(ctx, js_console_log, "table", 1));
  JS_SetPropertyStr(ctx, con, "dir", JS_NewCFunction(ctx, js_console_log, "dir", 1));
  return con;
}

JSValue js_fetch_start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* urlc = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
  if (!urlc) return JS_ThrowTypeError(ctx, "fetch(url)");
  std::string url = urlc;
  JS_FreeCString(ctx, urlc);
  if (url.rfind("http", 0) != 0 && url.rfind("file:", 0) != 0 && url.rfind("data:", 0) != 0)
    url = url_resolve(inst->final_url, url);
  PendingFetch* task = new PendingFetch();
  task->callback = JS_DupValue(ctx, argv[1]);
  inst->loading++;
  Instance* capt = inst;
  std::string u = url;
  capt->pool.submit([capt, u, task]() {
    HttpResult r = fetch_sync(capt, u);
    {
      std::lock_guard<std::mutex> lk(capt->js_mtx);
      task->status = r.status;
      task->err = r.err;
      task->body = std::move(r.body);
      task->done = true;
    }
    capt->loading--;
  });
  {
    std::lock_guard<std::mutex> lk(inst->js_mtx);
    inst->fetches.push_back(task);
  }
  return JS_UNDEFINED;
}

/* ============================ public C API ============================ */

} /* namespace sqrl */

using namespace sqrl;
static Instance* as_inst(square_instance* p) { return (Instance*)p; }

square_instance* square_create(void) {
  net_init();
  Instance* inst = new Instance();
  set_state_public(inst, SQ_STATE_IDLE, 0, "");
  return (square_instance*)inst;
}

void square_destroy(square_instance* p) {
  Instance* inst = as_inst(p);
  if (!inst) return;
  inst->cancel = true;
  if (inst->page_thread.joinable()) inst->page_thread.join();
  /* free JS values first, then context, then runtime */
  if (inst->jctx) {
    for (auto& t : inst->timers) JS_FreeValue(inst->jctx, t.fn);
    inst->timers.clear();
    for (auto& [id, fn] : inst->raf_map) JS_FreeValue(inst->jctx, fn);
    inst->raf_map.clear();
    for (auto* f : inst->fetches) {
      JS_FreeValue(inst->jctx, f->callback);
      delete f;
    }
    inst->fetches.clear();
    JS_FreeContext(inst->jctx);
    inst->jctx = nullptr;
  }
  if (inst->rt) {
    JS_RunGC(inst->rt);
    JS_FreeRuntime(inst->rt);
    inst->rt = nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(inst->mtx);
    if (inst->root_box) {
      free_box_tree(inst->root_box);
      inst->root_box = nullptr;
    }
    if (inst->doc) {
      delete inst->doc;
      inst->doc = nullptr;
    }
    for (Node* n : inst->graveyard) node_free(n);
    inst->graveyard.clear();
  }
  {
    std::lock_guard<std::mutex> plk(inst->paint_mtx);
    inst->latest.free_pix();
  }
  image_cache_clear();
  delete inst;
}

void square_set_viewport(square_instance* p, int width, int height) {
  Instance* inst = as_inst(p);
  if (!inst || width <= 0 || height <= 0) return;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lk(inst->mtx);
    if (inst->vw != width || inst->vh != height) {
      inst->vw = width;
      inst->vh = height;
      changed = true;
    }
  }
  if (changed && inst->doc) inst->dirty = true;
}

void square_get_viewport(square_instance* p, int* out_w, int* out_h) {
  Instance* inst = as_inst(p);
  if (!inst) return;
  if (out_w) *out_w = inst->vw;
  if (out_h) *out_h = inst->vh;
}

void square_set_user_agent(square_instance* p, const char* ua) {
  Instance* inst = as_inst(p);
  if (inst && ua) inst->ua = ua;
}

void square_set_log_callback(square_instance* p, square_log_fn fn, void* ud) {
  std::lock_guard<std::mutex> lk(g_log_mtx);
  g_log_fn = fn;
  g_log_ud = ud;
}

void square_set_state_callback(square_instance* p, square_state_fn fn, void* ud) {
  Instance* inst = as_inst(p);
  if (!inst) return;
  inst->state_fn = fn;
  inst->state_ud = ud;
}

int square_get_thread_count(square_instance* p) {
  Instance* inst = as_inst(p);
  return inst ? inst->pool.thread_count() : 0;
}

/* page thread runs with a large stack: JS<->binding recursion can go deep */
struct StackThreadWrapper {
  std::function<void()> fn;
  static void* entry(void* self) {
    StackThreadWrapper* w = (StackThreadWrapper*)self;
    w->fn();
    delete w;
    return nullptr;
  }
};
#ifdef _WIN32
static DWORD WINAPI stack_thread_entry(LPVOID self) {
  return (DWORD)(uintptr_t)StackThreadWrapper::entry(self);
}
static void run_with_big_stack(std::function<void()> fn, std::thread& out) {
  StackThreadWrapper* w = new StackThreadWrapper{std::move(fn)};
  HANDLE h = CreateThread(nullptr, 64ull * 1024 * 1024, stack_thread_entry, w, 0, nullptr);
  if (!h) { delete w; return; }
  out = std::thread([h]() { WaitForSingleObject(h, INFINITE); CloseHandle(h); });
}
#else
static void* stack_thread_posix(void* self) { return StackThreadWrapper::entry(self); }
static void run_with_big_stack(std::function<void()> fn, std::thread& out) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64ull * 1024 * 1024);
  StackThreadWrapper* w = new StackThreadWrapper{std::move(fn)};
  pthread_t tid;
  if (pthread_create(&tid, &attr, stack_thread_posix, w) != 0) {
    pthread_attr_destroy(&attr);
    delete w;
    return;
  }
  pthread_attr_destroy(&attr);
  out = std::thread([tid]() { void* r; pthread_join(tid, &r); (void)r; });
}
#endif

static void page_thread_entry(Instance* inst, std::string url, bool as_html) {
  if (as_html) {
    /* build a fake document from html string */
    inst->final_url = url.empty() ? "file:///local.html" : url;
    inst->doc = html_parse_document(inst->pending_html);
    inst->doc->base_url = inst->final_url;
    inst->images.base_url = inst->final_url;
    inst->page_title = inst->doc->title;
    set_state_public(inst, SQ_STATE_DOCUMENT, 0.2f, "parsed");
    /* styles inline only */
    std::function<void(Node*)> scan = [&](Node* n) {
      if (n->type == N_ELEMENT && n->tag == "style")
        for (Node* c : n->children)
          if (c->type == N_TEXT) css_parse_stylesheet(c->data, &inst->doc->sheet, inst->vw, inst->vh);
      for (Node* c : n->children) scan(c);
    };
    scan(inst->doc->root);
    { std::lock_guard<std::mutex> lk(inst->doc_mtx); refresh_page(inst); }
    /* images async */
    std::unordered_set<std::string> img_urls;
    std::function<void(Node*)> scan_imgs = [&](Node* n) {
      if (n->type == N_ELEMENT && n->tag == "img") {
        const std::string* src = n->attr("src");
        if (src && !src->empty() && src->rfind("data:", 0) != 0)
          img_urls.insert(inst->images.resolve(*src));
      }
      for (Node* c : n->children) scan_imgs(c);
    };
    scan_imgs(inst->doc->root);
    inst->loading = (int)img_urls.size();
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
          }
        }
        inst->loading--;
      });
    }
    /* still run JS if present */
    set_state_public(inst, SQ_STATE_ASSETS, 0.6f, "assets");
    double last = now_ms();
    while (!inst->cancel) {
      {
        std::lock_guard<std::mutex> lk(inst->doc_mtx);
        if (inst->dirty) refresh_page(inst);
        if (inst->loading <= 0 && now_ms() - last > 400) {
          refresh_page(inst);
          set_state_public(inst, SQ_STATE_READY, 1.f, "ready");
        }
      }
      if (inst->state == SQ_STATE_READY && inst->loading <= 0 && !inst->dirty)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    return;
  }
  /* page loop: navigate in-place when a link click requested a new URL */
  std::string cur = url;
  for (int hop = 0; hop < 64; hop++) {
    page_main(inst, cur);
    if (inst->cancel) break;
    std::string next;
    {
      std::lock_guard<std::mutex> nlk(inst->nav_mtx);
      if (inst->nav_pending) {
        next = inst->nav_request;
        inst->nav_request.clear();
        inst->nav_pending = false;
      }
    }
    if (next.empty()) break;
    /* guard: a page that hard-reloads itself endlessly (broken polyfill) —
     * allow at most 2 consecutive same-URL hops */
    if (next == cur) {
      inst->same_url_hops++;
      if (inst->same_url_hops >= 2) {
        engine_log(2, "[nav] same-URL reload loop detected, stopping");
        inst->nav_pending = false;
        break;
      }
    } else {
      inst->same_url_hops = 0;
    }
    /* reset per-page state and tear down the previous JS runtime */
    inst->cancel = false;
    inst->dirty = false;
    inst->loading = 0;
    inst->click_queue.clear();
    {
      std::lock_guard<std::mutex> lk(inst->js_mtx);
      /* release JS-held refs BEFORE freeing the context/runtime */
      for (auto& t : inst->timers) JS_FreeValue(inst->jctx, t.fn);
      for (auto& v : inst->raf_queue) JS_FreeValue(inst->jctx, v);
      for (auto& kv : inst->raf_map) JS_FreeValue(inst->jctx, kv.second);
      for (auto* pf : inst->fetches) {
        if (pf) {
          JS_FreeValue(inst->jctx, pf->callback);
          delete pf;
        }
      }
      inst->timers.clear();
      inst->fetches.clear();
      inst->raf_queue.clear();
      inst->raf_map.clear();
      inst->js_ready = false;
      if (inst->jctx) {
        JS_FreeContext(inst->jctx);
        inst->jctx = nullptr;
      }
      if (inst->rt) {
        JS_FreeRuntime(inst->rt);
        inst->rt = nullptr;
      }
    }
    {
      std::lock_guard<std::mutex> lk(g_reg_mtx);
      g_node_registry.clear();
      g_node_ids.clear();
      g_next_node_id = 1;
    }
    g_hover_node = nullptr;
    for (Node* n : inst->graveyard) node_free(n);
    inst->graveyard.clear();
    if (inst->root_box) {
      free_box_tree(inst->root_box);
      inst->root_box = nullptr;
    }
    cur = next;
  }
}

void square_load_url(square_instance* p, const char* url) {
  Instance* inst = as_inst(p);
  if (!inst || !url) return;
  if (inst->page_thread.joinable()) inst->page_thread.join();
  inst->cancel = false;
  inst->dirty = false;
  inst->loading = 0;
  std::string u = url;
  run_with_big_stack([inst, u]() { page_thread_entry(inst, u, false); }, inst->page_thread);
}

/* ============================ input / hit testing ============================ */
namespace sqrl { Node* g_hover_node = nullptr; } /* :hover target (declared in square_internal.h) */
using sqrl::g_hover_node;

static Node* hit_test_box(Box* b, float x, float y) {
  if (!b) return nullptr;
  /* topmost first: later siblings paint above earlier ones */
  for (size_t i = b->kids.size(); i-- > 0;) {
    Node* r = hit_test_box(b->kids[i], x, y);
    if (r) return r;
  }
  /* out-of-flow (absolute/fixed) boxes hit-test on their own geometry */
  for (size_t i = b->out_of_flow.size(); i-- > 0;) {
    Node* r = hit_test_box(b->out_of_flow[i], x, y);
    if (r) return r;
  }
  /* inline fragments (text runs, inline boxes, replaced elements) */
  for (size_t i = b->frags.size(); i-- > 0;) {
    InlineFrag& f = b->frags[i];
    if (f.kind == InlineFrag::F_BR) continue;
    const Rect* rr = f.kind == InlineFrag::F_TEXT ? nullptr : &f.rect;
    float rx = f.kind == InlineFrag::F_TEXT ? f.run.x : rr->x;
    float ry = f.kind == InlineFrag::F_TEXT ? f.run.y - f.run.size : rr->y;
    float rw = f.kind == InlineFrag::F_TEXT ? f.run.width : rr->w;
    float rh = f.kind == InlineFrag::F_TEXT ? f.run.size * 1.4f : rr->h;
    if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
      if (f.kind == InlineFrag::F_BOX && f.box) {
        Node* deep = hit_test_box(f.box, x, y);
        if (deep) return deep;
      }
      return f.node ? f.node : b->node;
    }
  }
  if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) return b->node;
  return nullptr;
}

static Node* hit_test_page(Instance* inst, float x, float y) {
  if (!inst->root_box) return nullptr;
  return hit_test_box(inst->root_box, x, y);
}

SQRL_API void square_send_mouse_move(square_instance* p, float x, float y) {
  Instance* inst = as_inst(p);
  if (!inst) return;
  std::lock_guard<std::mutex> doclk(inst->doc_mtx);
  inst->mouse_x = x;
  inst->mouse_y = y;
  inst->mouse_valid = true;
  Node* hit = hit_test_page(inst, x, y);
  Node* effective = nullptr;
  for (Node* n = hit; n; n = n->parent)
    if (n->type == N_ELEMENT) { effective = n; break; }
  if (effective != g_hover_node) {
    g_hover_node = effective;
    inst->dirty = true; /* recompute styles for :hover */
  }
}

SQRL_API void square_send_mouse_leave(square_instance* p) {
  Instance* inst = as_inst(p);
  if (!inst) return;
  std::lock_guard<std::mutex> doclk(inst->doc_mtx);
  inst->mouse_valid = false;
  if (g_hover_node) {
    g_hover_node = nullptr;
    inst->dirty = true;
  }
}

/* ev.preventDefault() — sets defaultPrevented on the event object (this) */
namespace sqrl {
static JSValue js_ev_prevent_default(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  JS_SetPropertyStr(ctx, this_val, "defaultPrevented", JS_TRUE);
  return JS_UNDEFINED;
}
/* ev.stopPropagation() — marks the event so the bubbling loop stops */
static JSValue js_ev_stop_propagation(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  JS_SetPropertyStr(ctx, this_val, "_propagationStopped", JS_TRUE);
  return JS_UNDEFINED;
}
} /* namespace sqrl */

/* fire click listeners on the JS thread; must hold js_mtx */
namespace sqrl {
void fire_queued_clicks(Instance* inst) {
  std::vector<Instance::QueuedClick> q;
  q.swap(inst->click_queue);
  if (q.empty() || !inst->jctx) return;
  JSContext* ctx = inst->jctx;
  JSValue g = JS_GetGlobalObject(ctx);
  JSValue fn = JS_GetPropertyStr(ctx, g, "__sqDispatch");
  for (auto& c : q) {
    Node* n = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_reg_mtx);
      auto it = g_node_registry.find(c.node_id);
      if (it != g_node_registry.end()) n = it->second;
    }
    if (!n) continue;
    /* navigation target? (link href) */
    std::string nav;
    for (Node* cur = n; cur; cur = cur->parent) {
      if (cur->type == N_ELEMENT && cur->tag == "a") {
        const std::string* href = cur->attr("href");
        if (href && !href->empty() && href->rfind("javascript:", 0) != 0 &&
            href->rfind("#", 0) != 0)
          nav = url_resolve(inst->final_url, *href);
        break;
      }
    }
    bool prevented = false;
    if (!JS_IsUndefined(fn) && !JS_IsException(fn)) {
      for (Node* cur = n; cur && !prevented; cur = cur->parent) {
        JSValue ev = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "click"));
        JS_SetPropertyStr(ctx, ev, "clientX", JS_NewFloat64(ctx, c.x));
        JS_SetPropertyStr(ctx, ev, "clientY", JS_NewFloat64(ctx, c.y));
        JS_SetPropertyStr(ctx, ev, "button", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_FALSE);
        JS_SetPropertyStr(ctx, ev, "preventDefault",
                          JS_NewCFunction(ctx, js_ev_prevent_default, "preventDefault", 0));
        JS_SetPropertyStr(ctx, ev, "stopPropagation",
                          JS_NewCFunction(ctx, js_ev_stop_propagation, "stopPropagation", 0));
        /* SetPropertyStr consumes its value; dup the refs we also keep */
        JSValue target = wrap_node(ctx, n);
        JS_SetPropertyStr(ctx, ev, "target", target); /* consumed */
        JSValue current = wrap_node(ctx, cur);
        JS_SetPropertyStr(ctx, ev, "currentTarget", JS_DupValue(ctx, current));
        JSValue ev_borrow = JS_DupValue(ctx, ev);
        JSValue args[2] = {current, ev_borrow};
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, current);
        JS_FreeValue(ctx, ev_borrow);
        if (JS_IsException(r)) report_js_exception(inst);
        else JS_FreeValue(ctx, r);
        JSValue dp = JS_GetPropertyStr(ctx, ev, "defaultPrevented");
        prevented = JS_ToBool(ctx, dp) > 0;
        JS_FreeValue(ctx, dp);
        JSValue sp = JS_GetPropertyStr(ctx, ev, "_propagationStopped");
        bool stopped = JS_ToBool(ctx, sp) > 0;
        JS_FreeValue(ctx, sp);
        JS_FreeValue(ctx, ev); /* single owner free */
        if (stopped) break;
      }
    }
    JS_FreeValue(ctx, fn);
    fn = JS_UNDEFINED;
    /* default action: follow links unless the page prevented it */
    if (!prevented && !nav.empty()) {
      std::lock_guard<std::mutex> nlk(inst->nav_mtx);
      inst->nav_request = nav;
      inst->nav_pending = true;
    }
  }
  JS_FreeValue(ctx, g);
}

} /* namespace sqrl */

SQRL_API int square_send_click(square_instance* p, float x, float y, int button) {
  Instance* inst = as_inst(p);
  if (!inst || button != 0) return 0;
  int nav_started = 0;
  {
    std::lock_guard<std::mutex> doclk(inst->doc_mtx);
    Node* hit = hit_test_page(inst, x, y);
    Node* n = nullptr;
    for (Node* cur = hit; cur; cur = cur->parent) {
      if (cur->type == N_ELEMENT) { n = cur; break; }
    }
    if (n) {
      std::lock_guard<std::mutex> lk(inst->js_mtx);
      int64_t id = node_id_for(n);
      if (id) {
        inst->click_queue.push_back({(int)id, x, y});
        inst->dirty = true;
      }
    }
  }
  /* did a queued click already request navigation? (may happen on re-entry) */
  {
    std::lock_guard<std::mutex> nlk(inst->nav_mtx);
    if (inst->nav_pending) nav_started = 1;
  }
  return nav_started;
}

void square_load_html(square_instance* p, const char* html_utf8, const char* base_url) {
  Instance* inst = as_inst(p);
  if (!inst || !html_utf8) return;
  if (inst->page_thread.joinable()) inst->page_thread.join();
  inst->cancel = false;
  inst->dirty = false;
  inst->loading = 0;
  inst->pending_html = html_utf8;
  std::string base = base_url ? base_url : "";
  run_with_big_stack([inst, base]() { page_thread_entry(inst, base, true); }, inst->page_thread);
}

void square_stop(square_instance* p) {
  Instance* inst = as_inst(p);
  if (inst) inst->cancel = true;
}

int square_get_state(square_instance* p) {
  Instance* inst = as_inst(p);
  return inst ? inst->state.load() : SQ_STATE_IDLE;
}

void square_wait_state(square_instance* p, int state, int timeout_ms) {
  Instance* inst = as_inst(p);
  if (!inst) return;
  auto start = std::chrono::steady_clock::now();
  while (inst->state.load() != state) {
    if (inst->state.load() == SQ_STATE_ERROR && state != SQ_STATE_ERROR) return;
    if (timeout_ms >= 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                .count() > timeout_ms)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
}

const char* square_get_title(square_instance* p) {
  Instance* inst = as_inst(p);
  return inst ? inst->page_title.c_str() : "";
}

const char* square_get_url(square_instance* p) {
  Instance* inst = as_inst(p);
  return inst ? inst->final_url.c_str() : "";
}

int square_get_page_size(square_instance* p, int* out_w, int* out_h) {
  Instance* inst = as_inst(p);
  if (!inst) return 0;
  std::lock_guard<std::mutex> lk(inst->mtx);
  if (out_w) *out_w = inst->vw;
  if (out_h) *out_h = inst->page_h > 0 ? inst->page_h : inst->vh;
  return inst->root_box ? 1 : 0;
}

int square_render_bgra(square_instance* p, uint8_t* dst, int dst_stride) {
  Instance* inst = as_inst(p);
  if (!inst || !dst) return 0;
  std::lock_guard<std::mutex> plk(inst->paint_mtx);
  if (!inst->latest.pix || inst->latest.w <= 0) return 0;
  int copy_w = std::min(dst_stride / 4, inst->latest.w);
  for (int y = 0; y < inst->latest.h; y++) {
    memcpy(dst + (size_t)y * dst_stride, inst->latest.row32(y), copy_w * 4);
  }
  return 1;
}

int square_save_png(square_instance* p, const char* path, int full_page) {
  Instance* inst = as_inst(p);
  if (!inst || !path) return 0;
  std::lock_guard<std::mutex> doclk(inst->doc_mtx);
  std::lock_guard<std::mutex> lk(inst->mtx);
  if (!inst->doc || !inst->doc->body) return 0;
  int w = inst->vw;
  int h = full_page ? std::max(inst->page_h, inst->vh) : inst->vh;
  if (h > 30000) h = 30000;
  Surface s;
  s.alloc(w, h);
  /* ensure layout fresh */
  style_tree(inst->doc, inst->vw, inst->vh);
  if (inst->root_box) free_box_tree(inst->root_box);
  inst->root_box = layout_document(inst->doc, inst->vw, inst->vh, &inst->images);
  inst->page_h = inst->root_box ? (int)layout_page_height(inst->root_box) : h;
  if (full_page) h = std::max(std::min(inst->page_h, 30000), inst->vh);
  engine_log(1, "[save] root=%p kids=%zu h=%.0f page_h=%d", (void*)inst->root_box,
             inst->root_box ? inst->root_box->kids.size() : 0,
             inst->root_box ? inst->root_box->h : 0.f, inst->page_h);
  paint_document(inst->doc, inst->root_box, &s, &inst->images);
  bool ok = gfx_write_png(path, s.pix, w, h, s.stride);
  s.free_pix();
  /* optional DOM/computed-style dump for diagnostics */
  if (getenv("SQRL_DUMP_DOM") && inst->doc && inst->doc->body) {
    int dumped = 0, limit = 500;
    const char* lim = getenv("SQRL_DUMP_DOM_LIMIT");
    if (lim) limit = atoi(lim);
    float hit_x = -1, hit_y = -1;
    const char* hit = getenv("SQRL_DUMP_HIT");
    if (hit) sscanf(hit, "%f,%f", &hit_x, &hit_y);
    std::function<void(Node*, int)> dump = [&](Node* n, int depth) {
      if (dumped >= limit || !n) return;
      if (n->type == N_ELEMENT && n->cs) {
        bool hit_this = hit_x >= 0 && n->box && hit_x >= n->box->x && hit_x < n->box->x + n->box->w &&
                        hit_y >= n->box->y && hit_y < n->box->y + n->box->h;
        if (hit && !hit_this) { for (Node* k : n->children) dump(k, depth + 1); return; }
        ComputedStyle* c = n->cs;
        const std::string* cls = n->attr("class");
        std::string cl = cls ? *cls : "";
        if (cl.size() > 90) cl = cl.substr(0, 90) + "…";
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "[%2d] %s.%s disp=%d pos=%d bg=rgba(%d,%d,%d,%d) col=rgba(%d,%d,%d,%d) "
                 "fs=%.0f fw=%d box=%s",
                 depth, n->tag.c_str(), cl.c_str(), (int)c->display, (int)c->position,
                 c->bg_color.r, c->bg_color.g, c->bg_color.b, c->bg_color.a, c->color.r,
                 c->color.g, c->color.b, c->color.a, c->font_size, (int)c->font_weight,
                 n->box ? "Y" : "N");
        std::string line = buf;
        if (n->box)
          snprintf(buf, sizeof(buf), " x=%.0f y=%.0f w=%.0f h=%.0f", n->box->x, n->box->y,
                   n->box->w, n->box->h);
        else
          buf[0] = 0;
        line += buf;
        if (c->display == D_FLEX || c->display == D_INLINE_FLEX ||
            c->display == D_INLINE_FLEX_BOX) {
          snprintf(buf, sizeof(buf), " flex-dir=%d wrap=%d justify=%d ai=%d grow=%.1f gapr=%.1f",
                   (int)c->flex_dir, (int)c->flex_wrap, (int)c->justify,
                   (int)c->align, c->flex_grow,
                   c->gap_row.unit == L_PX ? c->gap_row.val : -1.f);
          line += buf;
        }
        if (c->display == D_GRID) {
          snprintf(buf, sizeof(buf), " grid cols=%zu rows=%zu", c->grid_cols.size(),
                   c->grid_rows.size());
          line += buf;
        }
        engine_log(1, "%s", line.c_str());
        dumped++;
      }
      for (Node* k : n->children) dump(k, depth + 1);
    };
    engine_log(1, "=== DOM DUMP (limit %d) ===", limit);
    dump(inst->doc->body, 0);
  }
  return ok ? 1 : 0;
}
