package io.github.yestermester.simpsons;

import android.app.Activity;
import android.app.AlertDialog;
import android.net.Uri;
import android.os.Bundle;
import android.view.ViewGroup;
import android.webkit.JavascriptInterface;
import android.webkit.JsResult;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import java.io.ByteArrayInputStream;
import java.util.Collections;
import org.json.JSONObject;

/** Edits a preview without loading the game or sending controller input. */
public final class TouchEditorActivity extends Activity {
    // The page and its font come from the APK's assets under this origin. A
    // page loaded as data has a data: URL that WebView also passes through
    // shouldInterceptRequest, and the deny-all answer then blocked the editor
    // itself (ERR_HTTP_RESPONSE_CODE_FAILURE).
    private static final String ORIGIN = "https://touch.simpsons.local";
    private WebView web;
    private boolean loaded;
    private android.window.OnBackInvokedCallback back;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xff17213d);
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets safe = insets.getInsets(android.view.WindowInsets.Type.systemBars() | android.view.WindowInsets.Type.displayCutout());
                view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
                return android.view.WindowInsets.CONSUMED;
            } else view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets.consumeSystemWindowInsets().consumeDisplayCutout();
        });
        web = new WebView(this);
        web.setBackgroundColor(0xff17213d);
        web.getSettings().setJavaScriptEnabled(true);
        web.getSettings().setAllowFileAccess(false);
        web.getSettings().setAllowContentAccess(false);
        web.addJavascriptInterface(new Bridge(), "AndroidTouchEditor");
        web.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) { return true; }
            @Override public void onPageFinished(WebView view, String url) { loaded = true; }
            @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                Uri uri = request.getUrl();
                if ("https".equals(uri.getScheme()) && "touch.simpsons.local".equals(uri.getHost())
                        && request.getMethod().equals("GET")) {
                    String path = uri.getPath();
                    try {
                        if ("/touch-editor.html".equals(path)) {
                            return new WebResourceResponse("text/html", "UTF-8", getAssets().open("launcher/touch-editor.html"));
                        }
                        if ("/fonts/LuckiestGuy-Regular.ttf".equals(path)) {
                            return new WebResourceResponse("font/ttf", null, getAssets().open("launcher/fonts/LuckiestGuy-Regular.ttf"));
                        }
                    } catch (java.io.IOException ignored) { }
                }
                return new WebResourceResponse("text/plain", "UTF-8", 403, "Forbidden", Collections.emptyMap(), new ByteArrayInputStream(new byte[0]));
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override public boolean onJsConfirm(WebView view, String url, String message, JsResult result) {
                new AlertDialog.Builder(TouchEditorActivity.this).setMessage(message)
                        .setPositiveButton("Continue", (dialog, which) -> result.confirm())
                        .setNegativeButton("Cancel", (dialog, which) -> result.cancel())
                        .setOnCancelListener(dialog -> result.cancel()).show();
                return true;
            }
        });
        root.addView(web, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);
        web.loadUrl(ORIGIN + "/touch-editor.html");
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            back = this::requestClose;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, back);
        }
    }
    private final class Bridge {
        /** The editor's model for a scheme; an empty name means the chosen one. */
        @JavascriptInterface public String load(String requested) {
            try {
                String scheme = TouchLayoutStore.editableScheme(TouchEditorActivity.this, requested.isEmpty() ? null : requested);
                android.util.DisplayMetrics display = getResources().getDisplayMetrics();
                android.content.SharedPreferences preferences = getSharedPreferences("settings", MODE_PRIVATE);
                return new JSONObject().put("scheme", scheme)
                        .put("active", TouchLayoutStore.activeScheme(TouchEditorActivity.this))
                        .put("schemes", TouchLayoutStore.schemeList(TouchEditorActivity.this))
                        .put("schema", TouchLayoutStore.scheme(TouchEditorActivity.this, scheme))
                        .put("layout", TouchLayoutStore.load(TouchEditorActivity.this, scheme))
                        .put("scale", preferences.getFloat("touch_size", 1)).put("opacity", preferences.getFloat("touch_opacity", .5f))
                        .put("viewport", new JSONObject().put("width", getIntent().getIntExtra("preview_width", Math.max(display.widthPixels, display.heightPixels)))
                                .put("height", getIntent().getIntExtra("preview_height", Math.min(display.widthPixels, display.heightPixels)))).toString();
            } catch (Exception error) { return "{\"error\":\"Could not load touch controls\"}"; }
        }
        @JavascriptInterface public String save(String scheme, String text) {
            try {
                TouchLayoutStore.save(TouchEditorActivity.this, scheme, text);
                TouchLayoutStore.setActiveScheme(TouchEditorActivity.this, TouchLayoutStore.editableScheme(TouchEditorActivity.this, scheme));
                return "{\"ok\":true}";
            } catch (Exception error) {
                try { return new JSONObject().put("ok", false).put("msg", error.getMessage()).toString(); }
                catch (Exception ignored) { return "{\"ok\":false}"; }
            }
        }
        /** Physical controls only: hides the touch controls until a scheme is chosen again. */
        @JavascriptInterface public String turnOff() {
            try { TouchLayoutStore.setActiveScheme(TouchEditorActivity.this, TouchLayoutStore.OFF); return "{\"ok\":true}"; }
            catch (Exception error) { return "{\"ok\":false}"; }
        }
        @JavascriptInterface public void close() { runOnUiThread(TouchEditorActivity.this::finish); }
    }
    private void requestClose() {
        if (loaded) web.evaluateJavascript("requestClose()", null);
        else finish();
    }
    // Android 13 and newer use the dispatcher registered above.
    @android.annotation.SuppressLint("GestureBackNavigation")
    @Override public void onBackPressed() { requestClose(); }
    @Override protected void onPause() { web.onPause(); web.pauseTimers(); super.onPause(); }
    @Override protected void onResume() { super.onResume(); if (web != null) { web.resumeTimers(); web.onResume(); } }
    @Override protected void onDestroy() {
        if (back != null && android.os.Build.VERSION.SDK_INT >= 33) getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(back);
        web.removeJavascriptInterface("AndroidTouchEditor"); web.destroy(); super.onDestroy();
    }
}
