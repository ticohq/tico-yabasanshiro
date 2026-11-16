package org.uoyabause.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class VideoFilterTest {
    private val all = listOf("0", "1", "2", "3", "4", "5", "6")

    @Test
    fun `OpenGL offers every filter`() {
        assertEquals(all, VideoFilter.offeredValues(VideoCore.OPENGL, all))
    }

    @Test
    fun `Vulkan offers None and the CRT filters only`() {
        assertEquals(listOf("0", "4", "5", "6"), VideoFilter.offeredValues(VideoCore.VULKAN, all))
    }

    @Test
    fun `a value the core does not offer is shown as None`() {
        val vulkan = VideoFilter.offeredValues(VideoCore.VULKAN, all)
        assertEquals(VideoFilter.NONE, VideoFilter.displayValue("1", vulkan))
        assertEquals(VideoFilter.NONE, VideoFilter.displayValue("3", vulkan))
        assertEquals(VideoFilter.NONE, VideoFilter.displayValue(null, vulkan))
        assertEquals("5", VideoFilter.displayValue("5", vulkan))
        assertEquals("2", VideoFilter.displayValue("2", all))
    }

    @Test
    fun `the filter can be changed with OpenGL and Vulkan only`() {
        assertTrue(VideoFilter.isSelectable(VideoCore.OPENGL))
        assertTrue(VideoFilter.isSelectable(VideoCore.VULKAN))
        assertFalse(VideoFilter.isSelectable(VideoCore.UNSET))
        assertFalse(VideoFilter.isSelectable(VideoCore.SOFTWARE))
    }
}
