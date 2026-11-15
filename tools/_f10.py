import io

# ---- header: Rule.starting + cs snap fields + starting API ----
p='src/square_internal.h'
s=io.open(p,encoding='utf-8').read()
old="""struct Rule {
  std::vector<Selector> sels;
  std::vector<Declaration> decls;
  bool media_ok = true;
  int origin = 1;                /* 0 = user-agent, 1 = author */
  int order = 0;                 /* document order for tie-breaks */
};"""
assert s.count(old)==1
s=s.replace(old,old.replace("};","  bool starting = false;         /* @starting-style rule */\n};"))
old="""  uint8_t clear = 0;                     /* 0 none, 1 left, 2 right, 3 both */"""
assert s.count(old)==1
s=s.replace(old,"""  uint8_t snap_type = 0;                 /* scroll-snap-type: 1 = proximity-ish snap on */
  uint8_t snap_align = 0;                /* scroll-snap-align: 1 start, 2 center, 3 end */
  uint8_t clear = 0;                     /* 0 none, 1 left, 2 right, 3 both */""")
old="bool css_has_transitions();"
assert s.count(old)==1
s=s.replace(old,old+"\nbool css_starting_active();")
io.open(p,'w',encoding='utf-8').write(s)
print("header ok")

# ---- css.cpp: @starting-style at-rule ----
p='src/square_css.cpp'
s=io.open(p,encoding='utf-8').read()
old="""      } else if (name == "font-face") {"""
assert s.count(old)==1
s=s.replace(old,"""      } else if (name == "starting-style") {
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
        std::string inner = css.substr(start, c.i - start);
        if (c.i < css.size()) c.i++;
        StyleSheet tmp;
        css_parse_stylesheet(inner, &tmp, vw, vh);
        for (auto& r : tmp.rules) {
          r.starting = true;
          r.origin = 1;
          out->rules.push_back(r);
        }
      } else if (name == "font-face") {""")
io.open(p,'w',encoding='utf-8').write(s)
print("css ok")

# ---- style.cpp: apply starting rules in window + flag ----
p='src/square_style.cpp'
s=io.open(p,encoding='utf-8').read()
old="""static bool g_any_transitions = false;"""
assert s.count(old)==1
s=s.replace(old,old+"""
static double g_starting_until = -1;
bool css_starting_active() { return g_any_transitions && anim_time_s() < g_starting_until; }""")

# find rule matching loop in compute_style_for: search "origin" usage
import re
m = re.search(r'for \(auto& rule : sheet\.rules\)[^{]*\{', s)
assert m, "rules loop not found"
ins = m.end()
s = s[:ins] + "\n    if (rule.starting && !css_starting_active()) continue;" + s[ins:]

# style_tree: arm the window on first pass
old="""void style_tree(Document* doc, int vw, int vh) {
  ua_sheet(); /* ensure built */
  ComputedStyle initial;"""
new="""void style_tree(Document* doc, int vw, int vh) {
  ua_sheet(); /* ensure built */
  bool first_pass = !doc->starting_done;
  if (first_pass && !doc->sheet.rules.empty()) {
    bool has_starting = false;
    for (auto& r : doc->sheet.rules)
      if (r.starting) { has_starting = true; break; }
    if (has_starting) g_starting_until = anim_time_s() + 0.05;
  }
  ComputedStyle initial;"""
assert s.count(old)==1
s=s.replace(old,new)
old="""  if (doc->html && doc->html->cs) {
    doc->root_font_size = doc->html->cs->font_size;
    /* recompute html + subtree if font-size changed rem basis */
    compute_style_for(doc->html, &initial, &doc->root_vars, doc->sheet, doc->root_font_size, vw, vh);
  }
}"""
new=old+"""

  /* starting styles applied on the first pass: schedule the flip to final
   * styles (the animation loop keeps ticking while css_starting_active()) */
  if (first_pass && g_starting_until > 0) doc->starting_done = true;
  if (g_starting_until > 0 && anim_time_s() >= g_starting_until && !doc->starting_done2) {
    doc->starting_done2 = true;
    g_starting_until = -1;
  }"""
assert s.count(old)==1
s=s.replace(old,new)
io.open(p,'w',encoding='utf-8').write(s)
print("style ok")

# ---- Document: starting flags ----
p='src/square_internal.h'
s=io.open(p,encoding='utf-8').read()
old="""struct StyleSheet {
  std::vector<Rule> rules;"""
assert s.count(old)==1
# find Document struct instead
import re
m=re.search(r'struct Document \{', s)
assert m
s=s[:m.end()]+"\n  bool starting_done = false;\n  bool starting_done2 = false;"+s[m.end():]
io.open(p,'w',encoding='utf-8').write(s)
print("doc ok")

# ---- platform animation loop: keep ticking while starting active ----
p='src/square_platform.cpp'
s=io.open(p,encoding='utf-8').read()
old="""    if (inst->doc && (inst->doc->sheet.has_animations || css_has_transitions()) &&"""
assert s.count(old)==1
s=s.replace(old,"""    if (inst->doc && (inst->doc->sheet.has_animations || css_has_transitions() ||
                      css_starting_active()) &&""")
io.open(p,'w',encoding='utf-8').write(s)
print("loop ok")

# ---- scroll snapping: parse + apply ----
p='src/square_style.cpp'
s=io.open(p,encoding='utf-8').read()
old='"scroll-behavior", "scroll-snap-type", "scroll-margin", "scroll-padding",'
assert s.count(old)==1
s=s.replace(old,'"scroll-behavior", "scroll-margin", "scroll-padding",')
old="""  if (prop == "columns") {"""
assert s.count(old)==1
s=s.replace(old,"""  if (prop == "scroll-snap-type") {
    std::string s2 = lc(trim(substitute_vars(v, rc)));
    cs->snap_type = s2 == "none" || s2.empty() ? 0 : 1;
    return true;
  }
  if (prop == "scroll-snap-align") {
    std::string s2 = lc(trim(substitute_vars(v, rc)));
    if (s2.find("center") != std::string::npos) cs->snap_align = 2;
    else if (s2.find("end") != std::string::npos) cs->snap_align = 3;
    else if (s2.find("start") != std::string::npos || s2 == "none") cs->snap_align = s2 == "none" ? 0 : 1;
    else cs->snap_align = 1;
    return true;
  }
  if (prop == "columns") {""")
io.open(p,'w',encoding='utf-8').write(s)
print("snap parse ok")

# apply in apply_scroll: snap to nearest snap point when the root has snap-type
p='src/square_platform_api.cpp'
s=io.open(p,encoding='utf-8').read()
old="""  int new_y = std::max(0, std::min(y, scroll_max));
  std::lock_guard<std::mutex> plk(inst->paint_mtx);"""
assert s.count(old)==1
s=s.replace(old,"""  int new_y = std::max(0, std::min(y, scroll_max));
  /* scroll snapping: when the root has scroll-snap-type, snap to the nearest
   * aligned descendant (start/center/end) */
  if (inst->root_box && inst->doc && inst->doc->body && inst->doc->body->cs &&
      inst->doc->body->cs->snap_type) {
    float best = -1;
    float bestd = 1e9f;
    std::function<void(Box*)> scan = [&](Box* k) {
      if (k->node && k->node->cs && k->node->cs->snap_align) {
        float cand = k->y;
        if (k->node->cs->snap_align == 2) cand = k->y + k->h / 2 - vh / 2.f;
        else if (k->node->cs->snap_align == 3) cand = k->y + k->h - vh;
        cand = std::max(0.f, std::min(cand, (float)scroll_max));
        float d2 = fabsf(cand - (float)new_y);
        if (d2 < bestd) { bestd = d2; best = cand; }
      }
      for (Box* kk : k->kids) scan(kk);
      for (Box* kk : k->out_of_flow) scan(kk);
    };
    scan(inst->root_box);
    if (best >= 0 && bestd < vh * 0.5f) new_y = (int)best;
  }
  std::lock_guard<std::mutex> plk(inst->paint_mtx);"""
)
io.open(p,'w',encoding='utf-8').write(s)
print("snap apply ok")
