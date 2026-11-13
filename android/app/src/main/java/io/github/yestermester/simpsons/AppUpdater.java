package io.github.yestermester.simpsons;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.provider.Settings;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Updates the app from the project's GitHub releases: the latest release's APK, checked against
 * this app's signing key and installed through Android's package installer, which asks first.
 * Its state is the shape the shared launcher page shows for desktop updates.
 */
final class AppUpdater {
    static final String REPO = "YesterMester/TheSimpsonsGameRecomp";
    private static volatile String installResult;
    private final Activity activity;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile boolean checked, checking, available, applying;
    private volatile String message = "", applyMessage = "", latestTag = "", apkUrl;
    private volatile long apkSize;

    AppUpdater(Activity activity) { this.activity = activity; }

    synchronized JSONObject state() throws Exception {
        String result = installResult;
        if (result != null) {
            installResult = null;
            applying = false;
            applyMessage = result;
        }
        return new JSONObject().put("checked", checked).put("msg", checking ? "Checking…" : message)
                .put("update_available", available).put("latest_tag", latestTag)
                .put("applying", applying).put("apply_msg", applyMessage);
    }

    /** Looks for a newer release in the background. */
    synchronized void check() {
        if (checking || applying) return;
        checking = true;
        checked = true;
        worker.execute(() -> {
            String current = installedVersion();
            try {
                JSONObject release = new JSONObject(read("https://api.github.com/repos/" + REPO + "/releases/latest"));
                String tag = release.getString("tag_name");
                JSONObject apk = null;
                JSONArray assets = release.optJSONArray("assets");
                for (int i = 0; assets != null && i < assets.length(); i++) {
                    JSONObject asset = assets.getJSONObject(i);
                    if (asset.optString("name").toLowerCase(Locale.ROOT).endsWith(".apk")) { apk = asset; break; }
                }
                latestTag = tag;
                if (compareVersions(tag, current) <= 0) {
                    available = false;
                    message = "You have the latest version (v" + current + ").";
                } else if (apk == null) {
                    available = false;
                    message = tag + " is out, but it has no Android download yet. You have v" + current + ".";
                } else {
                    apkUrl = apk.getString("browser_download_url");
                    apkSize = apk.optLong("size");
                    available = true;
                    message = tag + " is available. You have v" + current + ".";
                }
            } catch (Exception error) {
                available = false;
                message = "Could not check for updates: " + describe(error);
            } finally { checking = false; }
        });
    }

    /** Downloads the update and hands it to Android's package installer. */
    synchronized void apply() {
        if (!available || applying) return;
        PackageManager packages = activity.getPackageManager();
        if (!packages.canRequestPackageInstalls()) {
            applyMessage = "Allow this app to install updates, then tap Update now again.";
            activity.runOnUiThread(() -> activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + activity.getPackageName()))));
            return;
        }
        applying = true;
        applyMessage = "Downloading " + latestTag + "…";
        String url = apkUrl, tag = latestTag;
        worker.execute(() -> {
            try {
                File folder = new File(activity.getCacheDir(), "updates");
                if (!folder.isDirectory() && !folder.mkdirs()) throw new IOException("Cannot create the download folder");
                File apk = new File(folder, "update.apk");
                download(url, apk, tag);
                checkPackage(apk);
                applyMessage = "Opening Android's installer…";
                install(apk);
                applyMessage = "Confirm the update in Android's installer. The app closes while it updates.";
            } catch (Exception error) {
                applyMessage = "The update failed: " + describe(error);
                applying = false;
            }
        });
    }

    void shutdown() { worker.shutdownNow(); }

    private String installedVersion() {
        try { return activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).versionName; }
        catch (PackageManager.NameNotFoundException error) { return "0"; }
    }

    private static String describe(Exception error) {
        return error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage();
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setRequestProperty("User-Agent", "TheSimpsonsGameRecomp-Android");
        connection.setRequestProperty("Accept", "application/vnd.github+json, application/octet-stream");
        connection.setConnectTimeout(20000);
        connection.setReadTimeout(30000);
        connection.setInstanceFollowRedirects(true);
        int code = connection.getResponseCode();
        if (code == 404) throw new IOException("no release was found");
        if (code != HttpURLConnection.HTTP_OK) throw new IOException("GitHub answered " + code);
        return connection;
    }

    private static String read(String url) throws IOException {
        HttpURLConnection connection = open(url);
        try (InputStream input = connection.getInputStream(); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[16384];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (output.size() > (4 << 20)) throw new IOException("The release information is too large");
                output.write(buffer, 0, count);
            }
            return output.toString(StandardCharsets.UTF_8.name());
        } finally { connection.disconnect(); }
    }

    private void download(String url, File destination, String tag) throws IOException {
        HttpURLConnection connection = open(url);
        File partial = new File(destination.getParentFile(), destination.getName() + ".part");
        try (InputStream input = connection.getInputStream(); FileOutputStream output = new FileOutputStream(partial)) {
            long total = connection.getContentLengthLong() > 0 ? connection.getContentLengthLong() : apkSize, done = 0;
            byte[] buffer = new byte[256 * 1024];
            int count, shown = -1;
            while ((count = input.read(buffer)) != -1) {
                if (Thread.currentThread().isInterrupted()) throw new IOException("cancelled");
                output.write(buffer, 0, count);
                done += count;
                int percent = total > 0 ? (int) (done * 100 / total) : -1;
                if (percent != shown) {
                    shown = percent;
                    applyMessage = "Downloading " + tag + "… " + (percent >= 0 ? percent + "%" : (done >> 20) + " MB");
                }
            }
            output.getFD().sync();
        } finally { connection.disconnect(); }
        if (apkSize > 0 && partial.length() != apkSize) throw new IOException("the download was incomplete");
        if (destination.exists() && !destination.delete()) throw new IOException("Cannot replace the previous download");
        if (!partial.renameTo(destination)) throw new IOException("Cannot finish the download");
    }

    /** The download must be this app, signed with the same key, and not older. */
    private void checkPackage(File apk) throws Exception {
        PackageManager packages = activity.getPackageManager();
        int flags = PackageManager.GET_SIGNING_CERTIFICATES | PackageManager.GET_SIGNATURES;
        PackageInfo update = packages.getPackageArchiveInfo(apk.getPath(), flags);
        PackageInfo self = packages.getPackageInfo(activity.getPackageName(), flags);
        if (update == null || !activity.getPackageName().equals(update.packageName))
            throw new IOException("the download is not this app");
        if (!signers(update).equals(signers(self)) || signers(self).isEmpty())
            throw new IOException("it is signed with a different key than this app. Install it from the releases page instead.");
        if (update.getLongVersionCode() < self.getLongVersionCode())
            throw new IOException("it is older than the installed app");
    }

    private static Set<String> signers(PackageInfo info) {
        Signature[] signatures = info.signingInfo != null ? info.signingInfo.getApkContentsSigners() : info.signatures;
        Set<String> result = new HashSet<>();
        if (signatures != null) for (Signature signature : signatures) result.add(Arrays.toString(signature.toByteArray()));
        return result;
    }

    private void install(File apk) throws IOException {
        PackageInstaller installer = activity.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(activity.getPackageName());
        params.setSize(apk.length());
        int id = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream input = new FileInputStream(apk); OutputStream output = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buffer = new byte[256 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                session.fsync(output);
            }
            Intent result = new Intent(activity, Receiver.class);
            PendingIntent pending = PendingIntent.getBroadcast(activity, id, result,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(pending.getIntentSender());
        } catch (IOException | RuntimeException error) {
            installer.abandonSession(id);
            throw error;
        }
    }

    /** "1.2.3" against "v1.2.4-dev": numbers first, then a release beats its pre-release. */
    static int compareVersions(String a, String b) {
        String[] left = a.replaceFirst("^[vV]", "").split("-", 2), right = b.replaceFirst("^[vV]", "").split("-", 2);
        String[] x = left[0].split("\\."), y = right[0].split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            long p = i < x.length ? number(x[i]) : 0, q = i < y.length ? number(y[i]) : 0;
            if (p != q) return Long.compare(p, q);
        }
        return Boolean.compare(left.length == 1, right.length == 1);
    }

    private static long number(String text) {
        try { return Long.parseLong(text.replaceAll("[^0-9].*$", "")); }
        catch (NumberFormatException error) { return 0; }
    }

    /** Android's installer reports here; it needs the user's confirmation first. */
    public static final class Receiver extends BroadcastReceiver {
        @Override public void onReceive(Context context, Intent intent) {
            int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
            if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                Intent confirm = android.os.Build.VERSION.SDK_INT >= 33
                        ? intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class)
                        : intent.getParcelableExtra(Intent.EXTRA_INTENT);
                if (confirm != null) context.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            } else if (status != PackageInstaller.STATUS_SUCCESS) {
                String detail = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                installResult = status == PackageInstaller.STATUS_FAILURE_ABORTED ? "The update was cancelled."
                        : "Android did not install the update" + (detail == null ? "." : ": " + detail);
            }
        }
    }
}
