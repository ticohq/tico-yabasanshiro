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
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.VisibleForTesting
import androidx.appcompat.widget.Toolbar
import androidx.coordinatorlayout.widget.CoordinatorLayout
import androidx.core.view.ViewCompat
import androidx.fragment.app.Fragment
import androidx.fragment.app.viewModels
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
 * Account Management Fragment
 *
 * Unified account management screen for Firebase, Discord, and RetroAchievements integration.
 * Provides user-friendly account management features with Material Design 3 compliant UI.
 *
 * This is the Fragment version that replaces SimpleAccountManagementActivity,
 * allowing it to be hosted inside the ext_fragment container so the Navigation Bar remains visible.
 *
 * @since 2025-08-13
 * @see AccountManagementViewModel
 */
class AccountManagementFragment : Fragment() {
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
        // Register launchers in onCreate (must be before STARTED state)
        setupSignInLauncher()
        setupExportDataLauncher()
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View? = inflater.inflate(R.layout.fragment_account_management, container, false)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        rootView = view.findViewById(R.id.root_layout)

        setupToolbar()
        setupObservers()
        setupClickListeners()
        setupFontSizeSupport()
    }

    override fun onResume() {
        super.onResume()
        viewModel.refresh()
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        setupFontSizeSupport()
        Log.d(TAG, "Configuration changed, new font scale: ${newConfig.fontScale}")
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
        val toolbar = requireView().findViewById<Toolbar>(R.id.toolbar)
        toolbar.title = getString(R.string.account_management)
        toolbar.setNavigationOnClickListener {
            requireActivity().onBackPressedDispatcher.onBackPressed()
        }
    }

    private fun setupObservers() {
        viewModel.uiState.observe(viewLifecycleOwner) { state ->
            when (state) {
                is AccountUiState.Loading -> showLoading()
                is AccountUiState.Success -> showSuccess(state)
                is AccountUiState.Error -> showError(state.message)
                is AccountUiState.NotAuthenticated -> showNotAuthenticated()
            }
        }

        viewModel.message.observe(viewLifecycleOwner) { message ->
            message?.let {
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

        viewModel.exportedData.observe(viewLifecycleOwner) { exportData ->
            exportData?.let {
                val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
                val fileName = "YabaSanshiro_UserData_$timestamp.json"
                exportDataLauncher.launch(fileName)
            }
        }

        // PIN observer must be registered once here (not inside setupCrossDeviceSyncClickListener)
        // to avoid missing events during Loading → Success state transitions
        viewModel.generatedPin.observe(viewLifecycleOwner) { pin ->
            pin?.let {
                showGeneratedPin(it)
            }
        }

        // When direct PIN generation fails (no valid idpToken), launch sign-in
        viewModel.pinNeedsAuth.observe(viewLifecycleOwner) { needsAuth ->
            if (needsAuth == true) {
                viewModel.clearPinNeedsAuth()
                launchSignInForPinGeneration()
            }
        }

        viewModel.refresh()
    }

    private fun setupClickListeners() {
        // Basic click listeners only; detailed card update logic is set up in updateAccountCards
    }

    @Suppress("DEPRECATION")
    private fun announceToAccessibilityService(message: String) {
        if (!isAdded) return
        rootView.announceForAccessibility(message)
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

    private fun setupFocusAnimations(view: View) {
        if (view is Button) {
            view.setTag(R.id.original_text_color, view.currentTextColor)
        }

        view.setOnFocusChangeListener { v, hasFocus ->
            if (hasFocus) {
                playFocusSound()

                v
                    .animate()
                    .scaleX(1.05f)
                    .scaleY(1.05f)
                    .alpha(1.0f)
                    .setDuration(150)
                    .setInterpolator(android.view.animation.DecelerateInterpolator())
                    .start()

                if (v is Button || v.isClickable) {
                    v.backgroundTintList = android.content.res.ColorStateList.valueOf(
                        resolveThemeColor(R.attr.colorSecondaryContainer),
                    )
                    if (v is Button) {
                        v.setTextColor(
                            resolveThemeColor(R.attr.colorOnSecondaryContainer),
                        )
                    }
                }

                scrollToView(v)

                val contentDescription =
                    v.contentDescription
                        ?: when (v) {
                            is Button -> v.text
                            is TextView -> v.text
                            else -> getString(R.string.focused_element)
                        }
                announceToAccessibilityService(getString(R.string.focused_format, contentDescription))
            } else {
                v
                    .animate()
                    .scaleX(1.0f)
                    .scaleY(1.0f)
                    .alpha(0.9f)
                    .setDuration(150)
                    .setInterpolator(android.view.animation.AccelerateInterpolator())
                    .start()

                if (v is Button || v.isClickable) {
                    v.backgroundTintList = null
                    if (v is Button) {
                        val originalColor = v.getTag(R.id.original_text_color) as? Int
                        if (originalColor != null) {
                            v.setTextColor(originalColor)
                        }
                    }
                }
            }
        }

        view.setOnKeyListener { v, keyCode, event ->
            if (event.action == android.view.KeyEvent.ACTION_DOWN) {
                when (keyCode) {
                    android.view.KeyEvent.KEYCODE_DPAD_CENTER,
                    android.view.KeyEvent.KEYCODE_ENTER,
                    android.view.KeyEvent.KEYCODE_NUMPAD_ENTER,
                    android.view.KeyEvent.KEYCODE_BUTTON_A,
                    -> {
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

    private fun playFocusSound() {
        try {
            val audioManager = requireContext().getSystemService(Context.AUDIO_SERVICE) as AudioManager
            audioManager.playSoundEffect(AudioManager.FX_FOCUS_NAVIGATION_UP)
        } catch (e: Exception) {
            Log.d(TAG, "Failed to play focus sound: ${e.message}")
        }
    }

    private fun scrollToView(view: View) {
        var parent: android.view.ViewParent? = view.parent
        var scrollView: androidx.core.widget.NestedScrollView? = null

        while (parent != null) {
            if (parent is androidx.core.widget.NestedScrollView) {
                scrollView = parent
                break
            }
            parent = parent.parent
        }

        if (scrollView != null) {
            view.post {
                try {
                    val viewRect = android.graphics.Rect()
                    view.getDrawingRect(viewRect)
                    scrollView.offsetDescendantRectToMyCoords(view, viewRect)

                    val scrollY = scrollView.scrollY
                    val scrollViewHeight = scrollView.height

                    val density = resources.displayMetrics.density
                    val visibilityMarginDp = 120
                    val scrollMarginDp = 150
                    val visibilityMargin = (visibilityMarginDp * density).toInt()
                    val scrollMargin = (scrollMarginDp * density).toInt()

                    val isViewFullyVisible =
                        viewRect.top >= scrollY + visibilityMargin &&
                            viewRect.bottom <= scrollY + scrollViewHeight - visibilityMargin

                    if (!isViewFullyVisible) {
                        val targetScrollY =
                            when {
                                viewRect.top < scrollY + visibilityMargin -> {
                                    maxOf(0, viewRect.top - scrollMargin)
                                }
                                viewRect.bottom > scrollY + scrollViewHeight - visibilityMargin -> {
                                    viewRect.bottom - scrollViewHeight + scrollMargin
                                }
                                else -> scrollY
                            }

                        scrollView.smoothScrollTo(0, targetScrollY)

                        Log.d(
                            TAG,
                            "Scrolling to view: ${view.javaClass.simpleName}, " +
                                "targetY: $targetScrollY, viewTop: ${viewRect.top}, viewBottom: ${viewRect.bottom}",
                        )
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to scroll to view: ${e.message}")
                }
            }
        }
    }

    private fun showRetroAchievementsLoginDialog() {
        if (!isAdded) return

        val dialogView = LayoutInflater.from(requireContext()).inflate(R.layout.dialog_ra_login, null)
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

    private fun showLoading() {
        if (!isAdded) return
        requireView().findViewById<View>(R.id.loading_layout).visibility = View.VISIBLE
        requireView().findViewById<View>(R.id.error_layout).visibility = View.GONE
        requireView().findViewById<View>(R.id.main_content).visibility = View.GONE
        announceToAccessibilityService(getString(R.string.loading))
        startLoadingTimeout()
    }

    private fun startLoadingTimeout() {
        loadingTimeoutHandler?.removeCallbacksAndMessages(null)

        loadingTimeoutHandler = android.os.Handler(android.os.Looper.getMainLooper())
        loadingTimeoutHandler?.postDelayed({
            if (!isAdded) return@postDelayed
            if (requireView().findViewById<View>(R.id.loading_layout).visibility == View.VISIBLE) {
                showLoadingTimeoutError()
            }
        }, loadingTimeoutMs)
    }

    private fun stopLoadingTimeout() {
        loadingTimeoutHandler?.removeCallbacksAndMessages(null)
        loadingTimeoutHandler = null
    }

    private fun showLoadingTimeoutError() {
        if (!isAdded) return
        showUnifiedErrorDialog(
            title = getString(R.string.loading_timeout_title),
            message = getString(R.string.loading_timeout_message),
            positiveButtonText = getString(R.string.retry),
            onPositiveClick = {
                viewModel.refresh()
            },
            neutralButtonText = getString(R.string.cancel_loading),
            onNeutralClick = {
                showNotAuthenticated()
            },
            negativeButtonText = getString(R.string.dismiss),
            errorType = ErrorType.WARNING,
            showIcon = true,
        )
    }

    private fun showSuccess(state: AccountUiState.Success) {
        if (!isAdded) return
        stopLoadingTimeout()

        requireView().findViewById<View>(R.id.loading_layout).visibility = View.GONE
        requireView().findViewById<View>(R.id.error_layout).visibility = View.GONE
        requireView().findViewById<View>(R.id.main_content).visibility = View.VISIBLE
        updateAccountCards(state.userProfile, state.connectedAccounts)

        // Show/hide Google-dependent sections based on Firebase auth state
        if (state.userProfile == null) {
            hideGoogleDependentSections()
        } else {
            showGoogleDependentSections()
        }

        val fontScale = resources.configuration.fontScale
        if (fontScale > 1.3f) {
            adjustLayoutForLargeFonts()
        }

        announceToAccessibilityService(getString(R.string.account_management_title))
    }

    /**
     * Show sections that require Google/Firebase authentication
     */
    private fun showGoogleDependentSections() {
        if (!isAdded) return
        val view = requireView()

        view.findViewById<View>(R.id.account_actions_header)?.visibility = View.VISIBLE
        view.findViewById<View>(R.id.cross_device_sync_card)?.visibility = View.VISIBLE
        // GDPR Data Export is hidden (not yet implemented)
        // view.findViewById<View>(R.id.data_export_card)?.visibility = View.VISIBLE
        view.findViewById<View>(R.id.account_deletion_card)?.visibility = View.VISIBLE

        // Show Discord card (RA card is always visible)
        view.findViewById<View>(R.id.discord_account_card)?.visibility = View.VISIBLE
    }

    private fun updateAccountCards(
        userProfile: UserProfile?,
        connectedAccounts: ConnectedAccountsState,
    ) {
        updateUserProfileCard(userProfile)
        updateDiscordCard(connectedAccounts.discord)
        updateRetroAchievementsCard(connectedAccounts.retroAchievements)
        updateConnectedAccountsEmptyHint(connectedAccounts)
        setupAllClickListeners(connectedAccounts)
    }

    private fun updateConnectedAccountsEmptyHint(connectedAccounts: ConnectedAccountsState) {
        if (!isAdded) return
        val hint = requireView().findViewById<TextView>(R.id.connected_accounts_empty_hint)
        hint?.visibility = if (!connectedAccounts.discord.isConnected && !connectedAccounts.retroAchievements.isConnected) {
            View.VISIBLE
        } else {
            View.GONE
        }
    }

    private fun updateUserProfileCard(userProfile: UserProfile?) {
        if (!isAdded) return
        try {
            val view = requireView()
            val userProfileCard = view.findViewById<View>(R.id.user_profile_card)
            val userAvatar = userProfileCard.findViewById<com.google.android.material.imageview.ShapeableImageView>(R.id.user_avatar)
            val userDisplayName = userProfileCard.findViewById<TextView>(R.id.user_display_name)
            val userEmail = userProfileCard.findViewById<TextView>(R.id.user_email)
            val lastLoginTime = userProfileCard.findViewById<TextView>(R.id.last_login_time)
            val accountActionsSection = userProfileCard.findViewById<View>(R.id.account_actions_section)
            val notSignedInSection = userProfileCard.findViewById<View>(R.id.not_signed_in_section)

            if (userProfile != null) {
                userDisplayName.text = userProfile.displayName ?: getString(R.string.unknown_user)
                userEmail.text = userProfile.email ?: getString(R.string.no_email)
                lastLoginTime.text = getString(R.string.last_login_format, formatLastLoginTime(userProfile.lastLoginTime))
                userEmail.visibility = View.VISIBLE
                lastLoginTime.visibility = View.VISIBLE

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

                accountActionsSection?.visibility = View.VISIBLE
                notSignedInSection?.visibility = View.GONE

                // Sign out button in card
                val signOutButton = userProfileCard.findViewById<Button>(R.id.user_sign_out_button)
                signOutButton?.setOnClickListener {
                    animateButtonClick(it) {
                        org.uoyabause.android.auth.FirebaseAuthManager(requireContext()).signOut(requireActivity()) {
                            viewModel.refresh()
                        }
                    }
                }

                // Privacy links in card
                setupPrivacyLinksInCard(userProfileCard)
                setupPrivacyLinksFooter()
            } else {
                userDisplayName.text = getString(R.string.not_signed_in)
                userEmail.text = ""//getString(R.string.please_sign_in_to_continue)
                lastLoginTime.text = "" //getString(R.string.no_login_data)
                userEmail.visibility = View.GONE
                lastLoginTime.visibility = View.GONE

                userAvatar.setImageResource(R.drawable.ic_account_circle_24)

                accountActionsSection?.visibility = View.GONE
                notSignedInSection?.visibility = View.VISIBLE

                val signInButton = userProfileCard.findViewById<Button>(R.id.user_sign_in_button)
                signInButton?.setOnClickListener {
                    animateButtonClick(it) {
                        launchFirebaseSignIn()
                    }
                }

                setupPrivacyLinksFooter()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to update user profile card: ${e.message}")
            showUserFriendlyError(
                message = getString(R.string.failed_to_update_profile),
                errorType = ErrorType.WARNING,
                showRetry = true,
                retryAction = { viewModel.refresh() },
            )
        }
    }

    private fun updateDiscordCard(discordState: AccountConnectionState) {
        if (!isAdded) return
        try {
            val view = requireView()
            val discordCard = view.findViewById<View>(R.id.discord_account_card)
            val discordStatus = discordCard.findViewById<TextView>(R.id.discord_status)
            val discordStatusIndicator = discordCard.findViewById<View>(R.id.discord_status_indicator)
            val discordConnectedContent = discordCard.findViewById<View>(R.id.discord_connected_content)
            val discordDisconnectedContent = discordCard.findViewById<View>(R.id.discord_disconnected_content)

            if (discordState.isConnected) {
                discordStatus.text = getString(R.string.connected)
                discordStatusIndicator.backgroundTintList =
                    android.content.res.ColorStateList.valueOf(
                        resolveThemeColor(R.attr.colorPrimary),
                    )
                discordConnectedContent.visibility = View.VISIBLE
                discordDisconnectedContent.visibility = View.GONE

                val discordUsername = discordCard.findViewById<TextView>(R.id.discord_username)
                val discordDisplayName = discordCard.findViewById<TextView>(R.id.discord_display_name)
                val discordAvatar = discordCard.findViewById<com.google.android.material.imageview.ShapeableImageView>(R.id.discord_avatar)

                discordUsername.text = discordState.username ?: getString(R.string.discord_user)
                discordDisplayName.text = discordState.displayName ?: discordState.username

                if (!discordState.avatarUrl.isNullOrEmpty()) {
                    Glide
                        .with(this)
                        .load(discordState.avatarUrl)
                        .transform(CircleCrop())
                        .placeholder(R.drawable.ic_discord_24)
                        .error(R.drawable.ic_discord_24)
                        .into(discordAvatar)
                    Log.d(TAG, "Loading Discord avatar: ${discordState.avatarUrl}")
                } else {
                    discordAvatar.setImageResource(R.drawable.ic_discord_24)
                }
            } else {
                discordStatus.text = getString(R.string.not_connected)
                discordStatusIndicator.backgroundTintList =
                    android.content.res.ColorStateList.valueOf(
                        resolveThemeColor(R.attr.colorError),
                    )
                discordConnectedContent.visibility = View.GONE
                discordDisconnectedContent.visibility = View.VISIBLE
            }
        } catch (e: Exception) {
            // Handle error silently
        }
    }

    private fun updateRetroAchievementsCard(raState: AccountConnectionState) {
        if (!isAdded) return
        try {
            val view = requireView()
            val raCard = view.findViewById<View>(R.id.retroachievements_account_card)
            val raStatus = raCard.findViewById<TextView>(R.id.ra_status)
            val raStatusIndicator = raCard.findViewById<View>(R.id.ra_status_indicator)
            val raConnectedContent = raCard.findViewById<View>(R.id.ra_logged_in_content)
            val raDisconnectedContent = raCard.findViewById<View>(R.id.ra_not_logged_in_content)

            if (raState.isConnected) {
                raStatus.text = getString(R.string.connected)
                raStatusIndicator.backgroundTintList =
                    android.content.res.ColorStateList.valueOf(
                        resolveThemeColor(R.attr.colorPrimary),
                    )
                raConnectedContent.visibility = View.VISIBLE
                raDisconnectedContent.visibility = View.GONE

                val raUsername = raCard.findViewById<TextView>(R.id.ra_username)
                raUsername.text = raState.username ?: getString(R.string.retroachievements_user)

                raState.additionalInfo?.let { info ->
                    val hardcoreScoreText = raCard.findViewById<TextView>(R.id.ra_points)
                    val softcoreScoreText = raCard.findViewById<TextView>(R.id.ra_achievements)
                    val unreadMessagesText = raCard.findViewById<TextView>(R.id.ra_games_completed)

                    hardcoreScoreText?.text = info["hardcoreScore"]?.toString() ?: "0"
                    softcoreScoreText?.text = info["softcoreScore"]?.toString() ?: "0"
                    unreadMessagesText?.text = info["unreadMessages"]?.toString() ?: "0"

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
                        Log.d(TAG, "Loading RetroAchievements avatar: $avatarUrl")
                    } else {
                        raAvatar?.setImageResource(R.drawable.ic_account_circle_24)
                    }
                }
            } else {
                raStatus.text = getString(R.string.not_connected)
                raStatusIndicator.backgroundTintList =
                    android.content.res.ColorStateList.valueOf(
                        resolveThemeColor(R.attr.colorError),
                    )
                raConnectedContent.visibility = View.GONE
                raDisconnectedContent.visibility = View.VISIBLE
            }
        } catch (e: Exception) {
            // Handle error silently
        }
    }

    private fun setupAllClickListeners(connectedAccounts: ConnectedAccountsState) {
        setupDiscordClickListeners(connectedAccounts.discord)
        setupRetroAchievementsClickListeners(connectedAccounts.retroAchievements)
        setupCrossDeviceSyncClickListener()
        setupAccountActionsClickListeners()
        setupAllFocusAnimations()
        setupGamepadNavigation(connectedAccounts)
    }

    private fun setupAllFocusAnimations() {
        if (!isAdded) return
        val view = requireView()

        view.findViewById<Button>(R.id.user_sign_in_button)?.let { setupFocusAnimations(it) }
        view.findViewById<Button>(R.id.user_sign_out_button)?.let { setupFocusAnimations(it) }

        view.findViewById<Button>(R.id.discord_link_button)?.let { setupFocusAnimations(it) }
        view.findViewById<Button>(R.id.discord_unlink_button)?.let { setupFocusAnimations(it) }
        view.findViewById<Button>(R.id.discord_create_account_button)?.let { setupFocusAnimations(it) }

        view.findViewById<Button>(R.id.ra_login_button)?.let { setupFocusAnimations(it) }
        view.findViewById<Button>(R.id.ra_logout_button)?.let { setupFocusAnimations(it) }
        view.findViewById<Button>(R.id.ra_create_account_button)?.let { setupFocusAnimations(it) }

        view.findViewById<Button>(R.id.generate_pin_button)?.let { setupFocusAnimations(it) }
        view.findViewById<Button>(R.id.refresh_pin_button)?.let { setupFocusAnimations(it) }

        view.findViewById<TextView>(R.id.pin_code_text)?.let {
            setupFocusAnimations(it)
        }

        view.findViewById<Button>(R.id.export_data_button)?.let { setupFocusAnimations(it) }
        view.findViewById<Button>(R.id.delete_account_button)?.let { setupFocusAnimations(it) }

        view.findViewById<Button>(R.id.sign_in_button)?.let { setupFocusAnimations(it) }

        view.findViewById<TextView>(R.id.privacy_policy_link)?.let {
            setupFocusAnimations(it)
            it.isFocusable = true
        }
        view.findViewById<TextView>(R.id.terms_of_service_link)?.let {
            setupFocusAnimations(it)
            it.isFocusable = true
        }
        view.findViewById<TextView>(R.id.privacy_policy_link_footer)?.let {
            setupFocusAnimations(it)
            it.isFocusable = true
        }
        view.findViewById<TextView>(R.id.terms_of_service_link_footer)?.let {
            setupFocusAnimations(it)
            it.isFocusable = true
        }

        view.findViewById<Toolbar>(R.id.toolbar)?.let { toolbar ->
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
        if (!isAdded) return
        val view = requireView()

        // Build a single linear chain of all visible focusable views (top to bottom order)
        val navChain = mutableListOf<View>()

        fun addIfVisible(v: View?) {
            if (v != null && isViewEffectivelyVisible(v)) {
                v.isFocusable = true
                navChain.add(v)
            }
        }

        // 1. Sign Out
        addIfVisible(view.findViewById(R.id.user_sign_out_button))

        // 2. Privacy Policy
        addIfVisible(view.findViewById(R.id.privacy_policy_link))

        // 3. Terms of Service
        addIfVisible(view.findViewById(R.id.terms_of_service_link))

        // 4-5. Discord: Create Account → Link/Unlink
        addIfVisible(view.findViewById(R.id.discord_create_account_button))
        if (connectedAccounts.discord.isConnected) {
            addIfVisible(view.findViewById(R.id.discord_unlink_button))
        } else {
            addIfVisible(view.findViewById(R.id.discord_link_button))
        }

        // 6-7. RetroAchievements: Create Account → Login/Logout
        addIfVisible(view.findViewById(R.id.ra_create_account_button))
        if (connectedAccounts.retroAchievements.isConnected) {
            addIfVisible(view.findViewById(R.id.ra_logout_button))
        } else {
            addIfVisible(view.findViewById(R.id.ra_login_button))
        }

        // 8. Generate PIN
        addIfVisible(view.findViewById(R.id.generate_pin_button))

        // 9. Delete Account
        addIfVisible(view.findViewById(R.id.delete_account_button))

        // Disable focus on Google provider chip
        view.findViewById<View>(R.id.account_provider_chip)?.apply {
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
        if (!isAdded) return
        val discordCard = requireView().findViewById<View>(R.id.discord_account_card)

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
                    val intent = Intent(requireContext(), DiscordLinkActivity::class.java)
                    startActivity(intent)
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
        if (!isAdded) return
        val raCard = requireView().findViewById<View>(R.id.retroachievements_account_card)

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
        if (!isAdded) return
        val view = requireView()
        val generatePinButton = view.findViewById<Button>(R.id.generate_pin_button)
        generatePinButton?.setOnClickListener {
            if (!YabauseApplication.isPro()) {
                YabauseApplication.checkDonated(requireActivity())
                return@setOnClickListener
            }
            animateButtonClick(it) {
                // Try direct PIN generation first; falls back to sign-in if no valid token
                viewModel.tryGenerateDevicePIN()
            }
        }
    }

    private fun launchSignInForPinGeneration() {
        pinGenerationCallback = { response ->
            if (response != null) {
                viewModel.generateDevicePINWithResponse(response)
            } else if (FirebaseAuth.getInstance().currentUser != null) {
                // IdpResponse is null but user is signed in (common in Fragment context).
                // Use Firebase ID token fallback.
                viewModel.tryGenerateDevicePIN()
            } else {
                showUserFriendlyError(
                    message = getString(R.string.sign_in_required_for_pin),
                    errorType = ErrorType.WARNING,
                    actionText = getString(R.string.sign_in),
                    action = { launchFirebaseSignIn() },
                )
            }
        }

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

    private fun setupAccountActionsClickListeners() {
        if (!isAdded) return
        val view = requireView()
        val exportButton = view.findViewById<Button>(R.id.export_data_button)
        exportButton?.setOnClickListener {
            animateButtonClick(it) {
                showGDPRExportDialog()
            }
        }

        val deleteButton = view.findViewById<Button>(R.id.delete_account_button)
        deleteButton?.setOnClickListener {
            animateButtonClick(it) {
                showDeleteAccountConfirmationDialog()
            }
        }
    }

    private fun showGeneratedPin(pin: String) {
        if (!isAdded) return
        if (!YabauseApplication.isPro()) {
            viewModel.clearGeneratedPin()
            return
        }
        val view = requireView()
        val pinDisplaySection = view.findViewById<View>(R.id.pin_display_section)
        val pinGenerationSection = view.findViewById<View>(R.id.pin_generation_section)
        val pinCodeText = view.findViewById<TextView>(R.id.pin_code_text)

        pinGenerationSection?.visibility = View.GONE
        pinDisplaySection?.visibility = View.VISIBLE
        pinCodeText?.text = pin

        pinCodeText?.requestFocus()

        val refreshButton = view.findViewById<Button>(R.id.refresh_pin_button)
        refreshButton?.setOnClickListener {
            if (!YabauseApplication.isPro()) {
                YabauseApplication.checkDonated(requireActivity())
                return@setOnClickListener
            }
            animateButtonClick(it) {
                launchSignInForPinGeneration()
            }
        }
    }

    private fun showDeleteAccountConfirmationDialog() {
        if (!isAdded) return
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
        if (!isAdded) return
        stopLoadingTimeout()

        val view = requireView()
        view.findViewById<View>(R.id.loading_layout).visibility = View.GONE
        view.findViewById<View>(R.id.error_layout).visibility = View.VISIBLE
        view.findViewById<View>(R.id.main_content).visibility = View.GONE

        view.findViewById<ImageView>(R.id.error_icon).apply {
            setImageResource(R.drawable.ic_error_outline_24)
            imageTintList =
                android.content.res.ColorStateList.valueOf(
                    resolveThemeColor(R.attr.colorError),
                )
        }

        view.findViewById<View>(R.id.benefits_section).visibility = View.GONE

        view.findViewById<TextView>(R.id.error_message).text = message
        view.findViewById<Button>(R.id.sign_in_button).setOnClickListener {
            animateButtonClick(it) {
                launchFirebaseSignIn()
            }
        }
    }

    private fun showNotAuthenticated() {
        if (!isAdded) return
        stopLoadingTimeout()

        // Show main_content with null profile so RA login is still accessible
        val view = requireView()
        view.findViewById<View>(R.id.loading_layout).visibility = View.GONE
        view.findViewById<View>(R.id.error_layout).visibility = View.GONE
        view.findViewById<View>(R.id.main_content).visibility = View.VISIBLE

        // Show user profile card in "not signed in" state
        updateUserProfileCard(null)

        // Get connected accounts to update RA/Discord cards
        viewModel.connectedAccounts.value?.let { accounts ->
            updateDiscordCard(accounts.discord)
            updateRetroAchievementsCard(accounts.retroAchievements)
            updateConnectedAccountsEmptyHint(accounts)
            setupAllClickListeners(accounts)
        }

        // Hide Google-dependent sections
        hideGoogleDependentSections()

        setupPrivacyLinksFooter()
        announceToAccessibilityService(getString(R.string.account_management_title))
    }

    /**
     * Hide sections that require Google/Firebase authentication
     */
    private fun hideGoogleDependentSections() {
        if (!isAdded) return
        val view = requireView()

        // Hide Account Actions header
        view.findViewById<View>(R.id.account_actions_header)?.visibility = View.GONE

        // Hide Cross-Device Sync card
        view.findViewById<View>(R.id.cross_device_sync_card)?.visibility = View.GONE

        // Hide Data Export card
        view.findViewById<View>(R.id.data_export_card)?.visibility = View.GONE

        // Hide Account Deletion card
        view.findViewById<View>(R.id.account_deletion_card)?.visibility = View.GONE

        // Hide Discord card (RA card remains visible)
        view.findViewById<View>(R.id.discord_account_card)?.visibility = View.GONE
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

        signInLauncher.launch(PcSignInActivity.wrap(requireContext(), signInIntent))
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
        if (!isAdded) return
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
        if (!isAdded) return
        val exportData = viewModel.exportedData.value
        if (exportData != null) {
            try {
                requireContext().contentResolver.openOutputStream(uri)?.use { outputStream ->
                    outputStream.write(exportData.toByteArray(Charsets.UTF_8))
                }

                showExportSuccessDialog(uri)
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
        if (!isAdded) return
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

    private fun setupPrivacyLinksInCard(cardView: View) {
        cardView.findViewById<TextView>(R.id.privacy_policy_link)?.setOnClickListener {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://www.yabasanshiro.com/privacy"))
            startActivity(intent)
        }
        cardView.findViewById<TextView>(R.id.terms_of_service_link)?.setOnClickListener {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://www.yabasanshiro.com/terms-of-use"))
            startActivity(intent)
        }
    }

    private fun setupPrivacyLinksFooter() {
        if (!isAdded) return
        val view = requireView()
        view.findViewById<TextView>(R.id.privacy_policy_link_footer)?.setOnClickListener {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://www.yabasanshiro.com/privacy"))
            startActivity(intent)
        }
        view.findViewById<TextView>(R.id.terms_of_service_link_footer)?.setOnClickListener {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://www.yabasanshiro.com/terms-of-use"))
            startActivity(intent)
        }
    }

    private fun openUrl(url: String) {
        try {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
            startActivity(intent)
        } catch (e: Exception) {
            showUserFriendlyError(
                message = getString(R.string.failed_to_open_url, e.message),
                errorType = ErrorType.ERROR,
                showRetry = true,
                retryAction = { openUrl(url) },
            )
        }
    }

    // Error display system

    enum class ErrorType {
        ERROR,
        WARNING,
        INFO,
        SUCCESS,
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
        if (!isAdded) return
        try {
            val dialog =
                MaterialAlertDialogBuilder(requireContext())
                    .setTitle(title)
                    .apply {
                        if (message != null) setMessage(message)
                        if (view != null) setView(view)

                        if (showIcon) {
                            val iconRes =
                                when (errorType) {
                                    ErrorType.ERROR -> R.drawable.ic_error_outline_24
                                    ErrorType.WARNING -> android.R.drawable.ic_dialog_alert
                                    ErrorType.INFO -> android.R.drawable.ic_dialog_info
                                    ErrorType.SUCCESS -> android.R.drawable.ic_input_add
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

            dialog.setOnShowListener {
                dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE)?.apply {
                    val colorAttr =
                        when (errorType) {
                            ErrorType.ERROR -> R.attr.colorError
                            ErrorType.WARNING -> R.attr.colorError
                            ErrorType.INFO -> R.attr.colorPrimary
                            ErrorType.SUCCESS -> R.attr.colorPrimary
                        }
                    setTextColor(resolveThemeColor(colorAttr))
                }
            }

            dialog.show()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to show unified dialog: ${e.message}")
            Toast.makeText(requireContext(), title, Toast.LENGTH_LONG).show()
        }
    }

    private fun showUserFriendlyError(
        message: String,
        errorType: ErrorType = ErrorType.ERROR,
        actionText: String? = null,
        action: (() -> Unit)? = null,
        showRetry: Boolean = false,
        retryAction: (() -> Unit)? = null,
    ) {
        if (!isAdded) return
        try {
            if (message.length > 50) {
                val snackbar = Snackbar.make(rootView, message, Snackbar.LENGTH_LONG)

                val colorAttr =
                    when (errorType) {
                        ErrorType.ERROR -> R.attr.colorError
                        ErrorType.WARNING -> R.attr.colorError
                        ErrorType.INFO -> R.attr.colorPrimary
                        ErrorType.SUCCESS -> R.attr.colorPrimary
                    }
                snackbar.view.backgroundTintList = android.content.res.ColorStateList.valueOf(
                    resolveThemeColor(colorAttr),
                )

                if (showRetry && retryAction != null) {
                    snackbar.setAction(getString(R.string.retry)) { retryAction() }
                } else if (actionText != null && action != null) {
                    snackbar.setAction(actionText) { action() }
                }

                snackbar.show()
            } else {
                Toast.makeText(requireContext(), message, Toast.LENGTH_LONG).show()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to show user-friendly error: ${e.message}")
            Toast.makeText(requireContext(), message, Toast.LENGTH_LONG).show()
        }
    }

    private fun handleConnectionError(
        exception: Exception,
        context: String = "",
    ) {
        if (!isAdded) return
        val (userMessage, solution) =
            when {
                exception.message?.contains("network", ignoreCase = true) == true ||
                    exception.message?.contains("timeout", ignoreCase = true) == true -> {
                    Pair(
                        getString(R.string.network_connection_error),
                        getString(R.string.check_internet_connection),
                    )
                }
                context.contains("discord", ignoreCase = true) -> {
                    Pair(
                        getString(R.string.discord_connection_failed),
                        getString(R.string.discord_oauth_troubleshooting),
                    )
                }
                context.contains("retroachievements", ignoreCase = true) -> {
                    Pair(
                        getString(R.string.retroachievements_api_error),
                        getString(R.string.check_credentials_and_connectivity),
                    )
                }
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

    private fun showErrorDetailsDialog(
        exception: Exception,
        context: String,
    ) {
        if (!isAdded) return
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

    private fun copyErrorToClipboard(errorText: String) {
        if (!isAdded) return
        try {
            val clipboard = requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            val clip = ClipData.newPlainText(getString(R.string.error_report), errorText)
            clipboard.setPrimaryClip(clip)

            showUserFriendlyError(
                message = getString(R.string.error_details_copied),
                errorType = ErrorType.SUCCESS,
            )
        } catch (e: Exception) {
            Log.e(TAG, "Failed to copy error to clipboard: ${e.message}")
            showUserFriendlyError(
                message = getString(R.string.failed_to_copy_error),
                errorType = ErrorType.ERROR,
            )
        }
    }

    private fun setupFontSizeSupport() {
        if (!isAdded) return
        val fontScale = resources.configuration.fontScale

        if (fontScale > 1.3f) {
            adjustLayoutForLargeFonts()
        }

        Log.d(TAG, "Font scale: $fontScale")
    }

    private fun adjustLayoutForLargeFonts() {
        if (!isAdded) return
        try {
            adjustTextViewForLargeFont(R.id.user_display_name, maxLines = 2)
            adjustTextViewForLargeFont(R.id.user_email, maxLines = 2)
            adjustTextViewForLargeFont(R.id.last_login_time, maxLines = 2)

            adjustTextViewForLargeFont(R.id.discord_status, maxLines = 1)
            adjustTextViewForLargeFont(R.id.discord_username, maxLines = 2)
            adjustTextViewForLargeFont(R.id.discord_display_name, maxLines = 2)

            adjustTextViewForLargeFont(R.id.ra_status, maxLines = 1)
            adjustTextViewForLargeFont(R.id.ra_username, maxLines = 2)

            adjustTextViewForLargeFont(R.id.error_message, maxLines = 4)

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

            adjustTextViewForLargeFont(R.id.privacy_policy_link, maxLines = 2)
            adjustTextViewForLargeFont(R.id.terms_of_service_link, maxLines = 2)
            adjustTextViewForLargeFont(R.id.privacy_policy_link_footer, maxLines = 2)
            adjustTextViewForLargeFont(R.id.terms_of_service_link_footer, maxLines = 2)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to adjust layout for large fonts: ${e.message}")
        }
    }

    private fun adjustTextViewForLargeFont(
        viewId: Int,
        maxLines: Int = 1,
    ) {
        if (!isAdded) return
        requireView().findViewById<TextView>(viewId)?.apply {
            this.maxLines = maxLines
            ellipsize = TextUtils.TruncateAt.END

            val fontScale = resources.configuration.fontScale
            if (fontScale > 1.5f) {
                val currentSizeSp = textSize / (resources.displayMetrics.density * fontScale)
                val adjustedSize = currentSizeSp * 0.9f
                setTextSize(TypedValue.COMPLEX_UNIT_SP, adjustedSize)
            }
        }
    }

    private fun adjustButtonForLargeFont(viewId: Int) {
        if (!isAdded) return
        requireView().findViewById<Button>(viewId)?.apply {
            val fontScale = resources.configuration.fontScale

            val baseMinHeight = 48
            val adjustedMinHeight = (baseMinHeight * fontScale * resources.displayMetrics.density).toInt()
            minHeight = adjustedMinHeight

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

    private fun resolveThemeColor(attr: Int): Int {
        val typedValue = TypedValue()
        requireContext().theme.resolveAttribute(attr, typedValue, true)
        return typedValue.data
    }

    override fun onDestroyView() {
        super.onDestroyView()
        stopLoadingTimeout()
    }

    companion object {
        private const val TAG = "AccountManagementFragment"

        fun newInstance(): AccountManagementFragment = AccountManagementFragment()
    }
}
