package me.magnum.melonds.domain.repositories

import android.net.Uri
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow
import me.magnum.melonds.domain.model.*
import me.magnum.melonds.domain.model.camera.DSiCameraSourceType
import me.magnum.melonds.domain.model.input.SoftInputBehaviour
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomConfig
import me.magnum.melonds.ui.Theme
import java.util.UUID

interface SettingsRepository {
    suspend fun getEmulatorConfiguration(): EmulatorConfiguration
    suspend fun getEmulatorConfiguration(romConfig: RomConfig): EmulatorConfiguration

    fun getTheme(): Theme
    fun getFastForwardSpeedMultiplier(): Float
    fun getFrameLimitSpeedMultiplier(): Float
    fun isRewindEnabled(): Boolean
    fun isSustainedPerformanceModeEnabled(): Boolean
    fun isAppLogFileEnabled(): Boolean
    fun observeAppLogFileEnabled(): Flow<Boolean>
    fun isTouchScreenSystemGestureExclusionEnabled(): Boolean
    fun observeTouchScreenSystemGestureExclusionEnabled(): Flow<Boolean>
    fun shouldIgnoreDisplayCutoutInLayouts(): Boolean

    fun getRomSearchDirectories(): Array<Uri>
    fun clearRomSearchDirectories()
    fun getRomIconFiltering(): RomIconFiltering
    fun getRomCacheMaxSize(): SizeUnit

    fun getRomViewMode(): RomViewMode
    fun setRomViewMode(viewMode: RomViewMode)
    fun observeRomViewMode(): Flow<RomViewMode>

    fun isRaCoverEnabled(): Boolean
    fun observeRaCoverEnabled(): Flow<Boolean>


    fun getDefaultConsoleType(): ConsoleType
    fun observeDefaultConsoleType(): Flow<ConsoleType>
    fun getFirmwareConfiguration(): FirmwareConfiguration
    fun useCustomBios(): Boolean
    fun getDsBiosDirectory(): Uri?
    fun getDsiBiosDirectory(): Uri?
    fun clearBiosDirectories()
    fun isDldiSdCardEnabled(): Boolean
    fun getDldiSdCardDirectory(): Uri?
    fun getDldiSdCardImageSize(): Int
    fun showBootScreen(): Boolean
    fun isJitEnabled(): Boolean

    fun getCurrentVideoRenderer(): VideoRenderer
    fun getEffectiveVideoRenderer(romConfig: RomConfig): VideoRenderer
    fun setCurrentVideoRenderer(renderer: VideoRenderer)
    fun getVideoRenderer(): Flow<VideoRenderer>
    fun getVulkanDriverConfiguration(nativeLibraryDir: String): VulkanDriverConfiguration
    fun getVulkanDriverMode(): VulkanDriverMode
    fun setVulkanDriverMode(mode: VulkanDriverMode)
    fun getInstalledVulkanDrivers(): List<VulkanDriverInfo>
    fun getSelectedVulkanDriverId(): String?
    fun setSelectedVulkanDriver(id: String)
    fun getCustomVulkanDriverDisplayName(): String?
    fun setCustomVulkanDriver(id: String, driverDir: String, driverName: String, displayName: String)
    fun removeCustomVulkanDriver(id: String)
    fun clearCustomVulkanDrivers()
    fun getVideoInternalResolutionScaling(): Flow<Int>
    fun getVideoFiltering(): Flow<VideoFiltering>
    fun isThreadedRenderingEnabled(): Flow<Boolean>
    fun isVulkanFastPathEnabled(): Flow<Boolean>
    fun isRendererDebugToolsEnabled(): Flow<Boolean>
    fun isRendererDebugBgObjEnabled(): Flow<Boolean>
    fun isRendererDebugLatchTraceEnabled(): Flow<Boolean>
    fun getFpsCounterPosition(): FpsCounterPosition
    fun observeRetroArchShaderRootValid(): Flow<Boolean>
    fun observeRetroArchShaderPresetPath(): Flow<String?>
    fun observeRetroArchShaderParametersText(): Flow<String?>
    fun getExternalDisplayMode(): ExternalDisplayMode
    fun observeExternalDisplayMode(): Flow<ExternalDisplayMode>

    fun isExternalDisplayKeepAspectRationEnabled(): Boolean
    fun observeExternalDisplayKeepAspectRationEnabled(): Flow<Boolean>

    fun getDualScreenPreset(): DualScreenPreset
    fun observeDualScreenPreset(): Flow<DualScreenPreset>
    fun isDualScreenIntegerScaleEnabled(): Boolean
    fun observeDualScreenIntegerScaleEnabled(): Flow<Boolean>
    fun isDualScreenInternalFillHeightEnabled(): Boolean
    fun observeDualScreenInternalFillHeightEnabled(): Flow<Boolean>
    fun isDualScreenInternalFillWidthEnabled(): Boolean
    fun observeDualScreenInternalFillWidthEnabled(): Flow<Boolean>
    fun isDualScreenExternalFillHeightEnabled(): Boolean
    fun observeDualScreenExternalFillHeightEnabled(): Flow<Boolean>
    fun isDualScreenExternalFillWidthEnabled(): Boolean
    fun observeDualScreenExternalFillWidthEnabled(): Flow<Boolean>
    fun getDualScreenInternalVerticalAlignmentOverride(): ScreenAlignment?
    fun observeDualScreenInternalVerticalAlignmentOverride(): Flow<ScreenAlignment?>
    fun getDualScreenExternalVerticalAlignmentOverride(): ScreenAlignment?
    fun observeDualScreenExternalVerticalAlignmentOverride(): Flow<ScreenAlignment?>
    fun getDSiCameraSource(): DSiCameraSourceType
    fun getDSiCameraStaticImage(): Uri?

    fun isSoundEnabled(): Boolean
    fun getAudioLatency(): AudioLatency
    fun getMicSource(): MicSource
    fun observeMicSource(): Flow<MicSource>

    fun getRomSortingMode(): SortingMode
    fun getRomSortingOrder(): SortingOrder
    fun saveNextToRomFile(): Boolean
    fun useSrmExtensionForSaveFiles(): Boolean
    fun isAutoSaveStateOnExitEnabled(): Boolean
    fun isAutoLoadStateOnLaunchEnabled(): Boolean
    fun getSaveFileDirectory(): Uri?
    fun getSaveFileDirectory(rom: Rom): Uri
    fun getSaveStateLocation(rom: Rom): SaveStateLocation
    fun getSaveStateDirectory(rom: Rom): Uri?

    fun getControllerConfiguration(): ControllerConfiguration
    fun observeControllerConfiguration(): StateFlow<ControllerConfiguration>
    fun getSelectedLayoutId(): UUID
    fun getSoftInputBehaviour(): Flow<SoftInputBehaviour>
    fun isTouchHapticFeedbackEnabled(): Flow<Boolean>
    fun getTouchHapticFeedbackStrength(): Int
    fun getSoftInputOpacity(): Flow<Int>

    fun isRetroAchievementsRichPresenceEnabled(): Boolean
    fun isRetroAchievementsEnabled(): Boolean
    fun observeRetroAchievementsEnabled(): Flow<Boolean>
    fun isRetroAchievementsHardcoreEnabled(): Boolean
    fun isRetroAchievementsOfflineSoftcoreEnabled(): Boolean
    fun areRetroAchievementsUnofficialAchievementsEnabled(): Boolean
    fun isRetroAchievementsEncoreModeEnabled(): Boolean
    fun areRetroAchievementsActiveChallengeIndicatorsEnabled(): Boolean
    fun areRetroAchievementsProgressIndicatorsEnabled(): Boolean
    fun areRetroAchievementsLeaderboardIndicatorsEnabled(): Boolean

    fun areCheatsEnabled(): Boolean

    fun observeTheme(): Flow<Theme>
    fun observeRomIconFiltering(): Flow<RomIconFiltering>
    fun observeRomSearchDirectories(): Flow<Array<Uri>>
    fun observeSelectedLayoutId(): Flow<UUID>
    fun observeDSiCameraSource(): Flow<DSiCameraSourceType>
    fun observeDSiCameraStaticImage(): Flow<Uri?>

    fun setDsBiosDirectory(directoryUri: Uri)
    fun setDsiBiosDirectory(directoryUri: Uri)
    fun addRomSearchDirectory(directoryUri: Uri)
    fun setControllerConfiguration(controllerConfiguration: ControllerConfiguration)
    fun setRomSortingMode(sortingMode: SortingMode)
    fun setRomSortingOrder(sortingOrder: SortingOrder)
    fun setSelectedLayoutId(layoutId: UUID)

    fun setExternalDisplayKeepAspectRatioEnabled(enabled: Boolean)
    fun setDualScreenPreset(preset: DualScreenPreset)
    fun setDualScreenIntegerScaleEnabled(enabled: Boolean)
    fun setDualScreenInternalFillHeightEnabled(enabled: Boolean)
    fun setDualScreenInternalFillWidthEnabled(enabled: Boolean)
    fun setDualScreenExternalFillHeightEnabled(enabled: Boolean)
    fun setDualScreenExternalFillWidthEnabled(enabled: Boolean)
    fun setDualScreenInternalVerticalAlignmentOverride(alignment: ScreenAlignment?)
    fun setDualScreenExternalVerticalAlignmentOverride(alignment: ScreenAlignment?)

    fun observeRenderConfiguration(): Flow<RendererConfiguration>
    fun observeRenderConfiguration(romConfig: RomConfig): Flow<RendererConfiguration>
}
