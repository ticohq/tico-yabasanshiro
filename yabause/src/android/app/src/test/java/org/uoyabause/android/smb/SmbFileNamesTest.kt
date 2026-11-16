package org.uoyabause.android.smb

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class SmbFileNamesTest {
    @Test
    fun gameFileFilterIsCaseInsensitive() {
        assertTrue(SmbFileNames.isGameFile("Game.CHD"))
        assertTrue(SmbFileNames.isGameFile("a.7z"))
        assertFalse(SmbFileNames.isGameFile("readme.txt"))
        assertFalse(SmbFileNames.isGameFile("noext"))
    }

    @Test
    fun cueParsingHandlesQuotedAndBareNames() {
        val cue = "FILE \"Track 01.bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\nFILE Track02.bin BINARY\r\nFILE \"Track 01.bin\" BINARY\r\n"
        assertEquals(listOf("Track 01.bin", "Track02.bin"), SmbFileNames.parseCueFileNames(cue))
    }

    @Test
    fun unsafeSegmentsAreRejected() {
        assertNull(SmbFileNames.safeSegments("..\\evil.bin"))
        assertNull(SmbFileNames.safeSegments("a/../../evil.bin"))
        assertNull(SmbFileNames.safeSegments("C:\\evil.bin"))
        assertEquals(listOf("sub", "t.bin"), SmbFileNames.safeSegments("sub\\t.bin"))
    }

    @Test
    fun resolveInsideStaysUnderRoot() {
        val root = File(System.getProperty("java.io.tmpdir"), "smb_root_test")
        root.mkdirs()
        assertNotNull(SmbFileNames.resolveInside(root, listOf("a", "b.bin")))
    }
}
