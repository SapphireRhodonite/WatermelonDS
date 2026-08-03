package me.magnum.melonds.impl.emulator.debug

import androidx.annotation.Keep

@Keep
internal object RendererDebugBridge {
    const val CAPTURE_WIDTH = 256
    const val CAPTURE_HEIGHT = 384
    const val DENSE_CAPTURE_SCREEN_FRAME = 1 shl 0
    const val DENSE_CAPTURE_PACKED_TOP_PRIMARY = 1 shl 1
    const val DENSE_CAPTURE_PACKED_BOTTOM_PRIMARY = 1 shl 2
    const val DENSE_CAPTURE_RENDERER3D_CAPTURE_FRAME = 1 shl 3
    const val DENSE_CAPTURE_PACKED_TOP_PLANE1 = 1 shl 4
    const val DENSE_CAPTURE_PACKED_TOP_CONTROL = 1 shl 5
    const val DENSE_CAPTURE_PACKED_BOTTOM_PLANE1 = 1 shl 6
    const val DENSE_CAPTURE_PACKED_BOTTOM_CONTROL = 1 shl 7
    const val DENSE_CAPTURE_CAPTURE3D_SOURCE = 1 shl 8
    const val DENSE_CAPTURE_CAPTURE_LINE_MASK = 1 shl 9
    const val DENSE_CAPTURE_SOFT_PACKED_META = 1 shl 10
    const val DENSE_CAPTURE_RENDERER3D_FRAME = 1 shl 11
    const val RENDERER_2D_DEBUG_FEATURE_STATIC_BACKGROUND = 1 shl 0
    const val RENDERER_2D_DEBUG_FEATURE_AFFINE_BACKGROUND = 1 shl 1
    const val RENDERER_2D_DEBUG_FEATURE_AFFINE_EXTENDED_TILED_BACKGROUND = 1 shl 2
    const val RENDERER_2D_DEBUG_FEATURE_AFFINE_EXTENDED_BITMAP_256_BACKGROUND = 1 shl 3
    const val RENDERER_2D_DEBUG_FEATURE_AFFINE_EXTENDED_DIRECT_COLOR_BACKGROUND = 1 shl 4
    const val RENDERER_2D_DEBUG_FEATURE_LARGE_SCREEN_BACKGROUND = 1 shl 5
    const val RENDERER_2D_DEBUG_FEATURE_3D_BACKGROUND = 1 shl 6
    const val RENDERER_2D_DEBUG_FEATURE_OBJECTS = 1 shl 7
    const val RENDERER_2D_DEBUG_FEATURE_REGULAR_OBJECT = 1 shl 8
    const val RENDERER_2D_DEBUG_FEATURE_AFFINE_OBJECT = 1 shl 9
    const val RENDERER_2D_DEBUG_FEATURE_TILED_4BPP_OBJECT = 1 shl 10
    const val RENDERER_2D_DEBUG_FEATURE_TILED_8BPP_OBJECT = 1 shl 11
    const val RENDERER_2D_DEBUG_FEATURE_BITMAP_OBJECT = 1 shl 12
    const val RENDERER_2D_DEBUG_FEATURE_BLENDED_OBJECT = 1 shl 13
    const val RENDERER_2D_DEBUG_FEATURE_WINDOW_OBJECT = 1 shl 14
    const val RENDERER_2D_DEBUG_FEATURE_MOSAIC_OBJECT = 1 shl 15
    const val RENDERER_2D_DEBUG_FEATURE_OBJECT_UPPER_BAND = 1 shl 16
    const val RENDERER_2D_DEBUG_FEATURE_OBJECT_MIDDLE_BAND = 1 shl 17
    const val RENDERER_2D_DEBUG_FEATURE_OBJECT_LOWER_BAND = 1 shl 18
    const val RENDERER_2D_DEBUG_FEATURE_ALL =
        RENDERER_2D_DEBUG_FEATURE_STATIC_BACKGROUND or
            RENDERER_2D_DEBUG_FEATURE_AFFINE_BACKGROUND or
            RENDERER_2D_DEBUG_FEATURE_AFFINE_EXTENDED_TILED_BACKGROUND or
            RENDERER_2D_DEBUG_FEATURE_AFFINE_EXTENDED_BITMAP_256_BACKGROUND or
            RENDERER_2D_DEBUG_FEATURE_AFFINE_EXTENDED_DIRECT_COLOR_BACKGROUND or
            RENDERER_2D_DEBUG_FEATURE_LARGE_SCREEN_BACKGROUND or
            RENDERER_2D_DEBUG_FEATURE_3D_BACKGROUND or
            RENDERER_2D_DEBUG_FEATURE_OBJECTS or
            RENDERER_2D_DEBUG_FEATURE_REGULAR_OBJECT or
            RENDERER_2D_DEBUG_FEATURE_AFFINE_OBJECT or
            RENDERER_2D_DEBUG_FEATURE_TILED_4BPP_OBJECT or
            RENDERER_2D_DEBUG_FEATURE_TILED_8BPP_OBJECT or
            RENDERER_2D_DEBUG_FEATURE_BITMAP_OBJECT or
            RENDERER_2D_DEBUG_FEATURE_BLENDED_OBJECT or
            RENDERER_2D_DEBUG_FEATURE_WINDOW_OBJECT or
            RENDERER_2D_DEBUG_FEATURE_MOSAIC_OBJECT or
            RENDERER_2D_DEBUG_FEATURE_OBJECT_UPPER_BAND or
            RENDERER_2D_DEBUG_FEATURE_OBJECT_MIDDLE_BAND or
            RENDERER_2D_DEBUG_FEATURE_OBJECT_LOWER_BAND

    const val RENDERER_3D_DEBUG_FEATURE_RENDERER_OUTPUT = 1 shl 0
    const val RENDERER_3D_DEBUG_FEATURE_TRIANGLE_POLYGONS = 1 shl 1
    const val RENDERER_3D_DEBUG_FEATURE_LINE_POLYGONS = 1 shl 2
    const val RENDERER_3D_DEBUG_FEATURE_OPAQUE_POLYGONS = 1 shl 3
    const val RENDERER_3D_DEBUG_FEATURE_TRANSLUCENT_POLYGONS = 1 shl 4
    const val RENDERER_3D_DEBUG_FEATURE_SHADOW_MASK_POLYGONS = 1 shl 5
    const val RENDERER_3D_DEBUG_FEATURE_SHADOW_POLYGONS = 1 shl 6
    const val RENDERER_3D_DEBUG_FEATURE_TEXTURED_POLYGONS = 1 shl 7
    const val RENDERER_3D_DEBUG_FEATURE_UNTEXTURED_POLYGONS = 1 shl 8
    const val RENDERER_3D_DEBUG_FEATURE_MODULATE_POLYGONS = 1 shl 9
    const val RENDERER_3D_DEBUG_FEATURE_DECAL_POLYGONS = 1 shl 10
    const val RENDERER_3D_DEBUG_FEATURE_TOON_HIGHLIGHT_POLYGONS = 1 shl 11
    const val RENDERER_3D_DEBUG_FEATURE_W_BUFFER_POLYGONS = 1 shl 12
    const val RENDERER_3D_DEBUG_FEATURE_Z_BUFFER_POLYGONS = 1 shl 13
    const val RENDERER_3D_DEBUG_FEATURE_DEPTH_WRITE_POLYGONS = 1 shl 14
    const val RENDERER_3D_DEBUG_FEATURE_FOG_WRITE_POLYGONS = 1 shl 15
    const val RENDERER_3D_DEBUG_FEATURE_UPPER_BAND = 1 shl 16
    const val RENDERER_3D_DEBUG_FEATURE_MIDDLE_BAND = 1 shl 17
    const val RENDERER_3D_DEBUG_FEATURE_LOWER_BAND = 1 shl 18
    const val RENDERER_3D_DEBUG_FEATURE_ALL =
        RENDERER_3D_DEBUG_FEATURE_RENDERER_OUTPUT or
            RENDERER_3D_DEBUG_FEATURE_TRIANGLE_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_LINE_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_OPAQUE_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_TRANSLUCENT_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_SHADOW_MASK_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_SHADOW_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_TEXTURED_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_UNTEXTURED_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_MODULATE_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_DECAL_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_TOON_HIGHLIGHT_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_W_BUFFER_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_Z_BUFFER_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_DEPTH_WRITE_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_FOG_WRITE_POLYGONS or
            RENDERER_3D_DEBUG_FEATURE_UPPER_BAND or
            RENDERER_3D_DEBUG_FEATURE_MIDDLE_BAND or
            RENDERER_3D_DEBUG_FEATURE_LOWER_BAND

    external fun getRenderer2DDebugControls(): IntArray?
    external fun setRenderer2DDebugControls(
        mainForcedMode: Int,
        subForcedMode: Int,
        topForcedCompMode: Int,
        bottomForcedCompMode: Int,
        disabledMainBgMask: Int,
        disabledSubBgMask: Int,
        disabledMainBgPriorityMask: Int,
        disabledSubBgPriorityMask: Int,
        disabledMainObjPriorityMask: Int,
        disabledSubObjPriorityMask: Int,
        disabledMainObjOrderMask: Int,
        disabledSubObjOrderMask: Int,
        featureMask: Int,
    )
    external fun getRenderer3DDebugControls(): IntArray?
    external fun setRenderer3DDebugControls(featureMask: Int)
    external fun captureCurrentFrame(): IntArray?
    external fun captureCurrentPackedTopPrimary(): IntArray?
    external fun captureCurrentPackedBottomPrimary(): IntArray?
    external fun captureCurrentPackedPlane(screenIndex: Int, planeIndex: Int): IntArray?
    external fun captureCurrentCapture3dSource(): IntArray?
    external fun captureCurrentCaptureLineUses3dMask(): IntArray?
    external fun captureCurrentComp4TopPlaceholder(): IntArray?
    external fun captureCurrentComp4BottomPlaceholder(): IntArray?
    external fun captureCurrentCaptureFallbackMask(): IntArray?
    external fun captureCurrentSoftPackedFrameMetaJson(): String?
    external fun captureCurrentCompositedDimensions(): IntArray?
    external fun captureCurrentCompositedFrame(): IntArray?
    external fun isCurrentFrameReadyForDebug(): Boolean
    external fun getCurrentFrameIndexForDebug(): Int
    external fun requestPreparedRendererSnapshot()
    external fun clearPreparedRendererSnapshot()
    external fun startDenseScreenBurstCapture(
        frameCount: Int,
        stepFrames: Int,
        warmupFrames: Int,
        captureKindsMask: Int,
    )
    external fun isDenseScreenBurstCaptureComplete(): Boolean
    external fun getDenseScreenBurstScheduleStats(): IntArray?
    external fun getDenseScreenBurstCaptureFrameCount(): Int
    external fun getDenseScreenBurstCaptureFrameId(index: Int): Int
    external fun getDenseScreenBurstCaptureFrame(index: Int): IntArray?
    external fun getDenseScreenBurstPackedTopFrame(index: Int): IntArray?
    external fun getDenseScreenBurstPackedBottomFrame(index: Int): IntArray?
    external fun getDenseScreenBurstPackedPlaneFrame(index: Int, screenIndex: Int, planeIndex: Int): IntArray?
    external fun getDenseScreenBurstCapture3dSourceFrame(index: Int): IntArray?
    external fun getDenseScreenBurstCaptureLineUses3dMaskFrame(index: Int): IntArray?
    external fun getDenseScreenBurstSoftPackedFrameMetaJson(index: Int): String?
    external fun getDenseScreenBurstRenderer3dFrame(index: Int): IntArray?
    external fun getDenseScreenBurstRenderer3dCaptureFrame(index: Int): IntArray?
    external fun clearDenseScreenBurstCapture()
    external fun captureCurrent3dDimensions(): IntArray?
    external fun captureCurrent3dFrame(): IntArray?
    external fun captureCurrent3dCaptureFrame(): IntArray?
    external fun captureCurrent3dDepth(): IntArray?
    external fun captureCurrent3dAttributes(): IntArray?
    external fun captureCurrent3dCoverage(): IntArray?

    external fun dumpCurrentRendererSnapshot()
}

internal data class RendererParityReport(
    val totalPixels: Int,
    val mismatchedPixels: Int,
    val exactMatchPixels: Int,
    val pixelsOutsideTolerance: Int,
    val maxChannelDelta: Int,
    val meanChannelDelta: Double,
    val channelTolerance: Int,
) {
    val exactMatchRatio: Double
        get() = if (totalPixels == 0) 1.0 else exactMatchPixels.toDouble() / totalPixels.toDouble()
}

internal object RendererParityComparator {
    fun compareFrames(
        referencePixels: IntArray,
        candidatePixels: IntArray,
        channelTolerance: Int = 0,
    ): RendererParityReport {
        require(referencePixels.size == candidatePixels.size) {
            "Renderer frame sizes must match: ${referencePixels.size} != ${candidatePixels.size}"
        }
        require(channelTolerance >= 0) {
            "channelTolerance must be >= 0"
        }

        var mismatchedPixels = 0
        var exactMatchPixels = 0
        var pixelsOutsideTolerance = 0
        var maxChannelDelta = 0
        var totalChannelDelta = 0L

        for (pixelIndex in referencePixels.indices) {
            val referencePixel = referencePixels[pixelIndex]
            val candidatePixel = candidatePixels[pixelIndex]
            if (referencePixel == candidatePixel) {
                exactMatchPixels++
                continue
            }

            mismatchedPixels++
            var pixelExceededTolerance = false
            for (shift in 0..24 step 8) {
                val referenceChannel = (referencePixel ushr shift) and 0xFF
                val candidateChannel = (candidatePixel ushr shift) and 0xFF
                val channelDelta = kotlin.math.abs(referenceChannel - candidateChannel)
                totalChannelDelta += channelDelta.toLong()
                if (channelDelta > maxChannelDelta) {
                    maxChannelDelta = channelDelta
                }
                if (channelDelta > channelTolerance) {
                    pixelExceededTolerance = true
                }
            }

            if (pixelExceededTolerance) {
                pixelsOutsideTolerance++
            }
        }

        val comparedChannels = referencePixels.size * 4
        val meanChannelDelta = if (comparedChannels == 0) {
            0.0
        } else {
            totalChannelDelta.toDouble() / comparedChannels.toDouble()
        }

        return RendererParityReport(
            totalPixels = referencePixels.size,
            mismatchedPixels = mismatchedPixels,
            exactMatchPixels = exactMatchPixels,
            pixelsOutsideTolerance = pixelsOutsideTolerance,
            maxChannelDelta = maxChannelDelta,
            meanChannelDelta = meanChannelDelta,
            channelTolerance = channelTolerance,
        )
    }
}
