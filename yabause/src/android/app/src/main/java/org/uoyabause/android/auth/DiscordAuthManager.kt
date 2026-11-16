package org.uoyabause.android.auth

import android.content.Context
import android.net.Uri
import android.util.Base64
import android.util.Log
import androidx.browser.customtabs.CustomTabsIntent
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.auth.FirebaseUser
import com.google.firebase.auth.UserProfileChangeRequest
import com.google.firebase.firestore.FirebaseFirestore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.withContext
import okhttp3.FormBody
import okhttp3.OkHttpClient
import okhttp3.Request
import org.devmiyax.yabasanshiro.R
import org.json.JSONObject
import java.security.MessageDigest
import java.security.SecureRandom

/**
 * Discord OAuth2 authentication provider with PKCE support for Firebase Auth integration
 */
class DiscordAuthManager(
    private val context: Context,
) {
    private val tag = "DiscordAuthManager"
    private val auth = FirebaseAuth.getInstance()
    private val firestore = FirebaseFirestore.getInstance()
    private val client = OkHttpClient()

    init {
        // Perform one-time migration from old storage format
        performOldDataMigration()
    }

    companion object {
        // PKCE constants
        private const val CODE_VERIFIER_LENGTH = 64 // Recommended length (43-128 chars)
        const val EXTRA_CANCELLED = "discord_auth_cancelled"

        // Firestore collection for storing Discord account links
        private const val COLLECTION_DISCORD_LINKS = "discord_links"
    }

    // Configuration from local_security.xml
    private val clientId: String = context.getString(R.string.discord_client_id)
    private val redirectUri: String = context.getString(R.string.discord_redirect_uri)
    private val discordAuthUrl: String = context.getString(R.string.discord_auth_url)
    private val discordTokenUrl: String = context.getString(R.string.discord_token_url)
    private val discordUserUrl: String = context.getString(R.string.discord_user_url)

    // PKCE state is now handled by TokenStorage

/**
     * Generate a code verifier for PKCE (RFC 7636)
     * @return The generated code verifier
     */
    private fun generateCodeVerifier(): String {
        val secureRandom = SecureRandom()
        val codeVerifierBytes = ByteArray(CODE_VERIFIER_LENGTH)
        secureRandom.nextBytes(codeVerifierBytes)
        return Base64.encodeToString(
            codeVerifierBytes,
            Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP,
        )
    }

    /**
     * Generate a code challenge using SHA-256 (S256 method in RFC 7636)
     * @param codeVerifier The code verifier to hash
     * @return The generated code challenge
     */
    private fun generateCodeChallenge(codeVerifier: String): String {
        val bytes = codeVerifier.toByteArray(Charsets.US_ASCII)
        val messageDigest = MessageDigest.getInstance("SHA-256")
        val digest = messageDigest.digest(bytes)
        return Base64.encodeToString(
            digest,
            Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP,
        )
    }

    /**
     * Start the Discord OAuth2 authorization flow with PKCE
     */
    fun startDiscordLogin() {
        // Generate code verifier and challenge
        val tokenStorage = TokenStorage(context)
        val localCodeVerifier = generateCodeVerifier()
        val codeChallenge = generateCodeChallenge(localCodeVerifier)

        // Store the code verifier securely with a 10-minute timeout
        tokenStorage.saveCodeVerifier(localCodeVerifier)

        Log.d(tag, "Generated PKCE Code Verifier: $localCodeVerifier")
        Log.d(tag, "Generated PKCE Code Challenge: $codeChallenge")

        // Build the authorization URL with PKCE parameters
        val authUrl =
            Uri
                .parse(discordAuthUrl)
                .buildUpon()
                .appendQueryParameter("client_id", clientId)
                .appendQueryParameter("redirect_uri", redirectUri)
                .appendQueryParameter("response_type", "code")
                .appendQueryParameter("scope", "identify email")
                .appendQueryParameter("code_challenge", codeChallenge)
                .appendQueryParameter("code_challenge_method", "S256")
                .build()

        try {
            // Launch the authorization URL in a CustomTabsIntent
            val customTabsIntent = CustomTabsIntent.Builder().build()
            customTabsIntent.launchUrl(context, authUrl)
            Log.d(tag, "Opening Discord Auth URL: $authUrl")
        } catch (e: Exception) {
            Log.e(tag, "Error starting Discord authentication", e)
        }
    }

    /**
     * Alias for startDiscordLogin for compatibility with existing code
     */
    fun startAuthorization() {
        startDiscordLogin()
    }

    /**
     * Handle the redirect URI from Discord OAuth2 authorization
     * @param uri The redirect URI with authorization code
     * @return Boolean true if the process succeeded, false otherwise
     */
    suspend fun handleRedirectAndSignIn(uri: Uri): Boolean {
        // Check for errors in the redirect
        val error = uri.getQueryParameter("error")
        if (error != null) {
            val errorDescription = uri.getQueryParameter("error_description") ?: error
            Log.e(tag, "OAuth Error: $error - $errorDescription")
            TokenStorage(context).clearCodeVerifier()
            return false
        }

        // Extract the authorization code
        val code = uri.getQueryParameter("code")
        if (code == null) {
            Log.e(tag, "Authorization code not found in redirect URI")
            TokenStorage(context).clearCodeVerifier()
            return false
        }

        Log.d(tag, "Received authorization code: $code")

        // Retrieve the stored code verifier from secure storage
        val tokenStorage = TokenStorage(context)
        val storedCodeVerifier = tokenStorage.getCodeVerifier()
        if (storedCodeVerifier == null) {
            Log.e(tag, "Code verifier not found - authentication session may have timed out")
            return false
        }

        // Clear the code verifier immediately to prevent reuse
        tokenStorage.clearCodeVerifier()

        try {
            // 1. Exchange the code for access token
            val tokenResponseJson = exchangeCodeForTokenResponse(code, storedCodeVerifier)
            val accessToken = tokenResponseJson.optString("access_token")
            val refreshToken = tokenResponseJson.optString("refresh_token", "")
            val expiresIn = tokenResponseJson.getLong("expires_in")

            Log.d(tag, "Access token obtained successfully")

            // Store tokens securely for future use
            tokenStorage.saveDiscordTokens(accessToken, refreshToken, expiresIn)

            // 2. Fetch user info from Discord
            val userInfo = getUserInfo(accessToken)
            val discordId = userInfo.getString("id")
            Log.d(tag, "Discord user info obtained for ID: $discordId")

            // 3. Update Firebase user profile with Discord info
            val firebaseSuccess = updateFirebaseWithDiscordInfo(discordId, accessToken, userInfo)
            if (!firebaseSuccess) {
                Log.e(tag, "Failed to update Firebase with Discord info")
                return false
            }

            Log.i(tag, "Discord OAuth flow completed successfully for user: ${userInfo.getString("username")}")
            return true
        } catch (e: Exception) {
            Log.e(tag, "Error during Discord authentication process", e)
            return false
        }
    }

    /**
     * Exchange authorization code for access token using PKCE
     * @param code The authorization code from Discord
     * @param codeVerifier The original code verifier used in the authorization request
     * @return JSONObject The token response
     * @throws Exception on network error or failed response
     */
    private suspend fun exchangeCodeForTokenResponse(
        code: String,
        codeVerifier: String,
    ): JSONObject =
        withContext(Dispatchers.IO) {
            val formBody =
                FormBody
                    .Builder()
                    .add("client_id", clientId)
                    .add("grant_type", "authorization_code")
                    .add("code", code)
                    .add("redirect_uri", redirectUri)
                    .add("code_verifier", codeVerifier)
                    .build()

            val request =
                Request
                    .Builder()
                    .url(discordTokenUrl)
                    .post(formBody)
                    .header("Accept", "application/json")
                    .build()

            Log.d(tag, "Requesting token from Discord")
            val response = client.newCall(request).execute()
            val responseBody = response.body?.string()

            if (!response.isSuccessful || responseBody == null) {
                val errorBody = responseBody ?: "Empty body"
                val errorMessage = "Token exchange failed: ${response.code} - $errorBody"
                Log.e(tag, errorMessage)
                response.close()
                throw Exception(errorMessage)
            }

            response.close()
            JSONObject(responseBody)
        }

    /**
     * Fetch user information from Discord API
     * @param accessToken The Discord access token
     * @return JSONObject The user information
     * @throws Exception on network error or failed response
     */
    private suspend fun getUserInfo(accessToken: String): JSONObject =
        withContext(Dispatchers.IO) {
            val request =
                Request
                    .Builder()
                    .url(discordUserUrl)
                    .header("Authorization", "Bearer $accessToken")
                    .header("Accept", "application/json")
                    .build()

            Log.d(tag, "Fetching user info from Discord")
            val response = client.newCall(request).execute()
            val responseBody = response.body?.string()

            if (!response.isSuccessful || responseBody == null) {
                val errorBody = responseBody ?: "Empty body"
                val errorMessage = "Failed to fetch user info: ${response.code} - $errorBody"
                Log.e(tag, errorMessage)
                response.close()
                throw Exception(errorMessage)
            }

            response.close()
            val jsonObject = JSONObject(responseBody)

            // Extract and format user data
            val username = jsonObject.optString("username", "Unknown")
            val discriminator = jsonObject.optString("discriminator", "0000")
            val avatarId = jsonObject.optString("avatar", "")
            val userId = jsonObject.getString("id")

            // Generate avatar URL
            val avatarUrl =
                if (avatarId.isNotEmpty() && avatarId != "null") {
                    "https://cdn.discordapp.com/avatars/$userId/$avatarId.png"
                } else {
                    try {
                        val defaultAvatarIndex = (discriminator.toInt()) % 5
                        "https://cdn.discordapp.com/embed/avatars/$defaultAvatarIndex.png"
                    } catch (e: NumberFormatException) {
                        "https://cdn.discordapp.com/embed/avatars/0.png" // Fallback default
                    }
                }

            Log.d(tag, "Discord User: $username#$discriminator (ID: $userId)")
            Log.d(tag, "Discord Avatar URL: $avatarUrl")

            // Add derived fields to the JSON object
            jsonObject.put("_avatarUrl", avatarUrl)
            jsonObject.put("_displayName", username)

            jsonObject
        }

    /**
     * Update Firebase with Discord user information
     * @param discordId The Discord user ID
     * @param accessToken The Discord access token
     * @param discordUserInfo The Discord user information
     * @return Boolean true if the operation was successful, false otherwise
     */
    private suspend fun updateFirebaseWithDiscordInfo(
        discordId: String,
        @Suppress("UNUSED_PARAMETER") accessToken: String,
        discordUserInfo: JSONObject,
    ): Boolean =
        withContext(Dispatchers.IO) {
            // Get Discord profile information
            val discordDisplayName = discordUserInfo.getString("global_name")
            val discordUsername = discordUserInfo.getString("username")
            val discordAvatarUrl = discordUserInfo.getString("_avatarUrl")

            // Get current Firebase user
            val currentUser = auth.currentUser
            if (currentUser == null) {
                Log.e(tag, "No Firebase user is currently signed in")
                return@withContext false
            }

            try {
                // 1. Store Discord link in Firestore
                storeDiscordLink(currentUser.uid, discordId)

                // 2. Update Firebase user profile with Discord info
                // We'll store the Discord display name and avatar, but preserve the Firebase UID
                updateUserProfile(currentUser, discordDisplayName, discordAvatarUrl)

                // 3. Store additional Discord info in Firestore to ensure we have both IDs
                storeDiscordUserInfo(currentUser.uid, discordId, discordUsername, discordDisplayName, discordAvatarUrl)

                return@withContext true
            } catch (e: Exception) {
                Log.e(tag, "Error updating Firebase with Discord info", e)
                return@withContext false
            }
        }

    /**
     * Store Discord account link locally (no longer using Firestore for privacy)
     * @param firebaseUid The Firebase user ID
     * @param discordId The Discord user ID
     */
    private fun storeDiscordLink(
        firebaseUid: String,
        discordId: String,
    ) {
        try {
            val linkStorage = DiscordLinkStorage(context)
            linkStorage.saveDiscordLink(firebaseUid, discordId)
            Log.d(tag, "Discord link stored locally for user $firebaseUid")
        } catch (e: Exception) {
            Log.e(tag, "Error storing Discord link locally", e)
            throw e
        }
    }

    /**
     * Store detailed Discord user information locally (no longer using Firestore for privacy)
     * @param firebaseUid The Firebase user ID
     * @param discordId The Discord user ID
     * @param discordUsername The Discord username
     * @param discordDisplayName The Discord display name
     * @param discordAvatarUrl The Discord avatar URL
     */
    private fun storeDiscordUserInfo(
        firebaseUid: String,
        discordId: String,
        discordUsername: String,
        discordDisplayName: String,
        discordAvatarUrl: String,
    ) {
        try {
            val linkStorage = DiscordLinkStorage(context)
            linkStorage.saveDiscordUserInfo(
                firebaseUid,
                discordId,
                discordUsername,
                discordDisplayName,
                discordAvatarUrl,
            )
            Log.d(tag, "Discord user info stored locally for user $firebaseUid")
        } catch (e: Exception) {
            Log.e(tag, "Error storing Discord user info locally", e)
            // Don't throw here, as this is an additional step that shouldn't break the flow
            // if it fails
        }
    }

    /**
     * Check if a Firebase user has linked their Discord account (now checking locally)
     * @param firebaseUid The Firebase user ID
     * @return Boolean true if Discord is linked, false otherwise
     */
    fun isDiscordLinked(firebaseUid: String): Boolean = try {
        val linkStorage = DiscordLinkStorage(context)
        linkStorage.isDiscordLinked(firebaseUid)
    } catch (e: Exception) {
        Log.e(tag, "Error checking Discord link status", e)
        false
    }

    /**
     * Unlink Discord account from Firebase user (now clearing local data)
     * @param firebaseUid The Firebase user ID
     * @return Boolean true if unlink was successful, false otherwise
     */
    fun unlinkDiscord(firebaseUid: String): Boolean = try {
        val linkStorage = DiscordLinkStorage(context)
        linkStorage.clearDiscordLink(firebaseUid)

        // Also clear Discord tokens
        val tokenStorage = TokenStorage(context)
        tokenStorage.clearTokens()

        Log.d(tag, "Discord account unlinked for user $firebaseUid")
        true
    } catch (e: Exception) {
        Log.e(tag, "Error unlinking Discord account", e)
        false
    }

    /**
     * Update Firebase user profile with Discord information
     * @param user The Firebase user to update
     * @param displayName The display name from Discord
     * @param photoUrl The avatar URL from Discord
     * @return Boolean true if update was successful, false otherwise
     */
    suspend fun updateFirebaseUserProfile(discordUserInfo: JSONObject): Boolean {
        val currentUser = auth.currentUser ?: return false
        val displayName = discordUserInfo.getString("global_name")
        val photoUrl = discordUserInfo.getString("_avatarUrl")
        return updateUserProfile(currentUser, displayName, photoUrl)
    }

    /**
     * Update Firebase user profile
     * @param user The Firebase user to update
     * @param displayName The display name to set
     * @param photoUrl The photo URL to set
     * @return Boolean true if update was successful, false otherwise
     */
    private suspend fun updateUserProfile(
        user: FirebaseUser,
        displayName: String,
        photoUrl: String,
    ): Boolean =
        withContext(Dispatchers.IO) {
            try {
                // Store the original Firebase user ID and email to ensure we don't lose them
                val originalUid = user.uid
                val originalEmail = user.email

                // Create a profile update that preserves the Firebase user ID
                val profileUpdatesBuilder =
                    UserProfileChangeRequest
                        .Builder()
                        .setDisplayName(displayName)

                try {
                    profileUpdatesBuilder.setPhotoUri(Uri.parse(photoUrl))
                } catch (e: Exception) {
                    Log.w(tag, "Invalid photo URL format: $photoUrl", e)
                }

                // Update the profile
                user.updateProfile(profileUpdatesBuilder.build()).await()

                // Log the update with both IDs to help with debugging
                Log.d(tag, "Updated Firebase profile for user ${user.uid} with Discord display name: $displayName")
                Log.d(tag, "Original Firebase UID: $originalUid, Email: $originalEmail")

                return@withContext true
            } catch (e: Exception) {
                Log.e(tag, "Failed to update Firebase profile", e)
                return@withContext false
            }
        }

    /**
     * Perform one-time migration from old storage format
     */
    private fun performOldDataMigration() {
        try {
            val migrationKey = "discord_migration_completed"
            val migrationPrefs = context.getSharedPreferences("discord_migration", Context.MODE_PRIVATE)

            if (!migrationPrefs.getBoolean(migrationKey, false)) {
                Log.d(tag, "Performing Discord data migration...")
                val linkStorage = DiscordLinkStorage(context)
                linkStorage.migrateFromOldStorage()

                // Mark migration as completed
                migrationPrefs.edit().putBoolean(migrationKey, true).apply()
                Log.d(tag, "Discord data migration completed")
            }
        } catch (e: Exception) {
            Log.e(tag, "Error during Discord data migration", e)
        }
    }

    /**
     * Get Discord user information from local storage
     * @param firebaseUid The Firebase user ID
     * @return Map with Discord user info or null if not found
     */
    fun getDiscordUserInfo(firebaseUid: String): Map<String, Any?>? {
        val linkStorage = DiscordLinkStorage(context)
        return linkStorage.getDiscordUserInfo(firebaseUid)
    }

    /**
     * Clean up old Discord data from Firestore (migration helper)
     * This should be called once to migrate from Firebase to local storage
     * @param firebaseUid The Firebase user ID
     */
    suspend fun cleanupFirebaseDiscordData(firebaseUid: String): Boolean =
        withContext(Dispatchers.IO) {
            try {
                // Delete from discord_links collection
                firestore
                    .collection(COLLECTION_DISCORD_LINKS)
                    .document(firebaseUid)
                    .delete()
                    .await()

                // Delete from user_profiles collection (Discord-related fields only)
                val userProfileRef = firestore.collection("user_profiles").document(firebaseUid)
                val userProfileDoc = userProfileRef.get().await()

                if (userProfileDoc.exists()) {
                    val updates =
                        mapOf(
                            "discordId" to null,
                            "discordUsername" to null,
                            "discordDisplayName" to null,
                            "discordAvatarUrl" to null,
                        )
                    userProfileRef.update(updates).await()
                }

                Log.d(tag, "Successfully cleaned up Firebase Discord data for user: $firebaseUid")
                true
            } catch (e: Exception) {
                Log.w(tag, "Error cleaning up Firebase Discord data (this is expected if data was already cleaned): ${e.message}")
                false
            }
        }

    // clearPkceState method removed as we now use TokenStorage

/**
     * Refresh Discord user information for an already linked account
     * @param firebaseUid The Firebase user ID
     * @return Pair<Boolean, JSONObject?> - success status and user info if available
     */
    suspend fun refreshDiscordUserInfo(firebaseUid: String): Pair<Boolean, JSONObject?> =
        withContext(Dispatchers.IO) {
            try {
                Log.d(tag, "Refreshing Discord user info for Firebase UID: $firebaseUid")

                // First check if Discord is linked
                if (!isDiscordLinked(firebaseUid)) {
                    Log.w(tag, "Discord is not linked for user $firebaseUid")
                    return@withContext Pair(false, null)
                }

                // Get Discord tokens from storage
                val tokenStorage = TokenStorage(context)
                val accessToken = tokenStorage.getAccessToken()

                if (accessToken == null) {
                    Log.w(tag, "No valid Discord access token found, cannot refresh user info")
                    return@withContext Pair(false, null)
                }

                // Fetch latest user info from Discord
                val userInfo = getUserInfo(accessToken)
                val discordId = userInfo.getString("id")
                val discordUsername = userInfo.getString("username")
                val discordDisplayName = userInfo.optString("global_name", discordUsername)
                val discordAvatarUrl = userInfo.getString("_avatarUrl")

                // Update local storage with the latest Discord info (but don't fail if it doesn't work)
                try {
                    storeDiscordUserInfo(firebaseUid, discordId, discordUsername, discordDisplayName, discordAvatarUrl)
                } catch (e: Exception) {
                    Log.w(tag, "Failed to store Discord info locally, but continuing with cached data", e)
                }

                Log.d(tag, "Successfully refreshed Discord user info for $firebaseUid: username=$discordUsername")

                // Return the user info even if local storage write failed
                return@withContext Pair(true, userInfo)
            } catch (e: Exception) {
                Log.e(tag, "Error refreshing Discord user info", e)
                return@withContext Pair(false, null)
            }
        }

    /**
     * TokenStorage for securely storing Discord tokens and PKCE state
     */
    private class TokenStorage(
        context: Context,
    ) {
        private val tag = "DiscordTokenStorage"
        private val prefsName = "discord_auth_prefs"
        private val keyAccessToken = "discord_access_token"
        private val keyRefreshToken = "discord_refresh_token"
        private val keyExpiresAt = "discord_expires_at"
        private val keyCodeVerifier = "discord_code_verifier"
        private val keyCodeVerifierTimestamp = "discord_code_verifier_timestamp"
        private val prefs = context.getSharedPreferences(prefsName, Context.MODE_PRIVATE)

        /**
         * Save Discord tokens to SharedPreferences
         * @param accessToken The Discord access token
         * @param refreshToken The Discord refresh token
         * @param expiresIn The token expiration time in seconds
         */
        fun saveDiscordTokens(
            accessToken: String,
            refreshToken: String,
            expiresIn: Long,
        ) {
            val expiresAtMillis = System.currentTimeMillis() + expiresIn * 1000
            Log.d(tag, "Saving Discord tokens, expires in $expiresIn seconds")
            prefs
                .edit()
                .putString(keyAccessToken, accessToken)
                .putString(keyRefreshToken, refreshToken)
                .putLong(keyExpiresAt, expiresAtMillis)
                .apply()
        }

        /**
         * Get the stored Discord access token
         * @return The access token or null if not found or expired
         */
        fun getAccessToken(): String? {
            val expiresAt = prefs.getLong(keyExpiresAt, 0)
            if (System.currentTimeMillis() > expiresAt) {
                Log.d(tag, "Access token has expired")
                return null
            }
            return prefs.getString(keyAccessToken, null)
        }

        /**
         * Save the PKCE code verifier
         * @param codeVerifier The code verifier to save
         * @param timeoutMinutes How long the code verifier should be valid (in minutes)
         */
        fun saveCodeVerifier(
            codeVerifier: String,
            timeoutMinutes: Int = 10,
        ) {
            val expiresAtMillis = System.currentTimeMillis() + (timeoutMinutes * 60 * 1000)
            Log.d(tag, "Saving PKCE code verifier, expires in $timeoutMinutes minutes")
            prefs
                .edit()
                .putString(keyCodeVerifier, codeVerifier)
                .putLong(keyCodeVerifierTimestamp, expiresAtMillis)
                .apply()
        }

        /**
         * Get the stored PKCE code verifier if it's still valid
         * @return The code verifier or null if not found or expired
         */
        fun getCodeVerifier(): String? {
            val expiresAt = prefs.getLong(keyCodeVerifierTimestamp, 0)
            if (System.currentTimeMillis() > expiresAt) {
                Log.d(tag, "Code verifier has expired")
                clearCodeVerifier()
                return null
            }
            return prefs.getString(keyCodeVerifier, null)
        }

        /**
         * Clear the stored PKCE code verifier
         */
        fun clearCodeVerifier() {
            prefs
                .edit()
                .remove(keyCodeVerifier)
                .remove(keyCodeVerifierTimestamp)
                .apply()
            Log.d(tag, "PKCE code verifier cleared")
        }

        /**
         * Clear all stored Discord tokens and PKCE state
         */
        fun clearTokens() {
            prefs
                .edit()
                .remove(keyAccessToken)
                .remove(keyRefreshToken)
                .remove(keyExpiresAt)
                .remove(keyCodeVerifier)
                .remove(keyCodeVerifierTimestamp)
                .apply()
            Log.d(tag, "Discord tokens and PKCE state cleared")
        }
    }

    /**
     * Local storage for Discord link information (replaces Firestore for privacy)
     * Uses user-specific SharedPreferences to avoid conflicts between multiple users
     */
    private class DiscordLinkStorage(
        context: Context,
    ) {
        private val tag = "DiscordLinkStorage"
        private val context = context

        /**
         * Get user-specific SharedPreferences
         * @param firebaseUid The Firebase user ID
         * @return SharedPreferences for this specific user
         */
        private fun getUserPrefs(firebaseUid: String): android.content.SharedPreferences {
            val prefsName = "discord_links_$firebaseUid"
            return context.getSharedPreferences(prefsName, Context.MODE_PRIVATE)
        }

        /**
         * Save Discord link information locally
         * @param firebaseUid The Firebase user ID
         * @param discordId The Discord user ID
         */
        fun saveDiscordLink(
            firebaseUid: String,
            discordId: String,
        ) {
            val prefs = getUserPrefs(firebaseUid)
            prefs
                .edit()
                .putString("discord_id", discordId)
                .putLong("linked_at", System.currentTimeMillis())
                .apply()
            Log.d(tag, "Discord link saved locally for Firebase UID: $firebaseUid")
        }

        /**
         * Save detailed Discord user information locally
         * @param firebaseUid The Firebase user ID
         * @param discordId The Discord user ID
         * @param discordUsername The Discord username
         * @param discordDisplayName The Discord display name
         * @param discordAvatarUrl The Discord avatar URL
         */
        fun saveDiscordUserInfo(
            firebaseUid: String,
            discordId: String,
            discordUsername: String,
            discordDisplayName: String,
            discordAvatarUrl: String,
        ) {
            val prefs = getUserPrefs(firebaseUid)
            prefs
                .edit()
                .putString("discord_id", discordId)
                .putString("username", discordUsername)
                .putString("display_name", discordDisplayName)
                .putString("avatar_url", discordAvatarUrl)
                .putLong("updated_at", System.currentTimeMillis())
                .apply()
            Log.d(tag, "Discord user info saved locally for Firebase UID: $firebaseUid")
        }

        /**
         * Check if Discord is linked for a Firebase user
         * @param firebaseUid The Firebase user ID
         * @return Boolean true if linked, false otherwise
         */
        fun isDiscordLinked(firebaseUid: String): Boolean {
            val prefs = getUserPrefs(firebaseUid)
            return prefs.contains("discord_id") && !prefs.getString("discord_id", null).isNullOrEmpty()
        }

        /**
         * Get Discord user information for a Firebase user
         * @param firebaseUid The Firebase user ID
         * @return Map with Discord user info or null if not found
         */
        fun getDiscordUserInfo(firebaseUid: String): Map<String, Any?>? {
            val prefs = getUserPrefs(firebaseUid)
            if (!prefs.contains("discord_id")) {
                return null
            }

            return mapOf(
                "discordId" to prefs.getString("discord_id", null),
                "username" to prefs.getString("username", null),
                "displayName" to prefs.getString("display_name", null),
                "avatarUrl" to prefs.getString("avatar_url", null),
                "updatedAt" to prefs.getLong("updated_at", 0),
                "linkedAt" to prefs.getLong("linked_at", 0),
            )
        }

        /**
         * Clear Discord link for a Firebase user
         * @param firebaseUid The Firebase user ID
         */
        fun clearDiscordLink(firebaseUid: String) {
            val prefs = getUserPrefs(firebaseUid)
            prefs
                .edit()
                .clear()
                .apply()

            Log.d(tag, "Discord link cleared locally for Firebase UID: $firebaseUid")
        }

        /**
         * Get all Discord links (for cleanup purposes)
         * @return Set of Firebase UIDs that have Discord links
         */
        fun getAllLinkedFirebaseUids(): Set<String> {
            val linkedUids = mutableSetOf<String>()
            try {
                // List all SharedPreferences files that match our pattern
                val prefsDir = java.io.File(context.applicationInfo.dataDir, "shared_prefs")
                if (prefsDir.exists()) {
                    prefsDir.listFiles()?.forEach { file ->
                        val fileName = file.name
                        if (fileName.startsWith("discord_links_") && fileName.endsWith(".xml")) {
                            // Extract Firebase UID from filename
                            val firebaseUid = fileName.removePrefix("discord_links_").removeSuffix(".xml")
                            if (firebaseUid.isNotEmpty() && isDiscordLinked(firebaseUid)) {
                                linkedUids.add(firebaseUid)
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                Log.w(tag, "Error scanning for linked Discord accounts", e)
            }
            return linkedUids
        }

        /**
         * Migrate old Discord data from the shared preferences file to user-specific files
         * @param oldPrefsName The old SharedPreferences file name
         */
        fun migrateFromOldStorage(oldPrefsName: String = "discord_links") {
            try {
                val oldPrefs = context.getSharedPreferences(oldPrefsName, Context.MODE_PRIVATE)
                val oldData = oldPrefs.all

                if (oldData.isEmpty()) {
                    Log.d(tag, "No old Discord data to migrate")
                    return
                }

                Log.d(tag, "Migrating ${oldData.size} old Discord entries to user-specific storage")

                // Process old data and migrate to new structure
                val processedUids = mutableSetOf<String>()

                oldData.keys.forEach { key ->
                    when {
                        key.startsWith("link_") && !key.endsWith("_linked_at") -> {
                            val firebaseUid = key.removePrefix("link_")
                            if (!processedUids.contains(firebaseUid)) {
                                processedUids.add(firebaseUid)
                                migrateUserData(oldPrefs, firebaseUid)
                            }
                        }
                        key.startsWith("user_") && key.endsWith("_discord_id") -> {
                            val firebaseUid = key.removePrefix("user_").removeSuffix("_discord_id")
                            if (!processedUids.contains(firebaseUid)) {
                                processedUids.add(firebaseUid)
                                migrateUserData(oldPrefs, firebaseUid)
                            }
                        }
                    }
                }

                // Clear old data after successful migration
                oldPrefs.edit().clear().apply()
                Log.d(tag, "Migration completed. Cleared old Discord data.")
            } catch (e: Exception) {
                Log.e(tag, "Error during Discord data migration", e)
            }
        }

        private fun migrateUserData(
            oldPrefs: android.content.SharedPreferences,
            firebaseUid: String,
        ) {
            try {
                // Extract old data
                val discordId = oldPrefs.getString("link_$firebaseUid", null)
                val linkedAt = oldPrefs.getLong("link_${firebaseUid}_linked_at", System.currentTimeMillis())
                val username = oldPrefs.getString("user_${firebaseUid}_username", null)
                val displayName = oldPrefs.getString("user_${firebaseUid}_display_name", null)
                val avatarUrl = oldPrefs.getString("user_${firebaseUid}_avatar_url", null)
                val updatedAt = oldPrefs.getLong("user_${firebaseUid}_updated_at", System.currentTimeMillis())

                if (!discordId.isNullOrEmpty()) {
                    // Save to new user-specific storage
                    val newPrefs = getUserPrefs(firebaseUid)
                    newPrefs
                        .edit()
                        .putString("discord_id", discordId)
                        .putLong("linked_at", linkedAt)
                        .putString("username", username)
                        .putString("display_name", displayName)
                        .putString("avatar_url", avatarUrl)
                        .putLong("updated_at", updatedAt)
                        .apply()

                    Log.d(tag, "Migrated Discord data for user: $firebaseUid")
                }
            } catch (e: Exception) {
                Log.e(tag, "Error migrating data for user $firebaseUid", e)
            }
        }
    }
}
