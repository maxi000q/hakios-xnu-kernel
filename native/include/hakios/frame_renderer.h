#pragma once

// The display path -- stage 5 of the app edition roadmap: what the guest
// draws in its storage-backed VRAM, presented on the phone's screen.
//
// The design is the Adreno 619 tuning doc come true (devices/adreno-619/):
//
//   - one texture, one quad, no readback: each presented frame is one
//     glTexImage2D of the guest's 640x960 RGB565 framebuffer (1.17 MiB --
//     the same bytes the guest wrote, no client-side conversion), one
//     clip-space quad, one swap;
//   - 30 fps cap: a tile-based GPU's worst enemy is a 60 Hz re-upload of
//     a texture it keeps re-reading, and a research interpreter cannot
//     fill 60 frames anyway -- the renderer paces itself at 33 ms and the
//     big core stays at low clocks;
//   - RGB565: the guest's display is 16-bit (guest_map.h), so the upload
//     needs no format conversion anywhere on the path;
//   - letterboxing: the emulated 2:3 screen is fitted whole inside whatever
//     surface the phone gives, never stretched.
//
// The GL itself sits behind Backend so the desktop rig can prove the whole
// policy -- which bytes are uploaded, when a frame is due, how the quad is
// placed -- with a recording mock, exactly the way CI runs it; the Android
// build uses the EGL/GLES2 backend in native/gpu/egl_backend.cpp.

#include <cstdint>
#include <string>

#include "hakios/storage_memory.h"

namespace hakios {

class FrameRenderer {
public:
    // What the renderer needs from a GLES 2 context. init() may be called
    // again after the surface is destroyed and recreated; upload/ draw/ swap
    // happen once per presented frame, in that order.
    class Backend {
    public:
        virtual ~Backend() = default;
        // Prepare a context on the current surface; reports its size.
        virtual bool init(int* surface_w, int* surface_h, std::string* err) = 0;
        // One frame of guest pixels, RGB565 little-endian, w*h*2 bytes.
        virtual void upload_rgb565(const uint16_t* pixels, int w, int h) = 0;
        // Draw the textured quad inside the given clip-space rectangle
        // (-1..1 on both axes), letterboxed by the caller.
        virtual void draw_quad(float x0, float y0, float x1, float y1) = 0;
        // Present (eglSwapBuffers on Android).
        virtual void swap() = 0;
    };

    static constexpr int64_t kFrameIntervalMs = 33;  // the 30 fps cap

    FrameRenderer(StorageMemory& guest, Backend* backend) : guest_(guest), backend_(backend) {}

    // The backend may be attached or swapped at any time (the JNI bridge
    // hands over the EGL one when the app's surface exists). A swap resets
    // the surface state: the new backend needs its own init() before any
    // frame goes out, and a null backend silences the renderer.
    void set_backend(Backend* backend) {
        backend_ = backend;
        surface_w_ = surface_h_ = 0;
        last_present_ms_ = -1;
    }
    bool has_backend() const { return backend_ != nullptr; }

    // Acquire the surface. Safe to re-run (surface recreated).
    bool init(std::string* err);

    // Present a frame if one is due at now_ms (a monotonic clock the caller
    // owns, so the tests own time too). Returns true when a frame went out;
    // false is normal -- it means the 33 ms budget is not spent yet.
    bool render(int64_t now_ms);

    // The letterbox rectangle in clip space for the current surface and the
    // guest screen's aspect; public because the tests check the arithmetic
    // and the UI can draw its bezel from the same numbers.
    void quad_rect(float* x0, float* y0, float* x1, float* y1) const;

    uint64_t frames_presented() const { return frames_; }
    int64_t last_present_ms() const { return last_present_ms_; }

private:
    StorageMemory& guest_;
    Backend* backend_;
    int surface_w_ = 0, surface_h_ = 0;
    int64_t last_present_ms_ = -1;
    uint64_t frames_ = 0;
};

}  // namespace hakios
