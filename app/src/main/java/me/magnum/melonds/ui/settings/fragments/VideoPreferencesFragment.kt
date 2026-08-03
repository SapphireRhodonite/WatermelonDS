package me.magnum.melonds.ui.settings.fragments

import android.app.ActivityManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
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
import me.magnum.melonds.ui.settings.preferences.InGameLockedListPreference
import me.magnum.melonds.ui.settings.preferences.InGameLockedSwitchPreference
import me.magnum.melonds.ui.settings.preferences.StoragePickerPreference
import me.magnum.melonds.extensions.addOnPreferenceChangeListener
import me.magnum.melonds.impl.AdrenoVulkanDriverManager
import me.magnum.melonds.utils.enumValueOfIgnoreCase
import androidx.appcompat.app.AlertDialog
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import androidx.appcompat.widget.SwitchCompat
import androidx.core.view.isVisible
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import javax.inject.Inject

@AndroidEntryPoint
class VideoPreferencesFragment : BasePreferenceFragment(), PreferenceFragmentTitleProvider {

    private companion object {
        const val GLES_3_2 = 0x30002
        const val VULKAN_SYSTEM_DRIVER_VALUE = "system"
    }

    private val helper by lazy { PreferenceFragmentHelper(this, uriPermissionManager, directoryAccessValidator) }
    @Inject lateinit var uriPermissionManager: UriPermissionManager
    @Inject lateinit var directoryAccessValidator: DirectoryAccessValidator
    @Inject lateinit var settingsRepository: SettingsRepository

    private val threadedRendererPreferences = mutableListOf<Preference>()
    private val highResRendererPreferences = mutableListOf<Preference>()
    private val vulkanRendererPreferences = mutableListOf<Preference>()
    private val rendererDebugPreferences = mutableListOf<Preference>()
    private val coverageFixPreferences = mutableListOf<Preference>()

    private lateinit var dualScreenPresetsPreference: Preference
    private lateinit var adrenoVulkanDriverManager: AdrenoVulkanDriverManager
    private var retroArchPresetScanJob: Job? = null
    private val vulkanDriverImportLauncher = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        handleVulkanDriverImport(uri)
    }

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.pref_video, rootKey)
        adrenoVulkanDriverManager = AdrenoVulkanDriverManager(requireContext(), settingsRepository)

        val launchedInGame = requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_IN_GAME, false)
        val rendererPreference = findPreference<InGameLockedListPreference>("video_renderer")!!
        val internalResolutionPreference = findPreference<InGameLockedListPreference>("video_internal_resolution")!!
        listOf(rendererPreference, internalResolutionPreference).forEach {
            it.isInGameLocked = launchedInGame
            it.inGameLockedMessageRes = R.string.video_setting_cannot_change_ingame
        }
        val vulkanFastPathPreference =
            findPreference<InGameLockedSwitchPreference>("video_vulkan_fastpath_enabled")!!
        vulkanFastPathPreference.isInGameLocked = launchedInGame
        vulkanFastPathPreference.inGameLockedMessageRes = R.string.video_setting_cannot_change_ingame
        vulkanFastPathPreference.setOnPreferenceChangeListener { preference, newValue ->
            if (newValue != true) {
                applyVulkanFastPathSelection(
                    preference as InGameLockedSwitchPreference,
                    enabled = false,
                )
                return@setOnPreferenceChangeListener false
            }

            AlertDialog.Builder(requireContext())
                .setTitle(R.string.video_vulkan_fastpath_warning_title)
                .setMessage(R.string.video_vulkan_fastpath_warning_message)
                .setPositiveButton(R.string.video_vulkan_fastpath_enable_action) { _, _ ->
                    applyVulkanFastPathSelection(
                        preference as InGameLockedSwitchPreference,
                        enabled = true,
                    )
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
            false
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

        vulkanRendererPreferences.apply {
            add(vulkanFastPathPreference)
        }

        coverageFixPreferences.apply {
            add(findPreference("video_conservative_coverage_enabled")!!)
            add(findPreference("video_conservative_coverage_px")!!)
            add(findPreference("video_conservative_coverage_apply_repeat")!!)
            add(findPreference("video_conservative_coverage_apply_clamp")!!)
            add(findPreference("video_conservative_coverage_depth_bias")!!)
        }

        val videoFilteringPreference = findPreference<InGameLockedListPreference>("video_filtering")!!
        videoFilteringPreference.isInGameLocked = launchedInGame &&
            requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_LOCK_VIDEO_FILTERING, false)
        videoFilteringPreference.inGameLockedMessageRes = R.string.cannot_change_use_rom_settings
        val dsiCameraSourcePreference = findPreference<ListPreference>("dsi_camera_source")!!
        val dsiCameraImagePreference = findPreference<StoragePickerPreference>("dsi_camera_static_image")!!
        val retroArchShaderRootPreference = findPreference<StoragePickerPreference>("video_retroarch_shader_root")!!
        val retroArchShaderPresetPreference = findPreference<ListPreference>("video_retroarch_shader_preset")!!
        val retroArchShaderParametersPreference = findPreference<EditTextPreference>("video_retroarch_shader_parameters")!!
        val retroArchShaderClearHistoryPreference = findPreference<SwitchPreference>("video_retroarch_shader_clear_history")!!
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
                updateVulkanDriverPreferenceState(
                    renderer = newRenderer,
                    category = vulkanDriverCategory,
                    modePreference = vulkanDriverModePreference,
                    importPreference = vulkanDriverImportPreference,
                    removePreference = vulkanDriverRemovePreference,
                    launchedInGame = launchedInGame,
                )
                if (newRenderer != VideoRenderer.VULKAN && vulkanFastPathPreference.isChecked) {
                    applyVulkanFastPathSelection(vulkanFastPathPreference, enabled = false)
                }
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
        helper.bindPreferenceSummaryToValue(retroArchShaderParametersPreference)
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
            updateShaderPickerPreferences(
                renderer = enumValueOfIgnoreCase(rendererPreference.value),
                filteringValue = newValue as String,
                retroArchShaderRootPreference = retroArchShaderRootPreference,
                retroArchShaderPresetPreference = retroArchShaderPresetPreference,
                retroArchShaderParametersPreference = retroArchShaderParametersPreference,
                retroArchShaderClearHistoryPreference = retroArchShaderClearHistoryPreference,
            )
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

    private fun setupVulkanDriverPreferences(
        renderer: VideoRenderer,
        category: PreferenceCategory,
        modePreference: ListPreference,
        importPreference: Preference,
        removePreference: Preference,
        launchedInGame: Boolean,
    ) {
        modePreference.isPersistent = false
        modePreference.setOnPreferenceClickListener {
            showVulkanDriverSelectionDialog(modePreference, removePreference)
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

    private fun updateVulkanDriverPreferenceState(
        renderer: VideoRenderer,
        category: PreferenceCategory,
        modePreference: ListPreference,
        importPreference: Preference,
        removePreference: Preference,
        launchedInGame: Boolean,
    ) {
        val visible = renderer == VideoRenderer.VULKAN && adrenoVulkanDriverManager.isSupported
        category.isVisible = visible
        if (!visible) {
            return
        }

        listOf(modePreference, importPreference, removePreference).forEach {
            it.isEnabled = !launchedInGame
        }
        category.summary = if (launchedInGame) {
            getString(R.string.video_setting_cannot_change_ingame)
        } else {
            null
        }
        updateVulkanDriverPreferenceSummaries(modePreference, removePreference)
    }

    private fun showVulkanDriverSelectionDialog(
        modePreference: ListPreference,
        removePreference: Preference,
    ) {
        val installedDrivers = settingsRepository.getInstalledVulkanDrivers()
        val values = listOf(VULKAN_SYSTEM_DRIVER_VALUE) + installedDrivers.map { it.id }
        val entries = listOf(getString(R.string.video_vulkan_driver_mode_system)) +
            installedDrivers.map { it.displayName }
        val selectedValue = modePreference.value ?: VULKAN_SYSTEM_DRIVER_VALUE
        val checkedIndex = values.indexOf(selectedValue).takeIf { it >= 0 } ?: 0

        AlertDialog.Builder(requireContext())
            .setTitle(R.string.video_vulkan_driver_mode)
            .setSingleChoiceItems(entries.toTypedArray(), checkedIndex) { dialog, which ->
                val value = values[which]
                if (value == VULKAN_SYSTEM_DRIVER_VALUE) {
                    settingsRepository.setVulkanDriverMode(VulkanDriverMode.SYSTEM)
                } else {
                    settingsRepository.setSelectedVulkanDriver(value)
                }
                updateVulkanDriverPreferenceSummaries(modePreference, removePreference)
                dialog.dismiss()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
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
        removePreference.isEnabled = installedDrivers.isNotEmpty() && modePreference.isEnabled
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

    private fun updateRetroArchPresetEntries(preference: ListPreference, rootUriOverride: Uri? = null) {
        val rootUri = rootUriOverride ?: preferenceManager.sharedPreferences
            ?.getStringSet("video_retroarch_shader_root", null)
            ?.firstOrNull()
            ?.toUri()

        retroArchPresetScanJob?.cancel()
        if (rootUriOverride != null) {
            preference.value = null
        }

        val selectedPreset = preference.value
        preference.entries = selectedPreset?.let { arrayOf(it) } ?: emptyArray()
        preference.entryValues = selectedPreset?.let { arrayOf(it) } ?: emptyArray()
        preference.summary = selectedPreset ?: getString(R.string.video_retroarch_shader_preset_summary)

        if (rootUri == null) {
            return
        }
    }

    private fun showRetroArchPresetBrowserDialog(preference: ListPreference) {
        val rootUri = preferenceManager.sharedPreferences
            ?.getStringSet("video_retroarch_shader_root", null)
            ?.firstOrNull()
            ?.toUri()

        if (rootUri == null) {
            preference.summary = getString(R.string.video_retroarch_shader_preset_summary)
            return
        }

        val context = requireContext()
        val density = resources.displayMetrics.density
        data class BrowserItem(
            val label: String,
            val path: String,
            val isDirectory: Boolean,
        )

        var currentDirectory = ""
        val browserItems = mutableListOf<BrowserItem>()
        val itemLabels = mutableListOf<String>()
        val folderCache = mutableMapOf<String, List<BrowserItem>>()
        val adapter = ArrayAdapter(context, android.R.layout.simple_list_item_1, itemLabels)
        val pathTextView = TextView(context)
        val listView = ListView(context).apply {
            this.adapter = adapter
            clipToPadding = false
            setPadding(0, 0, 0, (72 * density).toInt())
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                (360 * density).toInt(),
            )
        }
        val container = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(
                (24 * density).toInt(),
                (12 * density).toInt(),
                (24 * density).toInt(),
                0,
            )
            addView(
                pathTextView,
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ),
            )
            addView(listView)
        }

        fun applyItems(items: List<BrowserItem>) {
            pathTextView.text = if (currentDirectory.isBlank()) "/" else "/$currentDirectory"
            browserItems.clear()
            browserItems += items
            itemLabels.clear()
            itemLabels += items.map { it.label }
            adapter.notifyDataSetChanged()
        }

        fun resolveDocument(root: DocumentFile, relativePath: String): DocumentFile? {
            var current = root
            if (relativePath.isBlank()) {
                return current
            }

            relativePath.split('/').forEach { segment ->
                if (segment.isBlank()) {
                    return null
                }
                current = current.findFile(segment) ?: return null
            }
            return current
        }

        fun loadDirectory(relativePath: String) {
            retroArchPresetScanJob?.cancel()
            currentDirectory = relativePath
            applyItems(listOf(BrowserItem(getString(R.string.info_loading), relativePath, true)))

            val cachedItems = folderCache[relativePath]
            if (cachedItems != null) {
                applyItems(cachedItems)
                return
            }

            retroArchPresetScanJob = lifecycleScope.launch {
                val loadedItems = withContext(Dispatchers.IO) {
                    val rootDocument = DocumentFile.fromTreeUri(context, rootUri)
                    val directory = rootDocument?.let { resolveDocument(it, relativePath) }
                    if (directory?.isDirectory != true) {
                        emptyList()
                    } else {
                        val directories = mutableListOf<Pair<String, String>>()
                        val files = mutableListOf<Pair<String, String>>()
                        directory.listFiles().forEach { child ->
                            val name = child.name ?: return@forEach
                            val childPath = if (relativePath.isBlank()) name else "$relativePath/$name"
                            when {
                                child.isDirectory -> directories += name to childPath
                                child.isFile && name.endsWith(".slangp", ignoreCase = true) -> files += name to childPath
                            }
                        }

                        buildList {
                            if (relativePath.isNotBlank()) {
                                add(
                                    BrowserItem(
                                        "..",
                                        relativePath.substringBeforeLast('/', missingDelimiterValue = ""),
                                        true,
                                    ),
                                )
                            }
                            directories.sortedBy { it.first.lowercase() }.forEach { (name, childPath) ->
                                add(BrowserItem("📁 $name", childPath, true))
                            }
                            files.sortedBy { it.first.lowercase() }.forEach { (name, childPath) ->
                                val selectedMark = if (childPath == preference.value) "* " else ""
                                add(BrowserItem("$selectedMark$name", childPath, false))
                            }
                        }
                    }
                }

                folderCache[relativePath] = loadedItems
                applyItems(loadedItems)
            }
        }

        val dialog = AlertDialog.Builder(context)
            .setTitle(preference.title)
            .setView(container)
            .setNegativeButton(android.R.string.cancel, null)
            .create()

        listView.setOnItemClickListener { _, _, position, _ ->
            val item = browserItems.getOrNull(position) ?: return@setOnItemClickListener
            if (item.isDirectory) {
                if (item.label == getString(R.string.info_loading)) {
                    return@setOnItemClickListener
                }
                loadDirectory(item.path)
                return@setOnItemClickListener
            }

            val selectedPreset = item.path
            applyRetroArchPresetSelection(preference, selectedPreset)
            dialog.dismiss()
        }

        dialog.setOnShowListener {
            loadDirectory("")
        }
        dialog.setOnDismissListener {
            retroArchPresetScanJob?.cancel()
            retroArchPresetScanJob = null
        }
        dialog.show()
    }

    private fun onRendererPreferenceChanged(
        rendererValue: String,
        videoFilteringPreference: ListPreference,
        retroArchShaderRootPreference: StoragePickerPreference,
        retroArchShaderPresetPreference: ListPreference,
        retroArchShaderParametersPreference: EditTextPreference,
        retroArchShaderClearHistoryPreference: SwitchPreference,
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
                vulkanRendererPreferences.forEach {
                    it.isVisible = false
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
                vulkanRendererPreferences.forEach {
                    it.isVisible = false
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
                vulkanRendererPreferences.forEach {
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
                vulkanRendererPreferences.forEach {
                    it.isVisible = true
                }
            }
        }

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

    private fun applyVulkanFastPathSelection(
        preference: InGameLockedSwitchPreference,
        enabled: Boolean,
    ) {
        preference.isChecked = enabled
    }

    private fun updateFilteringPreferences(
        renderer: VideoRenderer,
        videoFilteringPreference: ListPreference,
        retroArchShaderRootPreference: StoragePickerPreference,
        retroArchShaderPresetPreference: ListPreference,
        retroArchShaderParametersPreference: EditTextPreference,
        retroArchShaderClearHistoryPreference: SwitchPreference,
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

    private fun updateShaderPickerPreferences(
        renderer: VideoRenderer,
        filteringValue: String,
        retroArchShaderRootPreference: StoragePickerPreference,
        retroArchShaderPresetPreference: ListPreference,
        retroArchShaderParametersPreference: EditTextPreference,
        retroArchShaderClearHistoryPreference: SwitchPreference,
    ) {
        val filtering = videoFilteringOrNone(filteringValue)
        val retroArchEnabled = renderer == VideoRenderer.VULKAN && filtering == VideoFiltering.RETROARCH
        retroArchShaderRootPreference.isEnabled = retroArchEnabled
        retroArchShaderPresetPreference.isEnabled = retroArchEnabled
        retroArchShaderParametersPreference.isEnabled = retroArchEnabled
        retroArchShaderClearHistoryPreference.isEnabled = retroArchEnabled
    }

    private fun videoFilteringOrNone(value: String?): VideoFiltering {
        return runCatching { enumValueOfIgnoreCase<VideoFiltering>(value.orEmpty()) }
            .getOrDefault(VideoFiltering.NONE)
    }

    private fun applyRetroArchPresetSelection(preference: ListPreference, selectedPreset: String) {
        preference.value = selectedPreset
        preference.entries = arrayOf(selectedPreset)
        preference.entryValues = arrayOf(selectedPreset)
        preference.summary = selectedPreset
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
        preference.isEnabled = newSource == DSiCameraSourceType.STATIC_IMAGE
    }

    override fun onResume() {
        super.onResume()
        updateDualScreenPresetSummary()
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
            if (keepAspect) getString(R.string.on) else getString(R.string.off),
            if (preset == DualScreenPreset.OFF) getString(R.string.off) else if (integerScale) getString(R.string.on) else getString(R.string.off),
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
        val integerScaleSwitch = dialogView.findViewById<SwitchCompat>(R.id.switchIntegerScale)
        val fillAreaButton = dialogView.findViewById<Button>(R.id.buttonFillAreaOptions)
        val verticalAlignmentButton = dialogView.findViewById<Button>(R.id.buttonVerticalAlignmentOptions)
        val verticalAlignmentSummary = dialogView.findViewById<TextView>(R.id.textVerticalAlignmentSummary)

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
        integerScaleSwitch.isEnabled = currentPreset != DualScreenPreset.OFF

        fun updateDualScreenButtonsState() {
            val enabled = selectedPreset != DualScreenPreset.OFF && (integerScale || keepAspectRatio)
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
            val integerScaleEnabled = selectedPreset != DualScreenPreset.OFF
            integerScaleSwitch.isEnabled = integerScaleEnabled
            if (!integerScaleEnabled) {
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
        return getString(R.string.category_video)
    }
}
