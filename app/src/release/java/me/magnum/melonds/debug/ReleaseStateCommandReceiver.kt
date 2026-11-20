package me.magnum.melonds.debug

import android.app.ActivityOptions
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.core.content.edit
import androidx.core.content.FileProvider
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.Input
import me.magnum.melonds.domain.model.SaveStateSlot
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.rom.config.RuntimeConsoleType
import me.magnum.melonds.impl.emulator.debug.RendererDebugBridge
import me.magnum.melonds.impl.emulator.debug.RendererDebugCaptureKind
import me.magnum.melonds.impl.emulator.debug.RendererDebugCaptureLogger
import me.magnum.melonds.impl.emulator.debug.RendererDebugCapturePresets
import me.magnum.melonds.multiplayer.MultiplayerRoom
import me.magnum.melonds.multiplayer.MultiplayerSession
import me.magnum.melonds.ui.emulator.EmulatorActivity
import java.io.File
import java.util.LinkedHashSet
import java.util.Locale

internal class ReleaseStateCommandReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {

        val pendingResult = goAsync()
        receiverScope.launch {
            try {
                DebugCommandExecutionLock.withLock {
                    handleIntent(context.applicationContext, intent)
                }
            } catch (error: Exception) {
                Log.w(TAG, "Release state command failed: action=${intent.action}", error)
            }
        }
        pendingResult.finish()
    }

    private suspend fun handleIntent(context: Context, intent: Intent) {
        val entryPoint = DebugCommandEntryPoint.resolve(context)
        val toolsEnabled = entryPoint.sharedPreferences().getBoolean(KEY_RENDERER_DEBUG_TOOLS_ENABLED, false)
        val propertyEnabled = readBooleanSystemProperty(RELEASE_STATE_COMMANDS_PROPERTY)
        if (!propertyEnabled || (!toolsEnabled && !isConfigurationAction(context, intent.action))) {
            Log.w(
                TAG,
                "action=ignored_release_state_command actionName=${intent.action} toolsEnabled=${if (toolsEnabled) 1 else 0} propertyEnabled=${if (propertyEnabled) 1 else 0}",
            )
            return
        }

        when (intent.action) {
            context.debugCommandAction(ACTION_SET_RENDERER_SUFFIX) -> handleSetRenderer(entryPoint, intent)
            context.debugCommandAction(ACTION_SET_RENDERER_DEBUG_TOOLS_SUFFIX) -> handleSetRendererDebugTools(entryPoint, intent)
            context.debugCommandAction(ACTION_SET_IR_SUFFIX) -> handleSetInternalResolution(entryPoint, intent)
            context.debugCommandAction(ACTION_SET_ROM_RUNTIME_CONSOLE_SUFFIX) -> handleSetRomRuntimeConsole(entryPoint, intent)
            context.debugCommandAction(ACTION_SET_RETROACHIEVEMENTS_SUFFIX) -> handleSetRetroAchievements(entryPoint, intent)
            context.debugCommandAction(ACTION_SET_FAST_FORWARD_SUFFIX) -> handleSetFastForward(intent)
            context.debugCommandAction(ACTION_SET_FRAME_LIMIT_SPEED_SUFFIX) -> handleSetFrameLimitSpeed(entryPoint, intent)
            context.debugCommandAction(ACTION_SET_JIT_SUFFIX) -> handleSetJit(entryPoint, intent)
            context.debugCommandAction(ACTION_SET_FRAMESKIP_SUFFIX) -> handleSetFrameskip(entryPoint, intent)
            context.debugCommandAction(ACTION_SET_MUTE_ON_FAST_FORWARD_SUFFIX) -> handleSetMuteOnFastForward(entryPoint, intent)
            context.debugCommandAction(ACTION_GET_FPS_SUFFIX) -> handleGetFps()
            context.debugCommandAction(ACTION_MULTIPLAYER_STATUS_SUFFIX) -> handleMultiplayerStatus()
            context.debugCommandAction(ACTION_START_AUDIO_PCM_CAPTURE_SUFFIX) -> handleStartAudioPcmCapture(context, intent)
            context.debugCommandAction(ACTION_DUMP_AUDIO_PCM_CAPTURE_SUFFIX) -> handleDumpAudioPcmCapture(context, intent)
            context.debugCommandAction(ACTION_GRANT_AUDIO_PCM_CAPTURE_READ_SUFFIX) -> handleGrantAudioPcmCaptureRead(context, intent)
            context.debugCommandAction(ACTION_SET_BGOBJ_LOG_SUFFIX) -> handleSetBgObjLog(entryPoint, intent)
            context.debugCommandAction(ACTION_SET_RENDERER_2D_DEBUG_CONTROLS_SUFFIX) -> handleSetRenderer2DDebugControls(intent)
            context.debugCommandAction(ACTION_SET_RENDERER_3D_DEBUG_CONTROLS_SUFFIX) -> handleSetRenderer3DDebugControls(intent)
            context.debugCommandAction(ACTION_LAUNCH_ROM_SUFFIX) -> handleLaunchRom(context, intent)
            context.debugCommandAction(ACTION_WAIT_ROM_READY_SUFFIX) -> handleWaitRomReady(intent)
            context.debugCommandAction(ACTION_SET_DEBUG_PAUSE_SUFFIX) -> handleSetDebugPause(intent)
            context.debugCommandAction(ACTION_STEP_FRAME_SUFFIX) -> handleStepFrame(intent)
            context.debugCommandAction(ACTION_STEP_FRAMES_SUFFIX) -> handleStepFrame(intent)
            context.debugCommandAction(ACTION_DUMP_RENDERER_CAPTURE_SUFFIX) -> handleDumpRendererCapture(context, entryPoint, intent)
            context.debugCommandAction(ACTION_ARM_EXACT_LIVE_GUIDE_SUFFIX) -> handleArmExactLiveGuide(intent)
            context.debugCommandAction(ACTION_GET_EXACT_LIVE_GUIDE_SUFFIX) -> handleGetExactLiveGuide()
            context.debugCommandAction(ACTION_ABORT_EXACT_LIVE_GUIDE_SUFFIX) -> handleAbortExactLiveGuide()
            context.debugCommandAction(ACTION_TOUCH_SCREEN_SUFFIX) -> handleTouchScreen(intent)
            context.debugCommandAction(ACTION_PRESS_INPUT_SUFFIX) -> handlePressInput(intent)
            context.debugCommandAction(ACTION_SET_INPUT_HELD_SUFFIX) -> handleSetInputHeld(intent)
            context.debugCommandAction(ACTION_SAVE_STATE_SUFFIX) -> handleSaveState(context, entryPoint, intent)
            context.debugCommandAction(ACTION_LOAD_STATE_SUFFIX) -> handleLoadState(context, entryPoint, intent)
            context.debugCommandAction(ACTION_DUMP_ROM_SEARCH_STATE_SUFFIX) -> handleDumpRomSearchState(entryPoint, intent)
            else -> Log.w(TAG, "Ignored unknown action=${intent.action}")
        }
    }

    private fun handleSetRenderer(
        entryPoint: DebugCommandEntryPoint,
        intent: Intent,
    ) {
        val rendererName = intent.firstStringExtra(EXTRA_RENDERER, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing renderer extra")
        val renderer = parseRenderer(rendererName)
            ?: throw IllegalArgumentException("Unsupported renderer=$rendererName")
        val preferences = entryPoint.sharedPreferences()
        val legacyFastPathPreference =
            preferences.getBoolean(KEY_VULKAN_FASTPATH_ENABLED, false)
        val requestedLegacyFastPath =
            intent.firstBooleanExtra(EXTRA_FASTPATH_ENABLED, EXTRA_FASTPATH)
        preferences.edit(commit = true) {
            putString(KEY_VIDEO_RENDERER, renderer.name.lowercase(Locale.US))
        }
        val refreshed = DebugCommandStateStore.requestSettingsRefresh()
        Log.w(
            TAG,
            "action=set_renderer mode=release renderer=${renderer.name.lowercase(Locale.US)} profile=faithful pipeline=simple_graphics raster=graphics legacyFastPathPreference=${if (legacyFastPathPreference) 1 else 0} legacyPreferenceIgnored=1 requestedLegacyFastPath=${requestedLegacyFastPath?.let { if (it) 1 else 0 } ?: "absent"} applies=next_session refreshed=${if (refreshed) 1 else 0}",
        )
    }

    private fun handleSetRendererDebugTools(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val enabled = intent.firstBooleanExtra(EXTRA_ENABLED, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing enabled extra")
        val latchTraceEnabled =
            intent.firstBooleanExtra(EXTRA_LATCH_TRACE_ENABLED)
        entryPoint.sharedPreferences().edit(commit = true) {
            putBoolean(KEY_RENDERER_DEBUG_TOOLS_ENABLED, enabled)
            latchTraceEnabled?.let {
                putBoolean(KEY_RENDERER_DEBUG_LATCH_TRACE_ENABLED, it)
            }
        }
        val refreshed = DebugCommandStateStore.requestSettingsRefresh()
        Log.w(
            TAG,
            "action=set_renderer_debug_tools mode=release enabled=${if (enabled) 1 else 0} latchTrace=${latchTraceEnabled?.let { if (it) 1 else 0 } ?: "unchanged"} refreshed=${if (refreshed) 1 else 0}",
        )
    }

    private fun handleSetInternalResolution(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val scale = intent.firstNullableIntExtra(EXTRA_SCALE, EXTRA_IR, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing internal resolution extra")
        require(scale in 1..8 || scale == 12 || scale == 16) { "Unsupported internal resolution=$scale" }
        entryPoint.sharedPreferences().edit(commit = true) {
            putString(KEY_VIDEO_INTERNAL_RESOLUTION, scale.toString())
        }
        val refreshed = DebugCommandStateStore.requestSettingsRefresh()
        Log.w(TAG, "action=set_ir mode=release scale=$scale refreshed=${if (refreshed) 1 else 0}")
    }

    private fun handleSetRetroAchievements(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val enabled = intent.firstBooleanExtra(EXTRA_RA_ENABLED, EXTRA_ENABLED)
        val hardcoreEnabled = intent.firstBooleanExtra(EXTRA_RA_HARDCORE_ENABLED, EXTRA_HARDCORE_ENABLED, EXTRA_HARDCORE)
        require(enabled != null || hardcoreEnabled != null) {
            "Missing RetroAchievements setting extra"
        }
        entryPoint.sharedPreferences().edit(commit = true) {
            enabled?.let { putBoolean(KEY_RA_ENABLED, it) }
            hardcoreEnabled?.let { putBoolean(KEY_RA_HARDCORE_ENABLED, it) }
        }
        val refreshed = DebugCommandStateStore.requestSettingsRefresh()
        Log.w(
            TAG,
            "action=set_retroachievements mode=release enabled=${enabled?.let { if (it) 1 else 0 } ?: "unchanged"} hardcore=${hardcoreEnabled?.let { if (it) 1 else 0 } ?: "unchanged"} refreshed=${if (refreshed) 1 else 0}",
        )
    }

    private suspend fun handleSetRomRuntimeConsole(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val romUri = intent.data ?: intent.firstStringExtra(EXTRA_ROM_URI, EXTRA_URI, EXTRA_PATH)?.let { Uri.parse(it) }
            ?: throw IllegalArgumentException("Missing ROM URI. Provide intent data or rom_uri.")
        val runtimeConsole = parseRuntimeConsole(
            intent.firstStringExtra(EXTRA_RUNTIME_CONSOLE, EXTRA_CONSOLE, EXTRA_VALUE)
                ?: throw IllegalArgumentException("Missing runtime console extra"),
        ) ?: throw IllegalArgumentException("Unsupported runtime console")

        val rom = entryPoint.romsRepository().getRomAtUri(romUri)
        val updated = if (rom != null) {
            entryPoint.romsRepository().updateRomConfig(
                rom,
                rom.config.copy(runtimeConsoleType = runtimeConsole),
            ).await()
            true
        } else {
            false
        }
        Log.w(
            TAG,
            "action=set_rom_runtime_console mode=release uri=$romUri runtimeConsole=${runtimeConsole.name} updated=${if (updated) 1 else 0}",
        )
    }

    private fun handleSetFastForward(intent: Intent) {
        val enabled = intent.firstBooleanExtra(EXTRA_ENABLED, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing enabled extra")
        MelonEmulator.setFastForwardEnabled(enabled)
        Log.w(TAG, "action=set_fast_forward mode=release enabled=${if (enabled) 1 else 0}")
    }

    private fun handleSetFrameLimitSpeed(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val multiplier = intent.firstNullableFloatExtra(EXTRA_MULTIPLIER, EXTRA_SPEED, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing frame limit speed extra")
        require(multiplier in 0.25f..1.0f) { "Unsupported frame limit speed=$multiplier" }
        entryPoint.sharedPreferences().edit(commit = true) {
            putString(KEY_FRAME_LIMIT_SPEED_MULTIPLIER, multiplier.toString())
        }
        MelonEmulator.setFrameLimitSpeedMultiplier(multiplier)
        val refreshed = DebugCommandStateStore.requestSettingsRefresh()
        Log.w(TAG, "action=set_frame_limit_speed mode=release multiplier=$multiplier refreshed=${if (refreshed) 1 else 0}")
    }

    private fun handleSetJit(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val enabled = intent.firstBooleanExtra(EXTRA_ENABLED, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing enabled extra")
        entryPoint.sharedPreferences().edit(commit = true) {
            putBoolean(KEY_ENABLE_JIT, enabled)
        }
        val refreshed = DebugCommandStateStore.requestSettingsRefresh()
        Log.w(TAG, "action=set_jit mode=release enabled=${if (enabled) 1 else 0} refreshed=${if (refreshed) 1 else 0}")
    }

    private fun handleSetFrameskip(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val mode = intent.firstStringExtra(EXTRA_MODE, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing mode extra")
        require(mode in setOf("off", "manual", "auto")) { "Unknown frameskip mode $mode" }
        val manualValue = intent.getIntExtra(EXTRA_MANUAL_VALUE, -1)
        entryPoint.sharedPreferences().edit(commit = true) {
            putString(KEY_FRAMESKIP_MODE, mode)
            if (manualValue in 0..4) {
                putString(KEY_FRAMESKIP_MANUAL_VALUE, manualValue.toString())
            }
        }
        Log.w(TAG, "action=set_frameskip mode=release frameskipMode=$mode manualValue=$manualValue")
    }

    private fun handleSetMuteOnFastForward(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val enabled = intent.firstBooleanExtra(EXTRA_ENABLED, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing enabled extra")
        entryPoint.sharedPreferences().edit(commit = true) {
            putBoolean(KEY_MUTE_ON_FAST_FORWARD, enabled)
        }
        Log.w(TAG, "action=set_mute_on_fast_forward mode=release enabled=${if (enabled) 1 else 0}")
    }

    private fun handleGetFps() {
        Log.w(TAG, "action=get_fps mode=release fps=${MelonEmulator.getFPS()} ${MelonEmulator.getVulkanFrameskipStats()}")
    }

    private suspend fun handleMultiplayerStatus() {
        val room = MultiplayerRoom.state.value
        val info = room?.info
        val summary = org.json.JSONObject()
            .put("role", room?.role?.name ?: "NONE")
            .put("linked", room?.linked == true)
            .put("closed", room?.closed == true)
            .put("room", info?.toJson())
        Log.w(TAG, "action=multiplayer_status session=${MultiplayerSession.status()} room=$summary")
    }

    private fun handleStartAudioPcmCapture(context: Context, intent: Intent) {
        val durationMs = intent.firstNullableIntExtra(EXTRA_DURATION_MS)
            ?: DEFAULT_AUDIO_PCM_CAPTURE_DURATION_MS
        require(durationMs in 1..MAX_AUDIO_PCM_CAPTURE_DURATION_MS) {
            "Audio PCM capture duration out of range=$durationMs"
        }
        val captureId = sanitizeAudioCaptureId(
            intent.firstStringExtra(EXTRA_CAPTURE_ID, EXTRA_CAPTURE_ID_BASE)
                ?: throw IllegalArgumentException("Missing audio capture_id"),
        )
        requireAudioCaptureOutputAvailable(audioPcmCaptureOutputRoot(context), captureId)
        val nativeResult = MelonEmulator.startAudioOutputPcmCapture(durationMs)
        if (nativeAudioCaptureCommandSucceeded(nativeResult))
            activeAudioPcmCaptureId = captureId
        Log.w(
            TAG,
            "action=start_audio_pcm_capture mode=release captureId=$captureId durationMs=$durationMs nativeResult=$nativeResult",
        )
    }

    private fun handleDumpAudioPcmCapture(context: Context, intent: Intent) {
        val captureId = sanitizeAudioCaptureId(
            intent.firstStringExtra(EXTRA_CAPTURE_ID, EXTRA_CAPTURE_ID_BASE)
                ?: throw IllegalArgumentException("Missing audio capture_id"),
        )
        require(activeAudioPcmCaptureId == captureId) {
            "Audio capture_id does not match armed capture expected=$activeAudioPcmCaptureId actual=$captureId"
        }
        val outputId = sanitizeAudioCaptureId(
            intent.firstStringExtra(EXTRA_OUTPUT_ID) ?: captureId,
        )
        val outputRoot = audioPcmCaptureOutputRoot(context)
        requireAudioCaptureOutputAvailable(outputRoot, outputId)
        val finalDirectory = File(outputRoot, outputId)

        val nativeResult = MelonEmulator.dumpAudioOutputPcmCapture(finalDirectory.absolutePath)
        var grantedUris = "none"
        if (nativeAudioCaptureCommandSucceeded(nativeResult)) {
            activeAudioPcmCaptureId = null
            grantedUris = grantAudioPcmCaptureReadAccess(context, finalDirectory)
                .joinToString(separator = ",")
        }
        Log.w(
            TAG,
            "action=dump_audio_pcm_capture mode=release captureId=$captureId outputId=$outputId outputDir=${finalDirectory.absolutePath} grantedUris=$grantedUris nativeResult=$nativeResult",
        )
    }

    private fun grantAudioPcmCaptureReadAccess(
        context: Context,
        outputDirectory: File,
    ): List<Uri> {
        val expectedFiles = listOf(
            "audio.wav",
            "callbacks.csv",
            "spu-pretransport.wav",
            "packets.csv",
            "drains.csv",
            "summary.json",
        )
            .map { name -> File(outputDirectory, name) }
        require(expectedFiles.all { file -> file.isFile }) {
            "Published audio capture is incomplete directory=${outputDirectory.absolutePath}"
        }
        return expectedFiles.map { file ->
            val uri = FileProvider.getUriForFile(
                context,
                "${context.packageName}.fileprovider",
                file,
            )
            context.grantUriPermission(
                SHELL_PACKAGE_NAME,
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION,
            )
            uri
        }
    }

    private fun handleGrantAudioPcmCaptureRead(context: Context, intent: Intent) {
        val outputId = sanitizeAudioCaptureId(
            intent.firstStringExtra(EXTRA_OUTPUT_ID, EXTRA_CAPTURE_ID, EXTRA_CAPTURE_ID_BASE)
                ?: throw IllegalArgumentException("Missing audio output_id"),
        )
        val outputDirectory = File(audioPcmCaptureOutputRoot(context), outputId)
        require(outputDirectory.isDirectory) {
            "Audio capture output does not exist outputId=$outputId"
        }
        val grantedUris = grantAudioPcmCaptureReadAccess(context, outputDirectory)
            .joinToString(separator = ",")
        Log.w(
            TAG,
            "action=grant_audio_pcm_capture_read mode=release outputId=$outputId grantedUris=$grantedUris",
        )
    }

    private fun audioPcmCaptureOutputRoot(context: Context): File {
        val outputRoot = context.getExternalFilesDir(AUDIO_PCM_CAPTURE_DIRECTORY)
            ?: throw IllegalStateException("External files directory unavailable")
        require(outputRoot.isDirectory || outputRoot.mkdirs()) {
            "Could not create audio capture directory=${outputRoot.absolutePath}"
        }
        return outputRoot
    }

    private fun requireAudioCaptureOutputAvailable(outputRoot: File, outputId: String) {
        val finalDirectory = File(outputRoot, outputId)
        val stagingDirectory = File(outputRoot, "$outputId.partial")
        require(!finalDirectory.exists() && !stagingDirectory.exists()) {
            "Audio capture output already exists outputId=$outputId"
        }
    }

    private fun sanitizeAudioCaptureId(value: String): String {
        val sanitized = value
            .take(MAX_AUDIO_PCM_CAPTURE_ID_LENGTH)
            .map { character ->
                if (character in 'a'..'z' || character in 'A'..'Z' ||
                    character in '0'..'9' || character == '.' ||
                    character == '_' || character == '-') {
                    character
                } else {
                    '_'
                }
            }
            .joinToString(separator = "")
        require(sanitized.isNotBlank() && sanitized != "." && sanitized != "..") {
            "Invalid audio capture id"
        }
        return sanitized
    }

    private fun nativeAudioCaptureCommandSucceeded(result: String?): Boolean {
        return result?.startsWith("{\"success\":1,") == true
    }

    private fun handleSetBgObjLog(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val enabled = intent.firstBooleanExtra(EXTRA_ENABLED, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing enabled extra")
        entryPoint.sharedPreferences().edit(commit = true) {
            putBoolean(KEY_RENDERER_DEBUG_BGOBJ_ENABLED, enabled)
        }
        val refreshed = DebugCommandStateStore.requestSettingsRefresh()
        Log.w(
            TAG,
            "action=set_bgobj_log mode=release enabled=${if (enabled) 1 else 0} refreshed=${if (refreshed) 1 else 0}",
        )
    }

    private fun handleSetRenderer2DDebugControls(intent: Intent) {
        val state = readRenderer2DDebugControlState()
        val featureMask = intent.firstNullableIntExtra(EXTRA_FEATURE_MASK)
            ?: state[RENDERER_2D_STATE_FEATURE_MASK_INDEX]
        val mainForcedMode = intent.firstNullableIntExtra(EXTRA_MAIN_FORCED_MODE)
            ?: state[RENDERER_2D_STATE_MAIN_FORCED_MODE_INDEX]
        val subForcedMode = intent.firstNullableIntExtra(EXTRA_SUB_FORCED_MODE)
            ?: state[RENDERER_2D_STATE_SUB_FORCED_MODE_INDEX]
        val topForcedCompMode = intent.firstNullableIntExtra(EXTRA_TOP_FORCED_COMP_MODE)
            ?: state[RENDERER_2D_STATE_TOP_FORCED_COMP_MODE_INDEX]
        val bottomForcedCompMode = intent.firstNullableIntExtra(EXTRA_BOTTOM_FORCED_COMP_MODE)
            ?: state[RENDERER_2D_STATE_BOTTOM_FORCED_COMP_MODE_INDEX]
        val disabledMainBgMask = intent.firstNullableIntExtra(EXTRA_DISABLED_MAIN_BG_MASK)
            ?: state[RENDERER_2D_STATE_DISABLED_MAIN_BG_MASK_INDEX]
        val disabledSubBgMask = intent.firstNullableIntExtra(EXTRA_DISABLED_SUB_BG_MASK)
            ?: state[RENDERER_2D_STATE_DISABLED_SUB_BG_MASK_INDEX]
        val disabledMainBgPriorityMask = intent.firstNullableIntExtra(EXTRA_DISABLED_MAIN_BG_PRIORITY_MASK)
            ?: state[RENDERER_2D_STATE_DISABLED_MAIN_BG_PRIORITY_MASK_INDEX]
        val disabledSubBgPriorityMask = intent.firstNullableIntExtra(EXTRA_DISABLED_SUB_BG_PRIORITY_MASK)
            ?: state[RENDERER_2D_STATE_DISABLED_SUB_BG_PRIORITY_MASK_INDEX]
        val disabledMainObjPriorityMask = intent.firstNullableIntExtra(EXTRA_DISABLED_MAIN_OBJ_PRIORITY_MASK)
            ?: state[RENDERER_2D_STATE_DISABLED_MAIN_OBJ_PRIORITY_MASK_INDEX]
        val disabledSubObjPriorityMask = intent.firstNullableIntExtra(EXTRA_DISABLED_SUB_OBJ_PRIORITY_MASK)
            ?: state[RENDERER_2D_STATE_DISABLED_SUB_OBJ_PRIORITY_MASK_INDEX]
        val disabledMainObjOrderMask = intent.firstNullableIntExtra(EXTRA_DISABLED_MAIN_OBJ_ORDER_MASK)
            ?: state[RENDERER_2D_STATE_DISABLED_MAIN_OBJ_ORDER_MASK_INDEX]
        val disabledSubObjOrderMask = intent.firstNullableIntExtra(EXTRA_DISABLED_SUB_OBJ_ORDER_MASK)
            ?: state[RENDERER_2D_STATE_DISABLED_SUB_OBJ_ORDER_MASK_INDEX]

        RendererDebugBridge.setRenderer2DDebugControls(
            mainForcedMode = mainForcedMode,
            subForcedMode = subForcedMode,
            topForcedCompMode = topForcedCompMode,
            bottomForcedCompMode = bottomForcedCompMode,
            disabledMainBgMask = disabledMainBgMask,
            disabledSubBgMask = disabledSubBgMask,
            disabledMainBgPriorityMask = disabledMainBgPriorityMask,
            disabledSubBgPriorityMask = disabledSubBgPriorityMask,
            disabledMainObjPriorityMask = disabledMainObjPriorityMask,
            disabledSubObjPriorityMask = disabledSubObjPriorityMask,
            disabledMainObjOrderMask = disabledMainObjOrderMask,
            disabledSubObjOrderMask = disabledSubObjOrderMask,
            featureMask = featureMask,
        )
        Log.w(
            TAG,
            "action=set_renderer_2d_debug_controls mode=release featureMask=$featureMask mainForcedMode=$mainForcedMode subForcedMode=$subForcedMode topComp=$topForcedCompMode bottomComp=$bottomForcedCompMode disabledMainBg=$disabledMainBgMask disabledSubBg=$disabledSubBgMask",
        )
        stepRendererDebugForwardFrameIfPaused("renderer_2d_debug_controls")
    }

    private fun handleSetRenderer3DDebugControls(intent: Intent) {
        val state = RendererDebugBridge.getRenderer3DDebugControls()
        val currentFeatureMask = if (state != null && state.isNotEmpty()) {
            state[0]
        } else {
            RendererDebugBridge.RENDERER_3D_DEBUG_FEATURE_ALL
        }
        val featureMask = intent.firstNullableIntExtra(EXTRA_FEATURE_MASK) ?: currentFeatureMask
        RendererDebugBridge.setRenderer3DDebugControls(featureMask)
        Log.w(TAG, "action=set_renderer_3d_debug_controls mode=release featureMask=$featureMask")
        stepRendererDebugForwardFrameIfPaused("renderer_3d_debug_controls")
    }

    private suspend fun handleDumpRomSearchState(entryPoint: DebugCommandEntryPoint, intent: Intent) {
        val query = intent.firstStringExtra(EXTRA_QUERY, EXTRA_VALUE)
            ?.trim()
            ?.takeIf { it.isNotEmpty() }
        val directories = entryPoint.settingsRepository().getRomSearchDirectories()
        Log.w(
            TAG,
            "action=dump_rom_search_state mode=release directoryCount=${directories.size} query=${query.orEmpty()}",
        )
        directories.forEachIndexed { index, uri ->
            Log.w(TAG, "action=dump_rom_search_dir mode=release index=$index uri=$uri")
        }

        val normalizedQuery = query?.lowercase(Locale.US)
        val matches = entryPoint.romsRepository().getRoms().first()
            .asSequence()
            .filter { rom ->
                normalizedQuery == null ||
                    rom.name.lowercase(Locale.US).contains(normalizedQuery) ||
                    rom.fileName.lowercase(Locale.US).contains(normalizedQuery) ||
                    rom.uri.toString().lowercase(Locale.US).contains(normalizedQuery)
            }
            .take(50)
            .toList()

        Log.w(TAG, "action=dump_rom_search_matches mode=release count=${matches.size}")
        matches.forEachIndexed { index, rom ->
            Log.w(
                TAG,
                "action=dump_rom_search_match mode=release index=$index name=${rom.name} fileName=${rom.fileName} uri=${rom.uri} parentTreeUri=${rom.parentTreeUri}",
            )
        }
    }

    private fun readRenderer2DDebugControlState(): IntArray {
        val state = RendererDebugBridge.getRenderer2DDebugControls()
        return if (state != null && state.size >= RENDERER_2D_STATE_SIZE) {
            state
        } else {
            intArrayOf(
                RENDERER_2D_NATIVE_MODE,
                RENDERER_2D_NATIVE_MODE,
                RENDERER_2D_NATIVE_MODE,
                RENDERER_2D_NATIVE_MODE,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                RendererDebugBridge.RENDERER_2D_DEBUG_FEATURE_ALL,
            )
        }
    }

    private fun handleSetDebugPause(intent: Intent) {
        val paused = intent.firstBooleanExtra(EXTRA_PAUSED, EXTRA_ENABLED, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing paused/enabled extra")
        DebugCommandStateStore.setDebugPauseHeld(paused)
        if (paused) {
            MelonEmulator.pauseEmulation()
        } else {
            MelonEmulator.resumeEmulation()
        }
        Log.w(TAG, "action=set_debug_pause mode=release paused=${if (paused) 1 else 0}")
    }

    private suspend fun handleStepFrame(intent: Intent) {
        val frames = intent.firstNullableIntExtra(EXTRA_STEP_FRAMES, EXTRA_FRAMES, EXTRA_VALUE)
            ?.coerceAtLeast(1)
            ?: 1
        val timeoutMs = intent.firstNullableIntExtra(EXTRA_TIMEOUT_MS, EXTRA_DURATION_MS, EXTRA_RESUME_MS)
            ?.coerceAtLeast(1)
            ?: 5_000
        val nativeRenderer = MelonEmulator.getCurrentRenderer()
        val renderer = VideoRenderer.entries.first { it.renderer == nativeRenderer }
        val startFrame = RendererDebugBridge.getCurrentFrameIndexForDebug()
        require(frames <= 10_000) { "Frame step budget exceeds 10000" }
        val expectedFrame = Math.addExact(startFrame, frames)
        try {

            check(MelonEmulator.debugStepFrames(frames)) { "Exact frame step was not accepted" }
            waitForRendererFrameOrTimeout(
                renderer = renderer,
                startFrame = startFrame,
                resumeFrames = frames,
                timeoutMs = timeoutMs.toLong(),
            )
        } finally {

            MelonEmulator.pauseEmulation()
            DebugCommandStateStore.setDebugPauseHeld(true)
        }
        val endFrame = RendererDebugBridge.getCurrentFrameIndexForDebug()
        val ready = renderer != VideoRenderer.VULKAN || RendererDebugBridge.isCurrentFrameReadyForDebug()
        val exact = ready && endFrame == expectedFrame
        Log.w(
            TAG,
            "action=step_frame mode=release exactBudget=1 renderer=${renderer.name.lowercase(Locale.US)} frames=$frames startFrame=$startFrame expectedFrame=$expectedFrame endFrame=$endFrame ready=${if (ready) 1 else 0} exact=${if (exact) 1 else 0}",
        )
        check(exact) { "Exact step incomplete: expected=$expectedFrame actual=$endFrame ready=$ready" }
    }

    private fun stepRendererDebugForwardFrameIfPaused(reason: String) {
        if (!DebugCommandStateStore.isDebugPauseHeld()) {
            return
        }
        val success = MelonEmulator.debugStepFrame()
        Log.w(TAG, "action=auto_step_frame mode=release reason=$reason success=${if (success) 1 else 0}")
    }

    private suspend fun handleDumpRendererCapture(
        context: Context,
        entryPoint: DebugCommandEntryPoint,
        intent: Intent,
    ) {
        val refreshSettings = intent.firstBooleanExtra(EXTRA_REFRESH, EXTRA_REFRESH_SETTINGS) ?: true
        val refreshed = if (refreshSettings) {
            entryPoint.sharedPreferences().edit(commit = true) {
                putBoolean(KEY_RENDERER_DEBUG_TOOLS_ENABLED, true)
            }
            DebugCommandStateStore.requestSettingsRefresh()
        } else {
            false
        }
        if (refreshed) {
            delay(350L)
        }

        val renderer = entryPoint.settingsRepository().getCurrentVideoRenderer()

        val keepDarkBurst = intent.firstStringExtra(EXTRA_CAPTURE_KINDS, EXTRA_KINDS)?.contains("keepdark", ignoreCase = true) == true
        val burstCount = intent.firstNullableIntExtra(EXTRA_BURST_COUNT, EXTRA_CAPTURE_COUNT)
            ?.coerceIn(1, if (keepDarkBurst) 60_000 else 600)
            ?: 1
        val burstLive = intent.firstBooleanExtra(EXTRA_BURST_LIVE, EXTRA_LIVE_BURST) ?: (burstCount > 1)
        val burstStepFrames = intent.firstNullableIntExtra(EXTRA_BURST_STEP_FRAMES, EXTRA_STEP_FRAMES)
            ?.coerceAtLeast(1)
            ?: 1

        val burstFfToggleAt = intent.getIntExtra(EXTRA_BURST_FF_TOGGLE_AT, -1)
        val burstFfToggleEnabled = intent.getBooleanExtra(EXTRA_BURST_FF_TOGGLE_ENABLED, false)
        val timeoutMs = intent.firstNullableIntExtra(EXTRA_TIMEOUT_MS)
            ?.coerceAtLeast(1)
            ?.toLong()
            ?: ((burstCount.toLong() * burstStepFrames.toLong() * 1_000L) / 24L + 5_000L)
        val resumeMs = intent.firstNullableIntExtra(EXTRA_RESUME_MS, EXTRA_DURATION_MS)
            ?.coerceAtLeast(0)
            ?: 0
        val resumeFrames = intent.firstNullableIntExtra(EXTRA_RESUME_FRAMES, EXTRA_FRAMES)
            ?.coerceAtLeast(0)
            ?: 0
        val captureKinds = parseCaptureKinds(
            intent.firstStringExtra(EXTRA_CAPTURE_KINDS, EXTRA_KINDS),
            if (burstCount > 1) {
                setOf(RendererDebugCaptureKind.SCREEN_FRAME)
            } else {
                RendererDebugCapturePresets.vulkanExactFrame
            },
        )
        val captureId = intent.firstStringExtra(EXTRA_CAPTURE_ID, EXTRA_CAPTURE_ID_BASE)
            ?.takeIf { it.isNotBlank() }
        val outputDir = context.getExternalFilesDir("renderer-debug-captures")
            ?: File(context.cacheDir, "renderer-debug-captures")
        val captureOutputDir = if (burstCount > 1) {
            File(outputDir, "burst_${System.currentTimeMillis()}").apply { mkdirs() }
        } else {
            outputDir
        }
        val pauseWasHeld = DebugCommandStateStore.isDebugPauseHeld()

        try {
            if (burstCount > 1 && burstLive) {
                if (resumeFrames <= 0 && resumeMs > 0) {
                    DebugCommandStateStore.setDebugPauseHeld(false)
                    MelonEmulator.resumeEmulation()
                    delay(resumeMs.toLong())
                }
                val resumeAfterCaptureArmed: suspend () -> Unit = {
                    DebugCommandStateStore.setDebugPauseHeld(false)
                    MelonEmulator.resumeEmulation()
                }
                val captureBaseId = captureId ?: java.lang.Long.toHexString(System.currentTimeMillis())
                val results = RendererDebugCaptureLogger.dumpDenseScreenBurstCapture(
                    configuredRenderer = renderer,
                    outputDir = captureOutputDir,
                    captureIdBase = captureBaseId,
                    burstCount = burstCount,
                    burstStepFrames = burstStepFrames,
                    timeoutMs = timeoutMs,
                    captureKinds = captureKinds,
                    warmupFrames = resumeFrames,
                    onCaptureArmed = if (resumeFrames > 0 || resumeMs <= 0) {
                        resumeAfterCaptureArmed
                    } else {
                        null
                    },
                    burstHookAtFrame = burstFfToggleAt,
                    onBurstFrame = if (burstFfToggleAt >= 0) {
                        {
                            MelonEmulator.setFastForwardEnabled(burstFfToggleEnabled)
                            Log.w(TAG, "action=set_fast_forward mode=release enabled=${if (burstFfToggleEnabled) 1 else 0} source=burst_hook")
                        }
                    } else {
                        null
                    },
                )
                val successCount = results.count { it.success }
                Log.w(
                    TAG,
                    "action=dump_renderer_capture mode=release renderer=${renderer.name.lowercase(Locale.US)} refreshed=${if (refreshed) 1 else 0} paused=${if (pauseWasHeld) 1 else 0} liveBurst=1 resumeMs=$resumeMs resumeFrames=$resumeFrames warmupMode=${if (resumeFrames > 0) "native_callbacks" else if (resumeMs > 0) "elapsed_ms" else "none"} burstCount=$burstCount burstStepFrames=$burstStepFrames captureId=$captureBaseId success=$successCount/${results.size} outputDir=${captureOutputDir.absolutePath}",
                )
                return
            }

            if (!pauseWasHeld) {
                MelonEmulator.pauseEmulation()
                DebugCommandStateStore.setDebugPauseHeld(true)
                waitForRendererReadyOrTimeout(renderer, timeoutMs = 1_000L)
            } else if (resumeMs > 0 || resumeFrames > 0) {
                DebugCommandStateStore.setDebugPauseHeld(false)
                val startFrame = RendererDebugBridge.getCurrentFrameIndexForDebug()
                MelonEmulator.resumeEmulation()
                waitForRendererFrameOrTimeout(
                    renderer = renderer,
                    startFrame = startFrame,
                    resumeFrames = resumeFrames,
                    timeoutMs = resumeMs.toLong(),
                )
                MelonEmulator.pauseEmulation()
                waitForRendererReadyOrTimeout(
                    renderer = renderer,
                    minFrame = RendererDebugBridge.getCurrentFrameIndexForDebug(),
                    timeoutMs = resumeMs.coerceAtLeast(1_000).toLong(),
                )
                DebugCommandStateStore.setDebugPauseHeld(true)
            }

            val result = RendererDebugCaptureLogger.dumpPauseMenuCapture(
                configuredRenderer = renderer,
                outputDir = outputDir,
                captureIdOverride = captureId,
                captureKinds = captureKinds,
                freezeRendererSnapshot = true,
            )
            Log.w(
                TAG,
                "action=dump_renderer_capture mode=release renderer=${renderer.name.lowercase(Locale.US)} refreshed=${if (refreshed) 1 else 0} paused=${if (pauseWasHeld) 1 else 0} resumeMs=$resumeMs resumeFrames=$resumeFrames captureId=${result.captureId} success=${if (result.success) 1 else 0} outputDir=${result.outputDir?.absolutePath ?: outputDir.absolutePath}",
            )
        } finally {
            if (!pauseWasHeld) {
                DebugCommandStateStore.setDebugPauseHeld(false)
                MelonEmulator.resumeEmulation()
            }
        }
    }

    private suspend fun handleTouchScreen(intent: Intent) {
        val x = intent.firstNullableIntExtra(EXTRA_X, EXTRA_VALUE_X, EXTRA_VALUE)
            ?.coerceIn(0, 255)
            ?: DEFAULT_TOUCH_X
        val y = intent.firstNullableIntExtra(EXTRA_Y, EXTRA_VALUE_Y)
            ?.coerceIn(0, 191)
            ?: DEFAULT_TOUCH_Y
        val durationMs = (intent.firstNullableIntExtra(EXTRA_DURATION_MS) ?: DEFAULT_TOUCH_DURATION_MS)
            .coerceIn(1, 2_000)
        MelonEmulator.onInputDown(Input.TOUCHSCREEN)
        MelonEmulator.onScreenTouch(x, y)
        delay(durationMs.toLong())
        MelonEmulator.onInputUp(Input.TOUCHSCREEN)
        MelonEmulator.onScreenRelease()
        Log.w(TAG, "action=touch_screen mode=release x=$x y=$y durationMs=$durationMs")
    }

    private fun handleArmExactLiveGuide(intent: Intent) {
        val anchorFrame = intent.firstNullableIntExtra(EXTRA_ANCHOR_FRAME)?.toLong() ?: -1L
        val x = intent.firstNullableIntExtra(EXTRA_X, EXTRA_VALUE_X, EXTRA_VALUE)
            ?.coerceIn(0, 255)
            ?: DEFAULT_TOUCH_X
        val y = intent.firstNullableIntExtra(EXTRA_Y, EXTRA_VALUE_Y)
            ?.coerceIn(0, 191)
            ?: DEFAULT_TOUCH_Y
        val status = MelonEmulator.armExactLiveGuide(anchorFrame, x, y)
            ?: "{\"state\":\"jni_error\"}"
        Log.w(
            TAG,
            "action=arm_exact_live_guide mode=release anchorFrame=$anchorFrame x=$x y=$y status=$status",
        )
    }

    private fun handleGetExactLiveGuide() {
        val status = MelonEmulator.getExactLiveGuideStatus()
            ?: "{\"state\":\"jni_error\"}"
        Log.w(TAG, "action=get_exact_live_guide mode=release status=$status")
    }

    private fun handleAbortExactLiveGuide() {
        val status = MelonEmulator.abortExactLiveGuide()
            ?: "{\"state\":\"jni_error\"}"
        Log.w(TAG, "action=abort_exact_live_guide mode=release status=$status")
    }

    private suspend fun handleLaunchRom(context: Context, intent: Intent) {
        val romUri = intent.data ?: intent.firstStringExtra(EXTRA_ROM_URI, EXTRA_URI, EXTRA_PATH)?.let { Uri.parse(it) }
            ?: throw IllegalArgumentException("Missing ROM URI. Provide intent data or rom_uri.")
        val waitReady = intent.firstBooleanExtra(EXTRA_WAIT_ROM_READY, EXTRA_WAIT_READY) ?: false
        val pauseAfterReady = intent.getBooleanExtra(EXTRA_PAUSE_AFTER, false)
        val requestedTimeoutMs = intent.firstNullableIntExtra(EXTRA_WAIT_TIMEOUT_MS, EXTRA_TIMEOUT_MS)
            ?.coerceAtLeast(1)
            ?: DEFAULT_ROM_READY_TIMEOUT_MS

        if (waitReady) {
            DebugCommandStateStore.requestPauseAfterNextRunningRom(pauseAfterReady)
        }

        startEmulatorActivityFromReleaseCommand(
            context = context,
            launchIntent = Intent(context, EmulatorActivity::class.java).apply {
                action = context.debugCommandAction(ACTION_LAUNCH_ROM_SUFFIX)
                data = romUri
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP)
                addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
            },
        )

        delay(LAUNCH_ACTIVITY_SEEN_TIMEOUT_MS)
        val activitySeen = DebugCommandStateStore.hasEmulatorActivity()
        val ready = DebugCommandStateStore.isRunningRom()
        if (ready && waitReady) {
            applyPauseAfterReady(pauseAfterReady)
        }
        Log.w(
            TAG,
            "action=launch_rom mode=release uri=$romUri waitReady=${if (waitReady) 1 else 0} activitySeen=${if (activitySeen) 1 else 0} ready=${if (ready) 1 else 0} pauseAfter=${if (pauseAfterReady) 1 else 0} requestedTimeoutMs=$requestedTimeoutMs deferredReady=1",
        )
    }

    private fun startEmulatorActivityFromReleaseCommand(context: Context, launchIntent: Intent) {
        val options = ActivityOptions.makeBasic()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            val mode = if (Build.VERSION.SDK_INT >= 36) {
                ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS
            } else {
                @Suppress("DEPRECATION")
                ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED
            }
            options.setPendingIntentBackgroundActivityStartMode(mode)
            options.setPendingIntentCreatorBackgroundActivityStartMode(mode)
        }

        val pendingIntent = PendingIntent.getActivity(
            context,
            REQUEST_CODE_LAUNCH_ROM,
            launchIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        pendingIntent.send(
            context,
            0,
            null,
            null,
            null,
            null,
            options.toBundle(),
        )
    }

    private suspend fun handleWaitRomReady(intent: Intent) {
        val pauseAfterReady = intent.getBooleanExtra(EXTRA_PAUSE_AFTER, false)
        val requestedTimeoutMs = intent.firstNullableIntExtra(EXTRA_WAIT_TIMEOUT_MS, EXTRA_TIMEOUT_MS)
            ?.coerceAtLeast(1)
            ?: DEFAULT_ROM_READY_TIMEOUT_MS
        val timeoutMs = requestedTimeoutMs.coerceAtMost(MAX_RECEIVER_WAIT_TIMEOUT_MS)
        val ready = DebugCommandStateStore.waitForRunningRom(timeoutMs.toLong())
        if (ready) {
            applyPauseAfterReady(pauseAfterReady)
        }
        Log.w(
            TAG,
            "action=wait_rom_ready mode=release ready=${if (ready) 1 else 0} pauseAfter=${if (pauseAfterReady) 1 else 0} timeoutMs=$timeoutMs requestedTimeoutMs=$requestedTimeoutMs",
        )
    }

    private suspend fun handlePressInput(intent: Intent) {
        val rawInputs = intent.firstStringExtra(EXTRA_INPUTS, EXTRA_INPUT, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing input extra")
        val durationMs = (intent.firstNullableIntExtra(EXTRA_DURATION_MS) ?: DEFAULT_INPUT_DURATION_MS)
            .coerceIn(0, 2_000)
        val gapMs = (intent.firstNullableIntExtra(EXTRA_GAP_MS, EXTRA_DELAY_MS) ?: DEFAULT_INPUT_GAP_MS)
            .coerceIn(0, 2_000)
        val repeatCount = (intent.firstNullableIntExtra(EXTRA_REPEAT, EXTRA_COUNT) ?: 1)
            .coerceIn(1, 100)
        val inputs = rawInputs
            .split(',', '+', ' ', ';')
            .mapNotNull { parseInput(it) }
        require(inputs.isNotEmpty()) { "No supported inputs in $rawInputs" }

        repeat(repeatCount) { repeatIndex ->
            inputs.forEachIndexed { index, input ->
                MelonEmulator.onInputDown(input)
                delay(durationMs.toLong())
                MelonEmulator.onInputUp(input)
                if (repeatIndex != repeatCount - 1 || index != inputs.lastIndex) {
                    delay(gapMs.toLong())
                }
            }
        }
        Log.w(
            TAG,
            "action=press_input mode=release inputs=${inputs.joinToString(",") { it.name }} repeat=$repeatCount durationMs=$durationMs gapMs=$gapMs",
        )
    }

    private fun handleSetInputHeld(intent: Intent) {
        val rawInputs = intent.firstStringExtra(EXTRA_INPUTS, EXTRA_INPUT, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing input extra")
        val held = intent.firstBooleanExtra(EXTRA_HELD, EXTRA_DOWN, EXTRA_PRESSED, EXTRA_ENABLED, EXTRA_VALUE)
            ?: throw IllegalArgumentException("Missing held/down/enabled extra")
        val inputs = rawInputs
            .split(',', '+', ' ', ';')
            .mapNotNull { parseInput(it) }
        require(inputs.isNotEmpty()) { "No supported inputs in $rawInputs" }

        inputs.forEach { input ->
            if (held) {
                MelonEmulator.onInputDown(input)
            } else {
                MelonEmulator.onInputUp(input)
            }
        }
        Log.w(
            TAG,
            "action=set_input_held mode=release inputs=${inputs.joinToString(",") { it.name }} held=${if (held) 1 else 0}",
        )
    }

    private suspend fun handleLoadState(
        context: Context,
        entryPoint: DebugCommandEntryPoint,
        intent: Intent,
    ) {
        val stateUri = resolveStateUri(context, entryPoint, intent, preferExistingSlotFallback = true)
            ?: throw IllegalArgumentException("Missing load target. Provide slot or path.")
        val pauseAfterLoad = intent.getBooleanExtra(EXTRA_PAUSE_AFTER, false)
        val pauseSequenceAtStart = DebugCommandStateStore.currentPauseCommandSequence()
        MelonEmulator.pauseEmulation()
        val success = try {
            MelonEmulator.loadState(stateUri)
        } finally {
            val explicitPauseCommandArrived =
                DebugCommandStateStore.currentPauseCommandSequence() != pauseSequenceAtStart
            if (explicitPauseCommandArrived) {
                if (!DebugCommandStateStore.isDebugPauseHeld()) {
                    MelonEmulator.resumeEmulation()
                }
            } else if (pauseAfterLoad) {
                DebugCommandStateStore.setDebugPauseHeld(true)
            } else {
                DebugCommandStateStore.setDebugPauseHeld(false)
                MelonEmulator.resumeEmulation()
            }
        }
        Log.w(
            TAG,
            "action=load_state mode=release uri=$stateUri success=${if (success) 1 else 0} pauseAfter=${if (pauseAfterLoad) 1 else 0}",
        )
    }

    private suspend fun handleSaveState(
        context: Context,
        entryPoint: DebugCommandEntryPoint,
        intent: Intent,
    ) {
        val stateUri = resolveStateUri(context, entryPoint, intent, preferExistingSlotFallback = false)
            ?: throw IllegalArgumentException("Missing save target. Provide slot or path.")
        val pauseAfterSave = intent.getBooleanExtra(EXTRA_PAUSE_AFTER, false)
        MelonEmulator.pauseEmulation()
        val success = try {
            MelonEmulator.saveState(stateUri)
        } finally {
            if (pauseAfterSave) {
                DebugCommandStateStore.setDebugPauseHeld(true)
            } else {
                DebugCommandStateStore.setDebugPauseHeld(false)
                MelonEmulator.resumeEmulation()
            }
        }
        Log.w(
            TAG,
            "action=save_state mode=release uri=$stateUri success=${if (success) 1 else 0} pauseAfter=${if (pauseAfterSave) 1 else 0}",
        )
    }

    private suspend fun resolveStateUri(
        context: Context,
        entryPoint: DebugCommandEntryPoint,
        intent: Intent,
        preferExistingSlotFallback: Boolean,
    ): Uri? {
        intent.firstStringExtra(EXTRA_PATH, EXTRA_URI)?.let { pathOrUri ->
            return parseUri(pathOrUri)
        }

        val slot = intent.firstNullableIntExtra(EXTRA_SLOT, EXTRA_VALUE) ?: return null
        require(slot in 0..8) { "Unsupported save state slot=$slot" }

        val romUri = resolveRomUriForSlot(context, intent) ?: return null
        val rom = entryPoint.romsRepository().getRomAtUri(romUri) ?: return null
        val resolvedUri = entryPoint.saveStatesRepository().getRomSaveStateUri(
            rom,
            SaveStateSlot(slot, exists = true, lastUsedDate = null, screenshot = null),
        )
        if (!preferExistingSlotFallback) {
            return resolvedUri
        }

        val fallbackUri = resolveExistingSlotFallbackUri(
            preferredUri = resolvedUri,
            romFileName = rom.fileName,
            slot = slot,
        ) ?: return resolvedUri
        Log.w(
            TAG,
            "action=slot_fallback mode=release slot=$slot preferred=$resolvedUri fallback=$fallbackUri",
        )
        return fallbackUri
    }

    private suspend fun resolveRomUriForSlot(context: Context, intent: Intent): Uri? {
        intent.firstStringExtra(EXTRA_ROM_URI)?.let { return Uri.parse(it) }

        var romUri = DebugCommandStateStore.getLastRomUri(context)
        if (romUri != null) {
            return romUri
        }

        val deadlineAt = System.nanoTime() + ROM_URI_RESOLVE_TIMEOUT_MS * 1_000_000L
        while (romUri == null && System.nanoTime() < deadlineAt) {
            delay(ROM_URI_RESOLVE_STEP_MS)
            romUri = DebugCommandStateStore.getLastRomUri(context)
        }
        return romUri
    }

    private fun resolveExistingSlotFallbackUri(
        preferredUri: Uri,
        romFileName: String,
        slot: Int,
    ): Uri? {
        if (preferredUri.scheme != "file") {
            return null
        }
        val preferredPath = preferredUri.path ?: return null
        val preferredFile = File(preferredPath)
        if (preferredFile.exists() && preferredFile.length() > 0L) {
            return null
        }
        val parentDirectory = preferredFile.parentFile
            ?.takeIf { it.exists() && it.isDirectory }
            ?: return null

        val romName = romFileName.substringBeforeLast('.', romFileName).trim()
        if (romName.isEmpty()) {
            return null
        }
        val candidates = buildAlternativeSaveStateNames(romName).asSequence()
            .map { candidateName -> File(parentDirectory, "$candidateName.ml$slot") }
            .firstOrNull { candidateFile -> candidateFile.exists() && candidateFile.length() > 0L }
            ?: return null
        return Uri.fromFile(candidates)
    }

    private fun buildAlternativeSaveStateNames(romName: String): List<String> {
        val normalized = romName.trim()
        if (normalized.isEmpty()) {
            return emptyList()
        }

        val names = LinkedHashSet<String>()
        val analogSuffixes = listOf(" Analog", " (Analog)", " [Analog]", "[Analog]")
        analogSuffixes.forEach { suffix ->
            if (normalized.endsWith(suffix, ignoreCase = true)) {
                val stripped = normalized.dropLast(suffix.length).trimEnd()
                if (stripped.isNotEmpty()) {
                    names.add(stripped)
                }
            }
        }
        if (!normalized.endsWith(" Analog", ignoreCase = true)) {
            names.add("$normalized Analog")
        }
        return names.toList()
    }

    private fun parseRenderer(value: String): VideoRenderer? {
        return when (value.trim().lowercase(Locale.US)) {
            "software", "soft" -> VideoRenderer.SOFTWARE
            "opengl", "gl" -> VideoRenderer.OPENGL
            "vulkan", "vk" -> VideoRenderer.VULKAN
            "compute" -> VideoRenderer.COMPUTE
            else -> null
        }
    }

    private fun parseRuntimeConsole(value: String): RuntimeConsoleType? {
        return when (value.trim().lowercase(Locale.US)) {
            "default", "global" -> RuntimeConsoleType.DEFAULT
            "ds", "nds" -> RuntimeConsoleType.DS
            "dsi" -> RuntimeConsoleType.DSi
            else -> null
        }
    }

    private fun parseInput(value: String): Input? {
        val normalized = value
            .trim()
            .uppercase(Locale.US)
            .replace('-', '_')
            .replace(".", "_")
        if (normalized.isEmpty()) {
            return null
        }

        return when (normalized.removePrefix("INPUT_").removePrefix("BUTTON_")) {
            "A" -> Input.A
            "B" -> Input.B
            "X" -> Input.X
            "Y" -> Input.Y
            "L", "L1" -> Input.L
            "R", "R1" -> Input.R
            "START" -> Input.START
            "SELECT" -> Input.SELECT
            "LEFT", "DPAD_LEFT", "HAT_LEFT", "AXIS_HAT_X_LEFT", "AXIS_HAT_X_NEGATIVE", "AXIS_X_LEFT", "AXIS_X_NEGATIVE" -> Input.LEFT
            "RIGHT", "DPAD_RIGHT", "HAT_RIGHT", "AXIS_HAT_X_RIGHT", "AXIS_HAT_X_POSITIVE", "AXIS_X_RIGHT", "AXIS_X_POSITIVE" -> Input.RIGHT
            "UP", "DPAD_UP", "HAT_UP", "AXIS_HAT_Y_UP", "AXIS_HAT_Y_NEGATIVE", "AXIS_Y_UP", "AXIS_Y_NEGATIVE" -> Input.UP
            "DOWN", "DPAD_DOWN", "HAT_DOWN", "AXIS_HAT_Y_DOWN", "AXIS_HAT_Y_POSITIVE", "AXIS_Y_DOWN", "AXIS_Y_POSITIVE" -> Input.DOWN
            else -> null
        }
    }

    private fun applyPauseAfterReady(pauseAfterReady: Boolean) {
        if (pauseAfterReady) {
            DebugCommandStateStore.setDebugPauseHeld(true)
            MelonEmulator.pauseEmulation()
        } else {
            DebugCommandStateStore.setDebugPauseHeld(false)
            MelonEmulator.resumeEmulation()
        }
    }

    private fun readBooleanSystemProperty(key: String): Boolean {
        return try {
            val process = ProcessBuilder(GETPROP_BINARY, key)
                .redirectErrorStream(true)
                .start()
            val value = process.inputStream.bufferedReader().use { it.readText().trim() }
            process.waitFor()
            when (value.lowercase(Locale.US)) {
                "1", "true", "on", "yes", "enabled" -> true
                else -> false
            }
        } catch (error: Exception) {
            Log.w(TAG, "Failed to read system property key=$key", error)
            false
        }
    }

    private fun parseUri(pathOrUri: String): Uri {
        val file = File(pathOrUri)
        return if (file.isAbsolute) {
            Uri.fromFile(file)
        } else {
            Uri.parse(pathOrUri)
        }
    }

    private fun Intent.firstStringExtra(vararg keys: String): String? {
        return keys.firstNotNullOfOrNull { key ->
            getStringExtra(key)?.takeIf { value -> value.isNotBlank() }
        }
    }

    @Suppress("DEPRECATION")
    private fun Intent.firstNullableIntExtra(vararg keys: String): Int? {
        keys.forEach { key ->
            if (!hasExtra(key)) {
                return@forEach
            }

            val raw = extras?.get(key)
            when (raw) {
                is Int -> return raw
                is String -> raw.toIntOrNull()?.let { return it }
            }
        }

        return null
    }

    private fun Intent.firstBooleanExtra(vararg keys: String): Boolean? {
        keys.forEach { key ->
            if (!hasExtra(key)) {
                return@forEach
            }

            val raw = extras?.get(key)
            when (raw) {
                is Boolean -> return raw
                is String -> when (raw.trim().lowercase(Locale.US)) {
                    "1", "true", "on", "yes", "enabled" -> return true
                    "0", "false", "off", "no", "disabled" -> return false
                }
                is Int -> return raw != 0
            }
        }

        return null
    }

    private fun Intent.firstNullableFloatExtra(vararg keys: String): Float? {
        keys.forEach { key ->
            if (!hasExtra(key)) {
                return@forEach
            }

            val raw = extras?.get(key)
            when (raw) {
                is Float -> return raw
                is Double -> return raw.toFloat()
                is Int -> return raw.toFloat()
                is String -> raw.toFloatOrNull()?.let { return it }
            }
        }

        return null
    }

    private suspend fun waitForRendererFrameOrTimeout(
        renderer: VideoRenderer,
        startFrame: Int,
        resumeFrames: Int,
        timeoutMs: Long,
    ) {
        val effectiveTimeoutMs = when {
            timeoutMs > 0L -> timeoutMs
            resumeFrames > 0 -> 5_000L
            else -> 0L
        }
        if (effectiveTimeoutMs <= 0L) {
            return
        }

        val targetFrame = if (resumeFrames > 0 && startFrame >= 0) {
            startFrame + resumeFrames
        } else {
            Int.MIN_VALUE
        }
        if (targetFrame == Int.MIN_VALUE) {
            delay(effectiveTimeoutMs)
            return
        }

        val deadlineAt = System.nanoTime() + effectiveTimeoutMs * 1_000_000L
        while (System.nanoTime() < deadlineAt) {
            val currentFrame = RendererDebugBridge.getCurrentFrameIndexForDebug()
            val rendererReady = renderer != VideoRenderer.VULKAN || RendererDebugBridge.isCurrentFrameReadyForDebug()
            if (currentFrame >= targetFrame && rendererReady) {
                return
            }
            delay(8L)
        }
    }

    private suspend fun waitForRendererReadyOrTimeout(renderer: VideoRenderer, timeoutMs: Long) {
        waitForRendererReadyOrTimeout(
            renderer = renderer,
            minFrame = RendererDebugBridge.getCurrentFrameIndexForDebug(),
            timeoutMs = timeoutMs,
        )
    }

    private suspend fun waitForRendererReadyOrTimeout(
        renderer: VideoRenderer,
        minFrame: Int,
        timeoutMs: Long,
    ) {
        if (renderer != VideoRenderer.VULKAN) {
            return
        }

        val deadlineAt = System.nanoTime() + timeoutMs.coerceAtLeast(1L) * 1_000_000L
        while (System.nanoTime() < deadlineAt) {
            if (RendererDebugBridge.getCurrentFrameIndexForDebug() >= minFrame
                && RendererDebugBridge.isCurrentFrameReadyForDebug()
            ) {
                return
            }
            delay(8L)
        }
    }

    private fun parseCaptureKinds(
        rawKinds: String?,
        defaultKinds: Set<RendererDebugCaptureKind>,
    ): Set<RendererDebugCaptureKind> {
        val value = rawKinds?.trim().orEmpty()
        if (value.isEmpty()) {
            return defaultKinds
        }

        val parsed = LinkedHashSet<RendererDebugCaptureKind>()
        value.split(',')
            .map { it.trim().lowercase(Locale.US) }
            .filter { it.isNotEmpty() }
            .forEach { token ->
                when (token) {
                    "all" -> parsed.addAll(RendererDebugCaptureKind.allKinds)
                    "vulkanexact", "vulkan_exact", "vulkan-exact", "exactframe", "exact_frame", "exact-frame" ->
                        parsed.addAll(RendererDebugCapturePresets.vulkanExactFrame)
                    "screen", "screenframe" -> parsed.add(RendererDebugCaptureKind.SCREEN_FRAME)
                    "packed" -> {
                        parsed.add(RendererDebugCaptureKind.PACKED_TOP_PRIMARY)
                        parsed.add(RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY)
                    }
                    "packedtop", "packed_top", "packedtopprimary" -> parsed.add(RendererDebugCaptureKind.PACKED_TOP_PRIMARY)
                    "packedbottom", "packed_bottom", "packedbottomprimary" -> parsed.add(RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY)
                    "packedtopplane1", "packed_top_plane1" -> parsed.add(RendererDebugCaptureKind.PACKED_TOP_PLANE1)
                    "packedtopcontrol", "packed_top_control" -> parsed.add(RendererDebugCaptureKind.PACKED_TOP_CONTROL)
                    "packedbottomplane1", "packed_bottom_plane1" -> parsed.add(RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1)
                    "packedbottomcontrol", "packed_bottom_control" -> parsed.add(RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL)
                    "capture3dsource", "capture3dsourceds", "capture3dsourceframe" ->
                        parsed.add(RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME)
                    "capturelinemask", "capturelineuses3dmask", "capture_line_uses_3d_mask" ->
                        parsed.add(RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK)
                    "comp4top", "comp4_top", "comp4topplaceholder", "comp4_top_placeholder" ->
                        parsed.add(RendererDebugCaptureKind.COMP4_TOP_PLACEHOLDER)
                    "comp4bottom", "comp4_bottom", "comp4bottomplaceholder", "comp4_bottom_placeholder" ->
                        parsed.add(RendererDebugCaptureKind.COMP4_BOTTOM_PLACEHOLDER)
                    "capturefallback", "capturefallbackmask", "capture_fallback_mask", "fallbackmask" ->
                        parsed.add(RendererDebugCaptureKind.CAPTURE_FALLBACK_MASK)
                    "softpackedmeta", "softpackedframemeta", "soft_packed_frame_meta", "softpackedframejson" ->
                        parsed.add(RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON)
                    "faithfulpayload", "faithfuldiagnosticpayload", "faithful_diagnostic_payload" ->
                        parsed.add(RendererDebugCaptureKind.FAITHFUL_DIAGNOSTIC_PAYLOAD)
                    "composited", "compositedframe", "composited_frame", "vulkancomposited", "vulkan_composited_frame" ->
                        parsed.add(RendererDebugCaptureKind.COMPOSITED_FRAME)
                    "renderer3d", "3d", "renderer3dframe" -> parsed.add(RendererDebugCaptureKind.RENDERER3D_FRAME)
                    "keepdark", "negros" -> parsed.add(RendererDebugCaptureKind.KEEP_DARK)
                    "capture3d", "3dcapture", "renderer3dcapture", "renderer3dcaptureframe" ->
                        parsed.add(RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME)
                    "depth", "renderer3ddepth" -> parsed.add(RendererDebugCaptureKind.RENDERER3D_DEPTH)
                    "attr", "attributes", "renderer3dattr" -> parsed.add(RendererDebugCaptureKind.RENDERER3D_ATTR)
                    "coverage", "renderer3dcoverage" -> parsed.add(RendererDebugCaptureKind.RENDERER3D_COVERAGE)
                    else -> throw IllegalArgumentException("Unsupported capture kind=$token")
                }
            }
        return if (parsed.isEmpty()) defaultKinds else parsed
    }

    private companion object {
        private const val TAG = "DebugCommand"
        private const val KEY_RENDERER_DEBUG_TOOLS_ENABLED = "video_renderer_debug_tools_enabled"
        private const val KEY_VIDEO_RENDERER = "video_renderer"
        private const val KEY_VULKAN_FASTPATH_ENABLED = "video_vulkan_fastpath_enabled"
        private const val KEY_VIDEO_INTERNAL_RESOLUTION = "video_internal_resolution"
        private const val KEY_FRAME_LIMIT_SPEED_MULTIPLIER = "frame_limit_speed_multiplier"
        private const val KEY_ENABLE_JIT = "enable_jit"
        private const val KEY_FRAMESKIP_MODE = "frameskip_mode"
        private const val KEY_FRAMESKIP_MANUAL_VALUE = "frameskip_manual_value"
        private const val KEY_MUTE_ON_FAST_FORWARD = "audio_mute_on_fast_forward"
        private const val EXTRA_MODE = "mode"
        private const val EXTRA_MANUAL_VALUE = "manual_value"
        private const val KEY_RENDERER_DEBUG_BGOBJ_ENABLED = "video_renderer_debug_bgobj_enabled"
        private const val KEY_RENDERER_DEBUG_LATCH_TRACE_ENABLED =
            "video_renderer_debug_latch_trace_enabled"
        private const val KEY_RA_ENABLED = "ra_enabled"
        private const val KEY_RA_HARDCORE_ENABLED = "ra_hardcore_enabled"
        private const val RELEASE_STATE_COMMANDS_PROPERTY = "debug.melonds.release_state_commands"
        private const val GETPROP_BINARY = "/system/bin/getprop"

        private const val EXTRA_RENDERER = "renderer"
        private const val EXTRA_FASTPATH_ENABLED = "fastpath_enabled"
        private const val EXTRA_FASTPATH = "fastpath"
        private const val EXTRA_SCALE = "scale"
        private const val EXTRA_IR = "ir"
        private const val EXTRA_RUNTIME_CONSOLE = "runtime_console"
        private const val EXTRA_CONSOLE = "console"
        private const val EXTRA_ENABLED = "enabled"
        private const val EXTRA_LATCH_TRACE_ENABLED = "latch_trace_enabled"
        private const val EXTRA_RA_ENABLED = "ra_enabled"
        private const val EXTRA_RA_HARDCORE_ENABLED = "ra_hardcore_enabled"
        private const val EXTRA_HARDCORE_ENABLED = "hardcore_enabled"
        private const val EXTRA_HARDCORE = "hardcore"
        private const val EXTRA_MULTIPLIER = "multiplier"
        private const val EXTRA_SPEED = "speed"
        private const val EXTRA_X = "x"
        private const val EXTRA_Y = "y"
        private const val EXTRA_ANCHOR_FRAME = "anchor_frame"
        private const val EXTRA_VALUE_X = "value_x"
        private const val EXTRA_VALUE_Y = "value_y"
        private const val EXTRA_DURATION_MS = "duration_ms"
        private const val EXTRA_GAP_MS = "gap_ms"
        private const val EXTRA_DELAY_MS = "delay_ms"
        private const val EXTRA_INPUT = "input"
        private const val EXTRA_INPUTS = "inputs"
        private const val EXTRA_HELD = "held"
        private const val EXTRA_DOWN = "down"
        private const val EXTRA_PRESSED = "pressed"
        private const val EXTRA_REPEAT = "repeat"
        private const val EXTRA_COUNT = "count"
        private const val EXTRA_SLOT = "slot"
        private const val EXTRA_PATH = "path"
        private const val EXTRA_URI = "uri"
        private const val EXTRA_ROM_URI = "rom_uri"
        private const val EXTRA_QUERY = "query"
        private const val EXTRA_WAIT_ROM_READY = "wait_rom_ready"
        private const val EXTRA_WAIT_READY = "wait_ready"
        private const val EXTRA_WAIT_TIMEOUT_MS = "wait_timeout_ms"
        private const val EXTRA_PAUSE_AFTER = "pause_after"
        private const val EXTRA_VALUE = "value"
        private const val EXTRA_PAUSED = "paused"
        private const val EXTRA_FRAMES = "frames"
        private const val EXTRA_STEP_FRAMES = "step_frames"
        private const val EXTRA_RESUME_MS = "resume_ms"
        private const val EXTRA_RESUME_FRAMES = "resume_frames"
        private const val EXTRA_CAPTURE_KINDS = "capture_kinds"
        private const val EXTRA_KINDS = "kinds"
        private const val EXTRA_CAPTURE_ID = "capture_id"
        private const val EXTRA_CAPTURE_ID_BASE = "capture_id_base"
        private const val EXTRA_OUTPUT_ID = "output_id"
        private const val EXTRA_BURST_COUNT = "burst_count"
        private const val EXTRA_BURST_FF_TOGGLE_AT = "burst_ff_toggle_at"
        private const val EXTRA_BURST_FF_TOGGLE_ENABLED = "burst_ff_toggle_enabled"
        private const val EXTRA_CAPTURE_COUNT = "capture_count"
        private const val EXTRA_BURST_STEP_FRAMES = "burst_step_frames"
        private const val EXTRA_TIMEOUT_MS = "timeout_ms"
        private const val EXTRA_BURST_LIVE = "burst_live"
        private const val EXTRA_LIVE_BURST = "live_burst"
        private const val EXTRA_REFRESH = "refresh"
        private const val EXTRA_REFRESH_SETTINGS = "refresh_settings"
        private const val EXTRA_FEATURE_MASK = "feature_mask"
        private const val EXTRA_MAIN_FORCED_MODE = "main_forced_mode"
        private const val EXTRA_SUB_FORCED_MODE = "sub_forced_mode"
        private const val EXTRA_TOP_FORCED_COMP_MODE = "top_forced_comp_mode"
        private const val EXTRA_BOTTOM_FORCED_COMP_MODE = "bottom_forced_comp_mode"
        private const val EXTRA_DISABLED_MAIN_BG_MASK = "disabled_main_bg_mask"
        private const val EXTRA_DISABLED_SUB_BG_MASK = "disabled_sub_bg_mask"
        private const val EXTRA_DISABLED_MAIN_BG_PRIORITY_MASK = "disabled_main_bg_priority_mask"
        private const val EXTRA_DISABLED_SUB_BG_PRIORITY_MASK = "disabled_sub_bg_priority_mask"
        private const val EXTRA_DISABLED_MAIN_OBJ_PRIORITY_MASK = "disabled_main_obj_priority_mask"
        private const val EXTRA_DISABLED_SUB_OBJ_PRIORITY_MASK = "disabled_sub_obj_priority_mask"
        private const val EXTRA_DISABLED_MAIN_OBJ_ORDER_MASK = "disabled_main_obj_order_mask"
        private const val EXTRA_DISABLED_SUB_OBJ_ORDER_MASK = "disabled_sub_obj_order_mask"

        private const val RENDERER_2D_NATIVE_MODE = -1
        private const val RENDERER_2D_STATE_SIZE = 13
        private const val RENDERER_2D_STATE_MAIN_FORCED_MODE_INDEX = 0
        private const val RENDERER_2D_STATE_SUB_FORCED_MODE_INDEX = 1
        private const val RENDERER_2D_STATE_TOP_FORCED_COMP_MODE_INDEX = 2
        private const val RENDERER_2D_STATE_BOTTOM_FORCED_COMP_MODE_INDEX = 3
        private const val RENDERER_2D_STATE_DISABLED_MAIN_BG_MASK_INDEX = 4
        private const val RENDERER_2D_STATE_DISABLED_SUB_BG_MASK_INDEX = 5
        private const val RENDERER_2D_STATE_DISABLED_MAIN_BG_PRIORITY_MASK_INDEX = 6
        private const val RENDERER_2D_STATE_DISABLED_SUB_BG_PRIORITY_MASK_INDEX = 7
        private const val RENDERER_2D_STATE_DISABLED_MAIN_OBJ_PRIORITY_MASK_INDEX = 8
        private const val RENDERER_2D_STATE_DISABLED_SUB_OBJ_PRIORITY_MASK_INDEX = 9
        private const val RENDERER_2D_STATE_DISABLED_MAIN_OBJ_ORDER_MASK_INDEX = 10
        private const val RENDERER_2D_STATE_DISABLED_SUB_OBJ_ORDER_MASK_INDEX = 11
        private const val RENDERER_2D_STATE_FEATURE_MASK_INDEX = 12

        private const val ROM_URI_RESOLVE_TIMEOUT_MS = 4_000L
        private const val ROM_URI_RESOLVE_STEP_MS = 100L
        private const val DEFAULT_ROM_READY_TIMEOUT_MS = 8_000
        private const val MAX_RECEIVER_WAIT_TIMEOUT_MS = 8_000
        private const val LAUNCH_ACTIVITY_SEEN_TIMEOUT_MS = 2_000L
        private const val REQUEST_CODE_LAUNCH_ROM = 1
        private const val DEFAULT_TOUCH_X = 128
        private const val DEFAULT_TOUCH_Y = 96
        private const val DEFAULT_TOUCH_DURATION_MS = 80
        private const val DEFAULT_INPUT_DURATION_MS = 80
        private const val DEFAULT_INPUT_GAP_MS = 80
        private const val DEFAULT_AUDIO_PCM_CAPTURE_DURATION_MS = 20_000
        private const val MAX_AUDIO_PCM_CAPTURE_DURATION_MS = 60_000
        private const val MAX_AUDIO_PCM_CAPTURE_ID_LENGTH = 80
        private const val AUDIO_PCM_CAPTURE_DIRECTORY = "audio-pcm-captures"
        private const val SHELL_PACKAGE_NAME = "com.android.shell"

        private val receiverScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        private var activeAudioPcmCaptureId: String? = null

        private const val ACTION_SET_RENDERER_SUFFIX = "SET_RENDERER"
        private const val ACTION_SET_RENDERER_DEBUG_TOOLS_SUFFIX = "SET_RENDERER_DEBUG_TOOLS"
        private const val ACTION_SET_IR_SUFFIX = "SET_IR"
        private const val ACTION_SET_ROM_RUNTIME_CONSOLE_SUFFIX = "SET_ROM_RUNTIME_CONSOLE"
        private const val ACTION_SET_RETROACHIEVEMENTS_SUFFIX = "SET_RETROACHIEVEMENTS"
        private const val ACTION_SET_FAST_FORWARD_SUFFIX = "SET_FAST_FORWARD"
        private const val ACTION_SET_FRAME_LIMIT_SPEED_SUFFIX = "SET_FRAME_LIMIT_SPEED"
        private const val ACTION_SET_JIT_SUFFIX = "SET_JIT"
        private const val ACTION_SET_FRAMESKIP_SUFFIX = "SET_FRAMESKIP"
        private const val ACTION_SET_MUTE_ON_FAST_FORWARD_SUFFIX = "SET_MUTE_ON_FAST_FORWARD"
        private const val ACTION_GET_FPS_SUFFIX = "GET_FPS"
        private const val ACTION_MULTIPLAYER_STATUS_SUFFIX = "MULTIPLAYER_STATUS"
        private const val ACTION_START_AUDIO_PCM_CAPTURE_SUFFIX = "START_AUDIO_PCM_CAPTURE"
        private const val ACTION_DUMP_AUDIO_PCM_CAPTURE_SUFFIX = "DUMP_AUDIO_PCM_CAPTURE"
        private const val ACTION_GRANT_AUDIO_PCM_CAPTURE_READ_SUFFIX = "GRANT_AUDIO_PCM_CAPTURE_READ"
        private const val ACTION_SET_BGOBJ_LOG_SUFFIX = "SET_BGOBJ_LOG"
        private const val ACTION_SET_RENDERER_2D_DEBUG_CONTROLS_SUFFIX = "SET_RENDERER_2D_DEBUG_CONTROLS"
        private const val ACTION_SET_RENDERER_3D_DEBUG_CONTROLS_SUFFIX = "SET_RENDERER_3D_DEBUG_CONTROLS"
        private const val ACTION_LAUNCH_ROM_SUFFIX = "LAUNCH_ROM"
        private const val ACTION_WAIT_ROM_READY_SUFFIX = "WAIT_ROM_READY"
        private const val ACTION_SET_DEBUG_PAUSE_SUFFIX = "SET_DEBUG_PAUSE"
        private const val ACTION_STEP_FRAME_SUFFIX = "STEP_FRAME"
        private const val ACTION_STEP_FRAMES_SUFFIX = "STEP_FRAMES"
        private const val ACTION_DUMP_RENDERER_CAPTURE_SUFFIX = "DUMP_RENDERER_CAPTURE"
        private const val ACTION_ARM_EXACT_LIVE_GUIDE_SUFFIX = "ARM_EXACT_LIVE_GUIDE"
        private const val ACTION_GET_EXACT_LIVE_GUIDE_SUFFIX = "GET_EXACT_LIVE_GUIDE"
        private const val ACTION_ABORT_EXACT_LIVE_GUIDE_SUFFIX = "ABORT_EXACT_LIVE_GUIDE"
        private const val ACTION_TOUCH_SCREEN_SUFFIX = "TOUCH_SCREEN"
        private const val ACTION_PRESS_INPUT_SUFFIX = "PRESS_INPUT"
        private const val ACTION_SET_INPUT_HELD_SUFFIX = "SET_INPUT_HELD"
        private const val ACTION_SAVE_STATE_SUFFIX = "SAVE_STATE"
        private const val ACTION_LOAD_STATE_SUFFIX = "LOAD_STATE"
        private const val ACTION_DUMP_ROM_SEARCH_STATE_SUFFIX = "DUMP_ROM_SEARCH_STATE"
    }

    private fun Context.debugCommandAction(suffix: String): String {
        return "$packageName.$suffix"
    }

    private fun isConfigurationAction(context: Context, action: String?): Boolean {
        return action == context.debugCommandAction(ACTION_SET_RENDERER_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_RENDERER_DEBUG_TOOLS_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_IR_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_ROM_RUNTIME_CONSOLE_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_RETROACHIEVEMENTS_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_FAST_FORWARD_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_FRAME_LIMIT_SPEED_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_JIT_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_FRAMESKIP_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_MUTE_ON_FAST_FORWARD_SUFFIX)
            || action == context.debugCommandAction(ACTION_GET_FPS_SUFFIX)
            || action == context.debugCommandAction(ACTION_MULTIPLAYER_STATUS_SUFFIX)
            || action == context.debugCommandAction(ACTION_START_AUDIO_PCM_CAPTURE_SUFFIX)
            || action == context.debugCommandAction(ACTION_DUMP_AUDIO_PCM_CAPTURE_SUFFIX)
            || action == context.debugCommandAction(ACTION_GRANT_AUDIO_PCM_CAPTURE_READ_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_BGOBJ_LOG_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_RENDERER_2D_DEBUG_CONTROLS_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_RENDERER_3D_DEBUG_CONTROLS_SUFFIX)
            || action == context.debugCommandAction(ACTION_LAUNCH_ROM_SUFFIX)
            || action == context.debugCommandAction(ACTION_WAIT_ROM_READY_SUFFIX)
            || action == context.debugCommandAction(ACTION_SET_DEBUG_PAUSE_SUFFIX)
            || action == context.debugCommandAction(ACTION_STEP_FRAME_SUFFIX)
            || action == context.debugCommandAction(ACTION_STEP_FRAMES_SUFFIX)
            || action == context.debugCommandAction(ACTION_DUMP_RENDERER_CAPTURE_SUFFIX)
            || action == context.debugCommandAction(ACTION_ARM_EXACT_LIVE_GUIDE_SUFFIX)
            || action == context.debugCommandAction(ACTION_GET_EXACT_LIVE_GUIDE_SUFFIX)
            || action == context.debugCommandAction(ACTION_ABORT_EXACT_LIVE_GUIDE_SUFFIX)
    }
}
