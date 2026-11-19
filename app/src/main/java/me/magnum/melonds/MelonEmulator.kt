package me.magnum.melonds

import android.graphics.Bitmap
import android.net.Uri
import android.view.Surface
import me.magnum.melonds.common.camera.DSiCameraSource
import me.magnum.melonds.domain.model.Cheat
import me.magnum.melonds.domain.model.EmulatorConfiguration
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.Input
import me.magnum.melonds.domain.model.retroachievements.RASimpleAchievement
import me.magnum.melonds.domain.model.retroachievements.RASimpleLeaderboard
import me.magnum.melonds.domain.model.retroachievements.RARuntimeBridgeConfig
import me.magnum.melonds.domain.model.retroachievements.RASimpleRuntimeAchievement
import me.magnum.melonds.domain.model.retroachievements.RASimpleRuntimeAchievementBucketEntry
import me.magnum.melonds.ui.emulator.render.FrameRenderCallback
import me.magnum.melonds.ui.emulator.model.VulkanPresentationConfig
import me.magnum.melonds.ui.emulator.rewind.model.RewindSaveState
import me.magnum.melonds.ui.emulator.rewind.model.RewindWindow
import java.nio.ByteBuffer

object MelonEmulator {
    enum class LoadResult(val isTerminal: Boolean) {
        SUCCESS(false),
        SUCCESS_GBA_FAILED(false),
        NDS_FAILED(true),
        BIOS_FAILED(true)
    }

    enum class FirmwareLoadResult {
        SUCCESS,
        BIOS9_MISSING,
        BIOS9_BAD,
        BIOS7_MISSING,
        BIOS7_BAD,
        FIRMWARE_MISSING,
        FIRMWARE_BAD,
        FIRMWARE_NOT_BOOTABLE,
        DSI_BIOS9_MISSING,
        DSI_BIOS9_BAD,
        DSI_BIOS7_MISSING,
        DSI_BIOS7_BAD,
        DSI_NAND_MISSING,
        DSI_NAND_BAD
    }

    enum class GbaSlotType {
        NONE,
        GBA_ROM,
        RUMBLE_PAK,
        MEMORY_EXPANSION,
        ANALOG_INPUT,
        MOTION_PAK_HOMEBREW,
        MOTION_PAK_RETAIL,
    }

    enum class VulkanPresentationResult(val nativeValue: Int) {
        PRESENTED(0),
        NO_SURFACE(1),
        NO_PRODUCT(2),
        GPU_NOT_READY(3),
        WSI_NOT_READY(4),
        GENERATION_CHANGED(5),
        STOPPED(6),
        RECOVERABLE_SURFACE_ERROR(7),
        FATAL_ERROR(8);

        companion object {
            fun fromNative(nativeValue: Int): VulkanPresentationResult =
                when (nativeValue) {
                    0 -> PRESENTED
                    1 -> NO_SURFACE
                    2 -> NO_PRODUCT
                    3 -> GPU_NOT_READY
                    4 -> WSI_NOT_READY
                    5 -> GENERATION_CHANGED
                    6 -> STOPPED
                    7 -> RECOVERABLE_SURFACE_ERROR
                    8 -> FATAL_ERROR
                    else -> FATAL_ERROR
                }
        }
    }

    enum class VulkanPresentationWaitResult {
        PRODUCT_READY,
        TIMED_OUT,
        GENERATION_CHANGED,
        STOPPED;

        companion object {
            fun fromNative(nativeValue: Int): VulkanPresentationWaitResult =
                when (nativeValue) {
                    0 -> PRODUCT_READY
                    1 -> TIMED_OUT
                    2 -> GENERATION_CHANGED
                    3 -> STOPPED
                    else -> STOPPED
                }
        }
    }

    @Volatile
    var isLowLatencyEnabled: Boolean = false
        private set

    fun setupEmulator(
        emulatorConfiguration: EmulatorConfiguration,
        dsiCameraSource: DSiCameraSource?,
        screenshotBuffer: ByteBuffer,
    ) {
        val configuration = emulatorConfiguration.copy(
            lowLatencyEnabled = emulatorConfiguration.lowLatencyEnabled
                && emulatorConfiguration.rendererConfiguration.renderer == VideoRenderer.VULKAN,
        )
        setupEmulatorInternal(configuration, dsiCameraSource, screenshotBuffer)
        isLowLatencyEnabled = configuration.lowLatencyEnabled
    }

    private external fun setupEmulatorInternal(
        emulatorConfiguration: EmulatorConfiguration,
        dsiCameraSource: DSiCameraSource?,
        screenshotBuffer: ByteBuffer,
    )

    external fun setupCheats(cheats: Array<Cheat>)

    external fun setupAchievements(
        achievements: Array<RASimpleAchievement>,
        leaderboards: Array<RASimpleLeaderboard>,
        richPresenceScript: String?,
        runtimeConfig: RARuntimeBridgeConfig?,
    ): Boolean

    external fun unloadRetroAchievementsData()

    external fun getRichPresenceStatus(): String?

    external fun getRuntimeAchievements(): Array<RASimpleRuntimeAchievement>

    external fun getRuntimeAchievementBuckets(): Array<RASimpleRuntimeAchievementBucketEntry>

    external fun getRuntimeSubsetIds(): LongArray

    external fun getRetroAchievementsSetupFailureReason(): Int

    external fun retryPendingRetroAchievementsSubmissions(
        expectedNativeSubmissionIds: LongArray,
    ): LongArray?

    external fun refreshPendingRetroAchievementsSubmissions(): Long

    external fun discardPendingRetroAchievementsSubmissions(
        expectedNativeSubmissionIds: LongArray,
    ): Int

    external fun setRetroAchievementsSubmissionTransportSuspended(suspended: Boolean)

    fun loadRom(romUri: Uri, sramUri: Uri, gbaSlotType: GbaSlotType, gbaRomUri: Uri?, gbaSramUri: Uri?): LoadResult {
        val loadResult = loadRomInternal(romUri.toString(), sramUri.toString(), gbaSlotType.ordinal, gbaRomUri?.toString(), gbaSramUri?.toString())
        return when (loadResult) {
            0 -> LoadResult.SUCCESS
            1 -> LoadResult.SUCCESS_GBA_FAILED
            2 -> LoadResult.NDS_FAILED
            3 -> LoadResult.BIOS_FAILED
            else -> throw RuntimeException("Unknown load result")
        }
    }

    fun bootFirmware(): FirmwareLoadResult {
        val loadResult = bootFirmwareInternal()
        return FirmwareLoadResult.entries[loadResult]
    }

    private external fun loadRomInternal(romPath: String, sramPath: String, gbaSlotType: Int, gbaRomPath: String?, gbaSramPath: String?): Int

    private external fun bootFirmwareInternal(): Int

	external fun startEmulation(startPaused: Boolean)
    external fun startAudioOutputPcmCapture(durationMs: Int): String?
    external fun dumpAudioOutputPcmCapture(finalDirectory: String): String?
    external fun precompileVulkanPipelines(
        videoFilteringOrdinal: Int,
        retroShaderPresetPath: String?,
        retroShaderSourceResolution: String,
        retroShaderPassCount: Int,
        retroShaderParameterOverrides: Map<String, Float>,
    ): Boolean

    external fun configureOpenGlRetroArchFilter(
        enabled: Boolean,
        presetPath: String?,
        parameterOverrides: String?,
        clearHistory: Boolean,
        sourceResolution: String,
        maxLayoutWidth: Int,
        maxLayoutHeight: Int,
        passCount: Int,
    )

    external fun prewarmOpenGlRetroArchFilter(atlasWidth: Int, atlasHeight: Int): Boolean

    external fun releaseOpenGlRetroArchFilter()

    external fun consumeShaderDiagnostics(): Array<String>?

    external fun presentFrame(deadlineNs: Long, frameRenderCallback: FrameRenderCallback)
    external fun attachVulkanSurface(surface: Surface, width: Int, height: Int): Int
    external fun resizeVulkanSurface(surfaceId: Int, width: Int, height: Int)
    external fun configureVulkanSurface(surfaceId: Int, presentationConfig: VulkanPresentationConfig, backgroundBitmap: Bitmap?)
    external fun detachVulkanSurface(surfaceId: Int)
    private external fun presentVulkanFrameNative(
        deadlineNs: Long,
        budgetDeadlineNs: Long,
        expectedWaitEpoch: Long,
    ): Int
    private external fun captureVulkanPresentationWaitEpochNative(): Long
    private external fun waitForVulkanPresentationProductNative(
        expectedWaitEpoch: Long,
        timeoutNs: Long,
    ): Int
    private external fun cancelVulkanPresentationWaitsNative()

    fun presentVulkanFrame(
        deadlineNs: Long,
        budgetDeadlineNs: Long,
        expectedWaitEpoch: Long,
    ): VulkanPresentationResult = VulkanPresentationResult.fromNative(
        presentVulkanFrameNative(
            deadlineNs,
            budgetDeadlineNs,
            expectedWaitEpoch,
        ),
    )

    fun captureVulkanPresentationWaitEpoch(): Long =
        captureVulkanPresentationWaitEpochNative()

    fun waitForVulkanPresentationProduct(
        expectedWaitEpoch: Long,
        timeoutNs: Long,
    ): VulkanPresentationWaitResult = VulkanPresentationWaitResult.fromNative(
        waitForVulkanPresentationProductNative(expectedWaitEpoch, timeoutNs),
    )

    fun cancelVulkanPresentationWaits() {
        cancelVulkanPresentationWaitsNative()
    }

	external fun getFPS(): Float

    external fun getVulkanFrameskipStats(): String

    external fun getCurrentRenderer(): Int

	external fun pauseEmulation()

	external fun resumeEmulation()

    external fun requestRtcSync()

    external fun debugStepFrame(): Boolean

    external fun debugStepFrames(frames: Int): Boolean

    external fun resetEmulation()

	external fun stopEmulation()

    fun saveState(path: Uri): Boolean {
        return saveStateInternal(path.toString())
    }

    private external fun saveStateInternal(path: String): Boolean

    fun loadState(path: Uri): Boolean {
        return loadStateInternal(path.toString())
    }

    private external fun loadStateInternal(path: String): Boolean

    external fun loadRewindState(rewindSaveState: RewindSaveState): Boolean

    external fun getRewindWindow(): RewindWindow

	external fun onScreenTouch(x: Int, y: Int)

	external fun onScreenRelease()

    external fun armExactLiveGuide(anchorFrame: Long, x: Int, y: Int): String?

    external fun getExactLiveGuideStatus(): String?

    external fun abortExactLiveGuide(): String?

	fun onInputDown(input: Input) {
        onKeyPress(input.keyCode)
    }

	fun onInputUp(input: Input) {
        onKeyRelease(input.keyCode)
    }

    private external fun onKeyPress(key: Int)

    private external fun onKeyRelease(key: Int)

    external fun updateMotionData(ax: Float, ay: Float, az: Float, rx: Float, ry: Float, rz: Float)
    external fun setSlot2AnalogInput(x: Float, y: Float)

    external fun takeScreenshot(): IntArray?

    external fun setFastForwardEnabled(enabled: Boolean)

    external fun isFastForwardEnabled(): Boolean

    external fun setFrameLimitSpeedMultiplier(multiplier: Float)

    external fun setFrameskipMode(mode: Int, manualValue: Int)

    external fun setVulkanDrsEnabled(enabled: Boolean)

    external fun setMuteOnFastForward(enabled: Boolean)

    external fun getVulkanRenderedInternalResolution(): Int

    external fun setMicrophoneEnabled(enabled: Boolean)

    external fun updateEmulatorConfiguration(emulatorConfiguration: EmulatorConfiguration)
}
