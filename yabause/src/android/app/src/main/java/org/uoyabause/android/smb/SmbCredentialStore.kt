package org.uoyabause.android.smb

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

/**
 * Remembers the last SMB connection in EncryptedSharedPreferences. Every access
 * is guarded: the keystore can fail on some devices, and the feature must still
 * work without remembering anything.
 */
class SmbCredentialStore(
    context: Context,
) {
    private val appContext = context.applicationContext

    private fun prefs(): SharedPreferences? = try {
        val masterKey = MasterKey
            .Builder(appContext)
            .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
            .build()
        EncryptedSharedPreferences.create(
            appContext,
            FILE_NAME,
            masterKey,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
        )
    } catch (e: Exception) {
        Log.w(TAG, "Encrypted preferences unavailable: ${e.message}")
        null
    }

    /** Returns the saved connection, or null when nothing was remembered. */
    fun load(): SmbConnectionInfo? {
        val p = prefs() ?: return null
        if (!p.getBoolean(KEY_REMEMBER, false)) return null
        return SmbConnectionInfo(
            host = p.getString(KEY_HOST, "") ?: "",
            share = p.getString(KEY_SHARE, "") ?: "",
            username = p.getString(KEY_USER, "") ?: "",
            password = p.getString(KEY_PASS, "") ?: "",
            domain = p.getString(KEY_DOMAIN, "") ?: "",
        )
    }

    fun save(info: SmbConnectionInfo) {
        val p = prefs() ?: return
        p
            .edit()
            .putBoolean(KEY_REMEMBER, true)
            .putString(KEY_HOST, info.host)
            .putString(KEY_SHARE, info.share)
            .putString(KEY_USER, info.username)
            .putString(KEY_PASS, info.password)
            .putString(KEY_DOMAIN, info.domain)
            .apply()
    }

    fun clear() {
        prefs()?.edit()?.clear()?.apply()
    }

    companion object {
        private const val TAG = "SmbCredentialStore"
        private const val FILE_NAME = "smb_credentials"
        private const val KEY_REMEMBER = "remember"
        private const val KEY_HOST = "host"
        private const val KEY_SHARE = "share"
        private const val KEY_USER = "user"
        private const val KEY_PASS = "pass"
        private const val KEY_DOMAIN = "domain"
    }
}
