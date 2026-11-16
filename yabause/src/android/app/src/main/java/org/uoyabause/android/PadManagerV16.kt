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
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.json.JSONException
import org.json.JSONObject
import org.uoyabause.android.PadManager.Companion.actionMapped
import org.uoyabause.android.PadManager.Companion.noActionMapped
import org.uoyabause.android.PadManager.Companion.toggleMenu
import org.uoyabause.android.YabauseStorage.Companion.storage
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.InputStream
import java.util.ArrayList
import java.util.HashMap

val excludedDevices =
    listOf(
        "msm8974-taiko-mtp-snd-card Button Jack",
        "uinput-fpc",
        "msm8974-taiko-mtp-snd-card Button Jack",
        "virtual-search",
        "shield-ask-remote",
    )

// class InputInfo{
// 	public float _oldRightTrigger = 0.0f;
// 	public float _oldLeftTrigger = 0.0f;
// 	public int selectedDeviceId = -1;
// }
internal open class BasicInputDevice(
    pdm: PadManagerV16,
) {
    var selectedDeviceId = -1
    var playerIndex = 0
    var testMode = false
    var currentButtonState = 0
    val showMenuCode = 0x1c40
    var keymap: HashMap<Int, Int>
    var pdm: PadManagerV16
    var isLTriggerAnalog = true
    var isRTriggerAnalog = true

    var productId = 0
    var vendorId = 0
    var deviceType = 0

    fun loadDefault() {
        keymap.clear()
        keymap[MotionEvent.AXIS_HAT_Y or 0x8000] = PadEvent.BUTTON_UP
        keymap[MotionEvent.AXIS_HAT_Y] = PadEvent.BUTTON_DOWN
        keymap[MotionEvent.AXIS_HAT_X or 0x8000] = PadEvent.BUTTON_LEFT
        keymap[MotionEvent.AXIS_HAT_X] = PadEvent.BUTTON_RIGHT
        keymap[MotionEvent.AXIS_LTRIGGER] = PadEvent.BUTTON_LEFT_TRIGGER
        keymap[MotionEvent.AXIS_RTRIGGER] = PadEvent.BUTTON_RIGHT_TRIGGER
        keymap[KeyEvent.KEYCODE_BUTTON_START] = PadEvent.BUTTON_START
        keymap[KeyEvent.KEYCODE_BUTTON_A] = PadEvent.BUTTON_A
        keymap[KeyEvent.KEYCODE_BUTTON_B] = PadEvent.BUTTON_B
        keymap[KeyEvent.KEYCODE_BUTTON_R1] = PadEvent.BUTTON_C
        keymap[KeyEvent.KEYCODE_BUTTON_X] = PadEvent.BUTTON_X
        keymap[KeyEvent.KEYCODE_BUTTON_Y] = PadEvent.BUTTON_Y
        keymap[KeyEvent.KEYCODE_BUTTON_L1] = PadEvent.BUTTON_Z
        keymap[MotionEvent.AXIS_X] = PadEvent.PERANALOG_AXIS_X
        keymap[MotionEvent.AXIS_Y] = PadEvent.PERANALOG_AXIS_Y
        keymap[MotionEvent.AXIS_LTRIGGER] = PadEvent.PERANALOG_AXIS_LTRIGGER
        keymap[MotionEvent.AXIS_RTRIGGER] = PadEvent.PERANALOG_AXIS_RTRIGGER
        keymap[KeyEvent.KEYCODE_BUTTON_SELECT] = PadEvent.MENU
        isRTriggerAnalog = true
        isRTriggerAnalog = true
    }

    fun loadSettings(settingFilename: String) {
        try {
            val yabroot = File(storage.rootPath)
            if (!yabroot.exists()) yabroot.mkdir()

            var json = ""
            val file = File(storage.rootPath + settingFilename)
            if (file.exists()) {
                val inputStream: InputStream = FileInputStream(storage.rootPath + settingFilename)
                val size = inputStream.available()
                val buffer = ByteArray(size)
                inputStream.read(buffer)
                inputStream.close()
                json = String(buffer)
            } else {
                if ((deviceType and (InputDevice.SOURCE_KEYBOARD or InputDevice.SOURCE_GAMEPAD)) == InputDevice.SOURCE_KEYBOARD) {
                    json =
                        "{\"BUTTON_UP\":19,\"BUTTON_DOWN\":20,\"BUTTON_LEFT\":21,\"BUTTON_RIGHT\":22,\"BUTTON_LEFT_TRIGGER\":65535,\"BUTTON_RIGHT_TRIGGER\":65535,\"BUTTON_START\":66,\"BUTTON_A\":54,\"BUTTON_B\":52,\"BUTTON_C\":31,\"BUTTON_X\":29,\"BUTTON_Y\":47,\"BUTTON_Z\":32,\"PERANALOG_AXIS_X\":65535,\"PERANALOG_AXIS_Y\":65535,\"PERANALOG_AXIS_LTRIGGER\":45,\"PERANALOG_AXIS_RTRIGGER\":33,\"MENU\":41,\"IS_LTRIGGER_ANALOG\":false,\"IS_RTRIGGER_ANALOG\":false}"
                } else {
                    // Retoro Pocket Pro
                    if (productId == 12289 && vendorId == 8226) {
                        json =
                            "{\"BUTTON_UP\":-2147450864,\"BUTTON_DOWN\":-2147483632,\"BUTTON_LEFT\":-2147450865,\"BUTTON_RIGHT\":-2147483633,\"BUTTON_LEFT_TRIGGER\":-2147483625,\"BUTTON_RIGHT_TRIGGER\":-2147483626,\"BUTTON_START\":108,\"BUTTON_A\":96,\"BUTTON_B\":97,\"BUTTON_C\":103,\"BUTTON_X\":99,\"BUTTON_Y\":100,\"BUTTON_Z\":102,\"PERANALOG_AXIS_X\":-1879048192,\"PERANALOG_AXIS_Y\":-1879048191,\"PERANALOG_AXIS_LTRIGGER\":-1879048169,\"PERANALOG_AXIS_RTRIGGER\":-1879048170,\"MENU\":109,\"IS_LTRIGGER_ANALOG\":true,\"IS_RTRIGGER_ANALOG\":true}"
                        // Odin
                    } else if (productId == 274 && vendorId == 8224) {
                        json =
                            "{\"BUTTON_UP\":-2147450864,\"BUTTON_DOWN\":-2147483632,\"BUTTON_LEFT\":-2147450865,\"BUTTON_RIGHT\":-2147483633,\"BUTTON_LEFT_TRIGGER\":65535,\"BUTTON_RIGHT_TRIGGER\":65535,\"BUTTON_START\":108,\"BUTTON_A\":96,\"BUTTON_B\":97,\"BUTTON_C\":103,\"BUTTON_X\":99,\"BUTTON_Y\":100,\"BUTTON_Z\":102,\"PERANALOG_AXIS_X\":-1879048192,\"PERANALOG_AXIS_Y\":-1879048191,\"PERANALOG_AXIS_LTRIGGER\":104,\"PERANALOG_AXIS_RTRIGGER\":105,\"MENU\":109,\"IS_LTRIGGER_ANALOG\":false,\"IS_RTRIGGER_ANALOG\":false}"

                        // nacon
                    } else if (productId == 773 && vendorId == 12933) {
                        json =
                            "{\"BUTTON_UP\":-2147450864,\"BUTTON_DOWN\":-2147483632,\"BUTTON_LEFT\":-2147450865,\"BUTTON_RIGHT\":-2147483633,\"BUTTON_LEFT_TRIGGER\":-2147483625,\"BUTTON_RIGHT_TRIGGER\":-2147483626,\"BUTTON_START\":108,\"BUTTON_A\":96,\"BUTTON_B\":97,\"BUTTON_C\":103,\"BUTTON_X\":99,\"BUTTON_Y\":100,\"BUTTON_Z\":102,\"PERANALOG_AXIS_X\":-1879048192,\"PERANALOG_AXIS_Y\":-1879048191,\"PERANALOG_AXIS_LTRIGGER\":-1879048169,\"PERANALOG_AXIS_RTRIGGER\":-1879048170,\"MENU\":109,\"IS_LTRIGGER_ANALOG\":true,\"IS_RTRIGGER_ANALOG\":true}"
                        // PlayStation 4
                    } else if (productId == 2508 && vendorId == 1356) {
                        json =
                            "{\"BUTTON_UP\":-2147450864,\"BUTTON_DOWN\":-2147483632,\"BUTTON_LEFT\":-2147450865,\"BUTTON_RIGHT\":-2147483633,\"BUTTON_LEFT_TRIGGER\":-2147483631,\"BUTTON_RIGHT_TRIGGER\":-2147483630,\"BUTTON_START\":108,\"BUTTON_A\":96,\"BUTTON_B\":97,\"BUTTON_C\":103,\"BUTTON_X\":99,\"BUTTON_Y\":100,\"BUTTON_Z\":102,\"PERANALOG_AXIS_X\":-1879048192,\"PERANALOG_AXIS_Y\":-1879048191,\"PERANALOG_AXIS_LTRIGGER\":-1879048175,\"PERANALOG_AXIS_RTRIGGER\":-1879048174,\"MENU\":109,\"IS_LTRIGGER_ANALOG\":true,\"IS_RTRIGGER_ANALOG\":true}"

                        // Anbermic RG405V
                    } else if (productId == 4353 && vendorId == 18507) {
                        json =
                            "{\"BUTTON_UP\":-2147450864,\"BUTTON_DOWN\":-2147483632,\"BUTTON_LEFT\":-2147450865,\"BUTTON_RIGHT\":-2147483633,\"BUTTON_LEFT_TRIGGER\":-2147483625,\"BUTTON_RIGHT_TRIGGER\":-2147483626,\"BUTTON_START\":108,\"BUTTON_A\":97,\"BUTTON_B\":96,\"BUTTON_C\":103,\"BUTTON_X\":100,\"BUTTON_Y\":99,\"BUTTON_Z\":102,\"PERANALOG_AXIS_X\":-1879048192,\"PERANALOG_AXIS_Y\":-1879048191,\"PERANALOG_AXIS_LTRIGGER\":-1879048169,\"PERANALOG_AXIS_RTRIGGER\":-1879048170,\"MENU\":109,\"IS_LTRIGGER_ANALOG\":true,\"IS_RTRIGGER_ANALOG\":true}"

                        // Generic XBox Controller
                    } else {
                        json =
                            "{\"BUTTON_UP\":-2147450864,\"BUTTON_DOWN\":-2147483632,\"BUTTON_LEFT\":-2147450865,\"BUTTON_RIGHT\":-2147483633,\"BUTTON_LEFT_TRIGGER\":-2147483625,\"BUTTON_RIGHT_TRIGGER\":-2147483626,\"BUTTON_START\":108,\"BUTTON_A\":96,\"BUTTON_B\":97,\"BUTTON_C\":103,\"BUTTON_X\":99,\"BUTTON_Y\":100,\"BUTTON_Z\":102,\"PERANALOG_AXIS_X\":-1879048192,\"PERANALOG_AXIS_Y\":-1879048191,\"PERANALOG_AXIS_LTRIGGER\":-1879048169,\"PERANALOG_AXIS_RTRIGGER\":-1879048170,\"MENU\":4,\"IS_LTRIGGER_ANALOG\":true,\"IS_RTRIGGER_ANALOG\":true}"
                    }
                }
            }

            Log.d("yabause", "keymap: $json")

            val jsonObject = JSONObject(json)
            keymap.clear()
            keymap[jsonObject.getInt("BUTTON_UP")] = PadEvent.BUTTON_UP
            keymap[jsonObject.getInt("BUTTON_DOWN")] = PadEvent.BUTTON_DOWN
            keymap[jsonObject.getInt("BUTTON_LEFT")] = PadEvent.BUTTON_LEFT
            keymap[jsonObject.getInt("BUTTON_RIGHT")] = PadEvent.BUTTON_RIGHT
            keymap[jsonObject.getInt("BUTTON_LEFT_TRIGGER")] = PadEvent.BUTTON_LEFT_TRIGGER
            keymap[jsonObject.getInt("BUTTON_RIGHT_TRIGGER")] = PadEvent.BUTTON_RIGHT_TRIGGER
            keymap[jsonObject.getInt("BUTTON_START")] = PadEvent.BUTTON_START
            keymap[jsonObject.getInt("BUTTON_A")] = PadEvent.BUTTON_A
            keymap[jsonObject.getInt("BUTTON_B")] = PadEvent.BUTTON_B
            keymap[jsonObject.getInt("BUTTON_C")] = PadEvent.BUTTON_C
            keymap[jsonObject.getInt("BUTTON_X")] = PadEvent.BUTTON_X
            keymap[jsonObject.getInt("BUTTON_Y")] = PadEvent.BUTTON_Y
            keymap[jsonObject.getInt("BUTTON_Z")] = PadEvent.BUTTON_Z
            keymap[jsonObject.getInt("PERANALOG_AXIS_X")] = PadEvent.PERANALOG_AXIS_X
            keymap[jsonObject.getInt("PERANALOG_AXIS_Y")] = PadEvent.PERANALOG_AXIS_Y
            keymap[jsonObject.getInt("PERANALOG_AXIS_LTRIGGER")] = PadEvent.PERANALOG_AXIS_LTRIGGER
            keymap[jsonObject.getInt("PERANALOG_AXIS_RTRIGGER")] = PadEvent.PERANALOG_AXIS_RTRIGGER
            keymap[jsonObject.getInt("MENU")] = PadEvent.MENU

            try {
                isLTriggerAnalog = jsonObject.getBoolean("IS_LTRIGGER_ANALOG")
            } catch (e: JSONException) {
                isLTriggerAnalog = true
            }

            try {
                isRTriggerAnalog = jsonObject.getBoolean("IS_RTRIGGER_ANALOG")
            } catch (e: JSONException) {
                isRTriggerAnalog = true
            }
        } catch (e: IOException) {
            e.printStackTrace()
            loadDefault()
        } catch (e: JSONException) {
            e.printStackTrace()
            loadDefault()
        }
    }

    // Limitaions
    // SS Controller adapter ... Left trigger and Right Trigger is not recognizeed as analog button. you need to skip them
    // Moga 000353 ... OK
    // SMACON ... Left trigger and Right Trigger is not recognizeed as analog button. you need to skip them
    // ipega ... OK, but too sensitive.

    // Helper method to get centered axis value considering flat (dead zone) region
    private fun getCenteredAxis(
        event: MotionEvent,
        device: InputDevice?,
        axis: Int,
    ): Float {
        val range: InputDevice.MotionRange? = device?.getMotionRange(axis, event.source)
        range?.apply {
            val value: Float = event.getAxisValue(axis)
            // Ignore axis values that are within the 'flat' region of the joystick axis center
            if (Math.abs(value) > flat) {
                return value
            }
        }
        return 0f
    }

    open fun onGenericMotionEvent(event: MotionEvent): Int {
        var rtn = 0
        if (event.isFromSource(InputDevice.SOURCE_CLASS_JOYSTICK)) {
            val motionEvent = event
            val inputDevice = event.device

            // Debug: Show all axis values (raw and centered)
            val axisX = motionEvent.getAxisValue(MotionEvent.AXIS_X)
            val axisY = motionEvent.getAxisValue(MotionEvent.AXIS_Y)
            val axisZ = motionEvent.getAxisValue(MotionEvent.AXIS_Z)
            val axisRZ = motionEvent.getAxisValue(MotionEvent.AXIS_RZ)
            val centeredX = getCenteredAxis(motionEvent, inputDevice, MotionEvent.AXIS_X)
            val centeredY = getCenteredAxis(motionEvent, inputDevice, MotionEvent.AXIS_Y)
            AxisDebugValues.rawX = axisX
            AxisDebugValues.rawY = axisY
            AxisDebugValues.rawZ = axisZ
            AxisDebugValues.rawRZ = axisRZ

            // Clamp to circle with center(128,128) and radius 128
            // Calculate distance from origin
            val distance = Math.sqrt((centeredX * centeredX + centeredY * centeredY).toDouble())
            val clampedX: Double
            val clampedY: Double
            if (distance > 1.0) {
                // Normalize to unit circle
                clampedX = centeredX / distance
                clampedY = centeredY / distance
            } else {
                clampedX = centeredX.toDouble()
                clampedY = centeredY.toDouble()
            }

            // Convert to 0-255 range (center 128, radius 128)
            val circleX = (clampedX * 128.0 + 128.0).toInt().coerceIn(0, 255)
            val circleY = (clampedY * 128.0 + 128.0).toInt().coerceIn(0, 255)

            // Send circle-clamped values directly for analog mode
            if (pdm.analogMode == PadManager.MODE_ANALOG &&
                playerIndex == 0 ||
                pdm.analogMode2 == PadManager.MODE_ANALOG &&
                playerIndex == 1
            ) {
                YabauseRunnable.axisWithDebug(PadEvent.PERANALOG_AXIS_X, playerIndex, circleX)
                YabauseRunnable.axisWithDebug(PadEvent.PERANALOG_AXIS_Y, playerIndex, circleY)
            }

            // Log.i("AxisDebug", "Raw X:$axisX Y:$axisY | Circle X:$circleX Y:$circleY")

            for ((btn, satBtn) in keymap) {
                // System.out.println(e.getKey() + " : " + e.getValue());

                // AnalogDevices
                if (pdm.analogMode == PadManager.MODE_ANALOG &&
                    playerIndex == 0 ||
                    pdm.analogMode2 == PadManager.MODE_ANALOG &&
                    playerIndex == 1
                ) {
                    // Skip AXIS_X/Y - already handled above with circle clamping
                    if (satBtn == PadEvent.PERANALOG_AXIS_X || satBtn == PadEvent.PERANALOG_AXIS_Y) {
                        continue
                    }

                    val axisId = btn and 0x00007FFF
                    val motionValue = motionEvent.getAxisValue(axisId)

                    if (satBtn == PadEvent.PERANALOG_AXIS_LTRIGGER && isLTriggerAnalog) {
                        var normalizeValue: Double = motionValue * 255.0
                        if (normalizeValue > 255.0) normalizeValue = 255.0
                        if (normalizeValue < 0.0) normalizeValue = 0.0
                        YabauseRunnable.axisWithDebug(satBtn, playerIndex, normalizeValue.toInt())
                        if (normalizeValue > 145.0) {
                            YabauseRunnable.press(PadEvent.BUTTON_LEFT_TRIGGER, playerIndex)
                        } else {
                            YabauseRunnable.release(PadEvent.BUTTON_LEFT_TRIGGER, playerIndex)
                        }
                        continue
                    } else if (satBtn == PadEvent.PERANALOG_AXIS_RTRIGGER && isRTriggerAnalog) {
                        var normalizeValue: Double = motionValue * 255.0
                        if (normalizeValue > 255.0) normalizeValue = 255.0
                        if (normalizeValue < 0.0) normalizeValue = 0.0
                        YabauseRunnable.axisWithDebug(satBtn, playerIndex, normalizeValue.toInt())
                        if (normalizeValue > 145.0) {
                            YabauseRunnable.press(PadEvent.BUTTON_RIGHT_TRIGGER, playerIndex)
                        } else {
                            YabauseRunnable.release(PadEvent.BUTTON_RIGHT_TRIGGER, playerIndex)
                        }
                        continue
                    }
                } else {
                    if (satBtn == PadEvent.PERANALOG_AXIS_X ||
                        satBtn == PadEvent.PERANALOG_AXIS_Y ||
                        satBtn == PadEvent.PERANALOG_AXIS_LTRIGGER ||
                        satBtn == PadEvent.PERANALOG_AXIS_RTRIGGER
                    ) {
                        continue
                    }
                }
                if (btn and -0x80000000 != 0) {
                    val motionValue = motionEvent.getAxisValue(btn and 0x00007FFF)
                    if (btn and 0x8000 != 0) { // Dir

                        if (java.lang.Float.compare(motionValue, -0.8f) < 0) { // ON
                            if (testMode) {
                                pdm.addDebugString("onGenericMotionEvent: On  $btn Satpad: $satBtn")
                            } else {
                                YabauseRunnable.press(satBtn, playerIndex)
                            }
                            // Log.d("BasicInputDevice", "onGenericMotionEvent: On  $btn Satpad: $satBtn")
                            rtn = 1
                        } else if (java.lang.Float.compare(motionValue, -0.5f) > 0) { // OFF
                            if (testMode) {
                                pdm.addDebugString("onGenericMotionEvent: Off  $btn Satpad: $satBtn")
                            } else {
                                YabauseRunnable.release(satBtn, playerIndex)
                            }
                            // Log.d("BasicInputDevice", "onGenericMotionEvent: Off  $btn Satpad: $satBtn")
                            rtn = 1
                        }
                    } else {
                        if (java.lang.Float.compare(motionValue, 0.8f) > 0) { // ON
                            if (testMode) {
                                pdm.addDebugString("onGenericMotionEvent: On  $btn Satpad: $satBtn")
                            } else {
                                YabauseRunnable.press(satBtn, playerIndex)
                            }
                            // Log.d("BasicInputDevice", "onGenericMotionEvent: On  $btn Satpad: $satBtn")
                            rtn = 1
                        } else if (java.lang.Float.compare(motionValue, 0.5f) < 0) { // OFF
                            if (testMode) {
                                pdm.addDebugString("onGenericMotionEvent: Off  $btn Satpad: $satBtn")
                            } else {
                                YabauseRunnable.release(satBtn, playerIndex)
                            }
                            // Log.d("BasicInputDevice", "onGenericMotionEvent: Off  $btn Satpad: $satBtn")
                            rtn = 1
                        }
                    }
                }

                // AnalogDvice
            }
        }
        return rtn
    }

    open fun onKeyDown(
        keyCode: Int,
        event: KeyEvent,
    ): Int {
        var lkeyCode = keyCode
        if (event.source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
            event.source and InputDevice.SOURCE_KEYBOARD == InputDevice.SOURCE_KEYBOARD
        ) {
            if (lkeyCode == KeyEvent.KEYCODE_BACK) {
                return PadManager.noActionMapped
            }
            if (lkeyCode == 0) {
                lkeyCode = event.scanCode
            }
            var padKey = keymap[lkeyCode]
            return if (padKey != null) {
                currentButtonState = currentButtonState or (1 shl padKey)
                if (showMenuCode == currentButtonState) {
                    for (i in 0..31) {
                        if (currentButtonState and (0x1 shl i) != 0) {
                            YabauseRunnable.release(i, playerIndex)
                        }
                    }
                    currentButtonState = 0 // clear
                    pdm.showMenu()
                    return PadManager.actionMapped
                }

                // Log.d(this.getClass().getSimpleName(),"currentButtonState = " + Integer.toHexString(currentButtonState) );
                event.startTracking()
                if (testMode) {
                    pdm.addDebugString("onKeyDown: $lkeyCode Satpad: $padKey")
                } else {
                    if ((
                            (pdm.analogMode == PadManager.MODE_ANALOG && playerIndex == 0) ||
                                (pdm.analogMode2 == PadManager.MODE_ANALOG && playerIndex == 1)
                        )
                    ) {
                        if (padKey == PadEvent.PERANALOG_AXIS_LTRIGGER && isLTriggerAnalog == false) {
                            YabauseRunnable.axisWithDebug(padKey, playerIndex, 255)
                            YabauseRunnable.press(PadEvent.BUTTON_LEFT_TRIGGER, playerIndex)
                        } else if (padKey == PadEvent.PERANALOG_AXIS_RTRIGGER && isRTriggerAnalog == false) {
                            YabauseRunnable.axisWithDebug(padKey, playerIndex, 255)
                            YabauseRunnable.press(PadEvent.BUTTON_RIGHT_TRIGGER, playerIndex)
                        } else if (padKey == PadEvent.PERANALOG_AXIS_X || padKey == PadEvent.PERANALOG_AXIS_Y) {
                        } else {
                            YabauseRunnable.press(padKey, playerIndex)
                        }
                    } else {
                        if (padKey == PadEvent.PERANALOG_AXIS_LTRIGGER) {
                            padKey = PadEvent.BUTTON_LEFT_TRIGGER
                        } else if (padKey == PadEvent.PERANALOG_AXIS_RTRIGGER) {
                            padKey = PadEvent.BUTTON_RIGHT_TRIGGER
                        }
                        YabauseRunnable.press(padKey, playerIndex)
                    }
                }
                PadManager.actionMapped // ignore this input
            } else {
                if (testMode) pdm.addDebugString("onKeyDown: $lkeyCode Satpad: none")
                PadManager.noActionMapped
            }
        }
        return 0
    }

    open fun onKeyUp(
        keyCode: Int,
        event: KeyEvent,
    ): Int {
        var lkeyCode = keyCode
        if (event.source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
            event.source and InputDevice.SOURCE_KEYBOARD == InputDevice.SOURCE_KEYBOARD
        ) {
            if (lkeyCode == KeyEvent.KEYCODE_BACK) {
                return noActionMapped
            }
            if (lkeyCode == 0) {
                lkeyCode = event.scanCode
            }
            if (!event.isCanceled) {
                var padKey = keymap[lkeyCode]
                return if (padKey != null) {
                    currentButtonState = currentButtonState and (1 shl padKey).inv()
                    // Log.d(this.getClass().getSimpleName(),"currentButtonState = " + Integer.toHexString(currentButtonState) );
                    if (testMode) {
                        pdm.addDebugString("onKeyUp: $lkeyCode Satpad: $padKey")
                    } else {
                        if (padKey == PadEvent.MENU) {
                            return toggleMenu
                        } else {
                            if ((
                                    (pdm.analogMode == PadManager.MODE_ANALOG && playerIndex == 0) ||
                                        (pdm.analogMode2 == PadManager.MODE_ANALOG && playerIndex == 1)
                                )
                            ) {
                                if (padKey == PadEvent.PERANALOG_AXIS_LTRIGGER && isLTriggerAnalog == false) {
                                    YabauseRunnable.axisWithDebug(padKey, playerIndex, 0)
                                    YabauseRunnable.release(PadEvent.BUTTON_LEFT_TRIGGER, playerIndex)
                                } else if (padKey == PadEvent.PERANALOG_AXIS_RTRIGGER && isRTriggerAnalog == false) {
                                    YabauseRunnable.axisWithDebug(padKey, playerIndex, 0)
                                    YabauseRunnable.release(PadEvent.BUTTON_RIGHT_TRIGGER, playerIndex)
                                } else if (padKey == PadEvent.PERANALOG_AXIS_X || padKey == PadEvent.PERANALOG_AXIS_Y) {
                                } else {
                                    YabauseRunnable.release(padKey, playerIndex)
                                }
                            } else {
                                if (padKey == PadEvent.PERANALOG_AXIS_LTRIGGER) {
                                    padKey = PadEvent.BUTTON_LEFT_TRIGGER
                                } else if (padKey == PadEvent.PERANALOG_AXIS_RTRIGGER) {
                                    padKey = PadEvent.BUTTON_RIGHT_TRIGGER
                                }
                                YabauseRunnable.release(padKey, playerIndex)
                            }
                        }
                    }
                    actionMapped // ignore this input
                } else {
                    if (testMode) pdm.addDebugString("onKeyUp: $lkeyCode Satpad: none")
                    noActionMapped
                }
            }
        }
        return noActionMapped
    }

    init {
        keymap = HashMap()
        this.pdm = pdm
    }
}

internal class SSController(
    pdm: PadManagerV16,
) : BasicInputDevice(pdm) {
    override fun onGenericMotionEvent(event: MotionEvent): Int = super.onGenericMotionEvent(event)

    override fun onKeyDown(
        keyCode: Int,
        event: KeyEvent,
    ): Int {
        if (event.scanCode == 0) {
            return actionMapped
        }
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            val padKey = keymap[keyCode]
            return if (padKey != null) {
                YabauseRunnable.press(padKey, playerIndex)
                actionMapped
            } else {
                noActionMapped
            }
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(
        keyCode: Int,
        event: KeyEvent,
    ): Int {
        if (event.scanCode == 0) {
            return actionMapped
        }
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            val padKey = keymap[keyCode]
            return if (padKey != null) {
                YabauseRunnable.release(padKey, playerIndex)
                actionMapped
            } else {
                noActionMapped
            }
        }
        return super.onKeyUp(keyCode, event)
    }
}

internal class PadManagerV16 : PadManager() {
    private val deviceIds: HashMap<String, Int?>
    val tag = "PadManagerV16"
    var debugMessage = String()
    private var testModeEnabled = false
    var currentMsgIndex = 0
    val maxMsgIndex = 24
    var debugMessageArray: Array<String?>
    val playerCount = 2
    var keymap: List<HashMap<Int, Int>>
    var pads: Array<BasicInputDevice?>

    override fun loadSettings() {
        if (pads[0] != null) {
            pads[0]!!.loadSettings("keymap_v2.json")
        }
        if (pads[1] != null) {
            pads[1]!!.loadSettings("keymap_player2_v2.json")
        }
    }

    override fun setTestMode(mode: Boolean) {
        testModeEnabled = mode
        if (pads[0] != null) pads[0]!!.testMode = testModeEnabled
        if (pads[1] != null) pads[1]!!.testMode = testModeEnabled
    }

    fun addDebugString(msg: String?) {
        debugMessageArray[currentMsgIndex] = msg
        currentMsgIndex++
        if (currentMsgIndex >= maxMsgIndex) currentMsgIndex = 0
    }

    override fun getStatusString(): String? {
        debugMessage = ""
        var start = currentMsgIndex
        for (i in 0 until maxMsgIndex) {
            if (debugMessageArray[start] != "") {
                debugMessage =
                    """
                    ${debugMessageArray[start]}
                    $debugMessage
                    """.trimIndent()
                start--
                if (start < 0) {
                    start = maxMsgIndex - 1
                }
            }
        }
        return debugMessage
    }

    override fun hasPad(): Boolean = deviceIds.size > 0

    override fun getDeviceList(): String = debugMessage

    override fun getDeviceCount(): Int = deviceIds.size

    override fun getName(index: Int): String? {
        if (index < 0 || index >= deviceIds.size) {
            return null
        }
        var counter = 0
        for (`val` in deviceIds.values) {
            if (counter == index) {
                val dev = InputDevice.getDevice(`val`!!)
                return dev?.name
            }
            counter++
        }
        return null
    }

    override fun getId(index: Int): String? {
        if (index < 0 || index >= deviceIds.size) {
            return null
        }
        var counter = 0
        for (key in deviceIds.keys) {
            if (counter == index) {
                return key
            }
            counter++
        }
        return null
    }

    override fun setPlayer1InputDevice(id: String?) {
        if (id == null) {
            pads[0] = BasicInputDevice(this)
            pads[0]?.selectedDeviceId = -1
            return
        }
        val did = deviceIds[id]
        if (did == null) {
            pads[0] = BasicInputDevice(this)
            pads[0]?.selectedDeviceId = -1
            pads[0]?.productId = -1
            pads[0]?.vendorId = -1
            pads[0]?.deviceType = -1
        } else {
            val dev = InputDevice.getDevice(did)
            if (dev?.name?.contains("HuiJia") == true) {
                pads[0] = SSController(this)
            } else {
                pads[0] = BasicInputDevice(this)
            }
            pads[0]?.selectedDeviceId = did
            pads[0]?.productId = dev?.productId ?: -1
            pads[0]?.vendorId = dev?.vendorId ?: -1
            pads[0]?.deviceType = dev?.sources ?: -1
        }
        pads[0]?.playerIndex = 0
        pads[0]?.loadSettings("keymap_v2.json")
        pads[0]?.testMode = testModeEnabled
        return
    }

    override fun getPlayer1InputDevice(): Int = if (pads[0] == null) -1 else pads[0]!!.selectedDeviceId

    override fun getPlayer1KeyboardMap(): Map<Int, Int>? {
        val pad = pads[0] ?: return null
        if (pad.selectedDeviceId < 0) return null
        val mask = InputDevice.SOURCE_KEYBOARD or InputDevice.SOURCE_GAMEPAD or InputDevice.SOURCE_JOYSTICK
        if ((pad.deviceType and mask) != InputDevice.SOURCE_KEYBOARD) return null

        // keymap is key code -> Saturn button; invert it. Unbound buttons use
        // 65535 and axes set the sign bit, so keep real key codes only.
        val result = HashMap<Int, Int>()
        for ((code, button) in pad.keymap) {
            if (code <= 0 || code > KeyEvent.getMaxKeyCode()) continue
            val target =
                when (button) {
                    // Digital triggers on a keyboard are bound through the analog slots.
                    PadEvent.PERANALOG_AXIS_LTRIGGER -> PadEvent.BUTTON_LEFT_TRIGGER
                    PadEvent.PERANALOG_AXIS_RTRIGGER -> PadEvent.BUTTON_RIGHT_TRIGGER
                    else -> button
                }
            if (target < PadEvent.BUTTON_LAST && !result.containsKey(target)) {
                result[target] = code
            } else if (target < PadEvent.BUTTON_LAST && button == target) {
                // A direct button binding wins over the analog-slot fallback.
                result[target] = code
            }
        }
        return result
    }

    override fun setPlayer2InputDevice(id: String?) {
        if (id == null) {
            pads[1] = BasicInputDevice(this)
            pads[1]!!.selectedDeviceId = -1
            return
        }
        val did = deviceIds[id]
        if (did == null) {
            pads[1] = BasicInputDevice(this)
            pads[1]!!.selectedDeviceId = -1
        } else {
            val dev = InputDevice.getDevice(did)
            if (dev?.name?.contains("HuiJia") == true) {
                pads[1] = SSController(this)
            } else {
                pads[1] = BasicInputDevice(this)
            }
            pads[1]?.selectedDeviceId = did
            pads[1]?.productId = dev?.productId ?: -1
            pads[1]?.vendorId = dev?.vendorId ?: -1
        }
        pads[1]?.playerIndex = 1
        pads[1]?.loadSettings("keymap_player2_v2.json")
        pads[1]?.testMode = testModeEnabled
        return
    }

    override fun getPlayer2InputDevice(): Int = if (pads[1] == null) -1 else pads[1]!!.selectedDeviceId

    fun findPlayerPad(deviceid: Int): BasicInputDevice? {
        for (i in 0 until playerCount) {
            if (pads[i] != null && deviceid == pads[i]!!.selectedDeviceId) {
                return pads[i]
            }
        }
        return null
    }

    override fun onGenericMotionEvent(event: MotionEvent): Int {
        if (pads[0] != null && pads[0]!!.selectedDeviceId == event.deviceId) {
            pads[0]!!.onGenericMotionEvent(event)
        }
        if (pads[1] != null && pads[1]!!.selectedDeviceId == event.deviceId) {
            pads[1]!!.onGenericMotionEvent(event)
        }
        return noActionMapped
    }

    override fun onKeyDown(
        keyCode: Int,
        event: KeyEvent,
    ): Int {
        var rtn = noActionMapped
        if (pads[0] != null && pads[0]!!.selectedDeviceId == event.deviceId) {
            rtn = rtn or pads[0]!!.onKeyDown(keyCode, event)
        }
        if (pads[1] != null && pads[1]!!.selectedDeviceId == event.deviceId) {
            rtn = rtn or pads[1]!!.onKeyDown(keyCode, event)
        }
        return rtn
    }

    override fun onKeyUp(
        keyCode: Int,
        event: KeyEvent,
    ): Int {
        var rtn = noActionMapped
        if (pads[0] != null && pads[0]!!.selectedDeviceId == event.deviceId) {
            rtn = rtn or pads[0]!!.onKeyUp(keyCode, event)
        }
        if (pads[1] != null && pads[1]!!.selectedDeviceId == event.deviceId) {
            rtn = rtn or pads[1]!!.onKeyUp(keyCode, event)
        }
        return rtn
    }

    init {
        deviceIds = HashMap()
        pads = arrayOfNulls(playerCount)
        keymap = ArrayList()
        for (i in 0 until playerCount) {
            pads[i] = null
            // pads[i] = new BasicInputDevice(this);
            // pads[i].selectedDeviceId = INVALID_DEVICE_ID;
        }
        val ids = InputDevice.getDeviceIds()
        for (deviceId in ids) {
            if (deviceId == -1) continue
            val dev = InputDevice.getDevice(deviceId)
            if (dev != null) {
                val sources = dev.sources ?: InputDevice.SOURCE_ANY
                if ((sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                    (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
                    (sources and InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD
                ) {

                    if ((
                            (sources and InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK &&
                                (sources and InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD
                        ) &&
                        dev.keyboardType == InputDevice.KEYBOARD_TYPE_NON_ALPHABETIC
                    ) {
                        continue
                    }

                    if (deviceIds[dev.descriptor] == null) {

                        if (dev.name in excludedDevices) {
                            continue
                        }

                        deviceIds[dev.descriptor] = deviceId
                    }
                }
                val isGamePad = sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD
                val isGameJoyStick = sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
                debugMessage +=
                    "Input Device:${dev.name} ID:${dev.descriptor} Product ID:${dev.productId} isGamePad?:$isGamePad isJoyStick?:$isGameJoyStick"
            }
        }

        // Setting moe
        currentMsgIndex = 0
        debugMessageArray = arrayOfNulls(maxMsgIndex)
        for (i in 0 until maxMsgIndex) {
            debugMessageArray[i] = ""
        }
        testModeEnabled = false
    }
}
