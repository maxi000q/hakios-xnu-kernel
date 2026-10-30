#pragma once

// The emulator engine -- stage 6 of the app edition roadmap: lifecycle, the
// run thread with a step budget per frame, halt/panic reporting, and the
// surface the app drives it all through (the JNI bridge in native/jni).
//
// One Emulator owns the whole machine: the stage-2 storage image, the
// stage-3 interpreter, the stage-4 loader's result and the stage-5 display.
// The lifecycle the app sees:
//
//   create()/open()   the guest's backing image -- created on first install,
//                     opened afterwards: "installed stays installed", the
//                     image is a file, a process kill loses nothing
//   load_kernel()     stage-4 chain into guest RAM, entry PC set
//   start()           the run thread: slices of instructions, a frame out
//                     every 33 ms through the stage-5 renderer, until halt.
//                     Starting a halted guest resets the CPU and boots again
//                     ("run again"), so the thread's death after a halt is
//                     never mistaken for a machine that is still running
//   pause()           the thread joins, the guest freezes mid-instruction,
//                     start() continues it
//   run_sync()        the same slices on the caller's thread -- the desktop
//                     rig's deterministic path, no timing in the tests
//
// Halts are first-class, never a crash: an unknown instruction, a fetch or
// data fault, a supervisor call, Thumb -- each lands with the PC and the
// offending word, and the app's screen shows exactly what the engine knows.
// One SVC is special by contract: SVC #0x0FF is the demo kernel's clean
// power-off (stage 8), so "the kernel finished on purpose" is distinguishable
// from "the kernel hit something it should not have".

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "hakios/arm7_cpu.h"
#include "hakios/frame_renderer.h"
#include "hakios/kernel_loader.h"
#include "hakios/storage_memory.h"

namespace hakios {

class Emulator {
public:
    enum class State : uint8_t {
        kEmpty = 0,    // no backing image yet
        kReady,        // image open, no kernel placed
        kLoaded,       // kernel in guest RAM, entry set
        kRunning,      // the run thread is retiring instructions
        kPaused,       // was running; the thread is joined, state kept
        kHalted,       // the guest stopped itself or hit a fault
    };

    // SVC #0x0FF -- the stage-8 self-test kernel's clean power-off.
    static constexpr uint32_t kPoweroffSvc = 0x0FF;

    struct Halt {
        Arm7Cpu::Halt reason = Arm7Cpu::Halt::kNone;
        uint32_t pc = 0;
        uint32_t insn = 0;
        bool clean_poweroff = false;   // SVC #0x0FF
        char detail[96] = {0};
    };

    struct Status {
        State    state = State::kEmpty;
        uint64_t executed = 0;         // instructions retired this session
        uint32_t pc = 0;
        Halt     halt;
        size_t   image_bytes = 0;      // the whole 2.5 GiB sparse address space
        size_t   resident_bytes = 0;   // what is actually in RAM (mincore)
        uint64_t frames = 0;           // frames presented through the renderer
        double   mips = 0.0;           // over the most recent slice
        uint32_t slice_steps = 0;      // the current step budget per frame
    };

    // The default step budget: ~60-100 MIPS on an Adreno 619 big core means
    // 2-3 million instructions fit inside the 33 ms frame; 2M keeps the big
    // core at low clocks with headroom, and completing the install and the
    // first boot is the goal, not saturating anything.
    static constexpr uint32_t kDefaultSliceSteps = 2 * 1000 * 1000;

    Emulator() = default;
    ~Emulator();
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    // -- lifecycle -----------------------------------------------------------
    bool create(const std::string& backing_path, std::string* err);   // first install
    bool open(const std::string& backing_path, std::string* err);     // afterwards
    void close() noexcept;

    // Place a kernel image with the stage-4 chain. iv_hex/key_hex may be
    // null: then an encrypted kernel fails with the message that says so.
    bool load_kernel(const uint8_t* bytes, size_t len,
                     const char* iv_hex, const char* key_hex, std::string* err);
    bool kernel_loaded() const { return entry_pc_ != 0; }
    uint32_t entry_pc() const { return entry_pc_; }

    // -- the run thread --------------------------------------------------------
    bool start(std::string* err);      // fails without a kernel
    void pause() noexcept;             // join the thread, keep everything
    // True while a run thread is actually alive. A thread that already
    // finished (the guest halted) is still joinable, so joinable() alone
    // must never be read as "running" -- that mistake is why "run again"
    // used to do nothing after the first halt.
    bool running() const { return thread_alive_.load(); }

    // Synchronous slices for the desktop rig: up to max_steps instructions,
    // rendering through the display if one is attached. Returns steps run.
    uint64_t run_sync(uint64_t max_steps);

    // Zero the CPU registers (the guest memory is untouched) and clear the
    // halt, so a halted guest can be restarted at the entry PC.
    void reset_cpu();

    // -- display ----------------------------------------------------------------
    // The backend is borrowed (the JNI bridge owns the EGL one); rendering
    // happens on the thread that runs slices, which is where an EGL context
    // may be made current.
    void set_display(FrameRenderer::Backend* backend);
    uint64_t frames_presented() const;

    // -- introspection ------------------------------------------------------------
    Status status() const;
    const std::string& last_error() const { return last_error_; }
    StorageMemory& memory() { return mem_; }

private:
    void run_loop();                   // the thread body
    bool slice(uint64_t steps);        // one budget; false on halt
    void publish_halt();               // copy CPU halt info into halt_
    void ensure_display();             // init the renderer's backend, once per attach

    mutable std::mutex mu_;
    StorageMemory mem_;
    Arm7Cpu cpu_{mem_};
    FrameRenderer renderer_{mem_, nullptr};
    std::thread run_thread_;
    std::atomic<bool> stop_requested_{false};
    // Set while a run thread is executing; cleared as its last act. start()
    // reads it to tell "genuinely running" from "finished but not joined",
    // and joins the corpse before launching the next thread.
    std::atomic<bool> thread_alive_{false};
    mutable double mips_cache_ = 0.0;
    // The backend's GL state was initialized on the thread that runs slices
    // (an EGL context belongs to its thread). Atomic: set_display() clears
    // it from the app thread, ensure_display() raises it on the run thread.
    std::atomic<bool> display_ready_{false};

    State state_ = State::kEmpty;
    uint32_t entry_pc_ = 0;
    uint32_t slice_steps_ = kDefaultSliceSteps;
    uint64_t executed_total_ = 0;
    uint64_t slice_executed_ = 0;      // for the MIPS figure
    Halt halt_;
    std::string last_error_;
};

}  // namespace hakios
