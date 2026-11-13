package io.github.yestermester.simpsons;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.ComponentName;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.webkit.JavascriptInterface;
import android.webkit.JsResult;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;
import org.json.JSONArray;
import org.json.JSONObject;

/** Hosts the shared launcher; Android owns file access and activity lifecycle. */
public final class LauncherActivity extends Activity {
    private static final int PICK_GAME = 1, EXPORT_SUPPORT = 2, PLAY_GAME = 3, NOTIFICATIONS = 4, PICK_ISO = 5, PICK_MOD = 6;
    private static final String ORIGIN = "https://launcher.simpsons.local";
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private SharedPreferences preferences;
    private WebView web;
    private JSONObject schema, values;
    private GamePatches gamePatches;
    private AppUpdater updater;
    private LauncherArt art;
    private ModPackages modPackages;
    private LinearLayout loading;
    private TextView loadingMessage;
    private ProgressBar loadingProgress;
    private boolean pageLoaded, startupFinished;
    private volatile boolean busy = true, prepared, engineReady;
    private volatile boolean gameRunning = true;
    private boolean gameBound, startGameWhenConnected;
    private volatile String launchUserData;
    private volatile String launchModProfile;
    private volatile boolean modsPreparing;
    private IBinder gameSession;
    private final ServiceConnection gameConnection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder session) {
            gameSession = session;
            boolean start = startGameWhenConnected;
            startGameWhenConnected = false;
            try {
                boolean state = sessionRequest(start ? GameSessionService.CLAIM : GameSessionService.STATUS);
                gameRunning = start || state;
                if (!gameRunning) { disconnectGameSession(); return; }
                if (start && state) startActivityForResult(new Intent(LauncherActivity.this, GameActivity.class)
                        .putExtra("user_data_root", launchUserData).putExtra("mods_profile_file", launchModProfile), PLAY_GAME);
            } catch (Exception error) {
                if (start) {
                    try { sessionRequest(GameSessionService.CANCEL); } catch (Exception ignored) { }
                    new AlertDialog.Builder(LauncherActivity.this).setMessage("Could not start the game: " + error.getMessage())
                            .setPositiveButton("OK", null).show();
                }
                gameRunning = false;
                disconnectGameSession();
            }
        }
        @Override public void onServiceDisconnected(ComponentName name) { sessionEnded(); }
        @Override public void onBindingDied(ComponentName name) { sessionEnded(); }
        @Override public void onNullBinding(ComponentName name) { sessionEnded(); }
        private void sessionEnded() {
            gameRunning = false;
            disconnectGameSession();
        }
    };
    private volatile Boolean installResult;
    private volatile String workMessage = "Preparing the game…";
    private volatile int startupPercent;
    private Uri pendingImport;
    private boolean pendingImportIso;

    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null && savedInstanceState.containsKey("pending_import")) {
            pendingImport = Uri.parse(savedInstanceState.getString("pending_import"));
            pendingImportIso = savedInstanceState.getBoolean("pending_import_iso");
        }
        preferences = getSharedPreferences("settings", MODE_PRIVATE);
        updater = new AppUpdater(this);
        art = new LauncherArt(this, gameDir(), new File(getFilesDir(), "art"));
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xff17213d);
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets safe = insets.getInsets(android.view.WindowInsets.Type.systemBars()
                        | android.view.WindowInsets.Type.displayCutout());
                view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
                return android.view.WindowInsets.CONSUMED;
            } else {
                view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                        insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            }
            return insets.consumeSystemWindowInsets().consumeDisplayCutout();
        });
        web = new WebView(this);
        web.setBackgroundColor(0xff19202d);
        web.getSettings().setJavaScriptEnabled(true);
        web.getSettings().setDomStorageEnabled(true);
        web.getSettings().setAllowFileAccess(false);
        web.getSettings().setAllowContentAccess(false);
        web.getSettings().setSupportMultipleWindows(false);
        web.addJavascriptInterface(new Bridge(), "AndroidLauncher");
        web.setWebViewClient(new WebViewClient() {
            @Override public void onPageFinished(WebView view, String url) {
                pageLoaded = true;
                showLauncherWhenReady();
            }
            @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                Uri uri = request.getUrl();
                if (!isLocal(uri)) return emptyResponse(403, "Forbidden");
                String path = uri.getPath();
                if (path == null || path.equals("/")) path = "/index.html";
                if (path.contains("..") || path.contains("\\") || !request.getMethod().equals("GET")) {
                    return emptyResponse(403, "Forbidden");
                }
                if (path.startsWith("/art/")) {
                    File picture = art.picture(path.substring(5));
                    if (picture == null) return emptyResponse(404, "Not Found");
                    try { return new WebResourceResponse("image/jpeg", null, new java.io.FileInputStream(picture)); }
                    catch (IOException error) { return emptyResponse(404, "Not Found"); }
                }
                String mime = path.endsWith(".html") ? "text/html" : path.endsWith(".ttf") ? "font/ttf" : path.endsWith(".js") ? "text/javascript" : "application/octet-stream";
                try {
                    InputStream input = getAssets().open("launcher" + path);
                    Map<String, String> headers = new LinkedHashMap<>();
                    headers.put("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self'; frame-src 'none'; object-src 'none'");
                    return new WebResourceResponse(mime, "UTF-8", 200, "OK", headers, input);
                } catch (IOException error) { return emptyResponse(404, "Not Found"); }
            }
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                if (isLocal(request.getUrl())) return false;
                if (request.isForMainFrame()) openExternal(request.getUrl());
                return true;
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override public boolean onJsAlert(WebView view, String url, String message, JsResult result) {
                new AlertDialog.Builder(LauncherActivity.this).setMessage(message)
                        .setPositiveButton("OK", (dialog, which) -> result.confirm())
                        .setOnCancelListener(dialog -> result.cancel()).show();
                return true;
            }
            @Override public boolean onJsConfirm(WebView view, String url, String message, JsResult result) {
                new AlertDialog.Builder(LauncherActivity.this).setMessage(message)
                        .setPositiveButton("Continue", (dialog, which) -> result.confirm())
                        .setNegativeButton("Cancel", (dialog, which) -> result.cancel())
                        .setOnCancelListener(dialog -> result.cancel()).show();
                return true;
            }
        });
        root.addView(web, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        loading = new LinearLayout(this);
        loading.setOrientation(LinearLayout.VERTICAL);
        loading.setGravity(Gravity.CENTER);
        loading.setPadding(dp(28), dp(24), dp(28), dp(24));
        loading.setBackgroundColor(0xff17213d);
        ImageView donut = new ImageView(this);
        donut.setImageResource(R.drawable.ic_donut);
        loading.addView(donut, new LinearLayout.LayoutParams(dp(100), dp(100)));
        TextView title = new TextView(this);
        title.setText("The Simpsons Game Recompiled"); title.setTextColor(0xffffd90f);
        title.setTextSize(22); title.setGravity(Gravity.CENTER);
        loading.addView(title, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        loadingMessage = new TextView(this);
        loadingMessage.setText("Opening the launcher…"); loadingMessage.setTextColor(0xfff6f2e6);
        loadingMessage.setTextSize(16); loadingMessage.setGravity(Gravity.CENTER); loadingMessage.setPadding(0, dp(20), 0, dp(12));
        loading.addView(loadingMessage);
        loadingProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        loadingProgress.setMax(100); loadingProgress.setIndeterminate(false);
        loading.addView(loadingProgress, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(12)));
        root.addView(loading, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);
        connectGameSession(false);
        worker.execute(() -> {
            try {
                startupProgress("Loading your settings", 2);
                schema = new JSONObject(readAsset("launcher-settings.json"));
                values = new JSONObject(schema.getJSONObject("values").toString());
                JSONObject saved = new JSONObject(preferences.getString("launcher_values", "{}"));
                for (Iterator<String> keys = values.keys(); keys.hasNext();) {
                    String key = keys.next();
                    if (saved.has(key)) values.put(key, saved.get(key));
                }
                values.put("gpu", "vulkan").put("fullscreen", true).put("resolution", "").put("mnk_mode", false);
                if (!saved.has("frame_rate") && isLowEndDevice()) {
                    // A steady 30 plays better than an uneven 40-something.
                    values.put("frame_rate", 30);
                }
                gamePatches = new GamePatches(gameDir(), new JSONObject(readAsset("game-patches.json")));
                runOnUiThread(() -> { if (!isDestroyed()) web.loadUrl(ORIGIN +
                        (getIntent().getBooleanExtra("show_mods", false) ? "/#mods" :
                         getIntent().getBooleanExtra("show_install", false) ? "/#install" : "/")); });
                try (ZipFile apk = new ZipFile(getApplicationInfo().sourceDir)) {
                    engineReady = apk.getEntry("lib/arm64-v8a/libmain.so") != null;
                }
                if (android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE) != 4096) {
                    throw new IOException("This development build requires Android with 4 KB memory pages. 16 KB runtime support is not ready yet.");
                }
                startupProgress("Checking your game files", 5);
                if (!GameImportService.isRunning()) recoverGameImport();
                File previousSaves = new File(getFilesDir(), "saves.previous");
                File saves = new File(getFilesDir(), "simpsons");
                if (!saves.exists() && previousSaves.exists() && !previousSaves.renameTo(saves)) {
                    throw new IOException("Could not recover the previous saves");
                }
                prepareAssets();
                writeSettings();
                startupProgress("Ready", 100);
                prepared = true;
                finishWork(null);
                updater.check();
            } catch (Exception error) { finishWork("Could not prepare the game: " + error.getMessage()); }
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                startupFinished = true;
                if (schema == null || values == null) {
                    loadingMessage.setText(workMessage);
                    loadingProgress.setVisibility(View.GONE);
                    return;
                }
                showLauncherWhenReady();
            });
        });
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private void startupProgress(String message, int percent) {
        workMessage = message; startupPercent = percent;
        runOnUiThread(() -> {
            if (isDestroyed()) return;
            loadingMessage.setText(message + " · " + percent + "%");
            loadingProgress.setProgress(percent);
        });
    }
    private void showLauncherWhenReady() {
        if (pageLoaded && startupFinished) {
            loading.setVisibility(View.GONE);
            web.evaluateJavascript("refresh()", null);
        }
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (pageLoaded && intent.getBooleanExtra("show_mods", false)) {
            web.evaluateJavascript("document.querySelector('nav [data-tab=mods]').click();refresh()", null);
        } else if (pageLoaded && intent.getBooleanExtra("show_install", false)) {
            web.evaluateJavascript("document.querySelector('nav [data-tab=install]').click();refresh()", null);
        }
    }

    private boolean sessionRequest(int code) throws Exception {
        Parcel request = Parcel.obtain(), response = Parcel.obtain();
        try {
            request.writeInterfaceToken(GameSessionService.DESCRIPTOR);
            if (gameSession == null || !gameSession.transact(code, request, response, 0)) throw new IOException("Game session unavailable");
            response.readException();
            return response.readInt() != 0;
        } finally { response.recycle(); request.recycle(); }
    }
    private void connectGameSession(boolean start) {
        startGameWhenConnected = start;
        gameBound = bindService(new Intent(this, GameSessionService.class), gameConnection, BIND_AUTO_CREATE);
        if (!gameBound) {
            gameRunning = false;
            new AlertDialog.Builder(this).setMessage("Could not open the game session.").setPositiveButton("OK", null).show();
        }
    }
    private void disconnectGameSession() {
        if (gameBound) { unbindService(gameConnection); gameBound = false; }
        gameSession = null;
    }

    private boolean isLocal(Uri uri) {
        return "https".equals(uri.getScheme()) && "launcher.simpsons.local".equals(uri.getHost()) && uri.getPort() == -1;
    }
    private WebResourceResponse emptyResponse(int code, String reason) {
        return new WebResourceResponse("text/plain", "UTF-8", code, reason, null, new ByteArrayInputStream(new byte[0]));
    }
    private void openExternal(Uri uri) {
        if (!"https".equals(uri.getScheme())) return;
        try { startActivity(new Intent(Intent.ACTION_VIEW, uri)); }
        catch (android.content.ActivityNotFoundException error) {
            new AlertDialog.Builder(this).setMessage("No browser is available to open this link.").setPositiveButton("OK", null).show();
        }
    }
    private String readAsset(String name) throws IOException {
        try (InputStream input = getAssets().open(name); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            return output.toString(StandardCharsets.UTF_8.name());
        }
    }
    private JSONObject result(boolean ok, String message) throws Exception {
        return new JSONObject().put("ok", ok).put("msg", message);
    }

    private final class Bridge {
        @JavascriptInterface public String theme() { return preferences.getString("ui_theme", "system"); }
        @JavascriptInterface public synchronized String request(String method, String path, String body) {
            try {
                JSONObject data = new JSONObject(body);
                if (method.equals("GET")) {
                    switch (path) {
                        case "/api/status": return status().toString();
                        case "/api/settings": return new JSONObject().put("values", values).put("defaults", schema.getJSONObject("defaults")).toString();
                        case "/api/log": return new JSONObject().put("lines", logTail()).toString();
                        case "/api/diagnostics": return new JSONObject().put("enabled", true).put("tail", logTail()).put("dir", "the app's private storage").toString();
                        default: return result(false, "This action is unavailable on Android.").toString();
                    }
                }
                if (!method.equals("POST")) return result(false, "Unsupported request").toString();
                switch (path) {
                    case "/api/settings":
                        saveValues(data.getJSONObject("values"));
                        return result(true, "Saved").toString();
                    case "/api/ui-theme":
                        String theme = data.optString("theme", "system");
                        if (!theme.equals("light") && !theme.equals("dark")) theme = "system";
                        preferences.edit().putString("ui_theme", theme).apply();
                        return result(true, "Saved").toString();
                    case "/api/launch":
                        if (isWorking() || !prepared || !engineReady || !hasGame()) return result(false, "Import your game files and wait for preparation to finish.").toString();
                        if (gameRunning) return result(false, "The game is already running.").toString();
                        return launchInstalledMods().toString();
                    case "/api/install":
                    case "/api/import-folder":
                        if (isWorking() || gameRunning) return result(false, "Close the game and wait for the current operation to finish.").toString();
                        boolean folder = path.equals("/api/import-folder");
                        runOnUiThread(() -> {
                            Intent picker = new Intent(folder ? Intent.ACTION_OPEN_DOCUMENT_TREE : Intent.ACTION_OPEN_DOCUMENT);
                            if (!folder) picker.addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                                    .putExtra(Intent.EXTRA_TITLE, "Choose your Xbox 360 game ISO");
                            picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                            startActivityForResult(picker, folder ? PICK_GAME : PICK_ISO);
                        });
                        return result(true, folder ? "Choose your extracted game folder. Subfolders are searched automatically." : "Choose your Xbox 360 game ISO. It will be extracted on this device.").toString();
                    case "/api/cancel-install":
                        runOnUiThread(() -> GameImportService.cancel(LauncherActivity.this));
                        return result(true, "Stopping the import…").toString();
                    case "/api/edit-touch-layout":
                        if (busy || !prepared) return result(false, "Wait for preparation to finish.").toString();
                        runOnUiThread(() -> startActivity(new Intent(LauncherActivity.this, TouchEditorActivity.class)));
                        return result(true, "Opened the control editor.").toString();
                    case "/api/patch":
                        if (isWorking() || gameRunning) return result(false, "Close the game and wait for the current operation to finish before changing patches.").toString();
                        if (data.optString("id").equals("cutscene_skip")) {
                            saveValues(new JSONObject().put("cutscene_skip", data.getBoolean("enable")));
                            return result(true, "Cutscene skipping updated.").toString();
                        }
                        return gamePatches.apply(data).toString();
                    case "/api/mods":
                        if (isWorking() || gameRunning) return result(false, "Close the game and wait for the current operation to finish.").toString();
                        if (data.optString("action").equals("install")) {
                            runOnUiThread(() -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                                    .addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip")
                                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION), PICK_MOD));
                            return result(true, "Choose a mod ZIP package.").toString();
                        }
                        if (modPackages == null) modPackages = new ModPackages(LauncherActivity.this);
                        return modPackages.change(data).toString();
                    case "/api/mod-projects":
                        return result(false, "Project authoring is available in the Windows/Linux Modkit application.").toString();
                    case "/api/backup-saves": return backupSaves().toString();
                    case "/api/restore-saves": return restoreSaves(data.getString("name")).toString();
                    case "/api/support-bundle":
                        runOnUiThread(() -> startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT)
                                .addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip")
                                .putExtra(Intent.EXTRA_TITLE, "simpsons-android-support.zip"), EXPORT_SUPPORT));
                        return result(true, "Choose where to save the support bundle.").toString();
                    case "/api/check-updates":
                        updater.check();
                        return result(true, "Checking for updates…").toString();
                    case "/api/apply-update":
                        if (gameRunning) return result(false, "Close the game before updating.").toString();
                        updater.apply();
                        return result(true, "Updating…").toString();
                    case "/api/regen-art":
                        if (!hasGame()) return result(false, "Import your game first.").toString();
                        art.generate(true);
                        return result(true, "Grabbing pictures from your cutscenes…").toString();
                    default: return result(false, "This action is unavailable on Android.").toString();
                }
            } catch (Exception error) {
                try { return result(false, error.getMessage() == null ? "Request failed" : error.getMessage()).toString(); }
                catch (Exception ignored) { return "{\"ok\":false}"; }
            }
        }
    }

    private JSONObject status() throws Exception {
        if (modPackages == null && prepared) modPackages = new ModPackages(this);
        // Pictures for the launcher come from the game's cutscenes once it is imported.
        if (prepared && !isWorking() && hasGame()) art.generate(false);
        JSONObject install = busy || installResult != null
                ? new JSONObject().put("running", busy).put("phase", "preparing").put("percent", startupPercent)
                    .put("message", workMessage).put("log", new JSONArray().put(workMessage))
                : GameImportService.snapshot(this);
        if (installResult != null) install.put("ok", installResult);
        return new JSONObject().put("engine_ready", engineReady && prepared).put("gamedata_ready", hasGame())
                .put("running", gameRunning).put("engine_date", "Android ARM64")
                .put("version", getPackageManager().getPackageInfo(getPackageName(), 0).versionName)
                .put("platforms", new JSONArray().put(new JSONObject().put("name", "Android · Vulkan").put("state", "development")))
                .put("github_repo", "YesterMester/TheSimpsonsGameRecomp").put("install", install).put("preparing", busy && !prepared)
                .put("art", art.pictures()).put("art_version", art.version()).put("art_running", art.running())
                .put("update", updater.state())
                .put("backups", listBackups()).put("mods", modPackages == null ? new JSONArray() : modPackages.list())
                .put("modkit_ready", prepared && modPackages != null).put("patches", gamePatches.list()
                        .put(new JSONObject().put("id", "cutscene_skip").put("name", "Skip cutscenes")
                                .put("desc", "Tap a button to show the skip prompt. B opens an in-game confirmation. Takes effect next launch.")
                                .put("state", values.optBoolean("cutscene_skip", true) ? "on" : "off").put("available", true)));
    }

    private synchronized ModPackages mods() {
        if (modPackages == null) modPackages = new ModPackages(this);
        return modPackages;
    }

    private JSONObject launchInstalledMods() throws Exception {
        gameRunning = true;
        modsPreparing = true;
        try {
            launchModProfile = null;
            launchUserData = mods().preparePlaythrough().getAbsolutePath();
            writeSettings();
        } catch (Exception error) {
            gameRunning = false;
            throw error;
        } finally { modsPreparing = false; }
        runOnUiThread(() -> connectGameSession(true));
        return result(true, "Starting");
    }

    // Phones that can't hold 60 FPS start at 30: little memory, or no CPU core
    // near the clock of a mid-range phone's big cores (2.2 GHz and up).
    private boolean isLowEndDevice() {
        android.app.ActivityManager manager = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        android.app.ActivityManager.MemoryInfo memory = new android.app.ActivityManager.MemoryInfo();
        manager.getMemoryInfo(memory);
        if (manager.isLowRamDevice() || memory.totalMem < (5L << 30)) return true;
        long fastestKhz = 0;
        for (int cpu = 0; cpu < 16; cpu++) {
            File frequency = new File("/sys/devices/system/cpu/cpu" + cpu + "/cpufreq/cpuinfo_max_freq");
            if (!frequency.canRead()) continue;
            try { fastestKhz = Math.max(fastestKhz, Long.parseLong(new String(Files.readAllBytes(frequency.toPath()), StandardCharsets.UTF_8).trim())); }
            catch (Exception ignored) { }
        }
        return fastestKhz > 0 && fastestKhz < 2_200_000;
    }

    private synchronized void saveValues(JSONObject incoming) throws Exception {
        JSONObject types = schema.getJSONObject("types");
        for (Iterator<String> keys = incoming.keys(); keys.hasNext();) {
            String key = keys.next();
            if (!values.has(key)) continue;
            Object value = incoming.get(key);
            String type = types.optString(key);
            if (key.equals("touch_size") || key.equals("touch_opacity") || key.equals("touch_deadzone") || key.equals("touch_camera_sensitivity")) {
                double number = incoming.getDouble(key);
                if (!Double.isFinite(number)) throw new IOException("Invalid control setting");
                value = key.equals("touch_size") ? Math.max(.8, Math.min(1.3, number))
                        : key.equals("touch_opacity") ? Math.max(.2, Math.min(.85, number))
                        : key.equals("touch_deadzone") ? Math.max(0, Math.min(.35, number)) : Math.max(.5, Math.min(2, number));
            } else if (key.equals("touch_scheme")) {
                String scheme = incoming.getString(key);
                if (!scheme.equals(TouchLayoutStore.OFF) && !TouchLayoutStore.schema(this).getJSONObject("schemes").has(scheme)) {
                    throw new IOException("Unknown control scheme");
                }
                value = scheme;
            } else if (key.equals("mods_language")) {
                String language = incoming.getString(key).trim();
                if (!language.isEmpty() && !language.matches("[A-Za-z]{2,8}(?:[-_][A-Za-z0-9]{1,8}){0,3}"))
                    throw new IOException("Use a mod language code such as fr or pt-BR");
                value = language.toLowerCase(java.util.Locale.ROOT).replace('_', '-');
            } else if (type.equals("bool") || values.get(key) instanceof Boolean) value = incoming.getBoolean(key);
            else if (type.equals("int")) value = incoming.getInt(key);
            else if (type.equals("float")) {
                double number = incoming.getDouble(key);
                if (!Double.isFinite(number)) throw new IOException("Invalid numeric setting");
                value = number;
            } else if (type.equals("str")) value = incoming.getString(key);
            values.put(key, value);
        }
        values.put("gpu", "vulkan").put("fullscreen", true).put("resolution", "").put("mnk_mode", false);
        writeSettings();
    }

    private synchronized void writeSettings() throws Exception {
        Map<String, Object> runtime = new LinkedHashMap<>();
        JSONObject defaults = schema.getJSONObject("runtime_defaults"), types = schema.getJSONObject("types");
        for (Iterator<String> keys = defaults.keys(); keys.hasNext();) {
            String key = keys.next(); runtime.put(key, defaults.get(key));
        }
        for (Iterator<String> keys = types.keys(); keys.hasNext();) {
            String key = keys.next();
            if (!key.equals("resolution")) runtime.put(key, values.get(key));
        }
        boolean tearing = !values.optBoolean("vsync", true);
        JSONArray derived = schema.getJSONArray("derived_keys");
        for (int i = 0; i < derived.length(); i++) runtime.put(derived.getString(i), tearing);
        File config = new File(getFilesDir(), "simpsons.toml");
        StringBuilder text = new StringBuilder();
        if (config.exists()) for (String line : Files.readAllLines(config.toPath(), StandardCharsets.UTF_8)) {
            String key = line.split("=", 2)[0].trim();
            if (!runtime.containsKey(key) && !line.startsWith("# Android launcher settings")) text.append(line).append('\n');
        }
        text.append("# Android launcher settings\n");
        for (Map.Entry<String, Object> entry : runtime.entrySet()) {
            Object value = entry.getValue();
            text.append(entry.getKey()).append(" = ").append(value instanceof String ? JSONObject.quote((String) value) : value).append('\n');
        }
        try (InputStream input = new ByteArrayInputStream(text.toString().getBytes(StandardCharsets.UTF_8))) { copyFile(input, config); }
        preferences.edit().putString("launcher_values", values.toString())
                .putFloat("touch_size", (float) values.getDouble("touch_size"))
                .putFloat("touch_opacity", (float) values.getDouble("touch_opacity"))
                .putFloat("touch_deadzone", (float) values.getDouble("touch_deadzone"))
                .putFloat("touch_camera_sensitivity", (float) values.getDouble("touch_camera_sensitivity"))
                .putBoolean("touch_camera_invert_y", values.getBoolean("touch_camera_invert_y"))
                .putBoolean("touch_show_editor", values.getBoolean("touch_show_editor"))
                .putBoolean("floating_stick", values.getBoolean("floating_stick"))
                .putBoolean("touch_haptics", values.getBoolean("touch_haptics"))
                .putBoolean("hide_with_controller", values.getBoolean("hide_with_controller"))
                .putString("touch_scheme", values.optString("touch_scheme", "simple")).commit();
        if (!values.optString("touch_scheme", "").isEmpty()
                && !values.optString("touch_scheme").equals(TouchLayoutStore.activeScheme(this))) {
            TouchLayoutStore.setActiveScheme(this, values.optString("touch_scheme"));
        }
    }

    private JSONArray logTail() throws IOException {
        File[] files = new File(getFilesDir(), "logs").listFiles();
        File latest = null;
        if (files != null) for (File file : files) if (file.isFile() && (latest == null || file.lastModified() > latest.lastModified())) latest = file;
        JSONArray lines = new JSONArray();
        if (latest != null) try (java.io.RandomAccessFile log = new java.io.RandomAccessFile(latest, "r")) {
            log.seek(Math.max(0, log.length() - 64 * 1024));
            byte[] buffer = new byte[(int) (log.length() - log.getFilePointer())];
            log.readFully(buffer);
            String[] tail = new String(buffer, StandardCharsets.UTF_8).split("\n");
            for (int i = Math.max(0, tail.length - 200); i < tail.length; i++) lines.put(tail[i]);
        }
        return lines;
    }

    private File gameDir() {
        File storage = getExternalFilesDir(null);
        return new File(storage == null ? getFilesDir() : storage, "game");
    }
    private boolean isWorking() { return busy || modsPreparing || GameImportService.isRunning(); }
    private boolean hasGame() {
        File[] files = gameDir().listFiles();
        if (files != null) for (File file : files) if (file.isFile() && file.getName().equalsIgnoreCase("default.xex")) return true;
        return false;
    }
    private void recoverGameImport() throws IOException {
        File previous = new File(getExternalFilesDir(null), "game.previous");
        if (!gameDir().exists() && previous.exists() && !previous.renameTo(gameDir())) throw new IOException("Could not recover the previous game import");
    }
    private void finishWork(String error) {
        workMessage = error != null ? error : hasGame() ? "Ready to play" : "Choose your game folder to get started.";
        installResult = error == null ? null : false;
        busy = false;
    }
    private void prepareAssets() throws Exception {
        JSONObject index = new JSONObject(readAsset("native-shaders-index.json"));
        String fingerprint = index.getString("fingerprint");
        File shaders = new File(getFilesDir(), "native_shaders");
        File marker = new File(shaders, "asset-index.json");
        if (!fingerprint.equals(preferences.getString("assets_fingerprint", "")) || !marker.isFile()) {
            JSONObject previous = marker.isFile() ? new JSONObject(new String(Files.readAllBytes(marker.toPath()), StandardCharsets.UTF_8)) : new JSONObject();
            JSONObject previousFiles = previous.optJSONObject("files");
            JSONObject files = index.getJSONObject("files");
            long total = index.getLong("bytes"), done = 0, lastUpdate = 0;
            int count = 0;
            if (!shaders.isDirectory() && !shaders.mkdirs()) throw new IOException("Cannot create the shader folder");
            byte[] buffer = new byte[256 * 1024];
            for (Iterator<String> keys = files.keys(); keys.hasNext();) {
                if (Thread.currentThread().isInterrupted()) throw new IOException("Preparation interrupted");
                String key = keys.next();
                if (key.startsWith("/") || key.contains("..") || key.contains("\\")) throw new IOException("Invalid shader asset path");
                JSONObject entry = files.getJSONObject(key);
                File destination = new File(shaders, key);
                JSONObject old = previousFiles == null ? null : previousFiles.optJSONObject(key);
                boolean unchanged = old != null && entry.getString("sha256").equals(old.optString("sha256"))
                        && destination.isFile() && destination.length() == entry.getLong("size");
                if (!unchanged) {
                    File parent = destination.getParentFile();
                    if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create the shader folder");
                    File temporary = new File(parent, destination.getName() + ".importing");
                    try (InputStream input = getAssets().open("native_shaders/" + key); FileOutputStream output = new FileOutputStream(temporary)) {
                        int length;
                        while ((length = input.read(buffer)) != -1) {
                            if (Thread.currentThread().isInterrupted()) throw new IOException("Preparation interrupted");
                            output.write(buffer, 0, length);
                        }
                    }
                    if (temporary.length() != entry.getLong("size") || !temporary.renameTo(destination)) throw new IOException("Could not prepare " + key);
                }
                done += entry.getLong("size"); count++;
                long now = android.os.SystemClock.uptimeMillis();
                if (now - lastUpdate >= 100 || count == files.length()) {
                    lastUpdate = now;
                    startupProgress("Preparing shader files " + count + " / " + files.length(), 8 + (int) (done * 86 / Math.max(1, total)));
                }
            }
            // Flush this marker once after the batch, rather than syncing thousands of shader files separately.
            try (InputStream input = new ByteArrayInputStream(index.toString().getBytes(StandardCharsets.UTF_8))) { copyFile(input, marker); }
            preferences.edit().putString("assets_fingerprint", fingerprint).commit();
        }
        startupProgress("Finishing preparation", 96);
        File config = new File(getFilesDir(), "simpsons.toml");
        if (!config.exists()) copyAsset("simpsons.toml", config);
    }
    private void copyAsset(String name, File destination) throws IOException {
        String[] children = getAssets().list(name);
        if (children != null && children.length != 0) {
            if (!destination.isDirectory() && !destination.mkdirs()) throw new IOException("Cannot create " + destination.getName());
            for (String child : children) copyAsset(name + "/" + child, new File(destination, child));
        } else try (InputStream input = getAssets().open(name)) { copyFile(input, destination); }
    }
    private void copyFile(InputStream input, File destination) throws IOException {
        File temporary = new File(destination.getParentFile(), destination.getName() + ".importing");
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            byte[] buffer = new byte[256 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (Thread.currentThread().isInterrupted()) throw new IOException("Operation cancelled");
                output.write(buffer, 0, count);
            }
            output.getFD().sync();
        }
        if (!temporary.renameTo(destination)) throw new IOException("Could not finish " + destination.getName());
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == PLAY_GAME) return; // Keep the guard until the native process has finished saving and exited.
        if (resultCode != RESULT_OK || data == null || data.getData() == null || isWorking()) return;
        Uri uri = data.getData();
        if (requestCode == EXPORT_SUPPORT) { exportSupport(uri); return; }
        if (requestCode == PICK_MOD) {
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) { }
            try {
                if (isWorking() || gameRunning) throw new IOException("Close the game before installing mods.");
                GameImportService.beginMod(this, uri);
                installResult = null;
                web.evaluateJavascript("document.querySelector('nav [data-tab=mods]').click();refresh()", null);
            } catch (Exception error) {
                new AlertDialog.Builder(this).setMessage("Could not install the mod: " + error.getMessage()).setPositiveButton("OK", null).show();
            }
            return;
        }
        if (requestCode != PICK_GAME && requestCode != PICK_ISO) return;
        try {
            if ((data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0)
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        }
        catch (SecurityException error) { android.util.Log.w("GameImport", "The provider did not offer persistent folder access", error); }
        if (hasGame()) new AlertDialog.Builder(this).setTitle("Replace the imported game?")
                .setMessage("Your saves and settings will be kept. The new files must finish importing before the existing game is replaced.")
                .setPositiveButton("Import", (dialog, which) -> importGame(uri, requestCode == PICK_ISO)).setNegativeButton("Cancel", null).show();
        else importGame(uri, requestCode == PICK_ISO);
    }
    private void importGame(Uri tree, boolean iso) {
        pendingImport = tree;
        pendingImportIso = iso;
        if (android.os.Build.VERSION.SDK_INT >= 33 && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
                != android.content.pm.PackageManager.PERMISSION_GRANTED && !preferences.getBoolean("notifications_requested", false)) {
            preferences.edit().putBoolean("notifications_requested", true).apply();
            requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS}, NOTIFICATIONS);
        } else startImport();
    }
    private void startImport() {
        if (pendingImport == null) return;
        try {
            GameImportService.begin(this, pendingImport, pendingImportIso);
            installResult = null;
            web.evaluateJavascript("document.querySelector('nav [data-tab=install]').click();refresh()", null);
        } catch (Exception error) {
            new AlertDialog.Builder(this).setMessage("Could not start the import: " + error.getMessage()).setPositiveButton("OK", null).show();
        }
        pendingImport = null;
    }
    @Override public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == NOTIFICATIONS) startImport(); // Background importing also works if notifications are declined.
    }
    @Override protected void onSaveInstanceState(Bundle state) {
        if (pendingImport != null) state.putString("pending_import", pendingImport.toString());
        state.putBoolean("pending_import_iso", pendingImportIso);
        super.onSaveInstanceState(state);
    }

    private File backupDir() { return new File(getFilesDir(), "save-backups"); }
    private JSONArray listBackups() throws Exception {
        JSONArray result = new JSONArray();
        File[] files = backupDir().listFiles();
        if (files != null) for (File file : files) if (file.isDirectory() && file.getName().matches("[0-9-]+")) result.put(new JSONObject()
                .put("name", file.getName()).put("date", file.getName()).put("size_kb", (treeSize(file) + 1023) / 1024));
        return result;
    }
    private long treeSize(File file) {
        if (file.isFile()) return file.length();
        long size = 0; File[] files = file.listFiles();
        if (files != null) for (File child : files) size += treeSize(child);
        return size;
    }
    private JSONObject backupSaves() throws Exception {
        if (isWorking() || gameRunning) return result(false, "Close the game before backing up saves.");
        File saves = new File(getFilesDir(), "simpsons");
        if (!saves.isDirectory()) return result(false, "No saves have been created yet.");
        String name = new SimpleDateFormat("yyyy-MM-dd-HH-mm-ss-SSS", Locale.ROOT).format(new Date());
        File destination = new File(backupDir(), name);
        copyTree(saves, destination);
        return result(true, "Saves backed up on this device.");
    }
    private JSONObject restoreSaves(String name) throws Exception {
        if (isWorking() || gameRunning) return result(false, "Close the game before restoring saves.");
        if (!name.matches("[0-9-]+")) return result(false, "Invalid backup name.");
        File source = new File(backupDir(), name);
        if (!source.isDirectory()) return result(false, "Backup not found.");
        // Keep an automatic copy of current saves before applying a backup.
        File saves = new File(getFilesDir(), "simpsons");
        if (saves.exists()) backupSaves();
        File staging = new File(getFilesDir(), "saves.restoring");
        removeTree(staging); copyTree(source, staging);
        File previous = new File(getFilesDir(), "saves.previous");
        removeTree(previous);
        if (saves.exists() && !saves.renameTo(previous)) throw new IOException("Cannot replace the current saves");
        if (!staging.renameTo(saves)) {
            previous.renameTo(saves); throw new IOException("Cannot finish restoring saves");
        }
        removeTree(previous);
        return result(true, "Restored.");
    }
    private void copyTree(File source, File destination) throws IOException {
        if (source.isDirectory()) {
            if (!destination.isDirectory() && !destination.mkdirs()) throw new IOException("Cannot create backup folder");
            File[] files = source.listFiles();
            if (files == null) throw new IOException("Cannot read save folder");
            for (File child : files) copyTree(child, new File(destination, child.getName()));
        } else try (InputStream input = Files.newInputStream(source.toPath())) { copyFile(input, destination); }
    }
    private void exportSupport(Uri destination) {
        busy = true; workMessage = "Saving support bundle…";
        worker.execute(() -> {
            try (java.io.OutputStream output = getContentResolver().openOutputStream(destination);
                 ZipOutputStream zip = new ZipOutputStream(java.util.Objects.requireNonNull(output))) {
                zip.putNextEntry(new ZipEntry("simpsons.toml"));
                zip.write(Files.readAllBytes(new File(getFilesDir(), "simpsons.toml").toPath())); zip.closeEntry();
                zip.putNextEntry(new ZipEntry("game-log-tail.txt"));
                zip.write(logTail().toString(2).getBytes(StandardCharsets.UTF_8)); zip.closeEntry();
                File[] logs = new File(getFilesDir(), "logs").listFiles(File::isFile);
                if (logs != null) {
                    java.util.Arrays.sort(logs, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
                    for (int i = 0; i < Math.min(3, logs.length); i++) {
                        zip.putNextEntry(new ZipEntry("game-logs/" + logs[i].getName()));
                        try (java.io.RandomAccessFile log = new java.io.RandomAccessFile(logs[i], "r")) {
                            long remaining = Math.min(log.length(), 2 * 1024 * 1024);
                            log.seek(log.length() - remaining);
                            byte[] buffer = new byte[64 * 1024]; int count;
                            while (remaining > 0 && (count = log.read(buffer, 0, (int) Math.min(buffer.length, remaining))) > 0) {
                                zip.write(buffer, 0, count); remaining -= count;
                            }
                        }
                        zip.closeEntry();
                    }
                }
                zip.putNextEntry(new ZipEntry("touch-layout.json"));
                String touchScheme = TouchLayoutStore.activeScheme(this);
                zip.write(new JSONObject().put("scheme", touchScheme)
                        .put("layout", TouchLayoutStore.load(this, null)).toString().getBytes(StandardCharsets.UTF_8)); zip.closeEntry();
                zip.putNextEntry(new ZipEntry("device.txt"));
                android.app.ActivityManager.MemoryInfo memory = new android.app.ActivityManager.MemoryInfo();
                getSystemService(android.app.ActivityManager.class).getMemoryInfo(memory);
                String device = android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL + "\nAndroid " + android.os.Build.VERSION.RELEASE
                        + " (API " + android.os.Build.VERSION.SDK_INT + ")\nApp " + getPackageManager().getPackageInfo(getPackageName(), 0).versionName
                        + "\nABI " + String.join(", ", android.os.Build.SUPPORTED_ABIS)
                        + "\nMemory pages " + android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE)
                        + "\nRAM total " + memory.totalMem + ", available " + memory.availMem + ", low " + memory.lowMemory
                        + "\nThermal status " + getSystemService(android.os.PowerManager.class).getCurrentThermalStatus() + "\n";
                zip.write(device.getBytes(StandardCharsets.UTF_8)); zip.closeEntry();
                finishWork(null);
            } catch (Exception error) { finishWork("Could not save support bundle: " + error.getMessage()); }
        });
    }

    private void removeTree(File file) throws IOException {
        if (!file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) removeTree(child);
        if (!file.delete()) throw new IOException("Cannot remove " + file.getName());
    }
    @Override protected void onPause() {
        if (web != null) { web.onPause(); web.pauseTimers(); }
        super.onPause();
    }
    @Override protected void onResume() {
        super.onResume();
        // The control editor can switch schemes; keep this copy in step so a
        // later settings save doesn't undo it.
        synchronized (this) {
            if (values != null) {
                try { values.put("touch_scheme", TouchLayoutStore.activeScheme(this)); }
                catch (Exception ignored) { }
            }
        }
        if (web != null) {
            web.resumeTimers(); web.onResume();
            if (pageLoaded) web.evaluateJavascript("refresh()", null);
        }
    }
    @Override protected void onDestroy() {
        disconnectGameSession();
        worker.shutdownNow();
        updater.shutdown();
        art.shutdown();
        web.removeJavascriptInterface("AndroidLauncher");
        web.destroy();
        super.onDestroy();
    }
}
