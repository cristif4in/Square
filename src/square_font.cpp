#include "square_gfx_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../thirdparty/stb_truetype.h"
#include "../thirdparty/fonts_embed.h"

namespace sqrl {

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

} /* namespace sqrl */
