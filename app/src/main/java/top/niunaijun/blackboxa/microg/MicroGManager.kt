package top.niunaijun.blackboxa.microg

import android.content.Context
import android.net.ConnectivityManager
import android.util.Log
import androidx.core.content.pm.PackageInfoCompat
import androidx.lifecycle.MutableLiveData
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import top.niunaijun.blackbox.BlackBoxCore
import top.niunaijun.blackboxa.app.App

/**
 * MICROG: downloads the microG gms and vending APKs in the background, checks size and
 * SHA-256, and installs them into one container user (gms first, then vending). Not built here.
 *
 * Network and disk work run on two single-thread executors; state is published through
 * LiveData (postValue), so the screen can observe from the main thread.
 */
object MicroGManager {
    private const val TAG = "MICROG"
    private const val PREFS = "MicroG"
    private const val KEY_VERIFIED_PREFIX = "verified_"
    private const val KEY_INSTALLED_PREFIX = "installed_"

    sealed class DownloadState {
        data class NotConfigured(val problem: String) : DownloadState()
        object Idle : DownloadState()
        object WaitingForWifi : DownloadState()
        /** total is -1 when unknown. Counts all files together. */
        data class Downloading(val done: Long, val total: Long) : DownloadState()
        object Verifying : DownloadState()
        object Ready : DownloadState()
        data class Failed(val message: String) : DownloadState()
    }

    class InstallOutcome(val userId: Int, val success: Boolean, val message: String)

    /** present: gms exists in that container user. byUs: this screen installed it. */
    class GmsStatus(val userId: Int, val present: Boolean, val byUs: Boolean)

    val downloadState = MutableLiveData<DownloadState>(DownloadState.Idle)
    val installing = MutableLiveData(false)
    val installOutcome = MutableLiveData<InstallOutcome?>()
    val gmsStatus = MutableLiveData<GmsStatus?>()

    private val downloadBusy = AtomicBoolean(false)
    private val installBusy = AtomicBoolean(false)
    private val downloadExec = Executors.newSingleThreadExecutor { r -> Thread(r, "microg-download") }
    private val installExec = Executors.newSingleThreadExecutor { r -> Thread(r, "microg-install") }

    private fun ctx(): Context = App.getContext()

    private fun prefs() = ctx().getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun dir() = File(ctx().filesDir, "microg").apply { mkdirs() }

    private fun apkFile(p: MicroGConfig.Pkg) = File(dir(), p.fileName)

    private fun partFile(p: MicroGConfig.Pkg) = File(dir(), p.fileName + ".part")

    private fun isMetered(): Boolean = try {
        (ctx().getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager)
            .isActiveNetworkMetered
    } catch (t: Throwable) {
        true // when unsure, do not spend mobile data without the user asking
    }

    /**
     * Starts (or resumes) the background download. Safe to call many times.
     * allowMetered = true is used when the user taps "Download on mobile data".
     * userInitiated = true is used by the Retry and mobile-data buttons. After a failure the
     * automatic calls (app start, opening the screen) do nothing, so a wrong checksum cannot
     * make the app re-download over and over.
     */
    fun startDownload(allowMetered: Boolean, userInitiated: Boolean = false) {
        val problem = MicroGConfig.problem()
        if (problem != null) {
            downloadState.postValue(DownloadState.NotConfigured(problem))
            return
        }
        if (!userInitiated && downloadState.value is DownloadState.Failed) return
        if (!downloadBusy.compareAndSet(false, true)) return
        downloadExec.execute {
            try {
                runDownload(allowMetered)
            } catch (t: Throwable) {
                Log.e(TAG, "MICROG: download failed: ${t.message}", t)
                downloadState.postValue(
                    DownloadState.Failed("Download failed: ${t.message}. Tap Retry to resume.")
                )
            } finally {
                downloadBusy.set(false)
            }
        }
    }

    private fun runDownload(allowMetered: Boolean) {
        val pkgs = MicroGConfig.ALL

        // 1. Which files are already saved and verified?
        val missing = pkgs.filter { !hasVerifiedFile(it) }
        if (missing.isEmpty()) {
            Log.d(TAG, "MICROG: all files already verified")
            downloadState.postValue(DownloadState.Ready)
            return
        }

        if (!allowMetered && isMetered()) {
            Log.d(TAG, "MICROG: metered network, waiting for Wi-Fi or user")
            downloadState.postValue(DownloadState.WaitingForWifi)
            return
        }

        // 2. Download what is missing, one file after the other.
        val total = if (pkgs.all { it.sizeBytes != null }) pkgs.sumOf { it.sizeBytes!! } else -1L
        var finishedBytes = pkgs.filter { it !in missing }.sumOf { apkFile(it).length() }
        downloadState.postValue(DownloadState.Downloading(finishedBytes, total))

        for (p in missing) {
            val apk = apkFile(p)
            val part = partFile(p)
            val base = finishedBytes
            MicroGDownloader.download(p.url!!, part) { done, _ ->
                downloadState.postValue(DownloadState.Downloading(base + done, total))
            }

            downloadState.postValue(DownloadState.Verifying)
            if (!sizeMatches(p, part)) {
                part.delete()
                downloadState.postValue(
                    DownloadState.Failed("${p.label}: file size is wrong. Tap Retry to download again.")
                )
                return
            }
            val actual = MicroGDownloader.sha256Hex(part)
            if (actual != p.sha256) {
                Log.e(TAG, "MICROG: ${p.label} SHA-256 mismatch, got $actual")
                part.delete()
                downloadState.postValue(
                    DownloadState.Failed("${p.label}: downloaded file does not match the expected SHA-256. Tap Retry.")
                )
                return
            }
            if (!part.renameTo(apk)) {
                downloadState.postValue(DownloadState.Failed("${p.label}: could not save the downloaded file."))
                return
            }
            rememberVerified(p)
            finishedBytes += apk.length()
            Log.d(TAG, "MICROG: ${p.label} verified, ${apk.length()} bytes")
        }
        downloadState.postValue(DownloadState.Ready)
    }

    /** True when the saved file exists and matches size and SHA-256 (re-hashed if not cached). */
    private fun hasVerifiedFile(p: MicroGConfig.Pkg): Boolean {
        val apk = apkFile(p)
        if (!apk.exists()) return false
        if (isCachedVerified(p, apk)) return true
        downloadState.postValue(DownloadState.Verifying)
        if (sizeMatches(p, apk) && MicroGDownloader.sha256Hex(apk) == p.sha256) {
            rememberVerified(p)
            return true
        }
        Log.w(TAG, "MICROG: saved ${p.label} failed the check, deleting it")
        apk.delete()
        clearVerified(p)
        return false
    }

    private fun sizeMatches(p: MicroGConfig.Pkg, f: File): Boolean {
        val expected = p.sizeBytes ?: return true
        return f.length() == expected
    }

    private fun verifiedKey(p: MicroGConfig.Pkg, f: File) = "${p.sha256}|${f.length()}|${f.lastModified()}"

    private fun isCachedVerified(p: MicroGConfig.Pkg, f: File) =
        prefs().getString(KEY_VERIFIED_PREFIX + p.packageName, null) == verifiedKey(p, f) && sizeMatches(p, f)

    private fun rememberVerified(p: MicroGConfig.Pkg) {
        prefs().edit().putString(KEY_VERIFIED_PREFIX + p.packageName, verifiedKey(p, apkFile(p))).apply()
    }

    private fun clearVerified(p: MicroGConfig.Pkg) {
        prefs().edit().remove(KEY_VERIFIED_PREFIX + p.packageName).apply()
    }

    private fun readGmsStatus(userId: Int): GmsStatus {
        val present = BlackBoxCore.get().isInstalled(MicroGConfig.GMS.packageName, userId)
        val byUs = present && prefs().getString(KEY_INSTALLED_PREFIX + userId, null) != null
        return GmsStatus(userId, present, byUs)
    }

    /** Looks up, in the background, whether gms exists in this container user. */
    fun refreshGmsStatus(userId: Int) {
        installExec.execute {
            try {
                gmsStatus.postValue(readGmsStatus(userId))
            } catch (t: Throwable) {
                Log.e(TAG, "MICROG: status check failed: ${t.message}", t)
            }
        }
    }

    /** Installs the verified APKs into the given container user. Does nothing while busy. */
    fun install(userId: Int) {
        if (!installBusy.compareAndSet(false, true)) return
        installing.postValue(true)
        installExec.execute {
            val outcome = try {
                doInstall(userId)
            } catch (t: Throwable) {
                Log.e(TAG, "MICROG: install error: ${t.message}", t)
                InstallOutcome(userId, false, "Install error: ${t.message}")
            }
            installOutcome.postValue(outcome)
            try {
                gmsStatus.postValue(readGmsStatus(userId))
            } catch (t: Throwable) {
                Log.e(TAG, "MICROG: status check failed: ${t.message}", t)
            }
            installing.postValue(false)
            installBusy.set(false)
        }
    }

    private fun doInstall(userId: Int): InstallOutcome {
        val core = BlackBoxCore.get()

        // 1. Re-check every file right before installing: they sit in app storage between runs.
        for (p in MicroGConfig.ALL) {
            val apk = apkFile(p)
            if (!apk.exists()) {
                return InstallOutcome(userId, false, "${p.label} file is not downloaded yet.")
            }
            if (MicroGDownloader.sha256Hex(apk) != p.sha256 || !sizeMatches(p, apk)) {
                apk.delete()
                clearVerified(p)
                downloadState.postValue(
                    DownloadState.Failed("${p.label}: saved file failed the check. Tap Retry to download again.")
                )
                return InstallOutcome(userId, false, "${p.label}: saved file failed the SHA-256 check and was deleted.")
            }
            val info = ctx().packageManager.getPackageArchiveInfo(apk.absolutePath, 0)
                ?: return InstallOutcome(userId, false, "Android could not read the ${p.label} APK.")
            if (info.packageName != p.packageName) {
                return InstallOutcome(userId, false, "${p.label}: wrong package in file: ${info.packageName}")
            }
            val code = PackageInfoCompat.getLongVersionCode(info)
            if (code != p.expectedVersionCode) {
                Log.w(TAG, "MICROG: ${p.label} version code $code differs from file name ${p.expectedVersionCode}")
            }
        }

        // 2. gms must not exist yet: never overwrite real Google services or an older microG.
        if (core.isInstalled(MicroGConfig.GMS.packageName, userId)) {
            return InstallOutcome(
                userId, false,
                "Google services are already installed in User $userId. Nothing was changed."
            )
        }

        // 3. gms first.
        val gmsError = installOne(core, MicroGConfig.GMS, userId)
        if (gmsError != null) return InstallOutcome(userId, false, gmsError)
        prefs().edit().putString(KEY_INSTALLED_PREFIX + userId, MicroGConfig.GMS.sha256).apply()

        // 4. Then vending, unless the user already has a Play Store package there.
        val vend = MicroGConfig.VENDING
        val vendingNote = if (core.isInstalled(vend.packageName, userId)) {
            "Play Store (vending) was already in User $userId and was not changed."
        } else {
            val err = installOne(core, vend, userId)
            if (err == null) "Play Store helper (vending) installed too." else "microG works, but vending failed: $err"
        }
        Log.d(TAG, "MICROG: installed into user $userId. $vendingNote")
        return InstallOutcome(userId, true, "microG installed in User $userId. $vendingNote")
    }

    /** Returns null on success, or a short error text. */
    private fun installOne(core: BlackBoxCore, p: MicroGConfig.Pkg, userId: Int): String? {
        Log.d(TAG, "MICROG: installing ${p.label} into user $userId")
        val result = core.installPackageAsUser(apkFile(p), userId)
        // BPackageManagerService returns success=true even when its install code threw,
        // so the only reliable check is whether the package now exists in the container.
        val nowInstalled = core.isInstalled(p.packageName, userId)
        if (!result.success || !nowInstalled) {
            val why = result.msg ?: "${p.label} is not present in the container after install"
            Log.e(TAG, "MICROG: ${p.label} install failed: success=${result.success} installed=$nowInstalled msg=${result.msg}")
            return "${p.label} install failed: $why"
        }
        return null
    }

    fun consumeInstallOutcome() {
        installOutcome.value = null
    }
}
