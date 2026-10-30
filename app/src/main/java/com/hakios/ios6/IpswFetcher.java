package com.hakios.ios6;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * Fetch one member out of an IPSW on Apple's CDN without downloading the
 * rest -- the Java twin of sysimage/fetch-from-ipsw.py, written for the
 * install screen: the kernel cache is 7.4 MB inside a 938 MB archive, and
 * a phone should not pay for the other 930.
 *
 * The archive is read with HTTP range requests: the end-of-central-
 * directory (classic or ZIP64 -- the 6.1.3 IPSW is a classic 2013 ZIP, the
 * 16.5 one is ZIP64), then the central directory, then the member's local
 * header, then the member itself, inflated on the fly when Apple stored it
 * deflated (the kernel cache is).
 */
public final class IpswFetcher {

    /** Progress in bytes of the (compressed) member, called off the UI thread. */
    public interface Progress {
        void onBytes(long total);
    }

    public interface Cancellable {
        boolean cancelled();
    }

    private static final int TIMEOUT_MS = 30000;

    private final String url;
    private final long size;

    public IpswFetcher(String url, long sizeBytes) {
        this.url = url;
        this.size = sizeBytes;
    }

    // -- HTTP with ranges ------------------------------------------------------------

    private byte[] fetch(long start, long end) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(TIMEOUT_MS);
            c.setReadTimeout(TIMEOUT_MS);
            c.setRequestProperty("Range", "bytes=" + start + "-" + end);
            int code = c.getResponseCode();
            if (code != HttpURLConnection.HTTP_PARTIAL && code != HttpURLConnection.HTTP_OK)
                throw new IOException("HTTP " + code + " for the range " + start + "-" + end);
            InputStream in = c.getInputStream();
            byte[] out = new byte[(int) (end - start + 1)];
            int have = 0;
            while (have < out.length) {
                int n = in.read(out, have, out.length - have);
                if (n < 0) break;
                have += n;
            }
            if (have < out.length) {
                byte[] trimmed = new byte[have];
                System.arraycopy(out, 0, trimmed, 0, have);
                return trimmed;
            }
            return out;
        } finally {
            c.disconnect();
        }
    }

    // -- the central directory ----------------------------------------------------------

    /** One member: name, zip method, sizes, and the offset of its local header. */
    public static final class Member {
        public final String name;
        public final int method;      // 0 stored, 8 deflated
        public final long csize, usize;
        public final long localHeaderOffset;

        Member(String name, int method, long csize, long usize, long lho) {
            this.name = name;
            this.method = method;
            this.csize = csize;
            this.usize = usize;
            this.localHeaderOffset = lho;
        }
    }

    private long cdOffset = -1, cdSize = -1;
    private byte[] directory;

    private byte[] readDirectory() throws IOException {
        if (directory != null) return directory;
        byte[] tail = fetch(size - 65536, size - 1);
        int at = lastIndexOf(tail, new byte[]{'P', 'K', 5, 6});
        long off = -1, len = -1;
        if (at >= 0) {
            long cdSize = u32le(tail, at + 12), cdOff = u32le(tail, at + 16);
            int n = u16le(tail, at + 10) & 0xFFFF;
            // ZIP64 archives carry a classic EOCD full of 0xFFFFFFFF placeholders
            if (cdOff != 0xFFFFFFFFL && cdSize != 0xFFFFFFFFL && n != 0xFFFF) {
                off = cdOff;
                len = cdSize;
            }
        }
        if (off < 0) {
            at = lastIndexOf(tail, new byte[]{'P', 'K', 6, 6});
            if (at < 0)
                throw new IOException("the archive has no readable directory");
            len = u64le(tail, at + 40);
            off = u64le(tail, at + 48);
        }
        // read it in bounded pieces; a 3 MB directory at once is fine too
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        long p = off;
        while (out.size() < len) {
            long end = Math.min(p + (1 << 20) - 1, size - 1);
            byte[] chunk = fetch(p, end);
            if (chunk.length == 0) break;
            out.write(chunk, 0, chunk.length);
            p += chunk.length;
        }
        directory = out.toByteArray();
        cdOffset = off;
        cdSize = len;
        return directory;
    }

    public Member find(String name) throws IOException {
        byte[] cd = readDirectory();
        int p = 0;
        while (p + 46 <= cd.length && u32le(cd, p) == 0x02014b50) {
            int method = u16le(cd, p + 10);
            long csize = u32le(cd, p + 20), usize = u32le(cd, p + 24);
            int nl = u16le(cd, p + 28), el = u16le(cd, p + 30), cl = u16le(cd, p + 32);
            long lho = u32le(cd, p + 42);
            String member = new String(cd, p + 46, nl, java.nio.charset.StandardCharsets.UTF_8);
            int q = p + 46 + nl;
            int extraEnd = q + el;
            while (q + 4 <= extraEnd) {   // the ZIP64 extra field, when sizes are 0xFFFFFFFF
                int tag = u16le(cd, q), len2 = u16le(cd, q + 2);
                if (tag == 1) {
                    int r = q + 4;
                    if (usize == 0xFFFFFFFFL && r + 8 <= q + 4 + len2) { usize = u64le(cd, r); r += 8; }
                    if (csize == 0xFFFFFFFFL && r + 8 <= q + 4 + len2) { csize = u64le(cd, r); r += 8; }
                    if (lho == 0xFFFFFFFFL && r + 8 <= q + 4 + len2) { lho = u64le(cd, r); r += 8; }
                }
                q += 4 + len2;
            }
            if (member.equals(name)) return new Member(member, method, csize, usize, lho);
            p += 46 + nl + el + cl;
        }
        throw new IOException(name + " is not in the archive");
    }

    // -- the member itself ---------------------------------------------------------------

    /** Stream the member into out, reporting bytes as they arrive. */
    public long download(Member m, File out, Progress progress, Cancellable cancel)
            throws IOException {
        byte[] local = fetch(m.localHeaderOffset, m.localHeaderOffset + 29);
        int nl = u16le(local, 26), el = u16le(local, 28);
        long data = m.localHeaderOffset + 30 + nl + el;
        if (m.method != 0 && m.method != 8)
            throw new IOException("unsupported zip method " + m.method);

        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(TIMEOUT_MS);
            c.setReadTimeout(TIMEOUT_MS);
            long last = Math.min(data + m.csize - 1, size - 1);
            c.setRequestProperty("Range", "bytes=" + data + "-" + last);
            InputStream raw = new DataInputStream(c.getInputStream());
            Inflater inf = null;
            if (m.method == 8) {
                inf = new Inflater(true);   // raw deflate, as zip stores it
                raw = new InflaterStream(raw, inf, m.usize);
            }
            try (OutputStream file = new FileOutputStream(out)) {
                byte[] buf = new byte[1 << 16];
                long written = 0;
                long since = 0;
                int n;
                while ((n = raw.read(buf)) >= 0) {
                    if (cancel != null && cancel.cancelled()) return -1;
                    file.write(buf, 0, n);
                    written += n;
                    since += n;
                    if (progress != null && since > (1 << 20)) {
                        progress.onBytes(written);
                        since = 0;
                    }
                }
                if (progress != null) progress.onBytes(written);
                return written;
            }
        } finally {
            c.disconnect();
        }
    }

    // -- small helpers ---------------------------------------------------------------------

    static int lastIndexOf(byte[] haystack, byte[] needle) {
        outer:
        for (int i = haystack.length - needle.length; i >= 0; i--) {
            for (int j = 0; j < needle.length; j++)
                if (haystack[i + j] != needle[j]) continue outer;
            return i;
        }
        return -1;
    }

    static int u16le(byte[] b, int o) {
        return (b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8;
    }

    static long u32le(byte[] b, int o) {
        return (b[o] & 0xFFL) | (b[o + 1] & 0xFFL) << 8 |
               (b[o + 2] & 0xFFL) << 16 | (b[o + 3] & 0xFFL) << 24;
    }

    static long u64le(byte[] b, int o) {
        return u32le(b, o) | u32le(b, o + 4) << 32;
    }

    /** A deflated member, inflated while streaming, with a size sanity bound. */
    static final class InflaterStream extends InputStream {
        private final InputStream in;
        private final Inflater inf;
        private final byte[] buf = new byte[1 << 16];
        private long produced = 0, limit;

        InflaterStream(InputStream in, Inflater inf, long limit) {
            this.in = in;
            this.inf = inf;
            this.limit = limit;
        }

        @Override
        public int read() throws IOException {
            byte[] one = new byte[1];
            int n = read(one, 0, 1);
            return n > 0 ? one[0] & 0xFF : -1;
        }

        @Override
        public int read(byte[] out, int off, int len) throws IOException {
            while (inf.getRemaining() == 0 && !inf.finished()) {
                int n = in.read(buf);
                if (n < 0) throw new EOFException("the archive ended inside the member");
                inf.setInput(buf, 0, n);
                if (inf.needsInput()) continue;
            }
            try {
                int n = inf.inflate(out, off, len);
                produced += n;
                if (limit > 0 && produced > limit)
                    throw new IOException("the member inflated past its promised size");
                return n == 0 && inf.finished() ? -1 : n;
            } catch (DataFormatException e) {
                throw new IOException("corrupt deflate stream: " + e.getMessage());
            }
        }

        @Override
        public void close() throws IOException {
            inf.end();
            in.close();
        }
    }
}
