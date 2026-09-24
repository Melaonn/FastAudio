package com.fastaudio;

import android.os.Build;

import java.lang.reflect.Method;
import java.util.Locale;

final class OemAudioRouting implements AutoCloseable {
    private static final String VIVO_VOICE_CAPTURE_UID =
            "VoicePlaybackAllowCaputuredUid";

    private final boolean vivoVoiceRouteEnabled;

    private OemAudioRouting(boolean vivoVoiceRouteEnabled) {
        this.vivoVoiceRouteEnabled = vivoVoiceRouteEnabled;
    }

    static OemAudioRouting enableForPackage(int uid) {
        if (!isVivoFamily()) {
            return new OemAudioRouting(false);
        }

        boolean enabled =
                setAudioSystemParameter(VIVO_VOICE_CAPTURE_UID + "=" + uid);
        if (enabled) {
            System.err.println("FastAudio: Vivo teammate voice route enabled for uid="
                    + uid);
        } else {
            System.err.println("FastAudio: Vivo teammate voice route unavailable");
        }
        return new OemAudioRouting(enabled);
    }

    private static boolean isVivoFamily() {
        String identity = (Build.MANUFACTURER + " " + Build.BRAND)
                .toLowerCase(Locale.ROOT);
        return identity.contains("vivo") || identity.contains("iqoo");
    }

    private static boolean setAudioSystemParameter(String keyValue) {
        try {
            Class<?> audioSystem = Class.forName("android.media.AudioSystem");
            Method setParameters =
                    audioSystem.getDeclaredMethod("setParameters", String.class);
            setParameters.setAccessible(true);
            Object result = setParameters.invoke(null, keyValue);
            return result instanceof Integer && ((Integer) result) == 0;
        } catch (ReflectiveOperationException | RuntimeException e) {
            System.err.println("FastAudio: OEM audio parameter failed: "
                    + e.getClass().getSimpleName());
            return false;
        }
    }

    @Override
    public void close() {
        if (vivoVoiceRouteEnabled) {
            setAudioSystemParameter(VIVO_VOICE_CAPTURE_UID + "=-1");
        }
    }
}
