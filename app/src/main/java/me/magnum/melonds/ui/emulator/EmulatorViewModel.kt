package me.magnum.melonds.ui.emulator

import android.content.Context
import android.content.pm.ApplicationInfo
import android.net.Uri
import android.util.Log
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onCompletion
import kotlinx.coroutines.flow.shareIn
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import me.magnum.melonds.MelonDSAndroidInterface
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.R
import me.magnum.melonds.common.romprocessors.RomFileProcessorFactory
import me.magnum.melonds.common.runtime.ScreenshotFrameBufferProvider
import me.magnum.melonds.database.daos.RetroAchievementsDao
import me.magnum.melonds.database.entities.retroachievements.RAUserAchievementEntity
import me.magnum.melonds.debug.DebugCommandStateStore
import me.magnum.melonds.domain.model.Cheat
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.DualScreenPreset
import me.magnum.melonds.domain.model.FpsCounterPosition
import me.magnum.melonds.domain.model.RomInfo
import me.magnum.melonds.domain.model.RuntimeBackground
import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.SaveStateSlot
import me.magnum.melonds.domain.model.SCREEN_HEIGHT
import me.magnum.melonds.domain.model.SCREEN_WIDTH
import me.magnum.melonds.domain.model.ScreenAlignment
import me.magnum.melonds.domain.model.VideoFiltering
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.VulkanPipelineProfile
import me.magnum.melonds.domain.model.defaultExternalAlignment
import me.magnum.melonds.domain.model.defaultInternalAlignment
import me.magnum.melonds.domain.model.emulator.EmulatorEvent
import me.magnum.melonds.domain.model.emulator.EmulatorSessionUpdateAction
import me.magnum.melonds.domain.model.emulator.FirmwareLaunchResult
import me.magnum.melonds.domain.model.emulator.RomLaunchResult
import me.magnum.melonds.domain.model.layout.BackgroundMode
import me.magnum.melonds.domain.model.layout.Insets
import me.magnum.melonds.domain.model.layout.LayoutConfiguration
import me.magnum.melonds.domain.model.layout.LayoutDisplayPair
import me.magnum.melonds.domain.model.layout.PositionedLayoutComponent
import me.magnum.melonds.domain.model.layout.ScreenFold
import me.magnum.melonds.domain.model.layout.ScreenLayout
import me.magnum.melonds.domain.model.layout.UILayout
import me.magnum.melonds.domain.model.layout.UILayoutVariant
import me.magnum.melonds.domain.model.retroachievements.GameAchievementData
import me.magnum.melonds.domain.model.retroachievements.RAEvent
import me.magnum.melonds.domain.model.retroachievements.RARuntimeBridgeConfig
import me.magnum.melonds.domain.model.retroachievements.RARuntimeBridgeMode
import me.magnum.melonds.domain.model.retroachievements.RASimpleAchievement
import me.magnum.melonds.domain.model.input.SoftInputBehaviour
import me.magnum.melonds.domain.model.retroachievements.RASimpleLeaderboard
import me.magnum.melonds.domain.model.retroachievements.exception.RAGameNotExist
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RuntimeMicSource
import me.magnum.melonds.domain.model.ui.Orientation
import me.magnum.melonds.domain.repositories.BackgroundRepository
import me.magnum.melonds.domain.repositories.CheatsRepository
import me.magnum.melonds.domain.repositories.LayoutsRepository
import me.magnum.melonds.domain.repositories.RetroAchievementsRepository
import me.magnum.melonds.domain.repositories.RomsRepository
import me.magnum.melonds.domain.repositories.SaveStatesRepository
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.domain.services.EmulatorManager
import me.magnum.melonds.impl.emulator.EmulatorSession
import me.magnum.melonds.impl.emulator.debug.RendererDebugCaptureLogger
import me.magnum.melonds.impl.retroachievements.offline.OfflineLedgerIntegrity
import me.magnum.melonds.impl.retroachievements.offline.OfflineLedgerExpiredException
import me.magnum.melonds.impl.retroachievements.offline.OfflineLedgerRepository
import me.magnum.melonds.impl.retroachievements.offline.OfflinePrefetchCacheAchievement
import me.magnum.melonds.impl.retroachievements.offline.OfflinePrefetchCacheFile
import me.magnum.melonds.impl.retroachievements.offline.OfflinePrefetchCacheLeaderboard
import me.magnum.melonds.impl.retroachievements.offline.OfflinePrefetchCacheRepository
import me.magnum.melonds.impl.retroachievements.offline.OfflineUnlockMode
import me.magnum.melonds.impl.retroachievements.offline.OfflineUnlockType
import me.magnum.melonds.impl.retroachievements.offline.HardcoreOfflineLossTracker
import me.magnum.melonds.ui.emulator.component.HardcoreSubmissionQueue
import me.magnum.melonds.impl.retroachievements.offline.RetroAchievementsImageCacheWarmer
import me.magnum.melonds.impl.retroachievements.offline.SmartSyncSkipReason
import me.magnum.melonds.impl.retroachievements.offline.SmartSyncEngine
import me.magnum.melonds.impl.layout.UILayoutProvider
import me.magnum.melonds.impl.system.NetworkStatusProvider
import me.magnum.melonds.ui.emulator.component.RetroAchievementsSubmissionHandler
import me.magnum.melonds.ui.emulator.firmware.FirmwarePauseMenuOption
import me.magnum.melonds.ui.emulator.model.RumbleEvent
import me.magnum.melonds.ui.emulator.model.EmulatorState
import me.magnum.melonds.ui.emulator.model.EmulatorUiEvent
import me.magnum.melonds.ui.emulator.model.HardcorePendingExitChoice
import me.magnum.melonds.ui.emulator.model.InGameRomSettingsOverrides
import me.magnum.melonds.ui.emulator.model.InGameRomSettingsMenuState
import me.magnum.melonds.ui.emulator.model.OfflineAchievementsSyncChoice
import me.magnum.melonds.ui.emulator.model.LaunchArgs
import me.magnum.melonds.ui.emulator.model.PauseMenu
import me.magnum.melonds.ui.emulator.model.RAEventUi
import me.magnum.melonds.ui.emulator.model.RAIntegrationEvent
import me.magnum.melonds.ui.emulator.model.RuntimeInputLayoutConfiguration
import me.magnum.melonds.ui.emulator.model.RuntimeRendererConfiguration
import me.magnum.melonds.ui.emulator.model.ToastEvent
import me.magnum.melonds.ui.emulator.model.RetroAchievementsLoadStage
import me.magnum.melonds.ui.emulator.model.VulkanCompileProgress
import me.magnum.melonds.ui.emulator.rewind.model.RewindSaveState
import me.magnum.melonds.ui.emulator.rom.RomPauseMenuOption
import me.magnum.melonds.utils.EventSharedFlow
import me.magnum.rcheevosapi.exception.UserTokenExpiredException
import me.magnum.rcheevosapi.model.RAAchievement
import me.magnum.rcheevosapi.model.RAAchievementSet
import me.magnum.rcheevosapi.model.RASetId
import me.magnum.rcheevosapi.model.RAUserAuth
import java.io.FileInputStream
import java.util.UUID
import javax.inject.Inject
import kotlin.coroutines.CoroutineContext
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.minutes
import kotlin.time.Duration.Companion.seconds

private const val RA_TRACE_TAG = "RATrace"
private const val RA_IDENTITY_TAG = "RAIdentity"
private const val RA_SUBMISSION_TAG = "RASubmission"
private const val AUTO_STATE_TAG = "AutoState"
private const val SAVESTATE_HEADER_SIZE = 12
private const val SAVESTATE_MAJOR = 13
private const val SAVESTATE_MINOR = 0

private const val RETROACHIEVEMENTS_REFRESH_TIMEOUT_MS = 12_000L

@OptIn(ExperimentalCoroutinesApi::class)
@HiltViewModel
class EmulatorViewModel @Inject constructor(
    @param:ApplicationContext private val context: Context,
    private val settingsRepository: SettingsRepository,
    private val romsRepository: RomsRepository,
    private val cheatsRepository: CheatsRepository,
    private val retroAchievementsRepository: RetroAchievementsRepository,
    private val retroAchievementsDao: RetroAchievementsDao,
    private val offlineLedgerRepository: OfflineLedgerRepository,
    private val offlinePrefetchCacheRepository: OfflinePrefetchCacheRepository,
    private val retroAchievementsImageCacheWarmer: RetroAchievementsImageCacheWarmer,
    private val smartSyncEngine: SmartSyncEngine,
    private val hardcoreOfflineLossTracker: HardcoreOfflineLossTracker,
    private val networkStatusProvider: NetworkStatusProvider,
    private val romFileProcessorFactory: RomFileProcessorFactory,
    private val layoutsRepository: LayoutsRepository,
    private val backgroundsRepository: BackgroundRepository,
    private val saveStatesRepository: SaveStatesRepository,
    private val screenshotFrameBufferProvider: ScreenshotFrameBufferProvider,
    private val uiLayoutProvider: UILayoutProvider,
    private val emulatorManager: EmulatorManager,
    private val emulatorSession: EmulatorSession,
    private val retroAchievementsSubmissionHandler: RetroAchievementsSubmissionHandler,
    savedStateHandle: SavedStateHandle,
) : ViewModel() {

    private val sessionCoroutineScope = EmulatorSessionCoroutineScope()
    private var raSessionJob: Job? = null

    private enum class RetroAchievementsNetworkMode {
        ONLINE_LIVE,
        OFFLINE_ACCUMULATING,
    }

    private enum class RetroAchievementsSessionMode {
        SOFTCORE,
        HARDCORE,
    }

    private enum class RetroAchievementsRuntimePath(val traceValue: String) {
        DISABLED("disabled"),
        RC_CLIENT("rc_client"),
        RC_CLIENT_OFFLINE("rc_client_offline"),
    }

    private data class RetroAchievementsLaunchDecision(
        val networkMode: RetroAchievementsNetworkMode,
        val sessionMode: RetroAchievementsSessionMode,
        val initialOfflineType: OfflineUnlockType?,
        val isHardcoreEligibleAfterOnlineStart: Boolean,
        val offlineDueToNoInternetAtStart: Boolean,
        val hardcoreOfflineDisabled: Boolean,
    )

    private data class OfflineRetroAchievementsSession(
        val userId: String,
        val contentId: String,
        val gameId: Long,
        val unlockMode: OfflineUnlockMode,
        val offlineType: OfflineUnlockType,
        val sessionId: String,
        val startedAtEpochMs: Long,
        var nextOrderIndex: Long,
    )

    private enum class OnlineRetroAchievementsBootstrapSource {
        CACHE,
        NETWORK,
    }

    private data class OnlineRetroAchievementsBootstrap(
        val achievementData: GameAchievementData,
        val source: OnlineRetroAchievementsBootstrapSource,
    )

    private var retroAchievementsNetworkMode: RetroAchievementsNetworkMode = RetroAchievementsNetworkMode.ONLINE_LIVE
    private var retroAchievementsSessionMode: RetroAchievementsSessionMode = RetroAchievementsSessionMode.SOFTCORE
    private var isHardcoreEligibleAfterOnlineStart = false
    private var startedSessionOnlineLive = false
    private var isRetroAchievementsOnlineSessionStarted = false
    private var currentRetroAchievementsGameId: Long? = null
    private var offlineRetroAchievementsSession: OfflineRetroAchievementsSession? = null
    private var activeRuntimeBridgeConfig: RARuntimeBridgeConfig? = null
    private var activeRuntimePath: RetroAchievementsRuntimePath = RetroAchievementsRuntimePath.DISABLED
    private var didReceiveRendererInitFailure = false
    private val announcedMasteryKeys = mutableSetOf<Pair<Long, Boolean>>()
    private val pendingRuntimeAchievementTriggers = mutableMapOf<Long, Long>()
    private val pendingRuntimeLeaderboardCompletions = mutableMapOf<Long, Long>()

    private var offlineSyncChoiceDeferred: CompletableDeferred<OfflineAchievementsSyncChoice>? = null
    private var hardcoreExitChoiceDeferred: CompletableDeferred<HardcorePendingExitChoice>? = null

    private val hardcoreSubmissionQueue = HardcoreSubmissionQueue(retroAchievementsRepository)

    private val _emulatorState = MutableStateFlow<EmulatorState>(EmulatorState.Uninitialized)
    val emulatorState = _emulatorState.asStateFlow()

    private val _layout = MutableStateFlow<LayoutConfiguration?>(null)

    private val _currentLayout = uiLayoutProvider.currentLayout.shareIn(viewModelScope, SharingStarted.Lazily)

    private val _runtimeLayout = MutableStateFlow<RuntimeInputLayoutConfiguration?>(null)
    val runtimeLayout = _runtimeLayout.asStateFlow()

    private val activeRomConfig = MutableStateFlow<Rom?>(null)

    val controllerConfiguration = combine(
        settingsRepository.observeControllerConfiguration(),
        activeRomConfig,
    ) { globalConfiguration, rom ->
        rom?.config?.getEffectiveControllerConfiguration(globalConfiguration) ?: globalConfiguration
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.Eagerly,
        initialValue = settingsRepository.getControllerConfiguration(),
    )

    private val _runtimeRendererConfiguration = MutableStateFlow<RuntimeRendererConfiguration?>(null)
    val runtimeRendererConfiguration = _runtimeRendererConfiguration.asStateFlow()

    private val _mainScreenBackground = MutableStateFlow(RuntimeBackground.None)
    val mainScreenBackground = _mainScreenBackground.asStateFlow()

    private val _secondaryScreenBackground = MutableStateFlow(RuntimeBackground.None)
    val secondaryScreenBackground = _secondaryScreenBackground.asStateFlow()

    private val _rumbleEvent = MutableSharedFlow<RumbleEvent>(extraBufferCapacity = 100, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    val rumbleEvent = _rumbleEvent.asSharedFlow()

    private val _achievementsEvent = MutableSharedFlow<RAEventUi>(extraBufferCapacity = 100, onBufferOverflow = BufferOverflow.SUSPEND)
    val achievementsEvent = _achievementsEvent.asSharedFlow()

    private val _currentFps = MutableStateFlow<Int?>(null)
    val currentFps = _currentFps.asStateFlow()

    private val _toastEvent = EventSharedFlow<ToastEvent>()
    val toastEvent = _toastEvent.asSharedFlow()

    private val retroAchievementsVersionName: String by lazy {
        runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        }.getOrNull().orEmpty().ifBlank { "unknown" }
    }

    private val retroAchievementsUserAgent: String by lazy {
        "melonDualDS-android/$retroAchievementsVersionName"
    }

    private val _raIntegrationEvent = EventSharedFlow<RAIntegrationEvent>()
    val integrationEvent = _raIntegrationEvent.asSharedFlow()

    val pendingSubmissionsSummary = retroAchievementsSubmissionHandler.getPendingSubmissionsSummaryFlow()

    private val _uiEvent = EventSharedFlow<EmulatorUiEvent>()
    val uiEvent = _uiEvent.asSharedFlow()

    private val _externalDisplayKeepAspectRatioEnabled = MutableStateFlow(settingsRepository.isExternalDisplayKeepAspectRationEnabled())
    val externalDisplayKeepAspectRatioEnabled = _externalDisplayKeepAspectRatioEnabled.asStateFlow()

    private val _dualScreenPreset = MutableStateFlow(settingsRepository.getDualScreenPreset())
    val dualScreenPreset = _dualScreenPreset.asStateFlow()

    private val _dualScreenIntegerScaleEnabled = MutableStateFlow(settingsRepository.isDualScreenIntegerScaleEnabled())
    val dualScreenIntegerScaleEnabled = _dualScreenIntegerScaleEnabled.asStateFlow()

    private val _dualScreenInternalFillHeightEnabled = MutableStateFlow(settingsRepository.isDualScreenInternalFillHeightEnabled())
    val dualScreenInternalFillHeightEnabled = _dualScreenInternalFillHeightEnabled.asStateFlow()

    private val _dualScreenInternalFillWidthEnabled = MutableStateFlow(settingsRepository.isDualScreenInternalFillWidthEnabled())
    val dualScreenInternalFillWidthEnabled = _dualScreenInternalFillWidthEnabled.asStateFlow()

    private val _dualScreenExternalFillHeightEnabled = MutableStateFlow(settingsRepository.isDualScreenExternalFillHeightEnabled())
    val dualScreenExternalFillHeightEnabled = _dualScreenExternalFillHeightEnabled.asStateFlow()

    private val _dualScreenExternalFillWidthEnabled = MutableStateFlow(settingsRepository.isDualScreenExternalFillWidthEnabled())
    val dualScreenExternalFillWidthEnabled = _dualScreenExternalFillWidthEnabled.asStateFlow()

    private val _dualScreenInternalVerticalAlignmentOverride = MutableStateFlow(settingsRepository.getDualScreenInternalVerticalAlignmentOverride())
    val dualScreenInternalVerticalAlignmentOverride = _dualScreenInternalVerticalAlignmentOverride.asStateFlow()

    private val _dualScreenExternalVerticalAlignmentOverride = MutableStateFlow(settingsRepository.getDualScreenExternalVerticalAlignmentOverride())
    val dualScreenExternalVerticalAlignmentOverride = _dualScreenExternalVerticalAlignmentOverride.asStateFlow()

    private var currentRom: Rom? = null
    init {
        viewModelScope.launch {
            _layout.filterNotNull().collect {
                uiLayoutProvider.setCurrentLayoutConfiguration(it)
            }
        }

        viewModelScope.launch {
            settingsRepository.observeExternalDisplayKeepAspectRationEnabled().collectLatest {
                _externalDisplayKeepAspectRatioEnabled.value = it
            }
        }
        viewModelScope.launch {
            settingsRepository.observeDualScreenPreset().collectLatest {
                _dualScreenPreset.value = it
            }
        }
        viewModelScope.launch {
            settingsRepository.observeDualScreenIntegerScaleEnabled().collectLatest {
                _dualScreenIntegerScaleEnabled.value = it
            }
        }
        viewModelScope.launch {
            settingsRepository.observeDualScreenInternalFillHeightEnabled().collectLatest {
                _dualScreenInternalFillHeightEnabled.value = it
            }
        }
        viewModelScope.launch {
            settingsRepository.observeDualScreenInternalFillWidthEnabled().collectLatest {
                _dualScreenInternalFillWidthEnabled.value = it
            }
        }
        viewModelScope.launch {
            settingsRepository.observeDualScreenExternalFillHeightEnabled().collectLatest {
                _dualScreenExternalFillHeightEnabled.value = it
            }
        }
        viewModelScope.launch {
            settingsRepository.observeDualScreenExternalFillWidthEnabled().collectLatest {
                _dualScreenExternalFillWidthEnabled.value = it
            }
        }
        viewModelScope.launch {
            settingsRepository.observeDualScreenInternalVerticalAlignmentOverride().collectLatest {
                _dualScreenInternalVerticalAlignmentOverride.value = it
            }
        }
        viewModelScope.launch {
            settingsRepository.observeDualScreenExternalVerticalAlignmentOverride().collectLatest {
                _dualScreenExternalVerticalAlignmentOverride.value = it
            }
        }

        val launchArgs = LaunchArgs.fromSavedStateHandle(savedStateHandle)
        if (launchArgs != null) {
            launchEmulator(launchArgs)
        } else {
            _uiEvent.tryEmit(EmulatorUiEvent.CloseEmulator)
        }
    }

    fun relaunchWithNewArgs(args: LaunchArgs) {
        if (!_emulatorState.value.isRunning()) {
            launchEmulator(args)
            return
        }

        sessionCoroutineScope.launch {
            val runningRom = _emulatorState.value as? EmulatorState.RunningRom
            if (runningRom != null) {
                val userAuth = retroAchievementsRepository.getUserAuthentication()
                if (userAuth != null) {
                    val userId = userAuth.username
                    val contentId = runningRom.rom.retroAchievementsHash
                    val ledgerStatus = withContext(Dispatchers.IO) {
                        offlineLedgerRepository.getStatus(userId, contentId)
                    }

                    if (hardcoreSubmissionQueue.pendingCount() > 0) {
                        val shouldExit = handleHardcorePendingBeforeExit(userId, contentId)
                        if (!shouldExit) {
                            emulatorManager.resumeEmulator()
                            return@launch
                        }
                    } else if (ledgerStatus.integrity == OfflineLedgerIntegrity.OK) {
                        hardcoreOfflineLossTracker.clearPendingUnlocks(userId, contentId)
                        if (ledgerStatus.pendingSoftcoreUnlockCount > 0) {
                            _toastEvent.tryEmit(
                                ToastEvent.OfflineSoftcorePendingNotice(
                                    pendingSoftcoreCount = ledgerStatus.pendingSoftcoreUnlockCount,
                                    ledgerExpiresInMs = ledgerStatus.ledgerExpiresInMs,
                                )
                            )
                        }
                    }
                }
            }

            runningRom?.let {
                maybeAutoSaveStateOnExit(it.rom)
            }
            stopEmulator()
            launchEmulator(args)
        }
    }

    fun onRomLaunchValidated(rom: Rom) {
        sessionCoroutineScope.launch {
            launchRom(refreshRomForLaunch(rom))
        }
    }

    fun onFirmwareLaunchValidated(consoleType: ConsoleType) {
        viewModelScope.launch {
            launchFirmware(consoleType)
        }
    }

    private fun launchEmulator(args: LaunchArgs) {
        when (args) {
            is LaunchArgs.RomObject -> loadRom(args.rom)
            is LaunchArgs.RomUri -> loadRom(args.uri)
            is LaunchArgs.RomPath -> loadRom(args.path)
            is LaunchArgs.Firmware -> _emulatorState.value = EmulatorState.ValidatingFirmware(args.consoleType)
        }
    }

    private fun loadRom(rom: Rom) {
        viewModelScope.launch {
            resetEmulatorState(EmulatorState.LoadingRom())
            sessionCoroutineScope.launch {
                _emulatorState.value = EmulatorState.ValidatingRom(refreshRomForLaunch(rom))
            }
        }
    }

    private suspend fun refreshRomForLaunch(rom: Rom): Rom {
        return romsRepository.getRomAtUri(rom.uri) ?: rom
    }

    private fun loadRom(romUri: Uri) {
        viewModelScope.launch {
            resetEmulatorState(EmulatorState.LoadingRom())
            sessionCoroutineScope.launch {
                val rom = romsRepository.getRomAtUri(romUri)
                if (rom != null) {
                    _emulatorState.value = EmulatorState.ValidatingRom(rom)
                } else {
                    _emulatorState.value = EmulatorState.RomNotFoundError(romUri.toString())
                }
            }
        }
    }

    private fun loadRom(romPath: String) {
        viewModelScope.launch {
            resetEmulatorState(EmulatorState.LoadingRom())
            sessionCoroutineScope.launch {
                val rom = romsRepository.getRomAtPath(romPath)
                if (rom != null) {
                    _emulatorState.value = EmulatorState.ValidatingRom(rom)
                } else {
                    _emulatorState.value = EmulatorState.RomNotFoundError(romPath)
                }
            }
        }
    }

    private suspend fun launchRom(rom: Rom) = coroutineScope {
        try {
            _emulatorState.value = EmulatorState.LoadingRom()
            currentRom = rom
            activeRomConfig.value = rom
            val isRetroAchievementsEnabledForLaunch = isRetroAchievementsEnabledForLaunch(rom)
            val launchDecision = if (isRetroAchievementsEnabledForLaunch) {
                runCatching {
                    decideRetroAchievementsLaunchDecision(rom)
                }.getOrElse { throwable ->
                    Log.e("EmulatorViewModel", "RetroAchievements launch decision failed for '${rom.name}'", throwable)
                    RetroAchievementsLaunchDecision(
                        networkMode = RetroAchievementsNetworkMode.ONLINE_LIVE,
                        sessionMode = RetroAchievementsSessionMode.SOFTCORE,
                        initialOfflineType = null,
                        isHardcoreEligibleAfterOnlineStart = false,
                        offlineDueToNoInternetAtStart = false,
                        hardcoreOfflineDisabled = false,
                    )
                }
            } else {
                RetroAchievementsLaunchDecision(
                    networkMode = RetroAchievementsNetworkMode.ONLINE_LIVE,
                    sessionMode = RetroAchievementsSessionMode.SOFTCORE,
                    initialOfflineType = null,
                    isHardcoreEligibleAfterOnlineStart = false,
                    offlineDueToNoInternetAtStart = false,
                    hardcoreOfflineDisabled = false,
                )
            }

            retroAchievementsNetworkMode = launchDecision.networkMode
            retroAchievementsSessionMode = launchDecision.sessionMode
            isHardcoreEligibleAfterOnlineStart = launchDecision.isHardcoreEligibleAfterOnlineStart
            startedSessionOnlineLive = launchDecision.networkMode == RetroAchievementsNetworkMode.ONLINE_LIVE

            startEmulatorSession(
                sessionType = EmulatorSession.SessionType.RomSession(rom),
                areRetroAchievementsEnabled = isRetroAchievementsEnabledForLaunch,
                isRetroAchievementsHardcoreModeEnabled = launchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE,
            )
            startObservingMainScreenBackground()
            startObservingSecondaryScreenBackground()
            startObservingRuntimeInputLayoutConfiguration()
            startObservingRendererConfiguration()
            startObservingEmulatorEvents()
            startObservingAchievementEvents()
            startObservingLayoutForRom()
            if (isRetroAchievementsEnabledForLaunch) {
                startRetroAchievementsSession(rom, launchDecision).await()
            } else {
                activeRuntimeBridgeConfig = null
                activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                emulatorSession.updateRetroAchievementsOfflineModeEnabled(false)
                emulatorSession.updateRetroAchievementsIntegrationStatus(
                    GameAchievementData.IntegrationStatus.DISABLED_BY_SETTING,
                )
            }

            val cheats = getRomInfo(rom)?.let { getRomEnabledCheats(it) } ?: emptyList()
            val result = emulatorManager.loadRom(rom, cheats)
            when (result) {
                is RomLaunchResult.LaunchFailedRomNotFound,
                is RomLaunchResult.LaunchFailedRomNotSupported,
                is RomLaunchResult.LaunchFailedSramProblem,
                is RomLaunchResult.LaunchFailed -> {
                    _emulatorState.value = EmulatorState.RomLoadError
                }
                is RomLaunchResult.LaunchSuccessful -> {
                    if (!result.isGbaLoadSuccessful) {
                        _toastEvent.tryEmit(ToastEvent.GbaLoadFailed)
                    }
                    _emulatorState.value = EmulatorState.RunningRom(rom)
                    maybeAutoLoadStateOnLaunch(rom)
                    DebugCommandStateStore.onRunningRomReady(rom.uri, rom.name)
                    startTrackingFps()
                    startTrackingPlayTime(rom)
                }
            }
        } catch (exception: Throwable) {
            if (exception is CancellationException) {
                throw exception
            }
            Log.e("EmulatorViewModel", "Failed to launch ROM '${rom.name}'", exception)
            _emulatorState.value = EmulatorState.RomLoadError
        }
    }

    private suspend fun isRetroAchievementsEnabledForLaunch(rom: Rom): Boolean {
        return retroAchievementsRepository.isUserAuthenticated() &&
            settingsRepository.isRetroAchievementsEnabled() &&
            (rom.config.retroAchievementsEnabled ?: true)
    }

    private suspend fun isRetroAchievementsEnabledForSettingsUpdate(currentState: EmulatorState): Boolean {
        return when (currentState) {
            is EmulatorState.RunningRom -> isRetroAchievementsEnabledForLaunch(currentState.rom)
            else -> retroAchievementsRepository.isUserAuthenticated() && settingsRepository.isRetroAchievementsEnabled()
        }
    }

    fun submitOfflineAchievementsSyncChoice(choice: OfflineAchievementsSyncChoice) {
        offlineSyncChoiceDeferred?.complete(choice)
    }

    fun submitHardcorePendingExitChoice(choice: HardcorePendingExitChoice) {
        hardcoreExitChoiceDeferred?.complete(choice)
    }

    private suspend fun decideRetroAchievementsLaunchDecision(rom: Rom): RetroAchievementsLaunchDecision {
        val startedOnline = networkStatusProvider.isOnline()
        val hardcoreSettingEnabled = settingsRepository.isRetroAchievementsHardcoreEnabled()
        val offlineSoftcoreEnabled = settingsRepository.isRetroAchievementsOfflineSoftcoreEnabled()
        val userAuth = retroAchievementsRepository.getUserAuthentication()

        if (userAuth == null) {
            return RetroAchievementsLaunchDecision(
                networkMode = RetroAchievementsNetworkMode.ONLINE_LIVE,
                sessionMode = RetroAchievementsSessionMode.SOFTCORE,
                initialOfflineType = null,
                isHardcoreEligibleAfterOnlineStart = false,
                offlineDueToNoInternetAtStart = false,
                hardcoreOfflineDisabled = false,
            )
        }

        val userId = userAuth.username
        val contentId = rom.retroAchievementsHash

        var ledgerStatus = withContext(Dispatchers.IO) {
            offlineLedgerRepository.getStatus(userId, contentId)
        }
        var ignoreLedgerForThisLaunch = false

        if (ledgerStatus.integrity != OfflineLedgerIntegrity.OK && ledgerStatus.integrity != OfflineLedgerIntegrity.EMPTY) {
            _toastEvent.tryEmit(ToastEvent.OfflineAchievementsLedgerTampered)

            val resetResult = withContext(Dispatchers.IO) {
                offlineLedgerRepository.resetLedger(userId, contentId)
            }
            if (resetResult.isSuccess) {
                hardcoreOfflineLossTracker.clearPendingUnlocks(userId, contentId)
                ledgerStatus = withContext(Dispatchers.IO) {
                    offlineLedgerRepository.getStatus(userId, contentId)
                }
            } else {
                ignoreLedgerForThisLaunch = true
            }
        }

        if (ledgerStatus.integrity == OfflineLedgerIntegrity.OK && ledgerStatus.hasPendingHardcoreUnlocks) {
            withContext(Dispatchers.IO) {
                offlineLedgerRepository.discardPendingHardcoreUnlocks(userId, contentId)
            }
            hardcoreOfflineLossTracker.clearPendingUnlocks(userId, contentId)
            ledgerStatus = withContext(Dispatchers.IO) {
                offlineLedgerRepository.getStatus(userId, contentId)
            }
            logRaTrace(
                "hardcore_ledger_legacy_discarded",
                "content_id" to contentId,
            )
        }

        if (!startedOnline && offlineSoftcoreEnabled) {
            if (ledgerStatus.integrity == OfflineLedgerIntegrity.OK && ledgerStatus.pendingSoftcoreUnlockCount > 0) {
                _toastEvent.tryEmit(
                    ToastEvent.OfflineSoftcorePendingNotice(
                        pendingSoftcoreCount = ledgerStatus.pendingSoftcoreUnlockCount,
                        ledgerExpiresInMs = ledgerStatus.ledgerExpiresInMs,
                    )
                )
            }
            return RetroAchievementsLaunchDecision(
                networkMode = RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING,
                sessionMode = RetroAchievementsSessionMode.SOFTCORE,
                initialOfflineType = OfflineUnlockType.OFFLINE_FROM_START,
                isHardcoreEligibleAfterOnlineStart = false,
                offlineDueToNoInternetAtStart = true,
                hardcoreOfflineDisabled = hardcoreSettingEnabled,
            )
        }

        if (ledgerStatus.isExpired) {
            _toastEvent.tryEmit(
                ToastEvent.OfflineSoftcorePendingNotice(
                    pendingSoftcoreCount = ledgerStatus.pendingSoftcoreUnlockCount,
                    ledgerExpiresInMs = ledgerStatus.ledgerExpiresInMs,
                )
            )
            return RetroAchievementsLaunchDecision(
                networkMode = RetroAchievementsNetworkMode.ONLINE_LIVE,
                sessionMode = if (hardcoreSettingEnabled) RetroAchievementsSessionMode.HARDCORE else RetroAchievementsSessionMode.SOFTCORE,
                initialOfflineType = null,
                isHardcoreEligibleAfterOnlineStart = hardcoreSettingEnabled,
                offlineDueToNoInternetAtStart = false,
                hardcoreOfflineDisabled = false,
            )
        }

        if (!offlineSoftcoreEnabled || ignoreLedgerForThisLaunch || ledgerStatus.integrity != OfflineLedgerIntegrity.OK || ledgerStatus.pendingSoftcoreUnlockCount <= 0) {
            return RetroAchievementsLaunchDecision(
                networkMode = RetroAchievementsNetworkMode.ONLINE_LIVE,
                sessionMode = if (hardcoreSettingEnabled) RetroAchievementsSessionMode.HARDCORE else RetroAchievementsSessionMode.SOFTCORE,
                initialOfflineType = null,
                isHardcoreEligibleAfterOnlineStart = hardcoreSettingEnabled,
                offlineDueToNoInternetAtStart = false,
                hardcoreOfflineDisabled = false,
            )
        }

        val choice = awaitOfflineAchievementsSyncChoice(
            pendingUnlockCount = ledgerStatus.pendingSoftcoreUnlockCount,
            ledgerExpiresInMs = ledgerStatus.ledgerExpiresInMs,
        )
        return when (choice) {
            OfflineAchievementsSyncChoice.CONTINUE_OFFLINE -> RetroAchievementsLaunchDecision(
                networkMode = RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING,
                sessionMode = RetroAchievementsSessionMode.SOFTCORE,
                initialOfflineType = OfflineUnlockType.OFFLINE_FROM_START,
                isHardcoreEligibleAfterOnlineStart = false,
                offlineDueToNoInternetAtStart = false,
                hardcoreOfflineDisabled = hardcoreSettingEnabled,
            )
            OfflineAchievementsSyncChoice.SYNC_NOW -> {
                if (syncPendingOfflineAchievements(userId, contentId, ledgerStatus.pendingSoftcoreUnlockCount)) {
                    RetroAchievementsLaunchDecision(
                        networkMode = RetroAchievementsNetworkMode.ONLINE_LIVE,
                        sessionMode = if (hardcoreSettingEnabled) RetroAchievementsSessionMode.HARDCORE else RetroAchievementsSessionMode.SOFTCORE,
                        initialOfflineType = null,
                        isHardcoreEligibleAfterOnlineStart = hardcoreSettingEnabled,
                        offlineDueToNoInternetAtStart = false,
                        hardcoreOfflineDisabled = false,
                    )
                } else {
                    RetroAchievementsLaunchDecision(
                        networkMode = RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING,
                        sessionMode = RetroAchievementsSessionMode.SOFTCORE,
                        initialOfflineType = OfflineUnlockType.OFFLINE_FROM_START,
                        isHardcoreEligibleAfterOnlineStart = false,
                        offlineDueToNoInternetAtStart = false,
                        hardcoreOfflineDisabled = hardcoreSettingEnabled,
                    )
                }
            }
        }
    }

    private suspend fun syncPendingOfflineAchievements(
        userId: String,
        contentId: String,
        pendingUnlockCount: Int,
    ): Boolean {
        if (!networkStatusProvider.isLikelyOnline()) {
            return false
        }
        _uiEvent.emit(EmulatorUiEvent.ShowOfflineAchievementsSyncProgress(pendingUnlockCount))
        val syncResult = smartSyncEngine.syncSoftcoreNow(userId, contentId)
        _uiEvent.emit(EmulatorUiEvent.HideOfflineAchievementsSyncProgress)
        if (syncResult.isSuccess) {
            val skipped = syncResult.getOrNull()?.skipped.orEmpty()
            emitOfflineAchievementsNotSyncedToasts(skipped)
            return true
        }
        val error = syncResult.exceptionOrNull()
        if (error is OfflineLedgerExpiredException) {
            _toastEvent.tryEmit(
                ToastEvent.OfflineSoftcorePendingNotice(
                    pendingSoftcoreCount = pendingUnlockCount,
                    ledgerExpiresInMs = 0L,
                )
            )
        }
        logRaTrace(
            "offline_sync_now_failed",
            "pending" to pendingUnlockCount,
            "content_id" to contentId,
            "error" to (error?.message ?: error?.javaClass?.simpleName ?: "unknown"),
        )
        return false
    }

    private suspend fun awaitOfflineAchievementsSyncChoice(
        pendingUnlockCount: Int,
        ledgerExpiresInMs: Long?,
    ): OfflineAchievementsSyncChoice {
        offlineSyncChoiceDeferred?.cancel()
        val deferred = CompletableDeferred<OfflineAchievementsSyncChoice>()
        offlineSyncChoiceDeferred = deferred
        _uiEvent.emit(
            EmulatorUiEvent.ShowOfflineAchievementsSyncChoice(
                pendingUnlockCount = pendingUnlockCount,
                ledgerExpiresInMs = ledgerExpiresInMs,
            )
        )
        return deferred.await().also {
            if (offlineSyncChoiceDeferred === deferred) {
                offlineSyncChoiceDeferred = null
            }
        }
    }

    private suspend fun awaitHardcorePendingExitChoice(
        pendingHardcoreCount: Int,
    ): HardcorePendingExitChoice {
        hardcoreExitChoiceDeferred?.cancel()
        val deferred = CompletableDeferred<HardcorePendingExitChoice>()
        hardcoreExitChoiceDeferred = deferred
        _uiEvent.emit(EmulatorUiEvent.ShowHardcorePendingExitWarning(pendingHardcoreCount))
        return deferred.await().also {
            if (hardcoreExitChoiceDeferred === deferred) {
                hardcoreExitChoiceDeferred = null
            }
        }
    }

    private suspend fun emitOfflineAchievementsNotSyncedToasts(skipped: List<me.magnum.melonds.impl.retroachievements.offline.SmartSyncSkippedAchievement>) {
        if (skipped.isEmpty()) return

        val maxIndividualToasts = 3
        val individual = skipped.take(maxIndividualToasts)

        individual.forEach { skip ->
            val title = retroAchievementsRepository.getAchievement(skip.achievementId).getOrNull()?.getCleanTitle()
                ?: "#${skip.achievementId}"

            val reason = when (skip.reason) {
                SmartSyncSkipReason.MISSING_FROM_CURRENT_SET -> ToastEvent.OfflineAchievementNotSyncedReason.MISSING_FROM_CURRENT_SET
                SmartSyncSkipReason.DEFINITION_CHANGED -> ToastEvent.OfflineAchievementNotSyncedReason.DEFINITION_CHANGED
                SmartSyncSkipReason.NOT_IN_PREFETCH_CACHE -> ToastEvent.OfflineAchievementNotSyncedReason.NOT_IN_PREFETCH_CACHE
                SmartSyncSkipReason.SERVER_REJECTED -> ToastEvent.OfflineAchievementNotSyncedReason.SERVER_REJECTED
            }

            _toastEvent.tryEmit(
                ToastEvent.OfflineAchievementNotSynced(
                    title = title,
                    reason = reason,
                    reasonDetail = skip.reasonDetail,
                )
            )
        }

        val remaining = skipped.size - individual.size
        if (remaining > 0) {
            _toastEvent.tryEmit(ToastEvent.OfflineAchievementsNotSyncedSummary(skippedCount = remaining))
        }
    }

    private fun launchFirmware(consoleType: ConsoleType) {
        viewModelScope.launch {
            resetEmulatorState(EmulatorState.LoadingFirmware())
            startEmulatorSession(
                sessionType = EmulatorSession.SessionType.FirmwareSession(consoleType),
                areRetroAchievementsEnabled = false,
            )
            sessionCoroutineScope.launch {
                startObservingMainScreenBackground()
                startObservingSecondaryScreenBackground()
                startObservingRuntimeInputLayoutConfiguration()
                startObservingRendererConfiguration()
                startObservingLayoutForFirmware()
                startObservingEmulatorEvents()

                val result = emulatorManager.loadFirmware(consoleType)
                when (result) {
                    is FirmwareLaunchResult.LaunchFailed -> {
                        _emulatorState.value = EmulatorState.FirmwareLoadError(result.reason)
                    }
                    FirmwareLaunchResult.LaunchSuccessful -> {
                        _emulatorState.value = EmulatorState.RunningFirmware(consoleType)
                        startTrackingFps()
                    }
                }
            }
        }
    }

    fun setSystemOrientation(orientation: Orientation) {
        uiLayoutProvider.updateCurrentOrientation(orientation)
    }

    fun setUiSize(width: Int, height: Int) {
        uiLayoutProvider.updateUiSize(width, height)
    }

    fun setUiInsets(insets: Insets) {
        uiLayoutProvider.updateUiInsets(insets)
    }

    fun shouldIgnoreDisplayCutoutInLayouts(): Boolean {
        return settingsRepository.shouldIgnoreDisplayCutoutInLayouts()
    }

    fun setScreenFolds(folds: List<ScreenFold>) {
        uiLayoutProvider.updateFolds(folds)
    }

    fun setConnectedDisplays(displays: LayoutDisplayPair) {
        uiLayoutProvider.updateDisplays(displays)
    }

    fun setExternalDisplayKeepAspectRatioEnabled(enabled: Boolean) {
        _externalDisplayKeepAspectRatioEnabled.value = enabled
        settingsRepository.setExternalDisplayKeepAspectRatioEnabled(enabled)
    }

    fun setDualScreenPreset(preset: DualScreenPreset) {
        _dualScreenPreset.value = preset
        settingsRepository.setDualScreenPreset(preset)
    }

    fun setDualScreenIntegerScaleEnabled(enabled: Boolean) {
        _dualScreenIntegerScaleEnabled.value = enabled
        settingsRepository.setDualScreenIntegerScaleEnabled(enabled)
    }

    fun setDualScreenInternalFillHeightEnabled(enabled: Boolean) {
        _dualScreenInternalFillHeightEnabled.value = enabled
        settingsRepository.setDualScreenInternalFillHeightEnabled(enabled)
    }

    fun setDualScreenInternalFillWidthEnabled(enabled: Boolean) {
        _dualScreenInternalFillWidthEnabled.value = enabled
        settingsRepository.setDualScreenInternalFillWidthEnabled(enabled)
    }

    fun setDualScreenExternalFillHeightEnabled(enabled: Boolean) {
        _dualScreenExternalFillHeightEnabled.value = enabled
        settingsRepository.setDualScreenExternalFillHeightEnabled(enabled)
    }

    fun setDualScreenExternalFillWidthEnabled(enabled: Boolean) {
        _dualScreenExternalFillWidthEnabled.value = enabled
        settingsRepository.setDualScreenExternalFillWidthEnabled(enabled)
    }

    fun setDualScreenInternalVerticalAlignmentOverride(alignment: ScreenAlignment?) {
        _dualScreenInternalVerticalAlignmentOverride.value = alignment
        settingsRepository.setDualScreenInternalVerticalAlignmentOverride(alignment)
    }

    fun setDualScreenExternalVerticalAlignmentOverride(alignment: ScreenAlignment?) {
        _dualScreenExternalVerticalAlignmentOverride.value = alignment
        settingsRepository.setDualScreenExternalVerticalAlignmentOverride(alignment)
    }

    fun onAppMovedToBackground() {
        sessionCoroutineScope.launch {
            val runningRom = _emulatorState.value as? EmulatorState.RunningRom ?: return@launch
            val userAuth = retroAchievementsRepository.getUserAuthentication() ?: return@launch
            val ledgerStatus = withContext(Dispatchers.IO) {
                offlineLedgerRepository.getStatus(userAuth.username, runningRom.rom.retroAchievementsHash)
            }
            if (ledgerStatus.integrity == OfflineLedgerIntegrity.OK && ledgerStatus.hasPendingHardcoreUnlocks) {
                _toastEvent.tryEmit(ToastEvent.HardcoreOfflineUnsyncedWarning(ledgerStatus.pendingHardcoreUnlockCount))
            }
        }
    }

    fun onSettingsChanged() {
        val currentState = _emulatorState.value
        sessionCoroutineScope.launch {
            if (settingsRepository.getCurrentVideoRenderer() == VideoRenderer.VULKAN) {
                val pipelineProfile = VulkanPipelineProfile.fromFastPathPreference(
                    settingsRepository.isVulkanFastPathEnabled().first()
                )
                val canUseVulkan = MelonDSAndroidInterface.isVulkanRendererSupported() &&
                    MelonDSAndroidInterface.canInitializeVulkanRenderer(pipelineProfile)
                if (!canUseVulkan) {
                    val activeRenderer = getRuntimeRendererOrNull() ?: VideoRenderer.SOFTWARE
                    settingsRepository.setCurrentVideoRenderer(activeRenderer)
                    _toastEvent.tryEmit(ToastEvent.RendererInitFailed(VideoRenderer.VULKAN))
                    return@launch
                }
            }

            val sessionUpdateActions = emulatorSession.updateRetroAchievementsSettings(
                isRetroAchievementsEnabledForSettingsUpdate(currentState),
                settingsRepository.isRetroAchievementsHardcoreEnabled(),
            )

            when (currentState) {
                is EmulatorState.RunningRom -> emulatorManager.updateRomEmulatorConfiguration(currentState.rom)
                is EmulatorState.RunningFirmware -> emulatorManager.updateFirmwareEmulatorConfiguration(currentState.console)
                else -> {
                    // Do nothing
                }
            }

            dispatchSessionUpdateActions(sessionUpdateActions)
        }
    }

    fun getConfiguredVideoRenderer(): VideoRenderer {
        return settingsRepository.getCurrentVideoRenderer()
    }

    fun onCheatsChanged() {
        val rom = (_emulatorState.value as? EmulatorState.RunningRom)?.rom ?: return

        getRomInfo(rom)?.let {
            sessionCoroutineScope.launch {
                val cheats = getRomEnabledCheats(it)
                emulatorManager.updateCheats(cheats)
            }
        }
    }

    fun onRunningRomVideoFilteringSelected(videoFiltering: VideoFiltering?) {
        updateRunningRomConfig { it.copy(videoFiltering = videoFiltering) }
    }

    fun onRunningRomRetroArchPresetPathSelected(presetPath: String?) {
        updateRunningRomConfig { it.copy(retroArchShaderPresetPath = presetPath) }
    }

    fun onRunningRomRetroArchParametersSelected(parameters: String?) {
        updateRunningRomConfig { it.copy(retroArchShaderParameters = parameters) }
    }

    fun onRunningRomLayoutSelected(layoutId: UUID?) {
        updateRunningRomConfig { it.copy(layoutId = layoutId) }
    }

    fun onRunningRomMicSourceSelected(micSource: RuntimeMicSource) {
        updateRunningRomConfig { it.copy(runtimeMicSource = micSource) }
    }

    fun onRomCustomInputConfigEdited() {
        val runningRom = (_emulatorState.value as? EmulatorState.RunningRom)?.rom ?: return
        sessionCoroutineScope.launch {
            val refreshedRom = romsRepository.getRomAtUri(runningRom.uri) ?: return@launch
            updateRunningRom(refreshedRom)
            emulatorManager.updateRomEmulatorConfiguration(refreshedRom)
        }
    }

    private fun updateRunningRomConfig(update: (me.magnum.melonds.domain.model.rom.config.RomConfig) -> me.magnum.melonds.domain.model.rom.config.RomConfig) {
        val runningRom = (_emulatorState.value as? EmulatorState.RunningRom)?.rom ?: return
        val updatedRom = runningRom.copy(config = update(runningRom.config))
        romsRepository.updateRomConfig(runningRom, updatedRom.config)
        updateRunningRom(updatedRom)
        sessionCoroutineScope.launch {
            emulatorManager.updateRomEmulatorConfiguration(updatedRom)
        }
    }

    private fun updateRunningRom(updatedRom: Rom) {
        currentRom = updatedRom
        activeRomConfig.value = updatedRom
        _emulatorState.update { currentState ->
            when (currentState) {
                is EmulatorState.RunningRom -> currentState.copy(rom = updatedRom)
                else -> currentState
            }
        }
    }

    fun pauseEmulator(showPauseMenu: Boolean) {
        sessionCoroutineScope.launch {
            emulatorManager.pauseEmulator()
            if (showPauseMenu) {
                val rendererDebugToolsEnabled = settingsRepository.isRendererDebugToolsEnabled().firstOrNull() == true
                val pauseOptions = when (_emulatorState.value) {
                    is EmulatorState.RunningRom -> {
                        RomPauseMenuOption.entries.filter {
                            filterRomPauseMenuOption(it, rendererDebugToolsEnabled)
                        }
                    }
                    is EmulatorState.RunningFirmware -> {
                        FirmwarePauseMenuOption.entries
                    }
                    else -> null
                }

                if (pauseOptions != null) {
                    _uiEvent.emit(EmulatorUiEvent.ShowPauseMenu(PauseMenu(pauseOptions)))
                }
            }
        }
    }

    fun resumeEmulator() {
        if (!_emulatorState.value.isRunning()) {
            return
        }

        sessionCoroutineScope.launch {
            emulatorManager.resumeEmulator()
        }
    }

    fun debugStepFrame() {
        sessionCoroutineScope.launch {
            emulatorManager.debugStepFrame()
        }
    }

    fun resetEmulator() {
        if (_emulatorState.value.isRunning()) {
            sessionCoroutineScope.launch {
                emulatorManager.resetEmulator()
                _achievementsEvent.emit(RAEventUi.Reset)
            }
        }
    }

    fun stopEmulator() {
        viewModelScope.launch {
            _achievementsEvent.emit(RAEventUi.Reset)
        }
        finalizeOfflineRetroAchievementsSessionIfNeeded()
        emulatorManager.stopEmulator()
        screenshotFrameBufferProvider.clearBuffer()
    }

    fun exitEmulator(force: Boolean = false) {
        if (!force && retroAchievementsSubmissionHandler.hasPendingSubmissions()) {
            _uiEvent.tryEmit(EmulatorUiEvent.ShowPendingSubmissionsDialog)
            retroAchievementsSubmissionHandler.retrySubmissionsImmediately()
            return
        }

        requestExitRom()
    }

    private fun finalizeOfflineRetroAchievementsSessionIfNeeded() {
        val offlineSession = offlineRetroAchievementsSession ?: return
        offlineRetroAchievementsSession = null

        val endedAtEpochMs = System.currentTimeMillis()
        val estimatedPlayDurationMs = (endedAtEpochMs - offlineSession.startedAtEpochMs).coerceAtLeast(0L)

        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                offlineLedgerRepository.appendSessionEnd(
                    userId = offlineSession.userId,
                    contentId = offlineSession.contentId,
                    gameId = offlineSession.gameId,
                    sessionId = offlineSession.sessionId,
                    endedAtEpochMs = endedAtEpochMs,
                    estimatedPlayDurationMs = estimatedPlayDurationMs,
                    isHardcore = offlineSession.unlockMode == OfflineUnlockMode.HARDCORE,
                    unlockMode = offlineSession.unlockMode,
                    offlineType = offlineSession.offlineType,
                )
            }
        }
    }

    private fun requestExitRom() {
        sessionCoroutineScope.launch {
            val runningRom = _emulatorState.value as? EmulatorState.RunningRom
            if (runningRom == null) {
                stopEmulator()
                _uiEvent.emit(EmulatorUiEvent.CloseEmulator)
                return@launch
            }

            val userAuth = retroAchievementsRepository.getUserAuthentication()
            if (userAuth == null) {
                maybeAutoSaveStateOnExit(runningRom.rom)
                stopEmulator()
                _uiEvent.emit(EmulatorUiEvent.CloseEmulator)
                return@launch
            }

            val userId = userAuth.username
            val contentId = runningRom.rom.retroAchievementsHash
            val ledgerStatus = withContext(Dispatchers.IO) {
                offlineLedgerRepository.getStatus(userId, contentId)
            }

            if (ledgerStatus.integrity == OfflineLedgerIntegrity.OK) {
                if (hardcoreSubmissionQueue.pendingCount() > 0) {
                    val shouldExit = handleHardcorePendingBeforeExit(userId, contentId)
                    if (!shouldExit) {
                        emulatorManager.resumeEmulator()
                        return@launch
                    }
                } else if (ledgerStatus.integrity == OfflineLedgerIntegrity.OK) {
                    hardcoreOfflineLossTracker.clearPendingUnlocks(userId, contentId)
                    if (ledgerStatus.pendingSoftcoreUnlockCount > 0) {
                        _toastEvent.tryEmit(
                            ToastEvent.OfflineSoftcorePendingNotice(
                                pendingSoftcoreCount = ledgerStatus.pendingSoftcoreUnlockCount,
                                ledgerExpiresInMs = ledgerStatus.ledgerExpiresInMs,
                            )
                        )
                    }
                }
            }

            maybeAutoSaveStateOnExit(runningRom.rom)
            stopEmulator()
            _uiEvent.emit(EmulatorUiEvent.CloseEmulator)
        }
    }

    private suspend fun handleHardcorePendingBeforeExit(
        userId: String,
        contentId: String,
    ): Boolean {
        val pending = hardcoreSubmissionQueue.pendingCount()
        if (pending == 0) {
            return true
        }

        val choice = awaitHardcorePendingExitChoice(pending)
        return when (choice) {
            HardcorePendingExitChoice.TRY_SYNC_NOW -> {
                val drainResult = hardcoreSubmissionQueue.drain()
                _toastEvent.tryEmit(
                    ToastEvent.HardcoreQueueSyncResult(
                        submittedCount = drainResult.submittedCount,
                        remainingCount = drainResult.remainingCount,
                    )
                )
                false
            }
            HardcorePendingExitChoice.DISCARD_AND_EXIT -> {
                hardcoreSubmissionQueue.discardAll()
                hardcoreOfflineLossTracker.clearPendingUnlocks(userId, contentId)
                true
            }
        }
    }

    private fun stopEmulatorAndExit() {
        stopEmulator()
        _uiEvent.tryEmit(EmulatorUiEvent.CloseEmulator)
    }

    private fun startTrackingPlayTime(rom: Rom) {
        sessionCoroutineScope.launch {
            var lastTime = System.currentTimeMillis()
            while (isActive) {
                delay(1000)
                val now = System.currentTimeMillis()
                romsRepository.addRomPlayTime(rom, (now - lastTime).milliseconds)
                lastTime = now
            }
        }
    }

    fun onPauseMenuOptionSelected(option: PauseMenuOption) {
        when (option) {
            is RomPauseMenuOption -> {
                when (option) {
                    RomPauseMenuOption.SETTINGS -> _uiEvent.tryEmit(
                        EmulatorUiEvent.OpenScreen.SettingsScreen(
                            (_emulatorState.value as? EmulatorState.RunningRom)?.rom?.let {
                                getInGameRomSettingsOverrides(it)
                            } ?: InGameRomSettingsOverrides(),
                        ),
                    )
                    RomPauseMenuOption.ROM_SETTINGS -> {
                        (_emulatorState.value as? EmulatorState.RunningRom)?.rom?.let { rom ->
                            sessionCoroutineScope.launch {
                                val renderConfiguration = settingsRepository.getEmulatorConfiguration(rom.config).rendererConfiguration
                                _uiEvent.emit(
                                    EmulatorUiEvent.ShowRomSettings(
                                        rom = rom,
                                        renderer = renderConfiguration.renderer,
                                        menuState = buildInGameRomSettingsMenuState(rom),
                                    ),
                                )
                            }
                        }
                    }
                    RomPauseMenuOption.SAVE_STATE -> {
                        if (emulatorSession.areSaveStatesAllowed()) {
                            (_emulatorState.value as? EmulatorState.RunningRom)?.let {
                                val saveStateSlots = getRomSaveStateSlots(it.rom)
                                _uiEvent.tryEmit(EmulatorUiEvent.ShowRomSaveStates(saveStateSlots, EmulatorUiEvent.ShowRomSaveStates.Reason.SAVING))
                            }
                        }
                    }
                    RomPauseMenuOption.LOAD_STATE -> {
                        if (emulatorSession.areSaveStateLoadsAllowed()) {
                            (_emulatorState.value as? EmulatorState.RunningRom)?.let {
                                val saveStateSlots = getRomSaveStateSlots(it.rom)
                                _uiEvent.tryEmit(EmulatorUiEvent.ShowRomSaveStates(saveStateSlots, EmulatorUiEvent.ShowRomSaveStates.Reason.LOADING))
                            }
                        } else {
                            _toastEvent.tryEmit(ToastEvent.CannotLoadSaveStatesWhenRAHardcoreIsEnabled)
                        }
                    }
                    RomPauseMenuOption.REWIND -> {
                        if (!settingsRepository.isRewindEnabled()) {
                            _toastEvent.tryEmit(ToastEvent.RewindNotEnabled)
                        } else if (!emulatorSession.areSaveStateLoadsAllowed()) {
                            _toastEvent.tryEmit(ToastEvent.RewindNotAvailableWhileRAHardcoreModeEnabled)
                        } else {
                            sessionCoroutineScope.launch {
                                val rewindWindow = emulatorManager.getRewindWindow()
                                _uiEvent.emit(EmulatorUiEvent.ShowRewindWindow(rewindWindow))
                            }
                        }
                    }
                    RomPauseMenuOption.CHEATS -> {
                        if (emulatorSession.areCheatsEnabled()) {
                            (_emulatorState.value as? EmulatorState.RunningRom)?.let {
                                getRomInfo(it.rom)?.let { romInfo ->
                                    _uiEvent.tryEmit(EmulatorUiEvent.OpenScreen.CheatsScreen(romInfo))
                                }
                            }
                        } else {
                            _toastEvent.tryEmit(ToastEvent.CannotUseCheatsWhenRAHardcoreIsEnabled)
                        }
                    }
                    RomPauseMenuOption.VIEW_ACHIEVEMENTS -> _uiEvent.tryEmit(EmulatorUiEvent.ShowAchievementList)
                    RomPauseMenuOption.PRESETS -> _uiEvent.tryEmit(EmulatorUiEvent.ShowDualScreenPresets)
                    RomPauseMenuOption.RENDERER_DEBUG -> _uiEvent.tryEmit(EmulatorUiEvent.ShowRendererDebugMenu)
                    RomPauseMenuOption.RESET -> resetEmulator()
                    RomPauseMenuOption.EXIT -> exitEmulator()
                }
            }
            is FirmwarePauseMenuOption -> {
                when (option) {
                    FirmwarePauseMenuOption.SETTINGS -> _uiEvent.tryEmit(EmulatorUiEvent.OpenScreen.SettingsScreen())
                    FirmwarePauseMenuOption.RESET -> resetEmulator()
                    FirmwarePauseMenuOption.EXIT -> {
                        stopEmulator()
                        _uiEvent.tryEmit(EmulatorUiEvent.CloseEmulator)
                    }
                }
            }
        }
    }

    fun dumpRendererDebugCapture() {
        sessionCoroutineScope.launch {
            val rendererDebugToolsEnabled = settingsRepository.isRendererDebugToolsEnabled().firstOrNull() == true
            if (!rendererDebugToolsEnabled) {
                _toastEvent.emit(ToastEvent.RendererDebugCaptureFailed)
                return@launch
            }

            val configuredRenderer = settingsRepository.getCurrentVideoRenderer()
            val captureResult = withContext(Dispatchers.Default) {
                RendererDebugCaptureLogger.dumpPauseMenuCapture(configuredRenderer)
            }

            if (captureResult.success) {
                _toastEvent.emit(ToastEvent.RendererDebugCaptureLogged(captureResult.captureId))
            } else {
                _toastEvent.emit(ToastEvent.RendererDebugCaptureFailed)
            }
        }
    }

    fun onOpenRewind() {
        if (!settingsRepository.isRewindEnabled()) {
            _toastEvent.tryEmit(ToastEvent.RewindNotEnabled)
            return
        }

        if (!emulatorSession.areSaveStateLoadsAllowed()) {
            _toastEvent.tryEmit(ToastEvent.RewindNotAvailableWhileRAHardcoreModeEnabled)
            return
        }

        sessionCoroutineScope.launch {
            emulatorManager.pauseEmulator()
            val rewindWindow = emulatorManager.getRewindWindow()
            _uiEvent.emit(EmulatorUiEvent.ShowRewindWindow(rewindWindow))
        }
    }

    fun onFastForwardToggleRequested(): Boolean {
        return true
    }

    fun rewindToState(rewindSaveState: RewindSaveState) {
        if (!emulatorSession.areSaveStateLoadsAllowed()) {
            _toastEvent.tryEmit(ToastEvent.RewindNotAvailableWhileRAHardcoreModeEnabled)
            return
        }

        sessionCoroutineScope.launch {
            emulatorManager.loadRewindState(rewindSaveState)
        }
    }

    fun saveStateToSlot(slot: SaveStateSlot) {
        sessionCoroutineScope.launch {
            (_emulatorState.value as? EmulatorState.RunningRom)?.let {
                emulatorManager.pauseEmulator()
                try {
                    if (!saveRomState(it.rom, slot)) {
                        _toastEvent.emit(ToastEvent.StateSaveFailed)
                    }
                } finally {
                    emulatorManager.resumeEmulator()
                }
            }
        }
    }

    fun loadStateFromSlot(slot: SaveStateSlot) {
        if (!emulatorSession.areSaveStateLoadsAllowed()) {
            _toastEvent.tryEmit(ToastEvent.CannotLoadSaveStatesWhenRAHardcoreIsEnabled)
            return
        }

        if (!slot.exists) {
            _toastEvent.tryEmit(ToastEvent.StateStateDoesNotExist)
        } else {
            sessionCoroutineScope.launch {
                (_emulatorState.value as? EmulatorState.RunningRom)?.let {
                    if (!loadRomState(it.rom, slot)) {
                        _toastEvent.emit(ToastEvent.StateLoadFailed)
                    }
                    emulatorManager.resumeEmulator()
                }
            }
        }
    }

    fun doQuickSave() {
        val currentState = _emulatorState.value
        when (currentState) {
            is EmulatorState.RunningRom -> {
                sessionCoroutineScope.launch {
                    emulatorManager.pauseEmulator()
                    val quickSlot = saveStatesRepository.getRomQuickSaveStateSlot(currentState.rom)
                    if (saveRomState(currentState.rom, quickSlot)) {
                        _toastEvent.emit(ToastEvent.QuickSaveSuccessful)
                    }
                    emulatorManager.resumeEmulator()
                }
            }
            is EmulatorState.RunningFirmware -> {
                _toastEvent.tryEmit(ToastEvent.CannotSaveStateWhenRunningFirmware)
            }
            else -> {
                // Do nothing
            }
        }
    }

    fun doQuickLoad() {
        val currentState = _emulatorState.value
        when (currentState) {
            is EmulatorState.RunningRom -> {
                if (emulatorSession.areSaveStateLoadsAllowed()) {
                    sessionCoroutineScope.launch {
                        emulatorManager.pauseEmulator()
                        val quickSlot = saveStatesRepository.getRomQuickSaveStateSlot(currentState.rom)
                        if (loadRomState(currentState.rom, quickSlot)) {
                            _toastEvent.emit(ToastEvent.QuickLoadSuccessful)
                        }
                        emulatorManager.resumeEmulator()
                    }
                } else {
                    _toastEvent.tryEmit(ToastEvent.CannotLoadSaveStatesWhenRAHardcoreIsEnabled)
                }
            }
            is EmulatorState.RunningFirmware -> {
                _toastEvent.tryEmit(ToastEvent.CannotLoadStateWhenRunningFirmware)
            }
            else -> {
                // Do nothing
            }
        }
    }

    fun deleteSaveStateSlot(slot: SaveStateSlot): List<SaveStateSlot>? {
        return (_emulatorState.value as? EmulatorState.RunningRom)?.let {
            saveStatesRepository.deleteRomSaveState(it.rom, slot)
            getRomSaveStateSlots(it.rom)
        }
    }

    private suspend fun saveRomState(rom: Rom, slot: SaveStateSlot): Boolean {
        val slotUri = saveStatesRepository.getRomSaveStateUri(rom, slot)
        if (!emulatorManager.saveState(slotUri)) {
            return false
        }

        withContext(Dispatchers.IO) {
            saveStatesRepository.deleteRomSaveStateScreenshot(rom, slot)
            val screenshot = screenshotFrameBufferProvider.getScreenshot()
            saveStatesRepository.setRomSaveStateScreenshot(rom, slot, screenshot)
        }

        return true
    }

    private suspend fun loadRomState(rom: Rom, slot: SaveStateSlot): Boolean {
        if (!slot.exists) {
            return false
        }

        val slotUri = saveStatesRepository.getRomSaveStateUri(rom, slot)
        val success = emulatorManager.loadState(slotUri)
        if (success) {
            _achievementsEvent.emit(RAEventUi.Reset)
        }

        return success
    }

    private suspend fun maybeAutoLoadStateOnLaunch(rom: Rom) {
        if (!settingsRepository.isAutoLoadStateOnLaunchEnabled()) {
            Log.i(AUTO_STATE_TAG, "auto-load skipped: setting disabled")
            return
        }

        if (!emulatorSession.areSaveStateLoadsAllowed()) {
            Log.i(AUTO_STATE_TAG, "auto-load skipped: save-state loads not allowed")
            return
        }

        val quickSlot = saveStatesRepository.getRomQuickSaveStateSlot(rom)
        if (!quickSlot.exists) {
            Log.i(AUTO_STATE_TAG, "auto-load skipped: quick slot missing")
            return
        }

        val quickSlotUri = runCatching { saveStatesRepository.getRomSaveStateUri(rom, quickSlot) }
            .onFailure { Log.w(AUTO_STATE_TAG, "auto-load skipped: failed to resolve quick slot for ${rom.name}", it) }
            .getOrNull()
        if (quickSlotUri == null || !isSavestateHeaderValid(quickSlotUri)) {
            _toastEvent.tryEmit(ToastEvent.InvalidAutoLoadState)
            Log.w(AUTO_STATE_TAG, "auto-load skipped: invalid quick slot for ${rom.name}")
            return
        }

        Log.i(AUTO_STATE_TAG, "auto-load start: slot=${quickSlot.slot} rom=${rom.name}")
        emulatorManager.pauseEmulator()
        val didLoad = runCatching {
            loadRomState(rom, quickSlot)
        }.onFailure {
            Log.w(AUTO_STATE_TAG, "auto-load failed with exception: slot=${quickSlot.slot} rom=${rom.name}", it)
        }.getOrDefault(false)
        emulatorManager.resumeEmulator()
        if (didLoad) {
            _toastEvent.tryEmit(ToastEvent.QuickLoadSuccessful)
            Log.i(AUTO_STATE_TAG, "auto-load success: slot=${quickSlot.slot} rom=${rom.name}")
        } else {
            _toastEvent.tryEmit(ToastEvent.InvalidAutoLoadState)
            Log.w(AUTO_STATE_TAG, "auto-load failed: slot=${quickSlot.slot} rom=${rom.name}")
        }
    }

    private suspend fun isSavestateHeaderValid(uri: Uri): Boolean = withContext(Dispatchers.IO) {
        runCatching {
            context.contentResolver.openFileDescriptor(uri, "r")?.use { descriptor ->
                val expectedSize = descriptor.statSize
                if (expectedSize in 0 until SAVESTATE_HEADER_SIZE) {
                    return@use false
                }

                val header = ByteArray(SAVESTATE_HEADER_SIZE)
                val read = FileInputStream(descriptor.fileDescriptor).use { stream ->
                    stream.read(header)
                }
                if (read < SAVESTATE_HEADER_SIZE) {
                    return@use false
                }

                val hasMagic = header[0] == 'M'.code.toByte() &&
                    header[1] == 'E'.code.toByte() &&
                    header[2] == 'L'.code.toByte() &&
                    header[3] == 'N'.code.toByte()
                val major = readLe16(header, 4)
                val minor = readLe16(header, 6)
                val stateLength = readLe32(header, 8)

                hasMagic &&
                    major == SAVESTATE_MAJOR &&
                    minor <= SAVESTATE_MINOR &&
                    (expectedSize < 0 || stateLength == expectedSize)
            } ?: false
        }.onFailure {
            Log.w(AUTO_STATE_TAG, "Failed to validate savestate header for $uri", it)
        }.getOrDefault(false)
    }

    private fun readLe16(bytes: ByteArray, offset: Int): Int {
        return (bytes[offset].toInt() and 0xFF) or
            ((bytes[offset + 1].toInt() and 0xFF) shl 8)
    }

    private fun readLe32(bytes: ByteArray, offset: Int): Long {
        return ((bytes[offset].toLong() and 0xFF) or
            ((bytes[offset + 1].toLong() and 0xFF) shl 8) or
            ((bytes[offset + 2].toLong() and 0xFF) shl 16) or
            ((bytes[offset + 3].toLong() and 0xFF) shl 24))
    }

    private suspend fun maybeAutoSaveStateOnExit(rom: Rom) {
        if (!settingsRepository.isAutoSaveStateOnExitEnabled()) {
            Log.i(AUTO_STATE_TAG, "auto-save skipped: setting disabled")
            return
        }

        if (!emulatorSession.areSaveStatesAllowed()) {
            Log.i(AUTO_STATE_TAG, "auto-save skipped: save-states not allowed")
            return
        }

        emulatorManager.pauseEmulator()
        val quickSlot = saveStatesRepository.getRomQuickSaveStateSlot(rom)
        Log.i(AUTO_STATE_TAG, "auto-save start: slot=${quickSlot.slot} rom=${rom.name}")
        val didSave = saveRomState(rom, quickSlot)
        if (didSave) {
            _toastEvent.tryEmit(ToastEvent.QuickSaveSuccessful)
            Log.i(AUTO_STATE_TAG, "auto-save success: slot=${quickSlot.slot} rom=${rom.name}")
        } else {
            _toastEvent.tryEmit(ToastEvent.StateSaveFailed)
            Log.w(AUTO_STATE_TAG, "auto-save failed: slot=${quickSlot.slot} rom=${rom.name}")
        }
    }

    private fun startObservingRuntimeInputLayoutConfiguration() {
        sessionCoroutineScope.launch {
            val dualScreenPresetConfiguration = combine(
                _dualScreenPreset,
                _dualScreenIntegerScaleEnabled,
                _externalDisplayKeepAspectRatioEnabled,
                _dualScreenInternalFillHeightEnabled,
                _dualScreenInternalFillWidthEnabled,
                _dualScreenExternalFillHeightEnabled,
                _dualScreenExternalFillWidthEnabled,
                _dualScreenInternalVerticalAlignmentOverride,
                _dualScreenExternalVerticalAlignmentOverride,
            ) { values: Array<Any?> ->
                @Suppress("UNCHECKED_CAST")
                DualScreenPresetConfiguration(
                    preset = values[0] as DualScreenPreset,
                    integerScale = values[1] as Boolean,
                    keepAspectRatio = values[2] as Boolean,
                    internalFillHeight = values[3] as Boolean,
                    internalFillWidth = values[4] as Boolean,
                    externalFillHeight = values[5] as Boolean,
                    externalFillWidth = values[6] as Boolean,
                    internalAlignmentOverride = values[7] as ScreenAlignment?,
                    externalAlignmentOverride = values[8] as ScreenAlignment?,
                )
            }

            val layoutConfiguration = combine(
                _layout,
                _currentLayout,
                settingsRepository.getSoftInputBehaviour(),
                settingsRepository.isTouchHapticFeedbackEnabled(),
                settingsRepository.getSoftInputOpacity(),
            ) { globalLayoutConfiguration, variant, softInputBehaviour, isHapticFeedbackEnabled, inputOpacity ->
                RuntimeLayoutConfiguration(
                    layoutConfiguration = globalLayoutConfiguration,
                    layoutVariant = variant,
                    softInputBehaviour = softInputBehaviour,
                    isHapticFeedbackEnabled = isHapticFeedbackEnabled,
                    inputOpacity = inputOpacity,
                )
            }

            combine(layoutConfiguration, dualScreenPresetConfiguration) { config, dualScreenConfig ->
                val currentLayoutConfiguration = config.layoutConfiguration
                val currentLayoutVariant = config.layoutVariant
                val currentVariant = currentLayoutVariant?.first
                val currentLayout = currentLayoutVariant?.second
                if (currentLayoutConfiguration == null || currentLayout == null || currentVariant == null) {
                    null
                } else {
                    val opacity = if (currentLayoutConfiguration.useCustomOpacity) {
                        currentLayoutConfiguration.opacity
                    } else {
                        config.inputOpacity
                    }

                    val adjustedLayout = applyDualScreenPresetLayoutOverrides(currentLayout, currentVariant, dualScreenConfig)
                    RuntimeInputLayoutConfiguration(
                        softInputBehaviour = config.softInputBehaviour,
                        softInputOpacity = opacity,
                        isHapticFeedbackEnabled = config.isHapticFeedbackEnabled,
                        layoutOrientation = currentLayoutConfiguration.orientation,
                        layout = adjustedLayout,
                    )
                }
            }.collect(_runtimeLayout)
        }
    }

    private data class RuntimeLayoutConfiguration(
        val layoutConfiguration: LayoutConfiguration?,
        val layoutVariant: Pair<UILayoutVariant, UILayout>?,
        val softInputBehaviour: SoftInputBehaviour,
        val isHapticFeedbackEnabled: Boolean,
        val inputOpacity: Int,
    )

    private data class DualScreenPresetConfiguration(
        val preset: DualScreenPreset,
        val integerScale: Boolean,
        val keepAspectRatio: Boolean,
        val internalFillHeight: Boolean,
        val internalFillWidth: Boolean,
        val externalFillHeight: Boolean,
        val externalFillWidth: Boolean,
        val internalAlignmentOverride: ScreenAlignment?,
        val externalAlignmentOverride: ScreenAlignment?,
    )

    private fun applyDualScreenPresetLayoutOverrides(layout: UILayout, variant: UILayoutVariant, config: DualScreenPresetConfiguration): UILayout {
        if (config.preset == DualScreenPreset.OFF || variant.displays.secondaryScreenDisplay == null) {
            return layout
        }

        val canFill = config.integerScale || config.keepAspectRatio
        val internalAlignment = config.internalAlignmentOverride ?: config.preset.defaultInternalAlignment()
        val externalAlignment = config.externalAlignmentOverride ?: config.preset.defaultExternalAlignment()

        val adjustedInternalLayout = applyScreenScaleToLayout(
            screenLayout = layout.mainScreenLayout,
            availableWidth = variant.uiSize.x,
            availableHeight = variant.uiSize.y,
            integerScale = config.integerScale,
            keepAspectRatio = config.keepAspectRatio,
            fillHeight = config.internalFillHeight && canFill,
            fillWidth = config.internalFillWidth && canFill,
            alignment = internalAlignment,
        )

        val secondaryDisplay = variant.displays.secondaryScreenDisplay
        val adjustedSecondaryLayout = applyScreenScaleToLayout(
            screenLayout = layout.secondaryScreenLayout,
            availableWidth = secondaryDisplay.width,
            availableHeight = secondaryDisplay.height,
            integerScale = config.integerScale,
            keepAspectRatio = config.keepAspectRatio,
            fillHeight = config.externalFillHeight && canFill,
            fillWidth = config.externalFillWidth && canFill,
            alignment = externalAlignment,
        )

        return layout.copy(
            mainScreenLayout = adjustedInternalLayout,
            secondaryScreenLayout = adjustedSecondaryLayout,
        )
    }

    private fun applyScreenScaleToLayout(
        screenLayout: ScreenLayout,
        availableWidth: Int,
        availableHeight: Int,
        integerScale: Boolean,
        keepAspectRatio: Boolean,
        fillHeight: Boolean,
        fillWidth: Boolean,
        alignment: ScreenAlignment,
    ): ScreenLayout {
        val currentComponents = screenLayout.components ?: return screenLayout
        val screenComponents = currentComponents.filter { it.isScreen() }
        if (screenComponents.size != 1) {
            return screenLayout
        }
        if (availableWidth <= 0 || availableHeight <= 0) {
            return screenLayout
        }

        val screenComponent = screenComponents.single()
        val scaledRect = computeScaledScreenRect(
            availableWidth = availableWidth,
            availableHeight = availableHeight,
            integerScale = integerScale,
            keepAspectRatio = keepAspectRatio,
            fillHeight = fillHeight,
            fillWidth = fillWidth,
            alignment = alignment,
        )

        val updatedComponents = currentComponents.map {
            if (it == screenComponent) {
                it.copy(rect = scaledRect)
            } else {
                it
            }
        }
        return screenLayout.copy(components = updatedComponents)
    }

    private fun computeScaledScreenRect(
        availableWidth: Int,
        availableHeight: Int,
        integerScale: Boolean,
        keepAspectRatio: Boolean,
        fillHeight: Boolean,
        fillWidth: Boolean,
        alignment: ScreenAlignment,
    ): Rect {
        val (baseWidth, baseHeight) = when {
            integerScale -> computeIntegerScaleDimensions(availableWidth, availableHeight)
            keepAspectRatio -> computeAspectRatioDimensions(availableWidth, availableHeight)
            else -> availableWidth to availableHeight
        }

        val scaledWidth = if (fillWidth) availableWidth else baseWidth
        val scaledHeight = if (fillHeight) availableHeight else baseHeight

        val left = ((availableWidth - scaledWidth) / 2f).roundToInt().coerceAtLeast(0)
        val top = when (alignment) {
            ScreenAlignment.TOP -> 0
            ScreenAlignment.CENTER -> ((availableHeight - scaledHeight) / 2f).roundToInt().coerceAtLeast(0)
            ScreenAlignment.BOTTOM -> (availableHeight - scaledHeight).coerceAtLeast(0)
        }

        return Rect(left, top, scaledWidth.coerceAtLeast(1), scaledHeight.coerceAtLeast(1))
    }

    private fun computeIntegerScaleDimensions(availableWidth: Int, availableHeight: Int): Pair<Int, Int> {
        val widthScale = availableWidth / SCREEN_WIDTH
        val heightScale = availableHeight / SCREEN_HEIGHT
        val maxIntegerScale = min(widthScale, heightScale)
        val scale = if (maxIntegerScale <= 0) {
            min(
                availableWidth.toFloat() / SCREEN_WIDTH,
                availableHeight.toFloat() / SCREEN_HEIGHT,
            )
        } else {
            maxIntegerScale.toFloat()
        }
        val width = (SCREEN_WIDTH * scale).roundToInt().coerceAtLeast(1).coerceAtMost(availableWidth)
        val height = (SCREEN_HEIGHT * scale).roundToInt().coerceAtLeast(1).coerceAtMost(availableHeight)
        return width to height
    }

    private fun computeAspectRatioDimensions(availableWidth: Int, availableHeight: Int): Pair<Int, Int> {
        val widthRatio = availableWidth.toFloat() / SCREEN_WIDTH
        val heightRatio = availableHeight.toFloat() / SCREEN_HEIGHT
        val scale = min(widthRatio, heightRatio)
        val width = (SCREEN_WIDTH * scale).roundToInt().coerceAtLeast(1).coerceAtMost(availableWidth)
        val height = (SCREEN_HEIGHT * scale).roundToInt().coerceAtLeast(1).coerceAtMost(availableHeight)
        return width to height
    }

    private fun resetEmulatorState(newState: EmulatorState) {
        finalizeOfflineRetroAchievementsSessionIfNeeded()
        sessionCoroutineScope.notifyNewSessionStarted()
        emulatorSession.reset()
        raSessionJob = null
        _currentFps.value = null
        _emulatorState.value = newState
        _mainScreenBackground.value = RuntimeBackground.None
        _secondaryScreenBackground.value = RuntimeBackground.None
        _layout.value = null
        currentRom = null
        activeRomConfig.value = null
        currentRetroAchievementsGameId = null
        offlineSyncChoiceDeferred?.cancel()
        offlineSyncChoiceDeferred = null
        hardcoreExitChoiceDeferred?.cancel()
        hardcoreExitChoiceDeferred = null
        retroAchievementsNetworkMode = RetroAchievementsNetworkMode.ONLINE_LIVE
        retroAchievementsSessionMode = RetroAchievementsSessionMode.SOFTCORE
        isHardcoreEligibleAfterOnlineStart = false
        startedSessionOnlineLive = false
        isRetroAchievementsOnlineSessionStarted = false
        activeRuntimeBridgeConfig = null
        activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
        didReceiveRendererInitFailure = false
        announcedMasteryKeys.clear()
        pendingRuntimeAchievementTriggers.clear()
        pendingRuntimeLeaderboardCompletions.clear()
    }

    private fun startObservingEmulatorEvents() {
        sessionCoroutineScope.launch {
            emulatorManager.emulatorEvents.collect {
                when (it) {
                    is EmulatorEvent.RumbleStart -> _rumbleEvent.tryEmit(RumbleEvent.RumbleStart(it.duration))
                    EmulatorEvent.RumbleStop -> _rumbleEvent.tryEmit(RumbleEvent.RumbleStop)
                    is EmulatorEvent.RendererInitFailed -> {
                        didReceiveRendererInitFailure = true
                        val failedRenderer = it.renderer
                        settingsRepository.getCurrentVideoRenderer()
                            .takeIf { configuredRenderer -> configuredRenderer == failedRenderer }
                            ?.let {
                                val activeRenderer = getRuntimeRendererOrNull()
                                if (activeRenderer != null && activeRenderer != failedRenderer) {
                                    settingsRepository.setCurrentVideoRenderer(activeRenderer)
                                }
                        }
                        _toastEvent.tryEmit(ToastEvent.RendererInitFailed(failedRenderer))
                    }
                    is EmulatorEvent.VulkanCompileProgress -> updateLoadingCompileProgress(it)
                    is EmulatorEvent.Stop -> {
                        when (it.reason) {
                            EmulatorEvent.Stop.Reason.GBAModeNotSupported -> _toastEvent.tryEmit(ToastEvent.GbaModeNotSupported)
                            EmulatorEvent.Stop.Reason.BadExceptionRegion -> {
                                if (!didReceiveRendererInitFailure) {
                                    _toastEvent.tryEmit(ToastEvent.InternalError)
                                }
                            }
                            EmulatorEvent.Stop.Reason.PowerOff -> { /* no-op */ }
                        }
                        when (_emulatorState.value) {
                            is EmulatorState.LoadingRom -> {
                                stopEmulator()
                                _emulatorState.value = EmulatorState.RomLoadError
                            }
                            is EmulatorState.LoadingFirmware -> {
                                stopEmulator()
                                _emulatorState.value = EmulatorState.FirmwareLoadError(MelonEmulator.FirmwareLoadResult.FIRMWARE_BAD)
                            }
                            else -> stopEmulatorAndExit()
                        }
                    }
                }
            }
        }
    }

    private fun updateLoadingCompileProgress(progressEvent: EmulatorEvent.VulkanCompileProgress) {
        val progress = VulkanCompileProgress(
            stageId = progressEvent.stageId,
            current = progressEvent.current,
            total = progressEvent.total,
        )
        when (val currentState = _emulatorState.value) {
            is EmulatorState.LoadingRom -> _emulatorState.value = currentState.copy(vulkanCompileProgress = progress)
            is EmulatorState.LoadingFirmware -> _emulatorState.value = currentState.copy(vulkanCompileProgress = progress)
            else -> Unit
        }
    }

    private fun startObservingAchievementEvents() {
        sessionCoroutineScope.launch {
            emulatorManager.observeRetroAchievementEvents().collect {
                logRaTrace(
                    "runtime_event_kotlin_received",
                    "event" to it::class.simpleName,
                    "runtime_path" to activeRuntimePath.name,
                )
                logRaRuntimeEvent(it)
                when (it) {
                    is RAEvent.OnAchievementPrimed -> onAchievementPrimed(it.achievementId)
                    is RAEvent.OnAchievementUnPrimed -> onAchievementUnPrimed(it.achievementId)
                    is RAEvent.OnAchievementTriggered -> onAchievementTriggered(it.achievementId)
                    is RAEvent.OnAchievementProgressUpdated -> onAchievementProgressUpdated(it)
                    is RAEvent.OnGameCompleted -> onSetCompleted(it.subsetId)
                    is RAEvent.OnSubsetCompleted -> onSetCompleted(it.subsetId)
                    is RAEvent.OnServerError -> onRuntimeServerError(it)
                    RAEvent.OnDisconnected -> onRuntimeDisconnected()
                    RAEvent.OnReconnected -> onRuntimeReconnected()
                    is RAEvent.OnLeaderboardAttemptStarted -> onLeaderboardAttemptStarted(it)
                    is RAEvent.OnLeaderboardAttemptUpdated -> onLeaderboardAttemptUpdated(it)
                    is RAEvent.OnLeaderboardAttemptCompleted -> onLeaderboardAttemptCompleted(it)
                    is RAEvent.OnLeaderboardAttemptCancelled -> onLeaderboardAttemptCancelled(it)
                    is RAEvent.OnAchievementProgressHidden -> onAchievementProgressHidden(it.achievementId)
                    is RAEvent.OnLeaderboardTrackerHidden -> onLeaderboardTrackerHidden(it.leaderboardId)
                }
            }
        }
    }

    private fun startObservingMainScreenBackground() {
        sessionCoroutineScope.launch {
            combine(_currentLayout, ensureEmulatorIsRunning()) { variant, _ ->
                val layout = variant?.second
                if (layout == null) {
                    RuntimeBackground.None
                } else {
                    loadBackground(layout.mainScreenLayout.backgroundId, layout.mainScreenLayout.backgroundMode)
                }
            }.collect(_mainScreenBackground)
        }
    }

    private fun startObservingSecondaryScreenBackground() {
        sessionCoroutineScope.launch {
            combine(_currentLayout, ensureEmulatorIsRunning()) { variant, _ ->
                val layout = variant?.second
                if (layout == null) {
                    RuntimeBackground.None
                } else {
                    loadBackground(layout.secondaryScreenLayout.backgroundId, layout.secondaryScreenLayout.backgroundMode)
                }
            }.collect(_secondaryScreenBackground)
        }
    }

    private fun startObservingLayoutForRom() {
        sessionCoroutineScope.launch {
            combine(
                activeRomConfig.flatMapLatest { rom ->
                    val romLayoutId = rom?.config?.layoutId
                    if (romLayoutId == null) {
                        getGlobalLayoutFlow()
                    } else {
                        layoutsRepository.observeLayout(romLayoutId)
                            .onCompletion {
                                emitAll(getGlobalLayoutFlow())
                            }
                    }
                },
                ensureEmulatorIsRunning(),
            ) { layout, _ ->
                layout
            }.collect(_layout)
        }
    }

    private fun startObservingRendererConfiguration() {
        sessionCoroutineScope.launch {
            _emulatorState.flatMapLatest { state ->
                val romConfig = (state as? EmulatorState.RunningRom)?.rom?.config
                if (romConfig == null) {
                    settingsRepository.observeRenderConfiguration()
                } else {
                    settingsRepository.observeRenderConfiguration(romConfig)
                }
            }.collectLatest {
                _runtimeRendererConfiguration.value = RuntimeRendererConfiguration(
                    renderer = it.renderer,
                    videoFiltering = it.videoFiltering,
                    resolutionScaling = it.resolutionScaling,
                    retroArchShader = it.retroArchShader,
                )
            }
        }
    }

    private fun startObservingLayoutForFirmware() {
        _layout.value = null

        sessionCoroutineScope.launch {
            combine(getGlobalLayoutFlow(), ensureEmulatorIsRunning()) { layout, _ ->
                layout
            }.collect(_layout)
        }
    }

    private suspend fun loadBackground(backgroundId: UUID?, mode: BackgroundMode): RuntimeBackground {
        return if (backgroundId == null) {
            RuntimeBackground(null, mode)
        } else {
            val background = backgroundsRepository.getBackground(backgroundId)
            RuntimeBackground(background, mode)
        }
    }

    private fun getGlobalLayoutFlow(): Flow<LayoutConfiguration> {
        return settingsRepository.observeSelectedLayoutId()
            .flatMapLatest {
                layoutsRepository.observeLayout(it)
                    .onCompletion {
                        emitAll(layoutsRepository.observeLayout(LayoutConfiguration.DEFAULT_ID))
                    }
            }
    }

    private fun getRuntimeRendererOrNull(): VideoRenderer? {
        val renderer = MelonEmulator.getCurrentRenderer()
        return VideoRenderer.entries.firstOrNull { it.renderer == renderer }
    }

    private fun getRomInfo(rom: Rom): RomInfo? {
        val fileRomProcessor = romFileProcessorFactory.getFileRomProcessorForDocument(rom.uri)
        return fileRomProcessor?.getRomInfo(rom)
    }

    private fun getRomSaveStateSlots(rom: Rom): List<SaveStateSlot> {
        return saveStatesRepository.getRomSaveStates(rom)
    }

    fun isSustainedPerformanceModeEnabled(): Boolean {
        return settingsRepository.isSustainedPerformanceModeEnabled()
    }

    fun getFpsCounterPosition(): FpsCounterPosition {
        return settingsRepository.getFpsCounterPosition()
    }

    private suspend fun getRomEnabledCheats(romInfo: RomInfo): List<Cheat> {
        if (!settingsRepository.areCheatsEnabled() || !emulatorSession.areCheatsEnabled()) {
            return emptyList()
        }

        return cheatsRepository.getRomEnabledCheats(romInfo)
    }

    private suspend fun getRomAchievementData(rom: Rom): OnlineRetroAchievementsBootstrap {
        val userAuth = retroAchievementsRepository.getUserAuthentication()
        when (userAuth) {
            is RAUserAuth.Authenticated -> { /* no-op */ }
            is RAUserAuth.AuthenticationExpired -> return OnlineRetroAchievementsBootstrap(
                achievementData = GameAchievementData.withDisabledRetroAchievementsIntegration(GameAchievementData.IntegrationStatus.DISABLED_LOGIN_EXPIRED),
                source = OnlineRetroAchievementsBootstrapSource.NETWORK,
            )
            null -> return OnlineRetroAchievementsBootstrap(
                achievementData = GameAchievementData.withDisabledRetroAchievementsIntegration(GameAchievementData.IntegrationStatus.DISABLED_NOT_LOGGED_IN),
                source = OnlineRetroAchievementsBootstrapSource.NETWORK,
            )
        }

        val forHardcoreMode = emulatorSession.isRetroAchievementsHardcoreModeEnabled

        markRetroAchievementsLoadStage(RetroAchievementsLoadStage.FETCHING_LATEST_DATA)
        try {
            val networkResult = withContext(Dispatchers.IO) {
                runCatching {
                    withTimeout(RETROACHIEVEMENTS_REFRESH_TIMEOUT_MS) {
                        retroAchievementsRepository.refreshUserGameData(rom.retroAchievementsHash, forHardcoreMode).getOrThrow()
                    }
                }
            }

            networkResult.getOrNull()?.let { refreshedGameData ->
                logRaTrace(
                    "ra_bootstrap_network_hit",
                    "content_id" to rom.retroAchievementsHash,
                    "game_id" to refreshedGameData.id.id,
                )
                currentRetroAchievementsGameId = refreshedGameData.id.id
                maybeWritePrefetchCache(
                    userId = userAuth.username,
                    contentId = rom.retroAchievementsHash,
                    userGameData = refreshedGameData,
                )
                return OnlineRetroAchievementsBootstrap(
                    achievementData = buildAchievementDataFromUserGameData(refreshedGameData),
                    source = OnlineRetroAchievementsBootstrapSource.NETWORK,
                )
            }

            val networkError = networkResult.exceptionOrNull()
            if (networkResult.isSuccess || networkError is RAGameNotExist) {
                logRaTrace(
                    "ra_bootstrap_game_not_found",
                    "content_id" to rom.retroAchievementsHash,
                    "error" to (networkError?.javaClass?.simpleName ?: "none"),
                )
                return OnlineRetroAchievementsBootstrap(
                    achievementData = buildGameNotFoundAchievementData(rom),
                    source = OnlineRetroAchievementsBootstrapSource.NETWORK,
                )
            }

            logRaTrace(
                "ra_bootstrap_network_failed",
                "content_id" to rom.retroAchievementsHash,
                "error" to (networkError?.javaClass?.simpleName ?: "Unknown"),
                "timed_out" to (networkError is TimeoutCancellationException),
            )

            val cachedResult = withContext(Dispatchers.IO) {
                retroAchievementsRepository.getCachedUserGameData(rom.retroAchievementsHash, forHardcoreMode)
            }
            cachedResult.getOrNull()?.let { cachedGameData ->
                logRaTrace(
                    "ra_bootstrap_cache_fallback_hit",
                    "content_id" to rom.retroAchievementsHash,
                    "game_id" to cachedGameData.id.id,
                )
                currentRetroAchievementsGameId = cachedGameData.id.id
                return OnlineRetroAchievementsBootstrap(
                    achievementData = buildAchievementDataFromUserGameData(cachedGameData),
                    source = OnlineRetroAchievementsBootstrapSource.CACHE,
                )
            }

            logRaTrace(
                "ra_bootstrap_no_cache_no_network",
                "content_id" to rom.retroAchievementsHash,
                "cache_error" to (cachedResult.exceptionOrNull()?.javaClass?.simpleName ?: "none"),
            )

            currentRetroAchievementsGameId = null
            val gameSummary = withContext(Dispatchers.IO) {
                retroAchievementsRepository.getGameSummary(rom.retroAchievementsHash)
            }
            return OnlineRetroAchievementsBootstrap(
                achievementData = GameAchievementData.withDisabledRetroAchievementsIntegration(
                    status = GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR,
                    icon = gameSummary?.icon,
                ),
                source = OnlineRetroAchievementsBootstrapSource.NETWORK,
            )
        } finally {
            markRetroAchievementsLoadStage(null)
        }
    }

    private fun markRetroAchievementsLoadStage(stage: RetroAchievementsLoadStage?) {
        _emulatorState.update { current ->
            when (current) {
                is EmulatorState.LoadingRom -> current.copy(retroAchievementsLoadStage = stage)
                else -> current
            }
        }
    }

    private suspend fun getRomAchievementDataFromNetwork(
        rom: Rom,
        userId: String,
        forHardcoreMode: Boolean,
    ): GameAchievementData {
        return withContext(Dispatchers.IO) {
            retroAchievementsRepository.getUserGameData(rom.retroAchievementsHash, forHardcoreMode)
        }.fold(
            onSuccess = { userGameData ->
                val gameSummary = withContext(Dispatchers.IO) {
                    retroAchievementsRepository.getGameSummary(rom.retroAchievementsHash)
                }

                if (userGameData != null) {
                    currentRetroAchievementsGameId = userGameData.id.id
                    maybeWritePrefetchCache(
                        userId = userId,
                        contentId = rom.retroAchievementsHash,
                        userGameData = userGameData,
                    )
                    buildAchievementDataFromUserGameData(userGameData)
                } else {
                    currentRetroAchievementsGameId = null
                    GameAchievementData.withDisabledRetroAchievementsIntegration(
                        status = GameAchievementData.IntegrationStatus.DISABLED_GAME_NOT_FOUND,
                        icon = gameSummary?.icon,
                    )
                }
            },
            onFailure = {
                if (it is RAGameNotExist) {
                    buildGameNotFoundAchievementData(rom)
                } else {
                    currentRetroAchievementsGameId = null
                    // Maybe we have the game summary cached. Could allow the icon to be displayed, which looks better
                    val gameSummary = withContext(Dispatchers.IO) {
                        retroAchievementsRepository.getGameSummary(rom.retroAchievementsHash)
                    }
                    GameAchievementData.withDisabledRetroAchievementsIntegration(GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR, gameSummary?.icon)
                }
            }
        )
    }

    private suspend fun buildGameNotFoundAchievementData(rom: Rom): GameAchievementData {
        currentRetroAchievementsGameId = null
        val gameSummary = withContext(Dispatchers.IO) {
            retroAchievementsRepository.getGameSummary(rom.retroAchievementsHash)
        }
        Log.i(
            RA_IDENTITY_TAG,
            "source=bootstrap stage=game_not_found runtime=disabled game_hash=${rom.retroAchievementsHash} rom=${rom.name.replace(' ', '_')}",
        )
        return GameAchievementData.withDisabledRetroAchievementsIntegration(
            status = GameAchievementData.IntegrationStatus.DISABLED_GAME_NOT_FOUND,
            icon = gameSummary?.icon,
        )
    }

    private fun buildAchievementDataFromUserGameData(
        userGameData: me.magnum.melonds.domain.model.retroachievements.RAUserGameData,
    ): GameAchievementData {
        val achievements = userGameData.sets.flatMap { it.achievements }
        val leaderboards = userGameData.sets.flatMap { it.leaderboards }
        val hasLeaderboards = leaderboards.isNotEmpty() && emulatorSession.areLeaderboardsEnabled()

        return if (achievements.isEmpty() && !hasLeaderboards) {
            GameAchievementData.withLimitedRetroAchievementsIntegration(
                richPresencePatch = userGameData.richPresencePatch,
                icon = userGameData.icon,
            )
        } else {
            val lockedAchievements = achievements
                .filter { !it.isUnlocked }
                .map { RASimpleAchievement(it.achievement.id, it.achievement.memoryAddress) }
            val runtimeLeaderboards = if (hasLeaderboards) {
                leaderboards.map { RASimpleLeaderboard(it.id, it.mem, it.format) }
            } else {
                emptyList()
            }

            GameAchievementData.withFullRetroAchievementsIntegration(
                lockedAchievements = lockedAchievements,
                leaderboards = runtimeLeaderboards,
                totalAchievementCount = achievements.size,
                richPresencePatch = userGameData.richPresencePatch,
                icon = userGameData.icon,
            )
        }
    }

    private suspend fun buildOnlineRuntimeConfig(
        rom: Rom,
        launchDecision: RetroAchievementsLaunchDecision,
    ): RARuntimeBridgeConfig? {
        val userAuth = retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated ?: return null
        Log.i(
            RA_IDENTITY_TAG,
                "source=runtime_config runtime=rc_client user_agent=$retroAchievementsUserAgent " +
                "package=${context.packageName} version=$retroAchievementsVersionName " +
                "game_id=${currentRetroAchievementsGameId ?: "none"} " +
                "game_hash=${rom.retroAchievementsHash} " +
                "hardcore=${launchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE} " +
                "unofficial=${settingsRepository.areRetroAchievementsUnofficialAchievementsEnabled()} " +
                "encore=${settingsRepository.isRetroAchievementsEncoreModeEnabled()}",
        )
        return RARuntimeBridgeConfig(
            runtimeMode = RARuntimeBridgeMode.RC_CLIENT_ONLINE,
            userAgent = retroAchievementsUserAgent,
            username = userAuth.username,
            apiToken = userAuth.token,
            gameHash = rom.retroAchievementsHash,
            gameId = currentRetroAchievementsGameId,
            hardcoreEnabled = launchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE,
            unofficialEnabled = settingsRepository.areRetroAchievementsUnofficialAchievementsEnabled(),
            encoreEnabled = settingsRepository.isRetroAchievementsEncoreModeEnabled(),
        )
    }

    private fun logRaRuntimeSetup(
        stage: String,
        runtimePath: RetroAchievementsRuntimePath,
        achievementData: GameAchievementData,
        runtimeConfig: RARuntimeBridgeConfig? = activeRuntimeBridgeConfig,
        throwable: Throwable? = null,
    ) {
        val message = buildString {
            append("source=runtime_setup")
            append(" stage=").append(stage)
            append(" runtime=").append(runtimePath.traceValue)
            append(" game_id=").append(runtimeConfig?.gameId ?: currentRetroAchievementsGameId ?: "none")
            append(" game_hash=").append(runtimeConfig?.gameHash ?: currentRom?.retroAchievementsHash ?: "none")
            append(" achievements=").append(achievementData.lockedAchievements.size)
            append(" leaderboards=").append(achievementData.leaderboards.size)
            append(" has_rich_presence=").append(achievementData.richPresencePatch != null)
            append(" status=").append(achievementData.retroAchievementsIntegrationStatus.name)
            throwable?.let {
                append(" error=").append(it.javaClass.simpleName)
                it.message?.takeIf { message -> message.isNotBlank() }?.let { errorMessage ->
                    append(" error_message=").append(errorMessage.replace(' ', '_'))
                }
            }
        }

        if (throwable == null) {
            Log.i(RA_IDENTITY_TAG, message)
        } else {
            Log.w(RA_IDENTITY_TAG, message, throwable)
        }
    }

    private fun logRaSubmission(eventType: String, vararg fields: Pair<String, Any?>) {
        val message = buildString {
            append("event_type=").append(eventType)
            append(" network_mode=").append(retroAchievementsNetworkMode.name)
            append(" session_mode=").append(retroAchievementsSessionMode.name)
            append(" runtime_path=").append(activeRuntimePath.traceValue)
            append(" current_game_id=").append(currentRetroAchievementsGameId ?: "none")
            fields.forEach { (key, value) ->
                if (value != null) {
                    append(' ')
                    append(key)
                    append('=')
                    append(value.toString().replace(' ', '_'))
                }
            }
        }
        Log.i(RA_SUBMISSION_TAG, message)
    }

    private fun buildAchievementDataSignature(achievementData: GameAchievementData): String {
        return buildString {
            append(achievementData.retroAchievementsIntegrationStatus.name)
            append('|')
            append(achievementData.totalAchievementCount)
            append('|')
            append(achievementData.richPresencePatch ?: "")

            achievementData.lockedAchievements
                .sortedWith(compareBy({ it.id }, { it.memoryAddress }))
                .forEach {
                    append("|A:")
                    append(it.id)
                    append(':')
                    append(it.memoryAddress)
                }

            achievementData.leaderboards
                .sortedWith(compareBy({ it.id }, { it.memoryAddress }, { it.format }))
                .forEach {
                    append("|L:")
                    append(it.id)
                    append(':')
                    append(it.memoryAddress)
                    append(':')
                    append(it.format)
                }
        }
    }

    private suspend fun maybeWritePrefetchCache(
        userId: String,
        contentId: String,
        userGameData: me.magnum.melonds.domain.model.retroachievements.RAUserGameData,
    ) {
        if (!networkStatusProvider.isLikelyOnline()) return

        val achievements = userGameData.sets
            .asSequence()
            .flatMap { it.achievements.asSequence() }
            .map { OfflinePrefetchCacheAchievement(it.achievement.id, it.achievement.memoryAddress) }
            .distinctBy { it.id }
            .toList()

        val leaderboards = userGameData.sets
            .asSequence()
            .flatMap { it.leaderboards.asSequence() }
            .map { OfflinePrefetchCacheLeaderboard(it.id, it.mem, it.format) }
            .distinctBy { it.id }
            .toList()

        val cacheFile = OfflinePrefetchCacheFile(
            romHash = contentId,
            gameId = userGameData.id.id,
            achievements = achievements,
            leaderboards = leaderboards,
            richPresencePatch = userGameData.richPresencePatch,
            iconUrl = userGameData.icon.toString(),
            fetchedAtEpochMs = System.currentTimeMillis(),
        )

        try {
            withContext(Dispatchers.IO) {
                offlinePrefetchCacheRepository.write(userId, contentId, cacheFile)
            }
        } catch (_: Exception) {
            // Best-effort cache write. Achievements should still work online even if caching fails.
        }

        // Best-effort: warm icon/badge images so offline popups and lists can render without network.
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val urls = buildList {
                    add(userGameData.icon.toString())
                    userGameData.sets.forEach { set ->
                        add(set.iconUrl.toString())
                        set.achievements.forEach { userAchievement ->
                            add(userAchievement.achievement.badgeUrlLocked.toString())
                            add(userAchievement.achievement.badgeUrlUnlocked.toString())
                        }
                    }
                }
                retroAchievementsImageCacheWarmer.warm(urls)
            } catch (_: Exception) {
                // Best-effort only.
            }
        }
    }

    private fun onAchievementTriggered(achievementId: Long) {
        sessionCoroutineScope.launch {
            logRaTrace(
                "achievement_trigger_received",
                "achievement_id" to achievementId,
                "network_mode" to retroAchievementsNetworkMode.name,
                "session_mode" to retroAchievementsSessionMode.name,
                "online" to networkStatusProvider.isOnline(),
            )
            val achievement = retroAchievementsRepository.getAchievement(achievementId).getOrNull()
            val encoreEnabled = settingsRepository.isRetroAchievementsEncoreModeEnabled()
            if (!encoreEnabled && achievement != null) {
                val isHardcoreModeEnabled = emulatorSession.isRetroAchievementsHardcoreModeEnabled
                val alreadyUnlocked = retroAchievementsRepository.isAchievementUnlocked(
                    gameId = achievement.gameId.id,
                    achievementId = achievementId,
                    forHardcoreMode = isHardcoreModeEnabled,
                )
                if (alreadyUnlocked) {
                    logRaTrace(
                        "achievement_trigger_suppressed",
                        "achievement_id" to achievementId,
                        "reason" to "already_unlocked_no_encore",
                        "hardcore" to isHardcoreModeEnabled,
                    )
                    completeAchievementSubmissionTrace(achievementId, "already_unlocked_no_encore")
                    return@launch
                }
            }
            if (retroAchievementsNetworkMode == RetroAchievementsNetworkMode.ONLINE_LIVE && !networkStatusProvider.isLikelyOnline()) {
                transitionToOfflineAccumulationIfNeeded()
            }

            if (retroAchievementsNetworkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING) {
                val hardcorePendingInMemory = isHardcoreEligibleAfterOnlineStart && achievement != null
                logRaSubmission(
                    "achievement_submit_expected",
                    "achievement_id" to achievementId,
                    "submit_path" to if (hardcorePendingInMemory) "hardcore_memory_queue" else "offline_ledger",
                    "expected_api" to if (hardcorePendingInMemory) "awardachievement_retry_in_session" else "awardachievement_after_smart_sync",
                    "pending_sync" to true,
                    "game_id" to currentRetroAchievementsGameId,
                    "game_hash" to currentRom?.retroAchievementsHash,
                    "hardcore" to hardcorePendingInMemory,
                )
                logRaTrace(
                    "achievement_trigger_offline_queued",
                    "achievement_id" to achievementId,
                    "session_mode" to retroAchievementsSessionMode.name,
                )
                completeAchievementSubmissionTrace(achievementId, "offline_queued")
                handleOfflineAchievementTriggered(achievementId, achievement)
                return@launch
            }

            if (achievement != null) {
                val isHardcoreModeEnabled = emulatorSession.isRetroAchievementsHardcoreModeEnabled

                if (activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT) {
                    logRaSubmission(
                        "achievement_submit_expected",
                        "achievement_id" to achievementId,
                        "submit_path" to "rc_client_http",
                        "expected_api" to "awardachievement",
                        "game_id" to achievement.gameId.id,
                        "game_hash" to currentRom?.retroAchievementsHash,
                        "hardcore" to isHardcoreModeEnabled,
                    )
                    logRaTrace(
                        "achievement_submit_owned_by_rc_client",
                        "achievement_id" to achievementId,
                        "hardcore" to isHardcoreModeEnabled,
                    )
                    _achievementsEvent.emit(RAEventUi.AchievementTriggered(achievement))
                    if (isHardcoreModeEnabled) {
                    }
                    completeAchievementSubmissionTrace(achievementId, "submitted_by_rc_client")
                    return@launch
                }

                if (!ensureAchievementSubmitContext(achievement)) {
                    completeAchievementSubmissionTrace(achievementId, "context_mismatch")
                    return@launch
                }

                if (isHardcoreModeEnabled) {
                    handleHardcoreAchievementTriggered(achievement)
                } else {
                    logRaTrace(
                        "achievement_submit_attempt",
                        "achievement_id" to achievementId,
                        "hardcore" to false,
                        "game_id" to currentRetroAchievementsGameId,
                    )
                    retroAchievementsSubmissionHandler.addPendingAchievementSubmission(achievement, false)
                }
            } else {
                completeAchievementSubmissionTrace(achievementId, "achievement_missing")
            }
        }
    }

    private suspend fun transitionToOfflineAccumulationIfNeeded() {
        if (retroAchievementsNetworkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING) {
            return
        }

        if (!isHardcoreEligibleAfterOnlineStart && !settingsRepository.isRetroAchievementsOfflineSoftcoreEnabled()) {
            logRaTrace(
                "network_transition_offline_softcore_disabled",
                "started_online" to startedSessionOnlineLive,
                "game_id" to currentRetroAchievementsGameId,
                "content_id" to currentRom?.retroAchievementsHash,
            )
            return
        }

        retroAchievementsNetworkMode = RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING
        emulatorSession.updateRetroAchievementsOfflineModeEnabled(true)
        isRetroAchievementsOnlineSessionStarted = false
        logRaTrace(
            "network_transition_offline",
            "hardcore_eligible" to isHardcoreEligibleAfterOnlineStart,
            "started_online" to startedSessionOnlineLive,
            "game_id" to currentRetroAchievementsGameId,
            "content_id" to currentRom?.retroAchievementsHash,
        )

        if (!isHardcoreEligibleAfterOnlineStart) {
            ensureOfflineAccumulationSession(
                unlockMode = OfflineUnlockMode.SOFTCORE,
                offlineType = OfflineUnlockType.OFFLINE_AFTER_START,
            )
        } else if (hardcoreSubmissionQueue.pendingCount() > 0) {
            _toastEvent.tryEmit(ToastEvent.HardcoreOfflineUnsyncedWarning(hardcoreSubmissionQueue.pendingCount()))
        }
    }

    private suspend fun ensureOfflineAccumulationSession(
        unlockMode: OfflineUnlockMode,
        offlineType: OfflineUnlockType,
    ): OfflineRetroAchievementsSession? {
        if (unlockMode == OfflineUnlockMode.SOFTCORE && !settingsRepository.isRetroAchievementsOfflineSoftcoreEnabled()) {
            return null
        }

        val existing = offlineRetroAchievementsSession
        if (existing != null) {
            return existing
        }

        val userAuth = retroAchievementsRepository.getUserAuthentication() ?: return null
        val rom = currentRom ?: return null
        val gameId = currentRetroAchievementsGameId ?: run {
            val offlineContext = buildOfflineRetroAchievementsContext(rom)
            if (offlineContext?.missingCache == true) {
                return null
            }
            offlineContext?.cache?.gameId
        } ?: return null

        val startedAtEpochMs = System.currentTimeMillis()
        val sessionId = UUID.randomUUID().toString()
        val created = OfflineRetroAchievementsSession(
            userId = userAuth.username,
            contentId = rom.retroAchievementsHash,
            gameId = gameId,
            unlockMode = unlockMode,
            offlineType = offlineType,
            sessionId = sessionId,
            startedAtEpochMs = startedAtEpochMs,
            nextOrderIndex = 0L,
        )

        val appendResult = withContext(Dispatchers.IO) {
            offlineLedgerRepository.appendSessionStart(
                userId = created.userId,
                contentId = created.contentId,
                gameId = created.gameId,
                sessionId = created.sessionId,
                startedAtEpochMs = startedAtEpochMs,
                isHardcore = unlockMode == OfflineUnlockMode.HARDCORE,
                unlockMode = unlockMode,
                offlineType = offlineType,
            )
        }

        if (appendResult.isFailure) {
            return null
        }

        offlineRetroAchievementsSession = created
        return created
    }

    private suspend fun handleHardcoreAchievementTriggered(achievement: me.magnum.rcheevosapi.model.RAAchievement) {
        if (networkStatusProvider.isLikelyOnline()) {
            hardcoreSubmissionQueue.drain()
        }

        logRaTrace(
            "hardcore_award_attempt",
            "achievement_id" to achievement.id,
            "game_id" to currentRetroAchievementsGameId,
            "online" to networkStatusProvider.isLikelyOnline(),
        )

        val awardResult = retroAchievementsRepository.awardAchievement(achievement, forHardcoreMode = true)
        _achievementsEvent.emit(RAEventUi.AchievementTriggered(achievement))

        if (awardResult.isSuccess) {
            logRaTrace(
                "hardcore_award_success",
                "achievement_id" to achievement.id,
                "awarded" to (awardResult.getOrNull()?.achievementAwarded ?: false),
            )
            completeAchievementSubmissionTrace(achievement.id, "submit_success")
            hardcoreSubmissionQueue.drain()
        } else {
            logRaTrace(
                "hardcore_award_failed",
                "achievement_id" to achievement.id,
                "error" to (awardResult.exceptionOrNull()?.message ?: "unknown"),
            )
            hardcoreSubmissionQueue.add(achievement)
            _achievementsEvent.emit(RAEventUi.AchievementTriggerError(achievement))
            completeAchievementSubmissionTrace(achievement.id, "submit_failed_queued")
        }
    }

    private suspend fun handleOfflineAchievementTriggered(achievementId: Long, achievement: me.magnum.rcheevosapi.model.RAAchievement?) {
        if (isHardcoreEligibleAfterOnlineStart && achievement != null) {
            if (networkStatusProvider.isLikelyOnline()) {
                handleHardcoreAchievementTriggered(achievement)
                return
            }

            hardcoreSubmissionQueue.add(achievement)
            logRaTrace(
                "hardcore_unlock_queued_in_memory",
                "achievement_id" to achievementId,
                "online" to networkStatusProvider.isLikelyOnline(),
            )
            _achievementsEvent.emit(RAEventUi.AchievementTriggered(achievement))
            return
        }

        val offlineSession = offlineRetroAchievementsSession ?: run {
            val offlineType = if (startedSessionOnlineLive) {
                OfflineUnlockType.OFFLINE_AFTER_START
            } else {
                OfflineUnlockType.OFFLINE_FROM_START
            }
            ensureOfflineAccumulationSession(unlockMode = OfflineUnlockMode.SOFTCORE, offlineType = offlineType)
        }
        if (offlineSession != null) {
            val now = System.currentTimeMillis()
            val offsetMs = (now - offlineSession.startedAtEpochMs).coerceAtLeast(0L)
            val orderIndex = offlineSession.nextOrderIndex
            offlineSession.nextOrderIndex = orderIndex + 1L
            logRaTrace(
                "offline_ledger_append_attempt",
                "achievement_id" to achievementId,
                "unlock_mode" to offlineSession.unlockMode.name,
                "offline_type" to offlineSession.offlineType.name,
                "order_index" to orderIndex,
                "offset_ms" to offsetMs,
                "game_id" to offlineSession.gameId,
                "content_id" to offlineSession.contentId,
            )
            logRaSubmission(
                "offline_ledger_append_start",
                "achievement_id" to achievementId,
                "game_id" to offlineSession.gameId,
                "game_hash" to offlineSession.contentId,
                "session_id" to offlineSession.sessionId,
                "unlock_mode" to offlineSession.unlockMode.name,
                "offline_type" to offlineSession.offlineType.name,
                "order_index" to orderIndex,
                "offset_ms" to offsetMs,
                "pending_sync" to true,
            )

            val appendResult = runCatching {
                withContext(Dispatchers.IO) {
                    retroAchievementsDao.addUserAchievement(
                        RAUserAchievementEntity(
                            gameId = offlineSession.gameId,
                            achievementId = achievementId,
                            isUnlocked = true,
                            isHardcore = false,
                        )
                    )

                    offlineLedgerRepository.appendAchievementUnlock(
                        userId = offlineSession.userId,
                        contentId = offlineSession.contentId,
                        gameId = offlineSession.gameId,
                        achievementId = achievementId,
                        isHardcore = false,
                        sessionId = offlineSession.sessionId,
                        localTimestampEpochMs = now,
                        offsetFromSessionStartMs = offsetMs,
                        orderIndex = orderIndex,
                        unlockMode = OfflineUnlockMode.SOFTCORE,
                        offlineType = offlineSession.offlineType,
                    ).getOrThrow()
                }
            }
            appendResult.onFailure { error ->
                logRaSubmission(
                    "offline_ledger_append_failed",
                    "achievement_id" to achievementId,
                    "game_id" to offlineSession.gameId,
                    "game_hash" to offlineSession.contentId,
                    "session_id" to offlineSession.sessionId,
                    "error" to (error.message ?: error.javaClass.simpleName),
                )
                return
            }
            logRaTrace(
                "offline_ledger_append_success",
                "achievement_id" to achievementId,
                "unlock_mode" to offlineSession.unlockMode.name,
                "offline_type" to offlineSession.offlineType.name,
                "order_index" to orderIndex,
            )
            logRaSubmission(
                "offline_ledger_append_success",
                "achievement_id" to achievementId,
                "game_id" to offlineSession.gameId,
                "game_hash" to offlineSession.contentId,
                "session_id" to offlineSession.sessionId,
                "order_index" to orderIndex,
                "pending_sync" to true,
            )
        }

        if (achievement != null) {
            _achievementsEvent.emit(RAEventUi.AchievementTriggered(achievement))
        }
    }

    private fun onAchievementPrimed(achievementId: Long) {
        if (settingsRepository.areRetroAchievementsActiveChallengeIndicatorsEnabled()) {
            sessionCoroutineScope.launch {
                retroAchievementsRepository.getAchievement(achievementId).onSuccess { achievement ->
                    if (achievement != null) {
                        _achievementsEvent.emit(RAEventUi.AchievementPrimed(achievement))
                    }
                }
            }
        }
    }

    private fun onAchievementUnPrimed(achievementId: Long) {
        sessionCoroutineScope.launch {
            retroAchievementsRepository.getAchievement(achievementId).onSuccess { achievement ->
                if (achievement != null) {
                    _achievementsEvent.emit(RAEventUi.AchievementUnPrimed(achievement))
                }
            }
        }
    }

    private fun onAchievementProgressUpdated(progressEvent: RAEvent.OnAchievementProgressUpdated) {
        if (settingsRepository.areRetroAchievementsProgressIndicatorsEnabled()) {
            sessionCoroutineScope.launch {
                retroAchievementsRepository.getAchievement(progressEvent.achievementId).onSuccess { achievement ->
                    if (achievement != null) {
                        _achievementsEvent.emit(RAEventUi.AchievementProgressUpdated(achievement, progressEvent.current, progressEvent.target, progressEvent.progress))
                    }
                }
            }
        }
    }

    private fun onAchievementProgressHidden(achievementId: Long) {
        sessionCoroutineScope.launch {
            _achievementsEvent.emit(RAEventUi.AchievementProgressHidden(achievementId))
        }
    }

    private fun onLeaderboardTrackerHidden(leaderboardId: Long) {
        sessionCoroutineScope.launch {
            _achievementsEvent.emit(RAEventUi.LeaderboardTrackerHidden(leaderboardId))
        }
    }

    private fun onSetCompleted(subsetId: Long) {
        sessionCoroutineScope.launch {
            showSetMastery(RASetId(subsetId), emulatorSession.isRetroAchievementsHardcoreModeEnabled)
        }
    }

    private fun onRuntimeServerError(event: RAEvent.OnServerError) {
        logRaSubmission(
            "runtime_server_error",
            "api" to event.api,
            "related_id" to event.relatedId,
            "result_code" to event.resultCode,
            "message" to event.message,
        )
        logRaTrace(
            "runtime_server_error",
            "api" to event.api,
            "related_id" to event.relatedId,
            "result_code" to event.resultCode,
            "message" to event.message,
        )

        if (event.api.equals("awardachievement", ignoreCase = true) && event.relatedId > 0L) {
            val achievementId = event.relatedId
            sessionCoroutineScope.launch {
                val achievement = retroAchievementsRepository.getAchievement(achievementId).getOrNull()
                    ?: return@launch
                val isHardcore = emulatorSession.isRetroAchievementsHardcoreModeEnabled
                if (isHardcore) {
                    hardcoreSubmissionQueue.add(achievement)
                    logRaTrace(
                        "rc_client_submit_failed_queued_hardcore",
                        "achievement_id" to achievementId,
                    )
                } else {
                    persistFailedSoftcoreAwardToLedger(achievement)
                }
            }
        }
    }

    private suspend fun persistFailedSoftcoreAwardToLedger(achievement: RAAchievement) {
        if (!settingsRepository.isRetroAchievementsOfflineSoftcoreEnabled()) {
            return
        }

        val rom = currentRom ?: return
        val userAuth = retroAchievementsRepository.getUserAuthentication() ?: return
        val gameId = currentRetroAchievementsGameId ?: achievement.gameId.id
        val sessionId = offlineRetroAchievementsSession?.sessionId ?: java.util.UUID.randomUUID().toString()
        val now = System.currentTimeMillis()
        val orderIndex = offlineRetroAchievementsSession?.let {
            val idx = it.nextOrderIndex
            it.nextOrderIndex = idx + 1L
            idx
        } ?: 0L
        val sessionStart = offlineRetroAchievementsSession?.startedAtEpochMs ?: now
        val offsetMs = (now - sessionStart).coerceAtLeast(0L)

        withContext(Dispatchers.IO) {
            retroAchievementsDao.addUserAchievement(
                RAUserAchievementEntity(
                    gameId = gameId,
                    achievementId = achievement.id,
                    isUnlocked = true,
                    isHardcore = false,
                )
            )
            offlineLedgerRepository.appendAchievementUnlock(
                userId = userAuth.username,
                contentId = rom.retroAchievementsHash,
                gameId = gameId,
                achievementId = achievement.id,
                isHardcore = false,
                sessionId = sessionId,
                localTimestampEpochMs = now,
                offsetFromSessionStartMs = offsetMs,
                orderIndex = orderIndex,
                unlockMode = OfflineUnlockMode.SOFTCORE,
                offlineType = OfflineUnlockType.OFFLINE_AFTER_START,
            )
        }
        logRaTrace(
            "rc_client_submit_failed_queued_softcore",
            "achievement_id" to achievement.id,
            "game_id" to gameId,
        )
    }

    private fun onRuntimeDisconnected() {
        logRaTrace("runtime_disconnected")
    }

    private fun onRuntimeReconnected() {
        logRaTrace("runtime_reconnected")
    }

    private suspend fun ensureAchievementSubmitContext(achievement: RAAchievement): Boolean {
        if (retroAchievementsNetworkMode != RetroAchievementsNetworkMode.ONLINE_LIVE) {
            logContextMismatch("achievement", achievement.id, "network_mode_offline", "achievement_game_id" to achievement.gameId.id)
            return false
        }

        if (!awaitOnlineSessionStart(timeoutMs = 30_000L)) {
            logRaTrace(
                "runtime_session_not_started_proceeding",
                "entity_type" to "achievement",
                "achievement_id" to achievement.id,
                "achievement_game_id" to achievement.gameId.id,
            )
        }

        val runtimeConfig = activeRuntimeBridgeConfig
        if (runtimeConfig == null) {
            logContextMismatch("achievement", achievement.id, "missing_runtime_config", "achievement_game_id" to achievement.gameId.id)
            return false
        }

        val currentHash = currentRom?.retroAchievementsHash
        if (currentHash.isNullOrBlank() || runtimeConfig.gameHash.isNullOrBlank() || currentHash != runtimeConfig.gameHash) {
            logContextMismatch(
                "achievement",
                achievement.id,
                "game_hash_mismatch",
                "achievement_game_id" to achievement.gameId.id,
                "runtime_game_hash" to runtimeConfig.gameHash,
                "current_game_hash" to currentHash,
            )
            return false
        }

        val expectedGameId = currentRetroAchievementsGameId ?: runtimeConfig.gameId
        if (expectedGameId == null || expectedGameId != achievement.gameId.id || (runtimeConfig.gameId != null && runtimeConfig.gameId != achievement.gameId.id)) {
            logContextMismatch(
                "achievement",
                achievement.id,
                "game_id_mismatch",
                "achievement_game_id" to achievement.gameId.id,
                "runtime_game_id" to runtimeConfig.gameId,
                "current_game_id" to currentRetroAchievementsGameId,
            )
            return false
        }

        val isHardcoreModeEnabled = emulatorSession.isRetroAchievementsHardcoreModeEnabled
        if (runtimeConfig.hardcoreEnabled != isHardcoreModeEnabled) {
            logContextMismatch(
                "achievement",
                achievement.id,
                "hardcore_mismatch",
                "achievement_game_id" to achievement.gameId.id,
                "runtime_hardcore" to runtimeConfig.hardcoreEnabled,
                "session_hardcore" to isHardcoreModeEnabled,
            )
            return false
        }

        if (!runtimeConfig.unofficialEnabled && achievement.type == RAAchievement.Type.UNOFFICIAL) {
            logContextMismatch(
                "achievement",
                achievement.id,
                "unofficial_disabled",
                "achievement_game_id" to achievement.gameId.id,
                "encore_enabled" to runtimeConfig.encoreEnabled,
            )
            return false
        }

        return true
    }

    private suspend fun ensureLeaderboardSubmitContext(leaderboardId: Long): Boolean {
        if (retroAchievementsNetworkMode != RetroAchievementsNetworkMode.ONLINE_LIVE) {
            logContextMismatch("leaderboard", leaderboardId, "network_mode_offline")
            return false
        }

        if (!awaitOnlineSessionStart(timeoutMs = 30_000L)) {
            logRaTrace(
                "runtime_session_not_started_proceeding",
                "entity_type" to "leaderboard",
                "leaderboard_id" to leaderboardId,
            )
        }

        val runtimeConfig = activeRuntimeBridgeConfig
        if (runtimeConfig == null) {
            logContextMismatch("leaderboard", leaderboardId, "missing_runtime_config")
            return false
        }

        val leaderboard = retroAchievementsRepository.getLeaderboard(leaderboardId)
        if (leaderboard == null) {
            logContextMismatch("leaderboard", leaderboardId, "missing_leaderboard")
            return false
        }

        val currentHash = currentRom?.retroAchievementsHash
        if (currentHash.isNullOrBlank() || runtimeConfig.gameHash.isNullOrBlank() || currentHash != runtimeConfig.gameHash) {
            logContextMismatch(
                "leaderboard",
                leaderboardId,
                "game_hash_mismatch",
                "runtime_game_hash" to runtimeConfig.gameHash,
                "current_game_hash" to currentHash,
                "leaderboard_game_id" to leaderboard.gameId.id,
            )
            return false
        }

        val expectedGameId = currentRetroAchievementsGameId ?: runtimeConfig.gameId
        if (expectedGameId == null || expectedGameId != leaderboard.gameId.id || (runtimeConfig.gameId != null && runtimeConfig.gameId != leaderboard.gameId.id)) {
            logContextMismatch(
                "leaderboard",
                leaderboardId,
                "game_id_mismatch",
                "leaderboard_game_id" to leaderboard.gameId.id,
                "runtime_game_id" to runtimeConfig.gameId,
                "current_game_id" to currentRetroAchievementsGameId,
            )
            return false
        }

        if (runtimeConfig.hardcoreEnabled != emulatorSession.isRetroAchievementsHardcoreModeEnabled) {
            logContextMismatch(
                "leaderboard",
                leaderboardId,
                "hardcore_mismatch",
                "runtime_hardcore" to runtimeConfig.hardcoreEnabled,
                "session_hardcore" to emulatorSession.isRetroAchievementsHardcoreModeEnabled,
            )
            return false
        }

        return true
    }

    private fun logContextMismatch(
        entityType: String,
        entityId: Long,
        reason: String,
        vararg fields: Pair<String, Any?>,
    ) {
        logRaTrace(
            "context_mismatch",
            "entity_type" to entityType,
            "entity_id" to entityId,
            "reason" to reason,
            "submit_path" to "kotlin_api",
            *fields,
        )
    }

    private suspend fun awaitOnlineSessionStart(timeoutMs: Long = 5_000L): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (!isRetroAchievementsOnlineSessionStarted && System.currentTimeMillis() < deadline) {
            delay(250.milliseconds)
        }
        return isRetroAchievementsOnlineSessionStarted
    }

    private fun completeAchievementSubmissionTrace(achievementId: Long, result: String) {
        val startedAt = pendingRuntimeAchievementTriggers.remove(achievementId)
        if (startedAt == null) {
            logRaTrace(
                "runtime_submit_orphan",
                "entity_type" to "achievement",
                "entity_id" to achievementId,
                "result" to result,
                "submit_path" to "kotlin_api",
            )
            return
        }

        logRaTrace(
            "runtime_submit_resolved",
            "entity_type" to "achievement",
            "entity_id" to achievementId,
            "result" to result,
            "latency_ms" to (System.currentTimeMillis() - startedAt).coerceAtLeast(0L),
            "submit_path" to "kotlin_api",
        )
    }

    private fun completeLeaderboardSubmissionTrace(leaderboardId: Long, result: String) {
        val startedAt = pendingRuntimeLeaderboardCompletions.remove(leaderboardId)
        if (startedAt == null) {
            logRaTrace(
                "runtime_submit_orphan",
                "entity_type" to "leaderboard",
                "entity_id" to leaderboardId,
                "result" to result,
                "submit_path" to "kotlin_api",
            )
            return
        }

        logRaTrace(
            "runtime_submit_resolved",
            "entity_type" to "leaderboard",
            "entity_id" to leaderboardId,
            "result" to result,
            "latency_ms" to (System.currentTimeMillis() - startedAt).coerceAtLeast(0L),
            "submit_path" to "kotlin_api",
        )
    }

    private fun onLeaderboardAttemptStarted(startEvent: RAEvent.OnLeaderboardAttemptStarted) {
        sessionCoroutineScope.launch {
            if (settingsRepository.areRetroAchievementsLeaderboardIndicatorsEnabled()) {
                val leaderboard = retroAchievementsRepository.getLeaderboard(startEvent.leaderboardId)
                if (leaderboard != null) {
                    val setSummary = retroAchievementsRepository.getAchievementSetSummary(leaderboard.setId)
                    if (setSummary != null) {
                        _achievementsEvent.emit(RAEventUi.LeaderboardAttemptStarted(leaderboard, setSummary.iconUrl))
                    }
                }
            }
        }
    }

    private fun onLeaderboardAttemptUpdated(updateEvent: RAEvent.OnLeaderboardAttemptUpdated) {
        sessionCoroutineScope.launch {
            if (settingsRepository.areRetroAchievementsLeaderboardIndicatorsEnabled()) {
                _achievementsEvent.emit(RAEventUi.LeaderboardAttemptUpdated(updateEvent.leaderboardId, updateEvent.formattedValue))
            }
        }
    }

    private fun onLeaderboardAttemptCompleted(completionEvent: RAEvent.OnLeaderboardAttemptCompleted) {
        sessionCoroutineScope.launch {
            logRaTrace(
                "leaderboard_complete_received",
                "leaderboard_id" to completionEvent.leaderboardId,
                "value" to completionEvent.value,
                "network_mode" to retroAchievementsNetworkMode.name,
                "session_mode" to retroAchievementsSessionMode.name,
                "online" to networkStatusProvider.isOnline(),
            )
            if (retroAchievementsNetworkMode == RetroAchievementsNetworkMode.ONLINE_LIVE && !networkStatusProvider.isLikelyOnline()) {
                transitionToOfflineAccumulationIfNeeded()
            }

            if (retroAchievementsNetworkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING) {
                // Offline mode: avoid server submission (no Smart Sync support for leaderboards in this POC).
                logRaTrace(
                    "leaderboard_submit_skipped_offline",
                    "leaderboard_id" to completionEvent.leaderboardId,
                )
                completeLeaderboardSubmissionTrace(completionEvent.leaderboardId, "offline_skipped")
                _achievementsEvent.emit(RAEventUi.LeaderboardAttemptCancelled(completionEvent.leaderboardId))
                return@launch
            }

            if (!emulatorSession.areLeaderboardsEnabled()) {
                logRaTrace(
                    "leaderboard_submit_skipped_mode",
                    "leaderboard_id" to completionEvent.leaderboardId,
                    "hardcore_enabled" to emulatorSession.isRetroAchievementsHardcoreModeEnabled,
                )
                completeLeaderboardSubmissionTrace(completionEvent.leaderboardId, "mode_skipped")
                _achievementsEvent.emit(RAEventUi.LeaderboardAttemptCancelled(completionEvent.leaderboardId))
                return@launch
            }

            if (activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT) {
                logRaTrace(
                    "leaderboard_submit_owned_by_rc_client",
                    "leaderboard_id" to completionEvent.leaderboardId,
                    "value" to completionEvent.value,
                )
                retroAchievementsRepository.getLeaderboard(completionEvent.leaderboardId)?.let { leaderboard ->
                    retroAchievementsRepository.getAchievementSetSummary(leaderboard.setId)?.let { setSummary ->
                        _achievementsEvent.emit(
                            RAEventUi.LeaderboardEntrySubmitted(
                                leaderboardId = leaderboard.id,
                                title = leaderboard.title,
                                gameIcon = setSummary.iconUrl,
                                formattedScore = completionEvent.formattedValue,
                                rank = 0,
                                numberOfEntries = 0,
                            )
                        )
                    }
                }
                completeLeaderboardSubmissionTrace(completionEvent.leaderboardId, "submitted_by_rc_client")
                return@launch
            }

            if (!ensureLeaderboardSubmitContext(completionEvent.leaderboardId)) {
                completeLeaderboardSubmissionTrace(completionEvent.leaderboardId, "context_mismatch")
                _achievementsEvent.emit(RAEventUi.LeaderboardAttemptCancelled(completionEvent.leaderboardId))
                return@launch
            }

            if (emulatorSession.isRetroAchievementsHardcoreModeEnabled) {
                attemptSilentHardcoreReplayBeforeOnlineSubmission()
            }

            retroAchievementsRepository.getLeaderboard(completionEvent.leaderboardId)?.let { leaderboard ->
                retroAchievementsSubmissionHandler.addPendingLeaderboardSubmission(
                    leaderboard = leaderboard,
                    value = completionEvent.value,
                    formattedValue = completionEvent.formattedValue,
                )
            }
        }
    }

    private suspend fun attemptSilentHardcoreReplayBeforeOnlineSubmission() {
        if (!networkStatusProvider.isLikelyOnline()) return
        if (hardcoreSubmissionQueue.pendingCount() == 0) return

        val pendingBefore = hardcoreSubmissionQueue.pendingCount()
        logRaTrace(
            "hardcore_silent_replay_attempt",
            "pending_hardcore" to pendingBefore,
            "content_id" to currentRom?.retroAchievementsHash,
        )

        val drainResult = hardcoreSubmissionQueue.drain()
        if (drainResult.remainingCount == 0) {
            logRaTrace(
                "hardcore_silent_replay_complete",
                "submitted" to drainResult.submittedCount,
            )
        } else {
            logRaTrace(
                "hardcore_silent_replay_partial",
                "submitted" to drainResult.submittedCount,
                "remaining" to drainResult.remainingCount,
            )
        }
    }

    private fun onLeaderboardAttemptCancelled(cancelEvent: RAEvent.OnLeaderboardAttemptCancelled) {
        sessionCoroutineScope.launch {
            _achievementsEvent.emit(RAEventUi.LeaderboardAttemptCancelled(cancelEvent.leaderboardId))
        }
    }

    private suspend fun showSetMastery(setId: RASetId, forHardcoreMode: Boolean) {
        val announcementKey = setId.id to forHardcoreMode
        if (!announcedMasteryKeys.add(announcementKey)) {
            return
        }

        val rom = (emulatorSession.currentSessionType() as? EmulatorSession.SessionType.RomSession)?.rom
        if (rom == null) {
            announcedMasteryKeys.remove(announcementKey)
            return
        }

        val setSummary = retroAchievementsRepository.getAchievementSetSummary(setId)
        val raUserName = (retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated)?.username
        val romPlayTime = romsRepository.getRomAtUri(rom.uri)?.totalPlayTime

        if (setSummary == null) {
            announcedMasteryKeys.remove(announcementKey)
            return
        }

        val title = if (setSummary.type == RAAchievementSet.Type.Core) {
            val gameSummary = retroAchievementsRepository.getGameSummary(rom.retroAchievementsHash)
            gameSummary?.title.orEmpty()
        } else {
            setSummary.title.orEmpty()
        }

        val masteryEvent = RAEventUi.GameMastered(
            gameTitle = title,
            gameIcon = setSummary.iconUrl,
            userName = raUserName,
            playTime = romPlayTime,
            forHardcodeMode = forHardcoreMode,
        )
        _achievementsEvent.emit(masteryEvent)
    }

    private fun startRetroAchievementsSession(rom: Rom, launchDecision: RetroAchievementsLaunchDecision): CompletableDeferred<Unit> {
        val bootstrapReady = CompletableDeferred<Unit>()
        sessionCoroutineScope.launch {
            try {
                offlineRetroAchievementsSession = null
                activeRuntimeBridgeConfig = null
                activeRuntimePath = RetroAchievementsRuntimePath.DISABLED

                val networkMode = launchDecision.networkMode
                val (offlineContext, onlineBootstrap) = run {
                    val ctx = if (networkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING) {
                        buildOfflineRetroAchievementsContext(rom)
                    } else {
                        null
                    }
                    val bootstrap = if (networkMode == RetroAchievementsNetworkMode.ONLINE_LIVE) {
                        withContext(Dispatchers.IO) { getRomAchievementData(rom) }
                    } else {
                        null
                    }
                    ctx to bootstrap
                }

                val achievementData = when (networkMode) {
                    RetroAchievementsNetworkMode.ONLINE_LIVE -> onlineBootstrap?.achievementData
                        ?: GameAchievementData.withDisabledRetroAchievementsIntegration(GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR)
                    RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING -> offlineContext?.achievementData
                        ?: GameAchievementData.withDisabledRetroAchievementsIntegration(GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR)
                }

                emulatorSession.updateRetroAchievementsIntegrationStatus(achievementData.retroAchievementsIntegrationStatus)
                if (!achievementData.isRetroAchievementsIntegrationEnabled) {
                    if (networkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING && offlineContext?.missingCache == true) {
                        _raIntegrationEvent.tryEmit(RAIntegrationEvent.OfflineDisabledNoCache(achievementData.icon))
                    } else if (achievementData.retroAchievementsIntegrationStatus == GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR) {
                        _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(achievementData.icon))
                    } else if (achievementData.retroAchievementsIntegrationStatus == GameAchievementData.IntegrationStatus.DISABLED_GAME_NOT_FOUND) {
                        _raIntegrationEvent.tryEmit(RAIntegrationEvent.LoadedNoAchievements(achievementData.icon))
                    } else if (achievementData.retroAchievementsIntegrationStatus == GameAchievementData.IntegrationStatus.DISABLED_LOGIN_EXPIRED) {
                        _raIntegrationEvent.tryEmit(RAIntegrationEvent.LoginExpired(achievementData.icon))
                    }

                    bootstrapReady.complete(Unit)
                    return@launch
                }

                raSessionJob = launch {
                    // Wait until the emulator has actually started
                    val expectedRuntimePath = when (networkMode) {
                        RetroAchievementsNetworkMode.ONLINE_LIVE -> RetroAchievementsRuntimePath.RC_CLIENT
                        RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING -> RetroAchievementsRuntimePath.RC_CLIENT_OFFLINE
                    }
                    logRaRuntimeSetup(
                        stage = "waiting_for_running",
                        runtimePath = expectedRuntimePath,
                        achievementData = achievementData,
                    )
                    ensureEmulatorIsRunning().firstOrNull()

                    when (networkMode) {
                        RetroAchievementsNetworkMode.ONLINE_LIVE -> {
                            val runtimeConfig = buildOnlineRuntimeConfig(rom, launchDecision)
                            if (runtimeConfig?.runtimeMode != RARuntimeBridgeMode.RC_CLIENT_ONLINE) {
                                activeRuntimeBridgeConfig = null
                                activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                                logRaTrace(
                                    "ra_setup_failed",
                                    "runtime_path" to activeRuntimePath.name,
                                    "error" to "missing_rc_client_config",
                                )
                                emulatorSession.updateRetroAchievementsIntegrationStatus(GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR)
                                _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(achievementData.icon))
                                return@launch
                            }

                            activeRuntimeBridgeConfig = runtimeConfig
                            activeRuntimePath = RetroAchievementsRuntimePath.RC_CLIENT
                            logRaRuntimeSetup(
                                stage = "running_state_ready",
                                runtimePath = activeRuntimePath,
                                achievementData = achievementData,
                                runtimeConfig = runtimeConfig,
                            )
                            logRaTrace(
                                "ra_setup_started",
                                "runtime_path" to activeRuntimePath.name,
                                "encore" to settingsRepository.isRetroAchievementsEncoreModeEnabled(),
                                "hardcore" to (launchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE),
                                "unofficial" to settingsRepository.areRetroAchievementsUnofficialAchievementsEnabled(),
                                "game_id" to currentRetroAchievementsGameId,
                            )

                            runCatching {
                                logRaRuntimeSetup(
                                    stage = "native_setup_start",
                                    runtimePath = activeRuntimePath,
                                    achievementData = achievementData,
                                    runtimeConfig = runtimeConfig,
                                )
                                emulatorManager.setupRetroAchievements(achievementData, runtimeConfig)
                            }.onFailure { throwable ->
                                logRaRuntimeSetup(
                                    stage = "native_setup_failed",
                                    runtimePath = activeRuntimePath,
                                    achievementData = achievementData,
                                    runtimeConfig = runtimeConfig,
                                    throwable = throwable,
                                )
                                logRaTrace(
                                    "ra_setup_failed",
                                    "runtime_path" to activeRuntimePath.name,
                                    "error" to (throwable.message ?: throwable.javaClass.simpleName),
                                )
                                emulatorSession.updateRetroAchievementsIntegrationStatus(GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR)
                                _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(achievementData.icon))
                                return@launch
                            }
                            emulatorSession.updateRetroAchievementsOfflineModeEnabled(false)
                            logRaRuntimeSetup(
                                stage = "native_setup_completed",
                                runtimePath = activeRuntimePath,
                                achievementData = achievementData,
                                runtimeConfig = runtimeConfig,
                            )
                            logRaTrace("ra_setup_completed", "runtime_path" to activeRuntimePath.name)
                            launch {
                                retroAchievementsSubmissionHandler.startEmulatorSession().collect { event ->
                                    when (event) {
                                        is RAEventUi.AchievementTriggered -> {
                                            logRaTrace(
                                                "achievement_submit_success",
                                                "achievement_id" to event.achievement.id,
                                                "hardcore" to emulatorSession.isRetroAchievementsHardcoreModeEnabled,
                                                "awarded" to true,
                                            )
                                            completeAchievementSubmissionTrace(event.achievement.id, "submit_success")
                                        }
                                        is RAEventUi.LeaderboardEntrySubmitted -> {
                                            logRaTrace(
                                                "leaderboard_submit_success",
                                                "leaderboard_id" to event.leaderboardId,
                                                "rank" to event.rank,
                                            )
                                            completeLeaderboardSubmissionTrace(event.leaderboardId, "submit_success")
                                        }
                                        is RAEventUi.LeaderboardEntrySubmitError -> {
                                            logRaTrace(
                                                "leaderboard_submit_failed",
                                                "leaderboard_id" to event.leaderboardId,
                                                "error" to "RetryQueued",
                                            )
                                        }
                                        is RAEventUi.AchievementTriggerError -> {
                                            logRaTrace(
                                                "achievement_submit_failed",
                                                "achievement_id" to event.achievement.id,
                                                "hardcore" to emulatorSession.isRetroAchievementsHardcoreModeEnabled,
                                                "error" to "RetryQueued",
                                            )
                                        }
                                        else -> Unit
                                    }
                                    _achievementsEvent.emit(event)
                                }
                            }
                            isRetroAchievementsOnlineSessionStarted = false
                            emitRetroAchievementsModeToast(
                                status = if (launchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE) {
                                    ToastEvent.RetroAchievementsModeStatus.HARDCORE
                                } else {
                                    ToastEvent.RetroAchievementsModeStatus.SOFTCORE
                                }
                            )
                            emitRetroAchievementsLoadedPopup(achievementData)

                            while (isActive) {
                                if (retroAchievementsNetworkMode != RetroAchievementsNetworkMode.ONLINE_LIVE) {
                                    break
                                }

                                if (!networkStatusProvider.isLikelyOnline()) {
                                    transitionToOfflineAccumulationIfNeeded()
                                    delay(15.seconds)
                                    continue
                                }

                                if (!isRetroAchievementsOnlineSessionStarted) {
                                    val isHardcoreModeEnabled = launchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE
                                    val startResult = withContext(Dispatchers.IO) {
                                        retroAchievementsRepository.startSession(rom.retroAchievementsHash, isHardcoreModeEnabled)
                                    }
                                    if (startResult.isFailure) {
                                        if (startResult.exceptionOrNull() is UserTokenExpiredException) {
                                            _raIntegrationEvent.tryEmit(RAIntegrationEvent.LoginExpired(achievementData.icon))
                                            break
                                        }
                                        delay(15.seconds)
                                        continue
                                    }

                                    isRetroAchievementsOnlineSessionStarted = true
                                }

                                // TODO: Should we pause the session if the app goes to background? If so, how?
                                delay(2.minutes)
                                val isHardcoreModeEnabled = launchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE
                                if (isHardcoreModeEnabled) {
                                    attemptSilentHardcoreReplayBeforeOnlineSubmission()
                                } else {
                                    retroAchievementsSubmissionHandler.retrySubmissionsImmediately()
                                }

                                val richPresenceDescription = MelonEmulator.getRichPresenceStatus()
                                withContext(Dispatchers.IO) {
                                    retroAchievementsRepository.sendSessionHeartbeat(
                                        rom.retroAchievementsHash,
                                        isHardcoreModeEnabled,
                                        richPresenceDescription,
                                    )
                                }
                            }
                        }
                        RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING -> {
                            val context = offlineContext ?: return@launch
                            currentRetroAchievementsGameId = context.cache.gameId
                            val userAuth = retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated
                            val runtimeConfig = if (userAuth != null) {
                                RARuntimeBridgeConfig(
                                    runtimeMode = RARuntimeBridgeMode.RC_CLIENT_OFFLINE,
                                    userAgent = retroAchievementsUserAgent,
                                    username = userAuth.username,
                                    apiToken = userAuth.token,
                                    gameHash = rom.retroAchievementsHash,
                                    gameId = context.cache.gameId,
                                    hardcoreEnabled = false,
                                    unofficialEnabled = settingsRepository.areRetroAchievementsUnofficialAchievementsEnabled(),
                                    encoreEnabled = settingsRepository.isRetroAchievementsEncoreModeEnabled(),
                                )
                            } else {
                                null
                            }
                            activeRuntimeBridgeConfig = runtimeConfig
                            activeRuntimePath = RetroAchievementsRuntimePath.RC_CLIENT_OFFLINE
                            logRaRuntimeSetup(
                                stage = "running_state_ready",
                                runtimePath = activeRuntimePath,
                                achievementData = achievementData,
                                runtimeConfig = runtimeConfig,
                            )

                            val startedAtEpochMs = System.currentTimeMillis()
                            val sessionId = UUID.randomUUID().toString()
                            val unlockMode = OfflineUnlockMode.SOFTCORE
                            val offlineType = launchDecision.initialOfflineType ?: OfflineUnlockType.OFFLINE_FROM_START

                            offlineRetroAchievementsSession = OfflineRetroAchievementsSession(
                                userId = context.userId,
                                contentId = context.contentId,
                                gameId = context.cache.gameId,
                                unlockMode = unlockMode,
                                offlineType = offlineType,
                                sessionId = sessionId,
                                startedAtEpochMs = startedAtEpochMs,
                                nextOrderIndex = 0L,
                            )

                            withContext(Dispatchers.IO) {
                                offlineLedgerRepository.appendSessionStart(
                                    userId = context.userId,
                                    contentId = context.contentId,
                                    gameId = context.cache.gameId,
                                    sessionId = sessionId,
                                    startedAtEpochMs = startedAtEpochMs,
                                    isHardcore = false,
                                    unlockMode = unlockMode,
                                    offlineType = offlineType,
                                )
                            }

                            runCatching {
                                logRaRuntimeSetup(
                                    stage = "native_setup_start",
                                    runtimePath = activeRuntimePath,
                                    achievementData = achievementData,
                                    runtimeConfig = runtimeConfig,
                                )
                                emulatorManager.setupRetroAchievements(achievementData, runtimeConfig)
                            }.onFailure { throwable ->
                                logRaRuntimeSetup(
                                    stage = "native_setup_failed",
                                    runtimePath = activeRuntimePath,
                                    achievementData = achievementData,
                                    runtimeConfig = runtimeConfig,
                                    throwable = throwable,
                                )
                                logRaTrace(
                                    "ra_setup_failed",
                                    "runtime_path" to activeRuntimePath.name,
                                    "error" to (throwable.message ?: throwable.javaClass.simpleName),
                                )
                                emulatorSession.updateRetroAchievementsIntegrationStatus(GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR)
                                _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(achievementData.icon))
                                return@launch
                            }
                            emulatorSession.updateRetroAchievementsOfflineModeEnabled(true)
                            logRaRuntimeSetup(
                                stage = "native_setup_completed",
                                runtimePath = activeRuntimePath,
                                achievementData = achievementData,
                                runtimeConfig = runtimeConfig,
                            )
                            emitRetroAchievementsModeToast(
                                status = ToastEvent.RetroAchievementsModeStatus.SOFTCORE_OFFLINE,
                                offlineNoInternetAtStart = launchDecision.offlineDueToNoInternetAtStart,
                                hardcoreOfflineDisabled = launchDecision.hardcoreOfflineDisabled,
                            )
                            emitRetroAchievementsLoadedPopup(achievementData)
                        }
                    }
                }
                bootstrapReady.complete(Unit)
            } catch (exception: Throwable) {
                if (exception is CancellationException) {
                    throw exception
                }
                Log.e("EmulatorViewModel", "RetroAchievements bootstrap failed for '${rom.name}'", exception)
                bootstrapReady.complete(Unit)
                markRetroAchievementsLoadStage(null)
                activeRuntimeBridgeConfig = null
                activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                emulatorSession.updateRetroAchievementsIntegrationStatus(GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR)
                _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(null))
            }
        }
        return bootstrapReady
    }

    private fun emitRetroAchievementsModeToast(
        status: ToastEvent.RetroAchievementsModeStatus,
        offlineNoInternetAtStart: Boolean = false,
        hardcoreOfflineDisabled: Boolean = false,
    ) {
        _toastEvent.tryEmit(
            ToastEvent.RetroAchievementsMode(
                status = status,
                offlineNoInternetAtStart = offlineNoInternetAtStart,
                hardcoreOfflineDisabled = hardcoreOfflineDisabled,
            )
        )
    }

    private fun emitRetroAchievementsLoadedPopup(achievementData: GameAchievementData) {
        if (achievementData.hasAchievements) {
            _raIntegrationEvent.tryEmit(
                RAIntegrationEvent.Loaded(
                    icon = achievementData.icon,
                    unlockedAchievements = achievementData.unlockedAchievementCount,
                    totalAchievements = achievementData.totalAchievementCount,
                )
            )
        } else {
            _raIntegrationEvent.tryEmit(RAIntegrationEvent.LoadedNoAchievements(achievementData.icon))
        }
    }

    private data class OfflineRetroAchievementsContext(
        val userId: String,
        val contentId: String,
        val cache: OfflinePrefetchCacheFile,
        val achievementData: GameAchievementData,
        val missingCache: Boolean,
    )

    private suspend fun buildOfflineRetroAchievementsContext(rom: Rom): OfflineRetroAchievementsContext? {
        val userAuth = retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated ?: return null
        val userId = userAuth.username
        val contentId = rom.retroAchievementsHash

        val cache = try {
            withContext(Dispatchers.IO) {
                offlinePrefetchCacheRepository.readValid(userId, contentId)
            }
        } catch (_: Exception) {
            null
        }

        val gameSummary = retroAchievementsRepository.getGameSummary(rom.retroAchievementsHash)

        if (cache == null) {
            return OfflineRetroAchievementsContext(
                userId = userId,
                contentId = contentId,
                cache = OfflinePrefetchCacheFile(),
                achievementData = GameAchievementData.withDisabledRetroAchievementsIntegration(
                    status = GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR,
                    icon = gameSummary?.icon,
                ),
                missingCache = true,
            )
        }

        val isHardcoreModeEnabled = emulatorSession.isRetroAchievementsHardcoreModeEnabled
        val unlockedIds = try {
            withContext(Dispatchers.IO) {
                retroAchievementsDao.getGameUserUnlockedAchievements(cache.gameId, isHardcoreModeEnabled).map { it.achievementId }.toSet()
            }
        } catch (_: Exception) {
            emptySet()
        }

        val lockedAchievements = cache.achievements
            .asSequence()
            .filterNot { unlockedIds.contains(it.id) }
            .map { RASimpleAchievement(it.id, it.memoryAddress) }
            .toList()
        val runtimeLeaderboards = if (emulatorSession.areLeaderboardsEnabled()) {
            cache.leaderboards
                .map { RASimpleLeaderboard(it.id, it.memoryAddress, it.format) }
        } else {
            emptyList()
        }

        val achievementData = if (cache.achievements.isEmpty() && runtimeLeaderboards.isEmpty()) {
            GameAchievementData.withLimitedRetroAchievementsIntegration(
                richPresencePatch = cache.richPresencePatch,
                icon = gameSummary?.icon,
            )
        } else {
            GameAchievementData.withFullRetroAchievementsIntegration(
                lockedAchievements = lockedAchievements,
                leaderboards = runtimeLeaderboards,
                totalAchievementCount = cache.achievements.size,
                richPresencePatch = cache.richPresencePatch,
                icon = gameSummary?.icon,
            )
        }

        return OfflineRetroAchievementsContext(
            userId = userId,
            contentId = contentId,
            cache = cache,
            achievementData = achievementData,
            missingCache = false,
        )
    }

    private fun startTrackingFps() {
        sessionCoroutineScope.launch {
            while (isActive) {
                delay(1.seconds)
                _currentFps.value = emulatorManager.getFps().roundToInt()
            }
        }
    }

    private fun filterRomPauseMenuOption(option: RomPauseMenuOption, rendererDebugToolsEnabled: Boolean): Boolean {
        return when (option) {
            RomPauseMenuOption.ROM_SETTINGS -> _emulatorState.value is EmulatorState.RunningRom
            RomPauseMenuOption.SAVE_STATE -> emulatorSession.areSaveStatesAllowed()
            RomPauseMenuOption.REWIND -> settingsRepository.isRewindEnabled() && emulatorSession.areSaveStateLoadsAllowed()
            RomPauseMenuOption.LOAD_STATE -> emulatorSession.areSaveStateLoadsAllowed()
            RomPauseMenuOption.CHEATS -> emulatorSession.areCheatsEnabled()
            RomPauseMenuOption.VIEW_ACHIEVEMENTS -> emulatorSession.isRetroAchievementsEnabledForSession()
            RomPauseMenuOption.RENDERER_DEBUG -> rendererDebugToolsEnabled
            else -> true
        }
    }

    private fun getInGameRomSettingsOverrides(rom: Rom): InGameRomSettingsOverrides {
        val globalLayoutId = settingsRepository.getSelectedLayoutId()
        return InGameRomSettingsOverrides(
            controllerMapping = rom.config.inputMode != me.magnum.melonds.domain.model.rom.config.RomInputMode.GLOBAL,
            controllerLayout = rom.config.layoutId != null && rom.config.layoutId != globalLayoutId,
            videoFiltering = rom.config.videoFiltering != null,
        )
    }

    private suspend fun buildInGameRomSettingsMenuState(rom: Rom): InGameRomSettingsMenuState {
        val inputModeOptions = context.resources.getStringArray(R.array.rom_input_mode_options)
        val filteringOptions = context.resources.getStringArray(R.array.video_filtering_options)
        val micOptions = context.resources.getStringArray(R.array.game_runtime_mic_source_options)
        val effectiveConfiguration = settingsRepository.getEmulatorConfiguration(rom.config)
        val effectiveVideoFiltering = effectiveConfiguration.rendererConfiguration.videoFiltering
        val globalRetroArchPresetPath = settingsRepository.observeRetroArchShaderPresetPath().firstOrNull()
        val globalRetroArchParameters = settingsRepository.observeRetroArchShaderParametersText().firstOrNull()
        val globalLayoutName = layoutsRepository.getLayout(settingsRepository.getSelectedLayoutId())?.name
            ?: context.getString(R.string.not_set)
        val globalRetroArchPresetPathLabel = globalRetroArchPresetPath ?: context.getString(R.string.not_set)
        val globalRetroArchParametersLabel = globalRetroArchParameters ?: context.getString(R.string.not_set)
        val useGlobalWithValue = { value: String ->
            context.getString(R.string.use_global_preference_with_value, value)
        }
        val effectiveMicSource = RuntimeMicSource.entries.firstOrNull { it.micSource == effectiveConfiguration.micSource }
            ?: RuntimeMicSource.DEFAULT
        val hasValidRetroArchShaderRoot = settingsRepository.observeRetroArchShaderRootValid().firstOrNull() == true
        val showRetroArchSettings = effectiveConfiguration.rendererConfiguration.renderer == VideoRenderer.VULKAN &&
            effectiveVideoFiltering == VideoFiltering.RETROARCH &&
            hasValidRetroArchShaderRoot

        return InGameRomSettingsMenuState(
            controllerMappingValue = if (rom.config.inputMode == me.magnum.melonds.domain.model.rom.config.RomInputMode.GLOBAL) {
                useGlobalWithValue(context.getString(R.string.global_controller_mapping))
            } else {
                inputModeOptions[rom.config.inputMode.ordinal]
            },
            layoutValue = rom.config.layoutId?.let { layoutId ->
                layoutsRepository.getLayout(layoutId)?.name ?: context.getString(R.string.not_set)
            } ?: useGlobalWithValue(globalLayoutName),
            videoFilteringValue = if (rom.config.videoFiltering == null) {
                useGlobalWithValue(filteringOptions[effectiveVideoFiltering.ordinal])
            } else {
                filteringOptions[effectiveVideoFiltering.ordinal]
            },
            showRetroArchSettings = showRetroArchSettings,
            retroArchPresetPathValue = rom.config.retroArchShaderPresetPath ?: useGlobalWithValue(globalRetroArchPresetPathLabel),
            retroArchParametersValue = rom.config.retroArchShaderParameters ?: useGlobalWithValue(globalRetroArchParametersLabel),
            hasValidRetroArchShaderRoot = hasValidRetroArchShaderRoot,
            micSourceValue = if (rom.config.runtimeMicSource == RuntimeMicSource.DEFAULT) {
                useGlobalWithValue(micOptions[effectiveMicSource.ordinal])
            } else {
                micOptions[rom.config.runtimeMicSource.ordinal]
            },
        )
    }

    private fun ensureEmulatorIsRunning(): Flow<Unit> {
        return _emulatorState.filter { it.isRunning() }.take(1).map { }
    }

    private suspend fun startEmulatorSession(
        sessionType: EmulatorSession.SessionType,
        areRetroAchievementsEnabled: Boolean,
        isRetroAchievementsHardcoreModeEnabled: Boolean = settingsRepository.isRetroAchievementsHardcoreEnabled(),
    ) {
        emulatorSession.startSession(
            areRetroAchievementsEnabled = areRetroAchievementsEnabled,
            isRetroAchievementsHardcoreModeEnabled = isRetroAchievementsHardcoreModeEnabled,
            sessionType = sessionType,
        )
    }

    private fun dispatchSessionUpdateActions(actions: List<EmulatorSessionUpdateAction>) {
        actions.forEach {
            when (it) {
                EmulatorSessionUpdateAction.DisableRetroAchievements -> {
                    _achievementsEvent.tryEmit(RAEventUi.Reset)
                    emulatorManager.unloadRetroAchievementsData()
                    raSessionJob?.cancel()
                    raSessionJob = null
                    activeRuntimeBridgeConfig = null
                    activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                    emulatorSession.updateRetroAchievementsOfflineModeEnabled(false)
                    announcedMasteryKeys.clear()
                    pendingRuntimeAchievementTriggers.clear()
                    pendingRuntimeLeaderboardCompletions.clear()
                }
                EmulatorSessionUpdateAction.EnableRetroAchievements -> {
                    (emulatorSession.currentSessionType() as? EmulatorSession.SessionType.RomSession)?.rom?.let { currentRom ->
                        startRetroAchievementsSession(
                            rom = currentRom,
                            launchDecision = RetroAchievementsLaunchDecision(
                                networkMode = retroAchievementsNetworkMode,
                                sessionMode = retroAchievementsSessionMode,
                                initialOfflineType = if (retroAchievementsNetworkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING) {
                                    if (startedSessionOnlineLive) {
                                        OfflineUnlockType.OFFLINE_AFTER_START
                                    } else {
                                        OfflineUnlockType.OFFLINE_FROM_START
                                    }
                                } else {
                                    null
                                },
                                isHardcoreEligibleAfterOnlineStart = isHardcoreEligibleAfterOnlineStart,
                                offlineDueToNoInternetAtStart = !startedSessionOnlineLive && retroAchievementsNetworkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING,
                                hardcoreOfflineDisabled = false,
                            ),
                        )
                    }
                }
                EmulatorSessionUpdateAction.NotifyRetroAchievementsModeSwitch -> {
                    _toastEvent.tryEmit(ToastEvent.CannotSwitchRetroAchievementsMode)
                }
            }
        }
    }

    private fun logRaRuntimeEvent(event: RAEvent) {
        when (event) {
            is RAEvent.OnAchievementTriggered -> {
                pendingRuntimeAchievementTriggers[event.achievementId] = System.currentTimeMillis()
                logRaTrace("runtime_event_achievement_triggered", "achievement_id" to event.achievementId)
            }
            is RAEvent.OnLeaderboardAttemptCompleted -> {
                pendingRuntimeLeaderboardCompletions[event.leaderboardId] = System.currentTimeMillis()
                logRaTrace(
                    "runtime_event_leaderboard_completed",
                    "leaderboard_id" to event.leaderboardId,
                    "value" to event.value,
                )
            }
            is RAEvent.OnGameCompleted -> {
                logRaTrace("runtime_event_game_completed", "subset_id" to event.subsetId)
            }
            is RAEvent.OnSubsetCompleted -> {
                logRaTrace("runtime_event_subset_completed", "subset_id" to event.subsetId)
            }
            is RAEvent.OnServerError -> {
                logRaTrace(
                    "runtime_event_server_error",
                    "api" to event.api,
                    "related_id" to event.relatedId,
                    "result_code" to event.resultCode,
                )
            }
            RAEvent.OnDisconnected -> {
                logRaTrace("runtime_event_disconnected")
            }
            RAEvent.OnReconnected -> {
                logRaTrace("runtime_event_reconnected")
            }
            is RAEvent.OnAchievementPrimed,
            is RAEvent.OnAchievementUnPrimed,
            is RAEvent.OnAchievementProgressUpdated,
            is RAEvent.OnAchievementProgressHidden,
            is RAEvent.OnLeaderboardAttemptStarted,
            is RAEvent.OnLeaderboardAttemptUpdated,
            is RAEvent.OnLeaderboardAttemptCancelled,
            is RAEvent.OnLeaderboardTrackerHidden -> {
                // Keep high-volume runtime events out of structured trace logs.
            }
        }
    }

    private fun logRaTrace(eventType: String, vararg fields: Pair<String, Any?>) {
        if (!isDebugBuild()) {
            return
        }

        val message = buildString {
            append("event_type=").append(eventType)
            append(" network_mode=").append(retroAchievementsNetworkMode.name)
            append(" session_mode=").append(retroAchievementsSessionMode.name)
            append(" game_id=").append(currentRetroAchievementsGameId ?: "none")
            append(" runtime_path=").append(activeRuntimePath.traceValue)
            append(" session_active=").append(currentSessionIsActive())
            fields.forEach { (key, value) ->
                if (value != null) {
                    append(' ')
                    append(key)
                    append('=')
                    append(value.toString().replace(' ', '_'))
                }
            }
        }
        Log.i(RA_TRACE_TAG, message)
    }

    private fun currentSessionIsActive(): Boolean {
        return isRetroAchievementsOnlineSessionStarted || offlineRetroAchievementsSession != null
    }

    private fun isDebugBuild(): Boolean {
        return (context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
    }

    override fun onCleared() {
        super.onCleared()
        sessionCoroutineScope.cancel()
        emulatorManager.cleanEmulator()
    }

    private class EmulatorSessionCoroutineScope : CoroutineScope {
        private var currentCoroutineContext: CoroutineContext = EmptyCoroutineContext

        override val coroutineContext: CoroutineContext get() = currentCoroutineContext

        fun notifyNewSessionStarted() {
            cancel()
            currentCoroutineContext = SupervisorJob() + Dispatchers.Main.immediate
        }

        fun cancel() {
            currentCoroutineContext.cancel()
        }
    }
}
