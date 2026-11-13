package io.github.yestermester.simpsons;

import android.content.Context;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.json.JSONArray;
import org.json.JSONObject;
import org.json.JSONTokener;

/** Source projects share the desktop templates and the native package validator. */
final class ModProjects {
    private static final int MAX_TEXT = 128 * 1024;
    private static final Set<String> TEXT_TYPES = new HashSet<>(Arrays.asList("lua", "json", "md", "txt", "ini", "cfg", "xml"));
    private final File root;
    private final Context context;
    ModProjects(Context context, ModPackages packages) { this.context = context; root = packages.root; }

    private static String utf8(byte[] data) throws Exception {
        return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(data)).toString();
    }
    private JSONObject catalog() throws Exception {
        try (InputStream input = context.getAssets().open("launcher/modkit-templates.json");
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] bytes = new byte[8192]; int size;
            while ((size = input.read(bytes)) >= 0) {
                if (output.size() + size > 4 * 1024 * 1024) throw new IOException("Project templates are too large");
                output.write(bytes, 0, size);
            }
            JSONObject data = new JSONObject(utf8(output.toByteArray()));
            if (data.optInt("schema") != 1 || data.optInt("api") != 1)
                throw new IOException("These project templates need a different Modkit version");
            return data;
        }
    }
    private static JSONObject manifest(File folder, boolean contents) throws Exception {
        return ModPackages.checked(ModPackages.validateNative(folder.getAbsolutePath(), contents)).getJSONObject("mod");
    }
    private static String field(JSONObject request, String key, int maximum, boolean required) throws Exception {
        Object value = request.opt(key);
        if (value == null) value = "";
        if (!(value instanceof String)) throw new IOException(key + " must be text");
        String text = ((String) value).trim();
        if (text.getBytes(StandardCharsets.UTF_8).length > maximum || (required && text.isEmpty()))
            throw new IOException(key + " needs readable text of at most " + maximum + " UTF-8 bytes");
        for (int i = 0; i < text.length(); ++i)
            if (text.charAt(i) < 32) throw new IOException(key + " cannot contain control characters");
        return text;
    }
    private static File inside(File folder, String relative) throws IOException {
        if (relative == null || relative.isEmpty() || relative.getBytes(StandardCharsets.UTF_8).length > 512
                || relative.matches(".*[\\\\:<>\"|?*].*")) throw new IOException("Use a relative source path");
        String[] parts = relative.split("/", -1);
        if (parts.length > 64) throw new IOException("The source path is too deep");
        File current = folder.getCanonicalFile();
        for (String part : parts) {
            if (part.isEmpty() || part.equals(".") || part.equals("..") || part.endsWith(".") || part.endsWith(" "))
                throw new IOException("Invalid source path");
            for (int i = 0; i < part.length(); ++i) if (part.charAt(i) < 32) throw new IOException("Invalid source path");
            if (part.split("\\.")[0].toUpperCase(Locale.ROOT).matches("CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]"))
                throw new IOException("A source path uses a reserved Windows name");
            current = new File(current, part);
            if (!current.getAbsolutePath().equals(current.getCanonicalPath()))
                throw new IOException("Source paths cannot contain symbolic links");
        }
        return current;
    }
    private File find(String id) throws Exception {
        if (id == null || !id.matches("[a-z0-9][a-z0-9_.-]{0,95}")) throw new IOException("Choose a mod project");
        File[] folders = root.listFiles();
        if (folders != null) for (File folder : folders) {
            if (!folder.isDirectory() || folder.getName().startsWith(".") || !new File(folder, "mod.json").isFile()) continue;
            if (!folder.getAbsolutePath().equals(folder.getCanonicalPath())) throw new IOException("Mod folders cannot be symbolic links");
            JSONObject m = manifest(folder, false);
            if (!id.equals(m.getString("id"))) continue;
            File marker = inside(folder, ".modkit/project.json");
            if (!marker.isFile() || new JSONObject(utf8(Files.readAllBytes(marker.toPath()))).optInt("schema") != 1)
                break;
            return folder;
        }
        throw new IOException("This mod is not an editable creator project");
    }
    private static boolean editable(String path) {
        int dot = path.lastIndexOf('.');
        return !path.startsWith(".") && dot >= 0 && TEXT_TYPES.contains(path.substring(dot + 1).toLowerCase(Locale.ROOT));
    }
    private static void gather(File folder, String prefix, TreeMap<String, File> files, Set<String> names, long[] total, int depth) throws Exception {
        if (depth > 64) throw new IOException("Mod folder tree is too deep");
        File[] children = folder.listFiles();
        if (children == null) throw new IOException("Cannot read project sources");
        for (File child : children) {
            String path = prefix + child.getName();
            if (!child.getAbsolutePath().equals(child.getCanonicalPath())) throw new IOException("Project sources cannot contain symbolic links");
            if (child.isDirectory()) {
                if (!child.getName().startsWith(".") && !child.getName().equals("build") && !child.getName().equals("__pycache__"))
                    gather(child, path + "/", files, names, total, depth + 1);
            } else {
                if (!child.isFile()) throw new IOException("Project sources must be regular files");
                if (path.equals("checksums.json")) continue;
                StringBuilder key = new StringBuilder(path);
                for (int i = 0; i < key.length(); ++i) if (key.charAt(i) >= 'A' && key.charAt(i) <= 'Z')
                    key.setCharAt(i, (char) (key.charAt(i) + 32));
                if (!names.add(key.toString())) throw new IOException("Source paths differ only in letter case");
                total[0] += child.length();
                files.put(path, child);
                if (total[0] > 8L * 1024 * 1024 * 1024 || files.size() > 100000) throw new IOException("Project exceeds package limits");
            }
        }
    }
    private static TreeMap<String, File> files(File folder) throws Exception {
        TreeMap<String, File> result = new TreeMap<>();
        gather(folder, "", result, new HashSet<>(), new long[]{0}, 0);
        return result;
    }
    private static JSONObject describe(File folder) throws Exception {
        JSONObject m = manifest(folder, false);
        JSONArray paths = new JSONArray();
        for (Map.Entry<String, File> entry : files(folder).entrySet())
            if (editable(entry.getKey()) && entry.getValue().length() <= MAX_TEXT && paths.length() < 256) paths.put(entry.getKey());
        return new JSONObject().put("id", m.getString("id")).put("name", m.getString("name"))
                .put("author", m.optString("author")).put("version", m.getString("version"))
                .put("files", paths).put("folder", folder.getAbsolutePath()).put("has_scene", m.has("scene"))
                .put("documents", documentKinds(m));
    }
    private static JSONArray documentKinds(JSONObject manifest) {
        JSONArray result = new JSONArray();
        for (String kind : new String[]{"scene", "mission"}) if (manifest.has(kind)) result.put(kind);
        return result;
    }
    private static void writeText(File file, String text) throws Exception {
        File parent = file.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create project folder");
        Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }
    private static void remove(File file) throws IOException {
        if (!file.exists()) return;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) throw new IOException("Cannot clean up temporary project");
            for (File child : children) remove(child);
        }
        Files.delete(file.toPath());
    }
    private JSONObject create(JSONObject request) throws Exception {
        JSONObject data = catalog(), template = null;
        JSONArray choices = data.getJSONArray("templates");
        for (int i = 0; i < choices.length(); ++i)
            if (choices.getJSONObject(i).getString("id").equals(request.optString("template"))) template = choices.getJSONObject(i);
        if (template == null) throw new IOException("Choose a project template");
        String name = field(request, "name", 160, true), id = request.optString("id");
        if (id.isEmpty()) id = name.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9_.-]+", "-").replaceAll("^[.\\-_]+|[.\\-_]+$", "");
        if (!id.matches("[a-z0-9][a-z0-9_.-]{0,95}")) throw new IOException("Choose a stable id such as yourname.my-mod");
        String author = field(request, "author", 160, false), description = field(request, "description", 1024, false);
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot prepare project storage");
        File destination = inside(root, id);
        File[] folders = root.listFiles();
        if (destination.exists()) throw new IOException("That project folder already exists");
        if (folders != null) for (File folder : folders)
            if (folder.isDirectory() && !folder.getName().startsWith(".") && new File(folder, "mod.json").isFile()
                    && manifest(folder, false).getString("id").equals(id)) throw new IOException("That mod id already exists");
        File staging = new File(root, ".create-" + UUID.randomUUID());
        if (!staging.mkdir()) throw new IOException("Cannot prepare project folder");
        try {
            JSONObject m = new JSONObject(template.getJSONObject("manifest").toString());
            m.put("id", id).put("name", name).put("author", author).put("version", "1.0.0").put("api", 1)
                    .put("description", description.isEmpty() ? template.getString("description") : description);
            if (!new File(staging, "gamedata").mkdir()) throw new IOException("Cannot prepare replacement folder");
            JSONObject sources = template.getJSONObject("files");
            Iterator<String> paths = sources.keys();
            while (paths.hasNext()) { String path = paths.next(); writeText(inside(staging, path), sources.getString(path)); }
            writeText(new File(staging, "mod.json"), m.toString(2) + "\n");
            writeText(inside(staging, ".modkit/project.json"), new JSONObject().put("schema", 1).put("template", template.getString("id")).toString());
            if (m.getString("kind").equals("lua")) {
                writeText(inside(staging, ".modkit/api/game.lua"), data.getString("completion"));
                writeText(new File(staging, ".luarc.json"), new JSONObject().put("runtime.version", "Lua 5.4")
                        .put("workspace.library", new JSONArray().put(".modkit/api"))
                        .put("diagnostics.globals", new JSONArray().put("game")).toString(2) + "\n");
            }
            manifest(staging, true);
            Files.move(staging.toPath(), destination.toPath());
            return describe(destination);
        } finally { remove(staging); }
    }
    private static String hex(byte[] bytes) {
        final char[] alphabet = "0123456789abcdef".toCharArray();
        StringBuilder text = new StringBuilder();
        for (byte b : bytes) { text.append(alphabet[(b & 255) >>> 4]); text.append(alphabet[b & 15]); }
        return text.toString();
    }
    private static String digest(byte[] bytes) throws Exception { return hex(MessageDigest.getInstance("SHA-256").digest(bytes)); }
    private static File source(File folder, String path) throws Exception {
        File file = inside(folder, path);
        if (!editable(path) || !file.isFile()) throw new IOException("Choose an existing text source file");
        if (file.length() > MAX_TEXT) throw new IOException("The source editor supports files up to 128 KiB");
        return file;
    }
    private static JSONObject read(File folder, String path) throws Exception {
        byte[] bytes = Files.readAllBytes(source(folder, path).toPath());
        return new JSONObject().put("path", path).put("text", utf8(bytes)).put("revision", digest(bytes));
    }
    private static JSONObject write(File folder, JSONObject request) throws Exception {
        String path = request.getString("path"), content = request.getString("text");
        File file = source(folder, path);
        if (content.getBytes(StandardCharsets.UTF_8).length > MAX_TEXT || content.indexOf('\0') >= 0) throw new IOException("Source must be text of at most 128 KiB");
        if (!read(folder, path).getString("revision").equals(request.getString("revision")))
            throw new IOException("This file changed since you opened it. Reopen it before saving");
        if (path.toLowerCase(Locale.ROOT).endsWith(".json")) {
            JSONTokener parser = new JSONTokener(content);
            Object value = parser.nextValue();
            if (parser.nextClean() != 0) throw new IOException("Unexpected text after JSON");
            if (path.equals("mod.json") && (!(value instanceof JSONObject)
                    || !((JSONObject) value).optString("id").equals(manifest(folder, false).getString("id"))))
                throw new IOException("Keep this project's stable mod id");
        }
        File temporary = new File(file.getParentFile(), "." + file.getName() + ".source-" + UUID.randomUUID());
        File probe = null;
        try {
            if (path.equals("mod.json")) {
                probe = new File(folder, ".metadata-" + UUID.randomUUID());
                if (!probe.mkdir()) throw new IOException("Cannot validate project metadata");
                writeText(new File(probe, "mod.json"), content); manifest(probe, false);
            }
            writeText(temporary, content);
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { remove(temporary); if (probe != null) remove(probe); }
        return read(folder, path).put("msg", "Source saved. Restart or reload the mod to apply it.");
    }
    private JSONObject validate(String id) throws Exception {
        JSONObject m = manifest(find(id), true);
        JSONArray order = ModPackages.checked(ModPackages.resolveNative(root.getAbsolutePath(),
                new JSONArray().put(id).toString(), true)).getJSONArray("mods");
        JSONArray ids = new JSONArray();
        for (int i = 0; i < order.length(); ++i) ids.put(order.getJSONObject(i).getString("id"));
        return new JSONObject().put("id", id).put("fingerprint", m.getString("fingerprint")).put("load_order", ids)
                .put("msg", "Package structure, files and dependencies are valid. Test checks script behavior in the game.");
    }
    synchronized JSONObject prepareTest(String id) throws Exception {
        validate(id);
        File session = inside(new File(context.getFilesDir(), "simpsons"), "mod-creator-tests/" + id);
        if (!session.isDirectory() && !session.mkdirs()) throw new IOException("Cannot prepare private test progress");
        File profile = new File(session, "mods-profile.json");
        writeText(profile, new JSONObject().put("version", 1).put("enabled", new JSONArray().put(id)).toString());
        return new JSONObject().put("user_data", session.getAbsolutePath()).put("profile", profile.getAbsolutePath());
    }
    private static boolean sameJson(Object a, Object b) throws Exception {
        if (a instanceof JSONObject && b instanceof JSONObject) {
            JSONObject left = (JSONObject) a, right = (JSONObject) b;
            if (left.length() != right.length()) return false;
            Iterator<String> keys = left.keys();
            while (keys.hasNext()) {
                String key = keys.next();
                if (!right.has(key) || !sameJson(left.get(key), right.get(key))) return false;
            }
            return true;
        }
        if (a instanceof JSONArray && b instanceof JSONArray) {
            JSONArray left = (JSONArray) a, right = (JSONArray) b;
            if (left.length() != right.length()) return false;
            for (int i = 0; i < left.length(); ++i) if (!sameJson(left.get(i), right.get(i))) return false;
            return true;
        }
        if (a instanceof Number && b instanceof Number)
            return Double.compare(((Number) a).doubleValue(), ((Number) b).doubleValue()) == 0;
        return a == b || (a != null && a.equals(b));
    }
    private JSONObject applyDrafts(File folder, boolean sceneOnly) throws Exception {
        JSONObject m = manifest(folder, true), updates = new JSONObject();
        JSONArray kinds = sceneOnly ? new JSONArray().put("scene") : documentKinds(m), applied = new JSONArray();
        if (kinds.length() == 0) throw new IOException("Choose a scene or mission editor project");
        int found = 0, objects = 0, objectives = 0;
        File combinedFile = inside(new File(context.getFilesDir(), "simpsons"),
                "mod-creator-tests/" + m.getString("id") + "/mod-storage/" + m.getString("id") + "/project-drafts.json");
        JSONObject combined = null;
        if (combinedFile.isFile()) {
            if (combinedFile.length() > 1024 * 1024) throw new IOException("The project drafts exceed their size limit");
            combined = new JSONObject(utf8(Files.readAllBytes(combinedFile.toPath())));
            if (combined.length() != 3 || !(combined.opt("schema") instanceof Integer) || combined.getInt("schema") != 1
                    || !(combined.opt("documents") instanceof JSONObject) || !combined.optString("fingerprint").matches("[0-9a-f]{64}"))
                throw new IOException("Invalid combined project drafts");
            JSONObject documents = combined.getJSONObject("documents");
            if (documents.length() < 1 || documents.length() > 2) throw new IOException("Choose scene or mission drafts");
            Iterator<String> keys = documents.keys();
            while (keys.hasNext()) {
                String key = keys.next();
                if ((!key.equals("scene") && !key.equals("mission")) || !(documents.opt(key) instanceof JSONObject))
                    throw new IOException("Unknown project draft document");
            }
        }
        for (int index = 0; index < kinds.length(); ++index) {
            String kind = kinds.getString(index);
            if (!m.has(kind)) throw new IOException("Choose a " + kind + " editor project");
            JSONObject document = null; String fingerprint = "";
            if (combined != null && combined.getJSONObject("documents").has(kind)) {
                document = combined.getJSONObject("documents").getJSONObject(kind);
                fingerprint = combined.getString("fingerprint");
            }
            File draftFile = inside(new File(context.getFilesDir(), "simpsons"),
                    "mod-creator-tests/" + m.getString("id") + "/mod-storage/" + m.getString("id") + "/" + kind + "-draft.json");
            if (draftFile.isFile()) {
                if (draftFile.length() > 1024 * 1024) throw new IOException("The project draft exceeds its size limit");
                JSONObject draft = new JSONObject(utf8(Files.readAllBytes(draftFile.toPath())));
                if (draft.length() != 3 || !(draft.opt("schema") instanceof Integer) || draft.getInt("schema") != 1
                        || !(draft.opt(kind) instanceof JSONObject) || !draft.optString("fingerprint").matches("[0-9a-f]{64}"))
                    throw new IOException("Invalid " + kind + " draft");
                if (document == null || draftFile.lastModified() > combinedFile.lastModified()) {
                    document = draft.getJSONObject(kind); fingerprint = draft.getString("fingerprint");
                }
            }
            if (document == null) continue;
            ++found;
            if (kind.equals("scene")) objects = document.getJSONArray("objects").length();
            else objectives = document.getJSONArray("objectives").length();
            if (sameJson(document, m.getJSONObject(kind))) continue;
            if (!fingerprint.equals(m.getString("fingerprint")))
                throw new IOException("Project sources changed after this draft. Test the current project and save a new draft");
            updates.put(kind, document); applied.put(kind);
        }
        if (found == 0) throw new IOException("Save a draft in this project's Test session, then close the game");
        if (applied.length() == 0) return new JSONObject().put("msg", sceneOnly ? "This scene draft is already applied"
                : "These saved drafts are already applied").put("documents", applied).put("objects", objects).put("objectives", objectives);
        JSONObject opened = read(folder, "mod.json"), source = new JSONObject(opened.getString("text"));
        for (int index = 0; index < applied.length(); ++index) {
            String kind = applied.getString(index); source.put(kind, updates.getJSONObject(kind));
        }
        if (!manifest(folder, true).getString("fingerprint").equals(m.getString("fingerprint")))
            throw new IOException("Project sources changed while applying the saved drafts");
        // One native-validated manifest replacement applies all documents, so
        // applying a scene cannot invalidate its matching mission draft.
        write(folder, new JSONObject().put("path", "mod.json").put("text", source.toString(2) + "\n")
                .put("revision", opened.getString("revision")));
        return new JSONObject().put("msg", sceneOnly ? "Scene applied to mod.json; Validate, Test or Export ZIP"
                : "Saved drafts applied to mod.json; Validate, Test or Export ZIP")
                .put("documents", applied).put("objects", objects).put("objectives", objectives);
    }
    synchronized JSONObject request(JSONObject data) throws Exception {
        String action = data.getString("action");
        if (action.equals("templates")) {
            JSONArray result = new JSONArray(), all = catalog().getJSONArray("templates");
            for (int i = 0; i < all.length(); ++i) {
                JSONObject item = all.getJSONObject(i);
                result.put(new JSONObject().put("id", item.getString("id")).put("name", item.getString("name"))
                        .put("description", item.getString("description")));
            }
            return new JSONObject().put("templates", result);
        }
        if (action.equals("create")) return new JSONObject().put("project", create(data))
                .put("msg", "Project created, disabled. Edit its source, then Validate and Test.");
        File folder = find(data.getString("id"));
        switch (action) {
            case "open": return new JSONObject().put("project", describe(folder));
            case "read": return read(folder, data.getString("path"));
            case "write": return write(folder, data);
            case "validate": return validate(data.getString("id"));
            case "apply-scene": return applyDrafts(folder, true);
            case "apply-drafts": return applyDrafts(folder, false);
            default: throw new IOException("Unknown project action");
        }
    }
    synchronized void export(String id, OutputStream output) throws Exception {
        validate(id);
        File folder = find(id);
        JSONObject checksums = new JSONObject();
        try (ZipOutputStream zip = new ZipOutputStream(output)) {
            byte[] buffer = new byte[65536];
            for (Map.Entry<String, File> entry : files(folder).entrySet()) {
                MessageDigest hash = MessageDigest.getInstance("SHA-256");
                ZipEntry item = new ZipEntry(entry.getKey()); item.setTime(1577836800000L); zip.putNextEntry(item);
                try (InputStream input = Files.newInputStream(entry.getValue().toPath())) {
                    int size;
                    while ((size = input.read(buffer)) != -1) { zip.write(buffer, 0, size); hash.update(buffer, 0, size); }
                }
                zip.closeEntry();
                checksums.put(entry.getKey(), hex(hash.digest()));
            }
            zip.putNextEntry(new ZipEntry("checksums.json")); zip.write(checksums.toString(2).getBytes(StandardCharsets.UTF_8)); zip.closeEntry();
        }
    }
}
