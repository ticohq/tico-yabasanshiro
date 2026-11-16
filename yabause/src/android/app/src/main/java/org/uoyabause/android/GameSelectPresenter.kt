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
package org.uoyabause.android

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.FileUtils
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.util.Log
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.appcompat.app.AlertDialog
import androidx.core.app.ActivityCompat
import androidx.fragment.app.Fragment
import androidx.preference.PreferenceManager
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.AuthUI.IdpConfig.AppleBuilder
import com.firebase.ui.auth.AuthUI.IdpConfig.GoogleBuilder
import com.firebase.ui.auth.IdpResponse
import com.google.android.gms.analytics.HitBuilders
import com.google.android.gms.analytics.Tracker
import com.google.firebase.analytics.FirebaseAnalytics
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.crashlytics.FirebaseCrashlytics
import com.google.firebase.database.FirebaseDatabase
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.devmiyax.yabasanshiro.BuildConfig
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.YabauseStorage.Companion.storage
import org.uoyabause.android.auth.ui.PcSignInActivity
import java.io.File
import java.io.FileDescriptor
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.IOException
import java.nio.channels.FileChannel
import java.util.Arrays
import java.util.Calendar
import java.util.Locale
import androidx.appcompat.view.ContextThemeWrapper as ContextThemeWrapper1

@OptIn(DelicateCoroutinesApi::class)
class GameSelectPresenter(
    target: Fragment,
    private var yabauseActivityLauncher: ActivityResultLauncher<Intent>,
    listener: GameSelectPresenterListener,
) {
    private val mFirebaseAnalytics: FirebaseAnalytics
    private val tag = "GameSelectPresenter"
    private var tracker: Tracker? = null
    private val scope = CoroutineScope(Dispatchers.IO)

    interface GameSelectPresenterListener {
        fun onShowMessage(string_id: Int)

        fun onShowDialog(message: String)

        fun onUpdateDialogMessage(message: String)

        fun onDismissDialog()

        fun onLoadRows()

        fun onSignOut()
    }

    suspend fun updateGameDatabase(level: Int = 0, onProgress: (String) -> Unit) =
        withContext(Dispatchers.IO) {
            val ybs = storage
            ybs.setProgressCallback { message ->
                MainScope().launch { onProgress(message) }
            }
            ybs.generateGameDB(level)
            ybs.setProgressCallback(null)
        }

    var targetFragment: Fragment
    var presenterListener: GameSelectPresenterListener

    fun updateReferences(
        target: Fragment,
        launcher: ActivityResultLauncher<Intent>,
        listener: GameSelectPresenterListener,
    ) {
        targetFragment = target
        yabauseActivityLauncher = launcher
        presenterListener = listener
    }

    private var username: String? = null
    private var photoUrl: Uri? = null

    fun prepareStorage(): Boolean {
        val activity = targetFragment.activity ?: return false
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            // Verify that all required contact permissions have been granted.
            if (ActivityCompat.checkSelfPermission(
                    activity,
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                )
                != PackageManager.PERMISSION_GRANTED ||
                ActivityCompat.checkSelfPermission(
                    activity,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE,
                )
                != PackageManager.PERMISSION_GRANTED
            ) {
                return false
            }
        }
        val externalAndRemovableStorageM1 = activity.getExternalFilesDirs(null)
        if (externalAndRemovableStorageM1.size > 1 && externalAndRemovableStorageM1[1] != null) {
            // Use Android/media on SD card so games persist after app uninstall.
            // File migration from old locations is handled by StorageMigrationHelper (with user dialog).
            val sdCardDataPath = externalAndRemovableStorageM1[1].absolutePath
            val sdCardRoot = sdCardDataPath.substringBefore("/Android/data")
            val sdMediaGames = "$sdCardRoot/Android/media/${activity.packageName}/games"
            val ys = storage
            ys.setExternalStoragePath(sdMediaGames)
        } else {
            val sharedPrefwrite = PreferenceManager.getDefaultSharedPreferences(activity)
            val editor = sharedPrefwrite.edit()
            editor.putString("pref_game_download_directory", "0")
            editor.apply()
        }
        return true
    }

    fun signIn(launcher: ActivityResultLauncher<Intent>) {
        val intent =
            AuthUI
                .getInstance()
                .createSignInIntentBuilder()
                .setAvailableProviders(
                    Arrays.asList(
                        GoogleBuilder().build(),
                        AppleBuilder().build(),
                    ),
                ).build()
        launcher.launch(PcSignInActivity.wrap(targetFragment.requireContext(), intent))
    }

    fun signOut() {
        AuthUI
            .getInstance()
            .signOut(targetFragment.requireActivity())
            .addOnCompleteListener {
                this.presenterListener.onSignOut()
                // user is now signed out
            }
    }

    fun onSignIn(
        resultCode: Int,
        data: Intent?,
    ) {
        val response = IdpResponse.fromResultIntent(data)
        val auth = FirebaseAuth.getInstance()
        val currentUser = auth.currentUser

        // Check actual auth state instead of resultCode,
        // because Firebase AuthUI may return RESULT_CANCELED even on successful sign-in
        if (currentUser != null) {
            val bundle = Bundle()
            mFirebaseAnalytics.logEvent(FirebaseAnalytics.Event.LOGIN, bundle)

            val token = response?.idpToken

            val baseref = FirebaseDatabase.getInstance().reference
            val baseurl = "/user-posts/" + currentUser.uid

            // Set username based on available information
            username =
                when {
                    !currentUser.displayName.isNullOrEmpty() -> currentUser.displayName
                    !currentUser.email.isNullOrEmpty() -> currentUser.email!!.split("@").firstOrNull() ?: currentUser.email
                    else -> currentUser.uid
                }

            // Store the username in Firebase
            baseref.child(baseurl).child("name").setValue(username)

            if (currentUser.email != null) {
                baseref.child(baseurl).child("email").setValue(currentUser.email)
            }
            // if (currentUser.photoUrl != null) {
            //    baseref.child(baseurl).child("photo").setValue(auth.getCurrentUser().getPhotoUrl());
            //    photoUrl = auth.getCurrentUser().getPhotoUrl();
            // }
            baseref.child(baseurl).child("android_token").setValue(token)
            FirebaseCrashlytics.getInstance().setUserId(username + "_" + currentUser.email)
            mFirebaseAnalytics.setUserId(username + "_" + currentUser.email)
            mFirebaseAnalytics.setUserProperty("name", username + "_" + currentUser.email)
            val activity: Activity? = targetFragment.activity
            val prefs = activity!!.getSharedPreferences("private", Context.MODE_PRIVATE)
            var hasDonated = false
            if (prefs != null) {
                hasDonated = prefs.getBoolean("donated", false)
            }
            if (BuildConfig.BUILD_TYPE == "pro" || hasDonated) {
                baseref.child(baseurl).child("max_backup_count").setValue(256)
            } else {
                baseref.child(baseurl).child("max_backup_count").setValue(0)
            }

            // startActivity(SignedInActivity.createIntent(this, response));
            // val application = targetFragment.activity!!.application as YabauseApplication
            FirebaseCrashlytics.getInstance().setUserId(username + "_" + currentUser.email)

            return
        } else {
            username = null
            photoUrl = null

            // Sign in failed
            if (response == null) {
                // User pressed back button
                presenterListener.onShowMessage(org.devmiyax.yabasanshiro.R.string.sign_in_cancelled)
                return
            }
/*
            if (response.error!!.errorCode == MediaDrm.ErrorCodes.NO_NETWORK) {
                presenterListener.onShowMessage(org.devmiyax.yabasanshiro.R.string.no_internet_connection)
                return
            }
            if (response.error!!.errorCode == MediaDrm.ErrorCodes.UNKNOWN_ERROR) {
                presenterListener.onShowMessage(org.devmiyax.yabasanshiro.R.string.unknown_error)
                return
            }

 */
        }
        presenterListener.onShowMessage(org.devmiyax.yabasanshiro.R.string.unknown_sign_in_response)
    }

    fun onPause() {
    }

    fun onResume() {
    }

    fun isGameLimitReached(): Boolean {
        if (YabauseApplication.isPro()) return false
        if (isOnSubscription) return false
        val gameCount = YabauseStorage.dao.getAll().size
        return gameCount >= BuildConfig.MAX_FREE_GAMES
    }

    fun onSelectFile(uri: Uri) {
        if (isGameLimitReached()) {
            YabauseApplication.checkDonated(targetFragment.requireActivity())
            return
        }

        Log.i(tag, "Uri: $uri")

        val cursor: Cursor? =
            targetFragment.requireActivity().contentResolver.query(
                uri,
                null,
                null,
                null,
                null,
            )

        cursor!!.moveToFirst()
        val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        val path = cursor.getString(nameIndex)
        if (path.lowercase(Locale.ROOT).endsWith("chd")) {
            val index = cursor.getColumnIndex(OpenableColumns.SIZE)

            var size: Long = cursor.getLong(index)
            cursor.close()

            size = size / 1024 / 1024

            val message =
                targetFragment.getString(R.string.install_game_message) + " " + size + targetFragment.getString(R.string.install_game_message_after)

            AlertDialog
                .Builder(
                    ContextThemeWrapper1(
                        targetFragment.activity,
                        R.style.Theme_AppCompat,
                    ),
                ).setTitle(targetFragment.getString(R.string.do_you_want_to_install))
                .setMessage(message)
                .setPositiveButton(R.string.yes) { _, _ ->
                    selectStorage {
                        installGameFile(uri)
                    }
                }.setNegativeButton(R.string.no) { _, _ ->
                    openGameFileDirect(uri)
                }.setCancelable(true)
                .show()
        } else if (path.lowercase(Locale.getDefault()).endsWith("zip") /*|| path.lowercase(Locale.getDefault()).endsWith("7z")*/) {
            selectStorage {
                installZipGameFile(uri, path)
            }
        } else {
            Toast
                .makeText(
                    targetFragment.requireContext(),
                    targetFragment.getString(R.string.only_chd_is_supported_for_load_game),
                    Toast.LENGTH_LONG,
                ).show()
        }
        return
    }

    fun selectStorage(onOk: () -> Unit) {
        if (storage.hasExternalSD()) {
            val ctx = YabauseApplication.appContext
            val sharedPref = PreferenceManager.getDefaultSharedPreferences(ctx)
            val path = sharedPref.getString("pref_install_location", "0")
            var selectItem = path?.toInt() ?: 0

            var option: Array<String> = arrayOf()
            option += "Internal" + " (" + storage.getAvailableInternalMemorySize() + " free)"
            option += "External" + " (" + storage.getAvailableExternalMemorySize() + " free)"

            AlertDialog
                .Builder(targetFragment.requireActivity())
                .setTitle(targetFragment.getString(R.string.which_storage))
                .setSingleChoiceItems(
                    option,
                    selectItem,
                ) { _, which ->
                    selectItem = which
                }.setPositiveButton(R.string.ok) { _, _ ->

                    val editor = sharedPref.edit()
                    editor.putString("pref_install_location", selectItem.toString())
                    editor.apply()
                    onOk()
                }.setNegativeButton(R.string.cancel) { _, _ -> }
                .setCancelable(true)
                .show()
        } else {
            onOk()
        }
    }

    @Suppress("BlockingMethodInNonBlockingContext")
    private fun openGameFileDirect(uri: Uri) {
        scope.launch {
            withContext(Dispatchers.Main) {
                presenterListener.onShowDialog("Opening ...")
            }

            var parcelFileDescriptor: ParcelFileDescriptor? = null
            val uriString = uri.toString().lowercase(Locale.ROOT)
            var apath = ""
            try {
                parcelFileDescriptor =
                    targetFragment.requireActivity().contentResolver.openFileDescriptor(uri, "r")
                if (parcelFileDescriptor != null) {
                    val fd: Int = parcelFileDescriptor.fd
                    apath = "/proc/self/fd/$fd"
                }
            } catch (fne: FileNotFoundException) {
                apath = ""
            }
            if (apath == "") {
                Toast
                    .makeText(targetFragment.requireContext(), "Fail to open $uriString", Toast.LENGTH_LONG)
                    .show()
                parcelFileDescriptor?.close()
                withContext(Dispatchers.Main) {
                    presenterListener.onDismissDialog()
                }
                return@launch
            }

            val gameinfo = GameInfo.genGameInfoFromCHD(apath)
            if (gameinfo != null) {
                val bundle = Bundle()
                bundle.putString(FirebaseAnalytics.Param.ITEM_ID, gameinfo.product_number)
                bundle.putString(FirebaseAnalytics.Param.ITEM_NAME, gameinfo.game_title)
                mFirebaseAnalytics.logEvent(
                    "yab_start_game",
                    bundle,
                )
                parcelFileDescriptor!!.close()
                val sharedPref = PreferenceManager.getDefaultSharedPreferences(targetFragment.requireActivity())
                sharedPref.edit().putString("last_play_Game", gameinfo.game_title).commit()
                val intent = Intent(targetFragment.requireActivity(), Yabause::class.java)
                intent.putExtra("org.uoyabause.android.FileNameUri", uri.toString())
                intent.putExtra("org.uoyabause.android.gamecode", gameinfo.product_number)
                yabauseActivityLauncher.launch(intent)
            } else {
                Toast.makeText(targetFragment.requireContext(), "Fail to open $apath", Toast.LENGTH_LONG).show()
                parcelFileDescriptor?.close()
            }
            withContext(Dispatchers.Main) {
                presenterListener.onDismissDialog()
            }
        }
    }

    @Throws(IOException::class)
    fun copyFileO(
        sourceFile: FileInputStream,
        destFile: FileOutputStream,
    ) {
        var source: FileChannel? = null
        var destination: FileChannel? = null
        try {
            source = sourceFile.channel
            destination = destFile.channel
            destination.transferFrom(source, 0, source.size())
        } finally {
            source?.close()
            destination?.close()
        }
    }

    fun installZipGameFile(
        uri: Uri,
        path: String,
    ) {
        GlobalScope.launch(Dispatchers.IO) {
            withContext(Dispatchers.Main) {
                presenterListener.onShowDialog("Installing ...")
            }

            var zipFileName = ""
            try {
                val f = File(path)
                zipFileName = storage.getInstallDir().absolutePath + "/" + f.name
                val fd = File(zipFileName)
                val parcelFileDescriptor =
                    targetFragment.requireActivity().contentResolver.openFileDescriptor(uri, "r")
                val fileDescriptor: FileDescriptor = parcelFileDescriptor!!.fileDescriptor
                FileInputStream(fileDescriptor).use { inputStream ->
                    FileOutputStream(fd).use { outputStream ->
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                            FileUtils.copy(inputStream, outputStream)
                        } else {
                            copyFileO(inputStream, outputStream)
                        }
                    }
                }
                parcelFileDescriptor.close()

                withContext(Dispatchers.Main) {
                    presenterListener.onUpdateDialogMessage("Extracting ${fd.name}")
                }

                val installDir = storage.getInstallDir()
                var lastPercent = -1
                val targetFileName = GameArchiveExtractor.extract(installDir, fd) { _, done, total ->
                    if (total > 0) {
                        val percent = (done.coerceAtMost(total) * 100 / total).toInt()
                        if (percent != lastPercent) {
                            lastPercent = percent
                            launch(Dispatchers.Main) {
                                presenterListener.onUpdateDialogMessage("Extracting ${fd.name} $percent%")
                            }
                        }
                    }
                }

                if (targetFileName.isNotEmpty()) {
                    withContext(Dispatchers.Main) {
                        fileSelected(File(targetFileName))
                    }
                } else {
                    withContext(Dispatchers.Main) {
                        Toast
                            .makeText(
                                targetFragment.requireContext(),
                                "ISO image is not found!!",
                                Toast.LENGTH_LONG,
                            ).show()
                    }
                    Log.e(tag, "ISO image is not found!!")
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    Toast
                        .makeText(
                            targetFragment.requireContext(),
                            "Fail to copy ${e.localizedMessage}",
                            Toast.LENGTH_LONG,
                        ).show()
                }
                Log.e(tag, "Fail to copy ${e.localizedMessage}")
            } finally {
                val fd = File(zipFileName)
                if (fd.isFile && fd.exists()) {
                    fd.delete()
                }

                withContext(Dispatchers.Main) {
                    presenterListener.onDismissDialog()
                }
            }
        }
    }

    @Suppress("BlockingMethodInNonBlockingContext")
    fun installGameFile(uri: Uri) {
        scope.launch {
            withContext(Dispatchers.Main) {
                presenterListener.onShowDialog("Installing ...")
            }
            try {
                val parcelFileDescriptor1: ParcelFileDescriptor?
                val uriString = uri.toString().lowercase(Locale.ROOT)
                var apath = ""
                try {
                    parcelFileDescriptor1 =
                        targetFragment.requireActivity().contentResolver.openFileDescriptor(uri, "r")
                    if (parcelFileDescriptor1 != null) {
                        val fd: Int = parcelFileDescriptor1.fd
                        apath = "/proc/self/fd/$fd"
                    }
                } catch (e: Exception) {
                    Toast
                        .makeText(
                            targetFragment.requireContext(),
                            "Fail to open $uriString with ${e.localizedMessage}",
                            Toast.LENGTH_LONG,
                        ).show()
                    withContext(Dispatchers.Main) {
                        presenterListener.onDismissDialog()
                    }
                    return@launch
                }

                if (apath == "") {
                    Toast
                        .makeText(targetFragment.requireContext(), "Fail to open $uriString", Toast.LENGTH_LONG)
                        .show()
                    withContext(Dispatchers.Main) {
                        presenterListener.onDismissDialog()
                    }
                    return@launch
                }

                val gameinfo = GameInfo.genGameInfoFromCHD(apath)
                if (gameinfo != null) {
                    val bundle = Bundle()
                    bundle.putString(FirebaseAnalytics.Param.ITEM_ID, gameinfo.product_number)
                    bundle.putString(FirebaseAnalytics.Param.ITEM_NAME, gameinfo.game_title)
                    mFirebaseAnalytics.logEvent(
                        "yab_start_game",
                        bundle,
                    )

                    val sharedPref = PreferenceManager.getDefaultSharedPreferences(targetFragment.requireActivity())
                    sharedPref.edit().putString("last_play_Game", gameinfo.game_title).commit()

                    parcelFileDescriptor1!!.close()
                } else {
                    Toast
                        .makeText(targetFragment.requireContext(), "Fail to open $apath", Toast.LENGTH_LONG)
                        .show()
                    parcelFileDescriptor1?.close()
                    return@launch
                }

                withContext(Dispatchers.Main) {
                    presenterListener.onUpdateDialogMessage("Installing ${gameinfo.game_title}")
                }

                val fd =
                    File(storage.getInstallDir().absolutePath + "/" + gameinfo.product_number + ".chd")
                val parcelFileDescriptor =
                    targetFragment.requireActivity().contentResolver.openFileDescriptor(uri, "r")
                val fileDescriptor: FileDescriptor = parcelFileDescriptor!!.fileDescriptor
                FileInputStream(fileDescriptor).use { inputStream ->
                    FileOutputStream(fd).use { outputStream ->
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                            FileUtils.copy(inputStream, outputStream)
                        } else {
                            copyFileO(inputStream, outputStream)
                        }
                        inputStream.close()
                        outputStream.close()
                    }
                }
                withContext(Dispatchers.Main) {
                    fileSelected(fd)
                }
                parcelFileDescriptor.close()
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    Toast
                        .makeText(
                            targetFragment.requireContext(),
                            "Fail to copy " + e.localizedMessage,
                            Toast.LENGTH_LONG,
                        ).show()
                }
            } finally {
                withContext(Dispatchers.Main) {
                    presenterListener.onDismissDialog()
                }
            }
        }
        return
    }

    fun startGame(
        item: GameInfo,
        launcher: ActivityResultLauncher<Intent>,
    ) {
        // Last line of defence for the free version's limit. Every route into
        // the emulator passes through here - the phone list, the TV list and
        // anything added later that forgets to ask - so a game that shows as
        // locked cannot be started by finding another way in.
        if (FreeGameSlots.isLockedNow(item)) {
            YabauseApplication.checkDonated(
                targetFragment.requireActivity(),
                targetFragment.getString(R.string.game_locked_pro_gate_message),
            )
            return
        }

        GlobalScope.launch(Dispatchers.IO) {
            val c = Calendar.getInstance()
            item.lastplay_date = c.time
            YabauseStorage.dao.update(item)
        }

        val application = targetFragment.requireActivity().application as YabauseApplication
        tracker = application.defaultTracker
        tracker?.send(
            HitBuilders
                .EventBuilder()
                .setCategory("Action")
                .setAction(item.game_title)
                .build(),
        )
        val bundle = Bundle()
        bundle.putString(FirebaseAnalytics.Param.ITEM_ID, item.product_number)
        bundle.putString(FirebaseAnalytics.Param.ITEM_NAME, item.game_title)
        mFirebaseAnalytics.logEvent(
            "yab_start_game",
            bundle,
        )

        val sharedPref = PreferenceManager.getDefaultSharedPreferences(targetFragment.requireActivity())
        sharedPref.edit().putString("last_play_Game", item.game_title).commit()

        if (item.file_path.contains("content://") == true) {
            val intent = Intent(targetFragment.activity, Yabause::class.java)
            intent.putExtra("org.uoyabause.android.FileNameUri", item.file_path)
            intent.putExtra("org.uoyabause.android.FileDir", item.iso_file_path)
            intent.putExtra("org.uoyabause.android.gamecode", item.product_number)
            launcher.launch(intent)
        } else {
            val intent = Intent(targetFragment.activity, Yabause::class.java)
            intent.putExtra("org.uoyabause.android.FileNameEx", item.file_path)
            intent.putExtra("org.uoyabause.android.gamecode", item.product_number)
            launcher.launch(intent)
        }
    }

    val currentUserName: String?
        get() {
            val auth = FirebaseAuth.getInstance()
            return if (auth.currentUser != null) {
                when {
                    // First try to use display name if it exists and is not empty
                    !auth.currentUser!!.displayName.isNullOrEmpty() -> {
                        auth.currentUser!!.displayName
                    }
                    // Then try to use email if it exists
                    !auth.currentUser!!.email.isNullOrEmpty() -> {
                        // Use the part before @ in the email
                        auth.currentUser!!
                            .email!!
                            .split("@")
                            .firstOrNull() ?: auth.currentUser!!.email
                    }
                    // Finally fall back to UID
                    else -> {
                        auth.currentUser!!.uid
                    }
                }
            } else {
                null
            }
        }
    val currentUserPhoto: Uri?
        get() {
            val auth = FirebaseAuth.getInstance()
            return if (auth.currentUser != null) {
                auth.currentUser!!.photoUrl
            } else {
                null
            }
        }

    fun fileSelected(file: File) {
        val apath: String = file.absolutePath
        // save last selected dir
        val sharedPref =
            PreferenceManager.getDefaultSharedPreferences(targetFragment.requireActivity())
        val editor = sharedPref.edit()
        editor.putString("pref_last_dir", file.parent)
        editor.apply()

        GlobalScope.launch(Dispatchers.IO) {
            var gameinfo: GameInfo? = YabauseStorage.dao.findByFilePath(apath)
            if (gameinfo == null) {
                gameinfo =
                    when {
                        apath.endsWith("CUE", ignoreCase = true) -> GameInfo.genGameInfoFromCUE(apath)
                        apath.endsWith("MDS", ignoreCase = true) -> GameInfo.genGameInfoFromMDS(apath)
                        apath.endsWith("CCD", ignoreCase = true) -> GameInfo.genGameInfoFromCCD(apath)
                        apath.endsWith("CHD", ignoreCase = true) -> GameInfo.genGameInfoFromCHD(apath)
                        else -> GameInfo.genGameInfoFromIso(apath)
                    }

                if (gameinfo != null) {
                    gameinfo.updateState()
                    val c = Calendar.getInstance()
                    gameinfo.lastplay_date = c.time
                    YabauseStorage.dao.insertAll(gameinfo)
                }
            } else {
                val c = Calendar.getInstance()
                gameinfo.lastplay_date = c.time
                YabauseStorage.dao.update(gameinfo)
            }

            if (gameinfo != null) {
                withContext(Dispatchers.Main) {
                    presenterListener.onLoadRows()
                    val bundle = Bundle()
                    bundle.putString(FirebaseAnalytics.Param.ITEM_ID, gameinfo.product_number)
                    bundle.putString(FirebaseAnalytics.Param.ITEM_NAME, gameinfo.game_title)
                    mFirebaseAnalytics.logEvent(
                        "yab_start_game",
                        bundle,
                    )

                    val sharedPref =
                        PreferenceManager.getDefaultSharedPreferences(targetFragment.requireActivity())
                    sharedPref.edit().putString("last_play_Game", gameinfo.game_title).commit()

                    val intent = Intent(targetFragment.requireActivity(), Yabause::class.java)
                    intent.putExtra("org.uoyabause.android.FileNameEx", apath)
                    intent.putExtra("org.uoyabause.android.gamecode", gameinfo.product_number)
                    this@GameSelectPresenter.yabauseActivityLauncher.launch(intent)
                }
            } else {
                withContext(Dispatchers.Main) {
                    Toast
                        .makeText(
                            targetFragment.requireContext(),
                            "Failed to decrypt $apath",
                            Toast.LENGTH_LONG,
                        ).show()
                    Log.e(tag, "Failed to decrypt $apath")
                }
            }
        }
    }

    companion object {
//        const val RC_SIGN_IN = 123
        const val YABAUSE_ACTIVITY = 0x02
    }

    init {
        targetFragment = target
        presenterListener = listener
        mFirebaseAnalytics = FirebaseAnalytics.getInstance(targetFragment.requireActivity())
    }

    var isOnSubscription: Boolean = false
}
