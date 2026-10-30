package com.hakios.ios6;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;

/**
 * The emulator screen -- stage 7's second face: the guest's display through
 * the stage-5 renderer (the SurfaceView hands its surface to the engine,
 * which uploads the guest's RGB565 framebuffer at its own 30 fps pace), and
 * a status bar that says what the machine is actually doing: state,
 * instructions retired, measured MIPS, how much of the guest is resident.
 *
 * Honesty on screen: when the guest halts, the bar says why and where
 * (reason, PC, the clean power-off of the self-test kernel included), the
 * same words the engine reported -- never a generic "something went wrong".
 */
public class EmulatorScreen extends Activity implements SurfaceHolder.Callback {

    private TextView statusBar;
    private Button pauseButton;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            Emulator.Status s = Emulator.status();
            statusBar.setText(String.format(Locale.US,
                    getString(R.string.emulator_status),
                    s.state, s.executed, s.mips, humanBytes(s.residentBytes)));
            if (s.halted()) {
                statusBar.setText(String.format(Locale.US,
                        getString(R.string.emulator_halted),
                        Emulator.describeHalt(s), s.haltPc));
            }
            pauseButton.setText(s.running()
                    ? R.string.emulator_pause : R.string.emulator_resume);
            ui.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(buildUi());
        ui.post(poll);
    }

    private View buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.parseColor("#101014"));

        // the guest's display: 2:3, letterboxed by the renderer itself
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        LinearLayout bottom = new LinearLayout(this);
        bottom.setOrientation(LinearLayout.HORIZONTAL);
        bottom.setGravity(Gravity.CENTER_VERTICAL);
        bottom.setBackgroundColor(0xCC101014);
        bottom.setPadding(dp(12), dp(8), dp(12), dp(8));

        statusBar = new TextView(this);
        statusBar.setTextSize(12);
        statusBar.setTextColor(Color.parseColor("#C9C9D1"));
        bottom.addView(statusBar, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        pauseButton = new Button(this);
        pauseButton.setText(R.string.emulator_pause);
        pauseButton.setOnClickListener(v -> {
            Emulator.Status s = Emulator.status();
            if (s.running()) Emulator.nativePause();
            else Emulator.nativeStart();   // resumes, or restarts after a halt
        });
        bottom.addView(pauseButton);

        root.addView(bottom, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM));
        return root;
    }

    // -- the surface is the display -----------------------------------------------------

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Emulator.nativeSetSurface(holder.getSurface());
        if (!Emulator.status().running()) Emulator.nativeStart();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // the renderer re-acquires the surface on its own thread; rotations
        // and splits are normal life, not errors
        Emulator.nativeSetSurface(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Emulator.nativeSetSurface(null);
    }

    @Override
    protected void onPause() {
        super.onPause();
        Emulator.nativePause();     // the guest freezes; its storage keeps it
    }

    @Override
    protected void onResume() {
        super.onResume();
        // the surface callback restarts the machine when the view is back
    }

    @Override
    protected void onDestroy() {
        ui.removeCallbacks(poll);
        Emulator.nativeSetSurface(null);
        super.onDestroy();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private static String humanBytes(long b) {
        if (b >= 1024 * 1024) return String.format(Locale.US, "%.0f MB", b / 1048576.0);
        if (b >= 1024) return String.format(Locale.US, "%.0f kB", b / 1024.0);
        return b + " B";
    }
}
