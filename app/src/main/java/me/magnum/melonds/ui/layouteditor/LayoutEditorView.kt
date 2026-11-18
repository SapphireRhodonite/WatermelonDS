package me.magnum.melonds.ui.layouteditor

import android.content.Context
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import me.magnum.melonds.domain.model.*
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.domain.model.layout.PositionedLayoutComponent
import me.magnum.melonds.domain.model.layout.VirtualButtonMode
import me.magnum.melonds.domain.model.layout.UILayout
import me.magnum.melonds.ui.common.LayoutComponentView
import me.magnum.melonds.ui.common.LayoutView
import me.magnum.melonds.ui.layouteditor.model.LayoutComponentPositionEditorState
import me.magnum.melonds.ui.layouteditor.model.LayoutTarget
import me.magnum.melonds.impl.dpToPixels
import kotlin.math.*

typealias ViewSelectedListener = (
                                  view: LayoutComponentView,
                                  widthScale: Float,
                                  heightScale: Float,
                                  maxWidth: Int,
                                  maxHeight: Int,
                                  minSize: Int
) -> Unit

class LayoutEditorView(context: Context, attrs: AttributeSet?) : LayoutView(context, attrs) {
    enum class Anchor {
        TOP_LEFT,
        TOP_RIGHT,
        BOTTOM_LEFT,
        BOTTOM_RIGHT
    }

    private var onViewSelectedListener: ViewSelectedListener? = null
    private var onViewDeselectedListener: ((LayoutComponentView) -> Unit)? = null
    private var otherClickListener: OnClickListener? = null
    private val defaultComponentWidth by lazy { context.dpToPixels(100f).toInt() }
    private val minComponentSize by lazy { context.dpToPixels(30f).toInt() }
    private var selectedView: LayoutComponentView? = null
    private var selectedViewAnchor = Anchor.TOP_LEFT
    private var modifiedByUser = false
    private var onLayoutChangedListener: ((List<PositionedLayoutComponent>, Int, Int) -> Unit)? = null
    private var onViewPositionEditRequestedListener: ((LayoutComponentPositionEditorState) -> Unit)? = null

    init {
        super.setOnClickListener {
            if (selectedView != null) {
                deselectCurrentView()
            } else {
                otherClickListener?.onClick(it)
            }
        }
    }

    override fun instantiateLayout(layoutConfiguration: UILayout, layoutTarget: LayoutTarget) {
        selectedView = null
        super.instantiateLayout(layoutConfiguration, layoutTarget)
        modifiedByUser = false
        notifyLayoutChanged()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (w > 0 && h > 0 && (w != oldw || h != oldh)) {
            notifyLayoutChanged()
        }
    }

    fun destroyEditorLayout() {
        super.destroyLayout()
        notifyLayoutChanged()
    }

    fun setOnViewSelectedListener(listener: ViewSelectedListener) {
        onViewSelectedListener = listener
    }

    fun setOnViewDeselectedListener(listener: (LayoutComponentView) -> Unit) {
        onViewDeselectedListener = listener
    }

    fun setOnLayoutChangedListener(listener: ((List<PositionedLayoutComponent>, Int, Int) -> Unit)?) {
        onLayoutChangedListener = listener
    }

    fun setOnViewPositionEditRequestedListener(listener: ((LayoutComponentPositionEditorState) -> Unit)?) {
        onViewPositionEditRequestedListener = listener
    }

    fun getSelectedComponent(): LayoutComponent? {
        return selectedView?.component
    }

    fun selectComponent(component: LayoutComponent): Boolean {
        val componentView = views[component] ?: return false
        selectView(componentView)
        componentView.view.alpha = 1f
        componentView.setHighlighted(true)
        return true
    }

    fun addLayoutComponent(component: LayoutComponent) {
        val componentBuilder = viewBuilderFactory.getLayoutComponentViewBuilder(component)
        val componentHeight = defaultComponentWidth / componentBuilder.getAspectRatio()
        val componentView = addPositionedLayoutComponent(PositionedLayoutComponent(Rect(0, 0, defaultComponentWidth, componentHeight.toInt()), component))
        views[component] = componentView
        modifiedByUser = true
        notifyLayoutChanged()
    }

    fun isModifiedByUser(): Boolean {
        return modifiedByUser
    }

    fun buildCurrentLayout(): List<PositionedLayoutComponent> {
        return views.values.map {
            PositionedLayoutComponent(it.getRect(), it.component, it.baseAlpha, it.onTop, it.buttonMode, it.screenAspectRatio)
        }
    }

    fun handleKeyDown(event: KeyEvent): Boolean {
        val currentlySelectedView = selectedView ?: return false

        val stepX = (width / 100f).coerceAtLeast(3f)
        val stepY = (height / 100f).coerceAtLeast(3f)
        when (event.keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> dragView(currentlySelectedView, 0f, -stepY)
            KeyEvent.KEYCODE_DPAD_DOWN -> dragView(currentlySelectedView, 0f, stepY)
            KeyEvent.KEYCODE_DPAD_LEFT -> dragView(currentlySelectedView, -stepX, 0f)
            KeyEvent.KEYCODE_DPAD_RIGHT -> dragView(currentlySelectedView, stepX, 0f)
            else -> return false
        }

        return true
    }

    fun hasSelectedComponent(): Boolean = selectedView != null

    fun deselectComponent() {
        deselectCurrentView()
    }

    fun cycleSelectedComponent(forward: Boolean): Boolean {
        val ordered = views.values.toList()
        if (ordered.isEmpty()) {
            return false
        }

        val currentIndex = selectedView?.let { ordered.indexOf(it) } ?: -1
        val nextIndex = when {
            currentIndex < 0 -> if (forward) 0 else ordered.lastIndex
            else -> ((currentIndex + if (forward) 1 else -1) + ordered.size) % ordered.size
        }
        val next = ordered[nextIndex]

        selectedView?.takeIf { it != next }?.let {
            it.view.alpha = 0.5f
            it.setHighlighted(false)
        }
        selectView(next)
        next.view.alpha = 1f
        next.setHighlighted(true)
        return true
    }

    override fun setOnClickListener(l: OnClickListener?) {
        otherClickListener = l
    }

    override fun onLayoutComponentViewAdded(layoutComponentView: LayoutComponentView) {
        super.onLayoutComponentViewAdded(layoutComponentView)
        setupDragHandler(layoutComponentView)
        layoutComponentView.view.alpha = 0.5f
        layoutComponentView.setHighlighted(false)
    }

    private fun setupDragHandler(layoutComponentView: LayoutComponentView) {
        val gestureDetector = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent): Boolean {
                return true
            }

            override fun onSingleTapUp(e: MotionEvent): Boolean {
                selectView(layoutComponentView)
                layoutComponentView.view.performClick()
                return true
            }

            override fun onDoubleTap(e: MotionEvent): Boolean {
                selectView(layoutComponentView)
                onViewPositionEditRequestedListener?.invoke(buildPositionEditorState(layoutComponentView))
                layoutComponentView.view.performClick()
                return true
            }
        })
        layoutComponentView.view.setOnTouchListener(object : OnTouchListener {
            private var dragging = false

            private var downOffsetX = -1f
            private var downOffsetY = -1f

            override fun onTouch(view: View?, motionEvent: MotionEvent?): Boolean {
                if (view == null || motionEvent == null)
                    return false

                return when (motionEvent.action) {
                    MotionEvent.ACTION_DOWN -> {
                        if (selectedView != null && selectedView != layoutComponentView) {
                            deselectCurrentView()
                        }
                        downOffsetX = motionEvent.x
                        downOffsetY = motionEvent.y
                        view.alpha = 1f
                        layoutComponentView.setHighlighted(true)
                        gestureDetector.onTouchEvent(motionEvent)
                        true
                    }
                    MotionEvent.ACTION_MOVE -> {
                        if (!dragging) {
                            val distance = sqrt((motionEvent.x - downOffsetX).pow(2f) + (motionEvent.y - downOffsetY).pow(2f))
                            if (distance >= 25) {
                                dragging = true
                            }
                        } else {
                            dragView(layoutComponentView, motionEvent.x - downOffsetX, motionEvent.y - downOffsetY)
                        }
                        gestureDetector.onTouchEvent(motionEvent)
                        true
                    }
                    MotionEvent.ACTION_CANCEL -> {
                        gestureDetector.onTouchEvent(motionEvent)
                        if (dragging) {
                            view.alpha = 0.5f
                            layoutComponentView.setHighlighted(false)
                            dragging = false
                        }
                        true
                    }
                    MotionEvent.ACTION_UP -> {
                        val gestureHandled = if (!dragging) {
                            gestureDetector.onTouchEvent(motionEvent)
                        } else {
                            false
                        }
                        if (dragging) {
                            view.alpha = 0.5f
                            layoutComponentView.setHighlighted(false)
                            dragging = false
                        } else if (!gestureHandled) {
                            selectView(layoutComponentView)
                            view.performClick()
                        }
                        true
                    }
                    else -> false
                }
            }
        })
    }

    private fun buildPositionEditorState(view: LayoutComponentView): LayoutComponentPositionEditorState {
        val position = view.getPosition()
        return LayoutComponentPositionEditorState(
            component = view.component,
            x = position.x,
            y = position.y,
            maxX = max(width - view.getWidth(), 0),
            maxY = max(height - view.getHeight(), 0),
        )
    }

    private fun selectView(view: LayoutComponentView) {
        val anchorDistances = mutableMapOf<Anchor, Double>()
        anchorDistances[Anchor.TOP_LEFT] = view.getPosition().x.toDouble().pow(2) + view.getPosition().y.toDouble().pow(2)
        anchorDistances[Anchor.TOP_RIGHT] = (width - (view.getPosition().x + view.getWidth())).toDouble().pow(2) + view.getPosition().y.toDouble().pow(2)
        anchorDistances[Anchor.BOTTOM_LEFT] = view.getPosition().x.toDouble().pow(2) + (height - (view.getPosition().y + view.getHeight())).toDouble().pow(2)
        anchorDistances[Anchor.BOTTOM_RIGHT] = (width - (view.getPosition().x + view.getWidth())).toDouble().pow(2) + (height - (view.getPosition().y + view.getHeight())).toDouble().pow(2)

        var anchor = Anchor.TOP_LEFT
        var minDistance = Double.MAX_VALUE
        anchorDistances.keys.forEach {
            if (anchorDistances[it]!! < minDistance) {
                minDistance = anchorDistances[it]!!
                anchor = it
            }
        }

        selectedViewAnchor = anchor
        selectedView = view

        val widthScale = (view.getWidth() - minComponentSize) / (width - minComponentSize).toFloat()
        val heightScale = (view.getHeight() - minComponentSize) / (height - minComponentSize).toFloat()
        onViewSelectedListener?.invoke(view, widthScale, heightScale, width, height, minComponentSize)
    }

    private fun deselectCurrentView() {
        selectedView?.let {
            it.view.alpha = 0.5f
            it.setHighlighted(false)
            onViewDeselectedListener?.invoke(it)
        }
        selectedView = null
    }

    fun deleteSelectedView() {
        val currentlySelectedView = selectedView ?: return
        removeView(currentlySelectedView.view)
        views.remove(currentlySelectedView.component)
        deselectCurrentView()
        modifiedByUser = true
        notifyLayoutChanged()
    }

    private fun dragView(view: LayoutComponentView, offsetX: Float, offsetY: Float) {
        val currentPosition = view.getPosition()
        val finalX = min(max(currentPosition.x + offsetX, 0f), width - view.getWidth().toFloat())
        val finalY = min(max(currentPosition.y + offsetY, 0f), height - view.getHeight().toFloat())
        view.setPosition(Point(finalX.toInt(), finalY.toInt()))
        modifiedByUser = true
        notifyLayoutChanged()
    }

    fun setSelectedViewAlpha(alpha: Float) {
        selectedView?.let {
            it.baseAlpha = alpha
            modifiedByUser = true
            notifyLayoutChanged()
        }
    }

    fun setComponentButtonMode(component: LayoutComponent, mode: VirtualButtonMode) {
        if (!component.supportsButtonMode()) return
        val view = views[component] ?: return
        view.buttonMode = mode
        modifiedByUser = true
        notifyLayoutChanged()
    }

    fun setSelectedScreenOnTop(onTop: Boolean) {
        selectedView?.let {
            it.onTop = onTop
            rearrangeScreens()
            modifiedByUser = true
            notifyLayoutChanged()
        }
    }

    fun setSelectedViewPosition(x: Int, y: Int) {
        val currentlySelectedView = selectedView ?: return
        setComponentPosition(currentlySelectedView.component, x, y)
    }

    fun setComponentPosition(component: LayoutComponent, x: Int, y: Int): Boolean {
        val view = views[component] ?: return false
        val boundedX = x.coerceIn(0, max(width - view.getWidth(), 0))
        val boundedY = y.coerceIn(0, max(height - view.getHeight(), 0))
        view.setPosition(Point(boundedX, boundedY))
        modifiedByUser = true
        notifyLayoutChanged()
        return true
    }

    fun buildComponentPositionEditorState(component: LayoutComponent): LayoutComponentPositionEditorState? {
        val view = views[component] ?: return null
        return buildPositionEditorState(view)
    }

    private fun rearrangeScreens() {
        val topScreen = views[LayoutComponent.TOP_SCREEN]
        val bottomScreen = views[LayoutComponent.BOTTOM_SCREEN]
        if (topScreen != null && bottomScreen != null) {
            removeView(topScreen.view)
            removeView(bottomScreen.view)
            if (topScreen.onTop) {
                addView(bottomScreen.view, 0)
                addView(topScreen.view, 0)
            } else if (bottomScreen.onTop) {
                addView(topScreen.view, 0)
                addView(bottomScreen.view, 0)
            } else {
                addView(bottomScreen.view, 0)
                addView(topScreen.view, 0)
            }
        }
    }

    fun centerSelectedViewHorizontally() {
        val view = selectedView ?: return
        val centerX = (width - view.getWidth()) / 2
        val position = view.getPosition()
        view.setPosition(Point(centerX, position.y))
        modifiedByUser = true
        notifyLayoutChanged()
    }

    fun centerSelectedViewVertically() {
        val view = selectedView ?: return
        val centerY = (height - view.getHeight()) / 2
        val position = view.getPosition()
        view.setPosition(Point(position.x, centerY))
        modifiedByUser = true
        notifyLayoutChanged()
    }

    fun scaleSelectedView(scale: Float) {
        // Assume view has a 1:1 aspect ratio. Always scale on the smallest axis
        if (width > height) {
            val widthScale = ((height - minComponentSize) * scale) / (width - minComponentSize)
            scaleSelectedView(widthScale, scale)
        } else {
            val heightScale =  ((width - minComponentSize) * scale) / (height - minComponentSize)
            scaleSelectedView(scale, heightScale)
        }
    }

    fun scaleSelectedView(widthScale: Float, heightScale: Float) {
        val currentlySelectedView = selectedView ?: return
        scaleComponent(currentlySelectedView.component, widthScale, heightScale, selectedViewAnchor)
    }

    fun scaleComponent(component: LayoutComponent, scale: Float): Boolean {
        // Assume view has a 1:1 aspect ratio. Always scale on the smallest axis
        return if (width > height) {
            val widthScale = ((height - minComponentSize) * scale) / (width - minComponentSize)
            scaleComponent(component, widthScale, scale)
        } else {
            val heightScale =  ((width - minComponentSize) * scale) / (height - minComponentSize)
            scaleComponent(component, scale, heightScale)
        }
    }

    fun scaleComponent(component: LayoutComponent, widthScale: Float, heightScale: Float): Boolean {
        return scaleComponent(component, widthScale, heightScale, findAnchor(views[component] ?: return false))
    }

    private fun scaleComponent(
        component: LayoutComponent,
        widthScale: Float,
        heightScale: Float,
        anchor: Anchor,
    ): Boolean {
        val currentlySelectedView = views[component] ?: return false
        val newViewWidth = ((width - minComponentSize) * widthScale + minComponentSize).roundToInt()
        val newViewHeight = ((height - minComponentSize) * heightScale + minComponentSize).roundToInt()

        val viewPosition = currentlySelectedView.getPosition()
        var viewX: Int
        var viewY: Int

        if (anchor == Anchor.TOP_LEFT) {
            viewX = viewPosition.x
            viewY = viewPosition.y
            if (viewX + newViewWidth > width) {
                viewX = width - newViewWidth
            }
            if (viewY + newViewHeight > height) {
                viewY = height - newViewHeight
            }
        } else if (anchor == Anchor.TOP_RIGHT) {
            viewX = viewPosition.x + currentlySelectedView.getWidth() - newViewWidth
            viewY = viewPosition.y
            if (viewX < 0) {
                viewX = 0
            }
            if (viewY + newViewHeight > height) {
                viewY = height - newViewHeight
            }
        } else if (anchor == Anchor.BOTTOM_LEFT) {
            viewX = viewPosition.x
            viewY = viewPosition.y + currentlySelectedView.getHeight() - newViewHeight
            if (viewX + newViewWidth > width) {
                viewX = width - newViewWidth
            }
            if (viewY < 0) {
                viewY = 0
            }
        } else {
            viewX = viewPosition.x + currentlySelectedView.getWidth() - newViewWidth
            viewY = viewPosition.y + currentlySelectedView.getHeight() - newViewHeight
            if (viewX < 0) {
                viewX = 0
            }
            if (viewY < 0) {
                viewY = 0
            }
        }
        currentlySelectedView.setPositionAndSize(Point(viewX, viewY), newViewWidth, newViewHeight)
        modifiedByUser = true
        notifyLayoutChanged()
        return true
    }

    fun releaseComponentEdit(component: LayoutComponent?) {
        val componentView = component?.let { views[it] }
        if (componentView != null && componentView != selectedView) {
            componentView.view.alpha = 0.5f
            componentView.setHighlighted(false)
            onViewDeselectedListener?.invoke(componentView)
        }
        deselectCurrentView()
    }

    private fun findAnchor(view: LayoutComponentView): Anchor {
        val anchorDistances = mutableMapOf<Anchor, Double>()
        anchorDistances[Anchor.TOP_LEFT] = view.getPosition().x.toDouble().pow(2) + view.getPosition().y.toDouble().pow(2)
        anchorDistances[Anchor.TOP_RIGHT] = (width - (view.getPosition().x + view.getWidth())).toDouble().pow(2) + view.getPosition().y.toDouble().pow(2)
        anchorDistances[Anchor.BOTTOM_LEFT] = view.getPosition().x.toDouble().pow(2) + (height - (view.getPosition().y + view.getHeight())).toDouble().pow(2)
        anchorDistances[Anchor.BOTTOM_RIGHT] = (width - (view.getPosition().x + view.getWidth())).toDouble().pow(2) + (height - (view.getPosition().y + view.getHeight())).toDouble().pow(2)

        var anchor = Anchor.TOP_LEFT
        var minDistance = Double.MAX_VALUE
        anchorDistances.keys.forEach {
            if (anchorDistances[it]!! < minDistance) {
                minDistance = anchorDistances[it]!!
                anchor = it
            }
        }

        return anchor
    }

    private fun notifyLayoutChanged() {
        if (width <= 0 || height <= 0) {
            return
        }
        val currentLayout = buildCurrentLayout()
        onLayoutChangedListener?.invoke(currentLayout, width, height)
    }
}
