/*
 * JNI bridge: app.xemu.XemuNative.native* -> ui/xemu-android.c
 *
 * Copyright (c) 2026 xemu contributors. SPDX-License-Identifier: GPL-2.0
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#define TAG "xemu-jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern int  xemu_android_start(const char *config_json, ANativeWindow *win);
extern void xemu_android_set_surface(ANativeWindow *win);
extern void xemu_android_pause(void);
extern void xemu_android_resume(void);
extern void xemu_android_shutdown(void);
extern void xemu_android_send_button(int controller, int button, int down);
extern void xemu_android_send_axis(int controller, int axis, float value);

#define JNI_FN(name) Java_app_xemu_XemuNative_##name

JNIEXPORT jint JNICALL JNI_FN(nativeStart)(JNIEnv *env, jclass cls,
                                            jstring jconfig, jobject jsurface)
{
    (void)cls;
    const char *cfg = jconfig ? (*env)->GetStringUTFChars(env, jconfig, NULL) : NULL;
    ANativeWindow *win = jsurface ? ANativeWindow_fromSurface(env, jsurface) : NULL;
    if (!win) {
        LOGE("nativeStart: surface is null");
    }
    jint rc = (jint)xemu_android_start(cfg, win);
    if (cfg) {
        (*env)->ReleaseStringUTFChars(env, jconfig, cfg);
    }
    return rc;
}

JNIEXPORT void JNICALL JNI_FN(nativeSurfaceChanged)(JNIEnv *env, jclass cls,
                                                     jobject jsurface)
{
    (void)cls;
    ANativeWindow *win = jsurface ? ANativeWindow_fromSurface(env, jsurface) : NULL;
    xemu_android_set_surface(win);
}

JNIEXPORT void JNICALL JNI_FN(nativePause)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    xemu_android_pause();
}

JNIEXPORT void JNICALL JNI_FN(nativeResume)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    xemu_android_resume();
}

JNIEXPORT void JNICALL JNI_FN(nativeShutdown)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    xemu_android_shutdown();
}

JNIEXPORT void JNICALL JNI_FN(nativeButton)(JNIEnv *env, jclass cls,
                                             jint controller, jint button,
                                             jboolean down)
{
    (void)env; (void)cls;
    xemu_android_send_button((int)controller, (int)button, down ? 1 : 0);
}

JNIEXPORT void JNICALL JNI_FN(nativeAxis)(JNIEnv *env, jclass cls,
                                           jint controller, jint axis,
                                           jfloat value)
{
    (void)env; (void)cls;
    xemu_android_send_axis((int)controller, (int)axis, (float)value);
}
