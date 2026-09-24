package com.fastaudio;

import android.app.Application;
import android.app.Instrumentation;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.Build;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

final class AppProcessWorkarounds {
    private static final Class<?> ACTIVITY_THREAD_CLASS;
    private static final Object ACTIVITY_THREAD;

    static {
        try {
            ACTIVITY_THREAD_CLASS = Class.forName("android.app.ActivityThread");
            Constructor<?> constructor =
                    ACTIVITY_THREAD_CLASS.getDeclaredConstructor();
            constructor.setAccessible(true);
            ACTIVITY_THREAD = constructor.newInstance();

            Field current =
                    ACTIVITY_THREAD_CLASS.getDeclaredField("sCurrentActivityThread");
            current.setAccessible(true);
            current.set(null, ACTIVITY_THREAD);

            Field systemThread =
                    ACTIVITY_THREAD_CLASS.getDeclaredField("mSystemThread");
            systemThread.setAccessible(true);
            systemThread.setBoolean(ACTIVITY_THREAD, true);
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    private AppProcessWorkarounds() {
    }

    static void apply() {
        if (Build.VERSION.SDK_INT >= 31) {
            fillConfigurationController();
        }
        fillAppInfo();
        fillAppContext();
    }

    static Context getSystemContext() {
        try {
            Method method =
                    ACTIVITY_THREAD_CLASS.getDeclaredMethod("getSystemContext");
            method.setAccessible(true);
            return (Context) method.invoke(ACTIVITY_THREAD);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Could not create system context", e);
        }
    }

    private static void fillAppInfo() {
        try {
            Class<?> bindDataClass =
                    Class.forName("android.app.ActivityThread$AppBindData");
            Constructor<?> constructor = bindDataClass.getDeclaredConstructor();
            constructor.setAccessible(true);
            Object bindData = constructor.newInstance();

            ApplicationInfo applicationInfo = new ApplicationInfo();
            applicationInfo.packageName = "com.android.shell";

            Field appInfo = bindDataClass.getDeclaredField("appInfo");
            appInfo.setAccessible(true);
            appInfo.set(bindData, applicationInfo);

            Field boundApplication =
                    ACTIVITY_THREAD_CLASS.getDeclaredField("mBoundApplication");
            boundApplication.setAccessible(true);
            boundApplication.set(ACTIVITY_THREAD, bindData);
        } catch (ReflectiveOperationException e) {
            System.err.println("FastAudio: app-info workaround failed: " + e);
        }
    }

    private static void fillAppContext() {
        try {
            Context context = new ShellContext(getSystemContext());
            Application app =
                    Instrumentation.newApplication(Application.class, context);
            Field initialApplication =
                    ACTIVITY_THREAD_CLASS.getDeclaredField("mInitialApplication");
            initialApplication.setAccessible(true);
            initialApplication.set(ACTIVITY_THREAD, app);
        } catch (ReflectiveOperationException e) {
            System.err.println("FastAudio: app-context workaround failed: " + e);
        }
    }

    private static void fillConfigurationController() {
        try {
            Class<?> controllerClass =
                    Class.forName("android.app.ConfigurationController");
            Constructor<?> constructor =
                    controllerClass.getDeclaredConstructor(ACTIVITY_THREAD_CLASS);
            constructor.setAccessible(true);
            Object controller = constructor.newInstance(ACTIVITY_THREAD);
            Field field =
                    ACTIVITY_THREAD_CLASS.getDeclaredField("mConfigurationController");
            field.setAccessible(true);
            field.set(ACTIVITY_THREAD, controller);
        } catch (ReflectiveOperationException e) {
            System.err.println("FastAudio: configuration workaround failed: " + e);
        }
    }
}
