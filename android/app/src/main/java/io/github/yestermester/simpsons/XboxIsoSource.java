package io.github.yestermester.simpsons;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Reads XDVDFS images with the same partition offsets as tools/extract-xiso. */
final class XboxIsoSource implements GameFileImporter.Source, AutoCloseable {
    private static final byte[] MEDIA = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    private static final long[] PARTITIONS = {0, 0x0FD90000L, 0x02080000L, 0x18300000L};
    private static final class Extent {
        final long offset, length;
        final boolean directory;
        Extent(long offset, long length, boolean directory) {
            this.offset = offset; this.length = length; this.directory = directory;
        }
    }
    private final FileChannel image;
    private final GameFileImporter.Listener listener;
    private final long imageSize, partition;
    private final Map<String, Extent> extents = new HashMap<>();

    XboxIsoSource(FileChannel image, GameFileImporter.Listener listener) throws IOException {
        this.image = image; this.listener = listener; imageSize = image.size();
        listener.checkCancelled();
        listener.progress("scanning", "Reading Xbox 360 disc filesystem…", 0, -1, 0, 0);
        long found = -1;
        byte[] header = new byte[2048];
        for (long candidate : PARTITIONS) {
            if (candidate + 0x10800 > imageSize) continue;
            readFully(candidate + 0x10000, header);
            if (!Arrays.equals(MEDIA, Arrays.copyOfRange(header, 0, 20))) continue;
            if (!Arrays.equals(MEDIA, Arrays.copyOfRange(header, 2028, 2048))) throw new IOException("The ISO's disc header is damaged or incomplete.");
            found = candidate; break;
        }
        if (found < 0) throw new IOException("This file is not an Xbox 360 ISO/XISO disc image. Choose the .iso itself, rather than a ZIP, torrent or shortcut.");
        partition = found;
        long root = partition + uint32(header, 20) * 2048, length = uint32(header, 24);
        if (length == 0) throw new IOException("The disc image contains no game files.");
        checkRange(root, length);
        extents.put("root", new Extent(root, length, true));
    }
    private static int uint16(byte[] data, int offset) {
        return (data[offset] & 255) | ((data[offset + 1] & 255) << 8);
    }
    private static long uint32(byte[] data, int offset) {
        return (data[offset] & 255L) | ((data[offset + 1] & 255L) << 8)
                | ((data[offset + 2] & 255L) << 16) | ((data[offset + 3] & 255L) << 24);
    }
    private void checkRange(long position, long length) throws IOException {
        if (position < partition || length < 0 || position > imageSize || length > imageSize - position) {
            throw new IOException("The ISO is truncated or contains a file outside the disc image. Copy the complete ISO to this device and retry.");
        }
    }
    private void readFully(long position, byte[] output) throws IOException {
        ByteBuffer buffer = ByteBuffer.wrap(output);
        while (buffer.hasRemaining()) {
            listener.checkCancelled();
            int count = image.read(buffer, position + buffer.position());
            if (count <= 0) throw new IOException("The ISO could not be read completely.");
        }
    }
    @Override public List<GameFileImporter.Document> children(String id) throws IOException {
        listener.checkCancelled();
        Extent directory = extents.get(id);
        if (directory == null || !directory.directory) throw new IOException("Invalid ISO directory.");
        List<GameFileImporter.Document> files = new ArrayList<>();
        if (directory.length == 0) return files;
        if (directory.length > 16 * 1024 * 1024) throw new IOException("The ISO contains an invalid directory table.");
        byte[] table = new byte[(int) directory.length];
        readFully(directory.offset, table);
        ArrayDeque<Integer> pending = new ArrayDeque<>();
        Set<Integer> visited = new HashSet<>();
        pending.push(0);
        while (!pending.isEmpty()) {
            listener.checkCancelled();
            int offset = pending.pop();
            if (!visited.add(offset)) throw new IOException("The ISO contains a directory loop.");
            if (offset < 0 || offset > table.length - 14) throw new IOException("The ISO contains a damaged directory entry.");
            int left = uint16(table, offset), right = uint16(table, offset + 2);
            if (left == 0xffff) {
                if (offset == 0) continue; // An empty XDVDFS directory is filled with 0xff.
                int nextSector = (offset / 2048 + 1) * 2048;
                if (nextSector < table.length) { pending.push(nextSector); continue; }
                throw new IOException("The ISO contains an invalid directory link.");
            }
            int nameLength = table[offset + 13] & 255;
            if (nameLength == 0 || nameLength > table.length - offset - 14) throw new IOException("The ISO contains a damaged filename.");
            String name = new String(table, offset + 14, nameLength, StandardCharsets.ISO_8859_1);
            long position = partition + uint32(table, offset + 4) * 2048, length = uint32(table, offset + 8);
            boolean isDirectory = (table[offset + 12] & 0x10) != 0;
            checkRange(position, length);
            String key = (isDirectory ? "D:" : "F:") + position + ":" + length;
            extents.put(key, new Extent(position, length, isDirectory));
            files.add(new GameFileImporter.Document(key, name, isDirectory, length));
            if (files.size() > 250000) throw new IOException("The ISO contains too many directory entries.");
            if (right != 0) pending.push(right * 4);
            if (left != 0) pending.push(left * 4);
        }
        return files;
    }
    @Override public InputStream open(String id) throws IOException {
        Extent extent = extents.get(id);
        if (extent == null || extent.directory) throw new IOException("Invalid ISO file.");
        return new InputStream() {
            private long position = extent.offset, remaining = extent.length;
            private boolean closed;
            @Override public int read() throws IOException {
                byte[] value = new byte[1];
                return read(value, 0, 1) < 0 ? -1 : value[0] & 255;
            }
            @Override public int read(byte[] buffer, int offset, int count) throws IOException {
                if (closed) throw new IOException("ISO file is closed.");
                if (offset < 0 || count < 0 || offset > buffer.length - count) throw new IndexOutOfBoundsException();
                if (count == 0) return 0;
                if (remaining == 0) return -1;
                listener.checkCancelled();
                int read = image.read(ByteBuffer.wrap(buffer, offset, (int) Math.min(count, remaining)), position);
                if (read <= 0) throw new IOException("The ISO could not be read completely.");
                position += read; remaining -= read;
                return read;
            }
            @Override public void close() { closed = true; }
        };
    }
    @Override public void close() throws IOException { image.close(); }
}
