import io

# ---- header: column fields ----
p='src/square_internal.h'
s=io.open(p,encoding='utf-8').read()
old="""  uint8_t clear = 0;                     /* 0 none, 1 left, 2 right, 3 both */"""
assert s.count(old)==1
s=s.replace(old,"""  uint8_t clear = 0;                     /* 0 none, 1 left, 2 right, 3 both */
  /* CSS multicol */
  int column_count = 0;                  /* 0 = auto (no columns) */
  Length column_gap;                     /* length; default 16px */
  uint8_t column_rule_style = 0;         /* 0 none, else like border styles */
  float column_rule_w = 1;
  Color column_rule_color{255, 28, 34, 48}; /* currentColor-ish default */
  bool column_rule_color_set = false;""")
io.open(p,'w',encoding='utf-8').write(s)
print("header ok")


# ---- style.cpp parse: column-* ----
p='src/square_style.cpp'
s=io.open(p,encoding='utf-8').read()
old='"column-count", "column-gap", "column-rule", "columns", "mask", "mask-image", "clip-path",'
assert s.count(old)==1
s=s.replace(old,'"mask", "mask-image", "clip-path",')
old="""  if (prop == "transition-property" || prop == "transition-duration" ||"""
assert s.count(old)==1
s=s.replace(old,"""  if (prop == "columns") {
    /* shorthand: <column-count> || <column-width>; count only supported */
    std::vector<std::string> toks;
    split_values(lc(trim(substitute_vars(v, rc))), &toks);
    for (auto& t : toks) {
      if (!t.empty() && (isdigit((unsigned char)t[0]))) cs->column_count = atoi(t.c_str());
    }
    return true;
  }
  if (prop == "column-count") {
    std::string s2 = lc(trim(substitute_vars(v, rc)));
    cs->column_count = s2 == "auto" ? 0 : atoi(s2.c_str());
    return true;
  }
  if (prop == "column-gap") {
    parse_length(substitute_vars(v, rc), &cs->column_gap, rc);
    if (cs->column_gap.unit == L_NORMAL || cs->column_gap.unit == L_AUTO)
      cs->column_gap = Length{};
    return true;
  }
  if (prop == "column-rule" || starts_with(prop, "column-rule-")) {
    std::string body = lc(trim(substitute_vars(v, rc)));
    if (body == "none") { cs->column_rule_style = 0; return true; }
    std::vector<std::string> toks;
    split_values(body, &toks);
    for (auto& t : toks) {
      if (t == "solid" || t == "dashed" || t == "dotted" || t == "double")
        cs->column_rule_style = t == "solid" ? 1 : (t == "dashed" ? 2 : (t == "dotted" ? 3 : 4));
      else if (!t.empty() && (isdigit((unsigned char)t[0]) || t[0] == '.'))
        cs->column_rule_w = (float)atof(t.c_str());
      else {
        Color c;
        if (parse_css_color(trim(substitute_vars(v, rc)), rc, &c)) {
          cs->column_rule_color = c;
          cs->column_rule_color_set = true;
        }
      }
    }
    return true;
  }
  if (prop == "transition-property" || prop == "transition-duration" ||""")
io.open(p,'w',encoding='utf-8').write(s)
print("style ok")

# ---- block.cpp: column layout function ----
p='src/square_layout_block.cpp'
s=io.open(p,encoding='utf-8').read()
old="""/* ============================ element dispatch ============================ */
float layout_element(Box* b, Node* el, LayoutCtx& lc) {"""
assert s.count(old)==1
new_fn=r'''/* ============================ multicol ============================ */
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

/* ============================ element dispatch ============================ */
float layout_element(Box* b, Node* el, LayoutCtx& lc) {'''
s=s.replace(old,new_fn)

# dispatch: route to columns
old="""  float content_h = 0;
  /* definite height for percentage children: percentages resolve against
   * the parent's definite content height (viewport height for the root) */"""
assert s.count(old)==1
s=s.replace(old,"""  if (cs->column_count > 1) return layout_block_columns(b, el, lc);
  float content_h = 0;
  /* definite height for percentage children: percentages resolve against
   * the parent's definite content height (viewport height for the root) */""")
io.open(p,'w',encoding='utf-8').write(s)
print("block ok")

# ---- Box: col_info ----
p='src/square_internal.h'
s=io.open(p,encoding='utf-8').read()
old="""  /* marker for list items */
  std::string marker_text;"""
assert s.count(old)==1
s=s.replace(old,"""  /* multicol info for rule painting */
  struct {
    int count = 0;
    float gap = 0, content_h = 0;
  } col_info;
  /* marker for list items */
  std::string marker_text;""")
io.open(p,'w',encoding='utf-8').write(s)
print("box ok")

# ---- paint: column rules ----
p='src/square_layout_paint.cpp'
s=io.open(p,encoding='utf-8').read()
old="""  /* form control appearance */
  if (b->node && (b->node->tag == "input" || b->node->tag == "textarea" ||
                  b->node->tag == "select")) {"""
assert s.count(old)==1
s=s.replace(old,"""  /* multicol column rules */
  if (b->col_info.count > 1 && cs->column_rule_style != 0) {
    float cw = (b->cw - cs->column_rule_w * 0 - (float)(b->col_info.count - 1) * b->col_info.gap) /
               (float)b->col_info.count;
    for (int i = 1; i < b->col_info.count; i++) {
      float rx = b->x + b->pad_l + b->bor_l + (float)i * (cw + b->col_info.gap) - b->col_info.gap / 2 -
                 cs->column_rule_w / 2;
      Rect rule{rx, b->y + b->pad_t + b->bor_t, cs->column_rule_w, b->col_info.content_h};
      if (cs->column_rule_style == 2) { /* dashed */
        float dash = std::max(4.f, cs->column_rule_w * 3);
        for (float dy = 0; dy < rule.h; dy += dash * 2) {
          Rect dseg{rule.x, rule.y + dy, rule.w, std::min(dash, rule.h - dy)};
          gfx_fill_rect(ts, dseg, cs->column_rule_color, nullptr, &target.clip,
                        target.rc.active ? &target.rc : nullptr);
        }
      } else if (cs->column_rule_style == 3) { /* dotted */
        for (float dy = 0; dy < rule.h; dy += cs->column_rule_w * 2 + 2) {
          Rect dseg{rule.x, rule.y + dy, rule.w, rule.w};
          gfx_fill_rect(ts, dseg, cs->column_rule_color, nullptr, &target.clip,
                        target.rc.active ? &target.rc : nullptr);
        }
      } else {
        gfx_fill_rect(ts, rule, cs->column_rule_color, nullptr, &target.clip,
                      target.rc.active ? &target.rc : nullptr);
      }
    }
  }

  /* form control appearance */
  if (b->node && (b->node->tag == "input" || b->node->tag == "textarea" ||
                  b->node->tag == "select")) {""")
io.open(p,'w',encoding='utf-8').write(s)
print("paint ok")
