/* =====================================================================
 *  Square Browser Engine — square_css_selectors.cpp
 *  Selector matching: pseudo-classes, attribute selectors, compound and
 *  full right-to-left matching.
 * ===================================================================== */
#include "square_css_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sqrl {
/* ============================ selector matching ============================ */
static bool nth_match(const std::string& arg, int index /*1-based*/) {
  /* forms: odd, even, 5, 2n, 2n+1, -n+3, n */
  std::string a = lc(trim(arg));
  a.erase(std::remove(a.begin(), a.end(), ' '), a.end());
  if (a == "odd") return index % 2 == 1;
  if (a == "even") return index % 2 == 0;
  size_t n_pos = a.find('n');
  if (n_pos == std::string::npos) {
    return atoi(a.c_str()) == index;
  }
  std::string an = a.substr(0, n_pos);
  std::string rest = a.substr(n_pos + 1);
  int step;
  if (an.empty() || an == "+") step = 1;
  else if (an == "-") step = -1;
  else step = atoi(an.c_str());
  int offset = 0;
  if (!rest.empty()) offset = atoi(rest.c_str());
  if (step == 0) return index == offset;
  if (step > 0) {
    int diff = index - offset;
    return diff >= 0 && diff % step == 0;
  }
  /* negative step (e.g. -n+3): matches index <= offset counting down */
  int diff = offset - index;
  return diff >= 0 && diff % (-step) == 0;
}

static bool pseudo_class_match(const Node* el, const std::string& pc) {
  if (pc == "root") return el->parent && el->parent->type == N_DOCUMENT;
  if (pc == "hover") {
    /* the hovered element and all its ancestors match :hover */
    for (const Node* a = g_hover_node; a; a = a->parent)
      if (a == el) return true;
    return false;
  }
  if (pc == "focus" || pc == "active" || pc == "focus-visible" ||
      pc == "focus-within" || pc == "target" || pc == "visited" || pc == "link" ||
      pc == "checked" || pc == "disabled" || pc == "enabled" || pc == "placeholder-shown" ||
      pc == "focus-within" || pc == "autofill")
    return false; /* static render: interactive states never match (link never visited) */
  if (pc == "first-child") {
    if (!el->parent) return true;
    for (const Node* c : el->parent->children)
      if (c->type == N_ELEMENT) return c == el;
    return false;
  }
  if (pc == "last-child") {
    if (!el->parent) return true;
    for (int i = (int)el->parent->children.size() - 1; i >= 0; i--) {
      const Node* c = el->parent->children[i];
      if (c->type == N_ELEMENT) return c == el;
    }
    return false;
  }
  if (pc == "only-child") {
    int count = 0;
    for (const Node* c : el->parent->children)
      if (c->type == N_ELEMENT) count++;
    return count == 1;
  }
  if (pc == "empty") {
    for (const Node* c : el->children)
      if (c->type == N_ELEMENT || (c->type == N_TEXT && !c->data.empty())) return false;
    return true;
  }
  if (starts_with(pc, "nth-child(")) {
    std::string arg = pc.substr(10, pc.size() - 11);
    int idx = 1;
    for (const Node* c : el->parent->children) {
      if (c->type == N_ELEMENT) {
        if (c == el) return nth_match(arg, idx);
        idx++;
      }
    }
    return false;
  }
  if (starts_with(pc, "nth-last-child(")) {
    std::string arg = pc.substr(15, pc.size() - 16);
    int idx = 1;
    for (int i = (int)el->parent->children.size() - 1; i >= 0; i--) {
      const Node* c = el->parent->children[i];
      if (c->type == N_ELEMENT) {
        if (c == el) return nth_match(arg, idx);
        idx++;
      }
    }
    return false;
  }
  if (starts_with(pc, "not(")) {
    std::string arg = pc.substr(4, pc.size() - 5);
    /* :not(a, b) = none of the listed selectors may match */
    std::vector<std::string> list;
    split_selector_list(arg, &list);
    if (list.empty()) return true;
    for (auto& one : list) {
      Selector sel;
      if (parse_selector(one, &sel) && selector_matches(el, sel)) return false;
    }
    return true;
  }
  if (starts_with(pc, "is(") || starts_with(pc, "where(")) {
    std::string arg = pc.substr(pc.find('(') + 1, pc.size() - pc.find('(') - 2);
    /* :is(a, b) = any of the listed selectors matches */
    std::vector<std::string> list;
    split_selector_list(arg, &list);
    for (auto& one : list) {
      Selector sel;
      if (parse_selector(one, &sel) && selector_matches(el, sel)) return true;
    }
    return false;
  }
  return false;
}

static bool attr_sel_match(const Node* el, const AttrSel& a) {
  const std::string* v = el->attr(a.name.c_str());
  if (a.op == 0) return v != nullptr;
  if (!v) return false;
  switch (a.op) {
    case '=': return *v == a.val;
    case '~': {
      size_t p = 0;
      while (p <= v->size()) {
        size_t e = v->find(' ', p);
        if (e == std::string::npos) e = v->size();
        if (v->compare(p, e - p, a.val) == 0) return true;
        p = e + 1;
      }
      return false;
    }
    case '^': return v->rfind(a.val, 0) == 0 && !a.val.empty();
    case '$': return v->size() >= a.val.size() && v->compare(v->size() - a.val.size(), a.val.size(), a.val) == 0;
    case '*': return v->find(a.val) != std::string::npos;
    case '|': return *v == a.val || v->rfind(a.val + "-", 0) == 0;
  }
  return false;
}

static bool compound_match(const Node* el, const SelPart& part) {
  if (!part.tag.empty() && part.tag != "*" && el->tag != part.tag) return false;
  for (auto& id : part.ids) {
    const std::string* v = el->attr("id");
    if (!v || *v != id) return false;
  }
  for (auto& cls : part.classes)
    if (!el->has_class(cls.c_str())) return false;
  for (auto& a : part.attrs)
    if (!attr_sel_match(el, a)) return false;
  for (auto& pc : part.pseudo_classes)
    if (!pseudo_class_match(el, pc)) return false;
  return true;
}

bool css_parse_selector_public(const char* text, Selector* out) {
  return parse_selector(text ? text : "", out);
}

bool selector_matches(const Node* el, const Selector& sel) {
  /* match right-to-left */
  const Node* cur = el;
  int idx = (int)sel.parts.size() - 1;
  if (!compound_match(cur, sel.parts[idx])) return false;
  while (idx > 0) {
    uint8_t comb = sel.parts[idx].combinator;
    if (comb == 0) { /* descendant */
      bool found = false;
      for (const Node* anc = cur->parent; anc; anc = anc->parent) {
        if (anc->type != N_ELEMENT) continue;
        if (compound_match(anc, sel.parts[idx - 1])) {
          cur = anc;
          found = true;
          break;
        }
      }
      if (!found) return false;
    } else if (comb == 1) { /* child */
      const Node* p = cur->parent;
      if (!p || p->type != N_ELEMENT || !compound_match(p, sel.parts[idx - 1])) return false;
      cur = p;
    } else if (comb == 2) { /* adjacent sibling */
      const Node* prev = nullptr;
      if (!cur->parent) return false;
      for (const Node* c : cur->parent->children) {
        if (c == cur) break;
        if (c->type == N_ELEMENT) prev = c;
      }
      if (!prev || !compound_match(prev, sel.parts[idx - 1])) return false;
      cur = prev;
    } else if (comb == 3) { /* general sibling */
      bool found = false;
      if (!cur->parent) return false;
      for (const Node* c : cur->parent->children) {
        if (c == cur) break;
        if (c->type == N_ELEMENT && compound_match(c, sel.parts[idx - 1])) {
          cur = c;
          found = true;
          break;
        }
      }
      if (!found) return false;
    }
    idx--;
  }
  return true;
}

} /* namespace sqrl */
