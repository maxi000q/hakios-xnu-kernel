#pragma once

// The Android side of the stage-5 display path: an EGL/GLES 2 Backend over
// an ANativeWindow, owned by the JNI bridge (stage 6) and driven by the app's
// SurfaceView (stage 7). Compiled only on Android -- the desktop rig proves
// the renderer policy with a mock Backend instead (tests/test_gpu.cpp).

#ifdef __ANDROID__

#include <string>

#include "hakios/frame_renderer.h"

namespace hakios {

class EglBackend : public FrameRenderer::Backend {
public:
    // window stays owned by the caller (the JNI surface reference);
    // EglBackend borrows it for the life of the object.
    explicit EglBackend(void* android_native_window) : window_(android_native_window) {}
    ~EglBackend() override;

    bool init(int* surface_w, int* surface_h, std::string* err) override;
    void upload_rgb565(const uint16_t* pixels, int w, int h) override;
    void draw_quad(float x0, float y0, float x1, float y1) override;
    void swap() override;

private:
    void release() noexcept;

    void*    window_ = nullptr;   // ANativeWindow*
    void*    display_ = nullptr;  // EGLDisplay
    void*    surface_ = nullptr;  // EGLSurface
    void*    context_ = nullptr;  // EGLContext
    int      width_ = 0, height_ = 0;  // of the EGL surface, re-read at init
    unsigned tex_ = 0;            // the one texture
    unsigned program_ = 0;        // the one program
    int      pos_loc_ = -1;
    int      uv_loc_ = -1;
};

}  // namespace hakios

#endif  // __ANDROID__
