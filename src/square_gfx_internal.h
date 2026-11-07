#pragma once
#include "square_internal.h"

namespace sqrl {

uint32_t blend_src_over(uint32_t d, uint32_t s);
uint32_t scale_alpha(uint32_t c, float k);
inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
void clamp_radii(const float in[4], float w, float h, float out[4]);
float clip_cov(float px, float py, const Rect* clip, const RoundedClip* rc);
void bounds_intersect(const Rect& r, const Rect* clip, int* x0, int* y0, int* x1, int* y1, const Surface& s);
struct GradLUT { uint32_t e[1024]; };
void build_lut(const std::vector<GradStop>& stops, GradLUT* lut);
std::string lower_copy(std::string s);
std::string trim_copy(const std::string& s);

} /* namespace sqrl */
