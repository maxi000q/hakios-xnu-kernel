package com.hakios.ios6;

/**
 * The emulator core, through the stage-6 JNI bridge: one machine per
 * process, every failure a string the UI can show. The native library is
 * the same C++ the desktop rig compiles (native/, built by CMake through
 * app/build.gradle).
 *
 * The status line is one "key=value" record; parseStatus() turns it into
 * the fields the screens use, so the wire format stays in one place.
 */
public final class Emulator {

    static {
        System.loadLibrary("hakios_jni");
    }

    // -- the bridge ----------------------------------------------------------------

    public static native boolean nativeCreate(String path);
    public static native boolean nativeOpen(String path);

    /** null on success, the error the engine named on failure. */
    public static native String nativeLoadKernel(byte[] img, String ivHex, String keyHex);

    public static native boolean nativeStart();
    public static native void nativePause();
    public static native String nativeStatus();
    public static native boolean nativeSetSurface(Object surface);  // android.view.Surface
    public static native void nativeClose();

    // -- the parsed status ------------------------------------------------------------

    public static final class Status {
        public String state = "empty";
        public long executed;
        public long pc;
        public String halt = "none";
        public boolean cleanPoweroff;
        public long haltPc;
        public String detail = "";
        public long imageBytes;
        public long residentBytes;
        public long frames;
        public double mips;
        public int sliceSteps;
        public String error = "";

        public boolean running() {
            return "running".equals(state);
        }

        public boolean halted() {
            return "halted".equals(state);
        }
    }

    public static Status status() {
        Status s = new Status();
        for (String pair : nativeStatus().split(" ")) {
            int eq = pair.indexOf('=');
            if (eq <= 0) continue;
            String k = pair.substring(0, eq), v = pair.substring(eq + 1);
            try {
                switch (k) {
                    case "state": s.state = v; break;
                    case "executed": s.executed = Long.parseLong(v); break;
                    case "pc": s.pc = Long.parseLong(v.replace("0x", ""), 16); break;
                    case "halt": s.halt = v; break;
                    case "pc_halt": s.haltPc = Long.parseLong(v.replace("0x", ""), 16); break;
                    case "detail": s.detail = v; break;
                    case "image": s.imageBytes = Long.parseLong(v); break;
                    case "resident": s.residentBytes = Long.parseLong(v); break;
                    case "frames": s.frames = Long.parseLong(v); break;
                    case "mips": s.mips = Double.parseDouble(v); break;
                    case "slice": s.sliceSteps = Integer.parseInt(v); break;
                    case "error": s.error = v; break;
                }
            } catch (NumberFormatException ignored) {
                // a field the parser does not know must never crash the UI
            }
        }
        s.cleanPoweroff = s.halt.contains("power-off");
        return s;
    }

    /** "halted"(power-off) is how the native side spells a clean finish. */
    public static String describeHalt(Status s) {
        if (s.cleanPoweroff) return "clean power-off";
        if (!s.halted()) return s.state;
        return s.halt + (s.detail.isEmpty() ? "" : " (" + s.detail + ")");
    }

    private Emulator() {}
}
