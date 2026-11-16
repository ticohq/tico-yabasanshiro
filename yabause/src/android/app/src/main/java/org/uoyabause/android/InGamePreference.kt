@file:JvmName("GameSharedPreference")

package org.uoyabause.android

import android.app.ActivityManager
import android.content.Context
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.appcompat.app.AlertDialog
import androidx.core.content.ContextCompat
import androidx.preference.CheckBoxPreference
import androidx.preference.ListPreference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.PreferenceManager
import com.frybits.harmony.getHarmonySharedPreferences
import org.devmiyax.yabasanshiro.R

/**
 * Safe getString that handles ClassCastException when a preference value
 * was stored as a different type (e.g., Int) by an older version of the app.
 */
fun SharedPreferences.getStringSafe(key: String, defValue: String): String = try {
    getString(key, defValue) ?: defValue
} catch (e: ClassCastException) {
    try {
        getInt(key, defValue.toIntOrNull() ?: 0).toString()
    } catch (e2: ClassCastException) {
        defValue
    }
}

/**
 * Convert a legacy String-typed SharedPreferences entry to Int.
 * Used when a key changes from ListPreference/EditTextPreference (String) to
 * SeekBarPreference (Int) and existing installs still have the old String value,
 * which would otherwise throw ClassCastException during SeekBarPreference init.
 */
fun SharedPreferences.migrateStringToIntPreference(
    key: String,
    defaultInt: Int,
    min: Int = Int.MIN_VALUE,
    max: Int = Int.MAX_VALUE,
) {
    if (!contains(key)) return
    val current = all[key] ?: return
    if (current is String) {
        val parsed = current.toIntOrNull() ?: defaultInt
        val coerced = parsed.coerceIn(min, max)
        edit().remove(key).putInt(key, coerced).apply()
    }
}

/**
 * A setting that exists per game.
 *
 * Each one is seeded from the global settings the first time a game is played,
 * and mirrored between the Harmony store (written by the settings UI) and the
 * plain SharedPreferences named after the game code (read by the emulator).
 * Adding a per-game setting means adding one entry to [IN_GAME_PREFS] and
 * nothing else.
 */
sealed class InGamePref(
    val key: String,
) {
    class Str(
        key: String,
        val default: String,
    ) : InGamePref(key)

    class Bool(
        key: String,
        val default: Boolean,
    ) : InGamePref(key)
}

val IN_GAME_PREFS =
    listOf(
        InGamePref.Bool("pref_fps", false),
        InGamePref.Bool("pref_frameskip", false),
        InGamePref.Bool("pref_rotate_screen", false),
        InGamePref.Bool("pref_use_compute_shader", false),
        InGamePref.Str("pref_polygon_generation", "0"),
        InGamePref.Str("pref_frameLimit", "0"),
        InGamePref.Str("pref_resolution", "0"),
        InGamePref.Str("pref_aspect_rate", "0"),
        InGamePref.Str("pref_rbg_resolution", "0"),
        InGamePref.Str(VideoFilter.KEY, VideoFilter.NONE),
        InGamePref.Str(Vdp2Composite.KEY, Vdp2Composite.DEFAULT_VALUE),
        InGamePref.Str(VideoCore.KEY, VideoCore.OPENGL),
    )

/** Copies every per-game setting from [from] to [to]. */
fun copyInGamePreferences(
    from: SharedPreferences,
    to: SharedPreferences,
) {
    val editor = to.edit()
    for (pref in IN_GAME_PREFS) {
        when (pref) {
            is InGamePref.Bool -> editor.putBoolean(pref.key, from.getBoolean(pref.key, pref.default))
            is InGamePref.Str -> editor.putString(pref.key, from.getStringSafe(pref.key, pref.default))
        }
    }
    editor.apply()
}

/** Fills in settings a game does not have yet from the global settings. */
private fun seedFromGlobalPreferences(
    gamePreference: SharedPreferences,
    defaultPreference: SharedPreferences,
) {
    val editor = gamePreference.edit()
    for (pref in IN_GAME_PREFS) {
        if (gamePreference.contains(pref.key)) continue
        when (pref) {
            is InGamePref.Bool ->
                editor.putBoolean(pref.key, defaultPreference.getBoolean(pref.key, pref.default))
            is InGamePref.Str ->
                editor.putString(pref.key, defaultPreference.getStringSafe(pref.key, pref.default))
        }
    }
    editor.apply()
}

fun setupInGamePreferences(
    context: Context,
    gameCode: String?,
) {
    if (gameCode.isNullOrEmpty()) {
        Log.d("setupInGamePreferences", "gamecode is null or empty. can not read game setting")
        return
    }
    val key = gameCode.replace(" ", "-")
    val gamePreference = context.getHarmonySharedPreferences(key)
    val defaultPreference = PreferenceManager.getDefaultSharedPreferences(context)

    seedFromGlobalPreferences(gamePreference, defaultPreference)

    // Polygon generation is the one setting whose global value cannot be copied
    // as is: the Vulkan path only accepts GPU_TESSERATION (2) and
    // COMPUTE_RASTERIZER (3).
    if (defaultPreference.getStringSafe(VideoCore.KEY, "0") == VideoCore.VULKAN) {
        val current = gamePreference.getStringSafe("pref_polygon_generation", "0")
        if (current !in setOf("2", "3")) {
            gamePreference.edit().putString("pref_polygon_generation", "2").apply()
        }
    }

    // The emulator reads the plain SharedPreferences named after the game code.
    copyInGamePreferences(gamePreference, context.getSharedPreferences(gameCode, 0))
}

class InGamePreference :
    PreferenceFragmentCompat(),
    SharedPreferences.OnSharedPreferenceChangeListener {
    companion object {
        const val TAG = "InGamePreference"
        private const val ARG_GAMECODE = "gamecode"

        fun newInstance(gamecode: String): InGamePreference =
            InGamePreference().apply {
                arguments = Bundle().apply { putString(ARG_GAMECODE, gamecode) }
            }
    }

    private val gamecode: String
        get() = arguments?.getString(ARG_GAMECODE).orEmpty()

    private var onEndCallback: (() -> Unit)? = null
    private lateinit var activityContext: Context
    private var previousVideoValue: String? = null
    private var needsRestart = false

    private fun showSummary(listPreference: ListPreference) {
        listPreference.summary = listPreference.entry
        listPreference.setOnPreferenceChangeListener { preference, newValue ->
            if (preference is ListPreference) {
                val index = preference.findIndexOfValue(newValue.toString())
                val entry = preference.entries.get(index)
                preference.summary = entry
            }
            true
        }
    }

    private fun setSummaries() {
        showSummary(findPreference<ListPreference?>("pref_video")!!)
        showSummary(findPreference<ListPreference?>("pref_polygon_generation")!!)
        showSummary(findPreference<ListPreference?>("pref_resolution")!!)
        showSummary(findPreference<ListPreference?>("pref_rbg_resolution")!!)
        showSummary(findPreference<ListPreference?>("pref_aspect_rate")!!)
        showSummary(findPreference<ListPreference?>("pref_frameLimit")!!)
        showSummary(findPreference<ListPreference?>(Vdp2Composite.KEY)!!)
    }

    override fun onAttach(context: Context) {
        this.activityContext = context
        super.onAttach(context)
    }

    override fun onCreatePreferences(
        savedInstanceState: Bundle?,
        rootKey: String?,
    ) {
        setupInGamePreferences(activityContext, gamecode)
        if (gamecode.isNotEmpty()) {
            preferenceManager.sharedPreferencesName = gamecode
        }
        setPreferencesFromResource(R.xml.in_game_preferences, rootKey)

        val defaultPreference = PreferenceManager.getDefaultSharedPreferences(activityContext)
        val res = activityContext.resources

        // Setup video core preference options
        val videoPref = findPreference<ListPreference?>("pref_video")
        if (videoPref != null) {
            val videoLabels: MutableList<CharSequence> = ArrayList()
            val videoValues: MutableList<CharSequence> = ArrayList()

            val activityManager = activityContext.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val configurationInfo = activityManager.deviceConfigurationInfo
            val supportsEs3 = configurationInfo.reqGlEsVersion >= 0x30000

            if (supportsEs3) {
                videoLabels.add(res.getString(R.string.opengl_video_interface))
                videoValues.add(VideoCore.OPENGL)
            }

            // The software renderer is no longer supported and is not offered.
            val vulkanSupported =
                activityContext.packageManager.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)
            if (vulkanSupported) {
                videoLabels.add(res.getString(R.string.vulkan_video_interface))
                videoValues.add(VideoCore.VULKAN)
            }

            videoPref.entries = videoLabels.toTypedArray()
            videoPref.entryValues = videoValues.toTypedArray()
            val resolvedCore = VideoCore.resolve(videoPref.value, vulkanSupported)
            if (resolvedCore != VideoCore.UNSET && resolvedCore != videoPref.value) {
                videoPref.value = resolvedCore
            }
        }

        val currentVideoValue = defaultPreference.getStringSafe("pref_video", "0")

        // Store the initial video value
        previousVideoValue = preferenceScreen.sharedPreferences?.getStringSafe("pref_video", currentVideoValue)

        // Update polygon generation and compute shader settings based on current video core
        updateVideoRelatedSettings(previousVideoValue)

        setSummaries()

        // Installed AFTER setSummaries(), because showSummary() inside setSummaries()
        // overwrites OnPreferenceChangeListener on each ListPreference. This listener
        // subsumes showSummary's summary-update behavior.
        findPreference<ListPreference?>("pref_polygon_generation")?.setOnPreferenceChangeListener { preference, newValue ->
            if (newValue == "3") {
                // VDP1 Compute Rasterizer is experimental: show the report-bugs notice once.
                ExperimentalFeatureNotice.maybeShow(
                    requireContext(),
                    ExperimentalFeatureNotice.KEY_VDP1_COMPUTE_RASTERIZER,
                )
            }
            if (preference is ListPreference) {
                val index = preference.findIndexOfValue(newValue.toString())
                if (index >= 0) preference.summary = preference.entries[index]
            }
            true
        }

        // Installed after setSummaries() for the same reason; the summary of the
        // filter list follows the choice (applyFilterChoices() sets the first one).
        findPreference<ListPreference?>(VideoFilter.KEY)?.setOnPreferenceChangeListener { preference, newValue ->
            if (preference is ListPreference) {
                val index = preference.findIndexOfValue(newValue.toString())
                if (index >= 0) preference.summary = preference.entries[index]
            }
            true
        }

        this.preferenceScreen.sharedPreferences!!.registerOnSharedPreferenceChangeListener(this)
    }

    override fun onPause() {
        super.onPause()
        this.preferenceScreen.sharedPreferences!!.unregisterOnSharedPreferenceChangeListener(this)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View {
        val view = super.onCreateView(inflater, container, savedInstanceState)
        view.setBackgroundColor(ContextCompat.getColor(activityContext, R.color.default_background))
        return view
    }

    fun setOnEndCallback(callback: () -> Unit) {
        this.onEndCallback = callback
    }

    fun onBackPressed() {
        onEndCallback?.invoke()
    }

    override fun onSharedPreferenceChanged(
        sharedPreferences: SharedPreferences?,
        key: String?,
    ) {
        if (sharedPreferences == null) {
            return
        }

        // Check if pref_video changed
        if (key == "pref_video") {
            val newVideoValue = sharedPreferences.getStringSafe("pref_video", "1")

            // Update polygon generation and compute shader settings based on video core
            updateVideoRelatedSettings(newVideoValue)

            if (newVideoValue != previousVideoValue) {
                // Video setting changed, show restart confirmation dialog
                showRestartConfirmationDialog(sharedPreferences)
                return
            }
        }

        val prefKey = gamecode.replace(" ", "-")
        val gamePreference = requireContext().getHarmonySharedPreferences(prefKey)

        copyInGamePreferences(sharedPreferences, gamePreference)
    }

    private fun updateVideoRelatedSettings(videoValue: String?) {
        val polygonPref = findPreference<ListPreference?>("pref_polygon_generation")
        val computeShaderPref = findPreference<CheckBoxPreference?>("pref_use_compute_shader")
        val compositePref = findPreference<ListPreference?>(Vdp2Composite.KEY)

        applyFilterChoices(videoValue)

        // Only the Vulkan renderer reads VDP_SETTING_VDP2_NEW_COMPOSITE, so the
        // item stays greyed out on the other cores.
        compositePref?.isEnabled =
            Vdp2Composite.isSelectable(videoValue?.toIntOrNull() ?: -1)

        when (videoValue) {
            "1" -> {
                // OpenGL: Triangles (0) and GPU_TESSERATION (2) only.
                // COMPUTE_RASTERIZER (3) is Vulkan-only and must not appear here.
                polygonPref?.isEnabled = true
                polygonPref?.entries =
                    arrayOf(
                        resources.getString(R.string.poly_triangle),
                        resources.getString(R.string.poly_gpu_tess),
                    )
                polygonPref?.entryValues = arrayOf("0", "2")
                if (polygonPref?.value !in setOf("0", "2")) {
                    polygonPref?.value = "0"
                }
                computeShaderPref?.isEnabled = true
            }
            "4" -> {
                // Vulkan: GPU_TESSERATION (2) and COMPUTE_RASTERIZER (3) are
                // selectable. The VDP2 compute shader stays forced on.
                polygonPref?.isEnabled = true
                polygonPref?.entries =
                    arrayOf(
                        resources.getString(R.string.poly_gpu_tess),
                        resources.getString(R.string.poly_compute_rasterizer),
                    )
                polygonPref?.entryValues = arrayOf("2", "3")
                if (polygonPref?.value !in setOf("2", "3")) {
                    polygonPref?.value = "2"
                }
                computeShaderPref?.isEnabled = false
                computeShaderPref?.isChecked = true
            }
            else -> {
                // Software or other: Disable both. Keep full entry set so the
                // user can switch to OpenGL/Vulkan later without losing options.
                polygonPref?.isEnabled = false
                polygonPref?.entries = resources.getStringArray(R.array.entries_polygon_generation_type)
                polygonPref?.entryValues = resources.getStringArray(R.array.entryvalues_polygon_generation_type)
                computeShaderPref?.isEnabled = false
                computeShaderPref?.isChecked = false
            }
        }
    }

    /**
     * Offers the filters the video core implements (see [VideoFilter]) and shows
     * the stored one, or None when this core does not offer it. The stored value
     * is not rewritten.
     */
    private fun applyFilterChoices(videoCore: String?) {
        val pref = findPreference<ListPreference?>(VideoFilter.KEY) ?: return
        val labels = resources.getStringArray(R.array.entries_video_filte_list_preference)
        val values = resources.getStringArray(R.array.entryvalues_video_filte_list_preference).toList()
        val offered = VideoFilter.offeredValues(videoCore, values)
        pref.entries = offered.map { labels[values.indexOf(it)] }.toTypedArray()
        pref.entryValues = offered.toTypedArray()
        pref.isEnabled = VideoFilter.isSelectable(videoCore)
        val shown = VideoFilter.displayValue(pref.value, offered)
        pref.summary = labels[values.indexOf(shown)]
    }

    private fun showRestartConfirmationDialog(sharedPreferences: SharedPreferences) {
        val newVideoValue = sharedPreferences.getStringSafe("pref_video", "1")

        AlertDialog
            .Builder(requireContext())
            .setTitle(R.string.restart_required_title)
            .setMessage(R.string.restart_required_message)
            .setPositiveButton(R.string.yes) { _, _ ->
                // Save the setting and restart
                saveVideoSetting(sharedPreferences)
                needsRestart = true
                restartEmulation()
            }.setNegativeButton(R.string.no) { _, _ ->
                // Just save the setting without restarting
                saveVideoSetting(sharedPreferences)
                // Revert the preference UI back to previous value to avoid confusion
                val videoPref = findPreference<ListPreference?>("pref_video")
                videoPref?.value = previousVideoValue
                // Also revert the related settings UI
                updateVideoRelatedSettings(previousVideoValue)
            }.setCancelable(false)
            .show()
    }

    private fun saveVideoSetting(sharedPreferences: SharedPreferences) {
        val prefKey = gamecode.replace(" ", "-")
        val gamePreference = requireContext().getHarmonySharedPreferences(prefKey)

        copyInGamePreferences(sharedPreferences, gamePreference)

        // Update previous value
        previousVideoValue = sharedPreferences.getStringSafe("pref_video", "1")
    }

    private fun restartEmulation() {
        // Trigger activity restart
        requireActivity().recreate()
    }
}
