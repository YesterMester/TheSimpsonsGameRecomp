package io.github.yestermester.simpsons;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONArray;
import org.json.JSONObject;

/** Owns game imports independently of the launcher's WebView and activity. */
public final class GameImportService extends Service {
    private static final String CHANNEL = "game-import", CANCEL = "cancel-import";
    private static final int NOTIFICATION = 7;
    private static JSONObject state;
    private static boolean active;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final CancellationSignal queryCancellation = new CancellationSignal();
    private volatile boolean cancelled;
    private volatile String cancellationMessage = "Import cancelled. Your previous game files are kept.";
    private PowerManager.WakeLock wakeLock;
    private long lastProgress, copyStarted;
    private volatile String lastPhase = "";
    private boolean started;
    private volatile boolean completed, destroyed;

    private static SharedPreferences preferences(Context context) {
        return context.getSharedPreferences("game-import", MODE_PRIVATE);
    }
    static synchronized JSONObject snapshot(Context context) throws Exception {
        if (state == null) {
            state = new JSONObject(preferences(context).getString("status", "{\"running\":false,\"log\":[]}"));
            if (state.optBoolean("running")) {
                state.put("running", false).put("ok", false).put("phase", "interrupted")
                        .put("message", "Android interrupted the previous import. Choose the folder again to retry; your previous game files are kept.")
                        .put("log", new JSONArray().put("Android interrupted the previous import. Choose the folder again to retry; your previous game files are kept."));
                preferences(context).edit().putString("status", state.toString()).apply();
            }
        }
        return new JSONObject(state.toString());
    }
    static synchronized boolean isRunning() { return active; }
    static synchronized void begin(Context context, Uri tree, boolean iso) throws Exception {
        begin(context, tree, iso, false);
    }
    static synchronized void beginMod(Context context, Uri archive) throws Exception {
        begin(context, archive, false, true);
    }
    private static synchronized void begin(Context context, Uri tree, boolean iso, boolean mod) throws Exception {
        if (active) throw new IOException("A game import is already running.");
        active = true;
        state = new JSONObject().put("running", true).put("phase", "scanning")
                .put("target", mod ? "mod" : "game")
                .put("message", mod ? "Opening mod package…" : "Looking for your game files…")
                .put("log", new JSONArray().put(mod ? "Opening mod package…" : "Looking for your game files…"));
        preferences(context).edit().putString("status", state.toString()).commit();
        try {
            context.startForegroundService(new Intent(context, GameImportService.class).setData(tree).putExtra("iso", iso).putExtra("mod", mod));
        } catch (Exception error) {
            active = false;
            state.put("running", false).put("ok", false).put("message", "Could not start the import: " + error.getMessage())
                    .put("log", new JSONArray().put("Could not start the import: " + error.getMessage()));
            preferences(context).edit().putString("status", state.toString()).apply();
            throw error;
        }
    }
    static void cancel(Context context) {
        if (isRunning()) context.startService(new Intent(context, GameImportService.class).setAction(CANCEL));
    }
    private void publish(JSONObject update, boolean persist) {
        synchronized (GameImportService.class) {
            state = update;
            if (persist) preferences(this).edit().putString("status", state.toString()).commit();
        }
        getSystemService(NotificationManager.class).notify(NOTIFICATION, notification(update));
    }
    private Notification notification(JSONObject update) {
        boolean running = update.optBoolean("running");
        String message = update.optString("message", "Preparing import…");
        boolean mod = update.optString("target").equals("mod");
        String title = mod ? (running ? "Installing mod" : update.optBoolean("ok") ? "Mod installed" : "Mod install stopped")
                : running ? "Importing The Simpsons Game" : update.optBoolean("ok") ? "Game ready to play" : "Game import stopped";
        int percent = update.optInt("percent", -1);
        String detail = message;
        if (running && percent >= 0) {
            int totalFiles = update.optInt("total_files", -1);
            detail = percent + "% · " + update.optInt("files") + (totalFiles >= 0 ? " / " + totalFiles : "") + " files\n" + message;
            if (update.optLong("total_bytes", -1) >= 0) detail += "\n" + GameFileImporter.formatBytes(update.optLong("bytes"))
                    + " / " + GameFileImporter.formatBytes(update.optLong("total_bytes"));
        }
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, LauncherActivity.class)
                .putExtra("show_install", !mod).putExtra("show_mods", mod)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder builder = new Notification.Builder(this, CHANNEL).setSmallIcon(R.drawable.ic_donut_notification)
                .setContentTitle(title).setContentText(detail).setStyle(new Notification.BigTextStyle().bigText(detail))
                .setContentIntent(open).setOngoing(running).setAutoCancel(!running).setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS);
        if (running) {
            builder.setProgress(100, Math.max(0, percent), percent < 0);
            if (!update.optString("phase").equals("finishing")) {
                PendingIntent cancel = PendingIntent.getService(this, 1, new Intent(this, GameImportService.class).setAction(CANCEL),
                        PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
                builder.addAction(new Notification.Action.Builder(null, "Cancel", cancel).build());
            }
        }
        return builder.build();
    }

    @Override public void onCreate() {
        super.onCreate();
        NotificationChannel channel = new NotificationChannel(CHANNEL, "Game installation", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Game-file import progress and completion");
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
    }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && CANCEL.equals(intent.getAction())) {
            if (!started || completed) { stopSelf(startId); return START_NOT_STICKY; }
            if (!lastPhase.equals("finishing")) { cancelled = true; queryCancellation.cancel(); }
            return START_NOT_STICKY;
        }
        if (started) return START_NOT_STICKY;
        if (intent == null || intent.getData() == null) { stopSelf(); return START_NOT_STICKY; }
        started = true;
        Uri tree = intent.getData();
        boolean iso = intent.getBooleanExtra("iso", false);
        boolean mod = intent.getBooleanExtra("mod", false);
        if (mod) cancellationMessage = "Mod installation cancelled. Installed mods are kept.";
        try {
            startForeground(NOTIFICATION, notification(snapshot(this)), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            wakeLock = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, getPackageName() + ":GameImport");
            wakeLock.acquire(6 * 60 * 60 * 1000L);
        } catch (Exception error) {
            finish(false, "Could not start the background import: " + error.getMessage());
            return START_NOT_STICKY;
        }
        worker.execute(() -> {
            try {
                if (mod) {
                    JSONObject installed = new ModPackages(this).install(tree, new ModPackages.Progress() {
                        @Override public void checkCancelled() throws IOException {
                            if (cancelled || Thread.currentThread().isInterrupted()) throw new IOException(cancellationMessage);
                        }
                        @Override public void update(String phase, String message, long bytes, long total, int files) {
                            lastPhase = phase;
                            try {
                                int percent = phase.equals("verifying") || phase.equals("finishing") ? 99
                                        : total > 0 ? (int) Math.min(98, bytes * 100.0 / total) : -1;
                                publish(new JSONObject().put("target", "mod").put("running", true).put("phase", phase)
                                        .put("message", message).put("percent", percent).put("bytes", bytes)
                                        .put("total_bytes", total).put("files", files).put("total_files", -1)
                                        .put("log", new JSONArray().put(message)), true);
                            } catch (Exception error) { android.util.Log.w("ModImport", "Could not update progress", error); }
                        }
                    });
                    finish(true, installed.getString("name") + " installed. Enable it in Mods.");
                    return;
                }
                GameFileImporter.Source source = new GameFileImporter.Source() {
                    @Override public List<GameFileImporter.Document> children(String id) throws IOException {
                        List<GameFileImporter.Document> children = new ArrayList<>();
                        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
                        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
                        try (Cursor cursor = getContentResolver().query(uri, columns, null, null, null, queryCancellation)) {
                            if (cursor == null) throw new IOException("Cannot read the selected folder. Choose a local extracted copy using the system Files picker.");
                            while (cursor.moveToNext()) children.add(new GameFileImporter.Document(cursor.getString(0), cursor.getString(1),
                                    DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2)), cursor.isNull(3) ? -1 : cursor.getLong(3)));
                        }
                        return children;
                    }
                    @Override public InputStream open(String id) throws IOException {
                        return getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                    }
                };
                GameFileImporter.Listener listener = new GameFileImporter.Listener() {
                    @Override public void checkCancelled() throws IOException {
                        if (cancelled || Thread.currentThread().isInterrupted()) throw new IOException(cancellationMessage);
                    }
                    @Override public void progress(String phase, String message, long bytes, long totalBytes, int files, int totalFiles) {
                        long now = SystemClock.elapsedRealtime();
                        boolean phaseChanged = !lastPhase.equals(phase);
                        if (!phaseChanged && now - lastProgress < 300) return;
                        lastProgress = now; lastPhase = phase;
                        if (phase.equals("copying") && copyStarted == 0) copyStarted = now;
                        if (iso && phase.equals("copying")) {
                            if (message.startsWith("Copying ")) message = "Extracting " + message.substring(8);
                            else if (message.startsWith("Copied ")) message = "Extracted " + message.substring(7);
                        }
                        try {
                            int percent = phase.equals("scanning") ? -1 : phase.equals("finishing") ? 99
                                    : phase.equals("verifying") ? 99 : totalBytes > 0
                                    ? (int) Math.min(99, bytes * 100.0 / totalBytes)
                                    : totalFiles > 0 ? Math.min(99, files * 100 / totalFiles) : 0;
                            JSONObject update = new JSONObject().put("running", true).put("phase", phase).put("message", message)
                                    .put("percent", percent).put("bytes", bytes).put("total_bytes", totalBytes)
                                    .put("files", files).put("total_files", totalFiles).put("log", new JSONArray().put(message));
                            if (totalBytes > bytes && bytes > 0 && copyStarted > 0 && now - copyStarted >= 10000) {
                                update.put("remaining_seconds", (long) ((totalBytes - bytes) * ((now - copyStarted) / 1000.0) / bytes));
                            }
                            publish(update, phaseChanged);
                        } catch (Exception error) { android.util.Log.w("GameImport", "Could not update import progress", error); }
                    }
                };
                File storage = getExternalFilesDir(null);
                if (storage == null) throw new IOException("Device storage is unavailable.");
                if (iso) extractIso(tree, listener, storage);
                else new GameFileImporter(source, listener, storage).run(DocumentsContract.getTreeDocumentId(tree));
                finish(true, "Installed successfully. Ready to play.");
            } catch (Exception error) {
                String message = error.getMessage();
                if (iso && message != null && message.startsWith("No default.xex")) {
                    message = "This ISO contains no default.xex. Choose the Xbox 360 edition of The Simpsons Game.";
                }
                finish(false, cancelled ? cancellationMessage : "Import failed: " + message);
            }
        });
        return START_NOT_STICKY;
    }
    private void extractIso(Uri uri, GameFileImporter.Listener listener, File storage) throws IOException {
        File cached = new File(storage, "install-source.iso.partial");
        if (cached.exists() && !cached.delete()) throw new IOException("Could not clear the interrupted ISO transfer.");
        ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(uri, "r", queryCancellation);
        if (descriptor == null) throw new IOException("Could not open the selected ISO.");
        try (FileInputStream input = new ParcelFileDescriptor.AutoCloseInputStream(descriptor)) {
            FileChannel channel = input.getChannel();
            boolean seekable;
            try {
                channel.read(ByteBuffer.allocate(1), 0);
                channel.size();
                seekable = true;
            } catch (IOException error) { seekable = false; }
            if (seekable) {
                try (XboxIsoSource source = new XboxIsoSource(channel, listener)) {
                    new GameFileImporter(source, listener, storage).run("root");
                }
                return;
            }
        }
        // Some cloud providers return pipes. Cache those once so disc reads can seek;
        // local Files/Downloads and USB providers extract directly from the selected file.
        long total = -1;
        try (Cursor cursor = getContentResolver().query(uri, new String[]{OpenableColumns.SIZE}, null, null, null, queryCancellation)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) total = cursor.getLong(0);
        }
        if (total > Math.max(0, storage.getUsableSpace() - 32L * 1024 * 1024)) {
            throw new IOException("This storage provider needs a temporary local ISO copy. Free more space or choose an ISO already stored in Files or Downloads.");
        }
        try {
            long copied = 0;
            listener.progress("reading_iso", "Reading the ISO from your storage provider…", 0, total, 0, 0);
            try (InputStream input = getContentResolver().openInputStream(uri); FileOutputStream output = new FileOutputStream(cached)) {
                if (input == null) throw new IOException("Could not read the selected ISO.");
                byte[] buffer = new byte[256 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    listener.checkCancelled();
                    output.write(buffer, 0, count); copied += count;
                    listener.progress("reading_iso", "Reading the ISO from your storage provider…", copied, total, 0, 0);
                }
                output.getFD().sync();
            }
            if (total >= 0 && copied != total) throw new IOException("The ISO transfer ended early. Choose a complete local copy and retry.");
            listener.checkCancelled();
            try (RandomAccessFile file = new RandomAccessFile(cached, "r"); XboxIsoSource source = new XboxIsoSource(file.getChannel(), listener)) {
                new GameFileImporter(source, listener, storage).run("root");
            }
        } finally {
            if (cached.exists() && !cached.delete()) android.util.Log.w("GameImport", "Could not remove the temporary ISO copy");
        }
    }
    private void finish(boolean ok, String message) {
        try {
            JSONObject update = snapshot(this).put("running", false).put("ok", ok).put("message", message)
                    .put("phase", ok ? "complete" : cancelled ? "cancelled" : "failed").put("log", new JSONArray().put(message));
            if (ok) update.put("percent", 100);
            // Detach before replacing the progress notification with a completion notification.
            stopForeground(STOP_FOREGROUND_DETACH);
            publish(update, true);
        } catch (Exception error) { android.util.Log.e("GameImport", "Could not save import result", error); }
        finally {
            completed = true;
            synchronized (GameImportService.class) { if (destroyed) active = false; }
            releaseWakeLock();
            stopSelf();
        }
    }
    private synchronized void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
    }
    @Override public void onTimeout(int startId, int type) {
        cancellationMessage = "Android's background installation time limit was reached. Choose the folder again to retry; your previous game files are kept.";
        cancelled = true;
        queryCancellation.cancel();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }
    @Override public void onDestroy() {
        destroyed = true;
        if (!completed) { cancelled = true; queryCancellation.cancel(); }
        synchronized (GameImportService.class) { if (completed) active = false; }
        releaseWakeLock();
        worker.shutdown();
        super.onDestroy();
    }
    @Override public IBinder onBind(Intent intent) { return null; }
}
