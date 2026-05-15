package app.xemu

import android.app.Activity
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.FrameLayout

/**
 * Hosts the SurfaceView that xemu's Vulkan swapchain attaches to.
 *
 * Foundation pass: standalone SurfaceView + SurfaceHolder.Callback. Later,
 * when we re-enable SDL3's input/lifecycle, this should extend
 * `org.libsdl.app.SDLActivity` and override `getMainSharedObject()` /
 * `getLibraries()` to load "xemu".
 */
class EmulatorActivity : Activity(), SurfaceHolder.Callback {

    private lateinit var surfaceView: SurfaceView
    private var started = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        XemuNative.ensureLoaded()

        val root = FrameLayout(this)
        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        root.addView(surfaceView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT))

        // Touch overlay sits above the SurfaceView and translates touches to
        // Xbox controller events via XemuNative.nativeButton / nativeAxis.
        val overlay = TouchGamepadOverlay(this)
        root.addView(overlay,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT))

        setContentView(root)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (!started) {
            val cfg = intent.getStringExtra("config_json") ?: "{}"
            val rc = XemuNative.nativeStart(cfg, holder.surface)
            if (rc != 0) {
                finish()
                return
            }
            started = true
        } else {
            XemuNative.nativeSurfaceChanged(holder.surface)
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        XemuNative.nativeSurfaceChanged(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Don't shut down here - the user may have just rotated/backgrounded.
        // We let xemu's swapchain recreate on the next surfaceCreated.
        XemuNative.nativeSurfaceChanged(null)
    }

    override fun onPause() {
        super.onPause()
        if (started) XemuNative.nativePause()
    }

    override fun onResume() {
        super.onResume()
        if (started) XemuNative.nativeResume()
    }

    override fun onDestroy() {
        if (started) XemuNative.nativeShutdown()
        super.onDestroy()
    }
}
