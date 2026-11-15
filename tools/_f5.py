import io

# ---- header: TransDef + fields ----
p='src/square_internal.h'
s=io.open(p,encoding='utf-8').read()
old="""  /* transitions (subset: opacity / color / background-color / transform) */
  uint8_t trans_props = 0;               /* bit1 opacity, bit2 color, bit4 bg-color, bit8 transform */
  float trans_dur = 0;                   /* seconds */
  std::string trans_timing = "ease";"""
assert s.count(old)==1
s=s.replace(old,old+"""
  /* full transition definitions: one per transition-property entry */
  struct TransDef {
    std::string prop;                    /* property name or "all" */
    float dur = 0, delay = 0;            /* seconds */
    std::string timing = "ease";
  };
  std::vector<TransDef> trans_list;""")

# anim_ease decl if exported? it's static in style.cpp, fine.
io.open(p,'w',encoding='utf-8').write(s)
print("header ok")

# ---- style.cpp: bezier easing + longhand/shorthand parse ----
p='src/square_style.cpp'
s=io.open(p,encoding='utf-8').read()
old="""static float anim_ease(const std::string& tf, float t) {
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  if (tf == "ease-in-out") return t * t * (3 - 2 * t);
  if (tf == "ease-in") return t * t;
  if (tf == "ease-out") return 1 - (1 - t) * (1 - t);
  if (tf == "ease") return t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
  return t; /* linear, steps (approximated) */
}"""
new="""static float cubic_bezier_ease(float x1, float y1, float x2, float y2, float t) {
  /* solve bezier x(u) = t by bisection, then evaluate y(u) */
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  auto bez = [](float p0, float p1, float p2, float p3, float u) {
    float v = 1 - u;
    return v * v * v * p0 + 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u * p3;
  };
  float lo = 0, hi = 1;
  for (int it = 0; it < 24; it++) {
    float mid = (lo + hi) / 2;
    float x = bez(0, x1, x2, 1, mid);
    if (x < t) lo = mid;
    else hi = mid;
  }
  float u = (lo + hi) / 2;
  return bez(0, y1, y2, 1, u);
}

static float anim_ease(const std::string& tf, float t) {
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  if (tf.rfind("cubic-bezier(", 0) == 0) {
    float x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1.f;
    if (sscanf(tf.c_str() + 13, "%f , %f , %f , %f", &x1, &y1, &x2, &y2) == 4 ||
        sscanf(tf.c_str() + 13, "%f,%f,%f,%f", &x1, &y1, &x2, &y2) == 4) {
      return cubic_bezier_ease(x1, y1, x2, y2, t);
    }
  }
  if (tf == "ease-in-out") return t * t * (3 - 2 * t);
  if (tf == "ease-in") return t * t;
  if (tf == "ease-out") return 1 - (1 - t) * (1 - t);
  if (tf == "ease") return t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
  return t; /* linear, steps (approximated) */
}"""
assert s.count(old)==1
s=s.replace(old,new)

# transition parse: defs
old="""  if (prop == "transition" || prop == "transition-duration") {"""
new="""  if (prop == "transition-property" || prop == "transition-duration" ||
      prop == "transition-delay" || prop == "transition-timing-function") {"""
assert s.count(old)==1
s=s.replace(old,new)
old="""    if (prop == "transition-duration") {
      std::string s = lc(trim(body));
      cs->trans_dur = (float)atof(s.c_str()) * (s.find("ms") != std::string::npos ? 0.001f : 1.f);
      if (cs->trans_dur > 0 && cs->trans_props) g_any_transitions = true;
      return true;
    }"""
new="""    auto parse_time = [](const std::string& tok) {
      std::string s = lc(trim(tok));
      float v = (float)atof(s.c_str());
      return s.find("ms") != std::string::npos ? v * 0.001f : v;
    };
    if (prop == "transition-duration" || prop == "transition-delay") {
      std::vector<std::string> ts;
      split_values(lc(body), &ts);
      /* first value sets the global default (used by the shorthand paths) */
      if (!ts.empty()) {
        if (prop == "transition-duration") cs->trans_dur = parse_time(ts[0]);
        else cs->trans_delay = parse_time(ts[0]);
      }
      /* per-property lists: align with trans_list order (or first def) */
      for (size_t qi = 0; qi < ts.size() && qi < cs->trans_list.size(); qi++) {
        if (prop == "transition-duration") cs->trans_list[qi].dur = parse_time(ts[qi]);
        else cs->trans_list[qi].delay = parse_time(ts[qi]);
      }
      if (cs->trans_list.empty() && !ts.empty()) {
        ComputedStyle::TransDef d;
        d.prop = "all";
        d.dur = prop == "transition-duration" ? cs->trans_dur : 0;
        d.delay = prop == "transition-delay" ? cs->trans_delay : 0;
        d.timing = cs->trans_timing;
        cs->trans_list.push_back(d);
      }
      if (!cs->trans_list.empty()) g_any_transitions = true;
      return true;
    }
    if (prop == "transition-property") {
      std::vector<std::string> ts;
      split_values(lc(body), &ts);
      cs->trans_list.clear();
      for (auto& t : ts) {
        std::string pn = lc(trim(t));
        if (pn.empty() || pn == "none") continue;
        ComputedStyle::TransDef d;
        d.prop = pn;
        d.dur = cs->trans_dur;
        d.delay = cs->trans_delay;
        d.timing = cs->trans_timing;
        cs->trans_list.push_back(d);
      }
      if (!cs->trans_list.empty()) g_any_transitions = true;
      return true;
    }
    if (prop == "transition-timing-function") {
      cs->trans_timing = lc(trim(substitute_vars(v, rc)));
      for (auto& d : cs->trans_list) d.timing = cs->trans_timing;
      return true;
    }"""
assert s.count(old)==1
s=s.replace(old,new)

# shorthand: also build defs
old="""    cs->trans_props = 0;
    cs->trans_dur = 0;
    cs->trans_timing = "ease";
    for (auto& t : toks) {"""
new="""    cs->trans_props = 0;
    cs->trans_dur = 0;
    cs->trans_timing = "ease";
    cs->trans_list.clear();
    ComputedStyle::TransDef cur_def;
    cur_def.prop = "all";
    bool have_def = false;
    for (auto& t : toks) {"""
assert s.count(old)==1
s=s.replace(old,new)
old="""      else if (s == "ease" || s == "linear" || s == "ease-in" || s == "ease-out" || s == "ease-in-out")
        cs->trans_timing = s;
      else if (!s.empty() && (isdigit((unsigned char)s[0]) || s[0] == '.') &&
               (s.back() == 's' || s.find("ms") == s.size() - 2))
        cs->trans_dur = (float)atof(s.c_str()) * (s.find("ms") != std::string::npos ? 0.001f : 1.f);
    }
    if (cs->trans_props && cs->trans_dur > 0) g_any_transitions = true;
    return true;
  }"""
new="""      else if (s.rfind("cubic-bezier(", 0) == 0)
        cs->trans_timing = s;
      else if (s == "ease" || s == "linear" || s == "ease-in" || s == "ease-out" || s == "ease-in-out")
        cs->trans_timing = s;
      else if (!s.empty() && (isdigit((unsigned char)s[0]) || s[0] == '.') &&
               (s.back() == 's' || s.find("ms") == s.size() - 2)) {
        float tv2 = (float)atof(s.c_str()) * (s.find("ms") != std::string::npos ? 0.001f : 1.f);
        if (!have_def || cur_def.dur > 0) {
          if (have_def) cs->trans_list.push_back(cur_def);
          cur_def = ComputedStyle::TransDef{};
          cur_def.prop = "all";
        }
        cur_def.dur = tv2;
        have_def = true;
      }
    }
    if (have_def) cs->trans_list.push_back(cur_def);
    if (cs->trans_props && cs->trans_dur > 0) g_any_transitions = true;
    if (!cs->trans_list.empty()) g_any_transitions = true;
    return true;
  }"""
assert s.count(old)==1
s=s.replace(old,new)
io.open(p,'w',encoding='utf-8').write(s)
print("style ok")

# ---- apply_transitions: per-property def lookup + delay ----
p='src/square_style.cpp'
s=io.open(p,encoding='utf-8').read()
old="""  if (cs->trans_props == 0 || cs->trans_dur <= 0) return;
  double now = anim_time_s();
  std::map<std::string, TransVal>& st = g_transitions[el];
  auto progress = [](const TransVal& tv, double now) {
    return tv.dur > 0 ? anim_ease(tv.timing, (float)((now - tv.t0) / tv.dur)) : 1.f;
  };"""
new="""  bool any_list = !cs->trans_list.empty();
  if (cs->trans_props == 0 && !any_list) return;
  double now = anim_time_s();
  std::map<std::string, TransVal>& st = g_transitions[el];
  auto def_for = [&](const char* prop) -> const ComputedStyle::TransDef* {
    for (auto& d : cs->trans_list)
      if (d.prop == prop) return &d;
    for (auto& d : cs->trans_list)
      if (d.prop == "all") return &d;
    return nullptr;
  };
  auto prop_enabled = [&](const char* prop, uint8_t bit) -> bool {
    if (def_for(prop)) return true;
    return (cs->trans_props & bit) != 0;
  };
  auto timing_for = [&](const char* prop) -> std::string {
    const ComputedStyle::TransDef* d = def_for(prop);
    return d ? d->timing : cs->trans_timing;
  };
  auto dur_for = [&](const char* prop) -> float {
    const ComputedStyle::TransDef* d = def_for(prop);
    return d && d->dur > 0 ? d->dur : cs->trans_dur;
  };
  auto delay_for = [&](const char* prop) -> float {
    const ComputedStyle::TransDef* d = def_for(prop);
    return d ? d->delay : cs->trans_delay;
  };
  auto progress = [&](const TransVal& tv, double now) {
    if (tv.dur <= 0) return 1.f;
    float e = (float)((now - tv.t0 - tv.delay) / tv.dur);
    if (e <= 0) return 0.f;
    return anim_ease(tv.timing, std::min(1.f, e));
  };"""
assert s.count(old)==1
s=s.replace(old,new)

# retarget: carry delay
old="""    tv.t0 = now;
    tv.dur = cs->trans_dur;
    tv.timing = cs->trans_timing;
  };"""
new="""    tv.t0 = now;
    tv.dur = tv.prop_dur > 0 ? tv.prop_dur : cs->trans_dur;
    tv.delay = tv.prop_delay;
    tv.timing = tv.prop_timing.empty() ? cs->trans_timing : tv.prop_timing;
  };"""
assert s.count(old)==1
s=s.replace(old,new)

# TransVal: add fields
old="""  double t0 = 0;
  float dur = 0;
  std::string timing;
};"""
new="""  double t0 = 0;
  float dur = 0;
  float delay = 0;
  float prop_dur = 0, prop_delay = 0;
  std::string prop_timing;
  std::string timing;
};"""
assert s.count(old)==1
s=s.replace(old,new)

# each prop block: use prop helpers. Replace the four "tv.dur = cs->trans_dur; tv.timing = cs->trans_timing;" initializations
old="""    if (tv.t0 == 0) {
      tv.from = tv.to = cs->opacity;
      tv.t0 = now;
      tv.dur = cs->trans_dur;
      tv.timing = cs->trans_timing;
    } else if (tv.to != cs->opacity) {"""
new="""    if (tv.t0 == 0) {
      tv.from = tv.to = cs->opacity;
      tv.t0 = now;
      tv.prop_dur = dur_for("opacity");
      tv.prop_delay = delay_for("opacity");
      tv.prop_timing = timing_for("opacity");
      tv.dur = tv.prop_dur;
      tv.delay = tv.prop_delay;
      tv.timing = tv.prop_timing.empty() ? cs->trans_timing : tv.prop_timing;
    } else if (tv.to != cs->opacity) {"""
assert s.count(old)==1
s=s.replace(old,new)
old="""    if (tv.t0 == 0) {
      tv.c_from = tv.c_to = cs->color;
      tv.c_valid = true;
      tv.t0 = now;
      tv.dur = cs->trans_dur;
      tv.timing = cs->trans_timing;
    } else if (!(tv.c_to == cs->color)) {"""
new="""    if (tv.t0 == 0) {
      tv.c_from = tv.c_to = cs->color;
      tv.c_valid = true;
      tv.t0 = now;
      tv.prop_dur = dur_for("color");
      tv.prop_delay = delay_for("color");
      tv.prop_timing = timing_for("color");
      tv.dur = tv.prop_dur;
      tv.delay = tv.prop_delay;
      tv.timing = tv.prop_timing.empty() ? cs->trans_timing : tv.prop_timing;
    } else if (!(tv.c_to == cs->color)) {"""
assert s.count(old)==1
s=s.replace(old,new)
old="""    if (tv.t0 == 0) {
      tv.c_from = tv.c_to = cs->bg_color;
      tv.c_valid = true;
      tv.t0 = now;
      tv.dur = cs->trans_dur;
      tv.timing = cs->trans_timing;
    } else if (!(tv.c_to == cs->bg_color)) {"""
new="""    if (tv.t0 == 0) {
      tv.c_from = tv.c_to = cs->bg_color;
      tv.c_valid = true;
      tv.t0 = now;
      tv.prop_dur = dur_for("background-color");
      tv.prop_delay = delay_for("background-color");
      tv.prop_timing = timing_for("background-color");
      tv.dur = tv.prop_dur;
      tv.delay = tv.prop_delay;
      tv.timing = tv.prop_timing.empty() ? cs->trans_timing : tv.prop_timing;
    } else if (!(tv.c_to == cs->bg_color)) {"""
assert s.count(old)==1
s=s.replace(old,new)
old="""    if (tv.t0 == 0) {
      memcpy(tv.m_from, cs->mat, sizeof(tv.m_from));
      memcpy(tv.m_to, cs->mat, sizeof(tv.m_to));
      tv.m_valid = true;
      tv.t0 = now;
      tv.dur = cs->trans_dur;
      tv.timing = cs->trans_timing;
    } else if (differs) {"""
new="""    if (tv.t0 == 0) {
      memcpy(tv.m_from, cs->mat, sizeof(tv.m_from));
      memcpy(tv.m_to, cs->mat, sizeof(tv.m_to));
      tv.m_valid = true;
      tv.t0 = now;
      tv.prop_dur = dur_for("transform");
      tv.prop_delay = delay_for("transform");
      tv.prop_timing = timing_for("transform");
      tv.dur = tv.prop_dur;
      tv.delay = tv.prop_delay;
      tv.timing = tv.prop_timing.empty() ? cs->trans_timing : tv.prop_timing;
    } else if (differs) {"""
assert s.count(old)==1
s=s.replace(old,new)

# gate each block on prop_enabled
s=s.replace("  if (cs->trans_props & 1) {\n    TransVal& tv = st[\"opacity\"];",
            "  if (prop_enabled(\"opacity\", 1)) {\n    TransVal& tv = st[\"opacity\"];")
s=s.replace("  if (cs->trans_props & 2) {\n    TransVal& tv = st[\"color\"];",
            "  if (prop_enabled(\"color\", 2)) {\n    TransVal& tv = st[\"color\"];")
s=s.replace("  if (cs->trans_props & 4) {\n    TransVal& tv = st[\"background-color\"];",
            "  if (prop_enabled(\"background-color\", 4)) {\n    TransVal& tv = st[\"background-color\"];")
s=s.replace("  if (cs->trans_props & 8) {\n    TransVal& tv = st[\"transform\"];",
            "  if (prop_enabled(\"transform\", 8)) {\n    TransVal& tv = st[\"transform\"];")

# trans_delay field in ComputedStyle
old="""  float trans_dur = 0;                   /* seconds */
  std::string trans_timing = "ease";"""
new="""  float trans_dur = 0;                   /* seconds */
  float trans_delay = 0;                 /* seconds */
  std::string trans_timing = "ease";"""
assert s.count(old)==1
s=s.replace(old,new)
io.open(p,'w',encoding='utf-8').write(s)
print("apply ok")
