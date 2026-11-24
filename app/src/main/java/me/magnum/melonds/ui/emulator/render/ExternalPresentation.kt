package me.magnum.melonds.ui.emulator.render

import android.app.Presentation
import android.app.Activity
import android.graphics.Color
import android.os.Build
import android.view.Display
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.core.view.isVisible
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ViewModelStoreOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.lifecycle.setViewTreeViewModelStoreOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import me.magnum.melonds.domain.model.RuntimeBackground
import me.magnum.melonds.domain.model.VideoFiltering
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.ui.emulator.DSRenderer
import me.magnum.melonds.ui.emulator.EmulatorSurfaceView
import me.magnum.melonds.ui.emulator.RuntimeLayoutView
import me.magnum.melonds.ui.emulator.model.RuntimeInputLayoutConfiguration
import me.magnum.melonds.ui.emulator.model.RuntimeRendererConfiguration
import me.magnum.melonds.ui.emulator.model.VulkanPresentationConfig
import me.magnum.melonds.ui.layouteditor.model.LayoutTarget
import kotlin.collections.orEmpty
import kotlin.math.max

class ExternalPresentation(
    context: Activity,
    display: Display,
    private val frameRenderCoordinator: FrameRenderCoordinator,
    private val usePanelActivity: Boolean,
    private var excludeTouchScreenFromSystemGestures: Boolean,
) : Presentation(context, display) {
    private val ownerContext = context
    private val hostContentInActivity = display.displayId == Display.DEFAULT_DISPLAY && display.flags and Display.FLAG_PRESENTATION == 0

    private data class ScreenPresentationAreas(
        val topScreenRect: me.magnum.melonds.domain.model.Rect?,
        val bottomScreenRect: me.magnum.melonds.domain.model.Rect?,
        val topAlpha: Float,
        val bottomAlpha: Float,
        val topOnTop: Boolean,
        val bottomOnTop: Boolean,
        val hybridTopScreenRect: me.magnum.melonds.domain.model.Rect?,
        val hybridBottomScreenRect: me.magnum.melonds.domain.model.Rect?,
        val hybridAlpha: Float,
        val hybridOnTop: Boolean,
    )


    val layoutView = RuntimeLayoutView(context)
    private val container = FrameLayout(context)
    private val pauseOverlay = View(context)
    private val infoOverlayContent = androidx.compose.runtime.mutableStateOf<(@androidx.compose.runtime.Composable () -> Unit)?>(null)
    private val infoOverlayView = androidx.compose.ui.platform.ComposeView(context)
    private val emulatorRenderer: DSRenderer
    private val surfaceView: EmulatorSurfaceView
    private var currentBackground: RuntimeBackground? = null
    private var currentRendererConfiguration: RuntimeRendererConfiguration? = null

    init {
        window?.setFlags(
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
        )

        val layoutChangeListener = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            updateRendererScreenAreas()
        }

        container.addOnLayoutChangeListener(layoutChangeListener)
        layoutView.addOnLayoutChangeListener(layoutChangeListener)
        emulatorRenderer = DSRenderer(context).also {
            surfaceView = createSurfaceView(it)
        }
        surfaceView.updateRendererConfiguration(currentRendererConfiguration)

        container.addView(surfaceView)
        container.addView(layoutView)
        container.addView(pauseOverlay)
        container.addView(infoOverlayView)

        infoOverlayView.setContent {
            me.magnum.melonds.ui.theme.MelonTheme(isDarkTheme = true) {
                infoOverlayContent.value?.invoke()
            }
        }

        pauseOverlay.apply {
            setBackgroundColor(Color.BLACK)
            alpha = 0.6f
            isVisible = false
            setOnClickListener {
                // Do nothing. Just intercept clicks
            }
        }

        frameRenderCoordinator.addSurface(surfaceView)

        (context as? LifecycleOwner)?.let { owner ->
            container.setViewTreeLifecycleOwner(owner)
        }
        (context as? ViewModelStoreOwner)?.let { owner ->
            container.setViewTreeViewModelStoreOwner(owner)
        }
        (context as? SavedStateRegistryOwner)?.let { owner ->
            container.setViewTreeSavedStateRegistryOwner(owner)
        }

        if (!hostContentInActivity) setContentView(container)
    }

    fun swapScreens() {
        layoutView.swapScreens()
        updateRendererScreenAreas()
    }

    fun updateRendererScreenAreas() {
        val areas = resolveScreenPresentationAreas()
        emulatorRenderer.updateScreenAreas(
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
        )

        frameRenderCoordinator.updateSurfacePresentation(
            surfaceView,
            buildVulkanPresentationConfig(
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
            ),
            currentBackground ?: RuntimeBackground.None,
        )

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && container.isAttachedToWindow) {
            val touchScreenArea = if (excludeTouchScreenFromSystemGestures) {
                listOfNotNull(areas.bottomScreenRect, areas.hybridBottomScreenRect).map {
                    android.graphics.Rect(it.x, it.y, it.right, it.bottom)
                }
            } else null
            layoutView.systemGestureExclusionRects = touchScreenArea.orEmpty()
        }
    }

    private fun resolveScreenPresentationAreas(): ScreenPresentationAreas {
        val (topScreen, bottomScreen) = if (layoutView.areScreensSwapped()) {
            LayoutComponent.BOTTOM_SCREEN to LayoutComponent.TOP_SCREEN
        } else {
            LayoutComponent.TOP_SCREEN to LayoutComponent.BOTTOM_SCREEN
        }
        val topView = layoutView.getLayoutComponentView(topScreen)
        val bottomView = layoutView.getLayoutComponentView(bottomScreen)
        val hybridView = layoutView.getLayoutComponentView(LayoutComponent.HYBRID_SCREEN)
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

    private fun splitHybridScreenRect(rect: me.magnum.melonds.domain.model.Rect): Pair<me.magnum.melonds.domain.model.Rect, me.magnum.melonds.domain.model.Rect> {
        val topHeight = max(1, rect.height / 2)
        val bottomHeight = max(1, rect.height - topHeight)
        return me.magnum.melonds.domain.model.Rect(rect.x, rect.y, rect.width, topHeight) to
                me.magnum.melonds.domain.model.Rect(rect.x, rect.y + topHeight, rect.width, bottomHeight)
    }

    fun setTouchScreenSystemGestureExclusionEnabled(enabled: Boolean) {
        excludeTouchScreenFromSystemGestures = enabled
        updateRendererScreenAreas()
    }

    fun setInfoOverlayContent(content: (@androidx.compose.runtime.Composable () -> Unit)?) {
        infoOverlayContent.value = content
    }

    fun setPauseOverlayVisibility(visible: Boolean) {
        pauseOverlay.isVisible = visible
    }

    private fun createSurfaceView(renderer: EmulatorRenderer): EmulatorSurfaceView {
        return EmulatorSurfaceView(context).apply {
            setRenderer(renderer)
            isFocusable = false
            isFocusableInTouchMode = false
        }
    }

    fun updateLayout(layoutConfiguration: RuntimeInputLayoutConfiguration) {
        layoutView.instantiateLayout(layoutConfiguration, LayoutTarget.SECONDARY_SCREEN)
        updateRendererScreenAreas()
    }

    fun updateRendererConfiguration(newRendererConfiguration: RuntimeRendererConfiguration?) {
        currentRendererConfiguration = newRendererConfiguration
        surfaceView.updateRendererConfiguration(newRendererConfiguration)
        updateRendererScreenAreas()
    }

    private var panelSession: GamePanelActivity.Session? = null

    fun showOnDisplay(): Boolean {
        if (hostContentInActivity) {
            panelSession = GamePanelActivity.open(ownerContext, display, container)
            if (panelSession == null) {
                frameRenderCoordinator.removeSurface(surfaceView)
                return false
            }
        } else {
            show()
        }
        return true
    }

    override fun dismiss() {
        if (hostContentInActivity) {
            panelSession?.close()
            panelSession = null
            frameRenderCoordinator.removeSurface(surfaceView)
        } else {
            super.dismiss()
        }
    }

    override fun onStart() {
        super.onStart()
        if (usePanelActivity) {
            panelSession = GamePanelActivity.open(ownerContext, display)
        }
        layoutView.post {
            updateRendererScreenAreas()
        }
    }

    override fun onStop() {
        super.onStop()
        panelSession?.close()
        panelSession = null
        frameRenderCoordinator.removeSurface(surfaceView)
    }

    fun updateBackground(background: RuntimeBackground) {
        currentBackground = background
        emulatorRenderer.setBackground(background)
        updateRendererScreenAreas()
    }

    private fun buildVulkanPresentationConfig(
        topScreenRect: me.magnum.melonds.domain.model.Rect?,
        bottomScreenRect: me.magnum.melonds.domain.model.Rect?,
        topAlpha: Float,
        bottomAlpha: Float,
        topOnTop: Boolean,
        bottomOnTop: Boolean,
        hybridTopScreenRect: me.magnum.melonds.domain.model.Rect?,
        hybridBottomScreenRect: me.magnum.melonds.domain.model.Rect?,
        hybridAlpha: Float,
        hybridOnTop: Boolean,
    ): VulkanPresentationConfig? {
        val rendererConfiguration = currentRendererConfiguration ?: return null
        if (rendererConfiguration.renderer != VideoRenderer.VULKAN) {
            return null
        }

        val (surfaceWidth, surfaceHeight) = surfaceView.getCurrentSurfaceSize()
        val (resolvedTopScreenRect, resolvedBottomScreenRect) = resolveVulkanScreenRects(
            topScreenRect = topScreenRect,
            bottomScreenRect = bottomScreenRect,
            surfaceWidth = if (surfaceWidth > 0) surfaceWidth else surfaceView.width,
            surfaceHeight = if (surfaceHeight > 0) surfaceHeight else surfaceView.height,
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
            backgroundMode = currentBackground?.mode ?: RuntimeBackground.None.mode,
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
        topScreenRect: me.magnum.melonds.domain.model.Rect?,
        bottomScreenRect: me.magnum.melonds.domain.model.Rect?,
        surfaceWidth: Int,
        surfaceHeight: Int,
    ): Pair<me.magnum.melonds.domain.model.Rect?, me.magnum.melonds.domain.model.Rect?> {
        val sanitizedTopRect = topScreenRect?.takeIf { it.width > 0 && it.height > 0 }
        val sanitizedBottomRect = bottomScreenRect?.takeIf { it.width > 0 && it.height > 0 }
        if (surfaceWidth <= 0 || surfaceHeight <= 0)
            return null to null
        return sanitizedTopRect to sanitizedBottomRect
    }
}
