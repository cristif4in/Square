/* =====================================================================
 *  Square Browser Engine — square_css.cpp
 *  CSS tokenizer (cursor in square_content_internal.h), selector parsing,
 *  media queries, stylesheet parsing, scrollbar styling.
 * ===================================================================== */
#include "square_css_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sqrl {
/* selector parsing ----------------------------------------------------- */
static uint16_t sel_specificity(const std::vector<SelPart>& parts) {
  int ids = 0, classes = 0, tags = 0;
  for (auto& p : parts) {
    ids += (int)p.ids.size();
    classes += (int)p.classes.size() + (int)p.attrs.size() + (int)p.pseudo_classes.size();
    if (p.pseudo_element.size()) classes++;
    if (!p.tag.empty() && p.tag != "*") tags++;
  }
  return (uint16_t)(std::min(ids, 15) << 8 | std::min(classes, 15) << 4 | std::min(tags, 15));
}

static void parse_compound(const std::string& comp, SelPart* part) {
  size_t i = 0, n = comp.size();
  /* tag or * */
  size_t j = i;
  while (j < n && (isalnum((unsigned char)comp[j]) || comp[j] == '-' || comp[j] == '_')) j++;
  if (j > i) {
    part->tag = lc(comp.substr(i, j - i));
    i = j;
  }
  while (i < n) {
    char c = comp[i];
    if (c == '.') {
      i++;
      j = i;
      while (j < n) {
        if (comp[j] == '\\' && j + 1 < n) { j += 2; continue; } /* escaped char: keep verbatim */
        if (isalnum((unsigned char)comp[j]) || comp[j] == '-' || comp[j] == '_' || comp[j] == '\\' ||
            (unsigned char)comp[j] > 127)
          j++;
        else
          break;
      }
      std::string cls = comp.substr(i, j - i);
      /* unescape \: etc */
      std::string un;
      for (size_t k = 0; k < cls.size(); k++) {
        if (cls[k] == '\\' && k + 1 < cls.size()) { un += cls[k + 1]; k++; }
        else un += cls[k];
      }
      part->classes.push_back(un);
      i = j;
    } else if (c == '#') {
      i++;
      j = i;
      while (j < n) {
        if (comp[j] == '\\' && j + 1 < n) { j += 2; continue; }
        if (isalnum((unsigned char)comp[j]) || comp[j] == '-' || comp[j] == '_' || comp[j] == '\\' ||
            (unsigned char)comp[j] > 127)
          j++;
        else
          break;
      }
      std::string idraw = comp.substr(i, j - i);
      std::string un;
      for (size_t k = 0; k < idraw.size(); k++) {
        if (idraw[k] == '\\' && k + 1 < idraw.size()) { un += idraw[k + 1]; k++; }
        else un += idraw[k];
      }
      part->ids.push_back(un);
      i = j;
    } else if (c == '[') {
      size_t e = comp.find(']', i);
      if (e == std::string::npos) break;
      std::string inner = comp.substr(i + 1, e - i - 1);
      i = e + 1;
      AttrSel a;
      size_t eq = inner.find('=');
      if (eq == std::string::npos) {
        a.name = lc(trim(inner));
      } else {
        a.name = lc(trim(inner.substr(0, eq)));
        char op = inner[eq - 1];
        size_t vs = eq + 1;
        if (op == '~' || op == '|' || op == '^' || op == '$' || op == '*') {
          a.op = op;
          vs = eq + 1;
        } else a.op = '=';
        std::string val = trim(inner.substr(vs));
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'')) val = val.substr(1, val.size() - 2);
        a.val = val;
      }
      part->attrs.push_back(a);
    } else if (c == ':') {
      i++;
      if (i < n && comp[i] == ':') { i++; /* pseudo-element */ }
      j = i;
      while (j < n && (isalnum((unsigned char)comp[j]) || comp[j] == '-' || comp[j] == '_')) j++;
      std::string name = lc(comp.substr(i, j - i));
      i = j;
      std::string arg;
      if (i < n && comp[i] == '(') {
        int depth = 0;
        size_t st = ++i;
        while (i < n) {
          if (comp[i] == '(') depth++;
          if (comp[i] == ')') {
            if (depth == 0) break;
            depth--;
          }
          i++;
        }
        arg = comp.substr(st, i - st);
        i++;
      }
      if (name == "before" || name == "after" || name == "first-line" || name == "first-letter" ||
          name == "placeholder" || name == "marker" || name == "selection" ||
          starts_with(name, "webkit-scrollbar")) {
        if (name == "before" || name == "after" || starts_with(name, "webkit-scrollbar"))
          part->pseudo_element = name;
      } else {
        if (!arg.empty()) name += "(" + arg + ")";
        part->pseudo_classes.push_back(name);
      }
    } else i++;
  }
}

/* split a selector list on top-level commas (respecting parens and backslash
 * escapes, e.g. Tailwind's `minmax(0\,400px)`), used by :not()/:is()/:where() */
void split_selector_list(const std::string& in, std::vector<std::string>* out) {
  int pd = 0;
  size_t ss = 0;
  for (size_t e = 0; e <= in.size(); e++) {
    if (e < in.size()) {
      char c = in[e];
      if (c == '\\' && e + 1 < in.size()) { e++; continue; }
      if (c == '(') pd++;
      if (c == ')') pd--;
      if (c != ',' || pd != 0) continue;
    }
    std::string one = trim(in.substr(ss, e - ss));
    if (!one.empty()) out->push_back(one);
    ss = e + 1;
  }
}

bool parse_selector(const std::string& in, Selector* out) {
  std::vector<std::string> compounds;
  std::vector<uint8_t> combs;
  size_t i = 0, n = in.size();
  std::string cur;
  char pending_comb = 0;
  auto flush = [&]() {
    if (!trim(cur).empty()) {
      compounds.push_back(trim(cur));
      cur.clear();
      combs.push_back((uint8_t)pending_comb);
      pending_comb = 0;
    }
  };
  int paren = 0;
  while (i < n) {
    char c = in[i];
    if (c == '\\' && i + 1 < n) {
      /* escaped char: keep both so unescaping happens in parse_compound */
      cur += c;
      cur += in[i + 1];
      i += 2;
      continue;
    }
    /* pseudo-class arguments like :nth-child(2n+1) / :not(.a, .b) /
     * :is(div > a) are verbatim — no combinators or commas inside parens */
    if (c == '(') { paren++; cur += c; i++; continue; }
    if (c == ')') { if (paren > 0) paren--; cur += c; i++; continue; }
    if (paren > 0) { cur += c; i++; continue; }
    if (c == ' ' || c == '\t' || c == '\n') {
      size_t j = i;
      while (j < n && isspace((unsigned char)in[j])) j++;
      if (j < n && in[j] != '>' && in[j] != '+' && in[j] != '~' && in[j] != ',' && !cur.empty()) {
        /* descendant combinator: the compound before the space ends here */
        flush();
        pending_comb = 0;
      }
      i = j;
      continue;
    }
    if (c == '>') { flush(); pending_comb = 1; i++; continue; }
    if (c == '+') { flush(); pending_comb = 2; i++; continue; }
    if (c == '~') { flush(); pending_comb = 3; i++; continue; }
    if (c == ',') { flush(); i++; continue; }
    cur += c;
    i++;
  }
  flush();
  if (compounds.empty()) return false;
  out->parts.clear();
  for (size_t k = 0; k < compounds.size(); k++) {
    SelPart part;
    part.combinator = k < combs.size() ? combs[k] : 0;
    parse_compound(compounds[k], &part);
    out->parts.push_back(part);
  }
  out->specificity = sel_specificity(out->parts);
  out->valid = true;
  return true;
}

/* media query parse ---------------------------------------------------- */
static bool parse_media_query(const std::string& q, MediaQuery* mq) {
  std::string s = lc(trim(q));
  if (starts_with(s, "only ")) s = s.substr(5);
  if (starts_with(s, "not ")) {
    s = s.substr(4);
    mq->negative = true;
  }
  /* drop "screen and" / "all and" prefixes */
  size_t pos = s.find(" and ");
  std::string first = pos == std::string::npos ? s : s.substr(0, pos);
  if (first == "screen" || first == "all" || first == "print" || first.find("screen") != std::string::npos) {
    if (pos == std::string::npos) return true; /* bare media type: match */
  }
  /* parse feature list */
  size_t i = pos;
  while (i != std::string::npos && i < s.size()) {
    size_t a = s.find('(', i);
    if (a == std::string::npos) break;
    size_t b = s.find(')', a);
    if (b == std::string::npos) break;
    std::string feat = trim(s.substr(a + 1, b - a - 1));
    size_t colon = feat.find(':');
    if (colon != std::string::npos) {
      std::string name = lc(trim(feat.substr(0, colon)));
      float val = (float)atof(trim(feat.substr(colon + 1)).c_str());
      if (name == "min-width") mq->minw = val;
      else if (name == "max-width") mq->maxw = val;
      else if (name == "min-height") mq->minh = val;
      else if (name == "max-height") mq->maxh = val;
      /* prefers-color-scheme etc: ignore (treated as matching) */
    }
    i = b;
    size_t nxt = s.find(" and ", b);
    i = nxt;
  }
  return true;
}

/* declaration block parse ---------------------------------------------- */
void parse_declarations(const std::string& body, std::vector<Declaration>* out) {
  size_t i = 0, n = body.size();
  while (i < n) {
    /* property */
    size_t colon = std::string::npos;
    size_t j = i;
    int paren = 0;
    while (j < n) {
      char c = body[j];
      if (c == '(') paren++;
      if (c == ')') paren--;
      if (c == ';' && paren == 0) break;
      if (c == ':' && paren == 0 && colon == std::string::npos) colon = j;
      if (c == '"' || c == '\'') {
        char q = c;
        j++;
        while (j < n && body[j] != q) {
          if (body[j] == '\\') j++;
          j++;
        }
      }
      j++;
    }
    if (colon == std::string::npos) { i = j + 1; continue; }
    std::string prop = lc(trim(body.substr(i, colon - i)));
    std::string val = trim(body.substr(colon + 1, j - colon - 1));
    bool important = false;
    if (val.size() >= 10) {
      std::string tail = val.substr(val.size() - 10);
      for (auto& c : tail) c = (char)tolower((unsigned char)c);
      if (tail == "!important") {
        important = true;
        val = trim(val.substr(0, val.size() - 10));
      }
    }
    if (!prop.empty() && !val.empty()) out->push_back(Declaration{prop, val, important});
    i = j + 1;
  }
}

static void apply_scrollbar_rule(const std::string& pe, const std::vector<Declaration>& decls,
                                 const std::vector<std::string>& pseudo_classes,
                                 ScrollbarStyle* sb);

void css_parse_stylesheet(const std::string& css, StyleSheet* out, int vw, int vh) {
  CssCursor c(css);
  int order = (int)out->rules.size();
  while (!c.eof()) {
    char ch = c.peek();
    if (ch == '@') {
      /* at-rule */
      size_t at = c.i;
      size_t name_end = c.i + 1;
      while (name_end < css.size() && (isalpha((unsigned char)css[name_end]) || css[name_end] == '-')) name_end++;
      std::string name = lc(css.substr(c.i + 1, name_end - c.i - 1));
      c.i = name_end;
      if (name == "media") {
        c.ws();
        /* read prelude to '{' */
        std::string prelude = c.read_until_chars("{");
        if (c.peek() != '{') { c.skip_until_char(';'); continue; }
        c.i++; /* consume { */
        /* parse prelude media, then read balanced block as sub-stylesheet */
        size_t start = c.i;
        int depth = 1;
        while (c.i < css.size() && depth > 0) {
          if (css[c.i] == '{') depth++;
          else if (css[c.i] == '}') depth--;
          if (depth > 0) c.i++;
        }
        std::string inner = css.substr(start, c.i - start);
        if (c.i < css.size()) c.i++; /* consume } */
        MediaQuery mq;
        if (parse_media_query(prelude, &mq)) {
          bool ok = mq.eval(vw, vh);
          if (ok) {
            size_t before = out->rules.size();
            css_parse_stylesheet(inner, out, vw, vh);
            /* stamp media on new rules? they matched; nothing needed */
          }
        }
      } else if (name == "supports" || name == "layer" || name == "container") {
        c.ws();
        std::string prelude = c.read_until_chars("{");
        if (c.peek() == '{') {
          c.i++;
          size_t start = c.i;
          int depth = 1;
          while (c.i < css.size() && depth > 0) {
            if (css[c.i] == '{') depth++;
            else if (css[c.i] == '}') depth--;
            if (depth > 0) c.i++;
          }
          std::string inner = css.substr(start, c.i - start);
          if (c.i < css.size()) c.i++;
          css_parse_stylesheet(inner, out, vw, vh); /* treat @supports as matching */
        }
      } else if (name == "font-face") {
        c.ws();
        if (c.peek() != '{') { c.skip_until_char(';'); continue; }
        c.i++;
        size_t start = c.i;
        int depth = 1;
        while (c.i < css.size() && depth > 0) {
          if (css[c.i] == '{') depth++;
          else if (css[c.i] == '}') depth--;
          if (depth > 0) c.i++;
        }
        std::string body = css.substr(start, c.i - start);
        if (c.i < css.size()) c.i++;
        FontFaceRule ff;
        std::vector<Declaration> decls;
        parse_declarations(body, &decls);
        for (auto& dcl : decls) {
          if (dcl.prop == "font-family") {
            std::string v = dcl.value;
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
            ff.family = v;
          } else if (dcl.prop == "font-weight") {
            std::string v = dcl.value;
            if (v == "bold") ff.weight = 700;
            else if (v == "normal") ff.weight = 400;
            else ff.weight = atoi(v.c_str());
          } else if (dcl.prop == "font-style") {
            ff.italic = dcl.value.find("italic") != std::string::npos;
          } else if (dcl.prop == "src") {
            /* url(...) format(...) list */
            size_t p = 0;
            while (p < dcl.value.size()) {
              size_t u = dcl.value.find("url(", p);
              if (u == std::string::npos) break;
              CssCursor tc(dcl.value);
              tc.i = u;
              FontFaceSrc src;
              src.url = tc.read_url();
              size_t fe = dcl.value.find("format(", u);
              if (fe != std::string::npos && fe < dcl.value.find(')', u) + 3) {
                size_t fe2 = dcl.value.find(')', fe);
                std::string fmt = dcl.value.substr(fe + 8, fe2 - fe - 8);
                for (auto& q : fmt) if (q == '"' || q == '\'') q = 0;
                src.format = fmt;
              }
              ff.srcs.push_back(src);
              p = u + 4;
              while (p < dcl.value.size() && dcl.value[p] != ',' ) p++;
              p++;
            }
          }
        }
        if (!ff.family.empty()) out->fontfaces.push_back(ff);
      } else if (name == "keyframes" || starts_with(name, "-webkit-keyframes") ||
                 name == "import" || name == "charset" || name == "property" || name == "scope") {
        if (name == "keyframes" || starts_with(name, "-webkit-keyframes")) {
          /* @keyframes name { 0% { .. } 50% { .. } to { .. } } */
          c.ws();
          std::string prelude = c.read_until_chars("{");
          if (c.peek() != '{') { c.skip_until_char('}'); if (c.peek() == '}') c.i++; continue; }
          c.i++; /* consume { */
          KeyframesRule kr;
          {
            std::string nm = trim(prelude);
            size_t sp = nm.rfind(' ');
            kr.name = sp == std::string::npos ? nm : nm.substr(sp + 1);
          }
          while (!c.eof()) {
            c.ws();
            if (c.peek() == '}') { c.i++; break; }
            std::string stepsel = c.read_until_chars("{");
            if (c.peek() != '{') break;
            c.i++;
            size_t start = c.i;
            int depth = 1;
            while (c.i < css.size() && depth > 0) {
              if (css[c.i] == '{') depth++;
              else if (css[c.i] == '}') depth--;
              if (depth > 0) c.i++;
            }
            std::string body = css.substr(start, c.i - start);
            if (c.i < css.size()) c.i++; /* consume } */
            std::string s2 = lc(trim(stepsel));
            float pct;
            if (s2 == "from") pct = 0;
            else if (s2 == "to") pct = 100;
            else pct = (float)atof(s2.c_str()); /* "50%" -> 50 */
            std::vector<Declaration> decls;
            parse_declarations(body, &decls);
            kr.steps.push_back({pct, decls});
          }
          out->keyframes.push_back(kr);
          out->has_animations = true;
          continue;
        }
        if (name == "import") {
          c.ws();
          std::string prelude = c.read_until_chars(";");
          size_t u = prelude.find("url(");
          std::string url;
          if (u != std::string::npos) {
            CssCursor tc(prelude);
            tc.i = u;
            url = tc.read_url();
          } else {
            size_t q1 = prelude.find('"');
            if (q1 != std::string::npos) {
              size_t q2 = prelude.find('"', q1 + 1);
              url = prelude.substr(q1 + 1, q2 - q1 - 1);
            }
          }
          if (!url.empty()) out->imports.push_back(url);
          if (c.peek() == ';') c.i++;
        } else if (name == "charset") {
          c.skip_until_char(';');
          if (c.peek() == ';') c.i++;
        } else {
          /* skip entire @keyframes block */
          c.ws();
          std::string prelude = c.read_until_chars("{");
          if (c.peek() == '{') {
            c.skip_block();
          } else c.skip_until_char(';');
          if (c.peek() == ';') c.i++;
        }
      } else {
        /* unknown at-rule: skip */
        c.ws();
        std::string prelude = c.read_until_chars("{;");
        if (c.peek() == '{') c.skip_block();
        else if (c.peek() == ';') c.i++;
      }
      continue;
    }
    if (ch == '}') { c.i++; continue; }
    /* normal rule */
    std::string sel_text = c.read_until_chars("{");
    if (c.peek() != '{') { c.skip_until_char(';'); continue; }
    c.i++; /* { */
    size_t start = c.i;
    int depth = 1;
    while (c.i < css.size() && depth > 0) {
      if (css[c.i] == '{') depth++;
      else if (css[c.i] == '}') depth--;
      if (depth > 0) c.i++;
    }
    std::string body = css.substr(start, c.i - start);
    if (c.i < css.size()) c.i++;
    if (trim(sel_text).empty()) continue;
    Rule rule;
    rule.origin = 1;
    rule.order = order++;
    size_t ss = 0;
    while (ss < sel_text.size()) {
      /* split on top-level commas, respecting backslash escapes (\, in
       * Tailwind arbitrary classes like [1fr_minmax(0\,400px)]) */
      size_t e = ss;
      while (e < sel_text.size()) {
        if (sel_text[e] == '\\' && e + 1 < sel_text.size()) { e += 2; continue; }
        if (sel_text[e] == ',') break;
        e++;
      }
      if (e > sel_text.size()) e = sel_text.size();
      std::string one = trim(sel_text.substr(ss, e - ss));
      ss = e + 1;
      if (one.empty()) continue;
      Selector sel;
      if (parse_selector(one, &sel)) rule.sels.push_back(sel);
    }
    if (rule.sels.empty()) continue;
    parse_declarations(body, &rule.decls);
    /* ::-webkit-scrollbar* rules configure the page scrollbar */
    const std::string& pe = rule.sels[0].parts.back().pseudo_element;
    if (starts_with(pe, "webkit-scrollbar")) {
      apply_scrollbar_rule(pe, rule.decls,
                           rule.sels[0].parts.back().pseudo_classes, &out->sbstyle);
      continue;
    }
    /* firefox-style scrollbar-width / scrollbar-color on any selector */
    for (auto& dcl : rule.decls) {
      if (dcl.prop == "scrollbar-width") {
        std::string v = lc(trim(dcl.value));
        if (v == "thin") { out->sbstyle.width = 8; out->sbstyle.radius = 0; }
        else if (v == "none") out->sbstyle.width = 0;
        else if (v == "auto") { out->sbstyle.width = 14; }
      } else if (dcl.prop == "scrollbar-color") {
        /* split "thumb track" on whitespace (top-level) */
        std::vector<std::string> cols;
        std::string cur;
        for (char c : dcl.value) {
          if (isspace((unsigned char)c)) {
            if (!trim(cur).empty()) { cols.push_back(trim(cur)); cur.clear(); }
          } else cur += c;
        }
        if (!trim(cur).empty()) cols.push_back(trim(cur));
        Color c1, c2;
        if (cols.size() >= 1 && parse_css_color(cols[0], &c1)) out->sbstyle.thumb = c1;
        if (cols.size() >= 2 && parse_css_color(cols[1], &c2)) out->sbstyle.track = c2;
      }
    }
    static bool dbg_all = getenv("SQRL_DBG_RULES_ALL") != nullptr;
    if ((getenv("SQRL_DBG_RULES") && sel_text.find("grid-cols") != std::string::npos) ||
        (dbg_all && sel_text.find(' ') != std::string::npos)) {
      fprintf(stderr, "[rule] '%s' sels=%zu\n", trim(sel_text).c_str(), rule.sels.size());
      for (auto& s2 : rule.sels) {
        fprintf(stderr, "[rule]   parts=%zu:", s2.parts.size());
        for (auto& p : s2.parts)
          fprintf(stderr, " [tag='%s' cls=%zu comb=%d]", p.tag.c_str(), p.classes.size(), (int)p.combinator);
        fprintf(stderr, "\n");
      }
    }
    if (!rule.decls.empty()) out->rules.push_back(rule);
  }
}

/* ============================ scrollbar styling ============================ */
/* apply one ::-webkit-scrollbar* rule to the document's scrollbar style */
static void apply_scrollbar_rule(const std::string& pe, const std::vector<Declaration>& decls,
                                 const std::vector<std::string>& pseudo_classes,
                                 ScrollbarStyle* sb) {
  bool hover = false;
  for (auto& pc : pseudo_classes) if (pc == "hover") hover = true;
  for (auto& dcl : decls) {
    std::string v = trim(dcl.value);
    if (pe == "webkit-scrollbar") {
      if (dcl.prop == "width") {
        float w = (float)atof(v.c_str());
        if (!v.empty() && v.back() == 'x' && w > 0) sb->width = w;
        else if (v == "none") sb->width = 0;
      }
    } else if (pe == "webkit-scrollbar-track") {
      Color c;
      if ((dcl.prop == "background" || dcl.prop == "background-color") && parse_css_color(v, &c))
        sb->track = c;
    } else if (pe == "webkit-scrollbar-thumb") {
      Color c;
      if ((dcl.prop == "background" || dcl.prop == "background-color") && parse_css_color(v, &c)) {
        if (hover) sb->thumb_hover = c;
        else sb->thumb = c;
      } else if (dcl.prop == "border-radius") {
        float r = (float)atof(v.c_str());
        if (hover) { /* hover radius not tracked separately */ }
        else sb->radius = r;
      } else if (dcl.prop == "border") {
        /* "2px solid #1a1a1a" */
        std::vector<std::string> toks;
        std::string cur;
        for (char c : v) {
          if (isspace((unsigned char)c)) { if (!trim(cur).empty()) { toks.push_back(trim(cur)); cur.clear(); } }
          else cur += c;
        }
        if (!trim(cur).empty()) toks.push_back(trim(cur));
        Color bc;
        for (auto& tk : toks) {
          float bw = (float)atof(tk.c_str());
          if (!tk.empty() && (isdigit((unsigned char)tk[0]) || tk[0] == '.') && bw > 0 && sb->thumb_border == 0)
            sb->thumb_border = bw;
          else if (parse_css_color(tk, &bc)) sb->thumb_border_color = bc;
        }
      }
    }
  }
}
} /* namespace sqrl */
