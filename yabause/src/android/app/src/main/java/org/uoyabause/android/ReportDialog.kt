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

import android.app.Dialog
import android.content.Context
import android.content.DialogInterface
import android.content.Intent
import android.os.Bundle
import android.text.Html
import android.text.method.LinkMovementMethod
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.RatingBar
import android.widget.RatingBar.OnRatingBarChangeListener
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.widget.NestedScrollView
import androidx.lifecycle.lifecycleScope
import com.firebase.ui.auth.AuthUI
import com.google.android.material.bottomsheet.BottomSheetBehavior
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.bottomsheet.BottomSheetDialogFragment
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.firebase.Timestamp
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.FieldValue
import com.google.firebase.firestore.FirebaseFirestore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.withContext
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.auth.AuthState
import org.uoyabause.android.auth.ui.PcSignInActivity
import java.io.File

class ReportDialog(
    private val activity: Context,
    val productionNumber: String,
) : BottomSheetDialogFragment() {
    private var emulationRating: RatingBar? = null
    private var gameRating: RatingBar? = null
    private var rateText: TextView? = null
    private var gameRateText: TextView? = null
    private var editText: EditText? = null
    private var checkbox: CheckBox? = null
    private var sendButton: ImageButton? = null

    // Attachment UI elements
    private var screenshotSwitch: androidx.appcompat.widget.SwitchCompat? = null
    private var reproductionDataSwitch: androidx.appcompat.widget.SwitchCompat? = null
    private var attachmentSizeText: TextView? = null
    private var screenshotPreview: android.widget.ImageView? = null
    private var screenshotPreviewCard: androidx.cardview.widget.CardView? = null

    // Attachment manager
    private lateinit var attachmentManager: ReportAttachmentManager

    // Cached screenshot file for preview
    private var previewScreenshotFile: java.io.File? = null

    // Launcher for the Firebase AuthUI sign-in flow when the user chooses "Sign in & submit".
    private val signInLauncher: ActivityResultLauncher<Intent> =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { _ ->
            // Firebase AuthUI can return RESULT_CANCELED even on a successful sign-in
            // (see Issue #78), so detect success by the actual auth state, not resultCode.
            if (AuthState.isSignedIn()) {
                handleSendClick()
            }
            // On cancel/failure (still signed out): keep the report dialog open with input intact.
        }

    interface OnReportFinishedListener {
        fun onFinishReport(
            rating: Int,
            message: String?,
            screenshot: Boolean,
        )
    }

    interface OnDialogDismissListener {
        fun onDialogDismissed()
    }

    private var onReportFinishedListener: OnReportFinishedListener? = null
    private var onDialogDismissListener: OnDialogDismissListener? = null

    fun setOnReportFinishedListener(listener: (rating: Int, message: String?, screenshot: Boolean) -> Unit) {
        this.onReportFinishedListener =
            object : OnReportFinishedListener {
                override fun onFinishReport(
                    rating: Int,
                    message: String?,
                    screenshot: Boolean,
                ) {
                    listener(rating, message, screenshot)
                }
            }
    }

    fun setOnDialogDismissListener(listener: () -> Unit) {
        this.onDialogDismissListener =
            object : OnDialogDismissListener {
                override fun onDialogDismissed() {
                    listener()
                }
            }
    }

    /**
     * Sets a pre-captured screenshot file to be used for preview
     * This should be called before showing the dialog to ensure screenshot is available
     */
    fun setPreCapturedScreenshot(screenshotFile: File?) {
        this.previewScreenshotFile = screenshotFile
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View? {
        val view = inflater.inflate(R.layout.report, container, false)

        // Initialize attachment manager
        attachmentManager = ReportAttachmentManager(activity)

        // Note: Screenshot should be pre-captured before dialog is shown (see setPreCapturedScreenshot)

        val reviewNoticeTextView = view.findViewById<TextView>(R.id.review_notice)
        reviewNoticeTextView.text = Html.fromHtml(getString(R.string.report_notice), Html.FROM_HTML_MODE_LEGACY)
        reviewNoticeTextView.movementMethod = LinkMovementMethod.getInstance()

        gameRating = view.findViewById(R.id.game_ratingBar)
        gameRating?.apply {
            numStars = 5
            rating = 3.0f
            stepSize = 1.0f
            onRatingBarChangeListener =
                OnRatingBarChangeListener { ratingBar, rating, _ ->
                    val iRate = rating.toInt()
                    if (rating == 0f) {
                        ratingBar.rating = 1f
                    }
                    when (iRate) {
                        1 -> gameRateText?.setText(R.string.game_report_message_1)
                        2 -> gameRateText?.setText(R.string.game_report_message_2)
                        3 -> gameRateText?.setText(R.string.game_report_message_3)
                        4 -> gameRateText?.setText(R.string.game_report_message_4)
                        5 -> gameRateText?.setText(R.string.game_report_message_5)
                        else -> {}
                    }
                }
        }

        emulationRating = view.findViewById(R.id.emulation_ratingBar)
        emulationRating?.apply {
            numStars = 5
            rating = 3.0f
            stepSize = 1.0f
            onRatingBarChangeListener =
                OnRatingBarChangeListener { ratingBar, rating, _ ->
                    val iRate = rating.toInt()
                    if (rating == 0f) {
                        ratingBar.rating = 1f
                    }
                    when (iRate) {
                        1 -> rateText?.setText(R.string.report_message_1)
                        2 -> rateText?.setText(R.string.report_message_2)
                        3 -> rateText?.setText(R.string.report_message_3)
                        4 -> rateText?.setText(R.string.report_message_4)
                        5 -> rateText?.setText(R.string.report_message_5)
                        else -> {}
                    }
                }
        }

        editText = view.findViewById(R.id.report_message)

        // Add focus listener to scroll EditText into view when keyboard appears
        editText?.setOnFocusChangeListener { v, hasFocus ->
            if (hasFocus) {
                // Post with delay to ensure keyboard is visible and layout is complete
                v.postDelayed({
                    // Request the view to be scrolled into the visible area
                    v.requestRectangleOnScreen(
                        android.graphics.Rect(0, 0, v.width, v.height),
                        false, // not immediate, use smooth scrolling
                    )

                    // Alternative: Find parent NestedScrollView and scroll manually
                    var parent = v.parent
                    while (parent != null) {
                        if (parent is androidx.core.widget.NestedScrollView) {
                            parent.post {
                                // Calculate position to scroll to, leaving space at bottom
                                val scrollY = v.bottom + v.height
                                (parent as NestedScrollView).smoothScrollTo(0, scrollY.coerceAtLeast(0))
                            }
                            break
                        }
                        parent = parent.parent as? android.view.ViewParent
                    }
                }, 300) // Delay to wait for keyboard animation
            }
        }

        // checkbox = view.findViewById(R.id.report_Screenshot)
        rateText = view.findViewById(R.id.emulation_rateString)
        rateText?.setText(R.string.report_message_3)
        gameRateText = view.findViewById(R.id.game_rateString)
        gameRateText?.setText(R.string.game_report_message_3)

        sendButton = view.findViewById(R.id.send_button)

        // Initialize attachment UI elements
        screenshotSwitch = view.findViewById(R.id.screenshot_switch)
        reproductionDataSwitch = view.findViewById(R.id.reproduction_data_switch)
        attachmentSizeText = view.findViewById(R.id.attachment_size)
        screenshotPreview = view.findViewById(R.id.screenshot_preview)
        screenshotPreviewCard = view.findViewById(R.id.screenshot_preview_card)

        // Set up switch listeners to update estimated size and preview
        screenshotSwitch?.setOnCheckedChangeListener { _, isChecked ->
            updateEstimatedSize()
            if (isChecked) {
                showScreenshotPreview()
            } else {
                hideScreenshotPreview()
            }
        }

        reproductionDataSwitch?.setOnCheckedChangeListener { _, _ ->
            updateEstimatedSize()
        }

        // onRatingChanged(emulationRating!!, 3.0f, false)

        sendButton?.setOnClickListener {
            handleSendClick()
        }

        // Handle keyboard insets for proper resizing when soft keyboard appears
        dialog?.window?.let { window ->
            WindowCompat.setDecorFitsSystemWindows(window, false)
            ViewCompat.setOnApplyWindowInsetsListener(view) { v, insets ->
                val imeInsets = insets.getInsets(WindowInsetsCompat.Type.ime())
                v.setPadding(v.paddingLeft, v.paddingTop, v.paddingRight, imeInsets.bottom)
                insets
            }
        }

        return view
    }

    /**
     * Updates the estimated attachment size display
     */
    private fun updateEstimatedSize() {
        val includeScreenshot = screenshotSwitch?.isChecked ?: false
        val includeReproductionData = reproductionDataSwitch?.isChecked ?: false
        // When reproduction data is enabled, both state and memory are included
        val includeState = includeReproductionData
        val includeMemory = includeReproductionData

        if (includeScreenshot || includeReproductionData) {
            val estimatedSize = attachmentManager.estimateAttachmentSize(includeScreenshot, includeState, includeMemory)
            val formattedSize = attachmentManager.formatFileSize(estimatedSize)
            attachmentSizeText?.text = "Estimated size: $formattedSize"
            attachmentSizeText?.visibility = View.VISIBLE
        } else {
            attachmentSizeText?.visibility = View.GONE
        }
    }

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val dialog = super.onCreateDialog(savedInstanceState) as BottomSheetDialog
        dialog.setOnShowListener { dialogInterface ->
            val bottomSheetDialog = dialogInterface as BottomSheetDialog
            val bottomSheet =
                bottomSheetDialog.findViewById<View>(
                    com.google.android.material.R.id.design_bottom_sheet,
                )
            bottomSheet?.let {
                val behavior = BottomSheetBehavior.from(it)
                behavior.skipCollapsed = true
                behavior.state = BottomSheetBehavior.STATE_EXPANDED
            }
        }
        return dialog
    }

    /**
     * Checks if the current user is an admin
     */
    private suspend fun checkIsAdmin(userId: String): Boolean = try {
        val db = FirebaseFirestore.getInstance()
        val adminDoc =
            withContext(Dispatchers.IO) {
                db
                    .collection("admins")
                    .document(userId)
                    .get()
                    .await()
            }
        adminDoc.exists()
    } catch (e: Exception) {
        Log.e("ReportDialog", "Error checking admin status", e)
        false
    }

    /**
     * Creates a progress dialog using AlertDialog with custom layout
     */
    private fun createProgressDialog(message: String): Pair<AlertDialog, TextView> {
        val dialogView =
            LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(48, 32, 48, 32)

                addView(
                    ProgressBar(activity).apply {
                        isIndeterminate = true
                    },
                )

                addView(
                    TextView(activity).apply {
                        text = message
                        setPadding(32, 0, 0, 0)
                        layoutParams =
                            LinearLayout
                                .LayoutParams(
                                    LinearLayout.LayoutParams.WRAP_CONTENT,
                                    LinearLayout.LayoutParams.WRAP_CONTENT,
                                ).apply {
                                    gravity = android.view.Gravity.CENTER_VERTICAL
                                }
                    },
                )
            }

        val messageTextView = dialogView.getChildAt(1) as TextView

        val dialog =
            MaterialAlertDialogBuilder(activity)
                .setView(dialogView)
                .setCancelable(false)
                .create()

        return Pair(dialog, messageTextView)
    }

    private fun handleSendClick() {
        val currentUser = AuthState.reportUser()
        if (currentUser == null) {
            // No Firebase session yet. If the user already opted into anonymous
            // reporting, go straight to an ephemeral anonymous submit; otherwise ask.
            if (hasAnonymousOptIn()) {
                signInAnonymouslyThenSubmit()
            } else {
                showAuthChoiceDialog()
            }
            return
        }
        val isAnonymousReport = currentUser.isAnonymous

        val emulationRating = emulationRating!!.rating.toInt()
        val gameRating = gameRating!!.rating.toInt()
        val message = editText!!.text.toString()
        val includeScreenshot = screenshotSwitch?.isChecked ?: false
        val includeReproductionData = reproductionDataSwitch?.isChecked ?: false
        // When reproduction data is enabled, both state and memory are included
        val includeState = includeReproductionData
        val includeMemory = includeReproductionData

        // Disable send button to prevent double submission
        sendButton?.isEnabled = false

        // Show progress dialog using AlertDialog
        val (progressDialog, progressMessageText) = createProgressDialog("Submitting report...")
        progressDialog.show()

        // Initialize Firestore
        val db = FirebaseFirestore.getInstance()
        val userId = currentUser.uid

        // Launch coroutine for background processing
        lifecycleScope.launch {
            try {
                // Get shared preferences for rate limiting
                val sharedPreferences = activity.getSharedPreferences("report_prefs", Context.MODE_PRIVATE)

                // Check if user is admin
                val isAdmin = checkIsAdmin(userId)

                // Check rate limit only for non-admin users
                if (!isAdmin) {
                    val lastReportTime = sharedPreferences.getLong("last_report_time", 0L)
                    val currentTime = System.currentTimeMillis()
                    val cooldownPeriod = 10 * 60 * 1000L // 10 minutes in milliseconds

                    if (currentTime - lastReportTime < cooldownPeriod) {
                        val remainingMinutes = ((cooldownPeriod - (currentTime - lastReportTime)) / 60000).toInt() + 1
                        withContext(Dispatchers.Main) {
                            progressDialog.dismiss()
                            sendButton?.isEnabled = true
                            Toast
                                .makeText(
                                    activity,
                                    "Please wait $remainingMinutes more minute(s) before submitting another report",
                                    Toast.LENGTH_LONG,
                                ).show()
                        }
                        return@launch
                    }
                } else {
                    Log.d("ReportDialog", "Admin user detected, skipping rate limit")
                }

                // Collect device information for the bug report
                val deviceModel = DeviceInfo.model()
                val osVersion = DeviceInfo.osVersion()
                val gpuRenderer = withContext(Dispatchers.IO) { DeviceInfo.gpuRenderer() }

                // Create rating document data
                val ratingData =
                    hashMapOf(
                        "rating" to gameRating,
                        "emulation_rating" to emulationRating,
                        "comment" to message,
                        "uid" to userId,
                        "display_name" to if (isAnonymousReport) {
                            getString(R.string.report_auth_anonymous_display_name)
                        } else {
                            currentUser.displayName
                        },
                        "photo_url" to if (isAnonymousReport) null else currentUser.photoUrl?.toString(),
                        "anonymous" to isAnonymousReport,
                        "platform" to "android",
                        "version" to YabauseApplication.getVersionName(),
                        "version_code" to YabauseApplication.getVersionCode(),
                        "device_model" to deviceModel,
                        "os_version" to osVersion,
                        "gpu_renderer" to gpuRenderer,
                        "timestamp" to FieldValue.serverTimestamp(),
                        "isVisible" to true,
                        "has_attachments" to (includeScreenshot || includeState || includeMemory),
                        "product_number" to productionNumber,
                        "notifyAdmins" to true, // Trigger Cloud Functions for admin notification
                    )

                // Collect current preferences for reproduction
                val reproduceManager = ReportReproduceManager(activity)
                val currentPreferences = reproduceManager.collectCurrentPreferences(productionNumber)
                if (currentPreferences.isNotEmpty()) {
                    ratingData["preferences"] = currentPreferences
                    Log.d("ReportDialog", "Collected ${currentPreferences.size} preferences for reproduction (gameCode: $productionNumber)")
                }

                var attachmentResult: ReportAttachmentManager.AttachmentResult? = null

                // Create and upload attachments if requested
                if (includeScreenshot || includeState || includeMemory) {
                    // Validate before creating attachments
                    val (canCreate, errorMessage) = attachmentManager.validateAttachmentCreation()
                    if (!canCreate) {
                        withContext(Dispatchers.Main) {
                            progressDialog.dismiss()
                            sendButton?.isEnabled = true
                            Toast.makeText(activity, errorMessage, Toast.LENGTH_LONG).show()
                        }
                        return@launch
                    }

                    // Update progress message
                    withContext(Dispatchers.Main) {
                        progressMessageText.text = "Creating attachments..."
                    }

                    // Create attachments in background
                    attachmentResult =
                        withContext(Dispatchers.IO) {
                            // Reuse preview screenshot if available
                            val screenshot =
                                if (includeScreenshot && previewScreenshotFile != null && previewScreenshotFile!!.exists()) {
                                    previewScreenshotFile
                                } else if (includeScreenshot) {
                                    attachmentManager.captureScreenshot()
                                } else {
                                    null
                                }

                            // Create save state if needed
                            val saveState =
                                if (includeState) {
                                    attachmentManager.createStateSave(compressed = false)
                                } else {
                                    null
                                }

                            // Copy memory file if needed
                            val memory =
                                if (includeMemory) {
                                    attachmentManager.copyMemoryFile()
                                } else {
                                    null
                                }

                            // Calculate total size
                            val totalSize = (screenshot?.length() ?: 0L) + (saveState?.length() ?: 0L) + (memory?.length() ?: 0L)

                            ReportAttachmentManager.AttachmentResult(
                                screenshotFile = screenshot,
                                stateSaveFile = saveState,
                                memoryFile = memory,
                                totalSize = totalSize,
                                success = true,
                            )
                        }

                    if (!attachmentResult.success) {
                        withContext(Dispatchers.Main) {
                            progressDialog.dismiss()
                            sendButton?.isEnabled = true
                            Toast.makeText(activity, attachmentResult.errorMessage, Toast.LENGTH_LONG).show()
                        }
                        return@launch
                    }

                    // Upload attachments to Firebase Storage
                    withContext(Dispatchers.Main) {
                        progressMessageText.text = "Uploading attachments..."
                    }

                    val uploadResult =
                        withContext(Dispatchers.IO) {
                            attachmentManager.uploadAttachments(
                                screenshotFile = attachmentResult.screenshotFile,
                                stateSaveFile = attachmentResult.stateSaveFile,
                                memoryFile = attachmentResult.memoryFile,
                                progressListener =
                                    object : ReportAttachmentManager.UploadProgressListener {
                                        override fun onProgress(
                                            bytesTransferred: Long,
                                            totalBytes: Long,
                                        ) {
                                            val progress = (bytesTransferred * 100 / totalBytes).toInt()
                                            lifecycleScope.launch(Dispatchers.Main) {
                                                progressMessageText.text = "Uploading... $progress%"
                                            }
                                        }

                                        override fun onComplete() {
                                            Log.d("ReportDialog", "Upload complete")
                                        }

                                        override fun onError(error: Exception) {
                                            Log.e("ReportDialog", "Upload error", error)
                                        }
                                    },
                            )
                        }

                    if (!uploadResult.success) {
                        withContext(Dispatchers.Main) {
                            progressDialog.dismiss()
                            sendButton?.isEnabled = true
                            Toast.makeText(activity, uploadResult.errorMessage, Toast.LENGTH_LONG).show()
                        }
                        // Clean up temporary files
                        attachmentResult.screenshotFile?.let { attachmentManager.cleanupFiles(it) }
                        attachmentResult.stateSaveFile?.let { attachmentManager.cleanupFiles(it) }
                        attachmentResult.memoryFile?.let { attachmentManager.cleanupFiles(it) }
                        return@launch
                    }

                    // Add attachment URLs to rating data
                    uploadResult.screenshotUrl?.let {
                        ratingData["screenshot_url"] = it
                    }
                    uploadResult.stateSaveUrl?.let {
                        ratingData["savestate_url"] = it
                    }
                    uploadResult.memoryUrl?.let {
                        ratingData["memory_url"] = it
                    }
                    ratingData["attachment_size"] = attachmentResult.totalSize

                    // Clean up temporary files after successful upload
                    withContext(Dispatchers.IO) {
                        attachmentResult.screenshotFile?.let { attachmentManager.cleanupFiles(it) }
                        attachmentResult.stateSaveFile?.let { attachmentManager.cleanupFiles(it) }
                        attachmentResult.memoryFile?.let { attachmentManager.cleanupFiles(it) }
                    }
                }

                // Update progress message
                withContext(Dispatchers.Main) {
                    progressMessageText.text = "Saving report..."
                }

                // Submit to Firestore (continue with existing logic)
                submitToFirestore(db, ratingData, progressDialog, sharedPreferences, isAnonymousReport)
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    progressDialog.dismiss()
                    sendButton?.isEnabled = true
                    Toast.makeText(activity, "Error: ${e.message}", Toast.LENGTH_LONG).show()
                    Log.e("ReportDialog", "Error in handleSendClick", e)
                }
            }
        }
    }

    /**
     * Shown when the user taps Send without any Firebase session. Lets them sign in
     * (recommended) or continue as an anonymous guest. Anonymous reports are kept out
     * of the public star average by routing them to the anon_ratings subcollection.
     */
    private fun showAuthChoiceDialog() {
        val dialog =
            MaterialAlertDialogBuilder(activity)
                .setTitle(R.string.report_auth_choice_title)
                .setMessage(R.string.report_auth_choice_message)
                .setPositiveButton(R.string.report_auth_choice_sign_in) { _, _ ->
                    launchSignIn()
                }.setNeutralButton(R.string.report_auth_choice_anonymous) { _, _ ->
                    setAnonymousOptIn()
                    signInAnonymouslyThenSubmit()
                }.setNegativeButton(android.R.string.cancel, null)
                .create()
        // Gamepad/D-pad focus: focus the primary (sign-in) button after show.
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

    private fun launchSignIn() {
        val signInIntent =
            AuthUI
                .getInstance()
                .createSignInIntentBuilder()
                .setTheme(R.style.AppTheme)
                .setTosAndPrivacyPolicyUrls(
                    "https://www.yabasanshiro.com/terms-of-use",
                    "https://www.yabasanshiro.com/privacy",
                ).setAvailableProviders(
                    listOf(
                        AuthUI.IdpConfig.GoogleBuilder().build(),
                        AuthUI.IdpConfig.AppleBuilder().build(),
                    ),
                ).build()
        signInLauncher.launch(PcSignInActivity.wrap(requireContext(), signInIntent))
    }

    private fun signInAnonymouslyThenSubmit() {
        lifecycleScope.launch {
            try {
                FirebaseAuth.getInstance().signInAnonymously().await()
                // Now reportUser() is the anonymous user; continue the submission.
                // The ephemeral session is signed out again in onDismiss().
                handleSendClick()
            } catch (e: Exception) {
                Log.e("ReportDialog", "Anonymous sign-in failed", e)
                Toast.makeText(activity, R.string.report_auth_anonymous_failed, Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun reportPrefs() =
        activity.getSharedPreferences("report_prefs", Context.MODE_PRIVATE)

    private fun hasAnonymousOptIn(): Boolean =
        reportPrefs().getBoolean("report_anonymous_opt_in", false)

    private fun setAnonymousOptIn() {
        reportPrefs().edit().putBoolean("report_anonymous_opt_in", true).apply()
    }

    private fun submitToFirestore(
        db: FirebaseFirestore,
        ratingData: HashMap<String, Any?>,
        progressDialog: AlertDialog,
        sharedPreferences: android.content.SharedPreferences,
        isAnonymousReport: Boolean,
    ) {
        val targetCollection = ReportRouting.collectionFor(isAnonymousReport)

        // 1. Search in games collection by production_number
        db
            .collection("games")
            .whereEqualTo("product_number", productionNumber)
            .get()
            .addOnSuccessListener { documents ->
                if (documents.isEmpty) {
                    // Game not found, create a new game document first
                    val gameData =
                        hashMapOf(
                            "product_number" to productionNumber,
                            "created_at" to FieldValue.serverTimestamp(),
                        )

                    db
                        .collection("games")
                        .add(gameData)
                        .addOnSuccessListener { gameDocRef ->
                            // 3. Add rating to the ratings subcollection with auto-generated ID
                            gameDocRef
                                .collection(targetCollection)
                                .add(ratingData)
                                .addOnSuccessListener {
                                    // Report submitted successfully
                                    // Update last report time to enforce rate limit
                                    sharedPreferences
                                        .edit()
                                        .putLong("last_report_time", System.currentTimeMillis())
                                        .apply()

                                    // Send Discord notification with game ID
                                    sendDiscordNotification(ratingData, gameDocRef.id)

                                    progressDialog.dismiss()
                                    Toast.makeText(activity, R.string.report_sent_success, Toast.LENGTH_SHORT).show()
                                    onReportFinishedListener?.onFinishReport(
                                        ratingData["rating"] as Int,
                                        ratingData["comment"] as String?,
                                        ratingData["has_attachments"] as Boolean,
                                    )
                                    dismiss()
                                }.addOnFailureListener { e ->
                                    // Handle error
                                    progressDialog.dismiss()
                                    sendButton?.isEnabled = true
                                    Toast.makeText(activity, R.string.report_sent_failed, Toast.LENGTH_SHORT).show()
                                    Log.e("ReportDialog", "Error adding rating document", e)
                                }
                        }.addOnFailureListener { e ->
                            // Handle error
                            progressDialog.dismiss()
                            sendButton?.isEnabled = true
                            Toast.makeText(activity, R.string.report_sent_failed, Toast.LENGTH_SHORT).show()
                            Log.e("ReportDialog", "Error creating game document", e)
                        }
                } else {
                    // Game exists, add rating to its ratings subcollection with auto-generated ID
                    val gameDoc = documents.documents[0]

                    // Extract game title from the document if available
                    val gameTitle = gameDoc.getString("game_title") ?: gameDoc.getString("title")
                    if (gameTitle != null) {
                        ratingData["game_title"] = gameTitle
                    }

                    gameDoc.reference
                        .collection(targetCollection)
                        .add(ratingData)
                        .addOnSuccessListener {
                            // Report submitted successfully
                            // Update last report time to enforce rate limit
                            sharedPreferences
                                .edit()
                                .putLong("last_report_time", System.currentTimeMillis())
                                .apply()

                            // Send Discord notification with game ID
                            sendDiscordNotification(ratingData, gameDoc.id)

                            progressDialog.dismiss()
                            Toast.makeText(activity, R.string.report_sent_success, Toast.LENGTH_SHORT).show()
                            onReportFinishedListener?.onFinishReport(
                                ratingData["rating"] as Int,
                                ratingData["comment"] as String?,
                                ratingData["has_attachments"] as Boolean,
                            )
                            dismiss()
                        }.addOnFailureListener { e ->
                            // Handle error
                            progressDialog.dismiss()
                            sendButton?.isEnabled = true
                            Toast.makeText(activity, R.string.report_sent_failed, Toast.LENGTH_SHORT).show()
                            Log.e("ReportDialog", "Error adding rating document", e)
                        }
                }
            }.addOnFailureListener { e ->
                // Handle error
                progressDialog.dismiss()
                sendButton?.isEnabled = true
                Toast.makeText(activity, R.string.report_sent_failed, Toast.LENGTH_SHORT).show()
                Log.e("ReportDialog", "Error querying games collection", e)
            }
    }

    /**
     * Shows screenshot preview by capturing a screenshot
     */
    private fun showScreenshotPreview() {
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                // Use pre-captured screenshot if available, otherwise capture new one
                val screenshotFile =
                    if (previewScreenshotFile != null && previewScreenshotFile!!.exists()) {
                        previewScreenshotFile
                    } else {
                        attachmentManager.captureScreenshot()
                    }

                if (screenshotFile != null && screenshotFile.exists()) {
                    if (previewScreenshotFile == null) {
                        previewScreenshotFile = screenshotFile
                    }

                    // Load and scale down bitmap for preview
                    val options =
                        android.graphics.BitmapFactory.Options().apply {
                            inJustDecodeBounds = true
                        }
                    android.graphics.BitmapFactory.decodeFile(screenshotFile.absolutePath, options)

                    // Calculate sample size to reduce memory usage
                    val targetWidth = 800
                    options.inSampleSize = calculateInSampleSize(options, targetWidth, targetWidth * 9 / 16)
                    options.inJustDecodeBounds = false

                    val bitmap = android.graphics.BitmapFactory.decodeFile(screenshotFile.absolutePath, options)

                    withContext(Dispatchers.Main) {
                        screenshotPreview?.setImageBitmap(bitmap)
                        screenshotPreviewCard?.visibility = View.VISIBLE
                    }
                } else {
                    withContext(Dispatchers.Main) {
                        Toast.makeText(activity, "Failed to capture screenshot preview", Toast.LENGTH_SHORT).show()
                        screenshotSwitch?.isChecked = false
                    }
                }
            } catch (e: Exception) {
                Log.e("ReportDialog", "Error showing screenshot preview", e)
                withContext(Dispatchers.Main) {
                    Toast.makeText(activity, "Error: ${e.message}", Toast.LENGTH_SHORT).show()
                    screenshotSwitch?.isChecked = false
                }
            }
        }
    }

    /**
     * Hides screenshot preview and cleans up preview file
     */
    private fun hideScreenshotPreview() {
        screenshotPreviewCard?.visibility = View.GONE
        screenshotPreview?.setImageBitmap(null)

        // Clean up preview file
        previewScreenshotFile?.let {
            if (it.exists()) {
                try {
                    it.delete()
                    Log.d("ReportDialog", "Cleaned up preview screenshot: ${it.absolutePath}")
                } catch (e: Exception) {
                    Log.e("ReportDialog", "Failed to delete preview screenshot", e)
                }
            }
        }
        previewScreenshotFile = null
    }

    /**
     * Calculate an inSampleSize for BitmapFactory.Options
     */
    private fun calculateInSampleSize(
        options: android.graphics.BitmapFactory.Options,
        reqWidth: Int,
        reqHeight: Int,
    ): Int {
        val height = options.outHeight
        val width = options.outWidth
        var inSampleSize = 1

        if (height > reqHeight || width > reqWidth) {
            val halfHeight = height / 2
            val halfWidth = width / 2

            while ((halfHeight / inSampleSize) >= reqHeight && (halfWidth / inSampleSize) >= reqWidth) {
                inSampleSize *= 2
            }
        }

        return inSampleSize
    }

    override fun onDismiss(dialog: DialogInterface) {
        // Ephemeral anonymous report session: end it as soon as the dialog closes,
        // so the rest of the app never observes an anonymous user.
        FirebaseAuth.getInstance().currentUser?.let { user ->
            if (user.isAnonymous) FirebaseAuth.getInstance().signOut()
        }
        super.onDismiss(dialog)
        // Notify listener when dialog is dismissed (for any reason)
        onDialogDismissListener?.onDialogDismissed()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        // Clean up preview screenshot when dialog is closed
        hideScreenshotPreview()
    }

    /**
     * Send Discord notification for the submitted report
     */
    @Suppress("UNCHECKED_CAST")
    private fun sendDiscordNotification(
        ratingData: HashMap<String, Any?>,
        gameId: String,
    ) {
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                // Create ReportData object from ratingData
                val productNumber = ratingData["product_number"] as? String ?: productionNumber
                val reportData =
                    ReportData(
                        id = "", // Not needed for notification
                        uid = ratingData["uid"] as? String ?: "",
                        display_name = ratingData["display_name"] as? String,
                        photo_url = ratingData["photo_url"] as? String,
                        rating = ratingData["rating"] as? Int ?: 0,
                        emulation_rating = ratingData["emulation_rating"] as? Int ?: 0,
                        comment = ratingData["comment"] as? String ?: "",
                        platform = ratingData["platform"] as? String ?: "",
                        version = ratingData["version"] as? String ?: "",
                        version_code = ratingData["version_code"] as? Int ?: 0,
                        timestamp = Timestamp.now(), // Use current time
                        isVisible = ratingData["isVisible"] as? Boolean ?: true,
                        has_attachments = ratingData["has_attachments"] as? Boolean ?: false,
                        screenshot_url = ratingData["screenshot_url"] as? String,
                        savestate_url = ratingData["savestate_url"] as? String,
                        memory_url = ratingData["memory_url"] as? String,
                        attachment_size = ratingData["attachment_size"] as? Long ?: 0L,
                        preferences = ratingData["preferences"] as? Map<String, String>,
                        game_title = ratingData["game_title"] as? String,
                        product_number = productNumber,
                        game_id = gameId,
                    )

                // Send notification
                val notifier = DiscordNotifier(activity)
                val success = notifier.sendReportNotification(reportData)

                if (success) {
                    Log.d("ReportDialog", "Discord notification sent successfully")
                } else {
                    Log.w("ReportDialog", "Failed to send Discord notification")
                }
            } catch (e: Exception) {
                Log.e("ReportDialog", "Error sending Discord notification", e)
            }
        }
    }
}
