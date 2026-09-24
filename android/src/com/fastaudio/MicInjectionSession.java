package com.fastaudio;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaRecorder;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/** Injects the desktop microphone into one app's game-voice recording streams. */
final class MicInjectionSession implements AutoCloseable {
    private final AudioTrack[] injectors;
    private final Object audioPolicy;
    private final AudioManager audioManager;
    private volatile boolean closed;

    private MicInjectionSession(AudioTrack[] injectors, Object audioPolicy,
            AudioManager audioManager) {
        this.injectors = injectors;
        this.audioPolicy = audioPolicy;
        this.audioManager = audioManager;
    }

    static MicInjectionSession create(Context context, int uid) throws Exception {
        Class<?> mixClass = Class.forName("android.media.audiopolicy.AudioMix");
        Class<?> mixBuilderClass = Class.forName("android.media.audiopolicy.AudioMix$Builder");
        Object[] mixes = {
            createMix(mixClass, mixBuilderClass, uid, MediaRecorder.AudioSource.MIC),
            createMix(mixClass, mixBuilderClass, uid,
                    MediaRecorder.AudioSource.VOICE_COMMUNICATION),
        };

        Class<?> policyClass = Class.forName("android.media.audiopolicy.AudioPolicy");
        Class<?> policyBuilderClass =
                Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
        Object policyBuilder = policyBuilderClass.getConstructor(Context.class).newInstance(context);
        for (Object mix : mixes) {
            policyBuilderClass.getMethod("addMix", mixClass).invoke(policyBuilder, mix);
        }
        Object policy = policyBuilderClass.getMethod("build").invoke(policyBuilder);

        AudioManager audioManager =
                (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (audioManager == null) {
            throw new IllegalStateException("AudioManager is unavailable");
        }
        Method register = AudioManager.class.getDeclaredMethod(
                "registerAudioPolicy", policyClass);
        register.setAccessible(true);
        int result = (int) register.invoke(audioManager, policy);
        if (result != 0) {
            throw new IllegalStateException("registerAudioPolicy microphone returned " + result);
        }

        List<AudioTrack> tracks = new ArrayList<>();
        try {
            for (Object mix : mixes) {
                AudioTrack track = (AudioTrack) policyClass
                        .getMethod("createAudioTrackSource", mixClass)
                        .invoke(policy, mix);
                if (track == null || track.getState() != AudioTrack.STATE_INITIALIZED) {
                    if (track != null) {
                        track.release();
                    }
                    throw new IllegalStateException(
                            "AudioTrack microphone injector is not initialized");
                }
                tracks.add(track);
            }
            return new MicInjectionSession(tracks.toArray(new AudioTrack[0]),
                    policy, audioManager);
        } catch (Exception error) {
            for (AudioTrack track : tracks) {
                track.release();
            }
            unregister(audioManager, policyClass, policy);
            throw error;
        }
    }

    private static Object createMix(Class<?> mixClass, Class<?> mixBuilderClass,
            int uid, int capturePreset) throws Exception {
        Class<?> ruleClass = Class.forName("android.media.audiopolicy.AudioMixingRule");
        Class<?> ruleBuilderClass =
                Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
        Object ruleBuilder = ruleBuilderClass.getConstructor().newInstance();
        int injectorRole = ruleClass.getField("MIX_ROLE_INJECTOR").getInt(null);
        int uidRule = ruleClass.getField("RULE_MATCH_UID").getInt(null);
        int capturePresetRule =
                ruleClass.getField("RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET").getInt(null);
        ruleBuilderClass.getMethod("setTargetMixRole", int.class)
                .invoke(ruleBuilder, injectorRole);
        ruleBuilderClass.getMethod("addMixRule", int.class, Object.class)
                .invoke(ruleBuilder, uidRule, Integer.valueOf(uid));

        // setInternalCapturePreset is hidden from android.jar but available to
        // the shell app_process after FastAudio's hidden-API exemption.
        AudioAttributes.Builder microphoneBuilder = new AudioAttributes.Builder();
        Method setCapturePreset = microphoneBuilder.getClass().getDeclaredMethod(
                "setInternalCapturePreset", int.class);
        setCapturePreset.setAccessible(true);
        setCapturePreset.invoke(microphoneBuilder, capturePreset);
        AudioAttributes microphoneAttributes = microphoneBuilder.build();
        ruleBuilderClass.getMethod("addRule", AudioAttributes.class, int.class)
                .invoke(ruleBuilder, microphoneAttributes, capturePresetRule);
        Object mixingRule = ruleBuilderClass.getMethod("build").invoke(ruleBuilder);

        Object mixBuilder = mixBuilderClass.getConstructor(ruleClass).newInstance(mixingRule);
        AudioFormat format = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(Protocol.MIC_SAMPLE_RATE)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                .build();
        mixBuilderClass.getMethod("setFormat", AudioFormat.class).invoke(mixBuilder, format);
        int loopback = mixClass.getField("ROUTE_FLAG_LOOP_BACK").getInt(null);
        mixBuilderClass.getMethod("setRouteFlags", int.class).invoke(mixBuilder, loopback);
        return mixBuilderClass.getMethod("build").invoke(mixBuilder);
    }

    static void writeHello(OutputStream output, int uid, boolean ready) throws IOException {
        byte[] hello = new byte[Protocol.MIC_HELLO_SIZE];
        Protocol.putU32(hello, 0, Protocol.MIC_HELLO_MAGIC);
        Protocol.putU16(hello, 4, Protocol.VERSION);
        Protocol.putU16(hello, 6, Protocol.MIC_HELLO_SIZE);
        Protocol.putU32(hello, 8, Protocol.MIC_SAMPLE_RATE);
        Protocol.putU16(hello, 12, Protocol.MIC_CHANNELS);
        Protocol.putU16(hello, 14, Protocol.MIC_BITS_PER_SAMPLE);
        Protocol.putU32(hello, 16, Protocol.MIC_PACKET_FRAMES);
        Protocol.putU32(hello, 20, ready ? Protocol.MIC_STATUS_READY : 0);
        Protocol.putU32(hello, 24, uid);
        Protocol.putU32(hello, 28, 0);
        output.write(hello);
        output.flush();
    }

    void stream(InputStream input) throws IOException {
        byte[] header = new byte[Protocol.MIC_PACKET_HEADER_SIZE];
        byte[] pcm = new byte[Protocol.MIC_PACKET_FRAMES * Protocol.MIC_BYTES_PER_FRAME];
        int largestBuffer = 0;
        for (AudioTrack injector : injectors) {
            injector.play();
            largestBuffer = Math.max(largestBuffer, injector.getBufferSizeInFrames());
        }
        System.err.println("FastAudio: microphone injection started, routes="
                + injectors.length + " trackBufferFrames=" + largestBuffer);
        try {
            while (!closed && readFullyOrEof(input, header)) {
                if (readU32(header, 0) != Protocol.MIC_PACKET_MAGIC
                        || readU16(header, 4) != Protocol.VERSION
                        || readU16(header, 6) != Protocol.MIC_PACKET_HEADER_SIZE) {
                    throw new IOException("Invalid microphone packet header");
                }
                int frames = (int) readU32(header, 24);
                if (frames <= 0 || frames > Protocol.MIC_PACKET_FRAMES) {
                    throw new IOException("Invalid microphone packet frames=" + frames);
                }
                int bytes = frames * Protocol.MIC_BYTES_PER_FRAME;
                readFully(input, pcm, bytes);
                for (AudioTrack injector : injectors) {
                    int written = injector.write(pcm, 0, bytes, AudioTrack.WRITE_BLOCKING);
                    if (written != bytes) {
                        throw new IOException("Microphone injector write=" + written
                                + " expected=" + bytes);
                    }
                }
            }
        } finally {
            try {
                for (AudioTrack injector : injectors) {
                    injector.stop();
                    injector.flush();
                }
            } catch (IllegalStateException ignored) {
                // Teardown is best effort.
            }
        }
    }

    private static boolean readFullyOrEof(InputStream input, byte[] target) throws IOException {
        int offset = 0;
        while (offset < target.length) {
            int count = input.read(target, offset, target.length - offset);
            if (count < 0) {
                if (offset == 0) {
                    return false;
                }
                throw new EOFException("Partial microphone packet header");
            }
            offset += count;
        }
        return true;
    }

    private static void readFully(InputStream input, byte[] target, int size) throws IOException {
        int offset = 0;
        while (offset < size) {
            int count = input.read(target, offset, size - offset);
            if (count < 0) {
                throw new EOFException("Microphone socket closed mid-packet");
            }
            offset += count;
        }
    }

    private static int readU16(byte[] source, int offset) {
        return ((source[offset] & 0xff) << 8) | (source[offset + 1] & 0xff);
    }

    private static long readU32(byte[] source, int offset) {
        return ((long) (source[offset] & 0xff) << 24)
                | ((long) (source[offset + 1] & 0xff) << 16)
                | ((long) (source[offset + 2] & 0xff) << 8)
                | (long) (source[offset + 3] & 0xff);
    }

    @Override
    public synchronized void close() {
        if (closed) {
            return;
        }
        closed = true;
        try {
            for (AudioTrack injector : injectors) {
                try {
                    injector.stop();
                } catch (IllegalStateException ignored) {
                    // Teardown is best effort.
                }
                injector.release();
            }
        } catch (RuntimeException ignored) {
            // Teardown must still unregister the policy below.
        }
        unregister(audioManager, audioPolicy.getClass(), audioPolicy);
        System.err.println("FastAudio: microphone injector removed; phone microphone restored");
    }

    private static void unregister(
            AudioManager audioManager, Class<?> policyClass, Object policy) {
        try {
            Method unregister = AudioManager.class.getDeclaredMethod(
                    "unregisterAudioPolicy", policyClass);
            unregister.setAccessible(true);
            unregister.invoke(audioManager, policy);
        } catch (ReflectiveOperationException ignored) {
            // Process exit also releases the policy.
        }
    }
}
