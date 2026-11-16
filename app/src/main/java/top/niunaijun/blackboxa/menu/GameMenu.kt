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
import java.util.concurrent.atomic.AtomicLong
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
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
    external fun requestPlayers(): Int
    external fun playersText(): String
    external fun setHostOptions(on: Int, roleIdx: Int)
    external fun requestHostInfo(): Int
    external fun hostInfoText(): String
    // v16 visual tasks: which = 1 Submit Scan, 2 Clear Asteroids, 3 Empty Trash, 4 Prime Shields
    external fun requestVisual(which: Int): Int
    external fun visualText(): String
    // v21 lag test: 0 = menu work OFF (soft), 1 = menu work ON, 2 = unhook ALL hooks (until the game is restarted)
    // returns 1 done/accepted, 0 menu not ready, -1 not in hook mode, 2 already unhooked
    external fun lagMode(mode: Int): Int
    // v23 Account: unlock-all tick (native default ON), name, level
    external fun setUnlockAll(on: Int)
    // returns 1 queued, 0 menu not ready, -1 invalid (name empty or > 100 UTF-16 units / level outside 0..4294967295)
    external fun requestSetName(name: String): Int
    external fun requestSetLevel(level: Long): Int
    external fun accountText(): String
    // v24: folder for the saved account file, and the saved values (name "" / level -1 = nothing saved)
    external fun setStorageDir(dir: String)
    external fun savedName(): String
    external fun savedLevel(): Long
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
    private var alwaysImp = false
    // v23 Account state (in memory only)
    private var unlockAll = true            // default ON, always
    private var acctName = ""
    private var acctLevel = ""
    private const val ACCT_MAX_LEVEL = 100L  // UNCONFIRMED: a Steam post says the in-game cap is 100; any other value can be typed

    private fun askText(activity: Activity, title: String, initial: String, numeric: Boolean, onOk: (String) -> Unit) {
        try {
            val et = android.widget.EditText(activity)
            et.setText(initial)
            et.setSelection(et.text.length)
            if (numeric) et.inputType = android.text.InputType.TYPE_CLASS_NUMBER
            android.app.AlertDialog.Builder(activity)
                .setTitle(title)
                .setView(et)
                .setPositiveButton("OK") { _, _ -> onOk(et.text.toString()) }
                .setNegativeButton("Cancel", null)
                .show()
        } catch (t: Throwable) {
            Log.i("GameMenu", "input dialog failed: $t")
            Toast.makeText(activity, "Input dialog failed: $t", Toast.LENGTH_LONG).show()
        }
    }
    private var wantIdx = 0
    private val wantNames = arrayOf("Random", "Impostor", "Shapeshifter", "Phantom", "Viper")
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
            startUiProbe()
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

    // ---- v21 UI-thread probe: every 200 ms a tiny message is posted to the Android main (UI) thread and the delay until it runs
    // is measured. A big delay = the UI thread is busy (touch events for kill/chat/move wait behind it). One log line per 5 s.
    private val uiProbeStarted = AtomicBoolean(false)
    private val uiMax = AtomicLong(0)
    private val uiSum = AtomicLong(0)
    private val uiCnt = AtomicLong(0)
    private val uiSlow = AtomicLong(0)

    private fun startUiProbe() {
        if (!uiProbeStarted.compareAndSet(false, true)) return
        val th = Thread {
            var lastLog = SystemClock.uptimeMillis()
            while (true) {
                try {
                    val t0 = SystemClock.uptimeMillis()
                    ui.post {
                        val d = SystemClock.uptimeMillis() - t0
                        uiCnt.incrementAndGet()
                        uiSum.addAndGet(d)
                        if (d > 100) uiSlow.incrementAndGet()
                        var cur = uiMax.get()
                        while (d > cur && !uiMax.compareAndSet(cur, d)) cur = uiMax.get()
                    }
                    Thread.sleep(200)
                    val now = SystemClock.uptimeMillis()
                    if (now - lastLog >= 5000) {
                        lastLog = now
                        val cnt = uiCnt.getAndSet(0)
                        val sum = uiSum.getAndSet(0)
                        val mx = uiMax.getAndSet(0)
                        val slow = uiSlow.getAndSet(0)
                        Log.i("MENUTEST", "PROBE-UI 5s: uiThreadDelay samples=$cnt avg=${if (cnt > 0) sum / cnt else 0}ms max=${mx}ms over100ms=$slow")
                    }
                } catch (e: Throwable) {
                }
            }
        }
        th.isDaemon = true
        th.name = "menuprobe-ui"
        th.start()
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

        val visualBtn = Button(activity)
        visualBtn.text = "Visual tasks"
        visualBtn.isAllCaps = false
        panel.addView(visualBtn)

        val closeBtn = Button(activity)
        closeBtn.text = "Close"
        closeBtn.isAllCaps = false

        val playersBtn = Button(activity)
        playersBtn.text = "Player info"
        playersBtn.isAllCaps = false
        panel.addView(playersBtn)

        val hostBtn = Button(activity)
        hostBtn.text = "Host settings"
        hostBtn.isAllCaps = false
        panel.addView(hostBtn)

        val consoleBtn = Button(activity)
        consoleBtn.text = "Console (logs)"
        consoleBtn.isAllCaps = false
        panel.addView(consoleBtn)

        val lagBtn = Button(activity)
        lagBtn.text = "Lag test"
        lagBtn.isAllCaps = false
        panel.addView(lagBtn)

        val acctBtn = Button(activity)
        acctBtn.text = "Account"
        acctBtn.isAllCaps = false
        panel.addView(acctBtn)
        panel.addView(closeBtn)

        // compact buttons so the panel (now 6 buttons) fits on a landscape phone screen
        for (b in listOf(completeBtn, visualBtn, playersBtn, hostBtn, consoleBtn, lagBtn, acctBtn, closeBtn)) {
            b.minHeight = 0
            b.minimumHeight = 0
            b.textSize = 13f
            b.setPadding(dp(activity, 8), dp(activity, 3), dp(activity, 8), dp(activity, 3))   // v21: 7 buttons now, so a bit more compact
        }

        val panelLp = FrameLayout.LayoutParams(dp(activity, 270), ViewGroup.LayoutParams.WRAP_CONTENT)
        panelLp.gravity = Gravity.TOP or Gravity.START
        panelLp.leftMargin = dp(activity, 16)
        panelLp.topMargin = dp(activity, 24)
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

        // ---- players box (name, role, alive/dead); refreshed every second while visible
        val players = LinearLayout(activity)
        players.orientation = LinearLayout.VERTICAL
        players.visibility = View.GONE
        players.background = cbg.constantState?.newDrawable()?.mutate() ?: cbg
        players.setPadding(dp(activity, 8), dp(activity, 8), dp(activity, 8), dp(activity, 8))
        val playersTitle = TextView(activity)
        playersTitle.setTextColor(Color.WHITE)
        playersTitle.textSize = 13f
        playersTitle.text = "Player info"
        players.addView(playersTitle)
        val playersView = TextView(activity)
        playersView.typeface = Typeface.MONOSPACE
        playersView.textSize = 11f
        playersView.setTextColor(Color.argb(255, 205, 210, 225))
        val playersScroll = ScrollView(activity)
        playersScroll.addView(playersView)
        players.addView(playersScroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        val pBtnRow = LinearLayout(activity)
        pBtnRow.orientation = LinearLayout.HORIZONTAL
        val pHideBtn = Button(activity)
        pHideBtn.text = "Hide"
        pHideBtn.isAllCaps = false
        pBtnRow.addView(pHideBtn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        players.addView(pBtnRow)
        val playersLp = FrameLayout.LayoutParams(consoleLp.width, consoleLp.height)
        playersLp.gravity = Gravity.TOP or Gravity.START
        playersLp.leftMargin = dp(activity, 296)
        playersLp.topMargin = dp(activity, 16)
        overlay.addView(players, playersLp)

        // ---- host settings box (Always Imposter). Enabled only when the game says you are host.
        val hostBox = LinearLayout(activity)
        hostBox.orientation = LinearLayout.VERTICAL
        hostBox.visibility = View.GONE
        hostBox.background = cbg.constantState?.newDrawable()?.mutate() ?: cbg
        hostBox.setPadding(dp(activity, 8), dp(activity, 8), dp(activity, 8), dp(activity, 8))
        val hostTitle = TextView(activity)
        hostTitle.setTextColor(Color.WHITE)
        hostTitle.textSize = 13f
        hostTitle.text = "Host settings"
        hostBox.addView(hostTitle)
        val hostInfoView = TextView(activity)
        hostInfoView.typeface = Typeface.MONOSPACE
        hostInfoView.textSize = 11f
        hostInfoView.setTextColor(Color.argb(255, 205, 210, 225))
        val hostScroll = ScrollView(activity)
        hostScroll.addView(hostInfoView)
        hostBox.addView(hostScroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        val alwaysBtn = Button(activity)
        alwaysBtn.isAllCaps = false
        val roleBtn = Button(activity)
        roleBtn.isAllCaps = false
        val hostHideBtn = Button(activity)
        hostHideBtn.text = "Hide"
        hostHideBtn.isAllCaps = false
        for (b in listOf(alwaysBtn, roleBtn, hostHideBtn)) {
            b.minHeight = 0
            b.minimumHeight = 0
            b.textSize = 12f
            b.setPadding(dp(activity, 6), dp(activity, 4), dp(activity, 6), dp(activity, 4))
            hostBox.addView(b)
        }
        val hostLp = FrameLayout.LayoutParams(consoleLp.width, consoleLp.height)
        hostLp.gravity = Gravity.TOP or Gravity.START
        hostLp.leftMargin = dp(activity, 296)
        hostLp.topMargin = dp(activity, 16)
        overlay.addView(hostBox, hostLp)

        fun updateHostButtons(isHost: Boolean) {
            alwaysBtn.isEnabled = isHost
            roleBtn.isEnabled = isHost
            alwaysBtn.text = "Always Imposter: " + (if (alwaysImp) "ON" else "OFF")
            roleBtn.text = "Imposter role: " + wantNames[wantIdx]
            hostTitle.text = if (isHost) "Host settings" else "Host settings - host only (you are not host)"
        }
        fun refreshHost() {
            try {
                NativeMenu.requestHostInfo()
                val t = NativeMenu.hostInfoText()
                hostInfoView.text = t
                updateHostButtons(t.contains("Host: YES"))
            } catch (t: Throwable) {
                hostInfoView.text = "Host tools: native library not loaded ($t)"
                updateHostButtons(false)
            }
        }
        fun applyHostOptions() {
            try {
                NativeMenu.setHostOptions(if (alwaysImp) 1 else 0, wantIdx)
            } catch (t: Throwable) {
                hostInfoView.text = "Host tools: native library not loaded ($t)"
            }
        }
        alwaysBtn.setOnClickListener {
            alwaysImp = !alwaysImp
            applyHostOptions()
            updateHostButtons(alwaysBtn.isEnabled)
        }
        roleBtn.setOnClickListener {
            wantIdx = (wantIdx + 1) % wantNames.size
            applyHostOptions()
            updateHostButtons(roleBtn.isEnabled)
        }
        hostHideBtn.setOnClickListener { hostBox.visibility = View.GONE }
        updateHostButtons(false)

        // ---- visual tasks box: Submit Scan / Clear Asteroids / Empty Trash / Prime Shields (works as crew and as impostor)
        val visBox = LinearLayout(activity)
        visBox.orientation = LinearLayout.VERTICAL
        visBox.visibility = View.GONE
        visBox.background = cbg.constantState?.newDrawable()?.mutate() ?: cbg
        visBox.setPadding(dp(activity, 8), dp(activity, 8), dp(activity, 8), dp(activity, 8))
        val visTitle = TextView(activity)
        visTitle.setTextColor(Color.WHITE)
        visTitle.textSize = 13f
        visTitle.text = "Visual tasks (other players see the animation)"
        visBox.addView(visTitle)
        val visInfoView = TextView(activity)
        visInfoView.typeface = Typeface.MONOSPACE
        visInfoView.textSize = 11f
        visInfoView.setTextColor(Color.argb(255, 205, 210, 225))
        val visScroll = ScrollView(activity)
        visScroll.addView(visInfoView)
        visBox.addView(visScroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        val scanBtn = Button(activity)
        scanBtn.text = "Submit Scan"
        val astBtn = Button(activity)
        astBtn.text = "Clear Asteroids"
        val trashBtn = Button(activity)
        trashBtn.text = "Empty Trash"
        val shieldBtn = Button(activity)
        shieldBtn.text = "Prime Shields"
        val visHideBtn = Button(activity)
        visHideBtn.text = "Hide"
        for (b in listOf(scanBtn, astBtn, trashBtn, shieldBtn, visHideBtn)) {
            b.isAllCaps = false
            b.minHeight = 0
            b.minimumHeight = 0
            b.textSize = 12f
            b.setPadding(dp(activity, 6), dp(activity, 4), dp(activity, 6), dp(activity, 4))
        }
        val visRow1 = LinearLayout(activity)
        visRow1.orientation = LinearLayout.HORIZONTAL
        val visRow2 = LinearLayout(activity)
        visRow2.orientation = LinearLayout.HORIZONTAL
        val visBtnLp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        visRow1.addView(scanBtn, visBtnLp)
        visRow1.addView(astBtn, visBtnLp)
        visRow2.addView(trashBtn, visBtnLp)
        visRow2.addView(shieldBtn, visBtnLp)
        visBox.addView(visRow1)
        visBox.addView(visRow2)
        visBox.addView(visHideBtn)
        val visLp = FrameLayout.LayoutParams(consoleLp.width, consoleLp.height)
        visLp.gravity = Gravity.TOP or Gravity.START
        visLp.leftMargin = dp(activity, 296)
        visLp.topMargin = dp(activity, 16)
        overlay.addView(visBox, visLp)

        // ---- v21 lag test box: find out what causes the lag by switching parts off while playing
        val lagBox = LinearLayout(activity)
        lagBox.orientation = LinearLayout.VERTICAL
        lagBox.visibility = View.GONE
        lagBox.background = cbg.constantState?.newDrawable()?.mutate() ?: cbg
        lagBox.setPadding(dp(activity, 8), dp(activity, 8), dp(activity, 8), dp(activity, 8))
        val lagTitle = TextView(activity)
        lagTitle.setTextColor(Color.WHITE)
        lagTitle.textSize = 13f
        lagTitle.text = "Lag test (keep the Console CLOSED while testing)"
        lagBox.addView(lagTitle)
        val lagInfo = TextView(activity)
        lagInfo.typeface = Typeface.MONOSPACE
        lagInfo.textSize = 10f
        lagInfo.setTextColor(Color.argb(255, 205, 210, 225))
        lagInfo.text = "1) Play normally and press 'Lag mark' whenever it lags.\n" +
            "2) 'Hide menu 60 s': menu fully hidden for 60 s, comes back alone.\n" +
            "3) 'Menu work OFF': the hook only calls the game (undo: ON).\n" +
            "4) 'Unhook ALL': every hook removed, game fully original. Menu features stop until you restart the game.\n" +
            "Then Console > Copy log and send it."
        val lagScroll = ScrollView(activity)
        lagScroll.addView(lagInfo)
        lagBox.addView(lagScroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        val lagMarkBtn = Button(activity)
        lagMarkBtn.text = "Lag mark"
        val lagHideBtn = Button(activity)
        lagHideBtn.text = "Hide menu 60 s"
        val lagSoftBtn = Button(activity)
        lagSoftBtn.text = "Menu work OFF"
        val lagUnhookBtn = Button(activity)
        lagUnhookBtn.text = "Unhook ALL"
        val lagCloseBtn = Button(activity)
        lagCloseBtn.text = "Hide"
        for (b in listOf(lagMarkBtn, lagHideBtn, lagSoftBtn, lagUnhookBtn, lagCloseBtn)) {
            b.isAllCaps = false
            b.minHeight = 0
            b.minimumHeight = 0
            b.textSize = 12f
            b.setPadding(dp(activity, 6), dp(activity, 4), dp(activity, 6), dp(activity, 4))
        }
        val lagRow1 = LinearLayout(activity)
        lagRow1.orientation = LinearLayout.HORIZONTAL
        val lagRow2 = LinearLayout(activity)
        lagRow2.orientation = LinearLayout.HORIZONTAL
        val lagBtnLp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        lagRow1.addView(lagMarkBtn, lagBtnLp)
        lagRow1.addView(lagHideBtn, lagBtnLp)
        lagRow2.addView(lagSoftBtn, lagBtnLp)
        lagRow2.addView(lagUnhookBtn, lagBtnLp)
        lagBox.addView(lagRow1)
        lagBox.addView(lagRow2)
        lagBox.addView(lagCloseBtn)
        val lagLp = FrameLayout.LayoutParams(consoleLp.width, consoleLp.height)
        lagLp.gravity = Gravity.TOP or Gravity.START
        lagLp.leftMargin = dp(activity, 296)
        lagLp.topMargin = dp(activity, 16)
        overlay.addView(lagBox, lagLp)

        // ---- v23 Account box: unlock-all tick (default ON), name, level
        val acctBox = LinearLayout(activity)
        acctBox.orientation = LinearLayout.VERTICAL
        acctBox.visibility = View.GONE
        acctBox.background = cbg.constantState?.newDrawable()?.mutate() ?: cbg
        acctBox.setPadding(dp(activity, 8), dp(activity, 8), dp(activity, 8), dp(activity, 8))
        val acctTitle = TextView(activity)
        acctTitle.setTextColor(Color.WHITE)
        acctTitle.textSize = 13f
        acctTitle.text = "Account"
        acctBox.addView(acctTitle)
        val acctInfo = TextView(activity)
        acctInfo.typeface = Typeface.MONOSPACE
        acctInfo.textSize = 10f
        acctInfo.setTextColor(Color.argb(255, 205, 210, 225))
        val acctScroll = ScrollView(activity)
        acctScroll.addView(acctInfo)
        acctBox.addView(acctScroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        val unlockBtn = Button(activity)
        val nameEditBtn = Button(activity)
        val nameApplyBtn = Button(activity)
        val levelEditBtn = Button(activity)
        val levelApplyBtn = Button(activity)
        val levelMaxBtn = Button(activity)
        val acctHideBtn = Button(activity)
        acctHideBtn.text = "Hide"
        for (b in listOf(unlockBtn, nameEditBtn, nameApplyBtn, levelEditBtn, levelApplyBtn, levelMaxBtn, acctHideBtn)) {
            b.isAllCaps = false
            b.minHeight = 0
            b.minimumHeight = 0
            b.textSize = 12f
            b.setPadding(dp(activity, 6), dp(activity, 4), dp(activity, 6), dp(activity, 4))
        }
        acctBox.addView(unlockBtn)
        val nameRow = LinearLayout(activity)
        nameRow.orientation = LinearLayout.HORIZONTAL
        nameRow.addView(nameEditBtn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 2f))
        nameRow.addView(nameApplyBtn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        acctBox.addView(nameRow)
        val levelRow = LinearLayout(activity)
        levelRow.orientation = LinearLayout.HORIZONTAL
        levelRow.addView(levelEditBtn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 2f))
        levelRow.addView(levelApplyBtn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        levelRow.addView(levelMaxBtn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        acctBox.addView(levelRow)
        acctBox.addView(acctHideBtn)
        val acctLp = FrameLayout.LayoutParams(consoleLp.width, consoleLp.height)
        acctLp.gravity = Gravity.TOP or Gravity.START
        acctLp.leftMargin = dp(activity, 296)
        acctLp.topMargin = dp(activity, 16)
        overlay.addView(acctBox, acctLp)

        fun updateAcctButtons() {
            unlockBtn.text = "Unlock all (skins, hats, visors, pets, nameplates, bundles, cubes): " + (if (unlockAll) "ON" else "OFF")
            nameEditBtn.text = "Name: " + (if (acctName.isEmpty()) "(tap to type)" else acctName)
            nameApplyBtn.text = "Select"
            levelEditBtn.text = "Level: " + (if (acctLevel.isEmpty()) "(tap to type)" else acctLevel)
            levelApplyBtn.text = "Select"
            levelMaxBtn.text = "Max ($ACCT_MAX_LEVEL)"
        }
        fun refreshAcct() {
            try {
                acctInfo.text = NativeMenu.accountText()
            } catch (t: Throwable) {
                acctInfo.text = "Account: native library not loaded ($t)"
            }
        }
        fun sendLevel(text: String) {
            val v = text.trim().toLongOrNull()
            if (v == null || v < 0L || v > 4294967295L) {
                acctInfo.text = "Level must be a whole number from 0 to 4294967295."
                return
            }
            try {
                val r = NativeMenu.requestSetLevel(v)
                if (r == 0) acctInfo.text = "Menu not ready yet (wait for 'Menu ready')."
                else if (r < 0) acctInfo.text = "Level rejected by the native side."
            } catch (t: Throwable) {
                acctInfo.text = "Account: native library not loaded ($t)"
            }
            ui.postDelayed({ refreshAcct() }, 500)
        }
        try {
            NativeMenu.setStorageDir(activity.filesDir.absolutePath)
            acctName = NativeMenu.savedName()
            val sl = NativeMenu.savedLevel()
            acctLevel = if (sl >= 0) sl.toString() else ""
        } catch (t: Throwable) { Log.i("GameMenu", "saved account values not available yet: $t") }
        updateAcctButtons()
        try { NativeMenu.setUnlockAll(if (unlockAll) 1 else 0) } catch (t: Throwable) { Log.i("GameMenu", "setUnlockAll not available yet: $t") }
        unlockBtn.setOnClickListener {
            unlockAll = !unlockAll
            try { NativeMenu.setUnlockAll(if (unlockAll) 1 else 0) } catch (t: Throwable) { acctInfo.text = "Account: native library not loaded ($t)" }
            updateAcctButtons()
            ui.postDelayed({ refreshAcct() }, 500)
        }
        nameEditBtn.setOnClickListener {
            askText(activity, "Name (saved; shown to others after you rejoin the lobby)", acctName, false) { acctName = it; updateAcctButtons() }
        }
        nameApplyBtn.setOnClickListener {
            if (acctName.isEmpty()) {
                acctInfo.text = "Type a name first (tap the Name button)."
            } else {
                try {
                    val r = NativeMenu.requestSetName(acctName)
                    if (r == 0) acctInfo.text = "Menu not ready yet (wait for 'Menu ready')."
                    else if (r < 0) acctInfo.text = "Name rejected: it must be 1 to 100 characters."
                } catch (t: Throwable) {
                    acctInfo.text = "Account: native library not loaded ($t)"
                }
                ui.postDelayed({ refreshAcct() }, 500)
            }
        }
        levelEditBtn.setOnClickListener {
            askText(activity, "Level (saved; shown to others after you rejoin the lobby)", acctLevel, true) { acctLevel = it.trim(); updateAcctButtons() }
        }
        levelApplyBtn.setOnClickListener {
            if (acctLevel.isEmpty()) acctInfo.text = "Type a level first (tap the Level button)." else sendLevel(acctLevel)
        }
        levelMaxBtn.setOnClickListener {
            acctLevel = ACCT_MAX_LEVEL.toString()
            updateAcctButtons()
            sendLevel(acctLevel)
        }
        acctHideBtn.setOnClickListener { acctBox.visibility = View.GONE }
        acctBtn.setOnClickListener {
            lagBox.visibility = View.GONE
            console.visibility = View.GONE
            players.visibility = View.GONE
            hostBox.visibility = View.GONE
            visBox.visibility = View.GONE
            acctBox.visibility = View.VISIBLE
            refreshAcct()
        }
        val acctRefresher = object : Runnable {
            override fun run() {
                if (overlay.parent == null) return
                if (acctBox.visibility == View.VISIBLE) refreshAcct()
                ui.postDelayed(this, 1000)
            }
        }
        ui.postDelayed(acctRefresher, 1000)

        var lagMarkCount = 0
        var hideSeq = 0
        var softOff = false
        lagMarkBtn.setOnClickListener {
            lagMarkCount++
            Log.i("MENUTEST", "LAGMARK #$lagMarkCount (the user feels lag right now)")
            lagInfo.text = "Lag mark #$lagMarkCount written to the log."
        }
        lagHideBtn.setOnClickListener {
            hideSeq++
            val mySeq = hideSeq
            Log.i("MENUTEST", "LAGTEST menu HIDDEN for 60 s (overlay GONE)")
            overlay.visibility = View.GONE
            ui.postDelayed({
                if (mySeq == hideSeq) {
                    overlay.visibility = View.VISIBLE
                    Log.i("MENUTEST", "LAGTEST menu visible again")
                }
            }, 60000)
        }
        lagSoftBtn.setOnClickListener {
            val r = try {
                NativeMenu.lagMode(if (softOff) 1 else 0)
            } catch (t: Throwable) {
                -2
            }
            if (r == 1) {
                softOff = !softOff
                lagSoftBtn.text = if (softOff) "Menu work ON" else "Menu work OFF"
                lagInfo.text = if (softOff) "Menu work is OFF: the hook only calls the game. Play and watch the lag." else "Menu work is ON again."
            } else {
                lagInfo.text = "Menu work switch: not available (result $r). 0 = menu not ready yet, -1 = no hook mode, -2 = native library missing."
            }
        }
        lagUnhookBtn.setOnClickListener {
            val r = try {
                NativeMenu.lagMode(2)
            } catch (t: Throwable) {
                -2
            }
            lagInfo.text = when (r) {
                1 -> "Unhook ALL requested: all hooks are removed on the next game frame (see 'LAGTEST' lines in the log). Menu features stop until you restart the game."
                2 -> "Hooks are already removed."
                -1 -> "No hooks were installed (helper-thread mode)."
                0 -> "Menu not ready yet."
                else -> "Native library missing ($r)."
            }
            Log.i("MENUTEST", "LAGTEST unhook ALL pressed -> $r")
        }
        lagCloseBtn.setOnClickListener { lagBox.visibility = View.GONE }
        lagBtn.setOnClickListener {
            acctBox.visibility = View.GONE
            console.visibility = View.GONE
            players.visibility = View.GONE
            hostBox.visibility = View.GONE
            visBox.visibility = View.GONE
            lagBox.visibility = View.VISIBLE
        }

        fun refreshVisual() {
            try {
                visInfoView.text = NativeMenu.visualText()
            } catch (t: Throwable) {
                visInfoView.text = "Visual tasks: native library not loaded ($t)"
            }
        }
        fun pressVisual(which: Int) {
            try {
                val r = NativeMenu.requestVisual(which)
                if (r == 0) visInfoView.text = "Menu not ready yet (wait for 'Menu ready')."
            } catch (t: Throwable) {
                visInfoView.text = "Visual tasks: native library not loaded ($t)"
            }
            ui.postDelayed({ refreshVisual() }, 400)
        }
        scanBtn.setOnClickListener { pressVisual(1) }
        astBtn.setOnClickListener { pressVisual(2) }
        trashBtn.setOnClickListener { pressVisual(3) }
        shieldBtn.setOnClickListener { pressVisual(4) }
        visHideBtn.setOnClickListener { visBox.visibility = View.GONE }
        visualBtn.setOnClickListener {
            lagBox.visibility = View.GONE
            acctBox.visibility = View.GONE
            console.visibility = View.GONE
            players.visibility = View.GONE
            hostBox.visibility = View.GONE
            visBox.visibility = View.VISIBLE
            refreshVisual()
        }
        val visRefresher = object : Runnable {
            override fun run() {
                if (overlay.parent == null) return
                if (visBox.visibility == View.VISIBLE) refreshVisual()
                ui.postDelayed(this, 1000)
            }
        }
        ui.postDelayed(visRefresher, 1000)

        fun refreshPlayers() {
            try {
                NativeMenu.requestPlayers()
                playersView.text = NativeMenu.playersText()
            } catch (t: Throwable) {
                playersView.text = "Player info: native library not loaded ($t)"
            }
        }
        playersBtn.setOnClickListener {
            lagBox.visibility = View.GONE
            acctBox.visibility = View.GONE
            console.visibility = View.GONE
            hostBox.visibility = View.GONE
            visBox.visibility = View.GONE
            players.visibility = View.VISIBLE
            refreshPlayers()
        }
        consoleBtn.setOnClickListener {
            lagBox.visibility = View.GONE
            acctBox.visibility = View.GONE
            players.visibility = View.GONE
            hostBox.visibility = View.GONE
            visBox.visibility = View.GONE
            console.visibility = View.VISIBLE
            consoleTitle.text = "Console - loading..."
            refreshConsole()
        }
        pHideBtn.setOnClickListener { players.visibility = View.GONE }
        val playersRefresher = object : Runnable {
            override fun run() {
                if (overlay.parent == null) return
                if (players.visibility == View.VISIBLE) refreshPlayers()
                ui.postDelayed(this, 1000)
            }
        }
        ui.postDelayed(playersRefresher, 1000)

        hostBtn.setOnClickListener {
            lagBox.visibility = View.GONE
            acctBox.visibility = View.GONE
            console.visibility = View.GONE
            players.visibility = View.GONE
            visBox.visibility = View.GONE
            hostBox.visibility = View.VISIBLE
            refreshHost()
        }
        val hostRefresher = object : Runnable {
            override fun run() {
                if (overlay.parent == null) return
                if (hostBox.visibility == View.VISIBLE) refreshHost()
                ui.postDelayed(this, 1000)
            }
        }
        ui.postDelayed(hostRefresher, 1000)

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
        closeBtn.setOnClickListener { panel.visibility = View.GONE; console.visibility = View.GONE; players.visibility = View.GONE; hostBox.visibility = View.GONE; visBox.visibility = View.GONE; lagBox.visibility = View.GONE; acctBox.visibility = View.GONE }

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
