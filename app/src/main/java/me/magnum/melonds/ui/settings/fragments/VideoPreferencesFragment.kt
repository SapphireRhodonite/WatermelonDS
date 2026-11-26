package me.magnum.melonds.ui.settings.fragments

import android.app.ActivityManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher
import android.view.KeyEvent
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.net.toUri
import androidx.core.content.getSystemService
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.lifecycleScope
import androidx.preference.EditTextPreference
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.PreferenceCategory
import androidx.preference.SwitchPreference
import dagger.hilt.android.AndroidEntryPoint
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.MelonDSAndroidInterface
import me.magnum.melonds.R
import me.magnum.melonds.common.DirectoryAccessValidator
import me.magnum.melonds.common.UriPermissionManager
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.VideoFiltering
import me.magnum.melonds.domain.model.DualScreenPreset
import me.magnum.melonds.domain.model.ScreenAlignment
import me.magnum.melonds.domain.model.VulkanDriverInfo
import me.magnum.melonds.domain.model.VulkanDriverMode
import me.magnum.melonds.domain.model.camera.DSiCameraSourceType
import me.magnum.melonds.domain.model.defaultExternalAlignment
import me.magnum.melonds.domain.model.defaultInternalAlignment
import me.magnum.melonds.ui.settings.PreferenceFragmentHelper
import me.magnum.melonds.ui.settings.PreferenceFragmentTitleProvider
import me.magnum.melonds.ui.settings.SettingsActivity
import me.magnum.melonds.ui.settings.preferences.InGameLockedSwitchPreference
import me.magnum.melonds.ui.settings.preferences.InGameLockedListPreference
import me.magnum.melonds.ui.settings.preferences.StoragePickerPreference
import me.magnum.melonds.extensions.addOnPreferenceChangeListener
import me.magnum.melonds.impl.AdrenoVulkanDriverSupport
import me.magnum.melonds.impl.AdrenoVulkanDriverManager
import me.magnum.melonds.utils.enumValueOfIgnoreCase
import androidx.appcompat.app.AlertDialog
import android.text.format.DateFormat
import android.text.format.Formatter
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.SimpleAdapter
import android.widget.TextView
import androidx.work.Constraints
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkInfo
import androidx.work.WorkManager
import java.io.File
import java.util.Date
import me.magnum.melonds.common.retroarch.RetroArchShaderRootResolver
import me.magnum.melonds.common.workers.RetroArchShaderInstallWorker
import me.magnum.melonds.domain.model.RetroArchShaderSource
import me.magnum.melonds.impl.RetroArchShaderLibraryManager
import me.magnum.melonds.impl.ShaderCompatibilityLog
import android.content.Intent
import androidx.appcompat.widget.SwitchCompat
import androidx.core.view.isVisible
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import javax.inject.Inject
import me.magnum.melonds.common.retroarch.RetroArchShaderPreset
import androidx.recyclerview.widget.RecyclerView
import androidx.recyclerview.widget.LinearLayoutManager
import android.annotation.SuppressLint
import android.widget.ImageView
import android.view.View

@AndroidEntryPoint
class VideoPreferencesFragment : BasePreferenceFragment(), PreferenceFragmentTitleProvider {

    private companion object {
        const val GLES_3_2 = 0x30002
        const val VULKAN_SYSTEM_DRIVER_VALUE = "system"
        const val SHADER_LOG_FILE_NAME = "librashader.log"
        const val FREEDRENO_SETTINGS_KEY = "video_freedreno_settings"
        const val SHADER_SETTINGS_KEY = "video_retroarch_shader_settings"
        const val HEAVY_PRESET_BADGE_THRESHOLD_MS = 60_000L
        const val PENDING_DSLITE_PRESET = "pending_dslite_preset"
        val DSLITE_PRESETS = listOf(
            R.string.video_dslite_color to "handheld/color-mod/dslite-color.slangp",
            R.string.video_dslite_lcd to "presets/handheld-plus-color-mod/lcd-grid-v2-dslite-color.slangp",
            R.string.video_dslite_motionblur to "presets/handheld-plus-color-mod/lcd-grid-v2-dslite-color-motionblur.slangp",
        )
    }

    private val helper by lazy { PreferenceFragmentHelper(this, uriPermissionManager, directoryAccessValidator) }
    @Inject lateinit var uriPermissionManager: UriPermissionManager
    @Inject lateinit var directoryAccessValidator: DirectoryAccessValidator
    @Inject lateinit var settingsRepository: SettingsRepository
    @Inject lateinit var shaderLibraryManager: RetroArchShaderLibraryManager
    @Inject lateinit var shaderCompatibilityLog: ShaderCompatibilityLog

    private val threadedRendererPreferences = mutableListOf<Preference>()
    private val highResRendererPreferences = mutableListOf<Preference>()
    private val rendererDebugPreferences = mutableListOf<Preference>()
    private val coverageFixPreferences = mutableListOf<Preference>()

    private lateinit var dualScreenPresetsPreference: Preference
    private lateinit var shaderSourcePreference: Preference
    private lateinit var shaderManagePreference: Preference
    private lateinit var shaderReportPreference: Preference
    private lateinit var shaderRootPreference: StoragePickerPreference
    private lateinit var shaderPresetPreference: ListPreference
    private var shaderSettingsPreference: Preference? = null
    private val shaderWeightCache = mutableMapOf<String, RetroArchShaderPreset.Weight>()
    private lateinit var adrenoVulkanDriverManager: AdrenoVulkanDriverManager
    private var retroArchPresetScanJob: Job? = null
    private var shaderInstallObserverJob: Job? = null
    private var shaderInstallProgressDialog: AlertDialog? = null
    private var shaderInstallProgressBar: ProgressBar? = null
    private var shaderInstallPhaseText: TextView? = null
    private var shaderInstallDetailText: TextView? = null
    private var pendingDsLitePreset: String? = null
    private val vulkanDriverImportLauncher = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        handleVulkanDriverImport(uri)
    }

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.pref_video, rootKey)
        pendingDsLitePreset = savedInstanceState?.getString(PENDING_DSLITE_PRESET)
            ?.takeIf { path -> DSLITE_PRESETS.any { it.second == path } }

        if (rootKey == FREEDRENO_SETTINGS_KEY) {
            setupFreedrenoSettingsSubScreen()
            return
        }

        if (rootKey == SHADER_SETTINGS_KEY) {
            setupShaderSettingsSubScreen()
            return
        }

        adrenoVulkanDriverManager = AdrenoVulkanDriverManager(requireContext(), settingsRepository)

        val launchedInGame = requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_IN_GAME, false)
        val rendererPreference = findPreference<InGameLockedListPreference>("video_renderer")!!
        val internalResolutionPreference = findPreference<InGameLockedListPreference>("video_internal_resolution")!!
        listOf(rendererPreference, internalResolutionPreference).forEach {
            it.isInGameLocked = launchedInGame
            it.inGameLockedMessageRes = R.string.video_setting_cannot_change_ingame
        }

        findPreference<InGameLockedSwitchPreference>("video_low_latency")!!.apply {
            isInGameLocked = launchedInGame
            inGameLockedMessageRes = R.string.video_setting_cannot_change_ingame
        }

        threadedRendererPreferences.apply {
            add(findPreference("enable_threaded_rendering")!!)
        }

        highResRendererPreferences.apply {
            add(internalResolutionPreference)
            add(findPreference("video_hacks_category")!!)
            add(findPreference("video_debug_3d_clear_magenta")!!)
        }

        rendererDebugPreferences.apply {
            add(findPreference("video_hacks_category")!!)
            add(findPreference("video_renderer_debug_tools_enabled")!!)
            add(findPreference("video_renderer_debug_bgobj_enabled")!!)
            add(findPreference("video_renderer_debug_latch_trace_enabled")!!)
        }

        coverageFixPreferences.apply {
            add(findPreference("video_conservative_coverage_enabled")!!)
            add(findPreference("video_conservative_coverage_px")!!)
            add(findPreference("video_conservative_coverage_apply_repeat")!!)
            add(findPreference("video_conservative_coverage_apply_clamp")!!)
            add(findPreference("video_conservative_coverage_depth_bias")!!)
        }

        findPreference<SwitchPreference>("video_renderer_debug_tools_enabled")?.setOnPreferenceChangeListener { _, newValue ->
            val on = newValue as Boolean
            findPreference<Preference>("video_renderer_debug_bgobj_enabled")?.isVisible = on
            findPreference<Preference>("video_renderer_debug_latch_trace_enabled")?.isVisible = on
            true
        }
        findPreference<SwitchPreference>("video_conservative_coverage_enabled")?.setOnPreferenceChangeListener { _, newValue ->
            val on = newValue as Boolean
            listOf(
                "video_conservative_coverage_px",
                "video_conservative_coverage_apply_repeat",
                "video_conservative_coverage_apply_clamp",
                "video_conservative_coverage_depth_bias",
            ).forEach { findPreference<Preference>(it)?.isVisible = on }
            true
        }

        val videoFilteringPreference = findPreference<InGameLockedListPreference>("video_filtering")!!
        videoFilteringPreference.isInGameLocked = launchedInGame &&
            requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_LOCK_VIDEO_FILTERING, false)
        videoFilteringPreference.inGameLockedMessageRes = R.string.cannot_change_use_rom_settings
        findPreference<Preference>("video_dslite_preset")!!.apply {
            isEnabled = !videoFilteringPreference.isInGameLocked
            setOnPreferenceClickListener {
                showDsLitePresetDialog()
                true
            }
        }
        val dsiCameraSourcePreference = findPreference<ListPreference>("dsi_camera_source")!!
        val dsiCameraImagePreference = findPreference<StoragePickerPreference>("dsi_camera_static_image")!!
        val retroArchShaderRootPreference = findPreference<StoragePickerPreference>("video_retroarch_shader_root")!!
        val retroArchShaderPresetPreference = findPreference<ListPreference>("video_retroarch_shader_preset")!!
        val retroArchShaderParametersPreference = findPreference<EditTextPreference>("video_retroarch_shader_parameters")
        val retroArchShaderClearHistoryPreference = findPreference<SwitchPreference>("video_retroarch_shader_clear_history")
        val vulkanDriverCategory = findPreference<PreferenceCategory>("video_vulkan_driver_category")!!
        val vulkanDriverModePreference = findPreference<ListPreference>("video_vulkan_driver_mode")!!
        val vulkanDriverImportPreference = findPreference<Preference>("video_vulkan_driver_import")!!
        val vulkanDriverRemovePreference = findPreference<Preference>("video_vulkan_driver_remove")!!
        dualScreenPresetsPreference = findPreference("dual_screen_presets")!!
        val allFilteringValues = resources.getStringArray(R.array.video_filtering_values)
        val allFilteringEntries = resources.getStringArray(R.array.video_filtering_options)

        val activityManager = requireContext().getSystemService<ActivityManager>()
        val deviceGlesVersion = activityManager?.deviceConfigurationInfo?.reqGlEsVersion ?: 0
        val supportsOpenGlRenderer = deviceGlesVersion >= GLES_3_2
        val supportsComputeRenderer = supportsOpenGlRenderer && Build.HARDWARE.equals("qcom", ignoreCase = true)

        rendererPreference.apply {
            if (!supportsOpenGlRenderer || !supportsComputeRenderer) {
                val values = context.resources.getStringArray(R.array.video_renderer_values)
                val entries = context.resources.getStringArray(R.array.video_renderer_options)
                val filteredPairs = values.zip(entries).filterNot { (value, _) ->
                    (!supportsOpenGlRenderer && value == "opengl") ||
                        (!supportsComputeRenderer && value == "compute")
                }
                entryValues = filteredPairs.map { it.first }.toTypedArray()
                this.entries = filteredPairs.map { it.second }.toTypedArray()

                if (!supportsOpenGlRenderer && value.equals("opengl", ignoreCase = true)) {
                    value = "software"
                }
                if (!supportsComputeRenderer && value.equals("compute", ignoreCase = true)) {
                    value = "software"
                }
            }

            setOnPreferenceChangeListener { _, newValue ->
                val rendererValue = newValue as String
                val newRenderer = enumValueOfIgnoreCase<VideoRenderer>(rendererValue)
                if (newRenderer == VideoRenderer.VULKAN) {
                    MelonDSAndroidInterface.configureVulkanDriver(
                        settingsRepository.getVulkanDriverConfiguration(requireContext().applicationInfo.nativeLibraryDir)
                    )
                    val canUseVulkan = MelonDSAndroidInterface.isVulkanRendererSupported()
                    if (!canUseVulkan) {
                        showVulkanUnavailableDialog()
                        return@setOnPreferenceChangeListener false
                    }
                }

                onRendererPreferenceChanged(
                    rendererValue = rendererValue,
                    videoFilteringPreference = videoFilteringPreference,
                    retroArchShaderRootPreference = retroArchShaderRootPreference,
                    retroArchShaderPresetPreference = retroArchShaderPresetPreference,
                    retroArchShaderParametersPreference = retroArchShaderParametersPreference,
                    retroArchShaderClearHistoryPreference = retroArchShaderClearHistoryPreference,
                    allFilteringValues = allFilteringValues,
                    allFilteringEntries = allFilteringEntries,
                )
                updateVulkanDrsPreferenceState(newRenderer)
                updateVulkanDriverPreferenceState(
                    renderer = newRenderer,
                    category = vulkanDriverCategory,
                    modePreference = vulkanDriverModePreference,
                    importPreference = vulkanDriverImportPreference,
                    removePreference = vulkanDriverRemovePreference,
                    launchedInGame = launchedInGame,
                )
                true
            }
        }

        dsiCameraSourcePreference.setOnPreferenceChangeListener { _, newValue ->
            updateDsiCameraImagePreference(dsiCameraImagePreference, newValue as String)
            true
        }

        dualScreenPresetsPreference.setOnPreferenceClickListener {
            showDualScreenPresetsDialog()
            true
        }

        helper.setupStoragePickerPreference(dsiCameraImagePreference)
        helper.setupStoragePickerPreference(retroArchShaderRootPreference)
        helper.bindPreferenceSummaryToValue(retroArchShaderPresetPreference)
        retroArchShaderParametersPreference?.let { helper.bindPreferenceSummaryToValue(it) }

        updateVulkanDrsPreferenceState(enumValueOfIgnoreCase(rendererPreference.value))
        setupVulkanDriverPreferences(
            renderer = enumValueOfIgnoreCase(rendererPreference.value),
            category = vulkanDriverCategory,
            modePreference = vulkanDriverModePreference,
            importPreference = vulkanDriverImportPreference,
            removePreference = vulkanDriverRemovePreference,
            launchedInGame = launchedInGame,
        )
        retroArchShaderRootPreference.addOnPreferenceChangeListener { _, newValue ->
            val rootUri = (newValue as? Set<*>)
                ?.firstOrNull()
                ?.let { it as? String }
                ?.toUri()
            updateRetroArchPresetEntries(retroArchShaderPresetPreference, rootUri)
            true
        }
        updateRetroArchPresetEntries(retroArchShaderPresetPreference)

        shaderRootPreference = retroArchShaderRootPreference
        shaderPresetPreference = retroArchShaderPresetPreference
        shaderSourcePreference = findPreference("video_retroarch_shader_source")!!
        shaderSourcePreference.setOnPreferenceClickListener {
            showShaderSourceDialog()
            true
        }
        shaderSettingsPreference = findPreference(SHADER_SETTINGS_KEY)
        observeShaderInstallWork()

        updateFilteringPreferences(
            renderer = enumValueOfIgnoreCase(rendererPreference.value),
            videoFilteringPreference = videoFilteringPreference,
            retroArchShaderRootPreference = retroArchShaderRootPreference,
            retroArchShaderPresetPreference = retroArchShaderPresetPreference,
            retroArchShaderParametersPreference = retroArchShaderParametersPreference,
            retroArchShaderClearHistoryPreference = retroArchShaderClearHistoryPreference,
            allFilteringValues = allFilteringValues,
            allFilteringEntries = allFilteringEntries,
        )
        videoFilteringPreference.setOnPreferenceChangeListener { _, newValue ->
            pendingDsLitePreset = null
            updateShaderPickerPreferences(
                renderer = enumValueOfIgnoreCase(rendererPreference.value),
                filteringValue = newValue as String,
                retroArchShaderRootPreference = retroArchShaderRootPreference,
                retroArchShaderPresetPreference = retroArchShaderPresetPreference,
                retroArchShaderParametersPreference = retroArchShaderParametersPreference,
                retroArchShaderClearHistoryPreference = retroArchShaderClearHistoryPreference,
            )
            if (videoFilteringOrNone(newValue) == VideoFiltering.RETROARCH && resolveShaderSource() == null) {
                showShaderSourceDialog()
            }
            true
        }

        onRendererPreferenceChanged(
            rendererValue = rendererPreference.value,
            videoFilteringPreference = videoFilteringPreference,
            retroArchShaderRootPreference = retroArchShaderRootPreference,
            retroArchShaderPresetPreference = retroArchShaderPresetPreference,
            retroArchShaderParametersPreference = retroArchShaderParametersPreference,
            retroArchShaderClearHistoryPreference = retroArchShaderClearHistoryPreference,
            allFilteringValues = allFilteringValues,
            allFilteringEntries = allFilteringEntries,
        )
        updateDsiCameraImagePreference(dsiCameraImagePreference, dsiCameraSourcePreference.value)
        updateDualScreenPresetSummary()
    }

    private fun setupFreedrenoSettingsSubScreen() {
        preferenceScreen.isEnabled =
            AdrenoVulkanDriverSupport.supportsFreedreno(requireContext()) &&
            !requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_IN_GAME, false)
        for (variable in me.magnum.melonds.impl.FreedrenoSettings.variables) {
            findPreference<EditTextPreference>(
                me.magnum.melonds.impl.FreedrenoSettings.preferenceKey(variable),
            )?.apply {
                summaryProvider = Preference.SummaryProvider<EditTextPreference> {
                    it.text?.takeIf { value -> value.isNotBlank() } ?: getString(R.string.freedreno_default)
                }
                setOnBindEditTextListener { it.setSingleLine(true) }
                setOnPreferenceChangeListener { _, value ->
                    val nextValue = value as String
                    if (!me.magnum.melonds.impl.FreedrenoSettings.isValid(nextValue)) {
                        Toast.makeText(requireContext(), R.string.freedreno_invalid, Toast.LENGTH_LONG).show()
                    } else if (nextValue.trim() != text.orEmpty().trim()) {
                        FreedrenoRestartDialog.create(key, nextValue.trim())
                            .show(childFragmentManager, "freedreno_restart")
                    }
                    false
                }
            }
        }
    }

    private fun setupVulkanDriverPreferences(
        renderer: VideoRenderer,
        category: PreferenceCategory,
        modePreference: ListPreference,
        importPreference: Preference,
        removePreference: Preference,
        launchedInGame: Boolean,
    ) {
        modePreference.isPersistent = false
        modePreference.setOnPreferenceChangeListener { _, value ->
            val driverId = value as String
            if (driverId == VULKAN_SYSTEM_DRIVER_VALUE) {
                settingsRepository.setVulkanDriverMode(VulkanDriverMode.SYSTEM)
            } else {
                settingsRepository.setSelectedVulkanDriver(driverId)
            }
            updateVulkanDriverPreferenceSummaries(modePreference, removePreference)
            true
        }

        importPreference.setOnPreferenceClickListener {
            vulkanDriverImportLauncher.launch(
                arrayOf(
                    "application/zip",
                    "application/x-zip-compressed",
                    "application/octet-stream",
                    "application/x-compressed",
                )
            )
            true
        }

        removePreference.setOnPreferenceClickListener {
            showRemoveVulkanDriverDialog(modePreference, removePreference)
            true
        }

        updateVulkanDriverPreferenceState(
            renderer = renderer,
            category = category,
            modePreference = modePreference,
            importPreference = importPreference,
            removePreference = removePreference,
            launchedInGame = launchedInGame,
        )
    }

    private fun updateVulkanDrsPreferenceState(renderer: VideoRenderer) {
        findPreference<PreferenceCategory>("video_vulkan_category")?.isVisible = renderer == VideoRenderer.VULKAN
    }

    private fun updateVulkanDriverPreferenceState(
        renderer: VideoRenderer,
        category: PreferenceCategory,
        modePreference: ListPreference,
        importPreference: Preference,
        removePreference: Preference,
        launchedInGame: Boolean,
    ) {
        val visible = renderer == VideoRenderer.VULKAN && adrenoVulkanDriverManager.isSupported && !launchedInGame
        category.isVisible = visible
        if (!visible) {
            return
        }

        val supportsFreedreno = AdrenoVulkanDriverSupport.supportsFreedreno(requireContext())
        findPreference<Preference>(FREEDRENO_SETTINGS_KEY)?.isVisible = supportsFreedreno
        importPreference.setSummary(
            if (supportsFreedreno) R.string.video_vulkan_driver_import_summary else R.string.video_vulkan_driver_import_other_gpu
        )
        category.summary = null
        updateVulkanDriverPreferenceSummaries(modePreference, removePreference)
    }

    private fun handleVulkanDriverImport(uri: Uri) {
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { adrenoVulkanDriverManager.importDriver(uri) }
            }
            result.onSuccess { importResult ->
                val modePreference = findPreference<ListPreference>("video_vulkan_driver_mode") ?: return@onSuccess
                val removePreference = findPreference<Preference>("video_vulkan_driver_remove") ?: return@onSuccess
                updateVulkanDriverPreferenceSummaries(modePreference, removePreference)
                Toast.makeText(
                    requireContext(),
                    getString(R.string.video_vulkan_driver_import_success, importResult.displayName),
                    Toast.LENGTH_LONG,
                ).show()
            }.onFailure { throwable ->
                val messageRes = when ((throwable as? AdrenoVulkanDriverManager.ImportException)?.reason) {
                    AdrenoVulkanDriverManager.ImportException.Reason.NotZip -> R.string.video_vulkan_driver_import_not_zip
                    AdrenoVulkanDriverManager.ImportException.Reason.NoDriver -> R.string.video_vulkan_driver_import_no_driver
                    AdrenoVulkanDriverManager.ImportException.Reason.AmbiguousDriver -> R.string.video_vulkan_driver_import_ambiguous
                    AdrenoVulkanDriverManager.ImportException.Reason.UnsupportedBuild -> R.string.video_vulkan_driver_unsupported
                    else -> R.string.video_vulkan_driver_import_failed
                }
                Toast.makeText(requireContext(), messageRes, Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun updateVulkanDriverPreferenceSummaries(
        modePreference: ListPreference,
        removePreference: Preference,
    ) {
        val installedDrivers = settingsRepository.getInstalledVulkanDrivers()
        val selectedDriverId = settingsRepository.getSelectedVulkanDriverId()
        val useCustomDriver = settingsRepository.getVulkanDriverMode() == VulkanDriverMode.CUSTOM &&
            selectedDriverId != null &&
            installedDrivers.any { it.id == selectedDriverId }
        modePreference.entryValues = arrayOf(VULKAN_SYSTEM_DRIVER_VALUE) + installedDrivers.map { it.id }.toTypedArray()
        modePreference.entries = arrayOf(getString(R.string.video_vulkan_driver_mode_system)) +
            installedDrivers.map { it.displayName }.toTypedArray()
        modePreference.value = if (useCustomDriver) selectedDriverId else VULKAN_SYSTEM_DRIVER_VALUE
        modePreference.summary = if (useCustomDriver) {
            getString(
                R.string.video_vulkan_driver_active_custom,
                installedDrivers.first { it.id == selectedDriverId }.displayName,
            )
        } else {
            getString(R.string.video_vulkan_driver_active_system)
        }
        removePreference.isVisible = installedDrivers.isNotEmpty()
    }

    private fun showRemoveVulkanDriverDialog(
        modePreference: ListPreference,
        removePreference: Preference,
    ) {
        val installedDrivers = settingsRepository.getInstalledVulkanDrivers()
        if (installedDrivers.isEmpty()) {
            Toast.makeText(requireContext(), R.string.video_vulkan_driver_no_custom, Toast.LENGTH_SHORT).show()
            return
        }

        AlertDialog.Builder(requireContext())
            .setTitle(R.string.video_vulkan_driver_remove)
            .setItems(installedDrivers.map { it.displayName }.toTypedArray()) { _, which ->
                val driver = installedDrivers[which]
                removeVulkanDriver(driver, modePreference, removePreference)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun removeVulkanDriver(
        driver: VulkanDriverInfo,
        modePreference: ListPreference,
        removePreference: Preference,
    ) {
        adrenoVulkanDriverManager.removeDriver(driver.id)
        updateVulkanDriverPreferenceSummaries(modePreference, removePreference)
        Toast.makeText(
            requireContext(),
            getString(R.string.video_vulkan_driver_removed, driver.displayName),
            Toast.LENGTH_LONG,
        ).show()
    }

    override fun onDisplayPreferenceDialog(preference: Preference) {
        if (preference.key == "video_retroarch_shader_preset") {
            showRetroArchPresetBrowserDialog(preference as ListPreference)
            return
        }

        super.onDisplayPreferenceDialog(preference)
    }

    private fun updateRetroArchPresetEntries(
        preference: ListPreference,
        rootUriOverride: Uri? = null,
        resetSelection: Boolean = rootUriOverride != null,
    ) {
        retroArchPresetScanJob?.cancel()
        if (resetSelection) {
            preference.value = null
        }

        val selectedPreset = preference.value
        preference.entries = selectedPreset?.let { arrayOf(it) } ?: emptyArray()
        preference.entryValues = selectedPreset?.let { arrayOf(it) } ?: emptyArray()
        preference.summary = selectedPreset ?: getString(R.string.video_retroarch_shader_preset_summary)
    }

    private data class ShaderBrowserEntry(val name: String, val isDirectory: Boolean)

    private fun interface ShaderDirectoryLister {
        fun list(relativePath: String): List<ShaderBrowserEntry>
    }

    private fun safDirectoryLister(rootUri: Uri): ShaderDirectoryLister {
        val context = requireContext()
        return ShaderDirectoryLister { relativePath ->
            var current = DocumentFile.fromTreeUri(context, rootUri) ?: return@ShaderDirectoryLister emptyList()
            if (relativePath.isNotBlank()) {
                relativePath.split('/').forEach { segment ->
                    if (segment.isBlank()) {
                        return@ShaderDirectoryLister emptyList()
                    }
                    current = current.findFile(segment) ?: return@ShaderDirectoryLister emptyList()
                }
            }
            if (!current.isDirectory) {
                return@ShaderDirectoryLister emptyList()
            }
            current.listFiles().mapNotNull { child ->
                val name = child.name ?: return@mapNotNull null
                ShaderBrowserEntry(name, child.isDirectory)
            }
        }
    }

    private fun fileDirectoryLister(rootDir: File): ShaderDirectoryLister {
        return ShaderDirectoryLister { relativePath ->
            val rootCanonical = rootDir.canonicalFile
            val directory = File(rootCanonical, relativePath).canonicalFile
            if (!directory.path.startsWith(rootCanonical.path) || !directory.isDirectory) {
                return@ShaderDirectoryLister emptyList()
            }
            directory.listFiles()?.map { ShaderBrowserEntry(it.name, it.isDirectory) }.orEmpty()
        }
    }

    private fun resolveShaderDirectoryLister(): ShaderDirectoryLister? {
        return when (resolveShaderSource()) {
            RetroArchShaderSource.INTERNAL -> shaderLibraryManager.libraryRoot?.let { fileDirectoryLister(it) }
            RetroArchShaderSource.FOLDER -> preferenceManager.sharedPreferences
                ?.getStringSet("video_retroarch_shader_root", null)
                ?.firstOrNull()
                ?.toUri()
                ?.let { safDirectoryLister(it) }
            null -> null
        }
    }

    private data class ShaderBrowserItem(
        val label: String,
        val path: String,
        val isDirectory: Boolean,
        val isParent: Boolean = false,
    )

    private fun showRetroArchPresetBrowserDialog(preference: ListPreference) {
        val lister = resolveShaderDirectoryLister()
        if (lister == null) {
            preference.summary = getString(R.string.video_retroarch_shader_preset_summary)
            Toast.makeText(requireContext(), R.string.retroarch_shader_root_not_valid, Toast.LENGTH_LONG).show()
            return
        }

        val context = requireContext()
        val view = layoutInflater.inflate(R.layout.dialog_shader_browser, null)
        val pathText = view.findViewById<TextView>(R.id.textShaderBrowserPath)
        val emptyText = view.findViewById<TextView>(R.id.textShaderBrowserEmpty)
        val listView = view.findViewById<RecyclerView>(R.id.listShaderBrowser)

        val listContainer = view.findViewById<View>(R.id.containerShaderBrowserList)
        listContainer.layoutParams = listContainer.layoutParams.apply {
            height = minOf(height, (resources.displayMetrics.heightPixels * 0.45f).toInt())
        }
        val folderCache = mutableMapOf<String, List<ShaderBrowserItem>>()
        var currentDirectory = ""

        val dialog = AlertDialog.Builder(context)
            .setTitle(preference.title)
            .setView(view)
            .setNegativeButton(android.R.string.cancel, null)
            .create()

        lateinit var adapter: ShaderBrowserAdapter
        fun openDirectory(relativePath: String) {
            currentDirectory = relativePath
            loadShaderBrowserDirectory(lister, folderCache, relativePath, pathText, emptyText, adapter)
        }
        adapter = ShaderBrowserAdapter(
            selectedPath = { preference.value },
            onClick = { item ->
                if (item.isDirectory) {
                    openDirectory(item.path)
                } else {
                    applyRetroArchPresetSelection(preference, item.path)
                    dialog.dismiss()
                }
            },
        )
        listView.layoutManager = LinearLayoutManager(context)
        listView.adapter = adapter


        fun handleBack(): Boolean {
            if (currentDirectory.isBlank()) {
                return false
            }
            openDirectory(currentDirectory.substringBeforeLast('/', missingDelimiterValue = ""))
            return true
        }

        val backCallback = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            OnBackInvokedCallback {
                if (!handleBack()) {
                    dialog.dismiss()
                }
            }
        } else {
            dialog.setOnKeyListener { _, keyCode, event ->
                keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP && handleBack()
            }
            null
        }

        dialog.setOnShowListener {
            if (backCallback != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                dialog.onBackInvokedDispatcher.registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                    backCallback,
                )
            }
            openDirectory("")
        }
        dialog.setOnDismissListener {
            if (backCallback != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                dialog.onBackInvokedDispatcher.unregisterOnBackInvokedCallback(backCallback)
            }
            retroArchPresetScanJob?.cancel()
            retroArchPresetScanJob = null
        }
        dialog.show()
    }

    private fun loadShaderBrowserDirectory(
        lister: ShaderDirectoryLister,
        cache: MutableMap<String, List<ShaderBrowserItem>>,
        relativePath: String,
        pathText: TextView,
        emptyText: TextView,
        adapter: ShaderBrowserAdapter,
    ) {
        retroArchPresetScanJob?.cancel()
        pathText.text = if (relativePath.isBlank()) "/" else "/$relativePath"

        cache[relativePath]?.let {
            adapter.submit(it)
            emptyText.setText(R.string.video_retroarch_shader_browser_empty)
            emptyText.isVisible = it.isEmpty()
            return
        }

        adapter.submit(emptyList())
        emptyText.setText(R.string.info_loading)
        emptyText.isVisible = true

        retroArchPresetScanJob = lifecycleScope.launch {
            val items = withContext(Dispatchers.IO) {
                val directories = mutableListOf<Pair<String, String>>()
                val presets = mutableListOf<Pair<String, String>>()
                lister.list(relativePath).forEach { child ->
                    val childPath = if (relativePath.isBlank()) child.name else "$relativePath/${child.name}"
                    when {
                        child.isDirectory -> directories += child.name to childPath
                        child.name.endsWith(".slangp", ignoreCase = true) -> presets += child.name to childPath
                    }
                }

                buildList {
                    if (relativePath.isNotBlank()) {
                        add(
                            ShaderBrowserItem(
                                label = "..",
                                path = relativePath.substringBeforeLast('/', missingDelimiterValue = ""),
                                isDirectory = true,
                                isParent = true,
                            ),
                        )
                    }
                    directories.sortedBy { it.first.lowercase() }.forEach { (name, childPath) ->
                        add(ShaderBrowserItem(name, childPath, isDirectory = true))
                    }
                    presets.sortedBy { it.first.lowercase() }.forEach { (name, childPath) ->
                        add(ShaderBrowserItem(name.removeSuffix(".slangp"), childPath, isDirectory = false))
                    }
                }
            }

            cache[relativePath] = items
            adapter.submit(items)
            emptyText.setText(R.string.video_retroarch_shader_browser_empty)
            emptyText.isVisible = items.isEmpty()
        }
    }

    private fun weighPresetAsync(relativePath: String, onResult: (RetroArchShaderPreset.Weight) -> Unit) {
        val root = shaderLibraryManager.libraryRoot?.takeIf { resolveShaderSource() == RetroArchShaderSource.INTERNAL }
            ?: return
        shaderWeightCache[relativePath]?.let {
            onResult(it)
            return
        }

        lifecycleScope.launch {
            val weight = withContext(Dispatchers.IO) {
                runCatching {
                    RetroArchShaderPreset.weigh(relativePath) { shaderPath ->
                        File(root, shaderPath).takeIf { it.isFile }?.readText()
                    }
                }.getOrNull()
            } ?: return@launch
            shaderWeightCache[relativePath] = weight
            onResult(weight)
        }
    }

    private inner class ShaderBrowserAdapter(
        private val selectedPath: () -> String?,
        private val onClick: (ShaderBrowserItem) -> Unit,
    ) : RecyclerView.Adapter<ShaderBrowserAdapter.ViewHolder>() {

        private val items = mutableListOf<ShaderBrowserItem>()

        fun itemAt(position: Int): ShaderBrowserItem? = items.getOrNull(position)

        @SuppressLint("NotifyDataSetChanged")
        fun submit(newItems: List<ShaderBrowserItem>) {
            items.clear()
            items += newItems
            notifyDataSetChanged()
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
            return ViewHolder(layoutInflater.inflate(R.layout.item_shader_browser_entry, parent, false))
        }

        override fun getItemCount(): Int = items.size

        override fun onBindViewHolder(holder: ViewHolder, position: Int) {
            holder.bind(items[position])
        }

        inner class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
            private val icon: ImageView = view.findViewById(R.id.imageShaderEntryIcon)
            private val name: TextView = view.findViewById(R.id.textShaderEntryName)
            private val detail: TextView = view.findViewById(R.id.textShaderEntryDetail)
            private val badge: TextView = view.findViewById(R.id.textShaderEntryBadge)
            private val chevron: ImageView = view.findViewById(R.id.imageShaderEntryChevron)

            fun bind(item: ShaderBrowserItem) {
                name.text = if (item.isParent) {
                    itemView.context.getString(R.string.video_retroarch_shader_browser_up)
                } else {
                    item.label
                }
                icon.setImageResource(
                    when {
                        item.isParent -> R.drawable.ic_arrow_up
                        item.isDirectory -> R.drawable.ic_folder
                        else -> R.drawable.ic_file
                    },
                )
                chevron.isVisible = item.isDirectory && !item.isParent
                itemView.isSelected = !item.isDirectory && item.path == selectedPath()
                itemView.setOnClickListener { onClick(item) }

                detail.isVisible = false
                badge.isVisible = false
                if (item.isDirectory) {
                    return
                }

                val boundPath = item.path
                weighPresetAsync(boundPath) { weight ->
                    if (boundPath != itemAt(bindingAdapterPosition)?.path) {
                        return@weighPresetAsync
                    }
                    detail.isVisible = true
                    detail.text = itemView.context.resources.getQuantityString(
                        R.plurals.video_retroarch_shader_browser_passes,
                        weight.passCount,
                        weight.passCount,
                    )
                    val isHeavy = weight.estimatedCompileMillis >= HEAVY_PRESET_BADGE_THRESHOLD_MS
                    badge.isVisible = isHeavy
                    if (isHeavy) {
                        badge.setText(R.string.video_retroarch_shader_browser_slow)
                    }
                }
            }
        }
    }

    private fun onRendererPreferenceChanged(
        rendererValue: String,
        videoFilteringPreference: ListPreference,
        retroArchShaderRootPreference: StoragePickerPreference,
        retroArchShaderPresetPreference: ListPreference,
        retroArchShaderParametersPreference: EditTextPreference?,
        retroArchShaderClearHistoryPreference: SwitchPreference?,
        allFilteringValues: Array<String>,
        allFilteringEntries: Array<String>,
    ) {
        val newRenderer = enumValueOfIgnoreCase<VideoRenderer>(rendererValue)
        when (newRenderer) {
            VideoRenderer.SOFTWARE -> {
                threadedRendererPreferences.forEach {
                    it.isVisible = true
                }
                highResRendererPreferences.forEach {
                    it.isVisible = false
                }
                coverageFixPreferences.forEach {
                    it.isVisible = false
                }
                rendererDebugPreferences.forEach {
                    it.isVisible = true
                }
            }
            VideoRenderer.OPENGL -> {
                threadedRendererPreferences.forEach {
                    it.isVisible = false
                }
                highResRendererPreferences.forEach {
                    it.isVisible = true
                }
                coverageFixPreferences.forEach {
                    it.isVisible = true
                }
                rendererDebugPreferences.forEach {
                    it.isVisible = true
                }
            }
            VideoRenderer.COMPUTE -> {
                threadedRendererPreferences.forEach {
                    it.isVisible = false
                }
                highResRendererPreferences.forEach {
                    it.isVisible = true
                }
                coverageFixPreferences.forEach {
                    it.isVisible = false
                }
                rendererDebugPreferences.forEach {
                    it.isVisible = false
                }
            }
            VideoRenderer.VULKAN -> {
                threadedRendererPreferences.forEach {
                    it.isVisible = false
                }
                highResRendererPreferences.forEach {
                    it.isVisible = true
                }
                coverageFixPreferences.forEach {
                    it.isVisible = false
                }
                rendererDebugPreferences.forEach {
                    it.isVisible = true
                }
            }
        }

        refreshConditionalVideoVisibility()

        updateFilteringPreferences(
            renderer = newRenderer,
            videoFilteringPreference = videoFilteringPreference,
            retroArchShaderRootPreference = retroArchShaderRootPreference,
            retroArchShaderPresetPreference = retroArchShaderPresetPreference,
            retroArchShaderParametersPreference = retroArchShaderParametersPreference,
            retroArchShaderClearHistoryPreference = retroArchShaderClearHistoryPreference,
            allFilteringValues = allFilteringValues,
            allFilteringEntries = allFilteringEntries,
        )
    }

    private fun refreshConditionalVideoVisibility() {
        val debugSwitch = findPreference<SwitchPreference>("video_renderer_debug_tools_enabled")
        val debugOn = debugSwitch?.isVisible == true && debugSwitch.isChecked
        listOf("video_renderer_debug_bgobj_enabled", "video_renderer_debug_latch_trace_enabled").forEach {
            findPreference<Preference>(it)?.isVisible = debugOn
        }

        val coverageSwitch = findPreference<SwitchPreference>("video_conservative_coverage_enabled")
        val coverageOn = coverageSwitch?.isVisible == true && coverageSwitch.isChecked
        listOf(
            "video_conservative_coverage_px",
            "video_conservative_coverage_apply_repeat",
            "video_conservative_coverage_apply_clamp",
            "video_conservative_coverage_depth_bias",
        ).forEach {
            findPreference<Preference>(it)?.isVisible = coverageOn
        }
    }

    private fun updateFilteringPreferences(
        renderer: VideoRenderer,
        videoFilteringPreference: ListPreference,
        retroArchShaderRootPreference: StoragePickerPreference,
        retroArchShaderPresetPreference: ListPreference,
        retroArchShaderParametersPreference: EditTextPreference?,
        retroArchShaderClearHistoryPreference: SwitchPreference?,
        allFilteringValues: Array<String>,
        allFilteringEntries: Array<String>,
    ) {
        val filteredPairs = allFilteringValues.zip(allFilteringEntries).filter { (value, _) ->
            val filtering = videoFilteringOrNone(value)
            when (renderer) {
                VideoRenderer.VULKAN -> filtering.isSupportedByVulkan()
                else -> filtering.isSupportedByOpenGlSurface()
            }
        }

        videoFilteringPreference.entryValues = filteredPairs.map { it.first }.toTypedArray()
        videoFilteringPreference.entries = filteredPairs.map { it.second }.toTypedArray()

        val currentFiltering = videoFilteringOrNone(videoFilteringPreference.value)
        val currentFilteringSupported = when (renderer) {
            VideoRenderer.VULKAN -> currentFiltering.isSupportedByVulkan()
            else -> currentFiltering.isSupportedByOpenGlSurface()
        }
        if (!currentFilteringSupported) {
            videoFilteringPreference.value = VideoFiltering.NONE.name.lowercase()
        }

        updateShaderPickerPreferences(
            renderer = renderer,
            filteringValue = videoFilteringPreference.value,
            retroArchShaderRootPreference = retroArchShaderRootPreference,
            retroArchShaderPresetPreference = retroArchShaderPresetPreference,
            retroArchShaderParametersPreference = retroArchShaderParametersPreference,
            retroArchShaderClearHistoryPreference = retroArchShaderClearHistoryPreference,
        )
    }

    private fun setupShaderSettingsSubScreen() {
        shaderManagePreference = findPreference("video_retroarch_shader_manage")!!
        shaderManagePreference.setOnPreferenceClickListener {
            if (shaderLibraryManager.isInstalled()) {
                showShaderManageDialog()
            } else {
                startShaderInstall()
            }
            true
        }
        shaderReportPreference = findPreference("video_retroarch_shader_report")!!
        shaderReportPreference.setOnPreferenceClickListener {
            showShaderCompatibilityReport()
            true
        }
        val rootPreference = findPreference<StoragePickerPreference>("video_retroarch_shader_root")!!
        shaderRootPreference = rootPreference
        helper.setupStoragePickerPreference(rootPreference)
        rootPreference.addOnPreferenceChangeListener { _, _ ->
            preferenceManager.sharedPreferences?.edit()?.remove("video_retroarch_shader_preset")?.apply()
            true
        }

        val source = resolveShaderSource()
        shaderManagePreference.isVisible = source == RetroArchShaderSource.INTERNAL
        rootPreference.isVisible = source == RetroArchShaderSource.FOLDER
        updateShaderManageSummary()
        observeShaderInstallWork()
    }

    private fun updateShaderPickerPreferences(
        renderer: VideoRenderer,
        filteringValue: String,
        retroArchShaderRootPreference: StoragePickerPreference,
        retroArchShaderPresetPreference: ListPreference,
        retroArchShaderParametersPreference: EditTextPreference?,
        retroArchShaderClearHistoryPreference: SwitchPreference?,
    ) {
        val filtering = videoFilteringOrNone(filteringValue)
        val retroArchEnabled = filtering == VideoFiltering.RETROARCH

        val source = resolveShaderSource()
        val isFolderSource = source == RetroArchShaderSource.FOLDER
        val isInternalSource = source == RetroArchShaderSource.INTERNAL
        val hasLibrary = isFolderSource || (isInternalSource && shaderLibraryManager.isInstalled())

        val dsLitePreset = DSLITE_PRESETS.firstOrNull {
            retroArchEnabled && isInternalSource && it.second == retroArchShaderPresetPreference.value
        }
        findPreference<Preference>("video_dslite_preset")?.setSummary(
            dsLitePreset?.first ?: R.string.video_dslite_preset_summary,
        )

        if (::shaderSourcePreference.isInitialized) {
            shaderSourcePreference.isVisible = retroArchEnabled
            shaderSourcePreference.summary = when (source) {
                RetroArchShaderSource.INTERNAL -> getString(R.string.video_retroarch_shader_source_internal)
                RetroArchShaderSource.FOLDER -> getString(R.string.video_retroarch_shader_source_folder)
                null -> getString(R.string.not_set)
            }
        }
        if (::shaderManagePreference.isInitialized) {
            shaderManagePreference.isVisible = retroArchEnabled && isInternalSource
            updateShaderManageSummary()
        }
        if (::shaderReportPreference.isInitialized) {
            shaderReportPreference.isVisible = retroArchEnabled
        }

        retroArchShaderRootPreference.isVisible = retroArchEnabled && isFolderSource
        retroArchShaderPresetPreference.isVisible = retroArchEnabled && hasLibrary
        retroArchShaderParametersPreference?.isVisible = retroArchEnabled && hasLibrary
        retroArchShaderClearHistoryPreference?.isVisible = retroArchEnabled && hasLibrary
        shaderSettingsPreference?.isVisible = retroArchEnabled
    }

    private fun resolveShaderSource(): RetroArchShaderSource? {
        val preferences = preferenceManager.sharedPreferences
        return RetroArchShaderRootResolver.resolveSource(
            rawSourcePreference = preferences?.getString("video_retroarch_shader_source", null),
            hasPickedFolder = !preferences?.getStringSet("video_retroarch_shader_root", null).isNullOrEmpty(),
            hasInternalInstall = shaderLibraryManager.isInstalled(),
        )
    }

    private fun persistShaderSource(source: RetroArchShaderSource) {
        preferenceManager.sharedPreferences
            ?.edit()
            ?.putString("video_retroarch_shader_source", source.preferenceValue)
            ?.apply()
    }

    private fun updateShaderManageSummary() {
        if (!::shaderManagePreference.isInitialized) {
            return
        }

        val manifest = shaderLibraryManager.readManifest()
        if (manifest == null || !shaderLibraryManager.isInstalled()) {
            shaderManagePreference.setTitle(R.string.video_retroarch_shader_install_title)
            shaderManagePreference.setSummary(R.string.video_retroarch_shader_install_summary)
            return
        }

        val date = DateFormat.getDateFormat(requireContext()).format(Date(manifest.installedAtMillis))
        val size = Formatter.formatShortFileSize(requireContext(), shaderLibraryManager.installedSizeBytes())
        shaderManagePreference.setTitle(R.string.video_retroarch_shader_installed_title)
        shaderManagePreference.summary = getString(R.string.video_retroarch_shader_installed_summary, date, size)
    }

    private fun refreshShaderPreferenceVisibility() {
        if (!::shaderPresetPreference.isInitialized) {
            val source = resolveShaderSource()
            if (::shaderManagePreference.isInitialized) {
                shaderManagePreference.isVisible = source == RetroArchShaderSource.INTERNAL
                updateShaderManageSummary()
            }
            if (::shaderRootPreference.isInitialized) {
                shaderRootPreference.isVisible = source == RetroArchShaderSource.FOLDER
            }
            return
        }
        val preferences = preferenceManager.sharedPreferences
        updateShaderPickerPreferences(
            renderer = enumValueOfIgnoreCase(preferences?.getString("video_renderer", "software") ?: "software"),
            filteringValue = preferences?.getString("video_filtering", "none") ?: "none",
            retroArchShaderRootPreference = shaderRootPreference,
            retroArchShaderPresetPreference = shaderPresetPreference,
            retroArchShaderParametersPreference = findPreference("video_retroarch_shader_parameters"),
            retroArchShaderClearHistoryPreference = findPreference("video_retroarch_shader_clear_history"),
        )
    }

    private fun showShaderCompatibilityReport() {
        val lines = shaderCompatibilityLog.read()
        val message = if (lines.isEmpty()) {
            getString(R.string.shader_compatibility_report_empty)
        } else {
            lines.asReversed().joinToString("\n\n")
        }

        AlertDialog.Builder(requireContext())
            .setTitle(R.string.shader_compatibility_report_title)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .apply {
                if (lines.isNotEmpty()) {
                    setNeutralButton(R.string.shader_compatibility_report_save) { _, _ ->
                        saveShaderCompatibilityLog(lines)
                    }
                    setNegativeButton(R.string.shader_compatibility_report_clear) { _, _ ->
                        shaderCompatibilityLog.clear()
                    }
                }
            }
            .show()
    }

    private fun saveShaderCompatibilityLog(lines: List<String>) {
        val romFolder = settingsRepository.getRomSearchDirectories().firstOrNull()
        if (romFolder == null) {
            Toast.makeText(requireContext(), R.string.shader_compatibility_report_no_rom_folder, Toast.LENGTH_LONG).show()
            return
        }

        lifecycleScope.launch {
            val savedName = withContext(Dispatchers.IO) {
                runCatching {
                    val directory = DocumentFile.fromTreeUri(requireContext(), romFolder)
                        ?: return@runCatching null
                    directory.findFile(SHADER_LOG_FILE_NAME)?.delete()
                    val file = directory.createFile("application/octet-stream", SHADER_LOG_FILE_NAME)
                        ?: return@runCatching null
                    requireContext().contentResolver.openOutputStream(file.uri)?.use { output ->
                        output.write(lines.joinToString("\n", postfix = "\n").toByteArray())
                    } ?: return@runCatching null
                    file.name ?: SHADER_LOG_FILE_NAME
                }.getOrNull()
            }

            if (savedName == null) {
                Toast.makeText(requireContext(), R.string.shader_compatibility_report_save_failed, Toast.LENGTH_LONG).show()
            } else {
                Toast.makeText(
                    requireContext(),
                    getString(R.string.shader_compatibility_report_saved, savedName),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }
    }

    private fun showShaderSourceDialog() {
        pendingDsLitePreset = null
        val context = requireContext()
        val options = listOf(
            mapOf(
                "title" to getString(R.string.video_retroarch_shader_source_internal),
                "description" to getString(R.string.video_retroarch_shader_source_internal_description),
            ),
            mapOf(
                "title" to getString(R.string.video_retroarch_shader_source_folder),
                "description" to getString(R.string.video_retroarch_shader_source_folder_description),
            ),
        )
        val adapter = SimpleAdapter(
            context,
            options,
            android.R.layout.simple_list_item_2,
            arrayOf("title", "description"),
            intArrayOf(android.R.id.text1, android.R.id.text2),
        )

        val listView = ListView(context).apply {
            this.adapter = adapter
            divider = null
        }

        val dialog = AlertDialog.Builder(context)
            .setTitle(R.string.video_retroarch_shader_source_title)
            .setView(listView)
            .setNegativeButton(android.R.string.cancel, null)
            .create()

        listView.setOnItemClickListener { _, _, position, _ ->
            dialog.dismiss()
            when (position) {
                0 -> {
                    persistShaderSource(RetroArchShaderSource.INTERNAL)
                    refreshShaderPreferenceVisibility()
                    if (!shaderLibraryManager.isInstalled()) {
                        startShaderInstall()
                    }
                }
                1 -> {
                    persistShaderSource(RetroArchShaderSource.FOLDER)
                    refreshShaderPreferenceVisibility()
                    if (shaderRootPreference.isEnabled && shaderRootPreference.isSelectable) {
                        shaderRootPreference.onPreferenceClickListener?.onPreferenceClick(shaderRootPreference)
                    }
                }
            }
        }
        dialog.show()
    }

    private fun showShaderManageDialog() {
        val context = requireContext()
        val entries = arrayOf(
            getString(R.string.video_retroarch_shader_check_updates),
            getString(R.string.video_retroarch_shader_reinstall),
            getString(R.string.video_retroarch_shader_uninstall),
        )

        AlertDialog.Builder(context)
            .setTitle(R.string.video_retroarch_shader_installed_title)
            .setItems(entries) { _, which ->
                when (which) {
                    0 -> checkShaderUpdates()
                    1 -> startShaderInstall()
                    2 -> confirmShaderUninstall()
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun checkShaderUpdates() {
        Toast.makeText(requireContext(), R.string.video_retroarch_shader_checking_updates, Toast.LENGTH_SHORT).show()
        lifecycleScope.launch {
            val remote = withContext(Dispatchers.IO) {
                runCatching { shaderLibraryManager.fetchRemoteInfo() }.getOrNull()
            }
            if (remote == null) {
                showShaderInstallError(RetroArchShaderLibraryManager.ShaderInstallException.Reason.NoNetwork, 0)
                return@launch
            }

            if (shaderLibraryManager.isUpdateAvailable(remote)) {
                AlertDialog.Builder(requireContext())
                    .setMessage(R.string.video_retroarch_shader_update_available)
                    .setPositiveButton(android.R.string.ok) { _, _ -> startShaderInstall() }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
            } else {
                Toast.makeText(requireContext(), R.string.video_retroarch_shader_up_to_date, Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun confirmShaderUninstall() {
        val size = Formatter.formatShortFileSize(requireContext(), shaderLibraryManager.installedSizeBytes())
        AlertDialog.Builder(requireContext())
            .setMessage(getString(R.string.video_retroarch_shader_uninstall_confirm, size))
            .setPositiveButton(R.string.video_retroarch_shader_uninstall) { _, _ ->
                lifecycleScope.launch {
                    withContext(Dispatchers.IO) { shaderLibraryManager.uninstall() }
                    if (::shaderPresetPreference.isInitialized) {
                        updateRetroArchPresetEntries(shaderPresetPreference, resetSelection = true)
                    }
                    refreshShaderPreferenceVisibility()
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun startShaderInstall() {
        val request = OneTimeWorkRequestBuilder<RetroArchShaderInstallWorker>()
            .setConstraints(
                Constraints.Builder()
                    .setRequiredNetworkType(NetworkType.CONNECTED)
                    .build(),
            )
            .build()

        WorkManager.getInstance(requireContext()).enqueueUniqueWork(
            RetroArchShaderInstallWorker.WORK_NAME,
            ExistingWorkPolicy.KEEP,
            request,
        )
        showShaderInstallProgressDialog()
    }

    private fun observeShaderInstallWork() {
        shaderInstallObserverJob?.cancel()
        shaderInstallObserverJob = lifecycleScope.launch {
            WorkManager.getInstance(requireContext())
                .getWorkInfosForUniqueWorkFlow(RetroArchShaderInstallWorker.WORK_NAME)
                .collect { workInfos ->
                    val info = workInfos.lastOrNull() ?: return@collect
                    when (info.state) {
                        WorkInfo.State.ENQUEUED, WorkInfo.State.BLOCKED -> {
                            showShaderInstallProgressDialog()
                            shaderInstallPhaseText?.setText(R.string.video_retroarch_shader_waiting_network)
                            shaderInstallProgressBar?.isIndeterminate = true
                            shaderInstallDetailText?.text = ""
                        }
                        WorkInfo.State.RUNNING -> {
                            showShaderInstallProgressDialog()
                            applyShaderInstallProgress(info)
                        }
                        WorkInfo.State.SUCCEEDED -> {
                            dismissShaderInstallProgressDialog()
                            if (::shaderPresetPreference.isInitialized) {
                                updateRetroArchPresetEntries(shaderPresetPreference, resetSelection = false)
                            }
                            refreshShaderPreferenceVisibility()
                            pendingDsLitePreset?.let { preset ->
                                pendingDsLitePreset = null
                                selectDsLitePreset(preset, allowDownload = false)
                            }
                        }
                        WorkInfo.State.FAILED -> {
                            pendingDsLitePreset = null
                            dismissShaderInstallProgressDialog()
                            val reasonName = info.outputData.getString(RetroArchShaderInstallWorker.KEY_FAILURE_REASON)
                            val reason = runCatching {
                                RetroArchShaderLibraryManager.ShaderInstallException.Reason.valueOf(reasonName.orEmpty())
                            }.getOrDefault(RetroArchShaderLibraryManager.ShaderInstallException.Reason.HttpError)
                            showShaderInstallError(
                                reason,
                                info.outputData.getLong(RetroArchShaderInstallWorker.KEY_REQUIRED_BYTES, 0),
                            )
                            refreshShaderPreferenceVisibility()
                        }
                        WorkInfo.State.CANCELLED -> {
                            pendingDsLitePreset = null
                            dismissShaderInstallProgressDialog()
                            refreshShaderPreferenceVisibility()
                        }
                    }
                }
        }
    }

    private fun applyShaderInstallProgress(info: WorkInfo) {
        val progressBar = shaderInstallProgressBar ?: return
        when (info.progress.getString(RetroArchShaderInstallWorker.KEY_PHASE)) {
            RetroArchShaderInstallWorker.PHASE_DOWNLOADING -> {
                val total = info.progress.getLong(RetroArchShaderInstallWorker.KEY_TOTAL_BYTES, 0)
                val downloaded = info.progress.getLong(RetroArchShaderInstallWorker.KEY_DOWNLOADED_BYTES, 0)
                shaderInstallPhaseText?.setText(R.string.video_retroarch_shader_downloading)
                if (total > 0) {
                    progressBar.isIndeterminate = false
                    progressBar.progress = ((downloaded * 100) / total).toInt()
                    shaderInstallDetailText?.text = getString(
                        R.string.video_retroarch_shader_progress_bytes,
                        Formatter.formatShortFileSize(requireContext(), downloaded),
                        Formatter.formatShortFileSize(requireContext(), total),
                    )
                } else {
                    progressBar.isIndeterminate = true
                }
            }
            RetroArchShaderInstallWorker.PHASE_EXTRACTING -> {
                val total = info.progress.getInt(RetroArchShaderInstallWorker.KEY_ENTRIES_TOTAL, 0)
                val done = info.progress.getInt(RetroArchShaderInstallWorker.KEY_ENTRIES_DONE, 0)
                shaderInstallPhaseText?.setText(R.string.video_retroarch_shader_extracting)
                if (total > 0) {
                    progressBar.isIndeterminate = false
                    progressBar.progress = (done * 100) / total
                    shaderInstallDetailText?.text = "$done / $total"
                } else {
                    progressBar.isIndeterminate = true
                }
            }
            RetroArchShaderInstallWorker.PHASE_FINALIZING -> {
                shaderInstallPhaseText?.setText(R.string.video_retroarch_shader_finalizing)
                progressBar.isIndeterminate = true
                shaderInstallDetailText?.text = ""
            }
        }
    }

    private fun showShaderInstallProgressDialog() {
        if (shaderInstallProgressDialog?.isShowing == true) {
            return
        }

        val context = requireContext()
        val density = resources.displayMetrics.density
        val phaseText = TextView(context)
        val progressBar = ProgressBar(context, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            isIndeterminate = true
        }
        val detailText = TextView(context)
        val container = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((24 * density).toInt(), (16 * density).toInt(), (24 * density).toInt(), 0)
            addView(phaseText)
            addView(progressBar)
            addView(detailText)
        }

        shaderInstallPhaseText = phaseText
        shaderInstallProgressBar = progressBar
        shaderInstallDetailText = detailText
        shaderInstallProgressDialog = AlertDialog.Builder(context)
            .setTitle(R.string.video_retroarch_shader_install_notification_title)
            .setView(container)
            .setCancelable(false)
            .setPositiveButton(R.string.move_to_background) { dialog, _ -> dialog.dismiss() }
            .setNegativeButton(android.R.string.cancel) { dialog, _ ->
                WorkManager.getInstance(context).cancelUniqueWork(RetroArchShaderInstallWorker.WORK_NAME)
                dialog.dismiss()
            }
            .create()
            .also { it.show() }
    }

    private fun dismissShaderInstallProgressDialog() {
        shaderInstallProgressDialog?.dismiss()
        shaderInstallProgressDialog = null
        shaderInstallProgressBar = null
        shaderInstallPhaseText = null
        shaderInstallDetailText = null
    }

    private fun showShaderInstallError(
        reason: RetroArchShaderLibraryManager.ShaderInstallException.Reason,
        requiredBytes: Long,
    ) {
        val message = when (reason) {
            RetroArchShaderLibraryManager.ShaderInstallException.Reason.NotEnoughSpace -> getString(
                R.string.video_retroarch_shader_error_space,
                Formatter.formatShortFileSize(requireContext(), requiredBytes),
            )
            RetroArchShaderLibraryManager.ShaderInstallException.Reason.Truncated,
            RetroArchShaderLibraryManager.ShaderInstallException.Reason.CorruptArchive,
            -> getString(R.string.video_retroarch_shader_error_corrupt)
            else -> getString(R.string.video_retroarch_shader_error_network)
        }

        AlertDialog.Builder(requireContext())
            .setMessage(message)
            .setPositiveButton(R.string.video_retroarch_shader_retry) { _, _ -> startShaderInstall() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun videoFilteringOrNone(value: String?): VideoFiltering {
        return runCatching { enumValueOfIgnoreCase<VideoFiltering>(value.orEmpty()) }
            .getOrDefault(VideoFiltering.NONE)
    }

    private fun applyRetroArchPresetSelection(preference: ListPreference, selectedPreset: String) {
        pendingDsLitePreset = null
        preference.value = selectedPreset
        preference.entries = arrayOf(selectedPreset)
        preference.entryValues = arrayOf(selectedPreset)
        preference.summary = selectedPreset
        refreshShaderPreferenceVisibility()
    }

    private fun showDsLitePresetDialog() {
        val preferences = preferenceManager.sharedPreferences
        val currentPreset = preferences?.getString("video_retroarch_shader_preset", null)
        val enabled = preferences?.getString("video_filtering", "none") == "retroarch" &&
            resolveShaderSource() == RetroArchShaderSource.INTERNAL
        val selectedIndex = DSLITE_PRESETS.indexOfFirst { enabled && it.second == currentPreset }
        AlertDialog.Builder(requireContext())
            .setTitle(R.string.video_dslite_preset_title)
            .setSingleChoiceItems(DSLITE_PRESETS.map { getString(it.first) }.toTypedArray(), selectedIndex) { dialog, which ->
                dialog.dismiss()
                pendingDsLitePreset = null
                selectDsLitePreset(DSLITE_PRESETS[which].second)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun selectDsLitePreset(preset: String, allowDownload: Boolean = true) {
        if (DSLITE_PRESETS.none { it.second == preset }) {
            return
        }
        lifecycleScope.launch {
            val available = withContext(Dispatchers.IO) {
                shaderLibraryManager.libraryRoot?.let { File(it, preset).isFile } == true
            }
            if (available) {
                preferenceManager.sharedPreferences?.edit()
                    ?.putString("video_retroarch_shader_source", RetroArchShaderSource.INTERNAL.preferenceValue)
                    ?.putString("video_retroarch_shader_preset", preset)
                    ?.putString("video_filtering", "retroarch")
                    ?.remove("video_retroarch_shader_parameters")
                    ?.putBoolean("video_retroarch_shader_clear_history", false)
                    ?.apply()
                findPreference<ListPreference>("video_filtering")?.value = "retroarch"
                findPreference<EditTextPreference>("video_retroarch_shader_parameters")?.text = null
                findPreference<SwitchPreference>("video_retroarch_shader_clear_history")?.isChecked = false
                applyRetroArchPresetSelection(shaderPresetPreference, preset)
            } else if (allowDownload) {
                AlertDialog.Builder(requireContext())
                    .setMessage(R.string.video_dslite_download)
                    .setPositiveButton(R.string.video_retroarch_shader_install_title) { _, _ ->
                        pendingDsLitePreset = preset
                        startShaderInstall()
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
            } else {
                Toast.makeText(requireContext(), R.string.video_dslite_unavailable, Toast.LENGTH_LONG).show()
            }
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(PENDING_DSLITE_PRESET, pendingDsLitePreset)
    }

    private fun showVulkanUnavailableDialog() {
        AlertDialog.Builder(requireContext())
            .setTitle(R.string.renderer_init_failed_title)
            .setMessage(getString(R.string.renderer_init_failed_message, "Vulkan"))
            .setPositiveButton(R.string.ok, null)
            .show()
    }

    private fun updateDsiCameraImagePreference(preference: StoragePickerPreference, dsiCameraSourceValue: String) {
        val newSource = enumValueOfIgnoreCase<DSiCameraSourceType>(dsiCameraSourceValue)
        preference.isVisible = newSource == DSiCameraSourceType.STATIC_IMAGE
    }

    override fun onResume() {
        super.onResume()
        updateDualScreenPresetSummary()
        if (::shaderPresetPreference.isInitialized) {
            updateRetroArchPresetEntries(shaderPresetPreference, resetSelection = false)
            refreshShaderPreferenceVisibility()
        }
    }

    private fun updateDualScreenPresetSummary() {
        if (!this::dualScreenPresetsPreference.isInitialized) {
            return
        }
        val preset = settingsRepository.getDualScreenPreset()
        val keepAspect = settingsRepository.isExternalDisplayKeepAspectRationEnabled()
        val integerScale = settingsRepository.isDualScreenIntegerScaleEnabled() && preset != DualScreenPreset.OFF
        val fillModesActive = preset != DualScreenPreset.OFF && (integerScale || keepAspect)
        val internalFillHeight = settingsRepository.isDualScreenInternalFillHeightEnabled() && fillModesActive
        val internalFillWidth = settingsRepository.isDualScreenInternalFillWidthEnabled() && fillModesActive
        val externalFillHeight = settingsRepository.isDualScreenExternalFillHeightEnabled() && fillModesActive
        val externalFillWidth = settingsRepository.isDualScreenExternalFillWidthEnabled() && fillModesActive

        val presetTextRes = when (preset) {
            DualScreenPreset.OFF -> R.string.dual_screen_preset_off
            DualScreenPreset.INTERNAL_TOP_EXTERNAL_BOTTOM -> R.string.dual_screen_preset_internal_top_external_bottom
            DualScreenPreset.INTERNAL_BOTTOM_EXTERNAL_TOP -> R.string.dual_screen_preset_internal_bottom_external_top
        }

        dualScreenPresetsPreference.summary = getString(
            R.string.dual_screen_presets_summary,
            getString(presetTextRes),
            if (preset != DualScreenPreset.OFF && keepAspect) getString(R.string.on) else getString(R.string.off),
            if (preset != DualScreenPreset.OFF && integerScale) getString(R.string.on) else getString(R.string.off),
            if (internalFillHeight) getString(R.string.on) else getString(R.string.off),
            if (internalFillWidth) getString(R.string.on) else getString(R.string.off),
            if (externalFillHeight) getString(R.string.on) else getString(R.string.off),
            if (externalFillWidth) getString(R.string.on) else getString(R.string.off),
        )
    }

    private fun showDualScreenPresetsDialog() {
        val currentPreset = settingsRepository.getDualScreenPreset()
        val keepAspectRatioInitial = settingsRepository.isExternalDisplayKeepAspectRationEnabled()
        val integerScaleInitial = settingsRepository.isDualScreenIntegerScaleEnabled()
        val internalFillInitial = settingsRepository.isDualScreenInternalFillHeightEnabled()
        val internalFillWidthInitial = settingsRepository.isDualScreenInternalFillWidthEnabled()
        val externalFillInitial = settingsRepository.isDualScreenExternalFillHeightEnabled()
        val externalFillWidthInitial = settingsRepository.isDualScreenExternalFillWidthEnabled()
        val internalAlignmentInitial = settingsRepository.getDualScreenInternalVerticalAlignmentOverride()
        val externalAlignmentInitial = settingsRepository.getDualScreenExternalVerticalAlignmentOverride()

        val dialogView = LayoutInflater.from(requireContext()).inflate(R.layout.dialog_dual_screen_presets, null)
        val radioGroup = dialogView.findViewById<RadioGroup>(R.id.radioGroupPresets)
        val keepAspectSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchKeepAspectRatio)
        val sameScreenSizeSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchSameScreenSize)
        sameScreenSizeSwitch.isChecked = settingsRepository.isSameScreenSizeEnabled()
        val integerScaleSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchIntegerScale)
        val fillAreaButton = dialogView.findViewById<Button>(R.id.buttonFillAreaOptions)
        val verticalAlignmentButton = dialogView.findViewById<Button>(R.id.buttonVerticalAlignmentOptions)
        val verticalAlignmentSummary = dialogView.findViewById<TextView>(R.id.textVerticalAlignmentSummary)
        val presetsDisabledHint = dialogView.findViewById<TextView>(R.id.textPresetsDisabledHint)

        val presetToButtonId = mapOf(
            DualScreenPreset.OFF to R.id.radioPresetOff,
            DualScreenPreset.INTERNAL_TOP_EXTERNAL_BOTTOM to R.id.radioPresetInternalTopExternalBottom,
            DualScreenPreset.INTERNAL_BOTTOM_EXTERNAL_TOP to R.id.radioPresetInternalBottomExternalTop,
        )
        var selectedPreset = currentPreset
        var keepAspectRatio = keepAspectRatioInitial
        var integerScale = integerScaleInitial && currentPreset != DualScreenPreset.OFF
        var internalFill = internalFillInitial
        var internalFillWidth = internalFillWidthInitial
        var externalFill = externalFillInitial
        var externalFillWidth = externalFillWidthInitial
        var internalAlignmentOverride = internalAlignmentInitial
        var externalAlignmentOverride = externalAlignmentInitial

        radioGroup.check(presetToButtonId.getValue(currentPreset))
        keepAspectSwitch.isChecked = keepAspectRatio
        integerScaleSwitch.isChecked = integerScale

        fun updateDualScreenButtonsState() {
            val presetSelected = selectedPreset != DualScreenPreset.OFF
            presetsDisabledHint.isVisible = !presetSelected
            keepAspectSwitch.isEnabled = presetSelected
            integerScaleSwitch.isEnabled = presetSelected

            val enabled = presetSelected && (integerScale || keepAspectRatio)
            fillAreaButton.isEnabled = enabled
            verticalAlignmentButton.isEnabled = enabled
        }
        fun updateVerticalAlignmentSummary() {
            verticalAlignmentSummary.text = getVerticalAlignmentSummary(selectedPreset, internalAlignmentOverride, externalAlignmentOverride)
            verticalAlignmentSummary.isVisible = true
        }
        updateVerticalAlignmentSummary()
        updateDualScreenButtonsState()

        radioGroup.setOnCheckedChangeListener { _, checkedId ->
            selectedPreset = presetToButtonId.entries.first { it.value == checkedId }.key
            if (selectedPreset == DualScreenPreset.OFF) {
                integerScaleSwitch.isChecked = false
                integerScale = false
            }
            updateDualScreenButtonsState()
            updateVerticalAlignmentSummary()
        }

        keepAspectSwitch.setOnCheckedChangeListener { _, isChecked ->
            keepAspectRatio = isChecked
            updateDualScreenButtonsState()
        }

        integerScaleSwitch.setOnCheckedChangeListener { _, isChecked ->
            integerScale = isChecked
            updateDualScreenButtonsState()
        }

        fillAreaButton.setOnClickListener {
            val fillOptionsEnabled = selectedPreset != DualScreenPreset.OFF && (integerScale || keepAspectRatio)
            showFillAreaOptionsDialog(
                fillOptionsEnabled = fillOptionsEnabled,
                initialInternalFillHeight = internalFill,
                initialInternalFillWidth = internalFillWidth,
                initialExternalFillHeight = externalFill,
                initialExternalFillWidth = externalFillWidth,
            ) { newInternalFill, newInternalFillWidth, newExternalFill, newExternalFillWidth ->
                internalFill = newInternalFill
                internalFillWidth = newInternalFillWidth
                externalFill = newExternalFill
                externalFillWidth = newExternalFillWidth
            }
        }

        verticalAlignmentButton.setOnClickListener {
            showVerticalAlignmentOptionsDialog(
                preset = selectedPreset,
                initialInternalAlignment = internalAlignmentOverride,
                initialExternalAlignment = externalAlignmentOverride,
            ) { newInternalAlignment, newExternalAlignment ->
                internalAlignmentOverride = newInternalAlignment
                externalAlignmentOverride = newExternalAlignment
                updateVerticalAlignmentSummary()
            }
        }

        AlertDialog.Builder(requireContext())
            .setTitle(R.string.dual_screen_presets_settings_title)
            .setView(dialogView)
            .setPositiveButton(R.string.ok) { _, _ ->
                settingsRepository.setSameScreenSizeEnabled(sameScreenSizeSwitch.isChecked)
                settingsRepository.setDualScreenPreset(selectedPreset)
                settingsRepository.setExternalDisplayKeepAspectRatioEnabled(keepAspectRatio)
                settingsRepository.setDualScreenIntegerScaleEnabled(integerScale && selectedPreset != DualScreenPreset.OFF)
                settingsRepository.setDualScreenInternalFillHeightEnabled(internalFill)
                settingsRepository.setDualScreenInternalFillWidthEnabled(internalFillWidth)
                settingsRepository.setDualScreenExternalFillHeightEnabled(externalFill)
                settingsRepository.setDualScreenExternalFillWidthEnabled(externalFillWidth)
                settingsRepository.setDualScreenInternalVerticalAlignmentOverride(internalAlignmentOverride)
                settingsRepository.setDualScreenExternalVerticalAlignmentOverride(externalAlignmentOverride)
                updateDualScreenPresetSummary()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showFillAreaOptionsDialog(
        fillOptionsEnabled: Boolean,
        initialInternalFillHeight: Boolean,
        initialInternalFillWidth: Boolean,
        initialExternalFillHeight: Boolean,
        initialExternalFillWidth: Boolean,
        onValuesConfirmed: (Boolean, Boolean, Boolean, Boolean) -> Unit,
    ) {
        val dialogView = LayoutInflater.from(requireContext()).inflate(R.layout.dialog_dual_screen_fill_area, null)
        val description = dialogView.findViewById<TextView>(R.id.textFillAreaDescription)
        val disabledText = dialogView.findViewById<TextView>(R.id.textFillAreaDisabled)
        val internalEnabledSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchInternalFillEnabled)
        val internalHeightSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchInternalFillHeight)
        val internalWidthSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchInternalFillWidth)
        val externalEnabledSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchExternalFillEnabled)
        val externalHeightSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchExternalFillHeight)
        val externalWidthSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchExternalFillWidth)

        description.text = getString(R.string.dual_screen_fill_area_description)
        internalHeightSwitch.isChecked = initialInternalFillHeight
        internalWidthSwitch.isChecked = initialInternalFillWidth
        externalHeightSwitch.isChecked = initialExternalFillHeight
        externalWidthSwitch.isChecked = initialExternalFillWidth
        internalEnabledSwitch.isChecked = initialInternalFillHeight || initialInternalFillWidth
        externalEnabledSwitch.isChecked = initialExternalFillHeight || initialExternalFillWidth

        fun updateInternalSection(enabled: Boolean, mutateValues: Boolean) {
            val childEnabled = fillOptionsEnabled && enabled
            internalHeightSwitch.isEnabled = childEnabled
            internalWidthSwitch.isEnabled = childEnabled
            if (!mutateValues) {
                return
            }
            if (!enabled) {
                internalHeightSwitch.isChecked = false
                internalWidthSwitch.isChecked = false
            } else if (!internalHeightSwitch.isChecked && !internalWidthSwitch.isChecked) {
                internalHeightSwitch.isChecked = true
            }
        }

        fun updateExternalSection(enabled: Boolean, mutateValues: Boolean) {
            val childEnabled = fillOptionsEnabled && enabled
            externalHeightSwitch.isEnabled = childEnabled
            externalWidthSwitch.isEnabled = childEnabled
            if (!mutateValues) {
                return
            }
            if (!enabled) {
                externalHeightSwitch.isChecked = false
                externalWidthSwitch.isChecked = false
            } else if (!externalHeightSwitch.isChecked && !externalWidthSwitch.isChecked) {
                externalHeightSwitch.isChecked = true
            }
        }

        internalEnabledSwitch.isEnabled = fillOptionsEnabled
        externalEnabledSwitch.isEnabled = fillOptionsEnabled
        updateInternalSection(internalEnabledSwitch.isChecked, mutateValues = false)
        updateExternalSection(externalEnabledSwitch.isChecked, mutateValues = false)
        internalEnabledSwitch.setOnCheckedChangeListener { _, isChecked ->
            updateInternalSection(isChecked, mutateValues = true)
        }
        externalEnabledSwitch.setOnCheckedChangeListener { _, isChecked ->
            updateExternalSection(isChecked, mutateValues = true)
        }
        disabledText.isVisible = !fillOptionsEnabled

        AlertDialog.Builder(requireContext())
            .setTitle(R.string.dual_screen_fill_area_title)
            .setView(dialogView)
            .setPositiveButton(R.string.ok) { _, _ ->
                val internalEnabled = fillOptionsEnabled && internalEnabledSwitch.isChecked
                val externalEnabled = fillOptionsEnabled && externalEnabledSwitch.isChecked
                onValuesConfirmed(
                    if (internalEnabled) internalHeightSwitch.isChecked else false,
                    if (internalEnabled) internalWidthSwitch.isChecked else false,
                    if (externalEnabled) externalHeightSwitch.isChecked else false,
                    if (externalEnabled) externalWidthSwitch.isChecked else false,
                )
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showVerticalAlignmentOptionsDialog(
        preset: DualScreenPreset,
        initialInternalAlignment: ScreenAlignment?,
        initialExternalAlignment: ScreenAlignment?,
        onValuesConfirmed: (ScreenAlignment?, ScreenAlignment?) -> Unit,
    ) {
        val dialogView = LayoutInflater.from(requireContext()).inflate(R.layout.dialog_dual_screen_vertical_alignment, null)
        val description = dialogView.findViewById<TextView>(R.id.textVerticalAlignmentDescription)
        val defaults = dialogView.findViewById<TextView>(R.id.textVerticalAlignmentDefaults)
        description.text = getString(R.string.dual_screen_vertical_alignment_description)
        defaults.text = getString(
            R.string.dual_screen_vertical_alignment_default_hint,
            getAlignmentDisplayName(preset.defaultInternalAlignment()),
            getAlignmentDisplayName(preset.defaultExternalAlignment()),
        )

        val internalToggle = dialogView.findViewById<SwitchCompat>(R.id.switchInternalAlignmentOverride)
        val externalToggle = dialogView.findViewById<SwitchCompat>(R.id.switchExternalAlignmentOverride)
        val internalRadioGroup = dialogView.findViewById<RadioGroup>(R.id.radioGroupInternalAlignment)
        val externalRadioGroup = dialogView.findViewById<RadioGroup>(R.id.radioGroupExternalAlignment)
        val internalRadios = mapOf(
            ScreenAlignment.TOP to dialogView.findViewById<RadioButton>(R.id.radioInternalAlignmentTop),
            ScreenAlignment.CENTER to dialogView.findViewById<RadioButton>(R.id.radioInternalAlignmentCenter),
            ScreenAlignment.BOTTOM to dialogView.findViewById<RadioButton>(R.id.radioInternalAlignmentBottom),
        )
        val externalRadios = mapOf(
            ScreenAlignment.TOP to dialogView.findViewById<RadioButton>(R.id.radioExternalAlignmentTop),
            ScreenAlignment.CENTER to dialogView.findViewById<RadioButton>(R.id.radioExternalAlignmentCenter),
            ScreenAlignment.BOTTOM to dialogView.findViewById<RadioButton>(R.id.radioExternalAlignmentBottom),
        )

        var currentInternalSelection = initialInternalAlignment ?: preset.defaultInternalAlignment()
        var currentExternalSelection = initialExternalAlignment ?: preset.defaultExternalAlignment()
        var pendingInternalAlignment = initialInternalAlignment
        var pendingExternalAlignment = initialExternalAlignment
        var updatingInternalRadios = false
        var updatingExternalRadios = false

        fun setRadiosEnabled(radios: Collection<RadioButton>, enabled: Boolean) {
            radios.forEach { it.isEnabled = enabled }
        }

        fun applyInternalSelection() {
            updatingInternalRadios = true
            internalRadioGroup.check(internalRadios.getValue(currentInternalSelection).id)
            updatingInternalRadios = false
        }

        fun applyExternalSelection() {
            updatingExternalRadios = true
            externalRadioGroup.check(externalRadios.getValue(currentExternalSelection).id)
            updatingExternalRadios = false
        }

        internalToggle.isChecked = pendingInternalAlignment != null
        externalToggle.isChecked = pendingExternalAlignment != null
        applyInternalSelection()
        applyExternalSelection()
        setRadiosEnabled(internalRadios.values, internalToggle.isChecked)
        setRadiosEnabled(externalRadios.values, externalToggle.isChecked)

        internalToggle.setOnCheckedChangeListener { _, isChecked ->
            setRadiosEnabled(internalRadios.values, isChecked)
            pendingInternalAlignment = if (isChecked) currentInternalSelection else null
        }
        externalToggle.setOnCheckedChangeListener { _, isChecked ->
            setRadiosEnabled(externalRadios.values, isChecked)
            pendingExternalAlignment = if (isChecked) currentExternalSelection else null
        }

        internalRadioGroup.setOnCheckedChangeListener { _, checkedId ->
            if (updatingInternalRadios) {
                return@setOnCheckedChangeListener
            }
            currentInternalSelection = when (checkedId) {
                R.id.radioInternalAlignmentTop -> ScreenAlignment.TOP
                R.id.radioInternalAlignmentCenter -> ScreenAlignment.CENTER
                R.id.radioInternalAlignmentBottom -> ScreenAlignment.BOTTOM
                else -> ScreenAlignment.TOP
            }
            if (internalToggle.isChecked) {
                pendingInternalAlignment = currentInternalSelection
            }
        }

        externalRadioGroup.setOnCheckedChangeListener { _, checkedId ->
            if (updatingExternalRadios) {
                return@setOnCheckedChangeListener
            }
            currentExternalSelection = when (checkedId) {
                R.id.radioExternalAlignmentTop -> ScreenAlignment.TOP
                R.id.radioExternalAlignmentCenter -> ScreenAlignment.CENTER
                R.id.radioExternalAlignmentBottom -> ScreenAlignment.BOTTOM
                else -> ScreenAlignment.TOP
            }
            if (externalToggle.isChecked) {
                pendingExternalAlignment = currentExternalSelection
            }
        }

        AlertDialog.Builder(requireContext())
            .setTitle(R.string.dual_screen_vertical_alignment_title)
            .setView(dialogView)
            .setPositiveButton(R.string.ok) { _, _ ->
                onValuesConfirmed(pendingInternalAlignment, pendingExternalAlignment)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun getVerticalAlignmentSummary(
        preset: DualScreenPreset,
        internalOverride: ScreenAlignment?,
        externalOverride: ScreenAlignment?,
    ): String {
        val internalDefault = preset.defaultInternalAlignment()
        val externalDefault = preset.defaultExternalAlignment()
        val internalLabel = formatAlignmentLabel(internalOverride, internalDefault)
        val externalLabel = formatAlignmentLabel(externalOverride, externalDefault)
        return getString(R.string.dual_screen_vertical_alignment_summary, internalLabel, externalLabel)
    }

    private fun formatAlignmentLabel(override: ScreenAlignment?, defaultAlignment: ScreenAlignment): String {
        return if (override == null) {
            getString(
                R.string.dual_screen_vertical_alignment_preset_value,
                getAlignmentDisplayName(defaultAlignment),
            )
        } else {
            getAlignmentDisplayName(override)
        }
    }

    private fun getAlignmentDisplayName(alignment: ScreenAlignment): String {
        return when (alignment) {
            ScreenAlignment.TOP -> getString(R.string.dual_screen_vertical_alignment_option_top)
            ScreenAlignment.CENTER -> getString(R.string.dual_screen_vertical_alignment_option_center)
            ScreenAlignment.BOTTOM -> getString(R.string.dual_screen_vertical_alignment_option_bottom)
        }
    }

    override fun getTitle(): String {
        return when (arguments?.getString(ARG_PREFERENCE_ROOT)) {
            FREEDRENO_SETTINGS_KEY -> getString(R.string.freedreno_settings)
            SHADER_SETTINGS_KEY -> getString(R.string.video_retroarch_shader_settings_title)
            else -> getString(R.string.category_video)
        }
    }
}
