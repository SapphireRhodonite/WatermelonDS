package me.magnum.melonds.ui.emulator

import me.magnum.melonds.multiplayer.phantom.PhantomPlayer
import me.magnum.melonds.multiplayer.MultiplayerSession
import me.magnum.melonds.multiplayer.MultiplayerRoom

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
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onCompletion
import kotlinx.coroutines.flow.shareIn
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import me.magnum.melonds.MelonDSAndroidInterface
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.R
import me.magnum.melonds.common.romprocessors.RomFileProcessorFactory
import me.magnum.melonds.common.runtime.ScreenshotFrameBufferProvider
import me.magnum.melonds.common.network.RETROACHIEVEMENTS_USER_AGENT
import me.magnum.melonds.common.retroachievements.RetroAchievementsEndpointProvider
import me.magnum.melonds.common.retroachievements.RetroAchievementsEndpointSnapshot
import me.magnum.melonds.common.retroachievements.RetroAchievementsEndpointStorage
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
import me.magnum.melonds.domain.model.retroachievements.PendingRaSubmission
import me.magnum.melonds.domain.model.retroachievements.PendingRaSubmissionSnapshot
import me.magnum.melonds.domain.model.retroachievements.RAEvent
import me.magnum.melonds.domain.model.retroachievements.RARuntimeBridgeConfig
import me.magnum.melonds.domain.model.retroachievements.RARuntimeBridgeMode
import me.magnum.melonds.domain.model.retroachievements.RetroAchievementsOfflineBackend
import me.magnum.melonds.domain.model.retroachievements.RaNativePendingSubmissionResolution
import me.magnum.melonds.domain.model.retroachievements.RaNativePendingSubmissionType
import me.magnum.melonds.domain.model.retroachievements.RaPendingCounts
import me.magnum.melonds.domain.model.retroachievements.RaPendingSubmissionType
import me.magnum.melonds.domain.model.retroachievements.RaSubmissionContext
import me.magnum.melonds.domain.model.retroachievements.RaSubmissionSessionIdGenerator
import me.magnum.melonds.domain.model.retroachievements.RASimpleAchievement
import me.magnum.melonds.domain.model.input.SoftInputBehaviour
import me.magnum.melonds.domain.model.retroachievements.RASimpleLeaderboard
import me.magnum.melonds.domain.model.retroachievements.exception.RAGameNotExist
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomConfig
import me.magnum.melonds.domain.model.rom.config.RuntimeMicSource
import me.magnum.melonds.domain.model.ui.Orientation
import me.magnum.melonds.domain.repositories.BackgroundRepository
import me.magnum.melonds.domain.repositories.CheatsRepository
import me.magnum.melonds.domain.repositories.LayoutsRepository
import me.magnum.melonds.domain.repositories.RetroAchievementsRepository
import me.magnum.melonds.domain.repositories.FrontendRomLookup
import me.magnum.melonds.domain.repositories.RomsRepository
import me.magnum.melonds.domain.repositories.SaveStatesRepository
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.domain.services.EmulatorManager
import me.magnum.melonds.impl.ShaderCompileTimeStore
import me.magnum.melonds.impl.ScreenshotFileManager
import me.magnum.melonds.impl.emulator.AndroidEmulatorManager
import me.magnum.melonds.impl.emulator.EmulatorSession
import me.magnum.melonds.impl.emulator.LeaderboardTrackerUpdateLogLimiter
import me.magnum.melonds.impl.emulator.debug.RendererDebugCaptureLogger
import me.magnum.melonds.impl.network.NetworkConnectivityObserver
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
import me.magnum.melonds.ui.emulator.component.AchievementSubmissionOwnership
import me.magnum.melonds.ui.emulator.component.LeaderboardAttemptCoordinator
import me.magnum.melonds.ui.emulator.component.LeaderboardAttemptKey
import me.magnum.melonds.ui.emulator.component.LeaderboardScoreboardUiMapper
import me.magnum.melonds.ui.emulator.component.LeaderboardSubmissionOwnership
import me.magnum.melonds.ui.emulator.component.PendingRaSubmissionStore
import me.magnum.melonds.ui.emulator.component.RaHardcoreContinuityEvent
import me.magnum.melonds.ui.emulator.component.RaHardcoreContinuityState
import me.magnum.melonds.ui.emulator.component.RaHardcoreContinuityStateMachine
import me.magnum.melonds.ui.emulator.component.RaHardcoreLaunchPolicy
import me.magnum.melonds.ui.emulator.component.RaInGameLogoutCoordinator
import me.magnum.melonds.ui.emulator.component.RaInGameLogoutFailureStage
import me.magnum.melonds.ui.emulator.component.RaInGameLogoutResult
import me.magnum.melonds.ui.emulator.component.RaPendingReconnectGate
import me.magnum.melonds.ui.emulator.component.RaPendingRuntimeDisableGate
import me.magnum.melonds.ui.emulator.component.RaPendingSubmissionSyncCoordinator
import me.magnum.melonds.ui.emulator.component.RaPendingSubmissionUiPolicy
import me.magnum.melonds.ui.emulator.component.RaPendingExitFollowUp
import me.magnum.melonds.ui.emulator.component.RaPendingExitContext
import me.magnum.melonds.ui.emulator.component.RaPendingManualSyncAction
import me.magnum.melonds.ui.emulator.component.RaPendingModalController
import me.magnum.melonds.ui.emulator.component.RaPendingRuntimeOwner
import me.magnum.melonds.ui.emulator.component.RaPendingSyncMenuContext
import me.magnum.melonds.ui.emulator.component.RaPendingSyncResult
import me.magnum.melonds.ui.emulator.component.RaPendingSyncSource
import me.magnum.melonds.ui.emulator.component.RaActiveSubmissionContext
import me.magnum.melonds.ui.emulator.component.RaNativeRetryResultMapper
import me.magnum.melonds.ui.emulator.component.RaSubmissionContextValidator
import me.magnum.melonds.ui.emulator.component.RaSessionStopGate
import me.magnum.melonds.ui.emulator.component.RaRuntimeAuthenticationPolicy
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
import me.magnum.melonds.ui.emulator.model.RaPendingSyncResultAction
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
import me.magnum.rcheevosapi.model.RALeaderboard
import me.magnum.rcheevosapi.model.RASetId
import me.magnum.rcheevosapi.model.RAUserAuth
import java.io.FileInputStream
import java.net.URL
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
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
private const val SAVESTATE_MINOR = 1

private const val RETROACHIEVEMENTS_REFRESH_TIMEOUT_MS = 12_000L
private const val RA_PENDING_BARRIER_TIMEOUT_MS = 3_000L

@OptIn(ExperimentalCoroutinesApi::class)
@HiltViewModel
class EmulatorViewModel @Inject constructor(
    @param:ApplicationContext private val context: Context,
    private val settingsRepository: SettingsRepository,
    private val retroAchievementsEndpointProvider: RetroAchievementsEndpointProvider,
    private val romsRepository: RomsRepository,
    private val cheatsRepository: CheatsRepository,
    private val retroAchievementsRepository: RetroAchievementsRepository,
    private val retroAchievementsDao: RetroAchievementsDao,
    private val offlineLedgerRepository: OfflineLedgerRepository,
    private val offlinePrefetchCacheRepository: OfflinePrefetchCacheRepository,
    private val retroAchievementsImageCacheWarmer: RetroAchievementsImageCacheWarmer,
    private val smartSyncEngine: SmartSyncEngine,
    private val hardcoreOfflineLossTracker: HardcoreOfflineLossTracker,
    private val networkConnectivityObserver: NetworkConnectivityObserver,
    private val networkStatusProvider: NetworkStatusProvider,
    private val romFileProcessorFactory: RomFileProcessorFactory,
    private val layoutsRepository: LayoutsRepository,
    private val backgroundsRepository: BackgroundRepository,
    private val saveStatesRepository: SaveStatesRepository,
    private val screenshotFrameBufferProvider: ScreenshotFrameBufferProvider,
    private val screenshotFileManager: ScreenshotFileManager,
    private val uiLayoutProvider: UILayoutProvider,
    private val emulatorManager: EmulatorManager,
    private val emulatorSession: EmulatorSession,
    private val retroAchievementsSubmissionHandler: RetroAchievementsSubmissionHandler,
    private val shaderCompileTimeStore: ShaderCompileTimeStore,
    savedStateHandle: SavedStateHandle,
) : ViewModel() {

    private val sessionCoroutineScope = EmulatorSessionCoroutineScope()
    private var screenshotJob: Job? = null

    data class HeavyShaderCompileRequest(
        val presetName: String,
        val estimatedMillis: Long,
        val isMeasured: Boolean,
        val response: CompletableDeferred<Boolean>,
    )

    private val heavyShaderCompileThresholdMs = 60_000L

    private val _heavyShaderCompileRequest = MutableSharedFlow<HeavyShaderCompileRequest>(extraBufferCapacity = 1)
    val heavyShaderCompileRequest = _heavyShaderCompileRequest.asSharedFlow()

    private val isRetroArchShaderDeclinedForSession = MutableStateFlow(false)

    private var raBootstrapJob: Job? = null
    private var raSessionJob: Job? = null

    private enum class RetroAchievementsNetworkMode {
        ONLINE_LIVE,
        OFFLINE_ACCUMULATING,
        RECONCILING_RA_SUBMISSIONS,
    }

    private enum class RetroAchievementsSessionMode {
        SOFTCORE,
        HARDCORE,
    }

    private enum class RetroAchievementsRuntimePath(val traceValue: String) {
        DISABLED("disabled"),
        LEGACY("legacy"),
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
        val usesProxyBackend: Boolean = false,
        val nativeClientHost: String = RetroAchievementsEndpointStorage.OFFICIAL_CLIENT_HOST,
        val endpointGeneration: Long = 0L,
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

    private data class LeaderboardUiContext(
        val leaderboard: RALeaderboard,
        val gameIcon: URL,
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
    private val runtimeAuthenticationLeaseMonitor = Any()
    private var activeRuntimeAuthenticationLeaseId: String? = null
    private var activeHardcoreSubmissionSessionId: String? = null
    private var hardcoreSubmissionQueueTeardown: CompletableDeferred<Unit>? = null
    private var leaderboardDiagnosticsEnabled = false
    private var didReceiveRendererInitFailure = false
    private val raSessionStopGate = RaSessionStopGate()
    private val announcedMasteryKeys = mutableSetOf<Pair<Long, Boolean>>()
    private val pendingRuntimeAchievementTriggers = mutableMapOf<Long, Long>()
    private val pendingRuntimeLeaderboardCompletions = mutableMapOf<Long, Long>()
    private val leaderboardAttemptCoordinator = LeaderboardAttemptCoordinator()
    private val leaderboardTrackerUpdateLogLimiter = LeaderboardTrackerUpdateLogLimiter()
    private var pendingRaSubmissionStore: PendingRaSubmissionStore? = null
    private var pendingRaSyncCoordinator: RaPendingSubmissionSyncCoordinator? = null
    private var pendingRaSessionJob: Job? = null
    private var pendingRaReconnectRequests: Channel<Unit>? = null
    private val pendingRaReconnectGate = RaPendingReconnectGate()
    private val pendingRaRuntimeDisableGate = RaPendingRuntimeDisableGate()
    private val pendingRaModalController = RaPendingModalController()
    private val pendingRaSubmissionBarrier = MutableStateFlow<PendingRaSubmissionBarrier?>(null)
    private val settingsReconciliationsInFlight = AtomicInteger(0)
    private val settingsReconciliationMutex = Mutex()
    private val resumeAfterSettingsReconciliation = AtomicBoolean(false)

    private var offlineSyncChoiceDeferred: CompletableDeferred<OfflineAchievementsSyncChoice>? = null
    private var hardcoreExitChoiceWaiter: HardcoreExitChoiceWaiter? = null

    private data class HardcoreExitChoiceWaiter(
        val requestId: Long,
        val deferred: CompletableDeferred<HardcorePendingExitChoice>,
    )

    private data class HardcoreExitChoiceResponse(
        val requestId: Long,
        val choice: HardcorePendingExitChoice,
    )

    private data class PendingRaSubmissionBarrier(
        val submissionSessionId: Long,
        val barrierId: Long,
    )

    private val hardcoreSubmissionQueue = HardcoreSubmissionQueue(
        submitAchievement = { achievement, authentication ->
            retroAchievementsRepository.awardAchievementForAuthentication(
                achievement = achievement,
                forHardcoreMode = true,
                expectedAuthentication = authentication,
            )
        },
        canSubmitForActiveIdentity = { authentication ->
            runtimeAuthenticationSnapshot() == authentication &&
                activeRuntimeAuthenticationMatches()
        },
    )

    private suspend fun addHardcoreSubmission(
        achievement: RAAchievement,
        authentication: RAUserAuth.Authenticated,
    ): Boolean {
        val sessionId = currentHardcoreSubmissionSessionId() ?: return false
        return hardcoreSubmissionQueue.add(sessionId, achievement, authentication)
    }

    private suspend fun drainHardcoreSubmissions(): HardcoreSubmissionQueue.DrainResult {
        val sessionId = currentHardcoreSubmissionSessionId()
            ?: return HardcoreSubmissionQueue.DrainResult(
                submittedCount = 0,
                remainingCount = hardcoreSubmissionQueue.pendingCount(),
            )
        return hardcoreSubmissionQueue.drain(sessionId)
    }

    private suspend fun discardHardcoreSubmissions(): Int {
        val sessionId = currentHardcoreSubmissionSessionId() ?: return 0
        return hardcoreSubmissionQueue.discardAll(sessionId)
    }

    private fun currentHardcoreSubmissionSessionId(): String? {
        return synchronized(runtimeAuthenticationLeaseMonitor) {
            activeHardcoreSubmissionSessionId
        }
    }

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

    private val _renderedInternalResolution = MutableStateFlow<Pair<Int, Int>?>(null)
    val renderedInternalResolution = _renderedInternalResolution.asStateFlow()

    private val _toastEvent = EventSharedFlow<ToastEvent>()
    val toastEvent = _toastEvent.asSharedFlow()

    private val retroAchievementsVersionName: String by lazy {
        runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        }.getOrNull().orEmpty().ifBlank { "unknown" }
    }

    private val retroAchievementsUserAgent = RETROACHIEVEMENTS_USER_AGENT

    private val _raIntegrationEvent = EventSharedFlow<RAIntegrationEvent>()
    val integrationEvent = _raIntegrationEvent.asSharedFlow()

    val pendingSubmissionsSummary = retroAchievementsSubmissionHandler.getPendingSubmissionsSummaryFlow()
    private val _pendingRaSubmissionSnapshot = MutableStateFlow<PendingRaSubmissionSnapshot?>(null)
    val pendingRaSubmissionSnapshot = _pendingRaSubmissionSnapshot.asStateFlow()
    val pendingRaModalState = pendingRaModalController.state
    private val _raHardcoreContinuityState = MutableStateFlow(RaHardcoreContinuityState.ONLINE_LIVE)
    val raHardcoreContinuityState = _raHardcoreContinuityState.asStateFlow()

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
    private var lastEndpointRestartNoticeGeneration: Long? = null
    init {
        viewModelScope.launch {
            retroAchievementsEndpointProvider.snapshot.collect { updated ->
                val frozen = retroAchievementsEndpointProvider.routingSnapshot()
                if (
                    currentRom != null &&
                    frozen.generation != updated.generation &&
                    lastEndpointRestartNoticeGeneration != updated.generation
                ) {
                    lastEndpointRestartNoticeGeneration = updated.generation
                    _toastEvent.tryEmit(ToastEvent.RetroAchievementsProviderChangedRestartRequired)
                    RetroAchievementsEndpointStorage.logSnapshot(updated, "restart_required")
                }
            }
        }

        viewModelScope.launch {
            _layout.filterNotNull().collect {
                uiLayoutProvider.setCurrentLayoutConfiguration(it)
            }
        }

        viewModelScope.launch {

            settingsRepository.observeFrameskipConfiguration().collectLatest {
                emulatorManager.setFrameskipConfiguration(it)
            }
        }
        viewModelScope.launch {

            kotlinx.coroutines.flow.combine(
                settingsRepository.observeVulkanDrsEnabled(),
                settingsRepository.getVideoRenderer(),
            ) { _, _ -> settingsRepository.isVulkanDrsActive() }.collectLatest {
                emulatorManager.setVulkanDrsEnabled(it)
            }
        }
        viewModelScope.launch {

            settingsRepository.observeMuteOnFastForwardEnabled().collectLatest {
                emulatorManager.setMuteOnFastForward(it)
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
            val player = PhantomPlayer.current
            if (player == null) {
                launchEmulator(launchArgs)
            } else viewModelScope.launch {
                try {
                    player.connect()
                    launchEmulator(launchArgs)
                } catch (e: kotlinx.coroutines.CancellationException) { throw e }
                catch (e: Exception) {
                    player.notify(PhantomPlayer.ERROR, e.localizedMessage)
                    _emulatorState.value = EmulatorState.RomLoadError(e.localizedMessage)
                }
            }
        } else {
            _uiEvent.tryEmit(EmulatorUiEvent.CloseEmulator)
        }
    }

    suspend fun matchesRunningLaunch(args: LaunchArgs): Boolean {
        val currentState = _emulatorState.value
        if (currentState is EmulatorState.RunningFirmware) {
            return args is LaunchArgs.Firmware && args.resumes(currentState.console)
        }
        val running = currentState as? EmulatorState.RunningRom ?: return false
        val matchedUri = when (args) {
            is LaunchArgs.RomObject -> args.rom.uri
            is LaunchArgs.RomUri -> (romsRepository.getFrontendRomAtUri(args.uri) as? FrontendRomLookup.Found)?.rom?.uri
            is LaunchArgs.RomPath -> (romsRepository.getFrontendRomAtPath(args.path) as? FrontendRomLookup.Found)?.rom?.uri
            is LaunchArgs.Firmware -> return false
        }
        return matchedUri == running.rom.uri && _emulatorState.value === running
    }

    fun relaunchWithNewArgs(args: LaunchArgs) {
        if (!_emulatorState.value.isRunning()) {
            launchEmulator(args)
            return
        }

        sessionCoroutineScope.launch {
            val runningRom = _emulatorState.value as? EmulatorState.RunningRom
            val exitContext = effectivePendingExitContext()
            if (runningRom != null) {
                emulatorManager.pauseEmulator()
                if (!refreshPendingRaSubmissionMirror()) {
                    _toastEvent.tryEmit(ToastEvent.PendingRaStateVerificationFailed)
                    if (exitContext == RaPendingExitContext.RESUMABLE_SESSION) {
                        resumeEmulatorIfSessionCanRun()
                    }
                    return@launch
                }
                if ((pendingRaSubmissionStore?.snapshot?.value?.counts?.total ?: 0) > 0) {
                    when (handleRcClientPendingBeforeExit(exitContext)) {
                        RaPendingExitFollowUp.EXIT -> Unit
                        RaPendingExitFollowUp.RESUME_SESSION -> {
                            if (exitContext == RaPendingExitContext.RESUMABLE_SESSION) {
                                resumeEmulatorIfSessionCanRun()
                            }
                            return@launch
                        }
                        RaPendingExitFollowUp.KEEP_SESSION_PAUSED -> return@launch
                    }
                }

                val userAuth = retroAchievementsRepository.getUserAuthentication()
                if (userAuth != null) {
                    val userId = userAuth.username
                    val contentId = runningRom.rom.retroAchievementsHash
                    val ledgerStatus = withContext(Dispatchers.IO) {
                        offlineLedgerRepository.getStatus(userId, contentId)
                    }

                    if (hardcoreSubmissionQueue.pendingCount() > 0) {
                        val shouldExit = handleHardcorePendingBeforeExit(
                            userId = userId,
                            contentId = contentId,
                            exitContext = exitContext,
                        )
                        if (!shouldExit) {
                            if (exitContext == RaPendingExitContext.RESUMABLE_SESSION) {
                                resumeEmulatorIfSessionCanRun()
                            }
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
            discardHardcoreSubmissions()
            stopEmulator()
            launchEmulator(args)
        }
    }

    fun onRomLaunchValidated(rom: Rom) {
        val validation = _emulatorState.value as? EmulatorState.ValidatingRom ?: return
        if (validation.rom.uri != rom.uri) return
        sessionCoroutineScope.launch {
            if (_emulatorState.value === validation) launchRom(rom, validation.saveDirectory)
        }
    }

    fun onFirmwareLaunchValidated(consoleType: ConsoleType, dsiProfile: me.magnum.melonds.domain.model.DSiProfile?) {
        viewModelScope.launch {
            launchFirmware(consoleType, dsiProfile)
        }
    }

    private fun launchEmulator(args: LaunchArgs) {
        when (args) {
            is LaunchArgs.RomObject -> loadRom(args.rom)
            is LaunchArgs.RomUri -> loadFrontendRom { romsRepository.getFrontendRomAtUri(args.uri) }
            is LaunchArgs.RomPath -> loadFrontendRom { romsRepository.getFrontendRomAtPath(args.path) }
            is LaunchArgs.Firmware -> _emulatorState.value = EmulatorState.ValidatingFirmware(args.consoleType)
        }
    }

    private fun loadRom(rom: Rom) {
        viewModelScope.launch {
            resetEmulatorState(EmulatorState.LoadingRom())
            sessionCoroutineScope.launch {
                val refreshedRom = refreshRomForLaunch(rom)
                _emulatorState.value = if (refreshedRom != null) {
                    EmulatorState.ValidatingRom(refreshedRom)
                } else {
                    EmulatorState.RomNotFoundError(rom.uri.toString())
                }
            }
        }
    }

    private suspend fun refreshRomForLaunch(rom: Rom): Rom? {
        return romsRepository.getRomAtUri(rom.uri) ?: rom.takeUnless { it.metadataPending }
    }

    private fun loadFrontendRom(resolve: suspend () -> FrontendRomLookup) {
        viewModelScope.launch {
            resetEmulatorState(EmulatorState.LoadingRom())
            sessionCoroutineScope.launch {
                _emulatorState.value = frontendRomLaunchState(resolve())
            }
        }
    }

    private fun frontendRomLaunchState(lookup: FrontendRomLookup): EmulatorState {
        val message = when (lookup) {
            is FrontendRomLookup.Found -> {
                val saveDirectory = if (settingsRepository.saveNextToRomFile()) {
                    lookup.rom.parentTreeUri
                } else {
                    settingsRepository.getSaveFileDirectory()
                }
                if (saveDirectory != null) return EmulatorState.ValidatingRom(lookup.rom, saveDirectory)
                context.getString(R.string.frontend_save_missing_directory)
            }
            is FrontendRomLookup.NameNotFound -> {
                val directories = lookup.searchedDirectories.joinToString("\n").ifEmpty {
                    context.getString(R.string.frontend_no_readable_rom_folders)
                }
                context.getString(R.string.frontend_rom_name_not_found, lookup.name, directories)
            }
            is FrontendRomLookup.AmbiguousName -> context.getString(R.string.frontend_rom_name_ambiguous, lookup.name)
            FrontendRomLookup.UnverifiedLocation -> context.getString(R.string.frontend_rom_unverified_location)
        }
        return EmulatorState.FrontendSaveSelectionRequired(message)
    }

    private suspend fun launchRom(rom: Rom, saveDirectory: Uri?) = coroutineScope {
        try {
            _emulatorState.value = EmulatorState.LoadingRom()
            currentRom = rom
            activeRomConfig.value = rom
            val isRetroAchievementsEnabledForLaunch = PhantomPlayer.current == null && isRetroAchievementsEnabledForLaunch(rom)
            val endpointSnapshot = if (isRetroAchievementsEnabledForLaunch) {
                retroAchievementsEndpointProvider.beginSession()
            } else {
                retroAchievementsEndpointProvider.endSession()
                retroAchievementsEndpointProvider.currentSnapshot()
            }
            val launchDecision = (if (isRetroAchievementsEnabledForLaunch) {
                runCatching {
                    decideRetroAchievementsLaunchDecision(rom, endpointSnapshot)
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
            }).copy(
                nativeClientHost = endpointSnapshot.nativeClientHost.orEmpty(),
                endpointGeneration = endpointSnapshot.generation,
            )

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
            confirmRetroArchShaderCompile(rom.config)
            val result = emulatorManager.loadRom(rom, cheats, saveDirectory)
            when (result) {
                is RomLaunchResult.LaunchFailedRomNotFound,
                is RomLaunchResult.LaunchFailedRomNotSupported,
                is RomLaunchResult.LaunchFailedSramProblem,
                is RomLaunchResult.LaunchFailed -> {
                    disableRetroAchievementsRuntime(reason = "rom_load_failed")
                    _emulatorState.value = EmulatorState.RomLoadError(rom.unsupportedReason)
                }
                is RomLaunchResult.LaunchSuccessful -> {
                    if (!result.isGbaLoadSuccessful) {
                        _toastEvent.tryEmit(ToastEvent.GbaLoadFailed)
                    }
                    _emulatorState.value = EmulatorState.RunningRom(rom)
                    maybeAutoLoadStateOnLaunch(rom)
                    DebugCommandStateStore.onRunningRomReady(rom.uri, rom.name)
                    startTrackingFps()
                    if (PhantomPlayer.current == null) startTrackingPlayTime(rom)
                }
            }
        } catch (exception: Throwable) {
            if (exception is CancellationException) {
                throw exception
            }
            Log.e("EmulatorViewModel", "Failed to launch ROM '${rom.name}'", exception)
            disableRetroAchievementsRuntime(reason = "rom_launch_exception")
            _emulatorState.value = EmulatorState.RomLoadError(rom.unsupportedReason)
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

    fun submitHardcorePendingExitChoice(
        requestId: Long,
        choice: HardcorePendingExitChoice,
    ) {
        val waiter = hardcoreExitChoiceWaiter ?: return
        if (
            waiter.requestId == requestId &&
            pendingRaModalController.isCurrentExitPrompt(requestId)
        ) {
            waiter.deferred.complete(choice)
        }
    }

    private suspend fun decideRetroAchievementsLaunchDecision(
        rom: Rom,
        endpointSnapshot: RetroAchievementsEndpointSnapshot,
    ): RetroAchievementsLaunchDecision {
        val startedOnline = networkStatusProvider.isOnline()
        val hardcoreSettingEnabled = settingsRepository.isRetroAchievementsHardcoreEnabled()
        val offlineSoftcoreEnabled = settingsRepository.isRetroAchievementsOfflineSoftcoreEnabled()
        val userAuth = retroAchievementsRepository.getUserAuthentication()

        if (endpointSnapshot.backendEffective == RetroAchievementsOfflineBackend.RA_OFFLINE_PROXY) {
            RetroAchievementsEndpointStorage.logSnapshot(endpointSnapshot, "session_frozen")
            if (endpointSnapshot.apiUrl == null || endpointSnapshot.nativeClientHost == null) {
                _toastEvent.tryEmit(ToastEvent.RAOfflineProxyNotActive)
            }
            return RetroAchievementsLaunchDecision(
                networkMode = RetroAchievementsNetworkMode.ONLINE_LIVE,
                sessionMode = RetroAchievementsSessionMode.SOFTCORE,
                initialOfflineType = null,
                isHardcoreEligibleAfterOnlineStart = false,
                offlineDueToNoInternetAtStart = false,
                hardcoreOfflineDisabled = hardcoreSettingEnabled,
                usesProxyBackend = true,
                nativeClientHost = endpointSnapshot.nativeClientHost.orEmpty(),
                endpointGeneration = endpointSnapshot.generation,
            )
        }

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
            _toastEvent.tryEmit(
                ToastEvent.HardcoreOfflineUnsyncedWarning(
                    ledgerStatus.pendingHardcoreUnlockCount,
                ),
            )
            logRaTrace(
                "hardcore_ledger_legacy_preserved",
                "content_id" to contentId,
                "pending" to ledgerStatus.pendingHardcoreUnlockCount,
                "same_session_sync_available" to false,
            )
        }

        if (RaHardcoreLaunchPolicy.mustUseOfflinePath(startedOnline)) {
            if (
                offlineSoftcoreEnabled &&
                ledgerStatus.integrity == OfflineLedgerIntegrity.OK &&
                ledgerStatus.pendingSoftcoreUnlockCount > 0
            ) {
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
            "error" to (error?.javaClass?.simpleName ?: "unknown"),
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
        pending: RaPendingCounts,
        exitContext: RaPendingExitContext = RaPendingExitContext.RESUMABLE_SESSION,
    ): HardcoreExitChoiceResponse {
        val requestId = pendingRaModalController.beginExitPrompt(
            pending = pending,
            exitContext = exitContext,
        ) ?: throw CancellationException("A higher-priority pending submission dialog is active")
        hardcoreExitChoiceWaiter?.deferred?.cancel()
        val deferred = CompletableDeferred<HardcorePendingExitChoice>()
        val waiter = HardcoreExitChoiceWaiter(requestId, deferred)
        hardcoreExitChoiceWaiter = waiter
        return HardcoreExitChoiceResponse(
            requestId = requestId,
            choice = deferred.await(),
        ).also {
            if (hardcoreExitChoiceWaiter === waiter) {
                hardcoreExitChoiceWaiter = null
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

    private fun launchFirmware(consoleType: ConsoleType, dsiProfile: me.magnum.melonds.domain.model.DSiProfile?) {
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

                confirmRetroArchShaderCompile(romConfig = null)
                val result = emulatorManager.loadFirmware(consoleType, dsiProfile)
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

    fun onSettingsChanged(resumeWhenFinished: Boolean = false) {
        if (resumeWhenFinished) {
            resumeAfterSettingsReconciliation.set(true)
        }
        settingsReconciliationsInFlight.incrementAndGet()
        sessionCoroutineScope.launch(start = CoroutineStart.UNDISPATCHED) {
            var completedNormally = false
            try {
                settingsReconciliationMutex.withLock {
                    val currentState = _emulatorState.value
                    val activeRuntimeAuthenticationMatches = run {
                        val runtimeConfig = activeRuntimeBridgeConfig
                        val authenticatedUser =
                            retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated
                        runtimeConfig == null ||
                            RaRuntimeAuthenticationPolicy.matches(
                                runtimeUserId = runtimeConfig.username,
                                runtimeToken = runtimeConfig.apiToken,
                                authenticatedUserId = authenticatedUser?.username,
                                authenticatedToken = authenticatedUser?.token,
                            )
                    }
                    val suspendRcClientSubmissionTransport =
                        activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT &&
                            !activeRuntimeAuthenticationMatches
                    emulatorManager.setRetroAchievementsSubmissionTransportSuspended(
                        suspendRcClientSubmissionTransport,
                    )
                    if (settingsRepository.getCurrentVideoRenderer() == VideoRenderer.VULKAN) {
                        val canUseVulkan = MelonDSAndroidInterface.isVulkanRendererSupported() &&
                            MelonDSAndroidInterface.canInitializeVulkanRenderer()
                        if (!canUseVulkan) {
                            val activeRenderer = getRuntimeRendererOrNull() ?: VideoRenderer.SOFTWARE
                            settingsRepository.setCurrentVideoRenderer(activeRenderer)
                            _toastEvent.tryEmit(ToastEvent.RendererInitFailed(VideoRenderer.VULKAN))
                            return@withLock
                        }
                    }

                    val requestedRaEnabled = isRetroAchievementsEnabledForSettingsUpdate(currentState)
                    if (
                        activeRuntimePath != RetroAchievementsRuntimePath.DISABLED &&
                        !activeRuntimeAuthenticationMatches
                    ) {
                        logRaSubmission(
                            "ra_runtime_authentication_changed",
                            "runtime_path" to activeRuntimePath.traceValue,
                            "pending_total" to pendingRaSubmissionStore?.snapshot?.value?.counts?.total,
                        )
                        _toastEvent.tryEmit(ToastEvent.CannotSwitchRetroAchievementsMode)
                        when (currentState) {
                            is EmulatorState.RunningRom ->
                                emulatorManager.updateRomEmulatorConfiguration(currentState.rom)
                            is EmulatorState.RunningFirmware ->
                                emulatorManager.updateFirmwareEmulatorConfiguration(currentState.console)
                            else -> Unit
                        }
                        return@withLock
                    }
                    if (
                        activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT &&
                        pendingRaSubmissionStore != null &&
                        !refreshPendingRaSubmissionMirror()
                    ) {
                        _toastEvent.tryEmit(ToastEvent.PendingRaStateVerificationFailed)
                        when (currentState) {
                            is EmulatorState.RunningRom ->
                                emulatorManager.updateRomEmulatorConfiguration(currentState.rom)
                            is EmulatorState.RunningFirmware ->
                                emulatorManager.updateFirmwareEmulatorConfiguration(currentState.console)
                            else -> Unit
                        }
                        return@withLock
                    }
                    if (
                        activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT &&
                        pendingRaSubmissionStore != null &&
                        networkStatusProvider.isLikelyOnline()
                    ) {
                        requestPendingRuntimeReconnect()
                    }
                    val pendingStore = pendingRaSubmissionStore
                    if (
                        activeRuntimePath == RetroAchievementsRuntimePath.DISABLED &&
                        (pendingStore?.snapshot?.value?.counts?.total ?: 0) > 0
                    ) {
                        _toastEvent.tryEmit(ToastEvent.CannotSwitchRetroAchievementsMode)
                        return@withLock
                    }
                    val pendingContextStillValid =
                        pendingStore?.let { pendingContextMatchesActiveRuntime(it.context) } ?: true
                    val preservePendingRcClientRuntime =
                        RaPendingSubmissionUiPolicy.mustKeepRuntimeForPendingSettingsDisable(
                            requestedEnabled = requestedRaEnabled,
                            isHardcore = emulatorSession.isRetroAchievementsHardcoreModeEnabled,
                            runtimeOwner = if (activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT) {
                                RaPendingRuntimeOwner.RC_CLIENT
                            } else {
                                RaPendingRuntimeOwner.NONE
                            },
                            pendingCount = pendingStore?.snapshot?.value?.counts?.total ?: 0,
                            contextStillValid = pendingContextStillValid,
                        )
                    pendingRaRuntimeDisableGate.update(preservePendingRcClientRuntime)
                    if (preservePendingRcClientRuntime) {
                        _toastEvent.tryEmit(ToastEvent.CannotSwitchRetroAchievementsMode)
                        logRaSubmission(
                            "ra_pending_settings_disable_deferred",
                            "pending_total" to pendingRaSubmissionStore?.snapshot?.value?.counts?.total,
                            "runtime_path" to "rc_client",
                            "hardcore" to true,
                        )
                    }
                    val sessionUpdateActions = emulatorSession.updateRetroAchievementsSettings(
                        requestedRaEnabled || preservePendingRcClientRuntime,
                        settingsRepository.isRetroAchievementsHardcoreEnabled(),
                    )

                    when (currentState) {
                        is EmulatorState.RunningRom -> emulatorManager.updateRomEmulatorConfiguration(currentState.rom)
                        is EmulatorState.RunningFirmware -> emulatorManager.updateFirmwareEmulatorConfiguration(currentState.console)
                        else -> Unit
                    }

                    dispatchSessionUpdateActions(sessionUpdateActions)
                }
                completedNormally = true
            } finally {
                val shouldResume =
                    settingsReconciliationsInFlight.decrementAndGet() == 0 &&
                        resumeAfterSettingsReconciliation.getAndSet(false)
                if (shouldResume && completedNormally && currentCoroutineContext().isActive) {
                    resumeEmulatorIfSessionCanRun()
                }
            }
        }
    }

    fun onRetroAchievementsLogoutRequested() {
        settingsReconciliationsInFlight.incrementAndGet()
        sessionCoroutineScope.launch(start = CoroutineStart.UNDISPATCHED) {
            var resumeSession = true
            var transportSuspended = false
            var terminalCommitStarted = false
            try {
                settingsReconciliationMutex.withLock {
                    emulatorManager.pauseEmulator()
                    val runtimeConfig = activeRuntimeBridgeConfig
                    val expectedUsername = runtimeConfig?.username?.takeIf(String::isNotBlank)
                    val expectedToken = runtimeConfig?.apiToken?.takeIf(String::isNotBlank)
                    val expectedStore = pendingRaSubmissionStore
                    val expectedLeaseId = synchronized(runtimeAuthenticationLeaseMonitor) {
                        activeRuntimeAuthenticationLeaseId
                    }
                    if (
                        runtimeConfig == null ||
                        expectedUsername == null ||
                        expectedToken == null ||
                        expectedStore == null ||
                        expectedLeaseId == null ||
                        !isInGameRetroAchievementsLogoutSupported()
                    ) {
                        logRaSubmission(
                            "ra_logout_rejected",
                            "reason" to "unsupported_runtime_context",
                            "runtime_path" to activeRuntimePath.traceValue,
                            "hardcore" to emulatorSession.isRetroAchievementsHardcoreModeEnabled,
                        )
                        _toastEvent.tryEmit(ToastEvent.CannotSwitchRetroAchievementsMode)
                        return@withLock
                    }

                    var authenticationMatchesRuntime = false
                    val coordinator = RaInGameLogoutCoordinator(
                        suspendSubmissionTransport = {
                            emulatorManager.setRetroAchievementsSubmissionTransportSuspended(true)
                            transportSuspended = true
                        },
                        runtimeIdentityMatches = {
                            val authenticatedUser =
                                retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated
                            authenticationMatchesRuntime =
                                RaRuntimeAuthenticationPolicy.matches(
                                    runtimeUserId = expectedUsername,
                                    runtimeToken = expectedToken,
                                    authenticatedUserId = authenticatedUser?.username,
                                    authenticatedToken = authenticatedUser?.token,
                                )
                            logRaSubmission(
                                "ra_logout_identity_checked",
                                "identity_match" to authenticationMatchesRuntime,
                                "submission_allowed" to false,
                                "pending_total" to expectedStore.snapshot.value.counts.total,
                            )
                            authenticationMatchesRuntime
                        },
                        preparePendingSubmissionIds = {
                            prepareCurrentPendingRaSubmissionsForLogout(
                                expectedRuntimeConfig = runtimeConfig,
                                expectedStore = expectedStore,
                            )
                        },
                        beginTerminalCommit = {
                            terminalCommitStarted = true
                            resumeSession = false
                            resumeAfterSettingsReconciliation.set(false)
                            raSessionStopGate.observeTerminalStop()
                            pendingRaModalController.reset()
                        },
                        discardPendingSubmissions = { expectedSubmissionIds ->
                            discardPreparedPendingRaSubmissionsForLogout(
                                expectedStore = expectedStore,
                                expectedSubmissionIds = expectedSubmissionIds,
                            )
                        },
                        discardKotlinPendingAchievements = {
                            discardHardcoreSubmissions()
                        },
                        terminateRuntime = {
                            check(
                                unloadAndHandoffRuntimeAuthenticationLeaseToLogout(
                                    expectedLeaseId,
                                ),
                            )
                            try {
                                disableRetroAchievementsRuntime(reason = "logout")
                            } finally {
                                stopEmulator()
                            }
                            transportSuspended = false
                        },
                        clearAuthenticationIfMatches = {
                            retroAchievementsRepository.completeRuntimeAuthenticationLogout(
                                leaseId = expectedLeaseId,
                                expectedUsername = expectedUsername,
                                expectedToken = expectedToken,
                            )
                        },
                        closeSession = {
                            _uiEvent.tryEmit(EmulatorUiEvent.CloseEmulator)
                        },
                    )

                    when (val result = coordinator.execute()) {
                        is RaInGameLogoutResult.Committed -> {
                            logRaSubmission(
                                "ra_logout_completed",
                                "identity_match" to authenticationMatchesRuntime,
                                "native_expected" to result.discarded.expectedNativeSubmissions,
                                "native_confirmed" to result.discarded.confirmedNativeSubmissions,
                                "kotlin_achievements_discarded" to
                                    result.discarded.confirmedKotlinAchievements,
                                "authentication_cleared" to result.authenticationCleared,
                                "failure_stages" to result.failures.joinToString(",") {
                                    it.stage.name.lowercase()
                                }.ifBlank { "none" },
                                "runtime_unloaded_before_auth_clear" to true,
                            )
                            if (result.failures.isNotEmpty()) {
                                _toastEvent.tryEmit(ToastEvent.RetroAchievementsLogoutFailed)
                            }
                        }
                        is RaInGameLogoutResult.PreflightFailed -> {
                            logRaSubmission(
                                "ra_logout_failed",
                                "stage" to result.stage.name.lowercase(),
                                "error" to result.errorType,
                                "terminal_commit_started" to false,
                            )
                            if (
                                result.stage == RaInGameLogoutFailureStage.IDENTITY_VERIFICATION
                            ) {
                                _toastEvent.tryEmit(ToastEvent.RetroAchievementsAccountChangedInGame)
                            } else {
                                _toastEvent.tryEmit(ToastEvent.PendingRaStateVerificationFailed)
                            }
                        }
                    }
                }
            } finally {
                val noSettingsReconciliationRemaining =
                    settingsReconciliationsInFlight.decrementAndGet() == 0
                if (
                    transportSuspended &&
                    !terminalCommitStarted &&
                    noSettingsReconciliationRemaining &&
                    currentCoroutineContext().isActive
                ) {
                    val runtime = activeRuntimeBridgeConfig
                    val authenticatedUser =
                        retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated
                    val canRestoreTransport =
                        runtime != null &&
                            RaRuntimeAuthenticationPolicy.matches(
                                runtimeUserId = runtime.username,
                                runtimeToken = runtime.apiToken,
                                authenticatedUserId = authenticatedUser?.username,
                                authenticatedToken = authenticatedUser?.token,
                            )
                    if (canRestoreTransport) {
                        emulatorManager.setRetroAchievementsSubmissionTransportSuspended(false)
                    }
                }
                if (
                    resumeSession &&
                    !terminalCommitStarted &&
                    noSettingsReconciliationRemaining &&
                    currentCoroutineContext().isActive
                ) {
                    resumeEmulatorIfSessionCanRun()
                }
            }
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

    fun cycleLayout() {
        if (!_emulatorState.value.isRunning()) return
        sessionCoroutineScope.launch {
            val includedIds = settingsRepository.getLayoutCycleIds()
            val candidates = layoutsRepository.getLayouts().first().filter {
                it.id != null && (includedIds == null || it.id in includedIds)
            }
            if (candidates.isEmpty()) return@launch
            val currentId = (_emulatorState.value as? EmulatorState.RunningRom)?.rom?.config?.layoutId
                ?: settingsRepository.getSelectedLayoutId()
            val currentIndex = candidates.indexOfFirst { it.id == currentId }
            val nextId = candidates[(currentIndex + 1) % candidates.size].id ?: return@launch
            if (nextId == currentId) return@launch
            if (_emulatorState.value is EmulatorState.RunningRom) {
                onRunningRomLayoutSelected(nextId)
            } else if (_emulatorState.value is EmulatorState.RunningFirmware) {
                settingsRepository.setSelectedLayoutId(nextId)
            }
        }
    }

    fun onRunningRomLayoutSelected(layoutId: UUID?) {
        updateRunningRomConfig(updateNativeConfiguration = false) { it.copy(layoutId = layoutId) }
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

    private fun updateRunningRomConfig(updateNativeConfiguration: Boolean = true, update: (me.magnum.melonds.domain.model.rom.config.RomConfig) -> me.magnum.melonds.domain.model.rom.config.RomConfig) {
        val runningRom = (_emulatorState.value as? EmulatorState.RunningRom)?.rom ?: return
        val updatedRom = runningRom.copy(config = update(runningRom.config))
        romsRepository.updateRomConfig(runningRom, updatedRom.config)
        updateRunningRom(updatedRom)
        if (updateNativeConfiguration) {
            sessionCoroutineScope.launch {
                emulatorManager.updateRomEmulatorConfiguration(updatedRom)
            }
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
            val shouldPause = !showPauseMenu || !MultiplayerSession.active
            if (shouldPause) {
                emulatorManager.pauseEmulator()
            }
            if (showPauseMenu) {
                val rendererDebugToolsEnabled = settingsRepository.isRendererDebugToolsEnabled().firstOrNull() == true
                val pauseOptions = when (_emulatorState.value) {
                    is EmulatorState.RunningRom -> {
                        if (PhantomPlayer.current != null) listOf(RomPauseMenuOption.EXIT) else RomPauseMenuOption.entries.filter {
                            filterRomPauseMenuOption(it, rendererDebugToolsEnabled)
                        }
                    }
                    is EmulatorState.RunningFirmware -> {
                        FirmwarePauseMenuOption.entries
                    }
                    else -> null
                }

                if (pauseOptions != null) {
                    val syncMenuState = buildRaPendingSyncMenuState()
                    val labelOverrides: Map<PauseMenuOption, String> = if (
                        syncMenuState.isVisible &&
                        syncMenuState.label != null &&
                        RomPauseMenuOption.SYNC_RETRO_ACHIEVEMENTS in pauseOptions
                    ) {
                        mapOf<PauseMenuOption, String>(
                            RomPauseMenuOption.SYNC_RETRO_ACHIEVEMENTS to syncMenuState.label,
                        )
                    } else {
                        emptyMap()
                    }
                    _uiEvent.emit(
                        EmulatorUiEvent.ShowPauseMenu(
                            PauseMenu(
                                options = pauseOptions,
                                labelOverrides = labelOverrides,
                                isPaused = shouldPause || DebugCommandStateStore.isDebugPauseHeld(),
                            ),
                        ),
                    )
                }
            }
        }
    }

    fun onHostResumed() {
        if (_emulatorState.value.isRunning()
            && settingsRepository.isRtcSyncOnResumeEnabled()
            && !DebugCommandStateStore.isDebugPauseHeld()
        ) {
            MelonEmulator.requestRtcSync()
        }
    }

    fun resumeEmulator() {
        if (!_emulatorState.value.isRunning() || !raSessionStopGate.canResume()) {
            return
        }

        sessionCoroutineScope.launch {
            resumeEmulatorIfSessionCanRun()
        }
    }

    private suspend fun resumeEmulatorIfSessionCanRun() {
        if (
            _emulatorState.value.isRunning() &&
            settingsReconciliationsInFlight.get() == 0 &&
            raSessionStopGate.canResume() &&
            !pendingRaModalController.blocksLifecycleResume() &&
            !DebugCommandStateStore.isDebugPauseHeld()
        ) {
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
                leaderboardAttemptCoordinator.beginRuntimeReset()
                leaderboardTrackerUpdateLogLimiter.resetAll()
                emulatorManager.resetEmulator()
                _achievementsEvent.emit(RAEventUi.Reset)
            }
        }
    }

    fun cancelRomLaunch() {
        val session = sessionCoroutineScope.coroutineContext[Job] ?: return
        if (!session.isActive) return
        session.cancel()
        viewModelScope.launch {
            session.join()
            withContext(Dispatchers.IO) { emulatorManager.stopEmulator() }
            _uiEvent.emit(EmulatorUiEvent.CloseEmulator)
        }
    }

    fun stopEmulator() {
        raBootstrapJob?.cancel()
        raSessionJob?.cancel()
        raSessionJob = null
        leaderboardAttemptCoordinator.reset()
        leaderboardTrackerUpdateLogLimiter.resetAll()
        viewModelScope.launch {
            _achievementsEvent.emit(RAEventUi.Reset)
        }
        finalizeOfflineRetroAchievementsSessionIfNeeded()
        unloadAndReleaseActiveRuntimeAuthenticationLease("emulator_stopped")
        activeRuntimeBridgeConfig = null
        activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
        emulatorManager.stopEmulator()
        screenshotFrameBufferProvider.clearBuffer()
    }

    fun exitEmulator(force: Boolean = false) {
        if ((pendingRaSubmissionStore?.snapshot?.value?.counts?.total ?: 0) > 0) {
            requestExitRom()
            return
        }

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

    private fun requestExitRom(
        exitContext: RaPendingExitContext = RaPendingExitContext.RESUMABLE_SESSION,
    ) {
        val effectiveExitContext = effectivePendingExitContext(exitContext)
        sessionCoroutineScope.launch {
            val runningRom = _emulatorState.value as? EmulatorState.RunningRom
            if (runningRom == null) {
                stopEmulator()
                _uiEvent.emit(EmulatorUiEvent.CloseEmulator)
                return@launch
            }

            emulatorManager.pauseEmulator()
            if (!refreshPendingRaSubmissionMirror()) {
                _toastEvent.tryEmit(ToastEvent.PendingRaStateVerificationFailed)
                if (effectiveExitContext == RaPendingExitContext.RESUMABLE_SESSION) {
                    resumeEmulatorIfSessionCanRun()
                }
                return@launch
            }
            if ((pendingRaSubmissionStore?.snapshot?.value?.counts?.total ?: 0) > 0) {
                when (handleRcClientPendingBeforeExit(effectiveExitContext)) {
                    RaPendingExitFollowUp.EXIT -> Unit
                    RaPendingExitFollowUp.RESUME_SESSION -> {
                        if (effectiveExitContext == RaPendingExitContext.RESUMABLE_SESSION) {
                            resumeEmulatorIfSessionCanRun()
                        }
                        return@launch
                    }
                    RaPendingExitFollowUp.KEEP_SESSION_PAUSED -> return@launch
                }
            }

            val userAuth = retroAchievementsRepository.getUserAuthentication()
            if (userAuth == null) {
                maybeAutoSaveStateOnExit(runningRom.rom)
                discardHardcoreSubmissions()
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
                    val shouldExit = handleHardcorePendingBeforeExit(
                        userId = userId,
                        contentId = contentId,
                        exitContext = effectiveExitContext,
                    )
                    if (!shouldExit) {
                        if (effectiveExitContext == RaPendingExitContext.RESUMABLE_SESSION) {
                            resumeEmulatorIfSessionCanRun()
                        } else {
                            requestExitRom(RaPendingExitContext.TERMINAL_STOP)
                        }
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
            discardHardcoreSubmissions()
            stopEmulator()
            _uiEvent.emit(EmulatorUiEvent.CloseEmulator)
        }
    }

    private fun effectivePendingExitContext(
        requested: RaPendingExitContext = RaPendingExitContext.RESUMABLE_SESSION,
    ): RaPendingExitContext {
        return raSessionStopGate.resolve(requested)
    }

    private suspend fun handleHardcorePendingBeforeExit(
        userId: String,
        contentId: String,
        exitContext: RaPendingExitContext = RaPendingExitContext.RESUMABLE_SESSION,
    ): Boolean {
        val pending = hardcoreSubmissionQueue.pendingCount()
        if (pending == 0) {
            return true
        }

        val counts = RaPendingCounts(
            total = pending,
            achievementUnlocks = pending,
            leaderboardEntries = 0,
            retryable = pending,
            permanentFailures = 0,
        )
        val response = awaitHardcorePendingExitChoice(counts, exitContext)
        return when (response.choice) {
            HardcorePendingExitChoice.SYNC_AND_EXIT -> {
                val drainResult = drainHardcoreSubmissions()
                _toastEvent.tryEmit(
                    ToastEvent.HardcoreQueueSyncResult(
                        submittedCount = drainResult.submittedCount,
                        remainingCount = drainResult.remainingCount,
                    )
                )
                pendingRaModalController.clear(response.requestId)
                drainResult.remainingCount == 0
            }
            HardcorePendingExitChoice.CONTINUE_PLAYING -> {
                pendingRaModalController.clear(response.requestId)
                false
            }
            HardcorePendingExitChoice.DISCARD_AND_EXIT -> {
                discardHardcoreSubmissions()
                hardcoreOfflineLossTracker.clearPendingUnlocks(userId, contentId)
                pendingRaModalController.clear(response.requestId)
                true
            }
        }
    }

    private suspend fun handleRcClientPendingBeforeExit(
        exitContext: RaPendingExitContext = RaPendingExitContext.RESUMABLE_SESSION,
    ): RaPendingExitFollowUp {
        val store = pendingRaSubmissionStore ?: return RaPendingExitFollowUp.EXIT
        val before = store.snapshot.value.counts
        if (before.total == 0) return RaPendingExitFollowUp.EXIT

        val response = awaitHardcorePendingExitChoice(before, exitContext)
        return when (response.choice) {
            HardcorePendingExitChoice.SYNC_AND_EXIT -> {
                if (
                    !pendingRaModalController.transitionExitToSyncing(
                        requestId = response.requestId,
                        pending = before,
                    )
                ) {
                    return RaPendingExitFollowUp.KEEP_SESSION_PAUSED
                }
                val result = try {
                    syncPendingRaSubmissions(RaPendingSyncSource.EXIT_DIALOG)
                } catch (cancellation: CancellationException) {
                    pendingRaModalController.clear(response.requestId)
                    throw cancellation
                }
                val decision = RaPendingSubmissionUiPolicy.afterSyncAndExit(result)
                if (decision.shouldExit) {
                    pendingRaModalController.clear(response.requestId)
                } else {
                    pendingRaModalController.showResult(
                        requestId = response.requestId,
                        result = result,
                        action = if (exitContext == RaPendingExitContext.TERMINAL_STOP) {
                            RaPendingSyncResultAction.REOPEN_TERMINAL_EXIT
                        } else {
                            RaPendingSyncResultAction.RESUME_SESSION
                        },
                    )
                }
                decision.followUp
            }
            HardcorePendingExitChoice.CONTINUE_PLAYING -> {
                pendingRaModalController.clear(response.requestId)
                RaPendingSubmissionUiPolicy.continuePlaying(before, exitContext).followUp
            }
            HardcorePendingExitChoice.DISCARD_AND_EXIT -> {
                val expectedNativeSubmissionIds = store.snapshot.value.records
                    .map { it.submission.nativeSubmissionId }
                val confirmedNativeDiscardCount =
                    emulatorManager.discardPendingRetroAchievementsSubmissions(
                        expectedNativeSubmissionIds,
                    )
                if (confirmedNativeDiscardCount != expectedNativeSubmissionIds.size) {
                    logRaSubmission(
                        "ra_pending_discard_rejected",
                        "expected" to expectedNativeSubmissionIds.size,
                        "confirmed" to confirmedNativeDiscardCount,
                        "session_scope" to "current",
                        "accepted" to false,
                    )
                    val refreshed = refreshPendingRaSubmissionMirror()
                    pendingRaModalController.clear(response.requestId)
                    if (refreshed) {
                        requestExitRom(exitContext)
                        return RaPendingExitFollowUp.KEEP_SESSION_PAUSED
                    }
                    _toastEvent.tryEmit(ToastEvent.PendingRaStateVerificationFailed)
                    return if (exitContext == RaPendingExitContext.RESUMABLE_SESSION) {
                        RaPendingExitFollowUp.RESUME_SESSION
                    } else {
                        RaPendingExitFollowUp.KEEP_SESSION_PAUSED
                    }
                }
                val discarded = store.discardByNativeSubmissionIds(
                    nativeSubmissionIds = expectedNativeSubmissionIds.toSet(),
                    requestedContext = store.context,
                )
                if (discarded != confirmedNativeDiscardCount) {
                    logRaSubmission(
                        "ra_pending_discard_mirror_mismatch",
                        "confirmed" to confirmedNativeDiscardCount,
                        "discarded" to discarded,
                        "session_scope" to "current",
                        "accepted" to false,
                    )
                    val refreshed = refreshPendingRaSubmissionMirror()
                    pendingRaModalController.clear(response.requestId)
                    if (refreshed) {
                        requestExitRom(exitContext)
                    } else {
                        _toastEvent.tryEmit(ToastEvent.PendingRaStateVerificationFailed)
                    }
                    return RaPendingExitFollowUp.KEEP_SESSION_PAUSED
                }
                val remaining = store.snapshot.value.counts
                val decision = RaPendingSubmissionUiPolicy.afterDiscardAndExit(before, remaining)
                logRaSubmission(
                    "ra_pending_discarded",
                    "discarded" to discarded,
                    "remaining" to remaining.total,
                    "session_scope" to "current",
                    "accepted" to false,
                )
                if (decision.shouldExit) {
                    hardcoreOfflineLossTracker.clearPendingUnlocks(
                        store.context.userId,
                        store.context.contentHash,
                    )
                    pendingRaModalController.clear(response.requestId)
                } else {
                    pendingRaModalController.clear(response.requestId)
                    requestExitRom(exitContext)
                }
                decision.followUp
            }
        }
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
        if ((option == RomPauseMenuOption.SAVE_STATE || option == RomPauseMenuOption.LOAD_STATE
                || option == RomPauseMenuOption.REWIND) && multiplayerOperationBlocked()) return
        when (option) {
            is RomPauseMenuOption -> {
                when (option) {
                    RomPauseMenuOption.SETTINGS -> _uiEvent.tryEmit(
                        EmulatorUiEvent.OpenScreen.SettingsScreen(
                            romSettingsOverrides =
                                (_emulatorState.value as? EmulatorState.RunningRom)?.rom?.let {
                                    getInGameRomSettingsOverrides(it)
                            } ?: InGameRomSettingsOverrides(),
                            retroAchievementsRuntimeIdentityLocked =
                                activeRuntimePath != RetroAchievementsRuntimePath.DISABLED ||
                                    emulatorSession.isRetroAchievementsEnabledForSession(),
                            retroAchievementsInGameLogoutSupported =
                                isInGameRetroAchievementsLogoutSupported(),
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
                                sessionCoroutineScope.launch {
                                    val saveStateSlots = getRomSaveStateSlots(it.rom)
                                    _uiEvent.emit(EmulatorUiEvent.ShowRomSaveStates(saveStateSlots, EmulatorUiEvent.ShowRomSaveStates.Reason.SAVING))
                                }
                            }
                        }
                    }
                    RomPauseMenuOption.LOAD_STATE -> {
                        if (emulatorSession.areSaveStateLoadsAllowed()) {
                            (_emulatorState.value as? EmulatorState.RunningRom)?.let {
                                sessionCoroutineScope.launch {
                                    val saveStateSlots = getRomSaveStateSlots(it.rom)
                                    _uiEvent.emit(EmulatorUiEvent.ShowRomSaveStates(saveStateSlots, EmulatorUiEvent.ShowRomSaveStates.Reason.LOADING))
                                }
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
                    RomPauseMenuOption.SYNC_RETRO_ACHIEVEMENTS -> syncPendingRaSubmissionsFromPauseMenu()
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
        if (multiplayerOperationBlocked()) return
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

    private fun multiplayerOperationBlocked(): Boolean {
        if (!MultiplayerSession.active) return false
        _toastEvent.tryEmit(ToastEvent.MultiplayerOperationUnavailable)
        return true
    }

    fun onFastForwardToggleRequested(): Boolean {
        return !multiplayerOperationBlocked()
    }

    fun rewindToState(rewindSaveState: RewindSaveState) {
        if (multiplayerOperationBlocked()) return
        if (!emulatorSession.areSaveStateLoadsAllowed()) {
            _toastEvent.tryEmit(ToastEvent.RewindNotAvailableWhileRAHardcoreModeEnabled)
            return
        }

        sessionCoroutineScope.launch {
            emulatorManager.loadRewindState(rewindSaveState)
        }
    }

    fun saveStateToSlot(slot: SaveStateSlot) {
        if (multiplayerOperationBlocked()) return
        sessionCoroutineScope.launch {
            (_emulatorState.value as? EmulatorState.RunningRom)?.let {
                emulatorManager.pauseEmulator()
                try {
                    if (!saveRomState(it.rom, slot)) {
                        _toastEvent.emit(ToastEvent.StateSaveFailed)
                    }
                } finally {
                    resumeEmulatorIfSessionCanRun()
                }
            }
        }
    }

    fun loadStateFromSlot(slot: SaveStateSlot) {
        if (multiplayerOperationBlocked()) return
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
                    resumeEmulatorIfSessionCanRun()
                }
            }
        }
    }

    fun doQuickSave() {
        if (multiplayerOperationBlocked()) return
        val currentState = _emulatorState.value
        when (currentState) {
            is EmulatorState.RunningRom -> {
                sessionCoroutineScope.launch {
                    emulatorManager.pauseEmulator()
                    val quickSlot = getRomQuickSaveStateSlot(currentState.rom)
                    if (saveRomState(currentState.rom, quickSlot)) {
                        _toastEvent.emit(ToastEvent.QuickSaveSuccessful)
                    }
                    resumeEmulatorIfSessionCanRun()
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
        if (multiplayerOperationBlocked()) return
        val currentState = _emulatorState.value
        when (currentState) {
            is EmulatorState.RunningRom -> {
                if (emulatorSession.areSaveStateLoadsAllowed()) {
                    sessionCoroutineScope.launch {
                        emulatorManager.pauseEmulator()
                        val quickSlot = getRomQuickSaveStateSlot(currentState.rom)
                        if (loadRomState(currentState.rom, quickSlot)) {
                            _toastEvent.emit(ToastEvent.QuickLoadSuccessful)
                        }
                        resumeEmulatorIfSessionCanRun()
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

    fun takeScreenshot() {
        val state = _emulatorState.value as? EmulatorState.RunningRom ?: return
        if (screenshotJob?.isActive == true) {
            return
        }
        screenshotJob = sessionCoroutineScope.launch {
            try {
                val fileName = withContext(Dispatchers.IO) {
                    val screenshot = emulatorManager.takeScreenshot() ?: return@withContext null
                    try {
                        screenshotFileManager.save(state.rom, screenshot)
                    } finally {
                        screenshot.recycle()
                    }
                }
                if (fileName == null) {
                    _toastEvent.emit(ToastEvent.ScreenshotFailed)
                    return@launch
                }
                _toastEvent.emit(ToastEvent.ScreenshotSaved(fileName))
            } catch (cancellation: CancellationException) {
                throw cancellation
            } catch (exception: Exception) {
                Log.w("Screenshot", "Capture failed", exception)
                _toastEvent.emit(ToastEvent.ScreenshotFailed)
            }
        }
    }

    fun deleteSaveStateSlot(slot: SaveStateSlot, onSlotsUpdated: (List<SaveStateSlot>) -> Unit) {
        (_emulatorState.value as? EmulatorState.RunningRom)?.let {
            sessionCoroutineScope.launch {
                val updatedSlots = withContext(Dispatchers.IO) {
                    saveStatesRepository.deleteRomSaveState(it.rom, slot)
                    saveStatesRepository.getRomSaveStates(it.rom)
                }
                onSlotsUpdated(updatedSlots)
            }
        }
    }

    private suspend fun saveRomState(rom: Rom, slot: SaveStateSlot): Boolean {
        if (MultiplayerSession.active) return false
        val slotUri = getRomSaveStateUri(rom, slot)
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
        if (MultiplayerSession.active) return false
        if (!slot.exists) {
            return false
        }

        val slotUri = getRomSaveStateUri(rom, slot)
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

        val quickSlot = getRomQuickSaveStateSlot(rom)
        if (!quickSlot.exists) {
            Log.i(AUTO_STATE_TAG, "auto-load skipped: quick slot missing")
            return
        }

        val quickSlotUri = runCatching { getRomSaveStateUri(rom, quickSlot) }
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
        resumeEmulatorIfSessionCanRun()
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
        val quickSlot = getRomQuickSaveStateSlot(rom)
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
        retroAchievementsEndpointProvider.endSession()
        raBootstrapJob?.cancel()
        raSessionJob?.cancel()
        raSessionJob = null
        finalizeOfflineRetroAchievementsSessionIfNeeded()
        unloadAndReleaseActiveRuntimeAuthenticationLease("session_reset")
        val previousPendingStore = detachPendingRaSubmissionSession()
        if (previousPendingStore != null) {
            viewModelScope.launch {
                previousPendingStore.cleanup()
            }
        }
        sessionCoroutineScope.notifyNewSessionStarted()
        leaderboardAttemptCoordinator.reset()
        leaderboardTrackerUpdateLogLimiter.resetAll()
        emulatorSession.reset()
        _currentFps.value = null
        _renderedInternalResolution.value = null
        _emulatorState.value = newState
        _mainScreenBackground.value = RuntimeBackground.None
        _secondaryScreenBackground.value = RuntimeBackground.None
        _layout.value = null
        currentRom = null
        lastEndpointRestartNoticeGeneration = null
        activeRomConfig.value = null
        currentRetroAchievementsGameId = null
        offlineSyncChoiceDeferred?.cancel()
        offlineSyncChoiceDeferred = null
        hardcoreExitChoiceWaiter?.deferred?.cancel()
        hardcoreExitChoiceWaiter = null
        pendingRaModalController.reset()
        retroAchievementsNetworkMode = RetroAchievementsNetworkMode.ONLINE_LIVE
        retroAchievementsSessionMode = RetroAchievementsSessionMode.SOFTCORE
        transitionRaHardcoreContinuity(
            RaHardcoreContinuityEvent.SessionReset,
            reason = "session_reset",
        )
        isHardcoreEligibleAfterOnlineStart = false
        startedSessionOnlineLive = false
        isRetroAchievementsOnlineSessionStarted = false
        activeRuntimeBridgeConfig = null
        activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
        leaderboardDiagnosticsEnabled = false
        didReceiveRendererInitFailure = false
        raSessionStopGate.reset()
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
                            EmulatorEvent.Stop.Reason.WirelessUnsupported -> {
                                runCatching { MultiplayerRoom.leave() }.onFailure {
                                    Log.e("Multiplayer", "Could not close unsupported wireless room", it)
                                }
                                _toastEvent.tryEmit(ToastEvent.WirelessUnsupported)
                            }
                            EmulatorEvent.Stop.Reason.PowerOff -> { /* no-op */ }
                        }
                        when (_emulatorState.value) {
                            is EmulatorState.LoadingRom -> {
                                stopEmulator()
                                _emulatorState.value = EmulatorState.RomLoadError()
                            }
                            is EmulatorState.LoadingFirmware -> {
                                stopEmulator()
                                _emulatorState.value = EmulatorState.FirmwareLoadError(MelonEmulator.FirmwareLoadResult.FIRMWARE_BAD)
                            }
                            else -> {
                                raSessionStopGate.observeTerminalStop()
                                requestExitRom(RaPendingExitContext.TERMINAL_STOP)
                            }
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
                    is RAEvent.OnLeaderboardAttemptStarted,
                    is RAEvent.OnLeaderboardAttemptUpdated,
                    is RAEvent.OnLeaderboardAttemptSubmitted,
                    is RAEvent.OnLeaderboardScoreboard,
                    is RAEvent.OnLeaderboardSubmissionFailed,
                    is RAEvent.OnLeaderboardAttemptCancelled,
                    is RAEvent.OnLeaderboardTrackerHidden,
                    is RAEvent.OnLeaderboardRuntimeReset -> onRcClientLeaderboardEvent(it)
                    is RAEvent.OnLeaderboardAttemptCompleted -> onLeaderboardAttemptCompleted(it)
                    is RAEvent.OnAchievementProgressHidden -> onAchievementProgressHidden(it.achievementId)
                    is RAEvent.OnPendingSubmissionAdded -> onPendingRaSubmissionAdded(it)
                    is RAEvent.OnPendingSubmissionResolved -> onPendingRaSubmissionResolved(it)
                    is RAEvent.OnPendingSubmissionBarrier -> onPendingRaSubmissionBarrier(it)
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
            }.combine(isRetroArchShaderDeclinedForSession) { configuration, declined ->
                configuration to declined
            }.collectLatest { (requested, declined) ->
                val it = MultiplayerSession.configureRenderer(requested)
                val dropShader = declined && it.videoFiltering == VideoFiltering.RETROARCH
                _runtimeRendererConfiguration.value = RuntimeRendererConfiguration(
                    renderer = it.renderer,
                    videoFiltering = if (dropShader) VideoFiltering.NONE else it.videoFiltering,
                    resolutionScaling = it.resolutionScaling,
                    retroArchShader = if (dropShader) it.retroArchShader.copy(presetPath = null) else it.retroArchShader,
                )
            }
        }
    }

    private suspend fun confirmRetroArchShaderCompile(romConfig: RomConfig?) {
        if (isRetroArchShaderDeclinedForSession.value) {
            return
        }

        val configuration = runCatching {
            if (romConfig == null) {
                settingsRepository.observeRenderConfiguration().first()
            } else {
                settingsRepository.observeRenderConfiguration(romConfig).first()
            }
        }.getOrNull()?.let(MultiplayerSession::configureRenderer) ?: return

        if (configuration.videoFiltering != VideoFiltering.RETROARCH) {
            return
        }
        val presetPath = configuration.retroArchShader.presetPath?.takeIf { it.isNotBlank() } ?: return

        val backend = if (configuration.renderer == VideoRenderer.VULKAN) {
            ShaderCompileTimeStore.Backend.VULKAN
        } else {
            ShaderCompileTimeStore.Backend.OPEN_GL
        }
        val measured = shaderCompileTimeStore.measured(presetPath, backend)
        val expectedMillis = measured ?: configuration.retroArchShader.estimatedCompileMillis
        if (expectedMillis < heavyShaderCompileThresholdMs) {
            return
        }

        val response = CompletableDeferred<Boolean>()
        val request = HeavyShaderCompileRequest(
            presetName = presetPath.substringAfterLast('/'),
            estimatedMillis = expectedMillis,
            isMeasured = measured != null,
            response = response,
        )
        if (!_heavyShaderCompileRequest.tryEmit(request)) {
            return
        }
        if (!response.await()) {
            isRetroArchShaderDeclinedForSession.value = true
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

    private suspend fun getRomSaveStateSlots(rom: Rom): List<SaveStateSlot> = withContext(Dispatchers.IO) {
        saveStatesRepository.getRomSaveStates(rom)
    }

    private suspend fun getRomQuickSaveStateSlot(rom: Rom): SaveStateSlot = withContext(Dispatchers.IO) {
        saveStatesRepository.getRomQuickSaveStateSlot(rom)
    }

    private suspend fun getRomSaveStateUri(rom: Rom, slot: SaveStateSlot): Uri = withContext(Dispatchers.IO) {
        saveStatesRepository.getRomSaveStateUri(rom, slot)
    }

    fun isSustainedPerformanceModeEnabled(): Boolean {
        return settingsRepository.isSustainedPerformanceModeEnabled()
    }

    fun getFpsCounterPosition(): FpsCounterPosition {
        return settingsRepository.getFpsCounterPosition()
    }

    fun getRenderedIrPosition(): FpsCounterPosition {
        return settingsRepository.getRenderedIrPosition()
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
            "source=bootstrap stage=game_not_found runtime=disabled game_hash=redacted",
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
                "game_hash=redacted " +
                "hardcore=${launchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE} " +
                "unofficial=${settingsRepository.areRetroAchievementsUnofficialAchievementsEnabled()} " +
                "encore=${settingsRepository.isRetroAchievementsEncoreModeEnabled()} " +
                "host_source=${if (launchDecision.usesProxyBackend) "raofflineproxy" else "official"} " +
                "native_client_host_configured=${launchDecision.nativeClientHost.isNotBlank()} " +
                "endpoint_generation=${launchDecision.endpointGeneration}",
        )
        return RARuntimeBridgeConfig(
            runtimeMode = RARuntimeBridgeMode.RC_CLIENT_ONLINE,
            userAgent = retroAchievementsUserAgent,
            username = userAuth.username,
            apiToken = userAuth.token,
            gameHash = rom.retroAchievementsHash,
            gameId = currentRetroAchievementsGameId,
            submissionSessionId = RaSubmissionSessionIdGenerator.next(),
            hardcoreEnabled = launchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE,
            unofficialEnabled = settingsRepository.areRetroAchievementsUnofficialAchievementsEnabled(),
            encoreEnabled = settingsRepository.isRetroAchievementsEncoreModeEnabled(),
            apiHost = launchDecision.nativeClientHost,
            usesProxyHost = launchDecision.usesProxyBackend,
            endpointGeneration = launchDecision.endpointGeneration,
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
            append(" game_hash=redacted")
            append(" achievements=").append(achievementData.lockedAchievements.size)
            append(" leaderboards=").append(achievementData.leaderboards.size)
            append(" has_rich_presence=").append(achievementData.richPresencePatch != null)
            append(" status=").append(achievementData.retroAchievementsIntegrationStatus.name)
            throwable?.let {
                append(" error=").append(it.javaClass.simpleName)
            }
        }

        if (throwable == null) {
            Log.i(RA_IDENTITY_TAG, message)
        } else {
            Log.w(RA_IDENTITY_TAG, message)
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

    private fun transitionRaHardcoreContinuity(
        event: RaHardcoreContinuityEvent,
        reason: String,
    ) {
        val previous = _raHardcoreContinuityState.value
        val next = RaHardcoreContinuityStateMachine.reduce(previous, event)
        if (next == previous) return

        _raHardcoreContinuityState.value = next
        logRaSubmission(
            "ra_hardcore_continuity_transition",
            "from" to previous.name.lowercase(),
            "to" to next.name.lowercase(),
            "reason" to reason,
            "hardcore" to emulatorSession.isRetroAchievementsHardcoreModeEnabled,
        )
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
        val runtimePathAtReceipt = activeRuntimePath
        val runtimeConfigAtReceipt = activeRuntimeBridgeConfig
        sessionCoroutineScope.launch {
            if (runtimePathAtReceipt == RetroAchievementsRuntimePath.DISABLED) {
                completeAchievementSubmissionTrace(achievementId, "runtime_disabled")
                return@launch
            }
            logRaTrace(
                "achievement_trigger_received",
                "achievement_id" to achievementId,
                "network_mode" to retroAchievementsNetworkMode.name,
                "session_mode" to retroAchievementsSessionMode.name,
                "online" to networkStatusProvider.isOnline(),
            )
            val achievement = retroAchievementsRepository.getAchievement(achievementId).getOrNull()
            if (
                activeRuntimePath != runtimePathAtReceipt ||
                activeRuntimeBridgeConfig !== runtimeConfigAtReceipt
            ) {
                completeAchievementSubmissionTrace(achievementId, "stale_runtime_event")
                return@launch
            }
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

            if (runtimePathAtReceipt == RetroAchievementsRuntimePath.RC_CLIENT) {
                check(
                    AchievementSubmissionOwnership.dispatch(
                        owner = AchievementSubmissionOwnership.Owner.RC_CLIENT,
                        submitFromKotlin = {
                            error("rc_client achievement ownership cannot invoke Kotlin submit")
                        },
                    ) == AchievementSubmissionOwnership.Action.RUNTIME_OWNS_SUBMIT,
                )
                if (achievement != null) {
                    val isHardcoreModeEnabled = emulatorSession.isRetroAchievementsHardcoreModeEnabled
                    logRaSubmission(
                        "achievement_submit_expected",
                        "achievement_id" to achievementId,
                        "submit_path" to "rc_client_http",
                        "expected_api" to "awardachievement",
                        "game_id" to achievement.gameId.id,
                        "game_hash" to currentRom?.retroAchievementsHash,
                        "hardcore" to isHardcoreModeEnabled,
                    )
                    _achievementsEvent.emit(RAEventUi.AchievementTriggered(achievement))
                }
                completeAchievementSubmissionTrace(achievementId, "owned_by_rc_client")
                return@launch
            }

            if (retroAchievementsNetworkMode == RetroAchievementsNetworkMode.ONLINE_LIVE && !networkStatusProvider.isLikelyOnline()) {
                transitionToOfflineAccumulationIfNeeded()
            }

            if (
                retroAchievementsNetworkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING ||
                retroAchievementsNetworkMode == RetroAchievementsNetworkMode.RECONCILING_RA_SUBMISSIONS
            ) {
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
                handleOfflineAchievementTriggered(
                    achievementId = achievementId,
                    achievement = achievement,
                    authentication = runtimeAuthenticationSnapshot(runtimeConfigAtReceipt),
                )
                return@launch
            }

            if (achievement != null) {
                val isHardcoreModeEnabled = emulatorSession.isRetroAchievementsHardcoreModeEnabled

                if (!ensureAchievementSubmitContext(achievement)) {
                    completeAchievementSubmissionTrace(achievementId, "context_mismatch")
                    return@launch
                }

                val submissionAuthentication =
                    runtimeAuthenticationSnapshot(runtimeConfigAtReceipt)
                        ?: run {
                            completeAchievementSubmissionTrace(achievementId, "missing_runtime_authentication")
                            return@launch
                        }
                if (isHardcoreModeEnabled) {
                    handleHardcoreAchievementTriggered(
                        achievement = achievement,
                        authentication = submissionAuthentication,
                    )
                } else {
                    logRaTrace(
                        "achievement_submit_attempt",
                        "achievement_id" to achievementId,
                        "hardcore" to false,
                        "game_id" to currentRetroAchievementsGameId,
                    )
                    retroAchievementsSubmissionHandler.addPendingAchievementSubmission(
                        achievement = achievement,
                        forHardcoreMode = false,
                        authentication = submissionAuthentication,
                    )
                }
            } else {
                completeAchievementSubmissionTrace(achievementId, "achievement_missing")
            }
        }
    }

    private suspend fun transitionToOfflineAccumulationIfNeeded() {
        if (activeRuntimeBridgeConfig?.usesProxyHost == true) {
            logRaTrace(
                "network_transition_owned_by_raofflineproxy",
                "built_in_ledger" to false,
                "built_in_sync" to false,
            )
            return
        }
        if (
            retroAchievementsNetworkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING ||
            retroAchievementsNetworkMode == RetroAchievementsNetworkMode.RECONCILING_RA_SUBMISSIONS
        ) {
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
        if (
            isHardcoreEligibleAfterOnlineStart &&
            startedSessionOnlineLive &&
            activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT
        ) {
            transitionRaHardcoreContinuity(
                RaHardcoreContinuityEvent.NetworkLost(
                    pendingTotal = pendingRaSubmissionStore?.snapshot?.value?.counts?.total ?: 0,
                ),
                reason = "network_lost",
            )
        }
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
        } else {
            val pending = pendingRaSubmissionStore?.snapshot?.value?.counts?.total ?: 0
            if (pending > 0) {
                _toastEvent.tryEmit(ToastEvent.HardcoreOfflineUnsyncedWarning(pending))
            }
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

    private suspend fun handleHardcoreAchievementTriggered(
        achievement: me.magnum.rcheevosapi.model.RAAchievement,
        authentication: RAUserAuth.Authenticated,
    ) {
        if (networkStatusProvider.isLikelyOnline()) {
            drainHardcoreSubmissions()
        }

        logRaTrace(
            "hardcore_award_attempt",
            "achievement_id" to achievement.id,
            "game_id" to currentRetroAchievementsGameId,
            "online" to networkStatusProvider.isLikelyOnline(),
        )

        val awardResult = retroAchievementsRepository.awardAchievementForAuthentication(
            achievement = achievement,
            forHardcoreMode = true,
            expectedAuthentication = authentication,
        )
        _achievementsEvent.emit(RAEventUi.AchievementTriggered(achievement))

        if (awardResult.isSuccess) {
            logRaTrace(
                "hardcore_award_success",
                "achievement_id" to achievement.id,
                "awarded" to (awardResult.getOrNull()?.achievementAwarded ?: false),
            )
            completeAchievementSubmissionTrace(achievement.id, "submit_success")
            drainHardcoreSubmissions()
        } else {
            logRaTrace(
                "hardcore_award_failed",
                "achievement_id" to achievement.id,
                "error" to (awardResult.exceptionOrNull()?.javaClass?.simpleName ?: "unknown"),
            )
            addHardcoreSubmission(achievement, authentication)
            _achievementsEvent.emit(RAEventUi.AchievementTriggerError(achievement))
            completeAchievementSubmissionTrace(achievement.id, "submit_failed_queued")
        }
    }

    private suspend fun handleOfflineAchievementTriggered(
        achievementId: Long,
        achievement: me.magnum.rcheevosapi.model.RAAchievement?,
        authentication: RAUserAuth.Authenticated?,
    ) {
        if (isHardcoreEligibleAfterOnlineStart && achievement != null) {
            if (networkStatusProvider.isLikelyOnline()) {
                if (authentication != null) {
                    handleHardcoreAchievementTriggered(achievement, authentication)
                }
                return
            }

            if (authentication == null || !addHardcoreSubmission(achievement, authentication)) {
                logRaTrace(
                    "hardcore_unlock_queue_rejected",
                    "achievement_id" to achievementId,
                    "reason" to "runtime_authentication_mismatch",
                )
                return
            }
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
                    "error" to error.javaClass.simpleName,
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
        )
        logRaTrace(
            "runtime_server_error",
            "api" to event.api,
            "related_id" to event.relatedId,
            "result_code" to event.resultCode,
        )

        if (
            activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT ||
            activeRuntimePath == RetroAchievementsRuntimePath.DISABLED
        ) {
            logRaSubmission(
                "runtime_server_error_not_owned_by_kotlin",
                "api" to event.api,
                "related_id" to event.relatedId,
                "result_code" to event.resultCode,
                "kotlin_submit" to false,
                "runtime_path" to activeRuntimePath.traceValue,
            )
            return
        }

        if (event.api.equals("awardachievement", ignoreCase = true) && event.relatedId > 0L) {
            val achievementId = event.relatedId
            sessionCoroutineScope.launch {
                val achievement = retroAchievementsRepository.getAchievement(achievementId).getOrNull()
                    ?: return@launch
                val isHardcore = emulatorSession.isRetroAchievementsHardcoreModeEnabled
                if (isHardcore) {
                    val authentication = runtimeAuthenticationSnapshot()
                    if (
                        authentication == null ||
                        !addHardcoreSubmission(achievement, authentication)
                    ) {
                        logRaTrace(
                            "rc_client_submit_failed_queue_rejected",
                            "achievement_id" to achievementId,
                            "reason" to "runtime_authentication_mismatch",
                        )
                        return@launch
                    }
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
        if (
            !settingsRepository.isRetroAchievementsOfflineSoftcoreEnabled() ||
            !retroAchievementsEndpointProvider.routingSnapshot().builtInLedgerEnabled
        ) {
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

    private suspend fun onRuntimeDisconnected() {
        logRaTrace("runtime_disconnected")
        pendingRaReconnectGate.onDisconnected()
        transitionToOfflineAccumulationIfNeeded()
    }

    private fun onRuntimeReconnected() {
        logRaTrace("runtime_reconnected")
        requestPendingRuntimeReconnect()
    }

    private fun requestPendingRuntimeReconnect() {
        val reconnectRequests = pendingRaReconnectRequests ?: return
        if (pendingRaReconnectGate.consumeReconnect()) {
            reconnectRequests.trySend(Unit)
        }
    }

    private suspend fun initializePendingRaSubmissionSession(
        runtimeConfig: RARuntimeBridgeConfig,
        rom: Rom,
    ) {
        if (
            !runtimeConfig.hardcoreEnabled ||
            runtimeConfig.runtimeMode != RARuntimeBridgeMode.RC_CLIENT_ONLINE
        ) {
            return
        }

        val userId = runtimeConfig.username?.takeIf(String::isNotBlank) ?: return
        val contentHash = runtimeConfig.gameHash?.takeIf(String::isNotBlank) ?: return
        val gameId = runtimeConfig.gameId ?: currentRetroAchievementsGameId ?: return
        val nativeSessionId = runtimeConfig.submissionSessionId.takeIf { it > 0 } ?: return
        val sessionContext = RaSubmissionContext(
            userId = userId,
            gameId = gameId,
            contentHash = contentHash,
            sessionId = UUID.randomUUID().toString(),
            nativeSessionId = nativeSessionId,
        )
        clearPendingRaSubmissionSession(
            reason = "session_reinitialized",
            clearLossMarker = true,
        )

        val store = PendingRaSubmissionStore(sessionContext)
        val coordinator = RaPendingSubmissionSyncCoordinator(
            store = store,
            operationScope = sessionCoroutineScope,
        ) { expectedSubmissions ->
            val expectedNativeSubmissionIds = expectedSubmissions.map { it.nativeSubmissionId }
            val expectedTypesByNativeId = expectedSubmissions.associate {
                it.nativeSubmissionId to it.type
            }
            val nativeResult = emulatorManager.retryPendingRetroAchievementsSubmissions(
                expectedNativeSubmissionIds,
            )
            RaNativeRetryResultMapper.map(
                nativeResult = nativeResult,
                expectedSessionId = store.context.nativeSessionId,
                expectedTypesByNativeId = expectedTypesByNativeId,
            )
        }
        val reconnectRequests = Channel<Unit>(capacity = Channel.CONFLATED)
        pendingRaSubmissionStore = store
        pendingRaSyncCoordinator = coordinator
        pendingRaReconnectRequests = reconnectRequests
        pendingRaReconnectGate.reset()
        pendingRaSubmissionBarrier.value = null
        _pendingRaSubmissionSnapshot.value = store.snapshot.value
        transitionRaHardcoreContinuity(
            RaHardcoreContinuityEvent.SessionReset,
            reason = "hardcore_online_session_initialized",
        )

        pendingRaSessionJob = sessionCoroutineScope.launch {
            coroutineScope {
                launch {
                    store.snapshot.collect { snapshot ->
                        _pendingRaSubmissionSnapshot.value = snapshot
                        transitionRaHardcoreContinuity(
                            RaHardcoreContinuityEvent.PendingChanged(
                                pendingTotal = snapshot.counts.total,
                                networkAvailable = networkStatusProvider.isLikelyOnline(),
                            ),
                            reason = "pending_snapshot_changed",
                        )
                        if (snapshot.counts.total == 0) {
                            hardcoreOfflineLossTracker.clearPendingUnlocks(
                                snapshot.context.userId,
                                snapshot.context.contentHash,
                            )
                            if (
                                pendingRaRuntimeDisableGate.consumeWhenEmpty(
                                    snapshot.counts.total,
                                )
                            ) {
                                finishDeferredRetroAchievementsDisable(store)
                                return@collect
                            }
                            if (
                                networkStatusProvider.isLikelyOnline() &&
                                pendingContextMatchesActiveRuntime(snapshot.context)
                            ) {
                                retroAchievementsNetworkMode = RetroAchievementsNetworkMode.ONLINE_LIVE
                                emulatorSession.updateRetroAchievementsOfflineModeEnabled(false)
                            }
                        } else {
                            hardcoreOfflineLossTracker.markPendingSubmissions(
                                userId = snapshot.context.userId,
                                contentId = snapshot.context.contentHash,
                                gameTitle = rom.name,
                                achievementCount = snapshot.counts.achievementUnlocks,
                                leaderboardCount = snapshot.counts.leaderboardEntries,
                            )
                        }
                    }
                }
                launch {
                    networkConnectivityObserver.networkState.collect { networkState ->
                        when (networkState) {
                            NetworkConnectivityObserver.NetworkState.DISCONNECTED -> {
                                pendingRaReconnectGate.onDisconnected()
                                transitionToOfflineAccumulationIfNeeded()
                            }
                            NetworkConnectivityObserver.NetworkState.CONNECTED -> {
                                requestPendingRuntimeReconnect()
                            }
                        }
                    }
                }
                launch {
                    for (ignored in reconnectRequests) {
                        delay(750.milliseconds)
                        handleValidatedRuntimeReconnect()
                    }
                }
            }
        }
    }

    private fun finishDeferredRetroAchievementsDisable(
        expectedStore: PendingRaSubmissionStore,
    ) {
        sessionCoroutineScope.launch {
            emulatorManager.pauseEmulator()
            val mirrorIsCurrent = refreshPendingRaSubmissionMirror()
            if (
                mirrorIsCurrent &&
                pendingRaSubmissionStore === expectedStore &&
                expectedStore.snapshot.value.counts.total == 0
            ) {
                dispatchSessionUpdateActions(
                    emulatorSession.updateRetroAchievementsSettings(
                        areRetroAchievementsEnabled = false,
                        isHardcoreModeEnabled =
                            settingsRepository.isRetroAchievementsHardcoreEnabled(),
                    ),
                )
            } else {
                pendingRaRuntimeDisableGate.update(true)
                if (!mirrorIsCurrent) {
                    _toastEvent.tryEmit(ToastEvent.PendingRaStateVerificationFailed)
                }
            }
            resumeEmulatorIfSessionCanRun()
        }
    }

    private suspend fun clearPendingRaSubmissionSession(
        reason: String,
        clearLossMarker: Boolean,
    ) {
        val store = detachPendingRaSubmissionSession()
        val discarded = store?.cleanup() ?: 0
        if (store != null && clearLossMarker) {
            hardcoreOfflineLossTracker.clearPendingUnlocks(
                store.context.userId,
                store.context.contentHash,
            )
        }
        if (store != null) {
            logRaSubmission(
                "ra_pending_session_cleared",
                "reason" to reason,
                "discarded" to discarded,
                "accepted" to false,
            )
        }
    }

    private fun detachPendingRaSubmissionSession(): PendingRaSubmissionStore? {
        pendingRaSessionJob?.cancel()
        pendingRaSessionJob = null
        pendingRaReconnectRequests?.close()
        pendingRaReconnectRequests = null
        pendingRaReconnectGate.reset()
        pendingRaSyncCoordinator?.close()
        pendingRaSyncCoordinator = null
        val store = pendingRaSubmissionStore
        pendingRaSubmissionStore = null
        _pendingRaSubmissionSnapshot.value = null
        pendingRaSubmissionBarrier.value = null
        pendingRaRuntimeDisableGate.reset()
        return store
    }

    private fun isInGameRetroAchievementsLogoutSupported(): Boolean {
        val runtimeConfig = activeRuntimeBridgeConfig ?: return false
        val store = pendingRaSubmissionStore ?: return false
        return activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT &&
            runtimeConfig.runtimeMode == RARuntimeBridgeMode.RC_CLIENT_ONLINE &&
            runtimeConfig.hardcoreEnabled &&
            emulatorSession.isRetroAchievementsHardcoreModeEnabled &&
            startedSessionOnlineLive &&
            !store.snapshot.value.closed &&
            pendingContextMatchesRuntimeSession(store.context)
    }

    private suspend fun prepareCurrentPendingRaSubmissionsForLogout(
        expectedRuntimeConfig: RARuntimeBridgeConfig,
        expectedStore: PendingRaSubmissionStore,
    ): List<Long>? {
        if (
            activeRuntimeBridgeConfig !== expectedRuntimeConfig ||
            pendingRaSubmissionStore !== expectedStore ||
            !isInGameRetroAchievementsLogoutSupported() ||
            !pendingContextMatchesActiveRuntime(expectedStore.context)
        ) {
            return null
        }
        if (!refreshPendingRaSubmissionMirror()) {
            return null
        }
        if (
            activeRuntimeBridgeConfig !== expectedRuntimeConfig ||
            pendingRaSubmissionStore !== expectedStore ||
            !pendingContextMatchesActiveRuntime(expectedStore.context)
        ) {
            return null
        }

        val expectedSubmissionIds = expectedStore.snapshot.value.records
            .map { it.submission.nativeSubmissionId }
        if (
            expectedSubmissionIds.any { it <= 0L } ||
            expectedSubmissionIds.distinct().size != expectedSubmissionIds.size
        ) {
            logRaSubmission(
                "ra_pending_discard_rejected",
                "reason" to "invalid_submission_ids",
                "discard_reason" to "logout",
                "accepted" to false,
            )
            return null
        }
        return expectedSubmissionIds
    }

    private suspend fun discardPreparedPendingRaSubmissionsForLogout(
        expectedStore: PendingRaSubmissionStore,
        expectedSubmissionIds: List<Long>,
    ): Int {
        val confirmedNativeDiscardCount =
            emulatorManager.discardPendingRetroAchievementsSubmissions(
                expectedSubmissionIds,
            )
        if (confirmedNativeDiscardCount == expectedSubmissionIds.size) {
            val discardedFromMirror = expectedStore.discardByNativeSubmissionIds(
                nativeSubmissionIds = expectedSubmissionIds.toSet(),
                requestedContext = expectedStore.context,
            )
            if (discardedFromMirror == confirmedNativeDiscardCount) {
                hardcoreOfflineLossTracker.clearPendingUnlocks(
                    expectedStore.context.userId,
                    expectedStore.context.contentHash,
                )
            } else {
                logRaSubmission(
                    "ra_pending_discard_mirror_mismatch",
                    "confirmed" to confirmedNativeDiscardCount,
                    "discarded" to discardedFromMirror,
                    "discard_reason" to "logout",
                    "accepted" to false,
                )
            }
        }
        logRaSubmission(
            if (confirmedNativeDiscardCount == expectedSubmissionIds.size) {
                "ra_pending_discarded"
            } else {
                "ra_pending_discard_rejected"
            },
            "expected" to expectedSubmissionIds.size,
            "confirmed" to confirmedNativeDiscardCount,
            "remaining" to expectedStore.snapshot.value.counts.total,
            "discard_reason" to "logout",
            "accepted" to false,
        )
        return confirmedNativeDiscardCount
    }

    private suspend fun refreshPendingRaSubmissionMirror(): Boolean {
        val store = pendingRaSubmissionStore ?: return true
        if (
            activeRuntimePath != RetroAchievementsRuntimePath.RC_CLIENT ||
            !pendingContextMatchesRuntimeSession(store.context)
        ) {
            return false
        }

        val requestedBarrierId = emulatorManager.refreshPendingRetroAchievementsSubmissions()
        if (requestedBarrierId <= 0L) {
            logRaSubmission(
                "ra_pending_refresh_failed",
                "reason" to "native_refresh_rejected",
                "submit_owner" to "rc_client",
            )
            return false
        }

        val observed = try {
            withTimeout(RA_PENDING_BARRIER_TIMEOUT_MS) {
                pendingRaSubmissionBarrier
                    .filterNotNull()
                    .first {
                        it.submissionSessionId == store.context.nativeSessionId &&
                            it.barrierId >= requestedBarrierId
                    }
            }
            true
        } catch (_: TimeoutCancellationException) {
            false
        }
        val contextStillMatches =
            pendingRaSubmissionStore === store &&
                pendingContextMatchesRuntimeSession(store.context)
        logRaSubmission(
            if (observed && contextStillMatches) {
                "ra_pending_refresh_completed"
            } else {
                "ra_pending_refresh_failed"
            },
            "barrier_id" to requestedBarrierId,
            "pending_total" to store.snapshot.value.counts.total,
            "reason" to when {
                !observed -> "barrier_timeout"
                !contextStillMatches -> "context_changed"
                else -> null
            },
            "submit_owner" to "rc_client",
        )
        return observed && contextStillMatches
    }

    private fun onPendingRaSubmissionBarrier(event: RAEvent.OnPendingSubmissionBarrier) {
        val store = pendingRaSubmissionStore
        if (
            store == null ||
            event.submissionSessionId != store.context.nativeSessionId ||
            !pendingContextMatchesRuntimeSession(store.context)
        ) {
            logRaSubmission(
                "ra_pending_barrier_rejected",
                "barrier_id" to event.barrierId,
                "reason" to "submission_session_mismatch",
            )
            return
        }
        pendingRaSubmissionBarrier.value = PendingRaSubmissionBarrier(
            submissionSessionId = event.submissionSessionId,
            barrierId = event.barrierId,
        )
    }

    private suspend fun onPendingRaSubmissionAdded(event: RAEvent.OnPendingSubmissionAdded) {
        val store = pendingRaSubmissionStore
        if (
            store == null ||
            activeRuntimePath != RetroAchievementsRuntimePath.RC_CLIENT ||
            !startedSessionOnlineLive ||
            !emulatorSession.isRetroAchievementsHardcoreModeEnabled ||
            !event.hardcore ||
            event.submissionSessionId != store.context.nativeSessionId ||
            !pendingContextMatchesRuntimeSession(store.context)
        ) {
            logRaSubmission(
                "ra_pending_rejected",
                "submission_id" to event.nativeSubmissionId,
                "reason" to "invalid_session_context",
                "kotlin_submit" to false,
            )
            return
        }

        val stableSubmissionId = "${store.context.sessionId}:${event.nativeSubmissionId}"
        val submission = when (event.submissionType) {
            RaNativePendingSubmissionType.ACHIEVEMENT -> {
                if (event.achievementId <= 0L) return
                PendingRaSubmission.AchievementUnlock(
                    context = store.context,
                    submissionId = stableSubmissionId,
                    nativeSubmissionId = event.nativeSubmissionId,
                    sequence = event.sequence,
                    createdAtEpochMs = event.createdAtEpochMs,
                    hardcore = true,
                    achievementId = event.achievementId,
                )
            }
            RaNativePendingSubmissionType.LEADERBOARD -> {
                if (event.leaderboardId <= 0L || event.attemptId <= 0L) return
                PendingRaSubmission.LeaderboardEntry(
                    context = store.context,
                    submissionId = stableSubmissionId,
                    nativeSubmissionId = event.nativeSubmissionId,
                    sequence = event.sequence,
                    createdAtEpochMs = event.createdAtEpochMs,
                    hardcore = true,
                    leaderboardId = event.leaderboardId,
                    attemptId = event.attemptId,
                    rawScore = event.rawScore,
                    formattedScore = event.formattedScore,
                )
            }
        }

        val addResult = store.add(submission)
        val counts = store.snapshot.value.counts
        logRaSubmission(
            "ra_pending_added",
            "submission_type" to submission.type.name.lowercase(),
            "submission_id" to event.nativeSubmissionId,
            "achievement_id" to (submission as? PendingRaSubmission.AchievementUnlock)?.achievementId,
            "leaderboard_id" to (submission as? PendingRaSubmission.LeaderboardEntry)?.leaderboardId,
            "attempt_id" to (submission as? PendingRaSubmission.LeaderboardEntry)?.attemptId,
            "raw_score" to (submission as? PendingRaSubmission.LeaderboardEntry)?.rawScore,
            "hardcore" to true,
            "submit_owner" to "rc_client",
            "pending_total" to counts.total,
            "add_result" to addResult.name.lowercase(),
            "kotlin_submit" to false,
        )
    }

    private suspend fun onPendingRaSubmissionResolved(event: RAEvent.OnPendingSubmissionResolved) {
        val store = pendingRaSubmissionStore ?: return
        if (event.submissionSessionId != store.context.nativeSessionId) {
            logRaSubmission(
                "ra_pending_resolution_rejected",
                "submission_id" to event.nativeSubmissionId,
                "reason" to "submission_session_mismatch",
            )
            return
        }
        val record = store.snapshot.value.records.firstOrNull {
            it.submission.nativeSubmissionId == event.nativeSubmissionId
        } ?: return
        val expectedType = when (record.submission.type) {
            RaPendingSubmissionType.ACHIEVEMENT -> RaNativePendingSubmissionType.ACHIEVEMENT
            RaPendingSubmissionType.LEADERBOARD -> RaNativePendingSubmissionType.LEADERBOARD
        }
        if (expectedType != event.submissionType) {
            logRaSubmission(
                "ra_pending_resolution_rejected",
                "submission_id" to event.nativeSubmissionId,
                "reason" to "submission_type_mismatch",
            )
            return
        }

        when (event.resolution) {
            RaNativePendingSubmissionResolution.ACCEPTED,
            RaNativePendingSubmissionResolution.ALREADY_ACCEPTED -> {
                val removed = store.acceptByNativeSubmissionId(event.nativeSubmissionId)
                if (removed && store.snapshot.value.counts.total == 0) {
                    hardcoreOfflineLossTracker.clearPendingUnlocks(
                        store.context.userId,
                        store.context.contentHash,
                    )
                }
            }
            RaNativePendingSubmissionResolution.PERMANENT_FAILURE -> {
                store.markPermanentFailureByNativeSubmissionId(event.nativeSubmissionId)
            }
            RaNativePendingSubmissionResolution.RETRYABLE_FAILURE -> {
                store.markRetryableByNativeSubmissionId(event.nativeSubmissionId)
            }
        }
        logRaSubmission(
            "ra_pending_resolved",
            "submission_type" to record.submission.type.name.lowercase(),
            "submission_id" to event.nativeSubmissionId,
            "resolution" to event.resolution.name.lowercase(),
            "result_code" to event.resultCode,
            "pending_total" to store.snapshot.value.counts.total,
            "submit_owner" to "rc_client",
        )
    }

    private suspend fun pendingContextMatchesActiveRuntime(context: RaSubmissionContext): Boolean {
        val runtimeConfig = activeRuntimeBridgeConfig ?: return false
        if (!activeRuntimeAuthenticationMatches(runtimeConfig)) {
            return false
        }
        return pendingContextMatchesRuntimeSession(context)
    }

    private suspend fun activeRuntimeAuthenticationMatches(
        runtimeConfig: RARuntimeBridgeConfig? = activeRuntimeBridgeConfig,
    ): Boolean {
        runtimeConfig ?: return false
        val authenticatedUser =
            retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated
        return RaRuntimeAuthenticationPolicy.matches(
            runtimeUserId = runtimeConfig.username,
            runtimeToken = runtimeConfig.apiToken,
            authenticatedUserId = authenticatedUser?.username,
            authenticatedToken = authenticatedUser?.token,
        )
    }

    private fun runtimeAuthenticationSnapshot(
        runtimeConfig: RARuntimeBridgeConfig? = activeRuntimeBridgeConfig,
    ): RAUserAuth.Authenticated? {
        val username = runtimeConfig?.username?.takeIf(String::isNotBlank) ?: return null
        val token = runtimeConfig.apiToken?.takeIf(String::isNotBlank) ?: return null
        return RAUserAuth.Authenticated(username, token)
    }

    private suspend fun acquireRuntimeAuthenticationLease(): String? {
        if (synchronized(runtimeAuthenticationLeaseMonitor) {
                activeRuntimeAuthenticationLeaseId != null
            }
        ) {
            return null
        }
        val authentication =
            retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated
                ?: return null
        val leaseId = UUID.randomUUID().toString()
        if (
            !retroAchievementsRepository.acquireRuntimeAuthenticationLease(
                leaseId = leaseId,
                expectedAuthentication = authentication,
            )
        ) {
            return null
        }
        synchronized(runtimeAuthenticationLeaseMonitor) {
            activeRuntimeAuthenticationLeaseId = leaseId
        }
        logRaSubmission(
            "ra_runtime_identity_lease_acquired",
            "runtime_path" to activeRuntimePath.traceValue,
        )
        return leaseId
    }

    private fun releaseRuntimeAuthenticationLease(
        leaseId: String,
        reason: String,
    ): Boolean {
        scheduleHardcoreSubmissionQueueTeardown(leaseId, reason)
        val released = synchronized(runtimeAuthenticationLeaseMonitor) {
            if (activeRuntimeAuthenticationLeaseId != leaseId) {
                return@synchronized null
            }
            val registryReleased =
                retroAchievementsRepository.releaseRuntimeAuthenticationLease(leaseId)
            if (registryReleased) {
                activeRuntimeAuthenticationLeaseId = null
                if (activeHardcoreSubmissionSessionId == leaseId) {
                    activeHardcoreSubmissionSessionId = null
                }
            }
            registryReleased
        }
        if (released == null) {
            logRaSubmission(
                "ra_runtime_identity_lease_release_ignored",
                "reason" to reason,
            )
            return false
        }
        logRaSubmission(
            if (released) {
                "ra_runtime_identity_lease_released"
            } else {
                "ra_runtime_identity_lease_release_failed"
            },
            "reason" to reason,
            "released" to released,
            "lease_retained" to !released,
        )
        return released
    }

    private fun unloadAndReleaseRuntimeAuthenticationLease(
        leaseId: String,
        reason: String,
        clearOwnedRuntimeState: () -> Unit = {},
    ): Boolean {
        scheduleHardcoreSubmissionQueueTeardown(leaseId, reason)
        var owned = false
        var unloadSucceeded = false
        var registryReleased = false
        var teardownError: Throwable? = null
        synchronized(runtimeAuthenticationLeaseMonitor) {
            if (activeRuntimeAuthenticationLeaseId != leaseId) {
                return@synchronized
            }
            owned = true
            try {
                emulatorManager.unloadRetroAchievementsData()
                unloadSucceeded = true
            } catch (throwable: Throwable) {
                teardownError = throwable
            }
            if (unloadSucceeded) {
                try {
                    clearOwnedRuntimeState()
                } catch (throwable: Throwable) {
                    teardownError = throwable
                }
                registryReleased =
                    retroAchievementsRepository.releaseRuntimeAuthenticationLease(leaseId)
                if (registryReleased) {
                    activeRuntimeAuthenticationLeaseId = null
                    if (activeHardcoreSubmissionSessionId == leaseId) {
                        activeHardcoreSubmissionSessionId = null
                    }
                }
            }
        }
        if (!owned) {
            logRaSubmission(
                "ra_runtime_identity_teardown_ignored",
                "reason" to reason,
            )
            return false
        }
        if (!unloadSucceeded) {
            logRaSubmission(
                "ra_runtime_identity_teardown_failed",
                "reason" to reason,
                "error" to teardownError?.javaClass?.simpleName,
                "lease_retained" to true,
            )
            return false
        }
        if (!registryReleased) {
            logRaSubmission(
                "ra_runtime_identity_teardown_failed",
                "reason" to reason,
                "error" to "LeaseReleaseRejected",
                "lease_retained" to true,
            )
            return false
        }
        logRaSubmission(
            "ra_runtime_identity_lease_released",
            "reason" to reason,
            "released" to registryReleased,
            "teardown_error" to teardownError?.javaClass?.simpleName,
        )
        return true
    }

    private fun scheduleHardcoreSubmissionQueueTeardown(
        leaseId: String,
        reason: String,
    ) {
        val teardown = synchronized(runtimeAuthenticationLeaseMonitor) {
            if (activeHardcoreSubmissionSessionId != leaseId) {
                return
            }
            activeHardcoreSubmissionSessionId = null
            val previous = hardcoreSubmissionQueueTeardown
            val completion = CompletableDeferred<Unit>()
            hardcoreSubmissionQueueTeardown = completion
            previous to completion
        }
        viewModelScope.launch(start = CoroutineStart.UNDISPATCHED) {
            try {
                withContext(NonCancellable) {
                    teardown.first?.await()
                    val discarded = hardcoreSubmissionQueue.discardAll(leaseId)
                    logRaSubmission(
                        "hardcore_queue_session_closed",
                        "reason" to reason,
                        "discarded" to discarded,
                    )
                }
            } finally {
                teardown.second.complete(Unit)
            }
        }
    }

    private suspend fun awaitHardcoreSubmissionQueueTeardown() {
        while (true) {
            val teardown = synchronized(runtimeAuthenticationLeaseMonitor) {
                hardcoreSubmissionQueueTeardown
            } ?: return
            teardown.await()
            if (
                synchronized(runtimeAuthenticationLeaseMonitor) {
                    hardcoreSubmissionQueueTeardown === teardown
                }
            ) {
                return
            }
        }
    }

    private fun unloadAndReleaseActiveRuntimeAuthenticationLease(reason: String) {
        val leaseId = synchronized(runtimeAuthenticationLeaseMonitor) {
            activeRuntimeAuthenticationLeaseId
        } ?: return
        unloadAndReleaseRuntimeAuthenticationLease(leaseId, reason)
    }

    private fun unloadAndHandoffRuntimeAuthenticationLeaseToLogout(
        leaseId: String,
    ): Boolean {
        var failureType: String? = null
        val handedOff = synchronized(runtimeAuthenticationLeaseMonitor) {
            if (activeRuntimeAuthenticationLeaseId != leaseId) {
                return@synchronized false
            }
            try {
                emulatorManager.unloadRetroAchievementsData()
            } catch (throwable: Throwable) {
                failureType = throwable.javaClass.simpleName
                return@synchronized false
            }
            if (!retroAchievementsRepository.handoffRuntimeAuthenticationLeaseToLogout(leaseId)) {
                failureType = "LeaseHandoffRejected"
                return@synchronized false
            }
            activeRuntimeAuthenticationLeaseId = null
            if (activeHardcoreSubmissionSessionId == leaseId) {
                activeHardcoreSubmissionSessionId = null
            }
            activeRuntimeBridgeConfig = null
            activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
            isRetroAchievementsOnlineSessionStarted = false
            true
        }
        logRaSubmission(
            if (handedOff) {
                "ra_runtime_identity_lease_handed_off"
            } else {
                "ra_runtime_identity_lease_handoff_failed"
            },
            "reason" to "logout",
            "error" to failureType,
            "lease_retained" to !handedOff,
        )
        return handedOff
    }

    private fun pendingContextMatchesRuntimeSession(context: RaSubmissionContext): Boolean {
        val runtimeConfig = activeRuntimeBridgeConfig ?: return false
        return RaSubmissionContextValidator.matches(
            pending = context,
            active = RaActiveSubmissionContext(
                isRcClientOnlineRuntime =
                    activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT &&
                        runtimeConfig.runtimeMode == RARuntimeBridgeMode.RC_CLIENT_ONLINE,
                runtimeHardcore = runtimeConfig.hardcoreEnabled,
                sessionHardcore = emulatorSession.isRetroAchievementsHardcoreModeEnabled,
                authenticatedUserId = runtimeConfig.username,
                authenticationTokenMatchesRuntime = !runtimeConfig.apiToken.isNullOrBlank(),
                runtimeUserId = runtimeConfig.username,
                runtimeGameId = runtimeConfig.gameId,
                activeGameId = currentRetroAchievementsGameId,
                runtimeContentHash = runtimeConfig.gameHash,
                activeContentHash = currentRom?.retroAchievementsHash,
                nativeSessionId = runtimeConfig.submissionSessionId,
            ),
        )
    }

    private suspend fun handleValidatedRuntimeReconnect() {
        val store = pendingRaSubmissionStore ?: return
        if (!pendingContextMatchesActiveRuntime(store.context)) {
            return
        }
        if (!networkStatusProvider.isOnline()) {
            pendingRaReconnectGate.onDisconnected()
            return
        }
        if (!refreshPendingRaSubmissionMirror()) {
            pendingRaReconnectGate.onDisconnected()
            return
        }
        if (store.snapshot.value.counts.total == 0) {
            retroAchievementsNetworkMode = RetroAchievementsNetworkMode.ONLINE_LIVE
            emulatorSession.updateRetroAchievementsOfflineModeEnabled(false)
            transitionRaHardcoreContinuity(
                RaHardcoreContinuityEvent.NetworkRestored(pendingTotal = 0),
                reason = "validated_runtime_reconnect",
            )
            return
        }
        transitionRaHardcoreContinuity(
            RaHardcoreContinuityEvent.NetworkRestored(
                pendingTotal = store.snapshot.value.counts.total,
            ),
            reason = "validated_runtime_reconnect_with_pending",
        )
        syncPendingRaSubmissions(RaPendingSyncSource.RUNTIME_RECONNECTED)
    }

    private suspend fun syncPendingRaSubmissions(
        source: RaPendingSyncSource,
    ): RaPendingSyncResult {
        val store = pendingRaSubmissionStore
        val coordinator = pendingRaSyncCoordinator
        val initialCounts = store?.snapshot?.value?.counts ?: RaPendingCounts.EMPTY
        if (
            store == null ||
            coordinator == null ||
            !pendingContextMatchesActiveRuntime(store.context) ||
            !networkStatusProvider.isOnline()
        ) {
            return RaPendingSyncResult(
                source = source,
                before = initialCounts,
                submittedAchievements = 0,
                submittedLeaderboardEntries = 0,
                alreadyAccepted = 0,
                failedAchievements = initialCounts.achievementUnlocks,
                failedLeaderboardEntries = initialCounts.leaderboardEntries,
                remaining = initialCounts,
                transientFailure = true,
            )
        }
        if (!refreshPendingRaSubmissionMirror()) {
            val remaining = store.snapshot.value.counts
            return RaPendingSyncResult(
                source = source,
                before = remaining,
                submittedAchievements = 0,
                submittedLeaderboardEntries = 0,
                alreadyAccepted = 0,
                failedAchievements = remaining.achievementUnlocks,
                failedLeaderboardEntries = remaining.leaderboardEntries,
                remaining = remaining,
                transientFailure = true,
            )
        }
        val before = store.snapshot.value.counts

        retroAchievementsNetworkMode = RetroAchievementsNetworkMode.RECONCILING_RA_SUBMISSIONS
        transitionRaHardcoreContinuity(
            RaHardcoreContinuityEvent.ReconciliationStarted(
                pendingTotal = before.total,
            ),
            reason = "sync_${source.name.lowercase()}_started",
        )
        logRaSubmission(
            "ra_sync_requested",
            "source" to source.name.lowercase(),
            "pending_achievements" to before.achievementUnlocks,
            "pending_leaderboards" to before.leaderboardEntries,
            "submit_owner" to "rc_client",
        )
        val result = try {
            coordinator.sync(source)
        } catch (cancellation: CancellationException) {
            val networkAvailable = networkStatusProvider.isLikelyOnline()
            val remaining = store.snapshot.value.counts
            retroAchievementsNetworkMode = if (networkAvailable) {
                RetroAchievementsNetworkMode.ONLINE_LIVE
            } else {
                RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING
            }
            emulatorSession.updateRetroAchievementsOfflineModeEnabled(!networkAvailable)
            transitionRaHardcoreContinuity(
                RaHardcoreContinuityEvent.ReconciliationFinished(
                    remainingTotal = remaining.total,
                    networkAvailable = networkAvailable,
                ),
                reason = "sync_${source.name.lowercase()}_cancelled",
            )
            logRaSubmission(
                "ra_sync_cancelled",
                "source" to source.name.lowercase(),
                "remaining_achievements" to remaining.achievementUnlocks,
                "remaining_leaderboards" to remaining.leaderboardEntries,
            )
            throw cancellation
        }
        val networkAvailable = networkStatusProvider.isOnline()
        if (networkAvailable) {
            retroAchievementsNetworkMode = RetroAchievementsNetworkMode.ONLINE_LIVE
            emulatorSession.updateRetroAchievementsOfflineModeEnabled(false)
            if (result.remaining.total == 0) {
                hardcoreOfflineLossTracker.clearPendingUnlocks(
                    store.context.userId,
                    store.context.contentHash,
                )
            }
        } else {
            retroAchievementsNetworkMode = RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING
            emulatorSession.updateRetroAchievementsOfflineModeEnabled(true)
        }
        transitionRaHardcoreContinuity(
            RaHardcoreContinuityEvent.ReconciliationFinished(
                remainingTotal = result.remaining.total,
                networkAvailable = networkAvailable,
            ),
            reason = "sync_${source.name.lowercase()}_finished",
        )
        logRaSubmission(
            "ra_sync_completed",
            "source" to source.name.lowercase(),
            "submitted_achievements" to result.submittedAchievements,
            "submitted_leaderboards" to result.submittedLeaderboardEntries,
            "already_accepted" to result.alreadyAccepted,
            "failed_achievements" to result.failedAchievements,
            "failed_leaderboards" to result.failedLeaderboardEntries,
            "remaining_achievements" to result.remaining.achievementUnlocks,
            "remaining_leaderboards" to result.remaining.leaderboardEntries,
            "remaining_permanent" to result.remaining.permanentFailures,
            "transition" to if (networkAvailable && result.remaining.total == 0) {
                "reconciling_to_online_live"
            } else if (networkAvailable) {
                "reconciling_to_pending_online"
            } else {
                "reconciling_to_pending_offline"
            },
        )
        return result
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
        if (!activeRuntimeAuthenticationMatches(runtimeConfig)) {
            logContextMismatch(
                "achievement",
                achievement.id,
                "runtime_authentication_mismatch",
                "achievement_game_id" to achievement.gameId.id,
            )
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
        if (!activeRuntimeAuthenticationMatches(runtimeConfig)) {
            logContextMismatch(
                "leaderboard",
                leaderboardId,
                "runtime_authentication_mismatch",
            )
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
        val submitPath = if (activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT) {
            "rc_client_http"
        } else {
            "kotlin_api"
        }
        val startedAt = pendingRuntimeAchievementTriggers.remove(achievementId)
        if (startedAt == null) {
            logRaTrace(
                "runtime_submit_orphan",
                "entity_type" to "achievement",
                "entity_id" to achievementId,
                "result" to result,
                "submit_path" to submitPath,
            )
            return
        }

        logRaTrace(
            "runtime_submit_resolved",
            "entity_type" to "achievement",
            "entity_id" to achievementId,
            "result" to result,
            "latency_ms" to (System.currentTimeMillis() - startedAt).coerceAtLeast(0L),
            "submit_path" to submitPath,
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

    private suspend fun onRcClientLeaderboardEvent(event: RAEvent) {
        if (event is RAEvent.OnLeaderboardRuntimeReset) {
            leaderboardTrackerUpdateLogLimiter.resetAll()
            leaderboardAttemptCoordinator.completeRuntimeReset(event.attemptFloor)
            logLeaderboardDiagnostic(
                "leaderboard_runtime_reset",
                "attempt_floor" to event.attemptFloor,
            )
            _achievementsEvent.emit(RAEventUi.Reset)
            return
        }

        if (
            activeRuntimePath != RetroAchievementsRuntimePath.RC_CLIENT &&
            activeRuntimePath != RetroAchievementsRuntimePath.RC_CLIENT_OFFLINE
        ) {
            val identity = leaderboardEventIdentity(event)
            val trackerLogDecision = (event as? RAEvent.OnLeaderboardAttemptUpdated)
                ?.takeUnless { it.trackerShown }
                ?.let { leaderboardTrackerUpdateLogLimiter.observe(it.leaderboardId, it.attemptId) }
            if (trackerLogDecision != null && !trackerLogDecision.shouldLog) {
                return
            }
            logLeaderboardDiagnostic(
                "leaderboard_event_ignored",
                "leaderboard_id" to identity?.leaderboardId,
                "attempt_id" to identity?.attemptId,
                "event_sequence" to identity?.eventSequence,
                "event" to event::class.simpleName,
                "reason" to "runtime_not_rc_client",
                "tracker_update_index" to trackerLogDecision?.updateIndex,
                "suppressed_updates" to trackerLogDecision?.suppressedUpdates,
            )
            return
        }

        val transition = leaderboardAttemptCoordinator.reduce(event)
        if (transition == null) {
            val identity = leaderboardEventIdentity(event)
            val trackerLogDecision = (event as? RAEvent.OnLeaderboardAttemptUpdated)
                ?.takeUnless { it.trackerShown }
                ?.let { leaderboardTrackerUpdateLogLimiter.observe(it.leaderboardId, it.attemptId) }
            if (trackerLogDecision != null && !trackerLogDecision.shouldLog) {
                return
            }
            logLeaderboardDiagnostic(
                "leaderboard_event_ignored",
                "leaderboard_id" to identity?.leaderboardId,
                "attempt_id" to identity?.attemptId,
                "event_sequence" to identity?.eventSequence,
                "event" to event::class.simpleName,
                "reason" to "stale_duplicate_or_terminal",
                "tracker_update_index" to trackerLogDecision?.updateIndex,
                "suppressed_updates" to trackerLogDecision?.suppressedUpdates,
            )
            return
        }

        val key = transition.key
        when (transition) {
            is LeaderboardAttemptCoordinator.Transition.Started -> {
                leaderboardTrackerUpdateLogLimiter.reset(key.leaderboardId, key.attemptId)
                logLeaderboardUiTransition(key, transition.event.eventSequence, "tracking")
                if (settingsRepository.areRetroAchievementsLeaderboardIndicatorsEnabled()) {
                    loadLeaderboardUiContext(key.leaderboardId)?.let { context ->
                        _achievementsEvent.emit(RAEventUi.LeaderboardAttemptStarted(key, context.leaderboard, context.gameIcon))
                    }
                }
            }
            is LeaderboardAttemptCoordinator.Transition.Updated -> {
                if (transition.event.trackerShown) {
                    leaderboardTrackerUpdateLogLimiter.reset(key.leaderboardId, key.attemptId)
                    logLeaderboardUiTransition(
                        key,
                        transition.event.eventSequence,
                        "tracker_show",
                        "tracker_display" to transition.event.formattedValue,
                    )
                } else {
                    val logDecision = leaderboardTrackerUpdateLogLimiter.observe(key.leaderboardId, key.attemptId)
                    if (logDecision.shouldLog) {
                        logLeaderboardUiTransition(
                            key,
                            transition.event.eventSequence,
                            "tracker_update",
                            "tracker_display" to transition.event.formattedValue,
                            "tracker_update_index" to logDecision.updateIndex,
                            "suppressed_updates" to logDecision.suppressedUpdates,
                        )
                    }
                }
                if (settingsRepository.areRetroAchievementsLeaderboardIndicatorsEnabled()) {
                    _achievementsEvent.emit(RAEventUi.LeaderboardAttemptUpdated(key, transition.event.formattedValue))
                }
            }
            is LeaderboardAttemptCoordinator.Transition.TrackerHidden -> {
                leaderboardTrackerUpdateLogLimiter.reset(key.leaderboardId, key.attemptId)
                logLeaderboardUiTransition(key, transition.event.eventSequence, "tracker_hidden")
                if (settingsRepository.areRetroAchievementsLeaderboardIndicatorsEnabled()) {
                    _achievementsEvent.emit(RAEventUi.LeaderboardTrackerHidden(key))
                }
            }
            is LeaderboardAttemptCoordinator.Transition.Canceled -> {
                leaderboardTrackerUpdateLogLimiter.reset(key.leaderboardId, key.attemptId)
                logLeaderboardUiTransition(key, transition.event.eventSequence, "canceled")
                _achievementsEvent.emit(
                    RAEventUi.LeaderboardAttemptCancelled(
                        leaderboardId = key.leaderboardId,
                        attemptKey = key,
                    )
                )
            }
            is LeaderboardAttemptCoordinator.Transition.Pending -> {
                logLeaderboardUiTransition(
                    key,
                    transition.event.eventSequence,
                    "pending",
                    "tracker_display" to transition.event.trackerDisplay,
                    "submit_owner" to "rc_client",
                    "kotlin_submit" to false,
                )
                val context = loadLeaderboardUiContext(key.leaderboardId)
                if (context == null) {
                    logLeaderboardUiTransition(
                        key,
                        transition.event.eventSequence,
                        "pending_metadata_unavailable",
                    )
                }
                _achievementsEvent.emit(
                    RAEventUi.LeaderboardSubmissionPending(
                        key = key,
                        title = context?.leaderboard?.title
                            ?: this@EmulatorViewModel.context.getString(R.string.leaderboard_generic_title, key.leaderboardId),
                        gameIcon = context?.gameIcon,
                        trackerDisplay = transition.event.trackerDisplay,
                    )
                )
            }
            is LeaderboardAttemptCoordinator.Transition.Scoreboard -> {
                val scoreboard = transition.event
                leaderboardTrackerUpdateLogLimiter.reset(key.leaderboardId, key.attemptId)
                logLeaderboardUiTransition(
                    key,
                    scoreboard.eventSequence,
                    "scoreboard_final",
                    "submitted_score" to scoreboard.submittedScore,
                    "best_score" to scoreboard.bestScore,
                    "rank" to scoreboard.newRank,
                    "num_entries" to scoreboard.numEntries,
                    "submit_owner" to "rc_client",
                    "kotlin_submit" to false,
                )
                val context = loadLeaderboardUiContext(key.leaderboardId)
                if (context == null) {
                    logLeaderboardUiTransition(
                        key,
                        scoreboard.eventSequence,
                        "scoreboard_metadata_unavailable",
                    )
                }
                _achievementsEvent.emit(
                    LeaderboardScoreboardUiMapper.map(
                        key = key,
                        scoreboard = scoreboard,
                        title = context?.leaderboard?.title
                            ?: this@EmulatorViewModel.context.getString(R.string.leaderboard_generic_title, key.leaderboardId),
                        gameIcon = context?.gameIcon,
                    )
                )
            }
            is LeaderboardAttemptCoordinator.Transition.Failed -> {
                leaderboardTrackerUpdateLogLimiter.reset(key.leaderboardId, key.attemptId)
                logLeaderboardUiTransition(
                    key,
                    transition.event.eventSequence,
                    "server_error",
                    "result_code" to transition.event.resultCode,
                    "submit_owner" to "rc_client",
                    "kotlin_submit" to false,
                )
                _achievementsEvent.emit(
                    RAEventUi.LeaderboardEntrySubmitError(
                        leaderboardId = key.leaderboardId,
                        attemptKey = key,
                        willRetryInBackground = false,
                    )
                )
            }
        }
    }

    private suspend fun loadLeaderboardUiContext(leaderboardId: Long): LeaderboardUiContext? {
        val leaderboard = retroAchievementsRepository.getLeaderboard(leaderboardId) ?: return null
        val setSummary = retroAchievementsRepository.getAchievementSetSummary(leaderboard.setId) ?: return null
        return LeaderboardUiContext(leaderboard, setSummary.iconUrl)
    }

    private data class LeaderboardEventIdentity(
        val leaderboardId: Long,
        val attemptId: Long,
        val eventSequence: Long,
    )

    private fun leaderboardEventIdentity(event: RAEvent): LeaderboardEventIdentity? {
        return when (event) {
            is RAEvent.OnLeaderboardAttemptStarted -> LeaderboardEventIdentity(event.leaderboardId, event.attemptId, event.eventSequence)
            is RAEvent.OnLeaderboardAttemptUpdated -> LeaderboardEventIdentity(event.leaderboardId, event.attemptId, event.eventSequence)
            is RAEvent.OnLeaderboardAttemptSubmitted -> LeaderboardEventIdentity(event.leaderboardId, event.attemptId, event.eventSequence)
            is RAEvent.OnLeaderboardScoreboard -> LeaderboardEventIdentity(event.leaderboardId, event.attemptId, event.eventSequence)
            is RAEvent.OnLeaderboardSubmissionFailed -> LeaderboardEventIdentity(event.leaderboardId, event.attemptId, event.eventSequence)
            is RAEvent.OnLeaderboardAttemptCancelled -> LeaderboardEventIdentity(event.leaderboardId, event.attemptId, event.eventSequence)
            is RAEvent.OnLeaderboardTrackerHidden -> LeaderboardEventIdentity(event.leaderboardId, event.attemptId, event.eventSequence)
            else -> null
        }
    }

    private fun logLeaderboardUiTransition(
        key: LeaderboardAttemptKey,
        eventSequence: Long,
        uiState: String,
        vararg fields: Pair<String, Any?>,
    ) {
        logLeaderboardDiagnostic(
            "leaderboard_ui_transition",
            "leaderboard_id" to key.leaderboardId,
            "attempt_id" to key.attemptId,
            "event_sequence" to eventSequence,
            "ui_state" to uiState,
            *fields,
        )
    }

    private fun logLeaderboardDiagnostic(eventType: String, vararg fields: Pair<String, Any?>) {
        if (!leaderboardDiagnosticsEnabled) return
        logRaSubmission(eventType, *fields)
    }

    private fun onLeaderboardAttemptCompleted(completionEvent: RAEvent.OnLeaderboardAttemptCompleted) {
        val owner = when (activeRuntimePath) {
            RetroAchievementsRuntimePath.RC_CLIENT,
            RetroAchievementsRuntimePath.RC_CLIENT_OFFLINE -> LeaderboardSubmissionOwnership.Owner.RC_CLIENT
            RetroAchievementsRuntimePath.LEGACY -> LeaderboardSubmissionOwnership.Owner.LEGACY
            RetroAchievementsRuntimePath.DISABLED -> LeaderboardSubmissionOwnership.Owner.NONE
        }
        val ownership = LeaderboardSubmissionOwnership.dispatch(owner, completionEvent) { legacySubmission ->
            sessionCoroutineScope.launch {
                submitLegacyLeaderboardCompletion(completionEvent, legacySubmission)
            }
        }
        when (ownership) {
            LeaderboardSubmissionOwnership.Action.RuntimeOwnsSubmit,
            LeaderboardSubmissionOwnership.Action.IgnoreProtocolMismatch -> {
                logLeaderboardDiagnostic(
                    "leaderboard_legacy_completion_ignored",
                    "leaderboard_id" to completionEvent.leaderboardId,
                    "runtime_path" to activeRuntimePath.traceValue,
                    "reason" to if (owner == LeaderboardSubmissionOwnership.Owner.RC_CLIENT) {
                        "rc_client_owns_submit"
                    } else {
                        "no_submit_owner"
                    },
                    "kotlin_submit" to false,
                )
                return
            }
            is LeaderboardSubmissionOwnership.Action.SubmitLegacy -> Unit
        }
    }

    private suspend fun submitLegacyLeaderboardCompletion(
        completionEvent: RAEvent.OnLeaderboardAttemptCompleted,
        ownership: LeaderboardSubmissionOwnership.Action.SubmitLegacy,
    ) {

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
            logRaTrace(
                "leaderboard_submit_skipped_offline",
                "leaderboard_id" to completionEvent.leaderboardId,
            )
            completeLeaderboardSubmissionTrace(completionEvent.leaderboardId, "offline_skipped")
            _achievementsEvent.emit(RAEventUi.LeaderboardAttemptCancelled(completionEvent.leaderboardId))
            return
        }

        if (!emulatorSession.areLeaderboardsEnabled()) {
            logRaTrace(
                "leaderboard_submit_skipped_mode",
                "leaderboard_id" to completionEvent.leaderboardId,
                "hardcore_enabled" to emulatorSession.isRetroAchievementsHardcoreModeEnabled,
            )
            completeLeaderboardSubmissionTrace(completionEvent.leaderboardId, "mode_skipped")
            _achievementsEvent.emit(RAEventUi.LeaderboardAttemptCancelled(completionEvent.leaderboardId))
            return
        }

        if (!ensureLeaderboardSubmitContext(completionEvent.leaderboardId)) {
            completeLeaderboardSubmissionTrace(completionEvent.leaderboardId, "context_mismatch")
            _achievementsEvent.emit(RAEventUi.LeaderboardAttemptCancelled(completionEvent.leaderboardId))
            return
        }

        if (emulatorSession.isRetroAchievementsHardcoreModeEnabled) {
            attemptSilentHardcoreReplayBeforeOnlineSubmission()
        }

        retroAchievementsRepository.getLeaderboard(completionEvent.leaderboardId)?.let { leaderboard ->
            val authentication = runtimeAuthenticationSnapshot()
            if (authentication == null) {
                completeLeaderboardSubmissionTrace(
                    completionEvent.leaderboardId,
                    "missing_runtime_authentication",
                )
                return
            }
            retroAchievementsSubmissionHandler.addPendingLegacyLeaderboardSubmission(
                leaderboard = leaderboard,
                value = ownership.value,
                formattedValue = ownership.formattedValue,
                authentication = authentication,
            )
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

        val drainResult = drainHardcoreSubmissions()
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
        val raUserName = activeRuntimeBridgeConfig?.username
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
        val previousBootstrapJob = raBootstrapJob
        val bootstrapJob = sessionCoroutineScope.launch(start = CoroutineStart.LAZY) {
            var acquiredLeaseId: String? = null
            try {
                previousBootstrapJob?.cancelAndJoin()
                awaitHardcoreSubmissionQueueTeardown()
                val runtimeLeaseId = acquireRuntimeAuthenticationLease()
                if (runtimeLeaseId == null) {
                    activeRuntimeBridgeConfig = null
                    activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                    emulatorSession.updateRetroAchievementsIntegrationStatus(
                        GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR,
                    )
                    logRaSubmission(
                        "ra_runtime_identity_lease_rejected",
                        "reason" to "authentication_mutation_or_mismatch",
                    )
                    _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(null))
                    bootstrapReady.complete(Unit)
                    return@launch
                }
                acquiredLeaseId = runtimeLeaseId
                awaitHardcoreSubmissionQueueTeardown()
                currentCoroutineContext().ensureActive()
                val leaseAuthentication =
                    retroAchievementsRepository.getUserAuthentication() as? RAUserAuth.Authenticated
                if (
                    leaseAuthentication == null ||
                    !hardcoreSubmissionQueue.beginSession(
                        runtimeLeaseId,
                        leaseAuthentication,
                    )
                ) {
                    releaseRuntimeAuthenticationLease(
                        runtimeLeaseId,
                        "hardcore_queue_session_rejected",
                    )
                    activeRuntimeBridgeConfig = null
                    activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                    emulatorSession.updateRetroAchievementsIntegrationStatus(
                        GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR,
                    )
                    logRaSubmission(
                        "ra_runtime_identity_lease_rejected",
                        "reason" to "hardcore_queue_not_empty_after_terminal_discard",
                    )
                    _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(null))
                    bootstrapReady.complete(Unit)
                    return@launch
                }
                synchronized(runtimeAuthenticationLeaseMonitor) {
                    activeHardcoreSubmissionSessionId = runtimeLeaseId
                }
                leaderboardAttemptCoordinator.beginRuntimeReset()
                leaderboardTrackerUpdateLogLimiter.resetAll()
                leaderboardDiagnosticsEnabled = settingsRepository.isRendererDebugToolsEnabled().firstOrNull() == true
                offlineRetroAchievementsSession = null
                activeRuntimeBridgeConfig = null
                activeRuntimePath = RetroAchievementsRuntimePath.DISABLED

                var effectiveLaunchDecision = launchDecision
                var networkMode = effectiveLaunchDecision.networkMode
                var offlineContext = if (networkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING) {
                    buildOfflineRetroAchievementsContext(rom)
                } else {
                    null
                }
                var onlineBootstrap = if (networkMode != RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING) {
                    withContext(Dispatchers.IO) { getRomAchievementData(rom) }
                } else {
                    null
                }

                if (
                    !effectiveLaunchDecision.usesProxyBackend &&
                    RaHardcoreLaunchPolicy.mustDowngradeHardcore(
                        hardcoreRequested =
                            effectiveLaunchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE,
                        bootstrapLoadedFromNetwork =
                            onlineBootstrap?.source == OnlineRetroAchievementsBootstrapSource.NETWORK,
                    )
                ) {
                    effectiveLaunchDecision = effectiveLaunchDecision.copy(
                        networkMode = RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING,
                        sessionMode = RetroAchievementsSessionMode.SOFTCORE,
                        initialOfflineType = OfflineUnlockType.OFFLINE_FROM_START,
                        isHardcoreEligibleAfterOnlineStart = false,
                        offlineDueToNoInternetAtStart = false,
                        hardcoreOfflineDisabled = true,
                    )
                    networkMode = effectiveLaunchDecision.networkMode
                    retroAchievementsNetworkMode = networkMode
                    retroAchievementsSessionMode = effectiveLaunchDecision.sessionMode
                    isHardcoreEligibleAfterOnlineStart = false
                    startedSessionOnlineLive = false
                    val offlineSoftcoreEnabled =
                        settingsRepository.isRetroAchievementsOfflineSoftcoreEnabled()
                    emulatorSession.startSession(
                        areRetroAchievementsEnabled = offlineSoftcoreEnabled,
                        isRetroAchievementsHardcoreModeEnabled = false,
                        sessionType = EmulatorSession.SessionType.RomSession(rom),
                    )
                    offlineContext = if (offlineSoftcoreEnabled) {
                        buildOfflineRetroAchievementsContext(rom)
                    } else {
                        null
                    }
                    onlineBootstrap = null
                    logRaTrace(
                        "ra_hardcore_online_bootstrap_rejected",
                        "reason" to "network_bootstrap_unavailable",
                        "fallback" to if (offlineContext != null) "softcore_offline" else "disabled",
                    )
                }

                val achievementData = when (networkMode) {
                    RetroAchievementsNetworkMode.ONLINE_LIVE,
                    RetroAchievementsNetworkMode.RECONCILING_RA_SUBMISSIONS -> onlineBootstrap?.achievementData
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

                    releaseRuntimeAuthenticationLease(runtimeLeaseId, "integration_disabled")
                    bootstrapReady.complete(Unit)
                    return@launch
                }

                val runtimeJob = launch {
                    // Wait until the emulator has actually started
                    val expectedRuntimePath = when (networkMode) {
                        RetroAchievementsNetworkMode.ONLINE_LIVE,
                        RetroAchievementsNetworkMode.RECONCILING_RA_SUBMISSIONS -> RetroAchievementsRuntimePath.RC_CLIENT
                        RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING -> RetroAchievementsRuntimePath.RC_CLIENT_OFFLINE
                    }
                    logRaRuntimeSetup(
                        stage = "waiting_for_running",
                        runtimePath = expectedRuntimePath,
                        achievementData = achievementData,
                    )
                    ensureEmulatorIsRunning().firstOrNull()

                    when (networkMode) {
                        RetroAchievementsNetworkMode.ONLINE_LIVE,
                        RetroAchievementsNetworkMode.RECONCILING_RA_SUBMISSIONS -> {
                            val runtimeConfig = buildOnlineRuntimeConfig(rom, effectiveLaunchDecision)
                            if (runtimeConfig?.runtimeMode != RARuntimeBridgeMode.RC_CLIENT_ONLINE) {
                                activeRuntimeBridgeConfig = null
                                activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                                releaseRuntimeAuthenticationLease(runtimeLeaseId, "missing_runtime_config")
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
                                "hardcore" to (effectiveLaunchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE),
                                "unofficial" to settingsRepository.areRetroAchievementsUnofficialAchievementsEnabled(),
                                "game_id" to currentRetroAchievementsGameId,
                            )

                            initializePendingRaSubmissionSession(runtimeConfig, rom)
                            try {
                                logRaRuntimeSetup(
                                    stage = "native_setup_start",
                                    runtimePath = activeRuntimePath,
                                    achievementData = achievementData,
                                    runtimeConfig = runtimeConfig,
                                )
                                emulatorManager.setupRetroAchievements(achievementData, runtimeConfig)
                            } catch (throwable: Throwable) {
                                val ownedRuntime =
                                    unloadAndReleaseRuntimeAuthenticationLease(
                                        runtimeLeaseId,
                                        "native_setup_failed",
                                    ) {
                                        activeRuntimeBridgeConfig = null
                                        activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                                        isRetroAchievementsOnlineSessionStarted = false
                                    }
                                if (!ownedRuntime) {
                                    if (throwable is CancellationException) {
                                        throw throwable
                                    }
                                    return@launch
                                }
                                clearPendingRaSubmissionSession(
                                    reason = "native_setup_failed",
                                    clearLossMarker = true,
                                )
                                if (throwable is CancellationException) {
                                    throw throwable
                                }
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
                                    "error" to throwable.javaClass.simpleName,
                                )

                                val responseTooLarge =
                                    (throwable as? AndroidEmulatorManager.RetroAchievementsSetupException)?.isResponseTooLarge == true
                                emulatorSession.updateRetroAchievementsIntegrationStatus(
                                    if (responseTooLarge) {
                                        GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR_RESPONSE_TOO_LARGE
                                    } else {
                                        GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR
                                    },
                                )
                                _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(achievementData.icon, responseTooLarge))
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
                            if (activeRuntimePath == RetroAchievementsRuntimePath.LEGACY) {
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
                            }
                            isRetroAchievementsOnlineSessionStarted = false
                            emitRetroAchievementsModeToast(
                                status = if (effectiveLaunchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE) {
                                    ToastEvent.RetroAchievementsModeStatus.HARDCORE
                                } else {
                                    ToastEvent.RetroAchievementsModeStatus.SOFTCORE
                                }
                            )
                            emitRetroAchievementsLoadedPopup(
                                achievementData,
                                hardcore = effectiveLaunchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE,
                            )

                            while (isActive) {
                                if (!networkStatusProvider.isLikelyOnline()) {
                                    transitionToOfflineAccumulationIfNeeded()
                                    delay(15.seconds)
                                    continue
                                }

                                if (
                                    retroAchievementsNetworkMode == RetroAchievementsNetworkMode.OFFLINE_ACCUMULATING ||
                                    retroAchievementsNetworkMode == RetroAchievementsNetworkMode.RECONCILING_RA_SUBMISSIONS
                                ) {
                                    delay(5.seconds)
                                    continue
                                }

                                if (!isRetroAchievementsOnlineSessionStarted) {
                                    val isHardcoreModeEnabled =
                                        effectiveLaunchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE
                                    val startResult = withContext(Dispatchers.IO) {
                                        retroAchievementsRepository.startSession(rom.retroAchievementsHash, isHardcoreModeEnabled)
                                    }
                                    if (startResult.isFailure) {
                                        if (startResult.exceptionOrNull() is UserTokenExpiredException) {
                                            _raIntegrationEvent.tryEmit(RAIntegrationEvent.LoginExpired(achievementData.icon))
                                            awaitCancellation()
                                        }
                                        delay(15.seconds)
                                        continue
                                    }

                                    isRetroAchievementsOnlineSessionStarted = true
                                }

                                // TODO: Should we pause the session if the app goes to background? If so, how?
                                delay(2.minutes)
                                val isHardcoreModeEnabled =
                                    effectiveLaunchDecision.sessionMode == RetroAchievementsSessionMode.HARDCORE
                                if (isHardcoreModeEnabled) {
                                    if ((pendingRaSubmissionStore?.snapshot?.value?.counts?.total ?: 0) > 0) {
                                        syncPendingRaSubmissions(RaPendingSyncSource.BEFORE_ONLINE_SUBMISSION)
                                    }
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
                                    submissionSessionId = 0,
                                    hardcoreEnabled = false,
                                    unofficialEnabled = settingsRepository.areRetroAchievementsUnofficialAchievementsEnabled(),
                                    encoreEnabled = settingsRepository.isRetroAchievementsEncoreModeEnabled(),
                                    apiHost = effectiveLaunchDecision.nativeClientHost,
                                    usesProxyHost = false,
                                    endpointGeneration = effectiveLaunchDecision.endpointGeneration,
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
                            val offlineType =
                                effectiveLaunchDecision.initialOfflineType ?: OfflineUnlockType.OFFLINE_FROM_START

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
                                val ownedRuntime =
                                    unloadAndReleaseRuntimeAuthenticationLease(
                                        runtimeLeaseId,
                                        "offline_native_setup_failed",
                                    ) {
                                        activeRuntimeBridgeConfig = null
                                        activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                                        isRetroAchievementsOnlineSessionStarted = false
                                    }
                                if (!ownedRuntime) {
                                    if (throwable is CancellationException) {
                                        throw throwable
                                    }
                                    return@launch
                                }
                                finalizeOfflineRetroAchievementsSessionIfNeeded()
                                emulatorSession.updateRetroAchievementsOfflineModeEnabled(false)
                                if (throwable is CancellationException) {
                                    throw throwable
                                }
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
                                    "error" to throwable.javaClass.simpleName,
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
                                offlineNoInternetAtStart = effectiveLaunchDecision.offlineDueToNoInternetAtStart,
                                hardcoreOfflineDisabled = effectiveLaunchDecision.hardcoreOfflineDisabled,
                            )
                            emitRetroAchievementsLoadedPopup(achievementData, hardcore = false)
                            awaitCancellation()
                        }
                    }
                }
                raSessionJob = runtimeJob
                runtimeJob.invokeOnCompletion completion@{ cause ->
                    if (cause == null) {
                        return@completion
                    }
                    var detachedStore: PendingRaSubmissionStore? = null
                    val ownedRuntime =
                        unloadAndReleaseRuntimeAuthenticationLease(
                            runtimeLeaseId,
                            "runtime_job_failed",
                        ) {
                            activeRuntimeBridgeConfig = null
                            activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                            isRetroAchievementsOnlineSessionStarted = false
                            detachedStore = detachPendingRaSubmissionSession()
                        }
                    if (!ownedRuntime) {
                        return@completion
                    }
                    finalizeOfflineRetroAchievementsSessionIfNeeded()
                    emulatorSession.updateRetroAchievementsOfflineModeEnabled(false)
                    detachedStore?.let { store ->
                        viewModelScope.launch {
                            val discarded = store.cleanup()
                            hardcoreOfflineLossTracker.clearPendingUnlocks(
                                store.context.userId,
                                store.context.contentHash,
                            )
                            logRaSubmission(
                                "ra_pending_session_cleared",
                                "reason" to "runtime_job_failed",
                                "discarded" to discarded,
                                "accepted" to false,
                            )
                        }
                    }
                    logRaSubmission(
                        "ra_runtime_job_terminated",
                        "error" to cause.javaClass.simpleName,
                    )
                    if (cause !is CancellationException) {
                        emulatorSession.updateRetroAchievementsIntegrationStatus(
                            GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR,
                        )
                        _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(achievementData.icon))
                    }
                }
                bootstrapReady.complete(Unit)
            } catch (exception: Throwable) {
                val ownedRuntime = acquiredLeaseId?.let { leaseId ->
                    unloadAndReleaseRuntimeAuthenticationLease(
                        leaseId,
                        "bootstrap_failed",
                    ) {
                        activeRuntimeBridgeConfig = null
                        activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                        isRetroAchievementsOnlineSessionStarted = false
                    }
                } ?: false
                if (exception is CancellationException) {
                    throw exception
                }
                if (!ownedRuntime) {
                    bootstrapReady.complete(Unit)
                    return@launch
                }
                Log.e("EmulatorViewModel", "RetroAchievements bootstrap failed for '${rom.name}'", exception)
                bootstrapReady.complete(Unit)
                markRetroAchievementsLoadStage(null)
                activeRuntimeBridgeConfig = null
                activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
                emulatorSession.updateRetroAchievementsIntegrationStatus(GameAchievementData.IntegrationStatus.DISABLED_LOAD_ERROR)
                _raIntegrationEvent.tryEmit(RAIntegrationEvent.Failed(null))
            } finally {
                bootstrapReady.complete(Unit)
            }
        }
        raBootstrapJob = bootstrapJob
        bootstrapJob.invokeOnCompletion {
            bootstrapReady.complete(Unit)
        }
        bootstrapJob.start()
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

    private suspend fun emitRetroAchievementsLoadedPopup(
        achievementData: GameAchievementData,
        hardcore: Boolean,
    ) {
        val username = runCatching { retroAchievementsRepository.getUserAuthentication()?.username }
            .getOrNull()
            ?.takeIf { it.isNotBlank() }
        if (username != null) {
            _raIntegrationEvent.tryEmit(
                RAIntegrationEvent.Welcome(
                    icon = runCatching { URL("https://media.retroachievements.org/UserPic/$username.png") }.getOrNull(),
                    username = username,
                    hardcore = hardcore,
                )
            )
            delay(250)
        }

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
        if (!settingsRepository.isRetroAchievementsOfflineSoftcoreEnabled()) {
            return null
        }
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
                val configuration = _runtimeRendererConfiguration.value ?: continue
                val configuredIr = configuration.resolutionScaling
                val renderedIr = if (configuration.renderer == VideoRenderer.VULKAN) {
                    emulatorManager.getVulkanRenderedInternalResolution().takeIf { it > 0 } ?: configuredIr
                } else {
                    configuredIr
                }
                _renderedInternalResolution.value = renderedIr to configuredIr
            }
        }
    }

    private fun buildRaPendingSyncMenuState() = RaPendingSubmissionUiPolicy.syncMenuState(
        context = RaPendingSyncMenuContext(
            isRomSessionActive = _emulatorState.value is EmulatorState.RunningRom,
            isRetroAchievementsActive = emulatorSession.areRetroAchievementsEnabled(),
            sessionStartedOnline = startedSessionOnlineLive,
            isHardcore = emulatorSession.isRetroAchievementsHardcoreModeEnabled,
            runtimeOwner = when (activeRuntimePath) {
                RetroAchievementsRuntimePath.RC_CLIENT -> RaPendingRuntimeOwner.RC_CLIENT
                RetroAchievementsRuntimePath.LEGACY -> RaPendingRuntimeOwner.LEGACY_KOTLIN
                RetroAchievementsRuntimePath.RC_CLIENT_OFFLINE,
                RetroAchievementsRuntimePath.DISABLED -> RaPendingRuntimeOwner.NONE
            },
            isBackendProxyActive = activeRuntimePath == RetroAchievementsRuntimePath.RC_CLIENT_OFFLINE,
            counts = pendingRaSubmissionStore?.snapshot?.value?.counts ?: RaPendingCounts.EMPTY,
        ),
        labelFormatter = { count ->
            context.getString(R.string.ra_pending_sync_menu_count, count)
        },
    )

    private fun syncPendingRaSubmissionsFromPauseMenu() {
        sessionCoroutineScope.launch {
            val pending = pendingRaSubmissionStore?.snapshot?.value?.counts ?: RaPendingCounts.EMPTY
            if (
                RaPendingSubmissionUiPolicy.manualSyncAction(pending) ==
                RaPendingManualSyncAction.REOPEN_PAUSE_MENU
            ) {
                pauseEmulator(showPauseMenu = true)
                return@launch
            }
            val requestId = pendingRaModalController.beginManualSync(pending)
                ?: return@launch
            val result = try {
                syncPendingRaSubmissions(RaPendingSyncSource.PAUSE_MENU)
            } catch (cancellation: CancellationException) {
                pendingRaModalController.clear(requestId)
                throw cancellation
            }
            pendingRaModalController.showResult(
                requestId = requestId,
                result = result,
                action = RaPendingSyncResultAction.REOPEN_PAUSE_MENU,
            )
        }
    }

    fun submitRaPendingSyncResultAction(
        requestId: Long,
        action: RaPendingSyncResultAction,
    ) {
        if (!pendingRaModalController.consumeResultAction(requestId, action)) {
            return
        }
        when (action) {
            RaPendingSyncResultAction.REOPEN_PAUSE_MENU ->
                pauseEmulator(showPauseMenu = true)
            RaPendingSyncResultAction.RESUME_SESSION -> resumeEmulator()
            RaPendingSyncResultAction.REOPEN_TERMINAL_EXIT ->
                requestExitRom(RaPendingExitContext.TERMINAL_STOP)
        }
    }

    fun canResumeEmulatorFromLifecycle(): Boolean {
        return settingsReconciliationsInFlight.get() == 0 &&
            !pendingRaModalController.blocksLifecycleResume() &&
            raSessionStopGate.canResume()
    }

    private fun filterRomPauseMenuOption(option: RomPauseMenuOption, rendererDebugToolsEnabled: Boolean): Boolean {
        return when (option) {
            RomPauseMenuOption.ROM_SETTINGS -> _emulatorState.value is EmulatorState.RunningRom
            RomPauseMenuOption.SAVE_STATE -> emulatorSession.areSaveStatesAllowed()
            RomPauseMenuOption.REWIND -> settingsRepository.isRewindEnabled() && emulatorSession.areSaveStateLoadsAllowed()
            RomPauseMenuOption.LOAD_STATE -> emulatorSession.areSaveStateLoadsAllowed()
            RomPauseMenuOption.CHEATS -> emulatorSession.areCheatsEnabled()
            RomPauseMenuOption.VIEW_ACHIEVEMENTS -> emulatorSession.isRetroAchievementsEnabledForSession()
            RomPauseMenuOption.SYNC_RETRO_ACHIEVEMENTS -> buildRaPendingSyncMenuState().isVisible
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
                    disableRetroAchievementsRuntime(
                        reason = "runtime_disabled",
                    )
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

    private fun disableRetroAchievementsRuntime(
        reason: String,
    ) {
        val detachedPendingStore = detachPendingRaSubmissionSession()
        raBootstrapJob?.cancel()
        raSessionJob?.cancel()
        raSessionJob = null
        leaderboardAttemptCoordinator.reset()
        leaderboardTrackerUpdateLogLimiter.resetAll()
        _achievementsEvent.tryEmit(RAEventUi.Reset)
        unloadAndReleaseActiveRuntimeAuthenticationLease(reason)
        activeRuntimeBridgeConfig = null
        activeRuntimePath = RetroAchievementsRuntimePath.DISABLED
        emulatorSession.updateRetroAchievementsOfflineModeEnabled(false)
        announcedMasteryKeys.clear()
        pendingRuntimeAchievementTriggers.clear()
        pendingRuntimeLeaderboardCompletions.clear()
        if (detachedPendingStore != null) {
            viewModelScope.launch {
                val discarded = detachedPendingStore.cleanup()
                hardcoreOfflineLossTracker.clearPendingUnlocks(
                    detachedPendingStore.context.userId,
                    detachedPendingStore.context.contentHash,
                )
                logRaSubmission(
                    "ra_pending_session_cleared",
                    "reason" to reason,
                    "discarded" to discarded,
                    "accepted" to false,
                )
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
            is RAEvent.OnLeaderboardAttemptSubmitted,
            is RAEvent.OnLeaderboardScoreboard,
            is RAEvent.OnLeaderboardSubmissionFailed,
            is RAEvent.OnLeaderboardAttemptCancelled,
            is RAEvent.OnLeaderboardTrackerHidden,
            is RAEvent.OnLeaderboardRuntimeReset,
            is RAEvent.OnPendingSubmissionAdded,
            is RAEvent.OnPendingSubmissionResolved,
            is RAEvent.OnPendingSubmissionBarrier -> Unit
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
        detachPendingRaSubmissionSession()
        raBootstrapJob?.cancel()
        raSessionJob?.cancel()
        raSessionJob = null
        sessionCoroutineScope.cancel()
        unloadAndReleaseActiveRuntimeAuthenticationLease("view_model_cleared")
        retroAchievementsEndpointProvider.endSession()
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
