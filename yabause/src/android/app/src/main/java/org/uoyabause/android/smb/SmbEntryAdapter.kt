package org.uoyabause.android.smb

import android.text.format.Formatter
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.card.MaterialCardView
import org.devmiyax.yabasanshiro.R

/**
 * Rows of the remote folder browser. The first row is an "up" row when the
 * current folder is not the share root. Every row is focusable so a gamepad can
 * walk the list; A / Enter / D-pad center activate the focused row.
 */
class SmbEntryAdapter(
    private val onUp: () -> Unit,
    private val onFolder: (String) -> Unit,
    private val onFile: (String) -> Unit,
) : RecyclerView.Adapter<SmbEntryAdapter.Holder>() {
    private var showUp = false
    private var entries: List<SmbEntry> = emptyList()
    private var selected: Set<String> = emptySet()

    fun submit(showUp: Boolean, entries: List<SmbEntry>, selected: Set<String>) {
        val sameList = showUp == this.showUp && entries == this.entries
        val previous = this.selected
        this.showUp = showUp
        this.entries = entries
        this.selected = selected
        if (sameList) {
            // Only selection changed: keep focus on the row the user is on.
            val offset = if (showUp) 1 else 0
            entries.forEachIndexed { i, e ->
                if ((e.name in previous) != (e.name in selected)) notifyItemChanged(i + offset)
            }
        } else {
            notifyDataSetChanged()
        }
    }

    override fun getItemCount(): Int = entries.size + if (showUp) 1 else 0

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder {
        val v = LayoutInflater.from(parent.context).inflate(R.layout.item_smb_entry, parent, false)
        return Holder(v as MaterialCardView)
    }

    override fun onBindViewHolder(holder: Holder, position: Int) {
        val ctx = holder.card.context
        val isUpRow = showUp && position == 0
        val entry = if (isUpRow) null else entries[position - if (showUp) 1 else 0]
        val card = holder.card
        card.isCheckable = entry != null && !entry.isDirectory
        card.isChecked = entry != null && entry.name in selected
        when {
            isUpRow -> {
                holder.name.text = ctx.getString(R.string.smb_up_folder)
                holder.size.text = ""
                holder.icon.setImageResource(R.drawable.baseline_folder_open_24)
                card.contentDescription = ctx.getString(R.string.smb_up_folder)
            }
            entry!!.isDirectory -> {
                holder.name.text = entry.name
                holder.size.text = ""
                holder.icon.setImageResource(R.drawable.baseline_folder_24)
                card.contentDescription = entry.name
            }
            else -> {
                holder.name.text = entry.name
                holder.size.text = Formatter.formatShortFileSize(ctx, entry.size)
                holder.icon.setImageResource(R.drawable.baseline_description_24)
                card.contentDescription = entry.name
            }
        }
        val activate = {
            val pos = holder.bindingAdapterPosition
            if (pos != RecyclerView.NO_POSITION) {
                val up = showUp && pos == 0
                if (up) {
                    onUp()
                } else {
                    val e = entries[pos - if (showUp) 1 else 0]
                    if (e.isDirectory) onFolder(e.name) else onFile(e.name)
                }
            }
        }
        card.setOnClickListener { activate() }
        card.setOnKeyListener { _, keyCode, event ->
            if (event.action == KeyEvent.ACTION_DOWN && keyCode == KeyEvent.KEYCODE_BUTTON_A) {
                activate()
                true
            } else {
                false
            }
        }
        card.setOnFocusChangeListener { v, hasFocus ->
            val c = v as MaterialCardView
            c.strokeWidth = if (hasFocus) {
                c.resources.getDimensionPixelSize(R.dimen.card_stroke_width_focused)
            } else {
                0
            }
        }
    }

    class Holder(
        val card: MaterialCardView,
    ) : RecyclerView.ViewHolder(card) {
        val icon: ImageView = card.findViewById(R.id.smb_entry_icon)
        val name: TextView = card.findViewById(R.id.smb_entry_name)
        val size: TextView = card.findViewById(R.id.smb_entry_size)
    }
}
