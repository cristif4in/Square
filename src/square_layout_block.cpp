/* Block layout, the layout_element() dispatch, out-of-flow (absolute /, fixed) positioning and the document layout entry point */
#include "square_layout_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace sqrl {

static void layout_out_of_flow_children(Box* b, Node* el, LayoutCtx& lc);

/* format an ordered-list marker: decimal / alpha / roman counters */
static std::string format_list_marker(int kind, int num) {
  char buf[32];
  if (num < 0) num = 0;
  if (kind == 4) {
    snprintf(buf, sizeof(buf), "%d.", num);
    return buf;
  }
  if (kind == 5 || kind == 6) {
    /* a..z, aa..zz (0 => 'z' wrap like browsers) */
    std::string s;
    int n = std::max(1, num);
    while (n > 0) {
      n--;
      char c = (char)('a' + (kind == 5 ? 0 : 'A' - 'a') + n % 26);
      s.insert(s.begin(), c);
      n /= 26;
    }
    return s + ".";
  }
  if (kind == 7 || kind == 8) {
    static const struct { int v; const char* lo; const char* up; } R[] = {
      {1000, "m", "M"}, {900, "cm", "CM"}, {500, "d", "D"}, {400, "cd", "CD"},
      {100, "c", "C"}, {90, "xc", "XC"}, {50, "l", "L"}, {40, "xl", "XL"},
      {10, "x", "X"}, {9, "ix", "IX"}, {5, "v", "V"}, {4, "iv", "IV"}, {1, "i", "I"}};
    std::string s;
    int n = std::max(1, num);
    for (auto& r : R)
      while (n >= r.v) { s += (kind == 7 ? r.lo : r.up); n -= r.v; }
    return s + ".";
  }
  return ".";
}

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
  struct FloatBox {
    int side = 0;
    float x = 0, y = 0, w = 0, h = 0;
  };
  std::vector<FloatBox> floats;
  auto float_band = [&](float y, float h, float* xl_out, float* w_out) {
    float xl = x0;
    float xr = x0 + avail_w;
    for (auto& fl : floats) {
      if (fl.y < y + h && fl.y + fl.h > y) {
        if (fl.side == 1) xl = std::max(xl, fl.x + fl.w);
        else xr = std::min(xr, fl.x);
      }
    }
    *xl_out = xl;
    *w_out = std::max(0.f, xr - xl);
  };
  auto clear_bottom = [&](int bits) {
    float bot = y0 + cur_y;
    for (auto& fl : floats) {
      if (bits == 3 || (bits == 1 && fl.side == 1) || (bits == 2 && fl.side == 2))
        bot = std::max(bot, fl.y + fl.h);
    }
    return bot;
  };
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
      float gy = y0 + cur_y + (first_done ? 0 : first_margin);
      anon->x = x0;
      anon->y = gy;
      anon->w = avail_w;
      anon->cw = avail_w;
      std::vector<InlineItem> seq;
      flatten_nodes(inline_group, cs, &seq, lc, cs->white_space == WS_PRE);
      float band_x = x0, band_w = avail_w;
      float h = 0;
      for (int iter = 0; iter < 5; iter++) {
        float bx_, bw_;
        float_band(gy, h > 0 ? h : 0.01f, &bx_, &bw_);
        if (iter > 0 && bx_ == band_x && bw_ == band_w) break;
        band_x = bx_;
        band_w = bw_;
        anon->x = band_x;
        anon->w = band_w;
        anon->cw = band_w;
        h = layout_inline_seq(anon, seq, lc, anon->x, anon->y);
      }
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
    if (ccs->css_float == 1 || ccs->css_float == 2) {
      flush_inline_group();
      float em = ccs->font_size;
      float mt = res_l(ccs->margin[0], avail_w, em, lc);
      float ml = res_l(ccs->margin[3], avail_w, em, lc);
      float mr = res_l(ccs->margin[1], avail_w, em, lc);
      if (ccs->clear) {
        float bot = clear_bottom(ccs->clear);
        if (bot > y0 + cur_y) cur_y = bot - y0;
      }
      Box* cb = new_box(c, b);
      b->kids.push_back(cb);
      resolve_box_edges(cb, ccs, avail_w, lc);
      float declared_w = avail_w;
      if (ccs->width.unit != L_AUTO && ccs->width.unit != L_NORMAL) {
        declared_w = res_l(ccs->width, avail_w, em, lc);
        declared_w += cb->pad_l + cb->pad_r + cb->bor_l + cb->bor_r;
      } else if (ccs->width_content_mode == 1) {
        declared_w = measure_max_content_width(c, lc);
      } else if (ccs->width_content_mode == 2) {
        declared_w = measure_min_content_width(c, lc);
      } else {
        declared_w = measure_max_content_width(c, lc);
      }
      if (ccs->max_w.unit != L_AUTO && ccs->max_w.unit != L_NORMAL)
        declared_w = std::min(declared_w, res_l(ccs->max_w, avail_w, em, lc));
      if (ccs->min_w.unit != L_AUTO && ccs->min_w.unit != L_NORMAL)
        declared_w = std::max(declared_w, res_l(ccs->min_w, avail_w, em, lc));
      if (declared_w > avail_w) declared_w = avail_w;
      cb->w = declared_w;
      cb->cw = std::max(0.f, declared_w - cb->pad_l - cb->pad_r - cb->bor_l - cb->bor_r);
      cb->x = 0;
      cb->y = 0;
      float ch = layout_element(cb, c, lc);
      float total_h = ch;
      if (ccs->height.unit == L_PCT && b->ch_definite > 0)
        total_h = ccs->height.val * b->ch_definite / 100.f;
      else if (ccs->height.unit != L_AUTO && ccs->height.unit != L_NORMAL)
        total_h = res_l(ccs->height, avail_w, em, lc);
      if (ccs->min_h.unit != L_AUTO && ccs->min_h.unit != L_NORMAL)
        total_h = std::max(total_h, res_pct_h(ccs->min_h, b->ch_definite, em, lc));
      if (ccs->max_h.unit != L_AUTO && ccs->max_h.unit != L_NORMAL)
        total_h = std::min(total_h, res_l(ccs->max_h, avail_w, em, lc));
      cb->h = total_h;
      int side = ccs->css_float;
      float fx = side == 1 ? x0 + ml : x0 + avail_w - declared_w - mr;
      float fy = y0 + cur_y + mt;
      bool moved = true;
      while (moved) {
        moved = false;
        for (auto& fl : floats) {
          if (fl.side == side && fy < fl.y + fl.h && fy + total_h > fl.y &&
              (side == 1 ? fx < fl.x + fl.w : fx + declared_w > fl.x)) {
            fy = fl.y + fl.h;
            moved = true;
          }
        }
      }
      offset_box_recursive(cb, fx - cb->x, fy - cb->y);
      floats.push_back({side, fx, fy, declared_w, total_h});
      continue;
    }
    bool leaf_control = c->tag == "input" || c->tag == "textarea" || c->tag == "select";
    if ((is_blocky(ccs->display) || atomic_non_media) && !leaf_control) {
      /* inline-flex/-block/-grid containers in block flow are laid out as
       * block boxes: the inline replaced-item path corrupts the heap when
       * they have element children (e.g. a ticker track with spans).
       * Leaf form controls (input/textarea/select) stay inline: they have no
       * element children and must flow with text. */
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

      if (ccs->clear) {
        float bot = clear_bottom(ccs->clear);
        if (bot > y0 + cur_y) cur_y = bot - y0;
      }
      /* shrink into the float-free band at this vertical position */
      float band_x = x0, band_w = avail_w;
      float child_y_page = !first_done ? y0 : y0 + cur_y + std::max(prev_margin, mt);
      float_band(child_y_page, 0.01f, &band_x, &band_w);
      if (declared_w > band_w) declared_w = band_w;

      if (ml_auto && mr_auto) {
        ml = std::max(0.f, (band_w - declared_w) / 2);
        mr = std::max(0.f, band_w - declared_w - ml);
      } else if (ml_auto) {
        ml = std::max(0.f, band_w - declared_w - mr);
      } else if (mr_auto) {
        mr = std::max(0.f, band_w - declared_w - ml);
      }

      float top_gap = std::max(prev_margin, mt);
      if (!first_done) first_margin = top_gap;

      cb->w = declared_w;
      cb->cw = declared_w - cb->pad_l - cb->pad_r - cb->bor_l - cb->bor_r;
      cb->cw = std::max(0.f, cb->cw);
      cb->x = band_x + ml;
      cb->y = y0 + cur_y + (first_done ? top_gap : 0);
      if (!first_done) cb->y = y0;
      float ch = layout_element(cb, c, lc);
      float total_h = ch;
      if (ccs->height.unit == L_PCT && b->ch_definite > 0)
        total_h = ccs->height.val * b->ch_definite / 100.f;
      else if (ccs->height.unit != L_AUTO && ccs->height.unit != L_NORMAL)
        total_h = res_l(ccs->height, avail_w, em, lc);
      if (ccs->min_h.unit != L_AUTO && ccs->min_h.unit != L_NORMAL)
        total_h = std::max(total_h, res_pct_h(ccs->min_h, b->ch_definite, em, lc));
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
  for (auto& fl : floats) cur_y = std::max(cur_y, fl.y + fl.h - y0);

  if (cs->display == D_LIST_ITEM && cs->list_style != 0) {
    void* f = font_for(cs);
    Node* n = b->node;
    /* ordinal among list-item siblings */
    int ord = 1;
    if (n && n->parent) {
      ord = 0;
      for (Node* sib : n->parent->children) {
        if (sib == n) break;
        if (sib->type == N_ELEMENT &&
            (sib->tag == "li" || (sib->cs && sib->cs->display == D_LIST_ITEM)))
          ord++;
      }
      ord++;
    }
    /* start / reversed from the owning list element */
    int start = 1;
    bool reversed = false;
    Node* lst = n ? n->parent : nullptr;
    if (lst && lst->type == N_ELEMENT) {
      if (const std::string* sv = lst->attr("start")) start = atoi(sv->c_str());
      if (lst->attr("reversed")) reversed = true;
      int num = reversed ? start - (ord - 1) : start + (ord - 1);
      b->marker_num = num;
      /* <ol type> presentational hint: overrides UA decimal, not author CSS */
      int kind = cs->list_kind;
      if (kind == 4) {
        if (const std::string* tv = lst->attr("type")) {
          char t = (*tv)[0];
          if (t == 'a') kind = 5;
          else if (t == 'A') kind = 6;
          else if (t == 'i') kind = 7;
          else if (t == 'I') kind = 8;
        }
      }
      b->marker_kind = kind <= 3 ? (uint8_t)kind : 0;
      if (kind <= 3) {
        b->marker_w = std::max(10.f, cs->font_size * 0.45f);
      } else {
        b->marker_text = format_list_marker(kind, num);
        b->marker_w = f ? font_measure_text(f, b->marker_text.c_str(), 0) + 8 : 14;
      }
    } else {
      /* detached / styled li without a list parent */
      /* detached / styled li without a list parent */
      int kind = cs->list_kind;
      b->marker_num = ord;
      b->marker_kind = kind <= 3 ? (uint8_t)kind : 0;
      if (kind <= 3) {
        b->marker_w = std::max(10.f, cs->font_size * 0.45f);
      } else {
        b->marker_text = format_list_marker(kind, ord);
        b->marker_w = f ? font_measure_text(f, b->marker_text.c_str(), 0) + 8 : 14;
      }
    }
  }
  float content_h = cur_y;
  b->last_line_baseline = y0 + content_h;
  (void)max_w;
  return content_h;
}

/* Balanced multi-column layout: children are measured at the column width,
 * assigned to columns sequentially (greedy fill of the balanced height),
 * then positioned. Inline runs are wrapped into anonymous boxes. */
static float layout_block_columns(Box* b, Node* el, LayoutCtx& lc) {
  ComputedStyle* cs = b->cs;
  float avail_w = b->cw;
  float em = cs->font_size;
  int ncols = std::max(1, cs->column_count);
  float gap = cs->column_gap.unit != L_AUTO && cs->column_gap.unit != L_NORMAL
                  ? res_l(cs->column_gap, avail_w, em, lc)
                  : 16.f;
  if (gap <= 0) gap = 0;
  float colw = (avail_w - gap * (float)(ncols - 1)) / (float)ncols;
  if (colw < 1) { colw = avail_w; ncols = 1; }
  float x0 = b->x + b->pad_l + b->bor_l;
  float y0 = b->y + b->pad_t + b->bor_t;

  /* partition children into items: elements or inline groups */
  struct ColItem {
    Node* el = nullptr;             /* element child (or null for inline group) */
    std::vector<Node*> group;       /* inline group nodes */
    Box* box = nullptr;
    float h = 0, mt = 0, mb = 0;
    int col = 0;
    float y = 0;
  };
  std::vector<ColItem> items;
  std::vector<Node*> group;
  auto flush_group = [&]() {
    bool any = false;
    for (Node* gn : group) {
      if (gn->type == N_TEXT) {
        for (char ch : gn->data)
          if (!isspace((unsigned char)ch)) { any = true; break; }
      } else any = true;
      if (any) break;
    }
    if (!any) { group.clear(); return; }
    ColItem it;
    it.group = group;
    items.push_back(it);
    group.clear();
  };
  for (Node* c : el->children) {
    if (c->type == N_TEXT) {
      if (!c->data.empty()) group.push_back(c);
      continue;
    }
    if (c->type != N_ELEMENT || !c->cs) continue;
    if (node_invisible(c->cs)) continue;
    if (c->cs->position == P_ABSOLUTE || c->cs->position == P_FIXED) continue;
    if (is_blocky(c->cs->display)) {
      flush_group();
      ColItem it;
      it.el = c;
      items.push_back(it);
    } else {
      group.push_back(c);
    }
  }
  flush_group();

  /* measure + lay out each item at the column width */
  float total = 0;
  for (auto& it : items) {
    Box* cb = new_box(it.el, b);
    it.box = cb;
    b->kids.push_back(cb);
    if (it.el) {
      ComputedStyle* ics = it.el->cs;
      resolve_box_edges(cb, ics, colw, lc);
      cb->w = colw;
      cb->cw = std::max(0.f, colw - cb->pad_l - cb->pad_r - cb->bor_l - cb->bor_r);
      cb->x = 0;
      cb->y = 0;
      float ch = layout_element(cb, it.el, lc);
      it.h = ch + cb->pad_t + cb->pad_b + cb->bor_t + cb->bor_b;
      it.mt = res_l(ics->margin[0], colw, ics->font_size, lc);
      it.mb = res_l(ics->margin[2], colw, ics->font_size, lc);
    } else {
      cb->cs = cs;
      cb->w = colw;
      cb->cw = colw;
      cb->x = 0;
      cb->y = 0;
      std::vector<InlineItem> seq;
      flatten_nodes(it.group, cs, &seq, lc, false);
      it.h = layout_inline_seq(cb, seq, lc, 0, 0);
    }
    total += it.h + it.mt + it.mb;
  }

  /* balanced column height, greedy sequential fill */
  float H = total / (float)ncols;
  int cur_col = 0;
  float cur_y = 0;
  float max_fill = 0;
  for (auto& it : items) {
    float block = it.h + it.mt + it.mb;
    if (cur_y > 0 && cur_col < ncols - 1 && cur_y + block > H) {
      cur_col++;
      cur_y = 0;
    }
    it.col = cur_col;
    it.y = cur_y;
    cur_y += block;
    max_fill = std::max(max_fill, cur_y);
  }

  /* position boxes into their columns */
  for (auto& it : items) {
    float fx = x0 + (float)it.col * (colw + gap) + (it.el ? 0.f : 0.f);
    float fy = y0 + it.y + it.mt;
    offset_box_recursive(it.box, fx - it.box->x, fy - it.box->y);
  }

  b->ch = max_fill;
  b->h = max_fill + b->pad_t + b->pad_b + b->bor_t + b->bor_b;
  layout_out_of_flow_children(b, el, lc);
  b->col_info.count = ncols;
  b->col_info.gap = gap;
  b->col_info.content_h = max_fill;
  return b->h;
}

float layout_element(Box* b, Node* el, LayoutCtx& lc) {
  ComputedStyle* cs = b->cs;
  /* content-visibility: hidden, the element's box is laid out but its
   * contents are not (subtree is skipped entirely). The element itself
   * retains its specified size, falling back to contain-intrinsic-size if
   * width/height are auto.
   *
   * content-visibility: auto, same as hidden when the element is far from
   * the viewport (more than 4 viewport heights below the top of the page);
   * behaves normally otherwise. This is the spec's "rendering is skipped
   * when not relevant to the user". */
  if (cs->content_visibility == 2 ||
      (cs->content_visibility == 1 &&
       b->y > (float)lc.vh * 4.f /* approximate: skip if far below viewport */)) {
    /* free any previous layout content */
    for (auto* k : b->kids) free_box_tree(k);
    for (auto* o : b->out_of_flow) free_box_tree(o);
    b->kids.clear();
    b->out_of_flow.clear();
    b->frags.clear();
    /* compute placeholder size: explicit width/height win, else
     * contain-intrinsic-size, else 0. */
    float w = b->w, h = b->h;
    if (cs->width.unit == L_AUTO || cs->width.unit == L_NORMAL)
      w = cs->contain_intrinsic_set ? cs->contain_intrinsic_w : 0;
    if (cs->height.unit == L_AUTO || cs->height.unit == L_NORMAL)
      h = cs->contain_intrinsic_set ? cs->contain_intrinsic_h : 0;
    b->w = w; b->h = h;
    b->cw = std::max(0.f, w - b->pad_l - b->pad_r - b->bor_l - b->bor_r);
    b->ch = std::max(0.f, h - b->pad_t - b->pad_b - b->bor_t - b->bor_b);
    return b->h;
  }
  if (cs->column_count > 1) return layout_block_columns(b, el, lc);
  float content_h = 0;
  /* definite height for percentage children: percentages resolve against
   * the parent's definite content height (viewport height for the root) */
  if (cs->height.unit != L_AUTO && cs->height.unit != L_NORMAL)
    b->ch_definite = res_pct_h(cs->height, b->parent ? b->parent->ch_definite : (float)lc.vh,
                               cs->font_size, lc);
  else
    b->ch_definite = 0;
  /* re-layout safety: free + clear previous content (kids/out_of_flow are
   * owning pointers, clearing without freeing would leak whole subtrees) */
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
      h = cs->height.unit == L_PCT
              ? (b->parent && b->parent->ch_definite > 0
                     ? cs->height.val * b->parent->ch_definite / 100.f
                     : res_l(cs->height, b->cw, cs->font_size, lc))
              : res_l(cs->height, b->cw, cs->font_size, lc);
    }
    if (cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL && !h_from_attr && cs->height.unit == L_AUTO)
      h = w * ih / iw; /* width set + height auto to keep intrinsic ratio */
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
  } else if (cs->display == D_TABLE || cs->display == D_INLINE_TABLE) {
    content_h = layout_table_content(b, el, lc);
  } else if (cs->display == D_TABLE_ROW || cs->display == D_TABLE_CELL) {
    /* stray rows/cells outside a table: fall back to block flow */
    content_h = layout_block_content(b, el, lc);
  } else {
    content_h = layout_block_content(b, el, lc);
  }
  /* clamp by height props: percentages against the parent's definite content
   * height (auto-height parent: percentage height behaves like auto) */
  float em = cs->font_size;
  float avail_w = b->cw;
  float h_basis = b->parent ? b->parent->ch_definite : (float)lc.vh;
  if (cs->height.unit != L_AUTO && cs->height.unit != L_NORMAL) {
    if (cs->height.unit == L_PCT && b->parent && b->parent->ch_definite <= 0) {
      /* keep content height */
    } else {
      content_h = res_pct_h(cs->height, h_basis, em, lc);
    }
  }
  if (cs->min_h.unit != L_AUTO && cs->min_h.unit != L_NORMAL)
    content_h = std::max(content_h, res_pct_h(cs->min_h, h_basis, em, lc));
  if (cs->max_h.unit != L_AUTO && cs->max_h.unit != L_NORMAL)
    content_h = std::min(content_h, res_l(cs->max_h, avail_w, em, lc));

  if (content_h <= 0 &&
      (el->tag == "input" || el->tag == "button" || el->tag == "select" || el->tag == "textarea")) {
    if (el->tag == "textarea") {
      int rows = el->attr("rows") ? atoi(el->attr("rows")->c_str()) : 2;
      content_h = (float)rows * cs->font_size * 1.4f + 6;
    } else if (el->tag == "input") {
      const std::string* ta = el->attr("type");
      std::string t = ta ? *ta : "text";
      if (t == "checkbox" || t == "radio")
        content_h = std::max(12.f, cs->font_size * 0.85f);
      else
        content_h = cs->font_size * 1.4f + 8;
    } else {
      content_h = cs->font_size * 1.4f + 8;
    }
  }
  b->ch = content_h;
  b->h = content_h + b->pad_t + b->pad_b + b->bor_t + b->bor_b;
  /* animated translateX (ticker-style keyframes): shift by pct of own width */
  if (cs->anim_tx) {
    float m[6] = {1, 0, 0, 1, cs->anim_tx_pct / 100.f * b->w, 0};
    memcpy(cs->mat, m, sizeof(m));
    cs->has_transform = true;
  }
  layout_out_of_flow_children(b, el, lc);
  /* container query support: record this box's content-box dimensions on its
   * computed style so the next cascade pass can evaluate @container rules
   * against the actual container size. container_type bitmask:
   *   1=inline-size (uses container_w), 2=block-size (uses container_h),
   *   3=size (both), 4=inline-block-size (both, treated same as size here). */
  if (cs->container_type != 0) {
    cs->container_w = b->cw;
    cs->container_h = b->ch;
  }
  return b->h;
}

/* resolve anchor() insets on an absolutely-positioned element. Walks up
 * the DOM tree from `node` to find the nearest element whose anchor_name
 * matches node->cs->position_anchor (or, when position_anchor is empty, the
 * default anchor per the spec is the nearest ancestor that establishes an
 * absolutely-positioned containing block, we approximate by using the
 * nearest element with an anchor_name). For each side where anchor_active
 * is set, the resolved px value replaces the regular inset. The anchor
 * element's box must already have been laid out (this runs after layout
 * completes on the containing block, which lays out abs children last). */
static Node* find_anchor_for(Node* node, const std::string& position_anchor) {
  if (!node) return nullptr;
  for (Node* p = node->parent; p; p = p->parent) {
    if (p->type != N_ELEMENT || !p->cs) continue;
    if (!position_anchor.empty()) {
      if (p->cs->anchor_name == position_anchor) return p;
    } else {
      /* implicit anchor: nearest element with any anchor_name */
      if (!p->cs->anchor_name.empty()) return p;
    }
  }
  return nullptr;
}

static void resolve_anchor_insets(Node* node) {
  ComputedStyle* cs = node->cs;
  if (!cs) return;
  bool any = false;
  for (int i = 0; i < 4; i++) if (cs->anchor_active[i]) { any = true; break; }
  if (!any) return;
  Node* anchor = find_anchor_for(node, cs->position_anchor);
  if (!anchor || !anchor->box) {
    for (int i = 0; i < 4; i++) cs->anchor_resolved_ok[i] = false;
    return;
  }
  Box* ab = anchor->box;
  /* anchor box edges in page coordinates */
  float a_left = ab->x;
  float a_top = ab->y;
  float a_right = ab->x + ab->w;
  float a_bottom = ab->y + ab->h;
  float a_cx = ab->x + ab->w / 2.f;
  float a_cy = ab->y + ab->h / 2.f;
  for (int i = 0; i < 4; i++) {
    if (!cs->anchor_active[i]) continue;
    uint8_t edge = cs->anchor_edge[i];
    float pct = cs->anchor_pct[i];
    /* side index 0=top, 1=right, 2=bottom, 3=left */
    float resolved = 0;
    switch (edge) {
      case 0: resolved = a_top; break;
      case 1: resolved = a_right; break;
      case 2: resolved = a_bottom; break;
      case 3: resolved = a_left; break;
      case 4: /* center, pick axis by side */
        resolved = (i == 0 || i == 2) ? a_cy : a_cx;
        break;
      case 6: /* start = same as left/top by axis */
        resolved = (i == 0 || i == 2) ? a_top : a_left;
        break;
      case 7: /* end = same as right/bottom by axis */
        resolved = (i == 0 || i == 2) ? a_bottom : a_right;
        break;
      case 8: /* percentage: along the anchor's inline axis for left/right,
               * block axis for top/bottom. */
        if (i == 0 || i == 2) resolved = a_top + pct / 100.f * ab->h;
        else resolved = a_left + pct / 100.f * ab->w;
        break;
    }
    cs->anchor_resolved[i] = resolved;
    cs->anchor_resolved_ok[i] = true;
  }
}

static void layout_out_of_flow_one(Box* b, Node* node, ComputedStyle* ccs, LayoutCtx& lc,
                                   float cb_x, float cb_y, float cb_w, float cb_h) {
  {
    float em = ccs->font_size;
    /* anchor positioning: resolve anchor() insets now, the containing block
     * (and thus the anchor, if it's an in-flow sibling or ancestor) has been
     * laid out by the time we get here (abs children are positioned last). */
    resolve_anchor_insets(node);
    Box* cb = new_box(node, b);
    cb->cs = ccs;
    b->out_of_flow.push_back(cb);
    /* resolve width */
    resolve_box_edges(cb, ccs, cb_w, lc);
    /* anchor insets override top/right/bottom/left when active and resolved */
    bool has_left  = ccs->anchor_resolved_ok[3] || ccs->left.unit != L_AUTO;
    bool has_right = ccs->anchor_resolved_ok[1] || ccs->right.unit != L_AUTO;
    bool has_top   = ccs->anchor_resolved_ok[0] || ccs->top.unit != L_AUTO;
    bool has_bottom= ccs->anchor_resolved_ok[2] || ccs->bottom.unit != L_AUTO;
    /* helper: resolve a side's inset value, preferring anchor result */
    auto side_val = [&](int idx, float cb_basis) -> float {
      if (ccs->anchor_resolved_ok[idx]) return ccs->anchor_resolved[idx];
      const Length& L = (idx == 0 ? ccs->top : idx == 1 ? ccs->right : idx == 2 ? ccs->bottom : ccs->left);
      return res_l(L, cb_basis, em, lc);
    };
    float w = 0;
    if (ccs->width.unit != L_AUTO && ccs->width.unit != L_NORMAL) {
      w = res_l(ccs->width, cb_w, em, lc);
    } else if (has_left && has_right) {
      /* both insets: derive width from them. anchor insets are in absolute
       * page coords, so the span is (right - left), margins handled later. */
      if (ccs->anchor_resolved_ok[3] || ccs->anchor_resolved_ok[1]) {
        float lv = side_val(3, cb_w);
        float rv = side_val(1, cb_w);
        w = std::max(0.f, rv - lv);
      } else {
        w = cb_w - side_val(3, cb_w) - side_val(1, cb_w);
      }
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
      if (ccs->anchor_resolved_ok[0] || ccs->anchor_resolved_ok[2]) {
        float tv = side_val(0, cbh);
        float bv = side_val(2, cbh);
        predef_h = std::max(0.f, bv - tv);
      } else {
        predef_h = cbh - side_val(0, cbh) - side_val(2, cbh);
      }
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
       * (CSS: over-constrained abs positioning to height comes from offsets) */
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
    float x, y;
    /* anchor-resolved values are absolute page coords; otherwise they're
     * relative to the containing block (cb_x, cb_y). */
    if (ccs->anchor_resolved_ok[3]) {
      x = ccs->anchor_resolved[3] + ml;
    } else if (has_left) {
      x = cb_x + side_val(3, cb_w) + ml;
    } else if (ccs->anchor_resolved_ok[1]) {
      x = ccs->anchor_resolved[1] - w - ml;
    } else if (has_right) {
      x = cb_x + cb_w - w - side_val(1, cb_w) - ml;
    } else {
      x = cb_x + ml;
    }
    if (ccs->anchor_resolved_ok[0]) {
      y = ccs->anchor_resolved[0] + mt;
    } else if (has_top) {
      y = cb_y + side_val(0, cb_h > 0 ? cb_h : (float)lc.vh) + mt;
    } else if (ccs->anchor_resolved_ok[2]) {
      y = ccs->anchor_resolved[2] - cb->h - mt;
    } else if (has_bottom) {
      y = cb_y + (cb_h > 0 ? cb_h : (float)lc.vh) - cb->h - side_val(2, cb_h > 0 ? cb_h : (float)lc.vh) - mt;
    } else {
      y = cb_y + mt;
    }
    if (ccs->position == P_FIXED && !ccs->anchor_resolved_ok[0] && !ccs->anchor_resolved_ok[1] &&
        !ccs->anchor_resolved_ok[2] && !ccs->anchor_resolved_ok[3]) {
      x = res_l(ccs->left, (float)lc.vw, em, lc);
      if (!has_left && has_right) x = lc.vw - cb->w - res_l(ccs->right, (float)lc.vw, em, lc);
      y = res_l(ccs->top, (float)lc.vh, em, lc);
      if (!has_top && has_bottom) y = lc.vh - cb->h - res_l(ccs->bottom, (float)lc.vh, em, lc);
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
  } else {
    body_declared = body_w - ml - mr;
  }
  /* max/min-width clamp on the body box (example.com: body{max-width:26em}) */
  if (bcs->max_w.unit != L_AUTO && bcs->max_w.unit != L_NORMAL)
    body_declared = std::min(body_declared, res_l(bcs->max_w, body_w, bcs->font_size, lc));
  if (bcs->min_w.unit != L_AUTO && bcs->min_w.unit != L_NORMAL)
    body_declared = std::max(body_declared, res_l(bcs->min_w, body_w, bcs->font_size, lc));
  resolve_box_edges(root, bcs, body_declared, lc);
  float border_w = body_declared + root->pad_l + root->pad_r + root->bor_l + root->bor_r;
  if (ml_auto && mr_auto && border_w < body_w) {
    ml = (body_w - border_w) / 2;
    mr = body_w - border_w - ml;
  } else if (ml_auto) {
    ml = std::max(0.f, body_w - border_w - mr);
  } else if (mr_auto) {
    mr = std::max(0.f, body_w - border_w - ml);
  }
  root->x = ml;
  root->y = res_l(bcs->margin[0], body_w, bcs->font_size, lc);
  root->w = border_w;
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
