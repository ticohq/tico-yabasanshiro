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

import android.content.res.Configuration
import android.os.Bundle
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.AutoCompleteTextView
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.widget.NestedScrollView
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout
import androidx.viewpager2.widget.ViewPager2
import com.bumptech.glide.Glide
import com.bumptech.glide.signature.ObjectKey
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.AuthUI.IdpConfig.AppleBuilder
import com.firebase.ui.auth.AuthUI.IdpConfig.GoogleBuilder
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.textfield.TextInputLayout
import com.google.firebase.auth.FirebaseAuth
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.YabauseApplication
import org.uoyabause.android.auth.ui.PcSignInActivity
import org.uoyabause.android.backup.model.BackupItem
import org.uoyabause.android.backup.model.DeviceType
import org.uoyabause.android.backup.model.LocalBackupFile
import org.uoyabause.android.backup.viewmodel.BackupManagerViewModel
import org.uoyabause.android.backup.viewmodel.BackupUiState
import java.io.File
import java.util.Arrays

/**
 * Fragment displaying a list of backup items for a specific device type.
 */
class BackupListFragment :
    Fragment(),
    BackupListAdapter.OnItemClickListener,
    BackupDetailBottomSheet.Listener {
    private val viewModel: BackupManagerViewModel by activityViewModels()

    private lateinit var signInLauncher: ActivityResultLauncher<android.content.Intent>

    private lateinit var deviceType: DeviceType
    private var isLocalTab: Boolean = false

    private lateinit var swipeRefresh: SwipeRefreshLayout
    private lateinit var recyclerView: RecyclerView
    private lateinit var layoutStorageStatus: LinearLayout
    private lateinit var progressStorage: LinearProgressIndicator
    private lateinit var tvStorageStatus: TextView
    private lateinit var tvBackupLimit: TextView
    private lateinit var layoutEmpty: LinearLayout
    private lateinit var tvEmptyMessage: TextView
    private lateinit var layoutAuthRequired: LinearLayout
    private lateinit var btnSignIn: Button
    private lateinit var layoutError: LinearLayout
    private lateinit var tvErrorMessage: TextView
    private lateinit var btnRetry: Button
    private lateinit var layoutLoading: View
    private lateinit var layoutFileSelector: TextInputLayout
    private lateinit var dropdownFileSelector: AutoCompleteTextView

    // Landscape detail panel (null in portrait)
    private var detailContainer: FrameLayout? = null
    private var detailPlaceholder: LinearLayout? = null

    private lateinit var adapter: BackupListAdapter

    private val isLandscape: Boolean
        get() = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        deviceType = arguments?.getInt(ARG_DEVICE_TYPE)?.let { DeviceType.fromId(it) }
            ?: DeviceType.INTERNAL
        isLocalTab = arguments?.getBoolean(ARG_IS_LOCAL_TAB, false) ?: false

        signInLauncher =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
                if (FirebaseAuth.getInstance().currentUser != null) {
                    viewModel.loadBackups(deviceType)
                }
            }
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View? = inflater.inflate(R.layout.fragment_backup_list, container, false)

    override fun onResume() {
        super.onResume()
        // Re-check auth state when becoming visible (e.g., user logged in on another tab)
        if (deviceType == DeviceType.CLOUD &&
            viewModel.screenState.value.cloudState is BackupUiState.RequiresAuth &&
            viewModel.isAuthenticated()
        ) {
            viewModel.loadBackups(deviceType)
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        setupViews(view)
        setupRecyclerView()
        if (isLocalTab) {
            setupFileSelector()
        }
        observeViewModel()
    }

    private fun setupViews(view: View) {
        swipeRefresh = view.findViewById(R.id.swipe_refresh)
        recyclerView = view.findViewById(R.id.recycler_view_backups)

        // Ensure D-pad focus reaches RecyclerView items instead of stopping at containers
        swipeRefresh.descendantFocusability = ViewGroup.FOCUS_AFTER_DESCENDANTS
        layoutStorageStatus = view.findViewById(R.id.layout_storage_status)
        progressStorage = view.findViewById(R.id.progress_storage)
        tvStorageStatus = view.findViewById(R.id.tv_storage_status)
        tvBackupLimit = view.findViewById(R.id.tv_backup_limit)
        layoutEmpty = view.findViewById(R.id.layout_empty)
        tvEmptyMessage = view.findViewById(R.id.tv_empty_message)
        layoutAuthRequired = view.findViewById(R.id.layout_auth_required)
        btnSignIn = view.findViewById(R.id.btn_sign_in)
        layoutError = view.findViewById(R.id.layout_error)
        tvErrorMessage = view.findViewById(R.id.tv_error_message)
        btnRetry = view.findViewById(R.id.btn_retry)
        layoutLoading = view.findViewById(R.id.layout_loading)
        layoutFileSelector = view.findViewById(R.id.layout_file_selector)
        dropdownFileSelector = view.findViewById(R.id.dropdown_file_selector)

        // Landscape detail panel
        detailContainer = view.findViewById(R.id.detail_container)
        detailPlaceholder = view.findViewById(R.id.layout_detail_placeholder)

        // Setup swipe refresh
        swipeRefresh.setOnRefreshListener {
            viewModel.loadBackups(deviceType)
        }

        // Setup retry button
        btnRetry.setOnClickListener {
            viewModel.loadBackups(deviceType)
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

        // Configure file selector visibility
        layoutFileSelector.visibility = if (isLocalTab) View.VISIBLE else View.GONE

        // Configure storage status visibility based on device type
        layoutStorageStatus.visibility = if (deviceType == DeviceType.CLOUD) {
            View.GONE
        } else {
            View.VISIBLE
        }

        // Configure backup limit visibility (only for Cloud)
        tvBackupLimit.visibility = if (deviceType == DeviceType.CLOUD) {
            View.VISIBLE
        } else {
            View.GONE
        }
    }

    private fun setupRecyclerView() {
        adapter = BackupListAdapter(this)
        recyclerView.layoutManager = LinearLayoutManager(requireContext())
        recyclerView.adapter = adapter

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

    private fun setupFileSelector() {
        dropdownFileSelector.setOnItemClickListener { _, _, position, _ ->
            val availableFiles = viewModel.screenState.value.availableLocalFiles
            if (position < availableFiles.size) {
                viewModel.selectLocalFile(availableFiles[position])
            }
        }

        dropdownFileSelector.setOnKeyListener { _, keyCode, event ->
            if (event.action == KeyEvent.ACTION_DOWN && keyCode == KeyEvent.KEYCODE_BUTTON_A) {
                showFileSelectionDialog()
                true
            } else {
                false
            }
        }
    }

    private fun showFileSelectionDialog() {
        val availableFiles = viewModel.screenState.value.availableLocalFiles
        if (availableFiles.isEmpty()) return

        val displayNames = availableFiles.map { it.displayName }.toTypedArray()
        val selectedFile = viewModel.screenState.value.selectedLocalFile
        val checkedItem = if (selectedFile != null) {
            availableFiles.indexOf(selectedFile)
        } else {
            -1
        }

        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.select_file)
            .setSingleChoiceItems(displayNames, checkedItem) { dialog, which ->
                viewModel.selectLocalFile(availableFiles[which])
                dialog.dismiss()
            }.setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun observeViewModel() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.screenState.collectLatest { screenState ->
                    if (isLocalTab) {
                        // Update file selector dropdown
                        updateFileSelector(screenState.availableLocalFiles, screenState.selectedLocalFile)

                        // Use localState for the backup list
                        val state = screenState.localState
                        swipeRefresh.isRefreshing = screenState.isRefreshing
                        updateUi(state)
                    } else {
                        // Get the state for this device type (Cloud)
                        val state = when (deviceType) {
                            DeviceType.CLOUD -> screenState.cloudState
                            else -> return@collectLatest
                        }

                        swipeRefresh.isRefreshing = screenState.isRefreshing
                        updateUi(state)
                    }
                }
            }
        }
    }

    private fun updateFileSelector(
        availableFiles: List<LocalBackupFile>,
        selectedFile: LocalBackupFile?,
    ) {
        val displayNames = availableFiles.map { it.displayName }
        val arrayAdapter = ArrayAdapter(
            requireContext(),
            android.R.layout.simple_dropdown_item_1line,
            displayNames,
        )
        dropdownFileSelector.setAdapter(arrayAdapter)

        // Set selected item text without triggering listener
        selectedFile?.let {
            dropdownFileSelector.setText(it.displayName, false)
        }
    }

    private fun updateUi(state: BackupUiState) {
        // Hide all state layouts first
        layoutLoading.visibility = View.GONE
        layoutEmpty.visibility = View.GONE
        layoutAuthRequired.visibility = View.GONE
        layoutError.visibility = View.GONE
        recyclerView.visibility = View.GONE

        when (state) {
            is BackupUiState.Initial -> {
                // Initial state - will be replaced quickly
            }
            is BackupUiState.Loading -> {
                layoutLoading.visibility = View.VISIBLE
            }
            is BackupUiState.Success -> {
                recyclerView.visibility = View.VISIBLE
                adapter.submitList(state.items)

                // Update storage status
                state.storageStatus?.let { status ->
                    layoutStorageStatus.visibility = View.VISIBLE
                    tvStorageStatus.text = getString(
                        R.string.storage_status,
                        status.displayFreeSize,
                        status.displayTotalSize,
                    )
                    progressStorage.progress = status.usagePercent
                }

                // Update backup limit (for cloud)
                state.backupLimits?.let { limits ->
                    tvBackupLimit.visibility = View.VISIBLE
                    tvBackupLimit.text = getString(
                        R.string.cloud_backups_count,
                        limits.currentCount,
                        limits.maxCount,
                    )
                }
            }
            is BackupUiState.Empty -> {
                layoutEmpty.visibility = View.VISIBLE
                tvEmptyMessage.text = state.message

                // Update storage status even when no backups exist
                state.storageStatus?.let { status ->
                    layoutStorageStatus.visibility = View.VISIBLE
                    tvStorageStatus.text = getString(
                        R.string.storage_status,
                        status.displayFreeSize,
                        status.displayTotalSize,
                    )
                    progressStorage.progress = status.usagePercent
                }
            }
            is BackupUiState.RequiresAuth -> {
                layoutAuthRequired.visibility = View.VISIBLE
            }
            is BackupUiState.Error -> {
                layoutError.visibility = View.VISIBLE
                tvErrorMessage.text = state.message
            }
        }
    }

    // BackupListAdapter.OnItemClickListener implementation

    override fun onItemClick(item: BackupItem) {
        val isCloud = deviceType == DeviceType.CLOUD
        if (isLandscape && detailContainer != null) {
            showDetailInPanel(item)
        } else {
            BackupDetailBottomSheet
                .newInstance(item, isCloud)
                .show(childFragmentManager, "backup_detail")
        }
    }

    /**
     * Show detail in the right-side panel (landscape mode).
     */
    private fun showDetailInPanel(item: BackupItem) {
        val container = detailContainer ?: return
        container.removeAllViews()

        val detailView = LayoutInflater
            .from(requireContext())
            .inflate(R.layout.bottom_sheet_backup_detail, container, false)

        // Adjust layout for inline panel: fill parent and pin buttons at bottom
        detailView.layoutParams = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT,
        )
        val scrollView = detailView.findViewById<NestedScrollView>(R.id.scroll_content)
        (scrollView.layoutParams as LinearLayout.LayoutParams).apply {
            height = 0
            weight = 1f
        }
        detailView.findViewById<View>(R.id.drag_handle).visibility = View.GONE

        // Screenshot
        val ivScreenshot = detailView.findViewById<ImageView>(R.id.iv_screenshot_detail)
        val screenshotPath = item.screenshotUrl
        if (!screenshotPath.isNullOrEmpty()) {
            if (screenshotPath.startsWith("https://")) {
                ivScreenshot.imageTintList = null
                Glide
                    .with(this)
                    .load(screenshotPath)
                    .placeholder(R.drawable.ic_save)
                    .fitCenter()
                    .into(ivScreenshot)
            } else {
                val file = File(screenshotPath)
                if (file.exists()) {
                    ivScreenshot.imageTintList = null
                    Glide
                        .with(this)
                        .load(file)
                        .signature(ObjectKey(file.lastModified()))
                        .fitCenter()
                        .into(ivScreenshot)
                } else {
                    ivScreenshot.setImageResource(R.drawable.ic_save)
                }
            }
        } else {
            ivScreenshot.setImageResource(R.drawable.ic_save)
        }

        // Game title
        val tvGameTitle = detailView.findViewById<TextView>(R.id.tv_game_title_detail)
        if (!item.gameTitle.isNullOrEmpty()) {
            tvGameTitle.text = item.gameTitle
            tvGameTitle.visibility = View.VISIBLE
        } else {
            tvGameTitle.visibility = View.GONE
        }

        // Filename
        detailView.findViewById<TextView>(R.id.tv_filename_detail).text = item.filename

        // Comment
        val tvComment = detailView.findViewById<TextView>(R.id.tv_comment_detail)
        if (item.comment.isNotEmpty()) {
            tvComment.text = item.comment
            tvComment.visibility = View.VISIBLE
        } else {
            tvComment.visibility = View.GONE
        }

        // Size + Date
        detailView.findViewById<TextView>(R.id.tv_size_detail).text = item.displayBlocks
        detailView.findViewById<TextView>(R.id.tv_date_detail).text = item.saveDate

        // Buttons
        val btnCopyTo = detailView.findViewById<MaterialButton>(R.id.btn_copy_to_detail)
        val btnShare = detailView.findViewById<MaterialButton>(R.id.btn_share_detail)
        val btnDelete = detailView.findViewById<MaterialButton>(R.id.btn_delete_detail)

        btnCopyTo.setOnClickListener { onCopyToClick(item) }
        btnShare.setOnClickListener { onShareClick(item) }
        btnDelete.setOnClickListener { onDeleteClick(item) }

        // Gamepad support for buttons
        val buttons = listOf<View>(btnDelete, btnShare, btnCopyTo)
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

        // Focus order (Share is available for both local and cloud items)
        btnDelete.nextFocusRightId = R.id.btn_share_detail
        btnShare.nextFocusLeftId = R.id.btn_delete_detail
        btnShare.nextFocusRightId = R.id.btn_copy_to_detail
        btnCopyTo.nextFocusLeftId = R.id.btn_share_detail

        container.addView(detailView)
        btnCopyTo.post { btnCopyTo.requestFocus() }
    }

    // BackupDetailBottomSheet.Listener implementation

    override fun onCopyToClick(item: BackupItem) {
        // Cloud tab: copying from cloud requires Pro ("クラウドバックアップからのコピー")
        // Local tab: local-to-local copy is allowed for free users
        if (deviceType == DeviceType.CLOUD && !YabauseApplication.isPro()) {
            YabauseApplication.checkDonated(requireActivity())
            return
        }
        viewModel.showCopyToDialog(item)
    }

    override fun onExportClick(item: BackupItem) {
        viewModel.requestExport(item)
    }

    override fun onShareClick(item: BackupItem) {
        if (!YabauseApplication.isPro()) {
            YabauseApplication.checkDonated(requireActivity())
            return
        }
        viewModel.requestShare(item)
    }

    override fun onDeleteClick(item: BackupItem) {
        viewModel.requestDelete(item)
    }

    companion object {
        private const val ARG_DEVICE_TYPE = "device_type"
        private const val ARG_IS_LOCAL_TAB = "is_local_tab"

        fun newInstance(deviceType: DeviceType): BackupListFragment = BackupListFragment().apply {
            arguments = Bundle().apply {
                putInt(ARG_DEVICE_TYPE, deviceType.id)
            }
        }

        fun newLocalInstance(): BackupListFragment = BackupListFragment().apply {
            arguments = Bundle().apply {
                putInt(ARG_DEVICE_TYPE, DeviceType.INTERNAL.id)
                putBoolean(ARG_IS_LOCAL_TAB, true)
            }
        }
    }
}
