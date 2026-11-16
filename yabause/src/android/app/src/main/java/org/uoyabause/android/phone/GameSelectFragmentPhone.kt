@file:OptIn(kotlinx.coroutines.DelicateCoroutinesApi::class)

/*  Copyright 2019 devMiyax(smiyaxdev@gmail.com)

    This file is part of YabaSanshiro.

    YabaSanshiro is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    YabaSanshiro is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with YabaSanshiro; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

package org.uoyabause.android.phone

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.net.Uri
import android.os.Build
import android.os.Build.VERSION_CODES
import android.os.Bundle
import android.provider.DocumentsContract
import android.util.Log
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.View.VISIBLE
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.PopupMenu
import androidx.appcompat.widget.SearchView
import androidx.appcompat.widget.Toolbar
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import androidx.fragment.app.viewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ViewModelProvider.NewInstanceFactory.Companion.instance
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.preference.PreferenceManager
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.bumptech.glide.Glide
import com.bumptech.glide.request.RequestOptions
import com.google.android.gms.analytics.HitBuilders.ScreenViewBuilder
import com.google.android.gms.analytics.Tracker
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.navigation.NavigationBarView
import com.google.android.material.navigationrail.NavigationRailView
import com.google.android.material.snackbar.Snackbar
import com.google.android.play.core.review.ReviewManager
import com.google.android.play.core.review.ReviewManagerFactory
import com.google.android.play.core.review.testing.FakeReviewManager
import com.google.firebase.analytics.FirebaseAnalytics
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.remoteconfig.FirebaseRemoteConfig
import com.google.firebase.remoteconfig.FirebaseRemoteConfigSettings
import io.noties.markwon.Markwon
import jp.wasabeef.glide.transformations.BlurTransformation
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.invoke
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.devmiyax.yabasanshiro.BuildConfig
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.BillingViewModel
import org.uoyabause.android.CloudGameInfo
import org.uoyabause.android.FileDialog
import org.uoyabause.android.FileDialog.FileSelectedListener
import org.uoyabause.android.GameInfo
import org.uoyabause.android.GameSelectPresenter
import org.uoyabause.android.GameSelectPresenter.GameSelectPresenterListener
import org.uoyabause.android.SettingsContainerFragment
import org.uoyabause.android.ShowPinInFragment
import org.uoyabause.android.StorageMigrationHelper
import org.uoyabause.android.YabauseApplication
import org.uoyabause.android.YabauseStorage
import org.uoyabause.android.YabauseStorage.Companion.dao
import org.uoyabause.android.backup.GameBackupManager
import org.uoyabause.android.game.AddGameBottomSheetFragment
import java.io.File

private const val PREFS_NAME = "game_select_prefs"
private const val KEY_SORT_MODE = "sort_mode"
private const val KEY_SELECTED_NAV_ITEM = "selected_nav_item_id"

class GameSelectFragmentPhone :
    Fragment(),
    GameItemAdapter.OnItemClickListener,
    FileSelectedListener,
    GameSelectPresenterListener,
    AddGameBottomSheetFragment.Listener {
    lateinit var presenter: GameSelectPresenter
    private var updateJob: Job? = null
    private var tracker: Tracker? = null
    private var firebaseAnalytics: FirebaseAnalytics? = null
    private var isFirstUpdate = true
    private var bottomNavigationView: BottomNavigationView? = null
    private var navigationRail: NavigationRailView? = null
    private lateinit var rootView: View
    private lateinit var recyclerView: RecyclerView
    private lateinit var searchView: SearchView
    private lateinit var sortButton: ImageButton
    private lateinit var gameAdapter: GameItemAdapter
    private lateinit var progressBar: View
    private lateinit var progressMessage: TextView
    private lateinit var boxartImage: ImageView

    // private lateinit var gameInfoOverlay: LinearLayout
    private lateinit var selectedGameTitle: TextView
    private lateinit var selectedGameInfo: TextView

    // private lateinit var selectedGameIcon: ImageView
    private lateinit var selectedGameVersion: TextView
    private lateinit var selectedGameMenu: ImageButton
    private var isBackGroundComplete = false
    private var isAutoSelecting = false // 自動選択中フラグ
    private var lastScrollTime = 0L // スクロール更新の間引き用
    private var isDPadNavigating = false // D-pad navigation mode flag
    private var lastInputSource = 0 // Track last input source to distinguish touch vs D-pad
    private var isManuallySelected = false // 手動/削除後選択フラグ
    private var currentSortMode = SortMode.NAME // 現在のソート方法を追跡
    private var savedNavItemId: Int = 0

    private var isBillingConnected = false
    private val viewModel by viewModels<BillingViewModel>()
    val connectionObserver =
        androidx.lifecycle.Observer<Boolean> { isConnecteed ->
            Log.d(TAG, "isConnected $isConnecteed")
            isBillingConnected = isConnecteed
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        instance = this
        presenter = GameSelectPresenter(this as Fragment, yabauseActivityLauncher, this)

        val remoteConfig = FirebaseRemoteConfig.getInstance()
        val configSettings =
            FirebaseRemoteConfigSettings
                .Builder()
                .setMinimumFetchIntervalInSeconds(3600)
                .build()
        remoteConfig.setConfigSettingsAsync(configSettings)
        remoteConfig.setDefaultsAsync(R.xml.config)

        // Debug builds always enable subscription checking for testing
        val isSubscriptionEnabled = remoteConfig.getBoolean("is_enable_subscription_new") ||
            BuildConfig.DEBUG
        if (!isSubscriptionEnabled) {
            presenter.isOnSubscription = true
            YabauseApplication.isSubscriptionDisabledByRemoteConfig = true
        } else {
            presenter.isOnSubscription = false
            viewModel.billingConnectionState.observe(this, connectionObserver)
            // Billing flow moved to onViewCreated to avoid viewLifecycleOwner crash
        }
    }

    private var readRequestLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                if (result.data != null) {
                    val uri = result.data!!.data
                    if (uri != null) {
                        presenter.onSelectFile(uri)
                    }
                }
            }
        }

    // Permission request launcher for SAF write permissions
    private var permissionRequestLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                val uri = result.data?.data
                if (uri != null) {
                    // Take persistable permission
                    try {
                        val takeFlags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                        requireContext().contentResolver.takePersistableUriPermission(uri, takeFlags)
                        Log.d(TAG, "Successfully obtained write permission for URI: $uri")

                        // Notify pending deletion callback
                        pendingDeletionCallback?.invoke(true)
                        pendingDeletionCallback = null
                    } catch (e: Exception) {
                        Log.e(TAG, "Failed to take persistable permission: ${e.message}")
                        pendingDeletionCallback?.invoke(false)
                        pendingDeletionCallback = null
                    }
                } else {
                    Log.w(TAG, "No URI returned from permission request")
                    pendingDeletionCallback?.invoke(false)
                    pendingDeletionCallback = null
                }
            } else {
                Log.w(TAG, "Permission request cancelled or failed")
                pendingDeletionCallback?.invoke(false)
                pendingDeletionCallback = null
            }
        }

    // Callback for pending deletion operations
    private var pendingDeletionCallback: ((Boolean) -> Unit)? = null

    /**
     * Request write permission for SAF URI
     */
    fun requestWritePermission(
        uri: Uri,
        callback: (Boolean) -> Unit,
    ) {
        pendingDeletionCallback = callback

        // Show dialog to explain why permission is needed
        AlertDialog
            .Builder(requireContext())
            .setTitle(getString(R.string.permission_required))
            .setMessage(getString(R.string.write_permission_explanation))
            .setPositiveButton(R.string.ok) { _, _ ->
                // Launch document tree picker to get write permission
                val intent =
                    Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
                        flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                        // Try to start with the same directory if possible
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                            putExtra(DocumentsContract.EXTRA_INITIAL_URI, uri)
                        }
                    }
                permissionRequestLauncher.launch(intent)
            }.setNegativeButton(R.string.cancel) { _, _ ->
                callback(false)
                pendingDeletionCallback = null
            }.show()
    }

    private fun selectGameFile() {
        if (presenter.isGameLimitReached()) {
            YabauseApplication.checkDonated(requireActivity())
            return
        }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
        intent.addCategory(Intent.CATEGORY_OPENABLE)
        intent.type = "*/*"
        readRequestLauncher.launch(intent)
    }

    private fun showAddGameBottomSheet() {
        if (childFragmentManager.findFragmentByTag(AddGameBottomSheetFragment.TAG) != null) return
        val bottomSheet = AddGameBottomSheetFragment.newInstance()
        bottomSheet.setListener(this)
        bottomSheet.show(childFragmentManager, AddGameBottomSheetFragment.TAG)
    }

    // AddGameBottomSheetFragment.Listener implementation

    override fun onFileSelected(uri: Uri) {
        presenter.onSelectFile(uri)
    }

    override fun onFolderAdded(path: String) {
        updateGameList(3)
    }

    override fun onFolderRemoved(path: String) {
        // Folder removed from preferences; refresh game list
        updateGameList(3)
    }

    override fun onScanFolder(path: String) {
        updateGameList(3)
    }

    override fun onScanAllFolders() {
        updateGameList(3)
    }

    override fun getImportPresenter(): GameSelectPresenter? = presenter

    override fun onGamesInstalled() {
        updateGameList(3)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View {
        rootView = inflater.inflate(org.devmiyax.yabasanshiro.R.layout.fragment_game_select_fragment_phone, container, false)
        progressBar = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.llProgressBar)
        progressBar.visibility = View.GONE
        progressMessage = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.pbText)

        // RecyclerViewの設定
        recyclerView = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.recycler_view_games)
        recyclerView.layoutManager = LinearLayoutManager(context)

        // RecyclerViewのフォーカス設定を調整
        // キーイベントの重複を防ぐため、RecyclerViewのフォーカスを無効化
        recyclerView.isFocusable = false
        recyclerView.isFocusableInTouchMode = false
        recyclerView.descendantFocusability = ViewGroup.FOCUS_BLOCK_DESCENDANTS

        // タッチリスナーを追加してD-pad navigation modeをリセット
        recyclerView.setOnTouchListener { _, event ->
            if (isDPadNavigating && event.action == android.view.MotionEvent.ACTION_DOWN) {
                Log.d(TAG, "Touch detected, switching from D-pad to touch navigation mode")
                isDPadNavigating = false
            }
            // タッチ操作時に手動選択フラグをリセット（スクロール操作による自動選択を再開）
            if (event.action == android.view.MotionEvent.ACTION_DOWN) {
                isManuallySelected = false
            }
            false // イベントを消費しない
        }

        // スクロールリスナーを追加（自動選択機能）
        recyclerView.addOnScrollListener(
            object : RecyclerView.OnScrollListener() {
                override fun onScrollStateChanged(
                    recyclerView: RecyclerView,
                    newState: Int,
                ) {
                    super.onScrollStateChanged(recyclerView, newState)
                    // スクロールが停止したときに一番上のアイテムを選択（D-pad navigation中は無効）
                    if (newState == RecyclerView.SCROLL_STATE_IDLE && !isDPadNavigating) {
                        Log.d(TAG, "Touch scroll idle - selecting top visible item")
                        selectTopVisibleItem()
                    } else if (newState == RecyclerView.SCROLL_STATE_IDLE && isDPadNavigating) {
                        Log.d(TAG, "D-pad scroll idle - maintaining D-pad selection")
                    }
                }

                override fun onScrolled(
                    recyclerView: RecyclerView,
                    dx: Int,
                    dy: Int,
                ) {
                    super.onScrolled(recyclerView, dx, dy)
                    // スクロール中も一番上のアイテムを更新（パフォーマンスを考慮して間引き）
                    // D-pad navigation中は無効
                    val currentTime = System.currentTimeMillis()
                    if (!isAutoSelecting && !isDPadNavigating && currentTime - lastScrollTime > 100) { // 100ms間隔で更新
                        lastScrollTime = currentTime
                        selectTopVisibleItem()
                    } else if (isDPadNavigating && currentTime - lastScrollTime > 100) {
                        // D-pad navigation中はログのみ出力（デバッグ用）
                        lastScrollTime = currentTime
                        Log.d(TAG, "Scroll during D-pad navigation - auto-selection disabled")
                    }
                }
            },
        )

        // 検索バーの設定
        searchView = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.search_view)

        // ソートボタンの設定
        sortButton = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.sort_button)
        sortButton.setOnClickListener {
            showSortMenu(it)
        }

        // Boxart表示エリアの設定
        boxartImage = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.boxart_image)
        // gameInfoOverlay = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.game_info_overlay)
        // selectedGameTitle = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.selected_game_title)
        // selectedGameInfo = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.selected_game_info)

        // 中央のゲーム情報エリアの設定
        // selectedGameIcon = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.selected_game_icon)
        selectedGameVersion = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.selected_game_version)
        selectedGameMenu = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.selected_game_menu)

        // Setup window insets for game info section to avoid navigation bar overlap
        val gameInfoSection = rootView.findViewById<android.widget.LinearLayout>(org.devmiyax.yabasanshiro.R.id.game_info_section)
        androidx.core.view.ViewCompat.setOnApplyWindowInsetsListener(gameInfoSection) { view, windowInsets ->
            val insets = windowInsets.getInsets(
                androidx.core.view.WindowInsetsCompat.Type
                    .systemBars(),
            )

            // Adjust padding based on orientation
            val configuration = resources.configuration
            if (configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE) {
                // Landscape: adjust right padding for system bars
                view.setPadding(
                    view.paddingLeft,
                    view.paddingTop,
                    insets.right + view.paddingRight,
                    view.paddingBottom,
                )
            } else {
                // Portrait: reset right padding
                view.setPadding(
                    view.paddingLeft,
                    view.paddingTop,
                    4, // Original padding from XML
                    view.paddingBottom,
                )
            }

            windowInsets
        }

        // Boxartエリアのクリックリスナー（ゲーム開始）
        val boxartContainer = rootView.findViewById<FrameLayout>(org.devmiyax.yabasanshiro.R.id.boxart_container)
        boxartContainer.setOnClickListener {
            startSelectedGame()
        }

        // 再生ボタンのクリックリスナー（ゲーム開始）
        val playGameButton = rootView.findViewById<ImageButton>(org.devmiyax.yabasanshiro.R.id.play_game_button)
        playGameButton.setOnClickListener {
            startSelectedGame()
        }

        if (adHeight != 0) {
            onAdViewIsShown(adHeight)
        }
        return rootView
    }

    override fun onViewCreated(
        view: View,
        savedInstanceState: Bundle?,
    ) {
        super.onViewCreated(view, savedInstanceState)

        // 保存されたソートモードを復元
        currentSortMode = loadSortMode()

        // 保存されたナビゲーションアイテムIDを復元
        savedNavItemId = savedInstanceState?.getInt(KEY_SELECTED_NAV_ITEM, 0) ?: 0

        // D-PadナビゲーションはActivityレベルで処理するため、
        // RecyclerViewのキーリスナーは削除
        // フォーカスは設定しておく（必要に応じて）
        recyclerView.post {
            // RecyclerViewにフォーカスを設定しないことで、
            // Activityレベルでのキーイベント処理を優先
            Log.d(TAG, "RecyclerView setup completed")
        }

        // Subscription billing flow (must be in onViewCreated where viewLifecycleOwner is available)
        if (YabauseApplication.isSubscriptionDisabledByRemoteConfig) {
            // Remote config disabled subscription checking - remove any already-loaded ads
            // (Activity.onCreate() loads ads before Fragment sets the flag)
            (activity as? GameSelectActivityPhone)?.removeAdView()
        } else {
            viewLifecycleOwner.lifecycleScope.launch {
                viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                    viewModel.userCurrentSubscriptionFlow.collect { collectedSubscriptions ->
                        val subscribed = collectedSubscriptions.hasProAnnual == true
                        if (subscribed) {
                            Log.d(TAG, "hasProAnnual")
                        } else {
                            Log.d(TAG, "no active subscription")
                        }
                        presenter.isOnSubscription = subscribed
                        YabauseApplication.setSubscriptionState(subscribed)
                        if (subscribed) {
                            (activity as? GameSelectActivityPhone)?.removeAdView()
                        }
                    }
                }
            }
        }

        // UI setup
        setupUI(view, savedInstanceState)
    }

    private fun handleDPadNavigation(keyCode: Int): Boolean {
        // Handle button navigation first (left/right/center keys)
        if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT ||
            keyCode == KeyEvent.KEYCODE_DPAD_RIGHT ||
            keyCode == KeyEvent.KEYCODE_DPAD_CENTER ||
            keyCode == KeyEvent.KEYCODE_ENTER
        ) {
            return handleButtonNavigation(keyCode)
        }

        // Handle list scrolling (up/down keys only)
        if (!::gameAdapter.isInitialized || !::recyclerView.isInitialized) {
            Log.d(TAG, "handleDPadNavigation: adapter or recyclerView not initialized")
            return false
        }

        val currentPosition = gameAdapter.getSelectedPosition()
        val itemCount = gameAdapter.itemCount

        if (itemCount == 0) {
            Log.d(TAG, "handleDPadNavigation: no items in adapter")
            return false
        }

        Log.d(TAG, "handleDPadNavigation: keyCode=$keyCode, currentPosition=$currentPosition, itemCount=$itemCount")

        var newPosition = currentPosition
        var handled = false

        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> {
                if (currentPosition > 0) {
                    newPosition = currentPosition - 1
                    handled = true
                }
                // Reset button navigation mode when scrolling
                isButtonNavigationMode = false
                clearButtonFocus()
            }
            KeyEvent.KEYCODE_DPAD_DOWN -> {
                if (currentPosition < itemCount - 1) {
                    newPosition = currentPosition + 1
                    handled = true
                }
                // Reset button navigation mode when scrolling
                isButtonNavigationMode = false
                clearButtonFocus()
            }
        }

        if (handled && newPosition != currentPosition) {
            Log.d(TAG, "D-pad navigation: moving from position $currentPosition to $newPosition")
            // D-pad navigation mode を有効にして自動選択を無効化
            isDPadNavigating = true
            isAutoSelecting = true

            // リスト選択が変わったときはplay_game_buttonにフォーカスを当てる
            currentFocusedButton = FocusableButton.PLAY_GAME_BUTTON
            isButtonNavigationMode = true
            updateButtonFocus()

            // D-pad navigationでのスクロール方向に応じてAppBarを制御（移動が実際に発生する場合のみ）
            val appBar = activity?.findViewById<com.google.android.material.appbar.AppBarLayout>(org.devmiyax.yabasanshiro.R.id.main_appbar)
            if (appBar != null) {
                when (keyCode) {
                    KeyEvent.KEYCODE_DPAD_DOWN -> {
                        // 下にスクロールする場合はAppBarを隠す
                        appBar.setExpanded(false, true)
                    }
                    KeyEvent.KEYCODE_DPAD_UP -> {
                        // 上にスクロールする場合はAppBarを表示
                        appBar.setExpanded(true, true)
                    }
                }
            }

            // D-pad navigation中はアニメーションを無効化してtmpDetachedエラーを防ぐ
            val originalAnimator = recyclerView.itemAnimator
            recyclerView.itemAnimator = null

            // 選択位置を更新
            gameAdapter.setSelectedPosition(newPosition)

            // RecyclerViewをスクロールして選択されたアイテムを表示
            val layoutManager = recyclerView.layoutManager as? LinearLayoutManager
            layoutManager?.let {
                // 選択されたアイテムが見えるようにスクロール
                val firstVisible = it.findFirstVisibleItemPosition()
                val lastVisible = it.findLastVisibleItemPosition()

                // AppBarの高さを取得してオフセットに反映
                val appBarHeight = appBar?.height ?: 0

                if (newPosition < firstVisible || newPosition > lastVisible) {
                    // アイテムが見えない場合はスムーズスクロール
                    recyclerView.smoothScrollToPosition(newPosition)
                } else {
                    // アイテムが見える場合は、より良い位置に表示されるようにスクロール
                    // 最後のアイテムの場合はAppBarの高さを考慮してマージンを確保
                    val isLastItem = newPosition == itemCount - 1
                    val offset =
                        if (isLastItem) {
                            // 最後のアイテムの場合はAppBarの高さ分を確保
                            -appBarHeight
                        } else {
                            // 通常は画面の上部1/3の位置に表示
                            recyclerView.height / 3
                        }
                    it.scrollToPositionWithOffset(newPosition, offset)
                }
            }

            // フラグをリセット（少し遅延させてスクロール完了を待つ）
            recyclerView.post {
                isAutoSelecting = false
                // アニメーションを復元
                recyclerView.itemAnimator = originalAnimator
                // D-pad navigation mode は一定時間後にリセット（タッチ操作に戻る準備）
                recyclerView.postDelayed({
                    // タッチ操作がない場合のみリセット
                    if (isDPadNavigating) {
                        isDPadNavigating = false
                    }
                }, 3000) // 3秒後にD-pad modeを解除
            }
        }

        return handled
    }

    // ActivityからのキーイベントをハンドルするためのPublicメソッド
    fun onKeyDown(
        keyCode: Int,
        event: KeyEvent?,
    ): Boolean {
        val repeatCount = event?.repeatCount ?: 0
        Log.d(TAG, "Fragment onKeyDown called: keyCode=$keyCode, repeatCount=$repeatCount, isDPadNavigating=$isDPadNavigating")
        return handleDPadNavigation(keyCode)
    }

    fun onKeyMultiple(
        keyCode: Int,
        repeatCount: Int,
        event: KeyEvent?,
    ): Boolean {
        Log.d(TAG, "Fragment onKeyMultiple called: keyCode=$keyCode, repeatCount=$repeatCount")
        return handleDPadNavigation(keyCode)
    }

    // Button navigation system
    private enum class FocusableButton {
        SORT_BUTTON,
        PLAY_GAME_BUTTON,
        SELECTED_GAME_MENU,
    }

    private var currentFocusedButton = FocusableButton.SORT_BUTTON
    private var isButtonNavigationMode = false

    private fun handleButtonNavigation(keyCode: Int): Boolean {
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> {
                moveFocusToPreviousButton()
                return true
            }
            KeyEvent.KEYCODE_DPAD_RIGHT -> {
                moveFocusToNextButton()
                return true
            }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> {
                performButtonAction()
                return true
            }
        }
        return false
    }

    private fun moveFocusToNextButton() {
        isButtonNavigationMode = true
        currentFocusedButton =
            when (currentFocusedButton) {
                FocusableButton.SORT_BUTTON -> FocusableButton.PLAY_GAME_BUTTON
                FocusableButton.PLAY_GAME_BUTTON -> FocusableButton.SELECTED_GAME_MENU
                FocusableButton.SELECTED_GAME_MENU -> FocusableButton.SORT_BUTTON
            }
        updateButtonFocus()
    }

    private fun moveFocusToPreviousButton() {
        isButtonNavigationMode = true
        currentFocusedButton =
            when (currentFocusedButton) {
                FocusableButton.SORT_BUTTON -> FocusableButton.SELECTED_GAME_MENU
                FocusableButton.PLAY_GAME_BUTTON -> FocusableButton.SORT_BUTTON
                FocusableButton.SELECTED_GAME_MENU -> FocusableButton.PLAY_GAME_BUTTON
            }
        updateButtonFocus()
    }

    private fun updateButtonFocus() {
        if (!::sortButton.isInitialized || !::selectedGameMenu.isInitialized) return

        // Clear all focus states
        clearButtonFocus()

        // Set focus on current button
        when (currentFocusedButton) {
            FocusableButton.SORT_BUTTON -> {
                sortButton.requestFocus()
                // sortButton.setBackgroundColor(0x44FFFFFF) // Semi-transparent white
            }
            FocusableButton.PLAY_GAME_BUTTON -> {
                val playGameButton = rootView.findViewById<ImageButton>(org.devmiyax.yabasanshiro.R.id.play_game_button)
                playGameButton?.requestFocus()
                // playGameButton?.setBackgroundColor(0x44FFFFFF) // Semi-transparent white
            }
            FocusableButton.SELECTED_GAME_MENU -> {
                selectedGameMenu.requestFocus()
                // selectedGameMenu.setBackgroundColor(0x44FFFFFF) // Semi-transparent white
            }
        }
    }

    private fun clearButtonFocus() {
        sortButton.clearFocus()
        sortButton.setBackgroundColor(android.graphics.Color.TRANSPARENT)

        val playGameButton = rootView.findViewById<ImageButton>(org.devmiyax.yabasanshiro.R.id.play_game_button)
        playGameButton?.clearFocus()
        playGameButton?.setBackgroundColor(android.graphics.Color.TRANSPARENT)

        selectedGameMenu.clearFocus()
        selectedGameMenu.setBackgroundColor(android.graphics.Color.TRANSPARENT)
    }

    private fun performButtonAction() {
        when (currentFocusedButton) {
            FocusableButton.SORT_BUTTON -> {
                sortButton.performClick()
            }
            FocusableButton.PLAY_GAME_BUTTON -> {
                val playGameButton = rootView.findViewById<ImageButton>(org.devmiyax.yabasanshiro.R.id.play_game_button)
                playGameButton?.performClick()
            }
            FocusableButton.SELECTED_GAME_MENU -> {
                selectedGameMenu.performClick()
            }
        }
    }

    /**
     * Fetches cloud-backed games that aren't downloaded locally and adds them to the game list
     */
    private suspend fun fetchCloudOnlyGames(): List<GameInfo> {
        // Check if user is signed in
        val auth = FirebaseAuth.getInstance()
        if (auth.currentUser == null) {
            return emptyList()
        }

        try {
            // Get backed up games
            val gameBackupManager = org.uoyabause.android.backup
                .GameBackupManager(requireContext())
            val backedUpGames = gameBackupManager.getBackedUpGames()

            if (backedUpGames.isEmpty()) {
                return emptyList()
            }

            // Get local games to filter out games that are already downloaded
            val localGames = YabauseStorage.dao.getAll()
            val localProductNumbers = localGames.map { it.product_number }

            // Filter out games that are already downloaded
            val cloudOnlyGames =
                backedUpGames.filter { backupGame ->
                    !localProductNumbers.contains(backupGame.productNumber)
                }

            // Convert to GameInfo objects
            return cloudOnlyGames.map { backupGame ->
                CloudGameInfo(backupGame).toGameInfo()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error fetching cloud-only games: ${e.message}")
            return emptyList()
        }
    }

    private fun showSortMenu(view: View) {
        val popup = PopupMenu(requireContext(), view)
        popup.menuInflater.inflate(R.menu.sort_menu, popup.menu)
        popup.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.sort_by_name -> {
                    currentSortMode = SortMode.NAME
                    saveSortMode()
                    gameAdapter.sortByName()
                    true
                }
                R.id.sort_by_date -> {
                    currentSortMode = SortMode.DATE
                    saveSortMode()
                    gameAdapter.sortByDate()
                    true
                }
                R.id.sort_by_recently_played -> {
                    currentSortMode = SortMode.RECENTLY_PLAYED
                    saveSortMode()
                    gameAdapter.sortByRecentlyPlayed()
                    true
                }
                else -> false
            }
        }
        popup.show()
    }

    private fun updateGameInfoSection(gameInfo: GameInfo?) {
        // Fragment がアタッチされていない場合は何もしない
        if (!isAdded || context == null) {
            return
        }

        if (gameInfo == null) {
            // ゲーム情報がない場合はデフォルト表示
            selectedGameVersion.text = ""
            selectedGameMenu.setOnClickListener(null)
            return
        }

        // バージョン情報を表示
        if (gameInfo.isCloudOnly) {
            selectedGameVersion.text = getString(R.string.cloud_only_game)
            selectedGameVersion.setCompoundDrawablesWithIntrinsicBounds(
                R.drawable.cloud_upload_48px,
                0,
                0,
                0,
            )
            selectedGameVersion.compoundDrawablePadding = 8
        } else {
            selectedGameVersion.setCompoundDrawablesWithIntrinsicBounds(0, 0, 0, 0)

            // レーティングとデバイス情報を表示
            lifecycleScope.launch(Dispatchers.IO) {
                gameInfo.updateState()
                var rate = ""
                for (i in 0 until gameInfo.rating) rate += "★"
                if (gameInfo.device_infomation != "CD-1/1") {
                    rate += " " + gameInfo.device_infomation
                }

                withContext(Dispatchers.Main) {
                    selectedGameVersion.text = rate
                }
            }
        }

        // メニューボタンのクリックリスナーを設定
        selectedGameMenu.setOnClickListener {
            if (::gameAdapter.isInitialized) {
                val selectedPosition = gameAdapter.getSelectedPosition()
                if (selectedPosition >= 0) {
                    // GameItemAdapterのshowPopupMenuメソッドを呼び出す
                    gameAdapter.showPopupMenu(selectedGameMenu, selectedPosition, boxartImage)
                }
            }
        }
    }

    private fun updateBoxartDisplay(gameInfo: GameInfo?) {
        // Fragment がアタッチされていない場合は何もしない
        if (!isAdded || context == null) {
            return
        }

        // デフォルト背景色のプレースホルダーを作成
        val placeholderDrawable =
            androidx.core.content.ContextCompat.getDrawable(
                requireContext(),
                android.R.color.transparent,
            )

        if (gameInfo == null) {
            // Glideを使ってエラー画像を設定（setImageResourceは使わない）
            Glide
                .with(boxartImage)
                .load(R.drawable.missing)
                .placeholder(placeholderDrawable) // 読み込み中は透明
                .into(boxartImage)
            // gameInfoOverlay.visibility = View.GONE
            return
        }

        // ゲーム情報を表示
        // selectedGameTitle.text = gameInfo.game_title
        // var infoText = ""
        // if (gameInfo.device_infomation != "CD-1/1") {
        //    infoText = gameInfo.device_infomation
        // }

        // レーティングを追加
        lifecycleScope.launch(Dispatchers.IO) {
            gameInfo.updateState()
            var rate = ""
            for (i in 0 until gameInfo.rating) rate += "★"
            if (gameInfo.device_infomation != "CD-1/1") {
                rate += " " + gameInfo.device_infomation
            }

            withContext(Dispatchers.Main) {
                // selectedGameInfo.text = rate
                // gameInfoOverlay.visibility = View.VISIBLE
            }
        }

        // Boxart画像を読み込み
        if (gameInfo.image_url != null && gameInfo.image_url != "") {
            if (gameInfo.image_url!!.startsWith("http")) {
                var url = gameInfo.image_url
                if (gameInfo.isCloudOnly) {
                    url += "?" + GameInfo.sigin
                }

                val glideRequest =
                    Glide
                        .with(boxartImage)
                        .load(url)
                        .placeholder(placeholderDrawable) // 読み込み中は透明
                        .error(R.drawable.missing) // エラー時のフォールバック画像を設定

                // クラウドゲームの場合はブラー効果を適用
                if (gameInfo.isCloudOnly) {
                    glideRequest.apply(RequestOptions.bitmapTransform(BlurTransformation(8)))
                }

                glideRequest.into(boxartImage)
            } else {
                Glide
                    .with(requireContext())
                    .load(gameInfo.image_url?.let { File(it) })
                    .placeholder(placeholderDrawable) // 読み込み中は透明
                    .error(R.drawable.missing) // エラー時のフォールバック画像を設定
                    .into(boxartImage)
            }
        } else {
            // Glideを使ってエラー画像を設定（setImageResourceは使わない）
            Glide
                .with(boxartImage)
                .load(R.drawable.missing)
                .placeholder(placeholderDrawable) // 読み込み中は透明
                .into(boxartImage)
        }
    }

    private var adHeight = 0

    fun onAdViewIsShown(height: Int) {
        try {
            val parentLayout = rootView.findViewById<View>(org.devmiyax.yabasanshiro.R.id.parent)
            val param = parentLayout.layoutParams as FrameLayout.LayoutParams
            param.bottomMargin = height + 4
            parentLayout.layoutParams = param
        } catch (e: Exception) {
            adHeight = height
        }
    }

    suspend fun startSub() {
        if (viewModel.billingConnectionState.value == true) {
            val yearlyBasicPlansTag = "yearly-basic"
            viewModel.proAnnualProductDetails.collectLatest { it ->
                it.let {
                    viewModel.buy(
                        productDetails = it,
                        currentPurchases = null,
                        tag = yearlyBasicPlansTag,
                        activity = requireActivity(),
                    )
                }
            }
        }
    }

    private fun handleNavigationItemSelected(itemId: Int): Boolean {
        playNavigationSound()
        return when (itemId) {
            org.devmiyax.yabasanshiro.R.id.nav_games -> {
                showLibraryView()
                true
            }
            org.devmiyax.yabasanshiro.R.id.nav_backup -> {
                firebaseAnalytics?.logEvent("game_select_fragment", Bundle().apply { putString("event", "menu_backup_manager") })
                rootView.findViewById<View>(org.devmiyax.yabasanshiro.R.id.main_content_layout)?.visibility = View.GONE
                val fragment = org.uoyabause.android.backup.ui.BackupManagerFragment
                    .newInstance()
                val transaction = requireActivity().supportFragmentManager.beginTransaction()
                transaction.replace(org.devmiyax.yabasanshiro.R.id.ext_fragment, fragment)
                transaction.commit()
                true
            }
            org.devmiyax.yabasanshiro.R.id.nav_add -> {
                firebaseAnalytics?.logEvent("game_select_fragment", Bundle().apply { putString("event", "menu_item_load_game") })
                if (Build.VERSION.SDK_INT >= VERSION_CODES.Q) {
                    showAddGameBottomSheet()
                } else {
                    val sharedPref = PreferenceManager.getDefaultSharedPreferences(this.requireActivity())
                    val lastDir = sharedPref.getString("pref_last_dir", YabauseStorage.storage.gamePath)
                    val fd = FileDialog(requireActivity(), lastDir)
                    fd.addFileListener(this)
                    fd.showDialog()
                }
                true
            }
            org.devmiyax.yabasanshiro.R.id.nav_settings -> {
                firebaseAnalytics?.logEvent("game_select_fragment", Bundle().apply { putString("event", "menu_item_setting") })
                rootView.findViewById<View>(org.devmiyax.yabasanshiro.R.id.main_content_layout)?.visibility = View.GONE
                val fragment = SettingsContainerFragment.newInstance()
                val transaction = requireActivity().supportFragmentManager.beginTransaction()
                transaction.replace(org.devmiyax.yabasanshiro.R.id.ext_fragment, fragment)
                transaction.commit()
                true
            }
            org.devmiyax.yabasanshiro.R.id.nav_account -> {
                firebaseAnalytics?.logEvent("game_select_fragment", Bundle().apply { putString("event", "menu_item_login") })
                rootView.findViewById<View>(org.devmiyax.yabasanshiro.R.id.main_content_layout)?.visibility = View.GONE
                val fragment = org.uoyabause.android.auth.ui.AccountManagementFragment
                    .newInstance()
                val transaction = requireActivity().supportFragmentManager.beginTransaction()
                transaction.replace(org.devmiyax.yabasanshiro.R.id.ext_fragment, fragment)
                transaction.commit()
                true
            }
            else -> false
        }
    }

    private fun playNavigationSound() {
        try {
            val audioManager = requireContext().getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
            audioManager.playSoundEffect(android.media.AudioManager.FX_FOCUS_NAVIGATION_UP)
        } catch (e: Exception) {
            // Ignore audio errors
        }
    }

    private fun showLibraryView() {
        val fm = requireActivity().supportFragmentManager
        // Clear any remaining back stack entries
        fm.popBackStack(null, androidx.fragment.app.FragmentManager.POP_BACK_STACK_INCLUSIVE)
        val extFragment = fm.findFragmentById(org.devmiyax.yabasanshiro.R.id.ext_fragment)
        if (extFragment != null) {
            fm
                .beginTransaction()
                .remove(extFragment)
                .commit()
        }
        rootView.findViewById<View>(org.devmiyax.yabasanshiro.R.id.main_content_layout)?.visibility = View.VISIBLE
    }

    private fun checkStoragePermission(): Int {
        if (Build.VERSION.SDK_INT < VERSION_CODES.Q) { // Verify that all required contact permissions have been granted.
            if (ActivityCompat.checkSelfPermission(
                    requireActivity().applicationContext,
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                )
                != PackageManager.PERMISSION_GRANTED ||
                ActivityCompat.checkSelfPermission(
                    requireActivity().applicationContext,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE,
                )
                != PackageManager.PERMISSION_GRANTED
            ) { // Contacts permissions have not been granted.
                Log.i(
                    TAG,
                    "Storage permissions has NOT been granted. Requesting permissions.",
                )
                requestStoragePermission.launch(permissionsStorage)
                return -1
            }
        }
        return 0
    }

    private val requestStoragePermission =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
            result.entries.forEach {
                if (!it.value) {
                    showRestartMessage()
                    return@registerForActivityResult
                }
            }
            updateGameList(0)
        }

    private fun showSnackBar(id: Int) {
        Snackbar
            .make(rootView.rootView, getString(id), Snackbar.LENGTH_SHORT)
            .show()
    }

    private fun updateRecent() {
        // 最近プレイしたゲームのリストを取得して表示を更新
        GlobalScope.launch(Dispatchers.IO) {
            try {
                val recentList = YabauseStorage.dao.getRecentGames()

                // Get cloud-only games
                val cloudOnlyGames = fetchCloudOnlyGames()

                launch(Dispatchers.Main) {
                    // 全ゲームリストを再取得
                    val localGames = YabauseStorage.dao.getAllSortedByTitle()

                    // Create a new mutable list with the correct type
                    val combinedGames: MutableList<GameInfo?> = mutableListOf()

                    // Add local games
                    combinedGames.addAll(localGames)

                    // Add cloud-only games if there are any
                    if (cloudOnlyGames.isNotEmpty()) {
                        combinedGames.addAll(cloudOnlyGames)
                    }

                    // Apply game limit for non-Pro users
                    val limitedGames: MutableList<GameInfo?> = if (!YabauseApplication.isPro() &&
                        combinedGames.size > BuildConfig.MAX_FREE_GAMES
                    ) {
                        combinedGames.take(BuildConfig.MAX_FREE_GAMES).toMutableList()
                    } else {
                        combinedGames
                    }

                    // Assign to allGames
                    allGames = limitedGames

                    gameAdapter = GameItemAdapter(allGames)
                    gameAdapter.setOnItemClickListener(this@GameSelectFragmentPhone)

                    // スリムモードに固定
                    gameAdapter.setViewMode(GameItemAdapter.Companion.VIEW_TYPE_SLIM)

                    // 最初のアイテムを選択
                    allGames?.let { games ->
                        if (games.isNotEmpty()) {
                            // 初期選択は最初のアイテム
                            gameAdapter.setSelectedPosition(0)
                            // レイアウト完了後に一番上のアイテムを選択
                            recyclerView.post {
                                selectTopVisibleItem()
                            }
                        }
                    }

                    recyclerView.adapter = gameAdapter

                    // 現在のソート順を適用
                    applySortMode()
                }
            } catch (e: Exception) {
                Log.d(TAG, e.localizedMessage ?: "Error updating recent games")
            }
        }
    }

    private var yabauseActivityLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {

            val playtime = it.data?.getLongExtra("playTime", 0) ?: 0L

            Log.d(TAG, "Play time is $playtime")

            firebaseAnalytics?.logEvent(
                "game_select_fragment",
                Bundle().apply {
                    putString("event", "On Game Finished")
                    putLong("playTime", playtime)
                },
            )
            val prefs =
                requireActivity().getSharedPreferences(
                    "private",
                    Context.MODE_PRIVATE,
                )

            // レビューリクエスト（30日に一度、5分以上プレイ後、30%の確率）
            val rn = Math.random()
            val lastReviewDateTime = prefs.getInt("last_review_date_time", 0)
            val unixTime = System.currentTimeMillis() / 1000L

            if (rn < 0.3 && (unixTime - lastReviewDateTime) > 60 * 60 * 24 * 30) {
                if (playtime >= 5 * 60) {
                    var manager: ReviewManager? = null
                    if (BuildConfig.DEBUG) {
                        manager = FakeReviewManager(requireContext())
                    } else {
                        val editor = prefs.edit()
                        editor.putInt("last_review_date_time", unixTime.toInt())
                        editor.commit()
                        manager = ReviewManagerFactory.create(requireContext())
                    }
                    val request = manager.requestReviewFlow()
                    request.addOnCompleteListener { task ->
                        if (task.isSuccessful) {
                            val reviewInfo = task.result
                            val flow = manager?.launchReviewFlow(requireActivity(), reviewInfo)
                            flow?.addOnCompleteListener { _ -> }
                        } else {
                            task.getException()?.message?.let { it1 ->
                                Log.d(TAG, it1)
                            }
                        }
                    }
                }
            }

            updateRecent()
        }

    override fun fileSelected(file: File?) {
        firebaseAnalytics?.logEvent(
            "game_select_fragment",
            Bundle().apply {
                putString("event", "fileSelected")
            },
        )

        if (file != null) {
            presenter.fileSelected(file)
        }
    }

    fun showDialog(message: String?) {
        if (message != null) {
            progressMessage.text = message
        } else {
            progressMessage.text = getString(org.devmiyax.yabasanshiro.R.string.updating)
        }
        progressBar.visibility = VISIBLE
    }

    fun updateDialogString(msg: String) {
        progressMessage.text = msg
    }

    fun dismissDialog() {
        progressBar.visibility = View.GONE
    }

    private fun setupUI(
        view: View,
        savedInstanceState: Bundle?,
    ) {
        val activity = requireActivity() as AppCompatActivity
        firebaseAnalytics = FirebaseAnalytics.getInstance(activity)
        val application = activity.application as YabauseApplication
        tracker = application.defaultTracker
        val toolbar =
            rootView.findViewById<View>(org.devmiyax.yabasanshiro.R.id.toolbar) as Toolbar
        toolbar.setLogo(org.devmiyax.yabasanshiro.R.mipmap.ic_launcher)
        toolbar.title = getString(org.devmiyax.yabasanshiro.R.string.app_name)
        toolbar.subtitle = getVersionName(activity)
        activity.setSupportActionBar(toolbar)

        // Remove hamburger icon since there's no drawer anymore
        toolbar.navigationIcon = null

        // Setup Bottom Navigation / Navigation Rail
        setupNavigation(rootView)

        if (checkStoragePermission() == 0) {
            updateGameList(0)
        }
    }

    private fun setupNavigation(rootView: View) {
        bottomNavigationView = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.bottom_navigation)
        navigationRail = rootView.findViewById(org.devmiyax.yabasanshiro.R.id.navigation_rail)

        val listener = NavigationBarView.OnItemSelectedListener { item ->
            handleNavigationItemSelected(item.itemId)
        }
        bottomNavigationView?.setOnItemSelectedListener(listener)
        navigationRail?.setOnItemSelectedListener(listener)

        // Default selection (or restored from saved state)
        val navView: NavigationBarView? = bottomNavigationView ?: navigationRail
        val targetItemId = if (savedNavItemId != 0) savedNavItemId else org.devmiyax.yabasanshiro.R.id.nav_games
        navView?.selectedItemId = targetItemId
        savedNavItemId = 0 // Reset after restoring

        // Update account icon with profile photo if available
        updateAccountIcon()
    }

    private fun updateAccountIcon() {
        val context = context ?: return
        val user = FirebaseAuth.getInstance().currentUser
        val photoUrl = user?.photoUrl

        // 未ログインまたはプロフィール画像がない場合はデフォルトアイコンに戻す
        if (user == null || photoUrl == null) {
            val defaultIcon = ContextCompat.getDrawable(context, org.devmiyax.yabasanshiro.R.drawable.ic_baseline_person_24px)
            bottomNavigationView?.menu?.findItem(org.devmiyax.yabasanshiro.R.id.nav_account)?.icon = defaultIcon
            navigationRail?.menu?.findItem(org.devmiyax.yabasanshiro.R.id.nav_account)?.icon = defaultIcon
            return
        }

        Glide
            .with(context)
            .asBitmap()
            .load(photoUrl)
            .apply(RequestOptions.circleCropTransform())
            .into(object : com.bumptech.glide.request.target.CustomTarget<android.graphics.Bitmap>() {
                override fun onResourceReady(resource: android.graphics.Bitmap, transition: com.bumptech.glide.request.transition.Transition<in android.graphics.Bitmap>?) {
                    if (!isAdded) return
                    // NavigationBarItemView が getConstantState().newDrawable() で
                    // 新インスタンスを生成し tint を適用するため、
                    // Drawable を直接継承して描画だけ委譲し tint を全て無視する
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
                    bottomNavigationView?.menu?.findItem(org.devmiyax.yabasanshiro.R.id.nav_account)?.icon = icon
                    navigationRail?.menu?.findItem(org.devmiyax.yabasanshiro.R.id.nav_account)?.icon = icon
                }

                override fun onLoadCleared(placeholder: android.graphics.drawable.Drawable?) {}
            })
    }

    fun resetNavigationToGames() {
        val navView: NavigationBarView? = bottomNavigationView ?: navigationRail
        navView?.selectedItemId = org.devmiyax.yabasanshiro.R.id.nav_games
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)

        val gameInfoSection = rootView.findViewById<android.widget.LinearLayout>(org.devmiyax.yabasanshiro.R.id.game_info_section)
        gameInfoSection?.let {
            androidx.core.view.ViewCompat
                .requestApplyInsets(it)
        }

        // 画面の向きに応じてRecyclerViewのレイアウトを変更
        // if (newConfig.orientation == Configuration.ORIENTATION_LANDSCAPE) {
        //    recyclerView.layoutManager = GridLayoutManager(context, 2)
        // } else {
        //    recyclerView.layoutManager = LinearLayoutManager(context)
        // }
    }

    private fun updateGameList(level: Int) {
        if (updateJob?.isActive == true) return

        val ctx = requireContext()
        val sdCardDir = requireActivity().getExternalFilesDirs(null).getOrNull(1)
        val needsInternal = StorageMigrationHelper.needsInternalMigration(ctx)
        val needsExternal = sdCardDir != null && StorageMigrationHelper.needsExternalMigration(ctx, sdCardDir)
        android.util.Log.i("StorageMigration", "needsInternal=$needsInternal needsExternal=$needsExternal sdCardDir=$sdCardDir")

        if (needsInternal || needsExternal) {
            showMigrationExplanationDialog(level, sdCardDir)
            return
        }

        proceedUpdateGameList(level)
    }

    private fun showMigrationExplanationDialog(
        level: Int,
        sdCardDir: java.io.File?,
    ) {
        MaterialAlertDialogBuilder(requireContext())
            .setIcon(org.devmiyax.yabasanshiro.R.drawable.baseline_folder_open_24)
            .setTitle(org.devmiyax.yabasanshiro.R.string.storage_migration_dialog_title)
            .setMessage(org.devmiyax.yabasanshiro.R.string.storage_migration_dialog_message)
            .setCancelable(false)
            .setPositiveButton(org.devmiyax.yabasanshiro.R.string.storage_migration_dialog_migrate) { _, _ ->
                runMigrationWithProgress(level, sdCardDir)
            }.setNegativeButton(org.devmiyax.yabasanshiro.R.string.storage_migration_dialog_later) { _, _ ->
                proceedUpdateGameList(level)
            }.show()
    }

    private fun runMigrationWithProgress(
        level: Int,
        sdCardDir: java.io.File?,
    ) {
        val ctx = requireContext()
        val progressView =
            layoutInflater.inflate(org.devmiyax.yabasanshiro.R.layout.dialog_migration_progress, null)
        val progressText =
            progressView.findViewById<android.widget.TextView>(org.devmiyax.yabasanshiro.R.id.migration_progress_text)
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
                proceedUpdateGameList(level)
            }
        }
    }

    private fun proceedUpdateGameList(level: Int) {
        isBackGroundComplete = false
        if (!presenter.prepareStorage()) return

        updateJob =
            lifecycleScope.launch {
                showDialog(null)
                try {
                    presenter.updateGameDatabase(level) { message ->
                        updateDialogString("${getString(org.devmiyax.yabasanshiro.R.string.updating)} $message")
                    }

                    firebaseAnalytics?.logEvent(
                        "game_select_fragment",
                        Bundle().apply {
                            putString("event", "updateGameList onComplete")
                        },
                    )

                    if (!isFront) {
                        dismissDialog()
                        isBackGroundComplete = true
                        return@launch
                    }

                    loadRows()
                    dismissDialog()

                    if (isFirstUpdate) {
                        isFirstUpdate = false
                        if (this@GameSelectFragmentPhone.requireActivity().intent!!.getBooleanExtra(
                                "showPin",
                                false,
                            )
                        ) {
                            if (!YabauseApplication.isPro()) {
                                YabauseApplication.checkDonated(requireActivity())
                            } else {
                                ShowPinInFragment.newInstance().show(
                                    childFragmentManager,
                                    "sample",
                                )
                            }
                        }
                    }
                } catch (e: Exception) {
                    firebaseAnalytics?.logEvent(
                        "game_select_fragment",
                        Bundle().apply {
                            putString("event", "updateGameList onError")
                        },
                    )
                    dismissDialog()
                }
            }
    }

    private fun showRestartMessage() { // need_to_accept
        val viewMessageParent = rootView.findViewById<ScrollView?>(org.devmiyax.yabasanshiro.R.id.empty_message_parent)
        val viewMessage = rootView.findViewById<TextView?>(org.devmiyax.yabasanshiro.R.id.empty_message)
        recyclerView.visibility = View.GONE
        viewMessageParent?.visibility = VISIBLE

        val welcomeMessage = resources.getString(org.devmiyax.yabasanshiro.R.string.need_to_accept)
        viewMessage.text = welcomeMessage
    }

    private var allGames: MutableList<GameInfo?>? = null

    private fun loadRows() {
        Log.d("GameSelect", "loadRows")

        GlobalScope.launch(Dispatchers.IO) {
            // ゲーム数を確認
            var dataCount = 0
            try {
                val glist: List<GameInfo> = YabauseStorage.dao.getAll()
                dataCount = YabauseStorage.dao.getRowCount()
                if (glist.size != dataCount) {
                    Log.d(TAG, "dataCount is not match")
                }
            } catch (e: Exception) {
                Log.d(TAG, e.localizedMessage!!)
            }

            // Get cloud-only games
            val cloudOnlyGames = fetchCloudOnlyGames()
            val totalGameCount = dataCount + cloudOnlyGames.size

            if (totalGameCount == 0) {
                // ゲームがない場合はウェルカムメッセージを表示（更新中は表示しない）
                launch(Dispatchers.Main) {
                    val viewMessageParent = rootView.findViewById<ScrollView?>(org.devmiyax.yabasanshiro.R.id.empty_message_parent)
                    val viewMessage = rootView.findViewById<TextView?>(org.devmiyax.yabasanshiro.R.id.empty_message)
                    recyclerView.visibility = View.GONE
                    viewMessageParent!!.visibility = VISIBLE

                    val markwon = Markwon.create(this@GameSelectFragmentPhone.activity as Context)

                    if (Build.VERSION.SDK_INT >= VERSION_CODES.Q) {
                        val welcomeMessage =
                            resources.getString(
                                org.devmiyax.yabasanshiro.R.string.welcome_11,
                                YabauseStorage.storage.gamePath,
                                YabauseStorage.storage.externalGamePath ?: "",
                            )
                        markwon.setMarkdown(viewMessage, welcomeMessage)
                    } else {
                        val welcomeMessage =
                            resources.getString(
                                org.devmiyax.yabasanshiro.R.string.welcome,
                                YabauseStorage.storage.gamePath,
                            )
                        markwon.setMarkdown(viewMessage, welcomeMessage)
                    }
                }
                return@launch
            }

            // ゲームがある場合はリストを表示
            launch(Dispatchers.Main) {
                val viewMessageParent = rootView.findViewById<ScrollView?>(org.devmiyax.yabasanshiro.R.id.empty_message_parent)
                viewMessageParent?.visibility = View.GONE
                recyclerView.visibility = VISIBLE

                // すべてのゲームを取得
                GlobalScope.launch(Dispatchers.IO) {
                    try {
                        // Get local games
                        val localGames = YabauseStorage.dao.getAllSortedByTitle()

                        // Create a new mutable list with the correct type
                        val combinedGames: MutableList<GameInfo?> = mutableListOf()

                        // Add local games
                        combinedGames.addAll(localGames)

                        // Add cloud-only games if there are any
                        if (cloudOnlyGames.isNotEmpty()) {
                            combinedGames.addAll(cloudOnlyGames)
                        }

                        // Apply game limit for non-Pro users
                        val limitedGames: MutableList<GameInfo?> = if (!YabauseApplication.isPro() &&
                            combinedGames.size > BuildConfig.MAX_FREE_GAMES
                        ) {
                            combinedGames.take(BuildConfig.MAX_FREE_GAMES).toMutableList()
                        } else {
                            combinedGames
                        }

                        // Assign to allGames
                        allGames = limitedGames

                        launch(Dispatchers.Main) {
                            gameAdapter = GameItemAdapter(allGames)
                            gameAdapter.setOnItemClickListener(this@GameSelectFragmentPhone)

                            // スリムモードに固定
                            gameAdapter.setViewMode(GameItemAdapter.Companion.VIEW_TYPE_SLIM)

                            // 最初のアイテムを選択
                            allGames?.let { games ->
                                if (games.isNotEmpty()) {
                                    // 初期選択は最初のアイテム
                                    gameAdapter.setSelectedPosition(0)
                                    // レイアウト完了後に一番上のアイテムを選択
                                    recyclerView.post {
                                        selectTopVisibleItem()
                                    }
                                }
                            }

                            recyclerView.adapter = gameAdapter

                            // 検索バーのリスナーを設定
                            searchView.setOnQueryTextListener(
                                object : SearchView.OnQueryTextListener {
                                    override fun onQueryTextSubmit(query: String?): Boolean = false

                                    override fun onQueryTextChange(newText: String?): Boolean {
                                        gameAdapter.filter.filter(newText)
                                        return true
                                    }
                                },
                            )

                            // 現在のソート順を適用（デフォルトは名前順）
                            applySortMode()

                            // Update RetroAchievements progress in background if logged in
                            updateRetroAchievementsProgress()
                        }
                    } catch (e: Exception) {
                        Log.d(TAG, "${e.localizedMessage}")
                    }
                }
            }
        }
    }

    /**
     * Update RetroAchievements progress for all games in background
     */
    private fun updateRetroAchievementsProgress() {
        Log.d(TAG, "updateRetroAchievementsProgress() called")
        val raManager = org.uoyabause.android.achievements.RetroAchievementsManager
            .getInstance(requireContext())

        if (!raManager.isUserLoggedIn()) {
            Log.d(TAG, "Skipping RA progress update - user not logged in")
            return
        }

        val games = allGames?.filterNotNull() ?: emptyList()

        lifecycleScope.launch(Dispatchers.IO) {
            try {
                raManager.updateProgressIfNeeded(games) { current, total ->
                    Log.d(TAG, "RA progress update: $current/$total")
                    if (current % 5 == 0 || current == total) {
                        launch(Dispatchers.Main) {
                            gameAdapter.notifyDataSetChanged()
                        }
                    }
                }
                launch(Dispatchers.Main) {
                    gameAdapter.notifyDataSetChanged()
                    Log.d(TAG, "RA progress update complete")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error updating RA progress", e)
            }
        }
    }

    override fun onItemClick(
        position: Int,
        item: GameInfo?,
        v: View?,
    ) {
        // リストアイテムのクリックは選択のみ（ゲーム開始はboxartエリアのクリックで実行）
        // 実際の処理はGameItemAdapterで直接setSelectedPositionを呼び出している
    }

    override fun onItemSelected(
        position: Int,
        item: GameInfo?,
    ) {
        // 削除後の選択や手動選択の場合、フラグを設定
        if (!isAutoSelecting) {
            isManuallySelected = true
            // 一定時間後にフラグをリセット（スクロール時の自動選択を再開）
            recyclerView.postDelayed({
                isManuallySelected = false
            }, 2000) // 2秒後にリセット
        }
        updateBoxartDisplay(item)
        updateGameInfoSection(item)
    }

    override fun onGameStart(item: GameInfo?) {
        startSelectedGame()
    }

    private fun startSelectedGame() {
        if (::gameAdapter.isInitialized) {
            val selectedGame = gameAdapter.getSelectedGame()
            if (selectedGame != null) {
                if (selectedGame.isCloudOnly && selectedGame.cloudBackupInfo != null) {
                    // Handle cloud-only game click - download it first
                    downloadCloudGame(selectedGame.cloudBackupInfo!!)
                } else {
                    // Normal game click - start the game
                    presenter.startGame(selectedGame, yabauseActivityLauncher)
                }
            }
        }
    }

    private fun selectTopVisibleItem() {
        if (!::recyclerView.isInitialized || !::gameAdapter.isInitialized) {
            return
        }

        // 手動選択や削除後の選択がある場合は、自動選択をスキップ
        if (isManuallySelected) {
            return
        }

        val layoutManager = recyclerView.layoutManager as? LinearLayoutManager ?: return

        // 現在表示されている一番上のアイテムの位置を取得
        val firstVisiblePosition = layoutManager.findFirstVisibleItemPosition()

        if (firstVisiblePosition == RecyclerView.NO_POSITION) {
            return
        }

        // 現在の選択と異なる場合のみ更新
        if (firstVisiblePosition != gameAdapter.getSelectedPosition() && firstVisiblePosition >= 0) {
            isAutoSelecting = true
            gameAdapter.setSelectedPosition(firstVisiblePosition)
            isAutoSelecting = false
        }
    }

    /**
     * Creates a progress dialog using AlertDialog with custom layout
     */
    private fun createProgressDialog(message: String): Pair<androidx.appcompat.app.AlertDialog, TextView> {
        val dialogView =
            LinearLayout(requireContext()).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(48, 32, 48, 32)

                addView(
                    ProgressBar(requireContext()).apply {
                        isIndeterminate = true
                    },
                )

                addView(
                    TextView(requireContext()).apply {
                        text = message
                        setPadding(32, 0, 0, 0)
                        layoutParams =
                            LinearLayout
                                .LayoutParams(
                                    LinearLayout.LayoutParams.WRAP_CONTENT,
                                    LinearLayout.LayoutParams.WRAP_CONTENT,
                                ).apply {
                                    gravity = android.view.Gravity.CENTER_VERTICAL
                                }
                    },
                )
            }

        val messageTextView = dialogView.getChildAt(1) as TextView

        val dialog =
            MaterialAlertDialogBuilder(requireContext())
                .setView(dialogView)
                .setCancelable(false)
                .create()

        return Pair(dialog, messageTextView)
    }

    /**
     * Downloads a cloud-backed game
     */
    private fun downloadCloudGame(backupGameInfo: org.uoyabause.android.backup.GameBackupManager.BackupGameInfo) {
        // Show progress dialog
        val (progressDialog, _) = createProgressDialog("Downloading game...")
        progressDialog.show()

        // Get game backup manager
        val gameBackupManager = org.uoyabause.android.backup
            .GameBackupManager(requireContext())

        // Launch coroutine to restore game
        CoroutineScope(Dispatchers.Main).launch {
            try {
                val result = gameBackupManager.restoreGame(backupGameInfo)

                // Dismiss progress dialog
                progressDialog.dismiss()

                // Show result
                if (result.success) {
                    Toast
                        .makeText(
                            requireContext(),
                            getString(org.devmiyax.yabasanshiro.R.string.restore_success),
                            Toast.LENGTH_SHORT,
                        ).show()

                    // Refresh game list
                    updateGameList(YabauseStorage.REFRESH_LEVEL_REBUILD)
                } else {
                    Toast
                        .makeText(
                            requireContext(),
                            "${getString(org.devmiyax.yabasanshiro.R.string.restore_failed)}: ${result.message}",
                            Toast.LENGTH_LONG,
                        ).show()
                }
            } catch (e: Exception) {
                // Dismiss progress dialog
                progressDialog.dismiss()

                Toast
                    .makeText(
                        requireContext(),
                        "${getString(org.devmiyax.yabasanshiro.R.string.restore_failed)}: ${e.message}",
                        Toast.LENGTH_LONG,
                    ).show()
            }
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        val navView: NavigationBarView? = bottomNavigationView ?: navigationRail
        navView?.let {
            outState.putInt(KEY_SELECTED_NAV_ITEM, it.selectedItemId)
        }
    }

    override fun onGameRemoved(item: GameInfo?) {
        firebaseAnalytics?.logEvent(
            "game_select_fragment",
            Bundle().apply {
                putString("event", "onGameRemoved")
            },
        )

        if (item == null) return

        // アダプターから削除
        gameAdapter.removeItem(item.id)
    }

    override fun onResume() {
        super.onResume()

        // Update account icon in case user signed in or changed profile photo
        updateAccountIcon()

        if (tracker != null) { // mTracker.setScreenName(TAG);
            tracker!!.send(ScreenViewBuilder().build())
        }

        isFront = true
        if (isBackGroundComplete) {
            updateGameList(0)
        }
        presenter.onResume()
    }

    var isFront = true

    override fun onPause() {
        isFront = false
        super.onPause()
        this.presenter.onPause()
    }

    override fun onDestroy() {
        System.gc()
        super.onDestroy()
    }

    override fun onShowMessage(string_id: Int) {
        showSnackBar(string_id)
    }

    override fun onShowDialog(message: String) {
        showDialog(message)
    }

    override fun onUpdateDialogMessage(message: String) {
        updateDialogString(message)
    }

    override fun onDismissDialog() {
        dismissDialog()
    }

    override fun onLoadRows() {
        loadRows()
    }

    override fun onSignOut() {
    }

    companion object {
        private const val TAG = "GameSelectFragmentPhone"
        private var instance: GameSelectFragmentPhone? = null

        @JvmField
        var myOnClickListener: View.OnClickListener? = null
        private val permissionsStorage =
            arrayOf(
                Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE,
            )

        fun newInstance(): GameSelectFragmentPhone {
            val fragment = GameSelectFragmentPhone()
            val args = Bundle()
            fragment.arguments = args
            return fragment
        }

        fun getInstance(): GameSelectFragmentPhone? = instance

        fun getVersionName(context: Context): String {
            val pm = context.packageManager
            var versionName = ""
            try {
                val packageInfo = pm.getPackageInfo(context.packageName, 0)
                versionName = packageInfo.versionName ?: ""
            } catch (e: PackageManager.NameNotFoundException) {
                e.printStackTrace()
            }
            return versionName
        }
    }

    // ソートモードを定義する列挙型
    private enum class SortMode {
        NAME,
        DATE,
        RECENTLY_PLAYED,
    }

    // 現在のソートモードを適用するメソッド
    private fun applySortMode() {
        if (::gameAdapter.isInitialized) {
            when (currentSortMode) {
                SortMode.NAME -> gameAdapter.sortByName()
                SortMode.DATE -> gameAdapter.sortByDate()
                SortMode.RECENTLY_PLAYED -> gameAdapter.sortByRecentlyPlayed()
            }
        }
    }

    private fun saveSortMode() {
        requireContext()
            .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_SORT_MODE, currentSortMode.name)
            .apply()
    }

    private fun loadSortMode(): SortMode {
        val prefs = requireContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val saved = prefs.getString(KEY_SORT_MODE, SortMode.NAME.name)
        return try {
            SortMode.valueOf(saved ?: SortMode.NAME.name)
        } catch (e: IllegalArgumentException) {
            SortMode.NAME
        }
    }
}
