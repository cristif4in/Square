/* =====================================================================
 *  Square Browser Engine — square_layout_block.cpp
 *  Block layout, the layout_element() dispatch, out-of-flow (absolute /
 *  fixed) positioning and the document layout entry point.
 * ===================================================================== */
#include "square_layout_internal.h"

#include <algorithm>
#include <cctype>

namespace sqrl {

static void layout_out_of_flow_children(Box* b, Node* el, LayoutCtx& lc);

/* ============================ block layout ============================ */
float layout_block_content(Box* b, Node* el, LayoutCtx& lc) {
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
  (void)w_measured;
  return content_h;
}

/* ============================ element dispatch ============================ */
float layout_element(Box* b, Node* el, LayoutCtx& lc) {
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
