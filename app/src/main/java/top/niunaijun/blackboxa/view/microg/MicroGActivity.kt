package top.niunaijun.blackboxa.view.microg

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import com.afollestad.materialdialogs.MaterialDialog
import java.util.Locale
import top.niunaijun.blackboxa.R
import top.niunaijun.blackboxa.databinding.ActivityMicrogBinding
import top.niunaijun.blackboxa.microg.MicroGManager
import top.niunaijun.blackboxa.microg.MicroGManager.DownloadState
import top.niunaijun.blackboxa.util.inflate
import top.niunaijun.blackboxa.util.toast
import top.niunaijun.blackboxa.view.base.BaseActivity

/** MICROG: status card and Install button for the microG gms APK. Not built here. */
class MicroGActivity : BaseActivity() {

    private val viewBinding: ActivityMicrogBinding by inflate()

    private var userId = 0

    /** What the primary button does right now. */
    private enum class PrimaryAction { NONE, INSTALL, RETRY }

    private var primaryAction = PrimaryAction.NONE

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(viewBinding.root)
        initToolbar(viewBinding.toolbarLayout.toolbar, R.string.microg_title, true)
        userId = currentUserID()

        viewBinding.primaryButton.setOnClickListener {
            when (primaryAction) {
                PrimaryAction.INSTALL -> MicroGManager.install(userId)
                PrimaryAction.RETRY -> MicroGManager.startDownload(false, true)
                PrimaryAction.NONE -> Unit
            }
        }
        viewBinding.mobileDataButton.setOnClickListener {
            MicroGManager.startDownload(true, true)
        }

        MicroGManager.downloadState.observe(this) { render() }
        MicroGManager.installing.observe(this) { render() }
        MicroGManager.gmsStatus.observe(this) { render() }
        MicroGManager.installOutcome.observe(this) { outcome ->
            if (outcome == null) return@observe
            MicroGManager.consumeInstallOutcome()
            if (outcome.success) {
                toast(outcome.message)
            } else {
                MaterialDialog(this).show {
                    title(R.string.microg_title)
                    message(text = outcome.message)
                    positiveButton(R.string.done)
                }
            }
        }

        MicroGManager.refreshGmsStatus(userId)
        MicroGManager.startDownload(false)
    }

    override fun onResume() {
        super.onResume()
        MicroGManager.refreshGmsStatus(userId)
    }

    private fun render() {
        val b = viewBinding
        val state = MicroGManager.downloadState.value
        val status = MicroGManager.gmsStatus.value
        val installing = MicroGManager.installing.value == true

        b.primaryButton.visibility = View.GONE
        b.mobileDataButton.visibility = View.GONE
        b.progressBar.visibility = View.GONE
        b.progressBar.isIndeterminate = false
        primaryAction = PrimaryAction.NONE

        if (installing) {
            b.statusText.setText(R.string.microg_installing)
            b.detailText.text = ""
            b.progressBar.visibility = View.VISIBLE
            b.progressBar.isIndeterminate = true
            return
        }

        if (status != null && status.userId == userId && status.present) {
            if (status.byUs) {
                b.statusText.setText(R.string.microg_installed)
                b.detailText.text = getString(R.string.microg_installed_detail, userId)
            } else {
                b.statusText.setText(R.string.microg_other_present)
                b.detailText.text = getString(R.string.microg_other_present_detail, userId)
            }
            return
        }

        when (state) {
            null, is DownloadState.Idle -> {
                b.statusText.setText(R.string.microg_checking)
                b.detailText.text = ""
            }
            is DownloadState.NotConfigured -> {
                b.statusText.setText(R.string.microg_not_set_up)
                b.detailText.text = getString(R.string.microg_not_set_up_detail, state.problem)
            }
            is DownloadState.WaitingForWifi -> {
                b.statusText.setText(R.string.microg_waiting_wifi)
                b.detailText.setText(R.string.microg_waiting_wifi_detail)
                b.mobileDataButton.visibility = View.VISIBLE
            }
            is DownloadState.Downloading -> {
                b.progressBar.visibility = View.VISIBLE
                if (state.total > 0) {
                    val pct = (state.done * 100 / state.total).toInt().coerceIn(0, 100)
                    b.progressBar.progress = pct
                    b.statusText.text = getString(R.string.microg_downloading, pct)
                } else {
                    b.progressBar.isIndeterminate = true
                    b.statusText.text = getString(R.string.microg_downloading_unknown, megabytes(state.done))
                }
                b.detailText.setText(R.string.microg_downloading_detail)
            }
            is DownloadState.Verifying -> {
                b.statusText.setText(R.string.microg_checking)
                b.detailText.text = ""
                b.progressBar.visibility = View.VISIBLE
                b.progressBar.isIndeterminate = true
            }
            is DownloadState.Ready -> {
                b.statusText.setText(R.string.microg_ready)
                b.detailText.text = getString(R.string.microg_ready_detail, userId)
                primaryAction = PrimaryAction.INSTALL
                b.primaryButton.setText(R.string.microg_install)
                b.primaryButton.visibility = View.VISIBLE
            }
            is DownloadState.Failed -> {
                b.statusText.setText(R.string.microg_failed)
                b.detailText.text = state.message
                primaryAction = PrimaryAction.RETRY
                b.primaryButton.setText(R.string.microg_retry)
                b.primaryButton.visibility = View.VISIBLE
            }
        }
    }

    private fun megabytes(bytes: Long): String =
        String.format(Locale.US, "%.1f MB", bytes / 1048576.0)

    companion object {
        fun start(context: Context, userId: Int) {
            val intent = Intent(context, MicroGActivity::class.java)
            intent.putExtra("userID", userId)
            context.startActivity(intent)
        }
    }
}
