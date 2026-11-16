package org.uoyabause.android.smb

import java.io.File
import java.util.Locale

/**
 * Pure helpers that decide which remote files are shown and which companion
 * files travel with a selected game file. No I/O, so they can be unit tested.
 */
object SmbFileNames {
    val GAME_EXTENSIONS = setOf(
        "chd",
        "cue",
        "iso",
        "bin",
        "img",
        "ccd",
        "mds",
        "mdf",
        "zip",
        "7z",
    )

    val ARCHIVE_EXTENSIONS = setOf("zip", "7z")

    fun extensionOf(name: String): String = name.substringAfterLast('.', "").lowercase(Locale.ROOT)

    fun isGameFile(name: String): Boolean = extensionOf(name) in GAME_EXTENSIONS

    fun isArchive(name: String): Boolean = extensionOf(name) in ARCHIVE_EXTENSIONS

    fun baseNameOf(name: String): String = name.substringBeforeLast('.', name)

    /**
     * Extracts the file names referenced by `FILE "name" BINARY` lines of a cue
     * sheet. Names may be quoted or bare; the result keeps the order of appearance
     * and drops duplicates.
     */
    fun parseCueFileNames(cueText: String): List<String> {
        val result = LinkedHashSet<String>()
        cueText.lineSequence().forEach { raw ->
            val line = raw.trim().dropWhile { it.code == 0xFEFF }.trim()
            if (!line.regionMatches(0, "FILE", 0, 4, ignoreCase = true)) return@forEach
            val rest = line.substring(4).trim()
            if (rest.isEmpty()) return@forEach
            val name = if (rest.startsWith("\"")) {
                val end = rest.indexOf('"', 1)
                if (end < 0) return@forEach
                rest.substring(1, end)
            } else {
                rest.split(Regex("\\s+")).first()
            }
            if (name.isNotEmpty()) result.add(name)
        }
        return result.toList()
    }

    /**
     * Splits a relative remote path ("a\b/c.bin") into its segments. Returns null
     * when the path is empty, absolute-looking or tries to leave its directory.
     */
    fun safeSegments(relative: String): List<String>? {
        val segments = relative.replace('\\', '/').split('/').filter { it.isNotEmpty() && it != "." }
        if (segments.isEmpty()) return null
        if (segments.any { it == ".." || it.contains(':') }) return null
        if (relative.startsWith("/") || relative.startsWith("\\")) return null
        return segments
    }

    /**
     * Resolves [segments] below [root] and verifies by canonical path that the
     * result stays inside [root]. Returns null when it would escape.
     */
    fun resolveInside(root: File, segments: List<String>): File? {
        var out = root
        segments.forEach { out = File(out, it) }
        val rootPath = root.canonicalPath + File.separator
        return if (out.canonicalPath.startsWith(rootPath)) out else null
    }
}
