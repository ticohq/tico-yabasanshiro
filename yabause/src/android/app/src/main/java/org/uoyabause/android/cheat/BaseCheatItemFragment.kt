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
package org.uoyabause.android.cheat

import android.content.Intent
import android.os.Bundle
import android.view.View
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.Fragment
import androidx.recyclerview.widget.RecyclerView
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.AuthUI.IdpConfig.AppleBuilder
import com.firebase.ui.auth.AuthUI.IdpConfig.GoogleBuilder
import com.google.firebase.database.DatabaseReference
import org.devmiyax.yabasanshiro.R
import org.uoyabause.android.auth.AuthState
import org.uoyabause.android.auth.ui.PcSignInActivity
import java.util.Arrays

/**
 * Base fragment for cheat item lists.
 * Provides common functionality shared between CloudCheatItemFragment and LocalCheatItemFragment.
 */
abstract class BaseCheatItemFragment : Fragment() {
    protected var columnCount = 1
    protected var gameCode: String? = null
    protected var database: DatabaseReference? = null
    protected var rootView: View? = null
    protected var listView: RecyclerView? = null
    protected var items: ArrayList<CheatItem?>? = null

    private lateinit var signInLauncher: ActivityResultLauncher<Intent>
    private var layoutAuthRequired: View? = null
    private var contentView: View? = null
    private var authRequiredShowing = false

    companion object {
        const val ARG_COLUMN_COUNT = "column-count"
        const val ARG_GAME_ID = "game_id"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        arguments?.let {
            columnCount = it.getInt(ARG_COLUMN_COUNT, 1)
            gameCode = it.getString(ARG_GAME_ID)
        }
        signInLauncher =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
                if (AuthState.isSignedIn()) {
                    hideAuthRequired()
                    updateCheatList()
                }
            }
    }

    override fun onResume() {
        super.onResume()
        if (authRequiredShowing && AuthState.isSignedIn()) {
            hideAuthRequired()
            updateCheatList()
        }
    }

    /**
     * Setup auth required UI elements and sign-in button handler.
     * Call this from onCreateView after inflating the layout.
     */
    protected fun setupAuthRequired(view: View, content: View) {
        layoutAuthRequired = view.findViewById(R.id.layout_auth_required)
        contentView = content
        view.findViewById<View>(R.id.btn_sign_in)?.setOnClickListener {
            val signInIntent =
                AuthUI
                    .getInstance()
                    .createSignInIntentBuilder()
                    .setTheme(R.style.AppTheme)
                    .setTosAndPrivacyPolicyUrls(
                        "https://www.yabasanshiro.com/terms-of-use",
                        "https://www.yabasanshiro.com/privacy",
                    ).setAvailableProviders(
                        Arrays.asList(
                            GoogleBuilder().build(),
                            AppleBuilder().build(),
                        ),
                    ).build()
            signInLauncher.launch(PcSignInActivity.wrap(requireContext(), signInIntent))
        }
    }

    /**
     * Show the auth required overlay and hide content.
     */
    protected fun showAuthRequired() {
        layoutAuthRequired?.visibility = View.VISIBLE
        contentView?.visibility = View.GONE
        authRequiredShowing = true
    }

    /**
     * Hide the auth required overlay and show content.
     */
    protected fun hideAuthRequired() {
        layoutAuthRequired?.visibility = View.GONE
        contentView?.visibility = View.VISIBLE
        authRequiredShowing = false
    }

    /**
     * Get the parent TabCheatFragment instance
     * @return TabCheatFragment or null if not found
     */
    val tabCheatFragmentInstance: TabCheatFragment?
        get() = parentFragment as? TabCheatFragment

    /**
     * Toggle the enable state of a cheat item
     * @param item The cheat item to toggle
     */
    protected fun toggleCheatEnable(item: CheatItem) {
        item.enable = !item.enable
        tabCheatFragmentInstance?.let { frag ->
            if (item.enable) {
                frag.addActiveCheat(item.cheat_code)
            } else {
                frag.removeActiveCheat(item.cheat_code)
            }
        }
    }

    /**
     * Update the cheat list from the data source
     */
    abstract fun updateCheatList()

    /** Move the cursor (selection highlight) in the list. direction: -1=up, +1=down */
    open fun moveCursor(direction: Int) {}

    /** Activate the currently highlighted item */
    open fun selectCurrentItem() {}

    /**
     * Interface for cheat item interaction callbacks
     */
    interface OnListFragmentInteractionListener {
        fun onListFragmentInteraction(item: CheatItem?)
    }
}
