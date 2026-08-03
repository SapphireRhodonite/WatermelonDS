package me.magnum.melonds.impl.emulator.debug

import android.graphics.Bitmap
import android.util.Log
import me.magnum.melonds.domain.model.VideoRenderer
import java.io.File
import java.io.FileOutputStream
import java.util.Locale
import java.util.zip.CRC32

private const val TAG = "RendererDebugCapture"
private const val CAPTURE_3D_LINE_WIDTH = 256
private const val CAPTURE_3D_LINE_HEIGHT = 192

internal enum class RendererDebugCaptureKind {
    SCREEN_FRAME,
    PACKED_TOP_PRIMARY,
    PACKED_BOTTOM_PRIMARY,
    PACKED_TOP_PLANE1,
    PACKED_TOP_CONTROL,
    PACKED_BOTTOM_PLANE1,
    PACKED_BOTTOM_CONTROL,
    CAPTURE3D_SOURCE_DS_FRAME,
    CAPTURE_LINE_USES_3D_MASK,
    COMP4_TOP_PLACEHOLDER,
    COMP4_BOTTOM_PLACEHOLDER,
    CAPTURE_FALLBACK_MASK,
    SOFT_PACKED_FRAME_META_JSON,
    COMPOSITED_FRAME,
    RENDERER3D_FRAME,
    RENDERER3D_CAPTURE_FRAME,
    RENDERER3D_DEPTH,
    RENDERER3D_ATTR,
    RENDERER3D_COVERAGE,
    ;

    companion object {
        val allKinds: Set<RendererDebugCaptureKind> = entries.toSet()
    }
}

internal object RendererDebugCapturePresets {
    val vulkanExactFrame: Set<RendererDebugCaptureKind> = linkedSetOf(
        RendererDebugCaptureKind.SCREEN_FRAME,
        RendererDebugCaptureKind.PACKED_TOP_PRIMARY,
        RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY,
        RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME,
        RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK,
        RendererDebugCaptureKind.COMP4_TOP_PLACEHOLDER,
        RendererDebugCaptureKind.COMP4_BOTTOM_PLACEHOLDER,
        RendererDebugCaptureKind.CAPTURE_FALLBACK_MASK,
        RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON,
        RendererDebugCaptureKind.COMPOSITED_FRAME,
        RendererDebugCaptureKind.RENDERER3D_FRAME,
        RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME,
    )
}

internal data class RendererDebugCaptureResult(
    val captureId: String,
    val success: Boolean,
    val outputDir: File? = null,
)

internal object RendererDebugCaptureLogger {
    suspend fun dumpDenseScreenBurstCapture(
        configuredRenderer: VideoRenderer,
        outputDir: File? = null,
        captureIdBase: String,
        burstCount: Int,
        burstStepFrames: Int,
        timeoutMs: Long,
        captureKinds: Set<RendererDebugCaptureKind> = setOf(RendererDebugCaptureKind.SCREEN_FRAME),
        warmupFrames: Int = 0,
        onCaptureArmed: (suspend () -> Unit)? = null,
    ): List<RendererDebugCaptureResult> {
        val safeBurstCount = burstCount.coerceAtLeast(1)
        val safeStepFrames = burstStepFrames.coerceAtLeast(1)
        val safeWarmupFrames = warmupFrames.coerceAtLeast(0)
        val requestedKinds = if (captureKinds.isEmpty()) {
            setOf(RendererDebugCaptureKind.SCREEN_FRAME)
        } else {
            captureKinds
        }
        var captureKindsMask = 0
        if (RendererDebugCaptureKind.SCREEN_FRAME in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_SCREEN_FRAME
        }
        if (RendererDebugCaptureKind.PACKED_TOP_PRIMARY in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_PACKED_TOP_PRIMARY
        }
        if (RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_PACKED_BOTTOM_PRIMARY
        }
        if (RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_RENDERER3D_CAPTURE_FRAME
        }
        if (RendererDebugCaptureKind.PACKED_TOP_PLANE1 in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_PACKED_TOP_PLANE1
        }
        if (RendererDebugCaptureKind.PACKED_TOP_CONTROL in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_PACKED_TOP_CONTROL
        }
        if (RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1 in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_PACKED_BOTTOM_PLANE1
        }
        if (RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_PACKED_BOTTOM_CONTROL
        }
        if (RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_CAPTURE3D_SOURCE
        }
        if (RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_CAPTURE_LINE_MASK
        }
        if (RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_SOFT_PACKED_META
        }
        if (RendererDebugCaptureKind.RENDERER3D_FRAME in requestedKinds) {
            captureKindsMask = captureKindsMask or RendererDebugBridge.DENSE_CAPTURE_RENDERER3D_FRAME
        }
        if (captureKindsMask == 0) {
            captureKindsMask = RendererDebugBridge.DENSE_CAPTURE_SCREEN_FRAME
        }
        val resolvedOutputDir = outputDir?.takeIf { directory ->
            directory.exists() || directory.mkdirs()
        }

        RendererDebugBridge.clearPreparedRendererSnapshot()
        RendererDebugBridge.clearDenseScreenBurstCapture()
        RendererDebugBridge.startDenseScreenBurstCapture(
            safeBurstCount,
            safeStepFrames,
            safeWarmupFrames,
            captureKindsMask,
        )
        onCaptureArmed?.invoke()

        val warmupTimeoutMs = if (safeWarmupFrames > 0) {
            (safeWarmupFrames.toLong() * 1_000L) / 24L + 5_000L
        } else {
            0L
        }
        val deadlineAt = System.nanoTime() +
            (timeoutMs.coerceAtLeast(1L) + warmupTimeoutMs) * 1_000_000L
        while (System.nanoTime() < deadlineAt) {
            if (RendererDebugBridge.isDenseScreenBurstCaptureComplete()) {
                break
            }
            kotlinx.coroutines.delay(8L)
        }

        val scheduleStats = RendererDebugBridge.getDenseScreenBurstScheduleStats()
        val warmupFramesRequested = scheduleStats?.getOrNull(0) ?: -1
        val warmupFramesObserved = scheduleStats?.getOrNull(1) ?: -1
        val eligibleCallbacksObserved = scheduleStats?.getOrNull(2) ?: -1
        val firstCaptureOrdinal = scheduleStats?.getOrNull(3) ?: -1
        val lastCaptureOrdinal = scheduleStats?.getOrNull(4) ?: -1
        val warmupSatisfied =
            warmupFramesRequested == safeWarmupFrames && warmupFramesObserved >= safeWarmupFrames
        val captureComplete = RendererDebugBridge.isDenseScreenBurstCaptureComplete()
        Log.w(
            TAG,
            "captureId=$captureIdBase source=dense_burst stage=summary requestedWarmupFrames=$warmupFramesRequested observedWarmupFrames=$warmupFramesObserved eligibleCallbacks=$eligibleCallbacksObserved firstCaptureOrdinal=$firstCaptureOrdinal lastCaptureOrdinal=$lastCaptureOrdinal warmupSatisfied=${if (warmupSatisfied) 1 else 0} complete=${if (captureComplete) 1 else 0}",
        )
        val availableFrameCount = RendererDebugBridge.getDenseScreenBurstCaptureFrameCount().coerceAtMost(safeBurstCount)
        val results = buildList {
            for (index in 0 until availableFrameCount) {
                val captureId = "${captureIdBase}_frame_${index.toString().padStart(4, '0')}"
                val frameId = RendererDebugBridge.getDenseScreenBurstCaptureFrameId(index)
                val frameReady = RendererDebugBridge.isCurrentFrameReadyForDebug()
                val screenFrame = if (RendererDebugCaptureKind.SCREEN_FRAME in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstCaptureFrame(index)
                } else {
                    null
                }
                val packedTopPrimary = if (RendererDebugCaptureKind.PACKED_TOP_PRIMARY in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstPackedTopFrame(index)
                } else {
                    null
                }
                val packedBottomPrimary = if (RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstPackedBottomFrame(index)
                } else {
                    null
                }
                val packedTopPlane1 = if (RendererDebugCaptureKind.PACKED_TOP_PLANE1 in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstPackedPlaneFrame(index, 0, 1)
                } else {
                    null
                }
                val packedTopControl = if (RendererDebugCaptureKind.PACKED_TOP_CONTROL in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstPackedPlaneFrame(index, 0, 2)
                } else {
                    null
                }
                val packedBottomPlane1 = if (RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1 in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstPackedPlaneFrame(index, 1, 1)
                } else {
                    null
                }
                val packedBottomControl = if (RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstPackedPlaneFrame(index, 1, 2)
                } else {
                    null
                }
                val capture3dSource = if (RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstCapture3dSourceFrame(index)
                } else {
                    null
                }
                val captureLineMask = if (RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstCaptureLineUses3dMaskFrame(index)
                } else {
                    null
                }
                val softPackedFrameMetaJson = if (RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstSoftPackedFrameMetaJson(index)
                } else {
                    null
                }
                val frame3d = if (RendererDebugCaptureKind.RENDERER3D_FRAME in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstRenderer3dFrame(index)
                } else {
                    null
                }
                val captureFrame3d = if (RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME in requestedKinds) {
                    RendererDebugBridge.getDenseScreenBurstRenderer3dCaptureFrame(index)
                } else {
                    null
                }
                Log.w(
                    TAG,
                    "captureId=$captureId stage=begin configuredRenderer=${configuredRenderer.name.lowercase(Locale.US)} frameId=$frameId frameReady=${if (frameReady) 1 else 0} freezeSnapshot=0 kinds=${requestedKinds.joinToString(separator = ",") { it.name.lowercase(Locale.US) }} source=dense_burst",
                )
                if (RendererDebugCaptureKind.SCREEN_FRAME in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "screenFrame",
                        width = RendererDebugBridge.CAPTURE_WIDTH,
                        height = RendererDebugBridge.CAPTURE_HEIGHT,
                        pixels = screenFrame,
                    )
                }
                if (RendererDebugCaptureKind.PACKED_TOP_PRIMARY in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "packedTopPrimary",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = packedTopPrimary,
                    )
                }
                if (RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "packedBottomPrimary",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = packedBottomPrimary,
                    )
                }
                if (RendererDebugCaptureKind.PACKED_TOP_PLANE1 in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "packedTopPlane1",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = packedTopPlane1,
                    )
                }
                if (RendererDebugCaptureKind.PACKED_TOP_CONTROL in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "packedTopControl",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = packedTopControl,
                    )
                }
                if (RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1 in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "packedBottomPlane1",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = packedBottomPlane1,
                    )
                }
                if (RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "packedBottomControl",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = packedBottomControl,
                    )
                }
                if (RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "capture3dSourceDsFrame",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = capture3dSource,
                    )
                }
                if (RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "captureLineUses3dMask",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = captureLineMask,
                    )
                }
                if (RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME in requestedKinds) {
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "renderer3dCaptureFrame",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = captureFrame3d,
                    )
                }
                if (RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON in requestedKinds) {
                    saveTextFile(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "softPackedFrameMeta",
                        contents = softPackedFrameMetaJson,
                        extension = "json",
                    )
                    Log.w(TAG, "captureId=$captureId kind=softPackedFrameMetaJson available=${if (softPackedFrameMetaJson.isNullOrBlank()) 0 else 1} length=${softPackedFrameMetaJson?.length ?: 0}")
                }
                if (RendererDebugCaptureKind.RENDERER3D_FRAME in requestedKinds) {
                    val renderer3dWidth = inferRenderer3dWidth(frame3d)
                    val renderer3dHeight = inferRenderer3dHeight(frame3d, renderer3dWidth)
                    saveFramePng(
                        outputDir = resolvedOutputDir,
                        captureId = captureId,
                        kind = "renderer3dFrame",
                        width = renderer3dWidth,
                        height = renderer3dHeight,
                        pixels = frame3d,
                    )
                }
                Log.w(
                    TAG,
                    "captureId=$captureId kind=meta screen=${describeBufferShape(RendererDebugBridge.CAPTURE_WIDTH, RendererDebugBridge.CAPTURE_HEIGHT, screenFrame)} packedTop=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedTopPrimary)} packedBottom=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedBottomPrimary)} packedTopPlane1=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedTopPlane1)} packedTopControl=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedTopControl)} packedBottomPlane1=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedBottomPlane1)} packedBottomControl=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedBottomControl)} capture3dSource=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, capture3dSource)} captureLineMask=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, captureLineMask)} softPackedMeta=${if (softPackedFrameMetaJson.isNullOrBlank()) 0 else 1} renderer3d=${describeBufferShape(inferRenderer3dWidth(frame3d), inferRenderer3dHeight(frame3d, inferRenderer3dWidth(frame3d)), frame3d)} renderer3dCapture=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, captureFrame3d)} depth=0x0:empty attr=0x0:empty coverage=0x0:empty",
                )
                if (RendererDebugCaptureKind.SCREEN_FRAME in requestedKinds) {
                    logFrameSummary(
                        captureId = captureId,
                        kind = "screenFrame",
                        width = RendererDebugBridge.CAPTURE_WIDTH,
                        height = RendererDebugBridge.CAPTURE_HEIGHT,
                        pixels = screenFrame,
                    )
                }
                if (RendererDebugCaptureKind.PACKED_TOP_PRIMARY in requestedKinds) {
                    logFrameSummary(
                        captureId = captureId,
                        kind = "packedTopPrimary",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = packedTopPrimary,
                    )
                }
                if (RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY in requestedKinds) {
                    logFrameSummary(
                        captureId = captureId,
                        kind = "packedBottomPrimary",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = packedBottomPrimary,
                    )
                }
                if (RendererDebugCaptureKind.PACKED_TOP_PLANE1 in requestedKinds) {
                    logFrameSummary(captureId, "packedTopPlane1", CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedTopPlane1)
                }
                if (RendererDebugCaptureKind.PACKED_TOP_CONTROL in requestedKinds) {
                    logFrameSummary(captureId, "packedTopControl", CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedTopControl)
                }
                if (RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1 in requestedKinds) {
                    logFrameSummary(captureId, "packedBottomPlane1", CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedBottomPlane1)
                }
                if (RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL in requestedKinds) {
                    logFrameSummary(captureId, "packedBottomControl", CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedBottomControl)
                }
                if (RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME in requestedKinds) {
                    logFrameSummary(captureId, "capture3dSourceDsFrame", CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, capture3dSource)
                }
                if (RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK in requestedKinds) {
                    logFrameSummary(captureId, "captureLineUses3dMask", CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, captureLineMask)
                }
                if (RendererDebugCaptureKind.RENDERER3D_FRAME in requestedKinds) {
                    val renderer3dWidth = inferRenderer3dWidth(frame3d)
                    logFrameSummary(captureId, "renderer3dFrame", renderer3dWidth, inferRenderer3dHeight(frame3d, renderer3dWidth), frame3d)
                }
                if (RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME in requestedKinds) {
                    logFrameSummary(
                        captureId = captureId,
                        kind = "renderer3dCaptureFrame",
                        width = CAPTURE_3D_LINE_WIDTH,
                        height = CAPTURE_3D_LINE_HEIGHT,
                        pixels = captureFrame3d,
                    )
                }
                val success =
                    ((RendererDebugCaptureKind.SCREEN_FRAME !in requestedKinds) || hasData(screenFrame))
                        && ((RendererDebugCaptureKind.PACKED_TOP_PRIMARY !in requestedKinds) || hasData(packedTopPrimary))
                        && ((RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY !in requestedKinds) || hasData(packedBottomPrimary))
                        && ((RendererDebugCaptureKind.PACKED_TOP_PLANE1 !in requestedKinds) || hasData(packedTopPlane1))
                        && ((RendererDebugCaptureKind.PACKED_TOP_CONTROL !in requestedKinds) || hasData(packedTopControl))
                        && ((RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1 !in requestedKinds) || hasData(packedBottomPlane1))
                        && ((RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL !in requestedKinds) || hasData(packedBottomControl))
                        && ((RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME !in requestedKinds) || hasData(capture3dSource))
                        && ((RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK !in requestedKinds) || hasData(captureLineMask))
                        && ((RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON !in requestedKinds) || !softPackedFrameMetaJson.isNullOrBlank())
                        && ((RendererDebugCaptureKind.RENDERER3D_FRAME !in requestedKinds) || hasData(frame3d))
                        && ((RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME !in requestedKinds) || hasData(captureFrame3d))
                Log.w(TAG, "captureId=$captureId stage=end success=${if (success) 1 else 0}")
                add(
                    RendererDebugCaptureResult(
                        captureId = captureId,
                        success = success,
                        outputDir = resolvedOutputDir,
                    ),
                )
            }
        }

        RendererDebugBridge.clearDenseScreenBurstCapture()
        return results
    }

    fun dumpPauseMenuCapture(
        configuredRenderer: VideoRenderer,
        outputDir: File? = null,
        captureIdOverride: String? = null,
        captureKinds: Set<RendererDebugCaptureKind> = RendererDebugCaptureKind.allKinds,
        freezeRendererSnapshot: Boolean = true,
    ): RendererDebugCaptureResult {
        val requestedKinds = if (captureKinds.isEmpty()) {
            RendererDebugCaptureKind.allKinds
        } else {
            captureKinds
        }
        val captureId = captureIdOverride ?: java.lang.Long.toHexString(System.currentTimeMillis())
        val frameId = RendererDebugBridge.getCurrentFrameIndexForDebug()
        val frameReady = RendererDebugBridge.isCurrentFrameReadyForDebug()
        Log.w(
            TAG,
            "captureId=$captureId stage=begin configuredRenderer=${configuredRenderer.name.lowercase(Locale.US)} frameId=$frameId frameReady=${if (frameReady) 1 else 0} freezeSnapshot=${if (freezeRendererSnapshot) 1 else 0} kinds=${requestedKinds.joinToString(separator = ",") { it.name.lowercase(Locale.US) }}",
        )

        if (freezeRendererSnapshot) {
            RendererDebugBridge.dumpCurrentRendererSnapshot()
        } else {
            RendererDebugBridge.clearPreparedRendererSnapshot()
        }

        val screenFrame = if (RendererDebugCaptureKind.SCREEN_FRAME in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentFrame", before = true)
            val pixels = RendererDebugBridge.captureCurrentFrame()
            logCaptureStep(captureId, "captureCurrentFrame", before = false)
            pixels
        } else {
            null
        }
        val packedTopPrimary = if (RendererDebugCaptureKind.PACKED_TOP_PRIMARY in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentPackedTopPrimary", before = true)
            val pixels = RendererDebugBridge.captureCurrentPackedTopPrimary()
            logCaptureStep(captureId, "captureCurrentPackedTopPrimary", before = false)
            pixels
        } else {
            null
        }
        val packedBottomPrimary = if (RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentPackedBottomPrimary", before = true)
            val pixels = RendererDebugBridge.captureCurrentPackedBottomPrimary()
            logCaptureStep(captureId, "captureCurrentPackedBottomPrimary", before = false)
            pixels
        } else {
            null
        }
        val packedTopPlane1 = if (RendererDebugCaptureKind.PACKED_TOP_PLANE1 in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentPackedPlane(top,1)", before = true)
            val pixels = RendererDebugBridge.captureCurrentPackedPlane(screenIndex = 0, planeIndex = 1)
            logCaptureStep(captureId, "captureCurrentPackedPlane(top,1)", before = false)
            pixels
        } else {
            null
        }
        val packedTopControl = if (RendererDebugCaptureKind.PACKED_TOP_CONTROL in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentPackedPlane(top,2)", before = true)
            val pixels = RendererDebugBridge.captureCurrentPackedPlane(screenIndex = 0, planeIndex = 2)
            logCaptureStep(captureId, "captureCurrentPackedPlane(top,2)", before = false)
            pixels
        } else {
            null
        }
        val packedBottomPlane1 = if (RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1 in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentPackedPlane(bottom,1)", before = true)
            val pixels = RendererDebugBridge.captureCurrentPackedPlane(screenIndex = 1, planeIndex = 1)
            logCaptureStep(captureId, "captureCurrentPackedPlane(bottom,1)", before = false)
            pixels
        } else {
            null
        }
        val packedBottomControl = if (RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentPackedPlane(bottom,2)", before = true)
            val pixels = RendererDebugBridge.captureCurrentPackedPlane(screenIndex = 1, planeIndex = 2)
            logCaptureStep(captureId, "captureCurrentPackedPlane(bottom,2)", before = false)
            pixels
        } else {
            null
        }
        val capture3dSourceDsFrame = if (RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentCapture3dSource", before = true)
            val pixels = RendererDebugBridge.captureCurrentCapture3dSource()
            logCaptureStep(captureId, "captureCurrentCapture3dSource", before = false)
            pixels
        } else {
            null
        }
        val captureLineUses3dMask = if (RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentCaptureLineUses3dMask", before = true)
            val pixels = RendererDebugBridge.captureCurrentCaptureLineUses3dMask()
            logCaptureStep(captureId, "captureCurrentCaptureLineUses3dMask", before = false)
            pixels
        } else {
            null
        }
        val comp4TopPlaceholder = if (RendererDebugCaptureKind.COMP4_TOP_PLACEHOLDER in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentComp4TopPlaceholder", before = true)
            val pixels = RendererDebugBridge.captureCurrentComp4TopPlaceholder()
            logCaptureStep(captureId, "captureCurrentComp4TopPlaceholder", before = false)
            pixels
        } else {
            null
        }
        val comp4BottomPlaceholder = if (RendererDebugCaptureKind.COMP4_BOTTOM_PLACEHOLDER in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentComp4BottomPlaceholder", before = true)
            val pixels = RendererDebugBridge.captureCurrentComp4BottomPlaceholder()
            logCaptureStep(captureId, "captureCurrentComp4BottomPlaceholder", before = false)
            pixels
        } else {
            null
        }
        val captureFallbackMask = if (RendererDebugCaptureKind.CAPTURE_FALLBACK_MASK in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentCaptureFallbackMask", before = true)
            val pixels = RendererDebugBridge.captureCurrentCaptureFallbackMask()
            logCaptureStep(captureId, "captureCurrentCaptureFallbackMask", before = false)
            pixels
        } else {
            null
        }
        val softPackedFrameMetaJson = if (RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentSoftPackedFrameMetaJson", before = true)
            val json = RendererDebugBridge.captureCurrentSoftPackedFrameMetaJson()
            logCaptureStep(captureId, "captureCurrentSoftPackedFrameMetaJson", before = false)
            json
        } else {
            null
        }
        val compositedDimensions = if (RendererDebugCaptureKind.COMPOSITED_FRAME in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentCompositedDimensions", before = true)
            val dimensions = RendererDebugBridge.captureCurrentCompositedDimensions()
            logCaptureStep(captureId, "captureCurrentCompositedDimensions", before = false)
            dimensions
        } else {
            null
        }
        val compositedWidth = compositedDimensions?.getOrNull(0) ?: 0
        val compositedHeight = compositedDimensions?.getOrNull(1) ?: 0
        val compositedFrame = if (RendererDebugCaptureKind.COMPOSITED_FRAME in requestedKinds) {
            logCaptureStep(captureId, "captureCurrentCompositedFrame", before = true)
            val pixels = RendererDebugBridge.captureCurrentCompositedFrame()
            logCaptureStep(captureId, "captureCurrentCompositedFrame", before = false)
            pixels
        } else {
            null
        }
        val canCapture3dTargets = requestedKinds.any {
            it == RendererDebugCaptureKind.RENDERER3D_FRAME
                || it == RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME
                || it == RendererDebugCaptureKind.RENDERER3D_DEPTH
                || it == RendererDebugCaptureKind.RENDERER3D_ATTR
                || it == RendererDebugCaptureKind.RENDERER3D_COVERAGE
        }
        val needs3dDimensions = canCapture3dTargets
        val renderer3dDimensions = if (needs3dDimensions) {
            logCaptureStep(captureId, "captureCurrent3dDimensions", before = true)
            val dimensions = RendererDebugBridge.captureCurrent3dDimensions()
            logCaptureStep(captureId, "captureCurrent3dDimensions", before = false)
            dimensions
        } else {
            null
        }
        val renderer3dWidth = renderer3dDimensions?.getOrNull(0) ?: 0
        val renderer3dHeight = renderer3dDimensions?.getOrNull(1) ?: 0
        val frame3d = if (canCapture3dTargets && RendererDebugCaptureKind.RENDERER3D_FRAME in requestedKinds) {
            logCaptureStep(captureId, "captureCurrent3dFrame", before = true)
            val pixels = RendererDebugBridge.captureCurrent3dFrame()
            logCaptureStep(captureId, "captureCurrent3dFrame", before = false)
            pixels
        } else {
            null
        }
        val captureFrame3d = if (canCapture3dTargets && RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME in requestedKinds) {
            logCaptureStep(captureId, "captureCurrent3dCaptureFrame", before = true)
            val pixels = RendererDebugBridge.captureCurrent3dCaptureFrame()
            logCaptureStep(captureId, "captureCurrent3dCaptureFrame", before = false)
            pixels
        } else {
            null
        }
        val depth3d = if (canCapture3dTargets && RendererDebugCaptureKind.RENDERER3D_DEPTH in requestedKinds) {
            logCaptureStep(captureId, "captureCurrent3dDepth", before = true)
            val values = RendererDebugBridge.captureCurrent3dDepth()
            logCaptureStep(captureId, "captureCurrent3dDepth", before = false)
            values
        } else {
            null
        }
        val attr3d = if (canCapture3dTargets && RendererDebugCaptureKind.RENDERER3D_ATTR in requestedKinds) {
            logCaptureStep(captureId, "captureCurrent3dAttributes", before = true)
            val values = RendererDebugBridge.captureCurrent3dAttributes()
            logCaptureStep(captureId, "captureCurrent3dAttributes", before = false)
            values
        } else {
            null
        }
        val coverage3d = if (canCapture3dTargets && RendererDebugCaptureKind.RENDERER3D_COVERAGE in requestedKinds) {
            logCaptureStep(captureId, "captureCurrent3dCoverage", before = true)
            val values = RendererDebugBridge.captureCurrent3dCoverage()
            logCaptureStep(captureId, "captureCurrent3dCoverage", before = false)
            values
        } else {
            null
        }

        val resolvedOutputDir = outputDir?.takeIf { directory ->
            directory.exists() || directory.mkdirs()
        }

        if (RendererDebugCaptureKind.SCREEN_FRAME in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "screenFrame",
                width = RendererDebugBridge.CAPTURE_WIDTH,
                height = RendererDebugBridge.CAPTURE_HEIGHT,
                pixels = screenFrame,
            )
        }
        if (RendererDebugCaptureKind.PACKED_TOP_PRIMARY in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "packedTopPrimary",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedTopPrimary,
            )
        }
        if (RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "packedBottomPrimary",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedBottomPrimary,
            )
        }
        if (RendererDebugCaptureKind.PACKED_TOP_PLANE1 in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "packedTopPlane1",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedTopPlane1,
            )
        }
        if (RendererDebugCaptureKind.PACKED_TOP_CONTROL in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "packedTopControl",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedTopControl,
            )
        }
        if (RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1 in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "packedBottomPlane1",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedBottomPlane1,
            )
        }
        if (RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "packedBottomControl",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedBottomControl,
            )
        }
        if (RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "capture3dSourceDsFrame",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = capture3dSourceDsFrame,
            )
        }
        if (RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "captureLineUses3dMask",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = captureLineUses3dMask,
            )
        }
        if (RendererDebugCaptureKind.COMP4_TOP_PLACEHOLDER in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "comp4TopPlaceholder",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = comp4TopPlaceholder,
            )
        }
        if (RendererDebugCaptureKind.COMP4_BOTTOM_PLACEHOLDER in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "comp4BottomPlaceholder",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = comp4BottomPlaceholder,
            )
        }
        if (RendererDebugCaptureKind.CAPTURE_FALLBACK_MASK in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "captureFallbackMask",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = captureFallbackMask,
            )
        }
        if (RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON in requestedKinds) {
            saveTextFile(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "softPackedFrameMeta",
                contents = softPackedFrameMetaJson,
                extension = "json",
            )
        }
        if (RendererDebugCaptureKind.COMPOSITED_FRAME in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "compositedFrame",
                width = compositedWidth,
                height = compositedHeight,
                pixels = compositedFrame,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_FRAME in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "renderer3dFrame",
                width = renderer3dWidth,
                height = renderer3dHeight,
                pixels = frame3d,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME in requestedKinds) {
            saveFramePng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "renderer3dCaptureFrame",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = captureFrame3d,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_DEPTH in requestedKinds) {
            saveValueMapPng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "renderer3dDepth",
                width = renderer3dWidth,
                height = renderer3dHeight,
                values = depth3d,
                mapper = ::encodeDepthDebugPixel,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_ATTR in requestedKinds) {
            saveValueMapPng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "renderer3dAttr",
                width = renderer3dWidth,
                height = renderer3dHeight,
                values = attr3d,
                mapper = ::encodeAttrDebugPixel,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_COVERAGE in requestedKinds) {
            saveValueMapPng(
                outputDir = resolvedOutputDir,
                captureId = captureId,
                kind = "renderer3dCoverage",
                width = renderer3dWidth,
                height = renderer3dHeight,
                values = coverage3d,
                mapper = ::encodeCoverageDebugPixel,
            )
        }

        Log.w(
            TAG,
            "captureId=$captureId kind=meta screen=${describeBufferShape(RendererDebugBridge.CAPTURE_WIDTH, RendererDebugBridge.CAPTURE_HEIGHT, screenFrame)} packedTop=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedTopPrimary)} packedBottom=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedBottomPrimary)} packedTopPlane1=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedTopPlane1)} packedTopControl=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedTopControl)} packedBottomPlane1=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedBottomPlane1)} packedBottomControl=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, packedBottomControl)} capture3dSource=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, capture3dSourceDsFrame)} captureLineMask=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, captureLineUses3dMask)} comp4Top=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, comp4TopPlaceholder)} comp4Bottom=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, comp4BottomPlaceholder)} fallbackMask=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, captureFallbackMask)} softPackedMeta=${if (softPackedFrameMetaJson.isNullOrBlank()) 0 else 1} composited=${describeBufferShape(compositedWidth, compositedHeight, compositedFrame)} renderer3d=${describeBufferShape(renderer3dWidth, renderer3dHeight, frame3d)} renderer3dCapture=${describeBufferShape(CAPTURE_3D_LINE_WIDTH, CAPTURE_3D_LINE_HEIGHT, captureFrame3d)} depth=${describeBufferShape(renderer3dWidth, renderer3dHeight, depth3d)} attr=${describeBufferShape(renderer3dWidth, renderer3dHeight, attr3d)} coverage=${describeBufferShape(renderer3dWidth, renderer3dHeight, coverage3d)}",
        )

        if (RendererDebugCaptureKind.SCREEN_FRAME in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "screenFrame",
                width = RendererDebugBridge.CAPTURE_WIDTH,
                height = RendererDebugBridge.CAPTURE_HEIGHT,
                pixels = screenFrame,
            )
        }
        if (RendererDebugCaptureKind.PACKED_TOP_PRIMARY in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "packedTopPrimary",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedTopPrimary,
            )
        }
        if (RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "packedBottomPrimary",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedBottomPrimary,
            )
        }
        if (RendererDebugCaptureKind.PACKED_TOP_PLANE1 in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "packedTopPlane1",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedTopPlane1,
            )
        }
        if (RendererDebugCaptureKind.PACKED_TOP_CONTROL in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "packedTopControl",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedTopControl,
            )
        }
        if (RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1 in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "packedBottomPlane1",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedBottomPlane1,
            )
        }
        if (RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "packedBottomControl",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = packedBottomControl,
            )
        }
        if (RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "capture3dSourceDsFrame",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = capture3dSourceDsFrame,
            )
        }
        if (RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "captureLineUses3dMask",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = captureLineUses3dMask,
            )
        }
        if (RendererDebugCaptureKind.COMP4_TOP_PLACEHOLDER in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "comp4TopPlaceholder",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = comp4TopPlaceholder,
            )
        }
        if (RendererDebugCaptureKind.COMP4_BOTTOM_PLACEHOLDER in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "comp4BottomPlaceholder",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = comp4BottomPlaceholder,
            )
        }
        if (RendererDebugCaptureKind.CAPTURE_FALLBACK_MASK in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "captureFallbackMask",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = captureFallbackMask,
            )
        }
        if (RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON in requestedKinds) {
            Log.w(
                TAG,
                "captureId=$captureId kind=softPackedFrameMetaJson available=${if (softPackedFrameMetaJson.isNullOrBlank()) 0 else 1} length=${softPackedFrameMetaJson?.length ?: 0}",
            )
        }
        if (RendererDebugCaptureKind.COMPOSITED_FRAME in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "compositedFrame",
                width = compositedWidth,
                height = compositedHeight,
                pixels = compositedFrame,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_FRAME in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "renderer3dFrame",
                width = renderer3dWidth,
                height = renderer3dHeight,
                pixels = frame3d,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME in requestedKinds) {
            logFrameSummary(
                captureId = captureId,
                kind = "renderer3dCaptureFrame",
                width = CAPTURE_3D_LINE_WIDTH,
                height = CAPTURE_3D_LINE_HEIGHT,
                pixels = captureFrame3d,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_DEPTH in requestedKinds) {
            logDepthSummary(
                captureId = captureId,
                width = renderer3dWidth,
                height = renderer3dHeight,
                values = depth3d,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_ATTR in requestedKinds) {
            logAttrSummary(
                captureId = captureId,
                width = renderer3dWidth,
                height = renderer3dHeight,
                values = attr3d,
            )
        }
        if (RendererDebugCaptureKind.RENDERER3D_COVERAGE in requestedKinds) {
            logCoverageSummary(
                captureId = captureId,
                width = renderer3dWidth,
                height = renderer3dHeight,
                values = coverage3d,
            )
        }

        val success = (
            (RendererDebugCaptureKind.SCREEN_FRAME in requestedKinds && hasData(screenFrame))
                || (RendererDebugCaptureKind.PACKED_TOP_PRIMARY in requestedKinds && hasData(packedTopPrimary))
                || (RendererDebugCaptureKind.PACKED_BOTTOM_PRIMARY in requestedKinds && hasData(packedBottomPrimary))
                || (RendererDebugCaptureKind.PACKED_TOP_PLANE1 in requestedKinds && hasData(packedTopPlane1))
                || (RendererDebugCaptureKind.PACKED_TOP_CONTROL in requestedKinds && hasData(packedTopControl))
                || (RendererDebugCaptureKind.PACKED_BOTTOM_PLANE1 in requestedKinds && hasData(packedBottomPlane1))
                || (RendererDebugCaptureKind.PACKED_BOTTOM_CONTROL in requestedKinds && hasData(packedBottomControl))
                || (RendererDebugCaptureKind.CAPTURE3D_SOURCE_DS_FRAME in requestedKinds && hasData(capture3dSourceDsFrame))
                || (RendererDebugCaptureKind.CAPTURE_LINE_USES_3D_MASK in requestedKinds && hasData(captureLineUses3dMask))
                || (RendererDebugCaptureKind.COMP4_TOP_PLACEHOLDER in requestedKinds && hasData(comp4TopPlaceholder))
                || (RendererDebugCaptureKind.COMP4_BOTTOM_PLACEHOLDER in requestedKinds && hasData(comp4BottomPlaceholder))
                || (RendererDebugCaptureKind.CAPTURE_FALLBACK_MASK in requestedKinds && hasData(captureFallbackMask))
                || (RendererDebugCaptureKind.SOFT_PACKED_FRAME_META_JSON in requestedKinds && !softPackedFrameMetaJson.isNullOrBlank())
                || (RendererDebugCaptureKind.COMPOSITED_FRAME in requestedKinds && hasData(compositedFrame))
                || (RendererDebugCaptureKind.RENDERER3D_FRAME in requestedKinds && hasData(frame3d))
                || (RendererDebugCaptureKind.RENDERER3D_CAPTURE_FRAME in requestedKinds && hasData(captureFrame3d))
                || (RendererDebugCaptureKind.RENDERER3D_DEPTH in requestedKinds && hasData(depth3d))
                || (RendererDebugCaptureKind.RENDERER3D_ATTR in requestedKinds && hasData(attr3d))
                || (RendererDebugCaptureKind.RENDERER3D_COVERAGE in requestedKinds && hasData(coverage3d))
            )
        Log.w(
            TAG,
            "captureId=$captureId stage=end success=${if (success) 1 else 0}",
        )
        return RendererDebugCaptureResult(
            captureId = captureId,
            success = success,
            outputDir = resolvedOutputDir,
        )
    }

    private fun hasData(values: IntArray?): Boolean {
        return values != null && values.isNotEmpty()
    }

    private fun inferRenderer3dWidth(values: IntArray?): Int {
        val size = values?.size ?: return 0
        val basePixels = CAPTURE_3D_LINE_WIDTH * CAPTURE_3D_LINE_HEIGHT
        if (size <= 0 || size % basePixels != 0) {
            return 0
        }
        val scaleSquared = size / basePixels
        val scale = (1..16).firstOrNull { candidate ->
            candidate * candidate == scaleSquared
        } ?: return 0
        return CAPTURE_3D_LINE_WIDTH * scale
    }

    private fun inferRenderer3dHeight(values: IntArray?, width: Int): Int {
        val size = values?.size ?: return 0
        if (size <= 0 || width <= 0 || size % width != 0) {
            return 0
        }
        return size / width
    }

    private fun logCaptureStep(captureId: String, step: String, before: Boolean) {
        Log.w(
            TAG,
            "captureId=$captureId step=$step phase=${if (before) "begin" else "end"}",
        )
    }

    private fun describeBufferShape(width: Int, height: Int, values: IntArray?): String {
        val data = values
        if (data == null || data.isEmpty()) {
            return "${width}x${height}:empty"
        }

        val expectedSize = if (width > 0 && height > 0) width * height else -1
        return if (expectedSize > 0 && expectedSize == data.size) {
            "${width}x${height}:${data.size}"
        } else {
            "${width}x${height}:${data.size}:expected=$expectedSize"
        }
    }

    private fun logFrameSummary(
        captureId: String,
        kind: String,
        width: Int,
        height: Int,
        pixels: IntArray?,
    ) {
        val data = pixels
        if (data == null || data.isEmpty()) {
            Log.w(TAG, "captureId=$captureId kind=$kind unavailable=1")
            return
        }

        var nonBlack = 0
        var nonTransparent = 0
        var opaque = 0
        var magenta = 0
        var minAlpha = 255
        var maxAlpha = 0
        for (pixel in data) {
            val rgb = pixel and 0x00FFFFFF
            val alpha = (pixel ushr 24) and 0xFF
            if (rgb != 0) nonBlack++
            if (alpha != 0) nonTransparent++
            if (alpha == 0xFF) opaque++
            if (rgb == 0x00FF00FF) magenta++
            if (alpha < minAlpha) minAlpha = alpha
            if (alpha > maxAlpha) maxAlpha = alpha
        }

        Log.w(
            TAG,
            "captureId=$captureId kind=$kind size=${width}x${height} pixels=${data.size} crc32=${crc32Hex(data)} nonBlack=$nonBlack nonTransparent=$nonTransparent opaque=$opaque magenta=$magenta alphaRange=$minAlpha-$maxAlpha samples=${buildSamplePreview(width, height, data)}",
        )
    }

    private fun saveFramePng(
        outputDir: File?,
        captureId: String,
        kind: String,
        width: Int,
        height: Int,
        pixels: IntArray?,
    ) {
        val directory = outputDir ?: return
        val data = pixels ?: return
        if (width <= 0 || height <= 0 || data.size != width * height) {
            return
        }

        val file = File(directory, "${captureId}_${kind}.png")
        try {
            val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            bitmap.setPixels(data, 0, width, 0, 0, width, height)
            FileOutputStream(file).use { stream ->
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream)
            }
            bitmap.recycle()
            Log.w(TAG, "captureId=$captureId kind=$kind png=${file.absolutePath}")
        } catch (error: Exception) {
            Log.w(TAG, "captureId=$captureId kind=$kind png_save_failed=1", error)
        }
    }

    private fun saveTextFile(
        outputDir: File?,
        captureId: String,
        kind: String,
        contents: String?,
        extension: String,
    ) {
        val directory = outputDir ?: return
        val text = contents?.takeIf { it.isNotBlank() } ?: return
        val file = File(directory, "${captureId}_${kind}.${extension}")
        try {
            file.writeText(text)
            Log.w(TAG, "captureId=$captureId kind=$kind text=${file.absolutePath}")
        } catch (error: Exception) {
            Log.w(TAG, "captureId=$captureId kind=$kind text_save_failed=1", error)
        }
    }

    private fun saveValueMapPng(
        outputDir: File?,
        captureId: String,
        kind: String,
        width: Int,
        height: Int,
        values: IntArray?,
        mapper: (Int) -> Int,
    ) {
        val data = values
        if (outputDir == null || data == null || data.isEmpty()) {
            return
        }
        if (width <= 0 || height <= 0 || data.size != width * height) {
            return
        }

        val pixels = IntArray(data.size)
        for (index in data.indices) {
            pixels[index] = mapper(data[index])
        }
        saveFramePng(
            outputDir = outputDir,
            captureId = captureId,
            kind = kind,
            width = width,
            height = height,
            pixels = pixels,
        )
    }

    private fun encodeDepthDebugPixel(value: Int): Int {
        val unsignedValue = value.toLong() and 0xFFFFFFFFL
        val r = ((unsignedValue ushr 16) and 0xFF).toInt()
        val g = ((unsignedValue ushr 8) and 0xFF).toInt()
        val b = (unsignedValue and 0xFF).toInt()
        return (0xFF shl 24) or (r shl 16) or (g shl 8) or b
    }

    private fun encodeAttrDebugPixel(value: Int): Int {
        val polyId = ((value ushr 24) and 0x3F) * 255 / 63
        val translucent = if ((value and (1 shl 22)) != 0) 0xFF else 0x00
        val fog = if ((value and (1 shl 15)) != 0) 0xFF else 0x00
        val edge = (value and 0x1F) * 255 / 31
        return (0xFF shl 24) or (polyId shl 16) or (fog shl 8) or maxOf(edge, translucent)
    }

    private fun encodeCoverageDebugPixel(value: Int): Int {
        val coverage = (value and 0xFF).coerceIn(0, 31) * 255 / 31
        return (0xFF shl 24) or (coverage shl 16) or (coverage shl 8) or coverage
    }

    private fun logDepthSummary(
        captureId: String,
        width: Int,
        height: Int,
        values: IntArray?,
    ) {
        val data = values
        if (data == null || data.isEmpty()) {
            Log.w(TAG, "captureId=$captureId kind=renderer3dDepth unavailable=1")
            return
        }

        var minValue = Long.MAX_VALUE
        var maxValue = Long.MIN_VALUE
        var zeroCount = 0
        for (value in data) {
            val unsignedValue = value.toLong() and 0xFFFFFFFFL
            if (unsignedValue < minValue) minValue = unsignedValue
            if (unsignedValue > maxValue) maxValue = unsignedValue
            if (unsignedValue == 0L) zeroCount++
        }

        Log.w(
            TAG,
            "captureId=$captureId kind=renderer3dDepth size=${width}x${height} pixels=${data.size} crc32=${crc32Hex(data)} min=${hex32(minValue)} max=${hex32(maxValue)} zero=$zeroCount samples=${buildSamplePreview(width, height, data)}",
        )
    }

    private fun logAttrSummary(
        captureId: String,
        width: Int,
        height: Int,
        values: IntArray?,
    ) {
        val data = values
        if (data == null || data.isEmpty()) {
            Log.w(TAG, "captureId=$captureId kind=renderer3dAttr unavailable=1")
            return
        }

        var nonZero = 0
        var edgePixels = 0
        var fogPixels = 0
        var backFacingPixels = 0
        val polyIdCounts = IntArray(64)

        for (value in data) {
            if (value != 0) nonZero++
            if ((value and 0xF) != 0) edgePixels++
            if ((value and (1 shl 15)) != 0) fogPixels++
            if ((value and (1 shl 4)) != 0) backFacingPixels++

            val polyId = (value ushr 24) and 0x3F
            polyIdCounts[polyId]++
        }

        var uniquePolyIds = 0
        for (count in polyIdCounts) {
            if (count > 0)
                uniquePolyIds++
        }

        val topPolyIdEntries = ArrayList<Pair<Int, Int>>()
        for (polyId in polyIdCounts.indices) {
            val count = polyIdCounts[polyId]
            if (count > 0)
                topPolyIdEntries.add(polyId to count)
        }
        topPolyIdEntries.sortByDescending { it.second }
        val topPolyIds = if (topPolyIdEntries.isEmpty()) {
            "none"
        } else {
            topPolyIdEntries.take(6).joinToString(separator = ",") { "${it.first}:${it.second}" }
        }

        Log.w(
            TAG,
            "captureId=$captureId kind=renderer3dAttr size=${width}x${height} pixels=${data.size} crc32=${crc32Hex(data)} nonZero=$nonZero edge=$edgePixels fog=$fogPixels backFacing=$backFacingPixels uniquePolyIds=$uniquePolyIds topPolyIds=$topPolyIds samples=${buildSamplePreview(width, height, data)}",
        )
    }

    private fun logCoverageSummary(
        captureId: String,
        width: Int,
        height: Int,
        values: IntArray?,
    ) {
        val data = values
        if (data == null || data.isEmpty()) {
            Log.w(TAG, "captureId=$captureId kind=renderer3dCoverage unavailable=1")
            return
        }

        var nonZero = 0
        var fullCoverage = 0
        var maxCoverage = 0
        var totalCoverage = 0L
        for (value in data) {
            val coverage = value and 0x1F
            if (coverage != 0) nonZero++
            if (coverage == 0x1F) fullCoverage++
            if (coverage > maxCoverage) maxCoverage = coverage
            totalCoverage += coverage.toLong()
        }

        val meanCoverage = if (data.isEmpty()) {
            0.0
        } else {
            totalCoverage.toDouble() / data.size.toDouble()
        }

        Log.w(
            TAG,
            "captureId=$captureId kind=renderer3dCoverage size=${width}x${height} pixels=${data.size} crc32=${crc32Hex(data)} nonZero=$nonZero full31=$fullCoverage max=$maxCoverage mean=${"%.3f".format(Locale.US, meanCoverage)} samples=${buildSamplePreview(width, height, data)}",
        )
    }

    private fun buildSamplePreview(width: Int, height: Int, values: IntArray): String {
        if (width <= 0 || height <= 0 || values.isEmpty()) {
            return "none"
        }

        val samplePoints = linkedMapOf<String, Pair<Int, Int>>(
            "tl" to (0 to 0),
            "tc" to (width / 2 to 0),
            "c" to (width / 2 to height / 2),
            "bc" to (width / 2 to (height - 1)),
            "br" to ((width - 1) to (height - 1)),
        )

        return samplePoints.entries.joinToString(separator = ",") { (label, point) ->
            val x = point.first.coerceIn(0, width - 1)
            val y = point.second.coerceIn(0, height - 1)
            val index = y * width + x
            val value = if (index in values.indices) {
                hex32(values[index])
            } else {
                "out"
            }
            "$label:$value"
        }
    }

    private fun crc32Hex(values: IntArray): String {
        val crc32 = CRC32()
        for (value in values) {
            crc32.update(value and 0xFF)
            crc32.update((value ushr 8) and 0xFF)
            crc32.update((value ushr 16) and 0xFF)
            crc32.update((value ushr 24) and 0xFF)
        }
        return hex32(crc32.value)
    }

    private fun hex32(value: Int): String {
        return hex32(value.toLong() and 0xFFFFFFFFL)
    }

    private fun hex32(value: Long): String {
        return java.lang.Long.toHexString(value and 0xFFFFFFFFL)
            .padStart(8, '0')
            .uppercase(Locale.US)
    }
}
