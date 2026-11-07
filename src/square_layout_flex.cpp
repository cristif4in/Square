/* =====================================================================
 *  Square Browser Engine — square_layout_flex.cpp
 *  Flexbox layout: item collection (blockification), hypothetical main
 *  sizes, per-line flexing, justify/align and auto-margin absorption.
 * ===================================================================== */
#include "square_layout_internal.h"

#include <algorithm>
#include <cctype>

namespace sqrl {

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

float layout_flex_content(Box* b, Node* el, LayoutCtx& lc) {
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
  (void)main_avail;

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

} /* namespace sqrl */
