/*  Copyright 2011-2013 Guillaume Duhamel

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA

    ============================================================================

    Copyright 2019 devMiyax(smiyaxdev@gmail.com)

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

import android.animation.ValueAnimator
import android.app.Dialog
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.Process.killProcess
import android.os.Process.myPid
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import android.view.MenuItem
import android.view.MotionEvent
import android.view.View
import android.view.ViewTreeObserver
import android.view.WindowInsets
import android.view.WindowInsets.Type
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.animation.addListener
import androidx.core.content.ContextCompat
import androidx.core.view.GravityCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.documentfile.provider.DocumentFile
import androidx.drawerlayout.widget.DrawerLayout
import androidx.drawerlayout.widget.DrawerLayout.DrawerListener
import androidx.lifecycle.lifecycleScope
import androidx.preference.PreferenceManager
import androidx.transition.Fade
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.AuthUI.IdpConfig.AppleBuilder
import com.firebase.ui.auth.AuthUI.IdpConfig.GoogleBuilder
import com.frybits.harmony.getHarmonySharedPreferences
import com.google.android.gms.analytics.HitBuilders.ScreenViewBuilder
import com.google.android.gms.analytics.Tracker
import com.google.android.material.navigation.NavigationView
import com.google.android.material.snackbar.Snackbar
import com.google.firebase.analytics.FirebaseAnalytics
import com.google.firebase.auth.FirebaseAuth
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancelChildren
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.devmiyax.yabasanshiro.BuildConfig
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.BiosManager
import org.uoyabause.android.GameInfo
import org.uoyabause.android.PadManager.ShowMenuListener
import org.uoyabause.android.PadTestFragment.PadTestListener
import org.uoyabause.android.StateListFragment.Companion.checkMaxFileCount
import org.uoyabause.android.achievements.AchievementListFragment
import org.uoyabause.android.achievements.RetroAchievementsManager
import org.uoyabause.android.achievements.RetroAchievementsNotification
import org.uoyabause.android.auth.RetroAchievementsAuthManager
import org.uoyabause.android.auth.ui.PcSignInActivity
import org.uoyabause.android.cheat.TabCheatFragment
import org.uoyabause.android.game.BaseGame
import org.uoyabause.android.game.GameUiEvent
import org.uoyabause.android.game.SegaRally
import org.uoyabause.android.game.SonicR
import org.uoyabause.android.leaderboard.LeaderBoardFragment
import org.uoyabause.android.phone.DiscSwapGameListBottomSheet
import java.io.BufferedInputStream
import java.io.BufferedReader
import java.io.File
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.io.FileReader
import java.io.IOException
import java.net.URLDecoder
import java.util.ArrayList
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

internal enum class TrayState {
    OPEN,
    CLOSE,
}

class Yabause :
    AppCompatActivity(),
    NavigationView.OnNavigationItemSelectedListener,
    SelInputDeviceFragment.InputDeviceListener,
    PadTestListener,
    InputSettingListener,
    InputManager.InputDeviceListener,
    ShowMenuListener,
    GameUiEvent,
    DiscSwapGameListBottomSheet.Listener {
    // RetroAchievements integration
    private lateinit var retroAchievementsAuthManager: RetroAchievementsAuthManager
    private lateinit var retroAchievementsManager: RetroAchievementsManager
    private lateinit var retroAchievementsNotification: RetroAchievementsNotification

    // Firebase sign-in launcher for RetroAchievements
    private lateinit var firebaseSignInLauncher: ActivityResultLauncher<Intent>
    private var pendingRALoginCallback: (() -> Unit)? = null

    var biosPath: String? = null
        private set
    var gamePath: String? = null
        private set
    var cartridgeType = 0
        private set
    var videoInterface = 0
        private set

    var currentGame: BaseGame? = null

    private var waitingResult = false
    private var tracker: Tracker? = null
    private var trayState: TrayState = TrayState.CLOSE

    // private var adView: AdView? = null
    private var firebaseAnalytics: FirebaseAnalytics? = null
    private var inputManager: InputManager? = null
    private val returnCodeSignIn = 0x8010

    private var gameCode: String? = null
    private var testCase: String? = null
    private var tmpBackupFilePath: String? = null
    private var loadStateFilePath: String? = null

    // Non-zero when launched via BenchmarkRequest: see startBenchmarkPolling/finishBenchmark.
    private var benchmarkDisc = 0
    private var benchmarkSession: BenchmarkSession? = null

    // Guards setBenchmarkResult() so only the first of several possible exit paths
    // (completion, stall, readiness timeout, pause, exit menu) wins. Volatile because
    // the polling coroutine runs on Dispatchers.Default while lifecycle callbacks run
    // on the main thread.
    @Volatile
    private var benchmarkResultSet = false

    private lateinit var padManager: PadManager
    private var yabauseThread: YabauseRunnable? = null
    private var audio: YabauseAudio? = null
    private lateinit var drawerLayout: DrawerLayout
    private lateinit var progressBar: View
    private lateinit var progressMessage: TextView

    private val menuIdLeaderboard = 0x8123

    private var startTime: Long = 0L

    // Axis debug display
    private var axisDebugView: AxisDebugView? = null
    private val axisDebugHandler = Handler(Looper.getMainLooper())
    private val axisDebugRunnable =
        object : Runnable {
            override fun run() {
                if (axisDebugView?.visibility == View.VISIBLE) {
                    axisDebugView?.invalidate()
                    axisDebugHandler.postDelayed(this, 16) // ~60fps
                }
            }
        }

    fun showDialog() {
        progressMessage.text = "Sending..."
        progressBar.visibility = View.VISIBLE
        waitingResult = true
    }

    fun dismissDialog() {
        progressBar.visibility = View.GONE
        waitingResult = false
        toggleMenu()
    }

    public override fun onStop() {
        val sharedPref = PreferenceManager.getDefaultSharedPreferences(this)
        if (sharedPref.getBoolean("pref_auto_state_save", false)) {
        }
        super.onStop()
    }

    fun showAutoStateLoadDialog() {
        val gameCode = YabauseRunnable.getCurrentGameCode()
        if (gameCode == null) {
            return
        }
        val directory = File(YabauseStorage.storage.stateSavePath, gameCode)

        // ディレクトリ内の指定した拡張子を持つファイルリストを取得
        val files = directory.listFiles { _, name -> name.endsWith(".yss") }

        if (files != null) {
            // 最新のファイルを見つける
            val autoSaveFile = files.maxByOrNull { it.lastModified() }
            if (autoSaveFile != null) {
                val builder = AlertDialog.Builder(this)
                builder.setTitle(R.string.auto_state_save_data_found)
                builder.setMessage(R.string.auto_state_detail)

                val layoutInflater = layoutInflater
                val progressButton = layoutInflater.inflate(R.layout.pbutton, null, false)
                builder.setView(progressButton)

                // ダイアログを表示
                val dialog = builder.create()

                val dialogButton = progressButton.findViewById<Button>(R.id.progress_btn_back)
                dialogButton.setOnClickListener {
                    YabauseRunnable.loadstate(autoSaveFile.absolutePath)
                    dialog.dismiss()
                }

                val dialogButtonFront =
                    progressButton.findViewById<Button>(R.id.progress_btn_front)
                dialogButtonFront.setOnClickListener {
                    YabauseRunnable.loadstate(autoSaveFile.absolutePath)
                    dialog.dismiss()
                }

                // ダイアログが表示されたときにアニメーションを開始する
                dialog.setOnShowListener {
                    val observer = dialogButton.viewTreeObserver
                    observer.addOnGlobalLayoutListener(
                        object : ViewTreeObserver.OnGlobalLayoutListener {
                            override fun onGlobalLayout() {
                                var isCanceled = false
                                // Ensure we only call this once
                                dialogButton.viewTreeObserver.removeOnGlobalLayoutListener(this)
                                val valueAnimator = ValueAnimator.ofInt(0, dialogButton.width)
                                valueAnimator.addUpdateListener { animation ->
                                    val animatedValue = animation.animatedValue as Int
                                    dialogButtonFront.layoutParams.width = animatedValue
                                    dialogButtonFront.requestLayout()
                                }
                                valueAnimator.addListener(
                                    onEnd = {
                                        if (!isCanceled) {
                                            dialogButtonFront.callOnClick()
                                        }
                                    },
                                    onCancel = {
                                        // Handle cancellation
                                        dialogButtonFront.isEnabled = false
                                    },
                                )

                                val dialogCancelButton =
                                    progressButton.findViewById<Button>(R.id.progress_btn_cancel)
                                dialogCancelButton.setOnClickListener {
                                    isCanceled = true
                                    valueAnimator.cancel()
                                    dialog.dismiss()
                                }

                                dialogButtonFront.visibility = View.VISIBLE
                                valueAnimator.duration = 5000
                                valueAnimator.start()
                            }
                        },
                    )
                }

                dialog.show()
            }
        }
    }

    var mParcelFileDescriptor: ParcelFileDescriptor? = null
    var subFileDescripters = mutableListOf<ParcelFileDescriptor>()

    private val apiscope = CoroutineScope(Dispatchers.IO)

    override fun onUpdateAnalogDpad(a: Boolean) {
        val sharedPref = PreferenceManager.getDefaultSharedPreferences(this@Yabause)
        val analogSwitch = findViewById<View>(R.id.layer_pad_mode)
        if (a) {
            analogSwitch.visibility = View.VISIBLE
        } else {
            analogSwitch.visibility = View.GONE
        }
    }

    private fun showInitFailedDialog(message: String) {
        val builder = AlertDialog.Builder(this)
        builder.setTitle(getString(R.string.failed_to_initialize))
        builder.setMessage(message)
        Log.e(TAG, message)
        builder.setPositiveButton(R.string.ok) { dialog, which ->
            // OKを押したらActivityを終了
            finish()
        }
        builder.setOnCancelListener {
            // ダイアログがキャンセルされた場合もActivityを終了
            finish()
        }
        builder.show()
    }

    private fun showProVersionRequiredDialog() {
        val builder = AlertDialog.Builder(this)
        builder.setTitle(getString(R.string.pro_version_required_title))
        builder.setMessage(getString(R.string.pro_version_required_message))
        builder.setPositiveButton(R.string.go_to_store) { _, _ ->
            try {
                val intent = Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=org.devmiyax.yabasanshioro2.pro"))
                startActivity(intent)
            } catch (e: Exception) {
                val intent =
                    Intent(Intent.ACTION_VIEW, Uri.parse("https://play.google.com/store/apps/details?id=org.devmiyax.yabasanshioro2.pro"))
                startActivity(intent)
            }
            finish()
        }
        builder.setNegativeButton(R.string.cancel) { _, _ ->
            finish()
        }
        builder.setOnCancelListener {
            finish()
        }

        val dialog = builder.show()

        // PositiveButtonにフォーカスを当てる
        val positiveButton = dialog.getButton(AlertDialog.BUTTON_POSITIVE)
        positiveButton.requestFocus()

        // ボタンの元のテキストを保存
        val originalText = positiveButton.text

        // カウントダウン用のHandler
        val handler = Handler(Looper.getMainLooper())
        var countdown = 5

        // カウントダウン更新用のRunnable
        val countdownRunnable =
            object : Runnable {
                override fun run() {
                    if (dialog.isShowing) {
                        if (countdown > 0) {
                            positiveButton.text = "$originalText ($countdown)"
                            countdown--
                            handler.postDelayed(this, 1000)
                        } else {
                            // カウントダウン終了、自動タップ
                            positiveButton.performClick()
                        }
                    }
                }
            }

        // カウントダウン開始
        handler.post(countdownRunnable)

        // ダイアログが閉じられたらタイマーをキャンセル
        dialog.setOnDismissListener {
            handler.removeCallbacks(countdownRunnable)
        }
    }

    /**
     * Called when the activity is first created.
     */
    public override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Setup Firebase sign-in launcher for RetroAchievements
        setupFirebaseSignInLauncher()

        // Register back press callback for modern back gesture support
        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    handleBackPress()
                }
            },
        )

        startTime = System.currentTimeMillis() / 1000L

        val sharedPref = PreferenceManager.getDefaultSharedPreferences(this@Yabause)

        // T019: Check BIOS file existence on startup
        verifyBiosConfiguration(sharedPref)

        val lockLandscape = sharedPref.getBoolean("pref_landscape", false)
        requestedOrientation =
            if (lockLandscape == true) {
                ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
            } else {
                ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
            }
        inputManager = getSystemService(Context.INPUT_SERVICE) as InputManager
        System.gc()
        firebaseAnalytics = FirebaseAnalytics.getInstance(this)
        val application = application as YabauseApplication
        tracker = application.defaultTracker

        setContentView(R.layout.main)

        // Setup edge-to-edge window insets handling
        setupEdgeToEdgeInsets()

        progressBar = findViewById(R.id.llProgressBar)
        progressBar.visibility = View.GONE
        progressMessage = findViewById(R.id.pbText)

        // Setup axis debug display (hidden by default)
        axisDebugView = findViewById(R.id.axisDebugView)

        padManager = PadManager.padManager!!
        padManager.loadSettings()
        padManager.showMenuListener = this // setShowMenulistener(this)

        val analogSwitch = findViewById<SwitchCompat>(R.id.toggleAnalogButton)

        val hprefernce = getHarmonySharedPreferences("pref_analog_pad")

        analogSwitch.isChecked = hprefernce.getBoolean("pref_analog_pad", false)

        val padModeLayer = findViewById<View>(R.id.layer_pad_mode)
        padModeLayer?.alpha = sharedPref.getFloat("pref_pad_trans", 0.7f)
        if (sharedPref.getBoolean("pref_show_analog_switch", false)) {
            padModeLayer.visibility = View.VISIBLE
        } else {
            padModeLayer.visibility = View.GONE
        }

        analogSwitch.setOnCheckedChangeListener { _, isChecked ->
            val padv = findViewById<View>(R.id.yabause_pad) as YabausePad
            if (isChecked) {
                padManager.analogMode = PadManager.MODE_ANALOG
                YabauseRunnable.switch_padmode(PadManager.MODE_ANALOG)
                padv.setPadMode(PadManager.MODE_ANALOG)

                val hprefernce = getHarmonySharedPreferences("pref_analog_pad")
                val editor = hprefernce.edit()
                editor.putBoolean("pref_analog_pad", true)
                editor.apply()
            } else {
                // The switch isn't checked.
                YabauseRunnable.switch_padmode(PadManager.MODE_HAT)
                padManager.analogMode = PadManager.MODE_HAT
                padv.setPadMode(PadManager.MODE_HAT)

                val hprefernce = getHarmonySharedPreferences("pref_analog_pad")
                val editor = hprefernce.edit()
                editor.putBoolean("pref_analog_pad", false)
                editor.apply()
            }
        }
/*
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                window.setSustainedPerformanceMode(true)
            }
        } catch (e: Exception) {
            // Do Nothing
        }
 */
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER
        }
        if (sharedPref.getBoolean("pref_immersive_mode", true)) {
            enableImmersiveMode()
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        drawerLayout = findViewById<View>(R.id.drawer_layout) as DrawerLayout
        updateViewLayout(resources.configuration.orientation)
        var navigationView = findViewById<View>(R.id.nav_view) as NavigationView
        navigationView.setNavigationItemSelectedListener(this)
        val menu = navigationView.menu
        if (BuildConfig.BUILD_TYPE != "debug") {
            val rec = menu.findItem(R.id.record)
            if (rec != null) {
                rec.isVisible = false
            }
            val play = menu.findItem(R.id.play)
            if (play != null) {
                play.isVisible = false
            }
        }
        val drawerListener: DrawerListener =
            object : DrawerListener {
                override fun onDrawerSlide(
                    view: View,
                    v: Float,
                ) {
                    // Log.d(this.javaClass.name,"onDrawerSlide ${v}")
                }

                override fun onDrawerOpened(view: View) {
                    // Log.d(this.javaClass.name,"onDrawerOpened")
                }

                override fun onDrawerClosed(view: View) {
                    // Log.d(this.javaClass.name,"onDrawerClosed")
                    if (waitingResult == false && menuShowing == true) {
                        menuShowing = false
                        YabauseRunnable.resume()
                        audio?.unmute(YabauseAudio.SYSTEM)
                    }
                }

                override fun onDrawerStateChanged(i: Int) {
                    // Log.d(this.javaClass.name,"onDrawerStateChanged")
                }
            }
        drawerLayout.addDrawerListener(drawerListener)
        val intent = intent
        val bundle = intent.extras
        if (bundle != null) {
            for (key in bundle.keySet()) {
                val value =
                    bundle.getString(key) ?: bundle.getInt(key, Int.MIN_VALUE).let {
                        if (it != Int.MIN_VALUE) it.toString() else "NULL"
                    }
                Log.e(TAG, "$key : $value")
            }
        }
        val game = intent.getStringExtra("org.uoyabause.android.FileName")
        if (game != null && game.length > 0) {
            val storage = YabauseStorage.storage
            gamePath = storage.getGamePath(game)
        } else {
            gamePath = ""
        }
        val exgame = intent.getStringExtra("org.uoyabause.android.FileNameEx")
        if (exgame != null) {
            gamePath = exgame
        }

        var fileDesc = -1
        val uriString: String? = intent.getStringExtra("org.uoyabause.android.FileNameUri")
        if (uriString != null && callingActivity == null) {
            // External launch (shortcut etc.) - require Pro
            if (!YabauseApplication.isPro()) {
                Toast.makeText(this, getString(R.string.pro_feature_only), Toast.LENGTH_LONG).show()
                finish()
                return
            }
        }
        if (uriString != null) {
            val fnameIndex = uriString.lastIndexOf("%2F", ignoreCase = true)
            val fname = uriString.substring(fnameIndex + 3)
            val uri = Uri.parse(uriString)
            var apath = ""
            try {
                // First, try to find existing tree permission for this document URI
                val treeUri = UriPermissionHelper.findTreeUriForDocument(this, uri)

                mParcelFileDescriptor =
                    if (treeUri != null) {
                        // Use tree permission to access the file
                        Log.d(TAG, "Using tree permission to access: $uri")
                        UriPermissionHelper.openDocumentViaTree(this, uri, treeUri)
                    } else {
                        // Try direct access (may work if Intent has FLAG_GRANT_READ_URI_PERMISSION)
                        Log.d(TAG, "Trying direct access for: $uri")
                        contentResolver.openFileDescriptor(uri, "r")
                    }

                if (mParcelFileDescriptor != null) {
                    val fd: Int? = mParcelFileDescriptor?.getFd()
                    if (fd != null) {
                        apath = "/proc/self/fd/$fd;$fname"
                        fileDesc = fd
                    }
                } else {
                    Log.e(TAG, "Failed to open file descriptor for: $uri")
                    showInitFailedDialog(getString(R.string.fail_to_open_file_access_denied, uri.toString()))
                    return
                }
            } catch (e: Exception) {
                Log.e(TAG, "Failed to open file: $uri", e)
                showInitFailedDialog(getString(R.string.fail_to_open_with, uri, e.localizedMessage))
                return
            }

            if (apath == "") {
                showInitFailedDialog(getString(R.string.fail_to_open, uri.toString()))
                return
            }
            gamePath = apath
        } else {
        }

        val dirString: String? = intent.getStringExtra("org.uoyabause.android.FileDir")
        if (dirString != null) {
            currentDocumentUri = Uri.parse(dirString)
        } else {
            currentDocumentUri = null
        }

        Log.d(TAG, "File is " + gamePath)
        if (gamePath.isNullOrEmpty()) {
            showInitFailedDialog(getString(R.string.no_game_file_is_selected))
            return
        }

        if (fileDesc == -1) {
            val file = File(gamePath!!)
            try {
                val filereader = FileReader(file)
                val br = BufferedReader(filereader)
                var c: CharArray = CharArray(4)
                br.read(c, 0, 1)
                br.close()
            } catch (e: FileNotFoundException) {
                showInitFailedDialog(getString(R.string.file_not_found, e.message))
                return
            } catch (e: IOException) {
                showInitFailedDialog(getString(R.string.i_o_error_occurred, e.message))
                return
            } catch (e: SecurityException) {
                showInitFailedDialog(getString(R.string.read_permission_denied, e.message))
                return
            } catch (e: Exception) {
                showInitFailedDialog(getString(R.string.other_file_error, e.message))
                return
            }
        }

        gameCode = intent.getStringExtra("org.uoyabause.android.gamecode")
        benchmarkDisc = intent.getIntExtra(BenchmarkRequest.EXTRA_BENCHMARK_DISC, 0)
        if (gameCode == null) {
            val dao = YabauseStorage.dao

            var gameinfo: GameInfo? = null
            val uriString: String? = intent.getStringExtra("org.uoyabause.android.FileNameUri")
            if (uriString != null) {
                // First try exact match
                Log.d(TAG, "Looking for game with URI: $uriString")
                gameinfo = dao.findByFilePath(uriString)
                Log.d(TAG, "Exact match result: ${gameinfo != null}")

                // If not found, try with normalized URI (tree/document -> document)
                if (gameinfo == null) {
                    val uri = Uri.parse(uriString)
                    val simpleUri = UriPermissionHelper.toSimpleDocumentUri(uri)
                    Log.d(TAG, "Normalized URI: $simpleUri")
                    if (simpleUri.toString() != uriString) {
                        gameinfo = dao.findByFilePath(simpleUri.toString())
                        Log.d(TAG, "Normalized match result: ${gameinfo != null}")
                    }
                }

                // If still not found, try matching by document ID suffix
                // This handles cases where tree URIs differ but document IDs are the same
                if (gameinfo == null) {
                    val uri = Uri.parse(uriString)
                    val documentId = UriPermissionHelper.extractDocumentId(uri)
                    Log.d(TAG, "Extracted document ID: $documentId")
                    if (documentId != null) {
                        // URL-encode the document ID since DB stores encoded paths
                        val encodedDocumentId = Uri.encode(documentId)
                        val documentSuffix = "/document/$encodedDocumentId"
                        Log.d(TAG, "Trying document ID suffix match: $documentSuffix")
                        gameinfo = dao.findByDocumentIdSuffix(documentSuffix)
                        Log.d(TAG, "Suffix match result: ${gameinfo != null}")
                    } else {
                        Log.w(TAG, "Failed to extract document ID from URI")
                    }
                }

                if (gameinfo != null) {
                    gameCode = gameinfo.product_number
                    currentDocumentUri = Uri.parse(gameinfo.iso_file_path)
                } else {
                    gameCode = null
                    Log.w(TAG, "Game not found in DB for URI: $uriString")
                    showInitFailedDialog(getString(R.string.game_not_in_list))
                    return
                }
            }
        }

        testCase = intent.getStringExtra("TestCase")
        tmpBackupFilePath = intent.getStringExtra("org.uoyabause.android.tmpbackupfile")
        loadStateFilePath = intent.getStringExtra("org.uoyabause.android.LoadState")
        audio = YabauseAudio(this)
        currentGame = null
        if (gameCode != null) {
            setupGameContext(gameCode!!)
        }

        if (currentGame != null) {
            YabauseRunnable.enableBackupWriteHook()
        } else {
            navigationView.menu.removeItem(menuIdLeaderboard)
        }

        waitingResult = false
        yabauseThread = YabauseRunnable(this)
        if (yabauseThread?.inited == false) {
            showInitFailedDialog(getString(R.string.fail_to_initialize_emulator))
            return
        }

        // Load state from intent if specified (for report reproduction)
        if (loadStateFilePath != null) {
            val stateFile = File(loadStateFilePath!!)
            if (stateFile.exists()) {
                // Post delayed to ensure emulator is fully initialized
                drawerLayout.postDelayed({
                    YabauseRunnable.loadstate(loadStateFilePath!!)
                    Log.d(TAG, "Loaded state from: $loadStateFilePath")
                }, 1000) // 1 second delay
            } else {
                Log.e(TAG, "State file not found: $loadStateFilePath")
            }
        } else if (sharedPref.getBoolean("pref_auto_state_save", false)) {
            showAutoStateLoadDialog()
        }

        // Initialize RetroAchievements
        initializeRetroAchievements()

        // Set current activity for RetroAchievements notifications
        val retroAchievementsManager = RetroAchievementsManager.getInstance(this)
        retroAchievementsManager.setCurrentActivity(this)

        showKeyboardMapIfNeeded()

        // TEST: Show RetroAchievements notification popup

/*
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
            try {
                Log.d(TAG, "Testing RetroAchievements notification...")
                val retroAchievementsManager = RetroAchievementsManager.getInstance(this)

                // Test official achievement
                retroAchievementsManager.onAchievementUnlocked(
                    achievementId = 12345,
                    title = "First Steps",
                    description = "Complete the first level of the game",
                    points = 5,
                    badge = "https://media.retroachievements.org/Badge/12345.png",
                    isOfficial = true
                )

                // Test unofficial achievement (after 3 seconds)
                android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                    retroAchievementsManager.onAchievementUnlocked(
                        achievementId = 67890,
                        title = "Speed Runner",
                        description = "Complete any level in under 30 seconds",
                        points = 10,
                        badge = null, // Test fallback icon
                        isOfficial = false
                    )
                }, 3000)

                // Test leaderboard submission (after 6 seconds)
                android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                    retroAchievementsManager.onLeaderboardSubmit(
                        leaderboardId = 54321,
                        title = "Best Time",
                        description = "Fastest completion time for this level",
                        scoreString = "02:03.45"
                    )
                }, 6000)

            } catch (e: Exception) {
                Log.e(TAG, "Error testing RetroAchievements notification", e)
            }
        }, 2000) // Wait 2 seconds after onCreate completes

 */
    }

    @Suppress("DEPRECATION")
    fun updateViewLayout(orientation: Int) {
        // ステータスバー背景色とアイコン色を設定
        window.statusBarColor = ContextCompat.getColor(this, R.color.black)
        WindowCompat.getInsetsController(window, window.decorView)?.apply {
            isAppearanceLightStatusBars = false // ダークモード: 白いアイコン
        }
        val sharedPref = PreferenceManager.getDefaultSharedPreferences(this)
        var immersiveFlags = 0
        if (sharedPref.getBoolean("pref_immersive_mode", true)) {
            immersiveFlags =
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        }

        val decorView = window.decorView
        if (orientation == Configuration.ORIENTATION_LANDSCAPE) {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                window.insetsController?.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                if (sharedPref.getBoolean("pref_immersive_mode", true)) {
                    window.insetsController?.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                } else {
                    window.insetsController?.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                    window.insetsController?.show(WindowInsets.Type.navigationBars())
                }
            } else {
                decorView.systemUiVisibility = (
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        or immersiveFlags
                        or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        or View.SYSTEM_UI_FLAG_FULLSCREEN
                        or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                )
            }
        } else if (orientation == Configuration.ORIENTATION_PORTRAIT) {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                window.insetsController?.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                if (sharedPref.getBoolean("pref_immersive_mode", true)) {
                    window.insetsController?.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                } else {
                    window.insetsController?.show(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                }
            } else {
                decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_LAYOUT_STABLE or immersiveFlags
            }
        }

        // Re-apply window insets after view layout changes
        val contentMain = findViewById<FrameLayout>(R.id.content_main)
        ViewCompat.requestApplyInsets(contentMain)
    }

    override fun onConfigurationChanged(_newConfig: Configuration) {
        updateViewLayout(_newConfig.orientation)
        super.onConfigurationChanged(_newConfig)
    }

    @Suppress("DEPRECATION")
    override fun onNavigationItemSelected(item: MenuItem): Boolean {
        // Handle action bar item clicks here. The action bar will
        // automatically handle clicks on the Home/Up button, so long
        // as you specify a parent activity in AndroidManifest.xml.
        val id = item.itemId
        val bundle = Bundle()
        val title = item.title.toString()
        bundle.putString(FirebaseAnalytics.Param.ITEM_ID, "MENU")
        bundle.putString(FirebaseAnalytics.Param.ITEM_NAME, title)
        firebaseAnalytics!!.logEvent(
            FirebaseAnalytics.Event.SELECT_CONTENT,
            bundle,
        )
        when (id) {
/*
            R.id.leaderboard -> {

                if( currentGame != null ) {
                    Games.getLeaderboardsClient(this, GoogleSignIn.getLastSignedInAccount(this))
                        .getLeaderboardIntent(currentGame!!.leaderBoardId)
                        .addOnSuccessListener(OnSuccessListener<Intent?> { intent ->
                            startActivityForResult(intent,
                                3)
                        })
                }
            }

 */
            R.id.reset -> YabauseRunnable.reset()
            R.id.report -> startReport()
/*
            R.id.gametitle -> {
                val save_path = YabauseStorage.storage.screenshotPath
                val current_gamecode = YabauseRunnable.getCurrentGameCode()
                val screen_shot_save_path = "$save_path$current_gamecode.png"
                if (YabauseRunnable.screenshot(screen_shot_save_path) == 0) {
                    try {
                        val gi = Select().from(GameInfo::class.java)
                            .where("product_number = ?", current_gamecode).executeSingle<GameInfo>()
                        if (gi != null) {
                            gi.image_url = screen_shot_save_path
                            gi.save()
                        }
                    } catch (e: Exception) {
                        Log.e(TAG, e.localizedMessage!!)
                    }
                }
            }
*/
            R.id.save_state -> {
                val savePath = YabauseStorage.storage.stateSavePath
                val currentGamecode = YabauseRunnable.getCurrentGameCode()
                val saveRoot =
                    currentGamecode?.let { File(YabauseStorage.storage.stateSavePath, it) }
                if (saveRoot != null) {
                    if (!saveRoot.exists()) saveRoot.mkdir()
                }
                var saveFilename = YabauseRunnable.savestate(savePath + currentGamecode)
                if (saveFilename != null) {
                    val point = saveFilename.lastIndexOf(".")
                    if (point != -1) {
                        saveFilename = saveFilename.substring(0, point)
                    }
                    val screenShotSavePath = "$saveFilename.png"
                    if (YabauseRunnable.screenshot(screenShotSavePath) != 0) {
                        Snackbar
                            .make(
                                drawerLayout,
                                "Failed to save the current state",
                                Snackbar.LENGTH_SHORT,
                            ).show()
                    } else {
                        Snackbar
                            .make(
                                drawerLayout,
                                "Current state is saved as $saveFilename",
                                Snackbar.LENGTH_LONG,
                            ).show()
                    }
                } else {
                    Snackbar
                        .make(
                            drawerLayout,
                            "Failed to save the current state",
                            Snackbar.LENGTH_SHORT,
                        ).show()
                }
                checkMaxFileCount(savePath + currentGamecode)
            }
            R.id.load_state -> {
                // Check if hardcore mode is enabled - loading save states is not allowed in hardcore
                val retroAchievementsManager = RetroAchievementsManager.getInstance(this)
                if (retroAchievementsManager.isHardcoreEnabled()) {
                    Toast
                        .makeText(
                            this,
                            "Loading save states is not allowed in hardcore mode. Disable hardcore mode to use this feature.",
                            Toast.LENGTH_LONG,
                        ).show()
                    return true
                }

                // String save_path = YabauseStorage.getStorage().getStateSavePath();
                // YabauseRunnable.loadstate(save_path);
                val basepath: String
                val savePath = YabauseStorage.storage.stateSavePath
                val currentGamecode = YabauseRunnable.getCurrentGameCode()
                basepath = savePath + currentGamecode
                waitingResult = true
                val transaction = supportFragmentManager.beginTransaction()
                val fragment = StateListFragment()
                fragment.setBasePath(basepath)
                transaction.setCustomAnimations(R.anim.fade_in, R.anim.fade_out)
                transaction.replace(R.id.ext_fragment, fragment, StateListFragment.TAG)
                transaction.show(fragment)
                transaction.commit()
            }
            R.id.record -> {
                // Check if hardcore mode is enabled - input recording is not allowed in hardcore
                val retroAchievementsManager = RetroAchievementsManager.getInstance(this)
                if (retroAchievementsManager.isHardcoreEnabled()) {
                    Toast
                        .makeText(
                            this,
                            "Input recording is not allowed in hardcore mode. Disable hardcore mode to use this feature.",
                            Toast.LENGTH_LONG,
                        ).show()
                    return true
                }

                if (BuildConfig.BUILD_TYPE == "debug") {
                    YabauseRunnable.record(YabauseStorage.storage.recordPath)
                }
            }
            R.id.play -> {
                // Check if hardcore mode is enabled - input playback is not allowed in hardcore
                val retroAchievementsManager = RetroAchievementsManager.getInstance(this)
                if (retroAchievementsManager.isHardcoreEnabled()) {
                    Toast
                        .makeText(
                            this,
                            "Input playback is not allowed in hardcore mode. Disable hardcore mode to use this feature.",
                            Toast.LENGTH_LONG,
                        ).show()
                    return true
                }

                // Input playback is not implemented yet
            }
            R.id.menu_achievements -> {
                waitingResult = true
                if (retroAchievementsAuthManager.isRetroAchievementsLoggedIn()) {
                    showAchievementsFragment()
                } else {
                    showRetroAchievementsLoginDialog {
                        showAchievementsFragment()
                    }
                }
            }
            R.id.menu_leaderboard -> {
                val gameCode = YabauseRunnable.getCurrentGameCode()
                if (gameCode != null) {
                    waitingResult = true
                    if (retroAchievementsAuthManager.isRetroAchievementsLoggedIn()) {
                        showLeaderBoardFragment(gameCode)
                    } else {
                        showRetroAchievementsLoginDialog {
                            showLeaderBoardFragment(gameCode)
                        }
                    }
                }
            }
            R.id.menu_item_pad_device -> {
                waitingResult = true
                val newFragment = SelInputDeviceFragment()
                newFragment.target = SelInputDeviceFragment.PLAYER1
                newFragment.listener = this
                newFragment.show(supportFragmentManager, "InputDevice")
            }
            R.id.menu_item_pad_setting -> {
                waitingResult = true
                if (padManager.getPlayer1InputDevice() == -1) { // Using pad?
                    val transaction = supportFragmentManager.beginTransaction()
                    val fragment = PadTestFragment.newInstance()
                    fragment.listener = this
                    transaction.setCustomAnimations(R.anim.fade_in, R.anim.fade_out)
                    transaction.replace(R.id.ext_fragment, fragment, PadTestFragment.TAG)
                    transaction.show(fragment)
                    transaction.commit()
                } else {
                    val newFragment = InputSettingFragment()
                    newFragment.setPlayerAndFilename(SelInputDeviceFragment.PLAYER1, "keymap")
                    newFragment.setListener(this)
                    newFragment.show(supportFragmentManager, "InputSettings")
                }
            }
            R.id.menu_item_pad_device_p2 -> {
                waitingResult = true
                val newFragment = SelInputDeviceFragment()
                newFragment.target = SelInputDeviceFragment.PLAYER2
                newFragment.listener = this
                newFragment.show(supportFragmentManager, "InputDevice")
            }
            R.id.menu_item_pad_setting_p2 -> {
                waitingResult = true
                if (padManager.getPlayer2InputDevice() != -1) { // Using pad?
                    val newFragment = InputSettingFragment()
                    newFragment.setPlayerAndFilename(
                        SelInputDeviceFragment.PLAYER2,
                        "keymap_player2",
                    )
                    newFragment.setListener(this)
                    newFragment.show(supportFragmentManager, "InputSettings")
                }
            }
            R.id.button_open_cd -> {
                val prefs = PreferenceManager.getDefaultSharedPreferences(this)
                val isBuiltinBios = prefs.getString(
                    BiosManager.KEY_BIOS_TYPE,
                    BiosManager.BIOS_TYPE_BUILTIN,
                ) != BiosManager.BIOS_TYPE_FILE

                if (isBuiltinBios) {
                    // Built-in BIOS: show game list immediately (no open/close tray UI)
                    YabauseRunnable.openTray()
                    showDiscSwapSheet()
                } else {
                    // BIOS file: two-step open/close tray
                    if (trayState == TrayState.CLOSE) {
                        YabauseRunnable.openTray()
                        item.title = getString(R.string.close_cd_tray)
                        trayState = TrayState.OPEN
                    } else {
                        item.title = getString(R.string.open_cd_tray)
                        trayState = TrayState.CLOSE
                        showDiscSwapSheet()
                    }
                }
            }
            R.id.pad_mode -> {
                var mode: Boolean
                val padv = findViewById<View>(R.id.yabause_pad) as YabausePad
                var hprefernce = getHarmonySharedPreferences("pref_analog_pad")
                if (hprefernce.getBoolean("pref_analog_pad", false)) {
                    item.isChecked = false
                    padManager.analogMode = PadManager.MODE_HAT
                    YabauseRunnable.switch_padmode(PadManager.MODE_HAT)
                    padv.setPadMode(PadManager.MODE_HAT)
                    mode = false
                } else {
                    item.isChecked = true
                    padManager.analogMode = PadManager.MODE_ANALOG
                    YabauseRunnable.switch_padmode(PadManager.MODE_ANALOG)
                    padv.setPadMode(PadManager.MODE_ANALOG)
                    mode = true
                }
                val editor = hprefernce.edit()
                editor.putBoolean("pref_analog_pad", mode)
                editor.apply()
                toggleMenu()
            }
            R.id.pad_mode_p2 -> {
                var mode: Boolean
                var hprefernce = getHarmonySharedPreferences("pref_analog_pad")
                if (hprefernce.getBoolean("pref_analog_pad2", false)) {
                    item.isChecked = false
                    padManager.analogMode2 = PadManager.MODE_HAT
                    YabauseRunnable.switch_padmode2(PadManager.MODE_HAT)
                    mode = false
                } else {
                    item.isChecked = true
                    padManager.analogMode2 = PadManager.MODE_ANALOG
                    YabauseRunnable.switch_padmode2(PadManager.MODE_ANALOG)
                    mode = true
                }
                val editor = hprefernce.edit()
                editor.putBoolean("pref_analog_pad2", mode)
                editor.apply()
                toggleMenu()
            }

            R.id.menu_item_acp -> {
                // Check if Pro version - Action Replay is a Pro-only feature
                if (!YabauseApplication.isPro()) {
                    YabauseApplication.checkDonated(this)
                    return true
                }

                // Check if hardcore mode is enabled - cheats are not allowed in hardcore
                val retroAchievementsManager = RetroAchievementsManager.getInstance(this)
                if (retroAchievementsManager.isHardcoreEnabled()) {
                    Toast
                        .makeText(
                            this,
                            "Cheats are not allowed in hardcore mode. Disable hardcore mode to use this feature.",
                            Toast.LENGTH_LONG,
                        ).show()
                    return true
                }

                waitingResult = true
                val transaction = supportFragmentManager.beginTransaction()
                val fragment =
                    TabCheatFragment.newInstance(
                        YabauseRunnable.getCurrentGameCode(),
                        cheatCodes,
                    )
                transaction.setCustomAnimations(R.anim.fade_in, R.anim.fade_out)
                transaction.replace(R.id.ext_fragment, fragment, TabCheatFragment.TAG)
                transaction.show(fragment)
                transaction.commit()
            }

            R.id.exit -> {
                progressMessage.text = "Exiting..."
                progressBar.visibility = View.VISIBLE
                waitingResult = true
                val myThread =
                    Thread {
                        val sharedPref = PreferenceManager.getDefaultSharedPreferences(this)
                        if (sharedPref.getBoolean("pref_auto_state_save", false)) {
                            val savePath = YabauseStorage.storage.stateSavePath
                            val currentGamecode = YabauseRunnable.getCurrentGameCode()
                            val saveRoot =
                                currentGamecode?.let { File(YabauseStorage.storage.stateSavePath, it) }
                            if (saveRoot != null) {
                                if (!saveRoot.exists()) saveRoot.mkdir()
                            }
                            var saveFilename = YabauseRunnable.savestate(savePath + currentGamecode)
                            if (saveFilename != null) {
                                val point = saveFilename!!.lastIndexOf(".")
                                if (point != -1) {
                                    saveFilename = saveFilename!!.substring(0, point)
                                }
                                val screenShotSavePath = "$saveFilename.png"
                                if (YabauseRunnable.screenshot(screenShotSavePath) != 0) {
                                } else {
                                }
                            } else {
                            }
                            checkMaxFileCount(savePath + currentGamecode)
                        }

                        YabauseRunnable.deinit()
                        runOnUiThread(
                            Runnable {
                                waitingResult = false
                                // Your code to run in GUI thread here
                                mParcelFileDescriptor?.close()
                                subFileDescripters.forEach {
                                    it.close()
                                }
                                subFileDescripters.clear()

                                if (benchmarkDisc != 0) {
                                    // A run the user exited out of is not a completed
                                    // cycle: it must never be reported as RESULT_OK, or
                                    // Task 14 would store an interrupted run as a real
                                    // measurement.
                                    setBenchmarkResult(RESULT_CANCELED)
                                } else {
                                    val playTime = (System.currentTimeMillis() / 1000L) - startTime
                                    val resultIntent = Intent()
                                    resultIntent.putExtra("playTime", playTime)
                                    setResult(RESULT_OK, resultIntent)
                                }
                                finish()
                                killProcess(myPid())
                            }, // public void run() {
                        )
                    }
                myThread.start()
            }

            R.id.menu_in_game_setting -> {
                waitingResult = true
                val transaction = supportFragmentManager.beginTransaction()
                val currentGameCode = YabauseRunnable.getCurrentGameCode()
                if (currentGameCode.isNullOrEmpty()) {
                    waitingResult = false
                    YabauseRunnable.resume()
                    audio?.unmute(YabauseAudio.SYSTEM)
                    return true
                }
                val fragment = InGamePreference.newInstance(currentGameCode)
                fragment.setOnEndCallback {
                    // getSupportFragmentManager().popBackStack();
                    YabauseRunnable.lockGL()

                    updateViewLayout(resources.configuration.orientation)
                    val gameCode = YabauseRunnable.getCurrentGameCode()
                    val gamePreference = getSharedPreferences(gameCode, Context.MODE_PRIVATE)
                    YabauseRunnable.enableRotateScreen(
                        if (gamePreference.getBoolean(
                                "pref_rotate_screen",
                                false,
                            )
                        ) {
                            1
                        } else {
                            0
                        },
                    )
                    val fps = gamePreference.getBoolean("pref_fps", false)
                    YabauseRunnable.enableFPS(if (fps) 1 else 0)
                    Log.d(TAG, "enable FPS $fps")

                    val rawPg = gamePreference.getStringSafe("pref_polygon_generation", "0").toInt()
                    // On the Vulkan path only GPU_TESSERATION (2) and
                    // COMPUTE_RASTERIZER (3) are valid; anything else falls back to 2.
                    val iPg =
                        if (videoInterface == 4 && rawPg != 3) {
                            2
                        } else {
                            rawPg
                        }
                    YabauseRunnable.setPolygonGenerationMode(iPg)

                    Log.d(TAG, "setPolygonGenerationMode $iPg (videoInterface=$videoInterface)")

                    // issue #22: the VDP2 compositor is user-selectable. Only the
                    // Vulkan core honours the setting (see Vdp2Composite).
                    val compositeMode =
                        Vdp2Composite.resolveMode(
                            videoInterface,
                            gamePreference.getStringSafe(Vdp2Composite.KEY, Vdp2Composite.DEFAULT_VALUE),
                        )
                    YabauseRunnable.setVdp2NewComposite(compositeMode)
                    Log.d(TAG, "setVdp2NewComposite $compositeMode (videoInterface=$videoInterface)")

                    val frameskip = gamePreference.getBoolean("pref_frameskip", true)
                    YabauseRunnable.enableFrameskip(if (frameskip) 1 else 0)
                    Log.d(TAG, "enable enableFrameskip $frameskip")

                    val aspect = gamePreference.getStringSafe("pref_aspect_rate", "0").toInt()
                    YabauseRunnable.setAspectRateMode(aspect)

                    val resolutionSetting = gamePreference.getStringSafe("pref_resolution", "0").toInt()
                    YabauseRunnable.setResolutionMode(resolutionSetting)

                    // The core keeps the user resolution apart from the effective one
                    // (CRT forces scale 1), so the order of these two calls does not matter.
                    val filterSetting = gamePreference.getStringSafe(VideoFilter.KEY, VideoFilter.NONE).toInt()
                    YabauseRunnable.setFilter(filterSetting)
                    Log.d(TAG, "setFilter $filterSetting")

                    YabauseRunnable.enableComputeShader(
                        if (gamePreference.getBoolean(
                                "pref_use_compute_shader",
                                false,
                            )
                        ) {
                            1
                        } else {
                            0
                        },
                    )
                    val rbgResolutionSetting = gamePreference.getStringSafe("pref_rbg_resolution", "0").toInt()
                    YabauseRunnable.setRbgResolutionMode(rbgResolutionSetting)

                    val frameLimitMode = gamePreference.getStringSafe("pref_frameLimit", "0").toInt()
                    YabauseRunnable.setFrameLimitMode(frameLimitMode)

                    // Re-applying the user's preferences would silently replace the
                    // configuration a benchmark run is measuring, so put it back.
                    applyBenchmarkOverrides()

                    YabauseRunnable.unlockGL()

                    // Recreate Yabause View
                    val v = findViewById<View>(R.id.yabause_view)
                    val layout = findViewById<FrameLayout>(R.id.content_main)
                    layout.removeView(v)
                    val yv = YabauseView(this@Yabause)
                    yv.id = R.id.yabause_view
                    layout.addView(yv, 0)
                    val exitFade = Fade()
                    exitFade.duration = 500
                    fragment.exitTransition = exitFade
                    supportFragmentManager
                        .beginTransaction()
                        .remove(fragment)
                        .commitNow()
                    waitingResult = false
                    menuShowing = false
                    val mainview = findViewById(R.id.yabause_view) as View
                    mainview.requestFocus()
                    YabauseRunnable.resume()
                    audio?.unmute(YabauseAudio.SYSTEM)
                }
                transaction.setCustomAnimations(
                    R.anim.fade_in,
                    R.anim.fade_out,
                )
                transaction.replace(R.id.ext_fragment, fragment, InGamePreference.TAG)
                // transaction.addToBackStack(InGamePreference.TAG);
                transaction.commit()
            }
        }
        drawerLayout.closeDrawer(GravityCompat.START)
        return true
    }

    private fun showDiscSwapSheet() {
        waitingResult = true
        val sheet = DiscSwapGameListBottomSheet.newInstance(gameCode)
        sheet.setListener(this)
        sheet.show(supportFragmentManager, DiscSwapGameListBottomSheet.TAG)
    }

    override fun onGameSelectedForDiscSwap(gameInfo: GameInfo) {
        Log.d(TAG, "onGameSelectedForDiscSwap: file_path=${gameInfo.file_path}")
        Log.d(TAG, "onGameSelectedForDiscSwap: iso_file_path=${gameInfo.iso_file_path}")

        waitingResult = false
        menuShowing = false
        YabauseRunnable.resume()
        audio?.unmute(YabauseAudio.SYSTEM)

        val newGameCode = gameInfo.product_number
        val isGameChanged = newGameCode.isNotBlank() && newGameCode != gameCode

        if (isGameChanged) {
            setupGameContext(newGameCode)
        }

        // Emulator must be running before updating gamePath and closing tray
        Handler(Looper.getMainLooper()).postDelayed({
            val filePath = gameInfo.file_path
            if (filePath.startsWith("content://")) {
                val uri = Uri.parse(filePath)
                // Extract filename from URI (same as normal startup at L592-614)
                val fnameIndex = filePath.lastIndexOf("%2F", ignoreCase = true)
                val fname = filePath.substring(fnameIndex + 3)
                val treeUri = UriPermissionHelper.findTreeUriForDocument(this, uri)
                val pfd = if (treeUri != null) {
                    UriPermissionHelper.openDocumentViaTree(this, uri, treeUri)
                } else {
                    contentResolver.openFileDescriptor(uri, "r")
                }
                if (pfd != null) {
                    mParcelFileDescriptor?.close()
                    mParcelFileDescriptor = pfd
                    gamePath = "/proc/self/fd/${pfd.fd};$fname"
                    if (gameInfo.iso_file_path.isNotEmpty()) {
                        currentDocumentUri = Uri.parse(gameInfo.iso_file_path)
                    }
                    Log.d(TAG, "onGameSelectedForDiscSwap: gamePath=$gamePath")
                }
            } else {
                gamePath = filePath
                Log.d(TAG, "onGameSelectedForDiscSwap: gamePath=$gamePath")
            }

            if (isGameChanged) {
                YabauseRunnable.setSkipRaChangeMedia(true)
            }
            YabauseRunnable.closeTray()

            if (isGameChanged && ::retroAchievementsManager.isInitialized) {
                initializeGameForRetroAchievements()
            }
        }, 200)
    }

    override fun onDiscSwapCancelled() {
        waitingResult = false
        YabauseRunnable.resume()
        audio?.unmute(YabauseAudio.SYSTEM)
    }

    public override fun onPause() {
        super.onPause()

        // Clear current activity reference from RetroAchievements manager
        val retroAchievementsManager = RetroAchievementsManager.getInstance(this)
        retroAchievementsManager.setCurrentActivity(null)

        YabauseRunnable.pause()
        audio?.mute(YabauseAudio.SYSTEM)
        inputManager!!.unregisterInputDeviceListener(this)
        scope.coroutineContext.cancelChildren()

        // A paused emulator advances no guest frames while wall time keeps running, so
        // resuming polling later would measure a fabricated slowness. Instead of trying
        // to resume, end the run now: any pause before completion - screen lock, an
        // incoming call, a system dialog - must report RESULT_CANCELED, never leave the
        // activity sitting in the foreground with nothing polling and no result.
        val session = benchmarkSession
        if (benchmarkDisc != 0 && session != null && !session.isComplete && !session.hasSlowLevel) {
            cancelBenchmark(BenchmarkRequest.REASON_PAUSED)
        }
    }

    public override fun onResume() {
        super.onResume()

        // Set current activity for RetroAchievements notifications
        val retroAchievementsManager = RetroAchievementsManager.getInstance(this)
        retroAchievementsManager.setCurrentActivity(this)

        if (tracker != null) {
            tracker!!.setScreenName(TAG)
            tracker!!.send(ScreenViewBuilder().build())
        }
        if (waitingResult == false) {
            audio?.unmute(YabauseAudio.SYSTEM)
            YabauseRunnable.resume()
        }
        inputManager!!.registerInputDeviceListener(this, null)

        if (benchmarkDisc != 0 && benchmarkSession == null) {
            startBenchmarkPolling()
        }
    }

    /**
     * Polls the benchmark ROM's status block until one full level cycle is measured,
     * the guest stalls, or the guest never becomes ready within
     * [BENCHMARK_READY_TIMEOUT_NANOS], then reports the result and finishes the
     * activity. Runs on [scope], which onPause cancels, so a backgrounded benchmark
     * run stops polling rather than continuing invisibly.
     *
     * A run has exactly two outcomes: a full cycle (RESULT_OK with measurements) or
     * nothing (RESULT_CANCELED). A stall or a never-ready guest is not a measurement,
     * so both route to cancelBenchmark() rather than finishBenchmark().
     */
    private fun startBenchmarkPolling() {
        val session = BenchmarkSession(intent.getDoubleArrayExtra(BenchmarkRequest.EXTRA_LEVEL_FPS_FLOOR))
        benchmarkSession = session
        scope.launch {
            val start = SystemClock.elapsedRealtimeNanos()
            var everReady = false
            while (isActive && !session.isComplete && !session.hasSlowLevel) {
                val now = SystemClock.elapsedRealtimeNanos() - start
                val status =
                    BenchmarkStatus.parse(
                        YabauseRunnable.peekMemory(BenchmarkStatus.ADDRESS, BenchmarkStatus.SIZE),
                    )
                if (status != null) {
                    everReady = true
                    session.onSample(status, now)
                    if (session.hasSlowLevel) {
                        // A rung passes only if every level holds its floor, so
                        // the first level that does not has already decided this run.
                        finishBenchmark(session, BenchmarkRequest.REASON_SLOW)
                        return@launch
                    }
                    if (session.isStalled(now)) {
                        cancelBenchmark(BenchmarkRequest.REASON_STALL)
                        return@launch
                    }
                } else if (!everReady && now > BENCHMARK_READY_TIMEOUT_NANOS) {
                    // The ROM never reached its main loop: BenchmarkSession has no
                    // clock of its own and cannot detect this, so the timeout lives
                    // here instead.
                    cancelBenchmark(BenchmarkRequest.REASON_NOT_READY)
                    return@launch
                }
                delay(BENCHMARK_POLL_INTERVAL_MS)
            }
            finishBenchmark(session, BenchmarkRequest.REASON_CYCLE)
        }
    }

    private fun finishBenchmark(
        session: BenchmarkSession,
        reason: String,
    ) {
        val levelFps = session.levelFps
        val maxLevel = levelFps.keys.maxOrNull() ?: 0
        val fpsArray = DoubleArray(maxLevel + 1)
        levelFps.forEach { (level, fps) -> fpsArray[level] = fps }

        val passed = BenchmarkVerdict.passed(levelFps)
        // The per-level numbers are what the thresholds in BenchmarkVerdict are
        // calibrated from, and the result Intent only carries the verdict, so log
        // them here where a device measurement can still be read back. Info level,
        // not debug: some retro-handheld ROMs drop debug logs device-wide, and a
        // measurement that cannot be read back off the device is worthless.
        Log.i(
            TAG,
            "benchmark disc $benchmarkDisc result: passed=$passed reason=$reason " +
                levelFps.toSortedMap().entries.joinToString(" ") {
                    "LV%02d=%.1f".format(it.key, it.value)
                },
        )

        val result =
            Intent().apply {
                putExtra(BenchmarkRequest.EXTRA_PASSED, passed)
                putExtra(BenchmarkRequest.EXTRA_END_REASON, reason)
                putExtra(BenchmarkRequest.EXTRA_LEVEL_FPS, fpsArray)
            }
        setBenchmarkResult(RESULT_OK, result)
        finish()
    }

    /** Ends an incomplete benchmark run: no measurements are reported. */
    private fun cancelBenchmark(reason: String) {
        setBenchmarkResult(
            RESULT_CANCELED,
            Intent().putExtra(BenchmarkRequest.EXTRA_END_REASON, reason),
        )
        finish()
    }

    /**
     * Sets the benchmark's result exactly once. A benchmark launch can reach its exit
     * through several independent paths (completion, stall, readiness timeout, pause,
     * the exit menu), and finishing one of them can re-enter another (finish() invokes
     * onPause()). Without this guard a completed run's RESULT_OK could be overwritten
     * by the RESULT_CANCELED that onPause sets for an incomplete run - so only the
     * first call takes effect.
     */
    private fun setBenchmarkResult(
        resultCode: Int,
        data: Intent? = null,
    ) {
        if (benchmarkResultSet) return
        benchmarkResultSet = true
        setResult(resultCode, data)
    }

    public override fun onDestroy() {
        // Stop axis debug updates
        axisDebugHandler.removeCallbacks(axisDebugRunnable)

        if (benchmarkDisc == 0) {
            // Benchmark mode already set its own result in finishBenchmark(); setResult()
            // replaces rather than merges, so this must not run for a benchmark launch.
            val playTime = (System.currentTimeMillis() / 1000L) - startTime
            val resultIntent = Intent()
            resultIntent.putExtra("playTime", playTime)
            setResult(RESULT_OK, resultIntent)
        }

        // Clean up RetroAchievements resources
        if (::retroAchievementsNotification.isInitialized) {
            retroAchievementsNotification.cleanup()
        }
        if (::retroAchievementsAuthManager.isInitialized) {
            retroAchievementsAuthManager.cleanup()
        }
        if (::retroAchievementsManager.isInitialized) {
            retroAchievementsManager.cleanup()
        }

        Log.v(TAG, "this is the end...")
        audio?.destroy()
        yabauseThread?.destroy()
        super.onDestroy()
    }

    @Deprecated("Deprecated in Java")
    public override fun onCreateDialog(
        dialogId: Int,
        args: Bundle,
    ): Dialog? {
        val builder = AlertDialog.Builder(this)
        builder
            .setMessage(args.getString("message"))
            .setCancelable(false)
            .setNegativeButton(R.string.exit) { _, _ -> finish() }
            .setPositiveButton(R.string.ignore) { dialog, _ -> dialog.cancel() }
        return builder.create()
    }

    fun startReport() {
        waitingResult = true
        val pn = YabauseRunnable.getCurrentGameCode()
        if (pn != null) {
            // Pre-capture screenshot before showing dialog to avoid capturing the dialog itself
            lifecycleScope.launch(kotlinx.coroutines.Dispatchers.IO) {
                val attachmentManager = ReportAttachmentManager(this@Yabause)
                val previewScreenshot =
                    try {
                        attachmentManager.captureScreenshot()
                    } catch (e: Exception) {
                        android.util.Log.e("Yabause", "Failed to pre-capture screenshot", e)
                        null
                    }

                // Show dialog on main thread
                withContext(kotlinx.coroutines.Dispatchers.Main) {
                    val reportDialog = ReportDialog(this@Yabause, pn)
                    reportDialog.setPreCapturedScreenshot(previewScreenshot)
                    reportDialog.setOnReportFinishedListener { rating, message, screenshot ->
                        doReportCurrentGame(rating, message, screenshot)
                    }
                    reportDialog.setOnDialogDismissListener {
                        // Resume emulation when dialog is dismissed
                        if (waitingResult) {
                            waitingResult = false
                            menuShowing = false
                            val mainview = findViewById<View>(R.id.yabause_view)
                            mainview.requestFocus()
                            YabauseRunnable.resume()
                            audio?.unmute(YabauseAudio.SYSTEM)
                        }
                    }
                    reportDialog.show(this@Yabause.supportFragmentManager, "ReportDialog")
                }
            }
        }

        // The device is smaller, so show the fragment fullscreen
        // android.app.FragmentTransaction transaction = getFragmentManager().beginTransaction();
        // For a little polish, specify a transition animation
        // transaction.setTransition(android.app.FragmentTransaction.TRANSIT_FRAGMENT_OPEN);
        // To make it fullscreen, use the 'content' root view as the container
        // for the fragment, which is always the root view for the activity
        // transaction.add(R.id.ext_fragment, newFragment)
        //    .addToBackStack(null).commit();

        // android.app.FragmentTransaction transaction = getFragmentManager().beginTransaction();
        // transaction.replace(R.id.ext_fragment, newFragment, StateListFragment.TAG);
        // transaction.show(newFragment);
        // transaction.commit();
    }

    fun onFinishReport() {}

    override fun onInputDeviceAdded(i: Int) {
        updateInputDevice()
    }

    override fun onInputDeviceRemoved(i: Int) {
        updateInputDevice()
    }

    override fun onInputDeviceChanged(i: Int) {}

    override fun show() {
        if (YabauseRunnable.getRecordingStatus() == YabauseRunnable.RECORDING) {
            YabauseRunnable.screenshot("")
        } else {
            toggleMenu()
        }
    }

    inner class ReportContents {
        @JvmField
        var rating = 0

        @JvmField
        var message: String? = null

        @JvmField
        var screenshot = false

        @JvmField
        var screenshotBase64: String? = null

        @JvmField
        var screenshotSavePath: String? = null
        var stateBase64: String? = null
        var stateSavePath: String? = null
    }

    @JvmField
    var reportStatus = REPORT_STATE_INIT
    var cheatCodes: Array<String?>? = null

    fun updateCheatCode(cheatCodes: Array<String?>?) {
        // Check if hardcore mode is enabled - cheats are not allowed in hardcore
        val retroAchievementsManager = RetroAchievementsManager.getInstance(this)
        if (retroAchievementsManager.isHardcoreEnabled()) {
            // Disable all cheats in hardcore mode
            YabauseRunnable.updateCheat(null)
            return
        }

        this.cheatCodes = cheatCodes
        if (cheatCodes == null || cheatCodes.size == 0) {
            YabauseRunnable.updateCheat(null)
        } else {
            val sendCodes = ArrayList<String>()
            for (i in cheatCodes.indices) {
                val tmp =
                    cheatCodes[i]
                        ?.split("\n".toRegex())
                        ?.dropLastWhile { it.isEmpty() }
                        ?.toTypedArray()
                for (j in tmp?.indices!!) {
                    sendCodes.add(tmp[j])
                }
            }
            YabauseRunnable.updateCheat(sendCodes.toTypedArray())
        }
        if (waitingResult) {
            waitingResult = false
            menuShowing = false
            val mainview = findViewById(R.id.yabause_view) as View
            mainview.requestFocus()
            YabauseRunnable.resume()
            audio?.unmute(YabauseAudio.SYSTEM)
        }
    }

    fun cancelStateLoad() {
        if (waitingResult) {
            waitingResult = false
            menuShowing = false
            val mainview = findViewById(R.id.yabause_view) as View
            mainview.requestFocus()
            YabauseRunnable.resume()
            audio?.unmute(YabauseAudio.SYSTEM)
        }
    }

    fun loadState(filename: String?) {
        YabauseRunnable.loadstate(filename)
        val fg = supportFragmentManager.findFragmentByTag(StateListFragment.TAG)
        if (fg != null) {
            val transaction = supportFragmentManager.beginTransaction()
            transaction.remove(fg)
            transaction.commit()
        }
        if (waitingResult) {
            waitingResult = false
            menuShowing = false
            val mainview = findViewById(R.id.yabause_view) as View
            mainview.requestFocus()
            YabauseRunnable.resume()
            audio?.unmute(YabauseAudio.SYSTEM)
        }
    }

    @Throws(IOException::class)
    private fun createZip(
        zos: ZipOutputStream,
        files: Array<File?>,
    ) {
        val buf = ByteArray(1024)

        for (file in files) {
            val entry = ZipEntry(file!!.name)
            zos.putNextEntry(entry)

            FileInputStream(file).use { fis ->
                BufferedInputStream(fis).use { bis ->
                    var len: Int
                    while (bis.read(buf).also { len = it } != -1) {
                        zos.write(buf, 0, len)
                    }
                }
            }
        }
    }

    val scope = CoroutineScope(Dispatchers.Default)

    fun doReportCurrentGame(
        rating: Int,
        message: String?,
        screenshot: Boolean,
    ) {
        dismissDialog()
/*
        val current_report = ReportContents()
        current_report._rating = rating
        current_report._message = message
        current_report._screenshot = screenshot
        reportStatus = REPORT_STATE_INIT
        val gameinfo = YabauseRunnable.getGameinfo() ?: return
        showDialog()
        val dateFormat: DateFormat = SimpleDateFormat("_yyyy_MM_dd_HH_mm_ss")
        val date = Date()
        val zippath = (YabauseStorage.storage.screenshotPath +
                YabauseRunnable.getCurrentGameCode() +
                dateFormat.format(date) + ".zip")
        val screen_shot_save_path = (YabauseStorage.storage.screenshotPath +
                "screenshot.png")
        if (YabauseRunnable.screenshot(screen_shot_save_path) != 0) {
            dismissDialog()
            return
        }
        val save_path = YabauseStorage.storage.stateSavePath
        val current_gamecode = YabauseRunnable.getCurrentGameCode()
        val save_root = current_gamecode?.let { File(YabauseStorage.storage.stateSavePath, it) }
        if (save_root != null) {
            if (!save_root.exists()) save_root.mkdir()
        }
        val save_filename = YabauseRunnable.savestate(save_path + current_gamecode)
        val files = arrayOfNulls<File>(1)
        files[0] = save_filename?.let { File(it) }
        var zos: ZipOutputStream? = null
        try {
            zos = ZipOutputStream(BufferedOutputStream(FileOutputStream(File(zippath))))
            createZip(zos, files)
        } catch (e: IOException) {
            Log.d(TAG, e.localizedMessage!!)
            dismissDialog()
            return
        } finally {
            IOUtils.closeQuietly(zos)
        }
        files[0]!!.delete()

        try {
            val asyncTask = AsyncReportV2(
                this,
                screen_shot_save_path,
                zippath,
                current_report,
                JSONObject(gameinfo)
            )
            val url = "https://www.uoyabause.org/api/"
            scope.launch {

                val cg = YabauseRunnable.getCurrentGameCode()
                if (cg != null) {
                    asyncTask.report(url, cg)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, e.localizedMessage!!)
            dismissDialog()
            return
        }
 */
    }

    fun cancelReportCurrentGame() {
        scope.coroutineContext.cancelChildren()
        waitingResult = false
        YabauseRunnable.resume()
        audio?.unmute(YabauseAudio.SYSTEM)
    }

    @Deprecated("Deprecated in Java")
    @Suppress("DEPRECATION")
    override fun onActivityResult(
        requestCode: Int,
        resultCode: Int,
        data: Intent?,
    ) {
        super.onActivityResult(requestCode, resultCode, data)
        when (requestCode) {
            menuIdLeaderboard -> {
                waitingResult = false
                toggleMenu()
            }
            0x01 -> {
                waitingResult = false
                toggleMenu()
            }
            returnCodeSignIn -> {
                // Sign-in result is now handled by signInActivityLauncher
            }
        }
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (menuShowing) {
            return super.onGenericMotionEvent(event)
        }
        val rtn = padManager.onGenericMotionEvent(event)
        return if (rtn != 0) {
            true
        } else {
            super.onGenericMotionEvent(event)
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val action = event.action
        val keyCode = event.keyCode

        if (waitingResult) {
            if (action == KeyEvent.ACTION_DOWN && hasWindowFocus()) {
                val cheatFrag = supportFragmentManager.findFragmentByTag(TabCheatFragment.TAG) as? TabCheatFragment
                if (cheatFrag != null) {
                    when (keyCode) {
                        KeyEvent.KEYCODE_BUTTON_L1 -> {
                            cheatFrag.switchToPreviousTab()
                            return true
                        }
                        KeyEvent.KEYCODE_BUTTON_R1 -> {
                            cheatFrag.switchToNextTab()
                            return true
                        }
                        KeyEvent.KEYCODE_DPAD_UP -> {
                            cheatFrag.moveCursor(-1)
                            return true
                        }
                        KeyEvent.KEYCODE_DPAD_DOWN -> {
                            cheatFrag.moveCursor(1)
                            return true
                        }
                        KeyEvent.KEYCODE_BUTTON_A,
                        KeyEvent.KEYCODE_DPAD_CENTER,
                        KeyEvent.KEYCODE_ENTER,
                        -> {
                            val focused = currentFocus
                            if (focused != null &&
                                (focused.id == R.id.button_add || focused.id == R.id.btn_sign_in)
                            ) {
                                focused.performClick()
                            } else {
                                cheatFrag.selectCurrentItem()
                            }
                            return true
                        }
                    }
                }
            }
            return super.dispatchKeyEvent(event)
        }

        if (menuShowing) {
            return super.dispatchKeyEvent(event)
        }

        // Log.d("dispatchKeyEvent","device:" + event.getDeviceId() + ",action:" + action +",keyCoe:" + keyCode );
        if (action == KeyEvent.ACTION_UP) {
            val rtn = padManager.onKeyUp(keyCode, event)
            if (rtn == PadManager.toggleMenu) {
                toggleMenu()
            }
            if (rtn != PadManager.noActionMapped) {
                return true
            }
        } else if (action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
            val rtn = padManager.onKeyDown(keyCode, event)
            if (rtn != PadManager.noActionMapped) {
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }

    private var menuShowing = false

    private fun handleBackPress() {
        val fgIngame =
            supportFragmentManager.findFragmentByTag(InGamePreference.TAG) as InGamePreference?
        if (fgIngame != null) {
            fgIngame.onBackPressed()
            return
        }
        var fg = supportFragmentManager.findFragmentByTag(StateListFragment.TAG)
        if (fg != null) {
            val transaction = supportFragmentManager.beginTransaction()
            transaction.remove(fg)
            transaction.commit()
            val mainv = findViewById<View>(R.id.yabause_view)
            mainv.isActivated = true
            mainv.requestFocus()
            waitingResult = false
            menuShowing = false
            YabauseRunnable.resume()
            audio?.unmute(YabauseAudio.SYSTEM)
            return
        }
        fg = supportFragmentManager.findFragmentByTag(LeaderBoardFragment.TAG)
        if (fg != null) {
            val transaction = supportFragmentManager.beginTransaction()
            transaction.remove(fg)
            transaction.commit()
            val mainv = findViewById<View>(R.id.yabause_view)
            mainv.isActivated = true
            mainv.requestFocus()
            waitingResult = false
            menuShowing = false
            YabauseRunnable.resume()
            audio?.unmute(YabauseAudio.SYSTEM)
            return
        }
        fg = supportFragmentManager.findFragmentByTag(AchievementListFragment.TAG)
        if (fg != null) {
            val transaction = supportFragmentManager.beginTransaction()
            transaction.remove(fg)
            transaction.commit()
            val mainv = findViewById<View>(R.id.yabause_view)
            mainv.isActivated = true
            mainv.requestFocus()
            waitingResult = false
            menuShowing = false
            YabauseRunnable.resume()
            audio?.unmute(YabauseAudio.SYSTEM)
            return
        }
        fg = supportFragmentManager.findFragmentByTag(TabCheatFragment.TAG)
        if (fg != null) {
            val transaction = supportFragmentManager.beginTransaction()
            transaction.remove(fg)
            transaction.commit()
            val mainv = findViewById<View>(R.id.yabause_view)
            mainv.isActivated = true
            mainv.requestFocus()
            waitingResult = false
            menuShowing = false
            YabauseRunnable.resume()
            audio?.unmute(YabauseAudio.SYSTEM)
            return
        }
        val fg2 =
            supportFragmentManager.findFragmentByTag(PadTestFragment.TAG) as PadTestFragment?
        if (fg2 != null) {
            fg2.onBackPressed()
            return
        }
        toggleMenu()
    }

    private fun toggleMenu() {
        if (menuShowing == true) {
            val sharedPref = PreferenceManager.getDefaultSharedPreferences(this)
            val padModeLayer = findViewById<View>(R.id.layer_pad_mode)
            padModeLayer?.alpha = sharedPref.getFloat("pref_pad_trans", 0.7f)

            menuShowing = false
            val mainview = findViewById(R.id.yabause_view) as View
            mainview.requestFocus()
            YabauseRunnable.resume()
            audio?.unmute(YabauseAudio.SYSTEM)
            drawerLayout.closeDrawer(GravityCompat.START)
        } else {
            menuShowing = true
            YabauseRunnable.pause()
            audio?.mute(YabauseAudio.SYSTEM)

            val tx = findViewById<TextView>(R.id.menu_title)
            if (tx != null) {
                val name = YabauseRunnable.getGameTitle()
                tx.text = name
            }
/*
            if (BuildConfig.BUILD_TYPE != "pro") {
                val prefs = getSharedPreferences("private", Context.MODE_PRIVATE)
                val hasDonated = prefs.getBoolean("donated", false)
                if (hasDonated == false) {
                    if (adView != null) {
                        val lp = findViewById<LinearLayout>(R.id.navilayer)
                        if (lp != null) {
                            val mCount = lp.childCount
                            var find = false
                            for (i in 0 until mCount) {
                                val mChild = lp.getChildAt(i)
                                if (mChild === adView) {
                                    find = true
                                }
                            }
                            if (find == false) {
                                lp.addView(adView)
                            }
                            val adRequest = AdRequest.Builder().build()
                            adView!!.loadAd(adRequest)
                        }
                    }
                }
            }
 */
            drawerLayout.openDrawer(GravityCompat.START)
        }
    }

    // private var errmsg: String? = null
    fun errorMsg(msg: String) {
        val errmsg = msg
        Log.d(TAG, "errorMsg $msg")
        runOnUiThread {
            AlertDialog
                .Builder(this)
                .setTitle("Error!")
                .setMessage(errmsg)
                .setPositiveButton(R.string.exit) { _, _ -> finish() }
                .show()

            // Snackbar.make(drawerLayout, errmsg!!, Snackbar.LENGTH_SHORT).show()
        }
    }

    /**
     * Applies the graphics configuration the benchmark is measuring.
     *
     * A benchmark run does not ask what the user has configured: it is told which
     * rung of [BenchmarkLadder] to run, and a rung is a complete configuration -
     * video core included. That is the point of the sweep. Measuring a disc under
     * the settings that happen to be stored would answer a different question for
     * every device.
     *
     * The frame limiter goes off too: with it on, every level would sit at 60fps
     * and nothing would be measured.
     */
    private fun applyBenchmarkOverrides() {
        if (benchmarkDisc == 0) return

        // 1 is "unlimited" in entryValuesFrameLimit.
        YabauseRunnable.setFrameLimitMode(1)

        // The native core asks this activity for the video interface after this
        // runs (GetVideoInterface in yui.cpp), so overwriting the field is what
        // actually switches the renderer for this launch.
        val core = intent.getStringExtra(BenchmarkRequest.EXTRA_VIDEO_CORE)
        videoInterface = core?.toIntOrNull() ?: videoInterface

        val polygon =
            intent.getStringExtra(BenchmarkRequest.EXTRA_POLYGON_GENERATION)
                ?: RecommendedSettings.POLYGON_TRIANGLE
        // Triangle is inert under Vulkan, which accepts only 2 and 3.
        val polygonMode =
            polygon.toIntOrNull()?.let {
                if (videoInterface == VIDEO_INTERFACE_VULKAN && it != 3) 2 else it
            } ?: 0
        YabauseRunnable.setPolygonGenerationMode(polygonMode)

        val computeShader = intent.getBooleanExtra(BenchmarkRequest.EXTRA_COMPUTE_SHADER, false)
        YabauseRunnable.enableComputeShader(if (computeShader) 1 else 0)

        val compositeMode =
            Vdp2Composite.resolveMode(
                videoInterface,
                intent.getStringExtra(BenchmarkRequest.EXTRA_VDP2_COMPOSITE),
            )
        YabauseRunnable.setVdp2NewComposite(compositeMode)

        val rbgResolution =
            intent.getStringExtra(BenchmarkRequest.EXTRA_RBG_RESOLUTION)?.toIntOrNull() ?: 0
        YabauseRunnable.setRbgResolutionMode(rbgResolution)

        // Overrides the per-game pref_resolution read in readPreferences(): the
        // rung under test decides the internal resolution, not whatever the
        // benchmark disc's own settings happen to hold.
        val resolution =
            intent.getStringExtra(BenchmarkRequest.EXTRA_RESOLUTION)?.toIntOrNull() ?: 0
        YabauseRunnable.setResolutionMode(resolution)

        // A rung never includes an output filter, and a CRT filter would force
        // scale 1 and replace the resolution under test.
        YabauseRunnable.setFilter(0)

        Log.i(
            TAG,
            "benchmark disc $benchmarkDisc config: core=$videoInterface polygon=$polygonMode " +
                "compute=$computeShader composite=$compositeMode rbgResolution=$rbgResolution " +
                "resolution=$resolution",
        )
    }

    private fun readPreferences(gamecode: String?) {
        if (gamecode.isNullOrEmpty()) return

        setupInGamePreferences(this, gamecode)

        // ------------------------------------------------------------------------------------------------
        // Load per game setting
        val key = gamecode.replace(" ", "-")
        val gamePreference = getHarmonySharedPreferences(key)
        YabauseRunnable.enableRotateScreen(
            if (gamePreference.getBoolean(
                    "pref_rotate_screen",
                    false,
                )
            ) {
                1
            } else {
                0
            },
        )
        val fps = gamePreference.getBoolean("pref_fps", false)
        YabauseRunnable.enableFPS(if (fps) 1 else 0)
        Log.d(TAG, "enable FPS $fps")
        // val iPg: Int? = gamePreference.getString("pref_polygon_generation", "0")?.toInt()
        // YabauseRunnable.setPolygonGenerationMode(iPg!!)
        // Log.d(TAG, "setPolygonGenerationMode $iPg")
        val frameskip = gamePreference.getBoolean("pref_frameskip", true)
        YabauseRunnable.enableFrameskip(if (frameskip) 1 else 0)
        Log.d(TAG, "enable enableFrameskip $frameskip")

        val aspect = gamePreference.getStringSafe("pref_aspect_rate", "0").toInt()
        YabauseRunnable.setAspectRateMode(aspect)

        val resolutionSetting = gamePreference.getStringSafe("pref_resolution", "0").toInt()
        YabauseRunnable.setResolutionMode(resolutionSetting)
        val rbgResolutionSetting = gamePreference.getStringSafe("pref_rbg_resolution", "0").toInt()
        YabauseRunnable.setRbgResolutionMode(rbgResolutionSetting)

        val frameLimitMode = gamePreference.getStringSafe("pref_frameLimit", "0").toInt()
        YabauseRunnable.setFrameLimitMode(frameLimitMode)

        // Benchmark mode also overrides the video settings, but those are applied
        // further down once videoInterface is known - see applyBenchmarkOverrides().

        // -------------------------------------------------------------------------------------
        // Load common setting
        val sharedPref = PreferenceManager.getDefaultSharedPreferences(this)

        // Check BIOS type for extended memory decision
        val biosType = sharedPref.getString(BiosManager.KEY_BIOS_TYPE, BiosManager.BIOS_TYPE_BUILTIN)
        if (biosType == BiosManager.BIOS_TYPE_FILE) {
            // Disable extended memory when using file-based BIOS
            YabauseRunnable.enableExtendedMemory(0)
            Log.d(TAG, "Extended Memory disabled (file BIOS active)")
        } else {
            // Always enable extended memory (8MB) for built-in BIOS
            YabauseRunnable.enableExtendedMemory(1)
            Log.d(TAG, "Extended Memory enabled (built-in BIOS)")
        }
        var icpu: Int? = sharedPref.getStringSafe("pref_cpu", "3").toInt()
        val abi = System.getProperty("os.arch")
        if (abi!!.contains("64")) {
            if (icpu == 2) {
                icpu = 3
            }
        }
        YabauseRunnable.setCpu(icpu!!.toInt())
        Log.d(TAG, "cpu $icpu")

        val cpuAffinity = sharedPref.getBoolean("pref_use_cpu_affinity", true)
        YabauseRunnable.setUseCpuAffinity(if (cpuAffinity) 1 else 0)

        val sh2Cache = sharedPref.getBoolean("pref_use_sh2_cache", true)
        YabauseRunnable.setUseSh2Cache(if (sh2Cache) 1 else 0)

        // Per game; seeded from the global setting (see IN_GAME_PREFS).
        val ifilter =
            gamePreference
                .getStringSafe(VideoFilter.KEY, sharedPref.getStringSafe(VideoFilter.KEY, VideoFilter.NONE))
                .toInt()
        YabauseRunnable.setFilter(ifilter)
        Log.d(TAG, "setFilter $ifilter")
        val audioout = sharedPref.getBoolean("pref_audio", true)
        if (audioout) {
            audio?.unmute(YabauseAudio.USER)
        } else {
            audio?.mute(YabauseAudio.USER)
        }
        Log.d(TAG, "Audio $audioout")

        // Get BIOS path based on BIOS type (already determined above)
        biosPath =
            if (biosType == BiosManager.BIOS_TYPE_FILE) {
                val biosFile = BiosManager.getBiosFile(this)
                if (biosFile.exists()) {
                    biosFile.absolutePath
                } else {
                    Log.w(TAG, "File BIOS selected but file not found, falling back to built-in")
                    ""
                }
            } else {
                "" // Empty string = use built-in BIOS
            }
        Log.d(TAG, "BIOS type: $biosType, path: $biosPath")
        val cart = sharedPref.getStringSafe("pref_cart", "7")
        if (cart.length > 0) {
            cartridgeType = cart.toInt()
        } else {
            cartridgeType = Cartridge.CART_DRAM32MBIT
        }
        Log.d(TAG, "cart $cart")

        // Load video setting from game-specific preferences. The software
        // renderer is no longer supported, so a stored value is resolved to a
        // supported core (VideoCore).
        val vulkanSupported =
            packageManager.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)
        val storedVideo =
            gamePreference.getStringSafe(
                VideoCore.KEY,
                sharedPref.getStringSafe(VideoCore.KEY, VideoCore.OPENGL),
            )
        val video = VideoCore.resolve(storedVideo, vulkanSupported)
        videoInterface = video.toIntOrNull() ?: -1

        // Vulkan: honour the user polygon_generation setting (only
        // GPU_TESSERATION=2 and COMPUTE_RASTERIZER=3 are valid, anything else
        // falls back to 2). The VDP2 compute shader stays on for Vulkan.
        if (videoInterface == 4) {
            val rawPg = gamePreference.getStringSafe("pref_polygon_generation", "2").toInt()
            val iPg = if (rawPg == 3) 3 else 2
            YabauseRunnable.setPolygonGenerationMode(iPg)
            Log.d(TAG, "setPolygonGenerationMode $iPg (Vulkan)")
            YabauseRunnable.enableComputeShader(1)
            // issue #22: the VDP2 compositor is user-selectable (Vulkan only).
            val compositeMode =
                Vdp2Composite.resolveMode(
                    videoInterface,
                    gamePreference.getStringSafe(Vdp2Composite.KEY, Vdp2Composite.DEFAULT_VALUE),
                )
            YabauseRunnable.setVdp2NewComposite(compositeMode)
            Log.d(TAG, "setVdp2NewComposite $compositeMode (Vulkan)")
        } else {
            val iPg = gamePreference.getStringSafe("pref_polygon_generation", "0").toInt()
            YabauseRunnable.setPolygonGenerationMode(iPg)
            Log.d(TAG, "setPolygonGenerationMode $iPg")
            YabauseRunnable.enableComputeShader(
                if (gamePreference.getBoolean(
                        "pref_use_compute_shader",
                        false,
                    )
                ) {
                    1
                } else {
                    0
                },
            )
        }

        applyBenchmarkOverrides()

        Log.d(TAG, "video $video")
        Log.d(TAG, "getGamePath $gamePath")
        Log.d(TAG, "getMemoryPath $memoryPath")
        Log.d(TAG, "getCartridgePath $cartridgePath")
        // Sound level: 0 = Light (legacy engine, no DSP), 1 = Normal, 2 = Enhanced
        // (engine 1 + high-quality renderer). Unknown values behave as Normal.
        val soundLevel = sharedPref.getStringSafe("pref_sound_engine", "1").toIntOrNull() ?: 1
        val isound = if (soundLevel == 0) 0 else 1
        YabauseRunnable.setSoundEngine(isound)
        Log.d(TAG, "setSoundEngine $isound")
        // A benchmark rung's fps must not depend on the user's sound choice, so
        // Enhanced runs as Normal there.
        val soundHq = if (benchmarkDisc == 0 && soundLevel == 2) 1 else 0
        YabauseRunnable.setSoundHighQuality(soundHq)
        Log.d(TAG, "setSoundHighQuality $soundHq")
        val scspSync = sharedPref.getStringSafe("pref_scsp_sync_per_frame", "1").toInt()
        YabauseRunnable.setScspSyncPerFrame(scspSync)
        val cpuSync = sharedPref.getStringSafe("pref_cpu_sync_per_line", "1").toInt()
        YabauseRunnable.setCpuSyncPerLine(cpuSync)
        val scspTimeSync = sharedPref.getStringSafe("scsp_time_sync_mode", "1").toInt()
        YabauseRunnable.setScspSyncTimeMode(scspTimeSync)

        updateInputDevice()
    }

    /** At game start, shows which key drives each Saturn button when player 1 uses a keyboard. */
    private fun showKeyboardMapIfNeeded() {
        if (!::padManager.isInitialized) {
            Log.d(TAG, "KeyboardMap: pad manager not ready")
            return
        }
        val keyMap = padManager.getPlayer1KeyboardMap()
        Log.d(TAG, "KeyboardMap: player1=${padManager.getPlayer1InputDevice()} map=$keyMap")
        if (keyMap == null) return
        val overlay = findViewById<KeyboardMapOverlayView>(R.id.keyboard_map_overlay)
        if (overlay == null) {
            Log.w(TAG, "KeyboardMap: overlay view is missing from this layout")
            return
        }
        overlay.show(keyMap, padManager.getPlayer1InputDevice())
    }

    fun updateInputDevice() {
        val sharedPref = PreferenceManager.getDefaultSharedPreferences(this)
        val navigationView = findViewById<View>(R.id.nav_view) as NavigationView
        val menu = navigationView.menu
        val navPadDevice = menu.findItem(R.id.menu_item_pad_device)
        val pad = findViewById<View>(R.id.yabause_pad) as YabausePad

        // InputDevice
        var selInputdevice = sharedPref.getString("pref_player1_inputdevice", "65535")
        padManager = PadManager.updatePadManager()!!
        padManager.showMenuListener = this
        Log.d(TAG, "input $selInputdevice")
        // First time
        if (selInputdevice == "65535") {
            // if game pad is connected use it.
            selInputdevice =
                if (padManager.getDeviceCount() > 0) {
                    padManager.setPlayer1InputDevice(null)
                    val editor = sharedPref.edit()
                    editor.putString("pref_player1_inputdevice", padManager.getId(0))
                    editor.commit()
                    padManager.getId(0)
                    // if no game pad is detected use on-screen game pad.
                } else {
                    val editor = sharedPref.edit()
                    editor.putString("pref_player1_inputdevice", "-1")
                    editor.commit()
                    "-1"
                }
        }
        if (padManager.getDeviceCount() > 0 && selInputdevice != "-1") {
            pad.show(false)
            Log.d(TAG, "ScreenPad Disable")
            padManager.setPlayer1InputDevice(selInputdevice)
            for (inputType in 0 until padManager.getDeviceCount()) {
                if (padManager.getId(inputType) == selInputdevice) {
                    navPadDevice.title = padManager.getName(inputType)
                }
            }
            // Enable Swipe
            drawerLayout.setDrawerLockMode(DrawerLayout.LOCK_MODE_UNLOCKED)
        } else {
            pad.show(true)
            Log.d(TAG, "ScreenPad Enable")
            padManager.setPlayer1InputDevice(null)

            // Set Menu item
            navPadDevice.title = getString(R.string.onscreen_pad)

            // Disable Swipe
            drawerLayout.setDrawerLockMode(DrawerLayout.LOCK_MODE_LOCKED_CLOSED)
        }
        val selInputdevice2 = sharedPref.getString("pref_player2_inputdevice", "65535")
        val navPadDeviceP2 = menu.findItem(R.id.menu_item_pad_device_p2)
        padManager.setPlayer2InputDevice(null)
        navPadDeviceP2.title = "Disconnected"
        menu.findItem(R.id.pad_mode_p2).isVisible = false
        menu.findItem(R.id.menu_item_pad_setting_p2).isVisible = false
        if (selInputdevice != "65535" && selInputdevice != "-1") {
            for (inputType in 0 until padManager.getDeviceCount()) {
                if (padManager.getId(inputType) == selInputdevice2) {
                    padManager.setPlayer2InputDevice(selInputdevice2)
                    navPadDeviceP2.title = padManager.getName(inputType)
                    menu.findItem(R.id.pad_mode_p2).isVisible = true
                    menu.findItem(R.id.menu_item_pad_setting_p2).isVisible = true
                }
            }
        }
        var hprefernce = getHarmonySharedPreferences("pref_analog_pad")
        var analog = hprefernce.getBoolean("pref_analog_pad", false)
        val padv = findViewById<View>(R.id.yabause_pad) as YabausePad
        if (analog) {
            padManager.analogMode = PadManager.MODE_ANALOG
            YabauseRunnable.switch_padmode(PadManager.MODE_ANALOG)
            padv.setPadMode(PadManager.MODE_ANALOG)
        } else {
            padManager.analogMode = PadManager.MODE_HAT
            YabauseRunnable.switch_padmode(PadManager.MODE_HAT)
            padv.setPadMode(PadManager.MODE_HAT)
        }
        menu.findItem(R.id.pad_mode).isChecked = analog
        analog = hprefernce.getBoolean("pref_analog_pad2", false)
        if (analog) {
            padManager.analogMode2 = PadManager.MODE_ANALOG
            YabauseRunnable.switch_padmode2(PadManager.MODE_ANALOG)
        } else {
            padManager.analogMode2 = PadManager.MODE_HAT
            YabauseRunnable.switch_padmode2(PadManager.MODE_HAT)
        }
        menu.findItem(R.id.pad_mode_p2).isChecked = analog
        val scspTimeSyncMode = sharedPref.getStringSafe("scsp_time_sync_mode", "1").toInt()
        YabauseRunnable.setScspSyncTimeMode(scspTimeSyncMode)
    }

    val shaderPath: String
        get() = YabauseStorage.storage.shaderPath

    val testPath: String?
        get() =
            if (testCase == null) {
                null
            } else {
                YabauseStorage.storage.recordPath + testCase
            }

    val memoryPath: String
        get() {
            if (tmpBackupFilePath != null) {
                return tmpBackupFilePath!!
            }
            // Use different backup file based on BIOS type
            val sharedPref = PreferenceManager.getDefaultSharedPreferences(this)
            val biosType = sharedPref.getString(BiosManager.KEY_BIOS_TYPE, BiosManager.BIOS_TYPE_BUILTIN)
            val memoryFilename =
                if (biosType == BiosManager.BIOS_TYPE_FILE) {
                    "memory_filebios.ram"
                } else {
                    "memory.ram"
                }
            return YabauseStorage.storage.getMemoryPath(memoryFilename)
        }

    val player2InputDevice: Int
        get() = padManager.getPlayer2InputDevice()

    val cartridgePath: String
        get() =
            YabauseStorage.storage
                .getCartridgePath(Cartridge.getDefaultFilename(cartridgeType))

    /**
     * T018/T019: Verify BIOS configuration is valid on app startup.
     * If file BIOS is selected but file doesn't exist, reset to built-in
     * and show a notification to the user.
     */
    private fun verifyBiosConfiguration(sharedPref: SharedPreferences) {
        val biosType = sharedPref.getString(BiosManager.KEY_BIOS_TYPE, BiosManager.BIOS_TYPE_BUILTIN)
        if (biosType == BiosManager.BIOS_TYPE_FILE) {
            val biosFile = BiosManager.getBiosFile(this)
            if (!biosFile.exists()) {
                Log.w(TAG, "File BIOS selected but file not found: ${biosFile.absolutePath}, falling back to built-in")
                // Reset to built-in BIOS
                sharedPref
                    .edit()
                    .putString(BiosManager.KEY_BIOS_TYPE, BiosManager.BIOS_TYPE_BUILTIN)
                    .apply()
                // Show notification to user
                Toast.makeText(this, R.string.bios_file_not_found, Toast.LENGTH_LONG).show()
            }
        }
    }

    companion object {
        private const val TAG = "Yabause"
        private const val VIDEO_INTERFACE_VULKAN = 4
        const val REPORT_STATE_INIT = 0
        const val REPORT_STATE_SUCCESS = 1
        const val REPORT_STATE_FAIL_DUPE = -1
        const val REPORT_STATE_FAIL_CONNECTION = -2
        const val REPORT_STATE_FAIL_AUTH = -3

        // Poll faster than one guest frame so that a level boundary is never missed
        // by more than a few frames.
        private const val BENCHMARK_POLL_INTERVAL_MS = 8L

        // Provisional value: a slow device booting the HLE BIOS needs real headroom
        // before the ROM's main loop starts writing its status block, so this errs
        // generous. Calibrate after measuring several devices.
        private const val BENCHMARK_READY_TIMEOUT_NANOS = 30_000_000_000L

        init {
            System.loadLibrary("yabause_native")
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && supportFragmentManager.findFragmentById(R.id.ext_fragment) == null) {
            updateViewLayout(resources.configuration.orientation)
        }

        // Reapply immersive mode when window regains focus (after showing system UI temporarily)
        if (hasFocus) {
            val sharedPref = PreferenceManager.getDefaultSharedPreferences(this)
            if (sharedPref.getBoolean("pref_immersive_mode", true)) {
                enableImmersiveMode()
            }
        }
    }

    override fun onDeviceUpdated(target: Int) {}

    override fun onSelected(
        target: Int,
        name: String?,
        id: String?,
    ) {
        val pad = findViewById<View>(R.id.yabause_pad) as YabausePad
        val navigationView = findViewById<View>(R.id.nav_view) as NavigationView
        val menu = navigationView.menu
        val navPadDevice = menu.findItem(R.id.menu_item_pad_device)
        val navPadDeviceP2 = menu.findItem(R.id.menu_item_pad_device_p2)
        padManager = PadManager.updatePadManager()!!
        padManager.showMenuListener = this
        if (padManager.getDeviceCount() > 0 && id != "-1") {
            when (target) {
                SelInputDeviceFragment.PLAYER1 -> {
                    Log.d(TAG, "ScreenPad Disable")
                    pad.show(false)
                    padManager.setPlayer1InputDevice(id)
                    navPadDevice.title = name
                }
                SelInputDeviceFragment.PLAYER2 -> {
                    padManager.setPlayer2InputDevice(id)
                    navPadDeviceP2.title = name
                }
            }
            drawerLayout.setDrawerLockMode(DrawerLayout.LOCK_MODE_UNLOCKED)
        } else {
            if (target == SelInputDeviceFragment.PLAYER1) {
                pad.updateScale()
                pad.show(true)
                navPadDevice.title = getString(R.string.onscreen_pad)
                Log.d(TAG, "ScreenPad Enable")
                padManager.setPlayer1InputDevice(null)
                drawerLayout.setDrawerLockMode(DrawerLayout.LOCK_MODE_LOCKED_CLOSED)
            } else if (target == SelInputDeviceFragment.PLAYER2) {
                padManager.setPlayer2InputDevice(null)
                navPadDeviceP2.title = "Disconnected"
            }
        }
        updateInputDevice()
        waitingResult = false
        toggleMenu()
    }

    override fun onCancel(target: Int) {
        waitingResult = false
        toggleMenu()
    }

    override fun onFinish() {
        val fg = supportFragmentManager.findFragmentByTag(PadTestFragment.TAG) as PadTestFragment?
        if (fg != null) {
            val transaction = supportFragmentManager.beginTransaction()
            transaction.remove(fg)
            transaction.commit()
            val padv = findViewById<View>(R.id.yabause_pad) as YabausePad
            padv.updateScale()

            val analogSwitch = findViewById<SwitchCompat>(R.id.toggleAnalogButton)

            val hprefernce = getHarmonySharedPreferences("pref_analog_pad")
            analogSwitch.isChecked = hprefernce.getBoolean("pref_analog_pad", false)

            val sharedPref = PreferenceManager.getDefaultSharedPreferences(this@Yabause)

            val padModeLayer = findViewById<View>(R.id.layer_pad_mode)
            padModeLayer?.alpha = sharedPref.getFloat("pref_pad_trans", 0.7f)
            if (sharedPref.getBoolean("pref_show_analog_switch", false)) {
                padModeLayer.visibility = View.VISIBLE
            } else {
                padModeLayer.visibility = View.GONE
            }
        }
        waitingResult = false
        toggleMenu()
    }

    override fun onCancel() {
        val fg = supportFragmentManager.findFragmentByTag(PadTestFragment.TAG) as PadTestFragment?
        if (fg != null) {
            val transaction = supportFragmentManager.beginTransaction()
            transaction.remove(fg)
            transaction.commit()
        }
        waitingResult = false
        toggleMenu()
    }

    override fun onUpdateTransparency(a: Float) {
        val padModeLayer = findViewById<View>(R.id.layer_pad_mode)
        padModeLayer?.alpha = a
    }

    override fun onFinishInputSetting() {
        updateInputDevice()
        waitingResult = false
        toggleMenu()
    }

    var currentDocumentUri: Uri? = null

    fun getFileDescriptorPath(fileName: String?): String? {
        if (fileName == null) {
            return null
        }

        val decodedResult: String = URLDecoder.decode(fileName, "UTF-8")

        if (currentDocumentUri == null) {
            return null
        }

        val dir = DocumentFile.fromTreeUri(YabauseApplication.appContext, currentDocumentUri!!)
        if (dir == null) {
            return null
        }

        // for (file in dir!!.listFiles()) {
        //    Log.d("Yabause", "Found file " + file.name + " with size " + file.length())
        // }

        val files = dir.findFile(decodedResult)
        if (files == null) {
            return null
        }

        val parcelFileDescriptor = contentResolver.openFileDescriptor(files.uri, "r")
        if (parcelFileDescriptor != null) {
            subFileDescripters.add(parcelFileDescriptor)
            val apath = "/proc/self/fd/${parcelFileDescriptor.fd}"
            return apath
        }
        return null
    }

    fun onBackupWrite(
        fname: String,
        deviceId: Int,
        before: ByteArray,
        after: ByteArray,
    ) {
        Log.d(this.javaClass.name, "onBackupWrite fname=$fname deviceId=$deviceId size=${before.size}")
        currentGame?.onBackUpUpdated(fname, before, after)

        // Determine backup file key from currently loaded file
        val backupFileKey = if (deviceId == 0) {
            File(memoryPath).nameWithoutExtension
        } else {
            File(cartridgePath).nameWithoutExtension
        }

        // Capture screenshot at backup write timing
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                val dir = File("${YabauseStorage.storage.screenshotPath}$backupFileKey/")
                if (!dir.exists()) dir.mkdirs()
                val path = "${dir.absolutePath}/backup_$fname.png"
                val result = YabauseRunnable.screenshot(path)
                if (result != 0) {
                    Log.w("Yabause", "Failed to capture backup screenshot for $fname")
                }

                // Save game info for auto-fill in Share dialog
                val gameTitle = YabauseRunnable.getGameTitle()
                val gameCode = YabauseRunnable.getCurrentGameCode()
                if (!gameTitle.isNullOrBlank() || !gameCode.isNullOrBlank()) {
                    val prefs = getSharedPreferences("backup_game_info", Context.MODE_PRIVATE)
                    prefs
                        .edit()
                        .putString("title_${backupFileKey}_$fname", gameTitle?.trim())
                        .putString("product_${backupFileKey}_$fname", gameCode?.trim())
                        .apply()
                }
            } catch (e: Exception) {
                Log.e("Yabause", "Error in onBackupWrite", e)
            }
        }
    }

    override fun onNewRecord(leaderBoardId: String) {
        runOnUiThread {
            var snackbar =
                Snackbar.make(
                    this.drawerLayout,
                    "Congratulations for the New Record!",
                    Snackbar.LENGTH_LONG,
                )
            snackbar.setAction(
                "Check Leader board",
            ) { _: View? ->
                YabauseRunnable.pause()
                audio?.mute(YabauseAudio.SYSTEM)
                val gameCode = YabauseRunnable.getCurrentGameCode()
                if (gameCode != null) {
                    waitingResult = true
                    val fragment = LeaderBoardFragment.newInstance(gameCode)
                    supportFragmentManager
                        .beginTransaction()
                        .replace(R.id.ext_fragment, fragment, LeaderBoardFragment.TAG)
                        .show(fragment)
                        .commit()
                }
            }
            snackbar.show()
        }
    }

    /**
     * Initialize RetroAchievements integration
     * Now checks login status first and only initializes if already logged in or performs login
     */
    private fun initializeRetroAchievements() {
        Log.d(TAG, "Initializing RetroAchievements integration...")

        try {
            // Get authentication manager (already initialized at application startup)
            retroAchievementsAuthManager = RetroAchievementsAuthManager.getInstance(this)
            retroAchievementsNotification = RetroAchievementsNotification(this)

            // Set game context in RetroAchievements notification if available
            if (currentGame != null) {
                retroAchievementsNotification.currentGame = currentGame
            }

            // Set this activity for game path access
            retroAchievementsAuthManager.setYabaseActivity(this)

            // Check if already logged in
            if (retroAchievementsAuthManager.isRetroAchievementsLoggedIn()) {
                Log.d(TAG, "RetroAchievements already logged in, proceeding with full initialization")

                // User is already logged in, proceed with full initialization
                initializeRetroAchievementsManagers()

                // Proceed directly to game initialization
                initializeGameForRetroAchievements()
            } else {
                Log.d(TAG, "RetroAchievements not logged in, checking for auto-login...")

                // Set up auth callbacks to handle login completion
                setupRetroAchievementsAuthCallbacks()

                // Check if auto-login is enabled and we have saved credentials
                if (retroAchievementsAuthManager.isAutoLoginEnabled()) {
                    Log.d(TAG, "Auto-login enabled, waiting for login completion...")
                    // Auth manager will handle auto-login, we wait for callback
                } else {
                    Log.d(TAG, "No auto-login configured, user needs to login manually")
                    // User needs to login manually via settings
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to initialize RetroAchievements", e)
        }
    }

    /**
     * Initialize RetroAchievements managers and native integration
     * Only called when user is confirmed to be logged in
     */
    private fun initializeRetroAchievementsManagers() {
        Log.d(TAG, "Initializing RetroAchievements managers...")

        // Initialize managers
        retroAchievementsManager = RetroAchievementsManager.getInstance(this)

        // Set up event callbacks
        setupRetroAchievementsCallbacks()

        // Initialize native RetroAchievements using the new manager system
        val initResult = retroAchievementsManager.initialize()
        if (initResult) {
            Log.d(TAG, "RetroAchievements native integration initialized successfully")
        } else {
            Log.e(TAG, "Failed to initialize RetroAchievements native integration")
        }
    }

    /**
     * Set up authentication callbacks to handle login state changes
     */
    private fun setupRetroAchievementsAuthCallbacks() {
        retroAchievementsAuthManager.onAuthStateChanged = { isLoggedIn, username ->
            if (isLoggedIn && username != null) {
                Log.d(TAG, "RetroAchievements login completed for user: $username")

                runOnUiThread {
                    // Show login success notification following rcheevos guidelines
                    retroAchievementsNotification.showLoginSuccess(username)
                }

                // Now initialize the full RetroAchievements system
                initializeRetroAchievementsManagers()

                // Initialize game for RetroAchievements
                initializeGameForRetroAchievements()
            } else {
                Log.d(TAG, "RetroAchievements login failed or logout detected")
                // Do not show success notification for failed logins
            }
        }
    }

    private fun setupGameContext(newGameCode: String) {
        gameCode = newGameCode
        readPreferences(newGameCode)

        try {
            val raManager = RetroAchievementsManager.getInstance(this)
            raManager.setCurrentGameCode(newGameCode)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to apply RA hardcore setting", e)
        }

        currentGame = when (newGameCode) {
            "GS-9170", "MK-81800" -> SonicR(newGameCode).also { it.uievent = this }
            "GS-9047", "MK-81207", "GS-9116", "MK-81215" -> SegaRally(newGameCode).also { it.uievent = this }
            else ->
                org.uoyabause.android.game
                    .GenericGame(newGameCode)
                    .also { it.uievent = this }
        }

        if (::retroAchievementsNotification.isInitialized) {
            retroAchievementsNotification.currentGame = currentGame
        }

        if (currentGame != null) {
            YabauseRunnable.enableBackupWriteHook()
        }
    }

    /**
     * Initialize current game for RetroAchievements if one is loaded
     */
    private fun initializeGameForRetroAchievements() {
        // Check if we have a current game path to load
        if (gamePath != null) {
            Log.d(TAG, "Loading game for RetroAchievements: $gamePath ")
            retroAchievementsManager.loadGame(gamePath!!) { success, error ->
                if (success) {
                    Log.d(TAG, "RetroAchievements game loaded successfully")
                } else {
                    Log.e(TAG, "Failed to load game for RetroAchievements: $error")
                }
            }
        } else {
            Log.d(TAG, "No current game to load for RetroAchievements")
        }
    }

    /**
     * Load game for RetroAchievements and show fragment after completion
     * @param onComplete Callback to execute after game is loaded (shows fragment)
     */
    private fun loadGameAndShowFragment(onComplete: () -> Unit) {
        if (gamePath != null) {
            Log.d(TAG, "Loading game for RetroAchievements before showing fragment: $gamePath")

            // Set up callback for when game is actually loaded by native layer
            var callbackHandled = false
            val handler = android.os.Handler(android.os.Looper.getMainLooper())

            val showFragmentRunnable = Runnable {
                if (!callbackHandled) {
                    callbackHandled = true
                    Log.d(TAG, "Timeout reached, showing fragment anyway")
                    onComplete()
                }
            }

            retroAchievementsManager.onGameLoaded = { success, error ->
                if (!callbackHandled) {
                    callbackHandled = true
                    handler.removeCallbacks(showFragmentRunnable)
                    runOnUiThread {
                        if (success) {
                            Log.d(TAG, "RetroAchievements game actually loaded, showing fragment")
                        } else {
                            Log.e(TAG, "Failed to load game for RetroAchievements: $error")
                        }
                        onComplete()
                    }
                }
            }

            // Set timeout fallback (3 seconds)
            handler.postDelayed(showFragmentRunnable, 3000)

            // Start loading game (async - will trigger onGameLoaded when complete)
            retroAchievementsManager.loadGame(gamePath!!) { success, error ->
                if (!success) {
                    // If loadGame fails immediately (e.g., not logged in), handle it here
                    if (!callbackHandled) {
                        callbackHandled = true
                        handler.removeCallbacks(showFragmentRunnable)
                        runOnUiThread {
                            Log.e(TAG, "Failed to start game load: $error")
                            onComplete()
                        }
                    }
                }
                // If success, we wait for onGameLoaded callback or timeout
            }
        } else {
            Log.d(TAG, "No current game to load for RetroAchievements, showing fragment directly")
            onComplete()
        }
    }

    /**
     * Set up RetroAchievements event callbacks
     */
    private fun setupRetroAchievementsCallbacks() {
        // Achievement unlocked callback (updated to new signature)
        retroAchievementsManager.onAchievementUnlocked = { achievementId, title, description, points, imageUrl, isUnofficial ->
            runOnUiThread {
                retroAchievementsNotification.showAchievementUnlocked(achievementId, title, description, points, imageUrl, isUnofficial)
            }
        }

        // Leaderboard submit callback (updated to new signature)
        retroAchievementsManager.onLeaderboardSubmit = { leaderboardId, title, description, scoreString ->
            runOnUiThread {
                retroAchievementsNotification.showLeaderboardSubmit(leaderboardId, title, description, scoreString)
            }
        }

        // Rich presence update callback
        retroAchievementsManager.onRichPresenceUpdate = { richPresence ->
            runOnUiThread {
                retroAchievementsNotification.showRichPresenceUpdate(richPresence)
            }
        }

        // Note: Authentication state change callback is now handled in setupRetroAchievementsAuthCallbacks()

        // Login result callback for explicit login attempts
        retroAchievementsAuthManager.onLoginResult = { success, error ->
            if (!success && error != null) {
                Log.e(TAG, "RetroAchievements login failed: $error")
                runOnUiThread {
                    // Show error toast for explicit login failures
                    android.widget.Toast
                        .makeText(
                            this@Yabause,
                            "RetroAchievements login failed: $error",
                            android.widget.Toast.LENGTH_LONG,
                        ).show()
                }
            }
        }
    }

    /**
     * Load game for RetroAchievements
     * This should be called after a game is successfully loaded
     */
    private fun loadRetroAchievementsGame() {
        if (!::retroAchievementsAuthManager.isInitialized) {
            Log.w(TAG, "RetroAchievements not initialized, skipping game load")
            return
        }

        // Only load if user is logged in to RetroAchievements
        if (!retroAchievementsAuthManager.isRetroAchievementsLoggedIn()) {
            Log.d(TAG, "User not logged in to RetroAchievements, skipping game load")
            return
        }

        val gamePathToLoad = gamePath
        if (gamePathToLoad.isNullOrEmpty()) {
            Log.w(TAG, "No game path available for RetroAchievements")
            return
        }

        // val gameCode = YabauseRunnable.getCurrentGameCode()
        // Log.d(TAG, "Loading RetroAchievements game: $gamePathToLoad (gameCode: $gameCode)")

        retroAchievementsManager.loadGame(gamePathToLoad) { success, error ->
            runOnUiThread {
                if (success) {
                    Log.d(TAG, "RetroAchievements game loaded successfully")
                    // Optionally show a toast or notification
                } else {
                    Log.e(TAG, "Failed to load RetroAchievements game: $error")
                }
            }
        }
    }

    /**
     * Show RetroAchievements login dialog
     * @param onSuccess Callback to execute after successful login
     */
    private fun showRetroAchievementsLoginDialog(onSuccess: () -> Unit) {
        val dialogView = layoutInflater.inflate(R.layout.dialog_ra_login, null)
        val usernameEdit = dialogView.findViewById<EditText>(R.id.username_input)
        val passwordEdit = dialogView.findViewById<EditText>(R.id.api_key_input)
        val createAccountLink = dialogView.findViewById<TextView>(R.id.create_account_link)

        createAccountLink.setOnClickListener {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://retroachievements.org/createaccount.php"))
            startActivity(intent)
        }

        val dialog = AlertDialog
            .Builder(this)
            .setTitle(R.string.retroachievements_login_dialog_title)
            .setView(dialogView)
            .setPositiveButton(R.string.login) { _, _ ->
                val username = usernameEdit.text.toString()
                val password = passwordEdit.text.toString()
                if (username.isNotEmpty() && password.isNotEmpty()) {
                    // Use a flag to ensure callback is only handled once
                    var loginHandled = false
                    val handleLoginResult: (Boolean, String?) -> Unit = { success, _ ->
                        if (!loginHandled) {
                            loginHandled = true
                            runOnUiThread {
                                if (success) {
                                    Toast.makeText(this, R.string.retroachievements_login_success, Toast.LENGTH_SHORT).show()
                                    // Initialize RetroAchievements managers
                                    initializeRetroAchievementsManagers()
                                    // Load game and then show fragment after game is loaded
                                    loadGameAndShowFragment(onSuccess)
                                } else {
                                    Toast.makeText(this, R.string.retroachievements_login_failed, Toast.LENGTH_LONG).show()
                                    waitingResult = false
                                    YabauseRunnable.resume()
                                    audio?.unmute(YabauseAudio.SYSTEM)
                                }
                            }
                        }
                    }

                    // Set both callbacks for redundancy (onAuthStateChanged is called first, then onLoginResult)
                    retroAchievementsAuthManager.onAuthStateChanged = { isLoggedIn, _ ->
                        if (isLoggedIn) {
                            handleLoginResult(true, null)
                        }
                    }
                    retroAchievementsAuthManager.onLoginResult = { success, error ->
                        handleLoginResult(success, error)
                    }
                    retroAchievementsAuthManager.loginRetroAchievements(username, password)
                } else {
                    Toast.makeText(this, R.string.please_enter_username_password, Toast.LENGTH_SHORT).show()
                    waitingResult = false
                    YabauseRunnable.resume()
                    audio?.unmute(YabauseAudio.SYSTEM)
                }
            }.setNegativeButton(R.string.cancel) { _, _ ->
                waitingResult = false
                YabauseRunnable.resume()
                audio?.unmute(YabauseAudio.SYSTEM)
            }.setOnCancelListener {
                // Handle back button or tap outside dialog
                waitingResult = false
                YabauseRunnable.resume()
                audio?.unmute(YabauseAudio.SYSTEM)
            }.create()
        dialog.show()
    }

    /**
     * Setup Firebase sign-in launcher for RetroAchievements
     */
    private fun setupFirebaseSignInLauncher() {
        firebaseSignInLauncher =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
                // Check if user is actually logged in (more reliable than result code)
                val currentUser = FirebaseAuth.getInstance().currentUser
                if (currentUser != null) {
                    // Firebase sign-in succeeded, now show RA login dialog
                    pendingRALoginCallback?.let { callback ->
                        pendingRALoginCallback = null
                        showRetroAchievementsLoginDialog(callback)
                    }
                } else {
                    // Firebase sign-in failed or cancelled
                    pendingRALoginCallback = null
                    waitingResult = false
                    YabauseRunnable.resume()
                    audio?.unmute(YabauseAudio.SYSTEM)
                    Toast.makeText(this, R.string.sign_in_cancelled, Toast.LENGTH_SHORT).show()
                }
            }
    }

    /**
     * Launch Firebase sign-in flow
     */
    private fun launchFirebaseSignIn() {
        val signInIntent =
            AuthUI
                .getInstance()
                .createSignInIntentBuilder()
                .setTheme(R.style.AppTheme)
                .setAvailableProviders(
                    listOf(
                        GoogleBuilder().build(),
                        AppleBuilder().build(),
                    ),
                ).build()
        firebaseSignInLauncher.launch(PcSignInActivity.wrap(this, signInIntent))
    }

    /**
     * Show Achievements fragment
     */
    private fun showAchievementsFragment() {
        val transaction = supportFragmentManager.beginTransaction()
        val fragment = AchievementListFragment.newInstance()
        transaction.setCustomAnimations(R.anim.fade_in, R.anim.fade_out)
        transaction.replace(R.id.ext_fragment, fragment, AchievementListFragment.TAG)
        transaction.show(fragment)
        transaction.commit()
    }

    /**
     * Show LeaderBoard fragment
     * @param gameCode The game code for loading leaderboard data
     */
    private fun showLeaderBoardFragment(gameCode: String) {
        val fragment = LeaderBoardFragment.newInstance(gameCode)
        fragment.closeListener = object : LeaderBoardFragment.OnLeaderboardCloseListener {
            override fun onLeaderboardClose() {
                val transaction = supportFragmentManager.beginTransaction()
                transaction.remove(fragment)
                transaction.commit()
                val mainv = findViewById<View>(R.id.yabause_view)
                mainv.isActivated = true
                mainv.requestFocus()
                waitingResult = false
                menuShowing = false
                YabauseRunnable.resume()
                audio?.unmute(YabauseAudio.SYSTEM)
            }
        }
        supportFragmentManager
            .beginTransaction()
            .replace(R.id.ext_fragment, fragment, LeaderBoardFragment.TAG)
            .show(fragment)
            .commit()
    }

    /**
     * Setup edge-to-edge window insets handling for Android 15+ (API 35+)
     */
    private fun setupEdgeToEdgeInsets() {
        val rootView = findViewById<DrawerLayout>(R.id.drawer_layout)
        val contentMain = findViewById<FrameLayout>(R.id.content_main)

        ViewCompat.setOnApplyWindowInsetsListener(rootView) { view, windowInsets ->
            val insets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())

            // Apply top padding to the main content area to avoid status bar overlap
            // Only apply when not in immersive mode to avoid conflicts with existing logic
            val sharedPref = PreferenceManager.getDefaultSharedPreferences(this)
            val isImmersiveMode = sharedPref.getBoolean("pref_immersive_mode", true)

            if (!isImmersiveMode) {
                contentMain.setPadding(
                    contentMain.paddingLeft,
                    insets.top,
                    contentMain.paddingRight,
                    contentMain.paddingBottom,
                )
            }

            // Update YabausePad with navigation bar height to adjust StartButton position
            val padView = findViewById<YabausePad>(R.id.yabause_pad)
            padView?.updateWindowInsets(insets.bottom)

            // Return the insets unchanged for other views
            windowInsets
        }
    }

    /**
     * Enable immersive mode to hide system bars (status bar and navigation bar)
     */
    private fun enableImmersiveMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Android 11 (API 30) and above
            window.insetsController?.let { controller ->
                controller.hide(WindowInsets.Type.systemBars())
                controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            // Below Android 11
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
            )
        }
    }

    /**
     * Disable immersive mode to show system bars
     */
    private fun disableImmersiveMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Android 11 (API 30) and above
            window.insetsController?.show(WindowInsets.Type.systemBars())
        } else {
            // Below Android 11
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_VISIBLE
        }
    }
}
