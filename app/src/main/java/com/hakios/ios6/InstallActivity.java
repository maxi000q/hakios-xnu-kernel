package com.hakios.ios6;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.Locale;

/**
 * The "Installing iOS 6" screen -- stage 7's first face: every step real,
 * every step reported, nothing pretended. The install is
 *
 *   image create    the emulated iPhone's 2.5 GiB sparse storage image
 *   download        the kernel cache out of the 6.1.3 IPSW on Apple's own
 *                   servers, by HTTP range requests (7.4 MB of a 938 MB
 *                   archive), exactly what sysimage/ios6/fetch.sh does
 *   unpack          the member, inflated as it arrives (it is stored
 *                   deflated in the archive)
 *   kernel load     the stage-4 chain in the engine; the kernel cache is
 *                   AES-encrypted, so this is where a key is asked for --
 *                   or the bundled self-test kernel is offered
 *   first boot      the engine's run thread starts, this screen hands over
 *                   to the emulator screen
 *
 * The system volume DMG (~860 MB) is deliberately NOT downloaded by the
 * app: this release's emulator has no HFSX driver to consume it, and the
 * install screen does not spend the user's data plan to store a file
 * nothing reads. The note at the bottom says so.
 */
public class InstallActivity extends Activity {

    /** The IPSW of the emulated machine, pinned in stage 4 and verified
     *  against Apple's archive (sysimage/ios6/README.md). */
    private static final String IPSW_URL =
            "https://secure-appldnld.apple.com/iOS6.1/091-2610.20130319.Bedr4/"
                    + "iPhone3,1_6.1.3_10B329_Restore.ipsw";
    private static final long IPSW_SIZE = 938891219L;
    private static final String KERNEL_MEMBER = "kernelcache.release.n90";

    private ProgressBar progress;
    private TextView stepView;
    private TextView detailView;
    private TextView errorView;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile boolean cancelled = false;
    private Thread worker;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(buildUi());

        worker = new Thread(this::install, "hakios-install");
        worker.start();
    }

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(dp(32), dp(96), dp(32), dp(32));
        root.setBackgroundColor(Color.parseColor("#101014"));

        TextView title = new TextView(this);
        title.setText(R.string.install_title);
        title.setTextSize(28);
        title.setTextColor(Color.WHITE);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("HakiOS app edition · iOS 6.1.3 (10B329) · iPhone 4");
        subtitle.setTextSize(13);
        subtitle.setTextColor(Color.parseColor("#8A8A93"));
        subtitle.setGravity(Gravity.CENTER);
        subtitle.setPadding(0, dp(4), 0, dp(32));
        root.addView(subtitle);

        stepView = new TextView(this);
        stepView.setTextSize(16);
        stepView.setTextColor(Color.parseColor("#D8D8DE"));
        root.addView(stepView);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(10000);
        progress.setPadding(0, dp(8), 0, dp(8));
        root.addView(progress, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        detailView = new TextView(this);
        detailView.setTextSize(12);
        detailView.setTextColor(Color.parseColor("#8A8A93"));
        root.addView(detailView);

        errorView = new TextView(this);
        errorView.setTextSize(12);
        errorView.setTextColor(Color.parseColor("#E96A6A"));
        errorView.setPadding(0, dp(12), 0, 0);
        root.addView(errorView);

        TextView note = new TextView(this);
        note.setText(R.string.install_note);
        note.setTextSize(11);
        note.setTextColor(Color.parseColor("#6B6B75"));
        note.setPadding(0, dp(40), 0, 0);
        root.addView(note);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        return scroll;
    }

    // -- the install, one step at a time ------------------------------------------------

    private void install() {
        try {
            step(getString(R.string.install_step_image), -1);
            File image = new File(getFilesDir(), "guest.img");
            boolean created = Emulator.nativeCreate(image.getAbsolutePath());
            if (!created && !Emulator.nativeOpen(image.getAbsolutePath()))
                throw new IOException("the engine refused the guest image: "
                        + Emulator.status().error);

            step(getString(R.string.install_step_download), 0);
            File kernel = new File(getFilesDir(), KERNEL_MEMBER);
            if (!kernel.exists() || kernel.length() == 0) {
                IpswFetcher ipsw = new IpswFetcher(IPSW_URL, IPSW_SIZE);
                IpswFetcher.Member member = ipsw.find(KERNEL_MEMBER);
                detail(String.format(Locale.US,
                        "%s: %,.1f MB compressed, %,.1f MB as stored",
                        KERNEL_MEMBER, member.csize / 1048576.0, member.usize / 1048576.0));
                // download and unpack are one stream: the member is deflated
                // and inflated on the fly, so the byte progress is both steps
                long got = ipsw.download(member, kernel, bytes -> {
                    int promille = (int) Math.min(10000L, bytes * 10000 / Math.max(1, member.usize));
                    step(getString(R.string.install_step_unpack), promille);
                    detail(String.format(Locale.US, "%,.1f MB", bytes / 1048576.0));
                }, () -> cancelled);
                if (got <= 0) throw new IOException("the download was cancelled");
                if (got != member.usize)
                    throw new IOException(String.format(Locale.US,
                            "the kernel cache is %,d bytes, the archive promised %,d",
                            got, member.usize));
            }

            step(getString(R.string.install_step_load), -1);
            byte[] kernelBytes = readAll(kernel);
            String err = Emulator.nativeLoadKernel(kernelBytes, null, null);
            if (err != null && err.contains("encrypted")) {
                // Apple's kernel cache: the honest fork of this install
                ui.post(() -> askForKey(kernelBytes, err));
                return;                       // the dialog continues the install
            }
            if (err != null) throw new IOException(err);

            boot();
        } catch (final IOException e) {
            ui.post(() -> failed(e.getMessage()));
        }
    }

    private void boot() {
        step(getString(R.string.install_step_boot), 10000);
        if (!Emulator.nativeStart()) {
            failed("the engine would not start: " + Emulator.status().error);
            return;
        }
        ui.postDelayed(() -> {
            startActivity(new Intent(this, EmulatorScreen.class));
            finish();
        }, 600);                              // let the bar reach the right edge
    }

    /** The kernel is encrypted and no key was given: offer both honest paths. */
    private void askForKey(final byte[] kernelBytes, String why) {
        errorView.setText(why);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        final EditText iv = new EditText(this);
        iv.setHint(R.string.key_hint_iv);
        iv.setInputType(InputType.TYPE_CLASS_TEXT);
        final EditText key = new EditText(this);
        key.setHint(R.string.key_hint_key);
        key.setInputType(InputType.TYPE_CLASS_TEXT);
        box.addView(iv);
        box.addView(key);
        new AlertDialog.Builder(this)
                .setTitle(R.string.key_dialog_title)
                .setMessage(R.string.key_dialog_body)
                .setView(box)
                .setPositiveButton(R.string.key_action_ok, (d, w) -> {
                    step(getString(R.string.install_step_load), -1);
                    String e = Emulator.nativeLoadKernel(kernelBytes,
                            iv.getText().toString().trim(),
                            key.getText().toString().trim());
                    if (e != null) {
                        failed(e);
                        return;
                    }
                    boot();
                })
                .setNeutralButton(R.string.key_action_use, (d, w) -> bootSelfTestKernel())
                .setCancelable(false)
                .show();
    }

    /** The bundled self-test kernel: the whole machine without Apple code. */
    private void bootSelfTestKernel() {
        try {
            File demo = new File(getFilesDir(), "demo-kernel.img3");
            try (InputStream in = getAssets().open("demo-kernel.img3")) {
                Files.copy(in, demo.toPath(),
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
            step(getString(R.string.install_step_load), -1);
            String e = Emulator.nativeLoadKernel(readAll(demo), null, null);
            if (e != null) {
                failed(e);
                return;
            }
            boot();
        } catch (IOException ex) {
            failed("the bundled self-test kernel is missing: " + ex.getMessage());
        }
    }

    // -- little UI helpers -----------------------------------------------------------------

    private void step(final String text, final int promille) {
        ui.post(() -> {
            stepView.setText(text);
            progress.setIndeterminate(promille < 0);
            if (promille >= 0) progress.setProgress(promille);
        });
    }

    private void detail(final String text) {
        ui.post(() -> detailView.setText(text));
    }

    private void failed(final String message) {
        ui.post(() -> {
            errorView.setText(message == null ? "the install failed" : message);
            progress.setIndeterminate(false);
            progress.setProgress(0);
        });
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private static byte[] readAll(File f) throws IOException {
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] all = new byte[(int) f.length()];
            int have = 0;
            while (have < all.length) {
                int n = in.read(all, have, all.length - have);
                if (n < 0) break;
                have += n;
            }
            if (have != all.length)
                throw new IOException("short read on " + f.getName());
            return all;
        }
    }

    @Override
    protected void onDestroy() {
        cancelled = true;
        super.onDestroy();
    }
}
