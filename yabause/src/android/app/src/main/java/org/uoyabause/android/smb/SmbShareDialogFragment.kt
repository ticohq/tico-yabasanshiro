package org.uoyabause.android.smb

import android.app.Dialog
import android.os.Bundle
import android.text.format.Formatter
import android.view.KeyEvent
import android.view.View
import android.widget.Toast
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.viewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.launch
import org.devmiyax.yabasanshiro.BuildConfig
import org.devmiyax.yabasanshiro.R
import org.devmiyax.yabasanshiro.databinding.DialogSmbShareBinding
import org.uoyabause.android.DeviceCapabilities
import org.uoyabause.android.YabauseApplication
import org.uoyabause.android.game.AddGameBottomSheetFragment

/**
 * Connection form, folder browser and import progress for the SMB import, all in
 * one dialog. State lives in [SmbShareViewModel] so the session survives
 * rotation. The host fragment supplies the presenter through [listener].
 */
class SmbShareDialogFragment : DialogFragment() {
    var listener: AddGameBottomSheetFragment.Listener? = null

    private val viewModel: SmbShareViewModel by viewModels()
    private var binding: DialogSmbShareBinding? = null
    private lateinit var adapter: SmbEntryAdapter
    private var lastStage: SmbStage? = null
    private var lastPath: String? = null
    private var restored = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        restored = savedInstanceState != null
    }

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val b = DialogSmbShareBinding.inflate(layoutInflater)
        binding = b

        adapter = SmbEntryAdapter(
            onUp = { viewModel.navigateUp() },
            onFolder = { viewModel.navigateInto(it) },
            onFile = { viewModel.toggleSelected(it) },
        )
        b.smbList.layoutManager = LinearLayoutManager(requireContext())
        b.smbList.adapter = adapter
        b.smbList.itemAnimator = null

        val ctx = requireContext()
        val isPc = DeviceCapabilities.isPc(ctx)
        b.smbPcHint.visibility = if (isPc) View.VISIBLE else View.GONE

        if (!restored) {
            lifecycleScope.launch {
                val saved = viewModel.loadSaved()
                if (saved != null) {
                    b.smbHost.setText(saved.host)
                    b.smbShare.setText(saved.share)
                    b.smbUser.setText(saved.username)
                    b.smbPassword.setText(saved.password)
                    b.smbDomain.setText(saved.domain)
                    b.smbRemember.isChecked = true
                } else if (isPc) {
                    // No host prefill: the retail Google Play Games NAT refuses
                    // the gateway 10.0.2.2 (ENETUNREACH), unlike the developer
                    // emulator where it forwards to the Windows host. The
                    // host's LAN address works, and the hint explains how to
                    // find it.
                    b.smbRemember.isChecked = true
                }
            }
        }

        b.smbBtnCancel.setOnClickListener {
            if (viewModel.state.value.stage == SmbStage.IMPORTING) {
                viewModel.cancelImport()
            } else {
                dismiss()
            }
        }
        b.smbBtnConnect.setOnClickListener { onPrimaryClicked(b) }
        b.smbDomain.setOnEditorActionListener { _, _, _ ->
            onPrimaryClicked(b)
            true
        }
        setupFocusAnimations(b)

        val dialog = MaterialAlertDialogBuilder(ctx).setView(b.root).create()
        dialog.setCanceledOnTouchOutside(false)
        dialog.setOnKeyListener { _, keyCode, event ->
            when {
                keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP -> handleBack()
                // Esc acts as back on Play Games on PC, which has no back button.
                // At the connection form there is nothing to go back to, so close.
                keyCode == KeyEvent.KEYCODE_ESCAPE -> {
                    if (event.action == KeyEvent.ACTION_UP && !handleBack()) dismiss()
                    true
                }
                else -> false
            }
        }
        dialog.setOnShowListener { focusPrimary(b) }

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch { viewModel.state.collect { render(b, it) } }
                launch { viewModel.events.collect { onEvent(it) } }
            }
        }
        return dialog
    }

    override fun onDestroyView() {
        binding = null
        super.onDestroyView()
    }

    private fun handleBack(): Boolean = when (viewModel.state.value.stage) {
        SmbStage.IMPORTING -> true
        SmbStage.LOADING, SmbStage.CONNECTING -> true
        SmbStage.BROWSING -> viewModel.navigateUp()
        SmbStage.FORM -> false
    }

    private fun onPrimaryClicked(b: DialogSmbShareBinding) {
        when (viewModel.state.value.stage) {
            SmbStage.FORM -> {
                val host = b.smbHost.text
                    ?.toString()
                    .orEmpty()
                    .trim()
                val share = b.smbShare.text
                    ?.toString()
                    .orEmpty()
                    .trim()
                if (host.isEmpty() || share.isEmpty()) {
                    showError(b, getString(R.string.smb_error_host_required))
                    return
                }
                val info = SmbConnectionInfo(
                    host = host,
                    share = share,
                    username = b.smbUser.text
                        ?.toString()
                        .orEmpty(),
                    password = b.smbPassword.text
                        ?.toString()
                        .orEmpty(),
                    domain = b.smbDomain.text
                        ?.toString()
                        .orEmpty(),
                )
                viewModel.connect(info, b.smbRemember.isChecked)
            }
            SmbStage.BROWSING -> requestImport()
            else -> Unit
        }
    }

    /**
     * Asks for the storage choice, then imports. The free-version limit is
     * enforced by the view model against the planned games; the check here only
     * answers early when no slot is left at all.
     */
    private fun requestImport() {
        val presenter = listener?.getImportPresenter()
        if (presenter == null) {
            // Recreated dialog without its host: isPro() still knows a saved
            // purchase or subscription, and the view model applies the limit.
            viewModel.startImport(isSubscribed = false)
            return
        }
        if (presenter.isGameLimitReached()) {
            YabauseApplication.checkDonated(requireActivity())
            return
        }
        presenter.selectStorage {
            viewModel.startImport(presenter.isOnSubscription)
        }
    }

    private fun showFreeLimit(e: FreeLimitExceededException) {
        val message = if (e.remaining == 0) {
            getString(R.string.smb_free_limit_none, BuildConfig.MAX_FREE_GAMES)
        } else {
            getString(R.string.smb_free_limit_some, BuildConfig.MAX_FREE_GAMES, e.remaining, e.requested)
        }
        Toast.makeText(requireContext(), message, Toast.LENGTH_LONG).show()
        YabauseApplication.checkDonated(requireActivity())
    }

    private fun onEvent(event: SmbEvent) {
        when (event) {
            is SmbEvent.ImportFinished -> {
                val outcome = event.outcome
                val host = listener
                val presenter = host?.getImportPresenter()
                val ctx = requireContext()
                if (outcome.cancelled) {
                    Toast.makeText(ctx, R.string.smb_import_cancelled, Toast.LENGTH_SHORT).show()
                }
                if (outcome.entries.isNotEmpty() && host != null) {
                    dismissAllowingStateLoss()
                    if (outcome.entries.size == 1 && presenter != null) {
                        presenter.fileSelected(outcome.entries.first())
                    } else {
                        Toast
                            .makeText(
                                ctx,
                                getString(R.string.smb_import_done, outcome.entries.size),
                                Toast.LENGTH_SHORT,
                            ).show()
                        host.onGamesInstalled()
                    }
                } else if (outcome.error == null && !outcome.cancelled) {
                    Toast.makeText(ctx, R.string.smb_import_no_game, Toast.LENGTH_LONG).show()
                }
                // The inline error line sits below the list and is easy to miss.
                val error = outcome.error
                if (error is FreeLimitExceededException) {
                    showFreeLimit(error)
                } else if (error != null && !outcome.cancelled) {
                    val kind = (error as? SmbClientException)?.kind ?: SmbErrorKind.OTHER
                    Toast.makeText(ctx, errorText(kind, error.message), Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    private fun render(b: DialogSmbShareBinding, s: SmbUiState) {
        val form = s.stage == SmbStage.FORM || s.stage == SmbStage.CONNECTING
        val browser = s.stage == SmbStage.BROWSING || s.stage == SmbStage.LOADING
        b.smbGroupForm.visibility = if (form) View.VISIBLE else View.GONE
        b.smbGroupBrowser.visibility = if (browser) View.VISIBLE else View.GONE
        b.smbGroupProgress.visibility = if (s.stage == SmbStage.IMPORTING) View.VISIBLE else View.GONE

        val connecting = s.stage == SmbStage.CONNECTING
        listOf(b.smbHost, b.smbShare, b.smbUser, b.smbPassword, b.smbDomain).forEach {
            it.isEnabled = !connecting
        }
        b.smbRemember.isEnabled = !connecting

        // Buttons
        when (s.stage) {
            SmbStage.FORM, SmbStage.CONNECTING -> {
                b.smbBtnCancel.setText(R.string.cancel)
                b.smbBtnConnect.visibility = View.VISIBLE
                b.smbBtnConnect.setText(R.string.smb_connect)
                b.smbBtnConnect.isEnabled = !connecting
            }
            SmbStage.BROWSING, SmbStage.LOADING -> {
                b.smbBtnCancel.setText(R.string.cancel)
                b.smbBtnConnect.visibility = View.VISIBLE
                b.smbBtnConnect.text = if (s.selected.isEmpty()) {
                    getString(R.string.smb_import)
                } else {
                    getString(R.string.smb_import_count, s.selected.size)
                }
                b.smbBtnConnect.isEnabled = s.stage == SmbStage.BROWSING && s.selected.isNotEmpty()
            }
            SmbStage.IMPORTING -> {
                b.smbBtnCancel.setText(R.string.smb_cancel_import)
                b.smbBtnConnect.visibility = View.GONE
            }
        }

        // Browser
        if (browser) {
            b.smbPath.text = "\\\\" + b.smbHost.text
                .toString()
                .trim() + "\\" +
                b.smbShare.text
                    .toString()
                    .trim() + if (s.path.isEmpty()) "" else "\\" + s.path
            adapter.submit(s.path.isNotEmpty(), s.entries, s.selected)
            b.smbLoading.visibility = if (s.stage == SmbStage.LOADING) View.VISIBLE else View.GONE
            b.smbEmpty.visibility =
                if (s.stage == SmbStage.BROWSING && s.entries.isEmpty()) View.VISIBLE else View.GONE
        }

        // Progress
        if (s.stage == SmbStage.IMPORTING) {
            val p = s.progress
            val x = p?.extract
            if (x != null) {
                // Extraction can take minutes for a large 7z: show the archive and how far it got.
                val title = getString(R.string.smb_extracting, x.archiveName)
                if (x.bytesTotal <= 0) {
                    b.smbProgressBar.isIndeterminate = true
                    b.smbProgressText.text = title
                } else {
                    b.smbProgressBar.isIndeterminate = false
                    b.smbProgressBar.max = PROGRESS_MAX
                    b.smbProgressBar.progress =
                        (x.bytesDone.coerceAtMost(x.bytesTotal) * PROGRESS_MAX / x.bytesTotal).toInt()
                    b.smbProgressText.text = title + "\n" +
                        Formatter.formatShortFileSize(requireContext(), x.bytesDone) + " / " +
                        Formatter.formatShortFileSize(requireContext(), x.bytesTotal)
                }
            } else if (p == null || p.bytesTotal <= 0) {
                b.smbProgressBar.isIndeterminate = true
                b.smbProgressText.text = ""
            } else {
                b.smbProgressBar.isIndeterminate = false
                b.smbProgressBar.max = PROGRESS_MAX
                b.smbProgressBar.progress = (p.bytesDone * PROGRESS_MAX / p.bytesTotal).toInt()
                b.smbProgressText.text = getString(
                    R.string.smb_progress_format,
                    p.fileName,
                    p.fileIndex.coerceAtMost(p.fileCount),
                    p.fileCount,
                    Formatter.formatShortFileSize(requireContext(), p.bytesDone),
                    Formatter.formatShortFileSize(requireContext(), p.bytesTotal),
                )
            }
        }

        // Error line
        val kind = s.errorKind
        if (kind != null) showError(b, errorText(kind, s.errorDetail)) else clearError(b)

        // Focus handling on stage / folder changes
        if (s.stage != lastStage || (browser && s.path != lastPath && s.stage == SmbStage.BROWSING)) {
            if (s.stage == SmbStage.BROWSING) {
                b.smbList.post {
                    b.smbList
                        .findViewHolderForAdapterPosition(0)
                        ?.itemView
                        ?.requestFocus()
                        ?: b.smbBtnCancel.requestFocus()
                }
            } else if (s.stage == SmbStage.IMPORTING) {
                b.smbBtnCancel.post { b.smbBtnCancel.requestFocus() }
            } else if (s.stage == SmbStage.FORM && lastStage != null) {
                b.smbBtnConnect.post { b.smbBtnConnect.requestFocus() }
            }
        }
        lastStage = s.stage
        if (s.stage == SmbStage.BROWSING) lastPath = s.path
    }

    private fun errorText(kind: SmbErrorKind, detail: String?): String = when (kind) {
        SmbErrorKind.AUTH_FAILED -> getString(R.string.smb_error_auth)
        SmbErrorKind.SHARE_NOT_FOUND -> getString(R.string.smb_error_share_not_found)
        SmbErrorKind.HOST_UNREACHABLE -> getString(R.string.smb_error_unreachable)
        SmbErrorKind.TIMEOUT -> getString(R.string.smb_error_timeout)
        SmbErrorKind.NOT_LOCAL_NETWORK -> getString(R.string.smb_error_not_local)
        SmbErrorKind.ACCESS_DENIED -> getString(R.string.smb_error_access_denied)
        SmbErrorKind.NOT_FOUND -> getString(R.string.smb_error_not_found)
        SmbErrorKind.OTHER -> getString(R.string.smb_error_other, detail ?: "")
    }

    private fun showError(b: DialogSmbShareBinding, text: String) {
        b.smbError.text = text
        b.smbError.visibility = View.VISIBLE
        b.smbError.announceForAccessibility(text)
    }

    private fun clearError(b: DialogSmbShareBinding) {
        b.smbError.visibility = View.GONE
    }

    /**
     * Gamepad: give the primary button initial focus. Only outside touch mode:
     * making buttons focusable in touch mode turns the first tap or mouse click
     * into a focus change instead of a click (Play Games on PC reports mouse
     * input as touch), so the import button appeared to do nothing.
     */
    private fun focusPrimary(b: DialogSmbShareBinding) {
        b.smbBtnConnect.post {
            if (!b.smbBtnConnect.isInTouchMode) b.smbBtnConnect.requestFocus()
        }
    }

    private fun setupFocusAnimations(b: DialogSmbShareBinding) {
        listOf(b.smbBtnCancel, b.smbBtnConnect).forEach { v ->
            v.setOnFocusChangeListener { view, hasFocus ->
                view
                    .animate()
                    .scaleX(if (hasFocus) 1.05f else 1.0f)
                    .scaleY(if (hasFocus) 1.05f else 1.0f)
                    .setDuration(150)
                    .start()
            }
        }
    }

    companion object {
        const val TAG = "SmbShareDialog"
        private const val PROGRESS_MAX = 1000

        fun newInstance(listener: AddGameBottomSheetFragment.Listener): SmbShareDialogFragment =
            SmbShareDialogFragment().also { it.listener = listener }
    }
}
