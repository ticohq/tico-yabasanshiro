package org.uoyabause.android

import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager

/**
 * What the running device can do. Values that cannot be determined are null or
 * false: a missing value is expected (sysfs and GL extensions are vendor
 * specific), and every consumer falls back to the cheaper setting.
 */
data class DeviceCapabilities(
    val vulkanSupported: Boolean,
    val glEsVersion: Int,
    /** GL ES tessellation, from the GL extension string. */
    val hasTessellation: Boolean,
    val cpuMaxClockKHz: Int?,
    val isPc: Boolean,
    val gpuKey: String?,
    /**
     * Whether the Vulkan device can run the GPU tessellation polygon path.
     * Advertising Vulkan does not imply it - the renderer needs the tessellation
     * and geometry shader features together, and some devices have neither.
     */
    val vulkanTessellation: Boolean,
) {
    /**
     * Whether Vulkan is worth recommending. The Vulkan core has no
     * perspective-corrected triangle path (the app rewrites that mode to GPU
     * tessellation under Vulkan), so a device without tessellation has nothing
     * usable there and belongs on OpenGL.
     */
    val canUseVulkan: Boolean get() = vulkanSupported && vulkanTessellation

    companion object {
        private const val FEATURE_PC = "com.google.android.play.feature.HPE_EXPERIENCE"
        const val GL_ES_31 = 0x30001

        /** Cheap Google Play Games on PC check, without the GPU probes of [forContext]. */
        fun isPc(context: Context): Boolean = context.packageManager.hasSystemFeature(FEATURE_PC)

        fun forContext(context: Context): DeviceCapabilities {
            val pm = context.packageManager
            val vulkan = pm.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)
            val activityManager =
                context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val glEsVersion = activityManager.deviceConfigurationInfo.reqGlEsVersion
            val extensions = DeviceInfo.glExtensions()
            val tessellation =
                extensions.contains("GL_EXT_tessellation_shader") ||
                    extensions.contains("GL_ANDROID_extension_pack_es31a")

            val vulkanInfo = if (vulkan) runCatching { YabauseRunnable.getVulkanDeviceInfo() }.getOrNull() else null
            val gpuKey =
                if (vulkanInfo != null && vulkanInfo.size >= 2) {
                    GpuIdentity.vulkanKey(vulkanInfo[0], vulkanInfo[1])
                } else {
                    GpuIdentity.openGlKey(DeviceInfo.gpuRenderer())
                }
            // Unknown counts as unsupported: the probe failing means no Vulkan
            // instance could be created here, which is not a device to recommend
            // the Vulkan core to.
            val vulkanTessellation = vulkanInfo != null && vulkanInfo.size >= 3 && vulkanInfo[2] != 0

            return DeviceCapabilities(
                vulkanSupported = vulkan,
                glEsVersion = glEsVersion,
                hasTessellation = tessellation,
                cpuMaxClockKHz = CpuClock.maxKhz(),
                isPc = isPc(context),
                gpuKey = gpuKey,
                vulkanTessellation = vulkanTessellation,
            )
        }
    }
}
