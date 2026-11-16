package top.niunaijun.blackboxa.util

import android.graphics.drawable.Drawable
import android.util.Log
import androidx.core.content.pm.PackageInfoCompat
import top.niunaijun.blackbox.BlackBoxCore
import top.niunaijun.blackboxa.app.AppManager
import top.niunaijun.blackboxa.bean.AppInfo

/**
 * HOSTSYNC: pinned Among Us icon + check on the REAL phone + install/update into the container.
 * Not built here.
 */
object HostGameSync {
    const val PKG = "com.innersloth.spacemafia"
    private const val TAG = "HOSTSYNC"

    enum class State { NOT_ON_HOST, READY, INSTALLED, UPDATED, FAILED }

    class Outcome(val state: State, val msg: String = "")

    private fun hostInfo() = try {
        BlackBoxCore.getPackageManager().getPackageInfo(PKG, 0)
    } catch (e: Exception) {
        null
    }

    private fun hostIcon(): Drawable? = try {
        BlackBoxCore.getPackageManager().getApplicationIcon(PKG)
    } catch (e: Exception) {
        null
    }

    /** Adds a pinned Among Us item when the container does not have it yet. */
    fun withPinned(list: List<AppInfo>): List<AppInfo> {
        if (list.any { it.packageName == PKG }) return list
        return list + AppInfo("Among Us", hostIcon(), PKG, "", false)
    }

    /** Blocking. Call on a background thread. */
    fun prepare(userId: Int): Outcome {
        try {
            val info = hostInfo()
            if (info == null) {
                Log.d(TAG, "HOSTSYNC: $PKG not on host")
                return Outcome(State.NOT_ON_HOST, "Among Us not installed")
            }
            val core = BlackBoxCore.get()
            val sig = "${PackageInfoCompat.getLongVersionCode(info)}|${info.applicationInfo?.sourceDir ?: ""}"
            val prefs = AppManager.mRemarkSharedPreferences
            val key = "HostSync_$userId"
            val installed = core.isInstalled(PKG, userId)
            val saved = prefs.getString(key, null)

            if (installed && saved == null) {
                // Container copy was installed before this feature: do not touch a working setup.
                prefs.edit().putString(key, sig).apply()
                Log.d(TAG, "HOSTSYNC: adopted existing container install, sig=$sig")
                return Outcome(State.READY)
            }
            if (installed && saved == sig) {
                Log.d(TAG, "HOSTSYNC: up to date, sig=$sig")
                return Outcome(State.READY)
            }

            Log.d(TAG, "HOSTSYNC: installing from host, installed=$installed saved=$saved new=$sig")
            val result = core.installPackageAsUser(PKG, userId)
            if (!result.success) {
                Log.e(TAG, "HOSTSYNC: install failed: ${result.msg}")
                return Outcome(State.FAILED, "Among Us install failed: ${result.msg}")
            }
            prefs.edit().putString(key, sig).apply()
            return if (installed) Outcome(State.UPDATED, "Among Us updated")
            else Outcome(State.INSTALLED, "Among Us installed")
        } catch (e: Throwable) {
            Log.e(TAG, "HOSTSYNC: error ${e.message}")
            return Outcome(State.FAILED, "Among Us check failed: ${e.message}")
        }
    }
}
