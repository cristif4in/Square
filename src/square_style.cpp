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

static bool g_any_transitions = false;

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
        /* longhand position/size/repeat may already have created a pending
         * layer: fill it instead of pushing a fresh one */
        if (!cs->bgs.empty() && cs->bgs.back().kind == BG_NONE && cs->bgs.back().url.empty())
          cs->bgs.back() = layer;
        else
          cs->bgs.push_back(layer);
      } else if (li.find("gradient(") != std::string::npos) {
        if (parse_gradient(img, &layer, rc)) {
          if (!cs->bgs.empty() && cs->bgs.back().kind == BG_NONE && cs->bgs.back().url.empty())
            cs->bgs.back() = layer;
          else
            cs->bgs.push_back(layer);
        }
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
    else if (s == "sub") {
      cs->vert_align = VA_LEN;
      cs->vert_len = Length();
      cs->vert_len.unit = L_EM;
      cs->vert_len.val = -0.25f;
    } else if (s == "super") {
      cs->vert_align = VA_LEN;
      cs->vert_len = Length();
      cs->vert_len.unit = L_EM;
      cs->vert_len.val = 0.35f;
    } else {
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
  if (prop == "border-collapse") { cs->border_collapse = lc(v) == "collapse"; return true; }
  if (prop == "clear") {
    std::string s = lc(v);
    cs->clear = s == "left" ? 1 : (s == "right" ? 2 : (s == "both" ? 3 : 0));
    return true;
  }
  if (prop == "float") {
    std::string s = lc(v);
    cs->css_float = s == "left" ? 1 : (s == "right" ? 2 : 0);
    return true;
  }
  if (prop == "transition" || prop == "transition-duration") {
    std::string body = substitute_vars(v, rc);
    if (prop == "transition-duration") {
      std::string s = lc(trim(body));
      cs->trans_dur = (float)atof(s.c_str()) * (s.find("ms") != std::string::npos ? 0.001f : 1.f);
      if (cs->trans_dur > 0 && cs->trans_props) g_any_transitions = true;
      return true;
    }
    std::vector<std::string> toks;
    {
      std::string cur;
      for (char c : body) {
        if (isspace((unsigned char)c)) {
          if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else cur += c;
      }
      if (!cur.empty()) toks.push_back(cur);
    }
    cs->trans_props = 0;
    cs->trans_dur = 0;
    cs->trans_timing = "ease";
    for (auto& t : toks) {
      std::string s = lc(t);
      if (s == "all") cs->trans_props = 15;
      else if (s == "opacity") cs->trans_props |= 1;
      else if (s == "color") cs->trans_props |= 2;
      else if (s == "background-color") cs->trans_props |= 4;
      else if (s == "transform") cs->trans_props |= 8;
      else if (s == "ease" || s == "linear" || s == "ease-in" || s == "ease-out" || s == "ease-in-out")
        cs->trans_timing = s;
      else if (!s.empty() && (isdigit((unsigned char)s[0]) || s[0] == '.') &&
               (s.back() == 's' || s.find("ms") == s.size() - 2))
        cs->trans_dur = (float)atof(s.c_str()) * (s.find("ms") != std::string::npos ? 0.001f : 1.f);
    }
    if (cs->trans_props && cs->trans_dur > 0) g_any_transitions = true;
    return true;
  }
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
      "table{display:table;border-collapse:collapse}"
      "caption,col,colgroup{display:none}"
      "tbody,thead,tfoot{display:table-row-group}"
      "tr{display:table-row}"
      "td,th{display:table-cell;padding:1px;vertical-align:middle}"
      "th{font-weight:700;text-align:center}"
      "legend,label,option,optgroup{display:block}"
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

void style_subtree(Document* doc, Node* root, int vw, int vh) {
  if (!doc || !root || root->type != N_ELEMENT) return;
  ComputedStyle* parent_cs = root->parent && root->parent->cs ? root->parent->cs : nullptr;
  if (!parent_cs) {
    style_tree(doc, vw, vh);
    return;
  }
  compute_style_for(root, parent_cs, &doc->root_vars, doc->sheet, doc->root_font_size, vw, vh);
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

/* ---- CSS transitions ---- */
struct TransVal {
  float from = 0, to = 0;                 /* opacity */
  Color c_from{}, c_to{};                 /* color / background-color */
  bool c_valid = false;
  float m_from[6] = {1, 0, 0, 1, 0, 0};   /* transform */
  float m_to[6] = {1, 0, 0, 1, 0, 0};
  bool m_valid = false;
  double t0 = 0;
  float dur = 0;
  std::string timing;
};
static std::unordered_map<Node*, std::map<std::string, TransVal>> g_transitions;

void transitions_reset() { g_transitions.clear(); }
bool css_has_transitions() { return g_any_transitions; }

static Color transition_lerp_color(const Color& a, const Color& b, float t) {
  Color r;
  r.a = (uint8_t)(a.a + ((int)b.a - (int)a.a) * t);
  r.r = (uint8_t)(a.r + ((int)b.r - (int)a.r) * t);
  r.g = (uint8_t)(a.g + ((int)b.g - (int)a.g) * t);
  r.b = (uint8_t)(a.b + ((int)b.b - (int)a.b) * t);
  return r;
}

static void apply_transitions(Node* el, ComputedStyle* cs) {
  if (!el || !cs || cs->trans_props == 0 || cs->trans_dur <= 0) return;
  double now = anim_time_s();
  std::map<std::string, TransVal>& st = g_transitions[el];
  auto progress = [](const TransVal& tv, double now) {
    return tv.dur > 0 ? anim_ease(tv.timing, (float)((now - tv.t0) / tv.dur)) : 1.f;
  };
  auto retarget = [&](TransVal& tv, double now) {
    float p = progress(tv, now);
    if (tv.t0 != 0) {
      if (tv.c_valid) tv.c_from = transition_lerp_color(tv.c_from, tv.c_to, p);
      if (tv.m_valid)
        for (int i = 0; i < 6; i++) tv.m_from[i] += (tv.m_to[i] - tv.m_from[i]) * p;
      tv.from += (tv.to - tv.from) * p;
    }
    tv.t0 = now;
    tv.dur = cs->trans_dur;
    tv.timing = cs->trans_timing;
  };
  if (cs->trans_props & 1) {
    TransVal& tv = st["opacity"];
    if (tv.t0 == 0) {
      tv.from = tv.to = cs->opacity;
      tv.t0 = now;
      tv.dur = cs->trans_dur;
      tv.timing = cs->trans_timing;
    } else if (tv.to != cs->opacity) {
      retarget(tv, now);
      tv.to = cs->opacity;
    }
    float p = progress(tv, now);
    cs->opacity = tv.from + (tv.to - tv.from) * p;
  }
  if (cs->trans_props & 2) {
    TransVal& tv = st["color"];
    if (tv.t0 == 0) {
      tv.c_from = tv.c_to = cs->color;
      tv.c_valid = true;
      tv.t0 = now;
      tv.dur = cs->trans_dur;
      tv.timing = cs->trans_timing;
    } else if (!(tv.c_to == cs->color)) {
      retarget(tv, now);
      tv.c_to = cs->color;
      tv.c_valid = true;
    }
    float p = progress(tv, now);
    cs->color = transition_lerp_color(tv.c_from, tv.c_to, p);
  }
  if (cs->trans_props & 4) {
    TransVal& tv = st["background-color"];
    if (tv.t0 == 0) {
      tv.c_from = tv.c_to = cs->bg_color;
      tv.c_valid = true;
      tv.t0 = now;
      tv.dur = cs->trans_dur;
      tv.timing = cs->trans_timing;
    } else if (!(tv.c_to == cs->bg_color)) {
      retarget(tv, now);
      tv.c_to = cs->bg_color;
      tv.c_valid = true;
    }
    float p = progress(tv, now);
    cs->bg_color = transition_lerp_color(tv.c_from, tv.c_to, p);
  }
  if (cs->trans_props & 8) {
    TransVal& tv = st["transform"];
    bool differs = !tv.m_valid || memcmp(tv.m_to, cs->mat, sizeof(tv.m_to)) != 0;
    if (tv.t0 == 0) {
      memcpy(tv.m_from, cs->mat, sizeof(tv.m_from));
      memcpy(tv.m_to, cs->mat, sizeof(tv.m_to));
      tv.m_valid = true;
      tv.t0 = now;
      tv.dur = cs->trans_dur;
      tv.timing = cs->trans_timing;
    } else if (differs) {
      retarget(tv, now);
      memcpy(tv.m_to, cs->mat, sizeof(tv.m_to));
      tv.m_valid = true;
    }
    float p = progress(tv, now);
    for (int i = 0; i < 6; i++) cs->mat[i] = tv.m_from[i] + (tv.m_to[i] - tv.m_from[i]) * p;
    static const float kIdentity[6] = {1, 0, 0, 1, 0, 0};
    if (memcmp(cs->mat, kIdentity, sizeof(kIdentity)) != 0) cs->has_transform = true;
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
  apply_transitions(el, cs);

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
  if (!doc || !doc->root || (doc->sheet.keyframes.empty() && !g_any_transitions)) return;
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
      apply_transitions(n, n->cs);
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
