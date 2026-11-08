/* =====================================================================
 *  Square Browser Engine — square_layout_inline.cpp
 *  Inline layout: InlineItem flattening, replaced items in inline flow,
 *  line breaking and fragment emission.
 * ===================================================================== */
#include "square_layout_internal.h"

#include <algorithm>
#include <cctype>

namespace sqrl {

/* ============================ inline items ============================ */
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
  it->va = cs->vert_align;
  it->va_len = (cs->vert_align == VA_LEN) ? res_l(cs->vert_len, 0, cs->font_size, lc) : 0;
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

void flatten_nodes(const std::vector<Node*>& nodes, const ComputedStyle* cs,
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
        float dy = 0;
        switch (it->va) {
          case VA_BOTTOM:
            dy = (ascent + descent) - (it->ascent + it->descent);
            break;
          case VA_MIDDLE:
            dy = ((ascent + descent) - (it->ascent + it->descent)) / 2;
            break;
          case VA_LEN:
            dy = -it->va_len;
            break;
          default:
            break;
        }
        run.y += dy;
        InlineFrag f;
        f.kind = InlineFrag::F_TEXT;
        f.node = it->node;
        f.run = run;
        f.ascent = it->ascent;
        f.descent = it->descent;
        f.rect = {x, y_line_top + dy, it->adv, it->ascent + it->descent};
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
            float bdy = 0;
            if (ics && ics->vert_align == VA_LEN)
              bdy = -res_l(ics->vert_len, 0, ics->font_size, lc);
            f.rise = -bdy;
            f.rect = {open[j].x0 - ml, y_line_top + bdy, x - open[j].x0 + ml + mr, ascent + descent};
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
        float def_y = (y_line_top + ascent) - it->baseline;
        float ry = def_y;
        const ComputedStyle* rcs = it->node ? it->node->cs : nullptr;
        switch (it->va) {
          case VA_TOP:
            ry = y_line_top;
            break;
          case VA_BOTTOM:
            ry = y_line_top + ascent + descent - bh;
            break;
          case VA_MIDDLE:
            ry = y_line_top + (ascent + descent - bh) / 2;
            break;
          case VA_LEN:
            ry = def_y - (rcs ? res_l(rcs->vert_len, 0, rcs->font_size, lc) : it->va_len);
            break;
          default:
            break;
        }
        f.rise = def_y - ry;
        f.rect = {x, ry, bw, bh};
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
    float bdy = 0;
    if (o.box->node && o.box->node->cs && o.box->node->cs->vert_align == VA_LEN)
      bdy = -res_l(o.box->node->cs->vert_len, 0, o.box->node->cs->font_size, lc);
    f.rise = -bdy;
    f.rect = {o.x0, y_line_top + bdy, x - o.x0, ascent + descent};
    f.ascent = ascent;
    f.descent = descent;
    container->frags.push_back(f);
  }
}

float layout_inline_seq(Box* container, std::vector<InlineItem>& seq, LayoutCtx& lc,
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

} /* namespace sqrl */
