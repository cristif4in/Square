/* =====================================================================
 *  Square Browser Engine — square_layout_table.cpp
 *  Table layout: row/cell collection with anonymous table objects,
 *  column sizing (width styles, min/max-content), colspan/rowspan and
 *  vertical cell alignment.
 * ===================================================================== */
#include "square_layout_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sqrl {

namespace {

struct TableCell {
  Node* node = nullptr;
  Box* box = nullptr;
  bool is_anon = false;
  std::vector<Node*> anon;
  int col = 0, cs_span = 1, rs_span = 1;
  float h = 0;
};

struct TableRow {
  Box* box = nullptr;
  std::vector<TableCell> cells;
};

static int span_attr(Node* n, const char* name) {
  const std::string* v = n ? n->attr(name) : nullptr;
  if (!v) return 1;
  int s = atoi(v->c_str());
  return s < 1 ? 1 : (s > 1000 ? 1000 : s);
}

static void collect_row_cells(Node* row_node, ComputedStyle* table_cs, Box* row_box,
                              TableRow* row) {
  std::vector<Node*> group;
  auto flush_group = [&]() {
    if (group.empty()) return;
    TableCell cell;
    cell.is_anon = true;
    cell.anon = group;
    cell.box = new_box(nullptr, row_box);
    cell.box->cs = row_box->cs;
    row_box->kids.push_back(cell.box);
    row->cells.push_back(cell);
    group.clear();
  };
  for (Node* c : row_node ? row_node->children : std::vector<Node*>{}) {
    if (c->type == N_TEXT) {
      bool only_ws = true;
      for (char ch : c->data)
        if (!isspace((unsigned char)ch)) { only_ws = false; break; }
      if (!only_ws) group.push_back(c);
      continue;
    }
    if (c->type != N_ELEMENT || node_invisible(c->cs)) continue;
    if (c->cs->position == P_ABSOLUTE || c->cs->position == P_FIXED) continue;
    flush_group();
    TableCell cell;
    cell.node = c;
    cell.box = new_box(c, row_box);
    row_box->kids.push_back(cell.box);
    cell.cs_span = span_attr(c, "colspan");
    cell.rs_span = span_attr(c, "rowspan");
    row->cells.push_back(cell);
  }
  flush_group();
}

static void collect_table_rows(Node* el, ComputedStyle* table_cs, Box* b,
                               std::vector<TableRow>* rows) {
  std::vector<TableCell> stray;
  auto flush_stray = [&]() {
    if (stray.empty()) return;
    TableRow row;
    row.box = new_box(nullptr, b);
    row.box->cs = table_cs;
    b->kids.push_back(row.box);
    for (TableCell& cell : stray) {
      row.box->kids.push_back(cell.box);
      row.cells.push_back(cell);
    }
    rows->push_back(row);
    stray.clear();
  };
  for (Node* c : el->children) {
    if (c->type == N_TEXT) continue;
    if (c->type != N_ELEMENT || !c->cs) continue;
    ComputedStyle* ccs = c->cs;
    if (node_invisible(ccs)) continue;
    if (ccs->position == P_ABSOLUTE || ccs->position == P_FIXED) continue;
    if (ccs->display == D_TABLE_ROW_GROUP) {
      flush_stray();
      for (Node* rc : c->children) {
        if (rc->type != N_ELEMENT || !rc->cs || node_invisible(rc->cs)) continue;
        if (rc->cs->position == P_ABSOLUTE || rc->cs->position == P_FIXED) continue;
        if (rc->cs->display == D_TABLE_ROW) {
          TableRow row;
          row.box = new_box(rc, b);
          row.box->cs = rc->cs;
          b->kids.push_back(row.box);
          collect_row_cells(rc, table_cs, row.box, &row);
          rows->push_back(row);
        } else if (rc->cs->display == D_TABLE_CELL) {
          TableRow row;
          row.box = new_box(nullptr, b);
          row.box->cs = table_cs;
          b->kids.push_back(row.box);
          TableCell cell;
          cell.node = rc;
          cell.box = new_box(rc, row.box);
          row.box->kids.push_back(cell.box);
          cell.cs_span = span_attr(rc, "colspan");
          cell.rs_span = span_attr(rc, "rowspan");
          row.cells.push_back(cell);
          rows->push_back(row);
        } else {
          TableRow row;
          row.box = new_box(nullptr, b);
          row.box->cs = table_cs;
          b->kids.push_back(row.box);
          TableCell cell;
          cell.is_anon = true;
          cell.anon.push_back(rc);
          cell.box = new_box(nullptr, row.box);
          cell.box->cs = table_cs;
          row.box->kids.push_back(cell.box);
          row.cells.push_back(cell);
          rows->push_back(row);
        }
      }
      continue;
    }
    if (ccs->display == D_TABLE_ROW) {
      flush_stray();
      TableRow row;
      row.box = new_box(c, b);
      row.box->cs = ccs;
      b->kids.push_back(row.box);
      collect_row_cells(c, table_cs, row.box, &row);
      rows->push_back(row);
      continue;
    }
    if (ccs->display == D_TABLE_CELL) {
      TableCell cell;
      cell.node = c;
      cell.box = new_box(c, nullptr);
      cell.cs_span = span_attr(c, "colspan");
      cell.rs_span = span_attr(c, "rowspan");
      stray.push_back(cell);
      continue;
    }
    TableCell cell;
    cell.is_anon = true;
    cell.anon.push_back(c);
    cell.box = new_box(nullptr, nullptr);
    cell.box->cs = table_cs;
    stray.push_back(cell);
  }
  flush_stray();
  for (TableRow& row : *rows)
    for (TableCell& cell : row.cells)
      if (!cell.box->parent) cell.box->parent = row.box;
}

} /* namespace */

float layout_table_content(Box* b, Node* el, LayoutCtx& lc) {
  ComputedStyle* cs = b->cs;
  float avail_w = b->cw;

  std::vector<TableRow> rows;
  collect_table_rows(el, cs, b, &rows);
  if (rows.empty()) {
    b->last_line_baseline = b->y;
    return 0;
  }

  int ncols = 1;
  for (TableRow& row : rows) {
    int sum = 0;
    for (TableCell& cell : row.cells) sum += cell.cs_span;
    ncols = std::max(ncols, sum);
  }

  /* grid-style column assignment with span-aware cursor */
  int cursor_col = 0;
  for (TableRow& row : rows) {
    for (TableCell& cell : row.cells) {
      if (cursor_col + cell.cs_span > ncols) cursor_col = 0;
      cell.col = cursor_col;
      cursor_col += cell.cs_span;
      if (cursor_col >= ncols) cursor_col = 0;
    }
  }

  /* column sizing: fixed (style width) beats min-content, then grow toward
   * max-content with leftover width, then stretch to fill the table */
  std::vector<float> colw(ncols, 0);
  std::vector<float> col_max(ncols, 0);
  std::vector<bool> col_has_content(ncols, false);
  for (TableRow& row : rows) {
    for (TableCell& cell : row.cells) {
      if (!cell.node || cell.is_anon) continue;
      ComputedStyle* ics = cell.node->cs;
      float em = ics->font_size;
      if (ics->width.unit != L_AUTO && ics->width.unit != L_NORMAL) {
        float w = res_l(ics->width, avail_w, em, lc);
        w += ics->border_style[3] ? res_l(ics->border_width[3], avail_w, em, lc) : 0;
        w += ics->border_style[1] ? res_l(ics->border_width[1], avail_w, em, lc) : 0;
        w += res_l(ics->padding[3], avail_w, em, lc) + res_l(ics->padding[1], avail_w, em, lc);
        float per = w / (float)cell.cs_span;
        for (int i = cell.col; i < std::min(ncols, cell.col + cell.cs_span); i++)
          colw[i] = std::max(colw[i], per);
      }
    }
  }
  for (TableRow& row : rows) {
    for (TableCell& cell : row.cells) {
      float mn, mx;
      if (cell.node && !cell.is_anon) {
        mn = measure_min_content_width(cell.node, lc);
        mx = measure_max_content_width(cell.node, lc);
      } else {
        float maxw = 0;
        std::vector<InlineItem> seq;
        flatten_nodes(cell.anon, cs, &seq, lc, false);
        for (auto& q : seq) maxw += q.adv;
        mn = mx = maxw;
      }
      float per_min = mn / (float)cell.cs_span;
      float per_max = mx / (float)cell.cs_span;
      for (int i = cell.col; i < std::min(ncols, cell.col + cell.cs_span); i++) {
        colw[i] = std::max(colw[i], per_min);
        col_max[i] = std::max(col_max[i], per_max);
        col_has_content[i] = true;
      }
    }
  }
  float col_total = 0;
  for (int i = 0; i < ncols; i++) col_total += colw[i];
  if (col_total > avail_w && col_total > 0) {
    float k = std::max(0.f, avail_w) / col_total;
    for (int i = 0; i < ncols; i++) colw[i] *= k;
  }
  float extra = avail_w - col_total;
  if (extra > 0) {
    for (int i = 0; i < ncols && extra > 0.01f; i++) {
      if (!col_has_content[i]) continue;
      float room = col_max[i] - colw[i];
      if (room > 0) {
        float add = std::min(room, extra);
        colw[i] += add;
        extra -= add;
      }
    }
    if (extra > 0.01f) {
      int stretchable = 0;
      for (int i = 0; i < ncols; i++) stretchable++;
      if (stretchable > 0) {
        float add = extra / (float)stretchable;
        for (int i = 0; i < ncols; i++) colw[i] += add;
      }
    }
  }

  /* layout cells, collect row heights (rowspan distributes evenly) */
  std::vector<float> rowh(rows.size(), 0);
  for (size_t r = 0; r < rows.size(); r++) {
    for (TableCell& cell : rows[r].cells) {
      float span_w = 0;
      for (int i = cell.col; i < std::min(ncols, cell.col + cell.cs_span); i++) span_w += colw[i];
      span_w = std::max(0.f, span_w);
      if (cell.node && !cell.is_anon) {
        ComputedStyle* ics = cell.node->cs;
        resolve_box_edges(cell.box, ics, span_w, lc);
        cell.box->w = span_w;
        cell.box->cw = std::max(0.f, span_w - cell.box->pad_l - cell.box->pad_r - cell.box->bor_l - cell.box->bor_r);
        cell.box->x = 0;
        cell.box->y = 0;
        float ch = layout_element(cell.box, cell.node, lc);
        cell.h = ch;
        if (ics->height.unit != L_AUTO && ics->height.unit != L_NORMAL)
          cell.h = res_l(ics->height, span_w, ics->font_size, lc) + cell.box->pad_t + cell.box->pad_b +
                   cell.box->bor_t + cell.box->bor_b;
      } else {
        cell.box->cs = rows[r].box->cs ? rows[r].box->cs : cs;
        cell.box->w = span_w;
        cell.box->cw = span_w;
        cell.box->x = 0;
        cell.box->y = 0;
        std::vector<InlineItem> seq;
        flatten_nodes(cell.anon, cell.box->cs, &seq, lc, false);
        cell.h = layout_inline_seq(cell.box, seq, lc, 0, 0);
      }
      for (size_t rr = r; rr < std::min(rows.size(), (size_t)(r + cell.rs_span)); rr++)
        rowh[rr] = std::max(rowh[rr], cell.h / (float)cell.rs_span);
    }
  }

  /* row height style floors */
  for (size_t r = 0; r < rows.size(); r++) {
    Box* rb = rows[r].box;
    if (rb && rb->cs && rb->cs->height.unit != L_AUTO && rb->cs->height.unit != L_NORMAL)
      rowh[r] = std::max(rowh[r], res_l(rb->cs->height, avail_w, rb->cs->font_size, lc));
  }

  /* placement */
  float y = 0;
  for (size_t r = 0; r < rows.size(); r++) {
    for (TableCell& cell : rows[r].cells) {
      float x0 = 0;
      for (int i = 0; i < cell.col; i++) x0 += colw[i];
      float span_w = 0;
      for (int i = cell.col; i < std::min(ncols, cell.col + cell.cs_span); i++) span_w += colw[i];
      float fx = x0;
      float fy = y;
      offset_box_recursive(cell.box, fx - cell.box->x, fy - cell.box->y);
      cell.box->w = span_w;
      cell.box->h = rowh[r];
      ComputedStyle* vcs = cell.node ? cell.node->cs : cell.box->cs;
      VertAlign va = vcs ? vcs->vert_align : VA_BASELINE;
      if (cell.h < rowh[r] && (va == VA_MIDDLE || va == VA_BOTTOM)) {
        float dy = va == VA_MIDDLE ? (rowh[r] - cell.h) / 2 : (rowh[r] - cell.h);
        for (auto* k : cell.box->kids) offset_box_recursive(k, 0, dy);
        for (auto& f : cell.box->frags) {
          f.rect.y += dy;
          if (f.kind == InlineFrag::F_TEXT) {
            f.run.y += dy;
          }
        }
        cell.box->last_line_baseline += dy;
      }
    }
    y += rowh[r];
  }

  float content_h = y;
  b->last_line_baseline = b->y + b->pad_t + b->bor_t + content_h;
  return content_h;
}

} /* namespace sqrl */
