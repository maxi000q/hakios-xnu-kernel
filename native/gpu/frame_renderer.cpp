#include "hakios/frame_renderer.h"

#include "hakios/guest_map.h"

namespace hakios {

bool FrameRenderer::init(std::string* err) {
    if (!backend_) {
        if (err) *err = "renderer: no backend";
        return false;
    }
    if (!backend_->init(&surface_w_, &surface_h_, err)) {
        surface_w_ = surface_h_ = 0;
        return false;
    }
    if (surface_w_ <= 0 || surface_h_ <= 0) {
        if (err) *err = "renderer: the surface has no size";
        return false;
    }
    return true;
}

void FrameRenderer::quad_rect(float* x0, float* y0, float* x1, float* y1) const {
    // Fit the guest screen (kScreenWidth x kScreenHeight) inside the surface
    // with 40:60 margins the way a phone bezel would, never stretched: the
    // longest side fills its axis, the other is centered.
    const float guest_aspect =
        float(map::kScreenWidth) / float(map::kScreenHeight);      // 2:3
    const float surface_aspect = surface_h_ > 0
        ? float(surface_w_) / float(surface_h_)
        : guest_aspect;
    float w = 2.0f, h = 2.0f;                     // the full clip-space square
    if (surface_aspect > guest_aspect) {           // surface is wider: fit height
        w = 2.0f * guest_aspect / surface_aspect;
    } else {                                       // surface is taller: fit width
        h = 2.0f * surface_aspect / guest_aspect;
    }
    if (x0) *x0 = -w / 2;
    if (y0) *y0 = -h / 2;
    if (x1) *x1 = w / 2;
    if (y1) *y1 = h / 2;
}

bool FrameRenderer::render(int64_t now_ms) {
    if (surface_w_ <= 0) return false;             // not initialized
    if (last_present_ms_ >= 0 && now_ms - last_present_ms_ < kFrameIntervalMs)
        return false;                              // the 30 fps cap holds

    // One frame: the guest's own bytes, straight out of storage-backed VRAM.
    // A cold app that Android evicted refaults here, exactly as designed --
    // nothing is cached in the heap across a kill.
    const size_t bytes = map::kVramBytesUsed;
    if (guest_.size() < map::kVramBase + bytes) return false;
    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(guest_.base() + map::kVramBase);

    backend_->upload_rgb565(pixels, map::kScreenWidth, map::kScreenHeight);
    float x0, y0, x1, y1;
    quad_rect(&x0, &y0, &x1, &y1);
    backend_->draw_quad(x0, y0, x1, y1);
    backend_->swap();
    last_present_ms_ = now_ms;
    frames_++;
    return true;
}

}  // namespace hakios
