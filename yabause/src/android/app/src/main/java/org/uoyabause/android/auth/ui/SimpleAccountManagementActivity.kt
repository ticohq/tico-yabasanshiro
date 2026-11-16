package org.uoyabause.android.auth.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.text.TextUtils
import android.util.Log
import android.util.TypedValue
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.annotation.VisibleForTesting
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.Toolbar
import androidx.coordinatorlayout.widget.CoordinatorLayout
import androidx.core.view.ViewCompat
import com.bumptech.glide.Glide
import com.bumptech.glide.load.resource.bitmap.CircleCrop
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.AuthUI.IdpConfig.AppleBuilder
import com.firebase.ui.auth.AuthUI.IdpConfig.GoogleBuilder
import com.firebase.ui.auth.IdpResponse
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import com.google.firebase.auth.FirebaseAuth
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.YabauseApplication
import org.uoyabause.android.auth.DiscordLinkActivity
import org.uoyabause.android.auth.models.AccountConnectionState
import org.uoyabause.android.auth.models.AccountUiState
import org.uoyabause.android.auth.models.ConnectedAccountsState
import org.uoyabause.android.auth.models.UserProfile
import org.uoyabause.android.auth.viewmodel.AccountManagementViewModel
import java.text.SimpleDateFormat
import java.util.Arrays
import java.util.Date
import java.util.Locale

/**
 * Account Management Activity
 *
 * Unified account management screen for Firebase, Discord, and RetroAchievements integration.
 * Provides user-friendly account management features with Material Design 3 compliant UI.
 *
 * ## Main Features
 * - Firebase authentication (Google/Apple) sign-in/sign-out
 * - Discord OAuth2.0 account linking/unlinking
 * - RetroAchievements account login/logout
 * - Cross-device sync PIN code generation
 * - User data export (GDPR compliance)
 * - Account deletion functionality
 *
 * ## UI/UX Features
 * - Animation effects (fade, scale, focus)
 * - Keyboard and gamepad navigation support
 * - Dynamic system font scale adaptation
 * - Comprehensive error handling and user feedback
 *
 * @since 2025-08-13
 * @see AccountManagementViewModel
 * @see AccountRepository
 */
@Deprecated("Use AccountManagementFragment instead. This Activity will be removed in a future release.")
class SimpleAccountManagementActivity : AppCompatActivity() {
    private val viewModel: AccountManagementViewModel by viewModels()

    // Firebase Auth launcher
    private lateinit var signInLauncher: ActivityResultLauncher<Intent>

    // Callback for PIN generation after sign-in
    @VisibleForTesting
    internal var pinGenerationCallback: ((IdpResponse?) -> Unit)? = null

    // Callback for RetroAchievements login after Firebase sign-in
    @VisibleForTesting
    internal var pendingRALoginCallback: (() -> Unit)? = null

    // File save launcher for data export
    private lateinit var exportDataLauncher: ActivityResultLauncher<String>

    // Root view for showing Snackbars
    private lateinit var rootView: CoordinatorLayout

    // Loading timeout handler
    private var loadingTimeoutHandler: android.os.Handler? = null
    private val loadingTimeoutMs = 30000L // 30 seconds

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 画面遷移のフェードアニメーション設定
        setActivityTransition(android.R.anim.fade_in, android.R.anim.fade_out)
        setContentView(R.layout.activity_account_management)

        rootView = findViewById(R.id.root_layout)

        setupSignInLauncher()
        setupExportDataLauncher()
        setupToolbar()
        setupObservers()
        setupClickListeners()
        setupFontSizeSupport()
    }

    override fun onResume() {
        super.onResume()

        // Check for user switch and refresh data when activity resumes
        // This ensures UI is updated after auto-login or user changes
        viewModel.refresh()
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)

        // フォントサイズが変更された場合の処理
        setupFontSizeSupport()

        Log.d("SimpleAccountManagement", "Configuration changed, new font scale: ${newConfig.fontScale}")
    }

    private val signInResultHandler = SignInResultHandler()

    private fun setupSignInLauncher() {
        signInLauncher =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
                handleSignInResult(
                    resultCode = result.resultCode,
                    data = result.data,
                    currentUser = FirebaseAuth.getInstance().currentUser,
                )
            }
    }

    @VisibleForTesting
    internal fun handleSignInResult(
        resultCode: Int,
        data: Intent?,
        currentUser: com.google.firebase.auth.FirebaseUser?,
    ) {
        val action = signInResultHandler.handleResult(
            resultCode = resultCode,
            data = data,
            currentUser = currentUser,
            hasPinCallback = pinGenerationCallback != null,
            hasRACallback = pendingRALoginCallback != null,
        )

        when (action) {
            is SignInResultHandler.Result.PinCallback -> {
                pinGenerationCallback?.invoke(action.response)
                pinGenerationCallback = null
            }
            is SignInResultHandler.Result.RALoginSuccess -> {
                val callback = pendingRALoginCallback
                pendingRALoginCallback = null
                viewModel.refresh()
                callback?.invoke()
            }
            is SignInResultHandler.Result.Cancelled -> {
                pendingRALoginCallback = null
                showUserFriendlyError(
                    message = getString(R.string.sign_in_cancelled_message),
                    errorType = ErrorType.INFO,
                )
            }
            is SignInResultHandler.Result.SignInSuccess -> {
                val welcomeMessage = getString(R.string.welcome_user, action.displayName)
                showUserFriendlyError(
                    message = welcomeMessage,
                    errorType = ErrorType.SUCCESS,
                )
                announceToAccessibilityService(welcomeMessage)
                viewModel.refresh()
            }
            is SignInResultHandler.Result.SignInFailed -> {
                val errorMessage = action.errorMessage ?: "Unknown error"
                if (action.errorMessage != null) {
                    handleConnectionError(Exception(errorMessage), "firebase")
                } else {
                    showUserFriendlyError(
                        message = getString(R.string.sign_in_failed_message, errorMessage),
                        errorType = ErrorType.ERROR,
                        showRetry = true,
                        retryAction = { launchFirebaseSignIn() },
                    )
                }
            }
            is SignInResultHandler.Result.NoAction -> {
                pendingRALoginCallback = null
            }
        }
    }

    private fun setupExportDataLauncher() {
        exportDataLauncher =
            registerForActivityResult(
                ActivityResultContracts.CreateDocument("application/json"),
            ) { uri ->
                uri?.let { saveExportDataToFile(it) }
            }
    }

    private fun setupToolbar() {
        val toolbar = findViewById<Toolbar>(R.id.toolbar)
        setSupportActionBar(toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        supportActionBar?.setDisplayShowHomeEnabled(true)
        supportActionBar?.title = getString(R.string.account_management)

        toolbar.setNavigationOnClickListener {
            finish()
        }
    }

    private fun setupObservers() {
        viewModel.uiState.observe(this) { state ->
            when (state) {
                is AccountUiState.Loading -> showLoading()
                is AccountUiState.Success -> showSuccess(state)
                is AccountUiState.Error -> showError(state.message)
                is AccountUiState.NotAuthenticated -> showNotAuthenticated()
            }
        }

        viewModel.message.observe(this) { message ->
            message?.let {
                // Use improved error display system for ViewModel messages
                showUserFriendlyError(
                    message = it,
                    errorType =
                        if (it.contains("error", ignoreCase = true) ||
                            it.contains("failed", ignoreCase = true)
                        ) {
                            ErrorType.ERROR
                        } else {
                            ErrorType.INFO
                        },
                )
                announceToAccessibilityService(it)
                viewModel.clearMessage()
            }
        }

        // Observe exported data for GDPR compliance
        viewModel.exportedData.observe(this) { exportData ->
            exportData?.let {
                // Data is ready, now prompt user to save file
                val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
                val fileName = "YabaSanshiro_UserData_$timestamp.json"
                exportDataLauncher.launch(fileName)
            }
        }

        // 初期データロード
        viewModel.refresh()
    }

    private fun setupClickListeners() {
        // 基本的なクリックリスナーのみ設定
        // 詳細なカード更新ロジックは段階的に実装
    }

    @Suppress("DEPRECATION")
    private fun announceToAccessibilityService(message: String) {
        // Using announceForAccessibility which is the recommended approach
        // The @Suppress is added because the underlying View method triggers a deprecation
        // warning in some lint configurations, but it's still the correct way to announce
        rootView.announceForAccessibility(message)
    }

    /**
     * Activity遷移アニメーション設定のラッパー
     * API 34以上では overrideActivityTransition() を使用、それ以前は overridePendingTransition() を使用
     */
    @Suppress("DEPRECATION")
    private fun setActivityTransition(
        enterAnim: Int,
        exitAnim: Int,
    ) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            overrideActivityTransition(OVERRIDE_TRANSITION_OPEN, enterAnim, exitAnim)
        } else {
            overridePendingTransition(enterAnim, exitAnim)
        }
    }

    /**
     * Activity終了時のアニメーション設定のラッパー
     * API 34以上では overrideActivityTransition() を使用、それ以前は overridePendingTransition() を使用
     */
    @Suppress("DEPRECATION")
    private fun setActivityCloseTransition(
        enterAnim: Int,
        exitAnim: Int,
    ) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            overrideActivityTransition(OVERRIDE_TRANSITION_CLOSE, enterAnim, exitAnim)
        } else {
            overridePendingTransition(enterAnim, exitAnim)
        }
    }

    private fun enhanceAccessibilityForView(
        view: View,
        contentDescription: String? = null,
        isHeading: Boolean = false,
    ) {
        contentDescription?.let { view.contentDescription = it }
        if (isHeading) {
            ViewCompat.setAccessibilityHeading(view, true)
        }
        view.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
    }

    /**
     * ボタンクリック時のアニメーション効果を追加
     * タップ時に少し縮小し、リリース時に元のサイズに戻る
     */
    private fun animateButtonClick(
        view: View,
        action: () -> Unit,
    ) {
        view
            .animate()
            .scaleX(0.92f)
            .scaleY(0.92f)
            .setDuration(80)
            .withEndAction {
                view
                    .animate()
                    .scaleX(1f)
                    .scaleY(1f)
                    .setDuration(80)
                    .withEndAction {
                        action()
                    }.start()
            }.start()
    }

    /**
     * キーボードやゲームパッドナビゲーション時のフォーカスアニメーション
     * フォーカス取得時に拡大し、輝度を上げて注目を集める
     */
    private fun setupFocusAnimations(view: View) {
        // 元のテキスト色を保存するためのタグを設定
        if (view is Button) {
            view.setTag(R.id.original_text_color, view.currentTextColor)
        }

        view.setOnFocusChangeListener { v, hasFocus ->
            if (hasFocus) {
                // ゲームパッド/キーボードナビゲーション時のサウンドフィードバック
                playFocusSound()

                // フォーカス取得時のアニメーション
                v
                    .animate()
                    .scaleX(1.05f)
                    .scaleY(1.05f)
                    .alpha(1.0f)
                    .setDuration(150)
                    .setInterpolator(android.view.animation.DecelerateInterpolator())
                    .start()

                // フォーカス時の背景色とテキスト色変更（押下可能な要素のみ）
                if (v is Button || v.isClickable) {
                    v.backgroundTintList = androidx.core.content.ContextCompat
                        .getColorStateList(this, R.color.colorAccent)
                    // ボタンのテキスト色を統一（白色で見やすく）
                    if (v is Button) {
                        v.setTextColor(
                            androidx.core.content.ContextCompat
                                .getColor(this, R.color.colorPrimaryDark),
                        )
                    }
                }

                // フォーカスされたビューを画面内に表示するためのスクロール
                scrollToView(v)

                // アクセシビリティ通知
                val contentDescription =
                    v.contentDescription
                        ?: when (v) {
                            is Button -> v.text
                            is TextView -> v.text
                            else -> getString(R.string.focused_element)
                        }
                announceToAccessibilityService(getString(R.string.focused_format, contentDescription))
            } else {
                // フォーカス失効時のアニメーション
                v
                    .animate()
                    .scaleX(1.0f)
                    .scaleY(1.0f)
                    .alpha(0.9f)
                    .setDuration(150)
                    .setInterpolator(android.view.animation.AccelerateInterpolator())
                    .start()

                // 背景色とテキスト色を元に戻す
                if (v is Button || v.isClickable) {
                    v.backgroundTintList = null
                    // ボタンのテキスト色を元に戻す
                    if (v is Button) {
                        val originalColor = v.getTag(R.id.original_text_color) as? Int
                        if (originalColor != null) {
                            v.setTextColor(originalColor)
                        }
                    }
                }
            }
        }

        // ゲームパッドのA/Enterボタンで実行されるキーリスナーを追加
        view.setOnKeyListener { v, keyCode, event ->
            if (event.action == android.view.KeyEvent.ACTION_DOWN) {
                when (keyCode) {
                    android.view.KeyEvent.KEYCODE_DPAD_CENTER,
                    android.view.KeyEvent.KEYCODE_ENTER,
                    android.view.KeyEvent.KEYCODE_NUMPAD_ENTER,
                    android.view.KeyEvent.KEYCODE_BUTTON_A,
                    -> {
                        // ゲームパッドのAボタン/Enterキーが押された時
                        if (v.isClickable) {
                            v.performClick()
                            true
                        } else {
                            false
                        }
                    }
                    else -> false
                }
            } else {
                false
            }
        }
    }

    /**
     * フォーカス時のサウンドフィードバック（ゲームパッド/キーボードナビゲーション用）
     */
    private fun playFocusSound() {
        try {
            val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
            // システムサウンドエフェクトを再生（音量設定に従う）
            audioManager.playSoundEffect(AudioManager.FX_FOCUS_NAVIGATION_UP)
        } catch (e: Exception) {
            // サウンド再生に失敗してもアプリを停止させない
            Log.d("SimpleAccountManagement", "Failed to play focus sound: ${e.message}")
        }
    }

    /**
     * フォーカスされたビューを画面内に表示するためのスムーズスクロール
     */
    private fun scrollToView(view: View) {
        // main_contentの親であるNestedScrollViewを検索
        var parent: android.view.ViewParent? = view.parent
        var scrollView: androidx.core.widget.NestedScrollView? = null

        // ビューの親を遡ってNestedScrollViewを見つける
        while (parent != null) {
            if (parent is androidx.core.widget.NestedScrollView) {
                scrollView = parent
                break
            }
            parent = parent.parent
        }

        if (scrollView != null) {
            // 少し遅延を入れてレイアウトが完了してからスクロール
            view.post {
                try {
                    // ビューの位置を計算
                    val viewRect = android.graphics.Rect()
                    view.getDrawingRect(viewRect)
                    scrollView.offsetDescendantRectToMyCoords(view, viewRect)

                    // 現在のスクロール位置を取得
                    val scrollY = scrollView.scrollY
                    val scrollViewHeight = scrollView.height

                    // DP単位でマージンを計算（画面密度に対応）
                    val density = resources.displayMetrics.density
                    val visibilityMarginDp = 120 // DP単位での必要な余白
                    val scrollMarginDp = 150 // DP単位でのスクロール時のマージン
                    val visibilityMargin = (visibilityMarginDp * density).toInt()
                    val scrollMargin = (scrollMarginDp * density).toInt()

                    // より寛容なビジビリティチェック（マージンを考慮）
                    val isViewFullyVisible =
                        viewRect.top >= scrollY + visibilityMargin &&
                            viewRect.bottom <= scrollY + scrollViewHeight - visibilityMargin

                    if (!isViewFullyVisible) {
                        // ビューが十分に見えない場合、余裕のある位置にスムーズスクロール
                        val targetScrollY =
                            when {
                                // ビューが上にある場合、または上部マージンが不足している場合
                                viewRect.top < scrollY + visibilityMargin -> {
                                    maxOf(0, viewRect.top - scrollMargin)
                                }
                                // ビューが下にある場合、または下部マージンが不足している場合
                                viewRect.bottom > scrollY + scrollViewHeight - visibilityMargin -> {
                                    viewRect.bottom - scrollViewHeight + scrollMargin
                                }
                                else -> scrollY
                            }

                        // スムーズスクロールを実行
                        scrollView.smoothScrollTo(0, targetScrollY)

                        Log.d(
                            "SimpleAccountManagement",
                            "Scrolling to view: ${view.javaClass.simpleName}, " +
                                "targetY: $targetScrollY, viewTop: ${viewRect.top}, viewBottom: ${viewRect.bottom}",
                        )
                    }
                } catch (e: Exception) {
                    Log.e("SimpleAccountManagement", "Failed to scroll to view: ${e.message}")
                }
            }
        }
    }

    private fun showRetroAchievementsLoginDialog() {
        // Check if Firebase user is logged in first
        if (FirebaseAuth.getInstance().currentUser == null) {
            showFirebaseLoginRequiredDialog()
            return
        }

        val dialogView = layoutInflater.inflate(R.layout.dialog_ra_login, null)
        val usernameEdit = dialogView.findViewById<EditText>(R.id.username_input)
        val passwordEdit = dialogView.findViewById<EditText>(R.id.api_key_input)

        showUnifiedErrorDialog(
            title = getString(R.string.retroachievements_login_dialog_title),
            view = dialogView,
            positiveButtonText = getString(R.string.login),
            onPositiveClick = {
                val username = usernameEdit.text.toString()
                val password = passwordEdit.text.toString()
                if (username.isNotEmpty() && password.isNotEmpty()) {
                    viewModel.loginRetroAchievements(username, password)
                    // Refresh stats after a short delay to allow login to complete
                    android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                        viewModel.refreshRetroAchievementsStats()
                    }, 2000)
                } else {
                    showUserFriendlyError(
                        message = getString(R.string.please_enter_username_password),
                        errorType = ErrorType.WARNING,
                    )
                }
            },
            negativeButtonText = getString(R.string.cancel),
        )
    }

    /**
     * Show dialog prompting user to sign in to Firebase before using RetroAchievements
     * After successful Firebase sign-in, automatically shows the RA login dialog
     */
    private fun showFirebaseLoginRequiredDialog() {
        showUnifiedErrorDialog(
            title = getString(R.string.firebase_login_required_title),
            message = getString(R.string.firebase_login_required_for_ra_message),
            positiveButtonText = getString(R.string.sign_in),
            onPositiveClick = {
                // Set callback to show RA login dialog after successful Firebase sign-in
                pendingRALoginCallback = { showRetroAchievementsLoginDialog() }
                launchFirebaseSignIn()
            },
            negativeButtonText = getString(R.string.cancel),
            errorType = ErrorType.WARNING,
            showIcon = true,
        )
    }

    private fun showLoading() {
        findViewById<View>(R.id.loading_layout).visibility = View.VISIBLE
        findViewById<View>(R.id.error_layout).visibility = View.GONE
        findViewById<View>(R.id.main_content).visibility = View.GONE
        announceToAccessibilityService(getString(R.string.loading))

        // Start loading timeout timer
        startLoadingTimeout()
    }

    // エラー表示改善: ローディング中のエラーハンドリング

    private fun startLoadingTimeout() {
        // Clear any existing timeout
        loadingTimeoutHandler?.removeCallbacksAndMessages(null)

        loadingTimeoutHandler = android.os.Handler(android.os.Looper.getMainLooper())
        loadingTimeoutHandler?.postDelayed({
            // Check if still loading
            if (findViewById<View>(R.id.loading_layout).visibility == View.VISIBLE) {
                // Loading has timed out
                showLoadingTimeoutError()
            }
        }, loadingTimeoutMs)
    }

    private fun stopLoadingTimeout() {
        loadingTimeoutHandler?.removeCallbacksAndMessages(null)
        loadingTimeoutHandler = null
    }

    private fun showLoadingTimeoutError() {
        showUnifiedErrorDialog(
            title = getString(R.string.loading_timeout_title),
            message = getString(R.string.loading_timeout_message),
            positiveButtonText = getString(R.string.retry),
            onPositiveClick = {
                viewModel.refresh()
            },
            neutralButtonText = getString(R.string.cancel_loading),
            onNeutralClick = {
                // Go back to not authenticated state
                showNotAuthenticated()
            },
            negativeButtonText = getString(R.string.dismiss),
            errorType = ErrorType.WARNING,
            showIcon = true,
        )
    }

    private fun showSuccess(state: AccountUiState.Success) {
        // Stop loading timeout when success is shown
        stopLoadingTimeout()

        findViewById<View>(R.id.loading_layout).visibility = View.GONE
        findViewById<View>(R.id.error_layout).visibility = View.GONE
        findViewById<View>(R.id.main_content).visibility = View.VISIBLE
        updateAccountCards(state.userProfile, state.connectedAccounts)

        // フォントサイズ対応を動的に適用
        val fontScale = resources.configuration.fontScale
        if (fontScale > 1.3f) {
            adjustLayoutForLargeFonts()
        }

        announceToAccessibilityService(getString(R.string.account_management_title))
    }

    private fun updateAccountCards(
        userProfile: UserProfile?,
        connectedAccounts: ConnectedAccountsState,
    ) {
        // Update all account cards with actual data
        updateUserProfileCard(userProfile)
        updateDiscordCard(connectedAccounts.discord)
        updateRetroAchievementsCard(connectedAccounts.retroAchievements)
        setupAllClickListeners(connectedAccounts)
    }

    private fun updateUserProfileCard(userProfile: UserProfile?) {
        try {
            val userProfileCard = findViewById<View>(R.id.user_profile_card)
            val userAvatar = userProfileCard.findViewById<com.google.android.material.imageview.ShapeableImageView>(R.id.user_avatar)
            val userDisplayName = userProfileCard.findViewById<TextView>(R.id.user_display_name)
            val userEmail = userProfileCard.findViewById<TextView>(R.id.user_email)
            val lastLoginTime = userProfileCard.findViewById<TextView>(R.id.last_login_time)
            val accountActionsSection = userProfileCard.findViewById<View>(R.id.account_actions_section)
            val notSignedInSection = userProfileCard.findViewById<View>(R.id.not_signed_in_section)

            if (userProfile != null) {
                // User is signed in
                userDisplayName.text = userProfile.displayName ?: getString(R.string.unknown_user)
                userEmail.text = userProfile.email ?: getString(R.string.no_email)
                lastLoginTime.text = getString(R.string.last_login_format, formatLastLoginTime(userProfile.lastLoginTime))

                // Load user avatar
                if (!userProfile.photoUrl.isNullOrEmpty()) {
                    Glide
                        .with(this)
                        .load(userProfile.photoUrl)
                        .transform(CircleCrop())
                        .placeholder(R.drawable.ic_account_circle_24)
                        .error(R.drawable.ic_account_circle_24)
                        .into(userAvatar)
                } else {
                    userAvatar.setImageResource(R.drawable.ic_account_circle_24)
                }

                // Show sign out button, hide sign in button
                accountActionsSection?.visibility = View.VISIBLE
                notSignedInSection?.visibility = View.GONE

                // Sign out button in card
                val signOutButton = userProfileCard.findViewById<Button>(R.id.user_sign_out_button)
                signOutButton?.setOnClickListener {
                    animateButtonClick(it) {
                        org.uoyabause.android.auth.FirebaseAuthManager(this).signOut(this) {
                            viewModel.refresh()
                        }
                    }
                }

                // Privacy links in card
                userProfileCard.findViewById<TextView>(R.id.privacy_policy_link)?.setOnClickListener {
                    val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://www.yabasanshiro.com/privacy"))
                    startActivity(intent)
                    setActivityTransition(android.R.anim.fade_in, android.R.anim.fade_out)
                }
                userProfileCard.findViewById<TextView>(R.id.terms_of_service_link)?.setOnClickListener {
                    val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://www.yabasanshiro.com/terms-of-use"))
                    startActivity(intent)
                    setActivityTransition(android.R.anim.fade_in, android.R.anim.fade_out)
                }
            } else {
                // User is not signed in
                userDisplayName.text = getString(R.string.not_signed_in)
                userEmail.text = getString(R.string.please_sign_in_to_continue)
                lastLoginTime.text = getString(R.string.no_login_data)

                // Show default avatar
                userAvatar.setImageResource(R.drawable.ic_account_circle_24)

                // Show sign in button, hide sign out button
                accountActionsSection?.visibility = View.GONE
                notSignedInSection?.visibility = View.VISIBLE

                // Setup sign in button
                val signInButton = userProfileCard.findViewById<Button>(R.id.user_sign_in_button)
                signInButton?.setOnClickListener {
                    animateButtonClick(it) {
                        launchFirebaseSignIn()
                    }
                }

                // Privacy links are now in the footer section
            }
        } catch (e: Exception) {
            // Fallback if Views not found
            Log.e("SimpleAccountManagement", "Failed to update user profile card: ${e.message}")
            showUserFriendlyError(
                message = getString(R.string.failed_to_update_profile),
                errorType = ErrorType.WARNING,
                showRetry = true,
                retryAction = { viewModel.refresh() },
            )
        }
    }

    private fun updateDiscordCard(discordState: AccountConnectionState) {
        try {
            val discordCard = findViewById<View>(R.id.discord_account_card)
            val discordStatus = discordCard.findViewById<TextView>(R.id.discord_status)
            val discordStatusIndicator = discordCard.findViewById<View>(R.id.discord_status_indicator)
            val discordConnectedContent = discordCard.findViewById<View>(R.id.discord_connected_content)
            val discordDisconnectedContent = discordCard.findViewById<View>(R.id.discord_disconnected_content)

            if (discordState.isConnected) {
                discordStatus.text = getString(R.string.connected)
                discordStatusIndicator.backgroundTintList =
                    androidx.core.content.ContextCompat
                        .getColorStateList(this, android.R.color.holo_green_light)
                discordConnectedContent.visibility = View.VISIBLE
                discordDisconnectedContent.visibility = View.GONE

                // Update Discord user info
                val discordUsername = discordCard.findViewById<TextView>(R.id.discord_username)
                val discordDisplayName = discordCard.findViewById<TextView>(R.id.discord_display_name)
                val discordAvatar = discordCard.findViewById<com.google.android.material.imageview.ShapeableImageView>(R.id.discord_avatar)

                discordUsername.text = discordState.username ?: getString(R.string.discord_user)
                discordDisplayName.text = discordState.displayName ?: discordState.username

                // Load Discord avatar
                if (!discordState.avatarUrl.isNullOrEmpty()) {
                    Glide
                        .with(this)
                        .load(discordState.avatarUrl)
                        .transform(CircleCrop())
                        .placeholder(R.drawable.ic_discord_24)
                        .error(R.drawable.ic_discord_24)
                        .into(discordAvatar)
                    Log.d("SimpleAccountManagement", "Loading Discord avatar: ${discordState.avatarUrl}")
                } else {
                    discordAvatar.setImageResource(R.drawable.ic_discord_24)
                }
            } else {
                discordStatus.text = getString(R.string.not_connected)
                discordStatusIndicator.backgroundTintList =
                    androidx.core.content.ContextCompat
                        .getColorStateList(this, android.R.color.holo_red_light)
                discordConnectedContent.visibility = View.GONE
                discordDisconnectedContent.visibility = View.VISIBLE
            }
        } catch (e: Exception) {
            // Handle error silently
        }
    }

    private fun updateRetroAchievementsCard(raState: AccountConnectionState) {
        try {
            val raCard = findViewById<View>(R.id.retroachievements_account_card)
            val raStatus = raCard.findViewById<TextView>(R.id.ra_status)
            val raStatusIndicator = raCard.findViewById<View>(R.id.ra_status_indicator)
            val raConnectedContent = raCard.findViewById<View>(R.id.ra_logged_in_content)
            val raDisconnectedContent = raCard.findViewById<View>(R.id.ra_not_logged_in_content)

            if (raState.isConnected) {
                raStatus.text = getString(R.string.connected)
                raStatusIndicator.backgroundTintList =
                    androidx.core.content.ContextCompat
                        .getColorStateList(this, android.R.color.holo_green_light)
                raConnectedContent.visibility = View.VISIBLE
                raDisconnectedContent.visibility = View.GONE

                // Update RetroAchievements user info
                val raUsername = raCard.findViewById<TextView>(R.id.ra_username)
                raUsername.text = raState.username ?: getString(R.string.retroachievements_user)

                // Update stats if available
                raState.additionalInfo?.let { info ->
                    val hardcoreScoreText = raCard.findViewById<TextView>(R.id.ra_points)
                    val softcoreScoreText = raCard.findViewById<TextView>(R.id.ra_achievements)
                    val unreadMessagesText = raCard.findViewById<TextView>(R.id.ra_games_completed)

                    hardcoreScoreText?.text = info["hardcoreScore"]?.toString() ?: "0"
                    softcoreScoreText?.text = info["softcoreScore"]?.toString() ?: "0"
                    unreadMessagesText?.text = info["unreadMessages"]?.toString() ?: "0"

                    // Update avatar if available
                    val avatarUrl = info["avatarUrl"] as? String
                    val raAvatar = raCard.findViewById<com.google.android.material.imageview.ShapeableImageView>(R.id.ra_avatar)
                    if (!avatarUrl.isNullOrEmpty() && raAvatar != null) {
                        Glide
                            .with(this)
                            .load(avatarUrl)
                            .transform(CircleCrop())
                            .placeholder(R.drawable.ic_account_circle_24)
                            .error(R.drawable.ic_account_circle_24)
                            .into(raAvatar)
                        Log.d("SimpleAccountManagement", "Loading RetroAchievements avatar: $avatarUrl")
                    } else {
                        raAvatar?.setImageResource(R.drawable.ic_account_circle_24)
                    }
                }
            } else {
                raStatus.text = getString(R.string.not_connected)
                raStatusIndicator.backgroundTintList =
                    androidx.core.content.ContextCompat
                        .getColorStateList(this, android.R.color.holo_red_light)
                raConnectedContent.visibility = View.GONE
                raDisconnectedContent.visibility = View.VISIBLE
            }
        } catch (e: Exception) {
            // Handle error silently
        }
    }

    private fun setupAllClickListeners(connectedAccounts: ConnectedAccountsState) {
        // Discord buttons
        setupDiscordClickListeners(connectedAccounts.discord)

        // RetroAchievements buttons
        setupRetroAchievementsClickListeners(connectedAccounts.retroAchievements)

        // Cross-device sync button
        setupCrossDeviceSyncClickListener()

        // Export and delete buttons
        setupAccountActionsClickListeners()

        // フォーカスアニメーションの設定
        setupAllFocusAnimations()

        // ゲームパッド対応のためのフォーカス順序設定（アカウント状態に基づいて動的に更新）
        setupGamepadNavigation(connectedAccounts)
    }

    /**
     * すべてのフォーカス可能な要素にフォーカスアニメーションを設定
     */
    private fun setupAllFocusAnimations() {
        // ユーザープロファイルカードのボタン
        findViewById<Button>(R.id.user_sign_in_button)?.let { setupFocusAnimations(it) }
        findViewById<Button>(R.id.user_sign_out_button)?.let { setupFocusAnimations(it) }

        // Discordカードのボタン
        findViewById<Button>(R.id.discord_link_button)?.let { setupFocusAnimations(it) }
        findViewById<Button>(R.id.discord_unlink_button)?.let { setupFocusAnimations(it) }
        findViewById<Button>(R.id.discord_create_account_button)?.let { setupFocusAnimations(it) }

        // RetroAchievementsカードのボタン
        findViewById<Button>(R.id.ra_login_button)?.let { setupFocusAnimations(it) }
        findViewById<Button>(R.id.ra_logout_button)?.let { setupFocusAnimations(it) }
        findViewById<Button>(R.id.ra_create_account_button)?.let { setupFocusAnimations(it) }

        // PINセクションのボタン
        findViewById<Button>(R.id.generate_pin_button)?.let { setupFocusAnimations(it) }
        findViewById<Button>(R.id.refresh_pin_button)?.let { setupFocusAnimations(it) }

        // PINテキスト（生成後に表示される）
        findViewById<TextView>(R.id.pin_code_text)?.let {
            setupFocusAnimations(it)
        }

        // アカウントアクションボタン
        findViewById<Button>(R.id.export_data_button)?.let { setupFocusAnimations(it) }
        findViewById<Button>(R.id.delete_account_button)?.let { setupFocusAnimations(it) }

        // エラー画面のサインインボタン
        findViewById<Button>(R.id.sign_in_button)?.let { setupFocusAnimations(it) }

        // クリック可能なテキストリンク（カード内）
        findViewById<TextView>(R.id.privacy_policy_link)?.let {
            setupFocusAnimations(it)
            it.isFocusable = true
        }
        findViewById<TextView>(R.id.terms_of_service_link)?.let {
            setupFocusAnimations(it)
            it.isFocusable = true
        }

        // ツールバーのナビゲーションボタン
        findViewById<Toolbar>(R.id.toolbar)?.let { toolbar ->
            toolbar.isFocusable = true
            setupFocusAnimations(toolbar)
        }
    }

    private fun isViewEffectivelyVisible(v: View?): Boolean {
        if (v == null) return false
        var current: View? = v
        while (current != null) {
            if (current.visibility == View.GONE) return false
            val parent = current.parent
            if (parent !is View) break
            current = parent
        }
        return true
    }

    private fun setupGamepadNavigation(connectedAccounts: ConnectedAccountsState) {
        val navChain = mutableListOf<View>()

        fun addIfVisible(v: View?) {
            if (v != null && isViewEffectivelyVisible(v)) {
                v.isFocusable = true
                navChain.add(v)
            }
        }

        // 1. Sign Out
        addIfVisible(findViewById(R.id.user_sign_out_button))

        // 2. Privacy Policy
        addIfVisible(findViewById(R.id.privacy_policy_link))

        // 3. Terms of Service
        addIfVisible(findViewById(R.id.terms_of_service_link))

        // 4-5. Discord: Create Account → Link/Unlink
        addIfVisible(findViewById(R.id.discord_create_account_button))
        if (connectedAccounts.discord.isConnected) {
            addIfVisible(findViewById(R.id.discord_unlink_button))
        } else {
            addIfVisible(findViewById(R.id.discord_link_button))
        }

        // 6-7. RetroAchievements: Create Account → Login/Logout
        addIfVisible(findViewById(R.id.ra_create_account_button))
        if (connectedAccounts.retroAchievements.isConnected) {
            addIfVisible(findViewById(R.id.ra_logout_button))
        } else {
            addIfVisible(findViewById(R.id.ra_login_button))
        }

        // 8. Generate PIN
        addIfVisible(findViewById(R.id.generate_pin_button))

        // 9. Delete Account
        addIfVisible(findViewById(R.id.delete_account_button))

        // Disable focus on Google provider chip
        findViewById<View>(R.id.account_provider_chip)?.apply {
            isFocusable = false
            isClickable = false
        }

        // Set up linear up/down chain
        for (i in navChain.indices) {
            val current = navChain[i]
            current.nextFocusUpId = if (i > 0) navChain[i - 1].id else View.NO_ID
            current.nextFocusDownId = if (i < navChain.size - 1) navChain[i + 1].id else View.NO_ID
        }

        // Set initial focus to Sign Out button
        if (navChain.isNotEmpty()) {
            navChain[0].post { navChain[0].requestFocus() }
        }
    }

    private fun setupDiscordClickListeners(discordState: AccountConnectionState) {
        val discordCard = findViewById<View>(R.id.discord_account_card)

        if (discordState.isConnected) {
            val unlinkButton = discordCard.findViewById<Button>(R.id.discord_unlink_button)
            unlinkButton?.setOnClickListener {
                animateButtonClick(it) {
                    viewModel.unlinkDiscord()
                }
            }
        } else {
            val linkButton = discordCard.findViewById<Button>(R.id.discord_link_button)
            linkButton?.setOnClickListener {
                animateButtonClick(it) {
                    // Launch DiscordLinkActivity instead of using viewModel
                    val intent = Intent(this, DiscordLinkActivity::class.java)
                    startActivity(intent)
                    setActivityTransition(android.R.anim.fade_in, android.R.anim.fade_out)
                }
            }

            val createAccountButton = discordCard.findViewById<Button>(R.id.discord_create_account_button)
            createAccountButton?.setOnClickListener {
                animateButtonClick(it) {
                    openUrl(discordState.createAccountUrl ?: "https://discord.com/register")
                }
            }
        }
    }

    private fun setupRetroAchievementsClickListeners(raState: AccountConnectionState) {
        val raCard = findViewById<View>(R.id.retroachievements_account_card)

        if (raState.isConnected) {
            val logoutButton = raCard.findViewById<Button>(R.id.ra_logout_button)
            logoutButton?.setOnClickListener {
                animateButtonClick(it) {
                    viewModel.logoutRetroAchievements()
                }
            }
        } else {
            val loginButton = raCard.findViewById<Button>(R.id.ra_login_button)
            loginButton?.setOnClickListener {
                animateButtonClick(it) {
                    showRetroAchievementsLoginDialog()
                }
            }

            val createAccountButton = raCard.findViewById<Button>(R.id.ra_create_account_button)
            createAccountButton?.setOnClickListener {
                animateButtonClick(it) {
                    openUrl(raState.createAccountUrl ?: "https://retroachievements.org/createaccount.php")
                }
            }
        }
    }

    private fun setupCrossDeviceSyncClickListener() {
        val generatePinButton = findViewById<Button>(R.id.generate_pin_button)
        generatePinButton?.setOnClickListener {
            if (!YabauseApplication.isPro()) {
                YabauseApplication.checkDonated(this)
                return@setOnClickListener
            }
            animateButtonClick(it) {
                // Trigger sign-in first, then generate PIN with the fresh idpToken
                launchSignInForPinGeneration()
            }
        }

        // Observe PIN generation
        viewModel.generatedPin.observe(this) { pin ->
            pin?.let {
                showGeneratedPin(it)
            }
        }
    }

    /**
     * Launch sign-in specifically for PIN generation
     * This ensures we get a fresh idpToken (valid for 1 hour)
     */
    private fun launchSignInForPinGeneration() {
        // Set the callback for PIN generation
        pinGenerationCallback = { response ->
            if (response != null) {
                // Pass the IdpResponse to the repository for PIN generation
                viewModel.generateDevicePINWithResponse(response)
            } else {
                showUserFriendlyError(
                    message = getString(R.string.sign_in_required_for_pin),
                    errorType = ErrorType.WARNING,
                    actionText = getString(R.string.sign_in),
                    action = { launchFirebaseSignIn() },
                )
            }
        }

        // Launch Firebase sign-in
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

        signInLauncher.launch(PcSignInActivity.wrap(this, signInIntent))
    }

    private fun setupAccountActionsClickListeners() {
        val exportButton = findViewById<Button>(R.id.export_data_button)
        exportButton?.setOnClickListener {
            animateButtonClick(it) {
                showGDPRExportDialog()
            }
        }

        val deleteButton = findViewById<Button>(R.id.delete_account_button)
        deleteButton?.setOnClickListener {
            animateButtonClick(it) {
                showDeleteAccountConfirmationDialog()
            }
        }
    }

    private fun showGeneratedPin(pin: String) {
        if (!YabauseApplication.isPro()) {
            viewModel.clearGeneratedPin()
            return
        }
        val pinDisplaySection = findViewById<View>(R.id.pin_display_section)
        val pinGenerationSection = findViewById<View>(R.id.pin_generation_section)
        val pinCodeText = findViewById<TextView>(R.id.pin_code_text)

        pinGenerationSection?.visibility = View.GONE
        pinDisplaySection?.visibility = View.VISIBLE
        pinCodeText?.text = pin

        // PIN生成後に自動的にPINテキストにフォーカスを当てる
        pinCodeText?.requestFocus()

        // Setup copy and share buttons

/*
        val copyButton = findViewById<Button>(R.id.copy_pin_button)
        copyButton?.setOnClickListener {
            val clipboard = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
            val clip = android.content.ClipData.newPlainText("PIN", pin)
            clipboard.setPrimaryClip(clip)
            Toast.makeText(this, "PIN copied to clipboard", Toast.LENGTH_SHORT).show()
        }

        val shareButton = findViewById<Button>(R.id.share_pin_button)
        shareButton?.setOnClickListener {
            val shareIntent = Intent().apply {
                action = Intent.ACTION_SEND
                putExtra(Intent.EXTRA_TEXT, "YabaSanshiro sync PIN: $pin")
                type = "text/plain"
            }
            startActivity(Intent.createChooser(shareIntent, "Share PIN"))
        }
*/
        val refreshButton = findViewById<Button>(R.id.refresh_pin_button)
        refreshButton?.setOnClickListener {
            if (!YabauseApplication.isPro()) {
                YabauseApplication.checkDonated(this)
                return@setOnClickListener
            }
            animateButtonClick(it) {
                // Trigger sign-in first, then generate PIN with the fresh idpToken
                launchSignInForPinGeneration()
            }
        }
    }

    private fun showDeleteAccountConfirmationDialog() {
        showUnifiedErrorDialog(
            title = getString(R.string.delete_account_dialog_title),
            message = getString(R.string.delete_account_dialog_message),
            positiveButtonText = getString(R.string.delete),
            onPositiveClick = { viewModel.deleteUserAccount() },
            negativeButtonText = getString(R.string.cancel),
            errorType = ErrorType.WARNING,
            showIcon = true,
        )
    }

    private fun showError(message: String) {
        // Stop loading timeout when error is shown
        stopLoadingTimeout()

        findViewById<View>(R.id.loading_layout).visibility = View.GONE
        findViewById<View>(R.id.error_layout).visibility = View.VISIBLE
        findViewById<View>(R.id.main_content).visibility = View.GONE

        // Reset error icon for actual errors
        findViewById<ImageView>(R.id.error_icon).apply {
            setImageResource(R.drawable.ic_error_outline_24)
            imageTintList =
                androidx.core.content.ContextCompat
                    .getColorStateList(this@SimpleAccountManagementActivity, android.R.color.holo_red_light)
        }

        // Hide benefits section for actual errors (not NotAuthenticated state)
        findViewById<View>(R.id.benefits_section).visibility = View.GONE

        findViewById<TextView>(R.id.error_message).text = message
        findViewById<Button>(R.id.sign_in_button).setOnClickListener {
            animateButtonClick(it) {
                launchFirebaseSignIn()
            }
        }
    }

    private fun showNotAuthenticated() {
        // Stop loading timeout when not authenticated state is shown
        stopLoadingTimeout()

        findViewById<View>(R.id.loading_layout).visibility = View.GONE
        findViewById<View>(R.id.error_layout).visibility = View.VISIBLE
        findViewById<View>(R.id.main_content).visibility = View.GONE

        // Update UI for NotAuthenticated state
        findViewById<ImageView>(R.id.error_icon).apply {
            setImageResource(R.drawable.ic_account_circle_24)
            imageTintList =
                androidx.core.content.ContextCompat
                    .getColorStateList(this@SimpleAccountManagementActivity, android.R.color.darker_gray)
        }

        findViewById<TextView>(R.id.error_message).text = getString(R.string.sign_in_to_access_features)

        // Show benefits section for NotAuthenticated state
        findViewById<View>(R.id.benefits_section).visibility = View.VISIBLE

        // Privacy links in error section
        findViewById<TextView>(R.id.privacy_policy_link_error)?.setOnClickListener {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://www.yabasanshiro.com/privacy"))
            startActivity(intent)
            setActivityTransition(android.R.anim.fade_in, android.R.anim.fade_out)
        }
        findViewById<TextView>(R.id.terms_of_service_link_error)?.setOnClickListener {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://www.yabasanshiro.com/terms-of-use"))
            startActivity(intent)
            setActivityTransition(android.R.anim.fade_in, android.R.anim.fade_out)
        }

        // Setup sign in button
        findViewById<Button>(R.id.sign_in_button).setOnClickListener {
            animateButtonClick(it) {
                launchFirebaseSignIn()
            }
        }
    }

    private fun launchFirebaseSignIn() {
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

        signInLauncher.launch(PcSignInActivity.wrap(this, signInIntent))
    }

    private fun formatLastLoginTime(lastLoginTimeMillis: Long?): String = try {
        if (lastLoginTimeMillis != null) {
            val date = Date(lastLoginTimeMillis)
            val formatter = SimpleDateFormat("yyyy:MM:dd", Locale.getDefault())
            formatter.format(date)
        } else {
            getString(R.string.unknown_login)
        }
    } catch (e: Exception) {
        getString(R.string.unknown_login)
    }

    private fun showGDPRExportDialog() {
        showUnifiedErrorDialog(
            title = getString(R.string.export_your_personal_data),
            message = getString(R.string.gdpr_export_message),
            positiveButtonText = getString(R.string.export_data),
            onPositiveClick = { viewModel.exportUserData() },
            neutralButtonText = getString(R.string.privacy_policy),
            onNeutralClick = { openUrl("https://www.yabasanshiro.com/privacy") },
            negativeButtonText = getString(R.string.cancel),
            errorType = ErrorType.INFO,
            showIcon = true,
        )
    }

    private fun saveExportDataToFile(uri: Uri) {
        val exportData = viewModel.exportedData.value
        if (exportData != null) {
            try {
                contentResolver.openOutputStream(uri)?.use { outputStream ->
                    outputStream.write(exportData.toByteArray(Charsets.UTF_8))
                }

                // Show success dialog with additional options
                showExportSuccessDialog(uri)

                // Clear the exported data
                viewModel.clearExportedData()
            } catch (e: Exception) {
                handleConnectionError(e, "export")
            }
        } else {
            showUserFriendlyError(
                message = getString(R.string.no_data_to_export),
                errorType = ErrorType.WARNING,
            )
        }
    }

    private fun showExportSuccessDialog(uri: Uri) {
        showUnifiedErrorDialog(
            title = getString(R.string.data_export_successful),
            message = getString(R.string.export_success_message, uri.lastPathSegment),
            positiveButtonText = getString(R.string.share_file),
            onPositiveClick = { shareExportedFile(uri) },
            neutralButtonText = getString(R.string.view_in_file_manager),
            onNeutralClick = { openFileInFileManager(uri) },
            negativeButtonText = getString(R.string.done),
            errorType = ErrorType.SUCCESS,
            showIcon = true,
        )
    }

    private fun shareExportedFile(uri: Uri) {
        try {
            val shareIntent =
                Intent().apply {
                    action = Intent.ACTION_SEND
                    putExtra(Intent.EXTRA_STREAM, uri)
                    type = "application/json"
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
            startActivity(Intent.createChooser(shareIntent, getString(R.string.share_data_export)))
        } catch (e: Exception) {
            showUserFriendlyError(
                message = getString(R.string.failed_to_share_file, e.message),
                errorType = ErrorType.ERROR,
            )
        }
    }

    private fun openFileInFileManager(uri: Uri) {
        try {
            val intent =
                Intent(Intent.ACTION_VIEW).apply {
                    setDataAndType(uri, "application/json")
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
            startActivity(intent)
        } catch (e: Exception) {
            showUserFriendlyError(
                message = getString(R.string.no_file_manager_available),
                errorType = ErrorType.WARNING,
            )
        }
    }

    private fun openUrl(url: String) {
        try {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
            startActivity(intent)
            setActivityTransition(android.R.anim.fade_in, android.R.anim.fade_out)
        } catch (e: Exception) {
            showUserFriendlyError(
                message = getString(R.string.failed_to_open_url, e.message),
                errorType = ErrorType.ERROR,
                showRetry = true,
                retryAction = { openUrl(url) },
            )
        }
    }

    // エラー表示改善: 統一されたエラーダイアログシステム

    enum class ErrorType {
        ERROR, // 赤色 - 重要なエラー
        WARNING, // 黄色 - 警告
        INFO, // 青色 - 情報
        SUCCESS, // 緑色 - 成功
    }

    private fun showUnifiedErrorDialog(
        title: String,
        message: String? = null,
        view: View? = null,
        positiveButtonText: String,
        onPositiveClick: (() -> Unit)? = null,
        negativeButtonText: String? = null,
        onNegativeClick: (() -> Unit)? = null,
        neutralButtonText: String? = null,
        onNeutralClick: (() -> Unit)? = null,
        errorType: ErrorType = ErrorType.ERROR,
        showIcon: Boolean = false,
    ) {
        try {
            val dialog =
                MaterialAlertDialogBuilder(this)
                    .setTitle(title)
                    .apply {
                        if (message != null) setMessage(message)
                        if (view != null) setView(view)

                        // エラータイプに応じたアイコン設定
                        if (showIcon) {
                            val iconRes =
                                when (errorType) {
                                    ErrorType.ERROR -> R.drawable.ic_error_outline_24
                                    ErrorType.WARNING -> android.R.drawable.ic_dialog_alert
                                    ErrorType.INFO -> android.R.drawable.ic_dialog_info
                                    ErrorType.SUCCESS -> android.R.drawable.ic_input_add // Best available approximation
                                }
                            setIcon(iconRes)
                        }
                    }.setPositiveButton(positiveButtonText) { _, _ ->
                        onPositiveClick?.invoke()
                    }.apply {
                        if (negativeButtonText != null) {
                            setNegativeButton(negativeButtonText) { _, _ ->
                                onNegativeClick?.invoke()
                            }
                        }
                        if (neutralButtonText != null) {
                            setNeutralButton(neutralButtonText) { _, _ ->
                                onNeutralClick?.invoke()
                            }
                        }
                    }.create()

            // ボタンの色を設定（Material Design 3準拠）
            dialog.setOnShowListener {
                dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE)?.apply {
                    val colorRes =
                        when (errorType) {
                            ErrorType.ERROR -> android.R.color.holo_red_dark
                            ErrorType.WARNING -> android.R.color.holo_orange_dark
                            ErrorType.INFO -> android.R.color.holo_blue_dark
                            ErrorType.SUCCESS -> android.R.color.holo_green_dark
                        }
                    setTextColor(
                        androidx.core.content.ContextCompat
                            .getColor(this@SimpleAccountManagementActivity, colorRes),
                    )
                }
            }

            dialog.show()
        } catch (e: Exception) {
            Log.e("SimpleAccountManagement", "Failed to show unified dialog: ${e.message}")
            // フォールバック: 基本的なToastメッセージ
            Toast.makeText(this, title, Toast.LENGTH_LONG).show()
        }
    }

    // エラー表示改善: ユーザーフレンドリーなエラーメッセージ

    private fun showUserFriendlyError(
        message: String,
        errorType: ErrorType = ErrorType.ERROR,
        actionText: String? = null,
        action: (() -> Unit)? = null,
        showRetry: Boolean = false,
        retryAction: (() -> Unit)? = null,
    ) {
        try {
            // 長いメッセージの場合はSnackbarを使用
            if (message.length > 50) {
                val snackbar = Snackbar.make(rootView, message, Snackbar.LENGTH_LONG)

                // エラータイプに応じた色分け
                val colorRes =
                    when (errorType) {
                        ErrorType.ERROR -> android.R.color.holo_red_dark
                        ErrorType.WARNING -> android.R.color.holo_orange_dark
                        ErrorType.INFO -> android.R.color.holo_blue_dark
                        ErrorType.SUCCESS -> android.R.color.holo_green_dark
                    }
                snackbar.view.backgroundTintList = androidx.core.content.ContextCompat
                    .getColorStateList(this, colorRes)

                // アクションボタンの追加
                if (showRetry && retryAction != null) {
                    snackbar.setAction(getString(R.string.retry)) { retryAction() }
                } else if (actionText != null && action != null) {
                    snackbar.setAction(actionText) { action() }
                }

                snackbar.show()
            } else {
                // 短いメッセージの場合はToastを使用（改善版）
                Toast.makeText(this, message, Toast.LENGTH_LONG).show()
            }
        } catch (e: Exception) {
            Log.e("SimpleAccountManagement", "Failed to show user-friendly error: ${e.message}")
            // 最終フォールバック
            Toast.makeText(this, message, Toast.LENGTH_LONG).show()
        }
    }

    // エラー表示改善: 接続エラー時の詳細フィードバック

    private fun handleConnectionError(
        exception: Exception,
        context: String = "",
    ) {
        val (userMessage, solution) =
            when {
                // ネットワーク接続エラー
                exception.message?.contains("network", ignoreCase = true) == true ||
                    exception.message?.contains("timeout", ignoreCase = true) == true -> {
                    Pair(
                        getString(R.string.network_connection_error),
                        getString(R.string.check_internet_connection),
                    )
                }
                // Discord OAuth エラー
                context.contains("discord", ignoreCase = true) -> {
                    Pair(
                        getString(R.string.discord_connection_failed),
                        getString(R.string.discord_oauth_troubleshooting),
                    )
                }
                // RetroAchievements API エラー
                context.contains("retroachievements", ignoreCase = true) -> {
                    Pair(
                        getString(R.string.retroachievements_api_error),
                        getString(R.string.check_credentials_and_connectivity),
                    )
                }
                // Firebase エラー
                context.contains("firebase", ignoreCase = true) -> {
                    Pair(
                        getString(R.string.authentication_service_error),
                        getString(R.string.try_again_or_contact_support),
                    )
                }
                else -> {
                    Pair(
                        getString(R.string.unexpected_error_occurred),
                        getString(R.string.please_try_again_later),
                    )
                }
            }

        showUnifiedErrorDialog(
            title = getString(R.string.connection_error_title),
            message = "$userMessage\n\n${getString(R.string.suggested_solution)}: $solution",
            positiveButtonText = getString(R.string.retry),
            onPositiveClick = { viewModel.refresh() },
            neutralButtonText = getString(R.string.more_info),
            onNeutralClick = { showErrorDetailsDialog(exception, context) },
            negativeButtonText = getString(R.string.dismiss),
            errorType = ErrorType.ERROR,
            showIcon = true,
        )
    }

    // エラー表示改善: エラー詳細ダイアログ（開発者向け）

    private fun showErrorDetailsDialog(
        exception: Exception,
        context: String,
    ) {
        val errorDetails =
            buildString {
                appendLine("Context: $context")
                appendLine("Error: ${exception.javaClass.simpleName}")
                appendLine("Message: ${exception.message}")
                appendLine("Time: ${SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault()).format(Date())}")
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    appendLine("Stack trace:")
                    exception.stackTrace.take(5).forEach { stackElement ->
                        appendLine("  at $stackElement")
                    }
                }
            }

        showUnifiedErrorDialog(
            title = getString(R.string.error_details_title),
            message = errorDetails,
            positiveButtonText = getString(R.string.copy_to_clipboard),
            onPositiveClick = { copyErrorToClipboard(errorDetails) },
            negativeButtonText = getString(R.string.close),
            errorType = ErrorType.INFO,
            showIcon = true,
        )
    }

    // エラー表示改善: エラー情報のクリップボードコピー

    private fun copyErrorToClipboard(errorText: String) {
        try {
            val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            val clip = ClipData.newPlainText(getString(R.string.error_report), errorText)
            clipboard.setPrimaryClip(clip)

            showUserFriendlyError(
                message = getString(R.string.error_details_copied),
                errorType = ErrorType.SUCCESS,
            )
        } catch (e: Exception) {
            Log.e("SimpleAccountManagement", "Failed to copy error to clipboard: ${e.message}")
            showUserFriendlyError(
                message = getString(R.string.failed_to_copy_error),
                errorType = ErrorType.ERROR,
            )
        }
    }

    // フォントサイズ対応: システムフォントサイズに応じた動的調整

    private fun setupFontSizeSupport() {
        // システムフォントスケールの取得
        val fontScale = resources.configuration.fontScale

        // 大きなフォントサイズの場合のレイアウト調整
        if (fontScale > 1.3f) {
            adjustLayoutForLargeFonts()
        }

        Log.d("SimpleAccountManagement", "Font scale: $fontScale")
    }

    private fun adjustLayoutForLargeFonts() {
        try {
            // ユーザープロフィール関連のテキスト調整
            adjustTextViewForLargeFont(R.id.user_display_name, maxLines = 2)
            adjustTextViewForLargeFont(R.id.user_email, maxLines = 2)
            adjustTextViewForLargeFont(R.id.last_login_time, maxLines = 2)

            // Discord関連のテキスト調整
            adjustTextViewForLargeFont(R.id.discord_status, maxLines = 1)
            adjustTextViewForLargeFont(R.id.discord_username, maxLines = 2)
            adjustTextViewForLargeFont(R.id.discord_display_name, maxLines = 2)

            // RetroAchievements関連のテキスト調整
            adjustTextViewForLargeFont(R.id.ra_status, maxLines = 1)
            adjustTextViewForLargeFont(R.id.ra_username, maxLines = 2)

            // エラーメッセージのテキスト調整
            adjustTextViewForLargeFont(R.id.error_message, maxLines = 4)

            // ボタンの最小高さ調整
            adjustButtonForLargeFont(R.id.user_sign_in_button)
            adjustButtonForLargeFont(R.id.user_sign_out_button)
            adjustButtonForLargeFont(R.id.discord_link_button)
            adjustButtonForLargeFont(R.id.discord_unlink_button)
            adjustButtonForLargeFont(R.id.discord_create_account_button)
            adjustButtonForLargeFont(R.id.ra_login_button)
            adjustButtonForLargeFont(R.id.ra_logout_button)
            adjustButtonForLargeFont(R.id.ra_create_account_button)
            adjustButtonForLargeFont(R.id.generate_pin_button)
            adjustButtonForLargeFont(R.id.refresh_pin_button)
            adjustButtonForLargeFont(R.id.export_data_button)
            adjustButtonForLargeFont(R.id.delete_account_button)
            adjustButtonForLargeFont(R.id.sign_in_button)

            // リンクテキストの調整（カード内）
            adjustTextViewForLargeFont(R.id.privacy_policy_link, maxLines = 2)
            adjustTextViewForLargeFont(R.id.terms_of_service_link, maxLines = 2)
        } catch (e: Exception) {
            Log.e("SimpleAccountManagement", "Failed to adjust layout for large fonts: ${e.message}")
        }
    }

    private fun adjustTextViewForLargeFont(
        viewId: Int,
        maxLines: Int = 1,
    ) {
        findViewById<TextView>(viewId)?.apply {
            this.maxLines = maxLines
            ellipsize = TextUtils.TruncateAt.END

            // テキストサイズが非常に大きい場合の追加調整
            val fontScale = resources.configuration.fontScale
            if (fontScale > 1.5f) {
                // 極端に大きなフォントサイズの場合、少し制限する
                // textSize はピクセル単位なので、SPに変換するには density と fontScale で割る
                val currentSizeSp = textSize / (resources.displayMetrics.density * fontScale)
                val adjustedSize = currentSizeSp * 0.9f // 10%縮小
                setTextSize(TypedValue.COMPLEX_UNIT_SP, adjustedSize)
            }
        }
    }

    private fun adjustButtonForLargeFont(viewId: Int) {
        findViewById<Button>(viewId)?.apply {
            val fontScale = resources.configuration.fontScale

            // フォントスケールに応じた最小高さの調整
            val baseMinHeight = 48 // dp
            val adjustedMinHeight = (baseMinHeight * fontScale * resources.displayMetrics.density).toInt()
            minHeight = adjustedMinHeight

            // 極端に大きなフォントの場合、パディングも調整
            if (fontScale > 1.5f) {
                val additionalPadding = (8 * resources.displayMetrics.density).toInt()
                setPadding(
                    paddingLeft,
                    paddingTop + additionalPadding,
                    paddingRight,
                    paddingBottom + additionalPadding,
                )
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // Clean up loading timeout handler
        stopLoadingTimeout()
    }

    override fun finish() {
        super.finish()
        // 画面終了時のフェードアニメーション
        setActivityCloseTransition(android.R.anim.fade_in, android.R.anim.fade_out)
    }
}
