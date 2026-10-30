// Stage 5 unit tests: the Adreno 619 display path's policy, proven with a
// recording mock GL -- the GLES calls themselves run on the phone, but what
// they are given (which bytes, when, where) is exactly what these tests pin.
//
// Run: tests/run_desktop_tests.sh   (or link manually with the host g++)

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hakios/frame_renderer.h"
#include "hakios/guest_map.h"
#include "hakios/storage_memory.h"

using hakios::FrameRenderer;
using hakios::StorageMemory;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

namespace {

struct Upload {
    int w = 0, h = 0;
    std::vector<uint16_t> pixels;
};
struct Quad {
    float x0, y0, x1, y1;
};

// The mock backend: records everything the renderer asks of GL, in order.
class MockBackend : public FrameRenderer::Backend {
public:
    bool init(int* surface_w, int* surface_h, std::string* err) override {
        if (broken_) {
            if (err) *err = "mock: the surface is broken";
            return false;
        }
        *surface_w = w;
        *surface_h = h;
        inits_++;
        return true;
    }
    void upload_rgb565(const uint16_t* pixels, int w, int h) override {
        Upload u;
        u.w = w; u.h = h;
        u.pixels.assign(pixels, pixels + size_t(w) * h);
        uploads.push_back(std::move(u));
    }
    void draw_quad(float x0, float y0, float x1, float y1) override {
        quads.push_back(Quad{x0, y0, x1, y1});
    }
    void swap() override { swaps_++; }

    int w = 1080, h = 2340;         // a real Adreno 619-class panel
    bool broken_ = false;
    int inits_ = 0;
    int swaps_ = 0;
    std::vector<Upload> uploads;
    std::vector<Quad> quads;
};

std::string test_file(const char* name) {
    static std::string dir;
    if (dir.empty()) {
        char tmpl[] = "/tmp/hakios-gpu-test-XXXXXX";
        if (!::mkdtemp(tmpl)) {
            std::printf("FAIL: mkdtemp\n");
            std::exit(1);
        }
        dir = tmpl;
    }
    return dir + "/" + name;
}

}  // namespace

static void test_pacing_and_uploads() {
    StorageMemory mem;
    CHECK(mem.create(test_file("guest.img"), hakios::map::kPhysSize));
    // a recognizable framebuffer: pixel (x, y) = rgb565(x & 31, y & 63, 7)
    for (uint32_t y = 0; y < hakios::map::kScreenHeight; y++)
        for (uint32_t x = 0; x < hakios::map::kScreenWidth; x++)
            mem.write16(hakios::map::kVramBase + 2 * (size_t(y) * hakios::map::kScreenWidth + x),
                        uint16_t(hakios::map::rgb565(x & 31, y & 63, 7)));

    MockBackend mock;
    FrameRenderer r(mem, &mock);
    std::string err;
    CHECK(r.init(&err));
    if (!err.empty()) std::printf("renderer init: %s\n", err.c_str());
    CHECK(mock.inits_ == 1);

    // -- the 30 fps cap --------------------------------------------------
    CHECK(r.render(1000));            // the first frame is due immediately
    CHECK(r.render(1010) == false);   // 10 ms later: not due
    CHECK(r.render(1020) == false);   // 20 ms later: not due
    CHECK(r.render(1032) == false);   // 32 ms later: still under 33
    CHECK(r.render(1033));            // exactly the budget: due
    CHECK(r.render(1034) == false);
    CHECK(r.render(1500));            // long gaps never starve the display
    CHECK(r.frames_presented() == 3);
    CHECK(mock.swaps_ == 3);
    CHECK(mock.uploads.size() == 3);
    CHECK(mock.quads.size() == 3);

    // -- one upload per presented frame, the guest's own bytes ------------
    const Upload& u = mock.uploads.front();
    CHECK(u.w == int(hakios::map::kScreenWidth));
    CHECK(u.h == int(hakios::map::kScreenHeight));
    CHECK(u.pixels.size() == hakios::map::kVramBytesUsed / 2);
    CHECK(u.pixels[0] == hakios::map::rgb565(0, 0, 7));
    CHECK(u.pixels[1] == hakios::map::rgb565(1, 0, 7));
    const size_t last = u.pixels.size() - 1;
    const uint32_t lx = hakios::map::kScreenWidth - 1, ly = hakios::map::kScreenHeight - 1;
    CHECK(u.pixels[last] == hakios::map::rgb565(lx & 31, ly & 63, 7));

    // a changed guest pixel reaches the next upload: no caching on the path
    mem.write16(hakios::map::kVramBase, 0xFFFF);
    CHECK(r.render(1600));
    CHECK(mock.uploads.back().pixels[0] == 0xFFFF);
}

static void test_letterbox() {
    StorageMemory mem;
    CHECK(mem.create(test_file("guest2.img"), hakios::map::kPhysSize));
    MockBackend mock;

    // a 1080x2340 phone panel is taller than the 2:3 guest screen: the
    // guest fills the width, the quad is a centered 2:3 column
    FrameRenderer tall(mem, &mock);
    CHECK(tall.init(nullptr));
    float x0, y0, x1, y1;
    tall.quad_rect(&x0, &y0, &x1, &y1);
    const float aspect_surface = 1080.0f / 2340.0f;
    const float aspect_guest = 640.0f / 960.0f;
    CHECK(x0 == -1.0f && x1 == 1.0f);                       // full width
    CHECK(y1 - y0 > 2.0f * aspect_surface / aspect_guest - 0.001f);
    CHECK(y1 - y0 < 2.0f * aspect_surface / aspect_guest + 0.001f);
    CHECK(y0 == -(y1) );                                    // centered
    tall.render(0);
    CHECK(mock.quads.back().x0 == -1.0f && mock.quads.back().x1 == 1.0f);

    // a square surface: the guest is a centered 2:3 column -- height fills,
    // width shrinks to 2 * (2/3)
    MockBackend square;
    square.w = 1000; square.h = 1000;
    FrameRenderer centered(mem, &square);
    CHECK(centered.init(nullptr));
    centered.quad_rect(&x0, &y0, &x1, &y1);
    CHECK(y0 == -1.0f && y1 == 1.0f);                       // full height
    CHECK(x1 - x0 > 2.0f * aspect_guest - 0.001f);          // exactly 2:3 wide
    CHECK(x1 - x0 < 2.0f * aspect_guest + 0.001f);
    CHECK(x0 == -(x1));                                     // centered
}

static void test_failure_paths() {
    StorageMemory mem;
    CHECK(mem.create(test_file("guest3.img"), hakios::map::kPhysSize));

    MockBackend broken;
    broken.broken_ = true;
    FrameRenderer r(mem, &broken);
    std::string err;
    CHECK(r.init(&err) == false);
    CHECK(!err.empty());
    CHECK(r.render(0) == false);        // uninit: nothing goes out

    MockBackend tiny;
    tiny.w = 0; tiny.h = 0;             // a surface with no size
    FrameRenderer r2(mem, &tiny);
    err.clear();
    CHECK(r2.init(&err) == false);
    CHECK(r2.render(0) == false);

    FrameRenderer r3(mem, nullptr);     // no backend at all
    CHECK(r3.init(&err) == false);

    // re-init after the surface went away and came back (rotation, resume)
    MockBackend fresh;
    fresh.w = 720; fresh.h = 1600;
    FrameRenderer r4(mem, &fresh);
    CHECK(r4.init(nullptr));
    CHECK(r4.render(0));
    CHECK(r4.render(5000));
    fresh.w = 1600; fresh.h = 720;      // rotated
    CHECK(r4.init(nullptr));            // re-acquire is a normal operation
    CHECK(r4.render(5033));
}

int main() {
    test_pacing_and_uploads();
    test_letterbox();
    test_failure_paths();
    if (g_failures == 0)
        std::printf("gpu tests: OK (pacing, uploads, letterbox, failure paths)\n");
    else
        std::printf("gpu tests: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
