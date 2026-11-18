package me.magnum.melonds.ui.romdetails

import android.content.ClipData
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.core.net.toUri
import androidx.lifecycle.lifecycleScope
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomIconSource
import me.magnum.melonds.impl.RomSaveFileManager
import me.magnum.melonds.ui.common.rom.EmulatorLaunchValidatorDelegate
import me.magnum.melonds.ui.emulator.EmulatorActivity
import me.magnum.melonds.ui.romdetails.model.RomDetailsToastEvent
import me.magnum.melonds.ui.romdetails.ui.RomDetailsScreen
import me.magnum.melonds.ui.romlist.boxart.BoxArtRepository
import me.magnum.melonds.ui.theme.MelonTheme
import javax.inject.Inject

@AndroidEntryPoint
class RomDetailsActivity : AppCompatActivity() {

    companion object {
        const val KEY_ROM = "rom"
    }

    @Inject lateinit var boxArtRepository: BoxArtRepository
    @Inject lateinit var romSaveFileManager: RomSaveFileManager

    private val romDetailsViewModel by viewModels<RomDetailsViewModel>()
    private val romRetroAchievementsViewModel by viewModels<RomDetailsRetroAchievementsViewModel>()

    private var pendingSaveImportRom: Rom? = null

    private val saveFileImportLauncher = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val rom = pendingSaveImportRom
        pendingSaveImportRom = null

        if (uri != null && rom != null) {
            validateAndConfirmSaveImport(rom, uri)
        }
    }

    private val externalInfoController by lazy { me.magnum.melonds.ui.common.ExternalInfoDisplayController(this) }
    private val focusedAchievement = kotlinx.coroutines.flow.MutableStateFlow<me.magnum.melonds.ui.common.achievements.ui.model.AchievementUiModel?>(null)
    private val focusedSetting = kotlinx.coroutines.flow.MutableStateFlow<Pair<String, String?>?>(null)

    override fun onStart() {
        super.onStart()
        externalInfoController.setContent {
            val rom = romDetailsViewModel.rom.collectAsState().value
            val achievement = focusedAchievement.collectAsState().value
            val setting = focusedSetting.collectAsState().value
            when {
                achievement != null -> me.magnum.melonds.ui.common.ExternalAchievementInfo(achievement)
                setting != null -> me.magnum.melonds.ui.common.ExternalSettingInfo(
                    iconDrawable = null,
                    title = setting.first,
                    description = setting.second,
                    crumb = rom.name,
                )
                else -> {
                    val raCoverUrl by romDetailsViewModel.raCoverUrl.collectAsState()
                    val boxArtUrl by produceState<String?>(initialValue = null, rom.uri, rom.config.iconSource) {
                        value = if (rom.config.iconSource == RomIconSource.DEFAULT) {
                            runCatching { boxArtRepository.getBoxArtUrl(rom) }.getOrNull()
                        } else null
                    }
                    me.magnum.melonds.ui.common.ExternalLibraryGameInfo(
                        rom = rom,
                        boxArtUrl = boxArtUrl,
                        raCoverUrl = raCoverUrl,
                    )
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        externalInfoController.attach()
        romRetroAchievementsViewModel.refreshOfflineAchievementsStatus()
    }

    override fun onPause() {
        externalInfoController.detach()
        super.onPause()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        val emulatorLauncherValidatorDelegate = EmulatorLaunchValidatorDelegate(this, object : EmulatorLaunchValidatorDelegate.Callback {
            override fun onRomValidated(rom: Rom) {
                val intent = EmulatorActivity.getRomEmulatorActivityIntent(this@RomDetailsActivity, rom)
                startActivity(intent)
                overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out)
            }

            override fun onFirmwareValidated(consoleType: ConsoleType, dsiProfile: me.magnum.melonds.domain.model.DSiProfile?) {
                // Do nothing (can't launch firmware fro this screen)
            }

            override fun onValidationAborted() {
                // Do nothing
            }
        })

        setContent {
            val rom by romDetailsViewModel.rom.collectAsState()
            val romConfig by romDetailsViewModel.romConfigUiState.collectAsState()
            val raCoverUrl by romDetailsViewModel.raCoverUrl.collectAsState()

            val retroAchievementsUiState by romRetroAchievementsViewModel.uiState.collectAsState()
            val offlineAchievementsUiState by romRetroAchievementsViewModel.offlineAchievementsUiState.collectAsState()

            val boxArtUrl by produceState<String?>(initialValue = null, rom.uri, rom.config.iconSource) {
                value = if (rom.config.iconSource == RomIconSource.DEFAULT) {
                    runCatching { boxArtRepository.getBoxArtUrl(rom) }.getOrNull()
                } else null
            }

            LaunchedEffect(null) {
                romRetroAchievementsViewModel.viewAchievementEvent.collect {
                    launchViewAchievementIntent(it)
                }
            }

            LaunchedEffect(Unit) {
                romRetroAchievementsViewModel.toastEvent.collectLatest { event ->
                    val message = when (event) {
                        RomDetailsToastEvent.OfflineLedgerDiscardFailed -> getString(R.string.offline_ra_discard_failed)
                        is RomDetailsToastEvent.OfflineAchievementNotSynced -> {
                            val messageRes = when (event.reason) {
                                RomDetailsToastEvent.OfflineAchievementNotSyncedReason.MISSING_FROM_CURRENT_SET -> R.string.offline_ra_sync_skipped_missing_toast
                                RomDetailsToastEvent.OfflineAchievementNotSyncedReason.DEFINITION_CHANGED -> R.string.offline_ra_sync_skipped_definition_changed_toast
                                RomDetailsToastEvent.OfflineAchievementNotSyncedReason.NOT_IN_PREFETCH_CACHE -> R.string.offline_ra_sync_skipped_cache_mismatch_toast
                                RomDetailsToastEvent.OfflineAchievementNotSyncedReason.SERVER_REJECTED -> R.string.offline_ra_sync_skipped_server_rejected_toast
                            }
                            if (event.reason == RomDetailsToastEvent.OfflineAchievementNotSyncedReason.SERVER_REJECTED) {
                                getString(
                                    messageRes,
                                    event.title,
                                    event.reasonDetail ?: getString(R.string.offline_ra_sync_skipped_server_rejected_unknown_reason),
                                )
                            } else {
                                getString(messageRes, event.title)
                            }
                        }
                        is RomDetailsToastEvent.OfflineAchievementsNotSyncedSummary -> {
                            getString(R.string.offline_ra_sync_skipped_summary_toast, event.skippedCount)
                        }
                    }

                    Toast.makeText(this@RomDetailsActivity, message, Toast.LENGTH_LONG).show()
                }
            }

            LaunchedEffect(retroAchievementsUiState) {
                romRetroAchievementsViewModel.refreshOfflineAchievementsStatus()
            }

            MelonTheme {
                RomDetailsScreen(
                    rom = rom,
                    boxArtUrl = boxArtUrl,
                    raCoverUrl = raCoverUrl,
                    romConfigUiState = romConfig,
                    retroAchievementsUiState = retroAchievementsUiState,
                    offlineAchievementsUiState = offlineAchievementsUiState,
                    onNavigateBack = { onNavigateUp() },
                    onLaunchRom = {
                        emulatorLauncherValidatorDelegate.validateRom(it)
                    },
                    onRomConfigUpdate = {
                        romDetailsViewModel.onRomConfigUpdateEvent(it)
                    },
                    onCustomInputConfigEdited = {
                        romDetailsViewModel.refreshRom()
                    },
                    onRetroAchievementsLogin = { username, password ->
                        romRetroAchievementsViewModel.login(username, password)
                    },
                    onRetroAchievementsRetryLoad = {
                        romRetroAchievementsViewModel.retryLoadAchievements()
                    },
                    onViewAchievement = {
                        romRetroAchievementsViewModel.viewAchievement(it)
                    },
                    onOfflineSyncNow = {
                        romRetroAchievementsViewModel.syncOfflineAchievementsNow()
                    },
                    onOfflineDiscardExpired = {
                        romRetroAchievementsViewModel.discardExpiredOfflineAchievements()
                    },
                    onSendSaveFile = { shareSaveFile(rom) },
                    onImportSaveFile = {
                        pendingSaveImportRom = rom
                        saveFileImportLauncher.launch(arrayOf("*/*"))
                    },
                    onAchievementFocused = { focusedAchievement.value = it },
                    onSettingFocused = { title, value ->
                        focusedSetting.value = if (title != null) title to value else null
                    },
                )
            }
        }
    }

    private fun shareSaveFile(rom: Rom) {
        lifecycleScope.launch {
            val sharedSaveFile = runCatching {
                withContext(Dispatchers.IO) {
                    romSaveFileManager.prepareShareFile(rom)
                }
            }.getOrElse {
                Toast.makeText(this@RomDetailsActivity, R.string.rom_save_file_share_failed, Toast.LENGTH_LONG).show()
                return@launch
            }

            if (sharedSaveFile == null) {
                Toast.makeText(this@RomDetailsActivity, R.string.rom_save_file_missing, Toast.LENGTH_LONG).show()
                return@launch
            }

            val shareIntent = Intent(Intent.ACTION_SEND).apply {
                type = "*/*"
                putExtra(Intent.EXTRA_STREAM, sharedSaveFile.uri)
                putExtra(Intent.EXTRA_TITLE, sharedSaveFile.fileName)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                clipData = ClipData.newUri(contentResolver, sharedSaveFile.fileName, sharedSaveFile.uri)
            }
            val chooser = Intent.createChooser(shareIntent, getString(R.string.rom_save_file_share_chooser))
            runCatching { startActivity(chooser) }
                .onFailure {
                    Toast.makeText(this@RomDetailsActivity, R.string.rom_save_file_share_failed, Toast.LENGTH_LONG).show()
                }
        }
    }

    private fun validateAndConfirmSaveImport(rom: Rom, sourceUri: Uri) {
        lifecycleScope.launch {
            val isPlausibleSaveFile = runCatching {
                withContext(Dispatchers.IO) {
                    romSaveFileManager.isPlausibleSaveFile(sourceUri)
                }
            }.getOrDefault(false)

            if (!isPlausibleSaveFile) {
                Toast.makeText(this@RomDetailsActivity, R.string.rom_save_file_import_invalid, Toast.LENGTH_LONG).show()
                return@launch
            }

            AlertDialog.Builder(this@RomDetailsActivity)
                .setTitle(R.string.rom_save_file_import_title)
                .setMessage(getString(R.string.rom_save_file_import_message, rom.config.customName ?: rom.name))
                .setPositiveButton(android.R.string.ok) { _, _ -> importSaveFile(rom, sourceUri) }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
    }

    private fun importSaveFile(rom: Rom, sourceUri: Uri) {
        lifecycleScope.launch {
            val result = runCatching {
                withContext(Dispatchers.IO) {
                    romSaveFileManager.importSaveFile(rom, sourceUri)
                }
            }
            val message = if (result.isSuccess) {
                R.string.rom_save_file_import_success
            } else {
                R.string.rom_save_file_import_failed
            }
            Toast.makeText(this@RomDetailsActivity, message, Toast.LENGTH_LONG).show()
        }
    }

    private fun launchViewAchievementIntent(achievementUrl: String) {
        val intent = Intent(Intent.ACTION_VIEW).apply {
            data = achievementUrl.toUri()
        }
        startActivity(intent)
    }
}
