/*
 * Copyright 2026 devMiyax
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.uoyabause.android.backup.ui

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.viewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.bumptech.glide.Glide
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.AuthUI.IdpConfig.AppleBuilder
import com.firebase.ui.auth.AuthUI.IdpConfig.GoogleBuilder
import com.google.android.material.bottomsheet.BottomSheetDialogFragment
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.YabauseApplication
import org.uoyabause.android.YabauseStorage
import org.uoyabause.android.auth.AuthState
import org.uoyabause.android.auth.ui.PcSignInActivity
import org.uoyabause.android.backup.model.LocalBackupFile
import org.uoyabause.android.backup.model.SharedBackupItem
import org.uoyabause.android.backup.viewmodel.BackupUiEvent
import org.uoyabause.android.backup.viewmodel.SharedBackupViewModel
import java.util.Arrays

/**
 * Self-contained bottom sheet that imports a shared backup identified by id,
 * launched from a backup share deep link. Previews the save, requires sign-in,
 * lets the user pick a target local backup file, soft-warns when the game ROM
 * is not in the library, then imports via the existing importBackupToFile path.
 */
class SharedBackupImportSheet : BottomSheetDialogFragment() {
    private val viewModel: SharedBackupViewModel by viewModels()
    private lateinit var signInLauncher: ActivityResultLauncher<Intent>
    private var loadedItem: SharedBackupItem? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        signInLauncher =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
                if (AuthState.isSignedIn()) {
                    loadedItem?.let { startImport(it) }
                }
            }
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View? = inflater.inflate(R.layout.bottom_sheet_shared_backup_detail, container, false)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        view.findViewById<MaterialButton>(R.id.btn_rate_detail).visibility = View.GONE
        view.findViewById<MaterialButton>(R.id.btn_delete_detail).visibility = View.GONE

        val id = arguments?.getString(ARG_BACKUP_ID).orEmpty()
        if (id.isBlank()) {
            dismiss()
            return
        }

        observe(view)
        viewModel.loadById(id)
    }

    private fun observe(view: View) {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    viewModel.deepLinkItem.collectLatest { item ->
                        if (item != null) {
                            loadedItem = item
                            bind(view, item)
                        }
                    }
                }
                launch {
                    viewModel.deepLinkNotFound.collectLatest { notFound ->
                        if (notFound) {
                            Toast
                                .makeText(
                                    requireContext(),
                                    R.string.import_shared_backup_not_found,
                                    Toast.LENGTH_LONG,
                                ).show()
                            dismiss()
                        }
                    }
                }
                launch {
                    viewModel.uiEvents.collectLatest { event ->
                        when (event) {
                            is BackupUiEvent.ShowToast ->
                                Toast.makeText(requireContext(), event.message, Toast.LENGTH_SHORT).show()
                            is BackupUiEvent.ShowConfirmation -> {
                                val dialog = MaterialAlertDialogBuilder(requireContext())
                                    .setTitle(event.title)
                                    .setMessage(event.message)
                                    .setPositiveButton(R.string.confirm) { _, _ -> event.confirmAction() }
                                    .setNegativeButton(R.string.cancel, null)
                                    .create()
                                dialog.setOnShowListener {
                                    val negative = dialog.getButton(android.content.DialogInterface.BUTTON_NEGATIVE)
                                    negative?.post {
                                        negative.isFocusable = true
                                        negative.isFocusableInTouchMode = true
                                        negative.requestFocus()
                                    }
                                }
                                dialog.show()
                            }
                            else -> {}
                        }
                    }
                }
            }
        }
    }

    private fun bind(view: View, item: SharedBackupItem) {
        val ivScreenshot = view.findViewById<ImageView>(R.id.iv_screenshot_detail)
        if (!item.screenshotUrl.isNullOrEmpty()) {
            ivScreenshot.imageTintList = null
            Glide
                .with(this)
                .load(item.screenshotUrl)
                .placeholder(R.drawable.ic_save)
                .fitCenter()
                .into(ivScreenshot)
        } else {
            ivScreenshot.setImageResource(R.drawable.ic_save)
        }
        view.findViewById<TextView>(R.id.tv_game_title_detail).text =
            item.gameTitle.ifEmpty { "Unknown Game" }
        view.findViewById<TextView>(R.id.tv_filename_detail).text = item.filename

        val tvDescription = view.findViewById<TextView>(R.id.tv_description_detail)
        if (item.description.isNotEmpty()) {
            tvDescription.text = item.description
            tvDescription.visibility = View.VISIBLE
        } else {
            tvDescription.visibility = View.GONE
        }
        val tvComment = view.findViewById<TextView>(R.id.tv_comment_detail)
        if (item.comment.isNotEmpty()) {
            tvComment.text = item.comment
            tvComment.visibility = View.VISIBLE
        } else {
            tvComment.visibility = View.GONE
        }
        view.findViewById<TextView>(R.id.tv_owner_name_detail).text = item.ownerName
        view.findViewById<TextView>(R.id.tv_rating_detail).text = item.displayRating
        view.findViewById<TextView>(R.id.tv_downloads_detail).text = item.downloadCount.toString()
        view.findViewById<TextView>(R.id.tv_shared_date_detail).text = item.displaySharedDate

        val btnImport = view.findViewById<MaterialButton>(R.id.btn_import_detail)
        btnImport.setText(R.string.import_shared_backup_title)
        btnImport.setOnClickListener { onImportClicked(item) }
        btnImport.setOnKeyListener { v, keyCode, event ->
            if (event.action == android.view.KeyEvent.ACTION_DOWN &&
                (
                    keyCode == android.view.KeyEvent.KEYCODE_BUTTON_A ||
                        keyCode == android.view.KeyEvent.KEYCODE_DPAD_CENTER ||
                        keyCode == android.view.KeyEvent.KEYCODE_ENTER
                )
            ) {
                v.performClick()
                true
            } else {
                false
            }
        }
        btnImport.post { btnImport.requestFocus() }
    }

    private fun onImportClicked(item: SharedBackupItem) {
        if (!YabauseApplication.isPro()) {
            YabauseApplication.checkDonated(requireActivity())
            return
        }
        if (!AuthState.isSignedIn()) {
            Toast.makeText(requireContext(), R.string.import_shared_backup_sign_in, Toast.LENGTH_SHORT).show()
            signInLauncher.launch(PcSignInActivity.wrap(requireContext(), buildSignInIntent()))
            return
        }
        startImport(item)
    }

    private fun startImport(item: SharedBackupItem) {
        viewLifecycleOwner.lifecycleScope.launch {
            val owned = withContext(Dispatchers.IO) {
                try {
                    val pn = item.productNumber
                    pn.isNotBlank() &&
                        YabauseStorage.dao.getAll().any { it.product_number == pn }
                } catch (e: Exception) {
                    true
                }
            }
            if (!owned && item.productNumber.isNotBlank()) {
                val dialog = MaterialAlertDialogBuilder(requireContext())
                    .setTitle(R.string.import_shared_rom_missing_title)
                    .setMessage(getString(R.string.import_shared_rom_missing_message, item.gameTitle))
                    .setPositiveButton(R.string.confirm) { _, _ -> pickTargetAndImport(item) }
                    .setNegativeButton(R.string.cancel, null)
                    .create()
                dialog.setOnShowListener {
                    val positive = dialog.getButton(android.content.DialogInterface.BUTTON_POSITIVE)
                    positive?.post {
                        positive.isFocusable = true
                        positive.isFocusableInTouchMode = true
                        positive.requestFocus()
                    }
                }
                dialog.show()
            } else {
                pickTargetAndImport(item)
            }
        }
    }

    private fun pickTargetAndImport(item: SharedBackupItem) {
        val existingFiles = LocalBackupFile.getExistingFiles()
        if (existingFiles.isEmpty()) {
            Toast.makeText(requireContext(), "No local backup files found", Toast.LENGTH_SHORT).show()
            return
        }
        val fileNames = existingFiles.map { it.displayName }.toTypedArray()
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.import_backup)
            .setItems(fileNames) { _, which ->
                viewModel.importBackupToFile(item, existingFiles[which])
            }.show()
    }

    private fun buildSignInIntent(): Intent =
        AuthUI
            .getInstance()
            .createSignInIntentBuilder()
            .setTheme(R.style.AppTheme)
            .setTosAndPrivacyPolicyUrls(
                "https://www.yabasanshiro.com/terms-of-use",
                "https://www.yabasanshiro.com/privacy",
            ).setAvailableProviders(Arrays.asList(GoogleBuilder().build(), AppleBuilder().build()))
            .build()

    companion object {
        private const val ARG_BACKUP_ID = "backup_id"

        fun newInstance(id: String): SharedBackupImportSheet =
            SharedBackupImportSheet().apply {
                arguments = Bundle().apply { putString(ARG_BACKUP_ID, id) }
            }
    }
}
