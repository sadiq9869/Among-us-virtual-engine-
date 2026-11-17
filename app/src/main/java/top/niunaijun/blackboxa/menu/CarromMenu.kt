package top.niunaijun.blackboxa.menu

import android.app.Activity
import android.app.Application
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
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
import android.widget.CheckBox
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView

/**
 * JNI bridge to libcarprobe.so (Bcore/src/main/cpp/carprobe/carprobe.cpp, v5).
 * The C++ functions are named Java_top_niunaijun_blackboxa_menu_NativeCarrom_<method>, so this object's
 * package, name and method names must not change. (Keep rule in app/proguard-rules.pro.)
 */
object NativeCarrom {
    external fun setEnabled(on: Int)
    // frictionPercent 50..150, maxBounces 0..5, deflection 0/1, recon 0/1 (shot-recording logs for tuning)
    external fun setOptions(frictionPercent: Int, maxBounces: Int, deflection: Int, recon: Int)
    // [designW, designH, nPaths, {type, nPoints, x, y, ...}...] in design points, y up. type 0 striker, 1 coin, 2 striker remainder.
    external fun lines(): FloatArray
    external fun statusText(): String
}

/**
 * Draws the prediction lines. It never consumes touches: the player aims through it.
 * Pixel mapping: the box of the game's SurfaceView (or this view if none is found) is the design area,
 *   px = left + x / designW * w ,  py = top + (designH - y) / designH * h
 * then the user calibration (offset in px, scale in percent around the centre of that box) is applied.
 */
class CarromLineView(context: Context) : View(context) {
    @Volatile var data: FloatArray = FloatArray(0)
    var calX = 0f
    var calY = 0f
    var calScalePct = 100
    var surface: View? = null

    private val density = context.resources.displayMetrics.density
    private val shadow = Paint(Paint.ANTI_ALIAS_FLAG)
    private val white = Paint(Paint.ANTI_ALIAS_FLAG)
    private val red = Paint(Paint.ANTI_ALIAS_FLAG)
    private val thin = Paint(Paint.ANTI_ALIAS_FLAG)
    private val path = Path()

    init {
        isClickable = false
        isFocusable = false
        isFocusableInTouchMode = false
        setWillNotDraw(false)
        for (p in listOf(shadow, white, red, thin)) {
            p.style = Paint.Style.STROKE
            p.strokeJoin = Paint.Join.ROUND
            p.strokeCap = Paint.Cap.ROUND
        }
        shadow.color = Color.argb(110, 0, 0, 0)
        shadow.strokeWidth = 7f * density
        white.color = Color.WHITE
        white.strokeWidth = 4f * density
        red.color = Color.rgb(235, 40, 40)
        red.strokeWidth = 4f * density
        thin.color = Color.argb(215, 255, 255, 255)
        thin.strokeWidth = 2.5f * density
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
            val a = IntArray(2)
            val b = IntArray(2)
            s.getLocationOnScreen(a)
            getLocationOnScreen(b)
            left = (a[0] - b[0]).toFloat()
            top = (a[1] - b[1]).toFloat()
            w = s.width.toFloat()
            h = s.height.toFloat()
        }
        val cs = calScalePct / 100f
        val cx = left + w / 2f
        val cy = top + h / 2f

        val nPaths = d[2].toInt()
        var i = 3
        for (k in 0 until nPaths) {
            if (i + 2 > d.size) break
            val type = d[i].toInt()
            val n = d[i + 1].toInt()
            i += 2
            if (n < 2 || i + 2 * n > d.size) break
            path.reset()
            for (j in 0 until n) {
                val x = d[i + 2 * j]
                val y = d[i + 2 * j + 1]
                var px = left + x / designW * w
                var py = top + (designH - y) / designH * h
                px = cx + (px - cx) * cs + calX
                py = cy + (py - cy) * cs + calY
                if (j == 0) path.moveTo(px, py) else path.lineTo(px, py)
            }
            i += 2 * n
            canvas.drawPath(path, shadow)
            canvas.drawPath(path, when (type) { 1 -> red; 2 -> thin; else -> white })
        }
    }
}

/**
 * In-game menu for Carrom Pool (com.miniclip.carrom): a small draggable "C" button, a panel with the
 * prediction on/off button, and the full-screen line overlay. Started from BlackBoxLoader.afterApplicationOnCreate.
 */
object CarromMenu {
    private const val TAG = "CarromMenu"
    private const val OVERLAY_TAG = "carrommenu_overlay"
    private const val PREFS = "carrom_menu"

    private var registered = false
    private val ui = Handler(Looper.getMainLooper())

    // settings (saved in SharedPreferences, best effort)
    private var enabled = true
    private var frictionPct = 100
    private var maxBounces = 3
    private var deflection = true
    private var recon = true
    private var calX = 0
    private var calY = 0
    private var calScale = 100

    private fun dp(activity: Activity, v: Int): Int =
        (v * activity.resources.displayMetrics.density + 0.5f).toInt()

    private fun loadPrefs(activity: Activity) {
        try {
            val p = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            enabled = p.getBoolean("enabled", true)
            frictionPct = p.getInt("friction", 100).coerceIn(50, 150)
            maxBounces = p.getInt("bounces", 3).coerceIn(0, 5)
            deflection = p.getBoolean("deflection", true)
            recon = p.getBoolean("recon", true)
            calX = p.getInt("calX", 0).coerceIn(-600, 600)
            calY = p.getInt("calY", 0).coerceIn(-600, 600)
            calScale = p.getInt("calScale", 100).coerceIn(50, 150)
        } catch (t: Throwable) {
            Log.w(TAG, "prefs load failed: $t")
        }
    }

    private fun savePrefs(activity: Activity) {
        try {
            activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putBoolean("enabled", enabled)
                .putInt("friction", frictionPct)
                .putInt("bounces", maxBounces)
                .putBoolean("deflection", deflection)
                .putBoolean("recon", recon)
                .putInt("calX", calX)
                .putInt("calY", calY)
                .putInt("calScale", calScale)
                .apply()
        } catch (t: Throwable) {
            Log.w(TAG, "prefs save failed: $t")
        }
    }

    private fun pushToNative() {
        try {
            NativeCarrom.setEnabled(if (enabled) 1 else 0)
            NativeCarrom.setOptions(frictionPct, maxBounces, if (deflection) 1 else 0, if (recon) 1 else 0)
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
        lineView.calX = calX.toFloat()
        lineView.calY = calY.toFloat()
        lineView.calScalePct = calScale
        overlay.addView(lineView, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        // ---- panel
        val panel = LinearLayout(activity)
        panel.orientation = LinearLayout.VERTICAL
        val bg = GradientDrawable()
        bg.setColor(Color.argb(235, 24, 26, 33))
        bg.cornerRadius = dp(activity, 12).toFloat()
        bg.setStroke(dp(activity, 1), Color.argb(255, 90, 94, 110))
        panel.background = bg
        panel.setPadding(dp(activity, 12), dp(activity, 12), dp(activity, 12), dp(activity, 12))

        val title = TextView(activity)
        title.text = "Carrom Menu - prediction line"
        title.setTextColor(Color.WHITE)
        title.textSize = 16f
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

        val status = TextView(activity)
        status.setTextColor(Color.argb(255, 190, 195, 210))
        status.textSize = 11f
        status.text = readStatus()
        val statusLp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        statusLp.topMargin = dp(activity, 6)
        statusLp.bottomMargin = dp(activity, 6)
        panel.addView(status, statusLp)

        fun label(text: String): TextView {
            val t = TextView(activity)
            t.text = text
            t.setTextColor(Color.WHITE)
            t.textSize = 12f
            return t
        }

        // friction slider 50..150 %
        val fricLabel = label("Friction: $frictionPct %  (lower = lines run longer)")
        panel.addView(fricLabel)
        val fricBar = SeekBar(activity)
        fricBar.max = 100
        fricBar.progress = frictionPct - 50
        panel.addView(fricBar)
        fricBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                frictionPct = 50 + p
                fricLabel.text = "Friction: $frictionPct %  (lower = lines run longer)"
                if (fromUser) pushToNative()
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) { pushToNative(); savePrefs(activity) }
        })

        // bounces slider 0..5
        val bounceLabel = label("Wall bounces drawn: $maxBounces")
        panel.addView(bounceLabel)
        val bounceBar = SeekBar(activity)
        bounceBar.max = 5
        bounceBar.progress = maxBounces
        panel.addView(bounceBar)
        bounceBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                maxBounces = p
                bounceLabel.text = "Wall bounces drawn: $maxBounces"
                if (fromUser) pushToNative()
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) { pushToNative(); savePrefs(activity) }
        })

        val defl = CheckBox(activity)
        defl.text = "Short striker line after the hit"
        defl.setTextColor(Color.WHITE)
        defl.textSize = 12f
        defl.isChecked = deflection
        defl.setOnCheckedChangeListener { _, c -> deflection = c; pushToNative(); savePrefs(activity) }
        panel.addView(defl)

        val rec = CheckBox(activity)
        rec.text = "Log shots to logcat (for tuning)"
        rec.setTextColor(Color.WHITE)
        rec.textSize = 12f
        rec.isChecked = recon
        rec.setOnCheckedChangeListener { _, c -> recon = c; pushToNative(); savePrefs(activity) }
        panel.addView(rec)

        // calibration rows
        val calLabel = label("Line position calibration (move the line until it starts exactly at the striker)")
        calLabel.textSize = 11f
        val calLp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        calLp.topMargin = dp(activity, 6)
        panel.addView(calLabel, calLp)

        fun applyCal() {
            lineView.calX = calX.toFloat()
            lineView.calY = calY.toFloat()
            lineView.calScalePct = calScale
            lineView.invalidate()
            savePrefs(activity)
        }

        fun calRow(name: String, getter: () -> Int, step: Int, change: (Int) -> Unit, unit: String) {
            val row = LinearLayout(activity)
            row.orientation = LinearLayout.HORIZONTAL
            row.gravity = Gravity.CENTER_VERTICAL
            val t = label("$name ${getter()}$unit")
            row.addView(t, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            val minus = Button(activity)
            minus.text = "-"
            styleButton(activity, minus)
            val plus = Button(activity)
            plus.text = "+"
            styleButton(activity, plus)
            minus.setOnClickListener { change(-step); t.text = "$name ${getter()}$unit"; applyCal() }
            plus.setOnClickListener { change(step); t.text = "$name ${getter()}$unit"; applyCal() }
            row.addView(minus, LinearLayout.LayoutParams(dp(activity, 48), dp(activity, 34)))
            row.addView(plus, LinearLayout.LayoutParams(dp(activity, 48), dp(activity, 34)))
            panel.addView(row)
        }
        calRow("X offset", { calX }, 2, { calX = (calX + it).coerceIn(-600, 600) }, " px")
        calRow("Y offset", { calY }, 2, { calY = (calY + it).coerceIn(-600, 600) }, " px")
        calRow("Scale", { calScale }, 1, { calScale = (calScale + it).coerceIn(50, 150) }, " %")

        val closeBtn = Button(activity)
        closeBtn.text = "Close"
        styleButton(activity, closeBtn)
        val closeLp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        closeLp.topMargin = dp(activity, 8)
        panel.addView(closeBtn, closeLp)

        val scroll = ScrollView(activity)
        scroll.visibility = View.GONE
        scroll.addView(panel)
        val dm = activity.resources.displayMetrics
        val scrollLp = FrameLayout.LayoutParams(dp(activity, 290), (dm.heightPixels * 0.78f).toInt())
        scrollLp.gravity = Gravity.TOP or Gravity.START
        scrollLp.leftMargin = dp(activity, 16)
        scrollLp.topMargin = dp(activity, 64)
        overlay.addView(scroll, scrollLp)

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
                        if (scroll.visibility == View.VISIBLE) {
                            scroll.visibility = View.GONE
                        } else {
                            status.text = readStatus()
                            scroll.visibility = View.VISIBLE
                        }
                    }
                    true
                }
                else -> false
            }
        }
        closeBtn.setOnClickListener { scroll.visibility = View.GONE }

        // ---- refresh: lines about every 33 ms, status text once a second while the panel is open, surface lookup every 2 s
        val refresher = object : Runnable {
            var n = 0
            override fun run() {
                if (overlay.parent == null) return
                try {
                    if (n % 60 == 0) lineView.surface = findSurface(root)
                    val arr = if (enabled) NativeCarrom.lines() else FloatArray(0)
                    val had = lineView.data.isNotEmpty()
                    lineView.data = arr
                    if (arr.isNotEmpty() || had) lineView.invalidate()
                    if (scroll.visibility == View.VISIBLE && n % 30 == 0) status.text = readStatus()
                } catch (t: Throwable) {
                    if (n % 300 == 0) Log.e(TAG, "refresh failed: $t")
                }
                n++
                ui.postDelayed(this, 33)
            }
        }

        root.addView(
            overlay,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        )
        ui.postDelayed(refresher, 33)
    }
}
