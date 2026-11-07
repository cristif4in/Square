#include "square_gfx_internal.h"

#include <algorithm>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace sqrl {

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
  /* Color field order is {a,r,g,b}: {0,0,0,255} here was accidentally
   * TRANSPARENT — SVG default fill must be opaque black */
  ps.st.fill = Color{255, 0, 0, 0};
  ps.st.stroke = Color{0, 0, 0, 0};
  ps.st.color = current_color;
  /* presentation attributes on the root <svg> element (fill="...", color=...)
   * are inherited by everything inside it */
  apply_paint_attrs(svg, &ps, &ctx, nullptr);
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

} /* namespace sqrl */
