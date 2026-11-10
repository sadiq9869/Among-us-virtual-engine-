package top.niunaijun.blackboxa.menu

import android.app.Activity
import android.app.Application
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.widget.ScrollView
import android.widget.Toast
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView

/**
 * JNI bridge to libmenutest.so (Bcore/src/main/cpp/menutest/menutest.cpp).
 * The C++ functions are named Java_top_niunaijun_blackboxa_menu_NativeMenu_<method>, so this object's
 * package, name and method names must not change.
 */
object NativeMenu {
    external fun requestCompleteTasks(): Int
    external fun statusText(): String
}

/**
 * In-game menu: a small draggable "M" button and a panel, added on top of the Unity activity.
 * Started from BlackBoxLoader.afterApplicationOnCreate for com.innersloth.spacemafia only.
 */
object GameMenu {
    private const val TAG = "GameMenu"
    private const val OVERLAY_TAG = "gamemenu_overlay"

    private var registered = false
    private val ui = Handler(Looper.getMainLooper())

    // ---- console: reads this app's own logcat lines tagged MENUTEST / GameMenu
    private const val MAX_SHOWN = 150
    @Volatile private var clearMarker = ""            // logcat lines older than this are hidden ("Clear log")
    @Volatile private var allLines: List<String> = emptyList()  // everything visible (used by "Copy log")
    private val logBusy = AtomicBoolean(false)

    private fun isLogLine(l: String) = l.length > 19 && l[2] == '-' && l[5] == ' '

    private fun readLogcat(): List<String> =
        try {
            val p = Runtime.getRuntime().exec(arrayOf("logcat", "-d", "-v", "time", "-s", "MENUTEST:V", "GameMenu:V"))
            val raw = p.inputStream.bufferedReader().readLines()
            p.destroy()
            val marker = clearMarker
            raw.filter { isLogLine(it) && it.substring(0, 18) > marker }
        } catch (t: Throwable) {
            listOf("[console] cannot read logcat: $t")
        }

    private fun colorize(lines: List<String>): CharSequence {
        val sb = SpannableStringBuilder()
        for (l in lines) {
            val start = sb.length
            sb.append(l).append('\n')
            val level = if (isLogLine(l)) l[19] else ' '
            val color = when {
                level == 'E' || l.contains("exception") || l.contains("FAULT") || l.contains("error", true) ->
                    Color.argb(255, 255, 110, 110)
                level == 'W' || l.contains("unavailable") || l.contains("skipped") -> Color.argb(255, 255, 200, 90)
                l.contains("completed via") || l.contains("installed") || l.contains("Menu ready") || l.contains("FOUND") ->
                    Color.argb(255, 120, 230, 140)
                else -> Color.argb(255, 205, 210, 225)
            }
            sb.setSpan(ForegroundColorSpan(color), start, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        return sb
    }

    fun init(application: Application?) {
        try {
            if (application == null || registered) return
            registered = true
            application.registerActivityLifecycleCallbacks(
                object : Application.ActivityLifecycleCallbacks {
                    override fun onActivityResumed(activity: Activity) {
                        try {
                            if (activity.javaClass.name.contains("Unity", ignoreCase = true)) {
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
            Log.d(TAG, "GameMenu registered")
        } catch (t: Throwable) {
            Log.e(TAG, "init failed: $t")
        }
    }

    private fun dp(activity: Activity, v: Int): Int =
        (v * activity.resources.displayMetrics.density + 0.5f).toInt()

    private fun readStatus(): String =
        try {
            NativeMenu.statusText()
        } catch (t: Throwable) {
            "Menu native library not loaded ($t)"
        }

    private fun attach(activity: Activity) {
        val root = activity.window?.decorView as? ViewGroup ?: return
        if (root.findViewWithTag<View>(OVERLAY_TAG) != null) return
        Log.d(TAG, "attaching menu to ${activity.javaClass.name}")

        val overlay = FrameLayout(activity)
        overlay.tag = OVERLAY_TAG

        // ---- panel (hidden until the button is tapped)
        val panel = LinearLayout(activity)
        panel.orientation = LinearLayout.VERTICAL
        panel.visibility = View.GONE
        val bg = GradientDrawable()
        bg.setColor(Color.argb(235, 24, 26, 33))
        bg.cornerRadius = dp(activity, 12).toFloat()
        bg.setStroke(dp(activity, 1), Color.argb(255, 90, 94, 110))
        panel.background = bg
        panel.setPadding(dp(activity, 12), dp(activity, 12), dp(activity, 12), dp(activity, 12))

        val title = TextView(activity)
        title.text = "Among Us Menu"
        title.setTextColor(Color.WHITE)
        title.textSize = 16f
        panel.addView(title)

        val status = TextView(activity)
        status.setTextColor(Color.argb(255, 190, 195, 210))
        status.textSize = 12f
        status.text = readStatus()
        val statusLp = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        )
        statusLp.topMargin = dp(activity, 6)
        statusLp.bottomMargin = dp(activity, 8)
        panel.addView(status, statusLp)

        val completeBtn = Button(activity)
        completeBtn.text = "Complete my tasks"
        completeBtn.isAllCaps = false
        completeBtn.setOnClickListener {
            val r = try {
                NativeMenu.requestCompleteTasks()
            } catch (t: Throwable) {
                Log.e(TAG, "requestCompleteTasks failed: $t")
                -1
            }
            Log.d(TAG, "requestCompleteTasks -> $r")
            status.text = readStatus()
        }
        panel.addView(completeBtn)

        val closeBtn = Button(activity)
        closeBtn.text = "Close"
        closeBtn.isAllCaps = false
        panel.addView(closeBtn)

        val consoleBtn = Button(activity)
        consoleBtn.text = "Console (logs)"
        consoleBtn.isAllCaps = false
        panel.addView(consoleBtn)

        val panelLp = FrameLayout.LayoutParams(dp(activity, 270), ViewGroup.LayoutParams.WRAP_CONTENT)
        panelLp.gravity = Gravity.TOP or Gravity.START
        panelLp.leftMargin = dp(activity, 16)
        panelLp.topMargin = dp(activity, 70)
        overlay.addView(panel, panelLp)

        // ---- console box (hidden until "Console (logs)" is tapped)
        val console = LinearLayout(activity)
        console.orientation = LinearLayout.VERTICAL
        console.visibility = View.GONE
        val cbg = GradientDrawable()
        cbg.setColor(Color.argb(240, 12, 14, 18))
        cbg.cornerRadius = dp(activity, 10).toFloat()
        cbg.setStroke(dp(activity, 1), Color.argb(255, 90, 94, 110))
        console.background = cbg
        console.setPadding(dp(activity, 8), dp(activity, 8), dp(activity, 8), dp(activity, 8))

        val consoleTitle = TextView(activity)
        consoleTitle.setTextColor(Color.WHITE)
        consoleTitle.textSize = 13f
        consoleTitle.text = "Console"
        console.addView(consoleTitle)

        val logView = TextView(activity)
        logView.typeface = Typeface.MONOSPACE
        logView.textSize = 9f
        logView.setTextIsSelectable(false)
        val scroll = ScrollView(activity)
        scroll.addView(logView)
        console.addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

        val btnRow = LinearLayout(activity)
        btnRow.orientation = LinearLayout.HORIZONTAL
        val copyBtn = Button(activity)
        copyBtn.text = "Copy log"
        copyBtn.isAllCaps = false
        val clearBtn = Button(activity)
        clearBtn.text = "Clear log"
        clearBtn.isAllCaps = false
        val hideBtn = Button(activity)
        hideBtn.text = "Hide"
        hideBtn.isAllCaps = false
        val rowLp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        btnRow.addView(copyBtn, rowLp)
        btnRow.addView(clearBtn, rowLp)
        btnRow.addView(hideBtn, rowLp)
        console.addView(btnRow)

        val dm = activity.resources.displayMetrics
        val consoleW = Math.min(dp(activity, 460), dm.widthPixels - dp(activity, 310))
        val consoleLp = FrameLayout.LayoutParams(
            Math.max(consoleW, dp(activity, 240)), Math.min(dp(activity, 300), dm.heightPixels - dp(activity, 40))
        )
        consoleLp.gravity = Gravity.TOP or Gravity.START
        consoleLp.leftMargin = dp(activity, 296)
        consoleLp.topMargin = dp(activity, 16)
        overlay.addView(console, consoleLp)

        fun refreshConsole() {
            if (!logBusy.compareAndSet(false, true)) return
            Thread {
                val lines = readLogcat()
                allLines = lines
                val shown = if (lines.size > MAX_SHOWN) lines.takeLast(MAX_SHOWN) else lines
                val text = colorize(shown)
                ui.post {
                    logBusy.set(false)
                    if (console.visibility == View.VISIBLE) {
                        consoleTitle.text = "Console - ${lines.size} lines (showing last ${shown.size})"
                        logView.text = text
                        scroll.post { scroll.fullScroll(View.FOCUS_DOWN) }
                    }
                }
            }.start()
        }

        consoleBtn.setOnClickListener {
            console.visibility = View.VISIBLE
            consoleTitle.text = "Console - loading..."
            refreshConsole()
        }
        hideBtn.setOnClickListener { console.visibility = View.GONE }
        clearBtn.setOnClickListener {
            clearMarker = SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US).format(Date())
            allLines = emptyList()
            logView.text = ""
            consoleTitle.text = "Console - cleared"
        }
        copyBtn.setOnClickListener {
            try {
                val text = allLines.joinToString("\n")
                val cm = activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("menu log", text))
                consoleTitle.text = "Console - copied ${allLines.size} lines"
                Toast.makeText(activity, "Copied ${allLines.size} lines", Toast.LENGTH_SHORT).show()
            } catch (t: Throwable) {
                consoleTitle.text = "Console - copy failed: $t"
            }
        }
        // the console refreshes itself every 1 s while it is visible
        val consoleRefresher = object : Runnable {
            override fun run() {
                if (overlay.parent == null) return
                if (console.visibility == View.VISIBLE) refreshConsole()
                ui.postDelayed(this, 1000)
            }
        }
        ui.postDelayed(consoleRefresher, 1000)

        // ---- draggable round button
        val size = dp(activity, 44)
        val fab = TextView(activity)
        fab.text = "M"
        fab.gravity = Gravity.CENTER
        fab.setTextColor(Color.WHITE)
        fab.textSize = 18f
        val fabBg = GradientDrawable()
        fabBg.shape = GradientDrawable.OVAL
        fabBg.setColor(Color.argb(220, 60, 90, 200))
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
                            status.text = readStatus()
                            panel.visibility = View.VISIBLE
                        }
                    }
                    true
                }
                else -> false
            }
        }
        closeBtn.setOnClickListener { panel.visibility = View.GONE; console.visibility = View.GONE }

        // keep the status line fresh while the panel is open
        val refresher = object : Runnable {
            override fun run() {
                if (overlay.parent == null) return
                if (panel.visibility == View.VISIBLE) status.text = readStatus()
                ui.postDelayed(this, 1000)
            }
        }

        root.addView(
            overlay,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        )
        ui.postDelayed(refresher, 1000)
    }
}
