/* =====================================================================
 *  Square Browser Engine — square_layout_common.cpp
 *  Shared layout helpers: length resolution, style predicates, fonts,
 *  intrinsic sizing, box edges, shrink-to-fit measurement, box tree utils.
 * ===================================================================== */
#include "square_layout_internal.h"

#include <algorithm>
#include <cctype>

namespace sqrl {

/* ============================ length resolution ============================ */
float res_l(const Length& L, float pct_basis, float em, LayoutCtx& lc) {
  switch (L.unit) {
    case L_AUTO:
    case L_NORMAL:
      return 0;
    case L_PX: return L.val;
    case L_PCT: return L.val * pct_basis / 100.f;
    case L_EM: return L.val * em;
    case L_REM: return L.val * lc.rem;
    case L_VW: return L.val * lc.vw / 100.f;
    case L_VH: return L.val * lc.vh / 100.f;
    case L_CH: return L.val;
    default: break;
  }
  if (L.unit == L_CALC || L.unit == L_CLAMP || L.unit == L_MM) {
    float sum = 0;
    for (int i = 0; i < L.ncalc; i++) {
      const CalcTerm& t = L.terms[i];
      switch ((LUnit)t.unit) {
        case L_PX: sum += t.val * t.mul; break;
        case L_PCT: sum += t.val * pct_basis / 100.f * t.mul; break;
        case L_EM: sum += t.val * em * t.mul; break;
        case L_REM: sum += t.val * lc.rem * t.mul; break;
        case L_VW: sum += t.val * lc.vw / 100.f * t.mul; break;
        case L_VH: sum += t.val * lc.vh / 100.f * t.mul; break;
        default: sum += t.val * t.mul; break;
      }
    }
    if (L.unit == L_MM && L.mm_op) {
      auto sum_terms = [&](const CalcTerm* ts, int n) {
        float s2 = 0;
        for (int i = 0; i < n; i++) {
          switch ((LUnit)ts[i].unit) {
            case L_PX: s2 += ts[i].val * ts[i].mul; break;
            case L_PCT: s2 += ts[i].val * pct_basis / 100.f * ts[i].mul; break;
            case L_EM: s2 += ts[i].val * em * ts[i].mul; break;
            case L_REM: s2 += ts[i].val * lc.rem * ts[i].mul; break;
            case L_VW: s2 += ts[i].val * lc.vw / 100.f * ts[i].mul; break;
            case L_VH: s2 += ts[i].val * lc.vh / 100.f * ts[i].mul; break;
            default: s2 += ts[i].val * ts[i].mul; break;
          }
        }
        return s2;
      };
      float r1 = sum_terms(L.terms_min, L.nmin);
      float r2 = L.nmax ? sum_terms(L.terms_max, L.nmax) : r1;
      return L.mm_op == 1 ? std::min(r1, r2) : std::max(r1, r2);
    }
    if (L.has_clamp && L.unit == L_CLAMP) {
      auto sum_terms = [&](const CalcTerm* ts, int n) {
        float s2 = 0;
        for (int i = 0; i < n; i++) {
          switch ((LUnit)ts[i].unit) {
            case L_PX: s2 += ts[i].val * ts[i].mul; break;
            case L_PCT: s2 += ts[i].val * pct_basis / 100.f * ts[i].mul; break;
            case L_EM: s2 += ts[i].val * em * ts[i].mul; break;
            case L_REM: s2 += ts[i].val * lc.rem * ts[i].mul; break;
            case L_VW: s2 += ts[i].val * lc.vw / 100.f * ts[i].mul; break;
            case L_VH: s2 += ts[i].val * lc.vh / 100.f * ts[i].mul; break;
            default: s2 += ts[i].val * ts[i].mul; break;
          }
        }
        return s2;
      };
      if (L.nmin) { float mn = sum_terms(L.terms_min, L.nmin); if (sum < mn) sum = mn; }
      if (L.nmax) { float mx = sum_terms(L.terms_max, L.nmax); if (sum > mx) sum = mx; }
    }
    return sum;
  }
  return L.val;
}

/* ============================ style predicates ============================ */
bool is_blocky(Display d) {
  return d == D_BLOCK || d == D_FLEX || d == D_GRID || d == D_TABLE || d == D_LIST_ITEM ||
         d == D_TABLE_ROW || d == D_TABLE_CELL || d == D_TABLE_ROW_GROUP;
}
bool is_atomic_inline(Display d) { return d == D_INLINE_BLOCK || d == D_INLINE_FLEX || d == D_INLINE_TABLE; }
bool node_invisible(const ComputedStyle* cs) {
  return !cs || cs->display == D_NONE || cs->visibility_hidden;
}

/* ============================ fonts / text ============================ */
void* font_for(const ComputedStyle* cs) {
  std::vector<std::string> fams = cs->font_family;
  if (fams.empty()) fams = {"Inter"};
  return font_resolve(fams, cs->font_weight, cs->italic, cs->font_size);
}

float line_height_px(const ComputedStyle* cs, void* font, LayoutCtx& lc, float* asc_out,
                     float* desc_out) {
  float asc = font ? font_ascent(font) : cs->font_size * 0.8f;
  float desc = font ? font_descent(font) : cs->font_size * 0.2f;
  float lh;
  if (cs->lh_normal || cs->line_height.unit == L_NORMAL) lh = (asc + desc) * cs->line_height_mul;
  else if (cs->line_height.unit == L_PCT) lh = cs->font_size * cs->line_height.val / 100.f;
  else lh = res_l(cs->line_height, cs->font_size, cs->font_size, lc);
  if (asc_out) *asc_out = asc;
  if (desc_out) *desc_out = desc;
  return lh;
}

std::string transform_text(const std::string& in, TextTransform tt) {
  if (tt == TT_NONE) return in;
  std::string out = in;
  if (tt == TT_UPPER) {
    for (auto& c : out) c = (char)toupper((unsigned char)c);
  } else if (tt == TT_LOWER) {
    for (auto& c : out) c = (char)tolower((unsigned char)c);
  } else if (tt == TT_CAP) {
    bool word_start = true;
    for (auto& c : out) {
      if (isspace((unsigned char)c)) word_start = true;
      else {
        if (word_start) c = (char)toupper((unsigned char)c);
        word_start = false;
      }
    }
  }
  return out;
}

/* ============================ box creation ============================ */
Box* new_box(Node* n, Box* parent) {
  Box* b = new Box();
  b->node = n;
  if (n && n->cs) b->cs = n->cs;
  b->parent = parent;
  if (n) n->box = b;
  return b;
}

/* ============================ replaced elements ============================ */
void replaced_intrinsic(Node* el, LayoutCtx& lc, float* w, float* h) {
  const std::string* wattr = el->attr("width");
  const std::string* hattr = el->attr("height");
  /* percentage/auto attr values carry no intrinsic size information */
  auto attr_px = [](const std::string* s) -> float {
    if (!s || s->empty()) return 0;
    if (s->find('%') != std::string::npos) return 0;
    if (*s == "auto") return 0;
    return (float)atof(s->c_str());
  };
  float aw = attr_px(wattr);
  float ah = attr_px(hattr);
  if (el->cs && el->cs->aspect_ratio > 0 && aw > 0 && ah <= 0) ah = aw / el->cs->aspect_ratio;
  if (el->cs && el->cs->aspect_ratio > 0 && ah > 0 && aw <= 0) aw = ah * el->cs->aspect_ratio;
  if (el->tag == "img") {
    const std::string* src = el->attr("src");
#ifdef LAYOUT_DEBUG
    if (getenv("SQRL_PROBE")) fprintf(stderr, "[probe] img el=%p src=%p val=%s\n", (void*)el, (void*)src, src ? src->c_str() : "-");
#endif
    if (src && lc.images) {
      ImageInfo ii = lc.images->find(*src);
      if (ii.decoded && ii.w > 0) {
        if (aw <= 0) aw = (float)ii.w;
        if (ah <= 0) ah = (float)ii.h;
      }
    }
  } else if (el->tag == "svg") {
    const std::string* vb = el->attr("viewbox");
    if (vb) {
      float nums[4] = {0, 0, 0, 0};
      int k = 0;
      size_t p = 0;
      while (k < 4 && p < vb->size()) {
        while (p < vb->size() && !isdigit((unsigned char)(*vb)[p]) && (*vb)[p] != '-' && (*vb)[p] != '.') p++;
        if (p >= vb->size()) break;
        nums[k++] = (float)atof(vb->c_str() + p);
        while (p < vb->size() && (isdigit((unsigned char)(*vb)[p]) || (*vb)[p] == '.' || (*vb)[p] == '-')) p++;
      }
      if (nums[2] > 0 && nums[3] > 0) {
        if (aw > 0 && ah <= 0) ah = aw * nums[3] / nums[2];
        else if (ah > 0 && aw <= 0) aw = ah * nums[2] / nums[3];
      }
    }
  }
  if (aw <= 0) aw = 300;
  if (ah <= 0) ah = 150;
  *w = aw;
  *h = ah;
}

/* ============================ measurement ============================ */
static float measure_text_width(Node* tn, const ComputedStyle* cs, LayoutCtx& lc) {
  (void)lc;
  const std::string txt = transform_text(tn->data, cs->text_transform);
  /* measure word-by-word exactly like the inline layout splits runs
   * (word items + single-space items), so shrink-to-fit sizing never
   * underestimates the laid-out width (kerning across spaces differs). */
  void* f = font_for(cs);
  if (!f) return 0;
  float total = 0;
  std::string word;
  auto flush_word = [&]() {
    if (word.empty()) return;
    total += font_measure_text(f, word.c_str(), cs->letter_spacing);
    word.clear();
  };
  bool any_word = false;
  for (char c : txt) {
    if (isspace((unsigned char)c)) {
      flush_word();
      if (any_word) total += font_measure_text(f, " ", cs->letter_spacing);
    } else {
      word += c;
      any_word = true;
    }
  }
  flush_word();
  return total;
}

float measure_max_content_width(Node* el, LayoutCtx& lc) {
  ComputedStyle* cs = el->cs;
  if (node_invisible(cs)) return 0;
  /* form controls have no element children: generic measurement would give 0 */
  if (el->tag == "input" || el->tag == "select") {
    float w = cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL
                  ? res_l(cs->width, 0, cs->font_size, lc)
                  : (el->attr("size") ? (float)atoi(el->attr("size")->c_str()) * 8.f : 150.f);
    return w + res_l(cs->padding[3], w, cs->font_size, lc) + res_l(cs->padding[1], w, cs->font_size, lc);
  }
  if (el->tag == "img" || el->tag == "svg") {
    float w, h;
    replaced_intrinsic(el, lc, &w, &h);
    if (cs->width.unit != L_AUTO) w = res_l(cs->width, 0, cs->font_size, lc);
    return w + res_l(cs->padding[3], w, cs->font_size, lc) + res_l(cs->padding[1], w, cs->font_size, lc);
  }
  if (cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL) {
    float w = res_l(cs->width, 0, cs->font_size, lc);
    return w + res_l(cs->padding[3], w, cs->font_size, lc) + res_l(cs->padding[1], w, cs->font_size, lc);
  }
  float inline_total = 0;
  float max_child = 0;
  bool has_inline = false;
  for (Node* c : el->children) {
    if (c->type == N_TEXT) {
      if (!c->data.empty()) has_inline = true;
      inline_total += measure_text_width(c, cs, lc);
      continue;
    }
    if (c->type != N_ELEMENT || node_invisible(c->cs)) continue;
    if (c->cs->position == P_ABSOLUTE || c->cs->position == P_FIXED) continue;
    float cw2 = measure_max_content_width(c, lc);
    ComputedStyle* ccs = c->cs;
    cw2 += res_l(ccs->margin[3], 0, ccs->font_size, lc) + res_l(ccs->margin[1], 0, ccs->font_size, lc);
    cw2 += res_l(ccs->padding[3], 0, ccs->font_size, lc) + res_l(ccs->padding[1], 0, ccs->font_size, lc);
    if (is_blocky(ccs->display)) max_child = std::max(max_child, cw2);
    else {
      inline_total += cw2;
      has_inline = true;
    }
  }
  float base = has_inline ? inline_total : max_child;
  /* flex containers space their items with column-gap — max-content must
   * include it or buttons shrink-wrap and their text wraps */
  if (cs->display == D_FLEX || cs->display == D_INLINE_FLEX) {
    int n_items = 0;
    bool any_text = false;
    for (Node* c : el->children) {
      if (c->type == N_TEXT) { if (!c->data.empty()) any_text = true; }
      else if (c->type == N_ELEMENT && !node_invisible(c->cs) &&
               c->cs->position != P_ABSOLUTE && c->cs->position != P_FIXED) n_items++;
    }
    int gaps = n_items + (any_text ? 1 : 0) - 1;
    if (gaps > 0) base += res_l(cs->gap_col, 0, cs->font_size, lc) * (float)gaps;
  }
  if (is_blocky(cs->display) || is_atomic_inline(cs->display)) {
    base += res_l(cs->padding[3], 0, cs->font_size, lc) + res_l(cs->padding[1], 0, cs->font_size, lc);
    base += (cs->border_style[3] ? res_l(cs->border_width[3], 0, cs->font_size, lc) : 0) +
            (cs->border_style[1] ? res_l(cs->border_width[1], 0, cs->font_size, lc) : 0);
  }
  if (getenv("SQRL_DBG_MEASURE") && el->tag == "a")
    fprintf(stderr, "[measure] a base=%.1f inline=%.1f maxchild=%.1f pads=%.1f\n", base,
            inline_total, max_child,
            res_l(cs->padding[3], 0, cs->font_size, lc) + res_l(cs->padding[1], 0, cs->font_size, lc));
  return base;
}

/* min-content width: longest word for text, tightest child for blocks.
 * Used as the flex-shrink floor (CSS min-width:auto on flex items). */
static float measure_text_min_width(Node* t, ComputedStyle* cs, LayoutCtx& lc) {
  const std::string& s = t->data;
  /* white-space:nowrap — the smallest the text can get is the whole line */
  if (cs->white_space == WS_NOWRAP) {
    void* f = font_for(cs);
    return f ? font_measure_text(f, s.c_str(), cs->letter_spacing) : 0;
  }
  float worst = 0;
  std::string word;
  auto flush_word = [&]() {
    if (word.empty()) return;
    void* f = font_for(cs);
    if (f) worst = std::max(worst, font_measure_text(f, word.c_str(), cs->letter_spacing));
    word.clear();
  };
  for (char ch : s) {
    if (isspace((unsigned char)ch))
      flush_word();
    else
      word.push_back(ch);
  }
  flush_word();
  return worst;
}

float measure_min_content_width(Node* el, LayoutCtx& lc) {
  ComputedStyle* cs = el->cs;
  if (node_invisible(cs)) return 0;
  if (el->tag == "input" || el->tag == "select") {
    float w = cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL
                  ? res_l(cs->width, 0, cs->font_size, lc)
                  : (el->attr("size") ? (float)atoi(el->attr("size")->c_str()) * 8.f : 150.f);
    return w + res_l(cs->padding[3], w, cs->font_size, lc) + res_l(cs->padding[1], w, cs->font_size, lc);
  }
  if (el->tag == "img" || el->tag == "svg") {
    float w, h;
    replaced_intrinsic(el, lc, &w, &h);
    if (cs->width.unit != L_AUTO) w = res_l(cs->width, 0, cs->font_size, lc);
    return w + res_l(cs->padding[3], w, cs->font_size, lc) + res_l(cs->padding[1], w, cs->font_size, lc);
  }
  if (cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL) {
    float w = res_l(cs->width, 0, cs->font_size, lc);
    return w + res_l(cs->padding[3], w, cs->font_size, lc) + res_l(cs->padding[1], w, cs->font_size, lc);
  }
  float run = 0, worst = 0;
  bool has_inline = false;
  for (Node* c : el->children) {
    if (c->type == N_TEXT) {
      run += measure_text_min_width(c, cs, lc);
      if (!c->data.empty()) has_inline = true;
      continue;
    }
    if (c->type != N_ELEMENT || node_invisible(c->cs)) continue;
    if (c->cs->position == P_ABSOLUTE || c->cs->position == P_FIXED) continue;
    float cw2 = measure_min_content_width(c, lc);
    ComputedStyle* ccs = c->cs;
    cw2 += res_l(ccs->margin[3], 0, ccs->font_size, lc) + res_l(ccs->margin[1], 0, ccs->font_size, lc);
    cw2 += res_l(ccs->padding[3], 0, ccs->font_size, lc) + res_l(ccs->padding[1], 0, ccs->font_size, lc);
    if (is_blocky(ccs->display)) {
      worst = std::max(worst, std::max(run, cw2));
      run = 0;
    } else {
      run += cw2;
      has_inline = true;
    }
  }
  worst = std::max(worst, run);
  if (is_blocky(cs->display) || is_atomic_inline(cs->display)) {
    worst += res_l(cs->padding[3], 0, cs->font_size, lc) + res_l(cs->padding[1], 0, cs->font_size, lc);
    worst += (cs->border_style[3] ? res_l(cs->border_width[3], 0, cs->font_size, lc) : 0) +
             (cs->border_style[1] ? res_l(cs->border_width[1], 0, cs->font_size, lc) : 0);
  }
  (void)has_inline;
  return worst;
}

/* ============================ box edges ============================ */
void resolve_box_edges(Box* b, const ComputedStyle* cs, float cw_basis, LayoutCtx& lc) {
  float em = cs->font_size;
  b->pad_t = res_l(cs->padding[0], cw_basis, em, lc);
  b->pad_r = res_l(cs->padding[1], cw_basis, em, lc);
  b->pad_b = res_l(cs->padding[2], cw_basis, em, lc);
  b->pad_l = res_l(cs->padding[3], cw_basis, em, lc);
  b->bor_t = cs->border_style[0] ? res_l(cs->border_width[0], cw_basis, em, lc) : 0;
  b->bor_r = cs->border_style[1] ? res_l(cs->border_width[1], cw_basis, em, lc) : 0;
  b->bor_b = cs->border_style[2] ? res_l(cs->border_width[2], cw_basis, em, lc) : 0;
  b->bor_l = cs->border_style[3] ? res_l(cs->border_width[3], cw_basis, em, lc) : 0;
}

/* ============================ box tree helpers ============================ */
void offset_box_recursive(Box* b, float dx, float dy) {
  b->x += dx;
  b->y += dy;
  for (auto* k : b->kids) offset_box_recursive(k, dx, dy);
  for (auto* o : b->out_of_flow) offset_box_recursive(o, dx, dy);
  for (auto& f : b->frags) {
    f.rect.x += dx;
    f.rect.y += dy;
    if (f.kind == InlineFrag::F_TEXT) {
      f.run.x += dx;
      f.run.y += dy;
    }
  }
}

void free_box_tree(Box* b) {
  if (!b) return;
  for (auto* k : b->kids) free_box_tree(k);
  for (auto* o : b->out_of_flow) free_box_tree(o);
  b->kids.clear();
  b->out_of_flow.clear();
  b->frags.clear();
  if (b->node && b->node->box == b) b->node->box = nullptr;
  delete b;
}

} /* namespace sqrl */
