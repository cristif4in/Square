#pragma once

#include "square_internal.h"

#include <string>
#include <vector>

namespace sqrl {

inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

struct ResolveCtx {
  const CustomProps* vars = nullptr;
  float em = 16;    /* current font size for em in non-font props */
  float rem = 16;
  float vw = 0, vh = 0; /* viewport, for vw/vh units in font-size etc. */
  Color current_color{0, 0, 0, 255};
};

std::string substitute_vars(const std::string& val, const ResolveCtx& rc, int depth = 0);
Length parse_length_component(const std::string& t);
bool parse_length(const std::string& raw, Length* out, const ResolveCtx& rc);
bool parse_css_color(const std::string& raw, const ResolveCtx& rc, Color* out);
bool parse_gradient(const std::string& raw, BgLayer* layer, const ResolveCtx& rc);
float resolve_length(const Length& L, float pct_basis, float em, float rem, float vw, float vh);
void split_values(const std::string& v, std::vector<std::string>* out, const char* seps = " ");
void parse_side_values(const std::string& v, const ResolveCtx& rc, Length out[4]);
Display parse_display(const std::string& v);
void parse_background(const std::string& raw, ComputedStyle* cs, const ResolveCtx& rc);
void parse_grid_tracks(const std::string& v, std::vector<Track>* out,
                       std::vector<Track>* auto_out = nullptr,
                       uint8_t* auto_mode = nullptr);
void parse_box_shadows(const std::string& v, ComputedStyle* cs, const ResolveCtx& rc);
void parse_transform(const std::string& v, ComputedStyle* cs);

} /* namespace sqrl */
