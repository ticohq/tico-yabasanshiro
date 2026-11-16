package org.uoyabause.android.auth.ui

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.View
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.firebase.ui.auth.IdpResponse
import com.firebase.ui.auth.data.model.User
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.auth.GoogleAuthProvider
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await
import org.devmiyax.yabasanshiro.R
import org.devmiyax.yabasanshiro.databinding.ActivityPcSignInBinding
import org.uoyabause.android.DeviceCapabilities
import org.uoyabause.android.auth.GoogleDeviceAuth

/**
 * Sign-in screen for Google Play Games on PC.
 *
 * FirebaseUI's Google provider and Credential Manager both fail there because
 * Google Play services cannot get OAuth tokens for the device account, and
 * FirebaseUI's method picker cannot be left on a device without a back
 * button. This screen offers Google sign-in through the device authorization
 * grant ([GoogleDeviceAuth]).
 *
 * The result follows the FirebaseUI contract closely enough for the existing
 * callers: RESULT_OK with an IdpResponse in the data on success. Callers decide
 * success by FirebaseAuth.currentUser, as they already do for FirebaseUI.
 */
class PcSignInActivity : AppCompatActivity() {
    private lateinit var binding: ActivityPcSignInBinding
    private val deviceAuth = GoogleDeviceAuth()
    private var signInJob: Job? = null
    private var verificationUrl: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityPcSignInBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setResult(Activity.RESULT_CANCELED)
        binding.toolbar.setNavigationOnClickListener { finish() }
        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    finish()
                }
            },
        )

        binding.btnGetCode.setOnClickListener { startGoogleSignIn() }
        binding.btnOpenBrowser.setOnClickListener { openVerificationPage() }

        if (!deviceAuth.isConfigured) {
            binding.btnGetCode.isEnabled = false
            showError(getString(R.string.pc_sign_in_not_configured))
        }

        setupGamepad()
        binding.btnGetCode.post { binding.btnGetCode.requestFocus() }
    }

    // Play Games on PC has no on-screen back button; Esc is the expected way
    // out of a screen there.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_ESCAPE) {
            if (event.action == KeyEvent.ACTION_UP) onBackPressedDispatcher.onBackPressed()
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    private fun startGoogleSignIn() {
        signInJob?.cancel()
        binding.errorText.visibility = View.GONE
        binding.btnGetCode.isEnabled = false
        signInJob =
            lifecycleScope.launch {
                try {
                    val code = deviceAuth.requestDeviceCode()
                    verificationUrl = code.verificationUrl
                    showCode(code)
                    val idToken = deviceAuth.awaitIdToken(code)
                    binding.statusText.setText(R.string.pc_sign_in_signing_in)
                    val result =
                        FirebaseAuth
                            .getInstance()
                            .signInWithCredential(GoogleAuthProvider.getCredential(idToken, null))
                            .await()
                    val user = result.user
                    val data = buildIdpResponse(user?.email, user?.displayName, idToken)
                    setResult(Activity.RESULT_OK, data)
                    finish()
                } catch (e: CancellationException) {
                    throw e
                } catch (e: GoogleDeviceAuth.Failure.Denied) {
                    onGoogleFailed(getString(R.string.pc_sign_in_denied))
                } catch (e: GoogleDeviceAuth.Failure.Expired) {
                    onGoogleFailed(getString(R.string.pc_sign_in_expired))
                } catch (e: Exception) {
                    Log.e(TAG, "Device sign-in failed", e)
                    onGoogleFailed(getString(R.string.pc_sign_in_failed, e.localizedMessage ?: e.javaClass.simpleName))
                }
            }
    }

    private fun showCode(code: GoogleDeviceAuth.DeviceCode) {
        binding.verificationUrl.text = code.verificationUrl
        binding.userCode.text = code.userCode
        binding.statusText.setText(R.string.pc_sign_in_waiting)
        binding.waitingProgress.visibility = View.VISIBLE
        binding.codeSection.visibility = View.VISIBLE
        binding.btnGetCode.visibility = View.GONE
        binding.btnOpenBrowser.post { binding.btnOpenBrowser.requestFocus() }
        binding.root.announceForAccessibility(code.userCode)
    }

    private fun onGoogleFailed(message: String) {
        binding.codeSection.visibility = View.GONE
        binding.btnGetCode.visibility = View.VISIBLE
        binding.btnGetCode.isEnabled = true
        binding.btnGetCode.setText(R.string.pc_sign_in_new_code)
        binding.btnGetCode.post { binding.btnGetCode.requestFocus() }
        showError(message)
    }

    private fun showError(message: String) {
        binding.errorText.text = message
        binding.errorText.visibility = View.VISIBLE
        binding.root.announceForAccessibility(message)
    }

    private fun openVerificationPage() {
        // Play Games on PC opens browser intents in the host's default browser.
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(verificationUrl)))
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "No browser for $verificationUrl")
        }
    }

    private fun buildIdpResponse(
        email: String?,
        name: String?,
        idToken: String,
    ): Intent =
        IdpResponse
            .Builder(
                User
                    .Builder(GoogleAuthProvider.PROVIDER_ID, email)
                    .setName(name)
                    .build(),
            ).setToken(idToken)
            .build()
            .toIntent()

    private fun setupGamepad() {
        val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val views = listOf(binding.btnGetCode, binding.btnOpenBrowser)
        views.forEach { view ->
            view.setOnKeyListener { v, keyCode, event ->
                if (event.action == KeyEvent.ACTION_DOWN &&
                    (
                        keyCode == KeyEvent.KEYCODE_BUTTON_A ||
                            keyCode == KeyEvent.KEYCODE_ENTER ||
                            keyCode == KeyEvent.KEYCODE_DPAD_CENTER
                    )
                ) {
                    v.performClick()
                    true
                } else {
                    false
                }
            }
            view.setOnFocusChangeListener { v, hasFocus ->
                val scale = if (hasFocus) 1.05f else 1.0f
                v
                    .animate()
                    .scaleX(scale)
                    .scaleY(scale)
                    .alpha(if (hasFocus) 1.0f else 0.9f)
                    .setDuration(150)
                    .start()
                if (hasFocus) {
                    audioManager.playSoundEffect(AudioManager.FX_FOCUS_NAVIGATION_UP)
                    binding.scrollView.post {
                        binding.scrollView.smoothScrollTo(0, v.top - binding.scrollView.height / 3)
                    }
                }
            }
        }
    }

    companion object {
        private const val TAG = "PcSignInActivity"

        /**
         * Returns [authUiIntent] unchanged, or on Google Play Games on PC the
         * intent for this screen. Use it wherever a FirebaseUI sign-in intent
         * is launched.
         */
        fun wrap(
            context: Context,
            authUiIntent: Intent,
        ): Intent =
            if (DeviceCapabilities.isPc(context)) {
                Intent(context, PcSignInActivity::class.java)
            } else {
                authUiIntent
            }
    }
}
