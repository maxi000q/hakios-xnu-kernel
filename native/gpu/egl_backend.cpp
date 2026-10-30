// The Android EGL/GLES 2 backend of the stage-5 renderer. Everything GL in
// the app lives here: one context, one texture, one two-triangle quad, one
// passthrough shader pair. No readback, no client copies, no extensions --
// GLES 2 is the floor on every Adreno 619 phone (devices/adreno-619/).
//
// The passthrough shaders are the trivial ones every GLES tutorial starts
// from, written for this file: a quad and a texture sample. The renderer's
// policy (pacing, letterboxing, which bytes to upload) lives in
// frame_renderer.cpp and is proven by the desktop rig; this file is only
// the surface it stands on, exercised on the phone itself.

#ifdef __ANDROID__

#include "hakios/egl_backend.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>

namespace hakios {

namespace {

const char* kVertexShader =
    "attribute vec2 pos;\n"
    "attribute vec2 uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "    v_uv = uv;\n"
    "}\n";

const char* kFragmentShader =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(tex, v_uv);\n"
    "}\n";

unsigned compile(unsigned kind, const char* src, std::string* err) {
    const unsigned shader = glCreateShader(kind);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        if (err) *err = std::string("gles: shader: ") + log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

EglBackend::~EglBackend() { release(); }

void EglBackend::release() noexcept {
    if (display_) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (tex_) glDeleteTextures(1, &tex_);
        if (program_) glDeleteProgram(program_);
        if (surface_) eglDestroySurface(display_, surface_);
        if (context_) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }
    display_ = surface_ = context_ = nullptr;
    program_ = 0;
    tex_ = 0;
}

bool EglBackend::init(int* surface_w, int* surface_h, std::string* err) {
    release();
    if (!window_) {
        if (err) *err = "egl: no native window";
        return false;
    }
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        if (err) *err = "egl: no display";
        return false;
    }
    if (!eglInitialize(display_, nullptr, nullptr)) {
        if (err) *err = "egl: initialize failed";
        release();
        return false;
    }
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,  // RGB565 where offered
        EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(display_, cfg_attrs, &cfg, 1, &n) || n != 1) {
        if (err) *err = "egl: no config with an RGB565 ES2 window surface";
        release();
        return false;
    }
    surface_ = eglCreateWindowSurface(display_, cfg, (EGLNativeWindowType)window_, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        if (err) *err = "egl: window surface failed";
        release();
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context_ = eglCreateContext(display_, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (context_ == EGL_NO_CONTEXT) {
        if (err) *err = "egl: ES2 context failed";
        release();
        return false;
    }
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        if (err) *err = "egl: make current failed";
        release();
        return false;
    }

    const unsigned vs = compile(GL_VERTEX_SHADER, kVertexShader, err);
    if (!vs) { release(); return false; }
    const unsigned fs = compile(GL_FRAGMENT_SHADER, kFragmentShader, err);
    if (!fs) { glDeleteShader(vs); release(); return false; }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    int ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(program_, sizeof log, nullptr, log);
        if (err) *err = std::string("gles: link: ") + log;
        release();
        return false;
    }
    glUseProgram(program_);
    pos_loc_ = glGetAttribLocation(program_, "pos");
    uv_loc_ = glGetAttribLocation(program_, "uv");
    const int tex_loc = glGetUniformLocation(program_, "tex");
    glUniform1i(tex_loc, 0);

    glGenTextures(1, &tex_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);  // RGB565 rows are 2-byte aligned

    EGLint w = 0, h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    width_ = int(w);
    height_ = int(h);
    if (surface_w) *surface_w = int(w);
    if (surface_h) *surface_h = int(h);
    return true;
}

void EglBackend::upload_rgb565(const uint16_t* pixels, int w, int h) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB,
                 GL_UNSIGNED_SHORT_5_6_5, pixels);
}

void EglBackend::draw_quad(float x0, float y0, float x1, float y1) {
    // x grows right, y grows up; guest pixel row 0 is the top, so v is
    // flipped: uv (0,1) at the bottom-left of the quad is guest (0, h).
    const float verts[] = {
        x0, y0,  0.0f, 1.0f,
        x1, y0,  1.0f, 1.0f,
        x0, y1,  0.0f, 0.0f,
        x1, y1,  1.0f, 0.0f,
    };
    glViewport(0, 0, width_, height_);   // re-checked every frame: rotations
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttribPointer(pos_loc_, 2, GL_FLOAT, GL_FALSE, 16, verts);
    glVertexAttribPointer(uv_loc_, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
    glEnableVertexAttribArray(pos_loc_);
    glEnableVertexAttribArray(uv_loc_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void EglBackend::swap() { eglSwapBuffers(display_, surface_); }

}  // namespace hakios

#endif  // __ANDROID__
