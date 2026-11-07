/* =====================================================================
 *  Square Browser Engine — square_layout_grid.cpp
 *  Grid layout: track resolution (px/pct/fr/minmax/auto-fit), item
 *  auto-placement, span handling and row sizing.
 * ===================================================================== */
#include "square_layout_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace sqrl {

/* ============================ grid layout ============================ */
float layout_grid_content(Box* b, Node* el, LayoutCtx& lc) {
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
    (void)x0;
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

} /* namespace sqrl */
