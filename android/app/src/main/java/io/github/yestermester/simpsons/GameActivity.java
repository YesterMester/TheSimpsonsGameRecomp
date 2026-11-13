package io.github.yestermester.simpsons;

import android.hardware.input.InputManager;
import android.os.Bundle;
import android.content.Intent;
import android.view.InputDevice;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.RelativeLayout;
import org.libsdl.app.SDLActivity;
import java.io.File;
import java.util.ArrayList;
import org.json.JSONObject;

public final class GameActivity extends SDLActivity implements InputManager.InputDeviceListener {
    static volatile boolean active;
    private boolean closing;
    private TouchControlsView controls;
    private InputManager inputManager;
    private android.window.OnBackInvokedCallback back;

    static native void nativeTouchState(int buttons, int lt, int rt, int lx, int ly, int rx, int ry);
    static native boolean nativeMenuActive();
    static native int[] nativeShaderPreparation();
    private android.widget.LinearLayout shaderPreparation;
    private android.widget.TextView shaderStatus;
    private android.widget.ProgressBar shaderProgress;
    // Leaving the app pauses the game as the Xbox Guide does (XN_SYS_UI).
    static native void nativeSetBackground(boolean background);
    // Grows while the game decodes a story video.
    static native int nativeVideoBlocks();
    // Caps the frame rate (0 = none), for Android's battery game mode.
    static native void nativeSetFrameRateCap(int fps);
    private boolean menuActive, videoActive;
    private int lastVideoBlocks;
    private long videoQuietSince, revealUntil, nextGameModeCheck;
    private int frameRateCap = -1;
    private final android.os.Handler menuHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable menuCheck = new Runnable() {
        @Override public void run() {
            updateShaderPreparation();
            long now = android.os.SystemClock.uptimeMillis();
            boolean activeMenu = nativeMenuActive();
            // A video plays while its decoder keeps working; a short gap
            // (keyframes, pauses) doesn't bring the pad back.
            int blocks = nativeVideoBlocks();
            if (blocks != lastVideoBlocks) { lastVideoBlocks = blocks; videoQuietSince = now; }
            boolean activeVideo = now - videoQuietSince < 600;
            boolean revealing = now < revealUntil;
            if (activeMenu != menuActive || activeVideo != videoActive || revealing != revealed) {
                menuActive = activeMenu;
                videoActive = activeVideo;
                revealed = revealing;
                updateControllerVisibility();
            }
            if (now >= nextGameModeCheck) {
                nextGameModeCheck = now + 2000;
                applyGameMode();
            }
            menuHandler.postDelayed(this, 100);
        }
    };
    private boolean revealed;

    @Override protected String[] getArguments() {
        ArrayList<String> args = new ArrayList<>();
        args.add("--game_data_root=" + new File(getExternalFilesDir(null), "game").getAbsolutePath());
        String userData = getIntent().getStringExtra("user_data_root");
        args.add("--user_data_root=" + (userData == null ? new File(getFilesDir(), "simpsons").getAbsolutePath() : userData));
        args.add("--mods_root=" + new File(getFilesDir(), "simpsons/mods").getAbsolutePath());
        String modProfile = getIntent().getStringExtra("mods_profile_file");
        if (modProfile != null) {
            args.add("--mods_profile_file=" + modProfile);
            args.add("--mods_developer=true");
        }
        args.add("--cache_path=" + getCacheDir().getAbsolutePath());
        args.add("--aot_shader_path=" + new File(getFilesDir(), "native_shaders").getAbsolutePath());
        args.add("--shader_precompile_recipe_path=" +
                new File(getFilesDir(), "native_shaders/warmup-vertex-patches.txt").getAbsolutePath());
        args.add("--gpu=vulkan");
        return args.toArray(new String[0]);
    }

    @Override protected void onCreate(Bundle savedInstanceState) {
        active = true;
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) return;
        mLayout.setMotionEventSplittingEnabled(true);
        createControls();
        showShaderPreparation();
        mSurface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) { requestFrameRate(holder.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { requestFrameRate(holder.getSurface()); }
            @Override public void surfaceDestroyed(SurfaceHolder holder) { }
        });
        // The gamepad view owns each touch independently; touches outside its
        // controls continue to SDL, including mouse use in the runtime overlays.
        inputManager = (InputManager) getSystemService(INPUT_SERVICE);
        inputManager.registerInputDeviceListener(this, null);
        updateControllerVisibility();
        hideSystemBars();
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            back = this::closeGame;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, back);
        }
    }
    // The pad's gear opens the layout editor, which can also switch schemes, so
    // the view is rebuilt for whatever was chosen when the game is back.
    private void createControls() {
        if (controls != null) { controls.reset(); mLayout.removeView(controls); }
        controls = new TouchControlsView(this);
        controls.setListener(this::openControlEditor);
        mLayout.addView(controls, new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }
    private void openControlEditor() {
        if (controls != null) controls.reset();
        startActivity(new Intent(this, TouchEditorActivity.class)
                .putExtra("preview_width", mSurface.getWidth()).putExtra("preview_height", mSurface.getHeight()));
    }
    // Android's game modes (the Game Dashboard's optimization choice): Battery
    // holds the game at 30 FPS, Standard and Performance use the configured
    // rate. The game reports its state so Android can schedule for gameplay.
    private void applyGameMode() {
        if (mBrokenLibraries || android.os.Build.VERSION.SDK_INT < 31) return;
        android.app.GameManager games = getSystemService(android.app.GameManager.class);
        if (games == null) return;
        int cap = games.getGameMode() == android.app.GameManager.GAME_MODE_BATTERY ? 30 : 0;
        if (cap != frameRateCap) {
            try {
                nativeSetFrameRateCap(cap);
                frameRateCap = cap;
            } catch (UnsatisfiedLinkError error) { return; }
        }
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            games.setGameState(new android.app.GameState(shaderPreparation != null,
                    menuActive || videoActive ? android.app.GameState.MODE_CONTENT
                                              : android.app.GameState.MODE_GAMEPLAY_INTERRUPTIBLE));
        }
    }
    @Override public boolean dispatchTouchEvent(android.view.MotionEvent event) {
        if (videoActive && event.getActionMasked() == android.view.MotionEvent.ACTION_DOWN) {
            revealUntil = android.os.SystemClock.uptimeMillis() + 4000;
            if (!revealed) { revealed = true; updateControllerVisibility(); }
        }
        return super.dispatchTouchEvent(event);
    }
    private void setBackground(boolean background) {
        if (mBrokenLibraries) return;
        try { nativeSetBackground(background); }
        catch (UnsatisfiedLinkError error) { android.util.Log.w("Game", "The game library has no background hook", error); }
    }
    private void requestFrameRate(Surface surface) {
        if (android.os.Build.VERSION.SDK_INT < 30 || !surface.isValid()) return;
        try {
            JSONObject settings = new JSONObject(getSharedPreferences("settings", MODE_PRIVATE).getString("launcher_values", "{}"));
            int rate = settings.optInt("frame_rate", 60);
            // Menus at 30 also fit a 60 Hz display. Let Android choose a
            // compatible refresh mode without forcing a resolution change.
            surface.setFrameRate(Math.max(0, rate), Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        } catch (Exception error) { android.util.Log.w("Game", "Could not request the display frame rate", error); }
    }

    private void hideSystemBars() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN |
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    private void updateControllerVisibility() {
        if (controls == null) return;
        boolean connected = false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && !device.isVirtual() &&
                    ((device.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                     (device.getSources() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)) {
                connected = true;
                break;
            }
        }
        boolean autoHide = getSharedPreferences("settings", MODE_PRIVATE).getBoolean("hide_with_controller", true);
        controls.reset();
        boolean off = TouchLayoutStore.OFF.equals(TouchLayoutStore.activeScheme(this));
        // Videos play without the pad; a tap shows it for a moment (to pause
        // or skip with the game's own buttons).
        boolean hide = shaderPreparation != null || off || menuActive ||
                connected && autoHide || videoActive && !revealed;
        controls.setVisibility(hide ? View.GONE : View.VISIBLE);
    }

    private void showShaderPreparation() {
        shaderPreparation = new android.widget.LinearLayout(this);
        shaderPreparation.setOrientation(android.widget.LinearLayout.VERTICAL);
        shaderPreparation.setGravity(android.view.Gravity.CENTER);
        shaderPreparation.setBackgroundColor(android.graphics.Color.rgb(10, 17, 39));
        int padding = (int) (32 * getResources().getDisplayMetrics().density);
        shaderPreparation.setPadding(padding, padding, padding, padding);
        shaderPreparation.setClickable(true);
        shaderStatus = new android.widget.TextView(this);
        shaderStatus.setTextColor(android.graphics.Color.WHITE);
        shaderStatus.setTextSize(20);
        shaderStatus.setGravity(android.view.Gravity.CENTER);
        shaderStatus.setText("Starting the game renderer…");
        shaderPreparation.addView(shaderStatus, new android.widget.LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        shaderProgress = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        shaderProgress.setIndeterminate(true);
        android.content.res.ColorStateList yellow = android.content.res.ColorStateList.valueOf(
                android.graphics.Color.rgb(255, 222, 0));
        shaderProgress.setProgressTintList(yellow);
        shaderProgress.setIndeterminateTintList(yellow);
        android.widget.LinearLayout.LayoutParams bar = new android.widget.LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        bar.topMargin = padding;
        shaderPreparation.addView(shaderProgress, bar);
        mLayout.addView(shaderPreparation, new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        updateControllerVisibility();
    }

    private void updateShaderPreparation() {
        if (shaderPreparation == null) return;
        int[] preparation = nativeShaderPreparation();
        if (preparation == null || preparation.length != 3) return;
        int done = preparation[0], total = preparation[1], phase = preparation[2];
        if (phase >= 4) {
            mLayout.removeView(shaderPreparation);
            shaderPreparation = null;
            shaderStatus = null;
            shaderProgress = null;
            updateControllerVisibility();
            return;
        }
        String action = phase == 3 ? "Preparing graphics pipelines" :
                phase == 2 ? "Preparing shaders for your phone" : "Reading shader programs";
        shaderStatus.setText(action + (total > 0 ? "\n" + done + " / " + total : "…") +
                "\nCompatible results are saved for the next launch.");
        shaderProgress.setIndeterminate(total == 0);
        if (total > 0) {
            shaderProgress.setMax(total);
            shaderProgress.setProgress(Math.min(done, total));
        }
    }

    @Override public void onInputDeviceAdded(int id) { updateControllerVisibility(); }
    @Override public void onInputDeviceRemoved(int id) { updateControllerVisibility(); }
    @Override public void onInputDeviceChanged(int id) { updateControllerVisibility(); }
    @Override protected void onPause() {
        menuHandler.removeCallbacks(menuCheck);
        if (controls != null) controls.reset();
        if (!closing) setBackground(true);
        super.onPause();
    }
    @Override protected void onResume() {
        super.onResume();
        menuHandler.removeCallbacks(menuCheck);
        if (!mBrokenLibraries && controls != null) menuHandler.post(menuCheck);
        if (controls != null) { createControls(); updateControllerVisibility(); }
        if (mSurface != null) requestFrameRate(mSurface.getHolder().getSurface());
        setBackground(false);
        nextGameModeCheck = 0;
    }
    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
        else if (controls != null) controls.reset();
    }
    // Android 13 and newer use the dispatcher registered above.
    @android.annotation.SuppressLint("GestureBackNavigation")
    @Override public void onBackPressed() { closeGame(); }
    private void closeGame() {
        if (mBrokenLibraries || mSDLThread == null) { finish(); return; }
        if (closing) return;
        closing = true;
        if (controls != null) controls.reset();
        Thread nativeThread = mSDLThread;
        nativeSendQuit();
        new Thread(() -> {
            try { nativeThread.join(12000); }
            catch (InterruptedException error) { Thread.currentThread().interrupt(); }
            runOnUiThread(this::finish);
        }, "Game shutdown").start();
    }
    @Override protected void onDestroy() {
        menuHandler.removeCallbacks(menuCheck);
        if (back != null && android.os.Build.VERSION.SDK_INT >= 33) getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(back);
        if (controls != null) controls.reset();
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        Thread nativeThread = mSDLThread;
        super.onDestroy();
        // SDL and the runtime own process-wide state. Wait for native cleanup
        // before ending this session; the launcher watches the service's death.
        new Thread(() -> {
            if (nativeThread != null) {
                try { nativeThread.join(12000); }
                catch (InterruptedException error) { Thread.currentThread().interrupt(); }
            }
            android.os.Process.killProcess(android.os.Process.myPid());
        }, "Game process shutdown").start();
    }
}
