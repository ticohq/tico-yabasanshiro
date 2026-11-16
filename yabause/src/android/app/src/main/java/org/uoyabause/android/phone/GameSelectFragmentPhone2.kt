@file:OptIn(kotlinx.coroutines.DelicateCoroutinesApi::class)

package org.uoyabause.android.phone

import android.animation.ObjectAnimator
import android.animation.PropertyValuesHolder
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.content.res.Configuration
import android.graphics.drawable.Icon
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.ViewTreeObserver
import android.view.inputmethod.InputMethodManager
import android.widget.ArrayAdapter
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.ListPopupWindow
import androidx.appcompat.widget.PopupMenu
import androidx.appcompat.widget.SearchView
import androidx.appcompat.widget.Toolbar
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.fragment.app.viewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout
import com.bumptech.glide.Glide
import com.bumptech.glide.request.RequestOptions
import com.frybits.harmony.getHarmonySharedPreferences
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.navigation.NavigationBarView
import com.google.android.material.navigationrail.NavigationRailView
import com.google.android.material.snackbar.Snackbar
import com.google.firebase.analytics.FirebaseAnalytics
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.FirebaseFirestore
import com.google.firebase.remoteconfig.FirebaseRemoteConfig
import com.google.firebase.remoteconfig.FirebaseRemoteConfigSettings
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.withContext
import org.devmiyax.yabasanshiro.BuildConfig
import org.devmiyax.yabasanshiro.R
import org.devmiyax.yabasanshiro.StartupActivity
import org.uoyabause.android.BillingViewModel
import org.uoyabause.android.GameInfo
import org.uoyabause.android.GameSelectPresenter
import org.uoyabause.android.SettingsContainerFragment
import org.uoyabause.android.ShowPinInFragment
import org.uoyabause.android.StorageMigrationHelper
import org.uoyabause.android.YabauseApplication
import org.uoyabause.android.YabauseStorage
import org.uoyabause.android.achievements.RetroAchievementsManager
import org.uoyabause.android.game.AddGameBottomSheetFragment
import org.uoyabause.android.news.NewsManager
import org.uoyabause.android.storage.PreferencesManager
import org.uoyabause.android.tv.GameSelectFragment

class GameSelectFragmentPhone2 :
    Fragment(),
    GameCardAdapter.OnItemClickListener,
    GameSelectPresenter.GameSelectPresenterListener,
    GameDetailBottomSheet.Listener,
    AddGameBottomSheetFragment.Listener {
    private val viewModel: GameSelectViewModel by viewModels()
    private val billingViewModel: BillingViewModel by activityViewModels()

    private lateinit var rootView: View
    private lateinit var recyclerView: RecyclerView
    private lateinit var gameAdapter: GameCardAdapter
    private lateinit var searchView: SearchView
    private lateinit var swipeRefresh: SwipeRefreshLayout
    private lateinit var searchBarContainer: View
    private var bottomNavigationView: BottomNavigationView? = null
    private var navigationRail: NavigationRailView? = null

    // State views
    private lateinit var layoutEmpty: View
    private lateinit var layoutLoading: View
    private lateinit var layoutError: View
    private var bannerGameLimit: View? = null

    private var firebaseAnalytics: FirebaseAnalytics? = null
    private var isFirstUpdate = true
    private var savedNavItemId: Int = 0

    // Activity result launchers
    private val yabauseActivityLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
            viewModel.loadGames()
        }

    private val settingActivityLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == GameSelectFragment.GAMELIST_NEED_TO_UPDATED) {
                viewModel.updateGameDatabase()
            } else if (result.resultCode == GameSelectFragment.GAMELIST_NEED_TO_RESTART) {
                val intent = Intent(activity, StartupActivity::class.java)
                intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_NEW_TASK)
                startActivity(intent)
                activity?.finish()
            }
        }

    private val readRequestLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                result.data?.data?.let { uri -> viewModel.onFileSelected(uri) }
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val remoteConfig = FirebaseRemoteConfig.getInstance()
        val configSettings = FirebaseRemoteConfigSettings
            .Builder()
            .setMinimumFetchIntervalInSeconds(3600)
            .build()
        remoteConfig.setConfigSettingsAsync(configSettings)
        remoteConfig.setDefaultsAsync(R.xml.config)

        // Initialize presenter through ViewModel
        viewModel.initPresenter(this, yabauseActivityLauncher, this)

        // Subscription handling (billing flow moved to onViewCreated to avoid viewLifecycleOwner crash)
        // Debug builds always enable subscription checking for testing
        val isSubscriptionEnabled = remoteConfig.getBoolean("is_enable_subscription_new") ||
            BuildConfig.DEBUG
        if (!isSubscriptionEnabled) {
            Log.i(TAG, "Subscription disabled by remote config, using runtime flag")
            viewModel.setSubscribed(true)
            YabauseApplication.isSubscriptionDisabledByRemoteConfig = true
        } else {
            Log.i(TAG, "Subscription enabled, will check billing in onViewCreated")
            viewModel.setSubscribed(false)
        }

        // Restore sort mode
        viewModel.setSortMode(viewModel.loadSortMode())
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View {
        rootView = inflater.inflate(R.layout.fragment_game_select_phone2, container, false)

        recyclerView = rootView.findViewById(R.id.recycler_games)
        recyclerView.layoutManager = LinearLayoutManager(context)
        recyclerView.descendantFocusability = ViewGroup.FOCUS_AFTER_DESCENDANTS

        gameAdapter = GameCardAdapter(this)
        recyclerView.adapter = gameAdapter

        searchView = rootView.findViewById(R.id.search_view)
        swipeRefresh = rootView.findViewById(R.id.swipe_refresh)
        searchBarContainer = rootView.findViewById(R.id.search_bar_container)
        layoutEmpty = rootView.findViewById(R.id.layout_empty)
        layoutLoading = rootView.findViewById(R.id.layout_loading)
        layoutError = rootView.findViewById(R.id.layout_error)
        bannerGameLimit = rootView.findViewById(R.id.banner_game_limit)
        bannerGameLimit?.findViewById<MaterialButton>(R.id.btn_game_limit_subscribe)?.setOnClickListener {
            viewModel.presentProGate()
        }

        // SwipeRefresh (disabled - not needed for game list)
        swipeRefresh.isEnabled = false

        // Search
        searchView.setOnQueryTextListener(object : SearchView.OnQueryTextListener {
            override fun onQueryTextSubmit(query: String?): Boolean = false

            override fun onQueryTextChange(newText: String?): Boolean {
                viewModel.setSearchQuery(newText ?: "")
                return true
            }
        })

        // Search button (opens collapsible search bar)
        rootView.findViewById<View>(R.id.btn_search).setOnClickListener { showSearchBar() }

        // Close search button
        rootView.findViewById<View>(R.id.btn_close_search).setOnClickListener { hideSearchBar() }

        // Sort button
        rootView.findViewById<View>(R.id.btn_sort).setOnClickListener { showSortMenu(it) }

        // Empty state add button
        rootView.findViewById<View>(R.id.btn_add_game_empty)?.setOnClickListener {
            showAddGameBottomSheet()
        }

        // Error state retry button
        rootView.findViewById<View>(R.id.btn_retry)?.setOnClickListener {
            viewModel.loadGames()
        }

        return rootView
    }

    private val searchBarBackCallback = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() {
            hideSearchBar()
        }
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        firebaseAnalytics = FirebaseAnalytics.getInstance(requireActivity())
        if (savedInstanceState == null) {
            NewsManager.checkAndShow(requireActivity())
        }
        savedNavItemId = savedInstanceState?.getInt(KEY_SELECTED_NAV_ITEM, 0) ?: 0
        setupToolbar(view)
        setupNavigation(rootView)

        // Subscription billing flow (must be in onViewCreated where viewLifecycleOwner is available)
        if (YabauseApplication.isSubscriptionDisabledByRemoteConfig) {
            // Remote config disabled subscription checking - remove any already-loaded ads
            // (Activity.onCreate() loads ads before Fragment sets the flag)
            (activity as? GameSelectActivityPhone)?.removeAdView()
        } else {
            viewLifecycleOwner.lifecycleScope.launch {
                viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                    billingViewModel.userCurrentSubscriptionFlow.collect { subscriptions ->
                        val subscribed = subscriptions.hasProAnnual == true
                        Log.i(TAG, "Subscription result: subscribed=$subscribed (proAnnual=${subscriptions.hasProAnnual})")
                        viewModel.setSubscribed(subscribed)
                        YabauseApplication.setSubscriptionState(subscribed)
                        if (subscribed) {
                            (activity as? GameSelectActivityPhone)?.removeAdView()
                        } else {
                            (activity as? GameSelectActivityPhone)?.showAdViewIfNeeded()
                        }
                    }
                }
            }
        }

        // Hide BottomNavigationView while the soft keyboard is visible
        setupKeyboardVisibilityListener()

        // Back press closes search bar when visible
        requireActivity().onBackPressedDispatcher.addCallback(viewLifecycleOwner, searchBarBackCallback)

        // Observe screen state
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.screenState.collectLatest { state ->
                    updateUi(state)
                }
            }
        }

        // Observe UI events
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.uiEvents.collect { event ->
                    handleUiEvent(event)
                }
            }
        }

        // Show migration dialog if needed
        showMigrationDialogIfNeeded()

        // Initial load — check storage migration before proceeding
        checkStorageMigrationThenLoad()

        // Show donation/pin prompt on first load
        if (isFirstUpdate) {
            isFirstUpdate = false
            if (requireActivity().intent?.getBooleanExtra("showPin", false) == true) {
                if (!YabauseApplication.isPro()) {
                    YabauseApplication.checkDonated(requireActivity())
                } else {
                    ShowPinInFragment.newInstance().show(childFragmentManager, "pin")
                }
            }
        }
    }

    override fun onDestroyView() {
        keyboardLayoutListener?.let { listener ->
            requireActivity()
                .findViewById<View>(android.R.id.content)
                ?.viewTreeObserver
                ?.removeOnGlobalLayoutListener(listener)
        }
        keyboardLayoutListener = null
        super.onDestroyView()
    }

    private fun showMigrationDialogIfNeeded() {
        val prefsManager = PreferencesManager.getInstance(requireContext())
        if (prefsManager.shouldShowMigrationDialog()) {
            val dialog = MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.migration_dialog_title)
                .setMessage(R.string.migration_dialog_message)
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    prefsManager.markMigrationDialogShown()
                }.setCancelable(false)
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
    }

    private fun checkStorageMigrationThenLoad() {
        val ctx = requireContext()
        val sdCardDir = requireActivity().getExternalFilesDirs(null).getOrNull(1)
        val needsInternal = StorageMigrationHelper.needsInternalMigration(ctx)
        val needsExternal = sdCardDir != null && StorageMigrationHelper.needsExternalMigration(ctx, sdCardDir)
        Log.i("StorageMigration", "needsInternal=$needsInternal needsExternal=$needsExternal sdCardDir=$sdCardDir")

        if (needsInternal || needsExternal) {
            MaterialAlertDialogBuilder(ctx)
                .setIcon(R.drawable.baseline_folder_open_24)
                .setTitle(R.string.storage_migration_dialog_title)
                .setMessage(R.string.storage_migration_dialog_message)
                .setCancelable(false)
                .setPositiveButton(R.string.storage_migration_dialog_migrate) { _, _ ->
                    runStorageMigration(sdCardDir)
                }.setNegativeButton(R.string.storage_migration_dialog_later) { _, _ ->
                    proceedInitialLoad()
                }.show()
        } else {
            proceedInitialLoad()
        }
    }

    private fun runStorageMigration(sdCardDir: java.io.File?) {
        val ctx = requireContext()
        val progressView =
            layoutInflater.inflate(R.layout.dialog_migration_progress, null)
        val progressText =
            progressView.findViewById<android.widget.TextView>(R.id.migration_progress_text)
        val progressDialog =
            MaterialAlertDialogBuilder(ctx)
                .setView(progressView)
                .setCancelable(false)
                .show()

        lifecycleScope.launch(Dispatchers.IO) {
            StorageMigrationHelper.performInternalMigration(ctx) { fileName ->
                launch(Dispatchers.Main) { progressText.text = fileName }
            }
            if (sdCardDir != null) {
                StorageMigrationHelper.performExternalMigration(ctx, sdCardDir) { fileName ->
                    launch(Dispatchers.Main) { progressText.text = fileName }
                }
            }
            withContext(Dispatchers.Main) {
                progressDialog.dismiss()
                proceedInitialLoad()
            }
        }
    }

    private fun proceedInitialLoad() {
        if (viewModel.presenter?.prepareStorage() != false) {
            viewModel.updateGameDatabase()
        }
    }

    /**
     * Shows the subscribe offer, and says how much of the library the limit is
     * holding back. Hidden entirely while nothing is locked: an offer that
     * stands there for a user with two games is noise, not information.
     */
    private fun updateGameLimitBanner(lockedCount: Int) {
        val banner = bannerGameLimit ?: return
        if (lockedCount <= 0) {
            banner.visibility = View.GONE
            return
        }
        banner.findViewById<TextView>(R.id.tv_game_limit_message)?.text =
            getString(R.string.game_limit_banner_message, BuildConfig.MAX_FREE_GAMES)
        banner.visibility = View.VISIBLE
    }

    private fun updateUi(state: GameSelectScreenState) {
        swipeRefresh.isRefreshing = state.isRefreshing

        // Update toolbar subtitle with game count
        val toolbar = rootView.findViewById<Toolbar>(R.id.toolbar)
        toolbar?.subtitle = if (state.gameCount > 0) {
            getString(R.string.games_count, state.gameCount)
        } else {
            getVersionName(requireContext())
        }

        when (val listState = state.listState) {
            is GameListUiState.Initial -> {
                // Do nothing, waiting for load
            }
            is GameListUiState.Loading -> {
                layoutLoading.visibility = View.VISIBLE
                layoutEmpty.visibility = View.GONE
                layoutError.visibility = View.GONE
                swipeRefresh.visibility = View.GONE
                // Hide detail panel placeholder (landscape only)
                rootView.findViewById<View>(R.id.detail_placeholder)?.visibility = View.GONE
                rootView.findViewById<View>(R.id.detail_content)?.visibility = View.GONE
                stopEmptyStateAnimation()
            }
            is GameListUiState.Success -> {
                layoutLoading.visibility = View.GONE
                layoutEmpty.visibility = View.GONE
                layoutError.visibility = View.GONE
                swipeRefresh.visibility = View.VISIBLE
                stopEmptyStateAnimation()
                gameAdapter.setUnlockedPaths(state.unlockedPaths)
                gameAdapter.submitList(listState.games)

                // Update RetroAchievements progress in background
                updateRetroAchievementsProgress(listState.games)

                // The offer shows only once the limit is actually costing the
                // user something, never merely because they are not subscribed.
                updateGameLimitBanner(state.lockedCount)

                // Landscape: update detail panel
                if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
                    updateDetailPanel(state.selectedGame)
                }
            }
            is GameListUiState.Empty -> {
                layoutLoading.visibility = View.GONE
                layoutEmpty.visibility = View.VISIBLE
                layoutError.visibility = View.GONE
                swipeRefresh.visibility = View.GONE
                // Show games directory path in empty state message
                val gamesPath = YabauseStorage.storage.gamePath.trimEnd('/')
                rootView.findViewById<TextView>(R.id.tv_add_games_message)?.text =
                    getString(R.string.add_games_message, gamesPath)
                // Hide detail panel placeholder (landscape only)
                rootView.findViewById<View>(R.id.detail_placeholder)?.visibility = View.GONE
                rootView.findViewById<View>(R.id.detail_content)?.visibility = View.GONE
                startEmptyStateAnimation()
            }
            is GameListUiState.Error -> {
                layoutLoading.visibility = View.GONE
                layoutEmpty.visibility = View.GONE
                layoutError.visibility = View.VISIBLE
                swipeRefresh.visibility = View.GONE
                // Hide detail panel placeholder (landscape only)
                rootView.findViewById<View>(R.id.detail_placeholder)?.visibility = View.GONE
                rootView.findViewById<View>(R.id.detail_content)?.visibility = View.GONE
                rootView.findViewById<TextView>(R.id.tv_error_message)?.text = listState.message
            }
        }
    }

    private fun handleUiEvent(event: GameSelectUiEvent) {
        when (event) {
            is GameSelectUiEvent.ShowSnackbar -> {
                Snackbar.make(rootView, event.message, Snackbar.LENGTH_SHORT).show()
            }
            is GameSelectUiEvent.ShowSnackbarRes -> {
                Snackbar.make(rootView, event.stringResId, Snackbar.LENGTH_SHORT).show()
            }
            is GameSelectUiEvent.ShowConfirmDelete -> {
                val dialog = MaterialAlertDialogBuilder(requireContext())
                    .setTitle(R.string.confirm_delete_title)
                    .setMessage(getString(R.string.confirm_delete_message, event.gameInfo.game_title))
                    .setPositiveButton(R.string.delete) { _, _ ->
                        viewModel.confirmDeleteGame(event.gameInfo)
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
            is GameSelectUiEvent.ShowProGate -> {
                YabauseApplication.checkDonated(
                    requireActivity(),
                    event.messageResId?.let { getString(it) } ?: "",
                )
            }
            is GameSelectUiEvent.ShowProgress -> {
                // Could show a progress dialog here
            }
            is GameSelectUiEvent.UpdateProgress -> {
                // Could update a progress dialog here
            }
            is GameSelectUiEvent.DismissProgress -> {
                // Could dismiss a progress dialog here
            }
            is GameSelectUiEvent.NavigateToGame,
            is GameSelectUiEvent.LaunchFilePicker,
            is GameSelectUiEvent.OpenSettings,
            is GameSelectUiEvent.OpenBackupManager,
            is GameSelectUiEvent.OpenAccountManager,
            is GameSelectUiEvent.SignedOut,
            -> {
                // Handled directly by fragment navigation
            }
        }
    }

    private fun setupToolbar(view: View) {
        val activity = requireActivity() as AppCompatActivity
        val toolbar = rootView.findViewById<Toolbar>(R.id.toolbar)
        val versionName = getVersionName(activity)
        toolbar.title = "${getString(R.string.app_name)} $versionName"
        activity.setSupportActionBar(toolbar)
    }

    private fun setupNavigation(rootView: View) {
        bottomNavigationView = rootView.findViewById(R.id.bottom_navigation)
        navigationRail = rootView.findViewById(R.id.navigation_rail)

        val listener = NavigationBarView.OnItemSelectedListener { item ->
            handleNavigationItemSelected(item.itemId)
        }
        bottomNavigationView?.setOnItemSelectedListener(listener)
        navigationRail?.setOnItemSelectedListener(listener)

        val navView: NavigationBarView? = bottomNavigationView ?: navigationRail
        val targetItemId = if (savedNavItemId != 0) savedNavItemId else R.id.nav_games
        navView?.selectedItemId = targetItemId
        savedNavItemId = 0

        // 画面回転復元時: ext_fragmentにフラグメントが存在する場合、
        // main_content_layoutを非表示にする。
        // nav_addが選択された状態（BottomSheet表示中）で回転した場合、
        // savedNavItemIdはnav_addだが、背後のext_fragmentのフラグメントは
        // FragmentManagerにより自動復元されるため、ナビアイテムIDではなく
        // ext_fragmentの実際の状態を確認する。
        val extFragment = requireActivity().supportFragmentManager.findFragmentById(R.id.ext_fragment)
        if (extFragment != null) {
            rootView.findViewById<View>(R.id.main_content_layout)?.visibility = View.GONE
            rootView.findViewById<View>(R.id.appbar)?.visibility = View.GONE
            // BackupManager表示中はナビゲーションを隠す（アニメーションなし）
            if (extFragment is org.uoyabause.android.backup.ui.BackupManagerFragment) {
                navigationRail?.visibility = View.GONE
            }
        }

        updateAccountIcon()
    }

    private var keyboardLayoutListener: ViewTreeObserver.OnGlobalLayoutListener? = null

    private fun setupKeyboardVisibilityListener() {
        val bnv = bottomNavigationView ?: return
        val contentView = requireActivity().findViewById<View>(android.R.id.content)
        val listener = ViewTreeObserver.OnGlobalLayoutListener {
            val rect = android.graphics.Rect()
            contentView.getWindowVisibleDisplayFrame(rect)
            val screenHeight = contentView.rootView.height
            val keypadHeight = screenHeight - rect.bottom
            // Keyboard is considered visible if it takes more than 15% of screen
            if (keypadHeight > screenHeight * 0.15) {
                bnv.visibility = View.GONE
            } else {
                bnv.visibility = View.VISIBLE
            }
        }
        keyboardLayoutListener = listener
        contentView.viewTreeObserver.addOnGlobalLayoutListener(listener)
    }

    private fun handleNavigationItemSelected(itemId: Int): Boolean {
        playNavigationSound()
        return when (itemId) {
            R.id.nav_games -> {
                showLibraryView()
                true
            }
            R.id.nav_backup -> {
                firebaseAnalytics?.logEvent(
                    "game_select_fragment",
                    Bundle().apply { putString("event", "menu_backup_manager") },
                )
                rootView.findViewById<View>(R.id.main_content_layout)?.visibility = View.GONE
                rootView.findViewById<View>(R.id.appbar)?.visibility = View.GONE
                hideNavigationRail {
                    val fragment = org.uoyabause.android.backup.ui.BackupManagerFragment
                        .newInstance()
                    requireActivity()
                        .supportFragmentManager
                        .beginTransaction()
                        .replace(R.id.ext_fragment, fragment)
                        .commit()
                }
                true
            }
            R.id.nav_add -> {
                firebaseAnalytics?.logEvent(
                    "game_select_fragment",
                    Bundle().apply { putString("event", "menu_item_load_game") },
                )
                showAddGameBottomSheet()
                true
            }
            R.id.nav_settings -> {
                firebaseAnalytics?.logEvent(
                    "game_select_fragment",
                    Bundle().apply { putString("event", "menu_item_setting") },
                )
                rootView.findViewById<View>(R.id.main_content_layout)?.visibility = View.GONE
                rootView.findViewById<View>(R.id.appbar)?.visibility = View.GONE
                val fragment = SettingsContainerFragment.newInstance()
                requireActivity()
                    .supportFragmentManager
                    .beginTransaction()
                    .replace(R.id.ext_fragment, fragment)
                    .commit()
                true
            }
            R.id.nav_account -> {
                firebaseAnalytics?.logEvent(
                    "game_select_fragment",
                    Bundle().apply { putString("event", "menu_item_login") },
                )
                rootView.findViewById<View>(R.id.main_content_layout)?.visibility = View.GONE
                rootView.findViewById<View>(R.id.appbar)?.visibility = View.GONE
                val fragment = org.uoyabause.android.auth.ui.AccountManagementFragment
                    .newInstance()
                requireActivity()
                    .supportFragmentManager
                    .beginTransaction()
                    .replace(R.id.ext_fragment, fragment)
                    .commit()
                true
            }
            else -> false
        }
    }

    private fun playNavigationSound() {
        try {
            val audioManager = requireContext().getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
            audioManager.playSoundEffect(android.media.AudioManager.FX_FOCUS_NAVIGATION_UP)
        } catch (_: Exception) {
        }
    }

    fun navigateToGames() {
        val navView: NavigationBarView? = bottomNavigationView ?: navigationRail
        navView?.selectedItemId = R.id.nav_games
    }

    private fun showLibraryView() {
        val fm = requireActivity().supportFragmentManager
        fm.popBackStack(null, androidx.fragment.app.FragmentManager.POP_BACK_STACK_INCLUSIVE)
        val extFragment = fm.findFragmentById(R.id.ext_fragment)
        if (extFragment != null) {
            fm.beginTransaction().remove(extFragment).commit()
        }
        rootView.findViewById<View>(R.id.appbar)?.visibility = View.VISIBLE
        rootView.findViewById<View>(R.id.main_content_layout)?.visibility = View.VISIBLE
        showNavigationRail()
    }

    private fun hideNavigationRail(onComplete: (() -> Unit)? = null) {
        val rail = navigationRail
        if (rail == null || rail.visibility == View.GONE) {
            onComplete?.invoke()
            return
        }
        val width = rail.width.toFloat().takeIf { it > 0f }
            ?: rail.measuredWidth.toFloat().takeIf { it > 0f }
            ?: run {
                onComplete?.invoke()
                return
            }
        rail
            .animate()
            .translationX(-width)
            .setDuration(250)
            .withEndAction {
                rail.visibility = View.GONE
                rail.translationX = 0f
                onComplete?.invoke()
            }.start()
    }

    private fun showNavigationRail() {
        val rail = navigationRail ?: return
        if (rail.visibility == View.VISIBLE) return
        rail.visibility = View.INVISIBLE
        rail.post {
            val width = rail.width.toFloat().takeIf { it > 0f }
                ?: rail.measuredWidth.toFloat().takeIf { it > 0f }
                ?: run {
                    rail.visibility = View.VISIBLE
                    return@post
                }
            rail.translationX = -width
            rail.visibility = View.VISIBLE
            rail
                .animate()
                .translationX(0f)
                .setDuration(250)
                .start()
        }
    }

    private fun updateAccountIcon() {
        val context = context ?: return
        val user = FirebaseAuth.getInstance().currentUser
        val photoUrl = user?.photoUrl

        if (user == null || photoUrl == null) {
            val defaultIcon = ContextCompat.getDrawable(context, R.drawable.ic_baseline_person_24px)
            bottomNavigationView?.menu?.findItem(R.id.nav_account)?.icon = defaultIcon
            navigationRail?.menu?.findItem(R.id.nav_account)?.icon = defaultIcon
            return
        }

        Glide
            .with(context)
            .asBitmap()
            .load(photoUrl)
            .apply(RequestOptions.circleCropTransform())
            .into(object : com.bumptech.glide.request.target.CustomTarget<android.graphics.Bitmap>() {
                override fun onResourceReady(
                    resource: android.graphics.Bitmap,
                    transition: com.bumptech.glide.request.transition.Transition<in android.graphics.Bitmap>?,
                ) {
                    if (!isAdded) return
                    val wrapped = android.graphics.drawable.BitmapDrawable(resources, resource)
                    val icon = object : android.graphics.drawable.Drawable() {
                        override fun draw(canvas: android.graphics.Canvas) = wrapped.draw(canvas)

                        override fun getIntrinsicWidth(): Int = wrapped.intrinsicWidth

                        override fun getIntrinsicHeight(): Int = wrapped.intrinsicHeight

                        override fun setAlpha(alpha: Int) {
                            wrapped.alpha = alpha
                        }

                        override fun setColorFilter(colorFilter: android.graphics.ColorFilter?) {}

                        @Deprecated("Deprecated in Java")
                        override fun getOpacity(): Int = wrapped.opacity

                        override fun getConstantState(): ConstantState? = null

                        override fun mutate(): android.graphics.drawable.Drawable = this

                        override fun setTintList(tint: android.content.res.ColorStateList?) {}

                        override fun setTint(tintColor: Int) {}

                        override fun setBounds(left: Int, top: Int, right: Int, bottom: Int) {
                            super.setBounds(left, top, right, bottom)
                            wrapped.setBounds(left, top, right, bottom)
                        }
                    }
                    bottomNavigationView?.menu?.findItem(R.id.nav_account)?.icon = icon
                    navigationRail?.menu?.findItem(R.id.nav_account)?.icon = icon
                }

                override fun onLoadCleared(placeholder: android.graphics.drawable.Drawable?) {}
            })
    }

    private fun updateDetailPanel(game: GameInfo?) {
        val detailContent = rootView.findViewById<View>(R.id.detail_content) ?: return
        val placeholderContainer = rootView.findViewById<View>(R.id.detail_placeholder_container) ?: return

        if (game == null) {
            placeholderContainer.visibility = View.VISIBLE
            detailContent.visibility = View.GONE
            return
        }

        placeholderContainer.visibility = View.GONE
        detailContent.visibility = View.VISIBLE

        // Hide drag handle in landscape inline panel
        detailContent.findViewById<View>(R.id.drag_handle)?.visibility = View.GONE

        val tvTitle = detailContent.findViewById<TextView>(R.id.tv_detail_title)
        val tvProduct = detailContent.findViewById<TextView>(R.id.tv_detail_product)
        val tvInfo = detailContent.findViewById<TextView>(R.id.tv_detail_info)
        val imgBoxart = detailContent.findViewById<ImageView>(R.id.img_detail_boxart)
        val ratingBar = detailContent.findViewById<android.widget.RatingBar>(R.id.rating_detail)
        val btnPlay = detailContent.findViewById<com.google.android.material.button.MaterialButton>(R.id.btn_play)
        val btnDelete = detailContent.findViewById<com.google.android.material.button.MaterialButton>(R.id.btn_delete)
        val btnShortcut = detailContent.findViewById<com.google.android.material.button.MaterialButton>(R.id.btn_shortcut)
        val btnOverflow = detailContent.findViewById<com.google.android.material.button.MaterialButton>(R.id.btn_overflow_menu)

        tvTitle?.text = game.game_title
        tvProduct?.text = game.product_number
        ratingBar?.rating = game.rating.toFloat()

        val infoText = buildString {
            if (game.device_infomation != "CD-1/1") append(game.device_infomation)
            if (game.area.isNotBlank()) {
                if (isNotEmpty()) append(" | ")
                append(game.area)
            }
            if (game.release_date.isNotBlank()) {
                if (isNotEmpty()) append(" | ")
                append(game.release_date)
            }
        }
        tvInfo?.text = infoText

        // Limit boxart height to 40% of screen to prevent tall cover art from pushing content off-screen
        imgBoxart?.let {
            val displayMetrics = resources.displayMetrics
            it.maxHeight = (displayMetrics.heightPixels * 0.55).toInt()
            it.adjustViewBounds = true
        }

        // Load image
        if (!game.image_url.isNullOrEmpty() && game.image_url!!.startsWith("http")) {
            Glide
                .with(this)
                .load(game.image_url)
                .error(R.drawable.missing)
                .into(imgBoxart!!)
        } else {
            imgBoxart?.setImageResource(R.drawable.missing)
        }

        btnPlay?.setOnClickListener { onPlayGame(game) }
        btnDelete?.setOnClickListener { viewModel.requestDeleteGame(game) }
        btnShortcut?.setOnClickListener { onCreateShortcut(game) }
        btnOverflow?.setOnClickListener { showOverflowMenu(it, game) }

        // M3 Expressive: pulse the play button to draw attention
        btnPlay?.let { startPlayButtonPulse(it) }

        if (game.isCloudOnly) {
            btnDelete?.visibility = View.GONE
            btnShortcut?.visibility = View.GONE
        } else {
            btnDelete?.visibility = View.VISIBLE
            btnShortcut?.visibility = View.VISIBLE
        }

        // Setup focus navigation: RecyclerView items → Play button (D-pad right)
        setupDetailPanelFocusNavigation(btnPlay)

        // Bind RetroAchievements data
        bindRaDataToPanel(detailContent, game)
    }

    private fun bindRaDataToPanel(
        view: View,
        game: GameInfo,
    ) {
        val raSection = view.findViewById<View>(R.id.ra_section) ?: return
        val tvAchievements = view.findViewById<TextView>(R.id.tv_ra_achievements) ?: return
        val raProgressBar =
            view.findViewById<com.google.android.material.progressindicator.LinearProgressIndicator>(R.id.ra_progress_bar)
                ?: return
        val raHardcoreSection = view.findViewById<View>(R.id.ra_hardcore_section)
        val tvHardcore = view.findViewById<TextView>(R.id.tv_ra_hardcore)
        val raProgressBarHardcore =
            view.findViewById<com.google.android.material.progressindicator.LinearProgressIndicator>(R.id.ra_progress_bar_hardcore)
        val raLeaderboardSection = view.findViewById<View>(R.id.ra_leaderboard_section) ?: return
        val raLeaderboardEntries =
            view.findViewById<android.widget.LinearLayout>(R.id.ra_leaderboard_entries) ?: return

        if (game.raGameId == null || game.raNotSupported || game.raTotal == 0) {
            raSection.visibility = View.GONE
            return
        }

        raSection.visibility = View.VISIBLE

        // Softcore row
        tvAchievements.text = "${getString(R.string.softcore)}: ${game.raUnlocked} / ${game.raTotal}"
        val progressPct = if (game.raTotal > 0) (game.raUnlocked * 100) / game.raTotal else 0
        raProgressBar.progress = progressPct

        // Hardcore row (only shown when user has hardcore unlocks)
        if (game.raUnlockedHardcore > 0) {
            raHardcoreSection?.visibility = View.VISIBLE
            tvHardcore?.text = "${getString(R.string.hardcore)}: ${game.raUnlockedHardcore} / ${game.raTotal}"
            val hardcorePct = if (game.raTotal > 0) (game.raUnlockedHardcore * 100) / game.raTotal else 0
            raProgressBarHardcore?.progress = hardcorePct
        } else {
            raHardcoreSection?.visibility = View.GONE
        }

        // Cancel previous fetch and clear entries before starting a new one
        leaderboardFetchJob?.cancel()
        raLeaderboardEntries.removeAllViews()
        raLeaderboardSection.visibility = View.GONE

        val raGameId = game.raGameId ?: return
        leaderboardFetchJob = lifecycleScope.launch(Dispatchers.IO) {
            val manager = RetroAchievementsManager.getInstance(requireContext())
            val leaderboards = manager.fetchGameLeaderboards(raGameId)
            withContext(Dispatchers.Main) {
                if (isAdded && leaderboards != null && leaderboards.isNotEmpty()) {
                    raLeaderboardSection.visibility = View.VISIBLE
                    leaderboards.forEach { entry ->
                        val entryView =
                            TextView(requireContext()).apply {
                                text = entry
                                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodySmall)
                                setTextColor(requireContext().getColor(android.R.color.tab_indicator_text))
                                setPadding(0, 4, 0, 4)
                            }
                        raLeaderboardEntries.addView(entryView)
                    }
                }
            }
        }
    }

    private fun setupDetailPanelFocusNavigation(btnPlay: com.google.android.material.button.MaterialButton?) {
        // Store btnPlay reference for adapter to use
        currentDetailPanelButton = btnPlay
    }

    private var currentDetailPanelButton: com.google.android.material.button.MaterialButton? = null
    private var leaderboardFetchJob: kotlinx.coroutines.Job? = null

    // Called from GameCardAdapter when D-pad right is pressed
    fun moveToDetailPanelButton(): Boolean = currentDetailPanelButton?.let {
        it.requestFocus()
        true
    } ?: false

    // GameCardAdapter.OnItemClickListener
    override fun onItemClick(position: Int, item: GameInfo) {
        // A locked game answers with the subscription gate. The card keeps the
        // click rather than ignoring it: a row that looks present and does
        // nothing at all says less than one that says why.
        if (!viewModel.allowOpen(item)) return
        viewModel.selectGame(item)

        if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
            updateDetailPanel(item)
        } else {
            val sheet = GameDetailBottomSheet()
            sheet.setGameInfo(item)
            sheet.setListener(this)
            sheet.show(childFragmentManager, GameDetailBottomSheet.TAG)
        }
    }

    override fun onItemLongClick(position: Int, item: GameInfo, anchor: View) {
        showGamePopupMenu(anchor, item)
    }

    override fun onItemFocused(position: Int, item: GameInfo) {
        // Focus alone opens no gate: the D-pad ring still travels over locked
        // cards, and moving onto one is not a request to play it.
        viewModel.selectGame(item)
        if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
            updateDetailPanel(item)
        }
    }

    override fun onItemPlayGame(position: Int, item: GameInfo) {
        // Start game directly (e.g., when pressing A button or Enter key)
        viewModel.startGame(item, yabauseActivityLauncher)
    }

    override fun onItemDPadRight(): Boolean {
        // Move focus to detail panel button (landscape mode)
        return moveToDetailPanelButton()
    }

    // GameDetailBottomSheet.Listener
    override fun onPlayGame(game: GameInfo) {
        viewModel.startGame(game, yabauseActivityLauncher)
    }

    override fun onDeleteGame(game: GameInfo) {
        viewModel.confirmDeleteGame(game)
    }

    override fun onCreateShortcut(game: GameInfo) {
        createHomeScreenShortcut(game)
    }

    // AddGameBottomSheetFragment.Listener
    override fun onFileSelected(uri: Uri) {
        viewModel.onFileSelected(uri)
    }

    override fun onFolderAdded(path: String) {
        viewModel.updateGameDatabase(refreshLevel = 3)
    }

    override fun onFolderRemoved(path: String) {
        viewModel.updateGameDatabase(refreshLevel = 3)
    }

    override fun onScanFolder(path: String) {
        viewModel.updateGameDatabase(refreshLevel = 3)
    }

    override fun onScanAllFolders() {
        viewModel.updateGameDatabase(refreshLevel = 3)
    }

    override fun getImportPresenter(): GameSelectPresenter? = viewModel.presenter

    override fun onGamesInstalled() {
        viewModel.updateGameDatabase(refreshLevel = 3)
    }

    // GameSelectPresenterListener
    override fun onShowMessage(string_id: Int) {
        Snackbar.make(rootView, string_id, Snackbar.LENGTH_SHORT).show()
    }

    override fun onShowDialog(message: String) {
        // Use a simple Snackbar for now
    }

    override fun onUpdateDialogMessage(message: String) {
        // Progress feedback
    }

    override fun onDismissDialog() {
        // Dismiss progress
    }

    override fun onLoadRows() {
        viewModel.loadGames()
    }

    override fun onSignOut() {
        // Handle sign out
    }

    // D-pad support
    fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean = handleDPadNavigation(keyCode)

    fun onKeyMultiple(keyCode: Int, repeatCount: Int, event: KeyEvent?): Boolean = handleDPadNavigation(keyCode)

    private fun isNavViewFocused(): Boolean = bottomNavigationView?.hasFocus() == true || navigationRail?.hasFocus() == true

    private fun isDetailPanelFocused(): Boolean {
        val detailPanel = rootView.findViewById<View>(R.id.detail_panel) ?: return false
        return detailPanel.hasFocus()
    }

    private fun handleDPadNavigation(keyCode: Int): Boolean {
        if (!::gameAdapter.isInitialized || !::recyclerView.isInitialized) return false
        if (isNavViewFocused()) return false

        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> {
                if (isDetailPanelFocused()) return false
                val navView: View? = bottomNavigationView ?: navigationRail
                navView?.requestFocus()
                return true
            }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> {
                val focusedView = recyclerView.findFocus()
                val viewHolder = focusedView?.let {
                    recyclerView.findContainingViewHolder(it)
                }
                val pos = viewHolder?.bindingAdapterPosition ?: return false
                if (pos in 0 until gameAdapter.itemCount) {
                    onPlayGame(gameAdapter.currentList[pos])
                }
                return true
            }
            else -> return false
        }
    }

    fun onAdViewIsShown(height: Int) {
        try {
            val parentLayout = rootView.findViewById<View>(R.id.parent)
            val param = parentLayout.layoutParams as ViewGroup.MarginLayoutParams
            param.bottomMargin = height + 4
            parentLayout.layoutParams = param
        } catch (_: Exception) {
        }
    }

    override fun onResume() {
        super.onResume()
        viewModel.presenter?.onResume()
        updateAccountIcon()
    }

    override fun onPause() {
        super.onPause()
        viewModel.presenter?.onPause()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        val navView: NavigationBarView? = bottomNavigationView ?: navigationRail
        navView?.let {
            outState.putInt(KEY_SELECTED_NAV_ITEM, it.selectedItemId)
        }
    }

    private var emptyStateAnimator: ObjectAnimator? = null

    private fun startEmptyStateAnimation() {
        val icon = (layoutEmpty as? ViewGroup)?.getChildAt(0) as? ImageView
            ?: return
        emptyStateAnimator?.cancel()
        val density = resources.displayMetrics.density
        emptyStateAnimator = ObjectAnimator
            .ofPropertyValuesHolder(
                icon,
                PropertyValuesHolder.ofFloat(View.TRANSLATION_Y, 0f, -8f * density, 0f),
            ).apply {
                duration = 2000
                repeatCount = ObjectAnimator.INFINITE
                interpolator = android.view.animation.AccelerateDecelerateInterpolator()
                start()
            }
    }

    private fun stopEmptyStateAnimation() {
        emptyStateAnimator?.cancel()
        emptyStateAnimator = null
    }

    private fun startPlayButtonPulse(btnPlay: View) {
        btnPlay.postDelayed({
            if (!isAdded) return@postDelayed
            btnPlay
                .animate()
                .scaleX(1.03f)
                .scaleY(1.03f)
                .setDuration(400)
                .withEndAction {
                    btnPlay
                        .animate()
                        .scaleX(1.0f)
                        .scaleY(1.0f)
                        .setDuration(400)
                        .start()
                }.start()
        }, 1000)
    }

    // --- Private helpers ---

    private fun showSearchBar() {
        searchBarContainer.visibility = View.VISIBLE
        searchBarBackCallback.isEnabled = true
        searchView.requestFocus()
        val imm = requireContext().getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(searchView, InputMethodManager.SHOW_IMPLICIT)
    }

    private fun hideSearchBar() {
        searchBarContainer.visibility = View.GONE
        searchBarBackCallback.isEnabled = false
        searchView.setQuery("", false)
        viewModel.setSearchQuery("")
        val imm = requireContext().getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(searchView.windowToken, 0)
    }

    private fun showSortMenu(anchor: View) {
        val popup = PopupMenu(requireContext(), anchor)
        popup.menuInflater.inflate(R.menu.sort_menu, popup.menu)

        // Check current sort mode
        val currentMode = viewModel.screenState.value.sortMode
        when (currentMode) {
            GameSortMode.NAME -> popup.menu.findItem(R.id.sort_by_name)?.isChecked = true
            GameSortMode.DATE -> popup.menu.findItem(R.id.sort_by_date)?.isChecked = true
            GameSortMode.RECENTLY_PLAYED -> popup.menu.findItem(R.id.sort_by_recently_played)?.isChecked = true
        }

        popup.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.sort_by_name -> {
                    viewModel.setSortMode(GameSortMode.NAME)
                    true
                }
                R.id.sort_by_date -> {
                    viewModel.setSortMode(GameSortMode.DATE)
                    true
                }
                R.id.sort_by_recently_played -> {
                    viewModel.setSortMode(GameSortMode.RECENTLY_PLAYED)
                    true
                }
                else -> false
            }
        }
        popup.show()
    }

    private fun showGamePopupMenu(anchor: View, game: GameInfo) {
        val popup = PopupMenu(requireContext(), anchor)
        if (game.isCloudOnly) {
            popup.menuInflater.inflate(R.menu.cloud_game_item_popup_menu, popup.menu)
        } else {
            popup.menuInflater.inflate(R.menu.game_item_popup_menu, popup.menu)
        }

        popup.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.download_from_cloud -> {
                    if (game.isCloudOnly && game.cloudBackupInfo != null) {
                        onPlayGame(game)
                    }
                    true
                }
                R.id.create_shortcut -> {
                    createHomeScreenShortcut(game)
                    true
                }
                R.id.delete -> {
                    viewModel.requestDeleteGame(game)
                    true
                }
                R.id.detail -> {
                    openGameDetail(game)
                    true
                }
                R.id.report -> {
                    val reportDialog = org.uoyabause.android.ReportDialog(requireContext(), game.product_number)
                    reportDialog.show(requireActivity().supportFragmentManager, "ReportDialog")
                    true
                }
                R.id.restore_defaults -> {
                    val gamePreference = requireContext()
                        .getSharedPreferences(game.product_number, Context.MODE_PRIVATE)
                    gamePreference.edit().clear().apply()
                    true
                }
                else -> false
            }
        }
        popup.show()
    }

    private fun openGameDetail(game: GameInfo) {
        val db = com.google.firebase.firestore.FirebaseFirestore
            .getInstance()
        db
            .collection("games")
            .whereEqualTo("product_number", game.product_number)
            .get()
            .addOnSuccessListener { documents ->
                if (!documents.isEmpty) {
                    val gameDoc = documents.documents[0]
                    val url = "https://www.yabasanshiro.com/games/${gameDoc.id}"
                    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                } else {
                    Toast.makeText(requireContext(), "Game not found in database", Toast.LENGTH_SHORT).show()
                }
            }.addOnFailureListener { e ->
                Toast.makeText(requireContext(), "Error: ${e.message}", Toast.LENGTH_SHORT).show()
            }
    }

    private fun showAddGameBottomSheet() {
        if (childFragmentManager.findFragmentByTag(AddGameBottomSheetFragment.TAG) != null) return
        val bottomSheet = AddGameBottomSheetFragment.newInstance()
        bottomSheet.setListener(this)
        bottomSheet.show(childFragmentManager, AddGameBottomSheetFragment.TAG)
    }

    private fun createHomeScreenShortcut(game: GameInfo) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            Toast.makeText(requireContext(), R.string.shortcut_not_supported, Toast.LENGTH_SHORT).show()
            return
        }

        val shortcutManager = requireContext().getSystemService(ShortcutManager::class.java)
        if (shortcutManager == null || !shortcutManager.isRequestPinShortcutSupported) {
            Toast.makeText(requireContext(), R.string.shortcut_not_supported, Toast.LENGTH_SHORT).show()
            return
        }

        val intent = Intent(requireContext(), org.uoyabause.android.Yabause::class.java).apply {
            action = Intent.ACTION_VIEW
            if (game.file_path.contains("content://")) {
                putExtra("org.uoyabause.android.FileNameUri", game.file_path)
                putExtra("org.uoyabause.android.FileDir", game.iso_file_path)
            } else {
                putExtra("org.uoyabause.android.FileNameEx", game.file_path)
            }
            putExtra("org.uoyabause.android.gamecode", game.product_number)
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK
        }

        lifecycleScope.launch(Dispatchers.IO) {
            val iconBitmap =
                try {
                    if (!game.image_url.isNullOrEmpty()) {
                        Glide
                            .with(requireContext())
                            .asBitmap()
                            .load(game.image_url)
                            .submit(512, 512)
                            .get()
                    } else {
                        null
                    }
                } catch (e: Exception) {
                    Log.e("GameSelectFragment", "Failed to load boxart for shortcut", e)
                    null
                }

            withContext(Dispatchers.Main) {
                if (!isAdded) return@withContext

                val icon =
                    if (iconBitmap != null) {
                        Icon.createWithAdaptiveBitmap(iconBitmap)
                    } else {
                        Icon.createWithResource(requireContext(), R.mipmap.ic_launcher)
                    }

                val shortcutInfo =
                    ShortcutInfo
                        .Builder(requireContext(), "game_${game.product_number}")
                        .setShortLabel(game.game_title)
                        .setLongLabel(game.game_title)
                        .setIntent(intent)
                        .setIcon(icon)
                        .build()

                shortcutManager.requestPinShortcut(shortcutInfo, null)
                Toast.makeText(requireContext(), R.string.shortcut_created, Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun showOverflowMenu(view: View, game: GameInfo) {
        val options = arrayOf(
            getString(R.string.restore_defaults),
            getString(R.string.reproduce_report),
        )

        val listPopupWindow = ListPopupWindow(requireContext())
        listPopupWindow.anchorView = view

        val adapter = ArrayAdapter(
            requireContext(),
            android.R.layout.simple_list_item_1,
            options,
        )
        listPopupWindow.setAdapter(adapter)

        listPopupWindow.setOnItemClickListener { _, _, position, _ ->
            when (position) {
                0 -> handleRestoreDefaults(game)
                1 -> handleReproduceReport(game)
            }
            listPopupWindow.dismiss()
        }

        listPopupWindow.width = 400
        listPopupWindow.isModal = true

        try {
            listPopupWindow.show()
        } catch (e: Exception) {
            Toast.makeText(requireContext(), "Error: ${e.message}", Toast.LENGTH_SHORT).show()
        }
    }

    private fun handleRestoreDefaults(game: GameInfo) {
        val dialog = MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.restore_defaults)
            .setMessage(R.string.restore_defaults_confirm)
            .setPositiveButton(R.string.ok) { _, _ ->
                val gamePreference = requireContext()
                    .getHarmonySharedPreferences(game.product_number)
                gamePreference.edit().clear().apply()

                Toast
                    .makeText(
                        requireContext(),
                        R.string.restore_defaults_success,
                        Toast.LENGTH_SHORT,
                    ).show()
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

    private fun handleReproduceReport(game: GameInfo) {
        val auth = FirebaseAuth.getInstance()
        if (auth.currentUser == null) {
            Toast
                .makeText(
                    requireContext(),
                    "Please sign in to access reports",
                    Toast.LENGTH_SHORT,
                ).show()
            return
        }

        lifecycleScope.launch {
            try {
                val isAdmin = checkIsAdmin(auth.currentUser!!.uid)

                if (isAdmin) {
                    val intent = Intent(requireContext(), org.uoyabause.android.ReportListActivity::class.java).apply {
                        putExtra(org.uoyabause.android.ReportListActivity.EXTRA_PRODUCT_NUMBER, game.product_number)
                        putExtra(org.uoyabause.android.ReportListActivity.EXTRA_GAME_TITLE, game.game_title)
                        putExtra(org.uoyabause.android.ReportListActivity.EXTRA_FILE_PATH, game.file_path)
                        putExtra(org.uoyabause.android.ReportListActivity.EXTRA_ISO_FILE_PATH, game.iso_file_path)
                    }
                    startActivity(intent)
                } else {
                    Toast
                        .makeText(
                            requireContext(),
                            "This feature is only available for administrators",
                            Toast.LENGTH_LONG,
                        ).show()
                }
            } catch (e: Exception) {
                Toast
                    .makeText(
                        requireContext(),
                        "Error: ${e.message}",
                        Toast.LENGTH_SHORT,
                    ).show()
            }
        }
    }

    private suspend fun checkIsAdmin(userId: String): Boolean = try {
        val db = FirebaseFirestore.getInstance()
        val adminDoc = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            db
                .collection("admins")
                .document(userId)
                .get()
                .await()
        }
        adminDoc.exists()
    } catch (e: Exception) {
        false
    }

    /**
     * Update RetroAchievements progress for all games in background
     */
    private fun updateRetroAchievementsProgress(games: List<GameInfo>) {
        val raManager = org.uoyabause.android.achievements.RetroAchievementsManager
            .getInstance(requireContext())

        if (!raManager.isUserLoggedIn()) return

        lifecycleScope.launch(kotlinx.coroutines.Dispatchers.IO) {
            try {
                raManager.updateProgressIfNeeded(games) { current, total ->
                    // 進捗コールバック中はUI更新しない（フォーカスリセット防止）
                }
                // 全件完了後に1回だけリスト更新（DiffUtilで差分のみ）
                launch(kotlinx.coroutines.Dispatchers.Main) {
                    gameAdapter.submitList(games.toList())
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error updating RA progress", e)
            }
        }
    }

    /**
     * Launch a game from Android TV deep link.
     */
    fun launchGameFromDeepLink(game: GameInfo) {
        viewModel.startGame(game, yabauseActivityLauncher)
    }

    companion object {
        private const val TAG = "GameSelectFragPhone2"
        private const val KEY_SELECTED_NAV_ITEM = "selected_nav_item_id"

        fun getVersionName(context: Context): String = try {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: ""
        } catch (e: PackageManager.NameNotFoundException) {
            ""
        }
    }
}
