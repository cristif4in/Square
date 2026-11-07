#include "square_gfx_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STB_IMAGE_STATIC
#include "../thirdparty/stb_image.h"
/* full WebP support (VP8 lossy + VP8L lossless + alpha) — vendored libwebp decoder */
#include "webp/decode.h"

namespace sqrl {

/* ============================ images ============================ */
struct StoredImage {
  int w, h;
  std::vector<uint32_t> px;  /* 0xAARRGGBB */
};
static std::mutex g_img_mtx;
static std::vector<StoredImage*> g_images; /* 1-based ids */

int image_add_rgba(int w, int h, std::vector<uint8_t> rgba) {
  StoredImage* im = new StoredImage{w, h, {}};
  im->px.resize((size_t)w * h);
  for (size_t i = 0; i < im->px.size(); i++) {
    uint8_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
    im->px[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }
  std::lock_guard<std::mutex> lk(g_img_mtx);
  g_images.push_back(im);
  return (int)g_images.size();
}
bool image_decode(const std::vector<uint8_t>& bytes, int* out_gfx_id, int* out_w, int* out_h) {
  int w, h, comp;
  if (bytes.size() >= 12)
    engine_log(1, "[img-decode] %zu bytes head %02x %02x %02x %02x", bytes.size(), bytes[0],
               bytes[1], bytes[2], bytes[3]);

  /* ---- WebP (RIFF....WEBP) via vendored libwebp decoder ---- */
  if (bytes.size() >= 12 && memcmp(bytes.data(), "RIFF", 4) == 0 &&
      memcmp(bytes.data() + 8, "WEBP", 4) == 0) {
    int wp_w = 0, wp_h = 0;
    uint8_t* rgba = WebPDecodeRGBA(bytes.data(), bytes.size(), &wp_w, &wp_h);
    if (rgba) {
      engine_log(1, "[img-decode] webp ok %dx%d", wp_w, wp_h);
      std::vector<uint8_t> buf(rgba, rgba + (size_t)wp_w * wp_h * 4);
      WebPFree(rgba);
      int id = image_add_rgba(wp_w, wp_h, std::move(buf));
      if (out_gfx_id) *out_gfx_id = id;
      if (out_w) *out_w = wp_w;
      if (out_h) *out_h = wp_h;
      return true;
    }
    engine_log(1, "[img-decode] FAIL: webp decode error");
    return false;
  }

  /* ---- SVG (<svg ...> possibly after <?xml ...?>) → raster via svg_render ---- */
  if (bytes.size() >= 8 && bytes.size() < 4 * 1024 * 1024) {
    size_t scan = std::min(bytes.size(), (size_t)512);
    bool maybe_svg = false;
    for (size_t i = 0; i + 4 <= scan; i++) {
      if (memcmp(bytes.data() + i, "<svg", 4) == 0 || memcmp(bytes.data() + i, "<SVG", 4) == 0) {
        maybe_svg = true;
        break;
      }
    }
    if (maybe_svg) {
      std::string text(bytes.begin(), bytes.end());
      /* intrinsic size: width/height attrs, else viewBox */
      float iw = 0, ih = 0;
      {
        auto find_attr = [&text](const char* name, float* out) -> bool {
          size_t p = text.find(name);
          if (p == std::string::npos) return false;
          p = text.find('=', p + strlen(name));
          if (p == std::string::npos) return false;
          p = text.find('"', p);
          if (p == std::string::npos) return false;
          *out = (float)atof(text.c_str() + p + 1);
          return *out > 0;
        };
        float tw = 0, th = 0;
        if (find_attr("width", &tw) && find_attr("height", &th)) {
          iw = tw;
          ih = th;
        } else {
          /* viewBox="minx miny w h" */
          size_t p = text.find("viewBox");
          if (p == std::string::npos) p = text.find("viewbox");
          if (p != std::string::npos) {
            p = text.find('=', p);
            if (p != std::string::npos) {
              p = text.find('"', p);
              if (p != std::string::npos) {
                float x0, y0, vw2, vh2;
                if (sscanf(text.c_str() + p + 1, "%f %f %f %f", &x0, &y0, &vw2, &vh2) == 4 ||
                    sscanf(text.c_str() + p + 1, "%f,%f,%f,%f", &x0, &y0, &vw2, &vh2) == 4) {
                  if (vw2 > 0 && vh2 > 0) {
                    iw = vw2;
                    ih = vh2;
                  }
                }
              }
            }
          }
        }
      }
      if (iw <= 0 || ih <= 0) {
        iw = 300;
        ih = 150;
      } /* CSS replaced-element default */
      /* rasterize at 2x intrinsic (capped) for crisp downscaling */
      float zoom = 2.0f;
      while ((iw * zoom > 1024 || ih * zoom > 1024) && zoom > 1) zoom *= 0.5f;
      Surface* sv = svg_render(text, iw, ih, Color{0, 0, 0, 255}, zoom);
      if (!sv) return false;
      int sw2 = sv->w, sh2 = sv->h;
      std::vector<uint8_t> buf((size_t)sw2 * sh2 * 4);
      for (int y = 0; y < sh2; y++) {
        uint32_t* row = sv->row32(y);
        for (int x = 0; x < sw2; x++) {
          uint32_t c = row[x];
          buf[((size_t)y * sw2 + x) * 4 + 0] = (uint8_t)((c >> 16) & 255); /* R */
          buf[((size_t)y * sw2 + x) * 4 + 1] = (uint8_t)((c >> 8) & 255);  /* G */
          buf[((size_t)y * sw2 + x) * 4 + 2] = (uint8_t)(c & 255);         /* B */
          buf[((size_t)y * sw2 + x) * 4 + 3] = (uint8_t)((c >> 24) & 255); /* A */
        }
      }
      delete sv;
      engine_log(1, "[img-decode] svg ok %dx%d", sw2, sh2);
      int id = image_add_rgba(sw2, sh2, std::move(buf));
      if (out_gfx_id) *out_gfx_id = id;
      if (out_w) *out_w = sw2;
      if (out_h) *out_h = sh2;
      return true;
    }
  }

  unsigned char* data = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
  if (!data) {
    engine_log(1, "[img-decode] FAIL: %s", stbi_failure_reason());
    return false;
  }
  engine_log(1, "[img-decode] ok %dx%d first px %02x%02x%02x%02x", w, h, data[0], data[1],
             data[2], data[3]);
  std::vector<uint8_t> rgba(data, data + (size_t)w * h * 4);
  stbi_image_free(data);
  int id = image_add_rgba(w, h, std::move(rgba));
  if (out_gfx_id) *out_gfx_id = id;
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  return true;
}
void image_cache_clear() {
  std::lock_guard<std::mutex> lk(g_img_mtx);
  for (auto* p : g_images) delete p;
  g_images.clear();
}

static inline uint32_t sample_bilinear(const StoredImage& im, float fx, float fy) {
  fx = std::max(-1.f, std::min((float)im.w, fx));
  fy = std::max(-1.f, std::min((float)im.h, fy));
  int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
  float tx = fx - x0, ty = fy - y0;
  auto get = [&](int x, int y) -> uint32_t {
    x = std::max(0, std::min(im.w - 1, x));
    y = std::max(0, std::min(im.h - 1, y));
    return im.px[(size_t)y * im.w + x];
  };
  uint32_t c00 = get(x0, y0), c10 = get(x0 + 1, y0), c01 = get(x0, y0 + 1), c11 = get(x0 + 1, y0 + 1);
  uint32_t out = 0;
  for (int ch = 0; ch < 4; ch++) {
    int shift = ch * 8;
    float v00 = ((c00 >> shift) & 255), v10 = ((c10 >> shift) & 255);
    float v01 = ((c01 >> shift) & 255), v11 = ((c11 >> shift) & 255);
    float v = v00 * (1 - tx) * (1 - ty) + v10 * tx * (1 - ty) + v01 * (1 - tx) * ty + v11 * tx * ty;
    out |= (uint32_t)(v + 0.5f) << shift;
  }
  return out;
}

void gfx_draw_image(Surface& s, int gfx_id, const Rect& dst, const Rect* src_crop,
                    uint8_t fit, const Rect* clip, const RoundedClip* rc, float alpha) {
  if (dst.empty()) return;
  StoredImage* im = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_img_mtx);
    if (gfx_id >= 1 && gfx_id <= (int)g_images.size()) im = g_images[gfx_id - 1];
  }
  if (!im || im->w == 0 || im->h == 0) return;
  float sx0 = 0, sy0 = 0, sw = (float)im->w, sh = (float)im->h;
  bool fit_none = false;
  if (src_crop && src_crop->w > 0 && src_crop->h > 0) {
    sx0 = src_crop->x; sy0 = src_crop->y; sw = src_crop->w; sh = src_crop->h;
    fit = 0; /* stretch crop into dst */
  } else if (fit == 1 || fit == 2) { /* cover / contain */
    float scale = fit == 1 ? std::max(dst.w / sw, dst.h / sh) : std::min(dst.w / sw, dst.h / sh);
    sx0 = (sw - dst.w / scale) / 2;
    sy0 = (sh - dst.h / scale) / 2;
    sw = dst.w / scale;
    sh = dst.h / scale;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx0 + sw > im->w) { sw = (float)im->w - sx0; }
    if (sy0 + sh > im->h) { sh = (float)im->h - sy0; }
  } else if (fit == 3) { /* none: intrinsic size, unscaled, clipped to dst */
    fit_none = true;
  }
  int x0, y0, x1, y1;
  bounds_intersect(dst, clip, &x0, &y0, &x1, &y1, s);
  for (int y = y0; y < y1; y++) {
    uint32_t* row = s.row32(y);
    float py = y + 0.5f;
    float fy = fit_none ? sy0 + (py - dst.y) : sy0 + (py - dst.y) * sh / dst.h;
    for (int x = x0; x < x1; x++) {
      float px = x + 0.5f;
      float cov = clip_cov(px, py, clip, rc);
      if (cov <= 0) continue;
      float fx = fit_none ? sx0 + (px - dst.x) : sx0 + (px - dst.x) * sw / dst.w;
      if (fit_none && (fx >= (float)im->w || fy >= (float)im->h)) continue; /* dst larger than image */
      uint32_t sc = sample_bilinear(*im, fx, fy);
      uint32_t src = scale_alpha(sc, alpha * cov);
      row[x] = blend_src_over(row[x], src);
    }
  }
}

} /* namespace sqrl */
