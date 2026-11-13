package io.github.yestermester.simpsons;

import android.content.Context;
import android.graphics.Bitmap;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONArray;

/**
 * The launcher's pictures, taken from the cutscenes of the imported game the way the desktop
 * launcher does it (launcher.py, _generate_art_locked): HD in-game cutscenes first, three moments
 * of each, dark and repeated frames left out. libsimpsons_art decodes the VP6 video.
 */
final class LauncherArt {
    private static final String MARKER = ".generated";
    private static final double[] MOMENTS = {3.0, 8.0, 14.0};
    private final File gameDir, folder;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile boolean running, unavailable;

    LauncherArt(Context context, File gameDir, File folder) {
        this.gameDir = gameDir;
        this.folder = folder;
    }

    static native int[][] nativeFrames(String path, double[] seconds);

    boolean running() { return running; }

    /** Makes the pictures in the background, once unless forced. */
    synchronized void generate(boolean force) {
        if (running || unavailable || (!force && new File(folder, MARKER).exists())) return;
        running = true;
        worker.execute(() -> {
            try { make(); }
            catch (UnsatisfiedLinkError error) {
                unavailable = true;
                android.util.Log.w("LauncherArt", "The picture decoder is missing", error);
            } catch (Exception error) { android.util.Log.w("LauncherArt", "Could not make the launcher's pictures", error); }
            finally { running = false; }
        });
    }

    void shutdown() { worker.shutdownNow(); }

    JSONArray pictures() {
        JSONArray result = new JSONArray();
        for (File picture : current()) result.put(picture.getName());
        return result;
    }

    long version() {
        long newest = 0;
        for (File picture : current()) newest = Math.max(newest, picture.lastModified() / 1000);
        return newest;
    }

    /** A picture by its name in the launcher page, or null. */
    File picture(String name) {
        if (!name.matches("hero[0-9]{1,3}\\.jpg")) return null;
        File file = new File(folder, name);
        return file.isFile() ? file : null;
    }

    private List<File> current() {
        File[] files = folder.listFiles((dir, name) -> name.matches("hero[0-9]{1,3}\\.jpg"));
        List<File> result = files == null ? new ArrayList<>() : new ArrayList<>(Arrays.asList(files));
        result.sort((a, b) -> a.getName().compareTo(b.getName()));
        return result;
    }

    private void make() throws Exception {
        System.loadLibrary("simpsons_art");
        if (!folder.isDirectory() && !folder.mkdirs()) throw new java.io.IOException("Cannot create the picture folder");
        File movies = new File(gameDir, "movies");
        File language = new File(movies, "en");
        if (!language.isDirectory()) {
            // Other languages keep their movies under their own code.
            File[] languages = movies.listFiles(File::isDirectory);
            if (languages == null || languages.length == 0) return;
            Arrays.sort(languages);
            language = languages[0];
        }
        File[] all = language.listFiles((dir, name) -> name.toLowerCase(Locale.ROOT).endsWith(".vp6"));
        if (all == null) return;
        Arrays.sort(all);
        // HD in-game cutscenes first, then the other movies, never the logos.
        List<File> candidates = new ArrayList<>();
        for (File movie : all) {
            String name = movie.getName().toLowerCase(Locale.ROOT);
            if (name.contains("_igc") && !name.contains("_sd")) candidates.add(movie);
        }
        for (File movie : all) {
            String name = movie.getName().toLowerCase(Locale.ROOT);
            if (!candidates.contains(movie) && !name.contains("_sd") && !name.contains("logo")) candidates.add(movie);
        }
        for (File stale : listing(".new", ".fb")) stale.delete();
        List<File> made = new ArrayList<>(), fallback = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        int attempts = 0;
        for (File movie : candidates) {
            if (made.size() >= 24 || attempts >= 48) break;
            if (Thread.currentThread().isInterrupted()) return;
            int[][] frames = nativeFrames(movie.getPath(), MOMENTS);
            for (int[] frame : frames == null ? new int[0][] : frames) {
                if (made.size() >= 24 || attempts >= 48) break;
                attempts++;
                if (frame == null || frame.length < 3) continue;
                int width = frame[0], height = frame[1];
                if (width <= 0 || height <= 0 || frame.length != 2 + width * height) continue;
                Bitmap bitmap = Bitmap.createBitmap(frame, 2, width, width, height, Bitmap.Config.ARGB_8888);
                ByteArrayOutputStream jpeg = new ByteArrayOutputStream();
                bitmap.compress(Bitmap.CompressFormat.JPEG, 85, jpeg);
                bitmap.recycle();
                byte[] data = jpeg.toByteArray();
                if (!seen.add(Arrays.toString(MessageDigest.getInstance("SHA-1").digest(data)))) continue;
                // Small files are dark or flat frames; middling ones are kept in case nothing better turns up.
                if (data.length > 45000) made.add(write(new File(folder, ".new" + attempts + ".jpg"), data));
                else if (data.length > 12000) fallback.add(write(new File(folder, ".fb" + attempts + ".jpg"), data));
            }
        }
        if (made.isEmpty()) {
            fallback.sort((a, b) -> Long.compare(b.length(), a.length()));
            made.addAll(fallback.subList(0, Math.min(8, fallback.size())));
        }
        if (!made.isEmpty()) {
            for (File old : current()) old.delete();
            for (int i = 0; i < made.size(); i++) made.get(i).renameTo(new File(folder, "hero" + i + ".jpg"));
        }
        for (File leftover : listing(".new", ".fb")) leftover.delete();
        write(new File(folder, MARKER), ("made=" + made.size() + " attempts=" + attempts + "\n").getBytes());
    }

    private List<File> listing(String... prefixes) {
        List<File> result = new ArrayList<>();
        File[] files = folder.listFiles();
        if (files != null) for (File file : files) for (String prefix : prefixes) if (file.getName().startsWith(prefix)) result.add(file);
        return result;
    }

    private static File write(File file, byte[] data) throws java.io.IOException {
        try (FileOutputStream output = new FileOutputStream(file)) { output.write(data); }
        return file;
    }
}
