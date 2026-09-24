package com.fastaudio;

final class Protocol {
    static final int VERSION = 1;
    static final int SAMPLE_RATE = 48_000;
    static final int CHANNELS = 2;
    static final int BITS_PER_SAMPLE = 16;
    static final int LEGACY_PACKET_FRAMES = 128;
    static final int LOW_LATENCY_PACKET_FRAMES = 1024;
    static final int BYTES_PER_FRAME = CHANNELS * BITS_PER_SAMPLE / 8;
    static final int HELLO_SIZE = 32;
    static final int PACKET_HEADER_SIZE = 32;

    static final int HELLO_MAGIC = 0x46415544; // FAUD
    static final int PACKET_MAGIC = 0x46415031; // FAP1

    static final int CAPTURE_VOICE = 1;
    static final int CAPTURE_UID_FILTER = 2;
    static final int FLAG_TIMESTAMP_FALLBACK = 1;
    static final int FLAG_DISCONTINUITY = 2;

    // The microphone stream deliberately uses a separate socket from playback so
    // microphone failures cannot disturb the latency-critical playback protocol.
    static final int MIC_SAMPLE_RATE = 48_000;
    static final int MIC_CHANNELS = 1;
    static final int MIC_BITS_PER_SAMPLE = 16;
    static final int MIC_BYTES_PER_FRAME = 2;
    static final int MIC_PACKET_FRAMES = 480; // 10 ms at 48 kHz.
    static final int MIC_HELLO_SIZE = 32;
    static final int MIC_PACKET_HEADER_SIZE = 32;
    static final int MIC_HELLO_MAGIC = 0x46414D48; // FAMH
    static final int MIC_PACKET_MAGIC = 0x46414D50; // FAMP
    static final int MIC_STATUS_READY = 1;
    static final int MIC_FLAG_DISCONTINUITY = 1;

    private Protocol() {
    }

    static void putU16(byte[] target, int offset, int value) {
        target[offset] = (byte) (value >>> 8);
        target[offset + 1] = (byte) value;
    }

    static void putU32(byte[] target, int offset, long value) {
        target[offset] = (byte) (value >>> 24);
        target[offset + 1] = (byte) (value >>> 16);
        target[offset + 2] = (byte) (value >>> 8);
        target[offset + 3] = (byte) value;
    }

    static void putU64(byte[] target, int offset, long value) {
        target[offset] = (byte) (value >>> 56);
        target[offset + 1] = (byte) (value >>> 48);
        target[offset + 2] = (byte) (value >>> 40);
        target[offset + 3] = (byte) (value >>> 32);
        target[offset + 4] = (byte) (value >>> 24);
        target[offset + 5] = (byte) (value >>> 16);
        target[offset + 6] = (byte) (value >>> 8);
        target[offset + 7] = (byte) value;
    }
}
