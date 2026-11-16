package org.uoyabause.android.smb

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.devmiyax.yabasanshiro.BuildConfig
import org.uoyabause.android.YabauseApplication
import org.uoyabause.android.YabauseStorage
import java.io.File
import java.util.Locale

enum class SmbStage { FORM, CONNECTING, BROWSING, LOADING, IMPORTING }

data class SmbUiState(
    val stage: SmbStage = SmbStage.FORM,
    /** Current remote directory, '\' separated, empty for the share root. */
    val path: String = "",
    val entries: List<SmbEntry> = emptyList(),
    val selected: Set<String> = emptySet(),
    val progress: SmbImportProgress? = null,
    val errorKind: SmbErrorKind? = null,
    val errorDetail: String? = null,
)

sealed interface SmbEvent {
    data class ImportFinished(
        val outcome: SmbImportOutcome,
    ) : SmbEvent
}

/**
 * Holds the SMB session and the browser state across configuration changes. The
 * connection is closed when the dialog is finally gone.
 */
class SmbShareViewModel(
    application: Application,
) : AndroidViewModel(application) {
    private val client = SmbShareClient()
    private val store = SmbCredentialStore(application)

    private val _state = MutableStateFlow(SmbUiState())
    val state: StateFlow<SmbUiState> = _state.asStateFlow()

    private val _events = Channel<SmbEvent>(Channel.BUFFERED)
    val events = _events.receiveAsFlow()

    private var importJob: Job? = null

    suspend fun loadSaved(): SmbConnectionInfo? = withContext(Dispatchers.IO) { store.load() }

    fun connect(info: SmbConnectionInfo, remember: Boolean) {
        if (_state.value.stage == SmbStage.CONNECTING) return
        _state.update { it.copy(stage = SmbStage.CONNECTING, errorKind = null, errorDetail = null) }
        viewModelScope.launch(Dispatchers.IO) {
            try {
                client.open(info)
                if (remember) store.save(info) else store.clear()
                val entries = browsable(client.list(""))
                _state.update {
                    it.copy(
                        stage = SmbStage.BROWSING,
                        path = "",
                        entries = entries,
                        selected = emptySet(),
                    )
                }
            } catch (e: SmbClientException) {
                _state.update {
                    it.copy(stage = SmbStage.FORM, errorKind = e.kind, errorDetail = e.message)
                }
            }
        }
    }

    fun navigateInto(folder: String) {
        val cur = _state.value
        load(if (cur.path.isEmpty()) folder else cur.path + "\\" + folder)
    }

    /** Goes up one folder. Returns false when already at the share root. */
    fun navigateUp(): Boolean {
        val cur = _state.value
        if (cur.path.isEmpty()) return false
        load(cur.path.substringBeforeLast('\\', ""))
        return true
    }

    private fun load(path: String) {
        val previous = _state.value
        _state.update { it.copy(stage = SmbStage.LOADING, errorKind = null, errorDetail = null) }
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val entries = browsable(client.list(path))
                _state.update {
                    it.copy(
                        stage = SmbStage.BROWSING,
                        path = path,
                        entries = entries,
                        selected = emptySet(),
                    )
                }
            } catch (e: SmbClientException) {
                _state.update {
                    previous.copy(
                        stage = SmbStage.BROWSING,
                        errorKind = e.kind,
                        errorDetail = e.message,
                    )
                }
            }
        }
    }

    fun toggleSelected(name: String) {
        _state.update {
            val next = if (name in it.selected) it.selected - name else it.selected + name
            it.copy(selected = next)
        }
    }

    /**
     * @param isSubscribed the subscription state from the game list; the free
     *   limit is enforced here rather than only in the dialog, so a recreated
     *   dialog that lost its host cannot skip it.
     */
    fun startImport(isSubscribed: Boolean) {
        val cur = _state.value
        if (cur.stage != SmbStage.BROWSING || cur.selected.isEmpty()) return
        val picked = cur.entries.filter { !it.isDirectory && it.name in cur.selected }
        if (picked.isEmpty()) return
        val dir = cur.path
        _state.update {
            it.copy(stage = SmbStage.IMPORTING, progress = null, errorKind = null, errorDetail = null)
        }
        importJob = viewModelScope.launch(Dispatchers.IO) {
            val installDir: File = YabauseStorage.storage.getInstallDir()
            installDir.mkdirs()
            val importer = SmbImporter(client, installDir) { p ->
                _state.update { it.copy(progress = p) }
            }
            val allowance = SmbFreeLimit.remaining(
                isPro = YabauseApplication.isPro(),
                isSubscribed = isSubscribed,
                installedCount = YabauseStorage.dao.getAll().size,
                max = BuildConfig.MAX_FREE_GAMES,
            )
            val outcome = importer.import(dir, picked, allowance)
            // The free limit is explained by the dialog, not the error line.
            val error = outcome.error?.takeIf { it !is FreeLimitExceededException }
            _state.update {
                it.copy(
                    stage = SmbStage.BROWSING,
                    progress = null,
                    selected = if (outcome.error is FreeLimitExceededException) it.selected else emptySet(),
                    errorKind = error?.let { e -> (e as? SmbClientException)?.kind ?: SmbErrorKind.OTHER },
                    errorDetail = error?.message,
                )
            }
            _events.send(SmbEvent.ImportFinished(outcome))
        }
    }

    fun cancelImport() {
        importJob?.cancel()
    }

    private fun browsable(list: List<SmbEntry>): List<SmbEntry> = list
        .filter { it.isDirectory || SmbFileNames.isGameFile(it.name) }
        .sortedWith(
            compareBy<SmbEntry> { !it.isDirectory }.thenBy { it.name.lowercase(Locale.getDefault()) },
        )

    override fun onCleared() {
        importJob?.cancel()
        val c = client
        Thread { c.close() }.start()
        super.onCleared()
    }
}
