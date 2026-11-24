package me.magnum.melonds.ui.emulator.model

import android.content.Intent
import android.net.Uri
import androidx.core.net.toUri
import androidx.lifecycle.SavedStateHandle
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.extensions.parcelable
import me.magnum.melonds.parcelables.RomParcelable
import me.magnum.melonds.ui.emulator.EmulatorActivity

sealed class LaunchArgs {
    data class RomObject(val rom: Rom) : LaunchArgs()
    data class RomUri(val uri: Uri) : LaunchArgs()
    data class RomPath(val path: String) : LaunchArgs()
    data class Firmware(val consoleType: ConsoleType, val startMultiplayerSession: Boolean = false) : LaunchArgs() {
        fun resumes(runningConsole: ConsoleType): Boolean = !startMultiplayerSession && consoleType == runningConsole
    }

    companion object {
        fun fromSavedStateHandle(savedStateHandle: SavedStateHandle): LaunchArgs? {
            return if (savedStateHandle.get<Boolean>(EmulatorActivity.KEY_BOOT_FIRMWARE_ONLY) == true) {
                val consoleTypeParameter = savedStateHandle.get<Int>(EmulatorActivity.KEY_BOOT_FIRMWARE_CONSOLE)
                consoleTypeParameter?.let(ConsoleType.entries::getOrNull)?.let {
                    Firmware(it, savedStateHandle.get<Boolean>(EmulatorActivity.KEY_START_MULTIPLAYER_FIRMWARE) == true)
                }
            } else {
                romUri(savedStateHandle.get<Any>(EmulatorActivity.KEY_URI))
                    ?: savedStateHandle.get<String>(EmulatorActivity.KEY_PATH)?.let(::RomPath)
                    ?: savedStateHandle.get<RomParcelable>(EmulatorActivity.KEY_ROM)?.rom?.let(::RomObject)
            }
        }

        fun fromIntent(intent: Intent): LaunchArgs? {
            val extras = intent.extras
            val bootFirmwareOnly = extras?.getBoolean(EmulatorActivity.KEY_BOOT_FIRMWARE_ONLY) ?: false

            return if (bootFirmwareOnly) {
                val consoleTypeParameter = extras.getInt(EmulatorActivity.KEY_BOOT_FIRMWARE_CONSOLE, -1)
                ConsoleType.entries.getOrNull(consoleTypeParameter)?.let {
                    Firmware(it, extras.getBoolean(EmulatorActivity.KEY_START_MULTIPLAYER_FIRMWARE, false))
                }
            } else {
                @Suppress("DEPRECATION")
                val uri = intent.data ?: extras?.get(EmulatorActivity.KEY_URI)
                romUri(uri)
                    ?: extras?.getString(EmulatorActivity.KEY_PATH)?.let(::RomPath)
                    ?: extras?.parcelable<RomParcelable>(EmulatorActivity.KEY_ROM)?.rom?.let(::RomObject)
            }
        }

        private fun romUri(uri: Any?): RomUri? = when (uri) {
            is String -> RomUri(uri.toUri())
            is Uri -> RomUri(uri)
            else -> null
        }
    }
}
