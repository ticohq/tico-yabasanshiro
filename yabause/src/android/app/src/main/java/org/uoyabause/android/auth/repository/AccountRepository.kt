package org.uoyabause.android.auth.repository

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.FragmentActivity
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.IdpResponse
import com.google.android.gms.auth.api.signin.GoogleSignIn
import com.google.android.gms.auth.api.signin.GoogleSignInOptions
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.FirebaseFirestore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaTypeOrNull
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import org.uoyabause.android.auth.DiscordAuthManager
import org.uoyabause.android.auth.FirebaseAuthManager
import org.uoyabause.android.auth.RetroAchievementsAuthManager
import org.uoyabause.android.auth.models.AccountConnectionState
import org.uoyabause.android.auth.models.ConnectedAccountsState
import org.uoyabause.android.auth.models.UserProfile
import org.uoyabause.android.auth.ui.PcSignInActivity
import org.uoyabause.android.auth.utils.AccountConstants
import kotlin.coroutines.resume

/**
 * Repository for managing all authentication-related operations
 *
 * Central repository that coordinates authentication operations across multiple providers
 * (Firebase, Discord, RetroAchievements) and manages user account data.
 *
 * ## Responsibilities
 * - User profile management and retrieval
 * - Account linking/unlinking operations
 * - PIN code generation for cross-device synchronization
 * - User data export for GDPR compliance
 * - Account deletion with proper cleanup
 * - Coordination between different authentication providers
 *
 * ## Architecture Notes
 * - Implements Repository pattern for data abstraction
 * - Uses coroutines for asynchronous operations
 * - Handles network communication with proper error handling
 * - Manages Firebase Firestore integration
 *
 * @property context Android context for accessing resources
 * @property firebaseAuthManager Manager for Firebase authentication operations
 * @property discordAuthManager Manager for Discord OAuth operations
 * @property retroAchievementsAuthManager Manager for RetroAchievements API operations
 *
 * @see FirebaseAuthManager
 * @see DiscordAuthManager
 * @see RetroAchievementsAuthManager
 */
class AccountRepository(
    private val context: Context,
    private val firebaseAuthManager: FirebaseAuthManager = FirebaseAuthManager(context),
    private val discordAuthManager: DiscordAuthManager = DiscordAuthManager(context),
    private val retroAchievementsAuthManager: RetroAchievementsAuthManager = RetroAchievementsAuthManager.getInstance(context),
) {
    companion object {
        private const val TAG = "AccountRepository"
        const val RC_SIGN_IN = 9001
    }

    private val firestore = FirebaseFirestore.getInstance()
    private var currentActivity: Activity? = null
    private var signInResult: IdpResponse? = null
    private var signInLauncher: ActivityResultLauncher<Intent>? = null

    // Track last known Firebase user for detecting user switches
    private var lastKnownFirebaseUid: String? = null

    init {
        // Initialize last known Firebase user ID
        lastKnownFirebaseUid = FirebaseAuth.getInstance().currentUser?.uid

        // Set up RetroAchievements UI update callback
        retroAchievementsAuthManager.onUIUpdateRequired = {
            // Notify that UI should be refreshed due to RA state change
            onAccountDataChanged?.invoke()
        }
    }

    // Callback for notifying when account data changes (e.g., auto-login)
    var onAccountDataChanged: (() -> Unit)? = null

    /**
     * Check if Firebase user has switched and handle RA login state accordingly
     * @param currentFirebaseUid Current Firebase user ID
     */
    private fun checkAndHandleUserSwitch(currentFirebaseUid: String) {
        if (lastKnownFirebaseUid != null && lastKnownFirebaseUid != currentFirebaseUid) {
            Log.d(TAG, "Firebase user switched from $lastKnownFirebaseUid to $currentFirebaseUid in AccountRepository")

            // Clear RetroAchievements login state through the auth manager
            if (retroAchievementsAuthManager.isRetroAchievementsLoggedIn()) {
                Log.d(TAG, "Logging out from RetroAchievements due to user switch")
                retroAchievementsAuthManager.logoutRetroAchievements()
            }

            // Trigger auto-login check for the new user
            Log.d(TAG, "Triggering auto-login check for new user: $currentFirebaseUid")
            retroAchievementsAuthManager.triggerAutoLoginForCurrentUser()
        } else if (lastKnownFirebaseUid == null) {
            // First time login or app restart with user already logged in
            Log.d(TAG, "Firebase user signed in: $currentFirebaseUid in AccountRepository")

            // Trigger auto-login check
            Log.d(TAG, "Triggering auto-login check for signed-in user: $currentFirebaseUid")
            retroAchievementsAuthManager.triggerAutoLoginForCurrentUser()
        }
        lastKnownFirebaseUid = currentFirebaseUid
    }

    /**
     * Set the current activity for sign-in operations
     */
    fun setActivity(activity: Activity?) {
        currentActivity = activity
    }

    /**
     * Manually check for user switch
     * RA state is no longer cleared on Firebase logout (RA is independent)
     */
    fun checkUserSwitchAndClearRAState() {
        val currentUser = FirebaseAuth.getInstance().currentUser
        if (currentUser != null) {
            checkAndHandleUserSwitch(currentUser.uid)
        } else if (lastKnownFirebaseUid != null) {
            Log.d(TAG, "Firebase user logged out completely (RA state preserved)")
            lastKnownFirebaseUid = null
        }
    }

    /**
     * Set the sign-in result from Firebase Auth UI
     * This should be called from onActivityResult after successful sign-in
     *
     * @param response The IdpResponse from Firebase Auth UI
     */
    fun setSignInResult(response: IdpResponse?) {
        signInResult = response
        if (response?.idpToken != null) {
            Log.d(TAG, "Received new idpToken from sign-in")
        }
    }

    /**
     * Data class for Discord user information
     */
    private data class DiscordUserInfo(
        val username: String,
        val displayName: String,
        val avatarUrl: String?,
        val lastSyncTime: Long,
    )

    /**
     * Get current user profile from Firebase Auth
     */
    suspend fun getUserProfile(): Result<UserProfile?> =
        withContext(Dispatchers.IO) {
            try {
                val currentUser = FirebaseAuth.getInstance().currentUser
                if (currentUser != null) {
                    val profile =
                        UserProfile(
                            uid = currentUser.uid,
                            displayName = currentUser.displayName ?: "Unknown User",
                            email = currentUser.email,
                            photoUrl = currentUser.photoUrl?.toString(),
                            accountProvider = getAccountProvider(currentUser),
                            lastLoginTime = currentUser.metadata?.lastSignInTimestamp ?: System.currentTimeMillis(),
                            isEmailVerified = currentUser.isEmailVerified,
                            phoneNumber = currentUser.phoneNumber,
                        )
                    Result.success(profile)
                } else {
                    Result.success(null)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error getting user profile", e)
                Result.failure(e)
            }
        }

    /**
     * Get connection status for all authentication services
     */
    suspend fun getConnectedAccountsStatus(): Result<ConnectedAccountsState> =
        withContext(Dispatchers.IO) {
            try {
                val firebaseState = getFirebaseConnectionState()
                val discordState = getDiscordConnectionState()
                val retroAchievementsState = getRetroAchievementsConnectionState()

                val connectedAccountsState =
                    ConnectedAccountsState(
                        firebase = firebaseState,
                        discord = discordState,
                        retroAchievements = retroAchievementsState,
                    )
                Result.success(connectedAccountsState)
            } catch (e: Exception) {
                Log.e(TAG, "Error getting connected accounts status", e)
                Result.failure(e)
            }
        }

    /**
     * Link Discord account
     */
    suspend fun linkDiscordAccount(): Result<Boolean> =
        withContext(Dispatchers.IO) {
            try {
                // Start the Discord OAuth flow
                discordAuthManager.startDiscordLogin()

                // Clean up any old Firebase Discord data for privacy
                val currentUser = FirebaseAuth.getInstance().currentUser
                if (currentUser != null) {
                    try {
                        discordAuthManager.cleanupFirebaseDiscordData(currentUser.uid)
                    } catch (e: Exception) {
                        // Don't fail the linking process if cleanup fails
                        Log.w(TAG, "Failed to clean up old Firebase Discord data, but continuing: ${e.message}")
                    }
                }

                Result.success(true)
            } catch (e: Exception) {
                Log.e(TAG, "Error linking Discord account", e)
                Result.failure(e)
            }
        }

    /**
     * Unlink Discord account
     */
    suspend fun unlinkDiscordAccount(): Result<Boolean> =
        withContext(Dispatchers.IO) {
            try {
                val currentUser = FirebaseAuth.getInstance().currentUser
                if (currentUser != null) {
                    val success = discordAuthManager.unlinkDiscord(currentUser.uid)
                    Result.success(success)
                } else {
                    Result.failure(Exception("No authenticated user"))
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error unlinking Discord account", e)
                Result.failure(e)
            }
        }

    /**
     * Login to RetroAchievements
     * No longer requires Firebase login
     */
    suspend fun loginRetroAchievements(
        username: String,
        apiKey: String,
    ): Result<Boolean> =
        withContext(Dispatchers.IO) {
            try {
                Log.d(TAG, "Starting RetroAchievements login for user: $username")

                // Store initial login state
                val initialLoginState = retroAchievementsAuthManager.isRetroAchievementsLoggedIn()
                Log.d(TAG, "Initial login state: $initialLoginState")

                // Start the login process
                retroAchievementsAuthManager.loginRetroAchievements(username, apiKey)

                // Poll for login state change with timeout
                var attempts = 0
                val maxAttempts = 30 // 15 seconds (500ms * 30)

                while (attempts < maxAttempts) {
                    kotlinx.coroutines.delay(500) // Wait 500ms between checks
                    attempts++

                    val currentLoginState = retroAchievementsAuthManager.isRetroAchievementsLoggedIn()
                    val currentUsername = retroAchievementsAuthManager.getCurrentRAUsername()

                    Log.d(TAG, "Login check attempt $attempts: loggedIn=$currentLoginState, username=$currentUsername")
                    Log.d(TAG, "Comparing usernames: expected='$username', actual='$currentUsername', equal=${currentUsername == username}")

                    // Check if login succeeded - if logged in successfully, accept any valid username
                    if (currentLoginState && !currentUsername.isNullOrEmpty()) {
                        Log.d(TAG, "RetroAchievements login successful for user: $currentUsername (requested: $username)")
                        // Try to fetch user stats after successful login
                        fetchRetroAchievementsStats(currentUsername)
                        return@withContext Result.success(true)
                    }

                    // Check if we're clearly not logged in and it's been a reasonable time
                    if (attempts >= 10 && !currentLoginState) {
                        Log.e(TAG, "RetroAchievements login failed - not logged in after ${attempts * 500}ms")
                        return@withContext Result.failure(Exception("Login failed"))
                    }
                }

                // Timeout reached
                Log.e(TAG, "RetroAchievements login timed out after ${maxAttempts * 500}ms")
                Result.failure(Exception("Login timed out"))
            } catch (e: Exception) {
                Log.e(TAG, "Error logging into RetroAchievements", e)
                Result.failure(e)
            }
        }

    /**
     * Logout from RetroAchievements
     */
    suspend fun logoutRetroAchievements(): Result<Boolean> =
        withContext(Dispatchers.IO) {
            try {
                retroAchievementsAuthManager.logoutRetroAchievements()
                Result.success(true)
            } catch (e: Exception) {
                Log.e(TAG, "Error logging out from RetroAchievements", e)
                Result.failure(e)
            }
        }

    /**
     * Export user data in GDPR-compliant format
     * Provides all personal data in structured, machine-readable JSON format
     */
    suspend fun exportUserData(): Result<String> =
        withContext(Dispatchers.IO) {
            try {
                val currentUser = FirebaseAuth.getInstance().currentUser
                if (currentUser == null) {
                    return@withContext Result.failure(Exception("User not authenticated"))
                }

                val userProfile = getUserProfile().getOrNull()
                val connectedAccounts = getConnectedAccountsStatus().getOrNull()

                // GDPR-compliant export data structure
                val exportData = mutableMapOf<String, Any?>()

                // GDPR metadata
                exportData["exportMetadata"] =
                    mapOf(
                        "dataSubject" to currentUser.email,
                        "exportDate" to java.text
                            .SimpleDateFormat(
                                "yyyy-MM-dd'T'HH:mm:ss.SSS'Z'",
                                java.util.Locale.US,
                            ).apply { timeZone = java.util.TimeZone.getTimeZone("UTC") }
                            .format(java.util.Date()),
                        "legalBasis" to "GDPR Article 20 - Right to data portability",
                        "dataFormat" to "JSON",
                        "appVersion" to
                            try {
                                context.packageManager.getPackageInfo(context.packageName, 0).versionName
                            } catch (e: Exception) {
                                "Unknown"
                            },
                        "dataController" to "YabaSanshiro Team",
                        "contactEmail" to "support@yabasanshiro.com",
                    )

                // Personal data provided by user
                exportData["userProfile"] =
                    mapOf(
                        "uid" to userProfile?.uid,
                        "displayName" to userProfile?.displayName,
                        "email" to userProfile?.email,
                        "photoUrl" to userProfile?.photoUrl,
                        "accountProvider" to userProfile?.accountProvider,
                        "isEmailVerified" to userProfile?.isEmailVerified,
                        "phoneNumber" to userProfile?.phoneNumber,
                        "lastLoginTime" to userProfile?.lastLoginTime,
                        "accountCreationTime" to currentUser.metadata?.creationTimestamp,
                    )

                // Connected account information (Discord data is now stored locally for privacy)
                exportData["connectedAccounts"] =
                    mapOf(
                        "firebase" to
                            mapOf(
                                "isConnected" to connectedAccounts?.firebase?.isConnected,
                                "lastSyncTime" to connectedAccounts?.firebase?.lastSyncTime,
                            ),
                        "discord" to
                            mapOf(
                                "note" to "Discord account data is stored locally on device for privacy protection",
                                "isConnected" to connectedAccounts?.discord?.isConnected,
                                "dataLocation" to "local_device_storage",
                            ),
                        "retroAchievements" to
                            mapOf(
                                "note" to "RetroAchievements account data is stored locally on device for privacy protection",
                                "isConnected" to connectedAccounts?.retroAchievements?.isConnected,
                                "username" to connectedAccounts?.retroAchievements?.username,
                                "stats" to connectedAccounts?.retroAchievements?.additionalInfo,
                                "dataLocation" to "local_device_storage",
                                "lastSyncTime" to connectedAccounts?.retroAchievements?.lastSyncTime,
                            ),
                    )

                // App usage data
                exportData["usageData"] =
                    mapOf(
                        "devicePinHistory" to getPinGenerationHistory(),
                        "lastExportDate" to System.currentTimeMillis(),
                    )

                // Data categories and retention information
                exportData["dataCategories"] =
                    listOf(
                        mapOf(
                            "category" to "Profile Information",
                            "description" to "User account details and authentication data",
                            "retentionPeriod" to "Until account deletion",
                            "sources" to listOf("user_input", "firebase_auth"),
                        ),
                        mapOf(
                            "category" to "Connected Accounts",
                            "description" to
                                "Third-party account connections and sync data (Discord and RetroAchievements data stored locally only)",
                            "retentionPeriod" to "Until disconnected by user",
                            "sources" to listOf("user_action", "third_party_apis"),
                        ),
                        mapOf(
                            "category" to "Cross-device Sync",
                            "description" to "PIN generation history for device synchronization",
                            "retentionPeriod" to "30 days",
                            "sources" to listOf("app_usage"),
                        ),
                    )

                // Privacy information
                exportData["privacyInformation"] =
                    mapOf(
                        "privacyPolicyUrl" to "https://www.yabasanshiro.com/privacy",
                        "termsOfServiceUrl" to "https://www.yabasanshiro.com/terms-of-use",
                        "dataProcessingPurposes" to
                            listOf(
                                "Account authentication and management",
                                "Cross-device game data synchronization",
                                "Third-party service integration",
                                "User experience enhancement",
                            ),
                        "dataRecipients" to
                            listOf(
                                "Firebase (Google Cloud Platform)",
                                "Discord (local device storage only - not transmitted to our servers)",
                                "RetroAchievements (when connected)",
                            ),
                        "yourRights" to
                            listOf(
                                "Right to access your data",
                                "Right to rectification",
                                "Right to erasure",
                                "Right to data portability",
                                "Right to object to processing",
                            ),
                    )

                // Convert to properly formatted JSON
                val jsonObject = JSONObject(exportData)
                val jsonString = jsonObject.toString(2) // 2-space indentation for readability

                Log.d(TAG, "GDPR-compliant data export completed for user: ${currentUser.email}")
                Result.success(jsonString)
            } catch (e: Exception) {
                Log.e(TAG, "Error exporting user data", e)
                Result.failure(e)
            }
        }

    /**
     * Get PIN generation history for export (last 30 days)
     */
    private fun getPinGenerationHistory(): List<Map<String, Any>> = try {
        val prefs = context.getSharedPreferences("device_pin", Context.MODE_PRIVATE)
        val history = mutableListOf<Map<String, Any>>()

        // Add current PIN info if exists
        val currentPin = prefs.getString("current_pin", null)
        val pinExpiration = prefs.getLong("pin_expiration", 0)

        if (currentPin != null) {
            history.add(
                mapOf(
                    "type" to "pin_generation",
                    "timestamp" to (System.currentTimeMillis() - AccountConstants.PIN_CODE_VALIDITY_MILLIS),
                    "expirationTime" to pinExpiration,
                    "status" to if (pinExpiration > System.currentTimeMillis()) "active" else "expired",
                ),
            )
        }

        history
    } catch (e: Exception) {
        Log.w(TAG, "Could not retrieve PIN history", e)
        emptyList()
    }

    /**
     * Delete user account and all associated data
     */
    suspend fun deleteUserAccount(): Result<Boolean> =
        withContext(Dispatchers.IO) {
            try {
                // Unlink all external accounts first
                unlinkDiscordAccount()
                logoutRetroAchievements()

                // Delete Firebase account
                val currentUser = FirebaseAuth.getInstance().currentUser
                currentUser?.delete()

                Result.success(true)
            } catch (e: Exception) {
                Log.e(TAG, "Error deleting user account", e)
                Result.failure(e)
            }
        }

    /**
     * Generate device PIN directly using the stored idpToken from setSignInResult
     * This should be called after setSignInResult() with a fresh IdpResponse
     *
     * @return The generated PIN or null if failed
     */
    suspend fun generateDevicePINDirect(): String? =
        withContext(Dispatchers.IO) {
            try {
                Log.d(TAG, "Generating PIN with stored idpToken")

                // Generate PIN using the stored idpToken
                val pin = generateRandomPIN()

                if (pin != null) {
                    // Store PIN with expiration time
                    storePINWithExpiration(pin)
                    Log.d(TAG, "PIN generated successfully: $pin")
                } else {
                    Log.e(TAG, "Failed to generate PIN - no valid idpToken")
                }

                return@withContext pin
            } catch (e: Exception) {
                Log.e(TAG, "Error generating PIN directly", e)
                return@withContext null
            }
        }

    /**
     * Generate device PIN for cross-device sync (without activity - will fail)
     * This method requires an activity. Use the overloaded version with FragmentActivity parameter.
     *
     * @return Always returns failure with message to use the overloaded method
     */
    suspend fun generateDevicePIN(): Result<String> =
        withContext(Dispatchers.IO) {
            Log.e(TAG, "generateDevicePIN() called without activity. Use generateDevicePIN(activity) instead.")
            Result.failure(Exception("Activity required. Call generateDevicePIN(activity) to automatically sign in and generate PIN."))
        }

    /**
     * Generate device PIN for cross-device sync
     * This automatically triggers re-authentication to get a fresh idpToken (valid for 1 hour)
     *
     * Usage example:
     * ```kotlin
     * lifecycleScope.launch {
     *     val result = accountRepository.generateDevicePIN(this@MyActivity)
     *     result.fold(
     *         onSuccess = { pin ->
     *             // Display the generated PIN
     *             showPinDialog(pin)
     *         },
     *         onFailure = { error ->
     *             // Handle error
     *             showError(error.message)
     *         }
     *     )
     * }
     * ```
     *
     * @param activity The FragmentActivity to use for sign-in (required)
     * @return The generated PIN or error
     */
    suspend fun generateDevicePIN(activity: FragmentActivity): Result<String> =
        withContext(Dispatchers.IO) {
            try {
                // Since idpToken expires after 1 hour, we always need fresh authentication
                Log.d(TAG, "Starting automatic sign-in for PIN generation")

                // Create sign-in result holder
                var authResult: IdpResponse?

                // Switch to main thread for UI operations
                withContext(Dispatchers.Main) {
                    authResult = triggerSignIn(activity)
                }

                if (authResult != null) {
                    // Store the new idpToken
                    setSignInResult(authResult)

                    // Generate PIN with fresh token
                    val pin = generateRandomPIN()

                    if (pin != null) {
                        // Store PIN with expiration time
                        storePINWithExpiration(pin)
                        Result.success(pin)
                    } else {
                        Result.failure(Exception("Failed to generate PIN after authentication"))
                    }
                } else {
                    Result.failure(Exception("Sign-in was cancelled or failed"))
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error generating device PIN", e)
                Result.failure(e)
            }
        }

    /**
     * Generate device PIN with fresh authentication
     * This method ALWAYS triggers re-authentication to ensure fresh idpToken (expires after 1 hour)
     *
     * Usage example:
     * ```kotlin
     * lifecycleScope.launch {
     *     val result = accountRepository.generateDevicePINWithAuth(this@MyActivity) {
     *         // Trigger Firebase Auth UI sign-in
     *         val signInIntent = AuthUI.getInstance()
     *             .createSignInIntentBuilder()
     *             .setAvailableProviders(providers)
     *             .build()
     *
     *         // Launch sign-in and wait for result
     *         val activityResult = signInLauncher.await() // Using ActivityResultLauncher
     *         IdpResponse.fromResultIntent(activityResult.data)
     *     }
     *
     *     result.fold(
     *         onSuccess = { pin ->
     *             // Display the generated PIN
     *             showPinDialog(pin)
     *         },
     *         onFailure = { error ->
     *             // Handle error
     *             showError(error.message)
     *         }
     *     )
     * }
     * ```
     *
     * @param activity The activity to use for sign-in UI
     * @param onSignInRequired Callback to trigger sign-in flow (WILL ALWAYS BE CALLED)
     * @return The generated PIN or error
     */
    suspend fun generateDevicePINWithAuth(
        activity: Activity,
        onSignInRequired: suspend () -> IdpResponse?,
    ): Result<String> =
        withContext(Dispatchers.IO) {
            try {
                setActivity(activity)

                // ALWAYS trigger re-authentication for fresh idpToken (expires after 1 hour)
                Log.d(TAG, "Triggering re-authentication for fresh idpToken")

                // Switch to Main thread for UI operations
                val response =
                    withContext(Dispatchers.Main) {
                        onSignInRequired()
                    }

                if (response != null) {
                    // Store the new idpToken
                    setSignInResult(response)

                    // Generate PIN with fresh token
                    val pin = generateRandomPIN()

                    if (pin != null) {
                        // Store PIN with expiration time
                        storePINWithExpiration(pin)
                        Result.success(pin)
                    } else {
                        Result.failure(Exception("Failed to generate PIN after re-authentication"))
                    }
                } else {
                    Result.failure(Exception("User cancelled sign-in"))
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error generating device PIN with auth", e)
                Result.failure(e)
            } finally {
                setActivity(null)
            }
        }

    /**
     * Refresh RetroAchievements stats for the current user
     */
    suspend fun refreshRetroAchievementsStats(): Result<Boolean> =
        withContext(Dispatchers.IO) {
            try {
                val prefs = context.getSharedPreferences("retroachievements_auth_global", Context.MODE_PRIVATE)
                val username = prefs.getString("ra_username", null)

                if (!username.isNullOrEmpty()) {
                    fetchRetroAchievementsStats(username)
                    Result.success(true)
                } else {
                    Log.w(TAG, "No RetroAchievements username found")
                    Result.success(false)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error refreshing RetroAchievements stats", e)
                Result.failure(e)
            }
        }

    // Private helper methods

    private fun getFirebaseConnectionState(): AccountConnectionState {
        val currentUser = FirebaseAuth.getInstance().currentUser
        return if (currentUser != null) {
            AccountConnectionState(
                isConnected = true,
                username = currentUser.email,
                displayName = currentUser.displayName,
                avatarUrl = currentUser.photoUrl?.toString(),
                lastSyncTime = currentUser.metadata?.lastSignInTimestamp,
            )
        } else {
            AccountConnectionState(
                isConnected = false,
                createAccountUrl = null, // Firebase doesn't need external account creation
            )
        }
    }

    private suspend fun getDiscordConnectionState(): AccountConnectionState =
        withContext(Dispatchers.IO) {
            val currentUser = FirebaseAuth.getInstance().currentUser
            if (currentUser != null) {
                val isLinked = discordAuthManager.isDiscordLinked(currentUser.uid)
                if (isLinked) {
                    // Get Discord user info from local storage
                    var discordUserInfo = getDiscordUserInfoFromLocal(currentUser.uid)

                    // If user info is incomplete or missing, try to refresh it
                    if (discordUserInfo == null || discordUserInfo.username == "Discord User") {
                        Log.d(TAG, "Discord user info is incomplete, attempting to refresh...")
                        val (refreshSuccess, freshUserInfo) = discordAuthManager.refreshDiscordUserInfo(currentUser.uid)
                        if (refreshSuccess && freshUserInfo != null) {
                            // Use the fresh info directly from API
                            val username = freshUserInfo.getString("username")
                            val displayName = freshUserInfo.optString("global_name", username)
                            val avatarUrl = freshUserInfo.getString("_avatarUrl")

                            discordUserInfo =
                                DiscordUserInfo(
                                    username = username,
                                    displayName = displayName,
                                    avatarUrl = avatarUrl,
                                    lastSyncTime = System.currentTimeMillis(),
                                )

                            Log.d(TAG, "Using fresh Discord info from API: username=$username, displayName=$displayName")
                        }
                    }

                    AccountConnectionState(
                        isConnected = true,
                        username = discordUserInfo?.username ?: "Discord User",
                        displayName = discordUserInfo?.displayName ?: discordUserInfo?.username ?: "Discord User",
                        avatarUrl = discordUserInfo?.avatarUrl,
                        lastSyncTime = discordUserInfo?.lastSyncTime ?: System.currentTimeMillis(),
                    )
                } else {
                    AccountConnectionState(
                        isConnected = false,
                        createAccountUrl = AccountConstants.DISCORD_CREATE_ACCOUNT_URL,
                    )
                }
            } else {
                AccountConnectionState(
                    isConnected = false,
                    createAccountUrl = AccountConstants.DISCORD_CREATE_ACCOUNT_URL,
                )
            }
        }

    private fun getRetroAchievementsConnectionState(): AccountConnectionState {
        Log.d(TAG, "Getting RetroAchievements connection state...")

        // First check if user is actually logged in via auth manager
        val isActuallyLoggedIn = retroAchievementsAuthManager.isRetroAchievementsLoggedIn()
        val currentUsername = retroAchievementsAuthManager.getCurrentRAUsername()

        Log.d(TAG, "RetroAchievements auth manager state: loggedIn=$isActuallyLoggedIn, username=$currentUsername")

        if (isActuallyLoggedIn && !currentUsername.isNullOrEmpty()) {
            Log.d(TAG, "RetroAchievements user is actually logged in: $currentUsername")

            // Get live stats directly from native layer (no caching)
            val liveStats = getLiveRetroAchievementsStatsFromNative(currentUsername)
            Log.d(TAG, "Live stats for $currentUsername: $liveStats")

            return AccountConnectionState(
                isConnected = true,
                username = currentUsername,
                displayName = currentUsername,
                additionalInfo = liveStats,
                lastSyncTime = System.currentTimeMillis(),
            )
        } else {
            Log.d(TAG, "RetroAchievements user is not logged in")

            return AccountConnectionState(
                isConnected = false,
                createAccountUrl = AccountConstants.RETROACHIEVEMENTS_CREATE_ACCOUNT_URL,
            )
        }
    }

    private fun getAccountProvider(user: com.google.firebase.auth.FirebaseUser): String = when {
        user.providerData.any { it.providerId == "google.com" } -> "Google"
        user.providerData.any { it.providerId == "password" } -> "Email"
        user.providerData.any { it.providerId == "phone" } -> "Phone"
        else -> "Unknown"
    }

    /**
     * Trigger Firebase Auth UI sign-in flow
     * @param activity The FragmentActivity to use for sign-in
     * @return IdpResponse if successful, null if cancelled or failed
     */
    private suspend fun triggerSignIn(activity: FragmentActivity): IdpResponse? =
        suspendCancellableCoroutine { continuation ->
            try {
                // Configure authentication providers
                val providers =
                    arrayListOf(
                        AuthUI.IdpConfig.EmailBuilder().build(),
                        AuthUI.IdpConfig.GoogleBuilder().build(),
                    )

                // Create sign-in intent
                val signInIntent =
                    AuthUI
                        .getInstance()
                        .createSignInIntentBuilder()
                        .setAvailableProviders(providers)
                        // .setIsSmartLockEnabled(false) // Disable Smart Lock to ensure fresh sign-in
                        .build()

                // Register for activity result
                val launcher =
                    activity.registerForActivityResult(
                        ActivityResultContracts.StartActivityForResult(),
                    ) { result ->
                        val response = IdpResponse.fromResultIntent(result.data)
                        if (result.resultCode == Activity.RESULT_OK && response != null) {
                            Log.d(TAG, "Sign-in successful, got idpToken")
                            continuation.resume(response)
                        } else {
                            Log.w(TAG, "Sign-in failed or cancelled")
                            continuation.resume(null)
                        }
                    }

                // Launch sign-in
                launcher.launch(PcSignInActivity.wrap(activity, signInIntent))
            } catch (e: Exception) {
                Log.e(TAG, "Error triggering sign-in", e)
                continuation.resume(null)
            }
        }

    /**
     * Generate a PIN by calling the API with idpToken
     * This implementation follows the pattern from ShowPinInFragment.getPinin()
     * IMPORTANT: Requires fresh idpToken from recent sign-in (expires after 1 hour)
     *
     * @return The generated PIN string or null if failed
     */
    private suspend fun generateRandomPIN(): String? =
        withContext(Dispatchers.IO) {
            try {
                // Get fresh idpToken from recent sign-in result ONLY
                // We don't try other sources since idpToken expires after 1 hour
                val idpToken = signInResult?.idpToken

                if (idpToken == null) {
                    Log.e(TAG, "No fresh idpToken available. User must sign in first.")
                    return@withContext null
                }

                return@withContext generateRandomPINWithToken(idpToken)
            } catch (e: Exception) {
                Log.e(TAG, "Error generating PIN", e)
                return@withContext null
            }
        }

    /**
     * Generate a PIN by calling the API with a specific idpToken
     *
     * @param idpToken The idpToken to use for PIN generation
     * @return The generated PIN string or null if failed
     */
    private suspend fun generateRandomPINWithToken(idpToken: String): String? =
        withContext(Dispatchers.IO) {
            try {
                Log.d(TAG, "Got idpToken, calling PIN generation API")

                // Call the PIN generation API
                val url = context.getString(org.devmiyax.yabasanshiro.R.string.url_getLoginPinIn)
                val apiKey = context.getString(org.devmiyax.yabasanshiro.R.string.key_getLoginPinIn)

                if (apiKey.isEmpty()) {
                    Log.e(TAG, "API key for PIN generation is empty")
                    return@withContext null
                }

                val client = OkHttpClient()
                val mediaType = "application/json; charset=utf-8".toMediaTypeOrNull()
                val requestBody = """{"token":"$idpToken"}""".toRequestBody(mediaType)

                val request =
                    Request
                        .Builder()
                        .url(url)
                        .post(requestBody)
                        .addHeader("x-api-key", apiKey)
                        .addHeader("Content-Type", "application/json")
                        .build()

                val response = client.newCall(request).execute()

                if (response.isSuccessful) {
                    val jsonData = response.body?.string()
                    Log.d(TAG, "PIN generation API response: $jsonData")

                    if (jsonData != null) {
                        val jsonObject = JSONObject(jsonData)
                        val pin = jsonObject.getString("pinin")
                        Log.d(TAG, "Successfully generated PIN: $pin")
                        return@withContext pin
                    }
                } else {
                    Log.e(TAG, "PIN generation API failed: ${response.message}")
                }

                return@withContext null
            } catch (e: Exception) {
                Log.e(TAG, "Error generating PIN with token", e)
                return@withContext null
            }
        }

    /**
     * Generate PIN using Firebase ID token from the currently signed-in user.
     * Falls back to this when IdpResponse is unavailable (e.g. Fragment context).
     *
     * @return The generated PIN or null if failed
     */
    suspend fun generateDevicePINWithFirebaseToken(): String? =
        withContext(Dispatchers.IO) {
            try {
                val user = com.google.firebase.auth.FirebaseAuth
                    .getInstance()
                    .currentUser
                    ?: return@withContext null

                // Get a fresh Google OAuth ID token via silentSignIn
                // (Firebase ID token won't work with GoogleAuthProvider::GetCredential on Windows)
                val gso = GoogleSignInOptions
                    .Builder(GoogleSignInOptions.DEFAULT_SIGN_IN)
                    .requestIdToken(context.getString(org.devmiyax.yabasanshiro.R.string.default_web_client_id))
                    .requestEmail()
                    .build()
                val client = GoogleSignIn.getClient(context, gso)
                val account = client.silentSignIn().await()
                val googleIdToken = account.idToken

                if (googleIdToken == null) {
                    Log.e(TAG, "Google ID token is null from silentSignIn")
                    return@withContext null
                }

                Log.d(TAG, "Got Google OAuth ID token via silentSignIn, calling PIN generation API")
                val pin = generateRandomPINWithToken(googleIdToken)
                if (pin != null) {
                    storePINWithExpiration(pin)
                }
                return@withContext pin
            } catch (e: Exception) {
                Log.e(TAG, "Error generating PIN with Google token", e)
                return@withContext null
            }
        }

    private fun storePINWithExpiration(pin: String) {
        val prefs = context.getSharedPreferences("device_pin", Context.MODE_PRIVATE)
        val expirationTime = System.currentTimeMillis() + AccountConstants.PIN_CODE_VALIDITY_MILLIS
        prefs
            .edit()
            .putString("current_pin", pin)
            .putLong("pin_expiration", expirationTime)
            .apply()
    }

    /**
     * Get live RetroAchievements stats directly from native layer
     */
    private fun getLiveRetroAchievementsStatsFromNative(username: String): Map<String, Any> = try {
        Log.d(TAG, "Fetching live RA stats from native layer for: $username")

        // Get user stats from native layer
        val userStats = org.uoyabause.android.achievements.RetroAchievementsManager
            .getUserStatsNative()
        val avatarUrl = org.uoyabause.android.achievements.RetroAchievementsManager
            .getUserAvatarUrlNative()

        Log.d(TAG, "Native getUserStatsNative() returned: $userStats")
        Log.d(TAG, "Native getUserAvatarUrlNative() returned: $avatarUrl")

        if (userStats != null && userStats.size >= 3) {
            val hardcoreScore = userStats[0]
            val softcoreScore = userStats[1]
            val unreadMessages = userStats[2]

            Log.d(
                TAG,
                "Successfully got live native stats: hardcore=$hardcoreScore, softcore=$softcoreScore, unread=$unreadMessages, avatar=$avatarUrl",
            )

            mapOf<String, Any>(
                "hardcoreScore" to hardcoreScore,
                "softcoreScore" to softcoreScore,
                "unreadMessages" to unreadMessages,
                "avatarUrl" to (avatarUrl ?: ""),
            )
        } else {
            Log.w(TAG, "Failed to get user stats from native layer - userStats is null or insufficient size")
            if (userStats != null) {
                Log.w(TAG, "userStats array size: ${userStats.size}, content: ${userStats.joinToString()}")
            }
            getDefaultRetroAchievementsStats()
        }
    } catch (e: Exception) {
        Log.e(TAG, "Error fetching live RetroAchievements stats from native layer", e)
        getDefaultRetroAchievementsStats()
    }

    /**
     * Get cached RetroAchievements stats for a user
     */
    private fun getCachedRetroAchievementsStats(username: String): Map<String, Any>? {
        Log.d(TAG, "getCachedRetroAchievementsStats: Using context: ${context.javaClass.simpleName} (${context.packageName})")
        val prefsName = org.uoyabause.android.auth.RetroAchievementsAuthManager
            .getStatsPreferenceName(username)
        Log.d(TAG, "Reading SharedPreferences file: $prefsName")
        val prefs = context.getSharedPreferences(prefsName, Context.MODE_PRIVATE)
        val lastUpdate = prefs.getLong("last_update", 0)

        @Suppress("UNUSED_VARIABLE")
        val cacheValidTime = 24 * 60 * 60 * 1000 // 24 hours

        // Check if cache is still valid (less than 24 hours old)
        // if (System.currentTimeMillis() - lastUpdate > cacheValidTime) {
        //    return null // Cache expired
        // }

        val hardcoreScore = prefs.getInt("hardcore_score", -1)
        val softcoreScore = prefs.getInt("softcore_score", -1)
        val unreadMessages = prefs.getInt("unread_messages", -1)
        val avatarUrl = prefs.getString("avatar_url", null)

        Log.d(
            TAG,
            "Raw prefs values: hardcore=$hardcoreScore, softcore=$softcoreScore, unread=$unreadMessages, avatar=$avatarUrl, lastUpdate=$lastUpdate",
        )
        Log.d(TAG, "All prefs keys: ${prefs.all.keys}")

        // Return cached data only if it exists (not default -1 values)
        return if (hardcoreScore >= 0 && softcoreScore >= 0) {
            mapOf<String, Any>(
                "hardcoreScore" to hardcoreScore,
                "softcoreScore" to softcoreScore,
                "unreadMessages" to unreadMessages,
                "avatarUrl" to (avatarUrl ?: ""),
            )
        } else {
            null
        }
    }

    /**
     * Get default RetroAchievements stats (when no real data is available)
     */
    private fun getDefaultRetroAchievementsStats(): Map<String, Any> = mapOf<String, Any>(
        "hardcoreScore" to "Loading...",
        "softcoreScore" to "Loading...",
        "unreadMessages" to "Loading...",
        "avatarUrl" to "",
    )

    /**
     * Cache RetroAchievements stats for a user with detailed information
     */
    private fun cacheRetroAchievementsStatsWithDetails(
        username: String,
        hardcoreScore: Int,
        softcoreScore: Int,
        unreadMessages: Int,
        avatarUrl: String?,
    ) {
        val prefsName = org.uoyabause.android.auth.RetroAchievementsAuthManager
            .getStatsPreferenceName(username)
        Log.d(TAG, "Caching stats to SharedPreferences file: $prefsName")
        val prefs = context.getSharedPreferences(prefsName, Context.MODE_PRIVATE)
        val success =
            prefs
                .edit()
                .putInt("hardcore_score", hardcoreScore)
                .putInt("softcore_score", softcoreScore)
                .putInt("unread_messages", unreadMessages)
                .putString("avatar_url", avatarUrl)
                .putLong("last_update", System.currentTimeMillis())
                .commit() // 同期的に保存

        Log.d(
            TAG,
            "Cached RA stats for $username (commit success: $success): hardcore=$hardcoreScore, softcore=$softcoreScore, unread=$unreadMessages, avatar=$avatarUrl",
        )
    }

    /**
     * Fetch RetroAchievements user stats from native layer
     */
    private suspend fun fetchRetroAchievementsStats(username: String) =
        withContext(Dispatchers.IO) {
            try {
                Log.d(TAG, "Fetching RA stats from native layer for user: $username")

                // First check if user is actually logged in at native layer
                val isLoggedIn =
                    try {
                        Class
                            .forName("org.uoyabause.android.achievements.RetroAchievementsManager")
                            .getDeclaredMethod("isUserLoggedInNative")
                            .invoke(null) as Boolean
                    } catch (e: Exception) {
                        Log.w(TAG, "Could not check native login status", e)
                        false
                    }

                Log.d(TAG, "Native login status: $isLoggedIn")

                if (!isLoggedIn) {
                    Log.w(TAG, "User is not logged in at native layer, cannot fetch stats")
                    return@withContext
                }

                // Try to get user stats from native layer
                val userStats = org.uoyabause.android.achievements.RetroAchievementsManager
                    .getUserStatsNative()

                Log.d(TAG, "Native getUserStatsNative() returned: $userStats")

                if (userStats != null && userStats.size >= 3) {
                    // userStats array format: [score, score_softcore, num_unread_messages]
                    val hardcoreScore = userStats[0]
                    val softcoreScore = userStats[1]
                    val unreadMessages = userStats[2]

                    Log.d(TAG, "Parsed stats: hardcore=$hardcoreScore, softcore=$softcoreScore, unread=$unreadMessages")

                    // Get avatar URL
                    val avatarUrl = org.uoyabause.android.achievements.RetroAchievementsManager
                        .getUserAvatarUrlNative()

                    Log.d(TAG, "Avatar URL from native: $avatarUrl")

                    // Cache the stats with actual values (no estimations)
                    cacheRetroAchievementsStatsWithDetails(username, hardcoreScore, softcoreScore, unreadMessages, avatarUrl)

                    Log.d(TAG, "Successfully cached RA stats for $username")
                } else {
                    Log.w(TAG, "No user stats available from native layer - userStats is null or size < 3")
                    if (userStats != null) {
                        Log.w(TAG, "userStats array size: ${userStats.size}, content: ${userStats.joinToString()}")
                    }

                    // Fallback: Cache placeholder data to avoid showing "Loading..." forever
                    Log.d(TAG, "Caching fallback data for $username")
                    cacheRetroAchievementsStatsWithDetails(username, 0, 0, 0, null)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error fetching RetroAchievements stats from native layer", e)
            }
        }

    /**
     * Get Discord user information from local storage (no longer using Firestore for privacy)
     */
    private fun getDiscordUserInfoFromLocal(firebaseUid: String): DiscordUserInfo? {
        return try {
            Log.d(TAG, "Fetching Discord user info from local storage for Firebase UID: $firebaseUid")

            val userInfoMap = discordAuthManager.getDiscordUserInfo(firebaseUid)

            if (userInfoMap != null) {
                val username = userInfoMap["username"] as? String
                val displayName = userInfoMap["displayName"] as? String
                val avatarUrl = userInfoMap["avatarUrl"] as? String
                val updatedAt = userInfoMap["updatedAt"] as? Long ?: System.currentTimeMillis()

                Log.d(TAG, "Found Discord user info locally: username=$username, displayName=$displayName")

                return DiscordUserInfo(
                    username = username ?: displayName ?: "Discord User",
                    displayName = displayName ?: username ?: "Discord User",
                    avatarUrl = avatarUrl,
                    lastSyncTime = updatedAt,
                )
            }

            Log.d(TAG, "No Discord user info found locally for Firebase UID: $firebaseUid")
            null
        } catch (e: Exception) {
            Log.e(TAG, "Error fetching Discord user info from local storage", e)
            null
        }
    }
}
