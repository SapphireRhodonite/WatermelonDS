package me.magnum.melonds.ui.emulator.model

import android.net.Uri
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.rom.Rom

sealed class EmulatorState {
    data object Uninitialized : EmulatorState()
    data class ValidatingRom(val rom: Rom, val saveDirectory: Uri? = null) : EmulatorState()
    data class LoadingRom(
        val vulkanCompileProgress: VulkanCompileProgress? = null,
        val retroAchievementsLoadStage: RetroAchievementsLoadStage? = null,
    ) : EmulatorState()
    data class ValidatingFirmware(val consoleType: ConsoleType) : EmulatorState()
    data class LoadingFirmware(
        val vulkanCompileProgress: VulkanCompileProgress? = null,
    ) : EmulatorState()
    data class RunningRom(val rom: Rom) : EmulatorState()
    data class RunningFirmware(val console: ConsoleType) : EmulatorState()

    data class RomLoadError(val reason: String? = null) : EmulatorState()
    data class FirmwareLoadError(val reason: MelonEmulator.FirmwareLoadResult) : EmulatorState()
    data class RomNotFoundError(val romPath: String) : EmulatorState()
    data class FrontendSaveSelectionRequired(val message: String) : EmulatorState()

    fun isRunning() = this is RunningRom || this is RunningFirmware
}

data class VulkanCompileProgress(
    val stageId: Int,
    val current: Int,
    val total: Int,
)

enum class RetroAchievementsLoadStage {
    FETCHING_LATEST_DATA,
}
