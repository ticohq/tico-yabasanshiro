/*
 * Copyright 2024 devMiyax
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.uoyabause.android.backup.ui

import android.content.Intent
import android.content.res.Configuration
import android.os.Bundle
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.Fragment
import androidx.fragment.app.viewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import androidx.viewpager2.widget.ViewPager2
import com.bumptech.glide.Glide
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.AuthUI.IdpConfig.AppleBuilder
import com.firebase.ui.auth.AuthUI.IdpConfig.GoogleBuilder
import com.google.android.material.button.MaterialButton
import com.google.android.material.chip.Chip
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.YabauseApplication
import org.uoyabause.android.auth.AuthState
import org.uoyabause.android.auth.ui.PcSignInActivity
import org.uoyabause.android.backup.BackupShareLink
import org.uoyabause.android.backup.model.LocalBackupFile
import org.uoyabause.android.backup.model.SharedBackupItem
import org.uoyabause.android.backup.model.SharedBackupReportReason
import org.uoyabause.android.backup.viewmodel.BackupUiEvent
import org.uoyabause.android.backup.viewmodel.SharedBackupSortOrder
import org.uoyabause.android.backup.viewmodel.SharedBackupUiState
import org.uoyabause.android.backup.viewmodel.SharedBackupViewModel
import java.util.Arrays

/**
 * Fragment for searching and browsing shared community backups.
 */
class SharedBackupSearchFragment :
    Fragment(),
    SharedBackupListAdapter.OnItemClickListener,
    SharedBackupDetailBottomSheet.Listener {
    private val viewModel: SharedBackupViewModel by viewModels()

    private lateinit var signInLauncher: ActivityResultLauncher<Intent>

    private lateinit var recyclerView: RecyclerView
    private lateinit var layoutAuthRequired: LinearLayout
    private lateinit var btnSignIn: Button
    private lateinit var layoutEmpty: LinearLayout
    private lateinit var tvEmptyMessage: TextView
    private lateinit var layoutError: LinearLayout
    private lateinit var tvErrorMessage: TextView
    private lateinit var btnRetry: Button
    private lateinit var layoutLoading: View

    private lateinit var chipMyLibrary: Chip

    private lateinit var progressLoadMore: View
    private lateinit var layoutBlockedFooter: LinearLayout
    private lateinit var tvBlockedCount: TextView
    private lateinit var btnShowAllHidden: MaterialButton

    private lateinit var adapter: SharedBackupListAdapter

    // Landscape detail panel (null in portrait)
    private var detailContainer: FrameLayout? = null
    private var detailPlaceholder: LinearLayout? = null

    // Item currently shown in the detail panel/sheet, used to refresh its
    // downloadCount in place after an import without reselecting.
    private var currentDetailItem: SharedBackupItem? = null

    private val isLandscape: Boolean
        get() = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        signInLauncher =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
                if (AuthState.isSignedIn()) {
                    viewModel.loadSharedBackups()
                }
            }
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View? = inflater.inflate(R.layout.fragment_shared_backup_search, container, false)

    override fun onResume() {
        super.onResume()
        // Re-check auth state when becoming visible (e.g., user logged in on another tab)
        if (viewModel.uiState.value is SharedBackupUiState.RequiresAuth &&
            viewModel.isAuthenticated()
        ) {
            viewModel.loadSharedBackups()
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        setupViews(view)
        setupRecyclerView()
        observeViewModel()
    }

    private fun setupViews(view: View) {
        recyclerView = view.findViewById(R.id.recycler_view_shared)
        layoutAuthRequired = view.findViewById(R.id.layout_auth_required)
        btnSignIn = view.findViewById(R.id.btn_sign_in)
        layoutEmpty = view.findViewById(R.id.layout_empty)
        tvEmptyMessage = view.findViewById(R.id.tv_empty_message)
        layoutError = view.findViewById(R.id.layout_error)
        tvErrorMessage = view.findViewById(R.id.tv_error_message)
        btnRetry = view.findViewById(R.id.btn_retry)
        layoutLoading = view.findViewById(R.id.layout_loading)

        progressLoadMore = view.findViewById(R.id.progress_load_more)
        layoutBlockedFooter = view.findViewById(R.id.layout_blocked_footer)
        tvBlockedCount = view.findViewById(R.id.tv_blocked_count)
        btnShowAllHidden = view.findViewById(R.id.btn_show_all_hidden)
        btnShowAllHidden.setOnClickListener { viewModel.unblockAll() }
        applyGamepadClick(btnShowAllHidden)

        // Landscape detail panel
        detailContainer = view.findViewById(R.id.detail_container)
        detailPlaceholder = view.findViewById(R.id.layout_detail_placeholder)

        // My Library filter chip
        chipMyLibrary = view.findViewById(R.id.chip_my_library)
        chipMyLibrary.setOnCheckedChangeListener { _, isChecked ->
            viewModel.setLibraryFilter(isChecked)
        }

        // Gamepad A-button support for chip
        chipMyLibrary.setOnKeyListener { v, keyCode, event ->
            if (event.action == KeyEvent.ACTION_DOWN &&
                (
                    keyCode == KeyEvent.KEYCODE_BUTTON_A ||
                        keyCode == KeyEvent.KEYCODE_DPAD_CENTER ||
                        keyCode == KeyEvent.KEYCODE_ENTER
                )
            ) {
                v.performClick()
                true
            } else {
                false
            }
        }

        // Setup retry button
        btnRetry.setOnClickListener {
            viewModel.loadSharedBackups()
        }

        // Setup sign in button
        btnSignIn.setOnClickListener {
            val signInIntent =
                AuthUI
                    .getInstance()
                    .createSignInIntentBuilder()
                    .setTheme(R.style.AppTheme)
                    .setTosAndPrivacyPolicyUrls(
                        "https://www.yabasanshiro.com/terms-of-use",
                        "https://www.yabasanshiro.com/privacy",
                    ).setAvailableProviders(
                        Arrays.asList(
                            GoogleBuilder().build(),
                            AppleBuilder().build(),
                        ),
                    ).build()
            signInLauncher.launch(PcSignInActivity.wrap(requireContext(), signInIntent))
        }
    }

    private fun setupRecyclerView() {
        adapter = SharedBackupListAdapter(this)
        val layoutManager = LinearLayoutManager(requireContext())
        recyclerView.layoutManager = layoutManager
        recyclerView.adapter = adapter

        // Fetch the next page as the end of the list comes into view, so the
        // community list can be walked past the first page.
        recyclerView.addOnScrollListener(object : RecyclerView.OnScrollListener() {
            override fun onScrolled(recyclerView: RecyclerView, dx: Int, dy: Int) {
                if (dy <= 0) return
                val lastVisible = layoutManager.findLastVisibleItemPosition()
                if (lastVisible >= layoutManager.itemCount - LOAD_MORE_THRESHOLD) {
                    viewModel.loadMore()
                }
            }
        })

        // Disable ViewPager2 tab switching while RecyclerView is scrolling (portrait only)
        if (!isLandscape) {
            recyclerView.addOnScrollListener(object : RecyclerView.OnScrollListener() {
                override fun onScrollStateChanged(recyclerView: RecyclerView, newState: Int) {
                    val viewPager = parentFragment
                        ?.view
                        ?.findViewById<ViewPager2>(R.id.view_pager_backup)
                    viewPager?.isUserInputEnabled =
                        (newState == RecyclerView.SCROLL_STATE_IDLE)
                }
            })
        }
    }

    /**
     * Make a view respond to the gamepad A button the way the existing action
     * buttons on this screen do.
     */
    private fun applyGamepadClick(view: View) {
        view.setOnKeyListener { v, keyCode, event ->
            if (event.action == KeyEvent.ACTION_DOWN &&
                (
                    keyCode == KeyEvent.KEYCODE_BUTTON_A ||
                        keyCode == KeyEvent.KEYCODE_DPAD_CENTER ||
                        keyCode == KeyEvent.KEYCODE_ENTER
                )
            ) {
                v.performClick()
                true
            } else {
                false
            }
        }
    }

    private fun observeViewModel() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    viewModel.uiState.collectLatest { state ->
                        updateUi(state)
                    }
                }

                launch {
                    viewModel.uiEvents.collectLatest { event ->
                        handleUiEvent(event)
                    }
                }

                launch {
                    viewModel.isLoadingMore.collectLatest { loading ->
                        progressLoadMore.visibility = if (loading) View.VISIBLE else View.GONE
                    }
                }

                launch {
                    viewModel.blockedCount.collectLatest { count ->
                        layoutBlockedFooter.visibility =
                            if (count > 0) View.VISIBLE else View.GONE
                        if (count > 0) {
                            tvBlockedCount.text =
                                getString(R.string.hidden_owners_count, count)
                        }
                    }
                }
            }
        }
    }

    private fun updateUi(state: SharedBackupUiState) {
        // Hide all state layouts first
        layoutLoading.visibility = View.GONE
        layoutEmpty.visibility = View.GONE
        layoutAuthRequired.visibility = View.GONE
        layoutError.visibility = View.GONE
        recyclerView.visibility = View.GONE

        when (state) {
            is SharedBackupUiState.Initial -> {
                // Initial state
            }
            is SharedBackupUiState.Loading -> {
                layoutLoading.visibility = View.VISIBLE
            }
            is SharedBackupUiState.Success -> {
                recyclerView.visibility = View.VISIBLE
                adapter.submitList(state.items)
                refreshShownDetailCount(state.items)
            }
            is SharedBackupUiState.Empty -> {
                layoutEmpty.visibility = View.VISIBLE
                tvEmptyMessage.text = if (state.searchQuery.isNullOrBlank()) {
                    getString(R.string.no_shared_backups_found)
                } else {
                    "No results for \"${state.searchQuery}\""
                }
            }
            is SharedBackupUiState.RequiresAuth -> {
                layoutAuthRequired.visibility = View.VISIBLE
            }
            is SharedBackupUiState.Error -> {
                layoutError.visibility = View.VISIBLE
                tvErrorMessage.text = state.message
            }
        }
    }

    private fun handleUiEvent(event: BackupUiEvent) {
        when (event) {
            is BackupUiEvent.ShowToast -> {
                Toast.makeText(requireContext(), event.message, Toast.LENGTH_SHORT).show()
            }
            is BackupUiEvent.ShowConfirmation -> {
                val dialog = MaterialAlertDialogBuilder(requireContext())
                    .setTitle(event.title)
                    .setMessage(event.message)
                    .setPositiveButton(R.string.confirm) { _, _ ->
                        event.confirmAction()
                    }.setNegativeButton(R.string.cancel, null)
                    .create()
                dialog.setOnShowListener {
                    val negativeButton = dialog.getButton(android.content.DialogInterface.BUTTON_NEGATIVE)
                    negativeButton?.post {
                        negativeButton.isFocusable = true
                        negativeButton.isFocusableInTouchMode = true
                        negativeButton.requestFocus()
                    }
                }
                dialog.show()
            }
            is BackupUiEvent.ShowRatingDialog -> {
                showRatingDialog(event.item, event.currentRating)
            }
            else -> {
                // Handle other events if needed
            }
        }
    }

    private fun showRatingDialog(item: SharedBackupItem, currentRating: Int?) {
        val ratings = arrayOf("1 Star", "2 Stars", "3 Stars", "4 Stars", "5 Stars")
        val selectedIndex = (currentRating ?: 3) - 1

        val dialog = MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.rate_backup)
            .setSingleChoiceItems(ratings, selectedIndex, null)
            .setPositiveButton(R.string.confirm) { dlg, _ ->
                val listView = (dlg as androidx.appcompat.app.AlertDialog).listView
                val selected = listView.checkedItemPosition + 1
                viewModel.rateBackup(item, selected)
            }.setNegativeButton(R.string.cancel, null)
            .create()
        dialog.setOnShowListener {
            val positiveButton = dialog.getButton(android.content.DialogInterface.BUTTON_POSITIVE)
            positiveButton?.post {
                positiveButton.isFocusable = true
                positiveButton.isFocusableInTouchMode = true
                positiveButton.requestFocus()
            }
        }
        dialog.show()
    }

    // SharedBackupListAdapter.OnItemClickListener implementation

    override fun onItemClick(item: SharedBackupItem) {
        val isOwner = viewModel.isOwner(item)
        currentDetailItem = item
        if (isLandscape && detailContainer != null) {
            showDetailInPanel(item, isOwner)
        } else {
            SharedBackupDetailBottomSheet
                .newInstance(item, isOwner)
                .show(childFragmentManager, "shared_backup_detail")
        }
    }

    /**
     * Update the downloadCount shown in the open detail panel (landscape) or
     * bottom sheet (portrait) when the backing list item changes, so an import
     * is reflected immediately without reselecting.
     */
    private fun refreshShownDetailCount(items: List<SharedBackupItem>) {
        val shown = currentDetailItem ?: return
        val updated = items.firstOrNull { it.id == shown.id } ?: return
        if (updated.downloadCount == shown.downloadCount) return
        currentDetailItem = updated

        // Landscape panel (no-op when not inflated).
        detailContainer
            ?.findViewById<TextView>(R.id.tv_downloads_detail)
            ?.text = updated.downloadCount.toString()

        // Portrait bottom sheet, if it is currently shown.
        (
            childFragmentManager.findFragmentByTag("shared_backup_detail")
                as? SharedBackupDetailBottomSheet
        )?.updateDownloadCount(updated.downloadCount)
    }

    /**
     * Show detail in the right-side panel (landscape mode).
     */
    private fun showDetailInPanel(item: SharedBackupItem, isOwner: Boolean) {
        val container = detailContainer ?: return
        container.removeAllViews()

        val detailView = LayoutInflater
            .from(requireContext())
            .inflate(R.layout.bottom_sheet_shared_backup_detail, container, false)

        // Bind data
        val ivScreenshot = detailView.findViewById<ImageView>(R.id.iv_screenshot_detail)
        if (!item.screenshotUrl.isNullOrEmpty()) {
            ivScreenshot.imageTintList = null
            Glide
                .with(this)
                .load(item.screenshotUrl)
                .placeholder(R.drawable.ic_save)
                .fitCenter()
                .into(ivScreenshot)
        } else {
            ivScreenshot.setImageResource(R.drawable.ic_save)
        }

        detailView.findViewById<TextView>(R.id.tv_game_title_detail).text =
            item.gameTitle.ifEmpty { "Unknown Game" }
        detailView.findViewById<TextView>(R.id.tv_filename_detail).text = item.filename

        val tvDescription = detailView.findViewById<TextView>(R.id.tv_description_detail)
        if (item.description.isNotEmpty()) {
            tvDescription.text = item.description
            tvDescription.visibility = View.VISIBLE
        } else {
            tvDescription.visibility = View.GONE
        }

        val tvComment = detailView.findViewById<TextView>(R.id.tv_comment_detail)
        if (item.comment.isNotEmpty()) {
            tvComment.text = item.comment
            tvComment.visibility = View.VISIBLE
        } else {
            tvComment.visibility = View.GONE
        }

        val ivOwnerPhoto = detailView.findViewById<ImageView>(R.id.iv_owner_photo_detail)
        if (!item.ownerPhotoUrl.isNullOrEmpty()) {
            Glide
                .with(this)
                .load(item.ownerPhotoUrl)
                .placeholder(R.drawable.ic_account_circle_24)
                .circleCrop()
                .into(ivOwnerPhoto)
        } else {
            ivOwnerPhoto.setImageResource(R.drawable.ic_account_circle_24)
        }

        detailView.findViewById<TextView>(R.id.tv_owner_name_detail).text = item.ownerName
        detailView.findViewById<TextView>(R.id.tv_rating_detail).text = item.displayRating
        detailView.findViewById<TextView>(R.id.tv_downloads_detail).text =
            item.downloadCount.toString()
        detailView.findViewById<TextView>(R.id.tv_shared_date_detail).text =
            item.displaySharedDate

        // Buttons
        val btnImport = detailView.findViewById<MaterialButton>(R.id.btn_import_detail)
        val btnRate = detailView.findViewById<MaterialButton>(R.id.btn_rate_detail)
        val btnShare = detailView.findViewById<MaterialButton>(R.id.btn_share_detail)
        val btnDelete = detailView.findViewById<MaterialButton>(R.id.btn_delete_detail)
        val btnMore = detailView.findViewById<MaterialButton>(R.id.btn_more_detail)

        btnImport.setOnClickListener { onImportClick(item) }
        btnRate.setOnClickListener { onRateClick(item) }
        btnShare.setOnClickListener { onShareClick(item) }
        btnDelete.setOnClickListener { onDeleteClick(item) }
        btnDelete.visibility = if (isOwner) View.VISIBLE else View.GONE
        btnMore.visibility = if (canReport(item)) View.VISIBLE else View.GONE
        btnMore.setOnClickListener { anchor -> onMoreClick(item, anchor) }

        // Gamepad support for buttons
        val buttons = listOf<View>(btnImport, btnRate, btnShare, btnDelete, btnMore)
        buttons.forEach { button ->
            button.setOnKeyListener { v, keyCode, event ->
                if (event.action == KeyEvent.ACTION_DOWN) {
                    when (keyCode) {
                        KeyEvent.KEYCODE_BUTTON_A,
                        KeyEvent.KEYCODE_ENTER,
                        KeyEvent.KEYCODE_DPAD_CENTER,
                        -> {
                            v.performClick()
                            true
                        }
                        else -> false
                    }
                } else {
                    false
                }
            }
            button.setOnFocusChangeListener { v, hasFocus ->
                val scale = if (hasFocus) 1.05f else 1.0f
                val alpha = if (hasFocus) 1.0f else 0.9f
                v
                    .animate()
                    .scaleX(scale)
                    .scaleY(scale)
                    .alpha(alpha)
                    .setDuration(150)
                    .start()
            }
        }

        // Focus order
        btnImport.nextFocusLeftId = R.id.btn_rate_detail
        btnRate.nextFocusRightId = R.id.btn_import_detail
        btnRate.nextFocusLeftId = R.id.btn_share_detail
        btnShare.nextFocusRightId = R.id.btn_rate_detail
        if (isOwner) {
            btnShare.nextFocusLeftId = R.id.btn_delete_detail
            btnDelete.nextFocusRightId = R.id.btn_share_detail
        }

        container.addView(detailView)
        btnImport.post { btnImport.requestFocus() }
    }

    // SharedBackupDetailBottomSheet.Listener implementation

    override fun onImportClick(item: SharedBackupItem) {
        if (!YabauseApplication.isPro()) {
            YabauseApplication.checkDonated(requireActivity())
            return
        }
        val existingFiles = LocalBackupFile.getExistingFiles()

        if (existingFiles.isEmpty()) {
            Toast.makeText(requireContext(), "No local backup files found", Toast.LENGTH_SHORT).show()
            return
        }

        val fileNames = existingFiles.map { it.displayName }.toTypedArray()

        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.import_backup)
            .setItems(fileNames) { _, which ->
                viewModel.importBackupToFile(item, existingFiles[which])
            }.show()
    }

    override fun onRateClick(item: SharedBackupItem) {
        if (!YabauseApplication.isPro()) {
            YabauseApplication.checkDonated(requireActivity())
            return
        }
        viewModel.showRatingDialog(item)
    }

    override fun onShareClick(item: SharedBackupItem) {
        // Already-shared items are public; just share the existing link (no re-upload).
        val url = BackupShareLink.buildUrl(item.id)
        val sendIntent = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, getString(R.string.share_backup_link_subject))
            putExtra(
                Intent.EXTRA_TEXT,
                getString(
                    R.string.share_backup_link_text,
                    item.gameTitle,
                    url,
                    BackupShareLink.hashtags(item.gameTitle),
                ),
            )
        }
        startActivity(
            Intent.createChooser(sendIntent, getString(R.string.share_backup_link_chooser)),
        )
    }

    override fun onDeleteClick(item: SharedBackupItem) {
        viewModel.requestDeleteSharedBackup(item)
    }

    override fun canReport(item: SharedBackupItem): Boolean = viewModel.canReport(item)

    override fun onMoreClick(item: SharedBackupItem, anchor: View) {
        PopupMenu(requireContext(), anchor)
            .apply {
                menuInflater.inflate(R.menu.menu_shared_backup_item, menu)
                setOnMenuItemClickListener { menuItem ->
                    when (menuItem.itemId) {
                        R.id.action_report_backup -> {
                            showReportDialog(item)
                            true
                        }
                        R.id.action_hide_owner -> {
                            confirmHideOwner(item)
                            true
                        }
                        else -> false
                    }
                }
            }.show()
    }

    /**
     * Ask which of the fixed reasons applies. Free text is deliberately not
     * offered: it would need moderating itself and is not what a reviewer can
     * act on.
     */
    private fun showReportDialog(item: SharedBackupItem) {
        val reasons = SharedBackupReportReason.entries.toTypedArray()
        val labels = reasons.map { getString(it.labelRes) }.toTypedArray()

        val dialog = MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.report_dialog_title)
            .setSingleChoiceItems(labels, 0, null)
            .setPositiveButton(R.string.report_send) { dlg, _ ->
                val listView = (dlg as androidx.appcompat.app.AlertDialog).listView
                val selected = listView.checkedItemPosition.coerceIn(reasons.indices)
                viewModel.reportBackup(item, reasons[selected])
            }.setNegativeButton(R.string.cancel, null)
            .create()
        dialog.setOnShowListener {
            val negativeButton = dialog.getButton(android.content.DialogInterface.BUTTON_NEGATIVE)
            negativeButton?.post {
                negativeButton.isFocusable = true
                negativeButton.isFocusableInTouchMode = true
                negativeButton.requestFocus()
            }
        }
        dialog.show()
    }

    private fun confirmHideOwner(item: SharedBackupItem) {
        val dialog = MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.hide_owner_confirm_title)
            .setMessage(getString(R.string.hide_owner_confirm_message, item.ownerName))
            .setPositiveButton(R.string.confirm) { _, _ ->
                viewModel.blockOwner(item)
                dismissDetailFor(item)
            }.setNegativeButton(R.string.cancel, null)
            .create()
        dialog.setOnShowListener {
            val negativeButton = dialog.getButton(android.content.DialogInterface.BUTTON_NEGATIVE)
            negativeButton?.post {
                negativeButton.isFocusable = true
                negativeButton.isFocusableInTouchMode = true
                negativeButton.requestFocus()
            }
        }
        dialog.show()
    }

    /**
     * Close whatever is showing the save that just got hidden, so the detail
     * does not outlive its row.
     */
    private fun dismissDetailFor(item: SharedBackupItem) {
        if (currentDetailItem?.id != item.id) return
        currentDetailItem = null
        detailContainer?.let { container ->
            container.removeAllViews()
            // showDetailInPanel() removes the placeholder from the container,
            // so put it back rather than leaving an empty panel.
            detailPlaceholder?.let { placeholder ->
                if (placeholder.parent == null) container.addView(placeholder)
                placeholder.visibility = View.VISIBLE
            }
        }
        (
            childFragmentManager.findFragmentByTag("shared_backup_detail")
                as? SharedBackupDetailBottomSheet
        )?.dismiss()
    }

    // region Toolbar bridge methods (called by parent BackupManagerFragment)

    /**
     * Called when the toolbar search query changes for the Shared tab.
     */
    fun onToolbarSearchQuery(query: String) {
        viewModel.search(query)
    }

    /**
     * Called when the toolbar sort menu is selected for the Shared tab.
     */
    fun onToolbarSortSelected(sortOrder: SharedBackupSortOrder) {
        viewModel.setSortOrder(sortOrder)
    }

    /**
     * Returns the current search query for toolbar state sync.
     */
    fun getCurrentSearchQuery(): String = viewModel.searchQuery.value

    /**
     * Returns the current sort order for toolbar state sync.
     */
    fun getCurrentSortOrder(): SharedBackupSortOrder = viewModel.sortOrder.value

    // endregion

    companion object {
        /**
         * How close to the end of the list a scroll must get before the next
         * page is requested. A few rows of lead time so the fetch overlaps the
         * remaining scroll instead of stalling at the bottom.
         */
        private const val LOAD_MORE_THRESHOLD = 4

        fun newInstance(): SharedBackupSearchFragment = SharedBackupSearchFragment()
    }
}
