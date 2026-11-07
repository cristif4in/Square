/* =====================================================================
 *  Square Browser Engine — square_layout.cpp
 *  Layout: block / inline / flexbox / grid / absolute positioning,
 *  plus the paint traversal into the gfx rasterizer.
 *
 *  Layout contract between parent and child:
 *    caller sets  b->x, b->y (border-box origin, page coords)
 *                 b->w (border-box width), b->cw (content width)
 *    layout_element() sets b->h (border-box height) and returns it.
 * ===================================================================== */
#include "square_internal.h"

#include <algorithm>
#include <cctype>
#include <deque>

namespace sqrl {

/* ============================ helpers ============================ */
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int g_painted_boxes = 0, g_painted_frags = 0, g_painted_images = 0;
void paint_debug_counts(int* boxes, int* frags, int* imgs) {
  *boxes = g_painted_boxes; *frags = g_painted_frags; *imgs = g_painted_images;
}

static float res_l(const Length& L, float pct_basis, float em, LayoutCtx& lc) {
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
  if (L.unit == L_CALC || L.unit == L_CLAMP) {
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

static bool is_blocky(Display d) {
  return d == D_BLOCK || d == D_FLEX || d == D_GRID || d == D_TABLE || d == D_LIST_ITEM ||
         d == D_TABLE_ROW || d == D_TABLE_CELL;
}
static bool is_atomic_inline(Display d) { return d == D_INLINE_BLOCK || d == D_INLINE_FLEX; }
static bool node_invisible(const ComputedStyle* cs) {
  return !cs || cs->display == D_NONE || cs->visibility_hidden;
}

static void* font_for(const ComputedStyle* cs) {
  std::vector<std::string> fams = cs->font_family;
  if (fams.empty()) fams = {"Inter"};
  return font_resolve(fams, cs->font_weight, cs->italic, cs->font_size);
}

static float line_height_px(const ComputedStyle* cs, void* font, LayoutCtx& lc, float* asc_out = nullptr,
                            float* desc_out = nullptr) {
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

static std::string transform_text(const std::string& in, TextTransform tt) {
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

/* ============================ prototypes ============================ */
static Box* new_box(Node* n, Box* parent);
static float layout_element(Box* b, Node* el, LayoutCtx& lc);
static float layout_block_content(Box* b, Node* el, LayoutCtx& lc);
static float layout_flex_content(Box* b, Node* el, LayoutCtx& lc);
static float layout_grid_content(Box* b, Node* el, LayoutCtx& lc);
static float layout_inline_seq(Box* container, std::vector<struct InlineItem>& seq, LayoutCtx& lc,
                               float x_origin, float y_origin);
static void flatten_nodes(const std::vector<Node*>& nodes, const ComputedStyle* cs,
                          std::vector<struct InlineItem>* items, LayoutCtx& lc, bool pre);
static void flatten_element(Node* el, const ComputedStyle* cs, std::vector<struct InlineItem>* items,
                            LayoutCtx& lc, bool pre);
static float layout_replaced_item(struct InlineItem* it, LayoutCtx& lc, float avail_w);
static float measure_max_content_width(Node* el, LayoutCtx& lc);
static float measure_min_content_width(Node* el, LayoutCtx& lc);
static void resolve_box_edges(Box* b, const ComputedStyle* cs, float cw_basis, LayoutCtx& lc);
static void layout_out_of_flow_children(Box* b, Node* el, LayoutCtx& lc);
void free_box_tree(Box* b);

static Box* new_box(Node* n, Box* parent) {
  Box* b = new Box();
  b->node = n;
  if (n && n->cs) b->cs = n->cs;
  b->parent = parent;
  if (n) n->box = b;
  return b;
}

/* ============================ replaced elements ============================ */
static void replaced_intrinsic(Node* el, LayoutCtx& lc, float* w, float* h) {
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

/* ============================ inline items ============================ */
struct InlineItem {
  enum Kind : uint8_t { I_TEXT, I_EL_START, I_EL_END, I_REPLACED, I_BR, I_SOFTBREAK } kind = I_TEXT;
  Node* node = nullptr;
  Box* box = nullptr;
  std::string text;
  TextRun run;
  float adv = 0;
  float ascent = 0, descent = 0;
  float h = 0;
  float baseline = 0;
  bool whitespace = false;
};

static void measure_text_item(InlineItem* it, const ComputedStyle* cs, LayoutCtx& lc) {
  void* f = font_for(cs);
  it->run.font = f;
  it->run.size = cs->font_size;
  it->run.spacing = cs->letter_spacing;
  it->run.color = cs->color;
  it->run.text = it->text;
  it->adv = f ? font_measure_text(f, it->text.c_str(), cs->letter_spacing) : 0;
  float asc, desc;
  float lh = line_height_px(cs, f, lc, &asc, &desc);
  float half = (lh - (asc + desc)) / 2;
  it->ascent = asc + half;
  it->descent = desc + half;
}

static void push_text_items(const std::string& data, const ComputedStyle* cs, Node* tn,
                            std::vector<InlineItem>* items, LayoutCtx& lc, bool pre) {
  if (data.empty()) return;
  if (pre) {
    size_t st = 0;
    for (size_t i = 0; i <= data.size(); i++) {
      if (i == data.size() || data[i] == '\n') {
        std::string chunk = data.substr(st, i - st);
        if (!chunk.empty()) {
          InlineItem it;
          it.kind = InlineItem::I_TEXT;
          it.node = tn;
          it.text = transform_text(chunk, cs->text_transform);
          measure_text_item(&it, cs, lc);
          items->push_back(it);
        }
        if (i < data.size()) {
          InlineItem br;
          br.kind = InlineItem::I_SOFTBREAK;
          br.node = tn;
          items->push_back(br);
        }
        st = i + 1;
      }
    }
    return;
  }
  const std::string txt = transform_text(data, cs->text_transform);
  size_t i = 0, n = txt.size();
  std::string word;
  while (i < n) {
    if (isspace((unsigned char)txt[i])) {
      if (!word.empty()) {
        InlineItem it;
        it.kind = InlineItem::I_TEXT;
        it.node = tn;
        it.text = word;
        measure_text_item(&it, cs, lc);
        items->push_back(it);
        word.clear();
      }
      while (i < n && isspace((unsigned char)txt[i])) i++;
      InlineItem sp;
      sp.kind = InlineItem::I_TEXT;
      sp.node = tn;
      sp.whitespace = true;
      sp.text = " ";
      measure_text_item(&sp, cs, lc);
      items->push_back(sp);
      continue;
    }
    word += txt[i];
    i++;
  }
  if (!word.empty()) {
    InlineItem it;
    it.kind = InlineItem::I_TEXT;
    it.node = tn;
    it.text = word;
    measure_text_item(&it, cs, lc);
    items->push_back(it);
  }
}

static void flatten_element(Node* el, const ComputedStyle* cs, std::vector<InlineItem>* items,
                            LayoutCtx& lc, bool pre) {
  bool prehere = pre || (cs && (cs->white_space == WS_PRE || cs->white_space == WS_PRE_WRAP ||
                                cs->white_space == WS_PRE_LINE));
  if (el->pseudo[0] && el->pseudo[0]->content_set) {
    InlineItem it;
    it.kind = InlineItem::I_TEXT;
    it.node = el;
    it.text = transform_text(el->pseudo[0]->content, el->pseudo[0]->text_transform);
    measure_text_item(&it, el->pseudo[0], lc);
    items->push_back(it);
  }
  for (Node* c : el->children) {
    if (c->type == N_TEXT) {
      push_text_items(c->data, cs, c, items, lc, prehere);
      continue;
    }
    if (c->type != N_ELEMENT || !c->cs) continue;
    ComputedStyle* ccs = c->cs;
    if (node_invisible(ccs)) continue;
    if (c->tag == "br") {
      InlineItem br;
      br.kind = InlineItem::I_BR;
      br.node = c;
      items->push_back(br);
      continue;
    }
    if (ccs->position == P_ABSOLUTE || ccs->position == P_FIXED) continue;
    if (is_blocky(ccs->display)) {
      /* block child inside inline flow: approximate as its inline content */
      flatten_element(c, ccs, items, lc, prehere);
      continue;
    }
    if (is_atomic_inline(ccs->display) || c->tag == "img" || c->tag == "svg" || c->tag == "video" ||
        c->tag == "canvas" || c->tag == "iframe") {
      InlineItem it;
      it.kind = InlineItem::I_REPLACED;
      it.node = c;
      items->push_back(it);
      continue;
    }
    InlineItem st;
    st.kind = InlineItem::I_EL_START;
    st.node = c;
    st.box = new_box(c, nullptr);
    items->push_back(st);
    flatten_element(c, ccs, items, lc, prehere);
    InlineItem en;
    en.kind = InlineItem::I_EL_END;
    en.node = c;
    en.box = st.box;
    items->push_back(en);
  }
  if (el->pseudo[1] && el->pseudo[1]->content_set) {
    InlineItem it;
    it.kind = InlineItem::I_TEXT;
    it.node = el;
    it.text = transform_text(el->pseudo[1]->content, el->pseudo[1]->text_transform);
    measure_text_item(&it, el->pseudo[1], lc);
    items->push_back(it);
  }
}

static void flatten_nodes(const std::vector<Node*>& nodes, const ComputedStyle* cs,
                          std::vector<InlineItem>* items, LayoutCtx& lc, bool pre) {
  for (Node* n : nodes) {
    if (n->type == N_TEXT) {
      push_text_items(n->data, cs, n, items, lc, pre);
      continue;
    }
    if (n->type != N_ELEMENT || !n->cs) continue;
    ComputedStyle* ncs = n->cs;
    if (node_invisible(ncs)) continue;
    if (n->tag == "br") {
      InlineItem br;
      br.kind = InlineItem::I_BR;
      br.node = n;
      items->push_back(br);
      continue;
    }
    if (ncs->position == P_ABSOLUTE || ncs->position == P_FIXED) continue;
    if (is_blocky(ncs->display)) {
      flatten_element(n, ncs, items, lc, pre);
      continue;
    }
    if (is_atomic_inline(ncs->display) || n->tag == "img" || n->tag == "svg" || n->tag == "video" ||
        n->tag == "canvas" || n->tag == "iframe") {
      InlineItem it;
      it.kind = InlineItem::I_REPLACED;
      it.node = n;
      items->push_back(it);
      continue;
    }
    flatten_element(n, ncs, items, lc, pre);
  }
}

/* ============================ replaced layout (in inline flow) ============================ */
static float layout_replaced_item(InlineItem* it, LayoutCtx& lc, float avail_w) {
  Node* el = it->node;
  ComputedStyle* cs = el->cs;
  float w, h;
  replaced_intrinsic(el, lc, &w, &h);
  float cw = res_l(cs->width, avail_w, cs->font_size, lc);
  float chh = res_l(cs->height, avail_w, cs->font_size, lc);
  bool real_replaced = el->tag == "img" || el->tag == "svg" || el->tag == "video" ||
                       el->tag == "canvas" || el->tag == "iframe" || el->tag == "input" ||
                       el->tag == "picture" || el->tag == "embed" || el->tag == "object";
  if (!real_replaced && cs->width.unit == L_AUTO) {
    /* non-replaced inline-block: shrink-to-fit = clamp(min-content, avail, max-content)
     * (both measures already include horizontal padding/border) */
    float maxw = measure_max_content_width(el, lc);
    float minw = measure_min_content_width(el, lc);
    w = std::min(maxw, std::max(0.f, avail_w));
    w = std::max(w, minw);
  } else if (cs->width.unit != L_AUTO) {
    w = cw;
  }
  if (cs->height.unit != L_AUTO) h = chh;
  if (cs->width.unit != L_AUTO && cs->height.unit == L_AUTO && cs->aspect_ratio > 0) h = w / cs->aspect_ratio;
  if (cs->height.unit != L_AUTO && cs->width.unit == L_AUTO && cs->aspect_ratio > 0) w = h * cs->aspect_ratio;
  if (cs->max_w.unit != L_AUTO && cs->max_w.unit != L_NORMAL)
    w = std::min(w, res_l(cs->max_w, avail_w, cs->font_size, lc));
  if (cs->max_h.unit != L_AUTO && cs->max_h.unit != L_NORMAL)
    h = std::min(h, res_l(cs->max_h, avail_w, cs->font_size, lc));
  if (is_atomic_inline(cs->display)) {
    Box* sub = new_box(el, nullptr);
    it->box = sub;
    resolve_box_edges(sub, cs, avail_w, lc);
    sub->w = w;
    sub->cw = std::max(0.f, w - sub->pad_l - sub->pad_r - sub->bor_l - sub->bor_r);
    float content_h = layout_element(sub, el, lc);
    h = content_h + sub->pad_t + sub->pad_b + sub->bor_t + sub->bor_b;
    float asc = 0;
    if (!sub->frags.empty()) {
      InlineFrag& last = sub->frags.back();
      asc = last.kind == InlineFrag::F_TEXT ? (last.run.y - sub->y) : (last.rect.y - sub->y + last.ascent);
    } else if (!sub->kids.empty()) {
      Box* last = sub->kids.back();
      asc = last->y - sub->y + last->last_line_baseline;
    } else {
      void* f = font_for(cs);
      asc = f ? font_ascent(f) : cs->font_size * 0.8f;
    }
    it->baseline = sub->pad_t + sub->bor_t + (asc > 0 ? asc : 0);
  } else {
    void* f = font_for(cs);
    it->baseline = f ? font_ascent(f) : cs->font_size * 0.8f;
  }
  it->h = h;
  it->adv = w;
  return w;
}

/* ============================ inline layout core ============================ */
static void emit_line(Box* container, std::vector<InlineItem>& seq, size_t start, size_t end,
                      float x_start, float line_w, float ascent, float descent, float y_line_top,
                      LayoutCtx& lc) {
  (void)line_w;
  float x = x_start;
  struct OpenEl {
    Box* box;
    float x0;
  };
  std::vector<OpenEl> open;
  for (size_t k = start; k < end; k++) {
    InlineItem* it = &seq[k];
    switch (it->kind) {
      case InlineItem::I_TEXT: {
        TextRun run = it->run;
        run.x = x;
        run.y = y_line_top + it->ascent;
        InlineFrag f;
        f.kind = InlineFrag::F_TEXT;
        f.node = it->node;
        f.run = run;
        f.ascent = it->ascent;
        f.descent = it->descent;
        f.rect = {x, y_line_top, it->adv, it->ascent + it->descent};
        container->frags.push_back(f);
        x += it->adv;
        break;
      }
      case InlineItem::I_EL_START: {
        if (!it->box) it->box = new_box(it->node, nullptr);
        open.push_back({it->box, x});
        break;
      }
      case InlineItem::I_EL_END: {
        Box* bx = it->box;
        for (int j = (int)open.size() - 1; j >= 0; j--) {
          if (open[j].box == bx) {
            ComputedStyle* ics = bx->node ? bx->node->cs : nullptr;
            float ml = 0, mr = 0;
            if (ics) {
              ml = res_l(ics->margin[3], 0, ics->font_size, lc);
              mr = res_l(ics->margin[1], 0, ics->font_size, lc);
            }
            InlineFrag f;
            f.kind = InlineFrag::F_BOX;
            f.node = bx->node;
            f.box = bx;
            f.rect = {open[j].x0 - ml, y_line_top, x - open[j].x0 + ml + mr, ascent + descent};
            f.ascent = ascent;
            f.descent = descent;
            container->frags.push_back(f);
            open.erase(open.begin() + j);
            break;
          }
        }
        break;
      }
      case InlineItem::I_REPLACED: {
        InlineFrag f;
        f.kind = it->node->tag == "svg" ? InlineFrag::F_REPLACED_SVG : InlineFrag::F_REPLACED_IMG;
        f.node = it->node;
        f.box = it->box;
        float bw = it->adv;
        float bh = it->h;
        f.rect = {x, (y_line_top + ascent) - it->baseline, bw, bh};
        f.ascent = it->baseline;
        f.descent = bh - it->baseline;
        container->frags.push_back(f);
        x += bw;
        break;
      }
      case InlineItem::I_BR:
      case InlineItem::I_SOFTBREAK:
        break;
    }
  }
  for (auto& o : open) {
    InlineFrag f;
    f.kind = InlineFrag::F_BOX;
    f.node = o.box->node;
    f.box = o.box;
    f.rect = {o.x0, y_line_top, x - o.x0, ascent + descent};
    f.ascent = ascent;
    f.descent = descent;
    container->frags.push_back(f);
  }
}

static float layout_inline_seq(Box* container, std::vector<InlineItem>& seq, LayoutCtx& lc,
                               float x_origin, float y_origin) {
  ComputedStyle* cs = container->cs;
  container->frags.clear();
  if (getenv("SQRL_SEQ_DEBUG")) {
    fprintf(stderr, "[seq] container w=%.0f items=%zu kinds=", container->cw, seq.size());
    for (auto& q : seq) fprintf(stderr, "%d,", (int)q.kind);
    fprintf(stderr, "\n");
  }
  if (seq.empty()) {
    container->ch = 0;
    return 0;
  }
  void* strut_font = font_for(cs);
  float strut_asc, strut_desc;
  float strut_lh = line_height_px(cs, strut_font, lc, &strut_asc, &strut_desc);
  float strut_half = (strut_lh - (strut_asc + strut_desc)) / 2;
  strut_asc += strut_half;
  strut_desc += strut_half;

  float avail = container->cw;
  float x = 0;
  float y = 0;
  float line_asc = strut_asc, line_desc = strut_desc;
  size_t line_start = 0;
  size_t line_end = 0;
  float line_w = 0;
  bool line_has_content = false;
  float last_baseline_off = 0;
  float max_w = 0;

  auto flush_line = [&](bool final_line) {
    if (line_has_content) {
      float free_w = avail - line_w;
      float x_start = 0;
      if (cs->text_align == TA_CENTER) x_start = std::max(0.f, free_w / 2);
      else if (cs->text_align == TA_END) x_start = std::max(0.f, free_w);
      emit_line(container, seq, line_start, line_end, x_origin + x_start, line_w, line_asc,
                line_desc, y_origin + y, lc);
      last_baseline_off = y + line_asc;
      y += line_asc + line_desc;
      max_w = std::max(max_w, line_w);
    }
    line_start = 0;
    line_end = 0;
    line_w = 0;
    line_asc = strut_asc;
    line_desc = strut_desc;
    line_has_content = false;
    x = 0;
    (void)final_line;
  };

  bool pre_mode = cs->white_space == WS_PRE || cs->white_space == WS_PRE_WRAP ||
                  cs->white_space == WS_PRE_LINE;
  bool nowrap = cs->white_space == WS_NOWRAP;

  for (size_t i = 0; i < seq.size(); i++) {
    InlineItem* it = &seq[i];
    if (!line_has_content) line_start = i;
    if (it->kind == InlineItem::I_BR || (it->kind == InlineItem::I_SOFTBREAK && pre_mode)) {
      line_end = i;
      flush_line(false);
      continue;
    }
    if (it->kind == InlineItem::I_TEXT && it->whitespace) {
      if (!line_has_content) continue;
      float space_w = it->adv;
      if (!nowrap && x + space_w > avail + 0.5f) {
        line_end = i;
        flush_line(false);
        continue;
      }
      line_w += space_w;
      x += space_w;
      line_end = i + 1;
      line_asc = std::max(line_asc, it->ascent);
      line_desc = std::max(line_desc, it->descent);
      continue;
    }
    if (it->kind == InlineItem::I_EL_START || it->kind == InlineItem::I_EL_END) continue;
    float item_w = it->adv;
    float item_asc = it->ascent, item_desc = it->descent;
    if (it->kind == InlineItem::I_REPLACED) {
      item_w = layout_replaced_item(it, lc, std::max(40.f, avail));
      item_asc = it->baseline;
      item_desc = it->h - it->baseline;
    }
    bool fits = nowrap || x + item_w <= avail + 0.5f || !line_has_content;
    if (!fits) {
      line_end = i;
      flush_line(false);
      line_start = i;
    }
    line_has_content = true;
    line_end = i + 1;
    line_w += item_w;
    x += item_w;
    line_asc = std::max(line_asc, item_asc);
    line_desc = std::max(line_desc, item_desc);
  }
  flush_line(true);
  container->last_line_baseline = y_origin + last_baseline_off;
  container->ch = y;
  container->w = std::max(container->w, max_w);
  return container->ch;
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

static float measure_max_content_width(Node* el, LayoutCtx& lc) {
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

static float measure_min_content_width(Node* el, LayoutCtx& lc) {
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
static void resolve_box_edges(Box* b, const ComputedStyle* cs, float cw_basis, LayoutCtx& lc) {
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

/* ============================ block layout ============================ */
static float layout_block_content(Box* b, Node* el, LayoutCtx& lc) {
  ComputedStyle* cs = b->cs;
  float avail_w = b->cw;
  float cur_y = 0;
  float prev_margin = 0;
  float max_w = 0;
  float x0 = b->x + b->pad_l + b->bor_l;
  float y0 = b->y + b->pad_t + b->bor_t;
  float first_margin = 0;
  bool first_done = false;

  std::vector<Node*> inline_group;
  auto flush_inline_group = [&]() {
    if (inline_group.empty()) return;
    /* skip pure-whitespace groups */
    bool any_content = false;
    for (Node* n : inline_group) {
      if (n->type == N_TEXT) {
        for (char ch : n->data)
          if (!isspace((unsigned char)ch)) { any_content = true; break; }
      } else any_content = true;
      if (any_content) break;
    }
    if (any_content) {
      Box* anon = new_box(nullptr, b);
      anon->cs = cs;
      anon->x = x0;
      anon->y = y0 + cur_y + (first_done ? 0 : first_margin);
      anon->w = avail_w;
      anon->cw = avail_w;
      std::vector<InlineItem> seq;
      flatten_nodes(inline_group, cs, &seq, lc, cs->white_space == WS_PRE);
      float h = layout_inline_seq(anon, seq, lc, anon->x, anon->y);
      anon->h = h;
      b->kids.push_back(anon);
      cur_y += h + (first_done ? 0 : first_margin);
      first_done = true;
      first_margin = 0;
      max_w = std::max(max_w, avail_w);
    }
    inline_group.clear();
  };

  for (Node* c : el->children) {
    if (c->type == N_TEXT) {
      if (c->data.empty()) continue;
      inline_group.push_back(c);
      continue;
    }
    if (c->type != N_ELEMENT || !c->cs) continue;
    ComputedStyle* ccs = c->cs;
    if (node_invisible(ccs)) continue;
    if (ccs->position == P_ABSOLUTE || ccs->position == P_FIXED) continue;
    if (c->tag == "br") {
      inline_group.push_back(c);
      continue;
    }
    bool atomic_non_media = is_atomic_inline(ccs->display) && c->tag != "img" && c->tag != "svg" &&
                            c->tag != "video" && c->tag != "canvas" && c->tag != "iframe";
    if (is_blocky(ccs->display) || atomic_non_media) {
      /* inline-flex/-block/-grid containers in block flow are laid out as
       * block boxes: the inline replaced-item path corrupts the heap when
       * they have element children (e.g. a ticker track with spans) */
      flush_inline_group();
      /* margins */
      float em = ccs->font_size;
      float mt = res_l(ccs->margin[0], avail_w, em, lc);
      float mb = res_l(ccs->margin[2], avail_w, em, lc);
      float ml = res_l(ccs->margin[3], avail_w, em, lc);
      float mr = res_l(ccs->margin[1], avail_w, em, lc);
      bool ml_auto = ccs->margin[3].unit == L_AUTO;
      bool mr_auto = ccs->margin[1].unit == L_AUTO;

      Box* cb = new_box(c, b);
      b->kids.push_back(cb);
      resolve_box_edges(cb, ccs, avail_w, lc);
      float declared_w = avail_w;
      bool w_measured = false; /* max/min-content measures already include padding */
      if (ccs->width.unit != L_AUTO && ccs->width.unit != L_NORMAL) {
        declared_w = res_l(ccs->width, avail_w, em, lc);
        declared_w += cb->pad_l + cb->pad_r + cb->bor_l + cb->bor_r;
      } else if (ccs->width_content_mode == 1) {
        declared_w = measure_max_content_width(c, lc);
        w_measured = true;
      } else if (ccs->width_content_mode == 2) {
        declared_w = measure_min_content_width(c, lc);
        w_measured = true;
      } else if (is_atomic_inline(ccs->display)) {
        declared_w = measure_max_content_width(c, lc);
        w_measured = true;
      }
      if (ccs->max_w.unit != L_AUTO && ccs->max_w.unit != L_NORMAL)
        declared_w = std::min(declared_w, res_l(ccs->max_w, avail_w, em, lc));
      if (ccs->min_w.unit != L_AUTO && ccs->min_w.unit != L_NORMAL)
        declared_w = std::max(declared_w, res_l(ccs->min_w, avail_w, em, lc));
      if (declared_w > avail_w) declared_w = avail_w;

      if (ml_auto && mr_auto) {
        ml = std::max(0.f, (avail_w - declared_w) / 2);
        mr = std::max(0.f, avail_w - declared_w - ml);
      } else if (ml_auto) {
        ml = std::max(0.f, avail_w - declared_w - mr);
      } else if (mr_auto) {
        mr = std::max(0.f, avail_w - declared_w - ml);
      }

      float top_gap = std::max(prev_margin, mt);
      if (!first_done) first_margin = top_gap;

      cb->w = declared_w;
      cb->cw = declared_w - cb->pad_l - cb->pad_r - cb->bor_l - cb->bor_r;
      cb->cw = std::max(0.f, cb->cw);
      cb->x = x0 + ml;
      cb->y = y0 + cur_y + (first_done ? top_gap : 0);
      if (!first_done) cb->y = y0;
      float ch = layout_element(cb, c, lc);
      float total_h = ch;
      if (ccs->height.unit != L_AUTO && ccs->height.unit != L_NORMAL)
        total_h = res_l(ccs->height, avail_w, em, lc);
      if (ccs->min_h.unit != L_AUTO && ccs->min_h.unit != L_NORMAL)
        total_h = std::max(total_h, res_l(ccs->min_h, avail_w, em, lc));
      if (ccs->max_h.unit != L_AUTO && ccs->max_h.unit != L_NORMAL)
        total_h = std::min(total_h, res_l(ccs->max_h, avail_w, em, lc));
      cb->h = total_h;

      /* relative offset */
      if (ccs->position == P_RELATIVE || ccs->position == P_STICKY) {
        float dx = 0, dy = 0;
        if (ccs->left.unit != L_AUTO) dx = res_l(ccs->left, avail_w, em, lc);
        else if (ccs->right.unit != L_AUTO) dx = -res_l(ccs->right, avail_w, em, lc);
        if (ccs->top.unit != L_AUTO) dy = res_l(ccs->top, avail_w, em, lc);
        else if (ccs->bottom.unit != L_AUTO) dy = -res_l(ccs->bottom, avail_w, em, lc);
        if (dx != 0 || dy != 0) offset_box_recursive(cb, dx, dy);
      }

      if (first_done) cur_y += top_gap + total_h + mb;
      else {
        cur_y += total_h + mb;
        first_done = true;
      }
      prev_margin = mb;
      max_w = std::max(max_w, ml + declared_w + mr);
      continue;
    }
    inline_group.push_back(c);
  }
  flush_inline_group();

  if (cs->display == D_LIST_ITEM && cs->list_style != 0) {
    void* f = font_for(cs);
    b->marker_text = "•";
    b->marker_w = f ? font_measure_text(f, "•", 0) + 8 : 14;
  }
  float content_h = cur_y;
  b->last_line_baseline = y0 + content_h;
  (void)max_w;
  return content_h;
}

/* ============================ flex layout ============================ */
struct FlexItem {
  Node* node = nullptr;
  Box* box = nullptr;
  std::vector<Node*> anon;      /* anonymous text group nodes */
  bool is_anon = false;
  ComputedStyle* ics = nullptr;
  float m[4] = {0, 0, 0, 0};
  bool m_auto[4] = {false, false, false, false}; /* auto margins absorb free space */
  float padb[4] = {0, 0, 0, 0};
  float basis = 0;
  float min_content = 0;      /* outer min-content size (shrink floor) */
  float grow = 0, shrink = 1;
  int order = 0;
  float w = 0, h = 0;
};

static void flex_item_layout_at(FlexItem& it, float item_w, LayoutCtx& lc) {
  Box* bx = it.box;
  bx->x = 0;
  bx->y = 0;
  /* clamp by max-width / min-width */
  if (it.node && it.ics->max_w.unit != L_AUTO && it.ics->max_w.unit != L_NORMAL) {
    float mw = res_l(it.ics->max_w, item_w, it.ics->font_size, lc) + it.padb[3] + it.padb[1];
    item_w = std::min(item_w, mw);
  }
  if (it.node && it.ics->min_w.unit != L_AUTO && it.ics->min_w.unit != L_NORMAL) {
    float mw = res_l(it.ics->min_w, item_w, it.ics->font_size, lc) + it.padb[3] + it.padb[1];
    item_w = std::max(item_w, mw);
  }
  bx->w = item_w;
  bx->cw = std::max(0.f, item_w - it.padb[3] - it.padb[1]);
  if (it.node) {
    resolve_box_edges(bx, it.ics, item_w, lc);
    bx->cw = std::max(0.f, item_w - bx->pad_l - bx->pad_r - bx->bor_l - bx->bor_r);
    float ch = layout_element(bx, it.node, lc);
    float total = ch + bx->pad_t + bx->pad_b + bx->bor_t + bx->bor_b;
    if (it.ics->height.unit != L_AUTO && it.ics->height.unit != L_NORMAL)
      total = res_l(it.ics->height, item_w, it.ics->font_size, lc);
    if (it.ics->min_h.unit != L_AUTO && it.ics->min_h.unit != L_NORMAL)
      total = std::max(total, res_l(it.ics->min_h, item_w, it.ics->font_size, lc));
    if (it.ics->max_h.unit != L_AUTO && it.ics->max_h.unit != L_NORMAL)
      total = std::min(total, res_l(it.ics->max_h, item_w, it.ics->font_size, lc));
    bx->h = total;
  } else if (it.is_anon) {
    bx->cs = bx->parent ? bx->parent->cs : it.ics;
    std::vector<InlineItem> seq;
    flatten_nodes(it.anon, bx->cs, &seq, lc, false);
    bx->x = 0;
    bx->y = 0;
    float h = layout_inline_seq(bx, seq, lc, 0, 0);
    /* re-emit at final position: shift frags */
    for (auto& f : bx->frags) {
      if (f.kind == InlineFrag::F_TEXT) { f.run.x += bx->x; f.run.y += bx->y; f.rect.x += bx->x; f.rect.y += bx->y; }
      else { f.rect.x += bx->x; f.rect.y += bx->y; }
    }
    bx->h = h;
  }
  it.h = bx->h;
  it.w = item_w;
}

static float layout_flex_content(Box* b, Node* el, LayoutCtx& lc) {
  ComputedStyle* cs = b->cs;
  bool col = cs->flex_dir == FD_COL || cs->flex_dir == FD_COL_REV;
  bool rev = cs->flex_dir == FD_ROW_REV || cs->flex_dir == FD_COL_REV;
  float avail_w = b->cw;
  float avail_h = b->ch_definite; /* set by caller (0 if auto) */

  std::vector<FlexItem> items;
  std::vector<Node*> group;
  auto flush_group = [&]() {
    if (group.empty()) return;
    FlexItem it;
    it.anon = group;
    it.is_anon = true;
    it.box = new_box(nullptr, b);
    it.ics = cs;
    it.box->cs = cs;
    b->kids.push_back(it.box);
    items.push_back(it);
    group.clear();
  };
  for (Node* c : el->children) {
    if (c->type == N_TEXT) {
      bool only_ws = true;
      for (char ch : c->data)
        if (!isspace((unsigned char)ch)) { only_ws = false; break; }
      if (only_ws && group.empty()) continue;
      group.push_back(c);
      continue;
    }
    if (c->type != N_ELEMENT || node_invisible(c->cs)) continue;
    if (c->cs->position == P_ABSOLUTE || c->cs->position == P_FIXED) continue;
    if (c->tag == "br") {
      group.push_back(c);
      continue;
    }
    /* every element child of a flex container becomes a flex item (CSS blockification) */
    {
      flush_group();
      FlexItem it;
      it.node = c;
      it.ics = c->cs;
      it.box = new_box(c, b);
      b->kids.push_back(it.box);
      it.grow = c->cs->flex_grow;
      it.shrink = c->cs->flex_shrink;
      it.order = c->cs->order;
      float em = c->cs->font_size;
      for (int s = 0; s < 4; s++) {
        it.m_auto[s] = c->cs->margin[s].unit == L_AUTO;
        it.m[s] = it.m_auto[s] ? 0 : res_l(c->cs->margin[s], avail_w, em, lc);
        it.padb[s] = (c->cs->border_style[s] ? res_l(c->cs->border_width[s], avail_w, em, lc) : 0) +
                     res_l(c->cs->padding[s], avail_w, em, lc);
      }
      items.push_back(it);
      continue;
    }
    group.push_back(c);
  }
  flush_group();

  std::stable_sort(items.begin(), items.end(),
                   [](const FlexItem& a, const FlexItem& bb) { return a.order < bb.order; });
  if (rev) std::reverse(items.begin(), items.end());

  float gap_main = res_l(col ? cs->gap_row : cs->gap_col, avail_w, cs->font_size, lc);
  float gap_cross = res_l(col ? cs->gap_col : cs->gap_row, avail_w, cs->font_size, lc);

  /* main size basis (hypothetical, per CSS flexbox §9.2) */
  float main_avail = col ? (avail_h > 0 ? avail_h : 0) : avail_w;
  for (auto& it : items) {
    float em = it.ics->font_size;
    bool basis_set = false;
    if (it.ics->flex_basis.unit != L_AUTO && it.ics->flex_basis.unit != L_NORMAL) {
      it.basis = res_l(it.ics->flex_basis, col ? avail_h : avail_w, em, lc);
      basis_set = true;
    } else if (!col && it.ics->width.unit != L_AUTO && it.ics->width.unit != L_NORMAL) {
      it.basis = res_l(it.ics->width, avail_w, em, lc) + it.padb[3] + it.padb[1];
      basis_set = true;
    } else if (col && it.ics->height.unit != L_AUTO && it.ics->height.unit != L_NORMAL) {
      it.basis = res_l(it.ics->height, avail_h, em, lc) + it.padb[0] + it.padb[2];
      basis_set = true;
    }
    if (!basis_set) {
      if (it.node) {
        if (col) {
          /* need width to measure height: assume stretch width */
          float w = avail_w;
          resolve_box_edges(it.box, it.ics, w, lc);
          it.box->w = w;
          it.box->cw = std::max(0.f, w - it.box->pad_l - it.box->pad_r - it.box->bor_l - it.box->bor_r);
          it.box->x = 0;
          it.box->y = 0;
          float ch = layout_element(it.box, it.node, lc);
          it.basis = ch + it.box->pad_t + it.box->pad_b + it.box->bor_t + it.box->bor_b;
          it.w = w;
        } else {
          float mw = measure_max_content_width(it.node, lc);
          it.basis = mw;
        }
      } else {
        /* anonymous text group: measure max line width */
        float maxw = 0;
        std::vector<InlineItem> seq;
        flatten_nodes(it.anon, cs, &seq, lc, false);
        for (auto& q : seq) maxw += q.adv;
        it.basis = col ? 0 : maxw;
      }
    }
    if (it.basis < 0) it.basis = 0;
    /* clamp hypothetical size by min/max (outer) */
    if (!col) {
      if (it.ics->max_w.unit != L_AUTO && it.ics->max_w.unit != L_NORMAL)
        it.basis = std::min(it.basis, res_l(it.ics->max_w, avail_w, em, lc) + it.padb[3] + it.padb[1]);
      if (it.ics->min_w.unit != L_AUTO && it.ics->min_w.unit != L_NORMAL)
        it.basis = std::max(it.basis, res_l(it.ics->min_w, avail_w, em, lc) + it.padb[3] + it.padb[1]);
      /* shrink floor: min-content width (CSS min-width:auto default on flex items) */
      if (it.node)
        it.min_content =
            measure_min_content_width(it.node, lc) + it.padb[3] + it.padb[1];
      else {
        /* anon text: longest word */
        float maxw = 0;
        std::vector<InlineItem> seq;
        flatten_nodes(it.anon, cs, &seq, lc, false);
        for (auto& q : seq) maxw = std::max(maxw, q.adv);
        it.min_content = maxw;
      }
    }
  }

  /* layout each item at final main size */
  for (auto& it : items) {
    if (col) {
      float w = it.w > 0 ? it.w : avail_w;
      if (it.ics->width.unit != L_AUTO && it.ics->width.unit != L_NORMAL)
        w = res_l(it.ics->width, avail_w, it.ics->font_size, lc) + it.padb[3] + it.padb[1];
      if (it.node) flex_item_layout_at(it, w, lc);
      else {
        it.basis = 0;
        it.h = 0;
        it.w = w;
      }
    }
    /* row items are laid out after per-line flexing below */
  }

  /* cross sizes + placement */
  float content_h = 0;
  if (!col) {
    /* wrap into lines using hypothetical main sizes (before flexing) */
    struct Line {
      std::vector<FlexItem*> its;
      float cross = 0;
      float main_used = 0;
    };
    std::vector<Line> lines;
    {
      Line cur;
      float x = 0;
      bool can_wrap = cs->flex_wrap != FW_NOWRAP;
      for (auto& it : items) {
        float advance = it.basis + it.m[3] + it.m[1];
        if (can_wrap && !cur.its.empty() && x + advance > avail_w + 0.5f) {
          lines.push_back(cur);
          cur = Line();
          x = 0;
        }
        cur.its.push_back(&it);
        cur.main_used += advance;
        x += advance;
      }
      if (!cur.its.empty()) lines.push_back(cur);
    }
    float y = 0;
    for (auto& line : lines) {
      /* ---- per-line flexing (spec §9.7) ---- */
      float used = line.its.size() > 1 ? gap_main * (float)(line.its.size() - 1) : 0;
      float sum_grow = 0, sum_shrink = 0;
      for (auto* itp : line.its) {
        used += itp->basis + itp->m[3] + itp->m[1];
        sum_grow += itp->grow;
        sum_shrink += itp->shrink;
      }
      float free = avail_w - used;
      if (free > 0 && sum_grow > 0) {
        for (auto* itp : line.its)
          if (itp->grow > 0) itp->basis += free * itp->grow / sum_grow;
      } else if (free < 0 && sum_shrink > 0) {
        float scaled = free / sum_shrink;
        for (auto* itp : line.its) {
          if (itp->shrink <= 0) continue;
          float target = itp->basis + scaled * itp->shrink;
          itp->basis = std::max(itp->min_content, target);
        }
      }
      /* layout each item at final main size */
      for (auto* itp : line.its) flex_item_layout_at(*itp, itp->basis, lc);

      float line_cross = 0;
      for (auto* itp : line.its) line_cross = std::max(line_cross, itp->h + itp->m[0] + itp->m[2]);
      /* align-content stretch when wrap && container height definite */
      if (cs->flex_wrap != FW_NOWRAP && avail_h > 0 && lines.size() == 1 && cs->align_content == 0)
        line_cross = std::max(line_cross, avail_h);
      /* justify (free2 recomputed after flexing; grow fills the line so
       * free space is normally 0 for grown items). Auto margins absorb
       * free space with priority over justify-content (spec §9.5). */
      float used2 = 0;
      int n_auto = 0;
      for (auto* itp : line.its) {
        used2 += itp->basis + itp->m[3] + itp->m[1];
        if (itp->m_auto[3] || itp->m_auto[1]) n_auto++;
      }
      used2 += line.its.size() > 1 ? gap_main * (float)(line.its.size() - 1) : 0;
      float free2 = avail_w - used2;
      float lead = 0, between = 0;
      float auto_share = 0;
      if (free2 > 0 && n_auto > 0) {
        auto_share = free2 / (float)n_auto;
      } else {
        switch (cs->justify) {
          case J_START: break;
          case J_END: lead = std::max(0.f, free2); break;
          case J_CENTER: lead = std::max(0.f, free2) / 2; break;
          case J_BETWEEN: between = line.its.size() > 1 ? std::max(0.f, free2) / (float)(line.its.size() - 1) : 0; break;
          case J_AROUND: between = line.its.size() ? std::max(0.f, free2) / (float)line.its.size() : 0; lead = between / 2; break;
          case J_EVENLY: between = line.its.size() ? std::max(0.f, free2) / (float)(line.its.size() + 1) : 0; lead = between; break;
        }
      }
      float x = b->x + b->pad_l + b->bor_l + lead;
      bool dbg_flex = getenv("SQRL_DBG_FLEX") != nullptr;
      if (dbg_flex) {
        fprintf(stderr, "[flex] box x=%.0f avail_w=%.0f justify=%d n_auto=%d free2=%.1f items=%zu\n",
                b->x, avail_w, (int)cs->justify, n_auto, free2, line.its.size());
        for (auto* itp : line.its)
          fprintf(stderr, "[flex]   item basis=%.1f m3=%.1f m1=%.1f auto=%d%d grow=%.1f minc=%.1f node=%s\n",
                  itp->basis, itp->m[3], itp->m[1], (int)itp->m_auto[3], (int)itp->m_auto[1],
                  itp->grow, itp->min_content, itp->node ? itp->node->tag.c_str() : "(anon)");
      }
      bool first = true;
      for (auto* itp : line.its) {
        FlexItem& it = *itp;
        if (!first) x += between + gap_main;
        first = false;
        float m_start = it.m_auto[3] ? auto_share : it.m[3];
        float m_end = it.m_auto[1] ? auto_share : it.m[1];
        Align al = it.ics->align_self_auto ? cs->align : it.ics->align_self;
        float yoff = 0;
        if (al == A_STRETCH) {
          /* stretch to line height: anon groups reflow, node boxes stretch
           * visually (bg/border fill; content stays top-aligned) */
          float target_h = line_cross - it.m[0] - it.m[2];
          if (!it.node) it.h = target_h;
          else if (it.ics->height.unit == L_AUTO && it.h < target_h) {
            it.h = target_h;
            it.box->h = target_h;
          }
        } else if (al == A_CENTER) yoff = (line_cross - it.h - it.m[0] - it.m[2]) / 2;
        else if (al == A_END) yoff = line_cross - it.h - it.m[0] - it.m[2];
        float fx = x + m_start;
        float fy = b->y + b->pad_t + b->bor_t + y + it.m[0] + yoff;
        /* move subtree from trial origin (0,0) to final position */
        offset_box_recursive(it.box, fx - it.box->x, fy - it.box->y);
        it.box->w = it.w;
        it.box->h = it.h;
        x += it.basis + m_start + m_end;
      }
      y += line_cross + gap_cross;
    }
    content_h = std::max(0.f, y - gap_cross);
  } else {
    /* column: single line */
    float line_cross = avail_w;
    float used = 0;
    for (auto& it : items) used += it.basis + it.m[0] + it.m[2];
    used += items.size() > 1 ? gap_main * (float)(items.size() - 1) : 0;
    float free2 = (avail_h > 0 ? avail_h : used) - used;
    float lead = 0, between = 0;
    if (free2 > 0) {
      switch (cs->justify) {
        case J_START: break;
        case J_END: lead = free2; break;
        case J_CENTER: lead = free2 / 2; break;
        case J_BETWEEN: between = items.size() > 1 ? free2 / (float)(items.size() - 1) : 0; break;
        case J_AROUND: between = items.size() ? free2 / (float)items.size() : 0; lead = between / 2; break;
        case J_EVENLY: between = items.size() ? free2 / (float)(items.size() + 1) : 0; lead = between; break;
      }
    }
    float y = b->y + b->pad_t + b->bor_t + lead;
    bool first = true;
    for (auto& it : items) {
      if (!first) y += between + gap_main;
      first = false;
      Align al = it.ics->align_self_auto ? cs->align : it.ics->align_self;
      float xoff = 0;
      float w = it.w > 0 ? it.w : avail_w;
      if (al == A_STRETCH && it.node && it.ics->width.unit == L_AUTO) w = line_cross;
      else if (al == A_CENTER) xoff = (line_cross - w) / 2;
      else if (al == A_END) xoff = line_cross - w;
      if (w != it.w && it.node) flex_item_layout_at(it, w, lc);
      float fx = b->x + b->pad_l + b->bor_l + xoff + it.m[3];
      float fy = y + it.m[0];
      offset_box_recursive(it.box, fx - it.box->x, fy - it.box->y);
      it.box->w = it.w = w;
      it.box->h = it.h;
      y += it.basis + it.m[0] + it.m[2];
    }
    content_h = std::max(0.f, y - lead - b->y - b->pad_t - b->bor_t);
  }

  b->last_line_baseline = b->y + b->pad_t + b->bor_t + content_h;
  return content_h;
}

/* ============================ grid layout ============================ */
static float layout_grid_content(Box* b, Node* el, LayoutCtx& lc) {
  ComputedStyle* cs = b->cs;
  float avail_w = b->cw;
  std::vector<Track> cols = cs->grid_cols;
  if (cs->grid_auto_mode && !cs->grid_auto_template.empty()) {
    /* repeat(auto-fit|auto-fill, T): repeat as many min-sized tracks as fit */
    float minw = 0;
    for (auto& t : cs->grid_auto_template)
      if (t.type == TR_MINMAX && t.mn && t.mn->type == TR_PX) minw = std::max(minw, t.mn->v);
      else if (t.type == TR_PX) minw = std::max(minw, t.v);
    float col_gap0 = res_l(cs->gap_col, avail_w, cs->font_size, lc);
    int nfit = minw > 0
                   ? (int)std::floor((avail_w + col_gap0) / (minw + col_gap0))
                   : 1;
    if (nfit < 1) nfit = 1;
    cols.assign(nfit, cs->grid_auto_template[0]);
  }
  if (cols.empty()) cols.push_back(Track(TR_FR));
  struct GridItem {
    Node* node = nullptr;
    Box* box = nullptr;
    std::vector<Node*> anon;      /* anonymous text group nodes */
    bool is_anon = false;
    int col = 0, row = 0, cs_span = 1, rs_span = 1;
    float w = 0, h = 0;
  };
  std::vector<GridItem> items;
  std::vector<Node*> group;
  auto flush_group = [&]() {
    if (group.empty()) return;
    GridItem it;
    it.anon = group;
    it.is_anon = true;
    it.box = new_box(nullptr, b);
    it.box->cs = cs;
    b->kids.push_back(it.box);
    items.push_back(it);
    group.clear();
  };
  for (Node* c : el->children) {
    if (c->type == N_TEXT) {
      bool only_ws = true;
      for (char ch : c->data)
        if (!isspace((unsigned char)ch)) { only_ws = false; break; }
      if (only_ws && group.empty()) continue;
      group.push_back(c);
      continue;
    }
    if (c->type != N_ELEMENT || node_invisible(c->cs)) continue;
    if (c->cs->position == P_ABSOLUTE || c->cs->position == P_FIXED) continue;
    flush_group();
    GridItem it;
    it.node = c;
    it.box = new_box(c, b);
    b->kids.push_back(it.box);
    it.cs_span = std::max(1, c->cs->grid_col_span);
    it.rs_span = std::max(1, c->cs->grid_row_span);
    items.push_back(it);
  }
  flush_group();

  /* auto-fit: collapse tracks that hold no items (empty columns vanish) */
  if (cs->grid_auto_mode == 1 && (int)items.size() < (int)cols.size() && !items.empty()) {
    cols.resize(std::max<int>(1, (int)items.size()));
  }

  int ncols = (int)cols.size();
  int cur_col = 0, cur_row = 0;
  for (auto& it : items) {
    if (cur_col + it.cs_span > ncols) {
      cur_col = 0;
      cur_row++;
    }
    it.col = cur_col;
    it.row = cur_row;
    cur_col += it.cs_span;
    if (cur_col >= ncols) {
      cur_col = 0;
      cur_row++;
    }
  }
  int nrows = std::max(cur_row + 1, (int)cs->grid_rows.size());
  /* column sizes (minmax(a,b) resolves by its max track; min clamped after) */
  std::vector<float> colw(ncols, 0);
  std::vector<float> col_min(ncols, 0);
  float fr_total = 0;
  for (int i = 0; i < ncols; i++) {
    Track t = cols[i];
    if (t.type == TR_MINMAX) {
      if (t.mn && t.mn->type == TR_PX) col_min[i] = t.mn->v;
      if (t.mx) t = *t.mx;
    }
    if (t.type == TR_PX) colw[i] = t.v;
    else if (t.type == TR_PCT) colw[i] = t.v * avail_w / 100.f;
    else if (t.type == TR_FR) fr_total += std::max(0.01f, t.v);
    else {
      float mx = 0;
      for (auto& it : items) {
        if (it.node && it.col <= i && i < it.col + it.cs_span)
          mx = std::max(mx, measure_max_content_width(it.node, lc));
      }
      colw[i] = mx;
    }
  }
  float col_gap = res_l(cs->gap_col, avail_w, cs->font_size, lc);
  float row_gap = res_l(cs->gap_row, avail_w, cs->font_size, lc);
  float used_fixed = 0;
  for (int i = 0; i < ncols; i++) used_fixed += colw[i];
  used_fixed += col_gap * (float)std::max(0, ncols - 1);
  float rem_w = avail_w - used_fixed;
  if (fr_total > 0) {
    float frw = std::max(0.f, rem_w);
    for (int i = 0; i < ncols; i++) {
      Track t = cols[i];
      if (t.type == TR_MINMAX && t.mx) t = *t.mx;
      if (t.type == TR_FR) colw[i] = frw * t.v / fr_total;
    }
  }
  for (int i = 0; i < ncols; i++)
    if (col_min[i] > 0) colw[i] = std::max(colw[i], col_min[i]);
  /* layout items at column positions, measure row heights */
  std::vector<float> rowh(nrows, 0);
  for (auto& it : items) {
    float x0 = 0;
    for (int i = 0; i < it.col; i++) x0 += colw[i] + col_gap;
    float span_w = 0;
    for (int i = it.col; i < std::min(ncols, it.col + it.cs_span); i++) span_w += colw[i] + col_gap;
    span_w -= col_gap;
    span_w = std::max(0.f, span_w);
    if (it.node) {
      ComputedStyle* ics = it.node->cs;
      resolve_box_edges(it.box, ics, span_w, lc);
      it.box->w = span_w;
      it.box->cw = std::max(0.f, span_w - it.box->pad_l - it.box->pad_r - it.box->bor_l - it.box->bor_r);
      it.box->x = 0;
      it.box->y = 0;
      float ch = layout_element(it.box, it.node, lc);
      it.h = ch + it.box->pad_t + it.box->pad_b + it.box->bor_t + it.box->bor_b;
      if (ics->height.unit != L_AUTO && ics->height.unit != L_NORMAL)
        it.h = res_l(ics->height, span_w, ics->font_size, lc);
      it.w = span_w;
    } else if (it.is_anon) {
      std::vector<InlineItem> seq;
      flatten_nodes(it.anon, cs, &seq, lc, false);
      it.box->w = span_w;
      it.box->cw = span_w;
      float h = layout_inline_seq(it.box, seq, lc, 0, 0);
      it.h = h;
      it.w = span_w;
    }
    for (int rr = it.row; rr < std::min(nrows, it.row + it.rs_span); rr++)
      rowh[rr] = std::max(rowh[rr], it.h / (float)it.rs_span);
  }
  for (int i = 0; i < (int)cs->grid_rows.size() && i < nrows; i++) {
    Track& t = cs->grid_rows[i];
    if (t.type == TR_PX) rowh[i] = t.v;
    else if (t.type == TR_PCT) rowh[i] = t.v * avail_w / 100.f;
    else if (t.type == TR_FR) {
      /* distribute leftover */
    }
  }
  /* place */
  float y = 0;
  float content_h = 0;
  for (int r = 0; r < nrows; r++) {
    for (auto& it : items) {
      if (it.row != r) continue;
      float x0 = 0;
      for (int i = 0; i < it.col; i++) x0 += colw[i] + col_gap;
      float span_w = 0;
      for (int i = it.col; i < std::min(ncols, it.col + it.cs_span); i++) span_w += colw[i] + col_gap;
      span_w -= col_gap;
      float fx = b->x + b->pad_l + b->bor_l + x0;
      float fy = b->y + b->pad_t + b->bor_t + y;
      offset_box_recursive(it.box, fx - it.box->x, fy - it.box->y);
      it.box->w = span_w;
      it.box->h = it.h;
      if (it.node && it.node->cs->height.unit == L_AUTO) it.box->h = rowh[r];
    }
    y += rowh[r] + row_gap;
  }
  content_h = std::max(0.f, y - row_gap);
  b->last_line_baseline = b->y + b->pad_t + b->bor_t + content_h;
  return content_h;
}

/* ============================ element dispatch ============================ */
static float layout_element(Box* b, Node* el, LayoutCtx& lc) {
  ComputedStyle* cs = b->cs;
  float content_h = 0;
  /* definite height for percentage children */
  if (cs->height.unit != L_AUTO && cs->height.unit != L_NORMAL)
    b->ch_definite = res_l(cs->height, b->cw, cs->font_size, lc);
  else
    b->ch_definite = 0;
  /* re-layout safety: free + clear previous content (kids/out_of_flow are
   * owning pointers — clearing without freeing would leak whole subtrees) */
  for (auto* k : b->kids) free_box_tree(k);
  for (auto* o : b->out_of_flow) free_box_tree(o);
  b->kids.clear();
  b->out_of_flow.clear();
  b->frags.clear();
  if (el->tag == "img" || el->tag == "svg" || el->tag == "video" || el->tag == "canvas" ||
      el->tag == "iframe") {
    float w, h;
    replaced_intrinsic(el, lc, &w, &h);
    float iw = w > 1 ? w : 1, ih = h > 1 ? h : 1; /* intrinsic ratio basis */
    /* width/height ATTRS act as presentational hints (lowest priority) */
    const std::string* wattr = el->attr("width");
    const std::string* hattr = el->attr("height");
    float attr_w = wattr ? (float)atof(wattr->c_str()) : 0;
    float attr_h = hattr ? (float)atof(hattr->c_str()) : 0;
    bool attr_w_pct = wattr && wattr->find('%') != std::string::npos;
    if (cs->width.unit == L_AUTO && attr_w > 0)
      w = attr_w_pct ? attr_w / 100.f * b->cw : attr_w;
    if (cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL) w = res_l(cs->width, b->cw + b->pad_l + b->pad_r, cs->font_size, lc);
    bool h_from_attr = false;
    bool attr_h_definite = el->tag == "iframe" || el->tag == "embed" || el->tag == "object";
    if (attr_h_definite && cs->height.unit == L_AUTO && attr_h > 0 && !hattr->empty() &&
        hattr->find('%') == std::string::npos) {
      h = attr_h;
      h_from_attr = true;
    } else if (cs->height.unit != L_AUTO && cs->height.unit != L_NORMAL) {
      h = res_l(cs->height, b->cw, cs->font_size, lc);
    }
    if (cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL && !h_from_attr && cs->height.unit == L_AUTO)
      h = w * ih / iw; /* width set + height auto → keep intrinsic ratio */
    else if (cs->width.unit == L_AUTO && cs->height.unit != L_AUTO && cs->height.unit != L_NORMAL)
      w = h * iw / ih;
    if (cs->width.unit != L_AUTO && cs->height.unit == L_AUTO && cs->aspect_ratio > 0) h = w / cs->aspect_ratio;
    b->cw = w;
    b->w = w + b->pad_l + b->pad_r + b->bor_l + b->bor_r;
    content_h = h;
  } else if (cs->display == D_FLEX || cs->display == D_INLINE_FLEX) {
    content_h = layout_flex_content(b, el, lc);
  } else if (cs->display == D_GRID || cs->display == D_INLINE_GRID) {
    content_h = layout_grid_content(b, el, lc);
  } else if (cs->display == D_TABLE || cs->display == D_TABLE_ROW || cs->display == D_TABLE_CELL) {
    /* table approximation: treat as block */
    content_h = layout_block_content(b, el, lc);
  } else {
    content_h = layout_block_content(b, el, lc);
  }
  /* clamp by height props */
  float em = cs->font_size;
  float avail_w = b->cw;
  if (cs->height.unit != L_AUTO && cs->height.unit != L_NORMAL)
    content_h = res_l(cs->height, avail_w, em, lc);
  if (cs->min_h.unit != L_AUTO && cs->min_h.unit != L_NORMAL)
    content_h = std::max(content_h, res_l(cs->min_h, avail_w, em, lc));
  if (cs->max_h.unit != L_AUTO && cs->max_h.unit != L_NORMAL)
    content_h = std::min(content_h, res_l(cs->max_h, avail_w, em, lc));

  b->ch = content_h;
  b->h = content_h + b->pad_t + b->pad_b + b->bor_t + b->bor_b;
  /* animated translateX (ticker-style keyframes): shift by pct of own width */
  if (cs->anim_tx) {
    float m[6] = {1, 0, 0, 1, cs->anim_tx_pct / 100.f * b->w, 0};
    memcpy(cs->mat, m, sizeof(m));
    cs->has_transform = true;
  }
  layout_out_of_flow_children(b, el, lc);
  return b->h;
}

/* ============================ out of flow (absolute/fixed) ============================ */
static void layout_out_of_flow_one(Box* b, Node* node, ComputedStyle* ccs, LayoutCtx& lc,
                                   float cb_x, float cb_y, float cb_w, float cb_h) {
  {
    float em = ccs->font_size;
    Box* cb = new_box(node, b);
    cb->cs = ccs;
    b->out_of_flow.push_back(cb);
    /* resolve width */
    resolve_box_edges(cb, ccs, cb_w, lc);
    bool has_left = ccs->left.unit != L_AUTO;
    bool has_right = ccs->right.unit != L_AUTO;
    bool has_top = ccs->top.unit != L_AUTO;
    bool has_bottom = ccs->bottom.unit != L_AUTO;
    float w = 0;
    if (ccs->width.unit != L_AUTO && ccs->width.unit != L_NORMAL) {
      w = res_l(ccs->width, cb_w, em, lc);
    } else if (has_left && has_right) {
      w = cb_w - res_l(ccs->left, cb_w, em, lc) - res_l(ccs->right, cb_w, em, lc);
    } else {
      if (node) {
        w = measure_max_content_width(node, lc);
        w = std::min(w, cb_w);
      } else w = cb_w;
    }
    w += cb->pad_l + cb->pad_r + cb->bor_l + cb->bor_r;
    if (ccs->max_w.unit != L_AUTO && ccs->max_w.unit != L_NORMAL)
      w = std::min(w, res_l(ccs->max_w, cb_w, em, lc));
    if (ccs->min_w.unit != L_AUTO && ccs->min_w.unit != L_NORMAL)
      w = std::max(w, res_l(ccs->min_w, cb_w, em, lc));
    cb->w = w;
    cb->cw = std::max(0.f, w - cb->pad_l - cb->pad_r - cb->bor_l - cb->bor_r);
    cb->x = 0;
    cb->y = 0;
    /* pre-size height when both top & bottom are specified (inset-0 stretch) */
    float predef_h = -1;
    if (has_top && has_bottom) {
      float cbh = cb_h > 0 ? cb_h : (float)lc.vh;
      predef_h = cbh - res_l(ccs->top, cbh, em, lc) - res_l(ccs->bottom, cbh, em, lc);
      if (predef_h > 0) {
        cb->ch_definite = predef_h - cb->pad_t - cb->pad_b - cb->bor_t - cb->bor_b;
        if (cb->ch_definite < 0) cb->ch_definite = 0;
      }
    }
    float ch;
    if (node) {
      ch = layout_element(cb, node, lc);
    } else {
      /* pseudo-element box: no children, size comes from its own style */
      cb->ch = 0;
      ch = cb->pad_t + cb->pad_b + cb->bor_t + cb->bor_b;
      cb->h = ch;
    }
    if (predef_h > 0) {
      /* top+bottom both specified: the box stretches to the resolved height
       * (CSS: over-constrained abs positioning → height comes from offsets) */
      cb->h = predef_h;
      cb->ch = std::max(0.f, predef_h - cb->pad_t - cb->pad_b - cb->bor_t - cb->bor_b);
    }
    (void)ch;
    /* position */
    float ml = res_l(ccs->margin[3], cb_w, em, lc);
    float mt = res_l(ccs->margin[0], cb_w, em, lc);
    if (ccs->margin[3].unit == L_AUTO && ccs->margin[1].unit == L_AUTO) {
      ml = std::max(0.f, (cb_w - w) / 2);
    }
    float x = cb_x + ml;
    float y = cb_y + mt;
    if (has_left) x = cb_x + res_l(ccs->left, cb_w, em, lc) + ml;
    else if (has_right) x = cb_x + cb_w - w - res_l(ccs->right, cb_w, em, lc) - ml;
    float y_off = 0;
    if (has_top) y = cb_y + res_l(ccs->top, cb_h > 0 ? cb_h : (float)lc.vh, em, lc) + mt;
    else if (has_bottom) y = cb_y + (cb_h > 0 ? cb_h : (float)lc.vh) - cb->h - res_l(ccs->bottom, cb_h > 0 ? cb_h : (float)lc.vh, em, lc) - mt;
    if (ccs->position == P_FIXED) {
      x = res_l(ccs->left, (float)lc.vw, em, lc);
      if (!has_left && has_right) x = lc.vw - cb->w - res_l(ccs->right, (float)lc.vw, em, lc);
      y = res_l(ccs->top, (float)lc.vh, em, lc);
      if (!has_top && has_bottom) y = lc.vh - cb->h - res_l(ccs->bottom, (float)lc.vh, em, lc);
      y_off = 0;
      (void)y_off;
    }
    cb->x = x;
    cb->y = y;
  }
}

static void layout_out_of_flow_children(Box* b, Node* el, LayoutCtx& lc) {
  std::vector<Node*> abs_kids;
  for (Node* c : el->children) {
    if (c->type == N_ELEMENT && c->cs && !node_invisible(c->cs) &&
        (c->cs->position == P_ABSOLUTE || c->cs->position == P_FIXED))
      abs_kids.push_back(c);
  }
  float cb_x = b->x + b->pad_l + b->bor_l;
  float cb_y = b->y + b->pad_t + b->bor_t;
  float cb_w = b->cw;
  float cb_h = b->ch;
  for (Node* c : abs_kids) {
    layout_out_of_flow_one(b, c, c->cs, lc, cb_x, cb_y, cb_w, cb_h);
  }
  /* absolutely-positioned ::before/::after pseudo elements (e.g. the
   * animated sweep line inside buttons) become real out-of-flow boxes */
  for (int pe = 0; pe < 2; pe++) {
    ComputedStyle* pcs = el->pseudo[pe];
    if (!pcs || pcs->display == D_NONE) continue;
    if (pcs->position != P_ABSOLUTE && pcs->position != P_FIXED) continue;
    if (!pcs->content_set) continue;
    layout_out_of_flow_one(b, nullptr, pcs, lc, cb_x, cb_y, cb_w, cb_h);
  }
}

/* ============================ document entry ============================ */
Box* layout_document(Document* doc, int vw, int vh, ImageCacheView* images) {
  LayoutCtx lc;
  lc.doc = doc;
  lc.vw = vw;
  lc.vh = vh;
  lc.rem = doc->root_font_size;
  lc.images = images;
  if (!doc->body) return nullptr;
  Box* root = new_box(doc->body, nullptr);
  lc.root_box = root;
  /* body fills viewport width */
  ComputedStyle* bcs = doc->body->cs;
  float body_w = (float)vw;
  float ml = res_l(bcs->margin[3], body_w, bcs->font_size, lc);
  float mr = res_l(bcs->margin[1], body_w, bcs->font_size, lc);
  bool ml_auto = bcs->margin[3].unit == L_AUTO;
  bool mr_auto = bcs->margin[1].unit == L_AUTO;
  float body_declared = body_w;
  if (bcs->width.unit != L_AUTO && bcs->width.unit != L_NORMAL) {
    body_declared = res_l(bcs->width, body_w, bcs->font_size, lc);
    if (ml_auto && mr_auto) {
      ml = (body_w - body_declared) / 2;
      mr = body_w - body_declared - ml;
    }
  } else {
    body_declared = body_w - ml - mr;
  }
  resolve_box_edges(root, bcs, body_declared, lc);
  root->x = ml;
  root->y = res_l(bcs->margin[0], body_w, bcs->font_size, lc);
  root->w = body_declared + root->pad_l + root->pad_r + root->bor_l + root->bor_r;
  root->cw = body_declared;
  float ch = layout_element(root, doc->body, lc);
  (void)ch;
  return root;
}

/* page height accessor */
static void page_height_walk(Box* b, float* out) {
  if (!b) return;
  float bot = b->y + b->h;
  if (bot > *out) *out = bot;
  for (auto* k : b->kids) page_height_walk(k, out);
  for (auto* o : b->out_of_flow) page_height_walk(o, out); /* abs/fixed content counts */
}

float layout_page_height(Box* root) {
  if (!root) return 0;
  float h = 0;
  page_height_walk(root, &h);
  return h;
}

} /* namespace sqrl */
/* =====================================================================
 *  square_layout.cpp (part 2: paint traversal)
 * ===================================================================== */

namespace sqrl {

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

/* ============================ paint ============================ */
struct PaintCtx {
  Surface* s = nullptr;
  Rect clip;
  RoundedClip rc;
  float alpha = 1;
  LayoutCtx* lc = nullptr;
};

static void paint_box(Box* b, PaintCtx& ctx);
static void paint_frags(Box* container, PaintCtx& ctx);

/* blur a region (for backdrop-filter): box blur on RGBA copy */
static void backdrop_blur_region(Surface& s, const Rect& r, float radius_px) {
  gfx_blur_region(s, r, radius_px);
}

static void resolve_radius(const ComputedStyle* cs, float w, float h, LayoutCtx& lc, float out[4]) {
  out[0] = res_l(cs->radius[0], h, cs->font_size, lc);
  out[1] = res_l(cs->radius[1], h, cs->font_size, lc);
  out[2] = res_l(cs->radius[2], h, cs->font_size, lc);
  out[3] = res_l(cs->radius[3], h, cs->font_size, lc);
}

/* exposed blend helper for paint code */
static inline uint32_t blend_src_over_public(uint32_t d, uint32_t s) {
  uint32_t sa = (s >> 24) & 255;
  if (sa == 255) return s | 0xFF000000;
  if (sa == 0) return d;
  uint32_t da = (d >> 24) & 255;
  uint32_t isa = 255 - sa;
  uint32_t oa = sa + da * isa / 255;
  if (oa == 0) return 0;
  uint32_t sr = (s >> 16) & 255, sg = (s >> 8) & 255, sb = s & 255;
  uint32_t dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
  uint32_t orr = (sr * sa + dr * da * isa / 255) / oa;
  uint32_t ogg = (sg * sa + dg * da * isa / 255) / oa;
  uint32_t obb = (sb * sa + db * da * isa / 255) / oa;
  return (oa << 24) | (orr << 16) | (ogg << 8) | obb;
}

static void paint_frags(Box* container, PaintCtx& ctx) {
  LayoutCtx& lc = *ctx.lc;
  g_painted_frags += (int)container->frags.size();
  Surface& s = *ctx.s;
  for (auto& f : container->frags) {
    Rect r = f.rect;
    if (r.x + r.w < ctx.clip.x || r.x > ctx.clip.x + ctx.clip.w || r.y + r.h < ctx.clip.y ||
        r.y > ctx.clip.y + ctx.clip.h)
      continue;
    if (f.kind == InlineFrag::F_TEXT) {
      std::vector<GlyphPos> glyphs;
      if (f.run.font) {
        font_shape(f.run.font, f.run.text.c_str(), f.run.spacing, &glyphs);
        gfx_draw_glyphs(s, f.run.font, f.run.x, f.run.y, glyphs, f.run.color,
                        ctx.alpha * f.run.alpha, &ctx.clip);
        /* decorations */
        if (f.run.deco & 1) {
          Rect u = {f.run.x, f.run.y + f.run.size * 0.12f, font_measure_text(f.run.font, f.run.text.c_str(), f.run.spacing), std::max(1.f, f.run.size / 16.f)};
          gfx_fill_rect(s, u, f.run.color, nullptr, &ctx.clip, ctx.rc.active ? &ctx.rc : nullptr);
        }
        if (f.run.deco & 2) {
          Rect u = {f.run.x, f.run.y - f.run.size * 0.28f, font_measure_text(f.run.font, f.run.text.c_str(), f.run.spacing), std::max(1.f, f.run.size / 16.f)};
          gfx_fill_rect(s, u, f.run.color, nullptr, &ctx.clip, ctx.rc.active ? &ctx.rc : nullptr);
        }
      }
      continue;
    }
    if (f.kind == InlineFrag::F_BOX) {
      ComputedStyle* ics = f.box && f.box->node ? f.box->node->cs : nullptr;
      if (ics) {
        /* inline element segment background/border/decoration */
        float rad[4];
        resolve_radius(ics, r.w, r.h, lc, rad);
        float bw_t = ics->border_style[0] ? res_l(ics->border_width[0], r.w, ics->font_size, lc) : 0;
        float bw_b = ics->border_style[2] ? res_l(ics->border_width[2], r.w, ics->font_size, lc) : 0;
        float bw_l = ics->border_style[3] ? res_l(ics->border_width[3], r.w, ics->font_size, lc) : 0;
        float bw_r = ics->border_style[1] ? res_l(ics->border_width[1], r.w, ics->font_size, lc) : 0;
        if (ics->bg_color.a > 0)
          gfx_fill_rect(s, r, ics->bg_color, rad, &ctx.clip, ctx.rc.active ? &ctx.rc : nullptr);
        if (bw_t > 0) gfx_fill_rect(s, {r.x, r.y, r.w, bw_t}, ics->border_color[0], nullptr, &ctx.clip, nullptr);
        if (bw_b > 0) gfx_fill_rect(s, {r.x, r.y + r.h - bw_b, r.w, bw_b}, ics->border_color[2], nullptr, &ctx.clip, nullptr);
        if (bw_l > 0) gfx_fill_rect(s, {r.x, r.y, bw_l, r.h}, ics->border_color[3], nullptr, &ctx.clip, nullptr);
        if (bw_r > 0) gfx_fill_rect(s, {r.x + r.w - bw_r, r.y, bw_r, r.h}, ics->border_color[1], nullptr, &ctx.clip, nullptr);
        if (ics->deco & 1) {
          float uy = r.y + f.ascent + 2;
          gfx_fill_rect(s, {r.x, uy, r.w, std::max(1.f, ics->font_size / 16.f)}, ics->color, nullptr, &ctx.clip, nullptr);
        }
        if (ics->deco & 2) {
          float uy = r.y + f.ascent - ics->font_size * 0.28f;
          gfx_fill_rect(s, {r.x, uy, r.w, std::max(1.f, ics->font_size / 16.f)}, ics->color, nullptr, &ctx.clip, nullptr);
        }
      }
      continue;
    }
    if (f.kind == InlineFrag::F_REPLACED_IMG) {
      Node* n = f.node;
      ComputedStyle* ics = n->cs;
      const std::string* src = n->attr("src");
      ImageInfo ii = src && ctx.lc->images ? ctx.lc->images->find(*src) : ImageInfo{};
      engine_log(1, "[paint-img] src=%.50s decoded=%d id=%d rect=%.0f,%.0f %.0fx%.0f",
                 src ? src->c_str() : "", (int)ii.decoded, ii.gfx_id, r.x, r.y, r.w, r.h);
      if (ii.decoded && ii.gfx_id > 0) {
        uint8_t fit = ics ? (uint8_t)ics->object_fit : 0;
        float rad[4] = {0, 0, 0, 0};
        if (ics) resolve_radius(ics, r.w, r.h, lc, rad);
        RoundedClip own;
        if (rad[0] > 0 || rad[1] > 0 || rad[2] > 0 || rad[3] > 0) {
          own.active = true;
          own.bounds = r;
          memcpy(own.radius, rad, 4 * sizeof(float));
        }
        gfx_draw_image(s, ii.gfx_id, r, nullptr, fit, &ctx.clip, own.active ? &own : (ctx.rc.active ? &ctx.rc : nullptr), ctx.alpha);
      } else {
        /* placeholder */
        gfx_fill_rect(s, r, Color{40, 60, 80, 80}, nullptr, &ctx.clip, nullptr);
        if (n->cs && n->cs->font_size >= 10) {
          void* fnt = font_for(n->cs);
          const std::string* alt = n->attr("alt");
          if (fnt && alt && !alt->empty()) {
            std::vector<GlyphPos> glyphs;
            float w = font_shape(fnt, alt->c_str(), 0, &glyphs);
            (void)w;
            gfx_draw_glyphs(s, fnt, r.x, r.y + r.h / 2, glyphs, Color{255, 150, 160, 170}, 1, &ctx.clip);
          }
        }
      }
      continue;
    }
    if (f.kind == InlineFrag::F_REPLACED_SVG) {
      Node* n = f.node;
      std::string svg_text = serialize_html(n);
      ComputedStyle* ics = n->cs;
      Color cc = ics ? ics->color : Color{255, 0, 0, 0};
      Surface* sv = svg_render(svg_text, r.w, r.h, cc, 1);
      if (sv) {
        /* blit sv onto s at r */
        Rect region = {r.x, r.y, (float)std::min(sv->w, (int)r.w), (float)std::min(sv->h, (int)r.h)};
        for (int y = 0; y < (int)region.h; y++) {
          int sy = y;
          int dy = (int)r.y + y;
          if (dy < 0 || dy >= s.h) continue;
          uint32_t* dr = s.row32(dy);
          uint32_t* sr = sv->row32(sy);
          for (int x = 0; x < (int)region.w; x++) {
            int dx = (int)r.x + x;
            if (dx < 0 || dx >= s.w) continue;
            if (ctx.clip.contains(dx + 0.5f, dy + 0.5f)) {
              uint32_t c = sr[x];
              uint32_t sa = (c >> 24) & 255;
              if (!sa) continue;
              uint32_t a8 = (uint32_t)(sa * ctx.alpha);
              dr[dx] = blend_src_over_public(dr[dx], (a8 << 24) | (c & 0xFFFFFF));
            }
          }
        }
        sv->free_pix();
        delete sv;
      }
      continue;
    }
  }
}

static void paint_bg_layers(Box* b, PaintCtx& ctx, const Rect& pad_box, const float rad[4]) {
  LayoutCtx& lc = *ctx.lc;
  Surface& s = *ctx.s;
  ComputedStyle* cs = b->cs;
  RoundedClip own_rc;
  bool has_radius = rad[0] > 0 || rad[1] > 0 || rad[2] > 0 || rad[3] > 0;
  if (has_radius) {
    own_rc.active = true;
    own_rc.bounds = {b->x, b->y, b->w, b->h};
    memcpy(own_rc.radius, rad, 4 * sizeof(float));
  }
  const RoundedClip* rc = has_radius ? &own_rc : (ctx.rc.active ? &ctx.rc : nullptr);
  for (auto& layer : cs->bgs) {
    if (layer.kind == BG_LINEAR || layer.kind == BG_RADIAL) {
      gfx_fill_gradient(s, pad_box, layer.grad, &ctx.clip, rc);
    } else if (layer.kind == BG_URL) {
      ImageInfo ii = lc.images ? lc.images->find(layer.url) : ImageInfo{};
      if (!ii.decoded || ii.gfx_id <= 0) continue;
      /* size */
      float iw = (float)ii.w, ih = (float)ii.h;
      Rect dst = pad_box;
      uint8_t fit = 0;
      if (layer.size_kind == 1) fit = 1;
      else if (layer.size_kind == 2) fit = 2;
      else if (layer.size_kind == 3) {
        float sw = res_l(layer.size_w, pad_box.w, cs->font_size, lc);
        float sh = res_l(layer.size_h, pad_box.h, cs->font_size, lc);
        if (layer.size_w.unit == L_AUTO) sw = iw * (layer.size_h.unit == L_AUTO ? 1 : sh / ih);
        if (layer.size_h.unit == L_AUTO) sh = ih * (sw / iw);
        dst = {pad_box.x, pad_box.y, sw, sh};
        fit = 0;
      } else {
        /* auto: intrinsic */
        dst = {pad_box.x, pad_box.y, iw, ih};
      }
      /* position */
      float px = res_l(layer.pos_x, pad_box.w - dst.w, cs->font_size, lc);
      float py = res_l(layer.pos_y, pad_box.h - dst.h, cs->font_size, lc);
      dst.x += px;
      dst.y += py;
      if (layer.repeat) {
        /* tile across pad_box */
        float x0 = pad_box.x - dst.w * ceilf((pad_box.x - dst.x) / dst.w);
        float y0 = pad_box.y - dst.h * ceilf((pad_box.y - dst.y) / dst.h);
        for (float yy = y0; yy < pad_box.y + pad_box.h + dst.h; yy += dst.h) {
          for (float xx = x0; xx < pad_box.x + pad_box.w + dst.w; xx += dst.w) {
            Rect tile = {xx, yy, dst.w, dst.h};
            if (tile.x + tile.w < pad_box.x || tile.x > pad_box.x + pad_box.w ||
                tile.y + tile.h < pad_box.y || tile.y > pad_box.y + pad_box.h)
              continue;
            gfx_draw_image(s, ii.gfx_id, tile, nullptr, 0, &ctx.clip, rc, ctx.alpha);
            if (!layer.repeat) break;
            if (dst.w <= 0) break;
          }
          if (dst.h <= 0) break;
        }
      } else {
        gfx_draw_image(s, ii.gfx_id, dst, nullptr, fit, &ctx.clip, rc, ctx.alpha);
      }
    }
  }
}

static void paint_box(Box* b, PaintCtx& ctx) {
  ComputedStyle* cs = b->cs;
  if (!cs || cs->display == D_NONE) return;
  g_painted_boxes++;
  Surface& s = *ctx.s;
  LayoutCtx& lc = *ctx.lc;

  float rad[4];
  resolve_radius(cs, b->w, b->h, lc, rad);
  bool has_radius = rad[0] > 0 || rad[1] > 0 || rad[2] > 0 || rad[3] > 0;

  /* backdrop blur first (blur what's behind) */
  if (cs->backdrop_blur_px > 0 && !getenv("SQRL_NO_BLUR")) {
    Rect region = {b->x, b->y, b->w, b->h};
    backdrop_blur_region(s, region, cs->backdrop_blur_px);
  }

  /* transform / opacity layering */
  bool needs_layer = cs->opacity < 0.999f || cs->has_transform;
  Surface layer_s;
  PaintCtx layer_ctx = ctx;
  Rect layer_region;
  if (needs_layer) {
    /* paint into a full-page-size layer so absolute child coordinates stay
     * valid, then composite back (region-sized layers would need an origin
     * offset through every paint call). */
    layer_region = {0, 0, (float)s.w, (float)s.h};
    layer_s.alloc(s.w, s.h);
    layer_ctx.s = &layer_s;
    layer_ctx.clip = {0, 0, (float)layer_s.w, (float)layer_s.h};
    layer_ctx.rc = RoundedClip{};
    layer_ctx.alpha = 1;
  }
  PaintCtx& target = needs_layer ? layer_ctx : ctx;
  Surface& ts = *target.s;

  /* shadows */
  for (auto& sh : cs->shadows) if (getenv("SQRL_NO_SHADOW")) break; else {
    gfx_draw_shadow(ts, {b->x, b->y, b->w, b->h}, rad, sh, &target.clip);
  }

  /* border: paint as 4 strips (ring), NOT as a full-box fill — a full fill
   * would flood bordered-but-transparent elements (Tailwind preflight sets
   * border-color on *). Different colors per side are respected. */
  bool any_border = b->bor_t > 0 || b->bor_r > 0 || b->bor_b > 0 || b->bor_l > 0;
  if (any_border && has_radius) {
    /* rounded box: paint the border as a ring — fill the full rounded box
     * with the border color, the background fill below covers the inside */
    Color ring = cs->border_color[0].a > 0 ? cs->border_color[0] : cs->border_color[3];
    gfx_fill_rect(ts, {b->x, b->y, b->w, b->h}, ring, rad, &target.clip,
                  target.rc.active ? &target.rc : nullptr);
  } else if (any_border) {
    const RoundedClip* brc = target.rc.active ? &target.rc : nullptr;
    auto side_col = [&](int i) -> Color { return cs->border_color[i]; };
    if (b->bor_t > 0 && side_col(0).a > 0)
      gfx_fill_rect(ts, {b->x, b->y, b->w, b->bor_t}, side_col(0), nullptr, &target.clip, brc);
    if (b->bor_b > 0 && side_col(2).a > 0)
      gfx_fill_rect(ts, {b->x, b->y + b->h - b->bor_b, b->w, b->bor_b}, side_col(2), nullptr,
                    &target.clip, brc);
    if (b->bor_l > 0 && side_col(3).a > 0)
      gfx_fill_rect(ts, {b->x, b->y + b->bor_t, b->bor_l, std::max(0.f, b->h - b->bor_t - b->bor_b)},
                    side_col(3), nullptr, &target.clip, brc);
    if (b->bor_r > 0 && side_col(1).a > 0)
      gfx_fill_rect(ts, {b->x + b->w - b->bor_r, b->y + b->bor_t, b->bor_r,
                         std::max(0.f, b->h - b->bor_t - b->bor_b)},
                    side_col(1), nullptr, &target.clip, brc);
  }
  Rect pad_box = {b->x + b->bor_l, b->y + b->bor_t, std::max(0.f, b->w - b->bor_l - b->bor_r),
                  std::max(0.f, b->h - b->bor_t - b->bor_b)};
  float inner_rad[4];
  for (int i = 0; i < 4; i++) inner_rad[i] = std::max(0.f, rad[i] - std::max(b->bor_l, b->bor_t));
  if (cs->bg_color.a > 0) {
    gfx_fill_rect(ts, pad_box, cs->bg_color, inner_rad, &target.clip, target.rc.active ? &target.rc : nullptr);
  }
  paint_bg_layers(b, target, pad_box, inner_rad);

  /* replaced element content (block-level img / svg) */
  if (b->node && (b->node->tag == "img" || b->node->tag == "svg")) {
    Rect content = {b->x + b->bor_l, b->y + b->bor_t,
                    std::max(0.f, b->w - b->bor_l - b->bor_r),
                    std::max(0.f, b->h - b->bor_t - b->bor_b)};
    float rad_i[4];
    resolve_radius(cs, b->w, b->h, lc, rad_i);
    RoundedClip own_rc;
    bool has_r = rad_i[0] > 0 || rad_i[1] > 0 || rad_i[2] > 0 || rad_i[3] > 0;
    if (has_r) {
      own_rc.active = true;
      own_rc.bounds = {b->x, b->y, b->w, b->h};
      memcpy(own_rc.radius, rad_i, 4 * sizeof(float));
    }
    if (b->node->tag == "img") {
      const std::string* src = b->node->attr("src");
      ImageInfo ii = src && lc.images ? lc.images->find(*src) : ImageInfo{};
      engine_log(1, "[block-img] src=%.50s decoded=%d id=%d rect=%.0f,%.0f %.0fx%.0f",
                 src ? src->c_str() : "", (int)ii.decoded, ii.gfx_id, content.x, content.y,
                 content.w, content.h);
      if (ii.decoded && ii.gfx_id > 0) {
        gfx_draw_image(ts, ii.gfx_id, content, nullptr, (uint8_t)cs->object_fit,
                       &target.clip, has_r ? &own_rc : (target.rc.active ? &target.rc : nullptr),
                       target.alpha);
        g_painted_images++;
      } else {
        gfx_fill_rect(ts, content, Color{40, 50, 60, 255}, nullptr, &target.clip, nullptr);
      }
    } else if (b->node->tag == "svg") {
      std::string svg_text = serialize_html(b->node);
      Color cc = cs->color;
      Surface* sv = svg_render(svg_text, content.w, content.h, cc, 1);
      if (sv) {
        for (int y = 0; y < sv->h && (int)content.y + y < ts.h; y++) {
          int dy = (int)content.y + y;
          if (dy < 0) continue;
          uint32_t* dr = ts.row32(dy);
          uint32_t* sr = sv->row32(y);
          for (int x = 0; x < sv->w && (int)content.x + x < ts.w; x++) {
            int dx = (int)content.x + x;
            if (dx < 0) continue;
            if (!target.clip.contains(dx + 0.5f, dy + 0.5f)) continue;
            uint32_t c = sr[x];
            uint32_t sa = (c >> 24) & 255;
            if (!sa) continue;
            uint32_t a8 = (uint32_t)(sa * target.alpha);
            dr[dx] = blend_src_over_public(dr[dx], (a8 << 24) | (c & 0xFFFFFF));
          }
        }
        sv->free_pix();
        delete sv;
      }
    }
  }

  /* list marker */
  if (!b->marker_text.empty()) {
    void* f = font_for(cs);
    if (f) {
      std::vector<GlyphPos> glyphs;
      font_shape(f, b->marker_text.c_str(), 0, &glyphs);
      gfx_draw_glyphs(ts, f, b->x + b->pad_l + b->bor_l - b->marker_w + 2, b->y + b->pad_t + b->bor_t + font_ascent(f),
                      glyphs, cs->color, target.alpha, &target.clip);
    }
  }

  /* children clip */
  RoundedClip saved_rc = target.rc;
  Rect saved_clip = target.clip;
  bool clipped = cs->overflow_x == O_HIDDEN || cs->overflow_x == O_AUTO || cs->overflow_x == O_SCROLL ||
                 cs->overflow_x == O_CLIP || cs->overflow_y == O_HIDDEN || cs->overflow_y == O_AUTO ||
                 cs->overflow_y == O_SCROLL || cs->overflow_y == O_CLIP;
  if (clipped) {
    target.clip.x = std::max(target.clip.x, pad_box.x);
    target.clip.y = std::max(target.clip.y, pad_box.y);
    target.clip.w = std::min(target.clip.x + target.clip.w, pad_box.x + pad_box.w) - target.clip.x;
    target.clip.h = std::min(target.clip.y + target.clip.h, pad_box.y + pad_box.h) - target.clip.y;
    if (has_radius) {
      target.rc.active = true;
      target.rc.bounds = pad_box;
      memcpy(target.rc.radius, inner_rad, sizeof(inner_rad));
    }
  }

  /* frags first (inline content), then non-positioned block kids, then
   * positioned content (relative in-flow + absolute/fixed) in DOM order —
   * CSS paint order: positioned z-auto boxes paint above normal flow. */
  paint_frags(b, target);
  std::vector<Box*> positioned_kids;
  for (Box* k : b->kids) {
    if (k->node && k->node->cs &&
        (k->node->cs->position == P_RELATIVE || k->node->cs->position == P_STICKY) &&
        (k->node->cs->z_auto && k->node->cs->z_index >= 0)) {
      positioned_kids.push_back(k);
      continue;
    }
    paint_box(k, target);
  }

  /* out of flow children + positioned in-flow, z-sorted then DOM-ordered */
  {
    std::vector<Box*> neg, auto0, pos;
    for (Box* o : b->out_of_flow) {
      ComputedStyle* ocs = o->cs;
      if (!ocs || ocs->z_auto) auto0.push_back(o);
      else if (ocs->z_index < 0) neg.push_back(o);
      else pos.push_back(o);
    }
    for (Box* k : positioned_kids) auto0.push_back(k);
    auto zsort = [](std::vector<Box*>& v) {
      std::stable_sort(v.begin(), v.end(), [](const Box* a, const Box* bb) {
        int za = a->cs ? (a->cs->z_auto ? 0 : a->cs->z_index) : 0;
        int zb = bb->cs ? (bb->cs->z_auto ? 0 : bb->cs->z_index) : 0;
        if (za != zb) return za < zb;
        int ia = a->node ? a->node->child_index() : 0;
        int ib = bb->node ? bb->node->child_index() : 0;
        return ia < ib;
      });
    };
    zsort(neg);
    zsort(auto0);
    zsort(pos);
    for (Box* o : neg) paint_box(o, target);
    for (Box* o : auto0) paint_box(o, target);
    for (Box* o : pos) paint_box(o, target);
  }

  target.rc = saved_rc;
  target.clip = saved_clip;

  /* composite layer back */
  if (needs_layer) {
    if (cs->has_transform) {
      float ox = b->x + res_l(cs->tx_ox, b->w, cs->font_size, lc);
      float oy = b->y + res_l(cs->tx_oy, b->h, cs->font_size, lc);
      if (cs->tx_ox.unit == L_AUTO || cs->tx_ox.unit == L_NORMAL) ox = b->x + b->w / 2;
      if (cs->tx_oy.unit == L_AUTO || cs->tx_oy.unit == L_NORMAL) oy = b->y + b->h / 2;
      gfx_blit_matrix(s, layer_s, cs->mat, ox, oy, cs->opacity * ctx.alpha, &ctx.clip);
    } else {
      gfx_blit(s, layer_s, cs->opacity * ctx.alpha, &ctx.clip, ctx.rc.active ? &ctx.rc : nullptr);
    }
    layer_s.free_pix();
  }
}

static void dump_box_tree(Box* b, int depth, int max_depth) {
  if (!b || depth > max_depth) return;
  const char* tag = b->node ? b->node->tag.c_str() : "(anon)";
  fprintf(stderr, "%*s%s y=%.0f x=%.0f w=%.0f h=%.0f kids=%zu\n", depth * 2, "", tag,
          b->y, b->x, b->w, b->h, b->kids.size());
  for (size_t i = 0; i < b->kids.size() && i < 8; i++) dump_box_tree(b->kids[i], depth + 1, max_depth);
}

/* ============================ scrollbar ============================ */
/* shared geometry: returns effective scrollbar width (0 = hidden); fills
 * thumb position/height in track coords (track spans 0..viewport_h) */
float scrollbar_metrics(const Document* doc, float viewport_h, float page_h,
                        float scroll_y, float* thumb_y, float* thumb_h) {
  if (page_h <= viewport_h + 1) return 0; /* not scrollable */
  const ScrollbarStyle& sb = doc ? doc->sheet.sbstyle : ScrollbarStyle();
  float w = sb.width;
  if (w <= 0) return 0;
  float track_h = viewport_h;
  float th = std::max(30.f, track_h * viewport_h / page_h);
  if (th > track_h) th = track_h;
  float denom = page_h - viewport_h;
  float frac = denom > 1 ? scroll_y / denom : 0;
  frac = clampf(frac, 0.f, 1.f);
  if (thumb_y) *thumb_y = frac * (track_h - th);
  if (thumb_h) *thumb_h = th;
  return w;
}

static void draw_scrollbar(Surface& s, const Document* doc, Box* root, float scroll_y) {
  if (!root) return;
  float page_h = layout_page_height(root);
  float thumb_y = 0, thumb_h = 0;
  float w = scrollbar_metrics(doc, (float)s.h, page_h, scroll_y, &thumb_y, &thumb_h);
  if (w <= 0) return;
  const ScrollbarStyle& sb = doc->sheet.sbstyle;
  float x = s.w - w;
  /* track */
  gfx_fill_rect(s, {x, 0, w, (float)s.h}, sb.track, nullptr, nullptr, nullptr);
  /* thumb */
  float bor = sb.thumb_border;
  Color tc = sb.thumb;
  Rect th = {x + bor, thumb_y + bor, std::max(1.f, w - 2 * bor), std::max(1.f, thumb_h - 2 * bor)};
  float rad[4] = {sb.radius, sb.radius, sb.radius, sb.radius};
  gfx_fill_rect(s, th, tc, rad, nullptr, nullptr);
}

void paint_document(Document* doc, Box* root, Surface* s, ImageCacheView* images,
                    float scroll_y) {
  g_painted_boxes = g_painted_frags = g_painted_images = 0;
  if (getenv("SQRL_DUMP_TREE") && root) dump_box_tree(root, 0, 4);
  LayoutCtx lc;
  lc.doc = doc;
  lc.images = images;
  lc.rem = doc->root_font_size;
  lc.vw = s->w;
  lc.vh = s->h;
  lc.root_box = root;
  PaintCtx ctx;
  ctx.s = s;
  ctx.clip = {0, 0, (float)s->w, (float)s->h};
  ctx.rc = RoundedClip{};
  ctx.alpha = 1;
  ctx.lc = &lc;
  /* canvas background: html then body */
  Color canvas{255, 255, 255, 255};
  if (doc->html && doc->html->cs && doc->html->cs->bg_color.a > 0) canvas = doc->html->cs->bg_color;
  if (doc->body && doc->body->cs && doc->body->cs->bg_color.a > 0) canvas = doc->body->cs->bg_color;
  gfx_fill_rect(*s, {0, 0, (float)s->w, (float)s->h}, canvas, nullptr, nullptr, nullptr);
  /* scroll: shift the (read-only) box tree up while painting, restore after.
   * Boxes above the viewport are culled by the clip rect. */
  bool shifted = root && scroll_y != 0;
  if (shifted) offset_box_recursive(root, 0, -scroll_y);
  if (root) paint_box(root, ctx);
  if (shifted) offset_box_recursive(root, 0, scroll_y);
  draw_scrollbar(*s, doc, root, scroll_y);
}

} /* namespace sqrl */
