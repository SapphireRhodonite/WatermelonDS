package me.magnum.melonds.domain.services

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import me.magnum.melonds.MelonDSAndroidInterface
import me.magnum.melonds.multiplayer.MultiplayerSession
import me.magnum.melonds.common.romprocessors.RomFileProcessorFactory
import me.magnum.melonds.domain.model.ConfigurationDirResult
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.emulator.validation.FirmwareLaunchPreconditionCheckResult
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.emulator.validation.RomLaunchPreconditionCheckResult
import me.magnum.melonds.domain.model.rom.config.RuntimeConsoleType
import me.magnum.melonds.domain.repositories.SettingsRepository
import java.nio.ByteBuffer
import java.nio.ByteOrder

class EmulatorLaunchPreconditionChecker(
    private val context: Context,
    private val configurationDirectoryVerifier: ConfigurationDirectoryVerifier,
    private val romFileProcessorFactory: RomFileProcessorFactory,
    private val dsiNandManager: DSiNandManager,
    private val settingsRepository: SettingsRepository,
) {

    suspend fun checkRomLaunchPreconditions(candidate: Rom): RomLaunchPreconditionCheckResult = withContext(Dispatchers.IO) {
        val rom = candidate.copy(resolvedDsiProfile = settingsRepository.getDsiProfile(candidate.config.dsiProfileId))
        val renderer = if (MultiplayerSession.active) VideoRenderer.VULKAN
            else settingsRepository.getEffectiveVideoRenderer(rom.config)
        getRendererValidationFailureOrNull(renderer)?.let {
            return@withContext when (it) {
                RendererValidationFailure.UNSUPPORTED -> RomLaunchPreconditionCheckResult.RendererUnsupported(VideoRenderer.VULKAN)
                RendererValidationFailure.INIT_FAILED -> RomLaunchPreconditionCheckResult.RendererInitFailed(VideoRenderer.VULKAN)
            }
        }

        val configurationDirResult = if (rom.isDsiWareTitle) {
            configurationDirectoryVerifier.checkDsiConfigurationDirectory(rom.resolvedDsiProfile)
        } else {
            getRomConfigurationDirectoryResult(rom)
        }
        if (configurationDirResult.status != ConfigurationDirResult.Status.VALID) {
            return@withContext RomLaunchPreconditionCheckResult.BiosConfigurationIncorrect(configurationDirResult)
        }

        if (rom.isDsiWareTitle) {
            val dsiWareCheckResult = checkDsiWarePreconditions(rom)
            if (dsiWareCheckResult !is RomLaunchPreconditionCheckResult.Success) {
                return@withContext dsiWareCheckResult
            }
        }

        return@withContext RomLaunchPreconditionCheckResult.Success(rom)
    }

    suspend fun checkFirmwareLaunchPreconditions(consoleType: ConsoleType): FirmwareLaunchPreconditionCheckResult = withContext(Dispatchers.IO) {
        val renderer = if (MultiplayerSession.active) VideoRenderer.VULKAN
            else settingsRepository.getCurrentVideoRenderer()
        getRendererValidationFailureOrNull(renderer)?.let {
            return@withContext when (it) {
                RendererValidationFailure.UNSUPPORTED -> FirmwareLaunchPreconditionCheckResult.RendererUnsupported(VideoRenderer.VULKAN)
                RendererValidationFailure.INIT_FAILED -> FirmwareLaunchPreconditionCheckResult.RendererInitFailed(VideoRenderer.VULKAN)
            }
        }

        val profile = settingsRepository.getDsiProfile()
        val configurationDirResult = if (consoleType == ConsoleType.DSi) {
            configurationDirectoryVerifier.checkDsiConfigurationDirectory(profile)
        } else {
            configurationDirectoryVerifier.checkDsConfigurationDirectory()
        }
        if (configurationDirResult.status != ConfigurationDirResult.Status.VALID) {
            return@withContext FirmwareLaunchPreconditionCheckResult.BiosConfigurationIncorrect(configurationDirResult)
        }

        return@withContext FirmwareLaunchPreconditionCheckResult.Success(consoleType, profile)
    }

    private suspend fun checkDsiWarePreconditions(rom: Rom): RomLaunchPreconditionCheckResult {
        if (rom.isInstalledDsiWareShortcut) {
            return checkInstalledDsiWareShortcutPreconditions(rom)
        }

        val romInfo = romFileProcessorFactory.getFileRomProcessorForDocument(rom.uri)?.getRomInfo(rom)

        if (romInfo == null) {
            return RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed(RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed.Reason.RomParseError)
        }

        // The DSi title ID is equal to the game code, but parsed as a Long in big-endian
        val dsiTitleIdByteData = romInfo.gameCode.encodeToByteArray()
        val dsiTitleId = ByteBuffer.wrap(dsiTitleIdByteData).order(ByteOrder.BIG_ENDIAN).getInt().toLong()
        val configuration = settingsRepository.getEmulatorConfiguration(rom.config, rom.resolvedDsiProfile)
        val isTitleInstalled = dsiNandManager.withNand(configuration) {
            listTitles().any { (it.titleId and 0xFFFFFFFFL) == (dsiTitleId and 0xFFFFFFFFL) }
        } ?: return RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed(RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed.Reason.NandError)

        if (!isTitleInstalled) {
            return RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed(RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed.Reason.TitleNotInstalled)
        }

        return RomLaunchPreconditionCheckResult.Success(rom)
    }

    private suspend fun checkInstalledDsiWareShortcutPreconditions(rom: Rom): RomLaunchPreconditionCheckResult {
        val installedTitleId = rom.installedDsiWareTitleId
            ?: return RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed(RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed.Reason.RomParseError)

        val configuration = settingsRepository.getEmulatorConfiguration(rom.config, rom.resolvedDsiProfile)
        val isTitleInstalled = dsiNandManager.withNand(configuration) {
            listTitles().any { (it.titleId and 0xFFFFFFFFL) == (installedTitleId and 0xFFFFFFFFL) }
        } ?: return RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed(RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed.Reason.NandError)

        if (!isTitleInstalled) {
            return RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed(RomLaunchPreconditionCheckResult.DSiWareTitleValidationFailed.Reason.TitleNotInstalled)
        }

        return RomLaunchPreconditionCheckResult.Success(rom)
    }

    private fun getRomConfigurationDirectoryResult(rom: Rom): ConfigurationDirResult {
        if (!settingsRepository.useCustomBios() && rom.config.runtimeConsoleType == RuntimeConsoleType.DEFAULT) {
            return ConfigurationDirResult(ConsoleType.DS, ConfigurationDirResult.Status.VALID, emptyArray(), emptyArray())
        }

        val romTargetConsoleType = rom.config.runtimeConsoleType.targetConsoleType ?: settingsRepository.getDefaultConsoleType()
        if (!settingsRepository.useCustomBios() && romTargetConsoleType == ConsoleType.DS) {
            return ConfigurationDirResult(ConsoleType.DS, ConfigurationDirResult.Status.VALID, emptyArray(), emptyArray())
        }

        return if (romTargetConsoleType == ConsoleType.DSi) {
            configurationDirectoryVerifier.checkDsiConfigurationDirectory(rom.resolvedDsiProfile)
        } else {
            configurationDirectoryVerifier.checkDsConfigurationDirectory()
        }
    }

    private suspend fun getRendererValidationFailureOrNull(renderer: VideoRenderer): RendererValidationFailure? {
        if (renderer != VideoRenderer.VULKAN) {
            return null
        }

        return withContext(Dispatchers.Default) {
            MelonDSAndroidInterface.configureVulkanDriver(
                settingsRepository.getVulkanDriverConfiguration(context.applicationInfo.nativeLibraryDir)
            )
            when {
                !MelonDSAndroidInterface.isVulkanRendererSupported() -> RendererValidationFailure.UNSUPPORTED
                !MelonDSAndroidInterface.canInitializeVulkanRenderer() -> RendererValidationFailure.INIT_FAILED
                else -> null
            }
        }
    }

    private enum class RendererValidationFailure {
        UNSUPPORTED,
        INIT_FAILED,
    }
}
