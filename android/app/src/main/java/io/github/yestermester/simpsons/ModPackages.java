package io.github.yestermester.simpsons;

import android.content.Context;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.database.Cursor;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.InputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.json.JSONArray;
import org.json.JSONObject;

/** Uses the C++ runtime's manifest, fingerprint and dependency implementation. */
final class ModPackages {
    static { System.loadLibrary("simpsons_art"); }
    static native String validateNative(String folder, boolean contents);
    static native String resolveNative(String root, String enabled, boolean contents);
    private final Context context;
    final File root;

    ModPackages(Context context) {
        this.context = context;
        root = new File(context.getFilesDir(), "simpsons/mods");
    }
    interface Progress {
        void checkCancelled() throws IOException;
        void update(String phase, String message, long bytes, long total, int files);
    }
    static JSONObject checked(String response) throws Exception {
        JSONObject result = new JSONObject(response);
        if (!result.optBoolean("ok")) throw new IOException(result.optString("msg", "Mod validation failed"));
        return result;
    }
    private File profileFile() { return new File(root, "profiles/default.json"); }
    private JSONObject profile() throws Exception {
        File file = profileFile();
        JSONObject result = file.isFile() ? new JSONObject(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8))
                : new JSONObject().put("version", 1).put("enabled", new JSONArray());
        if (result.optInt("version") != 1 || !(result.opt("enabled") instanceof JSONArray))
            throw new IOException("Invalid mod profile");
        Set<String> ids = new HashSet<>();
        JSONArray enabled = result.getJSONArray("enabled");
        for (int i = 0; i < enabled.length(); ++i)
            if (!(enabled.get(i) instanceof String) || !enabled.getString(i).matches("[a-z0-9][a-z0-9_.-]{0,95}") || !ids.add(enabled.getString(i)))
                throw new IOException("Mod profile needs unique mod ids");
        return result;
    }
    synchronized JSONArray list() throws Exception {
        JSONArray selected = profile().getJSONArray("enabled");
        JSONArray resolved = checked(resolveNative(root.getAbsolutePath(), selected.toString(), false)).getJSONArray("mods");
        Set<String> active = new HashSet<>();
        for (int i = 0; i < resolved.length(); ++i) active.add(resolved.getJSONObject(i).getString("id"));
        JSONArray result = new JSONArray();
        File[] folders = root.listFiles();
        if (folders == null) return result;
        Arrays.sort(folders, (a,b) -> a.getName().compareTo(b.getName()));
        for (File folder : folders) {
            if (!folder.isDirectory() || folder.getName().startsWith(".") || !new File(folder, "mod.json").isFile()) continue;
            JSONObject m = checked(validateNative(folder.getAbsolutePath(), false)).getJSONObject("mod");
            String id = m.getString("id");
            int priority = -1;
            for (int i = 0; i < selected.length(); ++i) if (id.equals(selected.getString(i))) priority = i;
            m.put("enabled", active.contains(id)).put("requested", priority >= 0).put("priority", priority);
            m.put("editable", new File(folder, ".modkit/project.json").isFile());
            result.put(m);
        }
        return result;
    }
    synchronized JSONObject change(JSONObject request) throws Exception {
        JSONObject previous = profile();
        JSONArray old = previous.getJSONArray("enabled"), selected = new JSONArray();
        String action = request.getString("action");
        if (action.equals("enable")) {
            String id = request.getString("id");
            if (request.optBoolean("enabled")) selected.put(id);
            for (int i = 0; i < old.length(); ++i) if (!id.equals(old.getString(i))) selected.put(old.getString(i));
        } else if (action.equals("order")) {
            selected = request.getJSONArray("enabled");
            Set<String> a = new HashSet<>(), b = new HashSet<>();
            for (int i = 0; i < old.length(); ++i) a.add(old.getString(i));
            for (int i = 0; i < selected.length(); ++i) b.add(selected.getString(i));
            if (!a.equals(b) || selected.length() != b.size()) throw new IOException("Invalid mod order");
        } else throw new IOException("Unknown mod action");
        checked(resolveNative(root.getAbsolutePath(), selected.toString(), true));
        File file = profileFile();
        file.getParentFile().mkdirs();
        AtomicFile atomic = new AtomicFile(file);
        FileOutputStream output = null;
        try {
            output = atomic.startWrite();
            output.write(new JSONObject().put("version", 1).put("enabled", selected).toString().getBytes(StandardCharsets.UTF_8));
            atomic.finishWrite(output);
        } catch (Exception error) {
            if (output != null) atomic.failWrite(output);
            throw error;
        }
        return new JSONObject().put("ok", true).put("msg", "Mods updated.");
    }
    synchronized File preparePlaythrough() throws Exception {
        File original = new File(context.getFilesDir(), "simpsons");
        JSONArray selected = profile().getJSONArray("enabled");
        if (selected.length() == 0) return original;
        JSONArray resolved = checked(resolveNative(root.getAbsolutePath(), selected.toString(), true)).getJSONArray("mods");
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        for (int i = 0; i < resolved.length(); ++i) {
            JSONObject mod = resolved.getJSONObject(i);
            digest.update((mod.getString("id") + '\0' + mod.getString("version") + '\0'
                    + mod.getString("fingerprint") + '\n').getBytes(StandardCharsets.UTF_8));
        }
        StringBuilder key = new StringBuilder();
        for (byte b : digest.digest()) key.append(String.format(Locale.ROOT, "%02x", b & 255));
        File sessions = new File(original, "mod-playthroughs");
        if (!sessions.isDirectory() && !sessions.mkdirs()) throw new IOException("Cannot prepare mod progress");
        File destination = new File(sessions, key.toString());
        if (destination.exists()) {
            if (!new File(destination, "mod-session.json").isFile()) throw new IOException("Mod playthrough is incomplete");
            return destination;
        }
        File staging = new File(sessions, ".prepare-" + UUID.randomUUID());
        if (!staging.mkdir()) throw new IOException("Cannot prepare mod progress");
        try {
            File[] profiles = original.listFiles();
            long[] copied = {0};
            if (profiles != null) for (File file : profiles)
                if (file.isDirectory() && file.getName().matches("[0-9a-fA-F]{16}"))
                    copyProgress(file, new File(staging, file.getName()), copied);
            JSONArray credits = new JSONArray();
            for (int i = 0; i < resolved.length(); ++i) {
                JSONObject mod = resolved.getJSONObject(i);
                credits.put(new JSONObject().put("id", mod.getString("id")).put("version", mod.getString("version"))
                        .put("author", mod.optString("author")));
            }
            Files.write(new File(staging, "mod-session.json").toPath(), new JSONObject()
                    .put("api", 1).put("profile", key.toString()).put("mods", credits).toString().getBytes(StandardCharsets.UTF_8));
            Files.move(staging.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE);
        } finally { remove(staging); }
        return destination;
    }
    private static void copyProgress(File source, File destination, long[] copied) throws IOException {
        if (!source.getAbsolutePath().equals(source.getCanonicalPath()))
            throw new IOException("Game checkpoint folders cannot contain symbolic links");
        if (source.isDirectory()) {
            if (!destination.mkdir()) throw new IOException("Cannot prepare game checkpoint folder");
            File[] children = source.listFiles();
            if (children == null) throw new IOException("Cannot read game checkpoints");
            for (File child : children) copyProgress(child, new File(destination, child.getName()), copied);
        } else {
            copied[0] += source.length();
            if (copied[0] > 256L * 1024 * 1024) throw new IOException("Game checkpoint data exceeds 256 MiB");
            Files.copy(source.toPath(), destination.toPath());
        }
    }
    JSONObject install(Uri uri, Progress progress) throws Exception {
        root.mkdirs();
        File staging = new File(root, ".import-" + UUID.randomUUID());
        if (!staging.mkdir()) throw new IOException("Cannot prepare mod storage");
        long total = -1;
        try (Cursor cursor = context.getContentResolver().query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) total = cursor.getLong(0);
        } catch (Exception ignored) { /* Some storage providers do not report file sizes. */ }
        final long totalBytes = total;
        try {
            InputStream incoming = context.getContentResolver().openInputStream(uri);
            if (incoming == null) throw new IOException("Cannot open the selected mod package");
            CountingInputStream counted = new CountingInputStream(incoming);
            try (ZipInputStream archive = new ZipInputStream(counted)) {
                byte[] buffer = new byte[65536];
                Set<String> paths = new HashSet<>();
                int files = 0;
                long extracted = 0, lastUpdate = 0;
                for (ZipEntry entry; (entry = archive.getNextEntry()) != null;) {
                    progress.checkCancelled();
                    String name = entry.getName();
                    if (name.endsWith("/")) name = name.substring(0, name.length() - 1);
                    if (name.isEmpty() || name.startsWith("/") || name.contains("\\") || name.contains(":") || name.indexOf('\0') >= 0)
                        throw new IOException("Invalid path in mod package");
                    if (name.split("/").length > 64) throw new IOException("Mod folder tree is too deep");
                    for (String part : name.split("/")) if (part.equals("..") || part.equals("."))
                        throw new IOException("Invalid path in mod package");
                    File output = new File(staging, name).getCanonicalFile();
                    if (!output.getPath().startsWith(staging.getCanonicalPath() + File.separator))
                        throw new IOException("A mod path escaped its folder");
                    String key = output.getPath().substring(staging.getCanonicalPath().length()).toLowerCase(Locale.ROOT);
                    if (!paths.add(key) || paths.size() > 100000) throw new IOException("Duplicate paths or too many files in mod package");
                    if (entry.isDirectory()) { output.mkdirs(); continue; }
                    output.getParentFile().mkdirs();
                    try (FileOutputStream stream = new FileOutputStream(output)) {
                        for (int size; (size = archive.read(buffer)) != -1;) {
                            progress.checkCancelled();
                            extracted += size;
                            if (extracted > 8L * 1024 * 1024 * 1024) throw new IOException("Mod package exceeds 8 GiB");
                            stream.write(buffer, 0, size);
                            long now = android.os.SystemClock.elapsedRealtime();
                            if (now - lastUpdate > 300) {
                                lastUpdate = now;
                                progress.update("copying", "Extracting " + name, counted.count, totalBytes, files);
                            }
                        }
                    }
                    ++files;
                    archive.closeEntry();
                }
                progress.update("verifying", "Checking mod files…", counted.count, totalBytes, files);
            }
            progress.checkCancelled();
            JSONObject mod = checked(validateNative(staging.getAbsolutePath(), true)).getJSONObject("mod");
            File destination = new File(root, mod.getString("id"));
            if (destination.exists()) throw new IOException("This mod is already installed. Remove it before importing this version.");
            progress.checkCancelled();
            progress.update("finishing", "Finishing " + mod.getString("name"), totalBytes, totalBytes, pathsCount(staging));
            Files.move(staging.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE);
            return mod;
        } finally { remove(staging); }
    }
    private static int pathsCount(File folder) {
        File[] files = folder.listFiles();
        if (files == null) return 0;
        int count = 0;
        for (File file : files) count += file.isDirectory() ? pathsCount(file) : 1;
        return count;
    }
    private static void remove(File file) throws IOException {
        if (!file.exists()) return;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) remove(child);
        }
        if (!file.delete()) throw new IOException("Cannot remove temporary mod data");
    }
    private static final class CountingInputStream extends FilterInputStream {
        long count;
        CountingInputStream(InputStream input) { super(input); }
        @Override public int read() throws IOException { int value = in.read(); if (value >= 0) ++count; return value; }
        @Override public int read(byte[] bytes, int offset, int size) throws IOException {
            int read = in.read(bytes, offset, size); if (read > 0) count += read; return read;
        }
    }
}
