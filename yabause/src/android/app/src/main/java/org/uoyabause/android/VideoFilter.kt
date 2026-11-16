package org.uoyabause.android

/**
 * Output filter selection (`pref_filter`). The values are the native AAMODE
 * values (ygl.h): 0 none, 1 FXAA, 2 scanline, 3 bilinear, 4 CRT Hyllian,
 * 5 CRT Lottes, 6 CRT NewPixie.
 *
 * The Vulkan core implements only the CRT filters, so it is offered None and
 * the three CRT entries. OpenGL ES is offered all of them. A stored value the
 * current core does not offer is shown as None (the core does nothing for
 * it); the stored value itself is left alone until the user picks again.
 */
object VideoFilter {
    const val KEY = "pref_filter"
    const val NONE = "0"

    private val VULKAN_VALUES = setOf(NONE, "4", "5", "6")

    /** Whether the filter setting can be changed with [videoCore]. */
    fun isSelectable(videoCore: String?): Boolean = videoCore == VideoCore.OPENGL || videoCore == VideoCore.VULKAN

    /** The values from [allValues] (in their order) that [videoCore] offers. */
    fun offeredValues(
        videoCore: String?,
        allValues: List<String>,
    ): List<String> =
        if (videoCore == VideoCore.VULKAN) {
            allValues.filter { it in VULKAN_VALUES }
        } else {
            allValues
        }

    /** The value to show for [stored]: itself if offered, otherwise None. */
    fun displayValue(
        stored: String?,
        offered: List<String>,
    ): String = if (stored != null && stored in offered) stored else NONE
}
