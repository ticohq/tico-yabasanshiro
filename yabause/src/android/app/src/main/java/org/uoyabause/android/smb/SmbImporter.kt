package org.uoyabause.android.smb

import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import org.uoyabause.android.GameArchiveExtractor
import java.io.File
import java.io.FileOutputStream
import java.util.Locale

/** Progress snapshot published while files are being copied. */
data class SmbImportProgress(
    val fileName: String,
    val fileIndex: Int,
    val fileCount: Int,
    val bytesDone: Long,
    val bytesTotal: Long,
    /** Non-null while a copied archive is being extracted. */
    val extract: ExtractProgress? = null,
)

/** Extraction of one archive: [bytesTotal] is 0 when the archive declares no sizes. */
data class ExtractProgress(
    val archiveName: String,
    val bytesDone: Long,
    val bytesTotal: Long,
)

/**
 * Result of an import run. [entries] are the local files that can be registered
 * as games; games that finished before an error or a cancel are still listed.
 */
data class SmbImportOutcome(
    val entries: List<File>,
    val error: Throwable?,
    val cancelled: Boolean,
)

/**
 * Copies the selected remote files, plus the companion files a disc image needs,
 * into the install directory. Blocking I/O: run on Dispatchers.IO.
 */
class SmbImporter(
    private val client: SmbShareClient,
    private val installDir: File,
    private val onProgress: (SmbImportProgress) -> Unit,
) {
    private class PlanItem(
        val remotePath: String,
        val segments: List<String>,
        val size: Long,
    )

    private class GamePlan(
        val displayName: String,
        val items: List<PlanItem>,
        val isArchive: Boolean,
    )

    private var bytesTotal = 0L
    private var bytesDone = 0L
    private var lastEmit = 0L
    private var fileIndex = 0
    private var fileCount = 0

    /**
     * @param allowance how many games the free version may still add, or null
     *   for no limit. Checked against the planned games (a cue and its tracks
     *   are one game) before anything is copied.
     */
    suspend fun import(remoteDir: String, selected: List<SmbEntry>, allowance: Int?): SmbImportOutcome {
        val entries = ArrayList<File>()
        try {
            val plans = buildPlans(remoteDir, selected)
            if (allowance != null && plans.size > allowance) {
                return SmbImportOutcome(entries, FreeLimitExceededException(allowance, plans.size), false)
            }
            fileCount = plans.sumOf { it.items.size }
            bytesTotal = plans.sumOf { p -> p.items.sumOf { it.size } }
            for (plan in plans) {
                currentCoroutineContext().ensureActive()
                val entry = copyPlan(plan)
                if (entry != null) entries.add(entry)
            }
            emit("", force = true)
            return SmbImportOutcome(entries, null, false)
        } catch (e: CancellationException) {
            return SmbImportOutcome(entries, null, true)
        } catch (e: Throwable) {
            Log.e(TAG, "Import failed", e)
            return SmbImportOutcome(entries, SmbShareClient.map(e), false)
        }
    }

    private fun joinRemote(dir: String, name: String): String = if (dir.isEmpty()) name else "$dir\\$name"

    private fun buildPlans(remoteDir: String, selected: List<SmbEntry>): List<GamePlan> {
        val claimed = HashSet<String>()
        val plans = ArrayList<GamePlan>()

        fun item(rel: String, size: Long?): PlanItem? {
            val segs = SmbFileNames.safeSegments(rel)
                ?: throw SmbClientException(SmbErrorKind.OTHER, null, "Unsafe file name: $rel")
            val remote = joinRemote(remoteDir, segs.joinToString("\\"))
            return PlanItem(remote, segs, size ?: client.fileSize(remote))
        }

        fun claim(i: PlanItem): Boolean = claimed.add(i.segments.joinToString("/").lowercase(Locale.ROOT))

        // Descriptor files first so that bin / img files they own are not
        // imported a second time as standalone games.
        val ordered = selected.sortedBy {
            when (SmbFileNames.extensionOf(it.name)) {
                "cue", "ccd", "mds" -> 0
                else -> 1
            }
        }

        for (e in ordered) {
            val ext = SmbFileNames.extensionOf(e.name)
            val base = SmbFileNames.baseNameOf(e.name)
            val items = ArrayList<PlanItem>()
            when (ext) {
                "cue" -> {
                    items.add(item(e.name, e.size)!!)
                    val text = String(client.readSmallFile(joinRemote(remoteDir, e.name)), Charsets.UTF_8)
                    SmbFileNames.parseCueFileNames(text).forEach { ref ->
                        items.add(item(ref, null)!!)
                    }
                }
                "ccd" -> {
                    items.add(item(e.name, e.size)!!)
                    items.add(item("$base.img", null)!!)
                    if (client.exists(joinRemote(remoteDir, "$base.sub"))) {
                        items.add(item("$base.sub", null)!!)
                    }
                }
                "mds" -> {
                    items.add(item(e.name, e.size)!!)
                    items.add(item("$base.mdf", null)!!)
                }
                else -> items.add(item(e.name, e.size)!!)
            }
            val fresh = items.filter { claim(it) }
            // The entry file itself is always first; if it was already claimed
            // by another descriptor there is nothing new to import.
            if (fresh.isEmpty() || fresh.first() !== items.first()) continue
            plans.add(GamePlan(e.name, fresh, SmbFileNames.isArchive(e.name)))
        }
        return plans
    }

    /** Copies one game. Returns the local entry file, or null when none is found. */
    private suspend fun copyPlan(plan: GamePlan): File? {
        val created = ArrayList<File>()
        var ok = false
        try {
            var firstDest: File? = null
            for (item in plan.items) {
                currentCoroutineContext().ensureActive()
                fileIndex++
                val dest = SmbFileNames.resolveInside(installDir, item.segments)
                    ?: throw SmbClientException(SmbErrorKind.OTHER, null, "Unsafe path: ${item.remotePath}")
                dest.parentFile?.mkdirs()
                copyOne(item, dest, created)
                if (firstDest == null) firstDest = dest
            }
            val first = firstDest
            val result = if (plan.isArchive && first != null) {
                val job = currentCoroutineContext()[Job]
                val archiveName = first.name
                emitExtract(ExtractProgress(archiveName, 0, 0), force = true)
                val target = try {
                    GameArchiveExtractor.extract(installDir, first) { _, done, total ->
                        job?.ensureActive()
                        emitExtract(ExtractProgress(archiveName, done, total), force = false)
                    }
                } finally {
                    first.delete()
                    created.remove(first)
                }
                if (target.isEmpty()) null else File(target)
            } else {
                first
            }
            ok = true
            return result
        } finally {
            if (!ok) {
                created.forEach { runCatching { it.delete() } }
            }
        }
    }

    private suspend fun copyOne(item: PlanItem, dest: File, created: MutableList<File>) {
        val part = File(dest.parentFile, dest.name + ".smbpart")
        created.add(part)
        client.openRead(item.remotePath).use { remote ->
            FileOutputStream(part).use { out ->
                val buf = ByteArray(BUFFER_SIZE)
                while (true) {
                    currentCoroutineContext().ensureActive()
                    val n = remote.input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    bytesDone += n
                    emit(dest.name, force = false)
                }
            }
        }
        if (dest.exists() && !dest.delete()) {
            throw SmbClientException(SmbErrorKind.OTHER, null, "Cannot replace ${dest.name}")
        }
        if (!part.renameTo(dest)) {
            throw SmbClientException(SmbErrorKind.OTHER, null, "Cannot create ${dest.name}")
        }
        created.remove(part)
        created.add(dest)
    }

    private var currentName = ""

    private fun emit(name: String, force: Boolean) {
        if (name.isNotEmpty()) currentName = name
        val now = System.nanoTime()
        if (!force && now - lastEmit < EMIT_INTERVAL_NS) return
        lastEmit = now
        onProgress(SmbImportProgress(currentName, fileIndex, fileCount, bytesDone, bytesTotal))
    }

    private fun emitExtract(extract: ExtractProgress, force: Boolean) {
        val now = System.nanoTime()
        if (!force && now - lastEmit < EMIT_INTERVAL_NS) return
        lastEmit = now
        onProgress(SmbImportProgress(currentName, fileIndex, fileCount, bytesDone, bytesTotal, extract))
    }

    companion object {
        private const val TAG = "SmbImporter"
        private const val BUFFER_SIZE = 512 * 1024
        private const val EMIT_INTERVAL_NS = 200_000_000L
    }
}
