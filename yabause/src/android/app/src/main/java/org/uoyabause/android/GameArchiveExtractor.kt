package org.uoyabause.android

import android.util.Log
import org.apache.commons.compress.archivers.sevenz.SevenZFile
import java.io.File
import java.io.InputStream
import java.util.Locale
import java.util.zip.ZipFile

/**
 * Extracts a zip / 7z game archive into the install directory. Shared by the
 * file picker flow and the SMB import flow.
 */
object GameArchiveExtractor {
    private const val TAG = "GameArchiveExtractor"
    private const val BUFFER_SIZE = 256 * 1024

    /**
     * Called while extracting. [bytesTotal] is the sum of the uncompressed entry
     * sizes the archive declares, or 0 when it declares none. The callback may
     * throw (for example a CancellationException) to abort the extraction.
     */
    fun interface ProgressListener {
        fun onProgress(entryName: String, bytesDone: Long, bytesTotal: Long)
    }

    /**
     * Extracts [archive] into [installDir] and returns the absolute path of the
     * last ccd / cue / mds entry, or an empty string when the archive has none.
     * Entries that would land outside [installDir] are skipped. When extraction
     * fails or is aborted, the files written so far are deleted.
     */
    fun extract(installDir: File, archive: File, listener: ProgressListener? = null): String {
        val lower = archive.name.lowercase(Locale.getDefault())
        val written = ArrayList<File>()
        var ok = false
        try {
            val target = when {
                lower.endsWith("zip") -> extractZip(installDir, archive, written, listener)
                lower.endsWith("7z") -> extract7z(installDir, archive, written, listener)
                else -> ""
            }
            ok = true
            return target
        } finally {
            if (!ok) written.forEach { runCatching { it.delete() } }
        }
    }

    private fun extractZip(
        installDir: File,
        archive: File,
        written: MutableList<File>,
        listener: ProgressListener?,
    ): String {
        var targetFileName = ""
        ZipFile(archive).use { zip ->
            val total = zip.entries().asSequence().sumOf { if (it.isDirectory) 0L else it.size.coerceAtLeast(0L) }
            val counter = Counter(total, listener)
            zip.entries().asSequence().forEach { entry ->
                val outputFile = safeOutput(installDir, entry.name) ?: return@forEach
                if (entry.isDirectory) {
                    outputFile.mkdirs()
                } else {
                    zip.getInputStream(entry).use { input ->
                        writeEntry(input, outputFile, entry.name, written, counter)
                    }
                }
                if (isCueLike(entry.name)) {
                    targetFileName = outputFile.absolutePath
                }
            }
        }
        return targetFileName
    }

    private fun extract7z(
        installDir: File,
        archive: File,
        written: MutableList<File>,
        listener: ProgressListener?,
    ): String {
        var targetFileName = ""
        SevenZFile.builder().setFile(archive).get().use { sz ->
            val total = sz.entries.sumOf { if (it.isDirectory || !it.hasStream()) 0L else it.size.coerceAtLeast(0L) }
            val counter = Counter(total, listener)
            sz.entries.asSequence().forEach { entry ->
                val outputFile = safeOutput(installDir, entry.name) ?: return@forEach
                if (entry.isDirectory) {
                    outputFile.mkdirs()
                } else {
                    sz.getInputStream(entry).use { input ->
                        writeEntry(input, outputFile, entry.name, written, counter)
                    }
                }
                if (isCueLike(entry.name)) {
                    targetFileName = outputFile.absolutePath
                }
            }
        }
        return targetFileName
    }

    private class Counter(
        val total: Long,
        val listener: ProgressListener?,
    ) {
        var done = 0L
    }

    private fun safeOutput(installDir: File, name: String): File? {
        val outputFile = File(installDir, name)
        // Guard against path traversal entries
        if (!outputFile.canonicalPath.startsWith(installDir.canonicalPath)) {
            Log.e(TAG, "Entry is outside of the target dir: $name")
            return null
        }
        return outputFile
    }

    private fun writeEntry(
        input: InputStream,
        outputFile: File,
        entryName: String,
        written: MutableList<File>,
        counter: Counter,
    ) {
        outputFile.parentFile?.mkdirs()
        written.add(outputFile)
        val displayName = entryName.substringAfterLast('/')
        counter.listener?.onProgress(displayName, counter.done, counter.total)
        outputFile.outputStream().use { output ->
            val buf = ByteArray(BUFFER_SIZE)
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                output.write(buf, 0, n)
                counter.done += n
                counter.listener?.onProgress(displayName, counter.done, counter.total)
            }
        }
    }

    private fun isCueLike(name: String): Boolean {
        val lower = name.lowercase(Locale.ROOT)
        return lower.endsWith("ccd") || lower.endsWith("cue") || lower.endsWith("mds")
    }
}
