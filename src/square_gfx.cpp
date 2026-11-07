/* =====================================================================
 *  Square Browser Engine — square_gfx.cpp
 *  Software rasterizer: rounded rects, gradients, shadows, images,
 *  glyphs (embedded Inter + system fonts), paths, SVG, PNG export.
 *  Pixel format: 32bpp BGRA in memory (0xAARRGGBB on little-endian),
 *  straight (non-premultiplied) alpha, src-over compositing.
 * ===================================================================== */
#include "square_internal.h"

#include <algorithm>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cctype>
#include <deque>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STB_IMAGE_STATIC
#include "../thirdparty/stb_image.h"
/* full WebP support (VP8 lossy + VP8L lossless + alpha) — vendored libwebp decoder */
#include "webp/decode.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../thirdparty/stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../thirdparty/stb_truetype.h"
#include "../thirdparty/fonts_embed.h"

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
static inline uint32_t blend_src_over(uint32_t d, uint32_t s) {
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
static inline uint32_t scale_alpha(uint32_t c, float k) {
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
static inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static inline float cov_round_rect(float px, float py, float x, float y, float w, float h, const float r[4]) {
  float d = sdf_round_rect(px - (x + w / 2), py - (y + h / 2), w / 2, h / 2, r);
  return clamp01(0.5f - d);
}
static void clamp_radii(const float in[4], float w, float h, float out[4]) {
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
    if (v > w / 2) v = w / 2;
    if (v > h / 2) v = h / 2;
    out[i] = v * k;
  }
}

/* clip test: returns coverage factor considering axis clip + rounded clip */
static inline float clip_cov(float px, float py, const Rect* clip, const RoundedClip* rc) {
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

static void bounds_intersect(const Rect& r, const Rect* clip, int* x0, int* y0, int* x1, int* y1, const Surface& s) {
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
struct GradLUT { uint32_t e[1024]; };
static void build_lut(const std::vector<GradStop>& stops, GradLUT* lut) {
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
static std::string lower_copy(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
static std::string trim_copy(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

/* ============================ fonts ============================ */
struct FontEntry {
  stbtt_fontinfo info;
  float size;
  float scale;
  float ascent, descent;   /* px, descent positive */
  int weight;
  bool italic;
  std::vector<uint8_t> data; /* owned font bytes (system fonts) */
};
static std::deque<FontEntry*> g_fonts;
static std::mutex g_font_mtx;
static std::unordered_map<std::string, FontEntry*> g_font_cache;
static std::vector<std::string> g_system_font_files;
static bool g_system_scanned = false;

struct FontSource {
  const unsigned char* data;
  size_t len;
  std::vector<uint8_t> owned;
  int weight;
};
static std::vector<FontSource> g_inter_sources;
static bool g_inter_loaded = false;

static void load_inter_sources() {
  if (g_inter_loaded) return;
  g_inter_loaded = true;
  g_inter_sources.push_back({sqrl_font_Inter_Regular, sqrl_font_Inter_Regular_len, {}, 400});
  g_inter_sources.push_back({sqrl_font_Inter_Medium, sqrl_font_Inter_Medium_len, {}, 500});
  g_inter_sources.push_back({sqrl_font_Inter_SemiBold, sqrl_font_Inter_SemiBold_len, {}, 600});
  g_inter_sources.push_back({sqrl_font_Inter_Bold, sqrl_font_Inter_Bold_len, {}, 700});
  g_inter_sources.push_back({sqrl_font_Inter_ExtraBold, sqrl_font_Inter_ExtraBold_len, {}, 800});
}

static void scan_dir_ttf(const std::string& dir, int depth, std::vector<std::string>* out) {
  if (depth > 4) return;
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    std::string full = dir + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) scan_dir_ttf(full, depth + 1, out);
    else {
      std::string ln = lower_copy(name);
      if (ln.size() > 4 && (ln.substr(ln.size() - 4) == ".ttf" || ln.substr(ln.size() - 4) == ".otf"))
        out->push_back(full);
    }
  }
  closedir(d);
}
static void scan_system_fonts() {
  if (g_system_scanned) return;
  g_system_scanned = true;
#ifdef _WIN32
  scan_dir_ttf("C:\\Windows\\Fonts", 0, &g_system_font_files);
#else
  scan_dir_ttf("/usr/share/fonts", 0, &g_system_font_files);
#endif
}

static FontSource* pick_inter(int weight) {
  static const int buckets[5] = {400, 500, 600, 700, 800};
  int best = 0, bestd = 1 << 30;
  for (int i = 0; i < 5; i++) {
    int d = abs(buckets[i] - weight);
    if (d < bestd) { bestd = d; best = i; }
  }
  return &g_inter_sources[best];
}

static FontSource* find_system_font(const std::string& family) {
  scan_system_fonts();
  std::string want = lower_copy(family);
  std::string stem = want;
  for (auto& c : stem) if (c == ' ') c = '_';
  FontSource* partial = nullptr;
  for (auto& path : g_system_font_files) {
    std::string base = path.substr(path.find_last_of("/\\") + 1);
    std::string lbase = lower_copy(base);
    if (lbase.find(want) != std::string::npos || lbase.find(stem) != std::string::npos) {
      FILE* f = fopen(path.c_str(), "rb");
      if (!f) continue;
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      std::vector<uint8_t> buf(n);
      if (fread(buf.data(), 1, n, f) == (size_t)n) {
        fclose(f);
        static FontSource sys;
        sys.data = buf.data();
        sys.len = n;
        sys.owned = std::move(buf);
        sys.weight = 400;
        return &sys;   /* single-slot: last match; fine for fallback use */
      }
      fclose(f);
    }
  }
  (void)partial;
  return nullptr;
}

void* font_resolve(const std::vector<std::string>& families, int weight, bool italic, float size_px) {
  load_inter_sources();
  scan_system_fonts();
  if (size_px < 1) size_px = 1;
  char keybuf[512];
  std::string fam0 = families.empty() ? "inter" : lower_copy(families[0]);
  snprintf(keybuf, sizeof(keybuf), "%s|%d|%d|%.2f", fam0.c_str(), weight, italic ? 1 : 0, size_px);
  std::string key = keybuf;
  std::lock_guard<std::mutex> lk(g_font_mtx);
  auto it = g_font_cache.find(key);
  if (it != g_font_cache.end()) return it->second;

  FontSource* src = nullptr;
  if (fam0.find("inter") != std::string::npos || fam0 == "sans-serif" || fam0 == "system-ui" ||
      fam0 == "-apple-system" || fam0 == "arial" || fam0 == "helvetica" || fam0 == "ui-sans-serif" ||
      fam0.find("segoe") != std::string::npos || fam0.find("roboto") != std::string::npos ||
      fam0 == "" || fam0 == "*" || fam0 == "inherit")
    src = pick_inter(weight);
  else if (fam0.find("mono") != std::string::npos || fam0.find("courier") != std::string::npos)
    src = find_system_font("mono");
  else if (fam0.find("serif") != std::string::npos && fam0.find("sans") == std::string::npos)
    src = find_system_font("serif");
  else if (fam0.find("sc") != std::string::npos || fam0.find("noto") != std::string::npos)
    src = find_system_font(fam0);
  else {
    src = find_system_font(fam0);
    if (!src) src = pick_inter(weight);
  }
  if (!src) return nullptr;

  FontEntry* fe = new FontEntry();
  /* keep system font bytes alive in fe->data; embedded fonts point at static data */
  fe->data = src->owned;
  const unsigned char* fdata = !src->owned.empty() ? fe->data.data() : src->data;
  stbtt_fontinfo* info = &fe->info;
  int offset = stbtt_GetFontOffsetForIndex(fdata, 0);
  if (offset < 0 || !stbtt_InitFont(info, fdata, offset)) {
    delete fe;
    return nullptr;
  }
  fe->size = size_px;
  /* CSS font-size is the EM size — ScaleForMappingEmToPixels maps em→px.
   * (ScaleForPixelHeight scales by ascent+descent, rendering Inter ~25% small.) */
  fe->scale = stbtt_ScaleForMappingEmToPixels(info, size_px);
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
  fe->ascent = ascent * fe->scale;
  fe->descent = -descent * fe->scale;
  fe->weight = weight;
  fe->italic = italic;
  g_fonts.push_back(fe);
  g_font_cache[key] = fe;
  return fe;
}

float font_ascent(void* font) { return font ? ((FontEntry*)font)->ascent : 0; }
float font_descent(void* font) { return font ? ((FontEntry*)font)->descent : 0; }

static inline int utf8_next(const char* s, size_t len, size_t* i, int* cp) {
  if (*i >= len) return 0;
  unsigned char c = (unsigned char)s[*i];
  int codepoint;
  size_t adv;
  if (c < 0x80) { codepoint = c; adv = 1; }
  else if ((c & 0xE0) == 0xC0) { codepoint = c & 0x1F; adv = 2; }
  else if ((c & 0xF0) == 0xE0) { codepoint = c & 0x0F; adv = 3; }
  else if ((c & 0xF8) == 0xF0) { codepoint = c & 0x07; adv = 4; }
  else { codepoint = 0xFFFD; adv = 1; }
  for (size_t k = 1; k < adv && *i + k < len; k++) {
    codepoint = (codepoint << 6) | ((unsigned char)s[*i + k] & 0x3F);
  }
  *i += adv;
  *cp = codepoint;
  return 1;
}

float font_measure_text(void* font, const char* utf8, float letter_spacing) {
  if (!font || !utf8) return 0;
  FontEntry* fe = (FontEntry*)font;
  size_t i = 0, len = strlen(utf8);
  int prev = 0;
  float x = 0;
  bool first = true;
  int cp;
  while (utf8_next(utf8, len, &i, &cp)) {
    int gi = stbtt_FindGlyphIndex(&fe->info, cp);
    if (!first && fe->scale > 0) {
      float kern = stbtt_GetCodepointKernAdvance(&fe->info, prev, cp) * fe->scale;
      x += kern;
    }
    int aw, lsb;
    stbtt_GetGlyphHMetrics(&fe->info, gi, &aw, &lsb);
    x += aw * fe->scale + letter_spacing;
    prev = cp;
    first = false;
  }
  return x;
}

float font_shape(void* font, const char* utf8, float letter_spacing, std::vector<GlyphPos>* out) {
  if (!font) return 0;
  FontEntry* fe = (FontEntry*)font;
  size_t i = 0, len = utf8 ? strlen(utf8) : 0;
  int prev = 0;
  float x = 0;
  bool first = true;
  int cp;
  if (out) out->clear();
  while (utf8_next(utf8, len, &i, &cp)) {
    int gi = stbtt_FindGlyphIndex(&fe->info, cp);
    if (!first) x += stbtt_GetCodepointKernAdvance(&fe->info, prev, cp) * fe->scale;
    if (out) out->push_back(GlyphPos{(uint32_t)gi, x});
    int aw, lsb;
    stbtt_GetGlyphHMetrics(&fe->info, gi, &aw, &lsb);
    x += aw * fe->scale + letter_spacing;
    prev = cp;
    first = false;
  }
  return x;
}

/* glyph bitmap cache */
struct GlyphBitmap {
  int w, h, xoff, yoff;
  std::vector<uint8_t> cov;
};
static std::mutex g_glyph_mtx;
static std::unordered_map<uint64_t, GlyphBitmap> g_glyph_cache;
static size_t g_glyph_bytes = 0;

static GlyphBitmap* glyph_bitmap(FontEntry* fe, uint32_t gi) {
  uint64_t key = ((uint64_t)(uintptr_t)fe << 1) | (fe->italic ? 1 : 0);
  key = key * 1000003 + gi;
  {
    std::lock_guard<std::mutex> lk(g_glyph_mtx);
    auto it = g_glyph_cache.find(key);
    if (it != g_glyph_cache.end()) return &it->second;
  }
  GlyphBitmap gb;
  float shear = fe->italic ? 0.25f : 0;
  unsigned char* bmp = stbtt_GetGlyphBitmap(&fe->info, 0, fe->scale, (int)gi, &gb.w, &gb.h, &gb.xoff, &gb.yoff);
  if (!bmp) {
    gb.w = gb.h = 0;
  } else {
    gb.cov.assign(bmp, bmp + (size_t)gb.w * gb.h);
    stbtt_FreeBitmap(bmp, nullptr);
    if (shear > 0 && gb.h > 0 && gb.w > 0) {
      int extra = (int)ceilf(shear * fe->size);
      int nw = gb.w + extra;
      std::vector<uint8_t> sh((size_t)nw * gb.h, 0);
      for (int y = 0; y < gb.h; y++) {
        int dx = (int)(shear * (fe->ascent - y));   /* top rows shift right */
        for (int x = 0; x < gb.w; x++) {
          int sx = x + dx;
          if (sx >= 0 && sx < nw) sh[(size_t)y * nw + sx] = gb.cov[(size_t)y * gb.w + x];
        }
      }
      gb.w = nw;
      gb.cov = std::move(sh);
    }
  }
  std::lock_guard<std::mutex> lk(g_glyph_mtx);
  g_glyph_bytes += gb.cov.size();
  if (g_glyph_bytes > 256ull * 1024 * 1024) {
    g_glyph_cache.clear();
    g_glyph_bytes = 0;
  }
  auto& slot = g_glyph_cache[key] = std::move(gb);
  return &slot;
}

void gfx_draw_glyphs(Surface& s, void* font, float x, float y_baseline,
                     const std::vector<GlyphPos>& glyphs, Color color, float alpha,
                     const Rect* clip) {
  if (!font || glyphs.empty()) return;
  FontEntry* fe = (FontEntry*)font;
  uint32_t col = color_u32(color);
  for (auto& g : glyphs) {
    if (g.gi == 0) continue;
    GlyphBitmap* gb = glyph_bitmap(fe, g.gi);
    if (!gb || gb->w == 0 || gb->h == 0) continue;
    float gx = x + g.x + gb->xoff;
    float gy = y_baseline + gb->yoff;
    int x0 = std::max(0, (int)floorf(gx));
    int y0 = std::max(0, (int)floorf(gy));
    int x1 = std::min(s.w, (int)ceilf(gx + gb->w));
    int y1 = std::min(s.h, (int)ceilf(gy + gb->h));
    for (int yy = y0; yy < y1; yy++) {
      uint32_t* row = s.row32(yy);
      int sy = yy - (int)floorf(gy);
      if (sy < 0 || sy >= gb->h) continue;
      for (int xx = x0; xx < x1; xx++) {
        if (clip && (xx + 0.5f < clip->x || xx + 0.5f >= clip->x + clip->w)) continue;
        int sx = xx - (int)floorf(gx);
        if (sx < 0 || sx >= gb->w) continue;
        uint8_t c = gb->cov[(size_t)sy * gb->w + sx];
        if (!c) continue;
        float a = (c / 255.0f) * alpha;
        uint32_t src = scale_alpha(col, a);
        row[xx] = blend_src_over(row[xx], src);
      }
    }
  }
}

/* ============================ images ============================ */
struct StoredImage {
  int w, h;
  std::vector<uint32_t> px;  /* 0xAARRGGBB */
};
static std::mutex g_img_mtx;
static std::vector<StoredImage*> g_images; /* 1-based ids */

int image_add_rgba(int w, int h, std::vector<uint8_t> rgba) {
  StoredImage* im = new StoredImage{w, h, {}};
  im->px.resize((size_t)w * h);
  for (size_t i = 0; i < im->px.size(); i++) {
    uint8_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
    im->px[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }
  std::lock_guard<std::mutex> lk(g_img_mtx);
  g_images.push_back(im);
  return (int)g_images.size();
}
bool image_decode(const std::vector<uint8_t>& bytes, int* out_gfx_id, int* out_w, int* out_h) {
  int w, h, comp;
  if (bytes.size() >= 12)
    engine_log(1, "[img-decode] %zu bytes head %02x %02x %02x %02x", bytes.size(), bytes[0],
               bytes[1], bytes[2], bytes[3]);

  /* ---- WebP (RIFF....WEBP) via vendored libwebp decoder ---- */
  if (bytes.size() >= 12 && memcmp(bytes.data(), "RIFF", 4) == 0 &&
      memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
    int wp_w = 0, wp_h = 0;
    uint8_t* rgba = WebPDecodeRGBA(bytes.data(), bytes.size(), &wp_w, &wp_h);
    if (rgba) {
      engine_log(1, "[img-decode] webp ok %dx%d", wp_w, wp_h);
      std::vector<uint8_t> buf(rgba, rgba + (size_t)wp_w * wp_h * 4);
      WebPFree(rgba);
      int id = image_add_rgba(wp_w, wp_h, std::move(buf));
      if (out_gfx_id) *out_gfx_id = id;
      if (out_w) *out_w = wp_w;
      if (out_h) *out_h = wp_h;
      return true;
    }
    engine_log(1, "[img-decode] FAIL: webp decode error");
    return false;
  }

  /* ---- SVG (<svg ...> possibly after <?xml ...?>) → raster via svg_render ---- */
  if (bytes.size() >= 8 && bytes.size() < 4 * 1024 * 1024) {
    size_t scan = std::min(bytes.size(), (size_t)512);
    bool maybe_svg = false;
    for (size_t i = 0; i + 4 <= scan; i++) {
      if (memcmp(bytes.data() + i, "<svg", 4) == 0 || memcmp(bytes.data() + i, "<SVG", 4) == 0) {
        maybe_svg = true;
        break;
      }
    }
    if (maybe_svg) {
      std::string text(bytes.begin(), bytes.end());
      /* intrinsic size: width/height attrs, else viewBox */
      float iw = 0, ih = 0;
      {
        auto find_attr = [&text](const char* name, float* out) -> bool {
          size_t p = text.find(name);
          if (p == std::string::npos) return false;
          p = text.find('=', p + strlen(name));
          if (p == std::string::npos) return false;
          p = text.find('"', p);
          if (p == std::string::npos) return false;
          *out = (float)atof(text.c_str() + p + 1);
          return *out > 0;
        };
        float tw = 0, th = 0;
        if (find_attr("width", &tw) && find_attr("height", &th)) {
          iw = tw;
          ih = th;
        } else {
          /* viewBox="minx miny w h" */
          size_t p = text.find("viewBox");
          if (p == std::string::npos) p = text.find("viewbox");
          if (p != std::string::npos) {
            p = text.find('=', p);
            if (p != std::string::npos) {
              p = text.find('"', p);
              if (p != std::string::npos) {
                float x0, y0, vw2, vh2;
                if (sscanf(text.c_str() + p + 1, "%f %f %f %f", &x0, &y0, &vw2, &vh2) == 4 ||
                    sscanf(text.c_str() + p + 1, "%f,%f,%f,%f", &x0, &y0, &vw2, &vh2) == 4) {
                  if (vw2 > 0 && vh2 > 0) {
                    iw = vw2;
                    ih = vh2;
                  }
                }
              }
            }
          }
        }
      }
      if (iw <= 0 || ih <= 0) {
        iw = 300;
        ih = 150;
      } /* CSS replaced-element default */
      /* rasterize at 2x intrinsic (capped) for crisp downscaling */
      float zoom = 2.0f;
      while ((iw * zoom > 1024 || ih * zoom > 1024) && zoom > 1) zoom *= 0.5f;
      Surface* sv = svg_render(text, iw, ih, Color{0, 0, 0, 255}, zoom);
      if (!sv) return false;
      int sw2 = sv->w, sh2 = sv->h;
      std::vector<uint8_t> buf((size_t)sw2 * sh2 * 4);
      for (int y = 0; y < sh2; y++) {
        uint32_t* row = sv->row32(y);
        for (int x = 0; x < sw2; x++) {
          uint32_t c = row[x];
          buf[((size_t)y * sw2 + x) * 4 + 0] = (uint8_t)((c >> 16) & 255); /* R */
          buf[((size_t)y * sw2 + x) * 4 + 1] = (uint8_t)((c >> 8) & 255);  /* G */
          buf[((size_t)y * sw2 + x) * 4 + 2] = (uint8_t)(c & 255);         /* B */
          buf[((size_t)y * sw2 + x) * 4 + 3] = (uint8_t)((c >> 24) & 255); /* A */
        }
      }
      delete sv;
      engine_log(1, "[img-decode] svg ok %dx%d", sw2, sh2);
      int id = image_add_rgba(sw2, sh2, std::move(buf));
      if (out_gfx_id) *out_gfx_id = id;
      if (out_w) *out_w = sw2;
      if (out_h) *out_h = sh2;
      return true;
    }
  }

  unsigned char* data = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
  if (!data) {
    engine_log(1, "[img-decode] FAIL: %s", stbi_failure_reason());
    return false;
  }
  engine_log(1, "[img-decode] ok %dx%d first px %02x%02x%02x%02x", w, h, data[0], data[1],
             data[2], data[3]);
  std::vector<uint8_t> rgba(data, data + (size_t)w * h * 4);
  stbi_image_free(data);
  int id = image_add_rgba(w, h, std::move(rgba));
  if (out_gfx_id) *out_gfx_id = id;
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  return true;
}
void image_cache_clear() {
  std::lock_guard<std::mutex> lk(g_img_mtx);
  for (auto* p : g_images) delete p;
  g_images.clear();
}

static inline uint32_t sample_bilinear(const StoredImage& im, float fx, float fy) {
  fx = std::max(-1.f, std::min((float)im.w, fx));
  fy = std::max(-1.f, std::min((float)im.h, fy));
  int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
  float tx = fx - x0, ty = fy - y0;
  auto get = [&](int x, int y) -> uint32_t {
    x = std::max(0, std::min(im.w - 1, x));
    y = std::max(0, std::min(im.h - 1, y));
    return im.px[(size_t)y * im.w + x];
  };
  uint32_t c00 = get(x0, y0), c10 = get(x0 + 1, y0), c01 = get(x0, y0 + 1), c11 = get(x0 + 1, y0 + 1);
  uint32_t out = 0;
  for (int ch = 0; ch < 4; ch++) {
    int shift = ch * 8;
    float v00 = ((c00 >> shift) & 255), v10 = ((c10 >> shift) & 255);
    float v01 = ((c01 >> shift) & 255), v11 = ((c11 >> shift) & 255);
    float v = v00 * (1 - tx) * (1 - ty) + v10 * tx * (1 - ty) + v01 * (1 - tx) * ty + v11 * tx * ty;
    out |= (uint32_t)(v + 0.5f) << shift;
  }
  return out;
}

void gfx_draw_image(Surface& s, int gfx_id, const Rect& dst, const Rect* src_crop,
                    uint8_t fit, const Rect* clip, const RoundedClip* rc, float alpha) {
  if (dst.empty()) return;
  StoredImage* im = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_img_mtx);
    if (gfx_id >= 1 && gfx_id <= (int)g_images.size()) im = g_images[gfx_id - 1];
  }
  if (!im || im->w == 0 || im->h == 0) return;
  float sx0 = 0, sy0 = 0, sw = (float)im->w, sh = (float)im->h;
  bool fit_none = false;
  if (src_crop && src_crop->w > 0 && src_crop->h > 0) {
    sx0 = src_crop->x; sy0 = src_crop->y; sw = src_crop->w; sh = src_crop->h;
    fit = 0; /* stretch crop into dst */
  } else if (fit == 1 || fit == 2) { /* cover / contain */
    float scale = fit == 1 ? std::max(dst.w / sw, dst.h / sh) : std::min(dst.w / sw, dst.h / sh);
    sx0 = (sw - dst.w / scale) / 2;
    sy0 = (sh - dst.h / scale) / 2;
    sw = dst.w / scale;
    sh = dst.h / scale;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx0 + sw > im->w) { sw = (float)im->w - sx0; }
    if (sy0 + sh > im->h) { sh = (float)im->h - sy0; }
  } else if (fit == 3) { /* none: intrinsic size, unscaled, clipped to dst */
    fit_none = true;
  }
  int x0, y0, x1, y1;
  bounds_intersect(dst, clip, &x0, &y0, &x1, &y1, s);
  for (int y = y0; y < y1; y++) {
    uint32_t* row = s.row32(y);
    float py = y + 0.5f;
    float fy = fit_none ? sy0 + (py - dst.y) : sy0 + (py - dst.y) * sh / dst.h;
    for (int x = x0; x < x1; x++) {
      float px = x + 0.5f;
      float cov = clip_cov(px, py, clip, rc);
      if (cov <= 0) continue;
      float fx = fit_none ? sx0 + (px - dst.x) : sx0 + (px - dst.x) * sw / dst.w;
      if (fit_none && (fx >= (float)im->w || fy >= (float)im->h)) continue; /* dst larger than image */
      uint32_t sc = sample_bilinear(*im, fx, fy);
      uint32_t src = scale_alpha(sc, alpha * cov);
      row[x] = blend_src_over(row[x], src);
    }
  }
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

/* =====================================================================
 *  SVG renderer
 * ===================================================================== */
struct SvgNode {
  std::string tag;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<SvgNode*> kids;
  SvgNode* parent = nullptr;
  const std::string* attr(const char* n) const {
    for (auto& a : attrs) if (a.first == n) return &a.second;
    return nullptr;
  }
  ~SvgNode() { for (auto k : kids) delete k; }
};

struct SvgMat {
  float m[6] = {1, 0, 0, 1, 0, 0};
  SvgMat() {}
  SvgMat(const float o[6]) { memcpy(m, o, sizeof(m)); }
  void compose(const SvgMat& b) { /* this = this * b (b applied first) */
    float a0 = m[0] * b.m[0] + m[2] * b.m[1];
    float a1 = m[1] * b.m[0] + m[3] * b.m[1];
    float a2 = m[0] * b.m[2] + m[2] * b.m[3];
    float a3 = m[1] * b.m[2] + m[3] * b.m[3];
    float a4 = m[0] * b.m[4] + m[2] * b.m[5] + m[4];
    float a5 = m[1] * b.m[4] + m[3] * b.m[5] + m[5];
    m[0] = a0; m[1] = a1; m[2] = a2; m[3] = a3; m[4] = a4; m[5] = a5;
  }
  void apply(float x, float y, float* ox, float* oy) const {
    *ox = m[0] * x + m[2] * y + m[4];
    *oy = m[1] * x + m[3] * y + m[5];
  }
};

struct SvgState {
  Color fill{0, 0, 0, 255};
  Color stroke{0, 0, 0, 0};
  float stroke_w = 1;
  float opacity = 1, fill_opacity = 1, stroke_opacity = 1;
  uint8_t fill_rule = 0;
  Color color{0, 0, 0, 255};
};

struct SvgGrad {
  bool linear;
  float x1, y1, x2, y2, cx, cy, r, fx, fy;
  bool user_space = false;
  std::vector<GradStop> stops;
  SvgMat mat;
};

struct SvgCtx {
  Surface* surf = nullptr;
  Rect clip;
  std::unordered_map<std::string, SvgGrad> grads;
  std::unordered_map<std::string, SvgNode*> byid;
  Color current_color{0, 0, 0, 255};
  float scale = 1;
};

static bool parse_color_str(const std::string& s, Color* out) {
  std::string v = lower_copy(trim_copy(s));
  if (v.empty() || v == "none" || v == "transparent") { *out = Color{0, 0, 0, 0}; return true; }
  if (v == "currentcolor") return false; /* caller resolves */
  auto hex2 = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  if (v[0] == '#') {
    std::string h = v.substr(1);
    if (h.size() == 3) {
      int r = hex2(h[0]), g = hex2(h[1]), b = hex2(h[2]);
      if (r < 0) return false;
      *out = Color{255, (uint8_t)(r * 17), (uint8_t)(g * 17), (uint8_t)(b * 17)};
      return true;
    }
    if (h.size() == 6) {
      int r = (hex2(h[0]) << 4) | hex2(h[1]);
      int g = (hex2(h[2]) << 4) | hex2(h[3]);
      int b = (hex2(h[4]) << 4) | hex2(h[5]);
      *out = Color{255, (uint8_t)r, (uint8_t)g, (uint8_t)b};
      return true;
    }
    if (h.size() == 8) {
      int r = (hex2(h[0]) << 4) | hex2(h[1]);
      int g = (hex2(h[2]) << 4) | hex2(h[3]);
      int b = (hex2(h[4]) << 4) | hex2(h[5]);
      int a = (hex2(h[6]) << 4) | hex2(h[7]);
      *out = Color{(uint8_t)a, (uint8_t)r, (uint8_t)g, (uint8_t)b};
      return true;
    }
    return false;
  }
  if (v.rfind("rgb", 0) == 0) {
    size_t a = v.find('('), b = v.rfind(')');
    if (a == std::string::npos || b == std::string::npos) return false;
    std::string body = v.substr(a + 1, b - a - 1);
    float vals[4] = {0, 0, 0, 1};
    int n = 0;
    size_t i = 0;
    bool pct = false;
    while (n < 4 && i < body.size()) {
      while (i < body.size() && (body[i] == ' ' || body[i] == ',')) i++;
      size_t st = i;
      while (i < body.size() && (isdigit((unsigned char)body[i]) || body[i] == '.' || body[i] == '-' || body[i] == '%' || body[i] == 'e')) {
        if (body[i] == '%') pct = true;
        i++;
      }
      if (i == st) break;
      vals[n++] = (float)atof(body.substr(st, i - st).c_str());
    }
    if (pct) { vals[0] *= 2.55f; vals[1] *= 2.55f; vals[2] *= 2.55f; }
    if (v.rfind("rgba", 0) == 0 && n == 4) {
      *out = Color{(uint8_t)(vals[3] > 1 ? vals[3] : vals[3] * 255), (uint8_t)vals[0], (uint8_t)vals[1], (uint8_t)vals[2]};
    } else {
      *out = Color{255, (uint8_t)vals[0], (uint8_t)vals[1], (uint8_t)vals[2]};
    }
    return true;
  }
  struct { const char* n; uint32_t v; } named[] = {
    {"white", 0xFFFFFF}, {"black", 0x000000}, {"red", 0xFF0000}, {"green", 0x008000},
    {"lime", 0x00FF00}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00}, {"orange", 0xFFA500},
    {"purple", 0x800080}, {"gray", 0x808080}, {"grey", 0x808080}, {"silver", 0xC0C0C0},
    {"maroon", 0x800000}, {"navy", 0x000080}, {"teal", 0x008080}, {"olive", 0x808000},
    {"cyan", 0x00FFFF}, {"aqua", 0x00FFFF}, {"magenta", 0xFF00FF}, {"fuchsia", 0xFF00FF},
    {"pink", 0xFFC0CB}, {"gold", 0xFFD700}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"lightgray", 0xD3D3D3}, {"darkgray", 0xA9A9A9}, {"whitesmoke", 0xF5F5F5},
    {"gainsboro", 0xDCDCDC}, {"lightsteelblue", 0xB0C4DE}, {"royalblue", 0x4169E1},
  };
  for (auto& n : named) {
    if (v == n.n) {
      *out = Color{255, (uint8_t)(n.v >> 16), (uint8_t)((n.v >> 8) & 255), (uint8_t)(n.v & 255)};
      return true;
    }
  }
  return false;
}

static std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    size_t st = i;
    while (i < s.size() && !isspace((unsigned char)s[i]) && s[i] != ',') i++;
    if (i > st) out.push_back(s.substr(st, i - st));
    if (i < s.size() && s[i] == ',') i++;
  }
  return out;
}

static bool parse_transform(const std::string& t, SvgMat* out) {
  *out = SvgMat();
  size_t i = 0;
  while (i < t.size()) {
    while (i < t.size() && !isalpha((unsigned char)t[i])) i++;
    size_t st = i;
    while (i < t.size() && isalpha((unsigned char)t[i])) i++;
    std::string fn = t.substr(st, i - st);
    while (i < t.size() && t[i] != '(') i++;
    if (i >= t.size()) break;
    i++;
    size_t pe = t.find(')', i);
    if (pe == std::string::npos) break;
    std::string body = t.substr(i, pe - i);
    i = pe + 1;
    auto nums = split_ws(body);
    std::vector<float> v;
    for (auto& nstr : nums) {
      /* may be comma-joined "a,b" */
      auto parts = split_ws(nstr);
      for (auto& p : parts) v.push_back((float)atof(p.c_str()));
      if (parts.empty()) v.push_back((float)atof(nstr.c_str()));
    }
    if (fn == "translate" && v.size() >= 1) {
      float m[6] = {1, 0, 0, 1, v[0], v.size() > 1 ? v[1] : 0};
      out->compose(SvgMat(m));
    } else if (fn == "scale" && v.size() >= 1) {
      float m[6] = {v[0], 0, 0, v.size() > 1 ? v[1] : v[0], 0, 0};
      out->compose(SvgMat(m));
    } else if (fn == "rotate" && v.size() >= 1) {
      float a = v[0] * 3.14159265f / 180.f;
      float ca = cosf(a), sa = sinf(a);
      if (v.size() >= 3) {
        float t1[6] = {1, 0, 0, 1, v[1], v[2]};
        out->compose(SvgMat(t1));
      }
      float m[6] = {ca, sa, -sa, ca, 0, 0};
      out->compose(SvgMat(m));
      if (v.size() >= 3) {
        float t2[6] = {1, 0, 0, 1, -v[1], -v[2]};
        out->compose(SvgMat(t2));
      }
    } else if (fn == "matrix" && v.size() >= 6) {
      float m[6] = {v[0], v[1], v[2], v[3], v[4], v[5]};
      out->compose(SvgMat(m));
    } else if (fn == "skewx") {
      float m[6] = {1, 0, tanf(v[0] * 3.14159265f / 180.f), 1, 0, 0};
      out->compose(SvgMat(m));
    } else if (fn == "skewy") {
      float m[6] = {1, tanf(v[0] * 3.14159265f / 180.f), 0, 1, 0, 0};
      out->compose(SvgMat(m));
    }
  }
  return true;
}

/* SVG path d → flattened polyline (cmds M/L/Z) */
struct SvgPathBuilder {
  std::vector<float> pts;
  std::vector<uint8_t> cmds;
  float cx = 0, cy = 0, sx = 0, sy = 0;
  void move(float x, float y) { cmds.push_back(0); pts.push_back(x); pts.push_back(y); cx = sx = x; cy = sy = y; }
  void line(float x, float y) { cmds.push_back(1); pts.push_back(x); pts.push_back(y); cx = x; cy = y; }
  void close() { cmds.push_back(2); cx = sx; cy = sy; }
  void curve(float x1, float y1, float x2, float y2, float x, float y) {
    int N = 24;
    for (int i = 1; i <= N; i++) {
      float t = (float)i / N;
      float mt = 1 - t;
      float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
      line(a * cx + b * x1 + c * x2 + d * x, a * cy + b * y1 + c * y2 + d * y);
    }
  }
};

/* normalize path arc (SVG spec F.6.5) */
static void arc_to_cubic(SvgPathBuilder& pb, float x0, float y0, float rx, float ry, float phi_deg,
                         bool large_arc, bool sweep, float x, float y) {
  if (rx == 0 || ry == 0) { pb.line(x, y); return; }
  rx = fabsf(rx); ry = fabsf(ry);
  float phi = phi_deg * 3.14159265f / 180.f;
  float cosP = cosf(phi), sinP = sinf(phi);
  float dx2 = (x0 - x) / 2, dy2 = (y0 - y) / 2;
  float x1p = cosP * dx2 + sinP * dy2;
  float y1p = -sinP * dx2 + cosP * dy2;
  float rx2 = rx * rx, ry2 = ry * ry;
  float lam = x1p * x1p / rx2 + y1p * y1p / ry2;
  if (lam > 1) {
    float s = sqrtf(lam);
    rx *= s; ry *= s;
    rx2 = rx * rx; ry2 = ry * ry;
  }
  float sign = (large_arc != sweep) ? 1 : -1;
  float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
  float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
  float co = sign * sqrtf(std::max(0.f, num / std::max(den, 1e-9f)));
  float cxp = co * rx * y1p / ry;
  float cyp = -co * ry * x1p / rx;
  float cx = cosP * cxp - sinP * cyp + (x0 + x) / 2;
  float cy = sinP * cxp + cosP * cyp + (y0 + y) / 2;
  float ang1 = atan2f((y1p - cyp) / ry, (x1p - cxp) / rx);
  float ang2 = atan2f((-y1p - cyp) / ry, (-x1p - cxp) / rx);
  float dAng = ang2 - ang1;
  if (!sweep && dAng > 0) dAng -= 2 * (float)M_PI;
  if (sweep && dAng < 0) dAng += 2 * (float)M_PI;
  int segments = (int)ceilf(fabsf(dAng) / (float)M_PI * 4) + 1;
  for (int i = 0; i < segments; i++) {
    float a0 = ang1 + dAng * i / segments;
    float a1 = ang1 + dAng * (i + 1) / segments;
    float t = 4.0f / 3.0f * tanf((a1 - a0) / 4);
    float p0x = cx + rx * cosf(a0) * cosP - ry * sinf(a0) * sinP;
    float p0y = cy + rx * cosf(a0) * sinP + ry * sinf(a0) * cosP;
    float p3x = cx + rx * cosf(a1) * cosP - ry * sinf(a1) * sinP;
    float p3y = cy + rx * cosf(a1) * sinP + ry * sinf(a1) * cosP;
    float p1x = p0x + t * (-rx * sinf(a0) * cosP - ry * cosf(a0) * sinP);
    float p1y = p0y + t * (-rx * sinf(a0) * sinP + ry * cosf(a0) * cosP);
    float p2x = p3x - t * (-rx * sinf(a1) * cosP - ry * cosf(a1) * sinP);
    float p2y = p3y - t * (-rx * sinf(a1) * sinP + ry * cosf(a1) * cosP);
    pb.curve(p1x, p1y, p2x, p2y, p3x, p3y);
  }
}

static bool parse_svg_path_d(const std::string& d, SvgPathBuilder& pb) {
  size_t i = 0, n = d.size();
  char cmd = 0;
  float last_cx = 0, last_cy = 0;  /* previous control point (S/T reflection) */
  bool have_ctl = false;
  auto skipsep = [&]() {
    while (i < n && (d[i] == ' ' || d[i] == ',' || d[i] == '\n' || d[i] == '\r' || d[i] == '\t')) i++;
  };
  auto num = [&](float* out) -> bool {
    skipsep();
    if (i >= n) return false;
    const char* start = d.c_str() + i;
    char* end = nullptr;
    double v = strtod(start, &end);
    if (end == start) return false;
    i += (size_t)(end - start);
    *out = (float)v;
    return true;
  };
  auto flags = [&](int* out) -> bool {
    skipsep();
    if (i >= n) return false;
    if (d[i] == '0') { *out = 0; i++; return true; }
    if (d[i] == '1') { *out = 1; i++; return true; }
    return false;
  };
  while (i < n) {
    skipsep();
    if (i >= n) break;
    char c = d[i];
    if (isalpha((unsigned char)c)) { cmd = c; i++; }
    else if (!cmd) { i++; continue; }
    float x, y, x1, y1, x2, y2, rx, ry, rot;
    int la, sw;
    switch (tolower((unsigned char)cmd)) {
      case 'm': {
        if (!num(&x) || !num(&y)) return false;
        if (islower((unsigned char)cmd)) x += pb.cx, y += pb.cy;
        pb.move(x, y);
        cmd = islower((unsigned char)cmd) ? 'l' : 'L';
        have_ctl = false;
        break;
      }
      case 'l': {
        if (!num(&x) || !num(&y)) return false;
        if (islower((unsigned char)cmd)) x += pb.cx, y += pb.cy;
        pb.line(x, y);
        have_ctl = false;
        break;
      }
      case 'h': {
        if (!num(&x)) return false;
        if (islower((unsigned char)cmd)) x += pb.cx;
        pb.line(x, pb.cy);
        have_ctl = false;
        break;
      }
      case 'v': {
        if (!num(&y)) return false;
        if (islower((unsigned char)cmd)) y += pb.cy;
        pb.line(pb.cx, y);
        have_ctl = false;
        break;
      }
      case 'c': {
        if (!num(&x1) || !num(&y1) || !num(&x2) || !num(&y2) || !num(&x) || !num(&y)) return false;
        if (islower((unsigned char)cmd)) { x1 += pb.cx; y1 += pb.cy; x2 += pb.cx; y2 += pb.cy; x += pb.cx; y += pb.cy; }
        pb.curve(x1, y1, x2, y2, x, y);
        last_cx = x2; last_cy = y2; have_ctl = true;
        break;
      }
      case 's': {
        if (!num(&x2) || !num(&y2) || !num(&x) || !num(&y)) return false;
        if (islower((unsigned char)cmd)) { x2 += pb.cx; y2 += pb.cy; x += pb.cx; y += pb.cy; }
        /* first control = reflection of the PREVIOUS control point */
        float rx1 = have_ctl ? 2 * pb.cx - last_cx : pb.cx;
        float ry1 = have_ctl ? 2 * pb.cy - last_cy : pb.cy;
        pb.curve(rx1, ry1, x2, y2, x, y);
        last_cx = x2; last_cy = y2; have_ctl = true;
        break;
      }
      case 'q': {
        if (!num(&x1) || !num(&y1) || !num(&x) || !num(&y)) return false;
        if (islower((unsigned char)cmd)) { x1 += pb.cx; y1 += pb.cy; x += pb.cx; y += pb.cy; }
        /* quadratic -> cubic */
        float cx1 = pb.cx + 2.0f / 3 * (x1 - pb.cx), cy1 = pb.cy + 2.0f / 3 * (y1 - pb.cy);
        float cx2 = x + 2.0f / 3 * (x1 - x), cy2 = y + 2.0f / 3 * (y1 - y);
        pb.curve(cx1, cy1, cx2, cy2, x, y);
        last_cx = x1; last_cy = y1; have_ctl = true;
        break;
      }
      case 't': {
        if (!num(&x) || !num(&y)) return false;
        if (islower((unsigned char)cmd)) { x += pb.cx; y += pb.cy; }
        /* control = reflection of the PREVIOUS quadratic control point */
        float px = have_ctl ? last_cx : pb.cx;
        float py = have_ctl ? last_cy : pb.cy;
        float tx = 2 * pb.cx - px, ty = 2 * pb.cy - py;
        float cx1 = pb.cx + 2.0f / 3 * (tx - pb.cx), cy1 = pb.cy + 2.0f / 3 * (ty - pb.cy);
        float cx2 = x + 2.0f / 3 * (tx - x), cy2 = y + 2.0f / 3 * (ty - y);
        pb.curve(cx1, cy1, cx2, cy2, x, y);
        last_cx = tx; last_cy = ty; have_ctl = true;
        break;
      }
      case 'a': {
        if (!num(&rx) || !num(&ry) || !num(&rot) || !flags(&la) || !flags(&sw) || !num(&x) || !num(&y)) return false;
        if (islower((unsigned char)cmd)) { x += pb.cx; y += pb.cy; }
        arc_to_cubic(pb, pb.cx, pb.cy, rx, ry, rot, la != 0, sw != 0, x, y);
        have_ctl = false;
        break;
      }
      case 'z': pb.close(); have_ctl = false; break;
      default: return false;
    }
  }
  return true;
}

/* XML parsing */
static void xml_decode(std::string* s) {
  std::string out;
  out.reserve(s->size());
  for (size_t i = 0; i < s->size(); i++) {
    if ((*s)[i] == '&') {
      size_t e = s->find(';', i);
      if (e != std::string::npos && e - i < 12) {
        std::string ent = s->substr(i + 1, e - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
          int cp = ent[1] == 'x' || ent[1] == 'X' ? (int)strtol(ent.c_str() + 2, nullptr, 16) : atoi(ent.c_str() + 1);
          if (cp < 128) out += (char)cp;
          else if (cp < 2048) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
          else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
        }
        i = e;
        continue;
      }
    }
    out += (*s)[i];
  }
  *s = out;
}

static SvgNode* xml_parse(const std::string& src) {
  std::vector<SvgNode*> stack;
  SvgNode* root = nullptr;
  size_t i = 0, n = src.size();
  while (i < n) {
    if (src.compare(i, 4, "<!--") == 0) {
      size_t e = src.find("-->", i);
      if (e == std::string::npos) break;
      i = e + 3;
      continue;
    }
    if (src.compare(i, 9, "<![CDATA[") == 0) {
      size_t e = src.find("]]>", i);
      i = e == std::string::npos ? n : e + 3;
      continue;
    }
    if (src[i] == '<') {
      if (i + 1 < n && src[i + 1] == '/') { /* close */
        size_t e = src.find('>', i);
        if (e == std::string::npos) break;
        if (!stack.empty()) stack.pop_back();
        i = e + 1;
        continue;
      }
      if (i + 1 < n && (src[i + 1] == '?' || (i + 1 < n && src.compare(i, 2, "<!") == 0))) {
        size_t e = src.find('>', i);
        i = e == std::string::npos ? n : e + 1;
        continue;
      }
      /* start tag */
      size_t e = i;
      bool in_q = false;
      char q = 0;
      while (e < n) {
        char c = src[e];
        if (in_q) { if (c == q) in_q = false; }
        else if (c == '"' || c == '\'') { in_q = true; q = c; }
        else if (c == '>') break;
        e++;
      }
      if (e >= n) break;
      bool selfclose = e > i && src[e - 1] == '/';
      size_t tag_end = selfclose ? e - 1 : e;
      std::string inner = src.substr(i + 1, tag_end - i - 1);
      /* tag name */
      size_t p = 0;
      while (p < inner.size() && !isspace((unsigned char)inner[p]) && inner[p] != '=') p++;
      SvgNode* node = new SvgNode();
      node->tag = lower_copy(inner.substr(0, p));
      /* attrs */
      while (p < inner.size()) {
        while (p < inner.size() && (isspace((unsigned char)inner[p]) || inner[p] == '/')) p++;
        size_t ns = p;
        while (p < inner.size() && inner[p] != '=' && !isspace((unsigned char)inner[p])) p++;
        if (p == ns) { p++; continue; }
        std::string aname = lower_copy(inner.substr(ns, p - ns));
        while (p < inner.size() && isspace((unsigned char)inner[p])) p++;
        std::string aval;
        if (p < inner.size() && inner[p] == '=') {
          p++;
          while (p < inner.size() && isspace((unsigned char)inner[p])) p++;
          if (p < inner.size() && (inner[p] == '"' || inner[p] == '\'')) {
            char q = inner[p++];
            size_t qe = inner.find(q, p);
            if (qe == std::string::npos) qe = inner.size();
            aval = inner.substr(p, qe - p);
            p = qe;
          } else {
            size_t ve = p;
            while (ve < inner.size() && !isspace((unsigned char)inner[ve]) && inner[ve] != '/') ve++;
            aval = inner.substr(p, ve - p);
            p = ve;
          }
        }
        xml_decode(&aval);
        node->attrs.push_back({aname, aval});
      }
      if (!root) root = node;
      if (!stack.empty()) {
        node->parent = stack.back();
        stack.back()->kids.push_back(node);
      }
      if (!selfclose) stack.push_back(node);
      i = e + 1;
      continue;
    }
    /* text */
    size_t te = src.find('<', i);
    if (te == std::string::npos) break;
    i = te;
  }
  return root;
}

/* register gradient defs */
static void collect_defs(SvgCtx* ctx, SvgNode* n) {
  if (n->attr("id")) ctx->byid[*n->attr("id")] = n;
  if (n->tag == "lineargradient" || n->tag == "radialgradient") {
    SvgGrad g;
    g.linear = n->tag == "lineargradient";
    auto getf = [&](const char* a, float d) {
      const std::string* v = n->attr(a);
      return v ? (float)atof(v->c_str()) : d;
    };
    g.x1 = getf("x1", 0); g.y1 = getf("y1", 0);
    g.x2 = getf("x2", 1); g.y2 = getf("y2", 0);
    g.cx = getf("cx", 0.5f); g.cy = getf("cy", 0.5f);
    g.r = getf("r", 0.5f);
    g.fx = getf("fx", g.cx); g.fy = getf("fy", g.cy);
    const std::string* gu = n->attr("gradientunits");
    if (gu && lower_copy(*gu) == "userspaceonuse") g.user_space = true;
    const std::string* gt = n->attr("gradienttransform");
    if (gt) parse_transform(*gt, &g.mat);
    for (auto* k : n->kids) {
      if (k->tag != "stop") continue;
      GradStop s;
      const std::string* off = k->attr("offset");
      if (off) {
        std::string o = *off;
        if (!o.empty() && o.back() == '%') s.pos = (float)atof(o.c_str()) / 100;
        else s.pos = (float)atof(o.c_str());
      }
      const std::string* sc = k->attr("stop-color");
      if (!sc && k->attr("style")) {
        /* style="stop-color:#fff;stop-opacity:1" */
        std::string st = *k->attr("style");
        size_t p = st.find("stop-color");
        if (p != std::string::npos) {
          size_t c = st.find(':', p);
          size_t e = st.find(';', c);
          sc = new std::string(st.substr(c + 1, e == std::string::npos ? std::string::npos : e - c - 1));
        }
      }
      if (sc) parse_color_str(*sc, &s.c);
      const std::string* so = k->attr("stop-opacity");
      if (so) s.c.a = (uint8_t)(clamp01((float)atof(so->c_str())) * 255);
      g.stops.push_back(s);
    }
    if (n->attr("id")) ctx->grads[*n->attr("id")] = g;
  }
  for (auto* k : n->kids) collect_defs(ctx, k);
}

struct SvgPaintState {
  SvgState st;
  SvgMat mat;
  std::string grad_id;   /* non-empty => fill comes from a gradient */
};

static void render_svg_node(SvgCtx* ctx, SvgNode* n, SvgPaintState ps);

static void svg_render_path_data(SvgCtx* ctx, const SvgPathBuilder& pb, SvgPaintState ps,
                                 const Rect* bbox_override = nullptr) {
  if (pb.cmds.empty()) return;
  /* transform points */
  std::vector<float> tpts(pb.pts.size());
  for (size_t i = 0; i + 1 < pb.pts.size(); i += 2) {
    float ox, oy;
    ps.mat.apply(pb.pts[i], pb.pts[i + 1], &ox, &oy);
    tpts[i] = ox;
    tpts[i + 1] = oy;
  }
  Rect bbox = bbox_override ? *bbox_override : Rect{0, 0, 0, 0};
  if (!bbox_override) {
    float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    for (size_t i = 0; i + 1 < tpts.size(); i += 2) {
      minx = std::min(minx, tpts[i]); maxx = std::max(maxx, tpts[i]);
      miny = std::min(miny, tpts[i + 1]); maxy = std::max(maxy, tpts[i + 1]);
    }
    bbox = {minx, miny, maxx - minx, maxy - miny};
  }
  PathData pd;
  pd.pts = tpts;
  pd.cmds = pb.cmds;
  pd.fill_rule = ps.st.fill_rule;
  if (ps.st.fill.a > 0) pd.fill = Color{(uint8_t)(ps.st.fill.a * ps.st.opacity * ps.st.fill_opacity), ps.st.fill.r, ps.st.fill.g, ps.st.fill.b};
  if (ps.st.stroke.a > 0) {
    pd.stroke = Color{(uint8_t)(ps.st.stroke.a * ps.st.opacity * ps.st.stroke_opacity), ps.st.stroke.r, ps.st.stroke.g, ps.st.stroke.b};
    float sx = sqrtf(ps.mat.m[0] * ps.mat.m[0] + ps.mat.m[1] * ps.mat.m[1]);
    pd.stroke_w = ps.st.stroke_w * (sx > 0.001f ? sx : 1);
  }
  if (pd.fill.a > 0 || pd.stroke.a > 0) gfx_draw_path(*ctx->surf, pd, &ctx->clip);
}

static void apply_paint_attrs(SvgNode* n, SvgPaintState* ps, SvgCtx* ctx, Rect* out_bbox) {
  auto get = [&](const char* a) -> const std::string* {
    const std::string* v = n->attr(a);
    if (!v && n->attr("style")) {
      std::string st = *n->attr("style");
      size_t p = st.find(a);
      if (p != std::string::npos) {
        size_t c = st.find(':', p);
        size_t e = st.find(';', c);
        static std::string buf;
        buf = trim_copy(st.substr(c + 1, e == std::string::npos ? std::string::npos : e - c - 1));
        return &buf;
      }
    }
    return v;
  };
  if (const std::string* v = get("color")) { Color c; if (parse_color_str(*v, &c)) ps->st.color = c; }
  if (const std::string* v = get("fill")) {
    if (*v == "currentColor") ps->st.fill = ps->st.color;
    else {
      Color c;
      if (parse_color_str(*v, &c)) { ps->st.fill = c; ps->grad_id.clear(); }
      else if (v->rfind("url(#", 0) == 0 && v->find(')', 5) != std::string::npos) {
        std::string id = v->substr(5, v->find(')', 5) - 5);
        if (ctx->grads.count(id)) { ps->grad_id = id; ps->st.fill = Color{0, 0, 0, 0}; }
      } else if (*v == "none") { ps->st.fill = Color{0,0,0,0}; ps->grad_id.clear(); }
    }
  }
  if (const std::string* v = get("stroke")) {
    if (*v == "currentColor") ps->st.stroke = ps->st.color;
    else {
      Color c;
      if (parse_color_str(*v, &c)) ps->st.stroke = c;
    }
  }
  if (const std::string* v = get("stroke-width")) ps->st.stroke_w = (float)atof(v->c_str());
  if (const std::string* v = get("opacity")) ps->st.opacity *= clamp01((float)atof(v->c_str()));
  if (const std::string* v = get("fill-opacity")) ps->st.fill_opacity *= clamp01((float)atof(v->c_str()));
  if (const std::string* v = get("stroke-opacity")) ps->st.stroke_opacity *= clamp01((float)atof(v->c_str()));
  if (const std::string* v = get("fill-rule")) {
    if (lower_copy(*v) == "evenodd") ps->st.fill_rule = 1;
  }
  (void)out_bbox;
}

static void render_svg_node(SvgCtx* ctx, SvgNode* n, SvgPaintState ps) {
  const std::string& tag = n->tag;
  if (tag == "defs" || tag == "title" || tag == "desc" || tag == "lineargradient" ||
      tag == "radialgradient" || tag == "stop" || tag == "clippath" || tag == "mask") {
    return;
  }
  if (tag == "text" || tag == "tspan") { engine_log(2, "svg: <text> not supported"); return; }
  if (tag == "use") {
    const std::string* href = n->attr("href");
    if (!href) href = n->attr("xlink:href");
    if (href && !href->empty() && (*href)[0] == '#') {
      auto it = ctx->byid.find(href->substr(1));
      if (it != ctx->byid.end()) {
        SvgPaintState ps2 = ps;
        float x = 0, y = 0;
        if (n->attr("x")) x = (float)atof(n->attr("x")->c_str());
        if (n->attr("y")) y = (float)atof(n->attr("y")->c_str());
        float m[6] = {1, 0, 0, 1, x, y};
        ps2.mat.compose(SvgMat(m));
        apply_paint_attrs(n, &ps2, ctx, nullptr);
        if (n->attr("transform")) {
          SvgMat tm;
          parse_transform(*n->attr("transform"), &tm);
          ps2.mat.compose(tm);
        }
        render_svg_node(ctx, it->second, ps2);
      }
    }
    return;
  }

  apply_paint_attrs(n, &ps, ctx, nullptr);
  if (n->attr("transform")) {
    SvgMat tm;
    parse_transform(*n->attr("transform"), &tm);
    ps.mat.compose(tm);
  }
  if (ps.st.opacity <= 0.001f) return;

  auto fnum = [&](const char* a, float d) -> float {
    const std::string* v = n->attr(a);
    return v ? (float)atof(v->c_str()) : d;
  };

  if (tag == "g" || tag == "svg" || tag == "a" || tag == "switch" || tag == "foreignobject") {
    for (auto* k : n->kids) render_svg_node(ctx, k, ps);
    return;
  }

  SvgPathBuilder pb;
  Rect bbox{0, 0, 0, 0};
  bool has_bbox = false;

  if (tag == "path") {
    const std::string* d = n->attr("d");
    if (!d || !parse_svg_path_d(*d, pb)) return;
  } else if (tag == "rect") {
    float x = fnum("x", 0), y = fnum("y", 0), w = fnum("width", 0), h = fnum("height", 0);
    float rx = fnum("rx", -1), ry = fnum("ry", -1);
    if (rx < 0 && ry < 0) { rx = ry = 0; }
    else if (rx < 0) rx = ry;
    else if (ry < 0) ry = rx;
    rx = std::min(rx, w / 2);
    ry = std::min(ry, h / 2);
    if (rx <= 0 || ry <= 0) {
      pb.move(x, y);
      pb.line(x + w, y);
      pb.line(x + w, y + h);
      pb.line(x, y + h);
      pb.close();
    } else {
      pb.move(x + rx, y);
      pb.line(x + w - rx, y);
      pb.curve(x + w - rx + rx * 0.5523f, y, x + w, y + ry - ry * 0.5523f, x + w, y + ry);
      pb.line(x + w, y + h - ry);
      pb.curve(x + w, y + h - ry + ry * 0.5523f, x + w - rx + rx * 0.5523f, y + h, x + w - rx, y + h);
      pb.line(x + rx, y + h);
      pb.curve(x + rx - rx * 0.5523f, y + h, x, y + h - ry + ry * 0.5523f, x, y + h - ry);
      pb.line(x, y + ry);
      pb.curve(x, y + ry - ry * 0.5523f, x + rx - rx * 0.5523f, y, x + rx, y);
      pb.close();
    }
  } else if (tag == "circle" || tag == "ellipse") {
    float cx = fnum("cx", 0), cy = fnum("cy", 0);
    float rx = tag == "circle" ? fnum("r", 0) : fnum("rx", 0);
    float ry = tag == "circle" ? rx : fnum("ry", 0);
    static const float K = 0.5522847f;
    pb.move(cx + rx, cy);
    pb.curve(cx + rx, cy + ry * K, cx + rx * K, cy + ry, cx, cy + ry);
    pb.curve(cx - rx * K, cy + ry, cx - rx, cy + ry * K, cx - rx, cy);
    pb.curve(cx - rx, cy - ry * K, cx - rx * K, cy - ry, cx, cy - ry);
    pb.curve(cx + rx * K, cy - ry, cx + rx, cy - ry * K, cx + rx, cy);
    pb.close();
  } else if (tag == "line") {
    pb.move(fnum("x1", 0), fnum("y1", 0));
    pb.line(fnum("x2", 0), fnum("y2", 0));
  } else if (tag == "polyline" || tag == "polygon") {
    const std::string* pstr = n->attr("points");
    if (!pstr) return;
    auto nums = split_ws(*pstr);
    std::vector<float> v;
    for (auto& s2 : nums) v.push_back((float)atof(s2.c_str()));
    if (v.size() < 4) return;
    pb.move(v[0], v[1]);
    for (size_t i = 2; i + 1 < v.size(); i += 2) pb.line(v[i], v[i + 1]);
    if (tag == "polygon") pb.close();
  } else if (tag == "image") {
    engine_log(2, "svg: <image> not supported");
    return;
  } else {
    /* unknown container: keep rendering children */
    for (auto* k : n->kids) render_svg_node(ctx, k, ps);
    return;
  }

  /* gradient fill resolution */
  bool use_grad = !ps.grad_id.empty() && ctx->grads.count(ps.grad_id) > 0;
  std::string grad_id = ps.grad_id;
  if (use_grad) {
    /* rasterize path coverage mask, then paint gradient through it */
    std::vector<float> tpts(pb.pts.size());
    for (size_t i = 0; i + 1 < pb.pts.size(); i += 2) {
      float ox, oy;
      ps.mat.apply(pb.pts[i], pb.pts[i + 1], &ox, &oy);
      tpts[i] = ox;
      tpts[i + 1] = oy;
    }
    PathData pd;
    pd.pts = tpts;
    pd.cmds = pb.cmds;
    pd.fill_rule = ps.st.fill_rule;
    Rect bounds{0, 0, (float)ctx->surf->w, (float)ctx->surf->h};
    std::vector<float> cov((size_t)ctx->surf->w * ctx->surf->h, 0.f);
    /* reuse path_coverage via a temp PathData raster (small helper below) */
    /* simple approach: draw path into temp surface alpha, then blit gradient */
    Surface tmp;
    tmp.alloc(ctx->surf->w, ctx->surf->h);
    PathData pd2 = pd;
    pd2.fill = Color{255, 255, 255, 255};
    pd2.stroke = Color{0, 0, 0, 0};
    gfx_draw_path(tmp, pd2, nullptr);
    SvgGrad& g = ctx->grads[grad_id];
    GradLUT lut;
    build_lut(g.stops, &lut);
    /* gradient axis in user space → transformed by g.mat then ps.mat */
    float bbx[2] = {0, 0}, bby[2] = {0, 0};
    if (g.user_space) {
      bbx[0] = g.x1; bbx[1] = g.x2; bby[0] = g.y1; bby[1] = g.y2;
    }
    for (int y = 0; y < ctx->surf->h; y++) {
      uint32_t* dr = ctx->surf->row32(y);
      uint32_t* tr = tmp.row32(y);
      for (int x = 0; x < ctx->surf->w; x++) {
        uint32_t tc = tr[x];
        uint32_t ta = (tc >> 24);
        if (!ta) continue;
        float t = 0;
        float ux, uy;
        /* objectBoundingBox gradient: bbox of transformed pts */
        if (!g.user_space) {
          float minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
          for (size_t i = 0; i + 1 < tpts.size(); i += 2) {
            minx = std::min(minx, tpts[i]); maxx = std::max(maxx, tpts[i]);
            miny = std::min(miny, tpts[i + 1]); maxy = std::max(maxy, tpts[i + 1]);
          }
          float fw = maxx - minx > 1e-6 ? (x - minx) / (maxx - minx) : 0;
          float fh = maxy - miny > 1e-6 ? (y - miny) / (maxy - miny) : 0;
          float gx0 = g.linear ? g.x1 : g.cx, gy0 = g.linear ? g.y1 : g.cy;
          float gx1 = g.linear ? g.x2 : g.cx, gy1 = g.linear ? g.y2 : g.cy;
          if (g.linear) {
            float dx = gx1 - gx0, dy = gy1 - gy0;
            float len2 = dx * dx + dy * dy;
            t = len2 > 1e-9 ? ((fw - gx0) * dx + (fh - gy0) * dy) / len2 : 0;
          } else {
            float r2 = g.r * g.r;
            float ddx = fw - gx0, ddy = fh - gy0;
            t = sqrtf(ddx * ddx + ddy * ddy) / std::max(g.r, 1e-4f);
            (void)r2;
          }
        } else {
          float wx, wy;
          SvgMat inv_all = ps.mat;
          /* approximate: assume content transform maps user → surface; invert locally */
          float det = inv_all.m[0] * inv_all.m[3] - inv_all.m[1] * inv_all.m[2];
          if (fabsf(det) < 1e-9) det = 1;
          float ix = (x - inv_all.m[4]) / det * inv_all.m[3] - (y - inv_all.m[5]) / det * inv_all.m[2];
          float iy = ((y - inv_all.m[5]) * inv_all.m[0] - (x - inv_all.m[4]) * inv_all.m[1]) / det;
          wx = ix; wy = iy;
          (void)bbx; (void)bby;
          if (g.linear) {
            float dx = g.x2 - g.x1, dy = g.y2 - g.y1;
            float len2 = dx * dx + dy * dy;
            t = len2 > 1e-9 ? ((wx - g.x1) * dx + (wy - g.y1) * dy) / len2 : 0;
          } else {
            float ddx = wx - g.cx, ddy = wy - g.cy;
            t = sqrtf(ddx * ddx + ddy * ddy) / std::max(g.r, 1e-4f);
          }
          (void)ux; (void)uy;
        }
        int idx = (int)(clamp01(t) * 1023.f + 0.5f);
        uint32_t col = lut.e[idx];
        uint32_t a = (uint32_t)((col >> 24) * ta / 255 * ps.st.opacity * ps.st.fill_opacity);
        uint32_t src = (a << 24) | (col & 0xFFFFFF);
        dr[x] = blend_src_over(dr[x], src);
      }
    }
    tmp.free_pix();
    return;
  }

  svg_render_path_data(ctx, pb, ps);
}

Surface* svg_render(const std::string& svg_text, float w, float h, Color current_color, float zoom) {
  if (w <= 0 || h <= 0) return nullptr;
  SvgNode* root = xml_parse(svg_text);
  if (!root) return nullptr;
  Surface* out = new Surface();
  /* determine size */
  float sw = w, sh = h;
  const std::string* attr_w = root->attr("width");
  const std::string* attr_h = root->attr("height");
  const std::string* vb = root->attr("viewbox");
  float vbx = 0, vby = 0, vbw = 0, vbh = 0;
  bool has_vb = false;
  if (vb) {
    auto nums = split_ws(*vb);
    if (nums.size() >= 4) {
      vbx = (float)atof(nums[0].c_str());
      vby = (float)atof(nums[1].c_str());
      vbw = (float)atof(nums[2].c_str());
      vbh = (float)atof(nums[3].c_str());
      has_vb = vbw > 0 && vbh > 0;
    }
  }
  if (attr_w && attr_h && !has_vb) {
    sw = (float)atof(attr_w->c_str()) * zoom;
    sh = (float)atof(attr_h->c_str()) * zoom;
  }
  out->alloc(std::max(1, (int)ceilf(sw)), std::max(1, (int)ceilf(sh)));
  SvgCtx ctx;
  ctx.surf = out;
  ctx.clip = Rect{0, 0, (float)out->w, (float)out->h};
  ctx.current_color = current_color;
  /* find svg root element (root node from parse might be svg) */
  SvgNode* svg = root;
  if (svg->tag != "svg") {
    for (auto* k : svg->kids)
      if (k->tag == "svg") { svg = k; break; }
  }
  collect_defs(&ctx, svg);
  /* content transform: viewBox → surface with xMidYMid meet */
  SvgPaintState ps;
  ps.grad_id = "";
  ps.st.fill = Color{0, 0, 0, 255};
  ps.st.stroke = Color{0, 0, 0, 0};
  ps.st.color = current_color;
  if (has_vb) {
    float scale = std::min(sw / vbw, sh / vbh);
    ctx.scale = scale;
    float tx = (sw - vbw * scale) / 2 - vbx * scale;
    float ty = (sh - vbh * scale) / 2 - vby * scale;
    float m[6] = {scale, 0, 0, scale, tx, ty};
    ps.mat = SvgMat(m);
  } else if (zoom != 1) {
    float m[6] = {zoom, 0, 0, zoom, 0, 0};
    ps.mat = SvgMat(m);
  }
  for (auto* k : svg->kids) render_svg_node(&ctx, k, ps);
  delete root;
  return out;
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
