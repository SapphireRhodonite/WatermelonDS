package me.magnum.melonds.impl

import android.app.ActivityManager
import android.content.Context
import android.content.SharedPreferences
import android.content.SharedPreferences.OnSharedPreferenceChangeListener
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.core.content.edit
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.conflate
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.shareIn
import kotlinx.serialization.ExperimentalSerializationApi
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.decodeFromStream
import kotlinx.serialization.json.encodeToStream
import me.magnum.melonds.common.retroarch.RetroArchShaderPreset
import me.magnum.melonds.common.uridelegates.UriHandler
import me.magnum.melonds.domain.model.AudioBitrate
import me.magnum.melonds.domain.model.AudioInterpolation
import me.magnum.melonds.domain.model.AudioLatency
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.ControllerConfiguration
import me.magnum.melonds.domain.model.DldiSdCardConfiguration
import me.magnum.melonds.domain.model.DualScreenPreset
import me.magnum.melonds.domain.model.EmulatorConfiguration
import me.magnum.melonds.domain.model.ExternalDisplayMode
import me.magnum.melonds.domain.model.FirmwareConfiguration
import me.magnum.melonds.domain.model.FpsCounterPosition
import me.magnum.melonds.domain.model.MacAddress
import me.magnum.melonds.domain.model.MicSource
import me.magnum.melonds.domain.model.RendererConfiguration
import me.magnum.melonds.domain.model.RetroArchShaderConfiguration
import me.magnum.melonds.domain.model.RetroArchShaderSourceResolution
import me.magnum.melonds.domain.model.RomIconFiltering
import me.magnum.melonds.domain.model.RomViewMode
import me.magnum.melonds.domain.model.SaveStateLocation
import me.magnum.melonds.domain.model.ScreenAlignment
import me.magnum.melonds.domain.model.SizeUnit
import me.magnum.melonds.domain.model.SortingMode
import me.magnum.melonds.domain.model.SortingOrder
import me.magnum.melonds.domain.model.VideoFiltering
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.VulkanPipelineProfile
import me.magnum.melonds.domain.model.resolveThreadedRendering
import me.magnum.melonds.domain.model.VulkanDriverConfiguration
import me.magnum.melonds.domain.model.VulkanDriverInfo
import me.magnum.melonds.domain.model.VulkanDriverMode
import me.magnum.melonds.domain.model.camera.DSiCameraSourceType
import me.magnum.melonds.domain.model.input.SoftInputBehaviour
import me.magnum.melonds.domain.model.layout.LayoutConfiguration
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomConfig
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.impl.dtos.input.ControllerConfigurationDto
import me.magnum.melonds.impl.input.ControllerConfigurationFactory
import me.magnum.melonds.ui.Theme
import me.magnum.melonds.utils.enumValueOfIgnoreCase
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID
import kotlin.math.pow
import kotlin.jvm.Volatile

class SharedPreferencesSettingsRepository(
    private val context: Context,
    private val preferences: SharedPreferences,
    private val controllerConfigurationFactory: ControllerConfigurationFactory,
    private val json: Json,
    private val uriHandler: UriHandler,
    preferencesCoroutineScope: CoroutineScope,
    private val settingsBackupManager: SettingsBackupManager,
) : SettingsRepository, OnSharedPreferenceChangeListener {

    companion object {
        private const val TAG = "SPSettingsRepository"
        private const val CONTROLLER_CONFIG_FILE = "controller_config.json"
        private const val KEY_EXTERNAL_DISPLAY_MODE = "external_display_mode"
        private const val KEY_DUAL_SCREEN_PRESET = "dual_screen_preset"
        private const val KEY_DUAL_SCREEN_INTEGER_SCALE = "dual_screen_integer_scale"
        private const val KEY_DUAL_SCREEN_INTERNAL_FILL = "dual_screen_internal_fill_height"
        private const val KEY_DUAL_SCREEN_EXTERNAL_FILL = "dual_screen_external_fill_height"
        private const val KEY_DUAL_SCREEN_INTERNAL_FILL_WIDTH = "dual_screen_internal_fill_width"
        private const val KEY_DUAL_SCREEN_EXTERNAL_FILL_WIDTH = "dual_screen_external_fill_width"
        private const val KEY_DUAL_SCREEN_INTERNAL_VERTICAL_ALIGNMENT = "dual_screen_internal_vertical_alignment"
        private const val KEY_DUAL_SCREEN_EXTERNAL_VERTICAL_ALIGNMENT = "dual_screen_external_vertical_alignment"
        private const val GLES_3_2 = 0x30002
        private val EmptyRetroArchShaderConfiguration = RetroArchShaderConfiguration(
            presetPath = null,
            sourceResolution = RetroArchShaderSourceResolution.VULKAN_IR,
            passCount = 0,
            parameterOverrides = emptyMap(),
            clearHistory = false,
        )
    }

    private inline fun <reified T> getEnumPreference(key: String, default: T): T where T : Enum<T> {
        val value = preferences.getString(key, default.name.lowercase()) ?: return default
        return runCatching { enumValueOfIgnoreCase<T>(value) }
            .onFailure { Log.w(TAG, "Invalid enum preference $key=$value; using ${default.name}") }
            .getOrDefault(default)
    }

    private fun getTreeDocument(uri: Uri?, key: String): DocumentFile? {
        if (uri == null) {
            return null
        }

        return runCatching { DocumentFile.fromTreeUri(context, uri) }
            .onFailure { Log.w(TAG, "Could not access restored tree preference $key=$uri", it) }
            .getOrNull()
    }

    private fun getFileUri(document: DocumentFile?, fileName: String): Uri? {
        if (document == null) {
            return null
        }

        return runCatching { document.findFile(fileName)?.uri }
            .onFailure { Log.w(TAG, "Could not access restored file $fileName", it) }
            .getOrNull()
    }

    @OptIn(ExperimentalSerializationApi::class)
    private val controllerConfiguration by lazy {
        val initialConfiguration = try {
            val configFile = File(context.filesDir, CONTROLLER_CONFIG_FILE)
            configFile.inputStream().use {
                val loadedConfiguration = json.decodeFromStream<ControllerConfigurationDto>(it)
                loadedConfiguration.toControllerConfiguration()
            }
        } catch (_: Exception) {
            controllerConfigurationFactory.buildDefaultControllerConfiguration()
        }

        MutableStateFlow(initialConfiguration)
    }
    private val preferenceSharedFlows = mutableMapOf<String, MutableSharedFlow<Unit>>()
    private val renderConfigurationFlow: SharedFlow<RendererConfiguration>
    @Volatile private var cachedRetroArchShaderRoot: String? = null
    @Volatile private var cachedRetroArchShaderImportKey: String? = null
    private data class RetroArchPresetReferences(
        val shaders: List<String>,
        val textures: List<String>,
    )
    private data class CoreRenderConfigurationInputs(
        val renderer: VideoRenderer,
        val filtering: VideoFiltering,
        val threadedRenderingEnabled: Boolean,
        val resolutionScaling: Int,
        val vulkanPipelineProfile: VulkanPipelineProfile,
        val rendererDebugToolsEnabled: Boolean,
        val rendererDebugBgObjEnabled: Boolean,
        val rendererDebugLatchTraceEnabled: Boolean,
    )
    private data class CoverageFixConfigurationInputs(
        val enabled: Boolean,
        val coveragePx: Float,
        val depthBias: Float,
        val applyRepeat: Boolean,
        val applyClamp: Boolean,
        val debugClearMagenta: Boolean,
    )
    private data class RenderConfigurationInputs(
        val core: CoreRenderConfigurationInputs,
        val coverageFix: CoverageFixConfigurationInputs,
    )

    init {
        preferences.registerOnSharedPreferenceChangeListener(this)
        setDefaultThemeIfRequired()
        setDefaultMacAddressIfRequired()

        val coreRenderInputsFlow = combine(
            combine(
                getVideoRenderer(),
                getVideoFiltering(),
                isThreadedRenderingEnabled(),
                getVideoInternalResolutionScaling(),
                isVulkanFastPathEnabled(),
            ) { renderer, filtering, threadedRenderingEnabled, resolutionScaling, vulkanFastPathEnabled ->
                CoreRenderConfigurationInputs(
                    renderer,
                    filtering,
                    threadedRenderingEnabled,
                    resolutionScaling,
                    VulkanPipelineProfile.fromFastPathPreference(vulkanFastPathEnabled),
                    rendererDebugToolsEnabled = false,
                    rendererDebugBgObjEnabled = false,
                    rendererDebugLatchTraceEnabled = false,
                )
            },
            isRendererDebugToolsEnabled(),
        ) { coreInputs, rendererDebugToolsEnabled ->
            coreInputs.copy(rendererDebugToolsEnabled = rendererDebugToolsEnabled)
        }

        val coreWithBgObjFlow = combine(
            coreRenderInputsFlow,
            isRendererDebugBgObjEnabled(),
        ) { coreInputs, rendererDebugBgObjEnabled ->
            coreInputs.copy(rendererDebugBgObjEnabled = rendererDebugBgObjEnabled)
        }

        val fullCoreRenderInputsFlow = combine(
            coreWithBgObjFlow,
            isRendererDebugLatchTraceEnabled(),
        ) { coreInputs, rendererDebugLatchTraceEnabled ->
            coreInputs.copy(rendererDebugLatchTraceEnabled = rendererDebugLatchTraceEnabled)
        }

        val coverageFixInputsFlow = combine(
            combine(
                isConservativeCoverageEnabled(),
                getConservativeCoveragePx(),
                getConservativeCoverageDepthBias(),
                isConservativeCoverageApplyRepeatEnabled(),
                isConservativeCoverageApplyClampEnabled(),
            ) { enabled, coveragePx, depthBias, applyRepeat, applyClamp ->
                CoverageFixConfigurationInputs(
                    enabled,
                    coveragePx,
                    depthBias,
                    applyRepeat,
                    applyClamp,
                    debugClearMagenta = false,
                )
            },
            isDebug3dClearMagentaEnabled(),
        ) { inputs, debugClearMagenta ->
            inputs.copy(debugClearMagenta = debugClearMagenta)
        }

        val renderInputsFlow = combine(fullCoreRenderInputsFlow, coverageFixInputsFlow) { core, coverageFix ->
            RenderConfigurationInputs(core, coverageFix)
        }

        renderConfigurationFlow = combine(
            renderInputsFlow,
            observeRetroArchShaderConfiguration(),
        ) { renderInputs, retroArchShader ->
            val effectiveFiltering = when {
                renderInputs.core.renderer == VideoRenderer.VULKAN && !renderInputs.core.filtering.isSupportedByVulkan() -> VideoFiltering.NONE
                renderInputs.core.renderer == VideoRenderer.VULKAN &&
                    renderInputs.core.filtering == VideoFiltering.RETROARCH &&
                    retroArchShader.presetPath.isNullOrBlank() -> VideoFiltering.NONE
                renderInputs.core.renderer != VideoRenderer.VULKAN && !renderInputs.core.filtering.isSupportedByOpenGlSurface() -> VideoFiltering.NONE
                else -> renderInputs.core.filtering
            }
            val effectiveThreadedRendering = resolveThreadedRendering(
                renderInputs.core.renderer,
                renderInputs.core.threadedRenderingEnabled,
            )
            val coverageFixEnabled = renderInputs.core.renderer == VideoRenderer.OPENGL && renderInputs.coverageFix.enabled
            RendererConfiguration(
                renderInputs.core.renderer,
                effectiveFiltering,
                effectiveThreadedRendering,
                if (renderInputs.core.renderer == VideoRenderer.VULKAN) {
                    renderInputs.core.vulkanPipelineProfile
                } else {
                    VulkanPipelineProfile.COMPATIBILITY
                },
                renderInputs.core.resolutionScaling,
                renderInputs.core.rendererDebugToolsEnabled,
                renderInputs.core.rendererDebugBgObjEnabled,
                renderInputs.core.rendererDebugLatchTraceEnabled,
                coverageFixEnabled,
                renderInputs.coverageFix.coveragePx,
                renderInputs.coverageFix.depthBias,
                renderInputs.coverageFix.applyRepeat,
                renderInputs.coverageFix.applyClamp,
                renderInputs.coverageFix.debugClearMagenta,
                if (effectiveFiltering == VideoFiltering.RETROARCH) retroArchShader else EmptyRetroArchShaderConfiguration,
            )
        }.conflate().shareIn(preferencesCoroutineScope, SharingStarted.Lazily, replay = 1)
    }

    private fun setDefaultThemeIfRequired() {
        if (preferences.getString("theme", null) != null)
            return

        val defaultTheme = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) "system" else "light"
        preferences.edit {
            putString("theme", defaultTheme)
        }
    }

    private fun setDefaultMacAddressIfRequired() {
        if (preferences.getString("internal_mac_address", null) != null) {
            return
        }

        val macAddress = MacAddress.randomDsAddress()
        preferences.edit {
            putString("internal_mac_address", macAddress.toString())
        }
    }

    override suspend fun getEmulatorConfiguration(): EmulatorConfiguration {
        val consoleType = getDefaultConsoleType()
        val useCustomBios = useCustomBios()
        val dsBiosDirUri = getDsBiosDirectory()
        val dsiBiosDirUri = getDsiBiosDirectory()

        if ((consoleType == ConsoleType.DS && useCustomBios && dsBiosDirUri == null) || (consoleType == ConsoleType.DSi && (dsBiosDirUri == null || dsiBiosDirUri == null))) {
            Log.w(TAG, "BIOS directory preference is incomplete; load will fail gracefully if custom BIOS is required")
        }

        val dsDirDocument = getTreeDocument(dsBiosDirUri, "bios_dir")
        val dsiDirDocument = getTreeDocument(dsiBiosDirUri, "dsi_bios_dir")

        return EmulatorConfiguration(
            useCustomBios(),
            showBootScreen(),
            getFileUri(dsDirDocument, "bios7.bin"),
            getFileUri(dsDirDocument, "bios9.bin"),
            getFileUri(dsDirDocument, "firmware.bin"),
            getFileUri(dsiDirDocument, "bios7.bin"),
            getFileUri(dsiDirDocument, "bios9.bin"),
            getFileUri(dsiDirDocument, "firmware.bin"),
            getFileUri(dsiDirDocument, "nand.bin"),
            context.filesDir.absolutePath,
            getFastForwardSpeedMultiplier(),
            getFrameLimitSpeedMultiplier(),
            isRewindEnabled(),
            getRewindPeriod(),
            getRewindWindow(),
            isJitEnabled(),
            false,
            consoleType,
            isSoundEnabled(),
            getAudioInterpolation(),
            getAudioBitrate(),
            getVolume(),
            getAudioLatency(),
            getMicSource(),
            getFirmwareConfiguration(),
            renderConfigurationFlow.first(),
            DldiSdCardConfiguration(
                enabled = isDldiSdCardEnabled(),
                imagePath = File(context.filesDir, "dldi/dldi_sd.img").absolutePath,
                imageSize = getDldiSdCardImageSize(),
                folderSync = isDldiSdCardEnabled() && getDldiSdCardDirectory() != null,
                folderPath = File(context.filesDir, "dldi/sync").absolutePath,
            ),
        )
    }

    override suspend fun getEmulatorConfiguration(romConfig: RomConfig): EmulatorConfiguration {
        val globalConfiguration = getEmulatorConfiguration()
        return globalConfiguration.copy(
            rendererConfiguration = buildRomRendererConfiguration(
                baseConfiguration = globalConfiguration.rendererConfiguration,
                romConfig = romConfig,
                rootUri = observeRetroArchShaderRoot().first(),
                globalPresetRelativePath = observeRetroArchShaderPreset().first(),
                globalParameterText = observeRetroArchShaderParameterText().first(),
            ),
        )
    }

    override fun getTheme(): Theme {
        return getEnumPreference("theme", Theme.LIGHT)
    }

    override fun getFastForwardSpeedMultiplier(): Float {
        val speedMultiplierPreference = preferences.getString("fast_forward_speed_multiplier", "-1")!!
        return speedMultiplierPreference.toFloatOrNull() ?: -1.0f
    }

    override fun getFrameLimitSpeedMultiplier(): Float {
        val speedMultiplierPreference = preferences.getString("frame_limit_speed_multiplier", "1")!!
        return speedMultiplierPreference.toFloatOrNull()?.coerceIn(0.25f, 1.0f) ?: 1.0f
    }

    override fun isRewindEnabled(): Boolean {
        return preferences.getBoolean("enable_rewind", false)
    }

    override fun isSustainedPerformanceModeEnabled(): Boolean {
        return preferences.getBoolean("enable_sustained_performance", false)
    }

    override fun isAppLogFileEnabled(): Boolean {
        return preferences.getBoolean("system_app_log_file_enabled", false)
    }

    override fun observeAppLogFileEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("system_app_log_file_enabled") {
            isAppLogFileEnabled()
        }
    }

    override fun isTouchScreenSystemGestureExclusionEnabled(): Boolean {
        return preferences.getBoolean("system_disable_touch_gestures_on_touch_screen_area", false)
    }

    override fun observeTouchScreenSystemGestureExclusionEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("system_disable_touch_gestures_on_touch_screen_area") {
            isTouchScreenSystemGestureExclusionEnabled()
        }
    }

    override fun shouldIgnoreDisplayCutoutInLayouts(): Boolean {
        return preferences.getBoolean("system_ignore_display_cutout_in_layouts", false)
    }

    override fun getRomSearchDirectories(): Array<Uri> {
        val dirPreference = preferences.getStringSet("rom_search_dirs", emptySet())
        return dirPreference?.map { it.toUri() }?.toTypedArray() ?: emptyArray()
    }

    override fun clearRomSearchDirectories() {
        preferences.edit {
            putStringSet("rom_search_dirs", null)
        }
    }

    override fun getRomIconFiltering(): RomIconFiltering {
        return getEnumPreference("rom_icon_filtering", RomIconFiltering.NONE)
    }

    override fun getRomCacheMaxSize(): SizeUnit {
        // Default cache size step is 3, or 1GB
        val cacheSizeStepPreference = preferences.getInt("rom_cache_max_size", 3)
        // Cache size is 128MB * (cacheSizeStepPreference ^ 2)
        return SizeUnit.MB(128) * 2.toDouble().pow(cacheSizeStepPreference).toLong()
    }

    override fun getRomViewMode(): RomViewMode {
        val viewModePreference = preferences.getString("rom_view_mode", "grid") ?: "grid"
        return runCatching { enumValueOfIgnoreCase<RomViewMode>(viewModePreference) }
            .getOrDefault(RomViewMode.GRID)
    }

    override fun setRomViewMode(viewMode: RomViewMode) {
        preferences.edit {
            putString("rom_view_mode", viewMode.name.lowercase())
        }
    }

    override fun observeRomViewMode(): Flow<RomViewMode> {
        return getOrCreatePreferenceSharedFlow("rom_view_mode") {
            getRomViewMode()
        }
    }

    override fun isRaCoverEnabled(): Boolean {
        return preferences.getBoolean("rom_ra_covers_enabled", true)
    }

    override fun observeRaCoverEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("rom_ra_covers_enabled") {
            isRaCoverEnabled()
        }
    }


    override fun getDefaultConsoleType(): ConsoleType {
        return getEnumPreference("console_type", ConsoleType.DS)
    }

    override fun observeDefaultConsoleType(): Flow<ConsoleType> {
        return getOrCreatePreferenceSharedFlow("console_type") {
            getDefaultConsoleType()
        }
    }

    override fun getFirmwareConfiguration(): FirmwareConfiguration {
        val birthdayPreference = preferences.getString("firmware_settings_birthday", "01/01")!!
        val parts = birthdayPreference.split("/")
        val birthday = if (parts.size != 2) {
            Pair(1, 1)
        } else {
            val day = parts[0].toIntOrNull() ?: 1
            val month = parts[1].toIntOrNull() ?: 1
            Pair(day, month)
        }

        val useCustomBios = useCustomBios()
        var macAddress: String? = null
        val randomizeMacAddress = if (useCustomBios) {
            preferences.getBoolean("custom_randomize_mac_address", false)
        } else {
            var randomize = preferences.getBoolean("internal_randomize_mac_address", false)

            if (!randomize) {
                macAddress = preferences.getString("internal_mac_address", null)
                // If the MAC address is not defined, enable MAC address randomization
                if (macAddress == null) {
                    randomize = true
                }
            }
            randomize
        }

        return FirmwareConfiguration(
                preferences.getString("firmware_settings_nickname", "Player")!!,
                preferences.getString("firmware_settings_message", "Hello!")!!,
                preferences.getString("firmware_settings_language", "1")!!.toIntOrNull() ?: 1,
                preferences.getInt("firmware_settings_colour", 0),
                birthday.second,
                birthday.first,
                randomizeMacAddress,
                macAddress
        )
    }

    override fun useCustomBios(): Boolean {
        return preferences.getBoolean("use_custom_bios", false)
    }

    override fun getDsBiosDirectory(): Uri? {
        val dirPreference = preferences.getStringSet("bios_dir", null)?.firstOrNull()
        return dirPreference?.toUri()
    }

    override fun getDsiBiosDirectory(): Uri? {
        val dirPreference = preferences.getStringSet("dsi_bios_dir", null)?.firstOrNull()
        return dirPreference?.toUri()
    }

    override fun clearBiosDirectories() {
        preferences.edit {
            remove("bios_dir")
            remove("dsi_bios_dir")
        }
    }

    override fun isDldiSdCardEnabled(): Boolean {
        return preferences.getBoolean("system_dldi_sd_card_enabled", false)
    }

    override fun getDldiSdCardDirectory(): Uri? {
        val dirPreference = preferences.getStringSet("system_dldi_sd_card_dir", null)?.firstOrNull()
        return dirPreference?.toUri()
    }

    override fun getDldiSdCardImageSize(): Int {
        return preferences.getString("system_dldi_sd_card_image_size", "0")
            ?.toIntOrNull()
            ?.coerceIn(0, 5)
            ?: 0
    }

    override fun showBootScreen(): Boolean {
        return preferences.getBoolean("show_bios", false)
    }

    override fun isJitEnabled(): Boolean {
        val defaultJitEnabled = Build.SUPPORTED_64_BIT_ABIS.isNotEmpty()
        return preferences.getBoolean("enable_jit", defaultJitEnabled)
    }

    override fun getCurrentVideoRenderer(): VideoRenderer {
        val videoRendererPreference = preferences.getString("video_renderer", "software")!!
        return sanitizeVideoRenderer(
            runCatching { VideoRenderer.valueOf(videoRendererPreference.uppercase()) }
                .getOrDefault(VideoRenderer.SOFTWARE),
            fallback = VideoRenderer.SOFTWARE,
        )
    }

    override fun getEffectiveVideoRenderer(romConfig: RomConfig): VideoRenderer {
        return sanitizeVideoRenderer(romConfig.videoRenderer, fallback = getCurrentVideoRenderer())
    }

    override fun setCurrentVideoRenderer(renderer: VideoRenderer) {
        preferences.edit {
            putString("video_renderer", renderer.name.lowercase())
        }
    }

    override fun getVideoRenderer(): Flow<VideoRenderer> {
        return getOrCreatePreferenceSharedFlow("video_renderer") {
            getCurrentVideoRenderer()
        }
    }

    override fun getVulkanDriverConfiguration(nativeLibraryDir: String): VulkanDriverConfiguration {
        val selectedDriver = getSelectedVulkanDriver()?.withResolvedRuntimePath()
        val requestedMode = getVulkanDriverMode()
        val useCustomDriver = requestedMode == VulkanDriverMode.CUSTOM &&
            selectedDriver != null &&
            AdrenoVulkanDriverSupport.isSupported(context)
        val tmpDir = File(context.cacheDir, "adrenotools/tmp").apply { mkdirs() }

        return VulkanDriverConfiguration(
            mode = if (useCustomDriver) VulkanDriverMode.CUSTOM else VulkanDriverMode.SYSTEM,
            tmpLibDir = tmpDir.absolutePath,
            hookLibDir = nativeLibraryDir,
            customDriverDir = selectedDriver
                ?.takeIf { useCustomDriver }
                ?.driverDir
                ?.let { if (it.endsWith(File.separator)) it else it + File.separator },
            customDriverName = selectedDriver?.takeIf { useCustomDriver }?.driverName,
            customDriverDisplayName = selectedDriver?.takeIf { useCustomDriver }?.displayName,
        )
    }

    override fun getVulkanDriverMode(): VulkanDriverMode {
        return getEnumPreference("video_vulkan_driver_mode", VulkanDriverMode.SYSTEM)
    }

    override fun setVulkanDriverMode(mode: VulkanDriverMode) {
        preferences.edit(commit = true) {
            putString("video_vulkan_driver_mode", mode.name.lowercase())
        }
    }

    override fun getInstalledVulkanDrivers(): List<VulkanDriverInfo> {
        val storedDrivers = preferences.getString("video_vulkan_custom_drivers", null)
            ?.let { parseVulkanDrivers(it) }
            .orEmpty()
        if (storedDrivers.isNotEmpty()) {
            return storedDrivers
        }

        val legacyDriverName = preferences.getString("video_vulkan_custom_driver_name", null)
        val legacyDriverDir = preferences.getString("video_vulkan_custom_driver_dir", null)
        val legacyDisplayName = preferences.getString("video_vulkan_custom_driver_display_name", null)
        if (!legacyDriverName.isNullOrBlank() && !legacyDriverDir.isNullOrBlank() && File(legacyDriverDir).isDirectory) {
            return listOf(
                VulkanDriverInfo(
                    id = "legacy",
                    displayName = legacyDisplayName ?: legacyDriverName,
                    driverDir = legacyDriverDir,
                    driverName = legacyDriverName,
                )
            )
        }

        return emptyList()
    }

    override fun getSelectedVulkanDriverId(): String? {
        return preferences.getString("video_vulkan_selected_driver_id", null)
            ?: getInstalledVulkanDrivers().firstOrNull()?.id
    }

    override fun setSelectedVulkanDriver(id: String) {
        preferences.edit(commit = true) {
            putString("video_vulkan_selected_driver_id", id)
            putString("video_vulkan_driver_mode", VulkanDriverMode.CUSTOM.name.lowercase())
        }
    }

    override fun getCustomVulkanDriverDisplayName(): String? {
        return getSelectedVulkanDriver()?.displayName
    }

    override fun setCustomVulkanDriver(id: String, driverDir: String, driverName: String, displayName: String) {
        val drivers = getInstalledVulkanDrivers()
            .filterNot { it.id == id }
            .plus(
                VulkanDriverInfo(
                    id = id,
                    displayName = displayName,
                    driverDir = driverDir,
                    driverName = driverName,
                )
            )
        preferences.edit(commit = true) {
            putString("video_vulkan_custom_drivers", serializeVulkanDrivers(drivers))
            putString("video_vulkan_selected_driver_id", id)
            putString("video_vulkan_driver_mode", VulkanDriverMode.CUSTOM.name.lowercase())
            remove("video_vulkan_custom_driver_dir")
            remove("video_vulkan_custom_driver_name")
            remove("video_vulkan_custom_driver_display_name")
        }
    }

    override fun removeCustomVulkanDriver(id: String) {
        val remainingDrivers = getInstalledVulkanDrivers().filterNot { it.id == id }
        preferences.edit(commit = true) {
            putString("video_vulkan_custom_drivers", serializeVulkanDrivers(remainingDrivers))
            remove("video_vulkan_custom_driver_dir")
            remove("video_vulkan_custom_driver_name")
            remove("video_vulkan_custom_driver_display_name")
            if (preferences.getString("video_vulkan_selected_driver_id", null) == id) {
                remove("video_vulkan_selected_driver_id")
                putString("video_vulkan_driver_mode", VulkanDriverMode.SYSTEM.name.lowercase())
            }
        }
    }

    override fun clearCustomVulkanDrivers() {
        preferences.edit(commit = true) {
            remove("video_vulkan_custom_driver_name")
            remove("video_vulkan_custom_driver_dir")
            remove("video_vulkan_custom_driver_display_name")
            remove("video_vulkan_custom_drivers")
            remove("video_vulkan_selected_driver_id")
            putString("video_vulkan_driver_mode", VulkanDriverMode.SYSTEM.name.lowercase())
        }
    }

    private fun getSelectedVulkanDriver(): VulkanDriverInfo? {
        val selectedId = getSelectedVulkanDriverId() ?: return null
        return getInstalledVulkanDrivers().firstOrNull { it.id == selectedId }
    }

    private fun VulkanDriverInfo.withResolvedRuntimePath(): VulkanDriverInfo {
        if (File(driverDir, driverName).isFile) {
            return this
        }

        val repairedDriverFile = File(context.filesDir, "adreno-drivers")
            .walkTopDown()
            .firstOrNull { it.isFile && it.name == driverName }
            ?: return this

        return copy(driverDir = repairedDriverFile.parentFile?.absolutePath ?: driverDir)
    }

    private fun parseVulkanDrivers(text: String): List<VulkanDriverInfo> {
        return runCatching {
            val array = JSONArray(text)
            buildList {
                for (i in 0 until array.length()) {
                    val item = array.optJSONObject(i) ?: continue
                    val id = item.optString("id").takeIf { it.isNotBlank() } ?: continue
                    val displayName = item.optString("displayName").takeIf { it.isNotBlank() } ?: continue
                    val driverDir = item.optString("driverDir").takeIf { it.isNotBlank() } ?: continue
                    val driverName = item.optString("driverName").takeIf { it.isNotBlank() } ?: continue
                    add(VulkanDriverInfo(id, displayName, driverDir, driverName))
                }
            }
        }.getOrDefault(emptyList())
    }

    private fun serializeVulkanDrivers(drivers: List<VulkanDriverInfo>): String {
        val array = JSONArray()
        drivers.forEach { driver ->
            array.put(
                JSONObject().apply {
                    put("id", driver.id)
                    put("displayName", driver.displayName)
                    put("driverDir", driver.driverDir)
                    put("driverName", driver.driverName)
                }
            )
        }
        return array.toString()
    }

    override fun getVideoInternalResolutionScaling(): Flow<Int> {
        return getOrCreatePreferenceSharedFlow("video_internal_resolution") {
            val internalResolutionPreference = preferences.getString("video_internal_resolution", "1")!!
            internalResolutionPreference.toIntOrNull() ?: 1
        }
    }

    override fun getVideoFiltering(): Flow<VideoFiltering> {
        return getOrCreatePreferenceSharedFlow("video_filtering") {
            val filteringPreference = preferences.getString("video_filtering", "none")!!
            runCatching { VideoFiltering.valueOf(filteringPreference.uppercase()) }
                .getOrDefault(VideoFiltering.NONE)
        }
    }

    private fun observeRetroArchShaderConfiguration(): Flow<RetroArchShaderConfiguration> {
        return combine(
            observeRetroArchShaderRoot(),
            observeRetroArchShaderPreset(),
            observeRetroArchShaderParameters(),
            observeRetroArchShaderClearHistory(),
        ) { rootUri, presetRelativePath, parameters, clearHistory ->
            importRetroArchShader(rootUri, presetRelativePath, parameters, clearHistory)
        }
    }

    private fun observeRetroArchShaderRoot(): Flow<Uri?> {
        return getOrCreatePreferenceSharedFlow("video_retroarch_shader_root") {
            preferences.getStringSet("video_retroarch_shader_root", null)?.firstOrNull()?.toUri()
        }
    }

    private fun observeRetroArchShaderPreset(): Flow<String?> {
        return getOrCreatePreferenceSharedFlow("video_retroarch_shader_preset") {
            preferences.getString("video_retroarch_shader_preset", null)
        }
    }

    private fun observeRetroArchShaderParameters(): Flow<Map<String, Float>> {
        return getOrCreatePreferenceSharedFlow("video_retroarch_shader_parameters") {
            parseRetroArchShaderParameters(getRetroArchShaderParameterText())
        }
    }

    private fun observeRetroArchShaderParameterText(): Flow<String?> {
        return getOrCreatePreferenceSharedFlow("video_retroarch_shader_parameters") {
            getRetroArchShaderParameterText()
        }
    }

    private fun getRetroArchShaderParameterText(): String? {
        return preferences.getString("video_retroarch_shader_parameters", null)
    }

    private fun observeRetroArchShaderClearHistory(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("video_retroarch_shader_clear_history") {
            preferences.getBoolean("video_retroarch_shader_clear_history", false)
        }
    }

    private fun parseRetroArchShaderParameters(rawValue: String?): Map<String, Float> {
        if (rawValue.isNullOrBlank()) {
            return emptyMap()
        }

        return rawValue
            .lineSequence()
            .flatMap { it.split(',', ';').asSequence() }
            .mapNotNull { entry ->
                val parts = entry.split('=', limit = 2)
                if (parts.size != 2) {
                    return@mapNotNull null
                }
                val name = parts[0].trim()
                val value = parts[1].trim().toFloatOrNull()
                if (name.isBlank() || value == null) {
                    null
                } else {
                    name to value
                }
            }
            .toMap()
    }

    private fun importRetroArchShader(
        rootUri: Uri?,
        presetRelativePath: String?,
        parameterOverrides: Map<String, Float>,
        clearHistory: Boolean,
    ): RetroArchShaderConfiguration {
        val relativePath = normalizeRetroArchPresetPath(presetRelativePath) ?: return EmptyRetroArchShaderConfiguration
        rootUri ?: return EmptyRetroArchShaderConfiguration

        val importRoot = File(context.filesDir, "retroarch-shaders/current")
        val rootDocument = DocumentFile.fromTreeUri(context, rootUri)
        if (rootDocument == null || !rootDocument.exists() || !rootDocument.isDirectory) {
            Log.w(TAG, "Invalid RetroArch shader root: $rootUri")
            cachedRetroArchShaderRoot = null
            cachedRetroArchShaderImportKey = null
            return buildImportedRetroArchShaderConfiguration(
                importRoot = importRoot,
                relativePath = relativePath,
                parameterOverrides = parameterOverrides,
                clearHistory = clearHistory,
            )
        }

        val rootCacheKey = rootUri.toString()
        val importCacheKey = "$rootCacheKey\n$relativePath"
        if (cachedRetroArchShaderRoot != rootCacheKey ||
            cachedRetroArchShaderImportKey != importCacheKey ||
            !importRoot.isDirectory
        ) {
            try {
                importRoot.deleteRecursively()
                importRoot.mkdirs()
                copyRetroArchShaderPresetDependencies(rootDocument, relativePath, importRoot)
                cachedRetroArchShaderRoot = rootCacheKey
                cachedRetroArchShaderImportKey = importCacheKey
            } catch (e: Exception) {
                Log.e(TAG, "Failed to import RetroArch shader preset $relativePath from $rootUri", e)
                cachedRetroArchShaderRoot = null
                cachedRetroArchShaderImportKey = null
                return EmptyRetroArchShaderConfiguration
            }
        }

        val presetFile = File(importRoot, relativePath)
        if (!presetFile.exists() || !presetFile.isFile) {
            Log.w(TAG, "RetroArch shader preset not found after import: $relativePath")
            return EmptyRetroArchShaderConfiguration
        }

        val escapedReference = findRetroArchPresetReferenceOutsideRoot(presetFile, importRoot)
        if (escapedReference != null) {
            Log.w(
                TAG,
                "RetroArch shader preset references files outside selected root: " +
                    "$relativePath -> $escapedReference. Select the top-level slang-shaders folder.",
            )
            return EmptyRetroArchShaderConfiguration
        }

        return buildImportedRetroArchShaderConfiguration(
            importRoot = importRoot,
            relativePath = relativePath,
            parameterOverrides = parameterOverrides,
            clearHistory = clearHistory,
        )
    }

    private fun isRetroArchShaderRootValid(rootUri: Uri?): Boolean {
        val rootDocument = rootUri?.let { DocumentFile.fromTreeUri(context, it) } ?: return false
        return rootDocument.exists() && rootDocument.isDirectory
    }

    private fun buildImportedRetroArchShaderConfiguration(
        importRoot: File,
        relativePath: String,
        parameterOverrides: Map<String, Float>,
        clearHistory: Boolean,
    ): RetroArchShaderConfiguration {
        val presetFile = File(importRoot, relativePath)
        if (!presetFile.exists() || !presetFile.isFile) {
            Log.w(TAG, "RetroArch shader preset not available in import cache: $relativePath")
            return EmptyRetroArchShaderConfiguration
        }

        val presetAssignments = RetroArchShaderPreset.parseAssignments(presetFile.readText())
        val passCount = RetroArchShaderPreset.passCount(presetAssignments)
        val sourceResolution = if (RetroArchShaderPreset.requiresNativeDsSource(relativePath) { shaderRelativePath ->
                File(importRoot, shaderRelativePath).takeIf { it.isFile }?.readText()
            }) {
            RetroArchShaderSourceResolution.NATIVE
        } else {
            RetroArchShaderSourceResolution.VULKAN_IR
        }
        logRetroArchShaderImportDiagnostics(importRoot, relativePath, presetAssignments, passCount, sourceResolution)

        return RetroArchShaderConfiguration(
            presetPath = presetFile.absolutePath,
            sourceResolution = sourceResolution,
            passCount = passCount,
            parameterOverrides = parameterOverrides,
            clearHistory = clearHistory,
        )
    }

    private fun logRetroArchShaderImportDiagnostics(
        importRoot: File,
        presetRelativePath: String,
        assignments: Map<String, String>,
        passCount: Int,
        sourceResolution: RetroArchShaderSourceResolution,
    ) {
        val references = RetroArchPresetReferences(
            shaders = RetroArchShaderPreset.shaderReferences(assignments),
            textures = RetroArchShaderPreset.textureReferences(assignments),
        )
        Log.i(
            TAG,
            "RetroArchShaderImport: preset=$presetRelativePath " +
                "passes=$passCount source=${sourceResolution.name.lowercase()} " +
                "shaders=${references.shaders.size} textures=${references.textures.size}",
        )
        references.textures.forEachIndexed { index, rawReference ->
            val resolvedPath = RetroArchShaderPreset.resolveRelativePath(presetRelativePath, rawReference)
            val textureFile = resolvedPath?.let { File(importRoot, it) }
            Log.i(
                TAG,
                "RetroArchShaderImport: texture[$index] ref=$rawReference " +
                    "resolved=${resolvedPath ?: "<unsupported>"} " +
                    "exists=${textureFile?.isFile == true} " +
                    "bytes=${textureFile?.takeIf { it.isFile }?.length() ?: 0}",
            )
        }
    }

    private fun normalizeRetroArchPresetPath(rawPath: String?): String? {
        val normalized = rawPath
            ?.trim()
            ?.replace('\\', '/')
            ?.trimStart('/')
            ?.takeIf { it.isNotBlank() }
            ?: return null

        if (normalized.split('/').any { it == ".." }) {
            return null
        }
        return normalized.takeIf { it.endsWith(".slangp", ignoreCase = true) }
    }

    private fun findRetroArchPresetReferenceOutsideRoot(presetFile: File, root: File): String? {
        val rootCanonical = root.canonicalFile
        val references = parseRetroArchPresetReferences(presetFile).let { it.shaders + it.textures }

        return references.firstOrNull { reference ->
            val normalizedReference = reference.replace('\\', '/').trim()
            if (normalizedReference.isBlank() || normalizedReference.startsWith('/')) {
                false
            } else {
                val referencedFile = File(presetFile.parentFile, normalizedReference).canonicalFile
                !referencedFile.startsWithDirectory(rootCanonical)
            }
        }
    }

    private fun parseRetroArchPresetReferences(presetFile: File): RetroArchPresetReferences {
        val assignments = RetroArchShaderPreset.parseAssignments(presetFile.readText())

        return RetroArchPresetReferences(
            shaders = RetroArchShaderPreset.shaderReferences(assignments),
            textures = RetroArchShaderPreset.textureReferences(assignments),
        )
    }

    private fun parseRetroArchIncludeReferences(shaderFile: File): List<String> {
        return RetroArchShaderPreset.includeReferences(shaderFile.readText())
    }

    private fun File.startsWithDirectory(directory: File): Boolean {
        var current: File? = this
        while (current != null) {
            if (current == directory) {
                return true
            }
            current = current.parentFile
        }
        return false
    }

    private fun copyRetroArchShaderPresetDependencies(root: DocumentFile, presetRelativePath: String, destination: File) {
        val pending = ArrayDeque<String>()
        val copied = mutableSetOf<String>()
        pending.add(presetRelativePath)

        while (pending.isNotEmpty()) {
            val relativePath = pending.removeFirst()
            if (!copied.add(relativePath)) {
                continue
            }

            val source = findDocumentFile(root, relativePath)
                ?: throw IllegalArgumentException("RetroArch shader dependency not found: $relativePath")
            if (!source.isFile) {
                throw IllegalArgumentException("RetroArch shader dependency is not a file: $relativePath")
            }

            val target = File(destination, relativePath)
            target.parentFile?.mkdirs()
            context.contentResolver.openInputStream(source.uri)?.use { input ->
                target.outputStream().use { output ->
                    input.copyTo(output)
                }
            } ?: throw IllegalArgumentException("Unable to open RetroArch shader dependency: $relativePath")

            when (target.extension.lowercase()) {
                "slangp" -> {
                    val presetReferences = parseRetroArchPresetReferences(target)
                    (presetReferences.shaders + presetReferences.textures).forEach { reference ->
                        pending.add(resolveRetroArchRelativePath(relativePath, reference))
                    }
                }
                "slang", "inc", "h", "glsl" -> {
                    parseRetroArchIncludeReferences(target).forEach { reference ->
                        pending.add(resolveRetroArchRelativePath(relativePath, reference))
                    }
                }
            }
        }
    }

    private fun findDocumentFile(root: DocumentFile, relativePath: String): DocumentFile? {
        var current = root
        relativePath.split('/').forEach { segment ->
            if (segment.isBlank()) {
                return null
            }
            current = current.findFile(segment) ?: return null
        }
        return current
    }

    private fun resolveRetroArchRelativePath(baseRelativePath: String, rawReference: String): String {
        return RetroArchShaderPreset.resolveRelativePath(baseRelativePath, rawReference)
            ?: throw IllegalArgumentException("Unsupported RetroArch shader dependency path: $rawReference")
    }

    override fun isThreadedRenderingEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("enable_threaded_rendering") {
            preferences.getBoolean("enable_threaded_rendering", true)
        }
    }

    override fun isVulkanFastPathEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("video_vulkan_fastpath_enabled") {
            preferences.getBoolean("video_vulkan_fastpath_enabled", false)
        }
    }

    override fun isRendererDebugToolsEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("video_renderer_debug_tools_enabled") {
            preferences.getBoolean("video_renderer_debug_tools_enabled", false)
        }
    }

    override fun isRendererDebugBgObjEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("video_renderer_debug_bgobj_enabled") {
            preferences.getBoolean("video_renderer_debug_bgobj_enabled", false)
        }
    }

    override fun isRendererDebugLatchTraceEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("video_renderer_debug_latch_trace_enabled") {
            preferences.getBoolean("video_renderer_debug_latch_trace_enabled", false)
        }
    }

    override fun observeRetroArchShaderRootValid(): Flow<Boolean> {
        return observeRetroArchShaderRoot().map { isRetroArchShaderRootValid(it) }
    }

    override fun observeRetroArchShaderPresetPath(): Flow<String?> {
        return observeRetroArchShaderPreset()
    }

    override fun observeRetroArchShaderParametersText(): Flow<String?> {
        return observeRetroArchShaderParameterText()
    }

    private fun isConservativeCoverageEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("video_conservative_coverage_enabled") {
            preferences.getBoolean("video_conservative_coverage_enabled", false)
        }
    }

    private fun getConservativeCoveragePx(): Flow<Float> {
        return getOrCreatePreferenceSharedFlow("video_conservative_coverage_px") {
            (preferences.getInt("video_conservative_coverage_px", 150)).toFloat() / 100.0f
        }
    }

    private fun getConservativeCoverageDepthBias(): Flow<Float> {
        return getOrCreatePreferenceSharedFlow("video_conservative_coverage_depth_bias") {
            (preferences.getInt("video_conservative_coverage_depth_bias", 0)).toFloat() / 1_000_000.0f
        }
    }

    private fun isConservativeCoverageApplyRepeatEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("video_conservative_coverage_apply_repeat") {
            preferences.getBoolean("video_conservative_coverage_apply_repeat", true)
        }
    }

    private fun isConservativeCoverageApplyClampEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("video_conservative_coverage_apply_clamp") {
            preferences.getBoolean("video_conservative_coverage_apply_clamp", false)
        }
    }

    private fun isDebug3dClearMagentaEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("video_debug_3d_clear_magenta") {
            preferences.getBoolean("video_debug_3d_clear_magenta", false)
        }
    }

    override fun getFpsCounterPosition(): FpsCounterPosition {
        return getEnumPreference("fps_counter_position", FpsCounterPosition.HIDDEN)
    }

    override fun getExternalDisplayMode(): ExternalDisplayMode {
        return getEnumPreference(KEY_EXTERNAL_DISPLAY_MODE, ExternalDisplayMode.MELON_DUAL_DS)
    }

    override fun observeExternalDisplayMode(): Flow<ExternalDisplayMode> {
        return getOrCreatePreferenceSharedFlow(KEY_EXTERNAL_DISPLAY_MODE) {
            getExternalDisplayMode()
        }
    }

    override fun isExternalDisplayKeepAspectRationEnabled(): Boolean {
        return preferences.getBoolean("external_display_keep_ratio", true)
    }

    override fun observeExternalDisplayKeepAspectRationEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("external_display_keep_ratio") {
            isExternalDisplayKeepAspectRationEnabled()
        }
    }

    override fun getDualScreenPreset(): DualScreenPreset {
        return getEnumPreference(KEY_DUAL_SCREEN_PRESET, DualScreenPreset.OFF)
    }

    override fun observeDualScreenPreset(): Flow<DualScreenPreset> {
        return getOrCreatePreferenceSharedFlow(KEY_DUAL_SCREEN_PRESET) {
            getDualScreenPreset()
        }
    }

    override fun isDualScreenIntegerScaleEnabled(): Boolean {
        return preferences.getBoolean(KEY_DUAL_SCREEN_INTEGER_SCALE, false)
    }

    override fun observeDualScreenIntegerScaleEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow(KEY_DUAL_SCREEN_INTEGER_SCALE) {
            isDualScreenIntegerScaleEnabled()
        }
    }

    override fun isDualScreenInternalFillHeightEnabled(): Boolean {
        return preferences.getBoolean(KEY_DUAL_SCREEN_INTERNAL_FILL, false)
    }

    override fun observeDualScreenInternalFillHeightEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow(KEY_DUAL_SCREEN_INTERNAL_FILL) {
            isDualScreenInternalFillHeightEnabled()
        }
    }

    override fun isDualScreenInternalFillWidthEnabled(): Boolean {
        return preferences.getBoolean(KEY_DUAL_SCREEN_INTERNAL_FILL_WIDTH, false)
    }

    override fun observeDualScreenInternalFillWidthEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow(KEY_DUAL_SCREEN_INTERNAL_FILL_WIDTH) {
            isDualScreenInternalFillWidthEnabled()
        }
    }

    override fun isDualScreenExternalFillHeightEnabled(): Boolean {
        return preferences.getBoolean(KEY_DUAL_SCREEN_EXTERNAL_FILL, false)
    }

    override fun observeDualScreenExternalFillHeightEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow(KEY_DUAL_SCREEN_EXTERNAL_FILL) {
            isDualScreenExternalFillHeightEnabled()
        }
    }

    override fun isDualScreenExternalFillWidthEnabled(): Boolean {
        return preferences.getBoolean(KEY_DUAL_SCREEN_EXTERNAL_FILL_WIDTH, false)
    }

    override fun observeDualScreenExternalFillWidthEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow(KEY_DUAL_SCREEN_EXTERNAL_FILL_WIDTH) {
            isDualScreenExternalFillWidthEnabled()
        }
    }

    override fun getDualScreenInternalVerticalAlignmentOverride(): ScreenAlignment? {
        val value = preferences.getString(KEY_DUAL_SCREEN_INTERNAL_VERTICAL_ALIGNMENT, null) ?: return null
        return runCatching { enumValueOfIgnoreCase<ScreenAlignment>(value) }
            .onFailure { Log.w(TAG, "Invalid enum preference $KEY_DUAL_SCREEN_INTERNAL_VERTICAL_ALIGNMENT=$value; ignoring") }
            .getOrNull()
    }

    override fun observeDualScreenInternalVerticalAlignmentOverride(): Flow<ScreenAlignment?> {
        return getOrCreatePreferenceSharedFlow(KEY_DUAL_SCREEN_INTERNAL_VERTICAL_ALIGNMENT) {
            getDualScreenInternalVerticalAlignmentOverride()
        }
    }

    override fun getDualScreenExternalVerticalAlignmentOverride(): ScreenAlignment? {
        val value = preferences.getString(KEY_DUAL_SCREEN_EXTERNAL_VERTICAL_ALIGNMENT, null) ?: return null
        return runCatching { enumValueOfIgnoreCase<ScreenAlignment>(value) }
            .onFailure { Log.w(TAG, "Invalid enum preference $KEY_DUAL_SCREEN_EXTERNAL_VERTICAL_ALIGNMENT=$value; ignoring") }
            .getOrNull()
    }

    override fun observeDualScreenExternalVerticalAlignmentOverride(): Flow<ScreenAlignment?> {
        return getOrCreatePreferenceSharedFlow(KEY_DUAL_SCREEN_EXTERNAL_VERTICAL_ALIGNMENT) {
            getDualScreenExternalVerticalAlignmentOverride()
        }
    }

    override fun getDSiCameraSource(): DSiCameraSourceType {
        return getEnumPreference("dsi_camera_source", DSiCameraSourceType.PHYSICAL_CAMERAS)
    }

    override fun getDSiCameraStaticImage(): Uri? {
        val staticImagePreference = preferences.getStringSet("dsi_camera_static_image", null)?.firstOrNull()
        return staticImagePreference?.toUri()
    }

    override fun isSoundEnabled(): Boolean {
        return preferences.getBoolean("sound_enabled", true)
    }

    private fun getRewindPeriod(): Int {
        return preferences.getInt("rewind_period", 10)
    }

    private fun getRewindWindow(): Int {
        return preferences.getInt("rewind_window", 6) * 10
    }

    private fun getVolume(): Int {
        return preferences.getInt("volume", 256).coerceIn(0, 256)
    }

    private fun getAudioInterpolation(): AudioInterpolation {
        return getEnumPreference("audio_interpolation", AudioInterpolation.NONE)
    }

    private fun getAudioBitrate(): AudioBitrate {
        return getEnumPreference("audio_bitrate", AudioBitrate.AUTO)
    }

    override fun getAudioLatency(): AudioLatency {
        return getEnumPreference("audio_latency", AudioLatency.LOW)
    }

    override fun getMicSource(): MicSource {
        return getEnumPreference("mic_source", MicSource.BLOW)
    }

    override fun observeMicSource(): Flow<MicSource> {
        return getOrCreatePreferenceSharedFlow("mic_source") {
            getMicSource()
        }
    }

    override fun getRomSortingMode(): SortingMode {
        val sortingMode = preferences.getString("rom_sorting_mode", "alphabetically") ?: "alphabetically"
        return runCatching { SortingMode.valueOf(sortingMode.uppercase()) }
            .getOrDefault(SortingMode.ALPHABETICALLY)
    }

    override fun getRomSortingOrder(): SortingOrder {
        val sortingOrder = preferences.getString("rom_sorting_order", null)
        return if (sortingOrder == null) {
            getRomSortingMode().defaultOrder
        } else {
            runCatching { SortingOrder.valueOf(sortingOrder.uppercase()) }
                .getOrDefault(getRomSortingMode().defaultOrder)
        }
    }

    override fun saveNextToRomFile(): Boolean {
        return preferences.getBoolean("use_rom_dir", true)
    }

    override fun useSrmExtensionForSaveFiles(): Boolean {
        return preferences.getBoolean("save_file_use_srm_extension", false)
    }

    override fun isAutoSaveStateOnExitEnabled(): Boolean {
        return preferences.getBoolean("auto_save_state_on_exit", false)
    }

    override fun isAutoLoadStateOnLaunchEnabled(): Boolean {
        return preferences.getBoolean("auto_load_state_on_launch", false)
    }

    override fun getSaveFileDirectory(): Uri? {
        val dirPreference = preferences.getStringSet("sram_dir", null)?.firstOrNull()
        return dirPreference?.toUri()
    }

    override fun getSaveFileDirectory(rom: Rom): Uri {
        return if (!saveNextToRomFile() && getSaveFileDirectory() != null) {
            getSaveFileDirectory()!!
        } else {
            if (rom.parentTreeUri != null) {
                getRomParentDirectory(rom)
            } else {
                // We don't know the ROM's directory, so we can't save next to it. Put save file in an app folder
                val externalFilesDir = context.getExternalFilesDir(null)
                val saveFileDirectory = File(externalFilesDir, "saves")
                if (!saveFileDirectory.isDirectory && !saveFileDirectory.mkdirs()) {
                    throw Exception("Could not create internal save directory")
                }

                Uri.fromFile(saveFileDirectory)
            }
        }
    }

    override fun getSaveStateLocation(rom: Rom): SaveStateLocation {
        return getEnumPreference("save_state_location", SaveStateLocation.SAVE_DIR)
    }

    override fun getSaveStateDirectory(rom: Rom): Uri? {
        val saveStateLocation = getSaveStateLocation(rom)

        return when (saveStateLocation) {
            SaveStateLocation.SAVE_DIR -> getSaveFileDirectory(rom)
            SaveStateLocation.ROM_DIR -> getRomParentDirectory(rom)
            SaveStateLocation.INTERNAL_DIR -> {
                val saveStateDir = File(context.getExternalFilesDir(null), "savestates")
                if (!saveStateDir.isDirectory) {
                    saveStateDir.mkdirs()
                }
                DocumentFile.fromFile(saveStateDir).uri
            }
        }
    }

    private fun getRomParentDirectory(rom: Rom): Uri {
        return rom.parentTreeUri?.let {
            uriHandler.getUriTreeDocument(rom.parentTreeUri)?.uri
        } ?: throw Exception("Could not determine ROMs parent document")
    }

    override fun getControllerConfiguration(): ControllerConfiguration {
        return controllerConfiguration.value
    }

    override fun observeControllerConfiguration(): StateFlow<ControllerConfiguration> {
        return controllerConfiguration
    }

    override fun getSelectedLayoutId(): UUID {
        val id = preferences.getString("input_layout_id", null)
        return id?.let { UUID.fromString(it) } ?: LayoutConfiguration.DEFAULT_ID
    }

    override fun getSoftInputBehaviour(): Flow<SoftInputBehaviour> {
        return getOrCreatePreferenceSharedFlow("soft_input_behaviour") {
            val preference = preferences.getString("soft_input_behaviour", "hide_system_buttons_when_controller_connected")

            when (preference) {
                "always_visible" -> SoftInputBehaviour.ALWAYS_VISIBLE
                "hide_system_buttons_when_controller_connected" -> SoftInputBehaviour.HIDE_SYSTEM_BUTTONS_WHEN_CONTROLLERS_CONNECTED
                "hide_mapped_buttons_when_controller_connected" -> SoftInputBehaviour.HIDE_ALL_BUTTONS_ASSIGNED_TO_CONNECTED_CONTROLLERS
                "always_invisible" -> SoftInputBehaviour.ALWAYS_INVISIBLE
                else -> SoftInputBehaviour.HIDE_SYSTEM_BUTTONS_WHEN_CONTROLLERS_CONNECTED
            }
        }
    }

    override fun isTouchHapticFeedbackEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("input_touch_haptic_feedback_enabled") {
            preferences.getBoolean("input_touch_haptic_feedback_enabled", true)
        }
    }

    override fun getTouchHapticFeedbackStrength(): Int {
        val strength = preferences.getInt("input_touch_haptic_feedback_strength", 30)
        return strength.coerceIn(1, 100)
    }

    override fun getSoftInputOpacity(): Flow<Int> {
        return getOrCreatePreferenceSharedFlow("input_opacity") {
            preferences.getInt("input_opacity", 50)
        }
    }

    override fun isRetroAchievementsRichPresenceEnabled(): Boolean {
        return preferences.getBoolean("ra_rich_presence", true)
    }

    override fun isRetroAchievementsEnabled(): Boolean {
        return preferences.getBoolean("ra_enabled", true)
    }

    override fun observeRetroAchievementsEnabled(): Flow<Boolean> {
        return getOrCreatePreferenceSharedFlow("ra_enabled") {
            isRetroAchievementsEnabled()
        }
    }

    override fun isRetroAchievementsHardcoreEnabled(): Boolean {
        return preferences.getBoolean("ra_hardcore_enabled", false)
    }

    override fun isRetroAchievementsOfflineSoftcoreEnabled(): Boolean {
        return preferences.getBoolean("ra_offline_softcore_enabled", true)
    }

    override fun areRetroAchievementsUnofficialAchievementsEnabled(): Boolean {
        return preferences.getBoolean("ra_unofficial_enabled", false)
    }

    override fun isRetroAchievementsEncoreModeEnabled(): Boolean {
        return preferences.getBoolean("ra_encore_enabled", false)
    }

    override fun areRetroAchievementsActiveChallengeIndicatorsEnabled(): Boolean {
        return preferences.getBoolean("ra_active_challenge_indicators", true)
    }

    override fun areRetroAchievementsProgressIndicatorsEnabled(): Boolean {
        return preferences.getBoolean("ra_progress_indicators", true)
    }

    override fun areRetroAchievementsLeaderboardIndicatorsEnabled(): Boolean {
        return preferences.getBoolean("ra_leaderboard_indicators", true)
    }

    override fun areCheatsEnabled(): Boolean {
        return preferences.getBoolean("cheats_enabled", false)
    }

    override fun observeRomSearchDirectories(): Flow<Array<Uri>> {
        return getOrCreatePreferenceSharedFlow("rom_search_dirs") {
            getRomSearchDirectories()
        }
    }

    override fun observeSelectedLayoutId(): Flow<UUID> {
        return getOrCreatePreferenceSharedFlow("input_layout_id") {
            getSelectedLayoutId()
        }
    }

    override fun observeDSiCameraSource(): Flow<DSiCameraSourceType> {
        return getOrCreatePreferenceSharedFlow("dsi_camera_source") {
            getDSiCameraSource()
        }
    }

    override fun observeDSiCameraStaticImage(): Flow<Uri?> {
        return getOrCreatePreferenceSharedFlow("dsi_camera_static_image") {
            getDSiCameraStaticImage()
        }
    }

    override fun setDsBiosDirectory(directoryUri: Uri) {
        preferences.edit {
            putStringSet("bios_dir", setOf(directoryUri.toString()))
        }
    }

    override fun setDsiBiosDirectory(directoryUri: Uri) {
        preferences.edit {
            putStringSet("dsi_bios_dir", setOf(directoryUri.toString()))
        }
    }

    override fun addRomSearchDirectory(directoryUri: Uri) {
        preferences.edit {
            val existingDirectories = preferences.getStringSet("rom_search_dirs", emptySet())?.toMutableSet() ?: mutableSetOf()
            existingDirectories.add(directoryUri.toString())
            putStringSet("rom_search_dirs", existingDirectories)
        }
    }

    @OptIn(ExperimentalSerializationApi::class)
    override fun setControllerConfiguration(controllerConfiguration: ControllerConfiguration) {
        this.controllerConfiguration.value = controllerConfiguration

        try {
            val configFile = File(context.filesDir, CONTROLLER_CONFIG_FILE)
            val dto = ControllerConfigurationDto.fromControllerConfiguration(controllerConfiguration)
            configFile.outputStream().use {
                json.encodeToStream(dto, it)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to save controller configuration", e)
        }
        settingsBackupManager.requestMirrorWrite()
    }

    override fun setRomSortingMode(sortingMode: SortingMode) {
        preferences.edit {
            putString("rom_sorting_mode", sortingMode.toString().lowercase())
        }
    }

    override fun setRomSortingOrder(sortingOrder: SortingOrder) {
        preferences.edit {
            putString("rom_sorting_order", sortingOrder.toString().lowercase())
        }
    }

    override fun setSelectedLayoutId(layoutId: UUID) {
        preferences.edit {
            putString("input_layout_id", layoutId.toString())
        }
    }

    override fun setExternalDisplayKeepAspectRatioEnabled(enabled: Boolean) {
        preferences.edit {
            putBoolean("external_display_keep_ratio", enabled)
        }
    }

    override fun setDualScreenPreset(preset: DualScreenPreset) {
        preferences.edit {
            putString(KEY_DUAL_SCREEN_PRESET, preset.name.lowercase())
        }
    }

    override fun setDualScreenIntegerScaleEnabled(enabled: Boolean) {
        preferences.edit {
            putBoolean(KEY_DUAL_SCREEN_INTEGER_SCALE, enabled)
        }
    }

    override fun setDualScreenInternalFillHeightEnabled(enabled: Boolean) {
        preferences.edit {
            putBoolean(KEY_DUAL_SCREEN_INTERNAL_FILL, enabled)
        }
    }

    override fun setDualScreenInternalFillWidthEnabled(enabled: Boolean) {
        preferences.edit {
            putBoolean(KEY_DUAL_SCREEN_INTERNAL_FILL_WIDTH, enabled)
        }
    }

    override fun setDualScreenExternalFillHeightEnabled(enabled: Boolean) {
        preferences.edit {
            putBoolean(KEY_DUAL_SCREEN_EXTERNAL_FILL, enabled)
        }
    }

    override fun setDualScreenExternalFillWidthEnabled(enabled: Boolean) {
        preferences.edit {
            putBoolean(KEY_DUAL_SCREEN_EXTERNAL_FILL_WIDTH, enabled)
        }
    }

    override fun setDualScreenInternalVerticalAlignmentOverride(alignment: ScreenAlignment?) {
        preferences.edit {
            if (alignment == null) {
                remove(KEY_DUAL_SCREEN_INTERNAL_VERTICAL_ALIGNMENT)
            } else {
                putString(KEY_DUAL_SCREEN_INTERNAL_VERTICAL_ALIGNMENT, alignment.name.lowercase())
            }
        }
    }

    override fun setDualScreenExternalVerticalAlignmentOverride(alignment: ScreenAlignment?) {
        preferences.edit {
            if (alignment == null) {
                remove(KEY_DUAL_SCREEN_EXTERNAL_VERTICAL_ALIGNMENT)
            } else {
                putString(KEY_DUAL_SCREEN_EXTERNAL_VERTICAL_ALIGNMENT, alignment.name.lowercase())
            }
        }
    }

    override fun observeTheme(): Flow<Theme> {
        return getOrCreatePreferenceSharedFlow("theme") {
            getTheme()
        }
    }

    override fun observeRomIconFiltering(): Flow<RomIconFiltering> {
        return getOrCreatePreferenceSharedFlow("rom_icon_filtering") {
            getRomIconFiltering()
        }
    }

    private fun <T> getOrCreatePreferenceSharedFlow(preference: String, mapper: () -> T): Flow<T> {
        val preferenceFlow = preferenceSharedFlows.getOrPut(preference) {
            MutableSharedFlow<Unit>(replay = 1, onBufferOverflow = BufferOverflow.DROP_OLDEST).apply {
                // Immediately trigger an event to load the initial value
                tryEmit(Unit)
            }
        }

        return preferenceFlow.map { mapper() }
    }

    override fun onSharedPreferenceChanged(sharedPreferences: SharedPreferences, key: String?) {
        preferenceSharedFlows[key]?.tryEmit(Unit)
    }

    override fun observeRenderConfiguration(): Flow<RendererConfiguration> {
        return renderConfigurationFlow
    }

    override fun observeRenderConfiguration(romConfig: RomConfig): Flow<RendererConfiguration> {
        return combine(
            renderConfigurationFlow,
            observeRetroArchShaderRoot(),
            observeRetroArchShaderPreset(),
            observeRetroArchShaderParameterText(),
        ) { baseConfiguration, rootUri, globalPresetRelativePath, globalParameterText ->
            buildRomRendererConfiguration(
                baseConfiguration = baseConfiguration,
                romConfig = romConfig,
                rootUri = rootUri,
                globalPresetRelativePath = globalPresetRelativePath,
                globalParameterText = globalParameterText,
            )
        }
    }

    private fun buildRomRendererConfiguration(
        baseConfiguration: RendererConfiguration,
        romConfig: RomConfig,
        rootUri: Uri?,
        globalPresetRelativePath: String?,
        globalParameterText: String?,
    ): RendererConfiguration {
        val renderer = sanitizeVideoRenderer(romConfig.videoRenderer, fallback = baseConfiguration.renderer)
        val requestedFiltering = romConfig.videoFiltering ?: baseConfiguration.videoFiltering
        val retroArchShader = if (renderer == VideoRenderer.VULKAN && requestedFiltering == VideoFiltering.RETROARCH) {
            if (romConfig.retroArchShaderPresetPath == null && romConfig.retroArchShaderParameters == null) {
                baseConfiguration.retroArchShader
            } else {
                importRetroArchShader(
                    rootUri = rootUri,
                    presetRelativePath = romConfig.retroArchShaderPresetPath ?: globalPresetRelativePath,
                    parameterOverrides = parseRetroArchShaderParameters(romConfig.retroArchShaderParameters ?: globalParameterText),
                    clearHistory = false,
                )
            }
        } else {
            EmptyRetroArchShaderConfiguration
        }
        val effectiveFiltering = when {
            renderer == VideoRenderer.VULKAN && !requestedFiltering.isSupportedByVulkan() -> VideoFiltering.NONE
            renderer == VideoRenderer.VULKAN &&
                requestedFiltering == VideoFiltering.RETROARCH &&
                retroArchShader.presetPath.isNullOrBlank() -> VideoFiltering.NONE
            renderer != VideoRenderer.VULKAN && !requestedFiltering.isSupportedByOpenGlSurface() -> VideoFiltering.NONE
            else -> requestedFiltering
        }
        val threadedRendering = resolveThreadedRendering(
            renderer,
            romConfig.threadedRendering ?: baseConfiguration.threadedRendering,
        )

        return baseConfiguration.copy(
            renderer = renderer,
            videoFiltering = effectiveFiltering,
            threadedRendering = threadedRendering,
            internalResolutionScaling = romConfig.internalResolutionScaling ?: baseConfiguration.resolutionScaling,
            retroArchShader = if (effectiveFiltering == VideoFiltering.RETROARCH) retroArchShader else EmptyRetroArchShaderConfiguration,
        )
    }

    private fun sanitizeVideoRenderer(renderer: VideoRenderer?, fallback: VideoRenderer): VideoRenderer {
        return when (renderer) {
            null -> fallback
            VideoRenderer.OPENGL -> if (isOpenGlRendererSupported()) VideoRenderer.OPENGL else fallback
            VideoRenderer.COMPUTE -> if (isComputeRendererSupported()) VideoRenderer.COMPUTE else fallback
            else -> renderer
        }
    }

    private fun isOpenGlRendererSupported(): Boolean {
        val activityManager = context.getSystemService(ActivityManager::class.java)
        val deviceGlesVersion = activityManager?.deviceConfigurationInfo?.reqGlEsVersion ?: 0
        return deviceGlesVersion >= GLES_3_2
    }

    private fun isComputeRendererSupported(): Boolean {
        return isOpenGlRendererSupported() && Build.HARDWARE.equals("qcom", ignoreCase = true)
    }
}
