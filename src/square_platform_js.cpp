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

/* =====================================================================
 *  Square Browser Engine — square_platform.cpp (part 2: JS runtime,
 *  prelude, event loop)
 * ===================================================================== */

namespace sqrl {

/* ============================ module loader ============================ */

char* module_normalize(JSContext* ctx, const char* base_name, const char* name, void* opaque) {
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

JSModuleDef* module_loader(JSContext* ctx, const char* module_name, void* opaque) {
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
const char* ctx_doc_key = "__document__";

const char* k_js_prelude = R"PRELUDE(
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

void pump_fetches(Instance* inst) {
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

void pump_raf(Instance* inst) {
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

void fire_timers(Instance* inst) {
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

} /* namespace sqrl */
