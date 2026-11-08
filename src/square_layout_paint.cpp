/* =====================================================================
 *  Square Browser Engine — square_layout_paint.cpp
 *  Paint traversal of the box tree into the gfx rasterizer: backgrounds,
 *  borders, inline fragments, replaced content, z-ordering, transform /
 *  opacity layering, scrollbar and the document paint entry point.
 * ===================================================================== */
#include "square_layout_internal.h"

#include <algorithm>
#include <functional>

namespace sqrl {

/* ============================ paint debug counters ============================ */
static int g_painted_boxes = 0, g_painted_frags = 0, g_painted_images = 0;
void paint_debug_counts(int* boxes, int* frags, int* imgs) {
  *boxes = g_painted_boxes; *frags = g_painted_frags; *imgs = g_painted_images;
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
  (void)w;
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
  (void)layer_region;

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
  /* sticky: collect boxes pinned by top/bottom against the viewport */
  std::vector<std::pair<Box*, float>> sticky;
  if (root) {
    std::function<void(Box*)> collect = [&](Box* bx) {
      if (bx->cs && bx->cs->position == P_STICKY) {
        float em = bx->cs->font_size;
        float delta = 0;
        if (bx->cs->top.unit != L_AUTO && bx->cs->top.unit != L_NORMAL) {
          float desired = scroll_y + res_l(bx->cs->top, 0, em, lc);
          delta = desired - bx->y;
          if (delta < 0) delta = 0;
        } else if (bx->cs->bottom.unit != L_AUTO && bx->cs->bottom.unit != L_NORMAL) {
          float desired_bottom = scroll_y + (float)s->h - res_l(bx->cs->bottom, 0, em, lc);
          delta = desired_bottom - (bx->y + bx->h);
          if (delta > 0) delta = 0;
        }
        if (delta != 0) sticky.push_back({bx, delta});
      }
      for (Box* k : bx->kids) collect(k);
      for (Box* o : bx->out_of_flow) collect(o);
    };
    collect(root);
  }
  /* scroll: shift the (read-only) box tree up while painting, restore after.
   * Boxes above the viewport are culled by the clip rect. */
  bool shifted = root && scroll_y != 0;
  if (shifted) offset_box_recursive(root, 0, -scroll_y);
  for (auto& sp : sticky) offset_box_recursive(sp.first, 0, sp.second);
  if (root) paint_box(root, ctx);
  for (auto& sp : sticky) offset_box_recursive(sp.first, 0, -sp.second);
  if (shifted) offset_box_recursive(root, 0, scroll_y);
  draw_scrollbar(*s, doc, root, scroll_y);
}

} /* namespace sqrl */
