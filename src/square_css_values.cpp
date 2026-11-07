/* =====================================================================
 *  Square Browser Engine — square_style.cpp
 *  CSS value parsing (lengths, calc, colors, gradients), declaration
 *  application (apply_declaration) and the cascade (style_tree).
 * ===================================================================== */
#include "square_content_internal.h"
#include "square_style_internal.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>

namespace sqrl {


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

std::string substitute_vars(const std::string& val, const ResolveCtx& rc, int depth) {
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
Length parse_length_component(const std::string& t) {
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
bool parse_length(const std::string& raw, Length* out, const ResolveCtx& rc) {
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
  if ((v.rfind("min(", 0) == 0 || v.rfind("max(", 0) == 0) && v.back() == ')') {
    std::vector<std::string> parts;
    split_top_commas(v.substr(4, v.size() - 5), &parts);
    if (parts.size() >= 2) {
      Length a, b;
      parse_length(parts[0], &a, rc);
      parse_length(parts[1], &b, rc);
      *out = Length();
      length_to_terms(a, out->terms_min, &out->nmin);   /* arg1 in terms_min */
      length_to_terms(b, out->terms_max, &out->nmax);   /* arg2 in terms_max */
      out->unit = L_MM;
      out->mm_op = v.rfind("min(", 0) == 0 ? 1 : 2;
      out->has_clamp = 0;
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
bool parse_css_color(const std::string& raw, const ResolveCtx& rc, Color* out) {
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
bool parse_gradient(const std::string& raw, BgLayer* layer, const ResolveCtx& rc) {
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
float resolve_length(const Length& L, float pct_basis, float em, float rem, float vw, float vh) {
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
  if (L.unit == L_CALC || L.unit == L_CLAMP || L.unit == L_MM) {
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
    if (L.unit == L_MM && L.mm_op) {
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
      float r1 = sum_terms(L.terms_min, L.nmin);
      float r2 = L.nmax ? sum_terms(L.terms_max, L.nmax) : r1;
      return L.mm_op == 1 ? std::min(r1, r2) : std::max(r1, r2);
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
void split_values(const std::string& v, std::vector<std::string>* out, const char* seps) {
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

void parse_side_values(const std::string& v, const ResolveCtx& rc, Length out[4]) {
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

Display parse_display(const std::string& v) {
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
  if (s == "inline-table") return D_INLINE_TABLE;
  if (s == "table-row") return D_TABLE_ROW;
  if (s == "table-cell") return D_TABLE_CELL;
  if (s == "table-row-group" || s == "table-header-group" || s == "table-footer-group")
    return D_TABLE_ROW_GROUP;
  if (s == "list-item") return D_LIST_ITEM;
  if (s == "contents" || s == "flow-root" || s == "table-caption")
    return s == "contents" ? D_NONE : D_BLOCK; /* flow-root -> block approximation */
  return D_INLINE;
}

void parse_background(const std::string& raw, ComputedStyle* cs, const ResolveCtx& rc) {
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

void parse_grid_tracks(const std::string& v, std::vector<Track>* out,
                              std::vector<Track>* auto_out,
                              uint8_t* auto_mode) {
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

void parse_box_shadows(const std::string& v, ComputedStyle* cs, const ResolveCtx& rc) {
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

void parse_transform(const std::string& v, ComputedStyle* cs) {
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
} /* namespace sqrl */
