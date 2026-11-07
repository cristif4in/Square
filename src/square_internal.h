/* =====================================================================
 *  Square Browser Engine — internal shared definitions
 *  Modules:
 *    square_content.cpp   HTML parser, CSS parser, cascade, computed styles
 *    square_layout.cpp    box tree, layout (block/inline/flex/grid/abs), paint
 *    square_gfx.cpp       software rasterizer, fonts, images, SVG
 *    square_net.cpp       HTTP/1.1 + TLS + gzip + URL + thread pool
 *    square_platform.cpp  engine instance, load pipeline, QuickJS + DOM API
 * ===================================================================== */
#pragma once

#include "square.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sqrl {

/* ============================ value types ============================ */

struct Color {
  uint8_t a = 255, r = 0, g = 0, b = 0;
  static Color rgba(int rr, int gg, int bb, int aa) {
    return Color{(uint8_t)aa, (uint8_t)rr, (uint8_t)gg, (uint8_t)bb};
  }
  bool operator==(const Color& o) const { return a == o.a && r == o.r && g == o.g && b == o.b; }
};
inline uint32_t color_u32(Color c) { return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b; }

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

/* rounded-corner clip region (children clipped to a rounded rect) */
struct RoundedClip {
  bool active = false;
  Rect bounds;
  float radius[4] = {0, 0, 0, 0}; /* tl tr br bl */
};

/* length / units ------------------------------------------------------ */
enum LUnit : uint8_t { L_AUTO = 0, L_PX, L_PCT, L_EM, L_REM, L_VW, L_VH, L_CH, L_FR, L_DEG, L_CALC, L_NORMAL, L_CLAMP };
struct CalcTerm { float val = 0; uint8_t unit = L_PX; float mul = 1; };
struct Length {
  LUnit unit = L_AUTO;
  float val = 0;                 /* px / percent / em factor / fr factor / deg */
  uint8_t ncalc = 0;             /* when unit==L_CALC: number of terms */
  CalcTerm terms[8];             /* flat sum: sum(term.val * mul) px+percent mixed */
  uint8_t has_clamp = 0;         /* unit==L_CLAMP: base in terms[], bounds below */
  uint8_t nmin = 0, nmax = 0;    /* clamp bounds as flat term lists (value type) */
  CalcTerm terms_min[8];
  CalcTerm terms_max[8];
  bool is_auto() const { return unit == L_AUTO; }
  bool is_none() const { return unit == L_NORMAL; }
};

/* zero length (margins/paddings/borders default to 0, not auto) */
static inline Length length_zero() { Length L; L.unit = L_PX; L.val = 0; return L; }

/* resolved gradients -------------------------------------------------- */
struct GradStop { float pos = -1; Color c; };
struct GradSpec {
  int kind = 2;                  /* 2=linear, 3=radial */
  float angle = 180;             /* linear, css degrees (0 = to top) */
  float cx = 0.5f, cy = 0.5f;    /* radial center (fractions of box) */
  float radius = -1;             /* radial, px; -1 = farthest corner */
  std::vector<GradStop> stops;
};

struct Shadow { Color color; float dx = 0, dy = 0, blur = 0, spread = 0; bool inset = false; };

/* grid track: type + value; minmax keeps nested min/max */
enum TrackType : uint8_t { TR_AUTO = 0, TR_PX, TR_PCT, TR_FR, TR_MINMAX };
struct Track {
  TrackType type = TR_AUTO;
  float v = 0;                   /* px / pct / fr factor */
  std::shared_ptr<Track> mn;     /* minmax: owned (shared_ptr → deep-copyable) */
  std::shared_ptr<Track> mx;
  Track() {}
  Track(TrackType t) : type(t), v(t == TR_FR ? 1.f : 0.f) {}
};

/* ============================ DOM ============================ */

enum NodeType : uint8_t { N_DOCUMENT = 0, N_ELEMENT, N_TEXT, N_COMMENT, N_DOCTYPE };
struct Attr { std::string name, value; };

struct ComputedStyle;
struct Box;
struct Node {
  NodeType type = N_ELEMENT;
  std::string tag;               /* lowercase element name ("" for non-elements) */
  std::string data;              /* text / comment payload */
  std::string ns;                /* "" html, "svg" */
  Node* parent = nullptr;
  std::vector<Node*> children;
  std::vector<Attr> attrs;
  ComputedStyle* cs = nullptr;   /* computed style (elements) */
  ComputedStyle* pseudo[2] = {nullptr, nullptr}; /* ::before, ::after */
  Box* box = nullptr;            /* generated box */
  bool needs_style = true;
  bool in_svg = false;           /* inside <svg> subtree */

  const std::string* attr(const char* name) const {
    for (auto& a : attrs) if (a.name == name) return &a.value;
    return nullptr;
  }
  void set_attr(const std::string& n, const std::string& v) {
    for (auto& a : attrs) if (a.name == n) { a.value = v; return; }
    attrs.push_back(Attr{n, v});
  }
  void remove_attr(const std::string& n) {
    for (size_t i = 0; i < attrs.size(); i++) if (attrs[i].name == n) { attrs.erase(attrs.begin() + i); return; }
  }
  bool has_class(const char* c) const {
    const std::string* cl = attr("class");
    if (!cl) return false;
    size_t p = 0;
    while (p < cl->size()) {
      size_t e = cl->find(' ', p);
      if (e == std::string::npos) e = cl->size();
      if (e - p == strlen(c) && cl->compare(p, e - p, c) == 0) return true;
      p = e + 1;
    }
    return false;
  }
  int child_index() const {
    if (!parent) return 0;
    for (size_t i = 0; i < parent->children.size(); i++) if (parent->children[i] == this) return (int)i;
    return -1;
  }
};

/* hovered element (for :hover matching); owned/set by square_platform */
extern Node* g_hover_node;

/* ============================ CSS structures ============================ */

struct AttrSel { std::string name, val; char op = 0; /* 0 exists, '=', '~', '|', '^', '$', '*' */ };
struct SelPart {
  std::string tag;                       /* "" or "*" = any */
  std::vector<std::string> ids, classes;
  std::vector<AttrSel> attrs;
  std::vector<std::string> pseudo_classes; /* hover, focus, first-child, nth-child(2n+1), not(<simple>), root, ... */
  std::string pseudo_element;              /* "", "before", "after" */
  uint8_t combinator = 0;                  /* combinator LEFT of this part: 0=none/descendant,1=child,2=adjacent,3=sibling */
};
struct Selector {
  std::vector<SelPart> parts;              /* document order, leftmost first */
  uint16_t specificity = 0;                /* (ids<<8)|(classes)<<4|tags roughly */
  bool valid = true;
};

struct Declaration { std::string prop, value; bool important = false; };

struct FontFaceSrc { std::string url, format; };
struct FontFaceRule { std::string family; int weight = 400; bool italic = false; std::vector<FontFaceSrc> srcs; };

struct MediaQuery {
  float minw = -1, maxw = -1, minh = -1, maxh = -1;
  bool negative = false;
  bool eval(int vw, int vh) const {
    bool ok = true;
    if (minw >= 0) ok = ok && vw >= minw;
    if (maxw >= 0) ok = ok && vw <= maxw;
    if (minh >= 0) ok = ok && vh >= minh;
    if (maxh >= 0) ok = ok && vh <= maxh;
    return negative ? !ok : ok;
  }
};

struct Rule {
  std::vector<Selector> sels;
  std::vector<Declaration> decls;
  bool media_ok = true;
  int origin = 1;                /* 0 = user-agent, 1 = author */
  int order = 0;                 /* document order for tie-breaks */
};

/* scrollbar appearance (:::webkit-scrollbar-* / scrollbar-width / scrollbar-color) */
struct ScrollbarStyle {
  float width = 14;                    /* px; 0 = hidden */
  Color track{255, 241, 241, 241};     /* field order: a,r,g,b */
  Color thumb{255, 193, 193, 193};
  Color thumb_hover{0, 0, 0, 0};       /* a==0 = not set (use thumb) */
  float radius = 0;
  float thumb_border = 0;
  Color thumb_border_color{0, 0, 0, 0};
};

struct KeyframesRule {
  std::string name;
  std::vector<std::pair<float, std::vector<Declaration>>> steps; /* pct 0..100 */
};

struct StyleSheet {
  std::vector<Rule> rules;
  std::vector<FontFaceRule> fontfaces;
  std::vector<std::string> imports;   /* @import urls to fetch */
  ScrollbarStyle sbstyle;             /* last scrollbar declaration wins */
  std::vector<KeyframesRule> keyframes;
  bool has_animations = false;
};

/* custom properties held per node */
typedef std::unordered_map<std::string, std::string> CustomProps;

/* parsed document (square_content.cpp) */
struct Document {
  Node* root = nullptr;               /* N_DOCUMENT */
  Node* html = nullptr;               /* <html> element */
  Node* head = nullptr;
  Node* body = nullptr;
  StyleSheet sheet;                   /* UA + author rules combined */
  std::string title;
  std::string base_url;               /* base for url resolution */
  float root_font_size = 16;          /* rem basis */
  CustomProps root_vars;              /* :root custom properties */
  ~Document();
};

/* ============================ computed style ============================ */

enum Display : uint8_t {
  D_NONE = 0, D_BLOCK, D_INLINE, D_INLINE_BLOCK, D_FLEX, D_INLINE_FLEX,
  D_GRID, D_INLINE_GRID, D_TABLE, D_TABLE_ROW, D_TABLE_CELL, D_LIST_ITEM,
  D_INLINE_FLEX_BOX /*internal*/, D_TEXT
};
enum PosKind : uint8_t { P_STATIC = 0, P_RELATIVE, P_ABSOLUTE, P_FIXED, P_STICKY };
enum Overflow : uint8_t { O_VISIBLE = 0, O_HIDDEN, O_SCROLL, O_AUTO, O_CLIP };
enum BgKind : uint8_t { BG_NONE = 0, BG_URL, BG_LINEAR, BG_RADIAL };
enum ObjectFit : uint8_t { FIT_FILL = 0, FIT_COVER, FIT_CONTAIN, FIT_NONE };
enum FlexDir : uint8_t { FD_ROW = 0, FD_ROW_REV, FD_COL, FD_COL_REV };
enum FlexWrap : uint8_t { FW_NOWRAP = 0, FW_WRAP, FW_WRAP_REV };
enum Justify : uint8_t { J_START = 0, J_END, J_CENTER, J_BETWEEN, J_AROUND, J_EVENLY };
enum Align : uint8_t { A_STRETCH = 0, A_START, A_END, A_CENTER, A_BASELINE };
enum TextAlign : uint8_t { TA_START = 0, TA_CENTER, TA_END, TA_JUSTIFY };
enum TextTransform : uint8_t { TT_NONE = 0, TT_UPPER, TT_LOWER, TT_CAP };
enum WhiteSpace : uint8_t { WS_NORMAL = 0, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE };
enum VertAlign : uint8_t { VA_BASELINE = 0, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_LEN };

struct BgLayer {
  BgKind kind = BG_NONE;
  std::string url;                       /* raw url (resolved at layout) */
  int image_id = -1;                     /* decoded image handle (-1 none) */
  GradSpec grad;
  uint8_t size_kind = 0;                 /* 0 auto, 1 cover, 2 contain, 3 lengths */
  Length size_w, size_h;
  Length pos_x, pos_y;                   /* offsets (px or %) */
  bool pos_pct_x = true, pos_pct_y = true;
  bool repeat = true;
};

struct ComputedStyle {
  /* boxes */
  Display display = D_INLINE;
  PosKind position = P_STATIC;
  bool box_border = false;               /* box-sizing: border-box */
  bool z_auto = true; int z_index = 0;
  Length top, right, bottom, left;
  Length width, height, min_w, max_w, min_h, max_h;
  uint8_t width_content_mode = 0;        /* 0 normal, 1 max-content/fit-content, 2 min-content */
  Length margin[4] = {length_zero(), length_zero(), length_zero(), length_zero()};          /* css order: top right bottom left */
  Length padding[4] = {length_zero(), length_zero(), length_zero(), length_zero()};
  Length border_width[4] = {length_zero(), length_zero(), length_zero(), length_zero()};
  uint8_t border_style[4] = {0, 0, 0, 0};/* 0 none, 1 solid, 2 dashed, 3 dotted */
  Color border_color[4];
  Length radius[4];                      /* tl tr br bl */
  /* background */
  Color bg_color{0, 0, 0, 0};            /* transparent (field order: a,r,g,b) */
  std::vector<BgLayer> bgs;
  /* font/text (inherited) */
  Color color{255, 0, 0, 0};
  std::vector<std::string> font_family;
  float font_size = 16;
  int font_weight = 400;
  bool italic = false;
  bool lh_normal = true;
  float line_height_mul = 1.2f;          /* unitless multiple */
  Length line_height;                    /* px / pct of font size */
  float letter_spacing = 0;
  TextAlign text_align = TA_START;
  TextTransform text_transform = TT_NONE;
  WhiteSpace white_space = WS_NORMAL;
  uint8_t deco = 0;                      /* bit1 underline, bit2 line-through */
  VertAlign vert_align = VA_BASELINE;
  Length vert_len;
  /* flex */
  FlexDir flex_dir = FD_ROW;
  FlexWrap flex_wrap = FW_NOWRAP;
  Justify justify = J_START;
  Align align = A_STRETCH;
  Align align_self = A_STRETCH; bool align_self_auto = true;
  uint8_t align_content = 0;             /* 0 stretch, 1 start, 2 center, 3 end, 4 between */
  Length gap_row, gap_col;
  float flex_grow = 0, flex_shrink = 1;
  Length flex_basis;                     /* auto default */
  int order = 0;
  /* grid */
  std::vector<Track> grid_cols, grid_rows;
  std::vector<Track> grid_auto_template;         /* repeat(auto-fit|fill, T) */
  uint8_t grid_auto_mode = 0;                    /* 0 off, 1 auto-fit, 2 auto-fill */
  int grid_col_span = 1, grid_row_span = 1;
  /* misc */
  Overflow overflow_x = O_VISIBLE, overflow_y = O_VISIBLE;
  float opacity = 1;
  std::vector<Shadow> shadows;
  bool has_transform = false;
  float mat[6] = {1, 0, 0, 1, 0, 0};     /* a b c d e f */
  Length tx_ox, tx_oy;                   /* transform-origin, default 50% 50% */
  ObjectFit object_fit = FIT_FILL;
  float aspect_ratio = 0;                /* width/height, 0 = none */
  float blur_px = 0;
  float backdrop_blur_px = 0;
  std::string content;                   /* pseudo-elements */
  bool content_set = false;
  /* animation (subset: name/duration/delay/timing/iterations) */
  std::string anim_name;
  std::string anim_timing = "linear";
  bool anim_tx = false;          /* animated transform: translateX(pct% of width) */
  float anim_tx_pct = 0;
  float anim_duration = 0, anim_delay = 0;
  bool anim_infinite = false;
  int anim_iterations = 1;
  uint8_t list_style = 0;                /* 0 none, 1 disc */
  bool visibility_hidden = false;        /* visibility: hidden (subtree skipped) */
  /* resolution context */
  bool pseudo_before = false, pseudo_after = false;
};

/* custom properties held per node */
typedef std::unordered_map<std::string, std::string> CustomProps;

/* ============================ layout box ============================ */

struct GlyphPos { uint32_t gi; float x; };

struct TextRun {
  std::string text;                      /* already transformed (upper/lower) */
  void* font = nullptr;                  /* gfx FontHandle */
  float size = 16, spacing = 0;
  Color color{255, 0, 0, 0};
  float x = 0, y = 0;                    /* x = left, y = BASELINE (page coords) */
  float width = 0;
  float alpha = 1;
  uint8_t deco = 0;                      /* bit1 underline, bit2 line-through */
};

struct InlineFrag {
  enum Kind : uint8_t { F_TEXT = 0, F_BOX, F_REPLACED_IMG, F_REPLACED_SVG, F_BR } kind = F_TEXT;
  Node* node = nullptr;                  /* source node (text node / inline el / img) */
  Box* box = nullptr;                    /* box for F_BOX / replaced */
  TextRun run;                           /* F_TEXT */
  Rect rect;                             /* border-box rect for F_BOX / replaced / br marker */
  float baseline = 0;                    /* baseline offset from frag top (box/replaced) */
  float ascent = 0, descent = 0;         /* for line box math */
  float rise = 0;                        /* vertical-align offset applied */
  float alpha = 1;
};

struct Box {
  Node* node = nullptr;
  ComputedStyle* cs = nullptr;
  Box* parent = nullptr;
  std::vector<Box*> kids;                /* in-flow children (block/flex/grid) */
  std::vector<Box*> out_of_flow;         /* absolutely/fixed positioned children */
  std::vector<InlineFrag> frags;         /* inline content (block containers) */
  /* geometry: border box in page coordinates */
  float x = 0, y = 0, w = 0, h = 0;
  float cw = 0, ch = 0;                  /* content box size */
  float pad_t = 0, pad_r = 0, pad_b = 0, pad_l = 0;
  float bor_t = 0, bor_r = 0, bor_b = 0, bor_l = 0;
  float mar_t = 0, mar_r = 0, mar_b = 0, mar_l = 0;
  /* inline layout info */
  float ch_definite = 0;                 /* definite content height if height set, else 0 */
  float strut_ascent = 0, strut_descent = 0;
  float last_line_baseline = 0;          /* offset from box top to last line baseline */
  bool has_in_flow_abs_child = false;
  /* paint state */
  float eff_opacity = 1;
  int stack_order = 0;                   /* resolved paint order index */
  Rect abs_clip_rect;                    /* for fixed elements etc. */
  /* grid placement */
  int grid_col = 0, grid_row = 0;
  /* marker for list items */
  std::string marker_text;
  float marker_w = 0;
  /* transform pivot */
  float pivot_x() const { return w / 2; }
  float pivot_y() const { return h / 2; }
};

/* resolved image entry (gfx owns pixel store) */
struct ImageInfo {
  int w = 0, h = 0;
  int gfx_id = -1;                       /* handle inside gfx image cache */
  bool decoded = false;
  bool failed = false;
  std::string url;
};

/* ============================ networking (square_net.cpp) ============================ */

struct HttpHeader { std::string name, value; };

struct HttpResult {
  bool ok = false;                       /* transport-level success */
  int status = 0;
  std::vector<HttpHeader> headers;
  std::vector<uint8_t> body;             /* decoded (gunzipped) body */
  std::string final_url;
  std::string err;
  std::string header(const char* name) const {
    for (auto& h : headers)
      if (strcasecmp_h(h.name, name)) return h.value;
    return "";
  }
  static bool strcasecmp_h(const std::string& a, const char* b) {
    size_t i = 0;
    if (a.size() != strlen(b)) return false;
    for (char ca : a) { if (tolower((unsigned char)ca) != tolower((unsigned char)b[i++])) return false; }
    return true;
  }
};

struct CookieJar {
  struct Cookie { std::string host, name, value; };
  std::vector<Cookie> cookies;
  void store(const std::string& host, const std::string& set_cookie_value);
  std::string get(const std::string& host) const;
 private:
  mutable std::mutex mtx;
};

/* implemented in square_net.cpp */
HttpResult net_http_get(const std::string& url, const std::vector<HttpHeader>& extra_headers,
                        const std::string& method, int timeout_ms, CookieJar* jar,
                        const std::atomic<bool>* cancel);
void net_init();
std::string url_resolve(const std::string& base, const std::string& rel);
bool url_split(const std::string& url, std::string* scheme, std::string* host,
               std::string* port, std::string* path_query);
bool gzip_inflate(const uint8_t* in, size_t n, std::vector<uint8_t>* out);
std::string base64_decode(const std::string& in);
bool data_url_parse(const std::string& url, std::string* mime, std::vector<uint8_t>* bytes);

class ThreadPool {
 public:
  explicit ThreadPool(int n_threads);
  ~ThreadPool();
  void submit(std::function<void()> f);
  void wait_idle();
  void drain();                          /* drop pending jobs (shutdown) */
  int  thread_count() const;
 private:
  struct Impl;
  Impl* impl_;
};

/* ============================ gfx (square_gfx.cpp) ============================ */

struct Surface {
  int w = 0, h = 0, stride = 0;          /* stride in bytes */
  uint8_t* pix = nullptr;
  bool own = false;
  Surface() {}
  Surface(int ww, int hh) { alloc(ww, hh); }
  ~Surface() { free_pix(); }
  /* owning pixel store: no copies (prevents double-free / leaks) */
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;
  void alloc(int ww, int hh);
  void free_pix();
  uint32_t* row32(int y) const { return (uint32_t*)(pix + (size_t)y * stride); }
};

/* font request → cached handle. families in preference order ("Inter", "sans-serif"...) */
void* font_resolve(const std::vector<std::string>& families, int weight, bool italic, float size_px);
float font_measure_text(void* font, const char* utf8, float letter_spacing);
float font_shape(void* font, const char* utf8, float letter_spacing, std::vector<GlyphPos>* out);
float font_ascent(void* font);
float font_descent(void* font);         /* positive value */

/* image cache */
int  image_add_rgba(int w, int h, std::vector<uint8_t> rgba); /* returns gfx id */
bool image_decode(const std::vector<uint8_t>& bytes, int* out_gfx_id, int* out_w, int* out_h);
void image_cache_clear();

/* drawing primitives — all coordinates in surface space, already clipped rects are
 * passed via `clip` (axis-aligned, may be nullptr) and optional rounded clip `rc`. */
void gfx_fill_rect(Surface& s, const Rect& r, Color c, const float radius[4],
                   const Rect* clip, const RoundedClip* rc);
void gfx_fill_gradient(Surface& s, const Rect& r, const GradSpec& g,
                       const Rect* clip, const RoundedClip* rc);
void gfx_draw_image(Surface& s, int gfx_id, const Rect& dst, const Rect* src_crop,
                    uint8_t fit, const Rect* clip, const RoundedClip* rc,
                    float alpha);
void gfx_draw_shadow(Surface& s, const Rect& r, const float radius[4], const Shadow& sh,
                     const Rect* clip);
void gfx_draw_glyphs(Surface& s, void* font, float x, float y_baseline,
                     const std::vector<GlyphPos>& glyphs, Color color, float alpha,
                     const Rect* clip);
/* path: svg-flattened polyline/polygon/curve commands.
 * cmds: 0 = move, 1 = line, 2 = close. pts flat x,y pairs (post-transform). */
struct PathData {
  std::vector<float> pts;
  std::vector<uint8_t> cmds;
  Color fill{0, 0, 0, 0};                /* a==0 → no fill */
  Color stroke{0, 0, 0, 0};              /* a==0 → no stroke */
  float stroke_w = 1;
  uint8_t fill_rule = 0;                 /* 0 nonzero, 1 evenodd */
};
void gfx_draw_path(Surface& s, const PathData& p, const Rect* clip);

/* composite a full surface with alpha (+ optional 2x3 matrix applied to src) */
void gfx_blit(Surface& dst, const Surface& src, float alpha, const Rect* clip,
              const RoundedClip* rc);
/* composite with affine transform (a,b,c,d,e,f) around pivot (px,py) */
void gfx_blit_matrix(Surface& dst, const Surface& src, const float mat[6],
                     float pivot_x, float pivot_y, float alpha, const Rect* clip);
/* box-blur a rectangular region of a surface in place (for backdrop-filter) */
void gfx_blur_region(Surface& s, const Rect& r, float radius);

/* svg renderer: takes a serialized <svg> document (utf8), renders into a fresh
 * Surface sized ceil(w x h) css-px * zoom. Returns nullptr on failure.
 * current_color resolves `currentColor`. */
Surface* svg_render(const std::string& svg_text, float w, float h, Color current_color,
                    float zoom);

/* png writer (bgra input) */
bool gfx_write_png(const char* path, const uint8_t* bgra, int w, int h, int stride);

/* log helper used by all modules (set by platform) */
void engine_log(int level, const char* fmt, ...);

/* ============================ layout entry (square_layout.cpp) ============================ */
struct ImageCacheView {
  /* returns a COPY (the underlying map may rehash while pool threads decode) */
  virtual ImageInfo find(const std::string& url) = 0;
  virtual ~ImageCacheView() {}
};

struct LayoutCtx {
  Document* doc = nullptr;
  int vw = 1280, vh = 800;
  float rem = 16;
  ImageCacheView* images = nullptr;
  int warned = 0;
  Box* root_box = nullptr;
};

/* full layout pass: returns root box (caller frees via free_box_tree) */
Box* layout_document(Document* doc, int vw, int vh, ImageCacheView* images);
float layout_page_height(Box* root);
void free_box_tree(Box* b);
void offset_box_recursive(Box* b, float dx, float dy);
/* paint whole document into surface */
void paint_document(Document* doc, Box* root, Surface* s, ImageCacheView* images,
                    float scroll_y = 0);
/* animation-only style update: re-evaluates keyframe values on the existing
 * computed styles WITHOUT the full selector cascade (cheap enough per frame) */
void style_reanimate(Document* doc, int vw, int vh);
/* scrollbar geometry shared by the painter and the drag API: returns the
 * effective scrollbar width (0 = hidden/not scrollable); fills thumb_y /
 * thumb_h (track coords, track spans 0..viewport_h) when the page scrolls */
float scrollbar_metrics(const Document* doc, float viewport_h, float page_h,
                        float scroll_y, float* thumb_y, float* thumb_h);
/* color parsing for the scrollbar rules (defined in square_style.cpp) */
bool parse_css_color(const std::string& raw, Color* out);
void paint_debug_counts(int* boxes, int* frags, int* imgs);

/* ============================ content module (square_content.cpp) ============================ */

/* parse a full HTML document (utf8/latin bytes) */
Document* html_parse_document(const std::string& bytes);
/* parse an html fragment (innerHTML), returns detached nodes */
std::vector<Node*> html_parse_fragment(const std::string& html);
/* parse a css stylesheet; media evaluation against vw/vh */
void css_parse_stylesheet(const std::string& css, StyleSheet* out, int vw, int vh);
/* cascade: compute styles for the whole tree (clears old computed styles) */
void style_tree(Document* doc, int vw, int vh);
/* serialize subtree back to html (innerHTML / svg export) */
std::string serialize_html(const Node* n);
/* free a detached node subtree */
void node_free(Node* n);
/* selector matching used by JS querySelector */
bool selector_matches(const Node* el, const Selector& sel);
bool css_parse_selector_public(const char* text, Selector* out);

} /* namespace sqrl */
