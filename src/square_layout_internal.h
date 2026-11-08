/* =====================================================================
 *  Square Browser Engine — square_layout_internal.h
 *  Shared declarations between the layout modules:
 *    square_layout_common.cpp  shared helpers, measurement, box tree utils
 *    square_layout_inline.cpp  inline items + inline layout core
 *    square_layout_block.cpp   block layout, element dispatch, entry point
 *    square_layout_flex.cpp    flexbox layout
 *    square_layout_grid.cpp    grid layout
 *    square_layout_paint.cpp   paint traversal, scrollbar, document paint
 *
 *  Layout contract between parent and child:
 *    caller sets  b->x, b->y (border-box origin, page coords)
 *                 b->w (border-box width), b->cw (content width)
 *    layout_element() sets b->h (border-box height) and returns it.
 * ===================================================================== */
#pragma once

#include "square_internal.h"

#include <string>
#include <vector>

namespace sqrl {

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
  VertAlign va = VA_BASELINE;
  float va_len = 0;
};

/* ============================ shared helpers (square_layout_common.cpp) ============================ */
float res_l(const Length& L, float pct_basis, float em, LayoutCtx& lc);
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* vertical lengths resolve percentages against the parent's definite content
 * height (0 = parent height auto: percentage behaves like auto/0) */
inline float res_pct_h(const Length& L, float parent_ch, float em, LayoutCtx& lc) {
  if (L.unit == L_PCT) return parent_ch > 0.f ? L.val * parent_ch / 100.f : 0.f;
  return res_l(L, parent_ch, em, lc);
}
bool is_blocky(Display d);
bool is_atomic_inline(Display d);
bool node_invisible(const ComputedStyle* cs);
void* font_for(const ComputedStyle* cs);
float line_height_px(const ComputedStyle* cs, void* font, LayoutCtx& lc,
                     float* asc_out = nullptr, float* desc_out = nullptr);
std::string transform_text(const std::string& in, TextTransform tt);

Box* new_box(Node* n, Box* parent);
void replaced_intrinsic(Node* el, LayoutCtx& lc, float* w, float* h);
void resolve_box_edges(Box* b, const ComputedStyle* cs, float cw_basis, LayoutCtx& lc);
float measure_max_content_width(Node* el, LayoutCtx& lc);
float measure_min_content_width(Node* el, LayoutCtx& lc);

/* ============================ inline layout (square_layout_inline.cpp) ============================ */
void flatten_nodes(const std::vector<Node*>& nodes, const ComputedStyle* cs,
                   std::vector<InlineItem>* items, LayoutCtx& lc, bool pre);
float layout_inline_seq(Box* container, std::vector<InlineItem>& seq, LayoutCtx& lc,
                        float x_origin, float y_origin);

/* ============================ layout dispatch (per-module) ============================ */
float layout_element(Box* b, Node* el, LayoutCtx& lc);
float layout_block_content(Box* b, Node* el, LayoutCtx& lc);
float layout_flex_content(Box* b, Node* el, LayoutCtx& lc);
float layout_grid_content(Box* b, Node* el, LayoutCtx& lc);
float layout_table_content(Box* b, Node* el, LayoutCtx& lc);

} /* namespace sqrl */
