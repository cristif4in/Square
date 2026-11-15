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

/* @font-face fonts fetched from the network, registered per family+weight */
struct WebFontFace {
  std::string family;
  int weight = 400;
  bool italic = false;
  std::shared_ptr<std::vector<uint8_t>> data;
};
static std::vector<WebFontFace> g_webfonts;

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

static inline int utf8_next(const char* s, size_t len, size_t* i, int* cp);
static void utf8_append(std::string& s, int cp);

/* ============================ script fallback ============================ */
/* Unicode script buckets for glyph-missing fallback (Arabic, CJK, ...) */
enum ScriptCat { SC_NONE = 0, SC_ARABIC, SC_HEBREW, SC_CJK, SC_KANA, SC_HANGUL, SC_THAI,
                 SC_DEVANAGARI, SC_CYRILLIC, SC_GREEK };
static ScriptCat script_category(int cp) {
  if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
      (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF))
    return SC_ARABIC;
  if (cp >= 0x0590 && cp <= 0x05FF) return SC_HEBREW;
  if ((cp >= 0x2E80 && cp <= 0x2EFF) || (cp >= 0x3000 && cp <= 0x303F) ||
      (cp >= 0x31C0 && cp <= 0x31EF) || (cp >= 0x3200 && cp <= 0x32FF) ||
      (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
      (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) || cp >= 0x20000)
    return SC_CJK;
  if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
      (cp >= 0x31F0 && cp <= 0x31FF))
    return SC_KANA;
  if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
      (cp >= 0x3130 && cp <= 0x318F))
    return SC_HANGUL;
  if (cp >= 0x0E00 && cp <= 0x0E7F) return SC_THAI;
  if (cp >= 0x0900 && cp <= 0x097F) return SC_DEVANAGARI;
  if (cp >= 0x0400 && cp <= 0x04FF) return SC_CYRILLIC;
  if (cp >= 0x0370 && cp <= 0x03FF) return SC_GREEK;
  return SC_NONE;
}

struct FallbackFont {
  std::string path;
  std::unordered_map<std::string, FontEntry*> cache; /* size|weight key */
};
static FallbackFont g_fallbacks[16];
static bool g_fallbacks_scanned = false;
static std::mutex g_fallback_mtx;

/* probe a candidate file for a probe codepoint; returns true when covered */
static bool font_file_covers(const std::string& path, int cp) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf((size_t)n);
  if (fread(buf.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return false; }
  fclose(f);
  int off = stbtt_GetFontOffsetForIndex(buf.data(), 0);
  if (off < 0) return false;
  stbtt_fontinfo info;
  if (!stbtt_InitFont(&info, buf.data(), off)) return false;
  return stbtt_FindGlyphIndex(&info, cp) != 0;
}

/* one-time scan: pick the first system font covering each script's probe */
static void scan_fallback_fonts() {
  std::lock_guard<std::mutex> lk(g_fallback_mtx);
  if (g_fallbacks_scanned) return;
  g_fallbacks_scanned = true;
  scan_system_fonts();
  static const struct { ScriptCat cat; int probe; } probes[] = {
    {SC_ARABIC, 0xFEDF}, /* presentation form: ensures the font maps shaped glyphs */ {SC_HEBREW, 0x05D0}, {SC_CJK, 0x4E2D}, {SC_KANA, 0x3042},
    {SC_HANGUL, 0xAC00}, {SC_THAI, 0x0E01}, {SC_DEVANAGARI, 0x0928},
    {SC_CYRILLIC, 0x0416}, {SC_GREEK, 0x0391},
  };
  for (auto& pr : probes) {
    /* first pass: fonts whose names hint at the script (better coverage) */
    for (auto& path : g_system_font_files) {
      std::string lb = lower_copy(path);
      bool hinted =
          (pr.cat == SC_CJK || pr.cat == SC_KANA)
              ? (lb.find("noto") != std::string::npos || lb.find("yahei") != std::string::npos ||
                 lb.find("msyh") != std::string::npos || lb.find("simsun") != std::string::npos ||
                 lb.find("simhei") != std::string::npos || lb.find("meiryo") != std::string::npos ||
                 lb.find("hiragino") != std::string::npos || lb.find("sourcehan") != std::string::npos)
              : (pr.cat == SC_HANGUL)
                    ? (lb.find("malgun") != std::string::npos || lb.find("gulim") != std::string::npos ||
                       lb.find("noto") != std::string::npos)
                    : (pr.cat == SC_ARABIC)
                          ? (lb.find("arabic") != std::string::npos || lb.find("tahoma") != std::string::npos ||
                             lb.find("noto") != std::string::npos || lb.find("arial") != std::string::npos)
                          : false;
      if (hinted && font_file_covers(path, pr.probe)) {
        g_fallbacks[pr.cat].path = path;
        break;
      }
    }
    if (g_fallbacks[pr.cat].path.empty()) {
      /* second pass: any font with the probe glyph */
      for (auto& path : g_system_font_files) {
        if (font_file_covers(path, pr.probe)) {
          g_fallbacks[pr.cat].path = path;
          break;
        }
      }
    }
  }
  for (auto& pr : probes)
    engine_log(2, "[font] fallback %d -> %s", (int)pr.cat,
               g_fallbacks[pr.cat].path.empty() ? "(none)" : g_fallbacks[pr.cat].path.c_str());
}

/* open a fallback font at the run's size (cached per category+size) */
static FontEntry* fallback_entry(ScriptCat cat, float size_px, int weight) {
  scan_fallback_fonts();
  FallbackFont& ff = g_fallbacks[cat];
  if (ff.path.empty()) return nullptr;
  char key[64];
  snprintf(key, sizeof(key), "%.1f|%d", size_px, weight);
  {
    std::lock_guard<std::mutex> lk(g_fallback_mtx);
    auto it = ff.cache.find(key);
    if (it != ff.cache.end()) return it->second;
  }
  FILE* f = fopen(ff.path.c_str(), "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf((size_t)n);
  if (fread(buf.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return nullptr; }
  fclose(f);
  int off = stbtt_GetFontOffsetForIndex(buf.data(), 0);
  if (off < 0) return nullptr;
  FontEntry* fe = new FontEntry();
  if (!stbtt_InitFont(&fe->info, buf.data(), off)) { delete fe; return nullptr; }
  fe->data = std::move(buf);
  fe->size = size_px;
  fe->scale = stbtt_ScaleForMappingEmToPixels(&fe->info, size_px);
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&fe->info, &ascent, &descent, &gap);
  fe->ascent = ascent * fe->scale;
  fe->descent = -descent * fe->scale;
  fe->weight = weight;
  {
    std::lock_guard<std::mutex> lk(g_fallback_mtx);
    g_fonts.push_back(fe);
    ff.cache[key] = fe;
  }
  return fe;
}

/* resolve a codepoint missing from the primary font to a fallback entry */
static FontEntry* fallback_glyph(FontEntry* primary, int cp, uint32_t* gi_out) {
  ScriptCat cat = script_category(cp);
  if (cat == SC_NONE) cat = SC_CYRILLIC; /* generic: try the widely-covering slot */
  FontEntry* fe = fallback_entry(cat, primary->size, primary->weight);
  if (fe) {
    int gi = stbtt_FindGlyphIndex(&fe->info, cp);
    static int dbg_ok = 8;
    if (dbg_ok > 0) {
      dbg_ok--;
      std::string msg = std::string("[fb-ok] cp=0x") + std::to_string(cp) + " cat=" +
                        std::to_string((int)cat) + " gi=" + std::to_string(gi);
      engine_log(2, "%s", msg.c_str());
    }
    if (gi) {
      *gi_out = (uint32_t)gi;
      return fe;
    }
  }
  static int dbg_left = 12;
  if (dbg_left > 0) {
    dbg_left--;
    std::string msg = std::string("[fb] cp=0x") + std::to_string(cp) + " cat=" +
                      std::to_string((int)cat) + (fe ? " font-but-missing" : " no-font");
    engine_log(2, "%s", msg.c_str());
  }
  return nullptr;
}

/* ============================ Arabic contextual shaping ============================
 * Maps Arabic letters to their Presentation Forms-B codepoints based on joining
 * context (isolated/initial/medial/final), including the lam-alef ligatures. */
struct ArabicLetter {
  uint16_t cp;      /* base letter 0x06xx */
  uint16_t iso, fin, ini, med;
  uint8_t join;     /* 0 = none, 1 = dual (both sides), 2 = right-only */
};
static const ArabicLetter kArabic[] = {
  {0x621, 0xFE80, 0, 0, 0, 0},
  {0x622, 0xFE81, 0xFE82, 0, 0, 2},
  {0x623, 0xFE83, 0xFE84, 0, 0, 2},
  {0x624, 0xFE85, 0xFE86, 0, 0, 2},
  {0x625, 0xFE87, 0xFE88, 0, 0, 2},
  {0x626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, 1},
  {0x627, 0xFE8D, 0xFE8E, 0, 0, 2},
  {0x628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92, 1},
  {0x629, 0xFE93, 0xFE94, 0, 0, 2},
  {0x62A, 0xFE95, 0xFE96, 0xFE97, 0xFE98, 1},
  {0x62B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, 1},
  {0x62C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, 1},
  {0x62D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, 1},
  {0x62E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, 1},
  {0x62F, 0xFEA9, 0xFEAA, 0, 0, 2},
  {0x630, 0xFEAB, 0xFEAC, 0, 0, 2},
  {0x631, 0xFEAD, 0xFEAE, 0, 0, 2},
  {0x632, 0xFEAF, 0xFEB0, 0, 0, 2},
  {0x633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, 1},
  {0x634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, 1},
  {0x635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, 1},
  {0x636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, 1},
  {0x637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, 1},
  {0x638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, 1},
  {0x639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC, 1},
  {0x63A, 0xFECD, 0xFECE, 0xFECF, 0xFED0, 1},
  {0x641, 0xFED1, 0xFED2, 0xFED3, 0xFED4, 1},
  {0x642, 0xFED5, 0xFED6, 0xFED7, 0xFED8, 1},
  {0x643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, 1},
  {0x644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, 1},
  {0x645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, 1},
  {0x646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, 1},
  {0x647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, 1},
  {0x648, 0xFEED, 0xFEEE, 0, 0, 2},
  {0x649, 0xFEEF, 0xFEF0, 0, 0, 2},
  {0x64A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, 1},
};
static const ArabicLetter* arabic_letter(int cp) {
  for (auto& a : kArabic)
    if (a.cp == cp) return &a;
  return nullptr;
}
static void utf8_append(std::string& s, int cp) {
  if (cp < 0x80) s += (char)cp;
  else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) {
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  } else {
    s += (char)(0xF0 | (cp >> 18));
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
}

/* rewrite Arabic letters in-place to presentation forms */
void arabic_shape(std::string& text) {
  /* decode to codepoints with byte offsets */
  std::vector<int> cps;
  std::vector<size_t> off;
  size_t i = 0, n = text.size();
  int cp;
  while (i < n) {
    size_t st = i;
    int c2 = 0;
    utf8_next(text.c_str(), n, &i, &c2);
    off.push_back(st);
    cps.push_back(c2);
  }
  bool has_arabic = false;
  for (int c : cps)
    if ((c >= 0x0621 && c <= 0x064A) || c == 0x0640) has_arabic = true;
  if (!has_arabic) return;

  auto join_class = [&](size_t idx) -> int {
    if (idx >= cps.size()) return 0;
    const ArabicLetter* a = arabic_letter(cps[idx]);
    if (cps[idx] == 0x640) return 1; /* tatweel: dual */
    if (!a) {
      /* diacritics are transparent; other chars do not join */
      if (cps[idx] >= 0x64B && cps[idx] <= 0x652) return -1;
      return 0;
    }
    return a->join;
  };
  auto next_significant = [&](size_t idx) -> int {
    /* join class of the next non-transparent char (or 0 at end) */
    size_t j = idx + 1;
    while (j < cps.size()) {
      int jc = join_class(j);
      if (jc != -1) return jc;
      j++;
    }
    return 0;
  };
  auto prev_can_join = [&](size_t idx) -> bool {
    /* whether the previous significant letter connects forward (dual) */
    size_t j = idx;
    while (j > 0) {
      j--;
      int jc = join_class(j);
      if (jc == -1) continue;
      return jc == 1;
    }
    return false;
  };

  std::string out;
  /* track pending lam for lam-alef ligatures */
  for (size_t k = 0; k < cps.size(); k++) {
    int c = cps[k];
    const ArabicLetter* a = arabic_letter(c);
    if (!a && c != 0x640) {
      /* pass through; encode original bytes */
      size_t st = off[k];
      size_t en = k + 1 < cps.size() ? off[k + 1] : n;
      out += text.substr(st, en - st);
      continue;
    }
    /* lam + alef ligature */
    if (c == 0x644 && k + 1 < cps.size()) {
      int nx = cps[k + 1];
      int lig = 0;
      if (nx == 0x622) lig = 0xFEF5;
      else if (nx == 0x623) lig = 0xFEF7;
      else if (nx == 0x625) lig = 0xFEF9;
      else if (nx == 0x627) lig = 0xFEFB;
      if (lig && prev_can_join(k)) lig += 1; /* final form */
      if (lig) {
        utf8_append(out, lig);
        /* emit any diacritics between lam and alef */
        for (size_t j = k + 1; j < cps.size() && cps[j] >= 0x64B && cps[j] <= 0x652; j++) {
          size_t st = off[j];
          size_t en = j + 1 < cps.size() ? off[j + 1] : n;
          out += text.substr(st, en - st);
        }
        /* skip the alef itself (consumed) */
        size_t j = k + 1;
        while (j < cps.size() && cps[j] >= 0x64B && cps[j] <= 0x652) j++;
        if (j < cps.size() && (arabic_letter(cps[j]) || cps[j] == 0x640) &&
            (cps[j] == 0x622 || cps[j] == 0x623 || cps[j] == 0x625 || cps[j] == 0x627))
          k = j;
        continue;
      }
    }
    int jc_next = next_significant(k);           /* class of next char (0 = none/end) */
    bool joins_prev = prev_can_join(k) && a;     /* a == nullptr only for tatweel handled above */
    bool joins_next = false;
    if (c == 0x640) {
      joins_prev = prev_can_join(k);
      joins_next = jc_next != 0;
      (void)a;
    } else {
      joins_next = a->join == 1 && jc_next != 0;
    }
    uint32_t form;
    if (c == 0x640) form = 0x640; /* tatweel stays */
    else if (joins_prev && joins_next && a->med) form = a->med;
    else if (joins_prev && a->fin) form = a->fin;
    else if (joins_next && a->ini) form = a->ini;
    else form = a->iso;
    utf8_append(out, (int)form);
  }
  if (getenv("SQRL_DBG_SHAPE")) {
    std::string msg = std::string("[shape] in_bytes=") + std::to_string(n) +
                      " out_bytes=" + std::to_string(out.size()) + " out_cps=" +
                      std::to_string(cps.size());
    engine_log(1, "%s", msg.c_str());
  }
  text = out;
}

/* logical -> visual bidi reordering (simple UAX#9 approximation):
 * reverse the whole string, then restore embedded LTR segments (latin,
 * digits); neutral characters ride with the RTL side. */
std::string bidi_visual_reorder(const std::string& text) {
  auto is_ltr_cp = [](int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           (c >= 0xC0 && c <= 0x24F) || (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x4FF);
  };
  auto is_rtl_cp = [](int c) {
    return (c >= 0x0590 && c <= 0x05FF) || (c >= 0x0600 && c <= 0x06FF) ||
           (c >= 0x0700 && c <= 0x074F) || (c >= 0xFB50 && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
  };
  bool has_rtl = false;
  size_t i = 0, n = text.size();
  int cp;
  while (utf8_next(text.c_str(), n, &i, &cp))
    if (is_rtl_cp(cp)) has_rtl = true;
  if (!has_rtl) return text;
  /* segment: LTR runs vs everything else */
  struct Seg {
    std::string s;
    bool ltr;
  };
  std::vector<Seg> segs;
  i = 0;
  bool cur_ltr = false;
  bool have_cur = false;
  std::string cur;
  while (i < n) {
    size_t st = i;
    int c2 = 0;
    utf8_next(text.c_str(), n, &i, &c2);
    bool ltr = is_ltr_cp(c2);
    if (have_cur && ltr != cur_ltr) {
      segs.push_back({cur, cur_ltr});
      cur.clear();
    }
    cur_ltr = ltr;
    have_cur = true;
    cur += text.substr(st, i - st);
  }
  if (have_cur && !cur.empty()) segs.push_back({cur, cur_ltr});
  /* paragraph base direction = first strong character (UAX#9 heuristic) */
  bool base_rtl = false;
  for (auto& sg : segs) {
    if (!sg.ltr) { base_rtl = true; break; }
    if (!sg.s.empty()) break; /* first segment was LTR */
  }
  /* visual: RTL base reverses segment order; LTR base keeps it. Either way,
   * non-LTR segments reverse internally so letters join in reading order. */
  std::string out;
  if (!base_rtl) {
    for (size_t k = 0; k < segs.size(); k++) {
      if (segs[k].ltr) {
        out += segs[k].s;
      } else {
        std::vector<int> cps;
        size_t i2 = 0, n2 = segs[k].s.size();
        int c2;
        while (utf8_next(segs[k].s.c_str(), n2, &i2, &c2)) cps.push_back(c2);
        for (size_t q = cps.size(); q-- > 0;) utf8_append(out, cps[q]);
      }
    }
    return out;
  }
  for (size_t k = segs.size(); k-- > 0;) {
    if (segs[k].ltr) {
      out += segs[k].s;
    } else {
      /* reverse by codepoint (never by bytes) */
      std::vector<int> cps;
      size_t i2 = 0, n2 = segs[k].s.size();
      int c2;
      while (utf8_next(segs[k].s.c_str(), n2, &i2, &c2)) cps.push_back(c2);
      for (size_t q = cps.size(); q-- > 0;) utf8_append(out, cps[q]);
    }
  }
  return out;
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

void font_register_webface(const std::string& family, int weight, bool italic,
                           std::vector<uint8_t> data) {
  /* keep only bytes stb_truetype can actually open (ttf/otf) */
  if (data.size() < 12) return;
  if (stbtt_GetFontOffsetForIndex(data.data(), 0) < 0) {
    engine_log(2, "[font] @font-face '%s': not a ttf/otf (woff2?) — skipped", family.c_str());
    return;
  }
  std::lock_guard<std::mutex> lk(g_font_mtx);
  WebFontFace wf;
  std::string fam = lower_copy(family);
  for (auto& c : fam)
    if (c == '"' || c == 39) c = ' '; /* strip quotes */
  /* trim */
  size_t a = fam.find_first_not_of(" 	");
  size_t b = fam.find_last_not_of(" 	");
  fam = a == std::string::npos ? "" : fam.substr(a, b - a + 1);
  wf.family = fam;
  wf.weight = weight;
  wf.italic = italic;
  wf.data = std::make_shared<std::vector<uint8_t>>(std::move(data));
  engine_log(1, "[font] registered @font-face '%s' w%d (%zu bytes)", wf.family.c_str(), weight,
             wf.data->size());
  g_webfonts.push_back(std::move(wf));
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
  /* @font-face fonts win over fallbacks for their exact family */
  {
    WebFontFace* best = nullptr;
    int bestd = 1 << 30;
    for (auto& wf : g_webfonts) {
      if (wf.family != fam0) continue;
      if (italic && !wf.italic) continue;
      if (!italic && wf.italic) continue;
      int d = abs(wf.weight - weight);
      if (d < bestd) {
        bestd = d;
        best = &wf;
      }
    }
    if (best) {
      /* FontSource view over the shared bytes; font_resolve copies owned bytes
       * into the FontEntry, so a stack-local source is safe here */
      FontSource wsrc;
      wsrc.owned = *best->data;
      wsrc.data = wsrc.owned.data();
      wsrc.len = wsrc.owned.size();
      wsrc.weight = best->weight;
      src = &wsrc;
      goto have_src;
    }
  }
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
have_src:

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

float font_measure_text(void* font, const char* utf8, float letter_spacing, float word_spacing) {
  if (!font || !utf8) return 0;
  FontEntry* fe = (FontEntry*)font;
  size_t i = 0, len = strlen(utf8);
  int prev = 0;
  float x = 0;
  bool first = true;
  int cp;
  while (utf8_next(utf8, len, &i, &cp)) {
    uint32_t gi = (uint32_t)stbtt_FindGlyphIndex(&fe->info, cp);
    FontEntry* use = fe;
    if (!gi) {
      /* glyph missing from the primary font: try script fallback */
      uint32_t fgi = 0;
      FontEntry* ffe = fallback_glyph(fe, cp, &fgi);
      if (ffe) {
        use = ffe;
        gi = fgi;
      }
    }
    if (!first && use == fe && fe->scale > 0) {
      float kern = stbtt_GetCodepointKernAdvance(&fe->info, prev, cp) * fe->scale;
      x += kern;
    }
    int aw, lsb;
    stbtt_GetGlyphHMetrics(&use->info, (int)gi, &aw, &lsb);
    x += aw * use->scale + letter_spacing;
    if (cp == ' ' && !first) x += word_spacing;
    prev = cp;
    first = false;
  }
  return x;
}

float font_shape(void* font, const char* utf8, float letter_spacing, std::vector<GlyphPos>* out,
                 float word_spacing) {
  if (!font) return 0;
  {
    static int dbg = 3;
    if (dbg > 0 && utf8 && strlen(utf8) > 0 && (unsigned char)utf8[0] > 0x7F) {
      dbg--;
      std::string msg = "[bytes]";
      for (size_t q = 0; q < strlen(utf8) && q < 20; q++) {
        char b[8];
        snprintf(b, sizeof(b), " %02X", (unsigned char)utf8[q]);
        msg += b;
      }
      engine_log(2, "%s", msg.c_str());
    }
  }
  FontEntry* fe = (FontEntry*)font;
  size_t i = 0, len = utf8 ? strlen(utf8) : 0;
  int prev = 0;
  float x = 0;
  bool first = true;
  int cp;
  if (out) out->clear();
  while (utf8_next(utf8, len, &i, &cp)) {
    uint32_t gi = (uint32_t)stbtt_FindGlyphIndex(&fe->info, cp);
    FontEntry* use = fe;
    if (!gi) {
      uint32_t fgi = 0;
      FontEntry* ffe = fallback_glyph(fe, cp, &fgi);
      if (ffe) {
        use = ffe;
        gi = fgi;
      }
    }
    if (!first && use == fe) x += stbtt_GetCodepointKernAdvance(&fe->info, prev, cp) * fe->scale;
    if (out) out->push_back(GlyphPos{gi, x, use == fe ? nullptr : use});
    int aw, lsb;
    stbtt_GetGlyphHMetrics(&use->info, (int)gi, &aw, &lsb);
    x += aw * use->scale + letter_spacing;
    if (cp == ' ' && !first) x += word_spacing;
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
    GlyphBitmap* gb = glyph_bitmap(g.font ? (FontEntry*)g.font : fe, g.gi);
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
