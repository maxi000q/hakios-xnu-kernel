#include "hakios/emulator.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "hakios/guest_map.h"

namespace hakios {

Emulator::~Emulator() {
    pause();
}

bool Emulator::create(const std::string& backing_path, std::string* err) {
    pause();
    close();
    if (!mem_.create(backing_path, map::kPhysSize)) {
        if (err) *err = "engine: creating the guest image failed: " + mem_.last_error();
        return false;
    }
    cpu_.reset(0);
    state_ = State::kReady;
    return true;
}

bool Emulator::open(const std::string& backing_path, std::string* err) {
    pause();
    close();
    if (!mem_.open_existing(backing_path, map::kPhysSize)) {
        if (err) *err = "engine: opening the guest image failed: " + mem_.last_error();
        return false;
    }
    state_ = State::kReady;
    return true;
}

void Emulator::close() noexcept {
    mem_.close();
    state_ = State::kEmpty;
    entry_pc_ = 0;
    executed_total_ = 0;
    halt_ = Halt{};
}

bool Emulator::load_kernel(const uint8_t* bytes, size_t len,
                           const char* iv_hex, const char* key_hex, std::string* err) {
    if (state_ == State::kEmpty) {
        if (err) *err = "engine: no guest image to load a kernel into";
        return false;
    }
    pause();
    KernelLoader::Key key;
    KernelLoader::Key* key_ptr = nullptr;
    const auto hex = [](const char* s, uint8_t* out, size_t n) {
        if (!s) return false;
        const size_t sl = std::strlen(s);
        if (sl != 2 * n) return false;
        for (size_t i = 0; i < n; i++) {
            unsigned v = 0;
            if (std::sscanf(s + 2 * i, "%2x", &v) != 1) return false;
            out[i] = uint8_t(v);
        }
        return true;
    };
    bool have_key = false;
    if (iv_hex && key_hex) {
        have_key = hex(iv_hex, key.iv, sizeof key.iv) && hex(key_hex, key.key, sizeof key.key);
        if (!have_key) {
            if (err) *err = "engine: the key and IV must each be 64/32 hex characters";
            return false;
        }
    }
    if (have_key) key_ptr = &key;

    KernelLoadReport report;
    std::string load_err;
    if (!KernelLoader::load(mem_, bytes, len, key_ptr, &report, &load_err)) {
        if (err) *err = "engine: " + load_err;
        return false;
    }
    entry_pc_ = report.entry_pc;
    reset_cpu();
    state_ = State::kLoaded;
    if (err) err->clear();
    return true;
}

void Emulator::reset_cpu() {
    cpu_.reset(entry_pc_);
    halt_ = Halt{};
    // Arm7Cpu::reset() zeroes info_.executed with the rest of Info; the
    // slice bookkeeping must follow it, or the first slice of a restarted
    // guest computes "0 - old_total" and the retired count wraps around.
    slice_executed_ = 0;
}

bool Emulator::start(std::string* err) {
    if (state_ == State::kEmpty) {
        if (err) *err = "engine: no guest image";
        return false;
    }
    if (!kernel_loaded()) {
        if (err) *err = "engine: no kernel loaded -- run the install first";
        return false;
    }
    if (thread_alive_.load()) return true;   // genuinely running
    // A thread that ran to a halt has finished but was never joined: it is
    // still joinable, and the old code read exactly that as "already
    // running" -- which is why "run again" after the first halt did
    // nothing. Join the corpse, then decide.
    if (run_thread_.joinable()) run_thread_.join();
    if (state_ == State::kHalted) reset_cpu();    // a restart after a halt
    stop_requested_ = false;
    state_ = State::kRunning;
    thread_alive_ = true;
    run_thread_ = std::thread([this] { run_loop(); });
    return true;
}

void Emulator::pause() noexcept {
    stop_requested_ = true;
    if (run_thread_.joinable()) run_thread_.join();
    if (state_ == State::kRunning) state_ = State::kPaused;
}

void Emulator::ensure_display() {
    if (display_ready_.load()) return;
    // No backend yet is normal life (the install screen's first boot starts
    // before the emulator screen exists): rendering is simply off, and the
    // next pass -- the run thread checks every slice -- picks the display
    // up when the app attaches one.
    if (!renderer_.has_backend()) return;
    std::string err;
    if (renderer_.init(&err)) {
        display_ready_ = true;
    } else {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = err;             // rendering is disabled, the run goes on
    }
}

void Emulator::run_loop() {
    // The frame clock is real time: slices of instructions, a frame out at
    // most every 33 ms, and the thread idles (never spins) once the guest
    // halts or a pause is requested.
    const auto start = std::chrono::steady_clock::now();
    int64_t now_ms = 0;
    while (!stop_requested_.load()) {
        ensure_display();   // cheap when ready; picks up a mid-run attach
        if (!slice(slice_steps_)) break;          // halt: report and stand down
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start).count();
        renderer_.render(now_ms);                 // the renderer paces itself
    }
    // A last frame so a halt is visible immediately, not at the next 33 ms.
    // The frame is drawn even when the loop left through a pause request:
    // pausing mid-paint must not freeze a half-updated texture.
    ensure_display();
    renderer_.render(now_ms + FrameRenderer::kFrameIntervalMs);
    // The EGL context was made current on THIS thread; it must be given up
    // here, before the thread dies -- a context left current on a dead
    // thread is undefined behavior on more than one Android GL driver, and
    // the next run thread ("run again") builds its own anyway.
    renderer_.release_on_thread();
    display_ready_ = false;
    // Last act: start() may now see the machine as restartable and join us.
    thread_alive_ = false;
}

bool Emulator::slice(uint64_t steps) {
    const auto t0 = std::chrono::steady_clock::now();
    const Arm7Cpu::Halt h = cpu_.run(steps);
    const uint64_t did = cpu_.info().executed - slice_executed_;
    slice_executed_ = cpu_.info().executed;
    executed_total_ += did;
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard<std::mutex> lock(mu_);
        mips_cache_ = secs > 0 ? double(did) / secs / 1e6 : 0.0;
    }
    if (h != Arm7Cpu::Halt::kNone) {
        publish_halt();
        return false;
    }
    return true;
}

void Emulator::publish_halt() {
    std::lock_guard<std::mutex> lock(mu_);
    halt_.reason = cpu_.info().halt;
    halt_.pc = cpu_.info().halt_pc;
    halt_.insn = cpu_.info().halt_insn;
    halt_.clean_poweroff =
        cpu_.info().halt == Arm7Cpu::Halt::kSupervisorCall &&
        (cpu_.info().halt_insn & 0x00FFFFFFu) == kPoweroffSvc;
    std::memcpy(halt_.detail, cpu_.info().detail, sizeof halt_.detail - 1);
    halt_.detail[sizeof halt_.detail - 1] = 0;
    state_ = State::kHalted;
}

uint64_t Emulator::run_sync(uint64_t max_steps) {
    // The deterministic path the desktop rig uses: no thread, no clock in
    // the assertions; the display, when attached, still sees every frame.
    ensure_display();
    uint64_t before = cpu_.info().executed;
    if (state_ == State::kHalted) return 0;
    slice(max_steps);
    renderer_.render(FrameRenderer::kFrameIntervalMs * (frames_presented() + 1));
    return cpu_.info().executed - before;
}

void Emulator::set_display(FrameRenderer::Backend* backend) {
    std::lock_guard<std::mutex> lock(mu_);
    renderer_.set_backend(backend);
    display_ready_ = false;            // init again on the next rendering thread
}

uint64_t Emulator::frames_presented() const {
    std::lock_guard<std::mutex> lock(mu_);
    return renderer_.frames_presented();
}

Emulator::Status Emulator::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    Status s;
    s.state = state_;
    s.executed = executed_total_;
    s.pc = cpu_.pc();
    s.halt = halt_;
    const StorageMemory::Stats st = mem_.stats();
    s.image_bytes = st.size_bytes;
    s.resident_bytes = st.resident_bytes;
    s.frames = renderer_.frames_presented();
    s.mips = mips_cache_;
    s.slice_steps = slice_steps_;
    return s;
}

}  // namespace hakios
