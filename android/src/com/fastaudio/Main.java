package com.fastaudio;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Build;
import android.os.Process;

import java.util.concurrent.atomic.AtomicReference;

public final class Main {
    private Main() {
    }

    public static void main(String[] args) {
        String packageName = null;
        String socketName = "fastaudio";
        String microphoneSocketName = "fastaudio-mic";
        int packetFrames = Protocol.LOW_LATENCY_PACKET_FRAMES;
        boolean microphone = false;
        for (int i = 0; i < args.length; ++i) {
            if ("--package".equals(args[i]) && i + 1 < args.length) {
                packageName = args[++i];
            } else if ("--socket".equals(args[i]) && i + 1 < args.length) {
                socketName = args[++i];
            } else if ("--packet-frames".equals(args[i]) && i + 1 < args.length) {
                packetFrames = Integer.parseInt(args[++i]);
            } else if ("--microphone".equals(args[i])) {
                microphone = true;
            } else if ("--microphone-socket".equals(args[i]) && i + 1 < args.length) {
                microphoneSocketName = args[++i];
            }
        }

        if (packageName == null || packageName.isEmpty()) {
            System.err.println("FastAudio: --package is required");
            System.exit(2);
        }
        if (Build.VERSION.SDK_INT < 33) {
            System.err.println("FastAudio: Android 13 or newer is required");
            System.exit(3);
        }
        if (packetFrames < 64 || packetFrames > 4096
                || (packetFrames & (packetFrames - 1)) != 0) {
            System.err.println("FastAudio: --packet-frames must be a power of two "
                    + "between 64 and 4096");
            System.exit(4);
        }

        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
            HiddenApis.exemptHiddenApis();
            HiddenApis.prepareMainLooper();
            Context context = HiddenApis.createShellContext();
            ApplicationInfo app = context.getPackageManager()
                    .getApplicationInfo(packageName, 0);
            int uid = app.uid;

            System.err.println("FastAudio: package=" + packageName + " uid=" + uid);
            try (OemAudioRouting oemRouting = OemAudioRouting.enableForPackage(uid);
                 CaptureSession capture = CaptureSession.create(context, uid);
                 LocalServerSocket server = new LocalServerSocket(socketName)) {
                MicInjectionSession microphoneSession = null;
                LocalServerSocket microphoneServer = null;
                Thread microphoneThread = null;
                AtomicReference<LocalSocket> microphoneClient = new AtomicReference<>();
                try {
                    if (microphone) {
                        try {
                            microphoneSession = MicInjectionSession.create(context, uid);
                            System.err.println("FastAudio: microphone injector ready for uid=" + uid);
                        } catch (Throwable microphoneError) {
                            // Playback must remain usable if an OEM rejects recorder injection.
                            System.err.println("FastAudio: microphone injector unavailable: "
                                    + microphoneError);
                        }
                        microphoneServer = new LocalServerSocket(microphoneSocketName);
                    }

                    try (LocalSocket client = server.accept()) {
                        capture.writeHello(client.getOutputStream(), packetFrames);
                        System.err.println("FastAudio: client connected, voiceRule="
                                + capture.isVoiceRuleAccepted() + " uidFilter="
                                + capture.isUidFilterAccepted() + " packetFrames="
                                + packetFrames + " recordBufferFrames="
                                + capture.getBufferSizeInFrames());

                        if (microphoneServer != null) {
                            LocalServerSocket finalMicrophoneServer = microphoneServer;
                            MicInjectionSession finalMicrophoneSession = microphoneSession;
                            microphoneThread = new Thread(() -> {
                                try (LocalSocket accepted = finalMicrophoneServer.accept()) {
                                    microphoneClient.set(accepted);
                                    MicInjectionSession.writeHello(
                                            accepted.getOutputStream(), uid,
                                            finalMicrophoneSession != null);
                                    if (finalMicrophoneSession != null) {
                                        finalMicrophoneSession.stream(accepted.getInputStream());
                                    }
                                } catch (Throwable microphoneError) {
                                    System.err.println("FastAudio: microphone stream stopped: "
                                            + microphoneError);
                                } finally {
                                    microphoneClient.set(null);
                                    if (finalMicrophoneSession != null) {
                                        finalMicrophoneSession.close();
                                    }
                                }
                            }, "FastAudioMicInjector");
                            microphoneThread.setPriority(Thread.MAX_PRIORITY);
                            microphoneThread.start();
                        }

                        // Mic socket setup is deliberately concurrent: a failed desktop
                        // microphone must never delay the phone-to-PC playback stream.
                        capture.stream(client.getOutputStream(), packetFrames);
                    }
                } finally {
                    if (microphoneServer != null) {
                        try {
                            microphoneServer.close();
                        } catch (Exception ignoredClose) {
                            // Socket teardown is best effort.
                        }
                    }
                    LocalSocket activeMicrophoneClient = microphoneClient.getAndSet(null);
                    if (activeMicrophoneClient != null) {
                        try {
                            activeMicrophoneClient.close();
                        } catch (Exception ignoredClose) {
                            // Socket teardown is best effort.
                        }
                    }
                    if (microphoneThread != null) {
                        try {
                            microphoneThread.join(1000);
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                        }
                    }
                    if (microphoneSession != null) {
                        microphoneSession.close();
                    }
                }
            }
        } catch (Throwable e) {
            System.err.println("FastAudio fatal: " + e);
            e.printStackTrace(System.err);
            System.exit(1);
        }
    }
}
