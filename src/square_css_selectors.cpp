/* Selector matching: pseudo-classes, attribute selectors, compound and, full right-to-left matching */
#include "square_css_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sqrl {
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

static bool is_form_control(const std::string& tag) {
  return tag == "input" || tag == "button" || tag == "select" || tag == "textarea" ||
         tag == "option" || tag == "optgroup" || tag == "fieldset";
}

static bool pseudo_class_match(const Node* el, const std::string& pc) {
  if (pc == "root") return el->parent && el->parent->type == N_DOCUMENT;
  if (pc == "hover") {
    /* the hovered element and all its ancestors match :hover */
    for (const Node* a = g_hover_node; a; a = a->parent)
      if (a == el) return true;
    return false;
  }
  if (pc == "focus") return g_focus_node == el;
  if (pc == "focus-within") {
    for (const Node* a = g_focus_node; a; a = a->parent)
      if (a == el) return true;
    return false;
  }
  if (pc == "active" || pc == "focus-visible" || pc == "target" || pc == "visited" ||
      pc == "placeholder-shown" || pc == "autofill")
    return false; /* no press/target tracking; link never visited */
  if (pc == "link") return el->tag == "a" && el->attr("href");
  if (pc == "disabled") return is_form_control(el->tag) && el->attr("disabled") != nullptr;
  if (pc == "enabled") return is_form_control(el->tag) && el->attr("disabled") == nullptr;
  if (pc == "checked") {
    if (el->tag == "input") return el->attr("checked") != nullptr;
    if (el->tag == "option") return el->attr("selected") != nullptr;
    return false;
  }
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
  /* of-type family: index among same-tag element siblings */
  if (pc == "first-of-type" || pc == "last-of-type" || pc == "only-of-type" ||
      starts_with(pc, "nth-of-type(") || starts_with(pc, "nth-last-of-type(")) {
    int fwd = 0, bwd = 0;
    int idx_fwd = 0, idx_bwd = 0;
    const auto& ch = el->parent ? el->parent->children : el->children;
    for (const Node* c : ch) {
      if (c->type != N_ELEMENT || c->tag != el->tag) continue;
      fwd++;
      if (c == el) idx_fwd = fwd;
    }
    for (int i = (int)ch.size() - 1; i >= 0; i--) {
      const Node* c = ch[i];
      if (c->type != N_ELEMENT || c->tag != el->tag) continue;
      bwd++;
      if (c == el) idx_bwd = bwd;
    }
    if (pc == "first-of-type") return idx_fwd == 1;
    if (pc == "last-of-type") return idx_bwd == 1;
    if (pc == "only-of-type") return fwd == 1;
    if (starts_with(pc, "nth-of-type(")) return nth_match(pc.substr(12, pc.size() - 13), idx_fwd);
    return nth_match(pc.substr(17, pc.size() - 18), idx_bwd);
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
  if (starts_with(pc, "has(")) {
    /* :has(<relative-selector>), the relative selector is evaluated against
     * el's descendants (and siblings, when the inner selector starts with a
     * combinator). The spec anchor element is `el`. We support:
     *   :has(.foo)              any descendant matching .foo
     *   :has(> .foo)            a child matching .foo
     *   :has(+ .foo)            an adjacent sibling matching .foo
     *   :has(~ .foo)            a general sibling matching .foo
     *   :has(a, b, c)           any of the listed selectors (logical OR)
     * Implementation: split the inner argument on top-level commas, parse each
     * as a Selector, then scan el's subtree (or sibling scope) for any node
     * that satisfies selector_matches against that selector. The first match
     * wins. Returns false on parse failure of every alternative.
     */
    std::string arg = pc.substr(4, pc.size() - 5);
    std::vector<std::string> list;
    split_selector_list(arg, &list);
    if (list.empty()) return false;
    /* collect candidate scope: descendants by default; for relative
     * combinators we operate on the parent's children starting after el. */
    auto collect_descendants = [](const Node* root, std::vector<const Node*>* out) {
      std::function<void(const Node*)> walk = [&](const Node* n) {
        for (const Node* c : n->children) {
          if (c->type == N_ELEMENT) { out->push_back(c); walk(c); }
          else if (c->type == N_TEXT) walk(c);
        }
      };
      walk(root);
    };
    for (auto& one : list) {
      std::string sel_text = trim(one);
      if (sel_text.empty()) continue;
      /* explicit combinator? parse_selector expects a leading compound, so we
       * strip the leading combinator and track it. Default scope = descendants. */
      uint8_t leading_comb = 0; /* 0 descendant, 1 child, 2 adjacent, 3 sibling */
      if (!sel_text.empty() && (sel_text[0] == '>' || sel_text[0] == '+' || sel_text[0] == '~')) {
        if (sel_text[0] == '>') leading_comb = 1;
        else if (sel_text[0] == '+') leading_comb = 2;
        else leading_comb = 3;
        sel_text.erase(sel_text.begin());
        sel_text = trim(sel_text);
        if (sel_text.empty()) continue;
      }
      Selector sel;
      if (!parse_selector(sel_text, &sel)) continue;
      if (leading_comb == 0 || leading_comb == 1) {
        /* descendants (any depth for 0, immediate children for 1) */
        std::vector<const Node*> scope;
        collect_descendants(el, &scope);
        for (const Node* cand : scope) {
          if (leading_comb == 1 && cand->parent != el) continue;
          if (selector_matches(cand, sel)) return true;
        }
      } else {
        /* siblings: scan parent's children starting from el */
        if (!el->parent) continue;
        const auto& sibs = el->parent->children;
        size_t i = 0;
        for (; i < sibs.size(); i++) if (sibs[i] == el) break;
        if (leading_comb == 2) {
          /* adjacent: nearest preceding element sibling, but :has(+ X) tests
           * the FOLLOWING sibling per spec; we test the next element after el */
          for (size_t k = i + 1; k < sibs.size(); k++) {
            if (sibs[k]->type == N_ELEMENT) {
              if (selector_matches(sibs[k], sel)) return true;
              break; /* only the immediately-following element sibling */
            }
          }
        } else {
          /* general sibling: any element sibling after el */
          for (size_t k = i + 1; k < sibs.size(); k++) {
            if (sibs[k]->type == N_ELEMENT && selector_matches(sibs[k], sel)) return true;
          }
        }
      }
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
