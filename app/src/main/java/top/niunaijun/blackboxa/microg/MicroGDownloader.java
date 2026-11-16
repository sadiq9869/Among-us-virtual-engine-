package top.niunaijun.blackboxa.microg;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * MICROG: resumable https download plus SHA-256. No Android dependencies, so it can be run
 * and tested on a plain JVM.
 */
public final class MicroGDownloader {

    public interface Progress {
        /** total is -1 when the server did not say how big the file is. */
        void onProgress(long done, long total);
    }

    private static final int CONNECT_TIMEOUT_MS = 15_000;
    private static final int READ_TIMEOUT_MS = 30_000;
    private static final long PROGRESS_INTERVAL_MS = 300L;

    private MicroGDownloader() {
    }

    /** Lowercase hex SHA-256 of a file. */
    public static String sha256Hex(File file) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException("SHA-256 not available", e);
        }
        byte[] buf = new byte[1 << 20];
        try (InputStream in = new FileInputStream(file)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                digest.update(buf, 0, n);
            }
        }
        byte[] hash = digest.digest();
        StringBuilder sb = new StringBuilder(hash.length * 2);
        for (byte b : hash) {
            sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        }
        return sb.toString();
    }

    /**
     * Parses a Content-Range header.
     * "bytes 100-199/1000" gives {100, 199, 1000}; "bytes 100-199/*" gives {100, 199, -1};
     * "bytes *\/1000" (416 answer) gives {-1, -1, 1000}. Anything else gives null.
     */
    static long[] parseContentRange(String header) {
        if (header == null) return null;
        String h = header.trim();
        if (!h.regionMatches(true, 0, "bytes ", 0, 6)) return null;
        String rest = h.substring(6).trim();
        int slash = rest.indexOf('/');
        if (slash < 0) return null;
        String range = rest.substring(0, slash).trim();
        String totalStr = rest.substring(slash + 1).trim();
        try {
            long total = totalStr.equals("*") ? -1 : Long.parseLong(totalStr);
            if (range.equals("*")) return new long[]{-1, -1, total};
            int dash = range.indexOf('-');
            if (dash < 0) return null;
            long start = Long.parseLong(range.substring(0, dash).trim());
            long end = Long.parseLong(range.substring(dash + 1).trim());
            if (start < 0 || end < start) return null;
            return new long[]{start, end, total};
        } catch (NumberFormatException e) {
            return null;
        }
    }

    /**
     * Downloads url into part. If part already has bytes the download resumes from there.
     * On return part holds the whole body; the caller must verify it (SHA-256) and rename it.
     * Throws IOException on any problem. Partial data is kept so the next call can resume.
     */
    public static void download(String url, File part, Progress progress) throws IOException {
        if (url == null || !url.regionMatches(true, 0, "https://", 0, 8)) {
            throw new IOException("Only https downloads are allowed");
        }
        // Two attempts: the second one starts from zero after the server rejected our Range.
        for (int attempt = 0; attempt < 2; attempt++) {
            if (attempt == 1 && part.exists() && !part.delete()) {
                throw new IOException("Cannot reset partial download");
            }
            if (downloadOnce(url, part, progress)) {
                return;
            }
        }
        throw new IOException("Server did not accept the download request");
    }

    /** Returns true when finished, false when the caller should restart from zero. */
    private static boolean downloadOnce(String url, File part, Progress progress) throws IOException {
        long existing = part.exists() ? part.length() : 0L;
        HttpURLConnection conn = (HttpURLConnection) URI.create(url).toURL().openConnection();
        try {
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(READ_TIMEOUT_MS);
            conn.setInstanceFollowRedirects(true);
            conn.setRequestProperty("User-Agent", "BlackBox-microG/1");
            conn.setRequestProperty("Accept-Encoding", "identity");
            if (existing > 0) {
                conn.setRequestProperty("Range", "bytes=" + existing + "-");
            }

            int code = conn.getResponseCode();
            if (!"https".equalsIgnoreCase(conn.getURL().getProtocol())) {
                throw new IOException("Download was redirected to a non-https address");
            }

            boolean append;
            long total;
            if (code == HttpURLConnection.HTTP_OK) {
                // Server ignored Range (or there was nothing to resume): start from zero.
                append = false;
                existing = 0;
                total = conn.getContentLengthLong();
            } else if (code == HttpURLConnection.HTTP_PARTIAL) {
                long[] range = parseContentRange(conn.getHeaderField("Content-Range"));
                if (range == null || range[0] != existing) {
                    return false;
                }
                append = true;
                total = range[2] > 0 ? range[2] : existing + conn.getContentLengthLong();
            } else if (code == 416) {
                long[] range = parseContentRange(conn.getHeaderField("Content-Range"));
                if (range != null && range[2] > 0 && range[2] == existing) {
                    return true; // partial file is already complete
                }
                return false;
            } else {
                throw new IOException("Server answered HTTP " + code);
            }

            long done = existing;
            long lastReport = 0L;
            byte[] buf = new byte[64 * 1024];
            try (InputStream in = conn.getInputStream();
                 FileOutputStream out = new FileOutputStream(part, append)) {
                if (progress != null) progress.onProgress(done, total);
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    done += n;
                    long now = System.currentTimeMillis();
                    if (progress != null && now - lastReport >= PROGRESS_INTERVAL_MS) {
                        lastReport = now;
                        progress.onProgress(done, total);
                    }
                }
                out.getFD().sync();
            }
            if (total > 0 && done != total) {
                throw new IOException("Download ended early (" + done + " of " + total + " bytes)");
            }
            if (progress != null) progress.onProgress(done, total > 0 ? total : done);
            return true;
        } finally {
            conn.disconnect();
        }
    }
}
