package app.xemu

import android.view.Surface

/**
 * Thin Kotlin facade for the JNI bridge in cpp/jni_xemu.c.
 *
 * Native methods land in ui/xemu-android.c via the shim in jni_xemu.c.
 */
object XemuNative {

    @Volatile private var loaded = false

    fun ensureLoaded() {
        if (loaded) return
        synchronized(this) {
            if (loaded) return
            // SDL3 loads itself when SDLActivity boots; we explicitly load
            // libxemu so the shared symbols are resolved before JNI calls.
            System.loadLibrary("xemu")
            loaded = true
        }
    }

    @JvmStatic external fun nativeStart(configJson: String, surface: Surface): Int
    @JvmStatic external fun nativeSurfaceChanged(surface: Surface?)
    @JvmStatic external fun nativePause()
    @JvmStatic external fun nativeResume()
    @JvmStatic external fun nativeShutdown()
    @JvmStatic external fun nativeButton(controller: Int, button: Int, down: Boolean)
    @JvmStatic external fun nativeAxis(controller: Int, axis: Int, value: Float)
}
