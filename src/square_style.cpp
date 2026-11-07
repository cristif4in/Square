/* =====================================================================
 *  Square Browser Engine — square_style.cpp
 *  CSS value parsing (lengths, calc, colors, gradients), declaration
 *  application (apply_declaration) and the cascade (style_tree).
 * ===================================================================== */
#include "square_content_internal.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>

namespace sqrl {

static inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
struct ResolveCtx {
  const CustomProps* vars = nullptr;
  float em = 16;    /* current font size for em in non-font props */
  float rem = 16;
  float vw = 0, vh = 0; /* viewport, for vw/vh units in font-size etc. */
  Color current_color{0, 0, 0, 255};
};

static std::string substitute_vars(const std::string& val, const ResolveCtx& rc, int depth = 0);

static std::string read_var_args(const std::string& val, size_t var_start, const ResolveCtx& rc, int depth) {
  /* val[var_start] == '(' ; returns inner content, advances not handled here */
  int depth_paren = 0;
  size_t i = var_start;
  while (i < val.size()) {
    if (val[i] == '(') depth_paren++;
    if (val[i] == ')') {
      depth_paren--;
      if (depth_paren == 0) break;
    }
    i++;
  }
  return val.substr(var_start + 1, i - var_start - 1);
}

static std::string substitute_vars(const std::string& val, const ResolveCtx& rc, int depth) {
  if (depth > 16) return val;
  std::string out;
  size_t i = 0, n = val.size();
  while (i < n) {
    if (val.compare(i, 4, "var(") == 0) {
      std::string inner = read_var_args(val, i + 3, rc, depth);
      /* inner = name [, fallback] (fallback may contain commas/parens) */
      int pd = 0;
      size_t comma = std::string::npos;
      for (size_t k = 0; k < inner.size(); k++) {
        if (inner[k] == '(') pd++;
        else if (inner[k] == ')') pd--;
        else if (inner[k] == ',' && pd == 0) { comma = k; break; }
      }
      std::string name = trim(comma == std::string::npos ? inner : inner.substr(0, comma));
      std::string fallback = comma == std::string::npos ? "" : trim(inner.substr(comma + 1));
      auto it = rc.vars ? rc.vars->find(name) : rc.vars->end();
      if (rc.vars && it != rc.vars->end()) {
        out += substitute_vars(it->second, rc, depth + 1);
      } else if (!fallback.empty()) {
        out += substitute_vars(fallback, rc, depth + 1);
      } else {
        /* unresolved: emit nothing (property becomes invalid → treated as unset) */
        out += " ";
      }
      i += 4 + inner.size() + 1;
      continue;
    }
    out += val[i];
    i++;
  }
  return out;
}

static bool parse_number(const std::string& s, float* out) {
  char* end = nullptr;
  double v = strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  *out = (float)v;
  return true;
}

/* parse a single length component (already var-substituted) */
static Length parse_length_component(const std::string& t) {
  Length L;
  std::string s = trim(t);
  if (s.empty()) return L;
  if (s == "auto") { L.unit = L_AUTO; return L; }
  if (s == "none") { L.unit = L_NORMAL; return L; }
  if (s == "normal") { L.unit = L_NORMAL; return L; }
  float v;
  size_t num_end = 0;
  while (num_end < s.size() && (isdigit((unsigned char)s[num_end]) || s[num_end] == '.' ||
                                s[num_end] == '-' || s[num_end] == '+'))
    num_end++;
  /* exponent: only consume e/E when it forms a valid exponent (e.g. 1e5, 2E-3) */
  if (num_end < s.size() && (s[num_end] == 'e' || s[num_end] == 'E')) {
    size_t save = num_end;
    size_t k = num_end + 1;
    if (k < s.size() && (s[k] == '+' || s[k] == '-')) k++;
    if (k < s.size() && isdigit((unsigned char)s[k])) {
      while (k < s.size() && isdigit((unsigned char)s[k])) k++;
      num_end = k;
    } else {
      (void)save; /* 'e' belongs to the unit (em/ex) — leave it */
    }
  }
  std::string numstr = s.substr(0, num_end);
  std::string unit = lc(s.substr(num_end));
  if (!parse_number(numstr, &v)) {
    /* bare word? maybe percentage-less keyword */
    return L;
  }
  if (unit == "" || unit == "px") { L.unit = L_PX; L.val = v; }
  else if (unit == "%") { L.unit = L_PCT; L.val = v; }
  else if (unit == "em") { L.unit = L_EM; L.val = v; }
  else if (unit == "rem") { L.unit = L_REM; L.val = v; }
  else if (unit == "vw") { L.unit = L_VW; L.val = v; }
  else if (unit == "vh") { L.unit = L_VH; L.val = v; }
  else if (unit == "ch") { L.unit = L_CH; L.val = v * 8; }   /* approx 8px */
  else if (unit == "pt") { L.unit = L_PX; L.val = v * 96.f / 72.f; }
  else if (unit == "fr") { L.unit = L_FR; L.val = v; }
  else if (unit == "deg") { L.unit = L_DEG; L.val = v; }
  else if (unit == "turn") { L.unit = L_DEG; L.val = v * 360; }
  else if (unit == "rad") { L.unit = L_DEG; L.val = v * 180.f / 3.14159265f; }
  else if (unit == "s" || unit == "ms") { L.unit = L_PX; L.val = 0; }
  else { L.unit = L_PX; L.val = v; }
  return L;
}

/* calc() → flat sum of terms (recursive descent with * / precedence) */
struct CalcParser {
  std::vector<std::string> toks;
  size_t pos = 0;
  const ResolveCtx& rc;
  explicit CalcParser(const std::string& s, const ResolveCtx& r) : rc(r) {
    /* tokenize */
    size_t i = 0;
    while (i < s.size()) {
      if (isspace((unsigned char)s[i])) { i++; continue; }
      if (s[i] == '(') {
        int d = 0;
        size_t st = i;
        while (i < s.size()) {
          if (s[i] == '(') d++;
          if (s[i] == ')') { d--; if (!d) break; }
          i++;
        }
        toks.push_back(s.substr(st, i - st + 1));
        i++;
        continue;
      }
      if (s[i] == '+' || s[i] == '-' || s[i] == '*' || s[i] == '/') {
        toks.push_back(std::string(1, s[i]));
        i++;
        continue;
      }
      size_t st = i;
      while (i < s.size() && !isspace((unsigned char)s[i]) && s[i] != '+' && s[i] != '-' &&
             s[i] != '*' && s[i] != '/' && s[i] != '(' && s[i] != ')')
        i++;
      toks.push_back(s.substr(st, i - st));
    }
  }
  bool parse_sum(std::vector<CalcTerm>* out) {
    char sign = '+';
    while (pos < toks.size()) {
      const std::string& t = toks[pos];
      if (t == "+" || t == "-") { sign = t[0]; pos++; continue; }
      std::vector<CalcTerm> term;
      if (!parse_term(&term)) return false;
      for (auto& x : term) {
        x.mul *= sign == '-' ? -1 : 1;
        out->push_back(x);
      }
      sign = '+';
    }
    return !out->empty();
  }
  bool parse_term(std::vector<CalcTerm>* out) {
    if (!parse_value(out)) return false;
    while (pos + 1 < toks.size() && (toks[pos + 1] == "*" || toks[pos + 1] == "/")) {
      std::string op = toks[pos + 1];
      pos += 2;
      std::vector<CalcTerm> rhs;
      if (!parse_value(&rhs)) return false;
      float scalar = 0;
      bool is_scalar = rhs.size() == 1 && (rhs[0].unit == L_PX || rhs[0].unit == L_PCT) ;
      if (is_scalar) scalar = rhs[0].val;
      if (op == "*") {
        if (is_scalar) for (auto& x : *out) x.mul *= scalar;
        else return false;
      } else {
        if (is_scalar && scalar != 0) for (auto& x : *out) x.mul /= scalar;
        else return false;
      }
    }
    return true;
  }
  bool parse_value(std::vector<CalcTerm>* out) {
    if (pos >= toks.size()) return false;
    const std::string& t = toks[pos];
    if (t.size() > 1 && t.front() == '(') {
      pos++;
      CalcParser sub(t.substr(1, t.size() - 2), rc);
      std::vector<CalcTerm> inner;
      if (!sub.parse_sum(&inner)) return false;
      *out = inner;
      return true;
    }
    pos++;
    Length L = parse_length_component(t);
    if (L.unit == L_AUTO || L.unit == L_NORMAL) return false;
    CalcTerm ct;
    ct.val = L.val;
    ct.unit = (uint8_t)L.unit;
    ct.mul = 1;
    out->push_back(ct);
    return true;
  }
};

static bool parse_calc(const std::string& inner, Length* out, const ResolveCtx& rc) {
  CalcParser p(inner, rc);
  std::vector<CalcTerm> terms;
  if (!p.parse_sum(&terms)) return false;
  if (terms.size() == 1) {
    out->unit = (LUnit)terms[0].unit;
    out->val = terms[0].val;
    return true;
  }
  out->unit = L_CALC;
  out->ncalc = (uint8_t)std::min(terms.size(), (size_t)8);
  for (int i = 0; i < out->ncalc; i++) out->terms[i] = terms[i];
  return true;
}

/* split top-level commas (paren aware) */
static void split_top_commas(const std::string& s, std::vector<std::string>* out) {
  int pd = 0;
  std::string cur;
  for (char c : s) {
    if (c == '(') pd++;
    if (c == ')') pd--;
    if (c == ',' && pd == 0) { out->push_back(trim(cur)); cur.clear(); continue; }
    cur += c;
  }
  if (!trim(cur).empty()) out->push_back(trim(cur));
}

/* flatten a Length into CalcTerms so clamp bounds are value-copyable */
static void length_to_terms(const Length& L, CalcTerm* out, uint8_t* n) {
  if (L.unit == L_CALC) {
    *n = (uint8_t)std::min((size_t)L.ncalc, (size_t)8);
    for (int i = 0; i < *n; i++) out[i] = L.terms[i];
    return;
  }
  out[0] = CalcTerm{L.val, (uint8_t)L.unit, 1};
  *n = 1;
}

/* parse any length value: single component, calc(...) or clamp(a, b, c) */
static bool parse_length(const std::string& raw, Length* out, const ResolveCtx& rc) {
  std::string v = trim(raw);
  if (v.rfind("clamp(", 0) == 0 && v.back() == ')') {
    std::vector<std::string> parts;
    split_top_commas(v.substr(6, v.size() - 7), &parts);
    if (parts.size() == 3) {
      Length mn, mid, mx;
      parse_length(parts[0], &mn, rc);
      parse_length(parts[1], &mid, rc);
      parse_length(parts[2], &mx, rc);
      *out = Length();
      if (mid.unit == L_CALC) {
        out->ncalc = (uint8_t)std::min((size_t)mid.ncalc, (size_t)8);
        for (int i = 0; i < out->ncalc; i++) out->terms[i] = mid.terms[i];
      } else if (mid.unit != L_AUTO && mid.unit != L_NORMAL) {
        out->ncalc = 1;
        out->terms[0] = CalcTerm{mid.val, (uint8_t)mid.unit, 1};
      } else {
        return true; /* auto middle: no clamp possible */
      }
      out->unit = L_CLAMP;
      out->has_clamp = 1;
      length_to_terms(mn, out->terms_min, &out->nmin);
      length_to_terms(mx, out->terms_max, &out->nmax);
      return true;
    }
  }
  if (v.rfind("calc(", 0) == 0) {
    return parse_calc(v.substr(5, v.size() - 6), out, rc);
  }
  *out = parse_length_component(v);
  return true;
}

/* colors ---------------------------------------------------------------- */
static bool parse_css_color(const std::string& raw, const ResolveCtx& rc, Color* out) {
  std::string s = trim(raw);
  if (s == "currentcolor" || s == "currentColor") { *out = rc.current_color; return true; }
  if (s == "transparent") { *out = Color{0, 0, 0, 0}; return true; }
  if (s == "none") { *out = Color{0, 0, 0, 0}; return true; }
  if (s == "inherit" || s.empty()) return false;
  if (s[0] == '#') {
    auto hexv = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    std::string h = s.substr(1);
    if (h.size() == 3 || h.size() == 4) {
      int r = hexv(h[0]), g = hexv(h[1]), b = hexv(h[2]);
      int a = h.size() == 4 ? hexv(h[3]) : 15;
      if (r < 0) return false;
      *out = Color{(uint8_t)(a * 17), (uint8_t)(r * 17), (uint8_t)(g * 17), (uint8_t)(b * 17)};
      return true;
    }
    if (h.size() == 6 || h.size() == 8) {
      int r = (hexv(h[0]) << 4) | hexv(h[1]);
      int g = (hexv(h[2]) << 4) | hexv(h[3]);
      int b = (hexv(h[4]) << 4) | hexv(h[5]);
      int a = h.size() == 8 ? ((hexv(h[6]) << 4) | hexv(h[7])) : 255;
      if (r < 0) return false;
      *out = Color{(uint8_t)a, (uint8_t)r, (uint8_t)g, (uint8_t)b};
      return true;
    }
    return false;
  }
  std::string ls = lc(s);
  if (starts_with(ls, "rgb(") || starts_with(ls, "rgba(") || starts_with(ls, "hsl(") || starts_with(ls, "hsla(")) {
    bool is_hsl = starts_with(ls, "hsl");
    size_t a = ls.find('('), b = ls.rfind(')');
    if (a == std::string::npos || b == std::string::npos) return false;
    std::string body = s.substr(a + 1, b - a - 1);
    /* split by commas or spaces (space syntax with / alpha) */
    std::vector<std::string> parts;
    std::vector<std::string> alpha_part;
    bool in_alpha = false;
    {
      std::string cur;
      int paren = 0;
      int comma_parts = 0;
      for (char c : body) {
        if (c == '(') paren++;
        if (c == ')') paren--;
        if ((c == ',' && paren == 0) || (c == ' ' && paren == 0) || (c == '/' && paren == 0)) {
          if (c == '/') in_alpha = true;
          if (!trim(cur).empty()) {
            if (in_alpha) alpha_part.push_back(trim(cur));
            else parts.push_back(trim(cur));
            /* legacy rgba()/hsla(): after the 3rd channel, further values are alpha */
            if (!in_alpha && parts.size() >= 3) in_alpha = true;
          }
          cur.clear();
          continue;
        }
        cur += c;
      }
      if (!trim(cur).empty()) {
        if (in_alpha) alpha_part.push_back(trim(cur));
        else parts.push_back(trim(cur));
      }
    }
    float vals[3] = {0, 0, 0};
    float alpha = 1;
    for (int k = 0; k < 3 && k < (int)parts.size(); k++) {
      std::string p = parts[k];
      bool pct = !p.empty() && p.back() == '%';
      vals[k] = (float)atof(p.c_str());
      if (pct && !is_hsl) vals[k] = vals[k] * 255.f / 100.f;
      if (pct && is_hsl) vals[k] = vals[k]; /* degrees/hsl percents stay */
    }
    if (!alpha_part.empty()) {
      std::string ap = alpha_part[0];
      if (!ap.empty() && ap.back() == '%') alpha = atof(ap.c_str()) / 100.f;
      else alpha = (float)atof(ap.c_str());
    }
    if (raw.find("rgba(200") != std::string::npos)
      engine_log(1, "[color-dbg] raw='%.40s' parts=%zu alpha_part=%zu alpha=%.2f", raw.c_str(),
                 parts.size(), alpha_part.size(), alpha);
    if (is_hsl) {
      float h = fmodf(vals[0], 360), sat = clamp01(vals[1] / 100), lig = clamp01(vals[2] / 100);
      float c = (1 - fabsf(2 * lig - 1)) * sat;
      float x = c * (1 - fabsf(fmodf(h / 60, 2) - 1));
      float m = lig - c / 2;
      float r = 0, g = 0, bl = 0;
      if (h < 60) { r = c; g = x; }
      else if (h < 120) { r = x; g = c; }
      else if (h < 180) { g = c; bl = x; }
      else if (h < 240) { g = x; bl = c; }
      else if (h < 300) { r = x; bl = c; }
      else { r = c; bl = x; }
      *out = Color{(uint8_t)(alpha * 255), (uint8_t)((r + m) * 255), (uint8_t)((g + m) * 255), (uint8_t)((bl + m) * 255)};
      return true;
    }
    *out = Color{(uint8_t)(clamp01(alpha) * 255),
                 (uint8_t)std::min(255.f, std::max(0.f, vals[0])),
                 (uint8_t)std::min(255.f, std::max(0.f, vals[1])),
                 (uint8_t)std::min(255.f, std::max(0.f, vals[2]))};
    return true;
  }
  /* named colors (subset) */
  struct { const char* n; uint32_t v; } named[] = {
    {"white", 0xFFFFFF}, {"black", 0x000000}, {"red", 0xFF0000}, {"green", 0x008000},
    {"lime", 0x00FF00}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00}, {"orange", 0xFFA500},
    {"purple", 0x800080}, {"gray", 0x808080}, {"grey", 0x808080}, {"silver", 0xC0C0C0},
    {"maroon", 0x800000}, {"navy", 0x000080}, {"teal", 0x008080}, {"olive", 0x808000},
    {"cyan", 0x00FFFF}, {"aqua", 0x00FFFF}, {"magenta", 0xFF00FF}, {"fuchsia", 0xFF00FF},
    {"pink", 0xFFC0CB}, {"gold", 0xFFD700}, {"beige", 0xF5F5DC}, {"ivory", 0xFFFFF0},
    {"brown", 0xA52A2A}, {"darkgray", 0xA9A9A9}, {"darkgrey", 0xA9A9A9},
    {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3}, {"dimgray", 0x696969},
    {"slategray", 0x708090}, {"whitesmoke", 0xF5F5F5}, {"gainsboro", 0xDCDCDC},
    {"lightblue", 0xADD8E6}, {"lightgreen", 0x90EE90}, {"lightyellow", 0xFFFFE0},
    {"darkred", 0x8B0000}, {"darkblue", 0x00008B}, {"darkgreen", 0x006400},
    {"steelblue", 0x4682B4}, {"royalblue", 0x4169E1}, {"skyblue", 0x87CEEB},
    {"lightsteelblue", 0xB0C4DE}, {"cornflowerblue", 0x6495ED}, {"dodgerblue", 0x1E90FF},
    {"aliceblue", 0xF0F8FF}, {"lavender", 0xE6E6FA}, {"orange", 0xFFA500},
    {"orangered", 0xFF4500}, {"crimson", 0xDC143C}, {"tomato", 0xFF6347},
    {"khaki", 0xF0E68C}, {"salmon", 0xFA8072}, {"plum", 0xDDA0DD}, {"violet", 0xEE82EE},
    {"orchid", 0xDA70D6}, {"indigo", 0x4B0082}, {"coral", 0xFF7F50}, {"chocolate", 0xD2691E},
    {"tan", 0xD2B48C}, {"wheat", 0xF5DEB3}, {"azure", 0xF0FFFF}, {"ghostwhite", 0xF8F8FF},
    {"honeydew", 0xF0FFF0}, {"mintcream", 0xF5FFFA}, {"linen", 0xFAF0E6},
    {"floralwhite", 0xFFFAF0}, {"oldlace", 0xFDF5E6}, {"papayawhip", 0xFFEFD5},
    {"blanchedalmond", 0xFFEBCD}, {"bisque", 0xFFE4C4}, {"moccasin", 0xFFE4B5},
    {"navajowhite", 0xFFDEAD}, {"peachpuff", 0xFFDAB9}, {"mistyrose", 0xFFE4E1},
    {"seashell", 0xFFF5EE}, {"snow", 0xFFFAFA}, {"white_smoke", 0xF5F5F5},
  };
  for (auto& nn : named) {
    if (ls == nn.n) {
      *out = Color{255, (uint8_t)(nn.v >> 16), (uint8_t)((nn.v >> 8) & 255), (uint8_t)(nn.v & 255)};
      return true;
    }
  }
  return false;
}

/* gradients -------------------------------------------------------------- */
static bool parse_gradient(const std::string& raw, BgLayer* layer, const ResolveCtx& rc) {
  size_t a = raw.find('(');
  if (a == std::string::npos) return false;
  std::string head = lc(trim(raw.substr(0, a)));
  /* find matching close of the gradient */
  int d = 0;
  size_t end = raw.size();
  for (size_t i = a; i < raw.size(); i++) {
    if (raw[i] == '(') d++;
    if (raw[i] == ')') { d--; if (!d) { end = i + 1; break; } }
  }
  std::string body = raw.substr(a + 1, end - a - 2);
  layer->kind = BG_LINEAR;
  layer->grad.kind = 2;
  /* split top-level commas */
  std::vector<std::string> args;
  {
    std::string cur;
    int paren = 0;
    for (char c : body) {
      if (c == '(') paren++;
      if (c == ')') paren--;
      if (c == ',' && paren == 0) {
        args.push_back(trim(cur));
        cur.clear();
        continue;
      }
      cur += c;
    }
    if (!trim(cur).empty()) args.push_back(trim(cur));
  }
  size_t stop_start = 0;
  if (head == "linear-gradient") {
    layer->grad.kind = 2;
    layer->grad.angle = 180;
    if (!args.empty()) {
      std::string a0 = lc(args[0]);
      bool is_angle = !a0.empty() && (isdigit((unsigned char)a0[0]) || a0[0] == '-');
      bool is_dir = a0.find("to ") == 0;
      if (is_angle || is_dir) {
        stop_start = 1;
        if (is_angle) layer->grad.angle = (float)atof(a0.c_str());
        else {
          if (a0.find("top") != std::string::npos && a0.find("left") != std::string::npos) layer->grad.angle = 315;
          else if (a0.find("top") != std::string::npos && a0.find("right") != std::string::npos) layer->grad.angle = 45;
          else if (a0.find("bottom") != std::string::npos && a0.find("left") != std::string::npos) layer->grad.angle = 225;
          else if (a0.find("bottom") != std::string::npos && a0.find("right") != std::string::npos) layer->grad.angle = 135;
          else if (a0.find("top") != std::string::npos) layer->grad.angle = 0;
          else if (a0.find("right") != std::string::npos) layer->grad.angle = 90;
          else if (a0.find("bottom") != std::string::npos) layer->grad.angle = 180;
          else layer->grad.angle = 270;
        }
      }
    }
  } else if (head == "radial-gradient") {
    layer->kind = BG_RADIAL;
    layer->grad.kind = 3;
    layer->grad.cx = 0.5f;
    layer->grad.cy = 0.5f;
    if (!args.empty()) {
      std::string a0 = lc(args[0]);
      if (a0.find("at ") != std::string::npos || a0.find("circle") != std::string::npos ||
          a0.find("ellipse") != std::string::npos || a0.find("farthest") != std::string::npos ||
          a0.find("closest") != std::string::npos) {
        stop_start = 1;
        size_t at = a0.find("at ");
        if (at != std::string::npos) {
          std::string pos = a0.substr(at + 3);
          size_t sp = pos.find(' ');
          auto pct = [&](std::string t, bool x) -> float {
            if (!t.empty() && t.back() == '%') return (float)atof(t.c_str()) / 100.f;
            if (t == "left") return 0;
            if (t == "right") return 1;
            if (t == "top") return x ? 0.5f : 0;
            if (t == "bottom") return x ? 0.5f : 1;
            if (t == "center") return 0.5f;
            return 0.5f;
          };
          if (sp != std::string::npos) {
            layer->grad.cx = pct(pos.substr(0, sp), true);
            layer->grad.cy = pct(pos.substr(sp + 1), false);
          }
        }
      }
    }
  } else if (head == "repeating-linear-gradient" || head == "repeating-radial-gradient") {
    layer->kind = head == "repeating-radial-gradient" ? BG_RADIAL : BG_LINEAR;
    layer->grad.kind = layer->kind == BG_RADIAL ? 3 : 2;
    stop_start = head == "repeating-radial-gradient" ? 1 : 1;
    if (head == "repeating-linear-gradient" && !args.empty()) {
      std::string a0 = lc(args[0]);
      if (!a0.empty() && (isdigit((unsigned char)a0[0]) || a0[0] == '-' || a0.find("to ") == 0)) stop_start = 1;
      else stop_start = 0;
    }
  } else {
    return false;
  }
  for (size_t k = stop_start; k < args.size(); k++) {
    std::string arg = args[k];
    /* "color pos" or "pos color" or just color */
    GradStop st;
    /* split top-level spaces */
    std::vector<std::string> toks;
    int paren = 0;
    std::string cur;
    for (char c : arg) {
      if (c == '(') paren++;
      if (c == ')') paren--;
      if (isspace((unsigned char)c) && paren == 0) {
        if (!cur.empty()) toks.push_back(cur);
        cur.clear();
        continue;
      }
      cur += c;
    }
    if (!cur.empty()) toks.push_back(cur);
    if (toks.empty()) continue;
    size_t ci = 0;
    if (!parse_css_color(toks[0], rc, &st.c)) {
      /* maybe pos first */
      if (toks.size() >= 2) {
        st.pos = (float)atof(toks[0].c_str());
        if (toks[0].back() == '%') st.pos /= 100;
        if (!parse_css_color(toks[1], rc, &st.c)) continue;
        ci = 2;
      } else continue;
    } else ci = 1;
    if (ci < toks.size()) {
      std::string p = toks[ci];
      if (!p.empty() && p.back() == '%') st.pos = (float)atof(p.c_str()) / 100.f;
      else st.pos = (float)atof(p.c_str());
    }
    layer->grad.stops.push_back(st);
  }
  if (layer->grad.stops.empty()) return false;
  return true;
}

/* resolve a Length against context at computed time (px result) */
static float resolve_length(const Length& L, float pct_basis, float em, float rem, float vw, float vh) {
  switch (L.unit) {
    case L_AUTO:
    case L_NORMAL:
      return 0;
    case L_PX: return L.val;
    case L_PCT: return L.val * pct_basis / 100.f;
    case L_EM: return L.val * em;
    case L_REM: return L.val * rem;
    case L_VW: return L.val * vw / 100.f;
    case L_VH: return L.val * vh / 100.f;
    case L_CH: return L.val; /* pre-scaled */
    default: break;
  }
  if (L.unit == L_CALC || L.unit == L_CLAMP) {
    float sum = 0;
    for (int i = 0; i < L.ncalc; i++) {
      const CalcTerm& t = L.terms[i];
      switch ((LUnit)t.unit) {
        case L_PX: sum += t.val * t.mul; break;
        case L_PCT: sum += t.val * pct_basis / 100.f * t.mul; break;
        case L_EM: sum += t.val * em * t.mul; break;
        case L_REM: sum += t.val * rem * t.mul; break;
        case L_VW: sum += t.val * vw / 100.f * t.mul; break;
        case L_VH: sum += t.val * vh / 100.f * t.mul; break;
        default: sum += t.val * t.mul; break;
      }
    }
    if (L.has_clamp && L.unit == L_CLAMP) {
      auto sum_terms = [&](const CalcTerm* ts, int n) {
        float s2 = 0;
        for (int i = 0; i < n; i++) {
          switch ((LUnit)ts[i].unit) {
            case L_PX: s2 += ts[i].val * ts[i].mul; break;
            case L_PCT: s2 += ts[i].val * pct_basis / 100.f * ts[i].mul; break;
            case L_EM: s2 += ts[i].val * em * ts[i].mul; break;
            case L_REM: s2 += ts[i].val * rem * ts[i].mul; break;
            case L_VW: s2 += ts[i].val * vw / 100.f * ts[i].mul; break;
            case L_VH: s2 += ts[i].val * vh / 100.f * ts[i].mul; break;
            default: s2 += ts[i].val * ts[i].mul; break;
          }
        }
        return s2;
      };
      if (L.nmin) { float mn = sum_terms(L.terms_min, L.nmin); if (sum < mn) sum = mn; }
      if (L.nmax) { float mx = sum_terms(L.terms_max, L.nmax); if (sum > mx) sum = mx; }
    }
    return sum;
  }
  return L.val;
}

/* ============================ declaration application ============================ */
static void split_values(const std::string& v, std::vector<std::string>* out, const char* seps = " ") {
  out->clear();
  std::string cur;
  int paren = 0;
  for (char c : v) {
    if (c == '(') paren++;
    if (c == ')') paren--;
    if (strchr(seps, c) && paren == 0) {
      if (!trim(cur).empty()) out->push_back(trim(cur));
      cur.clear();
      continue;
    }
    cur += c;
  }
  if (!trim(cur).empty()) out->push_back(trim(cur));
}

static void parse_side_values(const std::string& v, const ResolveCtx& rc, Length out[4]) {
  /* 1-4 values: t r b l */
  std::vector<std::string> toks;
  split_values(v, &toks);
  if (toks.empty()) return;
  Length vals[4];
  int n = 0;
  for (auto& t : toks) {
    if (n >= 4) break;
    parse_length(t, &vals[n], rc);
    n++;
  }
  if (n == 1) { out[0] = out[1] = out[2] = out[3] = vals[0]; }
  else if (n == 2) { out[0] = out[2] = vals[0]; out[1] = out[3] = vals[1]; }
  else if (n == 3) { out[0] = vals[0]; out[1] = out[3] = vals[1]; out[2] = vals[2]; }
  else { for (int i = 0; i < 4; i++) out[i] = vals[i]; }
}

static Display parse_display(const std::string& v) {
  std::string s = lc(trim(v));
  if (s == "none") return D_NONE;
  if (s == "block") return D_BLOCK;
  if (s == "inline") return D_INLINE;
  if (s == "inline-block") return D_INLINE_BLOCK;
  if (s == "flex") return D_FLEX;
  if (s == "inline-flex") return D_INLINE_FLEX;
  if (s == "grid") return D_GRID;
  if (s == "inline-grid") return D_INLINE_GRID;
  if (s == "table") return D_TABLE;
  if (s == "table-row") return D_TABLE_ROW;
  if (s == "table-cell") return D_TABLE_CELL;
  if (s == "list-item") return D_LIST_ITEM;
  if (s == "contents" || s == "flow-root" || s == "table-caption" || s == "table-row-group" ||
      s == "table-header-group" || s == "table-footer-group")
    return s == "contents" ? D_NONE : D_BLOCK; /* flow-root -> block approximation */
  return D_INLINE;
}

static void parse_background(const std::string& raw, ComputedStyle* cs, const ResolveCtx& rc) {
  /* background: color image pos/size repeat */
  std::vector<std::string> toks;
  {
    std::string cur;
    int paren = 0;
    for (char c : raw) {
      if (c == '(') paren++;
      if (c == ')') paren--;
      if (isspace((unsigned char)c) && paren == 0) {
        if (!cur.empty()) toks.push_back(cur);
        cur.clear();
        continue;
      }
      cur += c;
    }
    if (!cur.empty()) toks.push_back(cur);
  }
  BgLayer layer;
  for (size_t k = 0; k < toks.size(); k++) {
    const std::string& t = toks[k];
    std::string lt = lc(t);
    if (lt.rfind("url(", 0) == 0) {
      layer.kind = BG_URL;
      CssCursor tc(t);
      layer.url = tc.read_url();
    } else if (lt.find("gradient(") != std::string::npos) {
      std::string g = t;
      /* gradient may span multiple toks if it has spaces — rejoin until parens balance */
      std::string joined = t;
      int paren = 0;
      for (char c : t) { if (c == '(') paren++; if (c == ')') paren--; }
      while (paren > 0 && k + 1 < toks.size()) {
        joined += " " + toks[++k];
        for (char c : toks[k]) { if (c == '(') paren++; if (c == ')') paren--; }
      }
      layer.kind = BG_LINEAR;
      parse_gradient(joined, &layer, rc);
    } else if (lt.find("/") != std::string::npos) {
      /* pos/size */
      size_t sl = lt.find('/');
      std::string pos = t.substr(0, sl);
      std::string size = t.substr(sl + 1);
      parse_length(pos, &layer.pos_x, rc);
      if (layer.pos_x.unit == L_AUTO) layer.pos_x.unit = L_PCT;
      /* size may be "cover"/"contain"/"auto" or lengths (possibly 2 toks) */
      if (size == "cover") layer.size_kind = 1;
      else if (size == "contain") layer.size_kind = 2;
      else {
        layer.size_kind = 3;
        parse_length(size, &layer.size_w, rc);
        if (k + 1 < toks.size() && toks[k + 1] != "," ) {
          std::string h = toks[k + 1];
          if (h != "repeat" && h != "no-repeat" && h != "round" && h != "space") {
            parse_length(h, &layer.size_h, rc);
            k++;
          } else layer.size_h.unit = L_AUTO;
        } else layer.size_h.unit = L_AUTO;
      }
    } else if (lt == "no-repeat") layer.repeat = false;
    else if (lt == "repeat" || lt == "repeat-x" || lt == "repeat-y" || lt == "round" || lt == "space") layer.repeat = true;
    else if (lt == "fixed" || lt == "scroll" || lt == "local" || lt == "border-box" ||
             lt == "padding-box" || lt == "content-box" || lt == "text") {
      /* bg-attachment/clip: ignore */
    } else if (lt == "center" || lt == "left" || lt == "right" || lt == "top" || lt == "bottom") {
      if (lt == "center") { layer.pos_x.unit = L_PCT; layer.pos_x.val = 50; layer.pos_y.unit = L_PCT; layer.pos_y.val = 50; }
      else if (lt == "left") { layer.pos_x.unit = L_PCT; layer.pos_x.val = 0; }
      else if (lt == "right") { layer.pos_x.unit = L_PCT; layer.pos_x.val = 100; }
      else if (lt == "top") { layer.pos_y.unit = L_PCT; layer.pos_y.val = 0; }
      else if (lt == "bottom") { layer.pos_y.unit = L_PCT; layer.pos_y.val = 100; }
    } else {
      Color c;
      if (parse_css_color(t, rc, &c)) cs->bg_color = c;
      else {
        Length L;
        parse_length(t, &L, rc);
        if (L.unit == L_PX || L.unit == L_PCT || L.unit == L_CALC) layer.pos_x = L;
      }
    }
  }
  if (layer.kind != BG_NONE) cs->bgs.push_back(layer);
}

static Track parse_track(const std::string& t) {
  Track tr;
  std::string s = lc(trim(t));
  if (s == "auto" || s.empty()) { tr.type = TR_AUTO; return tr; }
  if (!s.empty() && s.back() == '%') { tr.type = TR_PCT; tr.v = (float)atof(s.c_str()); return tr; }
  if (s.find("fr") == s.size() - 2) { tr.type = TR_FR; tr.v = (float)atof(s.c_str()); return tr; }
  if (s.rfind("minmax(", 0) == 0) {
    tr.type = TR_MINMAX;
    std::string inner = s.substr(7, s.size() - 8);
    size_t comma = inner.find(',');
    if (comma != std::string::npos) {
      tr.mn = std::make_shared<Track>(parse_track(inner.substr(0, comma)));
      tr.mx = std::make_shared<Track>(parse_track(inner.substr(comma + 1)));
    } else {
      tr.mn = std::make_shared<Track>(parse_track(inner));
      tr.mx = std::make_shared<Track>(TR_AUTO);
    }
    return tr;
  }
  if (s == "min-content" || s == "max-content" || s == "fit-content") { tr.type = TR_AUTO; return tr; }
  tr.type = TR_PX;
  tr.v = (float)atof(s.c_str());
  return tr;
}

static void parse_grid_tracks(const std::string& v, std::vector<Track>* out,
                              std::vector<Track>* auto_out = nullptr,
                              uint8_t* auto_mode = nullptr) {
  /* handle repeat(N, ...) */
  std::string s = trim(v);
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    if (i >= s.size()) break;
    if (lc(s).rfind("repeat(", i) == i) {
      size_t rp = i + 7;
      int d = 1;
      size_t e = rp;
      while (e < s.size() && d > 0) {
        if (s[e] == '(') d++;
        if (s[e] == ')') d--;
        e++;
      }
      std::string inner = s.substr(rp, e - rp - 1);
      i = e;
      size_t comma = inner.find(',');
      std::string count_tok = comma != std::string::npos ? lc(trim(inner.substr(0, comma))) : "1";
      std::string tracks = comma != std::string::npos ? inner.substr(comma + 1) : inner;
      if (count_tok == "auto-fit" || count_tok == "auto-fill") {
        /* resolve at layout time against the available width */
        if (auto_out && auto_mode) {
          *auto_mode = count_tok == "auto-fit" ? 1 : 2;
          auto_out->clear();
          size_t p2 = 0;
          int pd2 = 0;
          std::string cur2;
          std::vector<std::string> list2;
          for (char cc : tracks) {
            if (cc == '(') pd2++;
            if (cc == ')') pd2--;
            if (cc == ',' && pd2 == 0) { list2.push_back(trim(cur2)); cur2.clear(); continue; }
            cur2 += cc;
          }
          if (!trim(cur2).empty()) list2.push_back(trim(cur2));
          for (auto& t : list2) auto_out->push_back(parse_track(t));
        }
        return;
      }
      int count = atoi(count_tok.c_str());
      if (count <= 0 || count > 64) count = 1;
      for (int k = 0; k < count; k++) {
        /* each comma-separated track inside */
        size_t p = 0;
        int pd = 0;
        std::string cur;
        std::vector<std::string> list;
        for (char c : tracks) {
          if (c == '(') pd++;
          if (c == ')') pd--;
          if (c == ',' && pd == 0) { list.push_back(trim(cur)); cur.clear(); continue; }
          cur += c;
        }
        if (!trim(cur).empty()) list.push_back(trim(cur));
        for (auto& t : list) out->push_back(parse_track(t));
      }
      continue;
    }
    size_t e = i;
    int pd = 0;
    while (e < s.size()) {
      if (s[e] == '(') pd++;
      if (s[e] == ')') pd--;
      if (isspace((unsigned char)s[e]) && pd == 0) break;
      e++;
    }
    out->push_back(parse_track(s.substr(i, e - i)));
    i = e;
  }
}

static void parse_box_shadows(const std::string& v, ComputedStyle* cs, const ResolveCtx& rc) {
  /* split top-level commas */
  std::vector<std::string> parts;
  {
    std::string cur;
    int paren = 0;
    for (char c : v) {
      if (c == '(') paren++;
      if (c == ')') paren--;
      if (c == ',' && paren == 0) {
        if (!trim(cur).empty()) parts.push_back(trim(cur));
        cur.clear();
        continue;
      }
      cur += c;
    }
    if (!trim(cur).empty()) parts.push_back(trim(cur));
  }
  for (auto& p : parts) {
    if (lc(trim(p)) == "none") continue;
    Shadow sh;
    std::vector<std::string> toks;
    split_values(p, &toks);
    float nums[4] = {0, 0, 0, 0};
    int ni = 0;
    bool have_color = false;
    for (auto& t : toks) {
      std::string lt = lc(t);
      if (lt == "inset") { sh.inset = true; continue; }
      Color c;
      if (!have_color && parse_css_color(t, rc, &c) && !isdigit((unsigned char)t[0]) && t[0] != '-' && t[0] != '.') {
        sh.color = c;
        have_color = true;
        continue;
      }
      if (ni < 4) nums[ni++] = (float)atof(t.c_str());
    }
    sh.dx = nums[0];
    sh.dy = nums[1];
    sh.blur = nums[2];
    sh.spread = nums[3];
    if (!have_color) sh.color = Color{102, 0, 0, 0};
    cs->shadows.push_back(sh);
  }
}

static void parse_transform(const std::string& v, ComputedStyle* cs) {
  /* identity */
  float acc[6] = {1, 0, 0, 1, 0, 0};
  auto compose = [&](const float m[6]) {
    float a0 = acc[0] * m[0] + acc[2] * m[1];
    float a1 = acc[1] * m[0] + acc[3] * m[1];
    float a2 = acc[0] * m[2] + acc[2] * m[3];
    float a3 = acc[1] * m[2] + acc[3] * m[3];
    float a4 = acc[0] * m[4] + acc[2] * m[5] + acc[4];
    float a5 = acc[1] * m[4] + acc[3] * m[5] + acc[5];
    float next[6] = {a0, a1, a2, a3, a4, a5};
    memcpy(acc, next, sizeof(acc));
  };
  size_t i = 0, n = v.size();
  while (i < n) {
    while (i < n && !isalpha((unsigned char)v[i])) i++;
    if (i >= n) break;
    size_t ns = i;
    while (i < n && isalpha((unsigned char)v[i])) i++;
    std::string fn = lc(v.substr(ns, i - ns));
    while (i < n && v[i] != '(') i++;
    if (i >= n) break;
    i++;
    size_t pe = v.find(')', i);
    if (pe == std::string::npos) break;
    std::string body = v.substr(i, pe - i);
    i = pe + 1;
    std::vector<std::string> toks;
    split_values(body, &toks, " ,");
    std::vector<float> x;
    for (auto& t : toks) {
      if (!t.empty() && t.back() == '%') x.push_back((float)atof(t.c_str())); /* % treated as px here (rare) */
      else x.push_back((float)atof(t.c_str()));
    }
    auto arg = [&](size_t k, float d) { return k < x.size() ? x[k] : d; };
    if (fn == "translate") {
      float m[6] = {1, 0, 0, 1, arg(0, 0), arg(1, 0)};
      compose(m);
    } else if (fn == "translatex") {
      float m[6] = {1, 0, 0, 1, arg(0, 0), 0};
      compose(m);
    } else if (fn == "translatey") {
      float m[6] = {1, 0, 0, 1, 0, arg(0, 0)};
      compose(m);
    } else if (fn == "scale") {
      float sx = arg(0, 1);
      float m[6] = {sx, 0, 0, arg(1, sx), 0, 0};
      compose(m);
    } else if (fn == "scalex") {
      float m[6] = {arg(0, 1), 0, 0, 1, 0, 0};
      compose(m);
    } else if (fn == "scaley") {
      float m[6] = {1, 0, 0, arg(0, 1), 0, 0};
      compose(m);
    } else if (fn == "rotate") {
      float a = arg(0, 0) * 3.14159265f / 180.f;
      float m[6] = {cosf(a), sinf(a), -sinf(a), cosf(a), 0, 0};
      compose(m);
    } else if (fn == "matrix") {
      float m[6] = {arg(0, 1), arg(1, 0), arg(2, 0), arg(3, 1), arg(4, 0), arg(5, 0)};
      compose(m);
    } else if (fn == "skewx") {
      float t = tanf(arg(0, 0) * 3.14159265f / 180.f);
      float m[6] = {1, 0, t, 1, 0, 0};
      compose(m);
    } else if (fn == "skewy") {
      float t = tanf(arg(0, 0) * 3.14159265f / 180.f);
      float m[6] = {1, t, 0, 1, 0, 0};
      compose(m);
    }
  }
  bool identity = acc[0] == 1 && acc[1] == 0 && acc[2] == 0 && acc[3] == 1 && acc[4] == 0 && acc[5] == 0;
  if (!identity) {
    memcpy(cs->mat, acc, sizeof(acc));
    cs->has_transform = true;
  }
}

/* apply one declaration to computed style */
static bool apply_declaration(ComputedStyle* cs, const std::string& prop_in, const std::string& raw_value,
                              ResolveCtx& rc, CustomProps* own_vars) {
  std::string prop = lc(trim(prop_in));
  std::string v = trim(raw_value);
  if (v.empty()) return false;
  if (starts_with(prop, "--")) {
    if (own_vars) (*own_vars)[prop] = v;
    return true;
  }
  if (v == "inherit") return false; /* handled by inheritance defaults */
  if (v == "initial" || v == "unset" || v == "revert") return false;

  /* font-size first (em basis for the rest) */
  if (prop == "font-size") {
    std::string s = lc(v);
    if (s == "small" || s == "smaller") { cs->font_size = rc.em * 0.85f; return true; }
    if (s == "large" || s == "larger") { cs->font_size = rc.em * 1.2f; return true; }
    if (s == "x-large") { cs->font_size = rc.em * 1.5f; return true; }
    if (s == "xx-large") { cs->font_size = rc.em * 2; return true; }
    if (s == "medium") { cs->font_size = 16; return true; }
    Length L;
    std::string sub = substitute_vars(v, rc);
    parse_length(sub, &L, rc);
    if (L.unit == L_AUTO || L.unit == L_NORMAL) return false;
    float val = resolve_length(L, rc.em /* % of parent font size */, rc.em, rc.rem, rc.vw, rc.vh);
    if (val > 0) cs->font_size = val;
    return true;
  }
  /* keep rc.em in sync once font-size applied externally */

  if (prop == "display") { cs->display = parse_display(v); return true; }
  if (prop == "visibility") {
    std::string s = lc(trim(v));
    cs->visibility_hidden = s == "hidden" || s == "collapse";
    return true;
  }
  if (prop == "position") {
    std::string s = lc(v);
    if (s == "static") cs->position = P_STATIC;
    else if (s == "relative") cs->position = P_RELATIVE;
    else if (s == "absolute") cs->position = P_ABSOLUTE;
    else if (s == "fixed") cs->position = P_FIXED;
    else if (s == "sticky") cs->position = P_STICKY;
    return true;
  }
  if (prop == "box-sizing") { cs->box_border = lc(v).find("border") != std::string::npos; return true; }
  if (prop == "z-index") {
    if (v == "auto") cs->z_auto = true;
    else { cs->z_auto = false; cs->z_index = atoi(v.c_str()); }
    return true;
  }
  if (prop == "top" || prop == "right" || prop == "bottom" || prop == "left") {
    Length L;
    parse_length(substitute_vars(v, rc), &L, rc);
    if (prop == "top") cs->top = L;
    else if (prop == "right") cs->right = L;
    else if (prop == "bottom") cs->bottom = L;
    else cs->left = L;
    return true;
  }
  if (prop == "inset") {
    Length L[4];
    parse_side_values(substitute_vars(v, rc), rc, L);
    cs->top = L[0]; cs->right = L[1]; cs->bottom = L[2]; cs->left = L[3];
    return true;
  }
  if (starts_with(prop, "inset-")) {
    std::string side = prop.substr(6);
    Length L;
    parse_length(substitute_vars(v, rc), &L, rc);
    if (side == "top") cs->top = L;
    else if (side == "bottom") cs->bottom = L;
    else if (side == "left" || side == "inline-start") cs->left = L;
    else if (side == "right" || side == "inline-end") cs->right = L;
    else if (side == "inline") { cs->left = L; cs->right = L; }
    else if (side == "block") { cs->top = L; cs->bottom = L; }
    return true;
  }
  if (prop == "width" || prop == "height" || prop == "min-width" || prop == "max-width" ||
      prop == "min-height" || prop == "max-height") {
    std::string sub = substitute_vars(v, rc);
    Length L;
    parse_length(sub, &L, rc);
    if (v == "auto") L.unit = L_AUTO;
    if (v == "fit-content" || v == "max-content") { L.unit = L_AUTO; cs->width_content_mode = 1; }
    else if (v == "min-content") { L.unit = L_AUTO; cs->width_content_mode = 2; }
    else if (prop == "width") cs->width_content_mode = 0;
    if (v == "none") L.unit = L_NORMAL;
    if (prop == "width") cs->width = L;
    else if (prop == "height") cs->height = L;
    else if (prop == "min-width") cs->min_w = L;
    else if (prop == "max-width") cs->max_w = L;
    else if (prop == "min-height") cs->min_h = L;
    else cs->max_h = L;
    return true;
  }
  if (prop == "aspect-ratio") {
    size_t sl = v.find('/');
    if (sl != std::string::npos) {
      float a = (float)atof(trim(v.substr(0, sl)).c_str());
      float b = (float)atof(trim(v.substr(sl + 1)).c_str());
      if (a > 0 && b > 0) cs->aspect_ratio = a / b;
    } else {
      float a = (float)atof(v.c_str());
      if (a > 0) cs->aspect_ratio = a;
    }
    return true;
  }
  if (prop == "margin" || prop == "padding") {
    Length L[4];
    parse_side_values(substitute_vars(v, rc), rc, L);
    if (prop == "margin") { for (int i = 0; i < 4; i++) cs->margin[i] = L[i]; }
    else { for (int i = 0; i < 4; i++) cs->padding[i] = L[i]; }
    return true;
  }
  if (starts_with(prop, "margin-")) {
    std::string side = prop.substr(7);
    Length L;
    parse_length(substitute_vars(v, rc), &L, rc);
    if (side == "top") cs->margin[0] = L;
    else if (side == "right" || side == "inline-end") cs->margin[1] = L;
    else if (side == "bottom") cs->margin[2] = L;
    else if (side == "left" || side == "inline-start") cs->margin[3] = L;
    else if (side == "inline") { cs->margin[3] = L; cs->margin[1] = L; }
    else if (side == "block") { cs->margin[0] = L; cs->margin[2] = L; }
    return true;
  }
  if (starts_with(prop, "padding-")) {
    std::string side = prop.substr(8);
    Length L;
    parse_length(substitute_vars(v, rc), &L, rc);
    if (side == "top") cs->padding[0] = L;
    else if (side == "right" || side == "inline-end") cs->padding[1] = L;
    else if (side == "bottom") cs->padding[2] = L;
    else if (side == "left" || side == "inline-start") cs->padding[3] = L;
    else if (side == "inline") { cs->padding[3] = L; cs->padding[1] = L; }
    else if (side == "block") { cs->padding[0] = L; cs->padding[2] = L; }
    return true;
  }
  if (prop == "border" || starts_with(prop, "border-top") || starts_with(prop, "border-right") ||
      starts_with(prop, "border-bottom") || starts_with(prop, "border-left") ||
      starts_with(prop, "border-inline") || starts_with(prop, "border-block") ||
      prop == "border-width" ||
      prop == "border-color" || prop == "border-style") {
    /* find side index */
    int side = -1;
    if (prop.find("-top") != std::string::npos) side = 0;
    else if (prop.find("-right") != std::string::npos) side = 1;
    else if (prop.find("-bottom") != std::string::npos) side = 2;
    else if (prop.find("-left") != std::string::npos) side = 3;
    else if (prop.find("-inline-start") != std::string::npos) side = 3;
    else if (prop.find("-inline-end") != std::string::npos) side = 1;
    else if (prop == "border-inline") side = -2; /* both inline sides */
    else if (prop == "border-block") side = -3;  /* both block sides */
    std::vector<std::string> toks;
    split_values(v, &toks);
    auto apply_side = [&](int s) {
      for (auto& t : toks) {
        std::string lt = lc(t);
        if (lt == "solid") cs->border_style[s] = 1;
        else if (lt == "dashed") cs->border_style[s] = 2;
        else if (lt == "dotted") cs->border_style[s] = 3;
        else if (lt == "none" || lt == "hidden") cs->border_style[s] = 0;
        else if (lt == "thin") cs->border_width[s] = parse_length_component("1px");
        else if (lt == "medium") cs->border_width[s] = parse_length_component("3px");
        else if (lt == "thick") cs->border_width[s] = parse_length_component("5px");
        else {
          Color c;
          if (parse_css_color(t, rc, &c)) cs->border_color[s] = c;
          else {
            Length L;
            parse_length(t, &L, rc);
            if (L.unit == L_PX || L.unit == L_CALC || L.unit == L_EM || L.unit == L_REM) cs->border_width[s] = L;
          }
        }
      }
    };
    if (prop == "border") {
      for (int s = 0; s < 4; s++) apply_side(s);
      if (cs->border_style[0] == 0 && toks.size() >= 2) { /* style missing → still none */ }
    } else if (side == -2) { /* border-inline: start + end */
      apply_side(3);
      apply_side(1);
    } else if (side == -3) { /* border-block: top + bottom */
      apply_side(0);
      apply_side(2);
    } else if (side >= 0) {
      if (prop.find("-width") != std::string::npos) {
        Length L;
        parse_length(substitute_vars(v, rc), &L, rc);
        cs->border_width[side] = L;
      } else if (prop.find("-color") != std::string::npos) {
        Color c;
        if (parse_css_color(v, rc, &c)) cs->border_color[side] = c;
      } else if (prop.find("-style") != std::string::npos) {
        std::string lt = lc(v);
        cs->border_style[side] = lt == "solid" ? 1 : lt == "dashed" ? 2 : lt == "dotted" ? 3 : 0;
      } else if (prop.find("-radius") != std::string::npos) {
        Length L;
        parse_length(substitute_vars(v, rc), &L, rc);
        cs->radius[side] = L;
      } else apply_side(side);
    } else if (prop == "border-width") {
      Length L[4];
      parse_side_values(substitute_vars(v, rc), rc, L);
      for (int s = 0; s < 4; s++) cs->border_width[s] = L[s];
    } else if (prop == "border-color") {
      Color c[4];
      std::vector<std::string> ct;
      split_values(v, &ct);
      for (int s = 0; s < 4; s++) {
        if (parse_css_color(ct[std::min(s, (int)ct.size() - 1)], rc, &c[s])) cs->border_color[s] = c[s];
      }
    } else if (prop == "border-style") {
      std::vector<std::string> st;
      split_values(v, &st);
      for (int s = 0; s < 4; s++) {
        std::string lt = lc(st[std::min(s, (int)st.size() - 1)]);
        cs->border_style[s] = lt == "solid" ? 1 : lt == "dashed" ? 2 : lt == "dotted" ? 3 : 0;
      }
    }
    return true;
  }
  if (prop == "border-radius") {
    /* may contain "/" for elliptical: ignore elliptical second part */
    std::string main = v;
    size_t sl = v.find('/');
    if (sl != std::string::npos) main = v.substr(0, sl);
    Length L[4];
    parse_side_values(substitute_vars(main, rc), rc, L);
    cs->radius[0] = L[0];
    cs->radius[1] = L[1];
    cs->radius[2] = L[2];
    cs->radius[3] = L[3];
    return true;
  }
  if (prop == "background") {
    parse_background(substitute_vars(v, rc), cs, rc);
    return true;
  }
  if (prop == "background-color") {
    Color c;
    if (parse_css_color(substitute_vars(v, rc), rc, &c)) cs->bg_color = c;
    return true;
  }
  if (prop == "background-image") {
    if (lc(trim(v)) == "none") return true;
    std::vector<std::string> imgs;
    {
      std::string cur;
      int paren = 0;
      for (char c : v) {
        if (c == '(') paren++;
        if (c == ')') paren--;
        if (c == ',' && paren == 0) {
          if (!trim(cur).empty()) imgs.push_back(trim(cur));
          cur.clear();
          continue;
        }
        cur += c;
      }
      if (!trim(cur).empty()) imgs.push_back(trim(cur));
    }
    for (auto& img : imgs) {
      BgLayer layer;
      std::string li = lc(img);
      if (li.rfind("url(", 0) == 0) {
        layer.kind = BG_URL;
        CssCursor tc(img);
        layer.url = tc.read_url();
        cs->bgs.push_back(layer);
      } else if (li.find("gradient(") != std::string::npos) {
        if (parse_gradient(img, &layer, rc)) cs->bgs.push_back(layer);
      }
    }
    return true;
  }
  if (prop == "background-size") {
    if (cs->bgs.empty()) cs->bgs.push_back(BgLayer());
    BgLayer& L = cs->bgs.back();
    std::string s = lc(trim(v));
    if (s == "cover") L.size_kind = 1;
    else if (s == "contain") L.size_kind = 2;
    else {
      L.size_kind = 3;
      std::vector<std::string> toks;
      split_values(v, &toks);
      parse_length(substitute_vars(toks[0], rc), &L.size_w, rc);
      if (toks.size() > 1) parse_length(substitute_vars(toks[1], rc), &L.size_h, rc);
      else L.size_h.unit = L_AUTO;
    }
    return true;
  }
  if (prop == "background-position") {
    if (cs->bgs.empty()) cs->bgs.push_back(BgLayer());
    BgLayer& L = cs->bgs.back();
    std::vector<std::string> toks;
    split_values(v, &toks);
    if (toks.empty()) return true;
    auto one = [&](const std::string& t, bool isx) {
      std::string lt = lc(t);
      if (lt == "left" || lt == "top") { (isx ? L.pos_x : L.pos_y) = parse_length_component("0%"); }
      else if (lt == "right" || lt == "bottom") { (isx ? L.pos_x : L.pos_y) = parse_length_component("100%"); }
      else if (lt == "center") { (isx ? L.pos_x : L.pos_y) = parse_length_component("50%"); }
      else parse_length(substitute_vars(t, rc), &(isx ? L.pos_x : L.pos_y), rc);
    };
    if (toks.size() >= 2) { one(toks[0], true); one(toks[1], false); }
    else one(toks[0], true);
    return true;
  }
  if (prop == "background-repeat") {
    if (!cs->bgs.empty()) cs->bgs.back().repeat = lc(trim(v)) != "no-repeat";
    return true;
  }
  if (prop == "color") {
    Color c;
    if (parse_css_color(substitute_vars(v, rc), rc, &c)) cs->color = c;
    return true;
  }
  if (prop == "font-family") {
    cs->font_family.clear();
    std::vector<std::string> fams;
    {
      std::string cur;
      bool in_q = false;
      char q = 0;
      for (char c : v) {
        if (in_q) {
          if (c == q) in_q = false;
          else cur += c;
          continue;
        }
        if (c == '"' || c == '\'') { in_q = true; q = c; continue; }
        if (c == ',') {
          fams.push_back(trim(cur));
          cur.clear();
          continue;
        }
        cur += c;
      }
      fams.push_back(trim(cur));
    }
    for (auto& f : fams) {
      std::string t = trim(f);
      if (!t.empty()) cs->font_family.push_back(t);
    }
    return true;
  }
  if (prop == "font-weight") {
    std::string s = lc(trim(v));
    if (s == "bold" || s == "bolder") cs->font_weight = 700;
    else if (s == "normal") cs->font_weight = 400;
    else if (s == "light") cs->font_weight = 300;
    else cs->font_weight = atoi(s.c_str());
    return true;
  }
  if (prop == "font-style") { cs->italic = lc(v).find("italic") != std::string::npos || lc(v).find("oblique") != std::string::npos; return true; }
  if (prop == "line-height") {
    std::string s = lc(trim(v));
    if (s == "normal") { cs->lh_normal = true; cs->line_height_mul = 1.2f; return true; }
    char* end = nullptr;
    double num = strtod(s.c_str(), &end);
    if (end && *end == 0) {
      cs->lh_normal = false;
      cs->line_height_mul = (float)num;
      cs->line_height.unit = L_NORMAL;
      return true;
    }
    Length L;
    parse_length(substitute_vars(v, rc), &L, rc);
    cs->lh_normal = false;
    cs->line_height = L;
    if (L.unit == L_PCT) cs->line_height_mul = L.val / 100.f;
    return true;
  }
  if (prop == "letter-spacing") {
    if (lc(v) == "normal") { cs->letter_spacing = 0; return true; }
    Length L;
    parse_length(substitute_vars(v, rc), &L, rc);
    cs->letter_spacing = resolve_length(L, cs->font_size, cs->font_size, rc.rem, 0, 0);
    return true;
  }
  if (prop == "text-align") {
    std::string s = lc(v);
    if (s == "center") cs->text_align = TA_CENTER;
    else if (s == "right" || s == "end") cs->text_align = TA_END;
    else if (s == "justify") cs->text_align = TA_JUSTIFY;
    else cs->text_align = TA_START;
    return true;
  }
  if (prop == "text-transform") {
    std::string s = lc(v);
    if (s == "uppercase") cs->text_transform = TT_UPPER;
    else if (s == "lowercase") cs->text_transform = TT_LOWER;
    else if (s == "capitalize") cs->text_transform = TT_CAP;
    else cs->text_transform = TT_NONE;
    return true;
  }
  if (prop == "white-space") {
    std::string s = lc(v);
    if (s == "nowrap") cs->white_space = WS_NOWRAP;
    else if (s == "pre") cs->white_space = WS_PRE;
    else if (s == "pre-wrap") cs->white_space = WS_PRE_WRAP;
    else if (s == "pre-line") cs->white_space = WS_PRE_LINE;
    else cs->white_space = WS_NORMAL;
    return true;
  }
  if (prop == "text-decoration" || starts_with(prop, "text-decoration-")) {
    std::string s = lc(v);
    if (s.find("underline") != std::string::npos) cs->deco |= 1;
    if (s.find("line-through") != std::string::npos) cs->deco |= 2;
    if (s == "none") cs->deco = 0;
    return true;
  }
  if (prop == "vertical-align") {
    std::string s = lc(trim(v));
    if (s == "top") cs->vert_align = VA_TOP;
    else if (s == "middle") cs->vert_align = VA_MIDDLE;
    else if (s == "bottom") cs->vert_align = VA_BOTTOM;
    else if (s == "baseline") cs->vert_align = VA_BASELINE;
    else {
      cs->vert_align = VA_LEN;
      parse_length(substitute_vars(v, rc), &cs->vert_len, rc);
    }
    return true;
  }
  if (prop == "flex-direction") {
    std::string s = lc(v);
    if (s == "row") cs->flex_dir = FD_ROW;
    else if (s == "row-reverse") cs->flex_dir = FD_ROW_REV;
    else if (s == "column") cs->flex_dir = FD_COL;
    else if (s == "column-reverse") cs->flex_dir = FD_COL_REV;
    return true;
  }
  if (prop == "flex-wrap") { cs->flex_wrap = lc(v).find("wrap") != std::string::npos ? (lc(v).find("reverse") != std::string::npos ? FW_WRAP_REV : FW_WRAP) : FW_NOWRAP; return true; }
  if (prop == "justify-content") {
    std::string s = lc(v);
    if (s == "flex-end" || s == "end") cs->justify = J_END;
    else if (s == "center") cs->justify = J_CENTER;
    else if (s == "space-between") cs->justify = J_BETWEEN;
    else if (s == "space-around") cs->justify = J_AROUND;
    else if (s == "space-evenly") cs->justify = J_EVENLY;
    else cs->justify = J_START;
    return true;
  }
  if (prop == "align-items") {
    std::string s = lc(v);
    if (s == "flex-start" || s == "start") cs->align = A_START;
    else if (s == "flex-end" || s == "end") cs->align = A_END;
    else if (s == "center") cs->align = A_CENTER;
    else if (s == "baseline") cs->align = A_BASELINE;
    else cs->align = A_STRETCH;
    return true;
  }
  if (prop == "align-self") {
    if (lc(v) == "auto") { cs->align_self_auto = true; return true; }
    cs->align_self_auto = false;
    std::string s = lc(v);
    if (s == "flex-start" || s == "start") cs->align_self = A_START;
    else if (s == "flex-end" || s == "end") cs->align_self = A_END;
    else if (s == "center") cs->align_self = A_CENTER;
    else if (s == "baseline") cs->align_self = A_BASELINE;
    else cs->align_self = A_STRETCH;
    return true;
  }
  if (prop == "align-content") {
    std::string s = lc(v);
    if (s == "flex-start" || s == "start") cs->align_content = 1;
    else if (s == "center") cs->align_content = 2;
    else if (s == "flex-end" || s == "end") cs->align_content = 3;
    else if (s == "space-between") cs->align_content = 4;
    else cs->align_content = 0;
    return true;
  }
  if (prop == "gap" || prop == "row-gap" || prop == "column-gap") {
    if (prop == "gap") {
      std::vector<std::string> toks;
      split_values(substitute_vars(v, rc), &toks);
      parse_length(toks[0], &cs->gap_row, rc);
      if (toks.size() > 1) parse_length(toks[1], &cs->gap_col, rc);
      else cs->gap_col = cs->gap_row;
    } else if (prop == "row-gap") parse_length(substitute_vars(v, rc), &cs->gap_row, rc);
    else parse_length(substitute_vars(v, rc), &cs->gap_col, rc);
    return true;
  }
  if (prop == "flex") {
    std::string s = lc(trim(v));
    if (s == "none") { cs->flex_grow = 0; cs->flex_shrink = 0; cs->flex_basis.unit = L_AUTO; return true; }
    if (s == "auto") { cs->flex_grow = 1; cs->flex_shrink = 1; cs->flex_basis.unit = L_AUTO; return true; }
    if (s == "initial") { cs->flex_grow = 0; cs->flex_shrink = 1; cs->flex_basis.unit = L_AUTO; return true; }
    std::vector<std::string> toks;
    split_values(s, &toks);
    if (!toks.empty()) cs->flex_grow = (float)atof(toks[0].c_str());
    if (toks.size() > 1) cs->flex_shrink = (float)atof(toks[1].c_str());
    if (toks.size() > 2) parse_length(substitute_vars(toks[2], rc), &cs->flex_basis, rc);
    else cs->flex_basis.unit = L_PCT, cs->flex_basis.val = 0; /* flex:1 → basis 0% */
    return true;
  }
  if (prop == "flex-grow") { cs->flex_grow = (float)atof(v.c_str()); return true; }
  if (prop == "flex-shrink") { cs->flex_shrink = (float)atof(v.c_str()); return true; }
  if (prop == "flex-basis") {
    parse_length(substitute_vars(v, rc), &cs->flex_basis, rc);
    if (v == "auto") cs->flex_basis.unit = L_AUTO;
    return true;
  }
  if (prop == "order") { cs->order = atoi(v.c_str()); return true; }
  if (prop == "grid-template-columns" || prop == "grid-template-rows") {
    /* cascade: later declarations REPLACE earlier tracks, never append */
    auto* vec = prop == "grid-template-columns" ? &cs->grid_cols : &cs->grid_rows;
    vec->clear();
    if (prop == "grid-template-columns") {
      cs->grid_auto_template.clear();
      cs->grid_auto_mode = 0;
      parse_grid_tracks(substitute_vars(v, rc), vec, &cs->grid_auto_template,
                        &cs->grid_auto_mode);
    } else {
      parse_grid_tracks(substitute_vars(v, rc), vec);
    }
    return true;
  }
  if (prop == "grid-column" || prop == "grid-row") {
    std::string s = lc(trim(v));
    if (starts_with(s, "span ")) {
      int n = atoi(s.c_str() + 5);
      if (prop == "grid-column") cs->grid_col_span = std::max(1, n);
      else cs->grid_row_span = std::max(1, n);
    }
    /* "2 / span 3" forms: take span part */
    size_t sp = s.find("span");
    if (sp != std::string::npos) {
      int n = atoi(trim(s.substr(sp + 4)).c_str());
      if (n > 0) {
        if (prop == "grid-column") cs->grid_col_span = n;
        else cs->grid_row_span = n;
      }
    }
    return true;
  }
  if (prop == "overflow" || prop == "overflow-x" || prop == "overflow-y") {
    Overflow o = O_VISIBLE;
    std::string s = lc(trim(v));
    if (s == "hidden" || s == "clip") o = s == "hidden" ? O_HIDDEN : O_CLIP;
    else if (s == "scroll") o = O_SCROLL;
    else if (s == "auto") o = O_AUTO;
    if (prop == "overflow") { cs->overflow_x = cs->overflow_y = o; }
    else if (prop == "overflow-x") cs->overflow_x = o;
    else cs->overflow_y = o;
    return true;
  }
  if (prop == "opacity") {
    std::string s = lc(trim(v));
    float val = (float)atof(s.c_str());
    if (!s.empty() && s.back() == '%') val /= 100.f; /* opacity: 50% == 0.5 */
    cs->opacity = clamp01(val);
    return true;
  }
  if (prop == "box-shadow") {
    parse_box_shadows(substitute_vars(v, rc), cs, rc);
    return true;
  }
  if (prop == "transform") {
    if (lc(trim(v)) == "none") { cs->has_transform = false; return true; }
    parse_transform(substitute_vars(v, rc), cs);
    return true;
  }
  if (prop == "transform-origin") {
    std::vector<std::string> toks;
    split_values(v, &toks);
    auto one = [&](const std::string& t, Length& L, bool isx) {
      std::string lt = lc(t);
      if (lt == "left" || lt == "top") { L.unit = L_PCT; L.val = 0; }
      else if (lt == "right" || lt == "bottom") { L.unit = L_PCT; L.val = 100; }
      else if (lt == "center") { L.unit = L_PCT; L.val = 50; }
      else parse_length(substitute_vars(t, rc), &L, rc);
    };
    if (toks.size() >= 2) { one(toks[0], cs->tx_ox, true); one(toks[1], cs->tx_oy, false); }
    else if (toks.size() == 1) { one(toks[0], cs->tx_ox, true); cs->tx_oy.unit = L_PCT, cs->tx_oy.val = 50; }
    return true;
  }
  if (prop == "object-fit") {
    std::string s = lc(trim(v));
    if (s == "cover") cs->object_fit = FIT_COVER;
    else if (s == "contain") cs->object_fit = FIT_CONTAIN;
    else if (s == "none") cs->object_fit = FIT_NONE;
    else cs->object_fit = FIT_FILL;
    return true;
  }
  if (prop == "filter") {
    std::string s = substitute_vars(v, rc);
    std::string ls = lc(s);
    if (ls == "none" || trim(ls).empty()) return true;
    size_t p = ls.find("blur(");
    if (p != std::string::npos) cs->blur_px = (float)atof(ls.c_str() + p + 5);
    return true;
  }
  if (prop == "backdrop-filter") {
    std::string s = substitute_vars(v, rc);
    std::string ls = lc(s);
    if (ls == "none" || trim(ls).empty()) return true;
    size_t p = ls.find("blur(");
    if (p != std::string::npos) cs->backdrop_blur_px = (float)atof(ls.c_str() + p + 5);
    return true;
  }
  if (prop == "content") {
    std::string s = trim(v);
    if (s == "none" || s == "normal") { cs->content_set = false; return true; }
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'')) {
      cs->content = s.substr(1, s.size() - 2);
      cs->content_set = true;
      return true;
    }
    if (s == "\"\"" || s == "''") { cs->content = ""; cs->content_set = true; return true; }
    return true;
  }
  if (prop == "font") {
    /* shorthand: [style/weight/variant] size[/line-height] family */
    std::vector<std::string> toks;
    split_values(v, &toks);
    size_t size_idx = toks.size();
    for (size_t i = 0; i < toks.size(); i++) {
      std::string s = lc(toks[i]);
      bool has_unit = s.find("px") != std::string::npos || s.find("pt") != std::string::npos ||
                      s.find("rem") != std::string::npos || s.find("em") != std::string::npos ||
                      s.find('%') != std::string::npos;
      bool has_slash = s.find('/') != std::string::npos;
      /* numeric-only tokens ("500") are weights, never the size */
      if (has_unit || has_slash) {
        size_idx = i;
        break;
      }
    }
    for (size_t i = 0; i < size_idx && i < toks.size(); i++) {
      std::string s = lc(toks[i]);
      if (s == "bold" || s == "bolder" || s == "700" || s == "800" || s == "900")
        cs->font_weight = 700;
      else if (s == "500" || s == "600") cs->font_weight = atoi(s.c_str());
      else if (s == "light" || s == "lighter" || s == "300") cs->font_weight = 300;
      else if (s == "italic" || s == "oblique") cs->italic = true;
    }
    if (size_idx < toks.size()) {
      std::string size_tok = toks[size_idx];
      std::string lh;
      size_t slash = size_tok.find('/');
      if (slash != std::string::npos) {
        lh = size_tok.substr(slash + 1);
        size_tok = size_tok.substr(0, slash);
      }
      float sz = (float)atof(size_tok.c_str());
      if (sz > 0) {
        if (size_tok.find("pt") != std::string::npos) sz = sz * 96.f / 72.f;
        else if (size_tok.size() > 3 && size_tok.substr(size_tok.size() - 3) == "rem") sz *= 16;
        else if (size_tok.size() > 2 && size_tok.substr(size_tok.size() - 2) == "em") sz *= cs->font_size;
        cs->font_size = sz;
        rc.em = sz;
      }
      if (!lh.empty()) {
        float n = (float)atof(lh.c_str());
        if (n > 0) { cs->lh_normal = false; cs->line_height_mul = n; cs->line_height.unit = L_NORMAL; }
      }
      /* family = everything after the size */
      std::string fam;
      for (size_t i = size_idx + 1; i < toks.size(); i++) {
        if (!fam.empty()) fam += ",";
        fam += toks[i];
      }
      if (!fam.empty()) {
        std::vector<std::string> fams;
        std::string cur;
        bool in_q = false;
        char q = 0;
        for (char c : fam) {
          if (in_q) { if (c == q) in_q = false; else cur += c; continue; }
          if (c == '"' || c == '\'') { in_q = true; q = c; continue; }
          if (c == ',') { fams.push_back(trim(cur)); cur.clear(); continue; }
          cur += c;
        }
        fams.push_back(trim(cur));
        cs->font_family.clear();
        for (auto& f2 : fams) if (!f2.empty()) cs->font_family.push_back(f2);
      }
    }
    return true;
  }
  if (prop == "animation" || starts_with(prop, "animation-")) {
    if (prop == "animation-name") { cs->anim_name = trim(v); return true; }
    if (prop == "animation-duration") {
      std::string s = lc(trim(v));
      cs->anim_duration = (float)atof(s.c_str()) * (s.find("ms") != std::string::npos ? 0.001f : 1.f);
      return true;
    }
    if (prop == "animation-delay") {
      std::string s = lc(trim(v));
      cs->anim_delay = (float)atof(s.c_str()) * (s.find("ms") != std::string::npos ? 0.001f : 1.f);
      return true;
    }
    if (prop == "animation-iteration-count") {
      std::string s = lc(trim(v));
      if (s == "infinite") cs->anim_infinite = true;
      else cs->anim_iterations = atoi(s.c_str());
      return true;
    }
    if (prop == "animation-timing-function") { cs->anim_timing = lc(trim(v)); return true; }
    if (prop == "animation") {
      /* shorthand: name duration timing-function delay iteration-count ... */
      std::vector<std::string> toks;
      split_values(v, &toks);
      int times = 0;
      for (auto& tk : toks) {
        std::string s = lc(trim(tk));
        if (s == "infinite") { cs->anim_infinite = true; continue; }
        bool numeric = !s.empty() && (isdigit((unsigned char)s[0]) || s[0] == '.');
        if (numeric && (s.back() == 's' || s.find("ms") == s.size() - 2)) {
          float val = (float)atof(s.c_str()) * (s.find("ms") == s.size() - 2 ? 0.001f : 1.f);
          if (times == 0) cs->anim_duration = val;
          else if (times == 1) cs->anim_delay = val;
          times++;
          continue;
        }
        if (s == "linear" || s == "ease" || s == "ease-in" || s == "ease-out" ||
            s == "ease-in-out" || s == "step-start" || s == "step-end") {
          cs->anim_timing = s;
          continue;
        }
        if (numeric) { cs->anim_iterations = atoi(s.c_str()); continue; }
        if (cs->anim_name.empty()) cs->anim_name = tk;
      }
      return true;
    }
    return true; /* animation-direction / fill-mode / play-state: consumed */
  }
  if (prop == "list-style-type") { cs->list_style = lc(v) == "none" ? 0 : 1; return true; }
  if (prop == "list-style") { cs->list_style = lc(v).find("none") != std::string::npos ? 0 : 1; return true; }
  /* ignored-but-consumed properties (animations, transitions, cursor, will-change, etc.) */
  static const char* kIgnored[] = {
    "transition", "transition-property", "transition-duration", "transition-timing-function",
    "transition-delay", "cursor", "will-change", "pointer-events", "user-select", "touch-action",
    "outline", "outline-width", "outline-style", "outline-color", "outline-offset", "resize",
    "scroll-behavior", "scroll-snap-type", "scroll-margin", "scroll-padding", "visibility",
    "unicode-bidi", "direction", "quotes", "counter-reset", "counter-increment", "tab-size",
    "text-overflow", "word-break", "overflow-wrap", "word-wrap", "hyphens", "text-justify",
    "font-variant", "font-feature-settings", "font-kerning", "font-stretch", "font-display",
    "src", "unicode-range", "text-rendering", "-webkit-font-smoothing", "-moz-osx-font-smoothing",
    "mix-blend-mode", "isolation", "contain", "all", "grid-area", "grid-auto-flow", "grid-auto-rows",
    "grid-auto-columns", "grid-template-areas", "place-items", "place-content", "place-self",
    "column-count", "column-gap", "column-rule", "columns", "mask", "mask-image", "clip-path",
    "text-wrap", "text-indent", "text-shadow", "caret-color", "accent-color", "color-scheme",
    "forced-color-adjust", "appearance", "-webkit-appearance", "-webkit-box-orient",
    "-webkit-line-clamp", "-webkit-box-pack", "-webkit-align-items", "-webkit-flex-direction",
    "zoom", "vector-effect", nullptr
  };
  for (int i = 0; kIgnored[i]; i++)
    if (prop == kIgnored[i]) return true;
  /* vendor prefixed anything else: swallow */
  if (starts_with(prop, "-webkit-") || starts_with(prop, "-moz-") || starts_with(prop, "-ms-") ||
      starts_with(prop, "-o-"))
    return true;
  return false;
}

/* ============================ cascade ============================ */
struct UaSheet {
  StyleSheet sheet;
  UaSheet() {
    static const char* ua_css =
      "html{display:block}"
      "body{display:block;margin:8px;}"
      "head,meta,title,link,style,script,template,base,noscript{display:none}"
      "div,section,article,header,footer,nav,main,aside,figure,figcaption,form,fieldset,"
      "address,blockquote,pre,hr,dl,dt,dd,center{display:block}"
      "h1{display:block;font-size:2em;font-weight:700;margin:0.67em 0}"
      "h2{display:block;font-size:1.5em;font-weight:700;margin:0.83em 0}"
      "h3{display:block;font-size:1.17em;font-weight:700;margin:1em 0}"
      "h4{display:block;font-size:1em;font-weight:700;margin:1.33em 0}"
      "h5{display:block;font-size:0.83em;font-weight:700;margin:1.67em 0}"
      "h6{display:block;font-size:0.67em;font-weight:700;margin:2.33em 0}"
      "p{display:block;margin:1em 0}"
      "ul,ol{display:block;margin:1em 0;padding-left:40px}"
      "li{display:list-item}"
      "dd{margin-left:40px}"
      "b,strong{font-weight:700}"
      "i,em,cite,var{font-style:italic}"
      "u,ins{text-decoration:underline}"
      "s,strike,del{text-decoration:line-through}"
      "small{font-size:0.83em}"
      "big{font-size:1.17em}"
      "sub{font-size:0.83em}"
      "sup{font-size:0.83em}"
      "a{color:#0000EE;text-decoration:underline}"
      "img,svg,video,canvas,picture,iframe,object,embed{display:inline-block}"
      "input,button,select,textarea{display:inline-block}"
      "button{font-weight:400}"
      "table{display:block}"
      "tbody,thead,tfoot,tr,td,th,caption,col,colgroup,legend,label,option,optgroup{display:block}"
      "td,th{padding:1px}"
      "code,tt,kbd,samp,pre{font-family:monospace}"
      "pre{white-space:pre;margin:1em 0}"
      "blockquote{margin:1em 40px}"
      "fieldset{margin:0 2px;padding:0.35em 0.75em 0.625em}"
      "br{display:inline}";
    css_parse_stylesheet(ua_css, &sheet, 99999, 99999);
    for (auto& r : sheet.rules) r.origin = 0;
  }
};

static UaSheet& ua_sheet() {
  static UaSheet ua;
  return ua;
}

/* initial computed style */
static void init_computed(ComputedStyle* cs, const ComputedStyle* parent) {
  if (parent) {
    /* inherited props */
    cs->color = parent->color;
    cs->font_family = parent->font_family;
    cs->font_weight = parent->font_weight;
    cs->italic = parent->italic;
    cs->letter_spacing = parent->letter_spacing;
    cs->text_align = parent->text_align;
    cs->text_transform = parent->text_transform;
    cs->white_space = parent->white_space;
    cs->line_height_mul = parent->line_height_mul;
    cs->lh_normal = parent->lh_normal;
    cs->line_height = parent->line_height;
    cs->list_style = parent->list_style;
    cs->font_size = parent->font_size;
  } else {
    cs->color = Color{255, 0, 0, 0};
    cs->font_family = {"Inter"};
    cs->font_weight = 400;
    cs->font_size = 16;
    cs->lh_normal = true;
    cs->line_height_mul = 1.2f;
  }
}

struct MatchedRule {
  const Rule* rule;
  const Selector* sel;
  uint32_t sort_key; /* origin<<24 | spec<<8 | order */
};

static void collect_rules_for(const Node* el, const StyleSheet& sheet, std::vector<MatchedRule>* out) {
  for (const Rule& r : sheet.rules) {
    for (const Selector& sel : r.sels) {
      const std::string& pe = sel.parts.back().pseudo_element;
      if (!pe.empty()) continue; /* pseudo-element rules handled separately */
      if (selector_matches(el, sel)) {
        uint32_t key = (uint32_t)r.origin << 24 | (uint32_t)sel.specificity << 8 | (uint32_t)(r.order & 0xFFFF);
        out->push_back(MatchedRule{&r, &sel, key});
        break;
      }
    }
  }
}

static void collect_pseudo_rules(const Node* el, const char* pe_name, const StyleSheet& sheet,
                                 std::vector<MatchedRule>* out) {
  for (const Rule& r : sheet.rules) {
    for (const Selector& sel : r.sels) {
      if (sel.parts.back().pseudo_element != pe_name) continue;
      Selector sel_no_pe = sel;
      sel_no_pe.parts.back().pseudo_element = "";
      if (selector_matches(el, sel_no_pe)) {
        uint32_t key = (uint32_t)r.origin << 24 | (uint32_t)sel.specificity << 8 | (uint32_t)(r.order & 0xFFFF);
        out->push_back(MatchedRule{&r, &sel, key});
        break;
      }
    }
  }
}

static void sort_rules(std::vector<MatchedRule>* rules) {
  std::stable_sort(rules->begin(), rules->end(),
                   [](const MatchedRule& a, const MatchedRule& b) { return a.sort_key < b.sort_key; });
}

static void compute_style_for(Node* el, const ComputedStyle* parent, const CustomProps* parent_vars,
                              const StyleSheet& sheet, float rem, int vw, int vh);

void style_tree(Document* doc, int vw, int vh) {
  ua_sheet(); /* ensure built */
  ComputedStyle initial;
  init_computed(&initial, nullptr);
  compute_style_for(doc->html ? doc->html : doc->root, &initial, &doc->root_vars, doc->sheet, doc->root_font_size, vw, vh);
  if (doc->html && doc->html->cs) {
    doc->root_font_size = doc->html->cs->font_size;
    /* recompute html + subtree if font-size changed rem basis */
    compute_style_for(doc->html, &initial, &doc->root_vars, doc->sheet, doc->root_font_size, vw, vh);
  }
}

/* ---- animation evaluation ---- */
static float anim_time_s() {
  using clock = std::chrono::steady_clock;
  static clock::time_point base = clock::now();
  return std::chrono::duration<float>(clock::now() - base).count();
}

static float anim_ease(const std::string& tf, float t) {
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  if (tf == "ease-in-out") return t * t * (3 - 2 * t);
  if (tf == "ease-in") return t * t;
  if (tf == "ease-out") return 1 - (1 - t) * (1 - t);
  if (tf == "ease") return t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
  return t; /* linear, steps (approximated) */
}

/* apply the keyframe-animated values for `cs` at the current time */
static void evaluate_animation(ComputedStyle* cs, const StyleSheet& sheet, ResolveCtx& rc) {
  if (cs->anim_name.empty() || cs->anim_duration <= 0) return;
  const KeyframesRule* kr = nullptr;
  for (auto& k : sheet.keyframes)
    if (k.name == cs->anim_name) { kr = &k; break; }
  if (!kr || kr->steps.size() < 2) return;

  float t = anim_time_s() - cs->anim_delay;
  if (t < 0) t = 0;
  float prog;
  if (cs->anim_infinite) {
    prog = fmodf(t, cs->anim_duration) / cs->anim_duration;
  } else {
    prog = t / cs->anim_duration;
    if (prog > 1) prog = 1;
  }
  float pct = prog * 100.f;

  /* gather per-property (pct, value) lists */
  std::vector<std::string> props;
  for (auto& st : kr->steps)
    for (auto& d : st.second)
      if (std::find(props.begin(), props.end(), d.prop) == props.end()) props.push_back(d.prop);

  for (auto& prop : props) {
    /* bracketing steps for this property */
    float lo_p = -1, hi_p = -1;
    std::string lo_v, hi_v;
    for (auto& st : kr->steps) {
      for (auto& d : st.second) {
        if (d.prop != prop) continue;
        if (st.first <= pct && st.first >= lo_p) { lo_p = st.first; lo_v = d.value; }
        if (st.first > pct && (hi_p < 0 || st.first < hi_p)) { hi_p = st.first; hi_v = d.value; }
      }
    }
    if (lo_p < 0) {
      /* only a `to` step: CSS implies from = the current (identity) value */
      if (prop == "transform" && hi_p >= 0) { lo_p = 0; lo_v = "translateX(0%)"; }
      else continue;
    }
    std::string val;
    if (hi_p < 0) {
      val = lo_v; /* past the last keyframe */
    } else {
      /* interpolate when both sides resolve to the same length unit */
      Length L1, L2;
      parse_length(substitute_vars(lo_v, rc), &L1, rc);
      parse_length(substitute_vars(hi_v, rc), &L2, rc);
      float f = anim_ease(cs->anim_timing, (pct - lo_p) / (hi_p - lo_p));
      if (L1.unit == L2.unit && (L1.unit == L_PX || L1.unit == L_PCT)) {
        float v = L1.val + (L2.val - L1.val) * f;
        char buf[64];
        snprintf(buf, sizeof(buf), L1.unit == L_PX ? "%.2fpx" : "%.2f%%", v);
        val = buf;
      } else if (prop == "transform") {
        /* interpolate translateX(<n>%) between keyframes */
        auto txpct = [](const std::string& v) -> float {
          size_t p = v.find("translateX");
          if (p == std::string::npos) return 0;
          return (float)atof(v.c_str() + p + 10);
        };
        cs->anim_tx = true;
        cs->anim_tx_pct = txpct(lo_v) + (txpct(hi_v) - txpct(lo_v)) * f;
        return; /* transform handled, no declaration re-apply */
      } else {
        val = f >= 1 ? hi_v : lo_v; /* non-interpolable: step */
      }
    }
    apply_declaration(cs, prop, val, rc, nullptr);
  }
}

static void compute_style_for(Node* el, const ComputedStyle* parent, const CustomProps* parent_vars,
                              const StyleSheet& sheet, float rem, int vw, int vh) {
  /* clear old */
  if (el->cs) { delete el->cs; el->cs = nullptr; }
  ComputedStyle* cs = new ComputedStyle();
  init_computed(cs, parent);
  el->cs = cs;

  /* own custom props */
  CustomProps own_vars;
  if (parent_vars) own_vars = *parent_vars;

  /* inline style attr: highest priority — parse as declarations applied last */
  std::vector<Declaration> inline_decls;
  if (el->type == N_ELEMENT) {
    const std::string* style_attr = el->attr("style");
    if (style_attr && !style_attr->empty()) parse_declarations(*style_attr, &inline_decls);
  }

  /* pass 0: collect declarations (author rules + inline) */
  std::vector<MatchedRule> matched;
  collect_rules_for(el, sheet, &matched);
  sort_rules(&matched);

  struct Pending { const std::string* prop; const std::string* val; bool important; };
  std::vector<Pending> pending;
  for (auto& m : matched)
    for (auto& d : m.rule->decls) pending.push_back({&d.prop, &d.value, d.important});
  for (auto& d : inline_decls) pending.push_back({&d.prop, &d.value, d.important});
  /* important flags: re-sort so importants win */
  std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
    return a.important < b.important;
  });

  /* pass 1: custom properties + font-size (needs var resolution context that includes
   * only inherited vars + own custom prop declarations) */
  ResolveCtx rc;
  rc.vars = &own_vars;
  rc.rem = rem;
  rc.em = cs->font_size;
  rc.vw = (float)vw;
  rc.vh = (float)vh;
  rc.current_color = cs->color;
  for (auto& p : pending) {
    if (starts_with(*p.prop, "--")) apply_declaration(cs, *p.prop, *p.val, rc, &own_vars);
  }
  rc.vars = &own_vars;
  /* font-size */
  for (auto& p : pending) {
    if (*p.prop == "font-size") {
      ResolveCtx rc2 = rc;
      rc2.em = parent ? parent->font_size : 16;
      apply_declaration(cs, *p.prop, *p.val, rc2, &own_vars);
    }
  }
  rc.em = cs->font_size;
  /* color next (for currentColor) */
  for (auto& p : pending)
    if (*p.prop == "color") apply_declaration(cs, *p.prop, *p.val, rc, &own_vars);
  rc.current_color = cs->color;
  /* pass 2: everything else */
  for (auto& p : pending) {
    if (*p.prop == "font-size" || *p.prop == "color" || starts_with(*p.prop, "--")) continue;
    apply_declaration(cs, *p.prop, *p.val, rc, &own_vars);
  }

  /* UA rules (lowest priority — apply first conceptually; emulate by filling gaps) */
  {
    std::vector<MatchedRule> uam;
    collect_rules_for(el, ua_sheet().sheet, &uam);
    sort_rules(&uam);
    for (auto& m : uam) {
      for (auto& d : m.rule->decls) {
        /* only apply if author didn't set the property */
        bool overridden = false;
        for (auto& p : pending)
          if (*p.prop == d.prop) { overridden = true; break; }
        if (!overridden) apply_declaration(cs, d.prop, d.value, rc, &own_vars);
      }
    }
  }

  /* animations (evaluated after all declarations are in) */
  evaluate_animation(cs, sheet, rc);

  /* elements hidden by UA (head/script/...) handled via UA display:none */
  el->needs_style = false;

  /* pseudo elements */
  for (int pe = 0; pe < 2; pe++) {
    const char* pe_name = pe == 0 ? "before" : "after";
    std::vector<MatchedRule> pm;
    collect_pseudo_rules(el, pe_name, sheet, &pm);
    if (pm.empty()) continue;
    ComputedStyle* pcs = new ComputedStyle();
    init_computed(pcs, cs);
    ResolveCtx rc2;
    rc2.vars = &own_vars;
    rc2.rem = rem;
    rc2.em = cs->font_size;
    rc2.current_color = cs->color;
    bool content_set = false;
    std::vector<Pending> ppending;
    for (auto& m : pm)
      for (auto& d : m.rule->decls) ppending.push_back({&d.prop, &d.value, d.important});
    std::stable_sort(ppending.begin(), ppending.end(), [](const Pending& a, const Pending& b) {
      return a.important < b.important;
    });
    for (auto& p : ppending) {
      if (*p.prop == "content") {
        apply_declaration(pcs, *p.prop, *p.val, rc2, nullptr);
        content_set = pcs->content_set;
      }
    }
    if (!content_set) { delete pcs; continue; }
    for (auto& p : ppending) {
      if (*p.prop == "content") continue;
      apply_declaration(pcs, *p.prop, *p.val, rc2, nullptr);
    }
    evaluate_animation(pcs, sheet, rc2);
    pcs->pseudo_before = pe == 0;
    pcs->pseudo_after = pe == 1;
    if (el->pseudo[pe]) delete el->pseudo[pe];
    el->pseudo[pe] = pcs;
  }

  /* recurse children */
  for (Node* c : el->children) {
    if (c->type == N_ELEMENT) compute_style_for(c, cs, &own_vars, sheet, rem, vw, vh);
  }
}


/* simplified entry point used by the scrollbar rule parser (square_css.cpp) */
bool parse_css_color(const std::string& raw, Color* out) {
  ResolveCtx rc;
  rc.current_color = Color{255, 0, 0, 0};
  return parse_css_color(raw, rc, out);
}

/* animation-only pass: walk the tree and re-apply keyframe values */
void style_reanimate(Document* doc, int vw, int vh) {
  if (!doc || !doc->root || doc->sheet.keyframes.empty()) return;
  std::function<void(Node*)> walk = [&](Node* n) {
    if (n->cs) {
      ResolveCtx rc;
      rc.vars = &doc->root_vars;
      rc.em = n->cs->font_size;
      rc.rem = doc->root_font_size;
      rc.vw = (float)vw;
      rc.vh = (float)vh;
      rc.current_color = n->cs->color;
      evaluate_animation(n->cs, doc->sheet, rc);
    }
    for (int pe = 0; pe < 2; pe++) {
      if (n->pseudo[pe]) {
        ResolveCtx rc;
        rc.vars = &doc->root_vars;
        rc.em = n->pseudo[pe]->font_size;
        rc.rem = doc->root_font_size;
        rc.vw = (float)vw;
        rc.vh = (float)vh;
        rc.current_color = n->pseudo[pe]->color;
        evaluate_animation(n->pseudo[pe], doc->sheet, rc);
      }
    }
    for (Node* c : n->children) walk(c);
  };
  walk(doc->root);
}
} /* namespace sqrl */
