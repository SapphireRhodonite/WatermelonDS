package me.magnum.melonds.ui.romdetails

import android.content.Context
import androidx.documentfile.provider.DocumentFile
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.MicSource
import me.magnum.melonds.domain.model.VideoFiltering
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.rom.config.RomConfig
import me.magnum.melonds.domain.model.rom.config.RomGbaSlotConfig
import me.magnum.melonds.domain.repositories.LayoutsRepository
import me.magnum.melonds.ui.romdetails.model.RomConfigUiModel
import me.magnum.melonds.ui.romdetails.model.RomGbaSlotConfigUiModel

class RomDetailsUiMapper(
    private val context: Context,
    private val layoutsRepository: LayoutsRepository,
) {

    suspend fun mapRomConfigToUi(
        romConfig: RomConfig,
        globalRuntimeConsoleType: ConsoleType,
        globalRuntimeMicSource: MicSource,
        globalVideoRenderer: VideoRenderer,
        globalThreadedRendering: Boolean,
        globalInternalResolutionScaling: Int,
        globalVideoFiltering: VideoFiltering,
        globalRetroArchShaderPresetPath: String?,
        globalRetroArchShaderParameters: String?,
        hasValidRetroArchShaderRoot: Boolean,
        globalRetroAchievementsEnabled: Boolean,
    ): RomConfigUiModel {
        return RomConfigUiModel(
            runtimeConsoleType = romConfig.runtimeConsoleType,
            globalRuntimeConsoleType = globalRuntimeConsoleType,
            runtimeMicSource = romConfig.runtimeMicSource,
            globalRuntimeMicSource = globalRuntimeMicSource,
            layoutId = romConfig.layoutId,
            layoutName = romConfig.layoutId?.let { layoutsRepository.getLayout(it)?.name },
            globalLayoutName = layoutsRepository.getGlobalLayoutPlaceholder().name,
            gbaSlotConfig = mapGbaSlotConfigToUi(romConfig.gbaSlotConfig),
            customName = romConfig.customName,
            iconSource = romConfig.iconSource,
            useHgEngineFix = romConfig.useHgEngineFix,
            inputMode = romConfig.inputMode,
            videoRenderer = romConfig.videoRenderer,
            globalVideoRenderer = globalVideoRenderer,
            threadedRendering = romConfig.threadedRendering,
            globalThreadedRendering = globalThreadedRendering,
            internalResolutionScaling = romConfig.internalResolutionScaling,
            globalInternalResolutionScaling = globalInternalResolutionScaling,
            videoFiltering = romConfig.videoFiltering,
            globalVideoFiltering = globalVideoFiltering,
            retroArchShaderPresetPath = romConfig.retroArchShaderPresetPath,
            globalRetroArchShaderPresetPath = globalRetroArchShaderPresetPath,
            retroArchShaderParameters = romConfig.retroArchShaderParameters,
            globalRetroArchShaderParameters = globalRetroArchShaderParameters,
            hasValidRetroArchShaderRoot = hasValidRetroArchShaderRoot,
            retroAchievementsEnabled = romConfig.retroAchievementsEnabled,
            globalRetroAchievementsEnabled = globalRetroAchievementsEnabled,
        )
    }

    private fun mapGbaSlotConfigToUi(gbaSlotConfig: RomGbaSlotConfig): RomGbaSlotConfigUiModel {
        return when (gbaSlotConfig) {
            is RomGbaSlotConfig.None -> RomGbaSlotConfigUiModel(type = RomGbaSlotConfigUiModel.Type.None)
            is RomGbaSlotConfig.GbaRom -> RomGbaSlotConfigUiModel(
                type = RomGbaSlotConfigUiModel.Type.GbaRom,
                gbaRomPath = gbaSlotConfig.romPath?.let { DocumentFile.fromSingleUri(context, it)?.name },
                gbaSavePath = gbaSlotConfig.savePath?.let { DocumentFile.fromSingleUri(context, it)?.name },
            )
            RomGbaSlotConfig.RumblePak -> RomGbaSlotConfigUiModel(type = RomGbaSlotConfigUiModel.Type.RumblePak)
            RomGbaSlotConfig.MemoryExpansion -> RomGbaSlotConfigUiModel(type = RomGbaSlotConfigUiModel.Type.MemoryExpansion)
            RomGbaSlotConfig.AnalogInput -> RomGbaSlotConfigUiModel(type = RomGbaSlotConfigUiModel.Type.AnalogInput)
            RomGbaSlotConfig.MotionPakHomebrew -> RomGbaSlotConfigUiModel(type = RomGbaSlotConfigUiModel.Type.MotionPakHomebrew)
            RomGbaSlotConfig.MotionPakRetail -> RomGbaSlotConfigUiModel(type = RomGbaSlotConfigUiModel.Type.MotionPakRetail)
        }
    }
}
