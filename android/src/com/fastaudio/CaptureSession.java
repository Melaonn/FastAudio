package com.fastaudio;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTimestamp;

import java.io.OutputStream;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

final class CaptureSession implements AutoCloseable {
    private final AudioRecord recorder;
    private final Object audioPolicy;
    private final AudioManager audioManager;
    private final boolean voiceRuleAccepted;
    private final boolean uidFilterAccepted;
    private final int packageUid;

    private CaptureSession(AudioRecord recorder, Object audioPolicy, AudioManager audioManager,
            boolean voiceRuleAccepted, boolean uidFilterAccepted, int packageUid) {
        this.recorder = recorder;
        this.audioPolicy = audioPolicy;
        this.audioManager = audioManager;
        this.voiceRuleAccepted = voiceRuleAccepted;
        this.uidFilterAccepted = uidFilterAccepted;
        this.packageUid = packageUid;
    }

    static CaptureSession create(Context context, int uid) throws Exception {
        Exception lastFailure = null;
        boolean[][] attempts = {
            {true, true},
            {true, false},
            {false, true},
            {false, false},
        };
        for (boolean[] attempt : attempts) {
            boolean uidFilter = attempt[0];
            boolean includeVoice = attempt[1];
            try {
                CaptureSession session =
                        createInternal(context, uid, includeVoice, uidFilter);
                if (!uidFilter) {
                    System.err.println("FastAudio: ROM rejected UID-filtered mix; "
                            + "using usage-only capture");
                }
                return session;
            } catch (Exception e) {
                lastFailure = e;
                System.err.println("FastAudio: policy attempt uidFilter=" + uidFilter
                        + " voice=" + includeVoice + " failed: "
                        + e.getClass().getSimpleName());
            }
        }
        throw lastFailure;
    }

    private static CaptureSession createInternal(Context context, int uid,
            boolean includeVoice, boolean uidFilter) throws Exception {
        Class<?> ruleClass = Class.forName("android.media.audiopolicy.AudioMixingRule");
        Class<?> ruleBuilderClass =
                Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
        Object ruleBuilder = ruleBuilderClass.getConstructor().newInstance();

        int playersRole = ruleClass.getField("MIX_ROLE_PLAYERS").getInt(null);
        ruleBuilderClass.getMethod("setTargetMixRole", int.class)
                .invoke(ruleBuilder, playersRole);

        int usageRule = ruleClass.getField("RULE_MATCH_ATTRIBUTE_USAGE").getInt(null);
        int uidRule = ruleClass.getField("RULE_MATCH_UID").getInt(null);
        Method addMixRule =
                ruleBuilderClass.getMethod("addMixRule", int.class, Object.class);
        Method addUsageRule =
                ruleBuilderClass.getMethod("addRule", AudioAttributes.class, int.class);
        if (uidFilter) {
            addMixRule.invoke(ruleBuilder, uidRule, Integer.valueOf(uid));
        }
        addUsage(addUsageRule, ruleBuilder, usageRule, AudioAttributes.USAGE_GAME);
        addUsage(addUsageRule, ruleBuilder, usageRule, AudioAttributes.USAGE_MEDIA);
        try {
            ruleBuilderClass.getMethod(
                    "allowPrivilegedPlaybackCapture", boolean.class)
                    .invoke(ruleBuilder, false);
        } catch (NoSuchMethodException ignored) {
            // Not available on every Android release.
        }
        if (includeVoice) {
            addUsage(addUsageRule, ruleBuilder, usageRule,
                    AudioAttributes.USAGE_VOICE_COMMUNICATION);
            addUsage(addUsageRule, ruleBuilder, usageRule,
                    AudioAttributes.USAGE_VOICE_COMMUNICATION_SIGNALLING);
        }
        ruleBuilderClass.getMethod("voiceCommunicationCaptureAllowed", boolean.class)
                .invoke(ruleBuilder, includeVoice);
        Object mixingRule = ruleBuilderClass.getMethod("build").invoke(ruleBuilder);

        Class<?> mixClass = Class.forName("android.media.audiopolicy.AudioMix");
        Class<?> mixBuilderClass = Class.forName("android.media.audiopolicy.AudioMix$Builder");
        Object mixBuilder = mixBuilderClass.getConstructor(ruleClass).newInstance(mixingRule);
        AudioFormat format = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(Protocol.SAMPLE_RATE)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .build();
        mixBuilderClass.getMethod("setFormat", AudioFormat.class).invoke(mixBuilder, format);
        int loopback =
                mixClass.getField("ROUTE_FLAG_LOOP_BACK").getInt(null);
        mixBuilderClass.getMethod("setRouteFlags", int.class)
                .invoke(mixBuilder, loopback);
        Object mix = mixBuilderClass.getMethod("build").invoke(mixBuilder);

        Class<?> policyClass = Class.forName("android.media.audiopolicy.AudioPolicy");
        Class<?> policyBuilderClass =
                Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
        Object policyBuilder = policyBuilderClass.getConstructor(Context.class).newInstance(context);
        policyBuilderClass.getMethod("addMix", mixClass).invoke(policyBuilder, mix);
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
            throw new IllegalStateException("registerAudioPolicy returned " + result);
        }

        try {
            Thread.sleep(1000);
            AudioRecord record = (AudioRecord) policyClass
                    .getMethod("createAudioRecordSink", mixClass)
                    .invoke(policy, mix);
            if (record == null || record.getState() != AudioRecord.STATE_INITIALIZED) {
                throw new IllegalStateException("AudioRecord sink is not initialized");
            }
            return new CaptureSession(
                    record, policy, audioManager, includeVoice, uidFilter, uid);
        } catch (Exception e) {
            unregister(audioManager, policyClass, policy);
            throw e;
        }
    }

    private static void addUsage(Method addRule, Object builder, int rule, int usage)
            throws ReflectiveOperationException {
        AudioAttributes attributes = new AudioAttributes.Builder()
                .setUsage(usage)
                .build();
        addRule.invoke(builder, attributes, rule);
    }

    boolean isVoiceRuleAccepted() {
        return voiceRuleAccepted;
    }

    boolean isUidFilterAccepted() {
        return uidFilterAccepted;
    }

    int getBufferSizeInFrames() {
        return recorder.getBufferSizeInFrames();
    }

    void writeHello(OutputStream output, int packetFrames) throws Exception {
        output.write(createHello(packetFrames));
        output.flush();
    }

    void stream(OutputStream output, int packetFrames) throws Exception {
        final int payloadBytes = packetFrames * Protocol.BYTES_PER_FRAME;
        byte[] packet = new byte[Protocol.PACKET_HEADER_SIZE + payloadBytes];
        ByteBuffer audio = ByteBuffer.allocateDirect(payloadBytes)
                .order(ByteOrder.LITTLE_ENDIAN);
        AudioTimestamp timestamp = new AudioTimestamp();
        recorder.startRecording();
        long sequence = 0;
        long previousTimestamp = 0;
        boolean discontinuity = false;

        while (!Thread.currentThread().isInterrupted()) {
            audio.clear();
            int bytes = recorder.read(audio, payloadBytes, AudioRecord.READ_BLOCKING);
            if (bytes <= 0) {
                discontinuity = true;
                continue;
            }

            int frames = bytes / Protocol.BYTES_PER_FRAME;
            int flags = discontinuity ? Protocol.FLAG_DISCONTINUITY : 0;
            discontinuity = false;

            long captureNs;
            int timestampResult =
                    recorder.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_MONOTONIC);
            if (timestampResult == AudioRecord.SUCCESS
                    && timestamp.nanoTime > previousTimestamp) {
                captureNs = timestamp.nanoTime;
                previousTimestamp = captureNs;
            } else {
                captureNs = System.nanoTime();
                flags |= Protocol.FLAG_TIMESTAMP_FALLBACK;
            }

            Protocol.putU32(packet, 0, Protocol.PACKET_MAGIC);
            Protocol.putU16(packet, 4, Protocol.VERSION);
            Protocol.putU16(packet, 6, Protocol.PACKET_HEADER_SIZE);
            Protocol.putU64(packet, 8, sequence++);
            Protocol.putU64(packet, 16, captureNs);
            Protocol.putU32(packet, 24, frames);
            Protocol.putU32(packet, 28, flags);

            audio.position(0);
            audio.limit(bytes);
            audio.get(packet, Protocol.PACKET_HEADER_SIZE, frames * Protocol.BYTES_PER_FRAME);
            output.write(packet, 0,
                    Protocol.PACKET_HEADER_SIZE + frames * Protocol.BYTES_PER_FRAME);
        }
    }

    private byte[] createHello(int packetFrames) {
        byte[] hello = new byte[Protocol.HELLO_SIZE];
        Protocol.putU32(hello, 0, Protocol.HELLO_MAGIC);
        Protocol.putU16(hello, 4, Protocol.VERSION);
        Protocol.putU16(hello, 6, Protocol.HELLO_SIZE);
        Protocol.putU32(hello, 8, Protocol.SAMPLE_RATE);
        Protocol.putU16(hello, 12, Protocol.CHANNELS);
        Protocol.putU16(hello, 14, Protocol.BITS_PER_SAMPLE);
        Protocol.putU32(hello, 16, packetFrames);
        Protocol.putU32(hello, 20, packageUid);
        int capabilities = 0;
        if (voiceRuleAccepted) {
            capabilities |= Protocol.CAPTURE_VOICE;
        }
        if (uidFilterAccepted) {
            capabilities |= Protocol.CAPTURE_UID_FILTER;
        }
        Protocol.putU32(hello, 24, capabilities);
        Protocol.putU32(hello, 28, 0);
        return hello;
    }

    @Override
    public void close() {
        try {
            if (recorder.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) {
                recorder.stop();
            }
        } catch (IllegalStateException ignored) {
            // Process teardown must continue.
        }
        recorder.release();
        unregister(audioManager, audioPolicy.getClass(), audioPolicy);
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
