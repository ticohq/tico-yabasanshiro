package org.uoyabause.android

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.util.AttributeSet
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.View
import androidx.preference.PreferenceManager
import com.google.android.material.color.MaterialColors
import org.devmiyax.yabasanshiro.R
import java.util.Locale

/**
 * Shows a Saturn controller built from the on-screen pad images, with the
 * keyboard key bound to each button, then fades out. Used at game start when
 * player 1 plays with a keyboard.
 */
class KeyboardMapOverlayView
    @JvmOverloads
    constructor(
        context: Context,
        attrs: AttributeSet? = null,
        defStyle: Int = 0,
    ) : View(context, attrs, defStyle) {
        private class Part(
            val res: Int,
            val src: Rect,
            val dstX: Float,
            val dstY: Float,
        ) {
            var bitmap: Bitmap? = null
        }

        private var parts: List<Part> = emptyList()
        private var labels: List<Pair<Int, String>> = emptyList()
        private val labelPos = HashMap<Int, Pair<Float, Float>>()

        private val bitmapPaint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
        private val bodyPaint = Paint(Paint.ANTI_ALIAS_FLAG)
        private val chipPaint = Paint(Paint.ANTI_ALIAS_FLAG)
        private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG)
        private val chipRect = RectF()
        private val dstRect = RectF()

        init {
            bodyPaint.color = MaterialColors.getColor(this, com.google.android.material.R.attr.colorSurfaceContainerHigh)
            bodyPaint.alpha = BODY_ALPHA
            chipPaint.color = MaterialColors.getColor(this, androidx.appcompat.R.attr.colorPrimary)
            textPaint.color = MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnPrimary)
            textPaint.textAlign = Paint.Align.CENTER
            textPaint.isFakeBoldText = true
            textPaint.textSize = LABEL_TEXT_SIZE
            visibility = GONE
        }

        /**
         * Shows [keyMap] (Saturn button -> key code) for a few seconds and fades
         * out. Does nothing when no button is bound.
         */
        fun show(
            keyMap: Map<Int, Int>,
            deviceId: Int,
        ) {
            val list = ArrayList<Pair<Int, String>>()
            for ((button, code) in keyMap) {
                if (labelPosition(button) == null) continue
                list.add(button to keyLabel(code, deviceId))
            }
            if (list.isEmpty()) return
            labels = list
            loadParts()
            animate().cancel()
            alpha = 1f
            visibility = VISIBLE
            invalidate()
            animate()
                .alpha(0f)
                .setStartDelay(SHOW_MS)
                .setDuration(FADE_MS)
                .withEndAction { hide() }
                .start()
        }

        fun hide() {
            animate().cancel()
            visibility = GONE
            parts.forEach { it.bitmap = null }
        }

        override fun onMeasure(
            widthMeasureSpec: Int,
            heightMeasureSpec: Int,
        ) {
            val w = MeasureSpec.getSize(widthMeasureSpec)
            setMeasuredDimension(w, (w * TOTAL_H / TOTAL_W).toInt())
        }

        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            if (width == 0) return
            val s = width / TOTAL_W
            canvas.save()
            canvas.scale(s, s)

            chipRect.set(0f, BODY_TOP - MARGIN / 2, TOTAL_W, TOTAL_H)
            canvas.drawRoundRect(chipRect, BODY_RADIUS, BODY_RADIUS, bodyPaint)

            for (p in parts) {
                val bmp = p.bitmap ?: continue
                val sampled =
                    Rect(
                        p.src.left / SAMPLE,
                        p.src.top / SAMPLE,
                        p.src.right / SAMPLE,
                        p.src.bottom / SAMPLE,
                    )
                dstRect.set(
                    p.dstX,
                    p.dstY,
                    p.dstX + p.src.width(),
                    p.dstY + p.src.height(),
                )
                canvas.drawBitmap(bmp, sampled, dstRect, bitmapPaint)
            }

            val fm = textPaint.fontMetrics
            for ((button, text) in labels) {
                val (cx, cy) = labelPosition(button) ?: continue
                val tw = textPaint.measureText(text)
                val half = maxOf(tw / 2 + CHIP_PAD_X, CHIP_MIN_HALF)
                val h = (fm.descent - fm.ascent) / 2 + CHIP_PAD_Y
                chipRect.set(cx - half, cy - h, cx + half, cy + h)
                canvas.drawRoundRect(chipRect, h, h, chipPaint)
                canvas.drawText(text, cx, cy - (fm.ascent + fm.descent) / 2, textPaint)
            }
            canvas.restore()
        }

        private fun loadParts() {
            val classic =
                PreferenceManager
                    .getDefaultSharedPreferences(context)
                    .getBoolean("pref_classic_pad_style", false)
            val l = if (classic) R.drawable.pad_l else R.drawable.pad_l_new
            val r = if (classic) R.drawable.pad_r else R.drawable.pad_r_new
            val m = if (classic) R.drawable.pad_m else R.drawable.pad_m_new
            val tl = if (classic) R.drawable.pad_top_l else R.drawable.pad_top_l_new
            val tr = if (classic) R.drawable.pad_top_r else R.drawable.pad_top_r_new
            parts =
                listOf(
                    Part(tl, SRC_TOP_L, TOP_L_X, 0f),
                    Part(tr, SRC_TOP_R, TOP_R_X, 0f),
                    Part(l, SRC_DPAD, DPAD_X, DPAD_Y),
                    Part(m, SRC_START, START_X, START_Y),
                    Part(r, SRC_BUTTONS, BUTTONS_X, BODY_TOP),
                )
            val options =
                BitmapFactory.Options().apply {
                    inScaled = false
                    inSampleSize = SAMPLE
                }
            parts.forEach { it.bitmap = BitmapFactory.decodeResource(resources, it.res, options) }
        }

        private fun labelPosition(button: Int): Pair<Float, Float>? {
            if (labelPos.isEmpty()) {
                val dcx = DPAD_X + (DPAD_CENTER_X - SRC_DPAD.left)
                val dcy = DPAD_Y + (DPAD_CENTER_Y - SRC_DPAD.top)
                labelPos[PadEvent.BUTTON_UP] = dcx to dcy - DPAD_ARM
                labelPos[PadEvent.BUTTON_DOWN] = dcx to dcy + DPAD_ARM
                labelPos[PadEvent.BUTTON_LEFT] = dcx - DPAD_ARM to dcy
                labelPos[PadEvent.BUTTON_RIGHT] = dcx + DPAD_ARM to dcy
                labelPos[PadEvent.BUTTON_START] = START_X + SRC_START.width() / 2f to START_Y + START_CENTER_Y
                labelPos[PadEvent.BUTTON_LEFT_TRIGGER] = TOP_L_X + SRC_TOP_L.width() / 2f to SRC_TOP_L.height() / 2f
                labelPos[PadEvent.BUTTON_RIGHT_TRIGGER] = TOP_R_X + SRC_TOP_R.width() / 2f to SRC_TOP_R.height() / 2f
                // Button centers inside pad_r (same rectangles as YabausePad's hit areas)
                val face =
                    mapOf(
                        PadEvent.BUTTON_A to Rect(59, 801, 59 + 213, 801 + 225),
                        PadEvent.BUTTON_B to Rect(268, 672, 268 + 229, 672 + 221),
                        PadEvent.BUTTON_C to Rect(507, 577, 507 + 224, 577 + 229),
                        PadEvent.BUTTON_X to Rect(15, 602, 15 + 149, 602 + 150),
                        PadEvent.BUTTON_Y to Rect(202, 481, 202 + 149, 481 + 148),
                        PadEvent.BUTTON_Z to Rect(397, 409, 397 + 151, 409 + 152),
                    )
                for ((b, rc) in face) {
                    labelPos[b] =
                        BUTTONS_X + (rc.exactCenterX() - SRC_BUTTONS.left) to
                        BODY_TOP + (rc.exactCenterY() - SRC_BUTTONS.top)
                }
            }
            return labelPos[button]
        }

        private fun keyLabel(
            code: Int,
            deviceId: Int,
        ): String {
            SPECIAL_KEYS[code]?.let { return it }
            val label =
                runCatching { KeyCharacterMap.load(deviceId).getDisplayLabel(code) }
                    .getOrElse { runCatching { KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD).getDisplayLabel(code) }.getOrDefault(0.toChar()) }
            if (label.code > ' '.code) return label.toString().uppercase(Locale.ROOT)
            return KeyEvent
                .keyCodeToString(code)
                .removePrefix("KEYCODE_")
                .lowercase(Locale.ROOT)
                .replaceFirstChar { it.titlecase(Locale.ROOT) }
        }

        companion object {
            private const val SAMPLE = 2
            private const val SHOW_MS = 5000L
            private const val FADE_MS = 1500L
            private const val BODY_ALPHA = 0xD0

            // Layout in source-image pixels; the view scales it to its width.
            private val SRC_TOP_L = Rect(50, 40, 445, 155)
            private val SRC_TOP_R = Rect(330, 40, 725, 155)
            private val SRC_DPAD = Rect(110, 480, 590, 955)
            private val SRC_START = Rect(0, 0, 196, 133)
            private val SRC_BUTTONS = Rect(10, 400, 740, 1030)
            private const val DPAD_CENTER_X = 347f
            private const val DPAD_CENTER_Y = 720f
            private const val DPAD_ARM = 165f
            private const val START_CENTER_Y = 92f

            private const val MARGIN = 40f
            private const val BODY_TOP = 140f
            private const val DPAD_X = MARGIN
            private const val DPAD_Y = BODY_TOP + 60f
            private const val START_X = DPAD_X + 480f + MARGIN
            private const val START_Y = BODY_TOP + 380f
            private const val BUTTONS_X = START_X + 196f + MARGIN
            private const val TOTAL_W = BUTTONS_X + 730f + MARGIN
            private const val TOTAL_H = BODY_TOP + 630f + MARGIN
            private const val TOP_L_X = MARGIN + 60f
            private const val TOP_R_X = TOTAL_W - MARGIN - 60f - 395f
            private const val BODY_RADIUS = 160f

            private const val LABEL_TEXT_SIZE = 64f
            private const val CHIP_PAD_X = 22f
            private const val CHIP_PAD_Y = 10f
            private const val CHIP_MIN_HALF = 44f

            private val SPECIAL_KEYS =
                mapOf(
                    KeyEvent.KEYCODE_DPAD_UP to "↑",
                    KeyEvent.KEYCODE_DPAD_DOWN to "↓",
                    KeyEvent.KEYCODE_DPAD_LEFT to "←",
                    KeyEvent.KEYCODE_DPAD_RIGHT to "→",
                    KeyEvent.KEYCODE_ENTER to "Enter",
                    KeyEvent.KEYCODE_NUMPAD_ENTER to "Enter",
                    KeyEvent.KEYCODE_SPACE to "Space",
                    KeyEvent.KEYCODE_TAB to "Tab",
                    KeyEvent.KEYCODE_ESCAPE to "Esc",
                    KeyEvent.KEYCODE_DEL to "BS",
                    KeyEvent.KEYCODE_SHIFT_LEFT to "Shift",
                    KeyEvent.KEYCODE_SHIFT_RIGHT to "Shift",
                    KeyEvent.KEYCODE_CTRL_LEFT to "Ctrl",
                    KeyEvent.KEYCODE_CTRL_RIGHT to "Ctrl",
                    KeyEvent.KEYCODE_ALT_LEFT to "Alt",
                    KeyEvent.KEYCODE_ALT_RIGHT to "Alt",
                )
        }
    }
