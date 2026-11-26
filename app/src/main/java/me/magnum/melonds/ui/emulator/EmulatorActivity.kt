package me.magnum.melonds.ui.emulator

import me.magnum.melonds.multiplayer.phantom.PhantomPlayer
import me.magnum.melonds.multiplayer.MultiplayerSession

import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.res.Configuration
import android.graphics.Typeface
import android.hardware.display.DisplayManager
import android.hardware.input.InputManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.text.InputType
import android.util.TypedValue
import android.view.Display
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.InputDevice
import android.view.View
import android.view.ViewGroup
import android.view.ViewTreeObserver
import android.view.Window
import android.view.WindowManager
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.widget.SwitchCompat
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.Modifier
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.constraintlayout.widget.ConstraintLayout
import androidx.core.content.ContextCompat
import androidx.core.content.getSystemService
import androidx.core.os.ConfigurationCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.isGone
import androidx.core.view.isInvisible
import androidx.core.view.isVisible
import androidx.core.view.updateLayoutParams
import androidx.lifecycle.DEFAULT_ARGS_KEY
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.lifecycle.viewmodel.MutableCreationExtras
import androidx.recyclerview.widget.DividerItemDecoration
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.squareup.picasso.Picasso
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.R
import me.magnum.melonds.common.PermissionHandler
import me.magnum.melonds.databinding.ActivityEmulatorBinding
import me.magnum.melonds.domain.model.layout.ScreenGap
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.ControllerConfiguration
import me.magnum.melonds.domain.model.DualScreenPreset
import me.magnum.melonds.domain.model.ExternalDisplayMode
import me.magnum.melonds.domain.model.FpsCounterPosition
import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.RetroArchShaderSourceResolution
import me.magnum.melonds.domain.model.SaveStateSlot
import me.magnum.melonds.domain.model.VideoFiltering
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.retroachievements.RaPendingCounts
import me.magnum.melonds.domain.model.layout.Insets
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.impl.layout.observeSeparatingFolds
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RuntimeMicSource
import me.magnum.melonds.domain.model.ui.Orientation
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.extensions.insetsControllerCompat
import me.magnum.melonds.extensions.setLayoutOrientation
import me.magnum.melonds.impl.ShaderCompatibilityLog
import me.magnum.melonds.impl.ShaderCompileTimeStore
import me.magnum.melonds.impl.emulator.LifecycleOwnerProvider
import me.magnum.melonds.impl.emulator.debug.RendererDebugBridge
import me.magnum.melonds.domain.model.layout.LayoutDisplay
import me.magnum.melonds.impl.layout.DeviceLayoutDisplayMapper
import me.magnum.melonds.impl.layout.SecondaryDisplaySelector
import me.magnum.melonds.impl.system.AppForegroundStateObserver
import me.magnum.melonds.parcelables.RomInfoParcelable
import me.magnum.melonds.parcelables.RomParcelable
import me.magnum.melonds.ui.cheats.CheatsActivity
import me.magnum.melonds.ui.romlist.RomListActivity
import me.magnum.melonds.ui.common.ExternalInfoDisplayController
import me.magnum.melonds.ui.common.rom.EmulatorLaunchValidatorDelegate
import me.magnum.melonds.ui.emulator.component.EmulatorOverlayTracker
import me.magnum.melonds.ui.emulator.component.RaPendingExitContext
import me.magnum.melonds.ui.emulator.component.RaPendingModalState
import me.magnum.melonds.ui.emulator.component.RaPendingSyncResult
import me.magnum.melonds.ui.emulator.input.ConnectedControllerManager
import me.magnum.melonds.domain.model.rom.config.RomGbaSlotConfig
import me.magnum.melonds.ui.emulator.input.EmulatorMotionManager
import me.magnum.melonds.ui.emulator.input.EmulatorRumbleManager
import me.magnum.melonds.ui.emulator.input.FrontendInputHandler
import me.magnum.melonds.ui.emulator.input.INativeInputListener
import me.magnum.melonds.ui.emulator.input.InputProcessor
import me.magnum.melonds.ui.emulator.input.Slot2AnalogInput
import me.magnum.melonds.ui.emulator.input.MelonTouchHandler
import me.magnum.melonds.ui.emulator.model.EmulatorOverlay
import me.magnum.melonds.ui.emulator.model.EmulatorState
import me.magnum.melonds.ui.emulator.model.EmulatorUiEvent
import me.magnum.melonds.ui.emulator.model.HardcorePendingExitChoice
import me.magnum.melonds.ui.emulator.model.RaPendingSyncResultAction
import me.magnum.melonds.ui.emulator.model.LaunchArgs
import me.magnum.melonds.ui.emulator.model.OfflineAchievementsSyncChoice
import me.magnum.melonds.ui.emulator.model.InGameRomSettingsMenuState
import me.magnum.melonds.ui.emulator.model.PauseMenu
import me.magnum.melonds.ui.emulator.model.RAEventUi
import me.magnum.melonds.ui.emulator.model.RumbleEvent
import me.magnum.melonds.ui.emulator.model.RuntimeInputLayoutConfiguration
import me.magnum.melonds.ui.emulator.model.RuntimeRendererConfiguration
import me.magnum.melonds.ui.emulator.model.ToastEvent
import me.magnum.melonds.ui.emulator.model.RetroAchievementsLoadStage
import me.magnum.melonds.ui.emulator.model.VulkanCompileProgress
import me.magnum.melonds.ui.emulator.model.VulkanPresentationConfig
import me.magnum.melonds.ui.emulator.render.ChoreographerFrameRenderer
import me.magnum.melonds.ui.emulator.render.ChoreographerFrameRendererFactory
import me.magnum.melonds.ui.emulator.render.GamePanelActivity
import me.magnum.melonds.ui.emulator.render.ExternalPresentation
import me.magnum.melonds.ui.emulator.render.FrameRenderCoordinator
import me.magnum.melonds.ui.emulator.render.OpenGlFrameRenderCoordinator
import me.magnum.melonds.ui.emulator.render.VulkanFrameRenderCoordinator
import me.magnum.melonds.ui.emulator.rewind.EdgeSpacingDecorator
import me.magnum.melonds.ui.emulator.rewind.RewindSaveStateAdapter
import me.magnum.melonds.ui.emulator.rewind.model.RewindWindow
import me.magnum.melonds.ui.emulator.rom.SaveStateAdapter
import me.magnum.melonds.ui.emulator.ui.AchievementListDialog
import me.magnum.melonds.ui.emulator.ui.AchievementUpdatesUi
import me.magnum.melonds.ui.emulator.ui.DualScreenPresetsDialog
import me.magnum.melonds.ui.emulator.ui.PendingSubmissionsDialog
import me.magnum.melonds.ui.inputsetup.InputSetupActivity
import me.magnum.melonds.ui.layouts.LayoutSelectorActivity
import me.magnum.melonds.ui.layouteditor.model.LayoutTarget
import me.magnum.melonds.ui.settings.SettingsActivity
import me.magnum.melonds.ui.theme.MelonTheme
import java.text.SimpleDateFormat
import java.util.UUID
import javax.inject.Inject
import kotlin.math.max

@AndroidEntryPoint
open class EmulatorActivity : AppCompatActivity() {
    companion object {
        const val KEY_ROM = "rom"
        const val KEY_PATH = "PATH"
        const val KEY_URI = "uri"
        const val KEY_BOOT_FIRMWARE_CONSOLE = "boot_firmware_console"
        const val KEY_BOOT_FIRMWARE_ONLY = "boot_firmware_only"
        const val KEY_START_MULTIPLAYER_FIRMWARE = "start_multiplayer_firmware"
        private const val STARTUP_PRESENTATION_REFRESH_ATTEMPTS = 24
        private const val STARTUP_PRESENTATION_REFRESH_INTERVAL_MS = 100L
        private const val OVERLAY_FOCUS_ATTEMPTS = 12
        private const val OVERLAY_FOCUS_RETRY_MS = 32L
        private const val SHADER_DIAGNOSTICS_POLL_MS = 1500L
        private const val DS_SCREEN_WIDTH = 256
        private const val DS_FRAME_ATLAS_HEIGHT = 386
        private const val SHADER_PREWARM_LAYOUT_TIMEOUT_MS = 2000L
        private const val SHADER_PREWARM_LAYOUT_POLL_MS = 50L
        private const val SHADER_PREWARM_MESSAGE_SETTLE_MS = 150L
        private const val LEDGER_EXPIRATION_DAY_MS = 24L * 60L * 60L * 1000L

        fun getRomEmulatorActivityIntent(context: Context, rom: Rom): Intent {
            return Intent(context, EmulatorActivity::class.java).apply {
                putExtra(KEY_ROM, RomParcelable(rom))
            }
        }

        fun getFirmwareEmulatorActivityIntent(context: Context, consoleType: ConsoleType, startMultiplayerSession: Boolean = false): Intent {
            return Intent(context, EmulatorActivity::class.java).apply {
                putExtra(KEY_BOOT_FIRMWARE_ONLY, true)
                putExtra(KEY_BOOT_FIRMWARE_CONSOLE, consoleType.ordinal)
                if (startMultiplayerSession) putExtra(KEY_START_MULTIPLAYER_FIRMWARE, true)
            }
        }
    }

    private lateinit var binding: ActivityEmulatorBinding
    private var joystickDispatchView: View? = null
    private val joystickFocusListener = ViewTreeObserver.OnGlobalFocusChangeListener { _, _ ->
        updateJoystickDispatch()
    }
    private val viewModel: EmulatorViewModel by viewModels(
        extrasProducer = {
            val extras = MutableCreationExtras(defaultViewModelCreationExtras)
            // Inject intent data into view-model creation extras to make it accessible through the SavedStateHandle
            intent.data?.let { dataUri ->
                val existingExtras = extras[DEFAULT_ARGS_KEY]?.let { Bundle(it) } ?: Bundle()
                existingExtras.putString(KEY_URI, dataUri.toString())
                extras[DEFAULT_ARGS_KEY] = existingExtras
            }
            extras
        }
    )

    @Inject
    lateinit var secondaryDisplaySelector: SecondaryDisplaySelector

    @Inject
    lateinit var deviceLayoutDisplayMapper: DeviceLayoutDisplayMapper

    @Inject
    lateinit var picasso: Picasso

    @Inject
    lateinit var permissionHandler: PermissionHandler

    @Inject
    lateinit var lifecycleOwnerProvider: LifecycleOwnerProvider

    @Inject
    lateinit var appForegroundStateObserver: AppForegroundStateObserver

    @Inject
    lateinit var settingsRepository: SettingsRepository

    @Inject
    lateinit var boxArtRepository: me.magnum.melonds.ui.romlist.boxart.BoxArtRepository

    private var presentation: ExternalPresentation? = null
    private var gameDisplayOwner: Any? = null
    private var phantomPlayerResumed = false

    private lateinit var handler: Handler
    private val displayListener = object : DisplayManager.DisplayListener {

        override fun onDisplayAdded(displayId: Int) {
            runOnUiThread {
                updateDisplays()
            }
        }

        override fun onDisplayRemoved(displayId: Int) {
            runOnUiThread {
                updateDisplays()
            }
        }

        override fun onDisplayChanged(displayId: Int) {
            updateDisplays()
        }
    }

    private val connectedControllerManager = ConnectedControllerManager()
    private lateinit var emulatorLaunchValidatorDelegate: EmulatorLaunchValidatorDelegate
    private lateinit var emulatorRumbleManager: EmulatorRumbleManager
    private lateinit var frameRenderCoordinator: FrameRenderCoordinator
    private lateinit var choreographerFrameRenderer: ChoreographerFrameRenderer
    private lateinit var mainScreenRenderer: DSRenderer
    private lateinit var melonTouchHandler: MelonTouchHandler
    private lateinit var nativeInputListener: INativeInputListener
    private val slot2AnalogInput = Slot2AnalogInput(MelonEmulator::setSlot2AnalogInput)
    private var currentRuntimeRendererConfiguration: RuntimeRendererConfiguration? = null
    private var lastOpenGlRetroArchFilterKey: String? = null
    private var prewarmedOpenGlRetroArchFilterKey: String? = null
    private var shaderDiagnosticsRunnable: Runnable? = null
    @Inject lateinit var shaderCompatibilityLog: ShaderCompatibilityLog
    @Inject lateinit var shaderCompileTimeStore: ShaderCompileTimeStore
    private var currentMainScreenBackground = me.magnum.melonds.domain.model.RuntimeBackground.None
    private var currentPresentationBackend = PresentationBackend.OPEN_GL
    private var startupPresentationRefreshRunnable: Runnable? = null
    private var startupPresentationRefreshAttempts = 0
    private var rendererDebugPauseEmulation = true
    private var isClosingEmulator = false
    private var isFrameRenderCoordinatorStopped = false
    private var excludeTouchScreenFromSystemGestures = false
    private var externalDisplayMode = ExternalDisplayMode.MELON_DUAL_DS
    private val frontendInputHandler = object : FrontendInputHandler() {
        var fastForwardEnabled = false
            private set
        private var fastForwardHoldPressed = false
        var microphoneEnabled = true
            private set

        override fun onSoftInputTogglePressed() {
            binding.viewLayoutControls.toggleSoftInputVisibility()
            presentation?.layoutView?.toggleSoftInputVisibility()
        }

        override fun onPausePressed() {
            viewModel.pauseEmulator(true)
        }

        override fun onFastForwardPressed() {
            if (!viewModel.onFastForwardToggleRequested()) {
                return
            }
            fastForwardEnabled = !fastForwardEnabled
            binding.viewLayoutControls.setLayoutComponentToggleState(LayoutComponent.BUTTON_FAST_FORWARD_TOGGLE, fastForwardEnabled)
            presentation?.layoutView?.setLayoutComponentToggleState(LayoutComponent.BUTTON_FAST_FORWARD_TOGGLE, fastForwardEnabled)
            updateFastForwardState()
        }

        override fun onFastForwardHoldPressed() {
            if (fastForwardHoldPressed || !viewModel.onFastForwardToggleRequested()) {
                return
            }
            fastForwardHoldPressed = true
            updateFastForwardState()
        }

        override fun onFastForwardHoldReleased() {
            if (!fastForwardHoldPressed) {
                return
            }
            fastForwardHoldPressed = false
            updateFastForwardState()
        }

        override fun onMicrophonePressed() {
            microphoneEnabled = !microphoneEnabled
            binding.viewLayoutControls.setLayoutComponentToggleState(LayoutComponent.BUTTON_MICROPHONE_TOGGLE, microphoneEnabled)
            presentation?.layoutView?.setLayoutComponentToggleState(LayoutComponent.BUTTON_MICROPHONE_TOGGLE, microphoneEnabled)
            MelonEmulator.setMicrophoneEnabled(microphoneEnabled)
        }

        override fun onResetPressed() {
            viewModel.resetEmulator()
        }

        override fun onSwapScreens() {
            swapScreen()
        }

        override fun onQuickSave() {
            viewModel.doQuickSave()
        }

        override fun onQuickLoad() {
            viewModel.doQuickLoad()
        }

        override fun onCycleLayout() {
            viewModel.cycleLayout()
        }

        override fun onExitGame() {
            viewModel.exitEmulator()
        }

        override fun onScreenshot() {
            viewModel.takeScreenshot()
        }

        override fun onRewind() {
            viewModel.onOpenRewind()
        }

        fun clearFastForwardHold() {
            if (!fastForwardHoldPressed) {
                return
            }
            fastForwardHoldPressed = false
            updateFastForwardState()
        }

        private fun updateFastForwardState() {
            MelonEmulator.setFastForwardEnabled(fastForwardEnabled || fastForwardHoldPressed)
        }
    }
    private val settingsLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val logoutRequested =
            result.resultCode == RESULT_OK &&
                result.data?.getBooleanExtra(SettingsActivity.KEY_RA_LOGOUT_REQUESTED, false) == true
        if (logoutRequested) {
            viewModel.onRetroAchievementsLogoutRequested()
        } else {
            val pauseMenuStillOpen = pauseMenuState.value != null || activeOverlays.hasActiveOverlays()
            viewModel.onSettingsChanged(resumeWhenFinished = MultiplayerSession.active || !pauseMenuStillOpen)
            if (pauseMenuStillOpen) {
                requestOverlayHostFocus()
            }
        }
        setupSustainedPerformanceMode()
        setupFpsCounter()
        setupRenderedIrOverlay()
        externalDisplayMode = settingsRepository.getExternalDisplayMode()
        updateDisplays()
    }
    private val romInputSettingsLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        viewModel.onRomCustomInputConfigEdited()
        onReturnedFromRomSettingsActivity()
    }
    private val romLayoutSettingsLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode == RESULT_OK) {
            val layoutId = result.data
                ?.getStringExtra(LayoutSelectorActivity.KEY_SELECTED_LAYOUT_ID)
                ?.let { UUID.fromString(it) }
            viewModel.onRunningRomLayoutSelected(layoutId)
        }
        onReturnedFromRomSettingsActivity()
    }
    private val cheatsLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        viewModel.onCheatsChanged()
        viewModel.resumeEmulator()
    }
    private val permissionRequestLauncher = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        lifecycleScope.launch {
            it.keys.forEach { permission ->
                permissionHandler.notifyPermissionStatusUpdated(permission)
            }
        }
    }
    private val backPressedCallback = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() {
            handleBackPressed()
        }
    }

    private var offlineSyncChoiceDialog: AlertDialog? = null
    private var offlineSyncProgressDialog: AlertDialog? = null
    private var hardcorePendingExitDialog: AlertDialog? = null
    private var raPendingSyncProgressDialog: AlertDialog? = null
    private var raPendingSyncResultDialog: AlertDialog? = null

    private enum class PresentationBackend {
        OPEN_GL,
        VULKAN,
    }

    private data class ScreenPresentationAreas(
        val topScreenRect: Rect?,
        val bottomScreenRect: Rect?,
        val topAlpha: Float,
        val bottomAlpha: Float,
        val topOnTop: Boolean,
        val bottomOnTop: Boolean,
        val hybridTopScreenRect: Rect?,
        val hybridBottomScreenRect: Rect?,
        val hybridAlpha: Float,
        val hybridOnTop: Boolean,
    )

    private val rewindSaveStateAdapter = RewindSaveStateAdapter {
        viewModel.rewindToState(it)
        closeRewindWindow()
    }
    private val showAchievementList = mutableStateOf(false)
    private val showPendingSubmissionsDialog = mutableStateOf(false)
    private val showDualScreenPresets = mutableStateOf(false)
    private val pauseMenuState = mutableStateOf<me.magnum.melonds.ui.emulator.model.PauseMenu?>(null)

    private val showBootAnimation = mutableStateOf(true)
    private val bootRomReady = mutableStateOf(false)
    private val bootRomTitle = mutableStateOf<String?>(null)
    private val bootRom = mutableStateOf<Rom?>(null)
    private val bootBoxArtUrl = mutableStateOf<String?>(null)

    private val rewindOverlayState = mutableStateOf<me.magnum.melonds.ui.emulator.rewind.model.RewindWindow?>(null)
    private val bootStatus = mutableStateOf<String?>(null)

    private data class SaveStatesOverlayData(
        val slots: List<SaveStateSlot>,
        val isSaving: Boolean,
        val onSlotPicked: (SaveStateSlot) -> Unit,
    )
    private val saveStatesOverlayState = mutableStateOf<SaveStatesOverlayData?>(null)

    private sealed interface ConsoleOverlayNode {
        val title: String
        data class Submenu(override val title: String, val entries: List<Pair<String, () -> Unit>>) : ConsoleOverlayNode
        data class Choice(
            override val title: String,
            val labels: List<String>,
            val selectedIndex: Int,
            val onSelect: (Int) -> Unit,
        ) : ConsoleOverlayNode
    }
    private val consoleOverlayStack = androidx.compose.runtime.mutableStateListOf<ConsoleOverlayNode>()
    private var overlayFocusManager: androidx.compose.ui.focus.FocusManager? = null
    private var lastOverlayHatX = 0f
    private var lastOverlayHatY = 0f
    private var lastPauseMenu: me.magnum.melonds.ui.emulator.model.PauseMenu? = null
    private var rewindOpenedFromPauseMenu = false

    private fun pushConsoleOverlay(node: ConsoleOverlayNode) {
        activeOverlays.addActiveOverlay(EmulatorOverlay.PAUSE_MENU)
        consoleOverlayStack.add(node)
        requestOverlayHostFocus()
    }

    private fun popConsoleOverlay() {
        if (consoleOverlayStack.isNotEmpty()) {
            consoleOverlayStack.removeAt(consoleOverlayStack.lastIndex)
            activeOverlays.removeActiveOverlay(EmulatorOverlay.PAUSE_MENU)
        }
        if (consoleOverlayStack.isEmpty() && pauseMenuState.value == null) {
            reopenPauseMenu()
        }
    }

    private fun reopenPauseMenu() {
        val menu = lastPauseMenu
        if (menu != null) {
            pauseMenuState.value = menu
        } else {
            viewModel.resumeEmulator()
        }
    }

    private val activeOverlays = EmulatorOverlayTracker(
        onOverlaysCleared = {
            disableScreenTimeOut()
            presentation?.setPauseOverlayVisibility(false)
            updateJoystickDispatch()
        },
        onOverlaysPresent = {
            enableScreenTimeOut()
            presentation?.setPauseOverlayVisibility(true)
            updateJoystickDispatch()
        }
    )

    private fun acquireMultiplayerWifiLock(): WifiManager.WifiLock? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || !MultiplayerSession.active) return null
        return try {
            applicationContext.getSystemService<WifiManager>()
                ?.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "watermelon-multiplayer")
                ?.apply {
                    setReferenceCounted(false)
                    acquire()
                }
        } catch (error: RuntimeException) {
            android.util.Log.e("Multiplayer", "Wi-Fi low latency unavailable", error)
            null
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        handler = Handler(mainLooper)
        externalDisplayMode = settingsRepository.getExternalDisplayMode()
        lifecycleOwnerProvider.setCurrentLifecycleOwner(this)
        binding = ActivityEmulatorBinding.inflate(layoutInflater)
        supportRequestWindowFeature(Window.FEATURE_NO_TITLE)
        setContentView(binding.root)
        if (MultiplayerSession.active) {
            lifecycleScope.launch {
                var notified = false
                lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                    withContext(Dispatchers.IO) {
                        val wifiLock = acquireMultiplayerWifiLock()
                        try {
                            while (true) {
                                if (MultiplayerSession.status().optBoolean("interrupted")) {
                                    if (!notified) {
                                        notified = true
                                        withContext(Dispatchers.Main) {
                                            Toast.makeText(this@EmulatorActivity, R.string.multiplayer_interrupted_message, Toast.LENGTH_LONG).show()
                                        }
                                    }
                                    break
                                }
                                delay(500)
                            }
                        } finally {
                            try {
                                if (wifiLock?.isHeld == true) wifiLock.release()
                            } catch (error: RuntimeException) {
                                android.util.Log.e("Multiplayer", "Wi-Fi low latency release failed", error)
                            }
                        }
                    }
                }
            }
        }
        setupFullscreen()
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, windowInsets ->
            val insets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())
            binding.listRewind.setPadding(insets.left, 0, insets.right, insets.bottom)
            binding.textFps.updateLayoutParams<ConstraintLayout.LayoutParams> {
                setMargins(insets.left, insets.top, insets.right, insets.bottom)
            }
            binding.textRenderedIr.updateLayoutParams<ConstraintLayout.LayoutParams> {
                setMargins(insets.left, insets.top, insets.right, insets.bottom)
            }

            val uiInsets = if (viewModel.shouldIgnoreDisplayCutoutInLayouts()) {
                Insets.Zero
            } else {
                Insets(insets.left, insets.top, insets.right, insets.bottom)
            }
            viewModel.setUiInsets(uiInsets)

            WindowInsetsCompat.CONSUMED
        }

        onBackPressedDispatcher.addCallback(backPressedCallback)

        emulatorLaunchValidatorDelegate = EmulatorLaunchValidatorDelegate(this, object : EmulatorLaunchValidatorDelegate.Callback {
            override fun onRomValidated(rom: Rom) {
                viewModel.onRomLaunchValidated(rom)
            }

            override fun onFirmwareValidated(consoleType: ConsoleType, dsiProfile: me.magnum.melonds.domain.model.DSiProfile?) {
                viewModel.onFirmwareLaunchValidated(consoleType, dsiProfile)
            }

            override fun onValidationAborted() {
                finish()
            }
        })
        emulatorRumbleManager = EmulatorRumbleManager(this, lifecycleScope, connectedControllerManager)
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.RESUMED) {
                viewModel.emulatorState.map { state ->
                    val slot = (state as? EmulatorState.RunningRom)?.rom?.config?.gbaSlotConfig
                    slot == RomGbaSlotConfig.MotionPakHomebrew || slot == RomGbaSlotConfig.MotionPakRetail
                }.distinctUntilChanged().collectLatest { usesMotionPak ->
                    if (usesMotionPak) {
                        EmulatorMotionManager(this@EmulatorActivity).track(connectedControllerManager)
                    }
                }
            }
        }
        currentPresentationBackend = viewModel.getConfiguredVideoRenderer().toPresentationBackend()
        frameRenderCoordinator = createFrameRenderCoordinator(currentPresentationBackend)
        choreographerFrameRenderer = ChoreographerFrameRendererFactory.createFrameRenderer(frameRenderCoordinator)
        melonTouchHandler = MelonTouchHandler()
        mainScreenRenderer = DSRenderer(this)
        binding.surfaceMain.apply {
            setRenderer(mainScreenRenderer)
        }

        binding.textFps.visibility = View.INVISIBLE
        binding.textRenderedIr.visibility = View.INVISIBLE
        binding.viewLayoutControls.setLayoutComponentViewBuilderFactory(RuntimeLayoutComponentViewBuilderFactory())
        binding.layoutRewind.setOnClickListener {
            closeRewindWindow()
        }
        binding.listRewind.apply {
            val listLayoutManager = LinearLayoutManager(context, LinearLayoutManager.HORIZONTAL, true)
            layoutManager = listLayoutManager
            addItemDecoration(EdgeSpacingDecorator())
            adapter = rewindSaveStateAdapter
        }
        binding.viewLayoutControls.apply {
            setFrontendInputHandler(frontendInputHandler)
            setSystemInputHandler(melonTouchHandler)
            setSlot2AnalogInput(slot2AnalogInput)
        }

        val layoutChangeListener = View.OnLayoutChangeListener { _, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom ->
            val oldWith = oldRight - oldLeft
            val oldHeight = oldBottom - oldTop

            val newWidth = right - left
            val newHeight = bottom - top

            if (newWidth != oldWith || newHeight != oldHeight) {
                updateRendererScreenAreas()
                viewModel.setUiSize(newWidth, newHeight)
            }
        }
        binding.viewLayoutControls.addOnLayoutChangeListener(layoutChangeListener)

        updateOrientation(resources.configuration)
        disableScreenTimeOut()

        binding.layoutAchievement.setContent {
            MelonTheme {
                val achievementsViewModel = viewModels<EmulatorRetroAchievementsViewModel>().value

                LaunchedEffect(Unit) {
                    viewModel.achievementsEvent.filterIsInstance<RAEventUi.Reset>().collect {
                        achievementsViewModel.onSessionReset()
                    }
                }

                AchievementUpdatesUi(viewModel)

                if (showAchievementList.value) {
                    AchievementListDialog(
                        viewModel = achievementsViewModel,
                        onDismiss = {
                            activeOverlays.removeActiveOverlay(EmulatorOverlay.ACHIEVEMENTS_DIALOG)
                            showAchievementList.value = false
                            presentation?.setInfoOverlayContent(null)
                            reopenPauseMenu()
                        },
                        onAchievementFocused = { model ->
                            presentation?.setInfoOverlayContent {
                                me.magnum.melonds.ui.common.ExternalAchievementInfo(model)
                            }
                        },
                    )
                }

                if (showPendingSubmissionsDialog.value) {
                    PendingSubmissionsDialog(
                        pendingSubmissionsSummaryFlow = viewModel.pendingSubmissionsSummary,
                        onExit = {
                            activeOverlays.removeActiveOverlay(EmulatorOverlay.PENDING_SUBMISSION_CONFIRM_EXIT)
                            showPendingSubmissionsDialog.value = false
                            viewModel.exitEmulator(force = true)
                        },
                        onCancel = {
                            activeOverlays.removeActiveOverlay(EmulatorOverlay.PENDING_SUBMISSION_CONFIRM_EXIT)
                            viewModel.resumeEmulator()
                            showPendingSubmissionsDialog.value = false
                        },
                    )
                }

            }
        }

        binding.layoutPauseMenu.setContent {
            MelonTheme {
                val focusManager = androidx.compose.ui.platform.LocalFocusManager.current
                androidx.compose.runtime.SideEffect {
                    overlayFocusManager = focusManager
                }

                val saveStatesData = saveStatesOverlayState.value
                val topConsoleNode = consoleOverlayStack.lastOrNull()
                val pauseMenu = pauseMenuState.value
                val rewindWindow = rewindOverlayState.value

                Box(Modifier.fillMaxSize()) {
                when {
                    rewindWindow != null -> {
                        me.magnum.melonds.ui.emulator.ui.RewindOverlay(
                            window = rewindWindow,
                            onStateSelected = { state -> onRewindStateSelected(state) },
                            onDismiss = { closeRewindWindow() },
                        )
                    }
                    showDualScreenPresets.value -> {
                        val preset by viewModel.dualScreenPreset.collectAsState()
                        val keepAspectRatio by viewModel.externalDisplayKeepAspectRatioEnabled.collectAsState()
                        val sameSize by viewModel.sameScreenSizeEnabled.collectAsState()
                        val integerScaleEnabled by viewModel.dualScreenIntegerScaleEnabled.collectAsState()
                        val internalFillHeight by viewModel.dualScreenInternalFillHeightEnabled.collectAsState()
                        val internalFillWidth by viewModel.dualScreenInternalFillWidthEnabled.collectAsState()
                        val externalFillHeight by viewModel.dualScreenExternalFillHeightEnabled.collectAsState()
                        val externalFillWidth by viewModel.dualScreenExternalFillWidthEnabled.collectAsState()
                        val internalAlignmentOverride by viewModel.dualScreenInternalVerticalAlignmentOverride.collectAsState()
                        val externalAlignmentOverride by viewModel.dualScreenExternalVerticalAlignmentOverride.collectAsState()
                        me.magnum.melonds.ui.emulator.ui.ConsolePresetsOverlay(
                            dualScreenPreset = preset,
                            onDualScreenPresetSelected = { selectedPreset ->
                                viewModel.setDualScreenPreset(selectedPreset)
                                handler.post {
                                    applyDualScreenPresetSwapState(selectedPreset)
                                    updateRendererScreenAreas()
                                    presentation?.updateRendererScreenAreas()
                                }
                            },
                            keepAspectRatio = keepAspectRatio,
                            onKeepAspectRatioChanged = { enabled -> viewModel.setExternalDisplayKeepAspectRatioEnabled(enabled) },
                            sameScreenSize = sameSize,
                            onSameScreenSizeChanged = viewModel::setSameScreenSizeEnabled,
                            integerScaleEnabled = integerScaleEnabled,
                            onIntegerScaleChanged = { enabled -> viewModel.setDualScreenIntegerScaleEnabled(enabled) },
                            internalFillHeight = internalFillHeight,
                            onInternalFillHeightChanged = { enabled -> viewModel.setDualScreenInternalFillHeightEnabled(enabled) },
                            internalFillWidth = internalFillWidth,
                            onInternalFillWidthChanged = { enabled -> viewModel.setDualScreenInternalFillWidthEnabled(enabled) },
                            externalFillHeight = externalFillHeight,
                            onExternalFillHeightChanged = { enabled -> viewModel.setDualScreenExternalFillHeightEnabled(enabled) },
                            externalFillWidth = externalFillWidth,
                            onExternalFillWidthChanged = { enabled -> viewModel.setDualScreenExternalFillWidthEnabled(enabled) },
                            internalAlignment = internalAlignmentOverride,
                            onInternalAlignmentChanged = { alignment -> viewModel.setDualScreenInternalVerticalAlignmentOverride(alignment) },
                            externalAlignment = externalAlignmentOverride,
                            onExternalAlignmentChanged = { alignment -> viewModel.setDualScreenExternalVerticalAlignmentOverride(alignment) },
                            onBack = {
                                activeOverlays.removeActiveOverlay(EmulatorOverlay.PRESETS_DIALOG)
                                showDualScreenPresets.value = false
                                reopenPauseMenu()
                            },
                        )
                    }
                    topConsoleNode != null -> {
                        when (val node = topConsoleNode!!) {
                            is ConsoleOverlayNode.Submenu -> me.magnum.melonds.ui.emulator.ui.ConsoleSubmenuOverlay(
                                title = node.title,
                                entries = node.entries.map { it.first },
                                onEntrySelected = { index -> node.entries[index].second.invoke() },
                                onDismiss = { popConsoleOverlay() },
                            )
                            is ConsoleOverlayNode.Choice -> me.magnum.melonds.ui.emulator.ui.ConsoleChoiceOverlay(
                                title = node.title,
                                options = node.labels,
                                selectedIndex = node.selectedIndex,
                                onOptionSelected = { index ->
                                    node.onSelect(index)
                                    popConsoleOverlay()
                                },
                                onBack = { popConsoleOverlay() },
                            )
                        }
                    }
                    saveStatesData != null -> {
                        val emulatorState by viewModel.emulatorState.collectAsState()
                        val currentRom = (emulatorState as? EmulatorState.RunningRom)?.rom
                        me.magnum.melonds.ui.emulator.ui.SaveStatesOverlay(
                            slots = saveStatesData.slots,
                            isSaving = saveStatesData.isSaving,
                            gameTitle = currentRom?.let { it.config.customName ?: it.name },
                            onSlotPicked = { slot ->
                                dismissSaveStatesOverlay()
                                dismissPauseMenu()
                                saveStatesData.onSlotPicked(slot)
                            },
                            onSlotDeleted = { slot ->
                                viewModel.deleteSaveStateSlot(slot) { newSlots ->
                                    saveStatesOverlayState.value = saveStatesData.copy(slots = newSlots)
                                }
                            },
                            onDismiss = {
                                dismissSaveStatesOverlay()
                                reopenPauseMenu()
                            },
                        )
                    }
                    pauseMenu != null -> {
                        val emulatorState by viewModel.emulatorState.collectAsState()
                        val currentRom = (emulatorState as? EmulatorState.RunningRom)?.rom
                        me.magnum.melonds.ui.emulator.ui.PauseMenuOverlay(
                            pauseMenu = pauseMenu,
                            rom = currentRom,
                            achievementsSummary = null,
                            onOptionSelected = { option ->
                                val isTerminal = option == me.magnum.melonds.ui.emulator.rom.RomPauseMenuOption.RESET ||
                                    option == me.magnum.melonds.ui.emulator.rom.RomPauseMenuOption.EXIT ||
                                    option == me.magnum.melonds.ui.emulator.firmware.FirmwarePauseMenuOption.RESET ||
                                    option == me.magnum.melonds.ui.emulator.firmware.FirmwarePauseMenuOption.EXIT
                                val isViewOverlay = option == me.magnum.melonds.ui.emulator.rom.RomPauseMenuOption.REWIND
                                if (isTerminal) {
                                    dismissPauseMenu()
                                } else if (isViewOverlay) {
                                    pauseMenuState.value = null
                                    rewindOpenedFromPauseMenu = true
                                }
                                viewModel.onPauseMenuOptionSelected(option)
                            },
                            onResume = {
                                dismissPauseMenu()
                                viewModel.resumeEmulator()
                            },
                        )
                    }
                    else -> Unit
                }

                if (showBootAnimation.value) {
                    val rom = bootRom.value
                    if (rom != null) {
                        me.magnum.melonds.ui.emulator.ui.BootInfoOverlay(
                            rom = rom,
                            boxArtUrl = bootBoxArtUrl.value,
                            statusText = bootStatus.value,
                            romReady = bootRomReady.value,
                            onFinished = { showBootAnimation.value = false },
                        )
                    } else {
                        me.magnum.melonds.ui.emulator.ui.DsBootOverlay(
                            half = me.magnum.melonds.ui.emulator.ui.DsBootScreenHalf.BOTH,
                            romReady = bootRomReady.value,
                            romTitle = bootRomTitle.value,
                            statusText = bootStatus.value,
                            onFinished = { showBootAnimation.value = false },
                        )
                    }
                }
                }
            }
        }
        binding.layoutPauseMenu.isFocusable = false
        binding.layoutPauseMenu.isFocusableInTouchMode = false

        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                permissionHandler.observePermissionRequests().collect {
                    permissionRequestLauncher.launch(arrayOf(it))
                }
            }
        }

        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.runtimeLayout.collectLatest {
                    setupSoftInput(it)
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.controllerConfiguration.collect {
                    setupInputHandling(it)
                    connectedControllerManager.setCurrentControllerConfiguration(it)
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                connectedControllerManager.managedControllers.collect {
                    slot2AnalogInput.clearPhysical()
                    if (::nativeInputListener.isInitialized) nativeInputListener.releaseChordInputs()
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                connectedControllerManager.controllersState.collect {
                    binding.viewLayoutControls.setConnectedControllersState(it)
                    presentation?.layoutView?.setConnectedControllersState(it)
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.mainScreenBackground.collectLatest {
                    currentMainScreenBackground = it
                    mainScreenRenderer.setBackground(it)
                    updateRendererScreenAreas()
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.secondaryScreenBackground.collectLatest {
                    presentation?.updateBackground(it)
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.runtimeRendererConfiguration.collectLatest {
                    currentRuntimeRendererConfiguration = it
                    ensurePresentationBackend(it?.renderer ?: viewModel.getConfiguredVideoRenderer())
                    updateOpenGlRetroArchFilterConfiguration(it)
                    mainScreenRenderer.updateRendererConfiguration(it)
                    presentation?.updateRendererConfiguration(it)
                    updateRendererScreenAreas()
                    scheduleStartupPresentationRefreshes()
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.currentFps.collectLatest {
                    if (it == null) {
                        binding.textFps.text = null
                    } else {
                        binding.textFps.text = getString(R.string.info_fps, it)
                    }
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.renderedInternalResolution.collectLatest {
                    if (it == null) {
                        binding.textRenderedIr.text = null
                    } else {
                        binding.textRenderedIr.text = getString(R.string.info_rendered_ir, it.first, it.second)
                    }
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                settingsRepository.observeTouchScreenSystemGestureExclusionEnabled().collectLatest {
                    excludeTouchScreenFromSystemGestures = it
                    updateRendererScreenAreas()
                    presentation?.setTouchScreenSystemGestureExclusionEnabled(it)
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                settingsRepository.observeExternalDisplayMode().collectLatest {
                    externalDisplayMode = it
                    updateDisplays()
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.toastEvent.collectLatest {
                    val (message, duration) = when (it) {
                        ToastEvent.GbaLoadFailed -> getString(R.string.error_load_gba_rom) to Toast.LENGTH_SHORT
                        ToastEvent.QuickSaveSuccessful -> getString(R.string.saved) to Toast.LENGTH_SHORT
                        ToastEvent.QuickLoadSuccessful -> getString(R.string.loaded) to Toast.LENGTH_SHORT
                        is ToastEvent.ScreenshotSaved -> getString(R.string.screenshot_saved, it.fileName) to Toast.LENGTH_SHORT
                        ToastEvent.ScreenshotFailed -> getString(R.string.screenshot_failed) to Toast.LENGTH_LONG
                        ToastEvent.RewindNotEnabled -> getString(R.string.rewind_not_enabled) to Toast.LENGTH_SHORT
                        ToastEvent.RewindNotAvailableWhileRAHardcoreModeEnabled -> getString(R.string.rewind_unavailable_ra_hardcore_enabled) to Toast.LENGTH_LONG
                        ToastEvent.StateLoadFailed -> getString(R.string.failed_load_state) to Toast.LENGTH_SHORT
                        ToastEvent.InvalidAutoLoadState -> getString(R.string.invalid_auto_load_state) to Toast.LENGTH_LONG
                        ToastEvent.MultiplayerOperationUnavailable -> getString(R.string.multiplayer_operation_unavailable) to Toast.LENGTH_LONG
                        ToastEvent.StateSaveFailed -> getString(R.string.failed_save_state) to Toast.LENGTH_SHORT
                        ToastEvent.StateStateDoesNotExist -> getString(R.string.cant_load_empty_slot) to Toast.LENGTH_SHORT
                        ToastEvent.CannotLoadSaveStatesWhenRAHardcoreIsEnabled -> getString(R.string.load_states_unavailable_ra_hardcore_enabled) to Toast.LENGTH_LONG
                        ToastEvent.CannotUseCheatsWhenRAHardcoreIsEnabled -> getString(R.string.cheats_unavailable_ra_hardcore_enabled) to Toast.LENGTH_LONG
                        ToastEvent.CannotLoadStateWhenRunningFirmware,
                        ToastEvent.CannotSaveStateWhenRunningFirmware -> getString(R.string.save_states_not_supported) to Toast.LENGTH_LONG
                        ToastEvent.CannotSwitchRetroAchievementsMode -> getString(R.string.retro_achievements_relaunch_to_apply_settings) to Toast.LENGTH_LONG
                        ToastEvent.GbaModeNotSupported -> getString(R.string.emulator_stop_gba_mode_unsupported) to Toast.LENGTH_SHORT
                        ToastEvent.InternalError -> getString(R.string.emulator_stop_internal_error) to Toast.LENGTH_LONG
                        ToastEvent.WirelessUnsupported -> getString(R.string.emulator_stop_wireless_unsupported) to Toast.LENGTH_LONG
                        ToastEvent.OfflineAchievementsLedgerTampered -> getString(R.string.offline_ra_ledger_tampered_toast) to Toast.LENGTH_LONG
                        ToastEvent.OfflineAchievementsSyncFailed -> getString(R.string.offline_ra_sync_failed_toast) to Toast.LENGTH_LONG
                        ToastEvent.PendingRaStateVerificationFailed ->
                            getString(R.string.ra_pending_state_verification_failed) to Toast.LENGTH_LONG
                        ToastEvent.RetroAchievementsAccountChangedInGame ->
                            getString(R.string.retroachievements_account_changed_in_game) to Toast.LENGTH_LONG
                        ToastEvent.RetroAchievementsLogoutFailed ->
                            getString(R.string.retroachievements_logout_failed) to Toast.LENGTH_LONG
                        ToastEvent.RAOfflineProxyNotActive ->
                            getString(R.string.ra_offline_proxy_not_active) to Toast.LENGTH_LONG
                        ToastEvent.RetroAchievementsProviderChangedRestartRequired ->
                            getString(R.string.ra_offline_proxy_restart_required) to Toast.LENGTH_LONG
                        is ToastEvent.HardcoreOfflineUnsyncedWarning -> {
                            getString(R.string.offline_ra_hardcore_unsynced_warning_toast, it.pendingHardcoreCount) to Toast.LENGTH_LONG
                        }
                        is ToastEvent.HardcoreQueueSyncResult -> {
                            val message = when {
                                it.remainingCount == 0 -> getString(R.string.offline_ra_hardcore_sync_result_all, it.submittedCount)
                                it.submittedCount == 0 -> getString(R.string.offline_ra_hardcore_sync_result_none, it.remainingCount)
                                else -> getString(R.string.offline_ra_hardcore_sync_result_partial, it.submittedCount, it.remainingCount)
                            }
                            message to Toast.LENGTH_LONG
                        }
                        is ToastEvent.RetroAchievementsMode -> {
                            val message = when (it.status) {
                                ToastEvent.RetroAchievementsModeStatus.SOFTCORE -> {
                                    getString(R.string.offline_ra_mode_softcore)
                                }
                                ToastEvent.RetroAchievementsModeStatus.HARDCORE -> {
                                    getString(R.string.offline_ra_mode_hardcore)
                                }
                                ToastEvent.RetroAchievementsModeStatus.SOFTCORE_OFFLINE -> {
                                    if (it.hardcoreOfflineDisabled) {
                                        getString(R.string.offline_ra_mode_softcore_offline_hardcore_disabled)
                                    } else if (it.offlineNoInternetAtStart) {
                                        getString(R.string.offline_ra_mode_softcore_offline_no_internet_start)
                                    } else {
                                        getString(R.string.offline_ra_mode_softcore_offline)
                                    }
                                }
                            }
                            message to Toast.LENGTH_LONG
                        }
                        is ToastEvent.OfflineSoftcorePendingNotice -> {
                            val expirationText = getLedgerExpirationText(it.ledgerExpiresInMs)
                            val message = when {
                                it.ledgerExpiresInMs != null && it.ledgerExpiresInMs <= 0L -> {
                                    getString(R.string.offline_ra_pending_softcore_expired_notice, it.pendingSoftcoreCount)
                                }
                                expirationText != null -> {
                                    getString(R.string.offline_ra_pending_softcore_notice_with_expiration, it.pendingSoftcoreCount, expirationText)
                                }
                                else -> {
                                    getString(R.string.offline_ra_pending_softcore_notice, it.pendingSoftcoreCount)
                                }
                            }
                            message to Toast.LENGTH_LONG
                        }
                        is ToastEvent.OfflineAchievementNotSynced -> {
                            val messageRes = when (it.reason) {
                                ToastEvent.OfflineAchievementNotSyncedReason.MISSING_FROM_CURRENT_SET -> R.string.offline_ra_sync_skipped_missing_toast
                                ToastEvent.OfflineAchievementNotSyncedReason.DEFINITION_CHANGED -> R.string.offline_ra_sync_skipped_definition_changed_toast
                                ToastEvent.OfflineAchievementNotSyncedReason.NOT_IN_PREFETCH_CACHE -> R.string.offline_ra_sync_skipped_cache_mismatch_toast
                                ToastEvent.OfflineAchievementNotSyncedReason.SERVER_REJECTED -> R.string.offline_ra_sync_skipped_server_rejected_toast
                            }
                            val message = if (it.reason == ToastEvent.OfflineAchievementNotSyncedReason.SERVER_REJECTED) {
                                getString(
                                    messageRes,
                                    it.title,
                                    it.reasonDetail ?: getString(R.string.offline_ra_sync_skipped_server_rejected_unknown_reason),
                                )
                            } else {
                                getString(messageRes, it.title)
                            }
                            message to Toast.LENGTH_LONG
                        }
                        is ToastEvent.OfflineAchievementsNotSyncedSummary -> {
                            getString(R.string.offline_ra_sync_skipped_summary_toast, it.skippedCount) to Toast.LENGTH_LONG
                        }
                        is ToastEvent.RendererInitFailed -> {
                            val rendererLabel = when (it.renderer) {
                                VideoRenderer.SOFTWARE -> "Software"
                                VideoRenderer.OPENGL -> "OpenGL"
                                VideoRenderer.VULKAN -> "Vulkan"
                                VideoRenderer.COMPUTE -> "Compute"
                            }
                            getString(R.string.renderer_init_failed_message, rendererLabel) to Toast.LENGTH_LONG
                        }
                        is ToastEvent.RendererDebugCaptureLogged -> {
                            getString(R.string.renderer_debug_capture_logged, it.captureId) to Toast.LENGTH_LONG
                        }
                        ToastEvent.RendererDebugCaptureFailed -> {
                            getString(R.string.renderer_debug_capture_failed) to Toast.LENGTH_LONG
                        }
                    }

                    Toast.makeText(this@EmulatorActivity, message, duration).show()
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.CREATED) {
                viewModel.uiEvent.collectLatest {
                    when (it) {
                        EmulatorUiEvent.CloseEmulator -> {
                            closeEmulatorActivity()
                        }
                        is EmulatorUiEvent.OpenScreen.CheatsScreen -> {
                            val intent = Intent(this@EmulatorActivity, CheatsActivity::class.java)
                            intent.putExtra(CheatsActivity.KEY_ROM_INFO, RomInfoParcelable.fromRomInfo(it.romInfo))
                            cheatsLauncher.launch(intent)
                        }
                        is EmulatorUiEvent.OpenScreen.SettingsScreen -> {
                            val settingsIntent = Intent(this@EmulatorActivity, SettingsActivity::class.java).apply {
                                putExtra(SettingsActivity.KEY_IN_GAME, true)
                                putExtra(SettingsActivity.KEY_LOCK_INPUT_MAPPING, it.romSettingsOverrides.controllerMapping)
                                putExtra(SettingsActivity.KEY_LOCK_INPUT_LAYOUT, it.romSettingsOverrides.controllerLayout)
                                putExtra(SettingsActivity.KEY_LOCK_VIDEO_FILTERING, it.romSettingsOverrides.videoFiltering)
                                putExtra(
                                    SettingsActivity.KEY_RA_RUNTIME_IDENTITY_LOCKED,
                                    it.retroAchievementsRuntimeIdentityLocked,
                                )
                                putExtra(
                                    SettingsActivity.KEY_RA_IN_GAME_LOGOUT_SUPPORTED,
                                    it.retroAchievementsInGameLogoutSupported,
                                )
                            }
                            settingsLauncher.launch(settingsIntent)
                        }
                        is EmulatorUiEvent.ShowPauseMenu -> showPauseMenu(it.pauseMenu)
                        is EmulatorUiEvent.ShowRewindWindow -> showRewindWindow(it.rewindWindow)
                        is EmulatorUiEvent.ShowRomSaveStates -> {
                            val isSaving = it.reason == EmulatorUiEvent.ShowRomSaveStates.Reason.SAVING
                            showSaveStateSlotsDialog(it.saveStates, isSaving) { slot ->
                                if (isSaving) {
                                    viewModel.saveStateToSlot(slot)
                                } else {
                                    viewModel.loadStateFromSlot(slot)
                                }
                            }
                        }
                        EmulatorUiEvent.ShowAchievementList -> {
                            activeOverlays.addActiveOverlay(EmulatorOverlay.ACHIEVEMENTS_DIALOG)
                            showAchievementList.value = true
                        }
                        EmulatorUiEvent.ShowPendingSubmissionsDialog -> {
                            activeOverlays.addActiveOverlay(EmulatorOverlay.PENDING_SUBMISSION_CONFIRM_EXIT)
                            showPendingSubmissionsDialog.value = true
                        }
                        is EmulatorUiEvent.ShowScreenGap -> pushConsoleOverlay(
                            ConsoleOverlayNode.Choice(
                                title = getString(R.string.screen_gap),
                                labels = resources.getStringArray(R.array.screen_gap_options).toList(),
                                selectedIndex = it.current.ordinal,
                                onSelect = { index -> viewModel.setScreenGap(ScreenGap.entries[index]) },
                            ),
                        )
                        EmulatorUiEvent.ShowDualScreenPresets -> {
                            activeOverlays.addActiveOverlay(EmulatorOverlay.PRESETS_DIALOG)
                            showDualScreenPresets.value = true
                            requestOverlayHostFocus()
                        }
                        EmulatorUiEvent.ShowRendererDebugMenu -> showRendererDebugMenu()
                        is EmulatorUiEvent.ShowRomSettings -> showRomSettingsMenu(
                            rom = it.rom,
                            renderer = it.renderer,
                            menuState = it.menuState,
                        )
                        EmulatorUiEvent.ShowRenderer2DDebugControls -> {
                            if (isDebuggableBuild()) {
                                showRenderer2DDebugControlsDialog()
                            }
                        }
                        is EmulatorUiEvent.ShowOfflineAchievementsSyncChoice -> {
                            showOfflineAchievementsSyncChoiceDialog(
                                pendingUnlockCount = it.pendingUnlockCount,
                                ledgerExpiresInMs = it.ledgerExpiresInMs,
                            )
                        }
                        is EmulatorUiEvent.ShowOfflineAchievementsSyncProgress -> {
                            showOfflineAchievementsSyncProgressDialog(it.totalUnlockCount)
                        }
                        EmulatorUiEvent.HideOfflineAchievementsSyncProgress -> {
                            offlineSyncProgressDialog?.dismiss()
                            offlineSyncProgressDialog = null
                        }
                    }
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.CREATED) {
                viewModel.pendingRaModalState.collectLatest(::renderRaPendingModalState)
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.rumbleEvent.collect {
                    when (it) {
                        is RumbleEvent.RumbleStart -> emulatorRumbleManager.startRumbling()
                        RumbleEvent.RumbleStop -> emulatorRumbleManager.stopRumbling()
                    }
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.CREATED) {
                viewModel.heavyShaderCompileRequest.collect(::showHeavyShaderCompileDialog)
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.CREATED) {
                viewModel.emulatorState.collectLatest {
                    updateJoystickDispatch()
                    backPressedCallback.isEnabled = it.isRunning() || it is EmulatorState.ValidatingRom || it is EmulatorState.LoadingRom
                    if (!it.isRunning()) releaseVirtualButtonInputs()
                    when (it) {
                        is EmulatorState.Uninitialized -> {
                            binding.viewLayoutControls.isInvisible = true
                            binding.textFps.isGone = true
                            binding.textRenderedIr.isGone = true
                            binding.textLoading.isGone = true
                            binding.progressLoading.isGone = true
                            binding.textLoadingDetail.isGone = true
                        }
                        is EmulatorState.ValidatingFirmware -> {
                            showLoadingState()
                            emulatorLaunchValidatorDelegate.validateFirmware(it.consoleType)
                        }
                        is EmulatorState.ValidatingRom -> {
                            bootRomTitle.value = it.rom.config.customName ?: it.rom.name
                            prepareBootExternalInfo(it.rom)
                            showLoadingState()
                            emulatorLaunchValidatorDelegate.validateRom(it.rom)
                        }
                        is EmulatorState.LoadingFirmware,
                        is EmulatorState.LoadingRom -> {
                            showLoadingState()
                            val compileProgress = when (it) {
                                is EmulatorState.LoadingRom -> it.vulkanCompileProgress
                                is EmulatorState.LoadingFirmware -> it.vulkanCompileProgress
                            }
                            val raLoadStage = (it as? EmulatorState.LoadingRom)?.retroAchievementsLoadStage
                            renderLoadingState(compileProgress, raLoadStage)
                        }
                        is EmulatorState.RunningRom,
                        is EmulatorState.RunningFirmware -> {
                            prewarmOpenGlShadersIfNeeded()
                            (it as? EmulatorState.RunningRom)?.let { running ->
                                bootRomTitle.value = running.rom.config.customName ?: running.rom.name
                            }
                            bootRomReady.value = true
                            presentation?.setInfoOverlayContent(null)
                            setupSustainedPerformanceMode()
                            setupFpsCounter()
                            setupRenderedIrOverlay()
                            binding.textLoading.isGone = true
                            binding.progressLoading.isGone = true
                            binding.textLoadingDetail.isGone = true
                            binding.viewLayoutControls.isVisible = true
                            scheduleStartupPresentationRefreshes()
                            if (
                                lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED) &&
                                !activeOverlays.hasActiveOverlays() &&
                                viewModel.canResumeEmulatorFromLifecycle()
                            ) {
                                viewModel.resumeEmulator()
                            }
                        }
                        is EmulatorState.RomLoadError,
                        is EmulatorState.FrontendSaveSelectionRequired -> {
                            binding.viewLayoutControls.isInvisible = true
                            binding.textFps.isGone = true
                            binding.textRenderedIr.isGone = true
                            binding.textLoading.isGone = true
                            binding.progressLoading.isGone = true
                            binding.textLoadingDetail.isGone = true
                            showBootAnimation.value = false
                            presentation?.setInfoOverlayContent(null)
                            if (it is EmulatorState.FrontendSaveSelectionRequired) {
                                showFrontendSaveSelectionDialog(it.message)
                            } else if (it is EmulatorState.RomLoadError) {
                                showRomLoadErrorDialog(it.reason)
                            }
                        }
                        is EmulatorState.FirmwareLoadError -> {
                            binding.viewLayoutControls.isInvisible = true
                            binding.textFps.isGone = true
                            binding.textRenderedIr.isGone = true
                            binding.textLoading.isGone = true
                            binding.progressLoading.isGone = true
                            binding.textLoadingDetail.isGone = true
                            showBootAnimation.value = false
                            presentation?.setInfoOverlayContent(null)
                            showFirmwareLoadErrorDialog(it)
                        }
                        is EmulatorState.RomNotFoundError -> {
                            binding.viewLayoutControls.isInvisible = true
                            binding.textFps.isGone = true
                            binding.textRenderedIr.isGone = true
                            binding.textLoading.isGone = true
                            binding.progressLoading.isGone = true
                            binding.textLoadingDetail.isGone = true
                            showBootAnimation.value = false
                            presentation?.setInfoOverlayContent(null)
                            showRomNotFoundDialog(it.romPath)
                        }
                    }
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                observeSeparatingFolds(binding.viewLayoutControls, viewModel::setFoldableWindowDensity).collect { folds ->
                    viewModel.setScreenFolds(folds)
                }
            }
        }
        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.CREATED) {
                appForegroundStateObserver.onAppMovedToBackgroundEvent.collect {
                    presentation?.dismiss()
                    presentation = null
                    viewModel.onAppMovedToBackground()
                }
            }
        }
    }

    private fun showLoadingState() {
        binding.viewLayoutControls.isInvisible = true
        binding.textFps.isGone = true
        binding.textRenderedIr.isGone = true
        binding.textLoading.isVisible = true
        if (bootStatus.value == null) {
            bootStatus.value = getString(R.string.info_loading)
        }
    }

    private fun renderLoadingState(progress: VulkanCompileProgress?, raLoadStage: RetroAchievementsLoadStage? = null) {
        if (raLoadStage == RetroAchievementsLoadStage.FETCHING_LATEST_DATA) {
            binding.textLoading.setText(R.string.info_refreshing_retroachievements_title)
            binding.progressLoading.isVisible = true
            binding.progressLoading.isIndeterminate = true
            binding.textLoadingDetail.isVisible = true
            binding.textLoadingDetail.setText(R.string.info_refreshing_retroachievements_detail)
            bootStatus.value = getString(R.string.info_refreshing_retroachievements_title)
            return
        }

        if (progress == null || progress.total <= 0) {
            binding.textLoading.setText(R.string.info_loading)
            binding.progressLoading.isVisible = true
            binding.progressLoading.isIndeterminate = true
            binding.textLoadingDetail.isGone = true
            bootStatus.value = getString(R.string.info_loading)
            return
        }

        binding.textLoading.setText(
            if (progress.stageId == 5) {
                R.string.info_retroarch_compiling_title
            } else {
                R.string.info_vulkan_compiling_title
            },
        )
        binding.progressLoading.isVisible = true
        binding.progressLoading.isIndeterminate = true
        binding.textLoadingDetail.isVisible = true
        binding.textLoadingDetail.text = getVulkanCompileStageLabel(progress.stageId)
        bootStatus.value = getString(
            if (progress.stageId == 5) R.string.info_retroarch_compiling_title else R.string.info_vulkan_compiling_title,
        )
    }

    private fun getVulkanCompileStageLabel(stageId: Int): String {
        val labelRes = when (stageId) {
            1 -> R.string.info_vulkan_compiling_stage_init
            2 -> R.string.info_vulkan_compiling_stage_pipelines
            3 -> R.string.info_vulkan_compiling_stage_output
            4 -> R.string.info_vulkan_compiling_stage_warmup
            5 -> R.string.info_vulkan_compiling_stage_retroarch
            else -> R.string.info_vulkan_compiling_stage_init
        }
        return getString(labelRes)
    }

    private fun showOfflineAchievementsSyncChoiceDialog(
        pendingUnlockCount: Int,
        ledgerExpiresInMs: Long?,
    ) {
        val expirationText = getLedgerExpirationText(ledgerExpiresInMs)
        val message = if (expirationText != null) {
            getString(R.string.offline_ra_pending_message_with_expiration, pendingUnlockCount, expirationText)
        } else {
            getString(R.string.offline_ra_pending_message, pendingUnlockCount)
        }
        offlineSyncChoiceDialog?.dismiss()
        offlineSyncChoiceDialog = AlertDialog.Builder(this)
            .setTitle(getString(R.string.offline_ra_pending_title))
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton(R.string.offline_ra_sync_now) { _, _ ->
                viewModel.submitOfflineAchievementsSyncChoice(OfflineAchievementsSyncChoice.SYNC_NOW)
            }
            .setNegativeButton(R.string.offline_ra_continue_offline) { _, _ ->
                viewModel.submitOfflineAchievementsSyncChoice(OfflineAchievementsSyncChoice.CONTINUE_OFFLINE)
            }
            .show()
    }

    private fun getLedgerExpirationText(expiresInMs: Long?): String? {
        if (expiresInMs == null) return null
        if (expiresInMs <= 0L) return getString(R.string.offline_ra_ledger_expired)

        val days = ((expiresInMs + LEDGER_EXPIRATION_DAY_MS - 1L) / LEDGER_EXPIRATION_DAY_MS)
            .coerceAtLeast(1L)
            .toInt()
        return resources.getQuantityString(R.plurals.offline_ra_ledger_expires_days, days, days)
    }

    private fun showHardcorePendingExitWarningDialog(
        requestId: Long,
        pending: RaPendingCounts,
        allowContinuePlaying: Boolean,
    ) {
        hardcorePendingExitDialog?.dismiss()
        activeOverlays.addActiveOverlay(EmulatorOverlay.RA_PENDING_EXIT)
        val builder = AlertDialog.Builder(this)
            .setTitle(getString(R.string.ra_pending_exit_title))
            .setMessage(
                getString(
                    R.string.ra_pending_exit_message,
                    pending.total,
                    pending.achievementUnlocks,
                    pending.leaderboardEntries,
                ),
            )
            .setCancelable(false)
            .setPositiveButton(R.string.ra_pending_sync_and_exit) { _, _ ->
                viewModel.submitHardcorePendingExitChoice(
                    requestId,
                    HardcorePendingExitChoice.SYNC_AND_EXIT,
                )
            }
            .setNegativeButton(R.string.ra_pending_discard_and_exit) { _, _ ->
                viewModel.submitHardcorePendingExitChoice(
                    requestId,
                    HardcorePendingExitChoice.DISCARD_AND_EXIT,
                )
            }
        if (allowContinuePlaying) {
            builder.setNeutralButton(R.string.offline_ra_continue_playing_button) { _, _ ->
                viewModel.submitHardcorePendingExitChoice(
                    requestId,
                    HardcorePendingExitChoice.CONTINUE_PLAYING,
                )
            }
        }
        val dialog = builder.create()
        dialog.setOnDismissListener {
            if (hardcorePendingExitDialog === dialog) {
                activeOverlays.removeActiveOverlay(EmulatorOverlay.RA_PENDING_EXIT)
                hardcorePendingExitDialog = null
            }
        }
        hardcorePendingExitDialog = dialog
        dialog.show()
    }

    private fun showRaPendingSyncProgressDialog(pending: RaPendingCounts) {
        raPendingSyncResultDialog?.dismiss()
        raPendingSyncProgressDialog?.dismiss()
        activeOverlays.addActiveOverlay(EmulatorOverlay.RA_PENDING_SYNC)
        val dialog = AlertDialog.Builder(this)
            .setTitle(R.string.ra_pending_syncing_title)
            .setMessage(
                getString(
                    R.string.ra_pending_syncing_message,
                    pending.total,
                    pending.achievementUnlocks,
                    pending.leaderboardEntries,
                ),
            )
            .setCancelable(false)
            .create()
        dialog.setOnDismissListener {
            if (raPendingSyncProgressDialog === dialog) {
                activeOverlays.removeActiveOverlay(EmulatorOverlay.RA_PENDING_SYNC)
                raPendingSyncProgressDialog = null
            }
        }
        raPendingSyncProgressDialog = dialog
        dialog.show()
    }

    private fun showRaPendingSyncResultDialog(
        requestId: Long,
        result: RaPendingSyncResult,
        action: RaPendingSyncResultAction,
    ) {
        raPendingSyncProgressDialog?.dismiss()
        raPendingSyncProgressDialog = null
        raPendingSyncResultDialog?.dismiss()
        activeOverlays.addActiveOverlay(EmulatorOverlay.RA_PENDING_SYNC)
        val dialog = AlertDialog.Builder(this)
            .setTitle(R.string.ra_pending_sync_result_title)
            .setMessage(
                getString(
                    R.string.ra_pending_sync_result_message,
                    result.submittedAchievements,
                    result.submittedLeaderboardEntries,
                    result.alreadyAccepted,
                    result.failedAchievements,
                    result.failedLeaderboardEntries,
                    result.remaining.total,
                    result.remaining.permanentFailures,
                ),
            )
            .setCancelable(false)
            .setPositiveButton(
                when (action) {
                    RaPendingSyncResultAction.REOPEN_PAUSE_MENU -> R.string.pause
                    RaPendingSyncResultAction.RESUME_SESSION ->
                        R.string.offline_ra_continue_playing_button
                    RaPendingSyncResultAction.REOPEN_TERMINAL_EXIT ->
                        R.string.ra_pending_review_submissions
                },
            ) { _, _ ->
                viewModel.submitRaPendingSyncResultAction(requestId, action)
            }
            .create()
        dialog.setOnDismissListener {
            if (raPendingSyncResultDialog === dialog) {
                activeOverlays.removeActiveOverlay(EmulatorOverlay.RA_PENDING_SYNC)
                raPendingSyncResultDialog = null
            }
        }
        raPendingSyncResultDialog = dialog
        dialog.show()
    }

    private fun renderRaPendingModalState(state: RaPendingModalState) {
        hardcorePendingExitDialog?.setOnDismissListener(null)
        hardcorePendingExitDialog?.dismiss()
        hardcorePendingExitDialog = null
        raPendingSyncProgressDialog?.setOnDismissListener(null)
        raPendingSyncProgressDialog?.dismiss()
        raPendingSyncProgressDialog = null
        raPendingSyncResultDialog?.setOnDismissListener(null)
        raPendingSyncResultDialog?.dismiss()
        raPendingSyncResultDialog = null
        activeOverlays.removeActiveOverlay(EmulatorOverlay.RA_PENDING_EXIT)
        activeOverlays.removeActiveOverlay(EmulatorOverlay.RA_PENDING_SYNC)

        when (state) {
            RaPendingModalState.None -> Unit
            is RaPendingModalState.ExitPrompt -> {
                showHardcorePendingExitWarningDialog(
                    requestId = state.requestId,
                    pending = state.pending,
                    allowContinuePlaying =
                        state.exitContext == RaPendingExitContext.RESUMABLE_SESSION,
                )
            }
            is RaPendingModalState.Syncing -> {
                showRaPendingSyncProgressDialog(state.pending)
            }
            is RaPendingModalState.Result -> {
                showRaPendingSyncResultDialog(
                    requestId = state.requestId,
                    result = state.result,
                    action = state.action,
                )
            }
        }
    }

    private fun showOfflineAchievementsSyncProgressDialog(totalUnlockCount: Int) {
        offlineSyncProgressDialog?.dismiss()
        offlineSyncProgressDialog = AlertDialog.Builder(this)
            .setTitle(getString(R.string.offline_ra_syncing_title))
            .setMessage(getString(R.string.offline_ra_syncing_message, totalUnlockCount))
            .setCancelable(false)
            .create()

        offlineSyncProgressDialog?.show()
    }

    override fun onStart() {
        super.onStart()
        if (isClosingEmulator) {
            return
        }
        if (PhantomPlayer.current == null) gameDisplayOwner = Any()
        updateDisplays()
        getSystemService<DisplayManager>()?.registerDisplayListener(displayListener, null)
        getSystemService<InputManager>()?.registerInputDeviceListener(connectedControllerManager, null)
        connectedControllerManager.startTrackingControllers()
        frameRenderCoordinator.addSurface(binding.surfaceMain)
    }

    private fun updateDisplays() {
        if (isClosingEmulator) {
            return
        }
        val currentDisplay = ContextCompat.getDisplayOrDefault(this)
        val secondaryDisplay = secondaryDisplaySelector.getSecondaryDisplay(this) {
            PhantomPlayer.current == null && GamePanelActivity.canOpen(this, it)
        }.takeIf { externalDisplayMode == ExternalDisplayMode.MELON_DUAL_DS }

        val displays = deviceLayoutDisplayMapper.mapDisplaysToLayoutDisplays(currentDisplay, secondaryDisplay)
        viewModel.setConnectedDisplays(displays)

        val usePanelActivity = PhantomPlayer.current == null && currentDisplay.displayId == Display.DEFAULT_DISPLAY &&
            displays.mainScreenDisplay.type == LayoutDisplay.Type.BUILT_IN &&
            displays.secondaryScreenDisplay?.type == LayoutDisplay.Type.BUILT_IN
        showExternalDisplay(secondaryDisplay.takeIf { PhantomPlayer.current == null || phantomPlayerResumed }, usePanelActivity)
        gameDisplayOwner?.let { owner ->
            val occupied = setOfNotNull(currentDisplay.displayId, presentation?.display?.displayId)
            ExternalInfoDisplayController.setGameDisplays(owner, occupied)
        }
    }

    private fun showExternalDisplay(secondaryDisplay: Display?, usePanelActivity: Boolean = false) {
        if (isClosingEmulator) {
            return
        }
        if (presentation?.display?.displayId == secondaryDisplay?.displayId) {
            return
        }

        presentation?.dismiss()
        presentation = null

        if (secondaryDisplay != null) {
            presentation = ExternalPresentation(
                context = this,
                display = secondaryDisplay,
                frameRenderCoordinator = frameRenderCoordinator,
                usePanelActivity = usePanelActivity,
                excludeTouchScreenFromSystemGestures = excludeTouchScreenFromSystemGestures,
            ).apply {
                layoutView.apply {
                    setLayoutComponentViewBuilderFactory(RuntimeLayoutComponentViewBuilderFactory())
                    setFrontendInputHandler(frontendInputHandler)
                    setSystemInputHandler(melonTouchHandler)
                    setSlot2AnalogInput(slot2AnalogInput)
                    viewModel.runtimeLayout.value?.let {
                        updateLayout(it)
                    }

                    setLayoutComponentToggleState(LayoutComponent.BUTTON_FAST_FORWARD_TOGGLE, frontendInputHandler.fastForwardEnabled)
                    setLayoutComponentToggleState(LayoutComponent.BUTTON_MICROPHONE_TOGGLE, frontendInputHandler.microphoneEnabled)
                    setConnectedControllersState(connectedControllerManager.controllersState.value)
                }

                updateRendererConfiguration(viewModel.runtimeRendererConfiguration.value)
                updateBackground(viewModel.secondaryScreenBackground.value)
                if (binding.viewLayoutControls.areScreensSwapped()) {
                    swapScreens()
                }
                if (activeOverlays.hasActiveOverlays()) {
                    setPauseOverlayVisibility(true)
                }

            }.takeIf { it.showOnDisplay() }
            if (presentation == null) {
                val primary = ContextCompat.getDisplayOrDefault(this)
                viewModel.setConnectedDisplays(deviceLayoutDisplayMapper.mapDisplaysToLayoutDisplays(primary, null))
                return
            }
            if (showBootAnimation.value) {
                showBootAnimationOnExternalDisplay()
            }
            scheduleStartupPresentationRefreshes()
        }
    }

    private fun showBootAnimationOnExternalDisplay() {
        presentation?.setInfoOverlayContent {
            me.magnum.melonds.ui.emulator.ui.DsBootOverlay(
                half = me.magnum.melonds.ui.emulator.ui.DsBootScreenHalf.BOTH,
                romReady = bootRomReady.value,
                romTitle = bootRomTitle.value,
                statusText = bootStatus.value,
                onFinished = { presentation?.setInfoOverlayContent(null) },
            )
        }
    }

    private fun prepareBootExternalInfo(rom: Rom) {
        bootRom.value = rom
        bootBoxArtUrl.value = null
        lifecycleScope.launch {
            val url = runCatching { boxArtRepository.getBoxArtUrl(rom) }.getOrNull()
            if (bootRom.value?.uri == rom.uri) {
                bootBoxArtUrl.value = url
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)

        val launchArgs = LaunchArgs.fromIntent(intent)
        // Invalid arguments. Ignore completely
        if (launchArgs == null)
            return

        lifecycleScope.launch {
            if (viewModel.matchesRunningLaunch(launchArgs)) {
                setIntent(intent)
                return@launch
            }
            if (!viewModel.emulatorState.value.isRunning()) return@launch
            viewModel.pauseEmulator(false)

            activeOverlays.addActiveOverlay(EmulatorOverlay.SWITCH_NEW_ROM_DIALOG)
            AlertDialog.Builder(this@EmulatorActivity)
                    .setTitle(getString(R.string.title_emulator_running))
                    .setMessage(getString(R.string.message_stop_emulation))
                    .setPositiveButton(R.string.ok) { _, _ ->
                        setIntent(intent)
                        viewModel.relaunchWithNewArgs(launchArgs)
                    }
                    .setNegativeButton(R.string.no) { dialog, _ ->
                        dialog.cancel()
                    }
                    .setOnDismissListener {
                        activeOverlays.removeActiveOverlay(EmulatorOverlay.SWITCH_NEW_ROM_DIALOG)
                    }
                    .setOnCancelListener {
                        viewModel.resumeEmulator()
                    }
                    .show()
        }
    }

    override fun onResume() {
        super.onResume()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.viewTreeObserver.addOnGlobalFocusChangeListener(joystickFocusListener)
            updateJoystickDispatch(resumed = true)
        }
        if (PhantomPlayer.current != null) {
            phantomPlayerResumed = true
            updateDisplays()
        }
        viewModel.onHostResumed()
        choreographerFrameRenderer.startRendering()
        startShaderDiagnosticsPolling()

        if (
            !activeOverlays.hasActiveOverlays() &&
            viewModel.canResumeEmulatorFromLifecycle()
        ) {
            disableScreenTimeOut()
            viewModel.resumeEmulator()
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        updateJoystickDispatch()
        if (!hasFocus) releaseVirtualButtonInputs()
        setupFullscreen()
    }

    private fun updateJoystickDispatch(resumed: Boolean = lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        val target = if (resumed && hasWindowFocus() && MelonEmulator.isLowLatencyEnabled &&
            viewModel.emulatorState.value.isRunning() && !activeOverlays.hasActiveOverlays()
        ) currentFocus ?: window.decorView else null
        if (joystickDispatchView === target) return
        joystickDispatchView?.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_NONE)
        target?.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_JOYSTICK)
        joystickDispatchView = target
    }

    private fun setupFullscreen() {
        window.insetsControllerCompat?.let {
            it.hide(WindowInsetsCompat.Type.navigationBars())
            it.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun setupSustainedPerformanceMode() {
        window.setSustainedPerformanceMode(viewModel.isSustainedPerformanceModeEnabled())
    }

    private fun setupFpsCounter() {
        positionOverlayText(binding.textFps, viewModel.getFpsCounterPosition())
    }

    private fun setupRenderedIrOverlay() {
        positionOverlayText(binding.textRenderedIr, viewModel.getRenderedIrPosition())
    }

    private fun positionOverlayText(view: android.widget.TextView, fpsCounterPosition: FpsCounterPosition) {
        if (fpsCounterPosition == FpsCounterPosition.HIDDEN) {
            view.isGone = true
        } else {
            view.isVisible = true
            val newParams = view.layoutParams as ConstraintLayout.LayoutParams
            when (fpsCounterPosition) {
                FpsCounterPosition.TOP_LEFT -> {
                    newParams.topToTop = ConstraintLayout.LayoutParams.PARENT_ID
                    newParams.leftToLeft = ConstraintLayout.LayoutParams.PARENT_ID
                }
                FpsCounterPosition.TOP_CENTER -> {
                    newParams.topToTop = ConstraintLayout.LayoutParams.PARENT_ID
                    newParams.leftToLeft = ConstraintLayout.LayoutParams.PARENT_ID
                    newParams.rightToRight = ConstraintLayout.LayoutParams.PARENT_ID
                }
                FpsCounterPosition.TOP_RIGHT -> {
                    newParams.topToTop = ConstraintLayout.LayoutParams.PARENT_ID
                    newParams.rightToRight = ConstraintLayout.LayoutParams.PARENT_ID
                }
                FpsCounterPosition.BOTTOM_LEFT -> {
                    newParams.bottomToBottom = ConstraintLayout.LayoutParams.PARENT_ID
                    newParams.leftToLeft = ConstraintLayout.LayoutParams.PARENT_ID
                }
                FpsCounterPosition.BOTTOM_CENTER -> {
                    newParams.bottomToBottom = ConstraintLayout.LayoutParams.PARENT_ID
                    newParams.leftToLeft = ConstraintLayout.LayoutParams.PARENT_ID
                    newParams.rightToRight = ConstraintLayout.LayoutParams.PARENT_ID
                }
                FpsCounterPosition.BOTTOM_RIGHT -> {
                    newParams.bottomToBottom = ConstraintLayout.LayoutParams.PARENT_ID
                    newParams.rightToRight = ConstraintLayout.LayoutParams.PARENT_ID
                }
                FpsCounterPosition.HIDDEN -> { /* Do nothing here */ }
            }
            view.layoutParams = newParams
        }
    }

    private fun setupSoftInput(layoutConfiguration: RuntimeInputLayoutConfiguration?) {
        if (::nativeInputListener.isInitialized) nativeInputListener.releaseChordInputs()
        if (layoutConfiguration != null) {
            setLayoutOrientation(layoutConfiguration.layoutOrientation)
            with(binding.viewLayoutControls) {
                instantiateLayout(layoutConfiguration, LayoutTarget.MAIN_SCREEN)
                setLayoutComponentToggleState(LayoutComponent.BUTTON_FAST_FORWARD_TOGGLE, frontendInputHandler.fastForwardEnabled)
                setLayoutComponentToggleState(LayoutComponent.BUTTON_MICROPHONE_TOGGLE, frontendInputHandler.microphoneEnabled)
            }
            presentation?.apply {
                updateLayout(layoutConfiguration)
                layoutView.setLayoutComponentToggleState(LayoutComponent.BUTTON_FAST_FORWARD_TOGGLE, frontendInputHandler.fastForwardEnabled)
                layoutView.setLayoutComponentToggleState(LayoutComponent.BUTTON_MICROPHONE_TOGGLE, frontendInputHandler.microphoneEnabled)
            }

            handler.post {
                applyDualScreenPresetSwapState()
                updateRendererScreenAreas()
                presentation?.updateRendererScreenAreas()
                scheduleStartupPresentationRefreshes()
            }
        } else {
            binding.viewLayoutControls.destroyLayout()
            presentation?.layoutView?.destroyLayout()
        }
    }

    private fun applyDualScreenPresetSwapState(preset: DualScreenPreset = viewModel.dualScreenPreset.value) {
        if (preset == DualScreenPreset.OFF || presentation == null) {
            return
        }

        val desiredInternalScreen = when (preset) {
            DualScreenPreset.INTERNAL_TOP_EXTERNAL_BOTTOM -> LayoutComponent.TOP_SCREEN
            DualScreenPreset.INTERNAL_BOTTOM_EXTERNAL_TOP -> LayoutComponent.BOTTOM_SCREEN
            DualScreenPreset.OFF -> return
        }

        val baselineInternalScreen = getBaselineInternalScreenComponent() ?: return
        val shouldSwap = baselineInternalScreen != desiredInternalScreen

        if (binding.viewLayoutControls.areScreensSwapped() != shouldSwap) {
            binding.viewLayoutControls.swapScreens()
        }
        presentation?.layoutView?.let { layoutView ->
            if (layoutView.areScreensSwapped() != shouldSwap) {
                layoutView.swapScreens()
            }
        }
    }

    private fun getBaselineInternalScreenComponent(): LayoutComponent? {
        val hasTop = binding.viewLayoutControls.getLayoutComponentView(LayoutComponent.TOP_SCREEN) != null
        val hasBottom = binding.viewLayoutControls.getLayoutComponentView(LayoutComponent.BOTTOM_SCREEN) != null
        return when {
            hasTop && !hasBottom -> LayoutComponent.TOP_SCREEN
            hasBottom && !hasTop -> LayoutComponent.BOTTOM_SCREEN
            hasBottom -> LayoutComponent.BOTTOM_SCREEN
            hasTop -> LayoutComponent.TOP_SCREEN
            else -> null
        }
    }

    private fun swapScreen() {
        binding.viewLayoutControls.swapScreens()
        presentation?.swapScreens()

        updateRendererScreenAreas()
        scheduleStartupPresentationRefreshes()
    }

    private fun updateRendererScreenAreas() {
        if (isClosingEmulator || isFrameRenderCoordinatorStopped) {
            return
        }
        val areas = resolveMainScreenPresentationAreas()
        updateOpenGlRetroArchFilterConfiguration(currentRuntimeRendererConfiguration)
        mainScreenRenderer.updateScreenAreas(
            areas.topScreenRect,
            areas.bottomScreenRect,
            areas.topAlpha,
            areas.bottomAlpha,
            areas.topOnTop,
            areas.bottomOnTop,
            areas.hybridTopScreenRect,
            areas.hybridBottomScreenRect,
            areas.hybridAlpha,
            areas.hybridOnTop,
        )
        frameRenderCoordinator.updateSurfacePresentation(
            binding.surfaceMain,
            buildVulkanPresentationConfig(
                topScreenRect = areas.topScreenRect,
                bottomScreenRect = areas.bottomScreenRect,
                topAlpha = areas.topAlpha,
                bottomAlpha = areas.bottomAlpha,
                topOnTop = areas.topOnTop,
                bottomOnTop = areas.bottomOnTop,
                hybridTopScreenRect = areas.hybridTopScreenRect,
                hybridBottomScreenRect = areas.hybridBottomScreenRect,
                hybridAlpha = areas.hybridAlpha,
                hybridOnTop = areas.hybridOnTop,
            ),
            currentMainScreenBackground,
        )

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && window?.decorView?.isAttachedToWindow == true) {
            val touchScreenArea = if (excludeTouchScreenFromSystemGestures) {
                listOfNotNull(areas.bottomScreenRect, areas.hybridBottomScreenRect).map {
                    android.graphics.Rect(it.x, it.y, it.right, it.bottom)
                }
            } else null
            window?.systemGestureExclusionRects = touchScreenArea.orEmpty()
        }
    }

    private fun resolveMainScreenPresentationAreas(): ScreenPresentationAreas {
        val (topScreen, bottomScreen) = if (binding.viewLayoutControls.areScreensSwapped()) {
            LayoutComponent.BOTTOM_SCREEN to LayoutComponent.TOP_SCREEN
        } else {
            LayoutComponent.TOP_SCREEN to LayoutComponent.BOTTOM_SCREEN
        }
        val topView = binding.viewLayoutControls.getLayoutComponentView(topScreen)
        val bottomView = binding.viewLayoutControls.getLayoutComponentView(bottomScreen)
        val hybridView = binding.viewLayoutControls.getLayoutComponentView(LayoutComponent.HYBRID_SCREEN)
        val (hybridTopRect, hybridBottomRect) = hybridView?.let { splitHybridScreenRect(it.getRect()) } ?: (null to null)
        return ScreenPresentationAreas(
            topScreenRect = topView?.getRect(),
            bottomScreenRect = bottomView?.getRect(),
            topAlpha = topView?.baseAlpha ?: 1f,
            bottomAlpha = bottomView?.baseAlpha ?: 1f,
            topOnTop = topView?.onTop ?: false,
            bottomOnTop = bottomView?.onTop ?: false,
            hybridTopScreenRect = hybridTopRect,
            hybridBottomScreenRect = hybridBottomRect,
            hybridAlpha = hybridView?.baseAlpha ?: 1f,
            hybridOnTop = hybridView?.onTop ?: false,
        )
    }

    private fun splitHybridScreenRect(rect: Rect): Pair<Rect, Rect> {
        val topHeight = max(1, rect.height / 2)
        val bottomHeight = max(1, rect.height - topHeight)
        return Rect(rect.x, rect.y, rect.width, topHeight) to Rect(rect.x, rect.y + topHeight, rect.width, bottomHeight)
    }

    private fun ensurePresentationBackend(renderer: VideoRenderer) {
        if (isClosingEmulator) {
            return
        }
        val targetBackend = renderer.toPresentationBackend()
        if (targetBackend == currentPresentationBackend) {
            return
        }

        val wasRendering = lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)
        choreographerFrameRenderer.stopRendering()

        if (lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) {
            frameRenderCoordinator.removeSurface(binding.surfaceMain)
        }
        presentation?.dismiss()
        presentation = null
        frameRenderCoordinator.stop()

        currentPresentationBackend = targetBackend
        frameRenderCoordinator = createFrameRenderCoordinator(targetBackend)
        prewarmedOpenGlRetroArchFilterKey = null
        isFrameRenderCoordinatorStopped = false
        choreographerFrameRenderer = ChoreographerFrameRendererFactory.createFrameRenderer(frameRenderCoordinator)

        if (lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) {
            frameRenderCoordinator.addSurface(binding.surfaceMain)
            updateDisplays()
        }

        updateRendererScreenAreas()
        scheduleStartupPresentationRefreshes()

        if (wasRendering) {
            choreographerFrameRenderer.startRendering()
        }
    }

    private fun scheduleStartupPresentationRefreshes() {
        cancelStartupPresentationRefreshes()
        if (isClosingEmulator) {
            return
        }
        if (currentPresentationBackend != PresentationBackend.VULKAN) {
            return
        }

        startupPresentationRefreshAttempts = 0
        val refreshRunnable = object : Runnable {
            override fun run() {
                if (isDestroyed || currentPresentationBackend != PresentationBackend.VULKAN) {
                    startupPresentationRefreshRunnable = null
                    return
                }

                updateRendererScreenAreas()
                presentation?.updateRendererScreenAreas()

                startupPresentationRefreshAttempts += 1
                if (startupPresentationRefreshAttempts < STARTUP_PRESENTATION_REFRESH_ATTEMPTS) {
                    handler.postDelayed(this, STARTUP_PRESENTATION_REFRESH_INTERVAL_MS)
                } else {
                    startupPresentationRefreshRunnable = null
                }
            }
        }
        startupPresentationRefreshRunnable = refreshRunnable
        handler.post(refreshRunnable)
    }

    private fun cancelStartupPresentationRefreshes() {
        startupPresentationRefreshRunnable?.let { handler.removeCallbacks(it) }
        startupPresentationRefreshRunnable = null
        startupPresentationRefreshAttempts = 0
    }

    private fun updateOpenGlRetroArchFilterConfiguration(configuration: RuntimeRendererConfiguration?) {
        val enabled = configuration?.videoFiltering == VideoFiltering.RETROARCH &&
            configuration.renderer != VideoRenderer.VULKAN
        val presetPath = configuration?.retroArchShader?.presetPath.takeIf { enabled }
        val parameterOverrides = if (enabled) {
            configuration?.retroArchShader?.parameterOverrides
                ?.entries
                ?.joinToString(",") { "${it.key}=${it.value}" }
        } else {
            null
        }
        val clearHistory = enabled && configuration?.retroArchShader?.clearHistory == true
        val sourceResolution = configuration?.retroArchShader?.sourceResolution?.name?.lowercase() ?: "vulkan_ir"
        val layoutSize = if (enabled) resolveMaxShaderLayoutSize() else 0 to 0
        val passCount = if (enabled) configuration?.retroArchShader?.passCount ?: 0 else 0

        val key = "$enabled|$presetPath|$parameterOverrides|$sourceResolution|${layoutSize.first}x${layoutSize.second}|$passCount"
        if (key == lastOpenGlRetroArchFilterKey && !clearHistory) {
            return
        }
        lastOpenGlRetroArchFilterKey = key

        runCatching {
            MelonEmulator.configureOpenGlRetroArchFilter(
                enabled,
                presetPath,
                parameterOverrides,
                clearHistory,
                sourceResolution,
                layoutSize.first,
                layoutSize.second,
                passCount,
            )
        }
    }

    private fun showHeavyShaderCompileDialog(request: EmulatorViewModel.HeavyShaderCompileRequest) {
        val duration = formatShaderCompileDuration(request.estimatedMillis)
        val message = getString(
            if (request.isMeasured) R.string.shader_heavy_compile_measured else R.string.shader_heavy_compile_estimated,
            request.presetName,
            duration,
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.shader_heavy_compile_title)
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton(R.string.shader_heavy_compile_continue) { _, _ -> request.response.complete(true) }
            .setNegativeButton(R.string.shader_heavy_compile_skip) { _, _ -> request.response.complete(false) }
            .show()
    }

    private fun formatShaderCompileDuration(millis: Long): String {
        val totalSeconds = (millis / 1000).coerceAtLeast(1)
        return if (totalSeconds >= 60) {
            getString(R.string.shader_duration_minutes, totalSeconds / 60, totalSeconds % 60)
        } else {
            getString(R.string.shader_duration_seconds, totalSeconds)
        }
    }

    private suspend fun prewarmOpenGlShadersIfNeeded() {
        val configuration = currentRuntimeRendererConfiguration ?: return
        if (configuration.videoFiltering != VideoFiltering.RETROARCH || configuration.renderer == VideoRenderer.VULKAN) {
            return
        }
        if (configuration.retroArchShader.presetPath.isNullOrEmpty()) {
            return
        }
        if (configuration.retroArchShader.sourceResolution == RetroArchShaderSourceResolution.NATIVE) {
            withTimeoutOrNull(SHADER_PREWARM_LAYOUT_TIMEOUT_MS) {
                while (resolveMaxShaderLayoutSize().first == 0) {
                    delay(SHADER_PREWARM_LAYOUT_POLL_MS)
                }
            }
        }
        updateOpenGlRetroArchFilterConfiguration(configuration)
        val key = lastOpenGlRetroArchFilterKey ?: return
        if (key == prewarmedOpenGlRetroArchFilterKey) {
            return
        }
        prewarmedOpenGlRetroArchFilterKey = key

        val scale = if (configuration.renderer == VideoRenderer.SOFTWARE) 1 else configuration.resolutionScaling.coerceAtLeast(1)
        val atlasWidth = DS_SCREEN_WIDTH * scale
        val atlasHeight = DS_FRAME_ATLAS_HEIGHT * scale

        binding.textLoading.setText(R.string.info_retroarch_compiling_title)
        binding.progressLoading.isVisible = true
        binding.progressLoading.isIndeterminate = true
        binding.textLoadingDetail.isVisible = true
        binding.textLoadingDetail.setText(R.string.info_vulkan_compiling_stage_retroarch)
        bootStatus.value = getString(R.string.info_retroarch_compiling_wait)
        delay(SHADER_PREWARM_MESSAGE_SETTLE_MS)

        val elapsedMillis = withContext(Dispatchers.Default) {
            runCatching { frameRenderCoordinator.prewarmShaders(atlasWidth, atlasHeight) }.getOrDefault(0L)
        }
        bootStatus.value = getString(R.string.info_loading)
        configuration.retroArchShader.presetPath?.let {
            shaderCompileTimeStore.record(it, ShaderCompileTimeStore.Backend.OPEN_GL, elapsedMillis)
        }
        drainShaderDiagnostics()
    }

    private fun startShaderDiagnosticsPolling() {
        stopShaderDiagnosticsPolling()
        val runnable = object : Runnable {
            override fun run() {
                drainShaderDiagnostics()
                handler.postDelayed(this, SHADER_DIAGNOSTICS_POLL_MS)
            }
        }
        shaderDiagnosticsRunnable = runnable
        handler.postDelayed(runnable, SHADER_DIAGNOSTICS_POLL_MS)
    }

    private fun stopShaderDiagnosticsPolling() {
        shaderDiagnosticsRunnable?.let { handler.removeCallbacks(it) }
        shaderDiagnosticsRunnable = null
    }

    private fun drainShaderDiagnostics() {
        val records = runCatching { MelonEmulator.consumeShaderDiagnostics() }.getOrNull() ?: return
        if (records.isEmpty()) {
            return
        }

        val entries = shaderCompatibilityLog.append(records)
        val failure = entries.firstOrNull { !it.succeeded } ?: return
        val reason = failure.reason.lineSequence().firstOrNull { it.isNotBlank() }.orEmpty()
        Toast.makeText(
            this,
            getString(R.string.shader_compatibility_preset_failed, failure.presetName, reason),
            Toast.LENGTH_LONG,
        ).show()
    }

    private fun resolveMaxShaderLayoutSize(): Pair<Int, Int> {
        var maxWidth = 0
        var maxHeight = 0

        fun include(rect: me.magnum.melonds.domain.model.Rect?, alpha: Float) {
            if (rect == null || alpha <= 0f) return
            maxWidth = maxOf(maxWidth, rect.width)
            maxHeight = maxOf(maxHeight, rect.height)
        }

        runCatching {
            val areas = resolveMainScreenPresentationAreas()
            include(areas.topScreenRect, areas.topAlpha)
            include(areas.bottomScreenRect, areas.bottomAlpha)
            include(areas.hybridTopScreenRect, areas.hybridAlpha)
            include(areas.hybridBottomScreenRect, areas.hybridAlpha)
        }
        return maxWidth to maxHeight
    }

    private fun buildVulkanPresentationConfig(
        topScreenRect: Rect?,
        bottomScreenRect: Rect?,
        topAlpha: Float,
        bottomAlpha: Float,
        topOnTop: Boolean,
        bottomOnTop: Boolean,
        hybridTopScreenRect: Rect?,
        hybridBottomScreenRect: Rect?,
        hybridAlpha: Float,
        hybridOnTop: Boolean,
    ): VulkanPresentationConfig? {
        val rendererConfiguration = currentRuntimeRendererConfiguration ?: return null
        if (rendererConfiguration.renderer != VideoRenderer.VULKAN) {
            return null
        }

        val (surfaceWidth, surfaceHeight) = binding.surfaceMain.getCurrentSurfaceSize()
        val (resolvedTopScreenRect, resolvedBottomScreenRect) = resolveVulkanScreenRects(
            topScreenRect = topScreenRect,
            bottomScreenRect = bottomScreenRect,
            surfaceWidth = if (surfaceWidth > 0) surfaceWidth else binding.surfaceMain.width,
            surfaceHeight = if (surfaceHeight > 0) surfaceHeight else binding.surfaceMain.height,
            fallbackWhenEmpty = hybridTopScreenRect == null && hybridBottomScreenRect == null,
        )

        return VulkanPresentationConfig(
            topScreenRect = resolvedTopScreenRect,
            bottomScreenRect = resolvedBottomScreenRect,
            topAlpha = topAlpha,
            bottomAlpha = bottomAlpha,
            topOnTop = topOnTop,
            bottomOnTop = bottomOnTop,
            hybridTopScreenRect = hybridTopScreenRect?.takeIf { it.width > 0 && it.height > 0 },
            hybridBottomScreenRect = hybridBottomScreenRect?.takeIf { it.width > 0 && it.height > 0 },
            hybridAlpha = hybridAlpha,
            hybridOnTop = hybridOnTop,
            backgroundMode = currentMainScreenBackground.mode,
            videoFiltering = rendererConfiguration.videoFiltering,
            retroShaderEnabled = rendererConfiguration.videoFiltering == VideoFiltering.RETROARCH,
            retroShaderPresetPath = rendererConfiguration.retroArchShader.presetPath,
            retroShaderSourceResolution = rendererConfiguration.retroArchShader.sourceResolution.name.lowercase(),
            retroShaderPassCount = rendererConfiguration.retroArchShader.passCount,
            retroShaderParameterOverrides = rendererConfiguration.retroArchShader.parameterOverrides,
            retroShaderClearHistory = rendererConfiguration.retroArchShader.clearHistory,
        )
    }

    private fun resolveVulkanScreenRects(
        topScreenRect: Rect?,
        bottomScreenRect: Rect?,
        surfaceWidth: Int,
        surfaceHeight: Int,
        fallbackWhenEmpty: Boolean,
    ): Pair<Rect?, Rect?> {
        val sanitizedTopRect = topScreenRect?.takeIf { it.width > 0 && it.height > 0 }
        val sanitizedBottomRect = bottomScreenRect?.takeIf { it.width > 0 && it.height > 0 }

        if (sanitizedTopRect != null || sanitizedBottomRect != null || !fallbackWhenEmpty) {
            return sanitizedTopRect to sanitizedBottomRect
        }

        if (surfaceWidth <= 0 || surfaceHeight <= 0) {
            return null to null
        }

        val topHeight = max(1, surfaceHeight / 2)
        val bottomHeight = max(1, surfaceHeight - topHeight)
        return Rect(0, 0, surfaceWidth, topHeight) to Rect(0, topHeight, surfaceWidth, bottomHeight)
    }

    private fun closeEmulatorActivity() {
        if (isClosingEmulator) {
            return
        }

        isClosingEmulator = true
        releaseEmulatorUiResources()
        val player = PhantomPlayer.current
        if (player == null) finish() else player.notify(PhantomPlayer.CLOSE)
    }

    private fun releaseEmulatorUiResources() {
        cancelStartupPresentationRefreshes()
        offlineSyncChoiceDialog?.dismiss()
        offlineSyncChoiceDialog = null
        offlineSyncProgressDialog?.dismiss()
        offlineSyncProgressDialog = null
        hardcorePendingExitDialog?.dismiss()
        hardcorePendingExitDialog = null
        raPendingSyncProgressDialog?.dismiss()
        raPendingSyncProgressDialog = null
        raPendingSyncResultDialog?.dismiss()
        raPendingSyncResultDialog = null
        showAchievementList.value = false
        showPendingSubmissionsDialog.value = false
        showDualScreenPresets.value = false

        if (::choreographerFrameRenderer.isInitialized) {
            choreographerFrameRenderer.stopRendering()
        }

        presentation?.let {
            runCatching {
                it.dismiss()
            }
        }
        presentation = null

        if (!isFrameRenderCoordinatorStopped && ::frameRenderCoordinator.isInitialized) {
            if (::binding.isInitialized) {
                frameRenderCoordinator.removeSurface(binding.surfaceMain)
            }
            frameRenderCoordinator.stop()
            isFrameRenderCoordinatorStopped = true
        }
    }

    private fun createFrameRenderCoordinator(backend: PresentationBackend): FrameRenderCoordinator {
        return when (backend) {
            PresentationBackend.OPEN_GL -> OpenGlFrameRenderCoordinator()
            PresentationBackend.VULKAN -> VulkanFrameRenderCoordinator(this)
        }
    }

    private fun VideoRenderer.toPresentationBackend(): PresentationBackend {
        return if (this == VideoRenderer.VULKAN) {
            PresentationBackend.VULKAN
        } else {
            PresentationBackend.OPEN_GL
        }
    }

    private fun setupInputHandling(controllerConfiguration: ControllerConfiguration) {
        releaseVirtualButtonInputs()
        nativeInputListener = InputProcessor(controllerConfiguration, melonTouchHandler, frontendInputHandler, slot2AnalogInput)
    }

    private fun handleBackPressed() {
        if (!viewModel.emulatorState.value.isRunning()) {
            viewModel.cancelRomLaunch()
        } else if (isRewindWindowOpen()) {
            closeRewindWindow()
        } else {
            viewModel.pauseEmulator(true)
        }
    }

    private fun showPauseMenu(pauseMenu: PauseMenu) {
        lastPauseMenu = pauseMenu
        requestOverlayHostFocus()
        activeOverlays.addActiveOverlay(EmulatorOverlay.PAUSE_MENU)
        pauseMenuState.value = pauseMenu
    }

    private fun dismissPauseMenu() {
        if (pauseMenuState.value != null) {
            pauseMenuState.value = null
            activeOverlays.removeActiveOverlay(EmulatorOverlay.PAUSE_MENU)
        }
    }

    private fun showRomSettingsMenu(
        rom: Rom,
        renderer: VideoRenderer,
        menuState: InGameRomSettingsMenuState,
    ) {
        val entries = buildList {
            add(romSettingsMenuLabel(getString(R.string.key_mapping), menuState.controllerMappingValue) to {
                romInputSettingsLauncher.launch(InputSetupActivity.getRomCustomIntent(this@EmulatorActivity, rom))
            })
            add(romSettingsMenuLabel(getString(R.string.controller_layout), menuState.layoutValue) to {
                val intent = Intent(this@EmulatorActivity, LayoutSelectorActivity::class.java).apply {
                    putExtra(LayoutSelectorActivity.KEY_SELECTED_LAYOUT_ID, rom.config.layoutId?.toString())
                }
                romLayoutSettingsLauncher.launch(intent)
            })
            add(romSettingsMenuLabel(getString(R.string.filter), menuState.videoFilteringValue) to {
                showRomVideoFilteringDialog(
                    renderer = renderer,
                    selectedFiltering = rom.config.videoFiltering,
                    hasValidRetroArchShaderRoot = menuState.hasValidRetroArchShaderRoot,
                )
            })
            if (menuState.showRetroArchSettings) {
                add(romSettingsMenuLabel(getString(R.string.video_retroarch_shader_preset_title), menuState.retroArchPresetPathValue) to {
                    showRomRetroArchPresetPathDialog(
                        hasValidRetroArchShaderRoot = menuState.hasValidRetroArchShaderRoot,
                        selectedPresetPath = rom.config.retroArchShaderPresetPath,
                    )
                })
                add(romSettingsMenuLabel(getString(R.string.video_retroarch_shader_parameters_title), menuState.retroArchParametersValue) to {
                    showRomRetroArchParametersDialog(
                        hasValidRetroArchShaderRoot = menuState.hasValidRetroArchShaderRoot,
                        selectedParameters = rom.config.retroArchShaderParameters,
                    )
                })
            }
            add(romSettingsMenuLabel(getString(R.string.microphone_source), menuState.micSourceValue) to {
                showRomMicSourceDialog(rom.config.runtimeMicSource)
            })
        }

        if (entries.isEmpty()) {
            reopenPauseMenu()
            return
        }

        val newNode = ConsoleOverlayNode.Submenu(getString(R.string.rom_settings), entries)
        val topNode = consoleOverlayStack.lastOrNull()
        if (topNode is ConsoleOverlayNode.Submenu && topNode.title == newNode.title) {
            consoleOverlayStack[consoleOverlayStack.lastIndex] = newNode
        } else {
            pushConsoleOverlay(newNode)
        }
    }

    private fun refreshRomSettingsMenuIfOpen(): Boolean {
        val topNode = consoleOverlayStack.lastOrNull()
        return if (topNode is ConsoleOverlayNode.Submenu && topNode.title == getString(R.string.rom_settings)) {
            viewModel.onPauseMenuOptionSelected(me.magnum.melonds.ui.emulator.rom.RomPauseMenuOption.ROM_SETTINGS)
            true
        } else {
            false
        }
    }

    private fun onReturnedFromRomSettingsActivity() {
        if (!refreshRomSettingsMenuIfOpen()) {
            viewModel.resumeEmulator()
        } else {
            requestOverlayHostFocus()
        }
    }

    private fun romSettingsMenuLabel(title: String, value: String): String {
        return "$title: $value"
    }

    private fun showRomVideoFilteringDialog(
        renderer: VideoRenderer,
        selectedFiltering: VideoFiltering?,
        hasValidRetroArchShaderRoot: Boolean,
    ) {
        val allFilteringOptions = resources.getStringArray(R.array.video_filtering_options)
        val items = listOf(null) + VideoFiltering.entries.filter { filtering ->
            when (renderer) {
                VideoRenderer.VULKAN -> filtering.isSupportedByVulkan() &&
                    (filtering != VideoFiltering.RETROARCH || hasValidRetroArchShaderRoot)
                else -> filtering.isSupportedByOpenGlSurface()
            }
        }
        val labels = items.map { filtering ->
            filtering?.let { allFilteringOptions[it.ordinal] } ?: getString(R.string.use_global_preference)
        }.toTypedArray()
        val checkedItem = items.indexOf(selectedFiltering).coerceAtLeast(0)

        pushConsoleOverlay(
            ConsoleOverlayNode.Choice(
                title = getString(R.string.filter),
                labels = labels.toList(),
                selectedIndex = checkedItem,
                onSelect = { which ->
                    viewModel.onRunningRomVideoFilteringSelected(items[which])
                    handler.post { refreshRomSettingsMenuIfOpen() }
                },
            ),
        )
    }

    private fun showRomRetroArchPresetPathDialog(
        hasValidRetroArchShaderRoot: Boolean,
        selectedPresetPath: String?,
    ) {
        showRomRetroArchTextDialog(
            titleRes = R.string.video_retroarch_shader_preset_title,
            hasValidRetroArchShaderRoot = hasValidRetroArchShaderRoot,
            initialText = selectedPresetPath,
            onConfirm = viewModel::onRunningRomRetroArchPresetPathSelected,
        )
    }

    private fun showRomRetroArchParametersDialog(
        hasValidRetroArchShaderRoot: Boolean,
        selectedParameters: String?,
    ) {
        showRomRetroArchTextDialog(
            titleRes = R.string.video_retroarch_shader_parameters_title,
            hasValidRetroArchShaderRoot = hasValidRetroArchShaderRoot,
            initialText = selectedParameters,
            onConfirm = viewModel::onRunningRomRetroArchParametersSelected,
        )
    }

    private fun showRomRetroArchTextDialog(
        titleRes: Int,
        hasValidRetroArchShaderRoot: Boolean,
        initialText: String?,
        onConfirm: (String?) -> Unit,
    ) {
        if (!hasValidRetroArchShaderRoot) {
            Toast.makeText(this, R.string.retroarch_shader_root_not_valid, Toast.LENGTH_LONG).show()
            return
        }

        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            setSingleLine(false)
            setText(initialText.orEmpty())
            setSelection(text.length)
        }

        AlertDialog.Builder(this)
            .setTitle(titleRes)
            .setView(input)
            .setPositiveButton(R.string.ok) { _, _ ->
                onConfirm(input.text.toString().ifBlank { null })
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showRomMicSourceDialog(selectedMicSource: RuntimeMicSource) {
        val micOptions = resources.getStringArray(R.array.game_runtime_mic_source_options)
        val items = RuntimeMicSource.entries.toList()
        val labels = items.map { micSource ->
            micOptions[micSource.ordinal]
        }.toTypedArray()
        val checkedItem = items.indexOf(selectedMicSource).coerceAtLeast(0)

        pushConsoleOverlay(
            ConsoleOverlayNode.Choice(
                title = getString(R.string.microphone_source),
                labels = labels.toList(),
                selectedIndex = checkedItem,
                onSelect = { which ->
                    viewModel.onRunningRomMicSourceSelected(items[which])
                    handler.post { refreshRomSettingsMenuIfOpen() }
                },
            ),
        )
    }

    private fun showRendererDebugMenu() {
        showRendererDebugListDialog(
            title = getString(R.string.renderer_debug_menu),
            entries = buildList {
                add(RendererDebugMenuEntry(getString(R.string.renderer_debug_capture)) { viewModel.dumpRendererDebugCapture() })
                if (isDebuggableBuild()) {
                    add(
                        RendererDebugMenuEntry(rendererDebugPauseLabel()) {
                            toggleRendererDebugPauseEmulation()
                            handler.post { showRendererDebugMenu() }
                        },
                    )
                    add(
                        RendererDebugMenuEntry(getString(R.string.renderer_2d_debug_controls)) {
                            syncRendererDebugEmulationMode()
                            handler.post { showRenderer2DDebugControlsDialog() }
                        },
                    )
                    add(
                        RendererDebugMenuEntry(getString(R.string.renderer_3d_debug_controls)) {
                            syncRendererDebugEmulationMode()
                            handler.post { showRenderer3DDebugControlsDialog() }
                        },
                    )
                }
            },
            backAction = null,
        )
    }

    private fun isDebuggableBuild(): Boolean {
        return (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
    }

    private fun showRenderer2DDebugControlsDialog() {
        if (!isDebuggableBuild()) {
            return
        }

        showRendererDebugListDialog(
            title = getString(R.string.renderer_2d_debug_controls),
            entries = buildList {
                add(RendererDebugMenuEntry("Background mode override") { handler.post { showRenderer2DModeOverrideDialog() } })
                add(RendererDebugMenuEntry("Packed compMode override") { handler.post { showRenderer2DCompModeOverrideDialog() } })
                add(RendererDebugMenuEntry("BG layers and priorities") { handler.post { showRenderer2DBgLayerDialog() } })
                add(RendererDebugMenuEntry("Background type enables") { handler.post { showRenderer2DBackgroundTypeDialog() } })
                add(RendererDebugMenuEntry("OBJ / Sprites layers") { handler.post { showRenderer2DObjectDialog() } })
                add(RendererDebugMenuEntry(getString(R.string.renderer_2d_debug_controls_reset)) { resetRenderer2DDebugControlState() })
            },
            backAction = { showRendererDebugMenu() },
        )
    }

    private fun rendererDebugPauseLabel(): String {
        return "Pause Emulation: ${if (rendererDebugPauseEmulation) getString(R.string.on) else getString(R.string.off)}"
    }

    private fun toggleRendererDebugPauseEmulation() {
        if (!isDebuggableBuild()) {
            return
        }

        rendererDebugPauseEmulation = !rendererDebugPauseEmulation
        syncRendererDebugEmulationMode()
    }

    private fun syncRendererDebugEmulationMode() {
        if (rendererDebugPauseEmulation) {
            viewModel.pauseEmulator(false)
        } else {
            viewModel.resumeEmulator()
        }
    }

    private fun onRendererDebugControlApplied() {
        if (!isDebuggableBuild()) {
            return
        }

        if (!rendererDebugPauseEmulation) {
            return
        }

        stepRendererDebugForwardFrame()
    }

    private fun stepRendererDebugForwardFrame() {
        if (!isDebuggableBuild()) {
            return
        }

        if (!rendererDebugPauseEmulation) {
            return
        }

        viewModel.debugStepFrame()
    }

    private fun showRendererDebugListDialog(
        title: String,
        entries: List<RendererDebugMenuEntry>,
        backAction: (() -> Unit)?,
    ) {
        val showRuntimeButtons = isDebuggableBuild() && backAction != null
        activeOverlays.addActiveOverlay(EmulatorOverlay.PAUSE_MENU)
        val dialog = AlertDialog.Builder(this)
            .setTitle(title)
            .setItems(entries.map { it.title }.toTypedArray()) { _, which ->
                entries[which].action()
            }
            .apply {
                if (backAction != null) {
                    setNegativeButton(R.string.navigate_back, null)
                }
                if (showRuntimeButtons) {
                    setNeutralButton(rendererDebugPauseLabel(), null)
                    setPositiveButton("+1 Frame", null)
                }
            }
            .setOnDismissListener {
                activeOverlays.removeActiveOverlay(EmulatorOverlay.PAUSE_MENU)
            }
            .setOnCancelListener {
                syncRendererDebugEmulationMode()
            }
            .create()

        dialog.setOnShowListener {
            if (backAction != null) {
                dialog.getButton(AlertDialog.BUTTON_NEGATIVE)?.setOnClickListener {
                    dialog.dismiss()
                    handler.post(backAction)
                }
            }
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL)?.setOnClickListener {
                toggleRendererDebugPauseEmulation()
                updateRendererDebugRuntimeButtons(dialog)
            }
            dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.setOnClickListener {
                stepRendererDebugForwardFrame()
            }
            updateRendererDebugRuntimeButtons(dialog)
        }
        dialog.show()
    }

    private fun showRendererDebugScrollDialog(
        title: String,
        scrollView: ScrollView,
        backAction: () -> Unit,
    ) {
        if (!isDebuggableBuild()) {
            return
        }

        activeOverlays.addActiveOverlay(EmulatorOverlay.PAUSE_MENU)
        val dialog = AlertDialog.Builder(this)
            .setTitle(title)
            .setView(scrollView)
            .setNegativeButton(R.string.navigate_back, null)
            .apply {
                setNeutralButton(rendererDebugPauseLabel(), null)
                setPositiveButton("+1 Frame", null)
            }
            .setOnDismissListener {
                activeOverlays.removeActiveOverlay(EmulatorOverlay.PAUSE_MENU)
            }
            .setOnCancelListener {
                syncRendererDebugEmulationMode()
            }
            .create()

        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE)?.setOnClickListener {
                dialog.dismiss()
                handler.post(backAction)
            }
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL)?.setOnClickListener {
                toggleRendererDebugPauseEmulation()
                updateRendererDebugRuntimeButtons(dialog)
            }
            dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.setOnClickListener {
                stepRendererDebugForwardFrame()
            }
            updateRendererDebugRuntimeButtons(dialog)
        }
        dialog.show()
    }

    private fun updateRendererDebugRuntimeButtons(dialog: AlertDialog) {
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL)?.text = rendererDebugPauseLabel()
        dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.isEnabled = rendererDebugPauseEmulation
    }

    private fun showRenderer2DModeOverrideDialog() {
        showRendererDebugListDialog(
            title = "Background mode override",
            entries = listOf(
                RendererDebugMenuEntry("Engine A (Main) BG mode") {
                    handler.post { showRenderer2DModeEngineDialog(mainEngine = true) }
                },
                RendererDebugMenuEntry("Engine B (Sub) BG mode") {
                    handler.post { showRenderer2DModeEngineDialog(mainEngine = false) }
                },
            ),
            backAction = { showRenderer2DDebugControlsDialog() },
        )
    }

    private fun showRenderer2DModeEngineDialog(mainEngine: Boolean) {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        val title = if (mainEngine) "Engine A (Main) BG mode" else "Engine B (Sub) BG mode"
        addRenderer2DSection(content, title)
        addRenderer2DDescription(
            content,
            "Forces the Nintendo DS BG mode used by `DrawScanline_BGOBJ`. Native keeps `CurUnit->DispCnt & 0x7`.",
        )
        addRenderer2DModeGroup(
            parent = content,
            title = title,
            selectedMode = if (mainEngine) state.mainForcedMode else state.subForcedMode,
            includeMode6 = mainEngine,
        ) {
            if (mainEngine) {
                state.mainForcedMode = it
            } else {
                state.subForcedMode = it
            }
            applyRenderer2DDebugControlState(state)
        }

        showRendererDebugScrollDialog(title, scrollView) {
            showRenderer2DModeOverrideDialog()
        }
    }

    private fun showRenderer2DCompModeOverrideDialog() {
        showRendererDebugListDialog(
            title = "Packed compMode override",
            entries = listOf(
                RendererDebugMenuEntry("Top screen compMode") {
                    handler.post { showRenderer2DCompModeScreenDialog(topScreen = true) }
                },
                RendererDebugMenuEntry("Bottom screen compMode") {
                    handler.post { showRenderer2DCompModeScreenDialog(topScreen = false) }
                },
            ),
            backAction = { showRenderer2DDebugControlsDialog() },
        )
    }

    private fun showRenderer2DCompModeScreenDialog(topScreen: Boolean) {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        val title = if (topScreen) "Top screen compMode" else "Bottom screen compMode"
        addRenderer2DSection(content, title)
        addRenderer2DDescription(
            content,
            "Forces the compositor mode stored in the packed control plane. Native keeps the value produced by `DrawScanline_BGOBJ`; the override is applied independently to top and bottom snapshots before Vulkan consumes them.",
        )
        addRenderer2DCompModeGroup(
            parent = content,
            title = title,
            selectedMode = if (topScreen) state.topForcedCompMode else state.bottomForcedCompMode,
        ) {
            if (topScreen) {
                state.topForcedCompMode = it
            } else {
                state.bottomForcedCompMode = it
            }
            applyRenderer2DDebugControlState(state)
        }

        showRendererDebugScrollDialog(title, scrollView) {
            showRenderer2DCompModeOverrideDialog()
        }
    }

    private fun showRenderer2DBgLayerDialog() {
        showRendererDebugListDialog(
            title = "BG layers and priorities",
            entries = listOf(
                RendererDebugMenuEntry("Engine A (Main) BG layers") {
                    handler.post { showRenderer2DBgLayerEngineDialog(mainEngine = true) }
                },
                RendererDebugMenuEntry("Engine B (Sub) BG layers") {
                    handler.post { showRenderer2DBgLayerEngineDialog(mainEngine = false) }
                },
                RendererDebugMenuEntry("Engine A (Main) BG priorities") {
                    handler.post { showRenderer2DBgPriorityEngineDialog(mainEngine = true) }
                },
                RendererDebugMenuEntry("Engine B (Sub) BG priorities") {
                    handler.post { showRenderer2DBgPriorityEngineDialog(mainEngine = false) }
                },
            ),
            backAction = { showRenderer2DDebugControlsDialog() },
        )
    }

    private fun showRenderer2DBgLayerEngineDialog(mainEngine: Boolean) {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        val title = if (mainEngine) "Engine A (Main) BG layers" else "Engine B (Sub) BG layers"
        addRenderer2DSection(content, title)
        addRenderer2DDescription(
            content,
            "Disables individual BG0-BG3 draw gates before `DrawBG_*` or `DrawBG_3D`; this is independent from the game's DISPCNT enable bits.",
        )
        addRenderer2DBgLayerSwitches(
            parent = content,
            title = title,
            disabledMask = { if (mainEngine) state.disabledMainBgMask else state.disabledSubBgMask },
            updateDisabledMask = {
                if (mainEngine) {
                    state.disabledMainBgMask = it
                } else {
                    state.disabledSubBgMask = it
                }
                applyRenderer2DDebugControlState(state)
            },
        )

        showRendererDebugScrollDialog(title, scrollView) {
            showRenderer2DBgLayerDialog()
        }
    }

    private fun showRenderer2DBgPriorityEngineDialog(mainEngine: Boolean) {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        val title = if (mainEngine) "Engine A (Main) BG priorities" else "Engine B (Sub) BG priorities"
        addRenderer2DSection(content, title)
        addRenderer2DDescription(
            content,
            "Disables BG layers by Nintendo DS BGCNT priority bits 0-1. Priority 0 is closest to the viewer; priority 3 is furthest back.",
        )
        addRenderer2DPrioritySwitches(
            parent = content,
            title = title,
            disabledMask = { if (mainEngine) state.disabledMainBgPriorityMask else state.disabledSubBgPriorityMask },
            updateDisabledMask = {
                if (mainEngine) {
                    state.disabledMainBgPriorityMask = it
                } else {
                    state.disabledSubBgPriorityMask = it
                }
                applyRenderer2DDebugControlState(state)
            },
            descriptionPrefix = "BGCNT priority",
            codeDescription = "Code gate: `bgCnt[n] & 0x3` inside `DrawScanlineBGMode`.",
        )

        showRendererDebugScrollDialog(title, scrollView) {
            showRenderer2DBgLayerDialog()
        }
    }

    private fun showRenderer2DBackgroundTypeDialog() {
        showRendererDebugListDialog(
            title = "Background type enables",
            entries = listOf(
                RendererDebugMenuEntry("Tile background types") {
                    handler.post { showRenderer2DBackgroundTileTypesDialog() }
                },
                RendererDebugMenuEntry("Bitmap background types") {
                    handler.post { showRenderer2DBackgroundBitmapTypesDialog() }
                },
                RendererDebugMenuEntry("Special background types") {
                    handler.post { showRenderer2DBackgroundSpecialTypesDialog() }
                },
            ),
            backAction = { showRenderer2DDebugControlsDialog() },
        )
    }

    private fun showRenderer2DBackgroundTileTypesDialog() {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "Tile background types")
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_STATIC_BACKGROUND,
            title = "Static background",
            description = "Nintendo DS static BG. Code: `DrawBG_Text`; used by BG0/BG1 and by BG2/BG3 when the active mode selects text/static layers.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_AFFINE_BACKGROUND,
            title = "Affine background",
            description = "Nintendo DS affine BG. Code: `DrawBG_Affine`; used for BG2/BG3 in modes that select affine transform layers.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_AFFINE_EXTENDED_TILED_BACKGROUND,
            title = "Affine Extended background - tiled",
            description = "Nintendo DS affine extended tiled BG. Code: `DrawBG_Extended` with BGCNT bitmap bit clear; keeps the tile path with H/V flip support.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )

        showRendererDebugScrollDialog("Tile background types", scrollView) {
            showRenderer2DBackgroundTypeDialog()
        }
    }

    private fun showRenderer2DBackgroundBitmapTypesDialog() {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "Bitmap background types")
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_AFFINE_EXTENDED_BITMAP_256_BACKGROUND,
            title = "Affine Extended background - 256 colors bitmap",
            description = "Nintendo DS affine extended 256-color bitmap BG. Code: `DrawBG_Extended` bitmap path without direct-color bit; VRAM is treated as a paletted framebuffer.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_AFFINE_EXTENDED_DIRECT_COLOR_BACKGROUND,
            title = "Affine Extended background - direct color bitmap",
            description = "Nintendo DS affine extended direct-color bitmap BG. Code: `DrawBG_Extended` bitmap path with BGCNT direct-color bit; VRAM is treated as 15-bit direct color.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_LARGE_SCREEN_BACKGROUND,
            title = "Large screen background",
            description = "Nintendo DS large screen BG. Code: `DrawBG_Large`; mode 6 BG2 large framebuffer path, available only on Engine A (Main).",
            applyState = { applyRenderer2DDebugControlState(state) },
        )

        showRendererDebugScrollDialog("Bitmap background types", scrollView) {
            showRenderer2DBackgroundTypeDialog()
        }
    }

    private fun showRenderer2DBackgroundSpecialTypesDialog() {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "Special background types")
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_3D_BACKGROUND,
            title = "3D background",
            description = "Nintendo DS 3D background layer. Code: `DrawBG_3D`; Engine A BG0 placeholder/output used to composite GPU3D with the 2D BG/OBJ stack.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )

        showRendererDebugScrollDialog("Special background types", scrollView) {
            showRenderer2DBackgroundTypeDialog()
        }
    }

    private fun showRenderer2DObjectDialog() {
        showRendererDebugListDialog(
            title = "OBJ / Sprites",
            entries = listOf(
                RendererDebugMenuEntry("OBJ master") { handler.post { showRenderer2DObjectMasterDialog() } },
                RendererDebugMenuEntry("OBJ priority enables") { handler.post { showRenderer2DObjectPriorityDialog() } },
                RendererDebugMenuEntry("OBJ OAM order / Z buckets") { handler.post { showRenderer2DObjectOrderDialog() } },
                RendererDebugMenuEntry("OBJ vertical bands") { handler.post { showRenderer2DObjectBandDialog() } },
                RendererDebugMenuEntry("OBJ transform and storage type") { handler.post { showRenderer2DObjectTypeDialog() } },
                RendererDebugMenuEntry("OBJ effects and masks") { handler.post { showRenderer2DObjectEffectsDialog() } },
            ),
            backAction = { showRenderer2DDebugControlsDialog() },
        )
    }

    private fun showRenderer2DObjectMasterDialog() {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "OBJ master")
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_OBJECTS,
            title = "OBJ (Objects / Sprites)",
            description = "Nintendo DS sprites from OAM. Code: `DrawSprites`, `DrawSprite_Normal`, `DrawSprite_Rotscale` and `InterleaveSprites`; covers tiled and bitmap OBJ pixels.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )

        showRendererDebugScrollDialog("OBJ master", scrollView) {
            showRenderer2DObjectDialog()
        }
    }

    private fun showRenderer2DObjectPriorityDialog() {
        showRendererDebugListDialog(
            title = "OBJ priority enables",
            entries = listOf(
                RendererDebugMenuEntry("Engine A (Main) OBJ priorities") {
                    handler.post { showRenderer2DObjectPriorityEngineDialog(mainEngine = true) }
                },
                RendererDebugMenuEntry("Engine B (Sub) OBJ priorities") {
                    handler.post { showRenderer2DObjectPriorityEngineDialog(mainEngine = false) }
                },
            ),
            backAction = { showRenderer2DObjectDialog() },
        )
    }

    private fun showRenderer2DObjectPriorityEngineDialog(mainEngine: Boolean) {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        val title = if (mainEngine) "Engine A (Main) OBJ priorities" else "Engine B (Sub) OBJ priorities"
        addRenderer2DSection(content, "OBJ priority enables")
        addRenderer2DDescription(
            content,
            "Disables sprite layers by OAM Attribute 2 priority bits 10-11. Priority 0 is closest to the viewer; priority 3 is furthest back.",
        )
        addRenderer2DPrioritySwitches(
            parent = content,
            title = title,
            disabledMask = { if (mainEngine) state.disabledMainObjPriorityMask else state.disabledSubObjPriorityMask },
            updateDisabledMask = {
                if (mainEngine) {
                    state.disabledMainObjPriorityMask = it
                } else {
                    state.disabledSubObjPriorityMask = it
                }
                applyRenderer2DDebugControlState(state)
            },
            descriptionPrefix = "OBJ priority",
            codeDescription = "Code gate: `attrib[2] & 0x0C00`, then `InterleaveSprites(0x40000 | priority << 16)`.",
        )

        showRendererDebugScrollDialog(title, scrollView) {
            showRenderer2DObjectPriorityDialog()
        }
    }

    private fun showRenderer2DObjectOrderDialog() {
        showRendererDebugListDialog(
            title = "OBJ OAM order / Z buckets",
            entries = listOf(
                RendererDebugMenuEntry("Engine A (Main) OBJ OAM order") {
                    handler.post { showRenderer2DObjectOrderEngineDialog(mainEngine = true) }
                },
                RendererDebugMenuEntry("Engine B (Sub) OBJ OAM order") {
                    handler.post { showRenderer2DObjectOrderEngineDialog(mainEngine = false) }
                },
            ),
            backAction = { showRenderer2DObjectDialog() },
        )
    }

    private fun showRenderer2DObjectOrderEngineDialog(mainEngine: Boolean) {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        val title = if (mainEngine) "Engine A (Main) OBJ OAM order" else "Engine B (Sub) OBJ OAM order"
        addRenderer2DSection(content, "OBJ OAM order / Z buckets")
        addRenderer2DDescription(
            content,
            "Filters sprites by OAM index order. For equal OBJ priority, lower OAM indices are drawn later by `DrawSprites` and appear closer to the viewer; this gives practical Z-position control for composite sprites.",
        )
        addRenderer2DObjectOrderSwitches(
            parent = content,
            title = title,
            disabledMask = { if (mainEngine) state.disabledMainObjOrderMask else state.disabledSubObjOrderMask },
            updateDisabledMask = {
                if (mainEngine) {
                    state.disabledMainObjOrderMask = it
                } else {
                    state.disabledSubObjOrderMask = it
                }
                applyRenderer2DDebugControlState(state)
            },
        )

        showRendererDebugScrollDialog(title, scrollView) {
            showRenderer2DObjectOrderDialog()
        }
    }

    private fun showRenderer2DObjectBandDialog() {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "OBJ vertical bands")
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_OBJECT_UPPER_BAND,
            title = "OBJ upper band - Y 0..63",
            description = "Nintendo DS OBJ pixels and OBJ Window mask for the upper third of the current LCD. Code gate: `DrawSprites(line)` returns after clearing `OBJLine`/`OBJWindow` when `line < 64`.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_OBJECT_MIDDLE_BAND,
            title = "OBJ middle band - Y 64..127",
            description = "Nintendo DS OBJ pixels and OBJ Window mask for the middle third of the current LCD. Code gate: `DrawSprites(line)` line range 64..127.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_OBJECT_LOWER_BAND,
            title = "OBJ lower band - Y 128..191",
            description = "Nintendo DS OBJ pixels and OBJ Window mask for the lower third of the current LCD. Code gate: `DrawSprites(line)` line range 128..191.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )

        showRendererDebugScrollDialog("OBJ vertical bands", scrollView) {
            showRenderer2DObjectDialog()
        }
    }

    private fun showRenderer2DObjectTypeDialog() {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "OBJ transform and storage type")
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_REGULAR_OBJECT,
            title = "OBJ regular transform",
            description = "Nintendo DS non-affine OBJ. Code: `DrawSprite_Normal`; OAM Attribute 0 affine flag bit 8 is clear.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_AFFINE_OBJECT,
            title = "OBJ affine / rotscale transform",
            description = "Nintendo DS affine OBJ. Code: `DrawSprite_Rotscale`; OAM Attribute 0 affine flag bit 8 is set.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_TILED_4BPP_OBJECT,
            title = "OBJ tiled - 16 colors",
            description = "Nintendo DS tiled OBJ using 4bpp/16-color data. Code path in `DrawSprite_*` when Attribute 0 color mode bit 13 is clear.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_TILED_8BPP_OBJECT,
            title = "OBJ tiled - 256 colors",
            description = "Nintendo DS tiled OBJ using 8bpp/256-color data. Code path in `DrawSprite_*` when Attribute 0 color mode bit 13 is set.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_BITMAP_OBJECT,
            title = "OBJ bitmap",
            description = "Nintendo DS bitmap OBJ. Code path in `DrawSprite_*` when OAM Attribute 0 object mode bits 10-11 equal 3.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )

        showRendererDebugScrollDialog("OBJ transform and storage type", scrollView) {
            showRenderer2DObjectDialog()
        }
    }

    private fun showRenderer2DObjectEffectsDialog() {
        val state = readRenderer2DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "OBJ effects and masks")
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_BLENDED_OBJECT,
            title = "OBJ semi-transparent",
            description = "Nintendo DS semi-transparent OBJ. Code path in `DrawSprite_*` when OAM Attribute 0 object mode bits 10-11 equal 1.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_WINDOW_OBJECT,
            title = "OBJ Window",
            description = "Nintendo DS OBJ Window mask. Code: `DrawSprite_*<true>` fills `OBJWindow`; affects window clipping rather than visible color directly.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )
        addRenderer2DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_MOSAIC_OBJECT,
            title = "OBJ mosaic",
            description = "Nintendo DS OBJ using mosaic. Code: Attribute 0 mosaic bit 12 and `ApplySpriteMosaicX`.",
            applyState = { applyRenderer2DDebugControlState(state) },
        )

        showRendererDebugScrollDialog("OBJ effects and masks", scrollView) {
            showRenderer2DObjectDialog()
        }
    }

    private fun showRenderer3DDebugControlsDialog() {
        if (!isDebuggableBuild()) {
            return
        }

        showRendererDebugListDialog(
            title = getString(R.string.renderer_3d_debug_controls),
            entries = buildList {
                add(RendererDebugMenuEntry("Renderer output and primitive buckets") { handler.post { showRenderer3DPrimitiveDialog() } })
                add(RendererDebugMenuEntry("Polygon material and effects") { handler.post { showRenderer3DMaterialDialog() } })
                add(RendererDebugMenuEntry("Depth, fog and screen bands") { handler.post { showRenderer3DDepthAndBandDialog() } })
                add(RendererDebugMenuEntry(getString(R.string.renderer_3d_debug_controls_reset)) { resetRenderer3DDebugControlState() })
            },
            backAction = { showRendererDebugMenu() },
        )
    }

    private fun showRenderer3DPrimitiveDialog() {
        showRendererDebugListDialog(
            title = "Renderer output and primitives",
            entries = listOf(
                RendererDebugMenuEntry("3D renderer output") { handler.post { showRenderer3DOutputDialog() } },
                RendererDebugMenuEntry("Primitive buckets") { handler.post { showRenderer3DPrimitiveBucketDialog() } },
                RendererDebugMenuEntry("Blend buckets") { handler.post { showRenderer3DBlendBucketDialog() } },
            ),
            backAction = { showRenderer3DDebugControlsDialog() },
        )
    }

    private fun showRenderer3DOutputDialog() {
        val state = readRenderer3DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "3D renderer output")
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_RENDERER_OUTPUT,
            title = "3D renderer output",
            description = "Master GPU3D output gate. Code: Vulkan `buildGraphicsTriangleList` / `buildTriangleList`; disables all 3D polygons before raster queues are populated.",
        )

        showRendererDebugScrollDialog("3D renderer output", scrollView) {
            showRenderer3DPrimitiveDialog()
        }
    }

    private fun showRenderer3DPrimitiveBucketDialog() {
        val state = readRenderer3DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "Primitive buckets")
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_TRIANGLE_POLYGONS,
            title = "Triangle polygons",
            description = "Nintendo DS 3D polygon primitives. Code gate: `AcceleratedPrimitiveType::Triangles` or `polygon->Type != 1`.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_LINE_POLYGONS,
            title = "Line polygons",
            description = "Nintendo DS 3D line primitives expanded into quads for Vulkan. Code gate: `AcceleratedPrimitiveType::Lines` or `polygon->Type == 1`.",
        )

        showRendererDebugScrollDialog("Primitive buckets", scrollView) {
            showRenderer3DPrimitiveDialog()
        }
    }

    private fun showRenderer3DBlendBucketDialog() {
        val state = readRenderer3DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "Blend buckets")
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_OPAQUE_POLYGONS,
            title = "Opaque polygons",
            description = "Opaque GPU3D polygons. Code bucket: `GraphicsOpaqueDrawIndices`; alpha is 31 and the accelerated translucent flag is clear.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_TRANSLUCENT_POLYGONS,
            title = "Translucent polygons",
            description = "Translucent GPU3D polygons. Code bucket: `GraphicsAlphaDrawIndices`; includes accelerated translucent pass or polygon alpha below 31.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_SHADOW_MASK_POLYGONS,
            title = "Shadow mask polygons",
            description = "Nintendo DS shadow mask polygons. Code bucket: `GraphicsShadowMaskDrawIndices` and `AcceleratedPolygonFlagShadowMask`.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_SHADOW_POLYGONS,
            title = "Shadow polygons",
            description = "Nintendo DS shadow blend polygons. Code bucket: `GraphicsShadowDrawIndices` and `AcceleratedPolygonFlagShadow`.",
        )

        showRendererDebugScrollDialog("Blend buckets", scrollView) {
            showRenderer3DPrimitiveDialog()
        }
    }

    private fun showRenderer3DMaterialDialog() {
        showRendererDebugListDialog(
            title = "Polygon material and effects",
            entries = listOf(
                RendererDebugMenuEntry("Texture state") { handler.post { showRenderer3DTextureStateDialog() } },
                RendererDebugMenuEntry("Polygon mode") { handler.post { showRenderer3DPolygonModeDialog() } },
            ),
            backAction = { showRenderer3DDebugControlsDialog() },
        )
    }

    private fun showRenderer3DTextureStateDialog() {
        val state = readRenderer3DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "Texture state")
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_TEXTURED_POLYGONS,
            title = "Textured polygons",
            description = "GPU3D polygons with texture mapping enabled and non-zero texture format. Code gate: `RenderDispCnt` texture bit plus `TexParam >> 26`.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_UNTEXTURED_POLYGONS,
            title = "Untextured polygons",
            description = "GPU3D polygons without active texture sampling. Code path uses fallback/untextured material data in Vulkan raster shaders.",
        )

        showRendererDebugScrollDialog("Texture state", scrollView) {
            showRenderer3DMaterialDialog()
        }
    }

    private fun showRenderer3DPolygonModeDialog() {
        val state = readRenderer3DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "Polygon mode")
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_MODULATE_POLYGONS,
            title = "Modulation polygons",
            description = "Nintendo DS polygon mode 0 or untextured fallback. Code gate: `PolyAttr` blend mode not decal/toon-highlight.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_DECAL_POLYGONS,
            title = "Decal polygons",
            description = "Nintendo DS decal-style textured polygons. Code gate: textured polygon with `PolyAttr` blend mode bit 0 set.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_TOON_HIGHLIGHT_POLYGONS,
            title = "Toon / highlight polygons",
            description = "Nintendo DS toon/highlight polygon mode. Code gate: `PolyAttr` blend mode 2; Vulkan chooses toon or highlight from `RenderDispCnt`.",
        )

        showRendererDebugScrollDialog("Polygon mode", scrollView) {
            showRenderer3DMaterialDialog()
        }
    }

    private fun showRenderer3DDepthAndBandDialog() {
        showRendererDebugListDialog(
            title = "Depth, fog and screen bands",
            entries = listOf(
                RendererDebugMenuEntry("Depth and fog mode") { handler.post { showRenderer3DDepthModeDialog() } },
                RendererDebugMenuEntry("Screen bands") { handler.post { showRenderer3DScreenBandDialog() } },
            ),
            backAction = { showRenderer3DDebugControlsDialog() },
        )
    }

    private fun showRenderer3DDepthModeDialog() {
        val state = readRenderer3DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "Depth mode")
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_W_BUFFER_POLYGONS,
            title = "W-buffer polygons",
            description = "Nintendo DS W-buffer polygons. Code gate: `AcceleratedPolygonFlagWBuffer`; Vulkan uses perspective W depth interpolation.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_Z_BUFFER_POLYGONS,
            title = "Z-buffer polygons",
            description = "Nintendo DS Z-buffer polygons. Code gate: absence of `AcceleratedPolygonFlagWBuffer`; Vulkan uses screen-space linear Z depth.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_DEPTH_WRITE_POLYGONS,
            title = "Depth write polygons",
            description = "GPU3D polygons that update depth. Code gate: `PolyAttr` bit 11; disabling this removes depth-writing polygons from the draw queues.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_FOG_WRITE_POLYGONS,
            title = "Fog write polygons",
            description = "GPU3D polygons that write fog attributes. Code gate: `AcceleratedPolygonFlagFogWrite`; final fog pass consumes the attribute target.",
        )

        showRendererDebugScrollDialog("Depth and fog mode", scrollView) {
            showRenderer3DDepthAndBandDialog()
        }
    }

    private fun showRenderer3DScreenBandDialog() {
        val state = readRenderer3DDebugControlState()
        val (content, scrollView) = createRenderer2DScrollContent()

        addRenderer2DSection(content, "Screen bands")
        addRenderer2DDescription(
            content,
            "Filters whole 3D polygons by their Y range in the active render target. This is a coarse isolation tool: polygons spanning an enabled band remain whole.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_UPPER_BAND,
            title = "3D upper band - Y 0..63",
            description = "GPU3D polygons touching the upper third of the LCD. Code gate uses packed polygon Y bounds scaled to the current internal resolution.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_MIDDLE_BAND,
            title = "3D middle band - Y 64..127",
            description = "GPU3D polygons touching the middle third of the LCD. Code gate uses packed polygon Y bounds scaled to the current internal resolution.",
        )
        addRenderer3DFeatureSwitch(
            parent = content,
            state = state,
            flag = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_LOWER_BAND,
            title = "3D lower band - Y 128..191",
            description = "GPU3D polygons touching the lower third of the LCD. Code gate uses packed polygon Y bounds scaled to the current internal resolution.",
        )

        showRendererDebugScrollDialog("Screen bands", scrollView) {
            showRenderer3DDepthAndBandDialog()
        }
    }

    private fun createRenderer2DScrollContent(): Pair<LinearLayout, ScrollView> {
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(12), dp(24), dp(8))
        }
        val scrollView = ScrollView(this).apply {
            addView(
                content,
                ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ),
            )
        }
        return content to scrollView
    }

    private fun applyRenderer2DDebugControlState(state: Renderer2DDebugControlState) {
        if (!isDebuggableBuild()) {
            return
        }

        RendererDebugBridge.setRenderer2DDebugControls(
            state.mainForcedMode,
            state.subForcedMode,
            state.topForcedCompMode,
            state.bottomForcedCompMode,
            state.disabledMainBgMask,
            state.disabledSubBgMask,
            state.disabledMainBgPriorityMask,
            state.disabledSubBgPriorityMask,
            state.disabledMainObjPriorityMask,
            state.disabledSubObjPriorityMask,
            state.disabledMainObjOrderMask,
            state.disabledSubObjOrderMask,
            state.featureMask,
        )
        onRendererDebugControlApplied()
    }

    private fun resetRenderer2DDebugControlState() {
        applyRenderer2DDebugControlState(Renderer2DDebugControlState())
    }

    private fun applyRenderer3DDebugControlState(state: Renderer3DDebugControlState) {
        if (!isDebuggableBuild()) {
            return
        }

        RendererDebugBridge.setRenderer3DDebugControls(state.featureMask)
        onRendererDebugControlApplied()
    }

    private fun resetRenderer3DDebugControlState() {
        applyRenderer3DDebugControlState(Renderer3DDebugControlState())
    }

    private fun readRenderer2DDebugControlState(): Renderer2DDebugControlState {
        if (!isDebuggableBuild()) {
            return Renderer2DDebugControlState()
        }

        val state = RendererDebugBridge.getRenderer2DDebugControls()
        return if (state != null && state.size >= RENDERER_2D_STATE_SIZE) {
            Renderer2DDebugControlState(
                mainForcedMode = state[0],
                subForcedMode = state[1],
                topForcedCompMode = state[2],
                bottomForcedCompMode = state[3],
                disabledMainBgMask = state[4],
                disabledSubBgMask = state[5],
                disabledMainBgPriorityMask = state[6],
                disabledSubBgPriorityMask = state[7],
                disabledMainObjPriorityMask = state[8],
                disabledSubObjPriorityMask = state[9],
                disabledMainObjOrderMask = state[10],
                disabledSubObjOrderMask = state[11],
                featureMask = state[12],
            )
        } else {
            Renderer2DDebugControlState()
        }
    }

    private fun readRenderer3DDebugControlState(): Renderer3DDebugControlState {
        if (!isDebuggableBuild()) {
            return Renderer3DDebugControlState()
        }

        val state = RendererDebugBridge.getRenderer3DDebugControls()
        return if (state != null && state.size >= RENDERER_3D_STATE_SIZE) {
            Renderer3DDebugControlState(featureMask = state[0])
        } else {
            Renderer3DDebugControlState()
        }
    }

    private fun addRenderer2DModeGroup(
        parent: LinearLayout,
        title: String,
        selectedMode: Int,
        includeMode6: Boolean,
        onModeChanged: (Int) -> Unit,
    ) {
        addRenderer2DSubsection(parent, title)
        val modes = renderer2DDebugModeItems(includeMode6)
        val radioIds = mutableMapOf<Int, Int>()
        val radioGroup = RadioGroup(this).apply {
            orientation = RadioGroup.VERTICAL
        }
        modes.forEach { modeItem ->
            val radioButton = RadioButton(this).apply {
                id = View.generateViewId()
                text = modeItem.label
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
                setSingleLine(false)
                ellipsize = null
                maxLines = 4
            }
            radioIds[radioButton.id] = modeItem.mode
            radioGroup.addView(
                radioButton,
                RadioGroup.LayoutParams(
                    RadioGroup.LayoutParams.MATCH_PARENT,
                    RadioGroup.LayoutParams.WRAP_CONTENT,
                ),
            )
        }
        radioGroup.check(
            radioIds.entries.firstOrNull { it.value == selectedMode }?.key
                ?: radioIds.entries.first { it.value == RENDERER_2D_NATIVE_MODE }.key,
        )
        radioGroup.setOnCheckedChangeListener { _, checkedId ->
            radioIds[checkedId]?.let(onModeChanged)
        }
        parent.addView(radioGroup)
    }

    private fun addRenderer2DCompModeGroup(
        parent: LinearLayout,
        title: String,
        selectedMode: Int,
        onModeChanged: (Int) -> Unit,
    ) {
        addRenderer2DSubsection(parent, title)
        val modes = renderer2DDebugCompModeItems
        val radioIds = mutableMapOf<Int, Int>()
        val radioGroup = RadioGroup(this).apply {
            orientation = RadioGroup.VERTICAL
        }
        modes.forEach { modeItem ->
            val radioButton = RadioButton(this).apply {
                id = View.generateViewId()
                text = modeItem.label
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
                setSingleLine(false)
                ellipsize = null
                maxLines = 4
            }
            radioIds[radioButton.id] = modeItem.mode
            radioGroup.addView(
                radioButton,
                RadioGroup.LayoutParams(
                    RadioGroup.LayoutParams.MATCH_PARENT,
                    RadioGroup.LayoutParams.WRAP_CONTENT,
                ),
            )
        }
        radioGroup.check(
            radioIds.entries.firstOrNull { it.value == selectedMode }?.key
                ?: radioIds.entries.first { it.value == RENDERER_2D_NATIVE_MODE }.key,
        )
        radioGroup.setOnCheckedChangeListener { _, checkedId ->
            radioIds[checkedId]?.let(onModeChanged)
        }
        parent.addView(radioGroup)
    }

    private fun addRenderer2DBgLayerSwitches(
        parent: LinearLayout,
        title: String,
        disabledMask: () -> Int,
        updateDisabledMask: (Int) -> Unit,
    ) {
        addRenderer2DSubsection(parent, title)
        renderer2DDebugBgLayerItems.forEach { item ->
            addRenderer2DSwitch(
                parent = parent,
                title = item.title,
                description = item.description,
                checked = (disabledMask() and (1 shl item.bgIndex)) == 0,
            ) { checked ->
                val bit = 1 shl item.bgIndex
                val nextMask = if (checked) {
                    disabledMask() and bit.inv()
                } else {
                    disabledMask() or bit
                }
                updateDisabledMask(nextMask)
            }
        }
    }

    private fun addRenderer2DPrioritySwitches(
        parent: LinearLayout,
        title: String,
        disabledMask: () -> Int,
        updateDisabledMask: (Int) -> Unit,
        descriptionPrefix: String,
        codeDescription: String,
    ) {
        addRenderer2DSubsection(parent, title)
        renderer2DDebugPriorityItems.forEach { item ->
            addRenderer2DSwitch(
                parent = parent,
                title = item.title,
                description = "$descriptionPrefix ${item.priority}. ${item.description} $codeDescription",
                checked = (disabledMask() and (1 shl item.priority)) == 0,
            ) { checked ->
                val bit = 1 shl item.priority
                val nextMask = if (checked) {
                    disabledMask() and bit.inv()
                } else {
                    disabledMask() or bit
                }
                updateDisabledMask(nextMask)
            }
        }
    }

    private fun addRenderer2DObjectOrderSwitches(
        parent: LinearLayout,
        title: String,
        disabledMask: () -> Int,
        updateDisabledMask: (Int) -> Unit,
    ) {
        addRenderer2DSubsection(parent, title)
        renderer2DDebugObjectOrderItems.forEach { item ->
            addRenderer2DSwitch(
                parent = parent,
                title = item.title,
                description = item.description,
                checked = (disabledMask() and (1 shl item.bucket)) == 0,
            ) { checked ->
                val bit = 1 shl item.bucket
                val nextMask = if (checked) {
                    disabledMask() and bit.inv()
                } else {
                    disabledMask() or bit
                }
                updateDisabledMask(nextMask)
            }
        }
    }

    private fun addRenderer2DFeatureSwitch(
        parent: LinearLayout,
        state: Renderer2DDebugControlState,
        flag: Int,
        title: String,
        description: String,
        applyState: () -> Unit,
    ) {
        addRenderer2DSwitch(
            parent = parent,
            title = title,
            description = description,
            checked = (state.featureMask and flag) != 0,
        ) { checked ->
            state.featureMask = if (checked) {
                state.featureMask or flag
            } else {
                state.featureMask and flag.inv()
            }
            applyState()
        }
    }

    private fun addRenderer3DFeatureSwitch(
        parent: LinearLayout,
        state: Renderer3DDebugControlState,
        flag: Int,
        title: String,
        description: String,
    ) {
        addRenderer2DSwitch(
            parent = parent,
            title = title,
            description = description,
            checked = (state.featureMask and flag) != 0,
        ) { checked ->
            state.featureMask = if (checked) {
                state.featureMask or flag
            } else {
                state.featureMask and flag.inv()
            }
            applyRenderer3DDebugControlState(state)
        }
    }

    private fun addRenderer2DSection(parent: LinearLayout, title: String) {
        parent.addView(
            TextView(this).apply {
                text = title
                setTypeface(typeface, Typeface.BOLD)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
                setPadding(0, dp(12), 0, dp(4))
            },
        )
    }

    private fun addRenderer2DSubsection(parent: LinearLayout, title: String) {
        parent.addView(
            TextView(this).apply {
                text = title
                setTypeface(typeface, Typeface.BOLD)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
                setPadding(0, dp(8), 0, dp(2))
            },
        )
    }

    private fun addRenderer2DDescription(parent: LinearLayout, description: String) {
        parent.addView(
            TextView(this).apply {
                text = description
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                setPadding(0, 0, 0, dp(4))
            },
        )
    }

    private fun addRenderer2DSwitch(
        parent: LinearLayout,
        title: String,
        description: String,
        checked: Boolean,
        onCheckedChanged: (Boolean) -> Unit,
    ) {
        val item = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(6), 0, dp(6))
        }
        val switch = SwitchCompat(this).apply {
            text = title
            isChecked = checked
            gravity = Gravity.CENTER_VERTICAL
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            setSingleLine(false)
            ellipsize = null
            setOnCheckedChangeListener { _, isChecked ->
                onCheckedChanged(isChecked)
            }
        }
        item.addView(
            switch,
            LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ),
        )
        item.addView(
            TextView(this).apply {
                text = description
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                setPadding(dp(4), 0, 0, 0)
            },
        )
        parent.addView(item)
    }

    private fun dp(value: Int): Int {
        return (value * resources.displayMetrics.density).toInt()
    }

    private fun disableScreenTimeOut() {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun enableScreenTimeOut() {
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (!activeOverlays.hasActiveOverlays() && nativeInputListener.onKeyEvent(event))
            return true

        if (hasComposeOverlayOpen() && event.action == KeyEvent.ACTION_DOWN) {
            val direction = when (event.keyCode) {
                KeyEvent.KEYCODE_DPAD_UP -> androidx.compose.ui.focus.FocusDirection.Up
                KeyEvent.KEYCODE_DPAD_DOWN -> androidx.compose.ui.focus.FocusDirection.Down
                KeyEvent.KEYCODE_DPAD_LEFT -> androidx.compose.ui.focus.FocusDirection.Left
                KeyEvent.KEYCODE_DPAD_RIGHT -> androidx.compose.ui.focus.FocusDirection.Right
                else -> null
            }
            if (direction != null) {
                if (!moveOverlayFocus(direction)) {
                    binding.layoutPauseMenu.dispatchKeyEvent(event)
                }
                return true
            }
        }

        return super.dispatchKeyEvent(event)
    }

    private fun moveOverlayFocus(direction: androidx.compose.ui.focus.FocusDirection): Boolean {
        val focusManager = overlayFocusManager
        if (!binding.layoutPauseMenu.hasFocus()) {
            binding.layoutPauseMenu.requestFocus()
        }

        var moved = focusManager?.moveFocus(direction) ?: false
        if (!moved) {
            moved = focusManager?.moveFocus(androidx.compose.ui.focus.FocusDirection.Enter) ?: false
        }
        return moved
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (activeOverlays.hasActiveOverlays()) {
            if (hasComposeOverlayOpen() && event.action == MotionEvent.ACTION_MOVE) {
                val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
                val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
                val direction = when {
                    hatX > 0.5f && lastOverlayHatX <= 0.5f -> androidx.compose.ui.focus.FocusDirection.Right
                    hatX < -0.5f && lastOverlayHatX >= -0.5f -> androidx.compose.ui.focus.FocusDirection.Left
                    hatY > 0.5f && lastOverlayHatY <= 0.5f -> androidx.compose.ui.focus.FocusDirection.Down
                    hatY < -0.5f && lastOverlayHatY >= -0.5f -> androidx.compose.ui.focus.FocusDirection.Up
                    else -> null
                }
                lastOverlayHatX = hatX
                lastOverlayHatY = hatY
                if (direction != null) {
                    moveOverlayFocus(direction)
                    return true
                }
            }
            nativeInputListener.onMotionEventSlot2(event)
            return super.dispatchGenericMotionEvent(event)
        }

        if (nativeInputListener.onMotionEvent(event))
            return true

        return super.dispatchGenericMotionEvent(event)
    }

    private fun hasComposeOverlayOpen(): Boolean {
        return pauseMenuState.value != null || saveStatesOverlayState.value != null ||
            consoleOverlayStack.isNotEmpty() || showDualScreenPresets.value ||
            rewindOverlayState.value != null
    }

    private fun requestOverlayHostFocus(attempt: Int = 0) {
        if (attempt == 0) releaseVirtualButtonInputs()
        val view = binding.layoutPauseMenu
        view.isFocusable = false
        view.isFocusableInTouchMode = false
        view.descendantFocusability = ViewGroup.FOCUS_AFTER_DESCENDANTS

        val target = (view as? ViewGroup)?.takeIf { it.childCount > 0 }?.getChildAt(0) ?: view
        target.isFocusableInTouchMode = true
        if (!target.requestFocusFromTouch()) {
            target.requestFocus()
        }

        if (view.findFocus()?.takeIf { it !== view } == null && attempt < OVERLAY_FOCUS_ATTEMPTS) {
            view.postDelayed({ requestOverlayHostFocus(attempt + 1) }, OVERLAY_FOCUS_RETRY_MS)
        }
    }

    private fun isRewindWindowOpen(): Boolean {
        return rewindOverlayState.value != null
    }

    private fun showSaveStateSlotsDialog(slots: List<SaveStateSlot>, isSaving: Boolean, onSlotPicked: (SaveStateSlot) -> Unit) {
        activeOverlays.addActiveOverlay(EmulatorOverlay.SAVE_STATES_DIALOG)
        saveStatesOverlayState.value = SaveStatesOverlayData(slots, isSaving, onSlotPicked)
        requestOverlayHostFocus()
        val title = getString(if (isSaving) R.string.save_state else R.string.load_state)
        presentation?.setInfoOverlayContent {
            me.magnum.melonds.ui.common.ExternalSaveStatesInfo(
                title = title,
                slots = slots,
                footer = getString(R.string.external_choose_on_device),
            )
        }
    }

    private fun dismissSaveStatesOverlay() {
        if (saveStatesOverlayState.value != null) {
            saveStatesOverlayState.value = null
            activeOverlays.removeActiveOverlay(EmulatorOverlay.SAVE_STATES_DIALOG)
            presentation?.setInfoOverlayContent(null)
        }
    }

    private fun showRomLoadErrorDialog(reason: String?) {
        activeOverlays.addActiveOverlay(EmulatorOverlay.ROM_LOAD_ERROR_DIALOG)
        AlertDialog.Builder(this)
            .setCancelable(false)
            .setTitle(R.string.error_load_rom)
            .setMessage(reason ?: getString(R.string.error_load_rom_message))
            .setPositiveButton(R.string.ok) { dialog, _ ->
                dialog.dismiss()
                finish()
            }
            .show()
    }

    private fun showFrontendSaveSelectionDialog(message: String) {
        activeOverlays.addActiveOverlay(EmulatorOverlay.ROM_LOAD_ERROR_DIALOG)
        AlertDialog.Builder(this)
            .setTitle(R.string.frontend_save_selection_title)
            .setMessage(message)
            .setPositiveButton(R.string.frontend_save_open_library) { _, _ ->
                startActivity(Intent(this, RomListActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP))
            }
            .setNegativeButton(R.string.cancel, null)
            .setOnDismissListener { finish() }
            .show()
    }

    private fun showRomNotFoundDialog(romPath: String) {
        activeOverlays.addActiveOverlay(EmulatorOverlay.ROM_NOT_FOUND_DIALOG)
        AlertDialog.Builder(this)
            .setTitle(R.string.error_rom_not_found)
            .setMessage(getString(R.string.error_rom_not_found_info, romPath))
            .setPositiveButton(R.string.ok) { _, _ ->
                finish()
            }
            .setOnDismissListener {
                finish()
            }
            .show()
    }

    private fun showFirmwareLoadErrorDialog(error: EmulatorState.FirmwareLoadError) {
        activeOverlays.addActiveOverlay(EmulatorOverlay.FIRMWARE_LOAD_ERROR_DIALOG)
        AlertDialog.Builder(this)
            .setCancelable(false)
            .setTitle(R.string.error_load_firmware)
            .setMessage(resources.getString(R.string.error_load_firmware_message, error.reason.toString()))
            .setPositiveButton(R.string.ok) { dialog, _ ->
                dialog.dismiss()
                finish()
            }
            .show()
    }

    private fun showRewindWindow(rewindWindow: RewindWindow) {
        activeOverlays.addActiveOverlay(EmulatorOverlay.REWIND_WINDOW)
        rewindOverlayState.value = rewindWindow
        requestOverlayHostFocus()
    }

    private fun onRewindStateSelected(state: me.magnum.melonds.ui.emulator.rewind.model.RewindSaveState) {
        activeOverlays.removeActiveOverlay(EmulatorOverlay.REWIND_WINDOW)
        rewindOverlayState.value = null
        if (rewindOpenedFromPauseMenu) {
            activeOverlays.removeActiveOverlay(EmulatorOverlay.PAUSE_MENU)
        }
        rewindOpenedFromPauseMenu = false
        viewModel.rewindToState(state)
        viewModel.resumeEmulator()
    }

    private fun closeRewindWindow() {
        activeOverlays.removeActiveOverlay(EmulatorOverlay.REWIND_WINDOW)
        rewindOverlayState.value = null
        if (rewindOpenedFromPauseMenu) {
            rewindOpenedFromPauseMenu = false
            reopenPauseMenu()
        } else {
            viewModel.resumeEmulator()
        }
    }

    private fun updateOrientation(configuration: Configuration) {
        val orientation = if (configuration.orientation == Configuration.ORIENTATION_PORTRAIT) {
            Orientation.PORTRAIT
        } else {
            Orientation.LANDSCAPE
        }
        viewModel.setSystemOrientation(orientation)
    }

    override fun onPause() {
        super.onPause()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.viewTreeObserver.removeOnGlobalFocusChangeListener(joystickFocusListener)
            updateJoystickDispatch(resumed = false)
        }
        releaseVirtualButtonInputs()
        if (PhantomPlayer.current != null) {
            phantomPlayerResumed = false
            showExternalDisplay(null)
        }
        cancelStartupPresentationRefreshes()
        stopShaderDiagnosticsPolling()
        frontendInputHandler.clearFastForwardHold()
        enableScreenTimeOut()
        if (!MultiplayerSession.active) {
            choreographerFrameRenderer.stopRendering()
            if (!isClosingEmulator && !isFinishing) {
                viewModel.pauseEmulator(false)
            }
        }
    }

    private fun releaseVirtualButtonInputs() {
        if (!::binding.isInitialized) return
        if (::nativeInputListener.isInitialized) nativeInputListener.releaseChordInputs()
        binding.viewLayoutControls.releaseVirtualButtonInputs()
        presentation?.layoutView?.releaseVirtualButtonInputs()
        slot2AnalogInput.clear()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        updateOrientation(newConfig)
        // There is an issue in which, after moving the app to a different display, the app reports that it is still running on the previous display. Adding a frame of delay
        // seems to fix the problem.
        handler.post {
            updateDisplays()
        }
    }

    override fun onStop() {
        super.onStop()
        showExternalDisplay(null)
        gameDisplayOwner?.let { ExternalInfoDisplayController.setGameDisplays(it, emptySet()) }
        gameDisplayOwner = null
        cancelStartupPresentationRefreshes()
        getSystemService<DisplayManager>()?.unregisterDisplayListener(displayListener)
        getSystemService<InputManager>()?.unregisterInputDeviceListener(connectedControllerManager)
        connectedControllerManager.stopTrackingControllers()
        if (!isFrameRenderCoordinatorStopped) {
            frameRenderCoordinator.removeSurface(binding.surfaceMain)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        releaseEmulatorUiResources()
    }
}

private const val RENDERER_2D_NATIVE_MODE = -1
private const val RENDERER_2D_STATE_SIZE = 13
private const val RENDERER_3D_STATE_SIZE = 1

private data class RendererDebugMenuEntry(
    val title: String,
    val action: () -> Unit,
)

private data class Renderer2DDebugControlState(
    var mainForcedMode: Int = RENDERER_2D_NATIVE_MODE,
    var subForcedMode: Int = RENDERER_2D_NATIVE_MODE,
    var topForcedCompMode: Int = RENDERER_2D_NATIVE_MODE,
    var bottomForcedCompMode: Int = RENDERER_2D_NATIVE_MODE,
    var disabledMainBgMask: Int = 0,
    var disabledSubBgMask: Int = 0,
    var disabledMainBgPriorityMask: Int = 0,
    var disabledSubBgPriorityMask: Int = 0,
    var disabledMainObjPriorityMask: Int = 0,
    var disabledSubObjPriorityMask: Int = 0,
    var disabledMainObjOrderMask: Int = 0,
    var disabledSubObjOrderMask: Int = 0,
    var featureMask: Int = RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_ALL,
)

private data class Renderer3DDebugControlState(
    var featureMask: Int = RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_ALL,
)

private data class Renderer2DModeItem(
    val mode: Int,
    val label: String,
)

private data class Renderer2DCompModeItem(
    val mode: Int,
    val label: String,
)

private data class Renderer2DBgLayerItem(
    val bgIndex: Int,
    val title: String,
    val description: String,
)

private data class Renderer2DPriorityItem(
    val priority: Int,
    val title: String,
    val description: String,
)

private data class Renderer2DObjectOrderItem(
    val bucket: Int,
    val title: String,
    val description: String,
)

private fun renderer2DDebugModeItems(includeMode6: Boolean): List<Renderer2DModeItem> {
    return buildList {
        add(Renderer2DModeItem(RENDERER_2D_NATIVE_MODE, "Native DISPCNT mode - use CurUnit->DispCnt & 0x7"))
        add(Renderer2DModeItem(0, "Mode 0 - 4 Static layers. Code: DrawScanlineBGMode<0>(); BG0-BG3 use DrawBG_Text."))
        add(Renderer2DModeItem(1, "Mode 1 - 3 Static layers + 1 Affine layer. Code: BG0-BG2 DrawBG_Text, BG3 DrawBG_Affine."))
        add(Renderer2DModeItem(2, "Mode 2 - 2 Static layers + 2 Affine layers. Code: BG0/BG1 DrawBG_Text, BG2/BG3 DrawBG_Affine."))
        add(Renderer2DModeItem(3, "Mode 3 - 3 Static layers + 1 Affine Extended layer. Code: BG3 DrawBG_Extended."))
        add(Renderer2DModeItem(4, "Mode 4 - 2 Static layers + 1 Affine layer + 1 Affine Extended layer. Code: BG2 DrawBG_Affine, BG3 DrawBG_Extended."))
        add(Renderer2DModeItem(5, "Mode 5 - 2 Static layers + 2 Affine Extended layers. Code: BG2/BG3 DrawBG_Extended."))
        if (includeMode6) {
            add(Renderer2DModeItem(6, "Mode 6 - 1 3D background layer + 1 Large screen. Code: BG0 DrawBG_3D, BG2 DrawBG_Large. Main only."))
        }
    }
}

private val renderer2DDebugCompModeItems = listOf(
    Renderer2DCompModeItem(RENDERER_2D_NATIVE_MODE, "Native compMode - keep packed control plane"),
    Renderer2DCompModeItem(0, "compMode 0 - sample 3D path, normal composite branch"),
    Renderer2DCompModeItem(1, "compMode 1 - 3D-aware branch with direct 2D/3D selection"),
    Renderer2DCompModeItem(2, "compMode 2 - 3D-aware blend branch"),
    Renderer2DCompModeItem(3, "compMode 3 - 3D-aware alternate blend branch"),
    Renderer2DCompModeItem(4, "compMode 4 - capture-backed 3D placeholder branch"),
    Renderer2DCompModeItem(5, "compMode 5 - reserved/debug passthrough branch"),
    Renderer2DCompModeItem(6, "compMode 6 - reserved/debug passthrough branch"),
    Renderer2DCompModeItem(7, "compMode 7 - no live 3D sample unless temporal fallback is marked"),
)

private val renderer2DDebugBgLayerItems = listOf(
    Renderer2DBgLayerItem(
        bgIndex = 0,
        title = "BG0 - first static or 3D background layer",
        description = "Nintendo DS BG0. Code gate: DISPCNT bit 8; DrawBG_Text, or DrawBG_3D on Engine A when DISPCNT bit 3 selects the 3D background.",
    ),
    Renderer2DBgLayerItem(
        bgIndex = 1,
        title = "BG1 - static background layer",
        description = "Nintendo DS BG1. Code gate: DISPCNT bit 9; currently routes through DrawBG_Text in the software 2D compositor.",
    ),
    Renderer2DBgLayerItem(
        bgIndex = 2,
        title = "BG2 - static, affine, affine extended or large screen background",
        description = "Nintendo DS BG2. Code gate: DISPCNT bit 10; routes through DrawBG_Text, DrawBG_Affine, DrawBG_Extended or DrawBG_Large depending on BG mode and BGCNT.",
    ),
    Renderer2DBgLayerItem(
        bgIndex = 3,
        title = "BG3 - static, affine or affine extended background",
        description = "Nintendo DS BG3. Code gate: DISPCNT bit 11; routes through DrawBG_Text, DrawBG_Affine or DrawBG_Extended depending on BG mode and BGCNT.",
    ),
)

private val renderer2DDebugPriorityItems = listOf(
    Renderer2DPriorityItem(
        priority = 0,
        title = "Priority 0 - frontmost",
        description = "Highest Nintendo DS priority; this layer is drawn closest to the viewer.",
    ),
    Renderer2DPriorityItem(
        priority = 1,
        title = "Priority 1",
        description = "Second-highest Nintendo DS priority.",
    ),
    Renderer2DPriorityItem(
        priority = 2,
        title = "Priority 2",
        description = "Second-lowest Nintendo DS priority.",
    ),
    Renderer2DPriorityItem(
        priority = 3,
        title = "Priority 3 - backmost",
        description = "Lowest Nintendo DS priority; this layer is drawn furthest back.",
    ),
)

private val renderer2DDebugObjectOrderItems = listOf(
    Renderer2DObjectOrderItem(
        bucket = 0,
        title = "OBJ OAM index 0..31 - frontmost order bucket",
        description = "OAM entries 0-31. Code gate: `sprnum / 32` in `DrawSprites`; lower OBJ indices are drawn later for equal priority and usually appear in front.",
    ),
    Renderer2DObjectOrderItem(
        bucket = 1,
        title = "OBJ OAM index 32..63",
        description = "OAM entries 32-63. Code gate: `sprnum / 32` in `DrawSprites`; useful for separating grouped composite sprites with the same OBJ priority.",
    ),
    Renderer2DObjectOrderItem(
        bucket = 2,
        title = "OBJ OAM index 64..95",
        description = "OAM entries 64-95. Code gate: `sprnum / 32` in `DrawSprites`; later than 96-127 but behind lower OAM index buckets at equal priority.",
    ),
    Renderer2DObjectOrderItem(
        bucket = 3,
        title = "OBJ OAM index 96..127 - backmost order bucket",
        description = "OAM entries 96-127. Code gate: `sprnum / 32` in `DrawSprites`; highest OBJ indices are drawn first for equal priority and usually sit furthest back.",
    ),
)
