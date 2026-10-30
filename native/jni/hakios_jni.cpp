// The JNI bridge -- stage 6: the thin line between the app (stage 7) and
// the emulator core (stages 2-5). One Emulator per process, owned here;
// every call is bounded, every failure a string the app can show.
//
// The Java side (app/src/.../Emulator.java) declares:
//
//   nativeCreate(String path)                      create the guest image
//   nativeOpen(String path)                        open an existing one
//   nativeLoadKernel(byte[] img, String iv, String key)   stage-4 chain
//   nativeStart() / nativePause()                  the run thread
//   nativeStatus()                                 one line, "key=value" pairs
//   nativeSetSurface(Surface s)                    the display, or null
//   nativeClose()
//
// iv/key are the hex strings of the kernel cache's own AES key and IV, typed
// by the user during install from their own research; either both or neither.

#ifdef __ANDROID__

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstring>
#include <string>

#include "hakios/egl_backend.h"
#include "hakios/emulator.h"

namespace {

hakios::Emulator& engine() {
    static hakios::Emulator e;   // one machine per process, by design
    return e;
}

hakios::EglBackend* display = nullptr;   // owned here, borrowed by the engine

std::string to_string(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return out;
}

const char* state_name(hakios::Emulator::State s) {
    switch (s) {
        case hakios::Emulator::State::kEmpty:   return "empty";
        case hakios::Emulator::State::kReady:   return "ready";
        case hakios::Emulator::State::kLoaded:  return "loaded";
        case hakios::Emulator::State::kRunning: return "running";
        case hakios::Emulator::State::kPaused:  return "paused";
        case hakios::Emulator::State::kHalted:  return "halted";
    }
    return "?";
}

}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_hakios_ios6_Emulator_nativeCreate(JNIEnv* env, jobject, jstring path) {
    std::string err;
    const bool ok = engine().create(to_string(env, path), &err);
    if (!ok) engine().memory();   // keep last_error() alive for nativeStatus
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_hakios_ios6_Emulator_nativeOpen(JNIEnv* env, jobject, jstring path) {
    std::string err;
    return engine().open(to_string(env, path), &err) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_hakios_ios6_Emulator_nativeLoadKernel(JNIEnv* env, jobject,
                                               jbyteArray img, jstring iv, jstring key) {
    const jsize len = env->GetArrayLength(img);
    std::string err;
    jboolean is_copy = JNI_FALSE;
    jbyte* bytes = env->GetByteArrayElements(img, &is_copy);
    const std::string iv_hex = to_string(env, iv);
    const std::string key_hex = to_string(env, key);
    const bool ok = engine().load_kernel(
        reinterpret_cast<const uint8_t*>(bytes), size_t(len),
        iv_hex.empty() ? nullptr : iv_hex.c_str(),
        key_hex.empty() ? nullptr : key_hex.c_str(), &err);
    env->ReleaseByteArrayElements(img, bytes, JNI_ABORT);
    if (ok) return nullptr;
    return env->NewStringUTF(err.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_hakios_ios6_Emulator_nativeStart(JNIEnv* env, jobject) {
    std::string err;
    return engine().start(&err) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_hakios_ios6_Emulator_nativePause(JNIEnv*, jobject) {
    engine().pause();
}

JNIEXPORT jstring JNICALL
Java_com_hakios_ios6_Emulator_nativeStatus(JNIEnv* env, jobject) {
    const hakios::Emulator::Status s = engine().status();
    char line[512];
    std::snprintf(line, sizeof line,
                  "state=%s executed=%llu pc=0x%08x halt=%s%s pc_halt=0x%08x "
                  "detail=%.60s image=%zu resident=%zu frames=%llu mips=%.1f "
                  "slice=%u error=%.120s",
                  state_name(s.state),
                  (unsigned long long)s.executed, s.pc,
                  hakios::Arm7Cpu::halt_name(s.halt.reason),
                  s.halt.clean_poweroff ? "(power-off)" : "",
                  s.halt.pc,
                  s.halt.detail,
                  s.image_bytes, s.resident_bytes,
                  (unsigned long long)s.frames, s.mips, s.slice_steps,
                  engine().last_error().c_str());
    return env->NewStringUTF(line);
}

JNIEXPORT jboolean JNICALL
Java_com_hakios_ios6_Emulator_nativeSetSurface(JNIEnv* env, jobject, jobject surface) {
    // Swapping the display under a live run thread is a use-after-free: the
    // thread would render through a backend the UI thread is deleting. So the
    // machine is parked first (the join also lets the run thread give up its
    // EGL context on its own thread), and a machine that was running is
    // started again on the new surface -- a new thread with its own context.
    const bool was_running = engine().running() &&
                             engine().status().state == hakios::Emulator::State::kRunning;
    engine().pause();
    delete display;
    display = nullptr;
    if (!surface) {                       // the view went away: stop drawing
        engine().set_display(nullptr);
        return JNI_TRUE;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return JNI_FALSE;
    display = new (std::nothrow) hakios::EglBackend(window);
    if (!display) {
        ANativeWindow_release(window);
        return JNI_FALSE;
    }
    engine().set_display(display);        // owns the window reference from here
    if (was_running) engine().start(nullptr);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_hakios_ios6_Emulator_nativeClose(JNIEnv*, jobject) {
    engine().pause();
    engine().set_display(nullptr);
    delete display;
    display = nullptr;
    engine().close();
}

}  // extern "C"

#endif  // __ANDROID__
