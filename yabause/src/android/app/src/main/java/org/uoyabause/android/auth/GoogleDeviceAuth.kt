package org.uoyabause.android.auth

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import okhttp3.FormBody
import okhttp3.OkHttpClient
import okhttp3.Request
import org.devmiyax.yabasanshiro.BuildConfig
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * Google sign-in through the OAuth 2.0 device authorization grant
 * ("TVs and Limited Input devices" client).
 *
 * Used on Google Play Games on PC, where Google Play services cannot mint
 * OAuth tokens for the device account (every request ends in
 * NEED_REMOTE_CONSENT, and the consent screen reports an invalid request).
 * The user enters the code shown by the app at google.com/device in any
 * browser; the resulting Google ID token is exchanged for a Firebase
 * credential, so the Firebase UID matches the one from a phone sign-in.
 *
 * See docs/play_games_pc/2026-10-03-phase0-findings.md.
 */
class GoogleDeviceAuth(
    private val clientId: String = BuildConfig.GOOGLE_DEVICE_CLIENT_ID,
    private val clientSecret: String = BuildConfig.GOOGLE_DEVICE_CLIENT_SECRET,
    private val http: OkHttpClient =
        OkHttpClient
            .Builder()
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(15, TimeUnit.SECONDS)
            .build(),
) {
    data class DeviceCode(
        val deviceCode: String,
        val userCode: String,
        val verificationUrl: String,
        val expiresInSec: Int,
        val intervalSec: Int,
    )

    sealed class Failure(
        message: String,
    ) : Exception(message) {
        class Denied : Failure("access_denied")

        class Expired : Failure("expired_token")

        class Server(
            message: String,
        ) : Failure(message)
    }

    val isConfigured: Boolean get() = clientId.isNotEmpty() && clientSecret.isNotEmpty()

    suspend fun requestDeviceCode(): DeviceCode =
        withContext(Dispatchers.IO) {
            val body =
                FormBody
                    .Builder()
                    .add("client_id", clientId)
                    .add("scope", "openid email profile")
                    .build()
            val json = post(DEVICE_CODE_URL, body)
            if (json.has("error")) {
                throw Failure.Server(describe(json))
            }
            DeviceCode(
                deviceCode = json.getString("device_code"),
                userCode = json.getString("user_code"),
                verificationUrl = json.optString("verification_url", DEFAULT_VERIFICATION_URL),
                expiresInSec = json.optInt("expires_in", 1800),
                intervalSec = json.optInt("interval", 5),
            )
        }

    /**
     * Polls the token endpoint until the user approves, denies, or the code
     * expires. Returns the Google ID token. Cancel the calling coroutine to stop.
     */
    suspend fun awaitIdToken(code: DeviceCode): String {
        var intervalMs = code.intervalSec.coerceAtLeast(1) * 1000L
        val deadline = System.currentTimeMillis() + code.expiresInSec * 1000L
        while (System.currentTimeMillis() < deadline) {
            delay(intervalMs)
            val json =
                withContext(Dispatchers.IO) {
                    val body =
                        FormBody
                            .Builder()
                            .add("client_id", clientId)
                            .add("client_secret", clientSecret)
                            .add("device_code", code.deviceCode)
                            .add("grant_type", DEVICE_GRANT_TYPE)
                            .build()
                    post(TOKEN_URL, body)
                }
            when (json.optString("error")) {
                "" -> {
                    val idToken = json.optString("id_token")
                    if (idToken.isEmpty()) throw Failure.Server("no id_token in response")
                    return idToken
                }
                "authorization_pending" -> Unit
                "slow_down" -> intervalMs += 5000L
                "access_denied" -> throw Failure.Denied()
                "expired_token" -> throw Failure.Expired()
                else -> throw Failure.Server(describe(json))
            }
        }
        throw Failure.Expired()
    }

    private fun post(
        url: String,
        body: FormBody,
    ): JSONObject {
        val request =
            Request
                .Builder()
                .url(url)
                .post(body)
                .build()
        http.newCall(request).execute().use { response ->
            val text = response.body.string()
            return try {
                JSONObject(text)
            } catch (e: Exception) {
                throw IOException("HTTP ${response.code}: unexpected response")
            }
        }
    }

    private fun describe(json: JSONObject): String {
        val error = json.optString("error")
        val desc = json.optString("error_description")
        return if (desc.isEmpty()) error else "$error: $desc"
    }

    companion object {
        private const val DEVICE_CODE_URL = "https://oauth2.googleapis.com/device/code"
        private const val TOKEN_URL = "https://oauth2.googleapis.com/token"
        private const val DEVICE_GRANT_TYPE = "urn:ietf:params:oauth:grant-type:device_code"
        private const val DEFAULT_VERIFICATION_URL = "https://www.google.com/device"
    }
}
