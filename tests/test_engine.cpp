// Stage 6 unit tests: the emulator engine -- lifecycle, slices, halt
// reporting, the display hand-off, persistence -- proven on the desktop with
// the stage-4 fixtures and the stage-5 mock. No threads in the assertions:
// run_sync() is the deterministic path, and the threaded path is exercised
// exactly once, with generous deadlines.
//
// Run: tests/run_desktop_tests.sh   (or link manually with the host g++)

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "hakios/emulator.h"
#include "hakios/guest_map.h"
#include "gen/image_fixtures.h"

using hakios::Arm7Cpu;
using hakios::Emulator;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

namespace {

class MockBackend : public hakios::FrameRenderer::Backend {
public:
    bool init(int* w, int* h, std::string*) override { *w = 1080; *h = 2340; return true; }
    void upload_rgb565(const uint16_t*, int, int) override { uploads_++; }
    void draw_quad(float, float, float, float) override {}
    void swap() override { swaps_++; }
    int uploads_ = 0, swaps_ = 0;
};

std::string test_file(const char* name) {
    static std::string dir;
    if (dir.empty()) {
        char tmpl[] = "/tmp/hakios-engine-test-XXXXXX";
        if (!::mkdtemp(tmpl)) {
            std::printf("FAIL: mkdtemp\n");
            std::exit(1);
        }
        dir = tmpl;
    }
    return dir + "/" + name;
}

}  // namespace

static void test_lifecycle_and_halt() {
    Emulator e;
    std::string err;

    // an engine with no image refuses to start, with a reason
    CHECK(e.start(&err) == false);
    CHECK(!err.empty());

    CHECK(e.create(test_file("guest.img"), &err));
    CHECK(e.status().state == Emulator::State::kReady);
    CHECK(e.start(&err) == false);           // no kernel yet
    err.clear();

    // the stage-4 chain through the engine API: plain fixture, no key
    CHECK(e.load_kernel(hakios::fixtures::kImg3Plain,
                        sizeof hakios::fixtures::kImg3Plain,
                        nullptr, nullptr, &err));
    CHECK(e.status().state == Emulator::State::kLoaded);
    CHECK(e.entry_pc() == hakios::fixtures::kEntryPc);
    CHECK(e.kernel_loaded());

    // the encrypted fixture without a key fails with the honest message
    Emulator e2;
    CHECK(e2.create(test_file("guest2.img"), &err));
    CHECK(e2.load_kernel(hakios::fixtures::kImg3Encrypted,
                         sizeof hakios::fixtures::kImg3Encrypted,
                         nullptr, nullptr, &err) == false);
    CHECK(err.find("encrypted") != std::string::npos);
    // with the key it loads
    CHECK(e2.load_kernel(hakios::fixtures::kImg3Encrypted,
                         sizeof hakios::fixtures::kImg3Encrypted,
                         "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf",
                         "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf",
                         &err));
    CHECK(e2.entry_pc() == hakios::fixtures::kEntryPc);
    // a malformed key is refused without touching the guest
    CHECK(e2.load_kernel(hakios::fixtures::kImg3Plain,
                         sizeof hakios::fixtures::kImg3Plain,
                         "c0c1", "a0a1", &err) == false);
    CHECK(err.find("hex") != std::string::npos);

    // run_sync: the fixture's "code" is a byte pattern, so the interpreter
    // halts on the first undecodable word -- which is exactly what the halt
    // reporting must surface, not crash on
    const uint64_t ran = e.run_sync(100000);
    CHECK(ran > 0);
    CHECK(ran <= 100000);
    Emulator::Status s = e.status();
    CHECK(s.state == Emulator::State::kHalted);
    CHECK(s.halt.reason == Arm7Cpu::Halt::kUnknownInstruction ||
          s.halt.reason == Arm7Cpu::Halt::kUnsupportedEncoding ||
          s.halt.reason == Arm7Cpu::Halt::kThumbUnsupported);
    CHECK(s.halt.pc != 0);
    CHECK(s.executed >= ran);
    CHECK(s.image_bytes == hakios::map::kPhysSize);

    // a halted engine runs nothing further until reset by start()
    CHECK(e.run_sync(1000) == 0);
    CHECK(e.start(&err));                    // restart after halt: resets CPU
    e.pause();
    s = e.status();
    CHECK(s.state == Emulator::State::kPaused || s.state == Emulator::State::kHalted);
}

static void test_persistence() {
    Emulator e;
    std::string err;
    const std::string path = test_file("persist.img");
    CHECK(e.create(path, &err));
    CHECK(e.load_kernel(hakios::fixtures::kImg3Plain,
                        sizeof hakios::fixtures::kImg3Plain,
                        nullptr, nullptr, &err));
    // the guest writes a mark through the CPU's memory... here: directly,
    // which is the same storage the CPU addresses
    e.memory().write32(0x80005000, 0x600DF00D);
    e.pause();
    e.close();

    // a fresh engine, the same image: "installed stays installed"
    Emulator again;
    CHECK(again.open(path, &err));
    CHECK(again.memory().read32(0x80005000) == 0x600DF00D);
    // the kernel is re-placed per session from the file the app kept
    CHECK(again.load_kernel(hakios::fixtures::kImg3Plain,
                            sizeof hakios::fixtures::kImg3Plain,
                            nullptr, nullptr, &err));
    CHECK(again.run_sync(1000) > 0);
}

static void test_display_handoff() {
    Emulator e;
    std::string err;
    CHECK(e.create(test_file("disp.img"), &err));
    CHECK(e.load_kernel(hakios::fixtures::kImg3Plain,
                        sizeof hakios::fixtures::kImg3Plain,
                        nullptr, nullptr, &err));
    // a guest that runs: replace the entry word with `B .` (0xEAFFFFFE) so
    // slices retire instructions instead of halting on the pattern bytes
    e.memory().write32(hakios::fixtures::kEntryPc, 0xEAFFFFFE);

    MockBackend mock;
    e.set_display(&mock);
    // the engine drives rendering from the thread that runs the slices:
    // run_sync renders once per call
    e.run_sync(500);
    CHECK(mock.uploads_ == 1);
    CHECK(mock.swaps_ == 1);
    e.run_sync(500);
    CHECK(mock.uploads_ == 2);

    // detaching the display is a normal operation (the view died)
    e.set_display(nullptr);
    const uint64_t frames_before = e.status().frames;
    e.run_sync(500);
    CHECK(e.status().frames == frames_before);
}

static void test_run_thread() {
    Emulator e;
    std::string err;
    CHECK(e.create(test_file("thread.img"), &err));
    CHECK(e.load_kernel(hakios::fixtures::kImg3Plain,
                        sizeof hakios::fixtures::kImg3Plain,
                        nullptr, nullptr, &err));
    MockBackend mock;
    e.set_display(&mock);
    CHECK(e.start(&err));
    CHECK(e.running());
    // the thread runs until the guest halts on the pattern "code"; give it
    // a generous deadline -- on a loaded CI box slices can be slow
    for (int i = 0; i < 200 && e.status().state == Emulator::State::kRunning; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    e.pause();
    const Emulator::Status s = e.status();
    CHECK(s.state == Emulator::State::kHalted);
    CHECK(s.executed > 0);
    CHECK(s.halt.reason != Arm7Cpu::Halt::kNone);
    CHECK(mock.uploads_ >= 1);               // frames went out while running

    // pause/resume keeps the machine intact
    CHECK(e.start(&err));                    // resets the CPU after the halt
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    e.pause();
    CHECK(e.status().state == Emulator::State::kPaused ||
          e.status().state == Emulator::State::kHalted);
}

int main() {
    test_lifecycle_and_halt();
    test_persistence();
    test_display_handoff();
    test_run_thread();
    if (g_failures == 0)
        std::printf("engine tests: OK (lifecycle, halt, persistence, display, thread)\n");
    else
        std::printf("engine tests: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
