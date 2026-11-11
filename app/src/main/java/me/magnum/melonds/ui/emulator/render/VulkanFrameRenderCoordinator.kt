package me.magnum.melonds.ui.emulator.render

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.HandlerThread
import android.os.Message
import android.view.Surface
import androidx.core.os.bundleOf
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.RuntimeBackground
import me.magnum.melonds.extensions.parcelable
import me.magnum.melonds.ui.emulator.EmulatorSurfaceView
import me.magnum.melonds.ui.emulator.model.VulkanPresentationConfig

class VulkanFrameRenderCoordinator(
    private val context: Context,
) : FrameRenderCoordinator, EmulatorSurfaceView.SurfaceLifecycleListener {

    private data class PendingSurfaceConfig(
        val generation: Int,
        val config: VulkanPresentationConfig?,
    )

    private data class ManagedSurface(
        var surfaceId: Int = 0,
        var config: VulkanPresentationConfig? = null,
        var background: RuntimeBackground = RuntimeBackground.None,
        var generation: Int = 0,
        var pendingSurface: Surface? = null,
    )

    private data class PacingPermit(
        val permitId: Long,
        val controlEpoch: Long,
        var vsyncSequence: Long,
        var frameDeadlineNanos: Long,
        val waitForProduct: Boolean = false,
        var nativeWaitEpoch: Long? = null,
        var consumed: Boolean = false,
    )

    private val coordinatorScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val surfacesLock = Any()
    private val managedSurfaces = mutableMapOf<EmulatorSurfaceView, ManagedSurface>()
    private val pendingConfigs = mutableMapOf<EmulatorSurfaceView, PendingSurfaceConfig>()
    private val frameRenderThread = FrameRenderThread()
    @Volatile private var stopped = false

    init {
        frameRenderThread.start()
    }

    override fun addSurface(surface: EmulatorSurfaceView) {
        if (stopped) {
            return
        }
        synchronized(surfacesLock) {
            managedSurfaces.putIfAbsent(surface, ManagedSurface())
        }

        surface.setSurfaceLifecycleListener(this)

        val currentSurface = surface.getCurrentSurface()
        if (currentSurface != null) {
            val (width, height) = surface.getCurrentSurfaceSize()
            requestSurfaceAttachmentIfNeeded(surface, currentSurface, width, height)
            refreshSurfacePresentation(surface)
        }
    }

    override fun removeSurface(surface: EmulatorSurfaceView) {
        surface.setSurfaceLifecycleListener(null)
        if (stopped) {
            return
        }

        val surfaceId = synchronized(surfacesLock) {
            pendingConfigs.remove(surface)
            managedSurfaces.remove(surface)?.also {
                it.pendingSurface = null
            }?.surfaceId ?: 0
        }

        frameRenderThread.requestSurfaceDetachment(surfaceId)
    }

    override fun updateSurfacePresentation(
        surface: EmulatorSurfaceView,
        config: VulkanPresentationConfig?,
        background: RuntimeBackground,
    ) {
        if (stopped) {
            return
        }
        val generation = synchronized(surfacesLock) {
            val currentState = managedSurfaces.getOrPut(surface) { ManagedSurface() }
            currentState.config = config
            currentState.background = background
            currentState.generation += 1
            currentState.generation
        }

        val (surfaceId, currentSurface, currentWidth, currentHeight) = synchronized(surfacesLock) {
            val managedSurface = managedSurfaces[surface]
            val (width, height) = surface.getCurrentSurfaceSize()
            Quadruple(
                managedSurface?.surfaceId ?: 0,
                surface.getCurrentSurface(),
                width,
                height,
            )
        }

        if (surfaceId == 0 && currentSurface != null) {
            requestSurfaceAttachmentIfNeeded(surface, currentSurface, currentWidth, currentHeight)
        }

        coordinatorScope.launch {
            val decodedBitmap = withContext(Dispatchers.IO) {
                decodeBackgroundBitmap(background)
            }

            frameRenderThread.requestSurfaceConfiguration(surface, generation, config, decodedBitmap)
        }
    }

    override fun renderFrame(frameDeadlineNanos: Long?) {
        if (stopped) {
            return
        }
        frameRenderThread.requestFrameRender(frameDeadlineNanos)
    }

    override fun cancelPendingFrameRender() {
        frameRenderThread.cancelPendingFrameRender()
    }

    override fun stop() {
        if (stopped) {
            return
        }
        stopped = true
        val surfaces = synchronized(surfacesLock) {
            managedSurfaces.keys.toList()
        }
        surfaces.forEach {
            it.setSurfaceLifecycleListener(null)
        }
        coordinatorScope.cancel()
        frameRenderThread.requestStop()
        frameRenderThread.quitSafely()
        frameRenderThread.join()
    }

    override fun onSurfaceCreated(surfaceView: EmulatorSurfaceView, surface: Surface) {
        if (stopped) {
            return
        }
        val (width, height) = surfaceView.getCurrentSurfaceSize()
        requestSurfaceAttachmentIfNeeded(surfaceView, surface, width, height)
        refreshSurfacePresentation(surfaceView)
    }

    override fun onSurfaceChanged(surfaceView: EmulatorSurfaceView, surface: Surface, width: Int, height: Int) {
        if (stopped) {
            return
        }
        val surfaceId = synchronized(surfacesLock) {
            managedSurfaces[surfaceView]?.surfaceId ?: 0
        }

        if (surfaceId == 0) {
            requestSurfaceAttachmentIfNeeded(surfaceView, surface, width, height)
        } else {
            frameRenderThread.requestSurfaceResize(surfaceView, width, height)
        }
        refreshSurfacePresentation(surfaceView)
    }

    override fun onSurfaceDestroyed(surfaceView: EmulatorSurfaceView) {
        if (stopped) {
            return
        }
        val surfaceId = synchronized(surfacesLock) {
            pendingConfigs.remove(surfaceView)
            managedSurfaces[surfaceView]?.also {
                it.pendingSurface = null
            }?.surfaceId ?: 0
        }
        frameRenderThread.requestSurfaceDetachment(surfaceId)
        synchronized(surfacesLock) {
            managedSurfaces[surfaceView]?.let {
                it.surfaceId = 0
                it.pendingSurface = null
            }
        }
    }

    private fun requestSurfaceAttachmentIfNeeded(
        surfaceView: EmulatorSurfaceView,
        surface: Surface,
        width: Int,
        height: Int,
    ) {
        if (stopped) {
            return
        }
        val shouldAttach = synchronized(surfacesLock) {
            val managedSurface = managedSurfaces[surfaceView] ?: return@synchronized false
            if (managedSurface.surfaceId != 0 || managedSurface.pendingSurface != null) {
                false
            } else {
                managedSurface.pendingSurface = surface
                true
            }
        }

        if (shouldAttach) {
            frameRenderThread.requestSurfaceAttachment(surfaceView, surface, width, height)
        }
    }

    private fun refreshSurfacePresentation(surface: EmulatorSurfaceView) {
        if (stopped) {
            return
        }
        val currentState = synchronized(surfacesLock) {
            managedSurfaces[surface]?.copy()
        } ?: return

        updateSurfacePresentation(surface, currentState.config, currentState.background)
    }

    private suspend fun decodeBackgroundBitmap(background: RuntimeBackground): Bitmap? {
        val backgroundUri = background.background?.uri ?: return null
        val decodedBitmap = context.contentResolver.openInputStream(backgroundUri)?.use { stream ->
            BitmapFactory.decodeStream(stream, null, BitmapFactory.Options().apply {
                inPreferredConfig = Bitmap.Config.ARGB_8888
            })
        } ?: return null

        if (decodedBitmap.config == Bitmap.Config.ARGB_8888) {
            return decodedBitmap
        }

        val convertedBitmap = decodedBitmap.copy(Bitmap.Config.ARGB_8888, false)
        if (convertedBitmap !== decodedBitmap) {
            decodedBitmap.recycle()
        }
        return convertedBitmap
    }

    private inner class FrameRenderThread : HandlerThread("VulkanPresentThread") {
        @Volatile
        private var handler: Handler? = null
        @Volatile
        private var running = true
        private var cleanedUp = false
        private val renderStatistics = RenderStatistics()
        private val pacingLock = Any()
        private var pacingTransitionCount = 0
        private var controlEpoch = 1L
        private var nextPermitId = 1L
        private var nextVsyncSequence = 1L
        private var pendingPacingPermit: PacingPermit? = null
        private var renderMessageQueued = false

        override fun onLooperPrepared() {
            handler = object : Handler(looper) {
                override fun handleMessage(msg: Message) {
                    when (msg.what) {
                        MSG_ATTACH_SURFACE -> attachSurface(
                            msg.obj as EmulatorSurfaceView,
                            msg.data.parcelable(MSG_SURFACE),
                            msg.data.getInt(MSG_WIDTH),
                            msg.data.getInt(MSG_HEIGHT),
                        )
                        MSG_RESIZE_SURFACE -> resizeSurface(
                            msg.obj as EmulatorSurfaceView,
                            msg.data.getInt(MSG_WIDTH),
                            msg.data.getInt(MSG_HEIGHT),
                        )
                        MSG_CONFIGURE_SURFACE -> configureSurface(
                            msg.obj as EmulatorSurfaceView,
                            msg.data.getInt(MSG_GENERATION),
                            msg.data.parcelable(MSG_BACKGROUND_BITMAP),
                        )
                        MSG_DETACH_SURFACE -> detachSurface(msg.arg1)
                        MSG_RENDER_FRAME -> pumpPresentation()
                        MSG_STOP -> stopThread()
                    }
                }
            }

            val pendingSurfaces = synchronized(surfacesLock) {
                managedSurfaces.mapNotNull { (surfaceView, managedSurface) ->
                    managedSurface.pendingSurface?.let { surfaceView to it }
                }
            }
            pendingSurfaces.forEach { (surfaceView, surface) ->
                val (width, height) = surfaceView.getCurrentSurfaceSize()
                attachSurface(surfaceView, surface, width, height)
            }
        }

        private fun getActiveHandler(): Handler? {
            val currentHandler = handler ?: return null
            if (currentHandler.looper.thread.isAlive) {
                return currentHandler
            }

            if (handler === currentHandler) {
                handler = null
            }
            return null
        }

        private fun advanceControlEpochLocked() {
            controlEpoch += 1L
            pendingPacingPermit = null
            renderMessageQueued = false
        }

        private fun invalidatePendingPacing(currentHandler: Handler?) {
            synchronized(pacingLock) {
                advanceControlEpochLocked()
            }
            currentHandler?.removeMessages(MSG_RENDER_FRAME)

            MelonEmulator.cancelVulkanPresentationWaits()
        }

        private fun beginPacingTransition(currentHandler: Handler) {
            synchronized(pacingLock) {
                pacingTransitionCount += 1
                advanceControlEpochLocked()
            }
            currentHandler.removeMessages(MSG_RENDER_FRAME)
            MelonEmulator.cancelVulkanPresentationWaits()
        }

        private fun endPacingTransition() {
            synchronized(pacingLock) {
                pacingTransitionCount = (pacingTransitionCount - 1).coerceAtLeast(0)
            }
        }

        private inline fun queueAfterPacingTransition(
            currentHandler: Handler,
            queueMessage: () -> Unit,
        ) {
            beginPacingTransition(currentHandler)
            try {
                queueMessage()
            } finally {
                endPacingTransition()
            }
        }

        fun cancelPendingFrameRender() {
            invalidatePendingPacing(getActiveHandler())
        }

        fun requestSurfaceAttachment(surfaceView: EmulatorSurfaceView, surface: Surface, width: Int, height: Int) {
            if (!running) {
                return
            }
            val currentHandler = getActiveHandler() ?: return
            queueAfterPacingTransition(currentHandler) {
                currentHandler.obtainMessage(MSG_ATTACH_SURFACE, surfaceView).also {
                    it.data = bundleOf(
                        MSG_SURFACE to surface,
                        MSG_WIDTH to width,
                        MSG_HEIGHT to height,
                    )
                    try {
                        currentHandler.sendMessage(it)
                    } catch (_: IllegalStateException) {
                        if (handler === currentHandler) {
                            currentHandler.removeCallbacksAndMessages(null)
                            handler = null
                        }
                        it.recycle()
                    }
                }
            }
        }

        fun requestSurfaceResize(surfaceView: EmulatorSurfaceView, width: Int, height: Int) {
            if (!running) {
                return
            }
            val currentHandler = getActiveHandler() ?: return
            queueAfterPacingTransition(currentHandler) {
                currentHandler.obtainMessage(MSG_RESIZE_SURFACE, surfaceView).also {
                    it.data = bundleOf(
                        MSG_WIDTH to width,
                        MSG_HEIGHT to height,
                    )
                    try {
                        currentHandler.sendMessage(it)
                    } catch (_: IllegalStateException) {
                        if (handler === currentHandler) {
                            currentHandler.removeCallbacksAndMessages(null)
                            handler = null
                        }
                        it.recycle()
                    }
                }
            }
        }

        fun requestSurfaceConfiguration(
            surfaceView: EmulatorSurfaceView,
            generation: Int,
            config: VulkanPresentationConfig?,
            backgroundBitmap: Bitmap?,
        ) {
            if (!running) {
                backgroundBitmap?.recycle()
                return
            }
            val shouldQueueMessage = synchronized(surfacesLock) {
                val currentPending = pendingConfigs[surfaceView]
                if (currentPending != null && generation < currentPending.generation) {
                    false
                } else {
                    pendingConfigs[surfaceView] = PendingSurfaceConfig(generation, config)
                    true
                }
            }

            if (!shouldQueueMessage) {
                backgroundBitmap?.recycle()
                return
            }

            val currentHandler = getActiveHandler()
            if (currentHandler == null) {
                backgroundBitmap?.recycle()
                return
            }

            queueAfterPacingTransition(currentHandler) {
                currentHandler.obtainMessage(MSG_CONFIGURE_SURFACE, surfaceView).also {
                    it.data = bundleOf(
                        MSG_GENERATION to generation,
                        MSG_HAS_CONFIG to (config != null),
                        MSG_BACKGROUND_BITMAP to backgroundBitmap,
                    )
                    try {
                        currentHandler.sendMessage(it)
                    } catch (_: IllegalStateException) {
                        if (handler === currentHandler) {
                            currentHandler.removeCallbacksAndMessages(null)
                            handler = null
                        }
                        backgroundBitmap?.recycle()
                        it.recycle()
                    }
                }
            }
        }

        fun requestSurfaceDetachment(surfaceId: Int) {
            if (!running) {
                return
            }
            if (surfaceId == 0) {
                cancelPendingFrameRender()
                return
            }
            val currentHandler = getActiveHandler() ?: return
            queueAfterPacingTransition(currentHandler) {
                currentHandler.obtainMessage(MSG_DETACH_SURFACE, surfaceId, 0).also {
                    try {
                        currentHandler.sendMessage(it)
                    } catch (_: IllegalStateException) {
                        if (handler === currentHandler) {
                            currentHandler.removeCallbacksAndMessages(null)
                            handler = null
                        }
                        it.recycle()
                    }
                }
            }
        }

        fun requestFrameRender(frameDeadlineNanos: Long?, expectedControlEpoch: Long? = null) {
            if (!running) {
                return
            }
            val currentHandler = getActiveHandler() ?: return
            val shouldQueueMessage = synchronized(pacingLock) {
                if (!running || pacingTransitionCount != 0
                    || (expectedControlEpoch != null && expectedControlEpoch != controlEpoch)
                ) {
                    false
                } else {
                    val vsyncSequence = nextVsyncSequence++
                    val currentPermit = pendingPacingPermit
                    if (currentPermit == null
                        || currentPermit.consumed
                        || currentPermit.controlEpoch != controlEpoch
                    ) {
                        pendingPacingPermit = PacingPermit(
                            permitId = nextPermitId++,
                            controlEpoch = controlEpoch,
                            vsyncSequence = vsyncSequence,
                            frameDeadlineNanos = frameDeadlineNanos ?: 0L,
                            waitForProduct = expectedControlEpoch != null,
                        )
                    } else {
                        currentPermit.vsyncSequence = vsyncSequence
                        currentPermit.frameDeadlineNanos = frameDeadlineNanos ?: 0L
                    }

                    if (renderMessageQueued) {
                        false
                    } else {
                        renderMessageQueued = true
                        true
                    }
                }
            }
            if (!shouldQueueMessage) {
                return
            }

            currentHandler.obtainMessage(MSG_RENDER_FRAME).also {
                try {
                    if (!currentHandler.sendMessage(it)) {
                        synchronized(pacingLock) {
                            renderMessageQueued = false
                            pendingPacingPermit = null
                        }
                    }
                } catch (_: IllegalStateException) {
                    synchronized(pacingLock) {
                        renderMessageQueued = false
                        pendingPacingPermit = null
                    }
                    if (handler === currentHandler) {
                        currentHandler.removeCallbacksAndMessages(null)
                        handler = null
                    }
                    it.recycle()
                }
            }
        }

        fun requestStop() {
            running = false
            val currentHandler = getActiveHandler()
            invalidatePendingPacing(currentHandler)
            if (currentHandler == null) {
                return
            }
            try {
                currentHandler.sendMessageAtFrontOfQueue(Message.obtain(currentHandler, MSG_STOP))
            } catch (_: IllegalStateException) {
                if (handler === currentHandler) {
                    currentHandler.removeCallbacksAndMessages(null)
                    handler = null
                }
            }
        }

        private fun startPresentationPump(): PacingPermit? = synchronized(pacingLock) {
            renderMessageQueued = false
            pendingPacingPermit?.takeIf {
                running
                    && pacingTransitionCount == 0
                    && !it.consumed
                    && it.controlEpoch == controlEpoch
            }?.copy()
        }

        private fun currentPacingPermit(permitId: Long, expectedControlEpoch: Long): PacingPermit? =
            synchronized(pacingLock) {
                pendingPacingPermit?.takeIf {
                    running
                        && pacingTransitionCount == 0
                        && !it.consumed
                        && it.permitId == permitId
                        && it.controlEpoch == expectedControlEpoch
                        && controlEpoch == expectedControlEpoch
                }?.copy()
            }

        private fun bindNativeWaitEpoch(permitId: Long, expectedControlEpoch: Long): Boolean {
            val capturedWaitEpoch = MelonEmulator.captureVulkanPresentationWaitEpoch()
            return synchronized(pacingLock) {
                val currentPermit = pendingPacingPermit
                if (!running
                    || pacingTransitionCount != 0
                    || currentPermit == null
                    || currentPermit.consumed
                    || currentPermit.permitId != permitId
                    || currentPermit.controlEpoch != expectedControlEpoch
                    || controlEpoch != expectedControlEpoch
                ) {
                    false
                } else if (currentPermit.nativeWaitEpoch != null
                    && currentPermit.nativeWaitEpoch != capturedWaitEpoch
                ) {
                    pendingPacingPermit = null
                    false
                } else {
                    currentPermit.nativeWaitEpoch = capturedWaitEpoch
                    true
                }
            }
        }

        private fun finishPacingPermit(
            permitId: Long,
            expectedControlEpoch: Long,
            consumed: Boolean,
        ) {
            synchronized(pacingLock) {
                val currentPermit = pendingPacingPermit
                if (currentPermit?.permitId == permitId
                    && currentPermit.controlEpoch == expectedControlEpoch
                    && controlEpoch == expectedControlEpoch
                ) {
                    currentPermit.consumed = consumed
                    pendingPacingPermit = null
                }
            }
        }

        private fun pumpPresentation() {
            val initialPermit = startPresentationPump() ?: return
            if (!bindNativeWaitEpoch(initialPermit.permitId, initialPermit.controlEpoch)) {
                return
            }

            if (initialPermit.waitForProduct) {
                val permit = currentPacingPermit(initialPermit.permitId, initialPermit.controlEpoch) ?: return
                val waitEpoch = permit.nativeWaitEpoch ?: return
                if (MelonEmulator.waitForVulkanPresentationProduct(waitEpoch, PRODUCT_WAIT_TIMEOUT_NS)
                    != MelonEmulator.VulkanPresentationWaitResult.PRODUCT_READY
                ) {
                    return
                }
            }

            var alreadyRetriedAfterProductWake = false
            while (running) {
                val permit = currentPacingPermit(
                    initialPermit.permitId,
                    initialPermit.controlEpoch,
                ) ?: return
                val deadline = permit.frameDeadlineNanos.takeIf { it > 0L } ?: 0L
                val budgetDeadline = if (deadline > 0L) {
                    (deadline - renderStatistics.getPresentationBudgetMarginNs()).coerceAtLeast(0L)
                } else {
                    0L
                }

                val renderStartNs = System.nanoTime()
                val waitEpoch = permit.nativeWaitEpoch ?: run {
                    finishPacingPermit(
                        permit.permitId,
                        permit.controlEpoch,
                        consumed = false,
                    )
                    return
                }
                val presentationResult = MelonEmulator.presentVulkanFrame(
                    deadline,
                    budgetDeadline,
                    waitEpoch,
                )
                renderStatistics.trackRenderEvent(System.nanoTime() - renderStartNs)
                if (currentPacingPermit(permit.permitId, permit.controlEpoch) == null) {
                    return
                }

                when (presentationResult) {
                    MelonEmulator.VulkanPresentationResult.PRESENTED,
                    MelonEmulator.VulkanPresentationResult.NO_SURFACE,
                    -> {
                        finishPacingPermit(permit.permitId, permit.controlEpoch, consumed = true)
                        if (MelonEmulator.isLowLatencyEnabled
                            && presentationResult == MelonEmulator.VulkanPresentationResult.PRESENTED
                            && !MelonEmulator.isFastForwardEnabled()
                        ) {
                            requestFrameRender(null, permit.controlEpoch)
                        }
                        return
                    }

                    MelonEmulator.VulkanPresentationResult.NO_PRODUCT -> {
                        if (alreadyRetriedAfterProductWake) {
                            return
                        }
                        val waitEpoch = permit.nativeWaitEpoch ?: run {
                            finishPacingPermit(permit.permitId, permit.controlEpoch, consumed = false)
                            return
                        }
                        val waitResult = MelonEmulator.waitForVulkanPresentationProduct(
                            waitEpoch,
                            PRODUCT_WAIT_TIMEOUT_NS,
                        )
                        if (currentPacingPermit(permit.permitId, permit.controlEpoch) == null) {
                            return
                        }
                        when (waitResult) {
                            MelonEmulator.VulkanPresentationWaitResult.PRODUCT_READY -> {
                                alreadyRetriedAfterProductWake = true
                                continue
                            }
                            MelonEmulator.VulkanPresentationWaitResult.TIMED_OUT -> return
                            MelonEmulator.VulkanPresentationWaitResult.GENERATION_CHANGED,
                            MelonEmulator.VulkanPresentationWaitResult.STOPPED,
                            -> {
                                finishPacingPermit(permit.permitId, permit.controlEpoch, consumed = false)
                                return
                            }
                        }
                    }

                    MelonEmulator.VulkanPresentationResult.GPU_NOT_READY,
                    MelonEmulator.VulkanPresentationResult.WSI_NOT_READY,
                    MelonEmulator.VulkanPresentationResult.RECOVERABLE_SURFACE_ERROR,
                    -> return

                    MelonEmulator.VulkanPresentationResult.GENERATION_CHANGED,
                    MelonEmulator.VulkanPresentationResult.STOPPED,
                    MelonEmulator.VulkanPresentationResult.FATAL_ERROR,
                    -> {
                        finishPacingPermit(permit.permitId, permit.controlEpoch, consumed = false)
                        return
                    }
                }
            }
        }

        private fun attachSurface(surfaceView: EmulatorSurfaceView, surface: Surface?, width: Int, height: Int) {
            if (!running) {
                return
            }
            if (surface == null) {
                return
            }

            val shouldAttach = synchronized(surfacesLock) {
                val managedSurface = managedSurfaces[surfaceView]
                managedSurface != null
                    && managedSurface.surfaceId == 0
                    && managedSurface.pendingSurface === surface
                    && surfaceView.getCurrentSurface() === surface
            }
            if (!shouldAttach) {
                return
            }

            val surfaceId = MelonEmulator.attachVulkanSurface(surface, width, height)
            var shouldRefreshPresentation = false
            var staleSurfaceId = 0
            var immediateConfig: VulkanPresentationConfig? = null
            synchronized(surfacesLock) {
                val managedSurface = managedSurfaces[surfaceView]
                if (managedSurface == null || managedSurface.pendingSurface !== surface || surfaceView.getCurrentSurface() !== surface) {
                    staleSurfaceId = surfaceId
                } else {
                    managedSurface.pendingSurface = null
                    managedSurface.surfaceId = surfaceId
                    immediateConfig = managedSurface.config
                    shouldRefreshPresentation = surfaceId != 0
                }
            }

            if (staleSurfaceId != 0) {
                MelonEmulator.detachVulkanSurface(staleSurfaceId)
                return
            }

            if (surfaceId != 0 && immediateConfig != null) {
                // Apply the latest layout immediately on attach so the presenter
                // becomes configured even before the async background decode path
                // finishes or a later UI refresh happens.
                MelonEmulator.configureVulkanSurface(surfaceId, immediateConfig!!, null)
            }

            if (shouldRefreshPresentation) {
                refreshSurfacePresentation(surfaceView)
            }
        }

        private fun resizeSurface(surfaceView: EmulatorSurfaceView, width: Int, height: Int) {
            if (!running) {
                return
            }
            val surfaceId = synchronized(surfacesLock) {
                managedSurfaces[surfaceView]?.surfaceId ?: 0
            }

            if (surfaceId != 0) {
                MelonEmulator.resizeVulkanSurface(surfaceId, width, height)
            }
        }

        private fun configureSurface(surfaceView: EmulatorSurfaceView, generation: Int, backgroundBitmap: Bitmap?) {
            if (!running) {
                backgroundBitmap?.recycle()
                return
            }
            val (surfaceId, currentGeneration) = synchronized(surfacesLock) {
                val managedSurface = managedSurfaces[surfaceView]
                (managedSurface?.surfaceId ?: 0) to (managedSurface?.generation ?: -1)
            }

            val pendingConfig = synchronized(surfacesLock) {
                pendingConfigs[surfaceView]
            }

            if (surfaceId == 0 || currentGeneration != generation || pendingConfig?.generation != generation) {
                backgroundBitmap?.recycle()
                return
            }

            synchronized(surfacesLock) {
                val currentPending = pendingConfigs[surfaceView]
                if (currentPending?.generation == generation) {
                    pendingConfigs.remove(surfaceView)
                }
            }

            val config = pendingConfig.config
            if (config == null) {
                backgroundBitmap?.recycle()
                return
            }

            MelonEmulator.configureVulkanSurface(surfaceId, config, backgroundBitmap)
            backgroundBitmap?.recycle()
        }

        private fun detachSurface(surfaceId: Int) {
            if (!running && cleanedUp) {
                return
            }
            MelonEmulator.detachVulkanSurface(surfaceId)
        }

        private fun stopThread() {
            if (cleanedUp) {
                return
            }
            cleanedUp = true
            running = false
            val currentHandler = handler
            currentHandler?.removeCallbacksAndMessages(null)
            handler = null

            val surfaceIds = synchronized(surfacesLock) {
                managedSurfaces.values.mapNotNull { it.surfaceId.takeIf { id -> id != 0 } }
            }

            surfaceIds.forEach {
                MelonEmulator.detachVulkanSurface(it)
            }
            synchronized(surfacesLock) {
                managedSurfaces.values.forEach {
                    it.surfaceId = 0
                    it.pendingSurface = null
                }
                pendingConfigs.clear()
            }
        }
    }

    private class RenderStatistics {
        private var estimatedRenderDurationNs = 0L

        fun trackRenderEvent(durationNs: Long) {
            val clampedDurationNs = durationNs.coerceAtLeast(0L)
            estimatedRenderDurationNs = if (estimatedRenderDurationNs == 0L) {
                clampedDurationNs
            } else {
                ((estimatedRenderDurationNs * 7L) + clampedDurationNs) / 8L
            }
        }

        fun getPresentationBudgetMarginNs(): Long {
            val dynamicMarginNs = if (estimatedRenderDurationNs > 0L) {
                (estimatedRenderDurationNs * 5L) / 4L
            } else {
                DEFAULT_PRESENTATION_MARGIN_NS
            }

            return dynamicMarginNs.coerceIn(MIN_PRESENTATION_MARGIN_NS, MAX_PRESENTATION_MARGIN_NS)
        }

        private companion object {
            const val MIN_PRESENTATION_MARGIN_NS = 500_000L
            const val DEFAULT_PRESENTATION_MARGIN_NS = 1_000_000L
            const val MAX_PRESENTATION_MARGIN_NS = 2_000_000L
        }
    }

    private companion object {
        const val MSG_ATTACH_SURFACE = 1
        const val MSG_RESIZE_SURFACE = 2
        const val MSG_CONFIGURE_SURFACE = 3
        const val MSG_DETACH_SURFACE = 4
        const val MSG_RENDER_FRAME = 5
        const val MSG_STOP = 6

        const val MSG_SURFACE = "surface"
        const val MSG_WIDTH = "width"
        const val MSG_HEIGHT = "height"
        const val MSG_GENERATION = "generation"
        const val MSG_HAS_CONFIG = "has-config"
        const val MSG_BACKGROUND_BITMAP = "background-bitmap"

        const val PRODUCT_WAIT_TIMEOUT_NS = 50_000_000L
    }
}

private data class Quadruple<out A, out B, out C, out D>(
    val first: A,
    val second: B,
    val third: C,
    val fourth: D,
)
