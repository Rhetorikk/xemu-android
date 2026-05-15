package app.xemu

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

/**
 * Translucent overlay that maps touch input to Xbox controller events.
 * Layout (landscape):
 *
 *     L  ─────────────────────────────  R
 *     ┌──────────┐              ┌──────────┐
 *     │  LSTICK  │   START      │  Y       │
 *     │   ◯      │   BACK       │ X   B    │
 *     │          │              │  A       │
 *     │  D-PAD   │              │  RSTICK  │
 *     └──────────┘              └──────────┘
 *
 * Multi-touch friendly. Each pointer is tracked independently so the
 * user can hold A while moving the left analog stick.
 *
 * Button IDs match the Xbox controller mapping in hw/xbox/usb/hid.h
 * (BTN_A=0, BTN_B=1, BTN_X=2, BTN_Y=3, BTN_L=4, BTN_R=5, BTN_BACK=6,
 *  BTN_START=7, BTN_LSTICK=8, BTN_RSTICK=9, BTN_DUP=10, BTN_DDOWN=11,
 *  BTN_DLEFT=12, BTN_DRIGHT=13).
 *
 * Axis IDs match hw/xbox/usb/hid.h (LX=0, LY=1, RX=2, RY=3, LT=4, RT=5).
 */
class TouchGamepadOverlay @JvmOverloads constructor(
    ctx: Context, attrs: AttributeSet? = null
) : View(ctx, attrs) {

    private val controllerIndex = 0

    private companion object {
        const val BTN_A = 0
        const val BTN_B = 1
        const val BTN_X = 2
        const val BTN_Y = 3
        const val BTN_L = 4
        const val BTN_R = 5
        const val BTN_BACK = 6
        const val BTN_START = 7
        const val BTN_DUP = 10
        const val BTN_DDOWN = 11
        const val BTN_DLEFT = 12
        const val BTN_DRIGHT = 13

        const val AXIS_LX = 0
        const val AXIS_LY = 1
        const val AXIS_LT = 4
        const val AXIS_RT = 5
    }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.WHITE
        alpha = 160
        strokeWidth = 3f
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.WHITE
        alpha = 60
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        alpha = 220
        textSize = 28f
        textAlign = Paint.Align.CENTER
    }

    private data class Button(
        val id: Int,
        val label: String,
        var rect: RectF = RectF(),
        var pressed: Boolean = false,
        var pointerId: Int = -1,
    )

    private data class Stick(
        val cx: Float,
        val cy: Float,
        val radius: Float,
        var dx: Float = 0f,
        var dy: Float = 0f,
        var pointerId: Int = -1,
        val axisX: Int,
        val axisY: Int,
    )

    private val buttons = mutableListOf<Button>()
    private var leftStick: Stick? = null
    private var rightStick: Stick? = null

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        layoutControls(w.toFloat(), h.toFloat())
    }

    private fun layoutControls(w: Float, h: Float) {
        buttons.clear()
        val unit = min(w, h) / 10f

        // Right cluster: A/B/X/Y diamond
        val rightCx = w - unit * 2f
        val rightCy = h - unit * 2.4f
        fun face(id: Int, label: String, ox: Float, oy: Float): Button {
            val r = unit * 0.7f
            val cx = rightCx + ox; val cy = rightCy + oy
            return Button(id, label, RectF(cx - r, cy - r, cx + r, cy + r))
        }
        buttons += face(BTN_A, "A", 0f,  unit * 0.95f)
        buttons += face(BTN_B, "B", unit * 0.95f, 0f)
        buttons += face(BTN_X, "X", -unit * 0.95f, 0f)
        buttons += face(BTN_Y, "Y", 0f, -unit * 0.95f)

        // Shoulder buttons / triggers
        buttons += Button(BTN_L, "LB",
            RectF(unit * 0.6f, unit * 0.6f, unit * 0.6f + unit * 2.2f, unit * 0.6f + unit * 0.9f))
        buttons += Button(BTN_R, "RB",
            RectF(w - unit * 0.6f - unit * 2.2f, unit * 0.6f,
                  w - unit * 0.6f, unit * 0.6f + unit * 0.9f))
        // Treat triggers as digital LT/RT for the touch UX
        buttons += Button(AXIS_LT or 0x100, "LT",
            RectF(unit * 0.6f, unit * 1.8f, unit * 0.6f + unit * 2.2f, unit * 1.8f + unit * 0.9f))
        buttons += Button(AXIS_RT or 0x100, "RT",
            RectF(w - unit * 0.6f - unit * 2.2f, unit * 1.8f,
                  w - unit * 0.6f, unit * 1.8f + unit * 0.9f))

        // Start / Back
        buttons += Button(BTN_START, "Start",
            RectF(w / 2f + unit * 0.4f, h - unit * 1.6f,
                  w / 2f + unit * 2.2f, h - unit * 0.7f))
        buttons += Button(BTN_BACK, "Back",
            RectF(w / 2f - unit * 2.2f, h - unit * 1.6f,
                  w / 2f - unit * 0.4f, h - unit * 0.7f))

        // D-Pad cluster (top-left of left zone)
        val dpadCx = unit * 2.2f
        val dpadCy = h - unit * 4.0f
        val dr = unit * 0.55f
        fun dpad(id: Int, label: String, ox: Float, oy: Float): Button {
            val cx = dpadCx + ox; val cy = dpadCy + oy
            return Button(id, label, RectF(cx - dr, cy - dr, cx + dr, cy + dr))
        }
        buttons += dpad(BTN_DUP,    "▲",  0f, -unit)
        buttons += dpad(BTN_DDOWN,  "▼",  0f,  unit)
        buttons += dpad(BTN_DLEFT,  "◀", -unit, 0f)
        buttons += dpad(BTN_DRIGHT, "▶",  unit, 0f)

        leftStick = Stick(
            cx = unit * 2.2f, cy = h - unit * 1.7f, radius = unit * 1.2f,
            axisX = AXIS_LX, axisY = AXIS_LY)

        rightStick = Stick(
            cx = w - unit * 2.2f, cy = h - unit * 5.0f, radius = unit * 1.0f,
            axisX = 2, axisY = 3)

        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        for (b in buttons) {
            paint.alpha = if (b.pressed) 220 else 160
            fillPaint.alpha = if (b.pressed) 130 else 60
            canvas.drawRoundRect(b.rect, 16f, 16f, fillPaint)
            canvas.drawRoundRect(b.rect, 16f, 16f, paint)
            canvas.drawText(b.label, b.rect.centerX(),
                b.rect.centerY() + labelPaint.textSize / 3f, labelPaint)
        }
        for (s in listOfNotNull(leftStick, rightStick)) {
            canvas.drawCircle(s.cx, s.cy, s.radius, paint)
            canvas.drawCircle(s.cx + s.dx, s.cy + s.dy, s.radius * 0.4f, fillPaint)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val idx = event.actionIndex
                handleDown(event.getPointerId(idx), event.getX(idx), event.getY(idx))
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    handleMove(event.getPointerId(i), event.getX(i), event.getY(i))
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                val idx = event.actionIndex
                handleUp(event.getPointerId(idx))
            }
        }
        invalidate()
        return true
    }

    private fun handleDown(pid: Int, x: Float, y: Float) {
        // Sticks claim the area within their radius first.
        for (s in listOfNotNull(leftStick, rightStick)) {
            if (s.pointerId == -1 && hypot(x - s.cx, y - s.cy) <= s.radius * 1.4f) {
                s.pointerId = pid
                updateStick(s, x, y)
                return
            }
        }
        for (b in buttons) {
            if (!b.pressed && b.rect.contains(x, y)) {
                b.pressed = true
                b.pointerId = pid
                emitButton(b.id, true)
                return
            }
        }
    }

    private fun handleMove(pid: Int, x: Float, y: Float) {
        for (s in listOfNotNull(leftStick, rightStick)) {
            if (s.pointerId == pid) {
                updateStick(s, x, y)
                return
            }
        }
    }

    private fun handleUp(pid: Int) {
        for (s in listOfNotNull(leftStick, rightStick)) {
            if (s.pointerId == pid) {
                s.pointerId = -1
                s.dx = 0f; s.dy = 0f
                XemuNative.nativeAxis(controllerIndex, s.axisX, 0f)
                XemuNative.nativeAxis(controllerIndex, s.axisY, 0f)
                return
            }
        }
        for (b in buttons) {
            if (b.pointerId == pid) {
                b.pressed = false
                b.pointerId = -1
                emitButton(b.id, false)
                return
            }
        }
    }

    private fun updateStick(s: Stick, x: Float, y: Float) {
        val rawDx = x - s.cx; val rawDy = y - s.cy
        val mag = hypot(rawDx, rawDy)
        val clamp = min(mag, s.radius)
        if (mag > 0f) {
            s.dx = rawDx / mag * clamp
            s.dy = rawDy / mag * clamp
        } else { s.dx = 0f; s.dy = 0f }
        val nx = max(-1f, min(1f, s.dx / s.radius))
        // Android Y points down; Xbox stick Y is up-positive, so invert.
        val ny = max(-1f, min(1f, -s.dy / s.radius))
        XemuNative.nativeAxis(controllerIndex, s.axisX, nx)
        XemuNative.nativeAxis(controllerIndex, s.axisY, ny)
    }

    private fun emitButton(id: Int, down: Boolean) {
        // Synthetic IDs with the high bit set are triggers reported as axes.
        if ((id and 0x100) != 0) {
            val axis = id and 0xff
            XemuNative.nativeAxis(controllerIndex, axis, if (down) 1f else 0f)
        } else {
            XemuNative.nativeButton(controllerIndex, id, down)
        }
    }
}
