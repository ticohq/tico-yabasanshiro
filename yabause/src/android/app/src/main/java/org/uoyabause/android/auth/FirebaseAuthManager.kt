package org.uoyabause.android.auth

import android.app.Activity
import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.fragment.app.FragmentActivity
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.FirebaseAuthUIActivityResultContract
import com.firebase.ui.auth.data.model.FirebaseAuthUIAuthenticationResult
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.auth.FirebaseUser
import org.uoyabause.android.auth.ui.PcSignInActivity

/**
 * Firebase Authentication Manager
 * Handles Firebase authentication and Discord account linking
 */
class FirebaseAuthManager(
    private val context: Context,
) {
    companion object {
        private const val TAG = "FirebaseAuthManager"
        private const val PREFS_NAME = "auth_prefs"
        private const val KEY_DISCORD_LINK_PROMPTED = "discord_link_prompted"
    }

    private val auth = FirebaseAuth.getInstance()
    private val discordAuthManager = DiscordAuthManager(context)

    /**
     * Get the current Firebase user
     * @return The current Firebase user or null if not signed in
     */
    fun getCurrentUser(): FirebaseUser? = auth.currentUser

    /**
     * Check if a user is currently signed in
     * @return true if a user is signed in, false otherwise
     */
    fun isUserLoggedIn(): Boolean = auth.currentUser != null

    /**
     * Check if the user has been prompted to link Discord
     * @return true if the user has been prompted, false otherwise
     */
    fun hasDiscordLinkPrompted(): Boolean {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return prefs.getBoolean(KEY_DISCORD_LINK_PROMPTED, false)
    }

    /**
     * Mark that the user has been prompted to link Discord
     */
    fun setDiscordLinkPrompted() {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit().putBoolean(KEY_DISCORD_LINK_PROMPTED, true).apply()
    }

    /**
     * Show the Discord link prompt dialog
     * @param activity The activity to show the dialog in
     */
    fun showDiscordLinkPrompt(activity: FragmentActivity) {
        if (activity is DiscordLinkPromptDialog.DiscordLinkPromptListener) {
            DiscordLinkPromptDialog.show(activity)
        } else {
            Log.e(TAG, "Activity must implement DiscordLinkPromptListener")
        }
    }

    /**
     * Create a sign-in contract for Firebase Auth UI
     * @param activity The activity to register the contract with
     * @param onResult Callback for the authentication result
     */
    fun createSignInContract(
        activity: FragmentActivity,
        onResult: (FirebaseAuthUIAuthenticationResult) -> Unit,
    ) {
        val signInLauncher =
            activity.registerForActivityResult(
                FirebaseAuthUIActivityResultContract(),
            ) { result ->
                // Check if sign-in was successful and prompt for Discord linking if needed
                if (result.resultCode == Activity.RESULT_OK && !hasDiscordLinkPrompted()) {
                    if (activity is DiscordLinkPromptDialog.DiscordLinkPromptListener) {
                        showDiscordLinkPrompt(activity)
                        setDiscordLinkPrompted()
                    }
                }
                onResult(result)
            }

        // Configure authentication providers
        val providers =
            arrayListOf(
                AuthUI.IdpConfig.EmailBuilder().build(),
                AuthUI.IdpConfig.GoogleBuilder().build(),
            )

        // Create and launch the sign-in intent
        val signInIntent =
            AuthUI
                .getInstance()
                .createSignInIntentBuilder()
                .setAvailableProviders(providers)
                .setTosAndPrivacyPolicyUrls(
                    "https://www.yabasanshiro.com/terms-of-use",
                    "https://www.yabasanshiro.com/privacy",
                ).build()

        signInLauncher.launch(PcSignInActivity.wrap(activity, signInIntent))
    }

    /**
     * Start the Discord account linking process
     * @param activity The activity context
     */
    fun startDiscordLinking(activity: Activity) {
        discordAuthManager.startDiscordLogin()
    }

    /**
     * Check if the current user has linked their Discord account
     * @return true if Discord is linked, false otherwise
     */
    suspend fun isDiscordLinked(): Boolean {
        val currentUser = auth.currentUser ?: return false
        return discordAuthManager.isDiscordLinked(currentUser.uid)
    }

    /**
     * Unlink the Discord account from the current user
     * @return true if unlink was successful, false otherwise
     */
    suspend fun unlinkDiscord(): Boolean {
        val currentUser = auth.currentUser ?: return false
        return discordAuthManager.unlinkDiscord(currentUser.uid)
    }

    /**
     * Handle the result of a Discord OAuth redirect
     * @param uri The redirect URI
     * @return true if handling was successful, false otherwise
     */
    suspend fun handleDiscordRedirect(uri: Uri): Boolean = discordAuthManager.handleRedirectAndSignIn(uri)

    /**
     * Sign out the current user
     * RetroAchievements login state is preserved (independent of Google account)
     * @param activity The activity context
     * @param onComplete Callback for when sign-out is complete
     */
    fun signOut(
        activity: Activity,
        onComplete: () -> Unit,
    ) {
        AuthUI
            .getInstance()
            .signOut(activity)
            .addOnCompleteListener {
                onComplete()
            }
    }
}
