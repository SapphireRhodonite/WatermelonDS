package me.magnum.melonds.ui.romdetails

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import me.magnum.melonds.common.Permission
import me.magnum.melonds.common.UriPermissionManager
import me.magnum.melonds.domain.model.VideoFiltering
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomConfig
import me.magnum.melonds.domain.model.rom.config.RomInputMode
import me.magnum.melonds.domain.model.rom.config.RomGbaSlotConfig
import me.magnum.melonds.domain.repositories.RomsRepository
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.domain.repositories.RetroAchievementsRepository
import me.magnum.melonds.impl.RomIconProvider
import me.magnum.melonds.parcelables.RomParcelable
import me.magnum.melonds.ui.romdetails.model.RomConfigUiState
import me.magnum.melonds.ui.romdetails.model.RomConfigUpdateEvent
import me.magnum.melonds.ui.romdetails.model.RomGbaSlotConfigUiModel
import me.magnum.melonds.ui.romlist.RomIcon
import javax.inject.Inject

@HiltViewModel
class RomDetailsViewModel @Inject constructor(
    private val romDetailsUiMapper: RomDetailsUiMapper,
    private val romsRepository: RomsRepository,
    private val settingsRepository: SettingsRepository,
    private val retroAchievementsRepository: RetroAchievementsRepository,
    private val romIconProvider: RomIconProvider,
    private val uriPermissionManager: UriPermissionManager,
    savedStateHandle: SavedStateHandle,
) : ViewModel() {

    private data class GlobalVideoConfig(
        val renderer: me.magnum.melonds.domain.model.VideoRenderer,
        val threadedRendering: Boolean,
        val internalResolutionScaling: Int,
        val filtering: me.magnum.melonds.domain.model.VideoFiltering,
        val retroArchShaderPresetPath: String?,
        val retroArchShaderParameters: String?,
        val hasValidRetroArchShaderRoot: Boolean,
    )

    private val _rom = MutableStateFlow(savedStateHandle.get<RomParcelable>(RomDetailsActivity.KEY_ROM)!!.rom)
    val rom = _rom.asStateFlow()

    private val _romConfig = MutableStateFlow(_rom.value.config)

    val raCoverUrl = combine(
        _rom,
        retroAchievementsRepository.observeRomCoverIcons(),
        settingsRepository.observeRaCoverEnabled(),
    ) { rom, covers, enabled ->
        covers[rom.retroAchievementsHash]
            ?.takeIf { rom.config.iconSource.usesRetroAchievements(enabled, it.hasAchievements) }
            ?.url
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), null)

    init {
        refreshRom()
    }

    val romConfigUiState by lazy {
        val uiStateFlow = MutableStateFlow<RomConfigUiState>(RomConfigUiState.Loading)
        viewModelScope.launch {
            val globalCoreVideoConfig = combine(
                settingsRepository.getVideoRenderer(),
                settingsRepository.isThreadedRenderingEnabled(),
                settingsRepository.getVideoInternalResolutionScaling(),
                settingsRepository.getVideoFiltering(),
            ) { renderer, threadedRendering, internalResolutionScaling, filtering ->
                GlobalVideoConfig(
                    renderer = renderer,
                    threadedRendering = threadedRendering,
                    internalResolutionScaling = internalResolutionScaling,
                    filtering = filtering,
                    retroArchShaderPresetPath = null,
                    retroArchShaderParameters = null,
                    hasValidRetroArchShaderRoot = false,
                )
            }
            val globalShaderConfig = combine(
                settingsRepository.observeRetroArchShaderPresetPath(),
                settingsRepository.observeRetroArchShaderParametersText(),
                settingsRepository.observeRetroArchShaderRootValid(),
            ) { presetPath, parameters, hasValidRoot ->
                Triple(presetPath, parameters, hasValidRoot)
            }
            val globalRuntimeConfig = combine(
                settingsRepository.observeDefaultConsoleType(),
                settingsRepository.observeMicSource(),
                settingsRepository.observeDsiProfileSettings(),
            ) { consoleType, micSource, dsiProfiles ->
                Triple(consoleType, micSource, dsiProfiles)
            }
            combine(
                _romConfig,
                globalRuntimeConfig,
                globalCoreVideoConfig,
                globalShaderConfig,
                settingsRepository.observeRetroAchievementsEnabled(),
            ) { romConfig, globalRuntimeConfig, globalVideoConfig, shaderConfig, globalRetroAchievementsEnabled ->
                romDetailsUiMapper.mapRomConfigToUi(
                    romConfig = romConfig,
                    globalRuntimeConsoleType = globalRuntimeConfig.first,
                    globalRuntimeMicSource = globalRuntimeConfig.second,
                    globalVideoRenderer = globalVideoConfig.renderer,
                    globalThreadedRendering = globalVideoConfig.threadedRendering,
                    globalInternalResolutionScaling = globalVideoConfig.internalResolutionScaling,
                    globalVideoFiltering = globalVideoConfig.filtering,
                    globalRetroArchShaderPresetPath = shaderConfig.first,
                    globalRetroArchShaderParameters = shaderConfig.second,
                    hasValidRetroArchShaderRoot = shaderConfig.third,
                    globalRetroAchievementsEnabled = globalRetroAchievementsEnabled,
                ).copy(dsiProfileId = romConfig.dsiProfileId, dsiProfiles = globalRuntimeConfig.third)
            }.collect {
                uiStateFlow.value = RomConfigUiState.Ready(it)
            }
        }

        uiStateFlow.asStateFlow()
    }

    fun onRomConfigUpdateEvent(event: RomConfigUpdateEvent) {
        val currentRomConfig = _romConfig.value
        val newRomConfig = when(event) {
            is RomConfigUpdateEvent.RuntimeConsoleUpdate -> currentRomConfig.copy(runtimeConsoleType = event.newRuntimeConsole)
            is RomConfigUpdateEvent.RuntimeMicSourceUpdate -> currentRomConfig.copy(runtimeMicSource = event.newRuntimeMicSource)
            is RomConfigUpdateEvent.UseHgEngineFixUpdate -> currentRomConfig.copy(useHgEngineFix = event.enabled)
            is RomConfigUpdateEvent.InputModeUpdate -> currentRomConfig.copy(
                inputMode = event.inputMode,
                customControllerConfiguration = when {
                    event.inputMode != RomInputMode.CUSTOM -> currentRomConfig.customControllerConfiguration
                    currentRomConfig.customControllerConfiguration != null -> currentRomConfig.customControllerConfiguration
                    else -> settingsRepository.getControllerConfiguration().copy()
                },
            )
            is RomConfigUpdateEvent.LayoutUpdate -> currentRomConfig.copy(layoutId = event.newLayoutId)
            is RomConfigUpdateEvent.GbaSlotTypeUpdated -> currentRomConfig.let {
                val newGbaSlotConfig = when (event.type) {
                    RomGbaSlotConfigUiModel.Type.None -> RomGbaSlotConfig.None
                    RomGbaSlotConfigUiModel.Type.GbaRom -> RomGbaSlotConfig.GbaRom(null, null)
                    RomGbaSlotConfigUiModel.Type.RumblePak -> RomGbaSlotConfig.RumblePak
                    RomGbaSlotConfigUiModel.Type.MemoryExpansion -> RomGbaSlotConfig.MemoryExpansion
                    RomGbaSlotConfigUiModel.Type.AnalogInput -> RomGbaSlotConfig.AnalogInput
                    RomGbaSlotConfigUiModel.Type.MotionPakHomebrew -> RomGbaSlotConfig.MotionPakHomebrew
                    RomGbaSlotConfigUiModel.Type.MotionPakRetail -> RomGbaSlotConfig.MotionPakRetail
                }
                it.copy(gbaSlotConfig = newGbaSlotConfig)
            }
            is RomConfigUpdateEvent.GbaRomPathUpdate -> currentRomConfig.let {
                (currentRomConfig.gbaSlotConfig as? RomGbaSlotConfig.GbaRom)?.let { gbaConfig ->
                    it.copy(gbaSlotConfig = gbaConfig.copy(romPath = event.gbaRomPath))
                }
            }
            is RomConfigUpdateEvent.GbaSavePathUpdate -> currentRomConfig.let {
                (currentRomConfig.gbaSlotConfig as? RomGbaSlotConfig.GbaRom)?.let { gbaConfig ->
                    it.copy(gbaSlotConfig = gbaConfig.copy(savePath = event.gbaSavePath))
                }
            }
            is RomConfigUpdateEvent.CustomNameUpdate -> currentRomConfig.copy(customName = event.customName)
            is RomConfigUpdateEvent.DSiProfileUpdate -> currentRomConfig.copy(dsiProfileId = event.profileId)
            is RomConfigUpdateEvent.IconSourceUpdate -> currentRomConfig.copy(iconSource = event.iconSource)
            is RomConfigUpdateEvent.VideoRendererUpdate -> {
                val targetRenderer = event.videoRenderer
                val targetFiltering = currentRomConfig.videoFiltering
                currentRomConfig.copy(
                    videoRenderer = targetRenderer,
                    videoFiltering = targetFiltering?.takeIf {
                        targetRenderer == null || isFilteringSupported(targetRenderer, it)
                    },
                )
            }
            is RomConfigUpdateEvent.ThreadedRenderingUpdate -> currentRomConfig.copy(threadedRendering = event.threadedRendering)
            is RomConfigUpdateEvent.InternalResolutionScalingUpdate -> currentRomConfig.copy(internalResolutionScaling = event.internalResolutionScaling)
            is RomConfigUpdateEvent.VideoFilteringUpdate -> currentRomConfig.copy(videoFiltering = event.videoFiltering)
            is RomConfigUpdateEvent.RetroArchShaderPresetPathUpdate -> currentRomConfig.copy(retroArchShaderPresetPath = event.presetPath)
            is RomConfigUpdateEvent.RetroArchShaderParametersUpdate -> currentRomConfig.copy(retroArchShaderParameters = event.parameters)
            is RomConfigUpdateEvent.RetroAchievementsEnabledUpdate -> currentRomConfig.copy(retroAchievementsEnabled = event.enabled)
        }

        newRomConfig?.let { newConfig ->
            _romConfig.value = newConfig
            _rom.update { it.copy(config = newConfig) }
            saveRomConfig(newConfig)
        }
    }

    fun refreshRom() {
        viewModelScope.launch {
            val refreshedRom = romsRepository.getRomAtUri(_rom.value.uri) ?: return@launch
            val currentConfig = _romConfig.value
            val refreshedConfig = if (isDefaultConfig(refreshedRom) && !isDefaultConfig(_rom.value.copy(config = currentConfig))) {
                currentConfig
            } else {
                refreshedRom.config
            }
            _rom.value = refreshedRom.copy(config = refreshedConfig)
            _romConfig.value = refreshedConfig
        }
    }

    suspend fun getRomIcon(rom: Rom): RomIcon {
        val romIconBitmap = romIconProvider.getRomIcon(rom)
        val iconFiltering = settingsRepository.getRomIconFiltering()
        return RomIcon(romIconBitmap, iconFiltering)
    }

    private fun saveRomConfig(newConfig: RomConfig) {
        if (newConfig.gbaSlotConfig is RomGbaSlotConfig.GbaRom) {
            newConfig.gbaSlotConfig.romPath?.let { uriPermissionManager.persistFilePermissions(it, Permission.READ) }
            newConfig.gbaSlotConfig.savePath?.let { uriPermissionManager.persistFilePermissions(it, Permission.READ_WRITE) }
        }
        romsRepository.updateRomConfig(_rom.value, newConfig)
    }

    private fun isFilteringSupported(renderer: VideoRenderer, filtering: VideoFiltering): Boolean {
        return when (renderer) {
            VideoRenderer.VULKAN -> filtering.isSupportedByVulkan()
            else -> filtering.isSupportedByOpenGlSurface()
        }
    }

    private fun isDefaultConfig(rom: Rom): Boolean {
        val defaultConfig = if (rom.isDsiWareTitle) {
            RomConfig.forDsiWareTitle()
        } else {
            RomConfig.default()
        }
        return rom.config == defaultConfig
    }
}
