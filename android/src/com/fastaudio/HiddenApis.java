package com.fastaudio;

import android.content.Context;
import android.os.Looper;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

final class HiddenApis {
    private HiddenApis() {
    }

    static void exemptHiddenApis() throws ReflectiveOperationException {
        Class<?> vmRuntimeClass = Class.forName("dalvik.system.VMRuntime");
        Method getRuntime = vmRuntimeClass.getDeclaredMethod("getRuntime");
        Method setHiddenApiExemptions =
                vmRuntimeClass.getDeclaredMethod("setHiddenApiExemptions", String[].class);
        Object runtime = getRuntime.invoke(null);
        setHiddenApiExemptions.invoke(runtime, (Object) new String[]{"L"});
    }

    static void prepareMainLooper() throws ReflectiveOperationException {
        if (Looper.myLooper() == null) {
            Looper.prepare();
        }
        Field mainLooper = Looper.class.getDeclaredField("sMainLooper");
        mainLooper.setAccessible(true);
        synchronized (Looper.class) {
            mainLooper.set(null, Looper.myLooper());
        }
    }

    static Context createShellContext() {
        AppProcessWorkarounds.apply();
        return new ShellContext(AppProcessWorkarounds.getSystemContext());
    }
}
