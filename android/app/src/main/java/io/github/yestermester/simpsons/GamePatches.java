package io.github.yestermester.simpsons;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;

/** Android file operations for the launcher's existing reversible game patches. */
final class GamePatches {
    private static final Pattern EPISODE = Pattern.compile("^([ \\t]*)episode\\s*=\\s*NewEpisode\\(\\s*game\\s*,\\s*\"([A-Za-z0-9_]+)\"", Pattern.MULTILINE);
    private static final Pattern DEFAULT = Pattern.compile("^[ \\t]*episode:SetDefault\\(\\)[^\\r\\n]*", Pattern.MULTILINE);
    private final File game;
    private final JSONObject definitions;
    GamePatches(File game, JSONObject definitions) { this.game = game; this.definitions = definitions; }

    private static File find(File directory, String name) throws IOException {
        File found = null;
        File[] children = directory.listFiles();
        if (children != null) for (File child : children) if (child.getName().equalsIgnoreCase(name)) {
            if (found != null) throw new IOException("More than one copy of " + name + " exists.");
            found = child;
        }
        return found;
    }
    private List<File> logos(boolean enabled) throws Exception {
        List<File> files = new ArrayList<>();
        File movies = find(game, "movies");
        if (movies == null || !movies.isDirectory()) return files;
        Set<String> names = new HashSet<>();
        JSONArray logos = definitions.getJSONArray("logo_movies");
        for (int i = 0; i < logos.length(); i++) names.add(logos.getString(i) + (enabled ? ".vp6" : ".vp6.disabled"));
        File[] languages = movies.listFiles();
        if (languages != null) for (File language : languages) if (language.isDirectory()) {
            File[] videos = language.listFiles();
            if (videos != null) for (File video : videos) if (video.isFile() && names.contains(video.getName().toLowerCase(Locale.ROOT))) files.add(video);
        }
        return files;
    }
    private String introState() throws Exception {
        boolean enabled = !logos(true).isEmpty(), disabled = !logos(false).isEmpty();
        return !enabled && !disabled ? "unavailable" : disabled && !enabled ? "on" : "off";
    }
    private JSONObject skipIntro(boolean enable) throws Exception {
        List<File> files = logos(enable), destinations = new ArrayList<>();
        if (files.isEmpty() && logos(!enable).isEmpty()) throw new IOException("Logo movies are not installed.");
        for (File source : files) {
            String name = source.getName();
            name = enable ? name + ".disabled" : name.substring(0, name.length() - ".disabled".length());
            if (find(source.getParentFile(), name) != null) throw new IOException("Both enabled and disabled copies of " + source.getName() + " exist.");
            destinations.add(new File(source.getParentFile(), name));
        }
        int moved = 0;
        try {
            for (; moved < files.size(); moved++) if (!files.get(moved).renameTo(destinations.get(moved))) throw new IOException("Could not update " + files.get(moved).getName());
        } catch (IOException error) {
            for (int i = moved - 1; i >= 0; i--) if (!destinations.get(i).renameTo(files.get(i))) {
                throw new IOException("Could not restore an intro movie after a failed patch: " + files.get(i).getName());
            }
            throw error;
        }
        return new JSONObject().put("ok", true).put("msg", (enable ? "Skipped " : "Restored ") + files.size() + " intro videos.");
    }
    private static final class Episode {
        final String id, indent;
        final int start, lineEnd;
        int end;
        Episode(Matcher match, String text) {
            id = match.group(2); indent = match.group(1); start = match.start();
            int newline = text.indexOf('\n', match.end());
            lineEnd = newline < 0 ? text.length() : newline + 1;
        }
    }
    private static List<Episode> episodes(String text) {
        List<Episode> result = new ArrayList<>();
        Matcher match = EPISODE.matcher(text);
        while (match.find()) result.add(new Episode(match, text));
        for (int i = 0; i < result.size(); i++) result.get(i).end = i + 1 < result.size() ? result.get(i + 1).start : text.length();
        return result;
    }
    private static String defaultEpisode(String text) {
        for (Episode episode : episodes(text)) if (DEFAULT.matcher(text).region(episode.start, episode.end).find()) return episode.id;
        return null;
    }
    private static int defaultCount(String text) {
        Matcher matcher = DEFAULT.matcher(text); int count = 0;
        while (matcher.find()) count++;
        return count;
    }
    private static String read(File file) throws IOException {
        // A one-to-one byte encoding keeps existing Lua text and line endings intact.
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.ISO_8859_1);
    }
    private static void write(File file, String text) throws IOException {
        File temporary = File.createTempFile(file.getName() + ".", ".tmp", file.getParentFile());
        try {
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                output.write(text.getBytes(StandardCharsets.ISO_8859_1)); output.getFD().sync();
            }
            if (!temporary.renameTo(file)) throw new IOException("Could not finish writing " + file.getName());
        } finally { if (temporary.exists()) temporary.delete(); }
    }
    private static String episodeText(String original, String id) throws IOException {
        if (id.equals(defaultEpisode(original))) return original;
        Matcher match = DEFAULT.matcher(original);
        if (!match.find()) throw new IOException("The gameflow has no default episode.");
        String text = original.substring(0, match.start()) + match.group().replace("episode:SetDefault()", "-- episode:SetDefault()")
                + "  -- start episode moved by the launcher" + original.substring(match.end());
        String newline = original.contains("\r\n") ? "\r\n" : "\n";
        Episode selected = null;
        for (Episode episode : episodes(text)) if (episode.id.equals(id)) selected = episode;
        if (selected == null) throw new IOException("Unknown episode: " + id);
        int insertion = selected.lineEnd;
        if (insertion == text.length() && !text.endsWith("\n")) { text += newline; insertion = text.length(); }
        text = text.substring(0, insertion) + selected.indent + "episode:SetDefault()  -- start episode chosen in the launcher"
                + newline + text.substring(insertion);
        if (!id.equals(defaultEpisode(text)) || defaultCount(text) != 1) throw new IOException("Could not move the start episode.");
        return text;
    }
    private JSONObject startEpisode(String id) throws Exception {
        File lua = find(game, "simpsons_gameflow.lua");
        if (lua == null || !lua.isFile()) throw new IOException("Game data is not installed.");
        File backup = new File(lua.getParentFile(), lua.getName() + ".original");
        if (backup.exists() && !backup.isFile()) throw new IOException("The original gameflow backup is not a file; it has been kept.");
        String original = read(backup.isFile() ? backup : lua), originalStart = defaultEpisode(original);
        Set<String> ids = new HashSet<>();
        List<Episode> entries = episodes(original);
        for (Episode episode : entries) ids.add(episode.id);
        if (originalStart == null || ids.size() != entries.size() || defaultCount(original) != 1) throw new IOException("Unexpected gameflow; the original file has been kept.");
        if (!ids.contains(id)) throw new IOException("Unknown episode: " + id);
        if (backup.isFile()) {
            String current = read(lua), selected = defaultEpisode(current);
            if (selected == null || !ids.contains(selected) || !current.equals(episodeText(original, selected))) {
                throw new IOException("The gameflow was edited outside the launcher; the original backup has been kept.");
            }
        }
        String replacement = episodeText(original, id);
        if (id.equals(originalStart)) {
            if (backup.isFile()) { write(lua, original); if (!backup.delete()) throw new IOException("The original gameflow is restored, but its backup could not be removed."); }
        } else {
            if (!backup.exists()) write(backup, original);
            write(lua, replacement);
        }
        return new JSONObject().put("ok", true).put("msg", "New games start in " + definitions.getJSONObject("episode_names").optString(id, id) + ".");
    }
    synchronized JSONArray list() throws Exception {
        String intro = introState();
        JSONArray result = new JSONArray().put(new JSONObject().put("id", "skip_intro").put("name", "Skip intro logo videos")
                .put("desc", "Boots straight past the EA / Fox / Gracie logo movies.").put("state", intro).put("available", !intro.equals("unavailable")));
        File lua = find(game, "simpsons_gameflow.lua");
        String state = "unavailable"; JSONArray options = new JSONArray();
        if (lua != null && lua.isFile()) {
            state = defaultEpisode(read(lua));
            File backup = new File(lua.getParentFile(), lua.getName() + ".original");
            for (Episode episode : episodes(read(backup.isFile() ? backup : lua))) options.put(new JSONObject().put("id", episode.id)
                    .put("name", definitions.getJSONObject("episode_names").optString(episode.id, episode.id)));
        }
        result.put(new JSONObject().put("id", "start_episode").put("name", "Start episode (level select)")
                .put("desc", "New games start in the chosen episode. Games already in progress keep their progress. Choose Land of Chocolate to restore the original start.")
                .put("state", state == null ? "unavailable" : state).put("available", state != null && !state.equals("unavailable")).put("options", options));
        return result.put(new JSONObject().put("id", "fps_unlock").put("name", "Frame rate").put("desc", "Set gameplay and menu frame rates in Settings → Frame rate.")
                .put("state", "see settings").put("available", false));
    }
    synchronized JSONObject apply(JSONObject request) throws Exception {
        switch (request.getString("id")) {
            case "skip_intro": return skipIntro(request.getBoolean("enable"));
            case "start_episode": return startEpisode(request.getString("value"));
            default: throw new IOException("Unknown patch.");
        }
    }
}
