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
package org.uoyabause.android.game

import android.app.Activity
import android.app.Dialog
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.preference.PreferenceManager
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.bottomsheet.BottomSheetBehavior
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.bottomsheet.BottomSheetDialogFragment
import com.google.android.material.card.MaterialCardView
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.DeviceCapabilities
import org.uoyabause.android.GameSelectPresenter
import org.uoyabause.android.YabauseApplication
import org.uoyabause.android.smb.SmbShareDialogFragment

class AddGameBottomSheetFragment : BottomSheetDialogFragment() {
    interface Listener {
        fun onFileSelected(uri: Uri)

        fun onFolderAdded(path: String)

        fun onFolderRemoved(path: String)

        fun onScanFolder(path: String)

        fun onScanAllFolders()

        /**
         * Presenter the SMB import uses to check the free limit, ask for the
         * install storage and register a single imported game.
         */
        fun getImportPresenter(): GameSelectPresenter? = null

        /** Called after several games were copied in; the host rescans its library. */
        fun onGamesInstalled() {}
    }

    private var mListener: Listener? = null
    private var mAdapter: ScanFolderAdapter? = null
    private var mRecyclerView: RecyclerView? = null
    private var mTextEmptyFolders: TextView? = null
    private var isPc = false
    private var mDirectoryManager: GameDirectoryManager? = null

    private lateinit var filePickerLauncher: ActivityResultLauncher<Intent>
    private lateinit var folderPickerLauncher: ActivityResultLauncher<Uri?>

    fun setListener(listener: Listener) {
        mListener = listener
    }

    fun updateFolderList(items: List<ScanFolderItem>) {
        mAdapter?.updateItems(items)
        updateEmptyState(items.isEmpty())
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        filePickerLauncher = registerForActivityResult(
            ActivityResultContracts.StartActivityForResult(),
        ) { result ->
            if (result.resultCode == Activity.RESULT_OK) {
                result.data?.data?.let { uri ->
                    mListener?.onFileSelected(uri)
                    dismiss()
                }
            }
        }

        folderPickerLauncher = registerForActivityResult(
            ActivityResultContracts.OpenDocumentTree(),
        ) { uri ->
            if (uri != null) {
                handleFolderSelected(uri)
            }
        }
    }

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val dialog = super.onCreateDialog(savedInstanceState) as BottomSheetDialog
        dialog.setOnShowListener { dialogInterface ->
            val bottomSheetDialog = dialogInterface as BottomSheetDialog
            val bottomSheet = bottomSheetDialog.findViewById<View>(
                com.google.android.material.R.id.design_bottom_sheet,
            )
            bottomSheet?.let {
                it.setBackgroundResource(android.R.color.transparent)
                val behavior = BottomSheetBehavior.from(it)
                behavior.state = BottomSheetBehavior.STATE_EXPANDED
                behavior.skipCollapsed = true
            }
        }
        return dialog
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View? = inflater.inflate(R.layout.bottom_sheet_add_game, container, false)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        val prefs = PreferenceManager.getDefaultSharedPreferences(requireContext())
        mDirectoryManager = GameDirectoryManager(prefs)
        isPc = DeviceCapabilities.isPc(requireContext())

        view.findViewById<MaterialCardView>(R.id.card_select_file).setOnClickListener {
            launchFilePicker()
        }

        view.findViewById<MaterialCardView>(R.id.card_add_folder).setOnClickListener {
            launchFolderPicker()
        }

        view.findViewById<MaterialCardView>(R.id.card_smb_share).setOnClickListener {
            launchSmbShare()
        }

        view.findViewById<Button>(R.id.button_scan_all).setOnClickListener {
            if (!YabauseApplication.isPro()) {
                YabauseApplication.checkDonated(requireActivity())
                return@setOnClickListener
            }
            mListener?.onScanAllFolders()
        }

        mTextEmptyFolders = view.findViewById(R.id.text_empty_folders)

        mAdapter = ScanFolderAdapter(
            emptyList(),
            object : ScanFolderAdapter.OnFolderActionListener {
                override fun onRescanClick(position: Int, item: ScanFolderItem) {
                    if (!YabauseApplication.isPro()) {
                        YabauseApplication.checkDonated(requireActivity())
                        return
                    }
                    mListener?.onScanFolder(item.path)
                }

                override fun onDeleteClick(position: Int, item: ScanFolderItem) {
                    showDeleteConfirmation(position, item)
                }
            },
        )

        mRecyclerView = view.findViewById(R.id.recycler_scan_folders)
        mRecyclerView?.layoutManager = LinearLayoutManager(context)
        mRecyclerView?.adapter = mAdapter

        loadFolderList()

        if (!YabauseApplication.isPro()) {
            applyScanSectionFreeState(view)
        }
        applyPlatformOptions(view)
    }

    private fun applyScanSectionFreeState(view: View) {
        view.findViewById<View>(R.id.scan_section_header)?.alpha = 0.5f
        view.findViewById<Button>(R.id.button_scan_all)?.alpha = 0.5f
        mRecyclerView?.alpha = 0.5f
        mTextEmptyFolders?.alpha = 0.5f
    }

    private fun launchFilePicker() {
        if (!YabauseApplication.isPro()) {
            val gameCount = org.uoyabause.android.YabauseStorage.dao
                .getAll()
                .size
            if (gameCount >= org.devmiyax.yabasanshiro.BuildConfig.MAX_FREE_GAMES) {
                YabauseApplication.checkDonated(requireActivity())
                return
            }
        }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        try {
            filePickerLauncher.launch(intent)
        } catch (e: android.content.ActivityNotFoundException) {
            showNoFileManagerMessage()
        }
    }

    private fun launchSmbShare() {
        val listener = mListener ?: return
        val fm = parentFragmentManager
        if (fm.findFragmentByTag(SmbShareDialogFragment.TAG) != null) return
        // Shown through the host's fragment manager so it outlives this sheet.
        SmbShareDialogFragment.newInstance(listener).show(fm, SmbShareDialogFragment.TAG)
        dismiss()
    }

    private fun launchFolderPicker() {
        if (!YabauseApplication.isPro()) {
            YabauseApplication.checkDonated(requireActivity())
            return
        }
        try {
            folderPickerLauncher.launch(null)
        } catch (e: android.content.ActivityNotFoundException) {
            showNoFileManagerMessage()
        }
    }

    private fun handleFolderSelected(uri: Uri) {
        val contentResolver = requireContext().contentResolver
        val takeFlags = Intent.FLAG_GRANT_READ_URI_PERMISSION
        contentResolver.takePersistableUriPermission(uri, takeFlags)

        val path = uri.toString()
        mDirectoryManager?.addDirectory(path)
        loadFolderList()
        mListener?.onFolderAdded(path)
    }

    private fun showNoFileManagerMessage() {
        val message = getString(R.string.no_file_manager_available, requireContext().packageName)
        android.widget.Toast
            .makeText(requireContext(), message, android.widget.Toast.LENGTH_LONG)
            .show()
    }

    private fun showDeleteConfirmation(position: Int, item: ScanFolderItem) {
        AlertDialog
            .Builder(requireContext())
            .setMessage(R.string.delete_folder_confirm)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                removeFolder(position, item)
            }.setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun removeFolder(position: Int, item: ScanFolderItem) {
        mDirectoryManager?.removeDirectory(position)
        loadFolderList()
        mListener?.onFolderRemoved(item.path)
    }

    fun loadFolderList() {
        val manager = mDirectoryManager ?: return
        val directories = manager.loadDirectoryList()
        val metaList = manager.loadMetaList()
        val metaMap = metaList.associateBy { it.path }

        val items = directories.map { path ->
            metaMap[path] ?: ScanFolderItem(path = path)
        }

        mAdapter?.updateItems(items)
        updateEmptyState(items.isEmpty())
    }

    private fun updateEmptyState(isEmpty: Boolean) {
        if (isPc) return // the folder list is hidden on Google Play Games on PC
        mTextEmptyFolders?.visibility = if (isEmpty) View.VISIBLE else View.GONE
        mRecyclerView?.visibility = if (isEmpty) View.GONE else View.VISIBLE
    }

    /**
     * Google Play Games on PC: the system file and folder pickers only see the
     * VM's internal storage, so the shared folder is the only way to bring games
     * in, and it is the only option shown. Other devices use the pickers and do
     * not show the shared folder.
     */
    private fun applyPlatformOptions(view: View) {
        val pcOnly = if (isPc) View.VISIBLE else View.GONE
        val nonPc = if (isPc) View.GONE else View.VISIBLE
        view.findViewById<View>(R.id.card_smb_share).visibility = pcOnly
        view.findViewById<View>(R.id.import_cards_row).visibility = nonPc
        view.findViewById<View>(R.id.scan_section_header).visibility = nonPc
        if (isPc) {
            mRecyclerView?.visibility = View.GONE
            mTextEmptyFolders?.visibility = View.GONE
        }
    }

    companion object {
        const val TAG = "AddGameBottomSheet"

        fun newInstance(): AddGameBottomSheetFragment = AddGameBottomSheetFragment()
    }
}
