#include "square_platform_internal.h"

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

void mark_dirty_node(Instance* inst, Node* n) {
  inst->dirty = true;
  if (n) {
    std::lock_guard<std::mutex> lk(inst->dirty_mtx);
    inst->dirty_nodes.push_back(n);
  }
}

JSValue js_create_element(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_create_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* data = argc > 0 ? JS_ToCString(ctx, argv[0]) : "";
  Node* n = new Node();
  n->type = N_TEXT;
  n->data = data ? data : "";
  if (data) JS_FreeCString(ctx, data);
  mark_dirty(inst);
  return wrap_node(ctx, n);
}

JSValue js_create_comment(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* data = argc > 0 ? JS_ToCString(ctx, argv[0]) : "";
  Node* n = new Node();
  n->type = N_COMMENT;
  n->data = data ? data : "";
  if (data) JS_FreeCString(ctx, data);
  mark_dirty(inst);
  return wrap_node(ctx, n);
}

JSValue js_append_child(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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
mark_dirty_node(inst, parent);
  return JS_DupValue(ctx, argv[1]);
}

JSValue js_insert_before(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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
mark_dirty_node(inst, parent);
  return JS_DupValue(ctx, argv[1]);
}

JSValue js_remove_child(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  if (argc < 2) return JS_ThrowTypeError(ctx, "removeChild(parent, child)");
  Node* parent = unwrap_node(argv[0]);
  Node* child = unwrap_node(argv[1]);
  if (!parent || !child) return JS_ThrowTypeError(ctx, "bad node");
  auto& sibs = parent->children;
  sibs.erase(std::remove(sibs.begin(), sibs.end(), child), sibs.end());
  child->parent = nullptr;
mark_dirty_node(inst, parent);
  return JS_DupValue(ctx, argv[1]);
}

JSValue js_child_index(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_NewInt32(ctx, -1);
  Node* n = unwrap_node(argv[0]);
  if (!n) return JS_NewInt32(ctx, -1);
  return JS_NewInt32(ctx, n->child_index());
}

JSValue js_child_nodes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_parent(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 0); }
JSValue js_first(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 1); }
JSValue js_last(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 2); }
JSValue js_next(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 3); }
JSValue js_prev(JSContext* c, JSValueConst t, int argc, JSValueConst* a) { return js_relative(c, t, argc, a, 4); }

JSValue js_node_type(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_tag(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_get_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_set_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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
mark_dirty_node(inst, n);
  return JS_UNDEFINED;
}

JSValue js_remove_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  const char* name = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
  if (n && name) {
    n->remove_attr(plat_lc(name));
mark_dirty_node(inst, n);
  }
  if (name) JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

JSValue js_has_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  const char* name = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
  bool has = n && name && n->attr(name) != nullptr;
  if (name) JS_FreeCString(ctx, name);
  return JS_NewBool(ctx, has);
}

JSValue js_set_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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
mark_dirty_node(inst, n);
  }
  if (val) JS_FreeCString(ctx, val);
  return JS_UNDEFINED;
}

JSValue js_get_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_set_inner_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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
mark_dirty_node(inst, n);
  }
  if (val) JS_FreeCString(ctx, val);
  return JS_UNDEFINED;
}

JSValue js_get_inner_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Node* n = argc > 0 ? unwrap_node(argv[0]) : nullptr;
  if (!n) return JS_NewString(ctx, "");
  std::string out = serialize_html(n);
  return JS_NewStringLen(ctx, out.c_str(), out.size());
}

JSValue js_get_by_id(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_query_selector_all(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_mark_dirty(JSContext* ctx, JSValueConst, int, JSValueConst*) {
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

JSValue js_set_timeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return timer_set(ctx, argc, argv, false);
}

JSValue js_set_interval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  return timer_set(ctx, argc, argv, true);
}

JSValue js_clear_timeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_request_animation_frame(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
  std::lock_guard<std::mutex> lk(inst->js_mtx);
  int id = inst->raf_id++;
  inst->raf_map[id] = JS_DupValue(ctx, argv[0]);
  return JS_NewInt32(ctx, id);
}

JSValue js_cancel_animation_frame(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_local_storage_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* k = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
  if (!k) return JS_NULL;
  auto it = inst->local_storage.find(k);
  JSValue r = it != inst->local_storage.end() ? JS_NewStringLen(ctx, it->second.c_str(), it->second.size()) : JS_NULL;
  JS_FreeCString(ctx, k);
  return r;
}

JSValue js_local_storage_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
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

JSValue js_local_storage_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  Instance* inst = (Instance*)JS_GetContextOpaque(ctx);
  const char* k = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
  if (k) {
    inst->local_storage.erase(k);
    JS_FreeCString(ctx, k);
  }
  return JS_UNDEFINED;
}

} /* namespace sqrl */
