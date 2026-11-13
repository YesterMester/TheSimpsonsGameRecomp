package io.github.yestermester.simpsons;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Plans an import before copying, then replaces only the game-data directory. */
final class GameFileImporter {
    static final class Document {
        final String id, name;
        final boolean directory;
        final long size;
        Document(String id, String name, boolean directory, long size) {
            this.id = id; this.name = name; this.directory = directory; this.size = size;
        }
    }
    interface Source {
        List<Document> children(String id) throws IOException;
        InputStream open(String id) throws IOException;
    }
    interface Listener {
        void checkCancelled() throws IOException;
        void progress(String phase, String message, long bytes, long totalBytes, int files, int totalFiles);
    }
    private static final class Folder {
        final String id, path;
        final int depth;
        Folder(String id, String path, int depth) { this.id = id; this.path = path; this.depth = depth; }
    }
    private static final class Entry {
        final Document document;
        final String path;
        Entry(Document document, String path) { this.document = document; this.path = path; }
    }
    private final Source source;
    private final Listener listener;
    private final File storage;
    private final Map<String, List<Document>> listings = new HashMap<>();

    GameFileImporter(Source source, Listener listener, File storage) {
        this.source = source; this.listener = listener; this.storage = storage;
    }

    private List<Document> children(String id) throws IOException {
        listener.checkCancelled();
        List<Document> result = listings.get(id);
        if (result != null) return result;
        result = source.children(id);
        Set<String> names = new HashSet<>();
        for (Document document : result) {
            String name = document.name;
            if (document.id == null || name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                    || name.contains("/") || name.contains("\\") || name.indexOf('\0') >= 0) {
                throw new IOException("Invalid filename in the selected folder.");
            }
            if (!names.add(name.toLowerCase(java.util.Locale.ROOT))) {
                throw new IOException("The folder contains duplicate filenames: " + name);
            }
        }
        listings.put(id, result);
        return result;
    }

    private String findGameRoot(String selected) throws IOException {
        ArrayDeque<Folder> pending = new ArrayDeque<>();
        pending.add(new Folder(selected, "Selected folder", 0));
        Set<String> visited = new HashSet<>();
        String found = null;
        int scanned = 0;
        boolean limited = false;
        while (!pending.isEmpty()) {
            listener.checkCancelled();
            Folder folder = pending.removeFirst();
            if (!visited.add(folder.id)) continue;
            if (++scanned > 4096) { limited = true; break; }
            listener.progress("scanning", "Looking for default.xex · " + folder.path, 0, -1, scanned, 0);
            List<Document> entries = children(folder.id);
            boolean hasExecutable = false;
            for (Document entry : entries) {
                if (!entry.directory && entry.name.equalsIgnoreCase("default.xex")) hasExecutable = true;
            }
            if (hasExecutable) {
                if (folder.depth == 0) return folder.id;
                if (found != null) throw new IOException("More than one game folder was found. Choose the specific game's folder.");
                found = folder.id;
                continue;
            }
            for (Document entry : entries) if (entry.directory) {
                if (folder.depth >= 8) { limited = true; continue; }
                pending.add(new Folder(entry.id, entry.name, folder.depth + 1));
            }
        }
        if (limited) throw new IOException("Choose a folder closer to the extracted game files; the selected folder is too large to search safely.");
        if (found == null) throw new IOException("No default.xex was found in this folder or its subfolders. To install a disc image, use Choose game ISO; the app will extract it for you. Otherwise choose your extracted Xbox 360 game folder.");
        return found;
    }

    void run(String selected) throws IOException {
        File game = new File(storage, "game"), staging = new File(storage, "game.importing");
        File previous = new File(storage, "game.previous");
        if (!storage.isDirectory()) throw new IOException("Device storage is unavailable.");
        if (!game.exists() && previous.exists() && !previous.renameTo(game)) {
            throw new IOException("Could not recover the previous game import.");
        }
        String root = findGameRoot(selected);
        List<Entry> files = new ArrayList<>();
        List<String> directories = new ArrayList<>();
        ArrayDeque<Folder> pending = new ArrayDeque<>();
        Set<String> visited = new HashSet<>();
        pending.add(new Folder(root, "", 0));
        long totalBytes = 0;
        boolean sizesKnown = true;
        while (!pending.isEmpty()) {
            Folder folder = pending.removeFirst();
            if (folder.depth > 64 || !visited.add(folder.id)) throw new IOException("The game folder contains a directory loop or is too deeply nested.");
            for (Document document : children(folder.id)) {
                String name = folder.depth == 0 && document.name.equalsIgnoreCase("default.xex") ? "default.xex" : document.name;
                String path = folder.path.isEmpty() ? name : folder.path + "/" + name;
                if (document.directory) {
                    directories.add(path);
                    pending.add(new Folder(document.id, path, folder.depth + 1));
                } else {
                    files.add(new Entry(document, path));
                    if (document.size < 0) sizesKnown = false;
                    else {
                        if (Long.MAX_VALUE - totalBytes < document.size) throw new IOException("The selected game is too large.");
                        totalBytes += document.size;
                    }
                }
                if (files.size() + directories.size() > 250000) throw new IOException("The selected folder contains too many files. Choose the game's folder.");
            }
            listener.progress("scanning", "Counting game files · " + files.size() + " found", 0, -1, 0, files.size());
        }
        listener.checkCancelled();
        removeTree(staging); // Reclaim an interrupted attempt before checking the space needed to retry.
        long space = storage.getUsableSpace();
        if (totalBytes > Math.max(0, space - 32L * 1024 * 1024)) {
            throw new IOException("Not enough free space. The new import needs " + formatBytes(totalBytes)
                    + " plus temporary space; " + formatBytes(space) + " is available. The current game is kept until the new import finishes.");
        }
        listener.checkCancelled();
        if (!staging.mkdirs()) throw new IOException("Cannot create the game folder.");
        boolean committed = false;
        try {
            for (String directory : directories) {
                listener.checkCancelled();
                File target = new File(staging, directory);
                if (!target.isDirectory() && !target.mkdirs()) throw new IOException("Cannot create " + directory);
            }
            byte[] buffer = new byte[256 * 1024];
            long copied = 0;
            int completed = 0;
            for (Entry entry : files) {
                listener.checkCancelled();
                long fileBytes = 0;
                listener.progress("copying", "Copying " + entry.path, copied, sizesKnown ? totalBytes : -1, completed, files.size());
                try (InputStream input = source.open(entry.document.id);
                        FileOutputStream output = new FileOutputStream(new File(staging, entry.path))) {
                    if (input == null) throw new IOException("Cannot read " + entry.path);
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        listener.checkCancelled();
                        output.write(buffer, 0, count);
                        copied += count; fileBytes += count;
                        listener.progress("copying", "Copying " + entry.path, copied, sizesKnown ? totalBytes : -1, completed, files.size());
                    }
                    output.getFD().sync();
                }
                if (entry.document.size >= 0 && fileBytes != entry.document.size) {
                    throw new IOException("The source file changed or could not be read completely: " + entry.path + ". Try a local extracted copy.");
                }
                listener.progress("copying", "Copied " + entry.path, copied, sizesKnown ? totalBytes : -1, ++completed, files.size());
            }
            listener.checkCancelled();
            listener.progress("verifying", "Checking the imported game…", copied, copied, completed, files.size());
            try (RandomAccessFile executable = new RandomAccessFile(new File(staging, "default.xex"), "r")) {
                if (executable.length() < 4 || executable.readInt() != 0x58455832) {
                    throw new IOException("default.xex is not an Xbox 360 XEX2 executable. Choose your extracted Xbox 360 copy of The Simpsons Game.");
                }
            }
            listener.checkCancelled();
            listener.progress("finishing", "Finishing installation · keeping your saves and settings…", copied, copied, completed, files.size());
            // Cancellation stops before this swap, so it can never remove a completed import.
            removeTree(previous);
            if (game.exists() && !game.renameTo(previous)) throw new IOException("Cannot replace the previous import.");
            if (!staging.renameTo(game)) {
                if (previous.exists() && !previous.renameTo(game)) throw new IOException("Could not finish the import. Your previous files are kept in game.previous and will be recovered on the next launch.");
                throw new IOException("Cannot finish importing the game. Your previous files have been restored.");
            }
            committed = true;
            // A leftover backup can be removed next time; it must not turn a successful swap into a failed import.
            try { removeTree(previous); } catch (IOException ignored) { }
        } finally {
            if (!committed) try { removeTree(staging); } catch (IOException ignored) { }
        }
    }

    static String formatBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format(java.util.Locale.ROOT, "%.1f KB", bytes / 1024.0);
        if (bytes < 1024L * 1024 * 1024) return String.format(java.util.Locale.ROOT, "%.1f MB", bytes / (1024.0 * 1024));
        return String.format(java.util.Locale.ROOT, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    static void removeTree(File file) throws IOException {
        if (!file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) removeTree(child);
        if (!file.delete()) throw new IOException("Cannot remove " + file.getName());
    }
}
