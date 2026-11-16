package org.uoyabause.android

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.app.Dialog
import android.content.Context
import android.content.DialogInterface
import android.content.Intent
import android.content.SharedPreferences
import android.content.SharedPreferences.OnSharedPreferenceChangeListener
import android.content.pm.PackageManager
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.os.storage.StorageManager
import android.os.storage.StorageVolume
import android.text.InputType
import android.util.Log
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.RequiresApi
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat.getSystemService
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.fragment.app.DialogFragment
import androidx.lifecycle.lifecycleScope
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.PreferenceCategory
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.PreferenceManager
import androidx.preference.PreferenceScreen
import androidx.preference.SeekBarPreference
import androidx.preference.SwitchPreferenceCompat
import com.firebase.ui.auth.AuthUI
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.firebase.database.FirebaseDatabase
import com.google.firebase.firestore.FirebaseFirestore
import com.google.firebase.storage.FirebaseStorage
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.YabauseStorage.Companion.storage
import org.uoyabause.android.auth.AuthState
import org.uoyabause.android.auth.DiscordLinkActivity
import org.uoyabause.android.auth.RetroAchievementsAuthManager
import org.uoyabause.android.tv.GameSelectFragment
import java.util.ArrayList

class SettingsActivity : AppCompatActivity() {
    class WarningDialogFragment : DialogFragment() {
        override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
            // Use the Builder class for convenient dialog construction
            val builder = AlertDialog.Builder(requireActivity())
            val res = resources
            builder
                .setMessage(res.getString(R.string.msg_opengl_not_supported))
                .setPositiveButton("OK") { _, _ ->
                    // FIRE ZE MISSILES!
                }

            // Create the AlertDialog object and return it
            return builder.create()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContentView(R.layout.settings_activity)

        // Setup edge-to-edge window insets handling
        setupEdgeToEdgeInsets()
        supportFragmentManager
            .beginTransaction()
            .replace(R.id.settings, SettingsFragment())
            .commit()
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
    }

    class SettingsFragment :
        PreferenceFragmentCompat(),
        InputManager.InputDeviceListener,
        OnSharedPreferenceChangeListener {
        private val dialogFragmentTag = "CustomPreference"
        var inputManager: InputManager? = null
        var restartLevel = 0

        // RetroAchievements manager
        private lateinit var retroAchievementsAuthManager: RetroAchievementsAuthManager

        // Launches BenchmarkActivity from the "apply recommended settings" entry
        // when this device's GPU has never been measured. On return, settings
        // are re-derived from whatever GpuProfileStore now holds rather than
        // from the activity result extras directly - RESULT_CANCELED saved
        // nothing, so this just leaves the current settings as they are.
        private val benchmarkLauncher =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
                // A cancelled benchmark saved nothing, so re-deriving would just
                // rewrite the safe-side values already in place - and recreate()
                // would flash the screen for no change the user asked for.
                if (result.resultCode == Activity.RESULT_OK) {
                    val context = requireContext()
                    val caps = DeviceCapabilities.forContext(context)
                    val profile = GpuProfileStore.find(context, caps.gpuKey)
                    val recommended = RecommendedSettings.forDevice(caps, profile)
                    val prefs = PreferenceManager.getDefaultSharedPreferences(context)
                    RecommendedSettingsApplier.apply(prefs, recommended)
                    requireActivity().recreate()
                }
            }

        /**
         * Confirms, then launches the benchmark for [gpuKey]. A run always
         * overwrites the stored profile for that GPU, so the caller supplies the
         * message that matches whether one exists yet.
         */
        private fun showBenchmarkDialog(
            context: Context,
            gpuKey: String,
            titleId: Int,
            messageId: Int,
        ) {
            BenchmarkConfirmDialog.show(
                activity = requireActivity(),
                titleId = titleId,
                messageId = messageId,
                negativeId = android.R.string.cancel,
            ) {
                val gpuName = DeviceInfo.gpuRenderer().ifBlank { gpuKey }
                benchmarkLauncher.launch(BenchmarkActivity.intent(context, gpuKey, gpuName))
            }
        }

        /**
         * Adds the "run benchmark" button to a recommended-settings dialog. The
         * measurement stays reachable once this GPU already has a profile: a
         * driver update or a new emulator build changes what the device
         * sustains, and re-measuring is the only way to refresh it. Nothing is
         * added where a result would have nowhere to go - PC never consults a
         * profile, and a GPU with no key cannot be saved.
         */
        private fun addBenchmarkButton(
            builder: MaterialAlertDialogBuilder,
            context: Context,
            caps: DeviceCapabilities,
            gpuKey: String?,
        ) {
            if (caps.isPc || gpuKey == null) return
            builder.setNeutralButton(R.string.benchmark_confirm_run) { _, _ ->
                val gpuName = DeviceInfo.gpuRenderer().ifBlank { gpuKey }
                // The confirm dialog is shown every time, not only on a first
                // run. It carries the checkbox that decides whether the result is
                // shared, and that checkbox is the only place the answer can be
                // seen or changed - skipping the dialog for someone who answered
                // once would lock their answer in for good.
                BenchmarkConfirmDialog.show(
                    activity = requireActivity(),
                    titleId = R.string.benchmark_confirm_title,
                    messageId = R.string.benchmark_confirm_message,
                    negativeId = android.R.string.cancel,
                ) {
                    benchmarkLauncher.launch(BenchmarkActivity.intent(context, gpuKey, gpuName))
                }
            }
        }

        /**
         * Safely set ListPreference summary from entry, handling ClassCastException
         * when the stored value type doesn't match (e.g., Integer stored as String).
         */
        private fun ListPreference.safeSummary() {
            summary = try {
                entry
            } catch (e: ClassCastException) {
                val sharedPref = PreferenceManager.getDefaultSharedPreferences(requireContext())
                value = sharedPref.getStringSafe(key, entryValues?.firstOrNull()?.toString() ?: "")
                entry
            }
        }

        /**
         * Offers the filters [videoCore] implements (see [VideoFilter]) and
         * shows the stored one, or None when this core does not offer it.
         * The stored value is not rewritten.
         */
        private fun ListPreference.applyFilterChoices(videoCore: String?) {
            val labels = resources.getStringArray(R.array.entries_video_filte_list_preference)
            val values = resources.getStringArray(R.array.entryvalues_video_filte_list_preference).toList()
            val offered = VideoFilter.offeredValues(videoCore, values)
            entries = offered.map { labels[values.indexOf(it)] }.toTypedArray()
            entryValues = offered.toTypedArray()
            isEnabled = VideoFilter.isSelectable(videoCore)
            updateFilterSummary()
        }

        /** The license notices of the third-party code in the app (res/raw/third_party_notices.txt). */
        private fun showThirdPartyNotices() {
            val context = requireContext()
            val text =
                context.resources
                    .openRawResource(R.raw.third_party_notices)
                    .bufferedReader()
                    .use { it.readText() }
            val dialog =
                MaterialAlertDialogBuilder(context)
                    .setTitle(R.string.third_party_notices)
                    .setMessage(text)
                    .setPositiveButton(R.string.close, null)
                    .create()
            dialog.setOnShowListener {
                val button = dialog.getButton(DialogInterface.BUTTON_POSITIVE)
                button?.post {
                    button.isFocusable = true
                    button.isFocusableInTouchMode = true
                    button.requestFocus()
                }
            }
            dialog.show()
        }

        private fun ListPreference.updateFilterSummary() {
            val offered = entryValues?.map { it.toString() } ?: emptyList()
            val sharedPref = PreferenceManager.getDefaultSharedPreferences(requireContext())
            val shown = VideoFilter.displayValue(sharedPref.getStringSafe(key, VideoFilter.NONE), offered)
            val index = offered.indexOf(shown)
            summary = if (index >= 0) entries[index] else null
        }

        override fun onResume() {
            super.onResume()
            inputManager?.registerInputDeviceListener(this, null)
            preferenceScreen.sharedPreferences
                ?.registerOnSharedPreferenceChangeListener(this)
        }

        override fun onPause() {
            super.onPause()
            inputManager?.unregisterInputDeviceListener(this)
            preferenceScreen.sharedPreferences
                ?.unregisterOnSharedPreferenceChangeListener(this)
        }

        override fun onDestroy() {
            super.onDestroy()
        }

        /**
         * Set up account preferences
         */
        private fun setupAccountPreferences() {
            // Set up Account Management preference (New unified interface)
            val accountManagementPref = findPreference("pref_account_management") as Preference?
            accountManagementPref?.onPreferenceClickListener =
                Preference.OnPreferenceClickListener {
                    val intent = Intent(requireContext(), org.uoyabause.android.auth.ui.SimpleAccountManagementActivity::class.java)
                    startActivity(intent)
                    true
                }

            // Hide legacy preferences (integrated into Account Management screen)
            // Set up Discord link preference (HIDDEN - integrated into Account Management)
            val discordLinkPref = findPreference("pref_discord_link") as Preference?
            discordLinkPref?.isVisible = false
            discordLinkPref?.onPreferenceClickListener =
                Preference.OnPreferenceClickListener {
                    val intent = Intent(requireContext(), DiscordLinkActivity::class.java)
                    startActivity(intent)
                    true
                }

            // Set up login to other devices preference (HIDDEN - integrated into Account Management as PIN feature)
            val loginToOtherPref = findPreference("pref_login_to_other") as Preference?
            loginToOtherPref?.isVisible = false
            loginToOtherPref?.onPreferenceClickListener =
                Preference.OnPreferenceClickListener {
                    // val presenter = GameSelectPresenter(this@SettingsFragment, null)
                    ShowPinInFragment.newInstance().show(parentFragmentManager, "pin_dialog")
                    true
                }

            // Set up delete account preference (HIDDEN - integrated into Account Management)
            val deleteAccountPref = findPreference("pref_delete_account") as Preference?
            deleteAccountPref?.isVisible = false
            deleteAccountPref?.isEnabled = AuthState.isSignedIn()
            deleteAccountPref?.onPreferenceClickListener =
                Preference.OnPreferenceClickListener {
                    showDeleteAccountConfirmation()
                    true
                }
        }

        /**
         * Show confirmation dialog for account deletion
         */
        private fun showDeleteAccountConfirmation() {
            val currentUser = AuthState.realUser()
            if (currentUser == null) {
                Toast
                    .makeText(
                        requireContext(),
                        "You must be signed in to delete your account",
                        Toast.LENGTH_SHORT,
                    ).show()
                return
            }

            AlertDialog
                .Builder(requireContext())
                .setTitle(R.string.delete_account_confirmation_title)
                .setMessage(R.string.delete_account_confirmation_message)
                .setPositiveButton(R.string.yes) { _, _ ->
                    deleteUserAccount()
                }.setNegativeButton(R.string.no, null)
                .show()
        }

        /**
         * Delete user account and all associated data
         */
        private fun deleteUserAccount() {
            val currentUser = AuthState.realUser() ?: return
            val userId = currentUser.uid

            lifecycleScope.launch {
                try {
                    val db = FirebaseFirestore.getInstance()

                    // 1. Delete user data from Realtime Database
                    deleteUserDataFromDatabase(userId)

                    // 2. Delete user data from Firestore: users collection
                    // Note: delete() does not throw an error if the document doesn't exist.
                    val userDocRef = db.collection("users").document(userId)
                    userDocRef.delete().await()
                    Log.d("SettingsActivity", "Attempted Firestore document deletion: users/$userId") // Log attempt

                    // 3. Delete user data from Firestore: discord_links collection
                    // Note: delete() does not throw an error if the document doesn't exist.
                    val discordLinkDocRef = db.collection("discord_links").document(userId)
                    discordLinkDocRef.delete().await()
                    Log.d("SettingsActivity", "Attempted Firestore document deletion: discord_links/$userId") // Log attempt

                    // 4. Delete user files from Storage
                    deleteUserFilesFromStorage(userId)

                    // 5. Delete the user account
                    currentUser.delete().await()

                    // 6. Sign out
                    AuthUI.getInstance().signOut(requireContext())

                    // 7. Show success message
                    Toast
                        .makeText(
                            requireContext(),
                            R.string.account_deleted,
                            Toast.LENGTH_SHORT,
                        ).show()

                    // 8. Update UI
                    findPreference<Preference>("pref_delete_account")?.isEnabled = false
                } catch (e: Exception) {
                    Log.e("SettingsActivity", "Error deleting user account", e)
                    // Show error message
                    Toast
                        .makeText(
                            requireContext(),
                            "${getString(R.string.account_deletion_failed)}: ${e.message}",
                            Toast.LENGTH_LONG,
                        ).show()
                }
            }
        }

        private suspend fun deleteUserDataFromDatabase(userId: String) {
            // Realtimedatabaseにある "/user-posts/{userId}" にある全データを削除
            val baseurl = "/user-posts/$userId" // Use string template for clarity
            val database = FirebaseDatabase.getInstance()
            val userPostsRef = database.getReference(baseurl)

            try {
                userPostsRef.removeValue().await() // Use await() for suspend function
                // Optionally log success or perform other actions
                Log.d("SettingsActivity", "Successfully deleted data for user: $userId at path $baseurl")
            } catch (e: Exception) {
                // Handle potential errors during deletion
                Log.e("SettingsActivity", "Error deleting data for user: $userId at path $baseurl", e)
                // Rethrow or handle the error as appropriate for the application context
                // Consider showing an error message to the user
                throw e // Re-throw the exception if the caller needs to handle it
            }
        }

        /**
         * Delete user files from Firebase Storage
         */
        private suspend fun deleteUserFilesFromStorage(userId: String) {
            val storage = FirebaseStorage.getInstance()

            try {
                // List all files in the user's directory
                val listResult = storage.reference
                    .child(userId)
                    .listAll()
                    .await()

                // Delete each item
                for (item in listResult.items) {
                    item.delete().await()
                }

                // Recursively delete each prefix (subdirectory)
                for (prefix in listResult.prefixes) {
                    deleteStorageDirectory(prefix.path)
                }
            } catch (e: Exception) {
                // Log error but continue
                Log.e("SettingsActivity", "Error deleting storage files: ${e.message}")
            }
        }

        /**
         * Recursively delete a directory in Firebase Storage
         */
        private suspend fun deleteStorageDirectory(path: String) {
            val storage = FirebaseStorage.getInstance()

            try {
                val listResult = storage.reference
                    .child(path)
                    .listAll()
                    .await()

                // Delete each item
                for (item in listResult.items) {
                    item.delete().await()
                }

                // Recursively delete each prefix
                for (prefix in listResult.prefixes) {
                    deleteStorageDirectory(prefix.path)
                }
            } catch (e: Exception) {
                // Log error but continue
                Log.e("SettingsActivity", "Error deleting storage directory: ${e.message}")
            }
        }

        fun setUpInstall() {
            // No-op: installCount-based display removed in favor of MAX_FREE_GAMES real game count check
        }

        override fun onInputDeviceAdded(id: Int) {
            PadManager.updatePadManager()
            syncInputDevice("player1")
            syncInputDevice("player2")
        }

        override fun onInputDeviceRemoved(id: Int) {
            PadManager.updatePadManager()
            syncInputDevice("player1")
            syncInputDevice("player2")
        }

        override fun onInputDeviceChanged(id: Int) {
            PadManager.updatePadManager()
            syncInputDevice("player1")
            syncInputDevice("player2")
        }

        @RequiresApi(Build.VERSION_CODES.N)
        override fun onCreatePreferences(
            savedInstanceState: Bundle?,
            rootKey: String?,
        ) {
            // Older versions stored pref_scsp_sync_per_frame as String (ListPreference).
            // It is now a SeekBarPreference (Int) — migrate stale String values before
            // setPreferencesFromResource, otherwise SeekBarPreference.onSetInitialValue
            // throws ClassCastException reading getInt() on a String entry.
            PreferenceManager
                .getDefaultSharedPreferences(requireContext())
                .migrateStringToIntPreference(
                    "pref_scsp_sync_per_frame",
                    defaultInt = 4,
                    min = 1,
                    max = 255,
                )

            setPreferencesFromResource(R.xml.preferences, rootKey)

            val installLocation = findPreference("pref_install_location") as ListPreference?
            if (installLocation != null) {
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
                    installLocation.isVisible = false
                } else {
                    val labels: MutableList<CharSequence> = ArrayList()
                    val values: MutableList<CharSequence> = ArrayList()

                    val sm = requireActivity().getSystemService(STORAGE_SERVICE) as StorageManager
                    val map: Map<String, String> =
                        when {
                            Build.VERSION.SDK_INT >= Build.VERSION_CODES.R -> {
                                // Android 11- (API 30)
                                sm.storageVolumes
                                    .mapNotNull { volume ->
                                        val path = volume.directory?.absolutePath ?: return@mapNotNull null
                                        val label = volume.getDescription(requireActivity()) ?: return@mapNotNull null
                                        path to label
                                    }.toMap()
                            }
                            Build.VERSION.SDK_INT >= Build.VERSION_CODES.N -> {
                                // Android 7-10 (API 24-29)
                                val getPath = StorageVolume::class.java.getDeclaredMethod("getPath")
                                sm.storageVolumes
                                    .mapNotNull { volume ->
                                        val path = (getPath.invoke(volume) as String?) ?: return@mapNotNull null
                                        val label = volume.getDescription(requireActivity()) ?: return@mapNotNull null
                                        path to label
                                    }.toMap()
                            }
                            else -> {
                                // Android 4-6 (API 14-23)
                                val getVolumeList = sm.javaClass.getDeclaredMethod("getVolumeList")
                                (getVolumeList.invoke(sm) as Array<*>)
                                    .filterNotNull()
                                    .mapNotNull { volume ->
                                        val getPath = volume.javaClass.getDeclaredMethod("getPath") ?: return@mapNotNull null
                                        val getLabel = volume.javaClass.getDeclaredMethod("getDescription", Context::class.java)
                                        val path = (getPath.invoke(volume) as String?) ?: return@mapNotNull null
                                        val label = (getLabel.invoke(volume, requireActivity()) as String?) ?: return@mapNotNull null
                                        path to label
                                    }.toMap()
                            }
                        }

                    var index = 0
                    map.forEach { _, label ->
                        labels.add(label)
                        values.add(index.toString())
                        index++
                    }

                    installLocation.entries = labels.toTypedArray()
                    installLocation.entryValues = values.toTypedArray()
                    installLocation.safeSummary()
                }
            }

            var inputSetting1 = findPreference("pref_player1_inputdef_file") as InputSettingPreference?
            inputSetting1!!.setPlayerAndFilename(0, "keymap")
            var inputSetting2 = findPreference("pref_player2_inputdef_file") as InputSettingPreference?
            inputSetting2!!.setPlayerAndFilename(1, "keymap_player2")

            val res = resources
            val storage = storage

            // cartridge
            val cart =
                preferenceManager.findPreference("pref_cart") as ListPreference?
            if (cart != null) {
                val cartLabels: MutableList<CharSequence> = ArrayList()
                val cartValues: MutableList<CharSequence> = ArrayList()

                for (cartType in 0 until Cartridge.typeCount) {
                    cartLabels.add(Cartridge.getName(cartType))
                    cartValues.add(Integer.toString(cartType))
                }

                cart.entries = cartLabels.toTypedArray() // cartentries
                cart.entryValues = cartValues.toTypedArray()
                cart.safeSummary()
            }

            // Cpu
            val cpuSetting =
                preferenceManager.findPreference("pref_cpu") as ListPreference?
            cpuSetting!!.safeSummary()
            val abi = System.getProperty("os.arch")
            if (abi?.contains("64") == true) {
                val cpuLabels: MutableList<CharSequence> = ArrayList()
                val cpuValues: MutableList<CharSequence> = ArrayList()
                cpuLabels.add(res.getString(R.string.new_dynrec_cpu_interface))
                cpuValues.add("3")
                cpuLabels.add(res.getString(R.string.software_cpu_interface))
                cpuValues.add("0")
                // val cpu_entries = arrayOfNulls<CharSequence>(cpuLabels.size)
                // cpuLabels.toArray<CharSequence>(cpu_entries)
                // val cpu_entryValues = arrayOfNulls<CharSequence>(cpuValues.size)
                // cpuValues.toArray<CharSequence>(cpu_entryValues)
                cpuSetting.entries = cpuLabels.toTypedArray()
                cpuSetting.entryValues = cpuValues.toTypedArray()
                cpuSetting.safeSummary()
            }

            // Video
            val videoCart =
                preferenceManager.findPreference("pref_video") as ListPreference?

            val videoLabels: MutableList<CharSequence> = ArrayList()
            val videoValues: MutableList<CharSequence> = ArrayList()

            val activityManager = requireActivity().getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val configurationInfo = activityManager.deviceConfigurationInfo
            val supportsEs3 = configurationInfo.reqGlEsVersion >= 0x30000

            val deviceSupportsAEP: Boolean =
                requireActivity().getPackageManager().hasSystemFeature(PackageManager.FEATURE_OPENGLES_EXTENSION_PACK)

            if (supportsEs3) {
                videoLabels.add(res.getString(R.string.opengl_video_interface))
                videoValues.add(VideoCore.OPENGL)
            } else {
                val newFragment = WarningDialogFragment()
                newFragment.show(parentFragmentManager, "OGL")
            }

            // The software renderer is no longer supported and is not offered.
            val vulkanSupported =
                requireActivity().packageManager.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)
            if (vulkanSupported) {
                videoLabels.add(res.getString(R.string.vulkan_video_interface))
                videoValues.add(VideoCore.VULKAN)
            }

            videoCart!!.entries = videoLabels.toTypedArray()
            videoCart.entryValues = videoValues.toTypedArray()
            // A device that still has the software renderer stored must not show
            // an entry that no longer exists.
            val resolvedCore = VideoCore.resolve(videoCart.value, vulkanSupported)
            if (resolvedCore != VideoCore.UNSET && resolvedCore != videoCart.value) {
                videoCart.value = resolvedCore
            }
            videoCart.safeSummary()

            // Filter
            val filterSetting =
                preferenceManager.findPreference(VideoFilter.KEY) as ListPreference?
            filterSetting!!.applyFilterChoices(videoCart.value)

            // scsp
            val scspSetting =
                preferenceManager.findPreference("pref_scsp_sync_per_frame") as SeekBarPreference?
            scspSetting!!.summary = scspSetting.value.toString()

            // Polygon Generation

            // ListPreference cpu_sync_setting = (ListPreference) getPreferenceManager().findPreference("pref_cpu_sync_per_line");
            // cpu_sync_setting.setSummary(cpu_sync_setting.getEntry());

            // Polygon Generation
            val polygonSetting =
                preferenceManager.findPreference("pref_polygon_generation") as ListPreference?
            polygonSetting!!.safeSummary()

            if ((videoCart.value == "4")) {
                // Vulkan: GPU_TESSERATION (2) または COMPUTE_RASTERIZER (3) を選択可能。
                // PERSPECTIVE_CORRECTION (0) / CPU_TESSERATION (1) は Vulkan 経路で
                // 未対応のため候補から除外する。
                polygonSetting.isEnabled = true
                polygonSetting.entries =
                    arrayOf(
                        res.getString(R.string.poly_gpu_tess),
                        res.getString(R.string.poly_compute_rasterizer),
                    )
                polygonSetting.entryValues = arrayOf("2", "3")
                // 既存値が Vulkan で無効な場合は GPU_TESSERATION にフォールバック。
                if (polygonSetting.value !in setOf("2", "3")) {
                    polygonSetting.value = "2"
                }
            } else if ((videoCart.value == "1")) {
                // OpenGL: Triangles (0) and GPU_TESSERATION (2) only.
                // COMPUTE_RASTERIZER (3) is Vulkan-only and must not appear here.
                polygonSetting.isEnabled = true
                polygonSetting.entries =
                    arrayOf(
                        res.getString(R.string.poly_triangle),
                        res.getString(R.string.poly_gpu_tess),
                    )
                polygonSetting.entryValues = arrayOf("0", "2")
                if (polygonSetting.value !in setOf("0", "2")) {
                    polygonSetting.value = "0"
                }
            } else {
                polygonSetting.isEnabled = true
                // 非 Vulkan に戻ったら全選択肢を復活させる。
                polygonSetting.entries = res.getStringArray(R.array.entries_polygon_generation_type)
                polygonSetting.entryValues = res.getStringArray(R.array.entryvalues_polygon_generation_type)
            }

            if (deviceSupportsAEP == false) {
                polygonSetting.entries =
                    arrayOf("Triangles using perspective correction")
                polygonSetting.entryValues = arrayOf("0")
            }

            polygonSetting.setOnPreferenceChangeListener { _, newValue ->
                if (newValue == "3") {
                    // VDP1 Compute Rasterizer is experimental: show the report-bugs notice once.
                    ExperimentalFeatureNotice.maybeShow(
                        requireContext(),
                        ExperimentalFeatureNotice.KEY_VDP1_COMPUTE_RASTERIZER,
                    )
                }
                true
            }

            val computeShaderSetting =
                preferenceManager.findPreference("pref_use_compute_shader") as SwitchPreferenceCompat?
            if (videoCart.value == "4") {
                computeShaderSetting?.isEnabled = false
                computeShaderSetting?.isChecked = true
            } else if (videoCart.value == "1") {
                computeShaderSetting?.isEnabled = true
            } else {
                computeShaderSetting?.isEnabled = false
                computeShaderSetting?.isChecked = false
            }

            // VDP2 compositing: the legacy path is cheaper on old GPUs, but only
            // the Vulkan renderer reads the setting at all.
            val compositeSetting =
                preferenceManager.findPreference(Vdp2Composite.KEY) as ListPreference?
            compositeSetting?.safeSummary()
            compositeSetting?.isEnabled =
                Vdp2Composite.isSelectable(videoCart.value?.toIntOrNull() ?: -1)

            findPreference<Preference>("pref_third_party_notices")?.setOnPreferenceClickListener {
                showThirdPartyNotices()
                true
            }

            findPreference<Preference>("pref_apply_recommended")?.setOnPreferenceClickListener {
                val context = requireContext()
                val caps = DeviceCapabilities.forContext(context)
                val profile = GpuProfileStore.find(context, caps.gpuKey)
                val recommended = RecommendedSettings.forDevice(caps, profile)
                val prefs = PreferenceManager.getDefaultSharedPreferences(context)
                val gpuKey = caps.gpuKey

                // No measured or curated profile exists for this GPU. Offer the
                // benchmark instead of a diff against the generic safe-side
                // defaults - PC never reaches here (forDevice always reports
                // needsBenchmark=false for it), and a GPU with no key has
                // nowhere to save a measurement, so both fall through to the
                // ordinary diff dialog below.
                if (recommended.needsBenchmark && !caps.isPc && gpuKey != null) {
                    showBenchmarkDialog(
                        context,
                        gpuKey,
                        R.string.apply_recommended_settings,
                        R.string.benchmark_settings_entry_message,
                    )
                    return@setOnPreferenceClickListener true
                }

                val changes = RecommendedSettingsApplier.diff(prefs, recommended)

                if (changes.isEmpty()) {
                    val builder =
                        MaterialAlertDialogBuilder(context)
                            .setMessage(R.string.recommended_settings_no_change)
                            .setPositiveButton(android.R.string.ok, null)
                    addBenchmarkButton(builder, context, caps, gpuKey)
                    val dialog = builder.create()
                    dialog.setOnShowListener {
                        val button = dialog.getButton(DialogInterface.BUTTON_POSITIVE)
                        button?.post {
                            button.isFocusable = true
                            button.isFocusableInTouchMode = true
                            button.requestFocus()
                        }
                    }
                    dialog.show()
                } else {
                    val message =
                        getString(R.string.recommended_settings_scope_note) +
                            "\n\n" +
                            changes.joinToString("\n") {
                                RecommendedSettingsLabels.describe(context, it)
                            }
                    val builder =
                        MaterialAlertDialogBuilder(context)
                            .setTitle(R.string.apply_recommended_settings)
                            .setMessage(message)
                            .setPositiveButton(R.string.recommended_settings_apply) { _, _ ->
                                RecommendedSettingsApplier.apply(prefs, recommended)
                                requireActivity().recreate()
                            }.setNegativeButton(android.R.string.cancel, null)
                    addBenchmarkButton(builder, context, caps, gpuKey)
                    val dialog = builder.create()
                    // Applying settings is a data change, so default D-pad focus goes to Cancel.
                    dialog.setOnShowListener {
                        val button = dialog.getButton(DialogInterface.BUTTON_NEGATIVE)
                        button?.post {
                            button.isFocusable = true
                            button.isFocusableInTouchMode = true
                            button.requestFocus()
                        }
                    }
                    dialog.show()
                }
                true
            }

            val onscreenPad =
                findPreference("on_screen_pad") as PreferenceScreen?
            onscreenPad!!.onPreferenceClickListener =
                Preference.OnPreferenceClickListener {
                    val nextActivity = Intent(requireContext(), PadTestActivity::class.java)
                    startActivity(nextActivity)
                    true
                }

            syncInputDevice("player1")
            syncInputDevice("player2")

            /*
        Preference select_image = findPreference("select_image");
        select_image.setOnPreferenceClickListener(new OnPreferenceClickListener() {
            @Override
            public boolean onPreferenceClick(Preference preference) {

                Intent intent = new Intent();
                intent.setType("image/ *");
                intent.setAction(Intent.ACTION_GET_CONTENT);
                int PICK_IMAGE = 1;
                startActivityForResult(Intent.createChooser(intent, "Select Picture"), PICK_IMAGE);

                return true;
            }
        });
*/
            val soundengineSetting =
                preferenceManager.findPreference("pref_sound_engine") as ListPreference?
            soundengineSetting!!.safeSummary()

            val resolutionSetting =
                preferenceManager.findPreference("pref_resolution") as ListPreference?
            resolutionSetting!!.safeSummary()

            val aspectSetting =
                preferenceManager.findPreference("pref_aspect_rate") as ListPreference?
            aspectSetting!!.safeSummary()

            val rbgResolutionSetting =
                preferenceManager.findPreference("pref_rbg_resolution") as ListPreference?
            rbgResolutionSetting!!.safeSummary()

            val scspTimeSyncSetting =
                preferenceManager.findPreference("scsp_time_sync_mode") as ListPreference?
            scspTimeSyncSetting!!.safeSummary()

            // scspSyncTime is SeekBarPreference - no need for setOnBindEditTextListener

            val frameLimitSetting =
                preferenceManager.findPreference("pref_frameLimit") as ListPreference?
            frameLimitSetting!!.safeSummary()

            // Set up account preferences
            setupAccountPreferences()

            // Set up RetroAchievements preferences
            setupRetroAchievementsPreferences()

            // T020: Clean up old BIOS preference if migrating from old version
            migrateOldBiosPreference()

            setUpInstall()
        }

        /**
         * T020: Handle migration from old pref_bios ListPreference to new picker.
         * Shows a one-time notice if old preference exists and clears it.
         */
        private fun migrateOldBiosPreference() {
            val prefs = PreferenceManager.getDefaultSharedPreferences(requireContext())
            val oldBiosPref = prefs.getString("pref_bios", null)
            if (!oldBiosPref.isNullOrEmpty()) {
                // Old preference exists with a value - show migration notice
                Toast
                    .makeText(
                        requireContext(),
                        "BIOS selection has changed. Please re-select your BIOS file in Settings if needed.",
                        Toast.LENGTH_LONG,
                    ).show()
                // Clear the old preference
                prefs.edit().remove("pref_bios").apply()
                Log.d("SettingsActivity", "Migrated old pref_bios: $oldBiosPref")
            }
        }

        override fun onDisplayPreferenceDialog(preference: Preference) {
            val f: DialogFragment?
            if (preference is InputSettingPreference) {
                f = InputSettingPreferenceFragment.newInstance(preference.getKey())
            } else if (preference is BiosFilePickerPreference) {
                f = BiosFilePickerFragment.newInstance(preference.getKey())
            } else {
                f = null
            }

            if (f != null) {
                @Suppress("DEPRECATION")
                f.setTargetFragment(this, 0)
                f.show(parentFragmentManager, dialogFragmentTag)
            } else {
                super.onDisplayPreferenceDialog(preference)
            }
        }

        private fun syncInputDevice(player: String) {
            val devicekey = "pref_" + player + "_inputdevice"
            val defkey = "pref_" + player + "_inputdef_file"
            val sharedPref = PreferenceManager.getDefaultSharedPreferences(this.requireActivity())
            val res = resources
            val padm = PadManager.padManager
            val inputDevice =
                preferenceManager.findPreference(devicekey) as ListPreference?
            val inputLabels: MutableList<CharSequence> = ArrayList()
            val inputValues: MutableList<CharSequence> = ArrayList()
            inputLabels.add(res.getString(R.string.onscreen_pad))
            inputValues.add("-1")
            for (inputType in 0 until (padm?.getDeviceCount() ?: 0)) {
                val name = padm?.getName(inputType) ?: continue
                val id = padm?.getId(inputType) ?: continue
                inputLabels.add(name)
                inputValues.add(id)
            }
            inputDevice?.entries = inputLabels.toTypedArray()
            inputDevice?.entryValues = inputValues.toTypedArray()
            inputDevice?.safeSummary()

            var inputSetting = preferenceManager.findPreference(defkey) as InputSettingPreference?
            var onscreenPad = preferenceManager.findPreference("on_screen_pad") as PreferenceScreen?
            if (inputSetting != null) {
                try {
                    val selectedInputDevice = sharedPref.getString(devicekey, "65535")
                    if ((padm?.getDeviceCount() ?: 0) > 0 && !selectedInputDevice.equals("-1")) {
                        inputSetting.setEnabled(true)
                        if (player == "player1") onscreenPad!!.setEnabled(true)
                    } else {
                        inputSetting.setEnabled(false)
                        if (player == "player1") onscreenPad!!.setEnabled(true)
                    }

                    if (player == "player1") {
                        padm?.setPlayer1InputDevice(selectedInputDevice)
                    } else {
                        padm?.setPlayer2InputDevice(selectedInputDevice)
                    }
                } catch (e: Exception) {
                    e.printStackTrace()
                }
            }
        }

        override fun onSharedPreferenceChanged(
            sharedPreferences: SharedPreferences?,
            key: String?,
        ) {
            if (key == "scsp_time_sync_mode" ||
                key == "pref_cart" ||
                key == "pref_video" ||
                key == "pref_cpu" ||
                key == "pref_filter" ||
                key == "pref_polygon_generation" ||
                key == "pref_sound_engine" ||
                key == "pref_resolution" ||
                key == "pref_rbg_resolution" ||
                key == "pref_cpu_sync_per_line" ||
                key == "pref_aspect_rate" ||
                key == "pref_frameLimit" ||
                key == Vdp2Composite.KEY
            ) {
                val pref = findPreference(key) as ListPreference?
                pref!!.safeSummary()
                if (key == VideoFilter.KEY) {
                    pref.updateFilterSummary()
                }
                if (key == "pref_video") {
                    val filterSetting =
                        preferenceManager.findPreference(VideoFilter.KEY) as ListPreference?
                    filterSetting!!.applyFilterChoices(pref.value)
                    val polygonSetting =
                        preferenceManager.findPreference("pref_polygon_generation") as ListPreference?
                    polygonSetting!!.safeSummary()
                    // polygonSetting.isEnabled = (pref.value == "1" || pref.value == "4")

                    if (pref.value == "4") {
                        // Vulkan: GPU_TESSERATION (2) と COMPUTE_RASTERIZER (3) を選択可能。
                        polygonSetting.isEnabled = true
                        polygonSetting.entries =
                            arrayOf(
                                resources.getString(R.string.poly_gpu_tess),
                                resources.getString(R.string.poly_compute_rasterizer),
                            )
                        polygonSetting.entryValues = arrayOf("2", "3")
                        if (polygonSetting.value !in setOf("2", "3")) {
                            polygonSetting.value = "2"
                        }
                    } else if (pref.value == "1") {
                        // OpenGL: Triangles (0) and GPU_TESSERATION (2) only.
                        // COMPUTE_RASTERIZER (3) is Vulkan-only and must not appear here.
                        polygonSetting.isEnabled = true
                        polygonSetting.entries =
                            arrayOf(
                                resources.getString(R.string.poly_triangle),
                                resources.getString(R.string.poly_gpu_tess),
                            )
                        polygonSetting.entryValues = arrayOf("0", "2")
                        if (polygonSetting.value !in setOf("0", "2")) {
                            polygonSetting.value = "0"
                        }
                    } else {
                        polygonSetting.isEnabled = true
                        polygonSetting.entries = resources.getStringArray(R.array.entries_polygon_generation_type)
                        polygonSetting.entryValues = resources.getStringArray(R.array.entryvalues_polygon_generation_type)
                    }

                    val computeShaderSetting =
                        preferenceManager.findPreference("pref_use_compute_shader") as SwitchPreferenceCompat?
                    if (pref.value == "4") {
                        computeShaderSetting?.isEnabled = false
                        computeShaderSetting?.isChecked = true
                    } else if (pref.value == "1") {
                        computeShaderSetting?.isEnabled = true
                    } else {
                        computeShaderSetting?.isEnabled = false
                        computeShaderSetting?.isChecked = false
                    }

                    val compositeSetting =
                        preferenceManager.findPreference(Vdp2Composite.KEY) as ListPreference?
                    compositeSetting?.isEnabled =
                        Vdp2Composite.isSelectable(pref.value?.toIntOrNull() ?: -1)
                }
            } else if (key == "pref_player1_inputdevice") {
                val pref = findPreference(key) as ListPreference?
                pref!!.safeSummary()
                syncInputDevice("player1")
                syncInputDevice("player2")
            } else if (key == "pref_player2_inputdevice") {
                val pref = findPreference(key) as ListPreference?
                pref!!.safeSummary()
                syncInputDevice("player1")
                syncInputDevice("player2")
            }

            val install =
                preferenceManager.findPreference("pref_install_location") as ListPreference?
            if (install != null) {
                install.summary = install.entry
            }

            val download =
                preferenceManager.findPreference("pref_game_download_directory") as ListPreference?
            if (download != null) {
                download.summary = download.entry
            }

            if (key == "pref_scsp_sync_per_frame") {
                val ep = findPreference(key) as SeekBarPreference?
                var synccount = ep?.value ?: 0

                if (synccount <= 0) {
                    synccount = 1
                    ep?.value = synccount
                } else if (synccount > 255) {
                    synccount = 255
                    ep?.value = synccount
                }
                ep?.summary = synccount.toString()
            }

            if (key == "pref_force_androidtv_mode") {
                if (restartLevel <= 1) restartLevel = 2
                updateResultCode()
            }
        }

        fun updateResultCode() {
            val resultIntent = Intent()
            if (restartLevel == 1) {
                requireActivity().setResult(
                    GameSelectFragment.GAMELIST_NEED_TO_UPDATED,
                    resultIntent,
                )
            } else if (restartLevel == 2) {
                requireActivity().setResult(
                    GameSelectFragment.GAMELIST_NEED_TO_RESTART,
                    resultIntent,
                )
            } else {
                requireActivity().setResult(0, resultIntent)
            }
        }

        /**
         * Set up RetroAchievements preferences (HIDDEN - integrated into Account Management)
         */
        private fun setupRetroAchievementsPreferences() {
            // Hide RetroAchievements category and preferences (integrated into Account Management screen)
            val retroAchievementsCategory = findPreference<PreferenceCategory>("retroachievements_category")
            retroAchievementsCategory?.isVisible = false

            // Initialize RetroAchievements auth manager
            retroAchievementsAuthManager = RetroAchievementsAuthManager.getInstance(requireContext())

            // Enable auto-login by default (always auto-login on app startup)
            retroAchievementsAuthManager.setAutoLoginEnabled(true)

            // Login preference (HIDDEN)
            val loginPref = findPreference<Preference>("pref_retroachievements_login")
            loginPref?.isVisible = false
            loginPref?.onPreferenceClickListener =
                Preference.OnPreferenceClickListener {
                    showRetroAchievementsLoginDialog()
                    true
                }

            // Logout preference (HIDDEN)
            val logoutPref = findPreference<Preference>("pref_retroachievements_logout")
            logoutPref?.isVisible = false
            logoutPref?.onPreferenceClickListener =
                Preference.OnPreferenceClickListener {
                    performRetroAchievementsLogout()
                    true
                }

            // Status preference (HIDDEN)
            val statusPref = findPreference<Preference>("pref_retroachievements_status")
            statusPref?.isVisible = false

            // Set up auth state change listener
            retroAchievementsAuthManager.onAuthStateChanged = { isLoggedIn, username ->
                activity?.runOnUiThread {
                    if (isAdded) {
                        updateRetroAchievementsPreferencesUI()
                    }
                }
            }

            // Update initial UI state
            updateRetroAchievementsPreferencesUI()
        }

        /**
         * Show RetroAchievements login dialog
         */
        private fun showRetroAchievementsLoginDialog() {
            val builder = AlertDialog.Builder(requireContext())
            builder.setTitle("Login to RetroAchievements")

            // Create layout for dialog
            val dialogLayout =
                android.widget.LinearLayout(requireContext()).apply {
                    orientation = android.widget.LinearLayout.VERTICAL
                    setPadding(50, 40, 50, 10)
                }

            // Username field
            val usernameField =
                android.widget.EditText(requireContext()).apply {
                    hint = "Username"
                    inputType = android.text.InputType.TYPE_CLASS_TEXT
                }
            dialogLayout.addView(usernameField)

            // Password field
            val passwordField =
                android.widget.EditText(requireContext()).apply {
                    hint = "Password"
                    inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
                }
            dialogLayout.addView(passwordField)

            builder.setView(dialogLayout)

            builder.setPositiveButton("Login") { _, _ ->
                val username = usernameField.text.toString().trim()
                val password = passwordField.text.toString().trim()

                if (username.isEmpty() || password.isEmpty()) {
                    Toast.makeText(requireContext(), "Please enter username and password", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }

                performRetroAchievementsLogin(username, password)
            }

            builder.setNegativeButton("Cancel", null)
            builder.show()
        }

        /**
         * Perform RetroAchievements login with credentials
         */
        private fun performRetroAchievementsLogin(
            username: String,
            password: String,
        ) {
            Toast.makeText(requireContext(), "Logging in...", Toast.LENGTH_SHORT).show()

            retroAchievementsAuthManager.onLoginResult = { success, error ->
                requireActivity().runOnUiThread {
                    if (success) {
                        Toast.makeText(requireContext(), "Login successful!", Toast.LENGTH_SHORT).show()
                    } else {
                        Toast.makeText(requireContext(), "Login failed: ${error ?: "Unknown error"}", Toast.LENGTH_LONG).show()
                    }
                    updateRetroAchievementsPreferencesUI()
                }
            }

            // Always use auto-login (remember credentials)
            retroAchievementsAuthManager.loginRetroAchievements(username, password, true)
        }

        /**
         * Perform RetroAchievements logout
         */
        private fun performRetroAchievementsLogout() {
            retroAchievementsAuthManager.logoutRetroAchievements()
            Toast.makeText(requireContext(), "Logged out from RetroAchievements", Toast.LENGTH_SHORT).show()
            updateRetroAchievementsPreferencesUI()
        }

        /**
         * Update RetroAchievements preferences UI based on current authentication state
         */
        private fun updateRetroAchievementsPreferencesUI() {
            lifecycleScope.launch {
                val authStatus = retroAchievementsAuthManager.getAuthenticationStatus()

                // Update status preference
                val statusPref = findPreference<Preference>("pref_retroachievements_status")
                statusPref?.summary =
                    if (authStatus.isRetroAchievementsLoggedIn) {
                        getString(R.string.retroachievements_logged_in_as, authStatus.retroAchievementsUsername)
                    } else {
                        getString(R.string.retroachievements_not_connected)
                    }

                // Show/hide login/logout buttons based on authentication state
                val loginPref = findPreference<Preference>("pref_retroachievements_login")
                val logoutPref = findPreference<Preference>("pref_retroachievements_logout")

                if (authStatus.isRetroAchievementsLoggedIn) {
                    // Logged in: hide login button, show logout button
                    loginPref?.isVisible = false
                    logoutPref?.isVisible = true
                } else {
                    // Not logged in: show login button, hide logout button
                    loginPref?.isVisible = true
                    logoutPref?.isVisible = false
                }
            }
        }
    }

    /**
     * Setup edge-to-edge window insets handling for Android 15+ (API 35+)
     */
    private fun setupEdgeToEdgeInsets() {
        val rootView = findViewById<android.view.View>(android.R.id.content)
        ViewCompat.setOnApplyWindowInsetsListener(rootView) { view, windowInsets ->
            val insets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())

            // Apply padding to avoid status bar overlap
            view.setPadding(
                view.paddingLeft,
                insets.top,
                view.paddingRight,
                view.paddingBottom,
            )

            windowInsets
        }
    }
}
