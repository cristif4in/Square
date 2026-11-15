import io

# ---- 1. paint: form control appearance (insert before "children clip") ----
p='src/square_layout_paint.cpp'
s=io.open(p,encoding='utf-8').read()
old="""  /* children clip */
  RoundedClip saved_rc = target.rc;"""
new="""  /* form control appearance: checkbox / radio / text / textarea / select /
   * submit-button — painted after background+border, before children */
  if (b->node && (b->node->tag == "input" || b->node->tag == "textarea" ||
                  b->node->tag == "select")) {
    Node* n = b->node;
    Rect cb = {b->x + b->pad_l + b->bor_l, b->y + b->pad_t + b->bor_t,
               std::max(0.f, b->w - b->pad_l - b->pad_r - b->bor_l - b->bor_r),
               std::max(0.f, b->h - b->pad_t - b->pad_b - b->bor_t - b->bor_b)};
    const std::string* ta = n->attr("type");
    std::string type = ta ? *ta : (n->tag == "input" ? "text" : "");
    for (auto& c : type) c = (char)tolower((unsigned char)c);
    const Rect* clip = &target.clip;
    const RoundedClip* rcp = target.rc.active ? &target.rc : nullptr;
    Color accent{255, 26, 115, 219};    /* a,r,g,b — #1a73db-ish */
    Color border{255, 118, 118, 118};   /* #767676 */
    Color ink = cs->color;
    Color muted{255, 130, 130, 130};
    float fs = cs->font_size;
    void* fnt = font_for(cs);
    auto draw_text = [&](const std::string& t, Color col) {
      if (!fnt || t.empty() || cb.w < 4) return;
      std::vector<GlyphPos> glyphs;
      font_shape(fnt, t.c_str(), cs->letter_spacing, &glyphs, cs->word_spacing);
      float asc = font_ascent(fnt);
      float ty = cb.y + (cb.h - (asc + font_descent(fnt))) / 2.f + asc;
      float maxw = cb.w - 6;
      /* clip glyphs beyond the box */
      for (size_t gi = 0; gi < glyphs.size(); gi++) {
        if (glyphs[gi].x > maxw) { glyphs.resize(gi); break; }
      }
      gfx_draw_glyphs(ts, fnt, cb.x + 3, ty, glyphs, col, target.alpha, clip);
    };
    if (n->tag == "input" && (type == "checkbox" || type == "radio")) {
      float sz = std::min(std::min(cb.w, cb.h), std::max(12.f, fs * 0.85f));
      Rect m{cb.x, cb.y + (cb.h - sz) / 2, sz, sz};
      bool checked = n->attr("checked") != nullptr;
      if (type == "checkbox") {
        float r3[4] = {3, 3, 3, 3};
        if (checked) {
          gfx_fill_rect(ts, m, accent, r3, clip, rcp);
          /* white check: two angled strokes approximated with small squares */
          float stroke = std::max(1.5f, sz / 7.f);
          auto seg = [&](float x0, float y0, float x1, float y1) {
            int steps = (int)std::max(2.f, (fabsf(x1 - x0) + fabsf(y1 - y0)) * 2);
            for (int q = 0; q <= steps; q++) {
              float t2 = (float)q / steps;
              Rect px{x0 + (x1 - x0) * t2 - stroke / 2, y0 + (y1 - y0) * t2 - stroke / 2,
                      stroke, stroke};
              gfx_fill_rect(ts, px, Color{255, 255, 255, 255}, nullptr, clip, rcp);
            }
          };
          seg(m.x + sz * 0.20f, m.y + sz * 0.52f, m.x + sz * 0.42f, m.y + sz * 0.74f);
          seg(m.x + sz * 0.42f, m.y + sz * 0.74f, m.x + sz * 0.80f, m.y + sz * 0.28f);
        } else {
          gfx_fill_rect(ts, m, Color{255, 255, 255, 255}, r3, clip, rcp);
          float bwd[4] = {1, 1, 1, 1};
          gfx_ring_rect(ts, m, r3, bwd, border, clip, rcp);
        }
      } else {
        /* radio */
        float rr[4] = {sz / 2, sz / 2, sz / 2, sz / 2};
        gfx_fill_rect(ts, m, Color{255, 255, 255, 255}, rr, clip, rcp);
        float bwd[4] = {1, 1, 1, 1};
        gfx_ring_rect(ts, m, rr, bwd, checked ? accent : border, clip, rcp);
        if (checked) {
          float dsz = sz * 0.45f;
          Rect d{m.x + (sz - dsz) / 2, m.y + (sz - dsz) / 2, dsz, dsz};
          float dr[4] = {dsz / 2, dsz / 2, dsz / 2, dsz / 2};
          gfx_fill_rect(ts, d, accent, dr, clip, rcp);
        }
      }
    } else if (n->tag == "input" &&
               (type == "submit" || type == "button" || type == "reset")) {
      /* button: label text centered */
      const std::string* val = n->attr("value");
      std::string label = val && !val->empty() ? *val : "Submit";
      void* fnt2 = font_for(cs);
      if (fnt2) {
        float tw = font_measure_text(fnt2, label.c_str(), 0, 0);
        std::vector<GlyphPos> glyphs;
        font_shape(fnt2, label.c_str(), 0, &glyphs);
        float asc = font_ascent(fnt2);
        float ty = cb.y + (cb.h - (asc + font_descent(fnt2))) / 2.f + asc;
        float tx = cb.x + std::max(0.f, (cb.w - tw) / 2);
        gfx_draw_glyphs(ts, fnt2, tx, ty, glyphs, ink, target.alpha, clip);
      }
    } else {
      /* text-like input, textarea, select */
      if (n->tag == "select") {
        /* selected option text */
        std::string value;
        bool have = false;
        for (Node* o : n->children) {
          if (o->type == N_ELEMENT && o->tag == "option") {
            if (o->attr("selected")) { value = o->text_content(); have = true; break; }
          }
        }
        if (!have) {
          for (Node* o : n->children)
            if (o->type == N_ELEMENT && o->tag == "option") { value = o->text_content(); break; }
        }
        draw_text(value, ink);
        /* chevron: filled triangle from shrinking bars */
        float chw = std::max(6.f, fs * 0.4f);
        float chh = std::max(4.f, fs * 0.3f);
        float cx0 = cb.x + cb.w - 4 - chw;
        float cy0 = cb.y + (cb.h - chh) / 2;
        for (int q = 0; q < 4; q++) {
          float wq = chw * (1.f - (float)q / 4.f);
          Rect bar{cx0 + (chw - wq) / 2, cy0 + chh * (float)q / 4.f, wq, chh / 4.f + 0.5f};
          gfx_fill_rect(ts, bar, muted, nullptr, clip, rcp);
        }
      } else {
        const std::string* val = n->attr("value");
        const std::string* ph = n->attr("placeholder");
        if (type == "password" && val && !val->empty()) {
          std::string dots;
          size_t nchars = 0;
          size_t q = 0;
          while (q < val->size()) {
            size_t adv = 1;
            unsigned char c0 = (unsigned char)(*val)[q];
            if (c0 >= 0xF0) adv = 4;
            else if (c0 >= 0xE0) adv = 3;
            else if (c0 >= 0xC0) adv = 2;
            q += adv;
            nchars++;
          }
          dots.assign(nchars, '\\x5f');
          draw_text(dots, ink);
        } else if (val && !val->empty()) {
          draw_text(*val, ink);
        } else if (ph && !ph->empty()) {
          draw_text(*ph, muted);
        }
      }
    }
  }

  /* children clip */
  RoundedClip saved_rc = target.rc;"""
assert s.count(old)==1
s=s.replace(old,new)
io.open(p,'w',encoding='utf-8').write(s)
print("paint ok")

# ---- 2. UA styles: focus ring + control chrome ----
p='src/square_style.cpp'
s=io.open(p,encoding='utf-8').read()
old='"input[type=hidden]{display:none}"'
assert s.count(old)==1
s=s.replace(old,'''"input[type=hidden]{display:none}"
      "input:not([type=checkbox]):not([type=radio]):not([type=submit]):not([type=button]):not([type=reset]):not([type=hidden]):not([type=range]):not([type=color]),"
      "textarea,select{background:#ffffff;border:1px solid #767676;border-radius:4px;padding:4px 8px}"
      "input:focus,textarea:focus,select:focus,button:focus{box-shadow:0 0 0 2px rgba(59,91,219,.45)}"''')
io.open(p,'w',encoding='utf-8').write(s)
print("ua ok")

# ---- 3. layout: checkbox/radio intrinsic 13px; textarea rows height ----
p='src/square_layout_common.cpp'
s=io.open(p,encoding='utf-8').read()
old="""  if (el->tag == "input" || el->tag == "select") {
    float w = cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL
                  ? res_l(cs->width, 0, cs->font_size, lc)
                  : (el->attr("size") ? (float)atoi(el->attr("size")->c_str()) * 8.f : 150.f);"""
new="""  if (el->tag == "input") {
    const std::string* ta = el->attr("type");
    std::string t = ta ? *ta : "text";
    if (t == "checkbox" || t == "radio") {
      float sz = std::max(12.f, cs->font_size * 0.85f);
      return sz + res_l(cs->padding[3], sz, cs->font_size, lc) +
             res_l(cs->padding[1], sz, cs->font_size, lc);
    }
  }
  if (el->tag == "textarea") {
    float w = cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL
                  ? res_l(cs->width, 0, cs->font_size, lc)
                  : (el->attr("cols") ? (float)atoi(el->attr("cols")->c_str()) * 8.5f : 173.f);
    return w + res_l(cs->padding[3], w, cs->font_size, lc) +
           res_l(cs->padding[1], w, cs->font_size, lc);
  }
  if (el->tag == "input" || el->tag == "select") {
    float w = cs->width.unit != L_AUTO && cs->width.unit != L_NORMAL
                  ? res_l(cs->width, 0, cs->font_size, lc)
                  : (el->tag == "select" ? 120.f
                     : (el->attr("size") ? (float)atoi(el->attr("size")->c_str()) * 8.f : 150.f));"""
assert s.count(old)==1
s=s.replace(old,new)
io.open(p,'w',encoding='utf-8').write(s)
print("measure ok")

# ---- 4. block layout: control default heights ----
p='src/square_layout_block.cpp'
s=io.open(p,encoding='utf-8').read()
old="""  if (content_h <= 0 &&
      (el->tag == "input" || el->tag == "button" || el->tag == "select" || el->tag == "textarea"))
    content_h = cs->font_size * 1.4f + 8;"""
new="""  if (content_h <= 0 &&
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
  }"""
assert s.count(old)==1
s=s.replace(old,new)
io.open(p,'w',encoding='utf-8').write(s)
print("block ok")
