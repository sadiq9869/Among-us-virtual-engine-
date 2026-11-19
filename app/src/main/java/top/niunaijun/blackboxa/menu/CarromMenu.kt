package top.niunaijun.blackboxa.menu

import android.app.Activity
import android.app.Application
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast

/**
 * JNI bridge to libcarprobe.so (Bcore/src/main/cpp/carprobe/carprobe.cpp, v6.0).
 * The C++ functions are named Java_top_niunaijun_blackboxa_menu_NativeCarrom_<method>, so this object's
 * package, name and method names must not change. (Keep rule in app/proguard-rules.pro.)
 */
object NativeCarrom {
    external fun setEnabled(on: Int)
    // frictionPercent 50..150, maxBounces 0..5, deflection 0/1, recon 0/1 (values outside the range are clamped by the native side)
    external fun setOptions(frictionPercent: Int, maxBounces: Int, deflection: Int, recon: Int)
    // [designW, designH, nPaths, {type, nPoints, x, y, ...}...] in design points, y up.
    // type 0 striker (white), 1 coin hit by the striker (red), 2 striker remainder (thin white), 5 coin moved by another coin (orange),
    // 3 ring "coin potted" (2 points: centre, point on the ring), 4 ring "striker potted = foul",
    // 6 ghost: striker where it stops, 7 ghost: coin where it stops, 8 ghost: striker at the first contact (all three: centre + point on the ring).
    external fun lines(): FloatArray
    external fun statusText(): String
    // text of the last <= 20 shots: start state, predicted ends, real ends, errors (empty string = no shot yet)
    external fun consoleText(): String
}

/**
 * Draws the prediction lines. It never consumes touches: the player aims through it.
 * Pixel mapping: the box of the game's SurfaceView (or this view if none is found) is the design area,
 *   px = left + x / designW * w ,  py = top + (designH - y) / designH * h
 */
class CarromLineView(context: Context) : View(context) {
    @Volatile var data: FloatArray = FloatArray(0)
    var surface: View? = null

    private val density = context.resources.displayMetrics.density
    private val shadow = Paint(Paint.ANTI_ALIAS_FLAG)
    private val white = Paint(Paint.ANTI_ALIAS_FLAG)
    private val red = Paint(Paint.ANTI_ALIAS_FLAG)
    private val thin = Paint(Paint.ANTI_ALIAS_FLAG)
    private val orange = Paint(Paint.ANTI_ALIAS_FLAG)
    private val ringOk = Paint(Paint.ANTI_ALIAS_FLAG)
    private val ringFoul = Paint(Paint.ANTI_ALIAS_FLAG)
    private val ghostStrikerLine = Paint(Paint.ANTI_ALIAS_FLAG)
    private val ghostStrikerFill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val ghostCoinLine = Paint(Paint.ANTI_ALIAS_FLAG)
    private val ghostCoinFill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val path = Path()
    private val loc = IntArray(2)
    private val locSelf = IntArray(2)

    init {
        isClickable = false
        isFocusable = false
        isFocusableInTouchMode = false
        setWillNotDraw(false)
        for (p in listOf(shadow, white, red, thin, orange, ringOk, ringFoul, ghostStrikerLine, ghostCoinLine)) {
            p.style = Paint.Style.STROKE
            p.strokeJoin = Paint.Join.ROUND
            p.strokeCap = Paint.Cap.ROUND
        }
        ghostStrikerFill.style = Paint.Style.FILL
        ghostCoinFill.style = Paint.Style.FILL
        shadow.color = Color.argb(110, 0, 0, 0)
        shadow.strokeWidth = 5.5f * density
        white.color = Color.WHITE
        white.strokeWidth = 3f * density
        red.color = Color.rgb(235, 40, 40)
        red.strokeWidth = 3f * density
        thin.color = Color.argb(215, 255, 255, 255)
        thin.strokeWidth = 2f * density
        orange.color = Color.rgb(255, 150, 30)
        orange.strokeWidth = 2.5f * density
        ringOk.color = Color.rgb(60, 220, 110)
        ringOk.strokeWidth = 3.5f * density
        ringFoul.color = Color.rgb(255, 60, 60)
        ringFoul.strokeWidth = 4.5f * density
        ghostStrikerLine.color = Color.argb(230, 255, 255, 255)
        ghostStrikerLine.strokeWidth = 2f * density
        ghostStrikerFill.color = Color.argb(55, 255, 255, 255)
        ghostCoinLine.color = Color.argb(235, 255, 90, 90)
        ghostCoinLine.strokeWidth = 2f * density
        ghostCoinFill.color = Color.argb(50, 255, 80, 80)
    }

    override fun onTouchEvent(event: MotionEvent?): Boolean = false

    override fun onDraw(canvas: Canvas) {
        val d = data
        if (d.size < 3) return
        val designW = d[0]
        val designH = d[1]
        if (!(designW > 1f) || !(designH > 1f)) return

        var left = 0f
        var top = 0f
        var w = width.toFloat()
        var h = height.toFloat()
        val s = surface
        if (s != null && s.isAttachedToWindow && s.width > 0 && s.height > 0) {
            s.getLocationOnScreen(loc)
            getLocationOnScreen(locSelf)
            left = (loc[0] - locSelf[0]).toFloat()
            top = (loc[1] - locSelf[1]).toFloat()
            w = s.width.toFloat()
            h = s.height.toFloat()
        }

        val nPaths = d[2].toInt()
        var i = 3
        for (k in 0 until nPaths) {
            if (i + 2 > d.size) break
            val type = d[i].toInt()
            val n = d[i + 1].toInt()
            i += 2
            if (n < 2 || i + 2 * n > d.size) break
            path.reset()
            var rx0 = 0f
            var ry0 = 0f
            var rx1 = 0f
            var ry1 = 0f
            for (j in 0 until n) {
                val x = d[i + 2 * j]
                val y = d[i + 2 * j + 1]
                val px = left + x / designW * w
                val py = top + (designH - y) / designH * h
                if (j == 0) { path.moveTo(px, py); rx0 = px; ry0 = py } else path.lineTo(px, py)
                if (j == 1) { rx1 = px; ry1 = py }
            }
            i += 2 * n
            when (type) {
                3, 4 -> {
                    val rad = Math.hypot((rx1 - rx0).toDouble(), (ry1 - ry0).toDouble()).toFloat()
                    canvas.drawCircle(rx0, ry0, rad, if (type == 4) ringFoul else ringOk)
                }
                6, 8 -> {
                    val rad = Math.hypot((rx1 - rx0).toDouble(), (ry1 - ry0).toDouble()).toFloat()
                    canvas.drawCircle(rx0, ry0, rad, ghostStrikerFill)
                    canvas.drawCircle(rx0, ry0, rad, ghostStrikerLine)
                }
                7 -> {
                    val rad = Math.hypot((rx1 - rx0).toDouble(), (ry1 - ry0).toDouble()).toFloat()
                    canvas.drawCircle(rx0, ry0, rad, ghostCoinFill)
                    canvas.drawCircle(rx0, ry0, rad, ghostCoinLine)
                }
                else -> {
                    canvas.drawPath(path, shadow)
                    canvas.drawPath(path, when (type) { 1 -> red; 2 -> thin; 5 -> orange; else -> white })
                }
            }
        }
    }
}

/**
 * In-game menu for Carrom Pool (com.miniclip.carrom): a small draggable "C" button; its panel has only the
 * prediction on/off button and a console box (last shots: prediction vs. real result) with Copy and Close buttons.
 * Plus the full-screen line overlay. Started from BlackBoxLoader.afterApplicationOnCreate.
 */
object CarromMenu {
    private const val TAG = "CarromMenu"
    private const val OVERLAY_TAG = "carrommenu_overlay"
    private const val PREFS = "carrom_menu"

    private var registered = false
    private val ui = Handler(Looper.getMainLooper())

    private var enabled = true

    private fun dp(activity: Activity, v: Int): Int =
        (v * activity.resources.displayMetrics.density + 0.5f).toInt()

    private fun loadPrefs(activity: Activity) {
        try {
            enabled = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean("enabled", true)
        } catch (t: Throwable) {
            Log.w(TAG, "prefs load failed: $t")
        }
    }

    private fun savePrefs(activity: Activity) {
        try {
            activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean("enabled", enabled).apply()
        } catch (t: Throwable) {
            Log.w(TAG, "prefs save failed: $t")
        }
    }

    // friction 100 %, 5 wall bounces, striker line after the hit on, shot recording on (the console needs it)
    private fun pushToNative() {
        try {
            NativeCarrom.setEnabled(if (enabled) 1 else 0)
            NativeCarrom.setOptions(100, 5, 1, 1)
        } catch (t: Throwable) {
            Log.e(TAG, "native call failed (is libcarprobe loaded?): $t")
        }
    }

    private fun readStatus(): String =
        try {
            NativeCarrom.statusText()
        } catch (t: Throwable) {
            "Native library not loaded ($t)"
        }

    private fun readConsole(): String =
        try {
            NativeCarrom.consoleText()
        } catch (t: Throwable) {
            ""
        }

    private fun findSurface(v: View): SurfaceView? {
        var best: SurfaceView? = null
        fun walk(x: View) {
            if (x is SurfaceView && x.width > 0 && x.height > 0) {
                val b = best
                if (b == null || x.width * x.height > b.width * b.height) best = x
            }
            if (x is ViewGroup) for (i in 0 until x.childCount) walk(x.getChildAt(i))
        }
        walk(v)
        return best
    }

    fun init(application: Application?) {
        try {
            if (application == null || registered) return
            registered = true
            application.registerActivityLifecycleCallbacks(
                object : Application.ActivityLifecycleCallbacks {
                    override fun onActivityResumed(activity: Activity) {
                        try {
                            val n = activity.javaClass.name
                            if (n.contains("carrom", ignoreCase = true) || n.startsWith("com.miniclip.")) {
                                attach(activity)
                            }
                        } catch (t: Throwable) {
                            Log.e(TAG, "attach failed: $t")
                        }
                    }

                    override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {}
                    override fun onActivityStarted(activity: Activity) {}
                    override fun onActivityPaused(activity: Activity) {}
                    override fun onActivityStopped(activity: Activity) {}
                    override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}
                    override fun onActivityDestroyed(activity: Activity) {}
                }
            )
            Log.d(TAG, "CarromMenu registered")
        } catch (t: Throwable) {
            Log.e(TAG, "init failed: $t")
        }
    }

    private fun styleButton(activity: Activity, b: Button) {
        b.isAllCaps = false
        b.minHeight = 0
        b.minimumHeight = 0
        b.textSize = 13f
        b.setPadding(dp(activity, 8), dp(activity, 3), dp(activity, 8), dp(activity, 3))
    }

    private fun attach(activity: Activity) {
        val root = activity.window?.decorView as? ViewGroup ?: return
        if (root.findViewWithTag<View>(OVERLAY_TAG) != null) return
        Log.d(TAG, "attaching to ${activity.javaClass.name}")
        loadPrefs(activity)
        pushToNative()

        val overlay = FrameLayout(activity)
        overlay.tag = OVERLAY_TAG

        // ---- line layer (below the button and the panel, never takes touches)
        val lineView = CarromLineView(activity)
        overlay.addView(lineView, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        // ---- panel: title, ON/OFF button, console box, Copy + Close
        val panel = LinearLayout(activity)
        panel.orientation = LinearLayout.VERTICAL
        val bg = GradientDrawable()
        bg.setColor(Color.argb(240, 24, 26, 33))
        bg.cornerRadius = dp(activity, 12).toFloat()
        bg.setStroke(dp(activity, 1), Color.argb(255, 90, 94, 110))
        panel.background = bg
        panel.setPadding(dp(activity, 10), dp(activity, 10), dp(activity, 10), dp(activity, 10))

        val title = TextView(activity)
        title.text = "Carrom prediction"
        title.setTextColor(Color.WHITE)
        title.textSize = 15f
        panel.addView(title)

        val toggle = Button(activity)
        styleButton(activity, toggle)
        fun paintToggle() {
            toggle.text = if (enabled) "Prediction: ON" else "Prediction: OFF"
            toggle.setTextColor(Color.WHITE)
            val g = GradientDrawable()
            g.cornerRadius = dp(activity, 8).toFloat()
            g.setColor(if (enabled) Color.rgb(30, 140, 70) else Color.rgb(160, 50, 50))
            toggle.background = g
        }
        paintToggle()
        val toggleLp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(activity, 40))
        toggleLp.topMargin = dp(activity, 8)
        panel.addView(toggle, toggleLp)
        toggle.setOnClickListener {
            enabled = !enabled
            paintToggle()
            pushToNative()
            savePrefs(activity)
            if (!enabled) lineView.data = FloatArray(0)
            lineView.invalidate()
        }

        // console box: one status line, then the last shots (newest at the bottom)
        val dm = activity.resources.displayMetrics
        val consoleText = TextView(activity)
        consoleText.setTextColor(Color.argb(255, 170, 235, 180))
        consoleText.typeface = Typeface.MONOSPACE
        consoleText.textSize = 9.5f
        consoleText.setPadding(dp(activity, 6), dp(activity, 4), dp(activity, 6), dp(activity, 4))
        val consoleBg = GradientDrawable()
        consoleBg.setColor(Color.argb(255, 8, 10, 14))
        consoleBg.cornerRadius = dp(activity, 6).toFloat()
        consoleBg.setStroke(dp(activity, 1), Color.argb(255, 60, 64, 78))
        val consoleScroll = ScrollView(activity)
        consoleScroll.background = consoleBg
        consoleScroll.addView(consoleText)
        val consoleLp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, (dm.heightPixels * 0.42f).toInt())
        consoleLp.topMargin = dp(activity, 8)
        panel.addView(consoleScroll, consoleLp)

        var lastConsole = "\u0000"
        var lastStatus = ""
        fun refreshConsole(force: Boolean) {
            val log = readConsole()
            val st = readStatus()
            if (!force && log == lastConsole && st == lastStatus) return
            val grew = log != lastConsole
            lastConsole = log
            lastStatus = st
            consoleText.text = "STATUS: $st\n\n" + (if (log.isEmpty()) "No shot recorded yet. Take a shot with prediction ON." else log)
            if (grew) consoleScroll.post { consoleScroll.fullScroll(View.FOCUS_DOWN) }
        }

        val btnRow = LinearLayout(activity)
        btnRow.orientation = LinearLayout.HORIZONTAL
        val copyBtn = Button(activity)
        copyBtn.text = "Copy"
        styleButton(activity, copyBtn)
        val closeBtn = Button(activity)
        closeBtn.text = "Close"
        styleButton(activity, closeBtn)
        val half1 = LinearLayout.LayoutParams(0, dp(activity, 38), 1f)
        half1.rightMargin = dp(activity, 4)
        val half2 = LinearLayout.LayoutParams(0, dp(activity, 38), 1f)
        half2.leftMargin = dp(activity, 4)
        btnRow.addView(copyBtn, half1)
        btnRow.addView(closeBtn, half2)
        val rowLp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        rowLp.topMargin = dp(activity, 8)
        panel.addView(btnRow, rowLp)

        val panelLp = FrameLayout.LayoutParams(minOf(dp(activity, 330), (dm.widthPixels * 0.94f).toInt()), ViewGroup.LayoutParams.WRAP_CONTENT)
        panelLp.gravity = Gravity.TOP or Gravity.START
        panelLp.leftMargin = dp(activity, 8)
        panelLp.topMargin = dp(activity, 64)
        panel.visibility = View.GONE
        overlay.addView(panel, panelLp)

        copyBtn.setOnClickListener {
            try {
                refreshConsole(true)
                val log = readConsole()
                val text = "STATUS: " + readStatus() + "\n\n" + log
                val cm = activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("carrom shot console", text))
                val shots = if (log.isEmpty()) 0 else log.split("\n\n").size
                Toast.makeText(activity, if (shots == 0) "Copied status only (no shot yet)" else "Copied $shots shot(s) - paste in the chat", Toast.LENGTH_LONG).show()
            } catch (t: Throwable) {
                Log.e(TAG, "copy failed: $t")
                Toast.makeText(activity, "Copy failed: $t", Toast.LENGTH_LONG).show()
            }
        }
        closeBtn.setOnClickListener { panel.visibility = View.GONE }

        // ---- draggable round button
        val size = dp(activity, 44)
        val fab = TextView(activity)
        fab.text = "C"
        fab.gravity = Gravity.CENTER
        fab.setTextColor(Color.WHITE)
        fab.textSize = 18f
        val fabBg = GradientDrawable()
        fabBg.shape = GradientDrawable.OVAL
        fabBg.setColor(Color.argb(220, 200, 90, 40))
        fab.background = fabBg
        val fabLp = FrameLayout.LayoutParams(size, size)
        fabLp.gravity = Gravity.TOP or Gravity.START
        fabLp.leftMargin = dp(activity, 16)
        fabLp.topMargin = dp(activity, 16)
        overlay.addView(fab, fabLp)

        var downX = 0f
        var downY = 0f
        var startLeft = 0
        var startTop = 0
        var dragged = false
        val slop = dp(activity, 8)
        fab.setOnTouchListener { v, ev ->
            val lp = v.layoutParams as FrameLayout.LayoutParams
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downX = ev.rawX
                    downY = ev.rawY
                    startLeft = lp.leftMargin
                    startTop = lp.topMargin
                    dragged = false
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = ev.rawX - downX
                    val dy = ev.rawY - downY
                    if (!dragged && (Math.abs(dx) > slop || Math.abs(dy) > slop)) dragged = true
                    if (dragged) {
                        lp.leftMargin = (startLeft + dx).toInt().coerceAtLeast(0)
                        lp.topMargin = (startTop + dy).toInt().coerceAtLeast(0)
                        v.layoutParams = lp
                    }
                    true
                }
                MotionEvent.ACTION_UP -> {
                    if (!dragged) {
                        if (panel.visibility == View.VISIBLE) {
                            panel.visibility = View.GONE
                        } else {
                            refreshConsole(true)
                            panel.visibility = View.VISIBLE
                        }
                    }
                    true
                }
                else -> false
            }
        }

        // ---- refresh: lines about every 16 ms (redraw only when they changed), console about once a second while the panel is open
        val refresher = object : Runnable {
            var n = 0
            override fun run() {
                if (overlay.parent == null) return
                try {
                    if (n % 120 == 0) lineView.surface = findSurface(root)
                    val arr = if (enabled) NativeCarrom.lines() else FloatArray(0)
                    if (!java.util.Arrays.equals(arr, lineView.data)) {
                        lineView.data = arr
                        lineView.invalidate()
                    }
                    if (panel.visibility == View.VISIBLE && n % 60 == 0) refreshConsole(false)
                } catch (t: Throwable) {
                    if (n % 300 == 0) Log.e(TAG, "refresh failed: $t")
                }
                n++
                ui.postDelayed(this, 16)
            }
        }

        root.addView(
            overlay,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        )
        ui.postDelayed(refresher, 16)
    }
}
