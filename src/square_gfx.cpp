/* =====================================================================
 *  Square Browser Engine — square_gfx.cpp
 *  Software rasterizer: rounded rects, gradients, shadows, images,
 *  glyphs (embedded Inter + system fonts), paths, SVG, PNG export.
 *  Pixel format: 32bpp BGRA in memory (0xAARRGGBB on little-endian),
 *  straight (non-premultiplied) alpha, src-over compositing.
 * ===================================================================== */
#include "square_gfx_internal.h"

#include <algorithm>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cctype>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../thirdparty/stb_image_write.h"

namespace sqrl {

void Surface::alloc(int ww, int hh) {
  free_pix();
  w = ww;
  h = hh;
  stride = (w * 4 + 31) & ~31;
  pix = (uint8_t*)calloc(1, (size_t)stride * (h > 0 ? h : 1));
  own = true;
}
void Surface::free_pix() {
  if (own && pix) free(pix);
  pix = nullptr;
  w = h = stride = 0;
  own = false;
}

/* ============================ blending ============================ */
uint32_t blend_src_over(uint32_t d, uint32_t s) {
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
uint32_t scale_alpha(uint32_t c, float k) {
  uint32_t a = (uint32_t)(((c >> 24) & 255) * k + 0.5f);
  if (a > 255) a = 255;
  return (a << 24) | (c & 0xFFFFFF);
}

/* ============================ rounded rect SDF ============================ */
/* p relative to box center; he = half extents; r = tl,tr,br,bl (already clamped) */
static inline float sdf_round_rect(float px, float py, float hx, float hy, const float r[4]) {
  float rad;
  if (px <= 0 && py <= 0) rad = r[0];
  else if (px >= 0 && py <= 0) rad = r[1];
  else if (px >= 0 && py >= 0) rad = r[2];
  else rad = r[3];
  float cx = hx - rad, cy = hy - rad;
  float qx = fabsf(px) - cx, qy = fabsf(py) - cy;
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  float d_out = sqrtf(ox * ox + oy * oy);
  float d_in = qx > qy ? qx : qy;
  return (d_out > 0 ? d_out : 0) + (d_in < 0 ? d_in : (qx > 0 || qy > 0 ? 0 : d_in)) - rad;
}
static inline float cov_round_rect(float px, float py, float x, float y, float w, float h, const float r[4]) {
  float d = sdf_round_rect(px - (x + w / 2), py - (y + h / 2), w / 2, h / 2, r);
  return clamp01(0.5f - d);
}
void clamp_radii(const float in[4], float w, float h, float out[4]) {
  float k = 1e9f;
  for (int i = 0; i < 4; i++) if (in[i] > 0) k = std::min(k, in[i]);
  if (k < 1e8f) {
    /* scale so sum of adjacent radii fits */
    float s1 = (in[0] > 0 ? in[0] : 0) + (in[1] > 0 ? in[1] : 0);
    float s2 = (in[1] > 0 ? in[1] : 0) + (in[2] > 0 ? in[2] : 0);
    float s3 = (in[2] > 0 ? in[2] : 0) + (in[3] > 0 ? in[3] : 0);
    float s4 = (in[3] > 0 ? in[3] : 0) + (in[0] > 0 ? in[0] : 0);
    float m = std::max(std::max(s1, s2), std::max(s3, s4));
    float lim = std::min(w, h);
    if (m > lim) k = lim / m;
    else k = 1;
  } else k = 1;
  for (int i = 0; i < 4; i++) {
    float v = in[i];
    if (v < 0) v = 0;
    v *= k;                    /* scale adjacent-sum overflow first... */
    if (v > w / 2) v = w / 2;  /* ...then clamp to half extents (order matters:
                                * clamping first double-shrinks 999px radii) */
    if (v > h / 2) v = h / 2;
    out[i] = v;
  }
}

/* clip test: returns coverage factor considering axis clip + rounded clip */
float clip_cov(float px, float py, const Rect* clip, const RoundedClip* rc) {
  if (clip) {
    if (px < clip->x || px >= clip->x + clip->w || py < clip->y || py >= clip->y + clip->h) return 0;
  }
  if (rc && rc->active) {
    float rr[4];
    clamp_radii(rc->radius, rc->bounds.w, rc->bounds.h, rr);
    return cov_round_rect(px, py, rc->bounds.x, rc->bounds.y, rc->bounds.w, rc->bounds.h, rr);
  }
  return 1;
}

void bounds_intersect(const Rect& r, const Rect* clip, int* x0, int* y0, int* x1, int* y1, const Surface& s) {
  float fx0 = r.x, fy0 = r.y, fx1 = r.x + r.w, fy1 = r.y + r.h;
  if (clip) {
    fx0 = std::max(fx0, clip->x); fy0 = std::max(fy0, clip->y);
    fx1 = std::min(fx1, clip->x + clip->w); fy1 = std::min(fy1, clip->y + clip->h);
  }
  *x0 = std::max(0, (int)floorf(fx0));
  *y0 = std::max(0, (int)floorf(fy0));
  *x1 = std::min(s.w, (int)ceilf(fx1));
  *y1 = std::min(s.h, (int)ceilf(fy1));
}

/* ============================ fill_rect ============================ */
void gfx_fill_rect(Surface& s, const Rect& r, Color c, const float radius[4],
                   const Rect* clip, const RoundedClip* rc) {
  if (c.a == 0 || r.empty()) return;
  float rad[4] = {0, 0, 0, 0};
  bool rounded = false;
  if (radius) {
    clamp_radii(radius, r.w, r.h, rad);
    rounded = rad[0] > 0 || rad[1] > 0 || rad[2] > 0 || rad[3] > 0;
  }
  int x0, y0, x1, y1;
  bounds_intersect(r, clip, &x0, &y0, &x1, &y1, s);
  if (x0 >= x1 || y0 >= y1) return;
  uint32_t col = color_u32(c);
  for (int y = y0; y < y1; y++) {
    uint32_t* row = s.row32(y);
    float py = y + 0.5f;
    for (int x = x0; x < x1; x++) {
      float px = x + 0.5f;
      float cov;
      if (rounded) cov = cov_round_rect(px, py, r.x, r.y, r.w, r.h, rad);
      else cov = 1;
      if (cov < 1) {
        if (rc && rc->active) cov *= 1; /* rc applied below */
        float rcov = clip_cov(px, py, clip, rc);
        cov *= rcov;
      } else {
        cov = clip_cov(px, py, clip, rc);
      }
      if (cov <= 0) continue;
      uint32_t src = cov >= 1 ? col : scale_alpha(col, cov);
      row[x] = blend_src_over(row[x], src);
    }
  }
}

/* ============================ gradients ============================ */
void build_lut(const std::vector<GradStop>& stops, GradLUT* lut) {
  std::vector<GradStop> st = stops;
  /* CSS: unset stop positions are distributed evenly between their defined
   * neighbours (first unset → 0, last unset → 1). Raw parse stores pos=-1. */
  if (!st.empty()) {
    if (st.front().pos < 0) st.front().pos = 0;
    if (st.back().pos < 0) st.back().pos = 1;
    size_t i = 1;
    while (i < st.size()) {
      if (st[i].pos < 0) {
        size_t j = i;
        while (j < st.size() && st[j].pos < 0) j++;
        float lo = st[i - 1].pos;
        float hi = j < st.size() ? st[j].pos : 1.f;
        size_t run = j - i + 1;
        for (size_t k = i; k < j; k++)
          st[k].pos = lo + (hi - lo) * (float)(k - i + 1) / (float)run;
        i = j;
      } else {
        i++;
      }
    }
  }
  std::sort(st.begin(), st.end(), [](const GradStop& a, const GradStop& b) { return a.pos < b.pos; });
  if (st.empty()) { for (int i = 0; i < 1024; i++) lut->e[i] = 0; return; }
  if (st.front().pos > 0) st.insert(st.begin(), GradStop{0, st.front().c});
  if (st.back().pos < 1) st.push_back(GradStop{1, st.back().c});
  size_t seg = 0;
  for (int i = 0; i < 1024; i++) {
    float t = i / 1023.0f;
    while (seg + 1 < st.size() - 1 && t > st[seg + 1].pos) seg++;
    const GradStop& a = st[seg];
    const GradStop& b = st[std::min(seg + 1, st.size() - 1)];
    float span = b.pos - a.pos;
    float f = span > 1e-6f ? (t - a.pos) / span : 0;
    f = clamp01(f);
    uint32_t aa = (uint32_t)(a.c.a + (b.c.a - a.c.a) * f);
    uint32_t rr = (uint32_t)(a.c.r + (b.c.r - a.c.r) * f);
    uint32_t gg = (uint32_t)(a.c.g + (b.c.g - a.c.g) * f);
    uint32_t bb = (uint32_t)(a.c.b + (b.c.b - a.c.b) * f);
    lut->e[i] = (aa << 24) | (rr << 16) | (gg << 8) | bb;
  }
}

void gfx_fill_gradient(Surface& s, const Rect& r, const GradSpec& g,
                       const Rect* clip, const RoundedClip* rc) {
  if (r.empty() || g.stops.empty()) return;
  int x0, y0, x1, y1;
  bounds_intersect(r, clip, &x0, &y0, &x1, &y1, s);
  if (x0 >= x1 || y0 >= y1) return;
  GradLUT lut;
  build_lut(g.stops, &lut);
  float a = g.angle * 3.14159265f / 180.f;
  float ux = sinf(a), uy = -cosf(a);
  /* projection range over corners */
  float corners_x[4] = {r.x, r.x + r.w, r.x + r.w, r.x};
  float corners_y[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
  float pmin = 1e9, pmax = -1e9;
  for (int i = 0; i < 4; i++) {
    float p = corners_x[i] * ux + corners_y[i] * uy;
    pmin = std::min(pmin, p);
    pmax = std::max(pmax, p);
  }
  float prad = g.radius;
  if (g.kind == 3 && prad < 0) {
    prad = 0;
    float ccx = r.x + g.cx * r.w, ccy = r.y + g.cy * r.h;
    for (int i = 0; i < 4; i++) {
      float d = sqrtf((corners_x[i] - ccx) * (corners_x[i] - ccx) + (corners_y[i] - ccy) * (corners_y[i] - ccy));
      prad = std::max(prad, d);
    }
  }
  float span_inv = pmax - pmin > 1e-6f ? 1.f / (pmax - pmin) : 0;
  for (int y = y0; y < y1; y++) {
    uint32_t* row = s.row32(y);
    float py = y + 0.5f;
    for (int x = x0; x < x1; x++) {
      float px = x + 0.5f;
      float cov = clip_cov(px, py, clip, rc);
      if (cov <= 0) continue;
      float t;
      if (g.kind == 2) {
        t = (px * ux + py * uy - pmin) * span_inv;
      } else {
        float ccx = r.x + g.cx * r.w, ccy = r.y + g.cy * r.h;
        t = sqrtf((px - ccx) * (px - ccx) + (py - ccy) * (py - ccy)) / std::max(prad, 1e-3f);
      }
      int idx = (int)(clamp01(t) * 1023.f + 0.5f);
      uint32_t src = scale_alpha(lut.e[idx], cov);
      row[x] = blend_src_over(row[x], src);
    }
  }
}

/* ============================ string helpers ============================ */
std::string lower_copy(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
std::string trim_copy(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}
/* ============================ shadows ============================ */
void gfx_draw_shadow(Surface& s, const Rect& r, const float radius[4], const Shadow& sh,
                     const Rect* clip) {
  if (r.empty()) return;
  if (sh.inset) { engine_log(2, "inset shadows not supported"); return; }
  float rad[4] = {0, 0, 0, 0};
  if (radius) clamp_radii(radius, r.w, r.h, rad);
  int pad = (int)(sh.blur * 1.5f + fabsf(sh.dx) + fabsf(sh.dy) + 8);
  float spread = sh.spread;
  Rect er = {r.x - spread - pad, r.y - spread - pad, r.w + 2 * spread + 2 * pad, r.h + 2 * spread + 2 * pad};
  int bw = (int)ceilf(er.w), bh = (int)ceilf(er.h);
  if (bw <= 0 || bh <= 0 || bw > 4096 || bh > 4096) return;
  std::vector<float> cov((size_t)bw * bh, 0.f);
  float rr[4];
  clamp_radii(radius ? radius : rad, r.w + 2 * spread, r.h + 2 * spread, rr);
  float k = 1;
  if (spread > 0) {
    float m = 2 * spread;
    float lim = std::min(r.w, r.h);
    if (m > lim) k = lim / m;
  }
  for (int y = 0; y < bh; y++) {
    for (int x = 0; x < bw; x++) {
      float px = er.x + x + 0.5f, py = er.y + y + 0.5f;
      float rad2[4];
      for (int i = 0; i < 4; i++) rad2[i] = (radius ? radius[i] : 0) + spread * k;
      cov[(size_t)y * bw + x] = cov_round_rect(px, py, r.x - spread, r.y - spread,
                                               r.w + 2 * spread, r.h + 2 * spread, rad2);
    }
  }
  /* box blur x3 */
  int br = std::max(1, (int)(sh.blur / 2));
  if (sh.blur > 0) {
    std::vector<float> tmp((size_t)bw * bh);
    for (int pass = 0; pass < 3; pass++) {
      /* horizontal */
      for (int y = 0; y < bh; y++) {
        float acc = 0;
        int cnt = 0;
        for (int x = -br; x <= br; x++) {
          int xx = std::max(0, std::min(bw - 1, x));
          acc += cov[(size_t)y * bw + xx]; cnt++;
        }
        for (int x = 0; x < bw; x++) {
          tmp[(size_t)y * bw + x] = acc / cnt;
          int x_add = std::min(bw - 1, x + br + 1);
          int x_sub = std::max(0, x - br);
          acc += cov[(size_t)y * bw + x_add] - cov[(size_t)y * bw + x_sub];
        }
      }
      /* vertical */
      for (int x = 0; x < bw; x++) {
        float acc = 0;
        int cnt = 0;
        for (int y = -br; y <= br; y++) {
          int yy = std::max(0, std::min(bh - 1, y));
          acc += tmp[(size_t)yy * bw + x]; cnt++;
        }
        for (int y = 0; y < bh; y++) {
          cov[(size_t)y * bw + x] = acc / cnt;
          int y_add = std::min(bh - 1, y + br + 1);
          int y_sub = std::max(0, y - br);
          acc += tmp[(size_t)y_add * bw + x] - tmp[(size_t)y_sub * bw + x];
        }
      }
    }
  }
  uint32_t col = color_u32(Color{sh.color.a, sh.color.r, sh.color.g, sh.color.b});
  int ox = (int)roundf(sh.dx), oy = (int)roundf(sh.dy);
  /* the cov buffer covers the rect starting at (er.x, er.y) — map surface
   * coords into buffer coords (buffer origin != surface origin) */
  int bx0 = (int)floorf(er.x), by0 = (int)floorf(er.y);
  int x0 = std::max(0, bx0 - ox), y0 = std::max(0, by0 - oy);
  int x1 = std::min(s.w, bx0 + bw - ox), y1 = std::min(s.h, by0 + bh - oy);
  for (int y = y0; y < y1; y++) {
    uint32_t* row = s.row32(y);
    for (int x = x0; x < x1; x++) {
      float px = x + 0.5f, py = y + 0.5f;
      float rc_cov = 1;
      if (clip && !clip->contains(px, py)) continue;
      float c = cov[(size_t)(y + oy - by0) * bw + (x + ox - bx0)];
      if (c <= 0.002f) continue;
      uint32_t src = scale_alpha(col, c * rc_cov);
      row[x] = blend_src_over(row[x], src);
    }
  }
}

/* ============================ paths ============================ */
struct PathEdge { float x0, y0, x1, y1; };

static void path_coverage(const PathData& p, int bw, int bh, const Rect& bounds, std::vector<float>& cov) {
  /* build subpaths: vector of edges */
  std::vector<PathEdge> edges;
  size_t i = 0, cmdi = 0;
  float cx = 0, cy = 0, sx = 0, sy = 0;
  for (cmdi = 0; cmdi < p.cmds.size(); cmdi++) {
    uint8_t cmd = p.cmds[cmdi];
    if (cmd == 0) { /* move */
      if (sx != cx || sy != cy) edges.push_back({cx, cy, sx, sy});
      cx = p.pts[i]; cy = p.pts[i + 1]; i += 2;
      sx = cx; sy = cy;
    } else if (cmd == 1) { /* line */
      float nx = p.pts[i], ny = p.pts[i + 1];
      i += 2;
      edges.push_back({cx, cy, nx, ny});
      cx = nx; cy = ny;
    } else if (cmd == 2) { /* close */
      if (sx != cx || sy != cy) edges.push_back({cx, cy, sx, sy});
      cx = sx; cy = sy;
    }
  }
  if (sx != cx || sy != cy) edges.push_back({cx, cy, sx, sy});
  if (edges.empty()) return;
  int samples = 4;
  float sstep = 1.f / samples;
  for (int py = 0; py < bh; py++) {
    for (int px = 0; px < bw; px++) {
      float acc = 0;
      for (int sy2 = 0; sy2 < samples; sy2++) {
        for (int sx2 = 0; sx2 < samples; sx2++) {
          float spx = bounds.x + px + (sx2 + 0.5f) * sstep;
          float spy = bounds.y + py + (sy2 + 0.5f) * sstep;
          int winding = 0;
          int crossings = 0;
          for (auto& e : edges) {
            float ex0 = std::min(e.x0, e.x1), ex1 = std::max(e.x0, e.x1);
            if (spx < ex0 || spx >= ex1) continue;
            float y_at = e.y0 + (e.y1 - e.y0) * (spx - e.x0) / (e.x1 != e.x0 ? (e.x1 - e.x0) : 1);
            if (y_at < spy) {
              crossings++;
              if (e.y1 > e.y0) winding++;
              else winding--;
            }
          }
          if (p.fill_rule == 1) { if (crossings & 1) acc += 1; }
          else if (winding != 0) acc += 1;
        }
      }
      cov[(size_t)py * bw + px] = acc / (samples * samples);
    }
  }
}

void gfx_draw_path(Surface& s, const PathData& p, const Rect* clip) {
  if (p.pts.empty() || p.cmds.empty()) return;
  Rect bounds;
  float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
  for (size_t i = 0; i + 1 < p.pts.size(); i += 2) {
    minx = std::min(minx, p.pts[i]); maxx = std::max(maxx, p.pts[i]);
    miny = std::min(miny, p.pts[i + 1]); maxy = std::max(maxy, p.pts[i + 1]);
  }
  bounds = {floorf(minx), floorf(miny), ceilf(maxx - minx) + 2, ceilf(maxy - miny) + 2};
  if (clip) {
    float cx0 = std::max(bounds.x, clip->x), cy0 = std::max(bounds.y, clip->y);
    float cx1 = std::min(bounds.x + bounds.w, clip->x + clip->w);
    float cy1 = std::min(bounds.y + bounds.h, clip->y + clip->h);
    if (cx1 <= cx0 || cy1 <= cy0) return;
    bounds = {cx0, cy0, cx1 - cx0, cy1 - cy0};
  }
  int bw = (int)ceilf(bounds.w), bh = (int)ceilf(bounds.h);
  if (bw <= 0 || bh <= 0 || bw * bh > 64 * 1024 * 1024) return;
  std::vector<float> cov((size_t)bw * bh, 0.f);
  path_coverage(p, bw, bh, bounds, cov);
  if (p.fill.a > 0) {
    uint32_t col = color_u32(p.fill);
    for (int y = 0; y < bh && y < s.h; y++) {
      int yy = (int)bounds.y + y;
      if (yy < 0 || yy >= s.h) continue;
      uint32_t* row = s.row32(yy);
      for (int x = 0; x < bw && x < s.w; x++) {
        int xx = (int)bounds.x + x;
        if (xx < 0 || xx >= s.w) continue;
        float c = cov[(size_t)y * bw + x];
        if (c <= 0.002f) continue;
        row[xx] = blend_src_over(row[xx], scale_alpha(col, c));
      }
    }
  }
  if (p.stroke.a > 0) {
    /* stroke: quads along segments + round joints */
    size_t i = 0;
    float cx = 0, cy = 0;
    bool have = false;
    float hw = std::max(0.5f, p.stroke_w / 2);
    uint32_t col = color_u32(p.stroke);
    auto dot = [&](float x, float y) {
      for (int dy = -(int)ceilf(hw) - 1; dy <= (int)ceilf(hw) + 1; dy++) {
        for (int dx = -(int)ceilf(hw) - 1; dx <= (int)ceilf(hw) + 1; dx++) {
          int xx = (int)floorf(x) + dx, yy = (int)floorf(y) + dy;
          if (xx < 0 || yy < 0 || xx >= s.w || yy >= s.h) continue;
          if (dx * dx + dy * dy > (hw + 0.5f) * (hw + 0.5f)) continue;
          s.row32(yy)[xx] = blend_src_over(s.row32(yy)[xx], col);
        }
      }
    };
    auto seg = [&](float x0, float y0, float x1, float y1) {
      float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
      int steps = std::max(1, (int)(len * 2));
      for (int k = 0; k <= steps; k++) {
        float t = (float)k / steps;
        dot(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
      }
    };
    for (uint8_t cmd : p.cmds) {
      if (cmd == 0) {
        if (have) dot(cx, cy);
        cx = p.pts[i]; cy = p.pts[i + 1]; i += 2;
        have = true;
        dot(cx, cy);
      } else if (cmd == 1) {
        float nx = p.pts[i], ny = p.pts[i + 1];
        i += 2;
        seg(cx, cy, nx, ny);
        cx = nx; cy = ny;
      } else if (cmd == 2) {
        seg(cx, cy, cx, cy);
        have = false;
      }
    }
  }
}

/* ============================ blit ============================ */
void gfx_blit(Surface& dst, const Surface& src, float alpha, const Rect* clip,
              const RoundedClip* rc) {
  int x0 = 0, y0 = 0;
  int w = std::min(dst.w, src.w), h = std::min(dst.h, src.h);
  /* respect the clip rect (e.g. reduced by a parent's overflow:hidden) */
  if (clip && clip->w > 0 && clip->h > 0) {
    x0 = std::max(0, (int)floorf(clip->x));
    y0 = std::max(0, (int)floorf(clip->y));
    int x1 = std::min(w, (int)ceilf(clip->x + clip->w));
    int y1 = std::min(h, (int)ceilf(clip->y + clip->h));
    w = x1 - x0;
    h = y1 - y0;
  }
  if (w <= 0 || h <= 0) return;
  for (int y = y0; y < y0 + h; y++) {
    uint32_t* dr = dst.row32(y);
    uint32_t* sr = src.row32(y);
    for (int x = x0; x < x0 + w; x++) {
      float cov = rc && rc->active ? clip_cov(x + 0.5f, y + 0.5f, nullptr, rc) : 1;
      if (cov <= 0) continue;
      uint32_t sv = sr[x];
      if (!(sv >> 24)) continue;
      if (alpha >= 1 && cov >= 1) {
        if ((sv >> 24) == 255) dr[x] = sv | 0xFF000000;
        else dr[x] = blend_src_over(dr[x], sv);
      } else {
        dr[x] = blend_src_over(dr[x], scale_alpha(sv, alpha * cov));
      }
    }
  }
}

/* ============================ PNG export ============================ */
bool gfx_write_png(const char* path, const uint8_t* bgra, int w, int h, int stride) {
  if (!path || !bgra || w <= 0 || h <= 0) return false;
  std::vector<uint8_t> rgba((size_t)w * h * 4);
  for (int y = 0; y < h; y++) {
    const uint32_t* src = (const uint32_t*)(bgra + (size_t)y * stride);
    uint8_t* dst = rgba.data() + (size_t)y * w * 4;
    for (int x = 0; x < w; x++) {
      uint32_t c = src[x];
      dst[x * 4] = (c >> 16) & 255;
      dst[x * 4 + 1] = (c >> 8) & 255;
      dst[x * 4 + 2] = c & 255;
      dst[x * 4 + 3] = (c >> 24) & 255;
    }
  }
  return stbi_write_png(path, w, h, 4, rgba.data(), w * 4) != 0;
}
/* ============================ blit with matrix + region blur ============================ */
void gfx_blit_matrix(Surface& dst, const Surface& src, const float mat[6],
                     float pivot_x, float pivot_y, float alpha, const Rect* clip) {
  /* src coords -> dst coords: d = M * (s - pivot) + pivot */
  float det = mat[0] * mat[3] - mat[1] * mat[2];
  if (fabsf(det) < 1e-9f) return;
  /* transform src corners to find dst bbox */
  float cs[4][2] = {{0, 0}, {(float)src.w, 0}, {(float)src.w, (float)src.h}, {0, (float)src.h}};
  float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
  for (auto& p : cs) {
    float rx = p[0] - pivot_x, ry = p[1] - pivot_y;
    float tx = mat[0] * rx + mat[2] * ry + mat[4] + pivot_x;
    float ty = mat[1] * rx + mat[3] * ry + mat[5] + pivot_y;
    minx = std::min(minx, tx); miny = std::min(miny, ty);
    maxx = std::max(maxx, tx); maxy = std::max(maxy, ty);
  }
  int x0 = std::max(0, (int)floorf(minx));
  int y0 = std::max(0, (int)floorf(miny));
  int x1 = std::min(dst.w, (int)ceilf(maxx));
  int y1 = std::min(dst.h, (int)ceilf(maxy));
  if (clip) {
    x0 = std::max(x0, (int)clip->x);
    y0 = std::max(y0, (int)clip->y);
    x1 = std::min(x1, (int)(clip->x + clip->w));
    y1 = std::min(y1, (int)(clip->y + clip->h));
  }
  /* inverse matrix: s = M^-1 * (d - pivot) + pivot */
  float i00 = mat[3] / det, i01 = -mat[2] / det, i10 = -mat[1] / det, i11 = mat[0] / det;
  for (int y = y0; y < y1; y++) {
    uint32_t* dr = dst.row32(y);
    for (int x = x0; x < x1; x++) {
      float dx = x + 0.5f - pivot_x - mat[4];
      float dy = y + 0.5f - pivot_y - mat[5];
      float sx = i00 * dx + i01 * dy + pivot_x;
      float sy = i10 * dx + i11 * dy + pivot_y;
      if (sx < -0.5f || sy < -0.5f || sx > src.w - 0.5f || sy > src.h - 0.5f) continue;
      /* nearest for speed with alpha blend */
      int ix = (int)(sx + 0.5f), iy = (int)(sy + 0.5f);
      ix = std::max(0, std::min(src.w - 1, ix));
      iy = std::max(0, std::min(src.h - 1, iy));
      uint32_t sc = src.row32(iy)[ix];
      uint32_t sa = (sc >> 24) & 255;
      if (!sa) continue;
      uint32_t a = (uint32_t)(sa * alpha);
      if (a > 255) a = 255;
      dr[x] = blend_src_over(dr[x], (a << 24) | (sc & 0xFFFFFF));
    }
  }
}

void gfx_blur_region(Surface& s, const Rect& r, float radius_px) {
  int x0 = std::max(0, (int)floorf(r.x));
  int y0 = std::max(0, (int)floorf(r.y));
  int x1 = std::min(s.w, (int)ceilf(r.x + r.w));
  int y1 = std::min(s.h, (int)ceilf(r.y + r.h));
  int bw = x1 - x0, bh = y1 - y0;
  if (bw <= 2 || bh <= 2 || bw * bh > 16 * 1024 * 1024) return;
  int br = std::max(1, (int)(radius_px / 2));
  /* extract region */
  std::vector<uint32_t> buf((size_t)bw * bh);
  for (int y = 0; y < bh; y++) memcpy(&buf[(size_t)y * bw], s.row32(y0 + y) + x0, bw * 4);
  std::vector<uint32_t> tmp((size_t)bw * bh);
  for (int pass = 0; pass < 3; pass++) {
    /* horizontal */
    for (int y = 0; y < bh; y++) {
      uint32_t acc[4] = {0, 0, 0, 0};
      int cnt = 0;
      for (int x = -br; x <= br; x++) {
        int xx = std::max(0, std::min(bw - 1, x));
        uint32_t c = buf[(size_t)y * bw + xx];
        acc[0] += (c >> 24) & 255; acc[1] += (c >> 16) & 255; acc[2] += (c >> 8) & 255; acc[3] += c & 255;
        cnt++;
      }
      for (int x = 0; x < bw; x++) {
        tmp[(size_t)y * bw + x] = ((acc[0] / cnt) << 24) | ((acc[1] / cnt) << 16) | ((acc[2] / cnt) << 8) | (acc[3] / cnt);
        int xa = std::min(bw - 1, x + br + 1), xs = std::max(0, x - br);
        uint32_t ca = buf[(size_t)y * bw + xa], csr = buf[(size_t)y * bw + xs];
        acc[0] += ((ca >> 24) & 255) - ((csr >> 24) & 255);
        acc[1] += ((ca >> 16) & 255) - ((csr >> 16) & 255);
        acc[2] += ((ca >> 8) & 255) - ((csr >> 8) & 255);
        acc[3] += (ca & 255) - (csr & 255);
      }
    }
    /* vertical */
    for (int x = 0; x < bw; x++) {
      uint32_t acc[4] = {0, 0, 0, 0};
      int cnt = 0;
      for (int y = -br; y <= br; y++) {
        int yy = std::max(0, std::min(bh - 1, y));
        uint32_t c = tmp[(size_t)yy * bw + x];
        acc[0] += (c >> 24) & 255; acc[1] += (c >> 16) & 255; acc[2] += (c >> 8) & 255; acc[3] += c & 255;
        cnt++;
      }
      for (int y = 0; y < bh; y++) {
        buf[(size_t)y * bw + x] = ((acc[0] / cnt) << 24) | ((acc[1] / cnt) << 16) | ((acc[2] / cnt) << 8) | (acc[3] / cnt);
        int ya = std::min(bh - 1, y + br + 1), ys = std::max(0, y - br);
        uint32_t ca = tmp[(size_t)ya * bw + x], csr = tmp[(size_t)ys * bw + x];
        acc[0] += ((ca >> 24) & 255) - ((csr >> 24) & 255);
        acc[1] += ((ca >> 16) & 255) - ((csr >> 16) & 255);
        acc[2] += ((ca >> 8) & 255) - ((csr >> 8) & 255);
        acc[3] += (ca & 255) - (csr & 255);
      }
    }
  }
  for (int y = 0; y < bh; y++) memcpy(s.row32(y0 + y) + x0, &buf[(size_t)y * bw], bw * 4);
}

} /* namespace sqrl */
