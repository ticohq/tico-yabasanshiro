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
package org.uoyabause.android

import android.util.Log
import android.view.Surface

// Axis debug values for display - top level object for easy access
object AxisDebugValues {
    @Volatile var axisX: Int = 128

    @Volatile var axisY: Int = 128

    @Volatile var axisLTrigger: Int = 0

    @Volatile var axisRTrigger: Int = 0

    // Raw axis values from controller (before mapping)
    @Volatile var rawX: Float = 0f

    @Volatile var rawY: Float = 0f

    @Volatile var rawZ: Float = 0f

    @Volatile var rawRZ: Float = 0f
}

class YabauseRunnable(
    yabause: Yabause?,
) : Runnable {
    var inited: Boolean
        private set
    private val paused = false

    fun destroy() {
        Log.v("Yabause", "destroying yabause...")
        inited = false
        deinit()
    }

    override fun run() {
        if (inited && !paused) {
            exec()
            // Process RetroAchievements frame
            try {
                doFrameRetroAchievements()
            } catch (e: UnsatisfiedLinkError) {
                // RetroAchievements not available, ignore
            }
        }
    }

    companion object {
        @JvmStatic
        fun axisWithDebug(
            key: Int,
            player: Int,
            value: Int,
        ) {
            when (key) {
                18 -> AxisDebugValues.axisX = value // PERANALOG_AXIS_X
                19 -> AxisDebugValues.axisY = value // PERANALOG_AXIS_Y
                20 -> AxisDebugValues.axisRTrigger = value // PERANALOG_AXIS_RTRIGGER
                21 -> AxisDebugValues.axisLTrigger = value // PERANALOG_AXIS_LTRIGGER
            }
            axis(key, player, value)
        }

        @JvmStatic
        external fun isRunning(): Int

        @JvmStatic
        external fun init(yabause: Yabause?): Int

        @JvmStatic
        external fun deinit()

        @JvmStatic
        external fun exec()

        @JvmStatic
        external fun reset()

        @JvmStatic
        external fun press(
            key: Int,
            player: Int,
        )

        @JvmStatic
        external fun axis(
            key: Int,
            player: Int,
            `val`: Int,
        )

        @JvmStatic
        external fun release(
            key: Int,
            player: Int,
        )

        @JvmStatic
        external fun initViewport(
            sf: Surface?,
            width: Int,
            hieght: Int,
        ): Int

        @JvmStatic
        external fun drawScreen(): Int

        /**
         * Tells the renderer the surface is about to be destroyed and returns once
         * no frame is still presenting to it. Must be called without holding
         * lockGL(): it waits on the render thread.
         */
        @JvmStatic
        external fun surfaceLost()

        @JvmStatic
        external fun lockGL(): Int

        @JvmStatic
        external fun unlockGL(): Int

        @JvmStatic
        external fun enableFPS(enable: Int)

        @JvmStatic
        external fun enableExtendedMemory(enable: Int)

        @JvmStatic
        external fun enableRotateScreen(enable: Int)

        @JvmStatic
        external fun enableComputeShader(enable: Int)

        @JvmStatic
        external fun enableFrameskip(enable: Int)

        @JvmStatic
        external fun setUseCpuAffinity(enable: Int)

        @JvmStatic
        external fun setUseSh2Cache(enable: Int)

        @JvmStatic
        external fun setCpu(cpu: Int)

        @JvmStatic
        external fun setFilter(filter: Int)

        @JvmStatic
        external fun setVolume(volume: Int)

        @JvmStatic
        external fun restartAudioStream()

        @JvmStatic
        external fun screenshot(filename: String?): Int

        @JvmStatic
        external fun getCurrentGameCode(): String?

        @JvmStatic
        external fun getCurrentGamePath(): String?

        @JvmStatic
        external fun getGameTitle(): String?

        @JvmStatic
        external fun getGameinfo(): String?

        /** {vendorID, deviceID, 1 if GPU tessellation is usable}, or null without Vulkan. */
        @JvmStatic
        external fun getVulkanDeviceInfo(): IntArray?

        @JvmStatic
        external fun peekMemory(
            address: Int,
            length: Int,
        ): ByteArray?

        @JvmStatic
        external fun savestate(path: String?): String?

        @JvmStatic
        external fun loadstate(path: String?)

        @JvmStatic
        external fun savestate_compress(path: String?): String?

        @JvmStatic
        external fun loadstate_compress(path: String?)

        @JvmStatic
        external fun record(path: String?): Int

        @JvmStatic
        external fun play(path: String?): Int

        @JvmStatic
        external fun getRecordingStatus(): Int

        @JvmStatic
        external fun pause()

        @JvmStatic
        external fun resume()

        @JvmStatic
        external fun setPolygonGenerationMode(pg: Int)

        @JvmStatic
        external fun setAspectRateMode(ka: Int)

        @JvmStatic
        external fun setVdp2NewComposite(enable: Int)

        @JvmStatic
        external fun setSoundEngine(engine: Int)

        /** High-quality sound of the current sound engine: 1 on, 0 off. */
        @JvmStatic
        external fun setSoundHighQuality(enable: Int)

        @JvmStatic
        external fun setResolutionMode(resoution_mode: Int)

        @JvmStatic
        external fun setRbgResolutionMode(resoution_mode: Int)

        @JvmStatic
        external fun openTray()

        @JvmStatic
        external fun closeTray()

        @JvmStatic
        external fun switch_padmode(mode: Int)

        @JvmStatic
        external fun switch_padmode2(mode: Int)

        @JvmStatic
        external fun updateCheat(cheat_code: Array<String?>?)

        @JvmStatic
        external fun setScspSyncPerFrame(scsp_sync_count: Int)

        @JvmStatic
        external fun setCpuSyncPerLine(cpu_sync_count: Int)

        @JvmStatic
        external fun setScspSyncTimeMode(mode: Int)

        @JvmStatic
        external fun getDevicelist(): String?

        @JvmStatic
        external fun getFilelist(deviceid: Int): String?

        @JvmStatic
        external fun deletefile(index: Int): Int

        @JvmStatic
        external fun getFile(index: Int): String?

        @JvmStatic
        external fun putFile(path: String?): String?

        @JvmStatic
        external fun copy(
            target_device: Int,
            file_index: Int,
        ): Int

        @JvmStatic
        external fun getGameinfoFromChd(path: String?): ByteArray?

        @JvmStatic
        external fun enableBackupWriteHook(): Int

        @JvmStatic
        external fun setFrameLimitMode(mode: Int)

        // RetroAchievements integration methods
        @JvmStatic
        external fun initRetroAchievements(callback: Any): Int

        @JvmStatic
        external fun loginRetroAchievements(
            username: String,
            password: String,
        ): Boolean

        @JvmStatic
        external fun loadGameRetroAchievements(gamePath: String): Boolean

        @JvmStatic
        external fun doFrameRetroAchievements()

        @JvmStatic
        external fun setSkipRaChangeMedia(skip: Boolean)

        @JvmStatic
        external fun setHardcoreEnabledRetroAchievements(enabled: Boolean)

        @JvmStatic
        external fun isHardcoreEnabledRetroAchievements(): Boolean

        @JvmStatic
        external fun handleHttpResponse(
            callbackPtr: Long,
            httpCode: Int,
            response: String,
        )

        const val IDLE = -1
        const val RECORDING = 0
        const val PLAYING = 1
    }

    init {
        val ok = init(yabause)
        Log.v("Yabause", "init = $ok")
        inited = ok == 0
    }
}
