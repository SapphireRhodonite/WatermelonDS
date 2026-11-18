package me.magnum.melonds.ui.common

import android.view.View
import android.widget.FrameLayout
import androidx.core.view.updateLayoutParams
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.domain.model.layout.ScreenAspectRatio
import me.magnum.melonds.domain.model.layout.VirtualButtonMode
import me.magnum.melonds.domain.model.Point
import me.magnum.melonds.domain.model.Rect

class LayoutComponentView(val view: View, val aspectRatio: Float, val component: LayoutComponent) {
    var baseAlpha: Float = 1f
        set(value) {
            field = value
            applyAlpha()
        }

    var onTop: Boolean = false
    var buttonMode: VirtualButtonMode = VirtualButtonMode.NORMAL
    var screenAspectRatio: ScreenAspectRatio? = null

    fun setPosition(position: Point) {
        view.updateLayoutParams<FrameLayout.LayoutParams> {
            leftMargin = position.x
            topMargin = position.y
        }
    }

    fun setSize(width: Int, height: Int) {
        view.updateLayoutParams {
            this.width = width
            this.height = height
        }
    }

    fun setPositionAndSize(position: Point, width: Int, height: Int) {
        view.updateLayoutParams<FrameLayout.LayoutParams> {
            this.width = width
            this.height = height
            leftMargin = position.x
            topMargin = position.y
        }
    }

    fun getPosition(): Point {
        return Point().apply {
            x = view.x.toInt()
            y = view.y.toInt()
        }
    }

    fun getWidth(): Int {
        return view.width
    }

    fun getHeight(): Int {
        return view.height
    }

    fun getRect(): Rect {
        val layoutParams = view.layoutParams as? FrameLayout.LayoutParams
        val measuredWidth = view.width
        val measuredHeight = view.height
        val width = if (measuredWidth > 0) measuredWidth else layoutParams?.width?.coerceAtLeast(0) ?: 0
        val height = if (measuredHeight > 0) measuredHeight else layoutParams?.height?.coerceAtLeast(0) ?: 0
        val x = if (measuredWidth > 0) {
            view.x.toInt()
        } else {
            layoutParams?.leftMargin ?: view.x.toInt()
        }
        val y = if (measuredHeight > 0) {
            view.y.toInt()
        } else {
            layoutParams?.topMargin ?: view.y.toInt()
        }

        return Rect(
                x,
                y,
                width,
                height,
        )
    }

    fun setHighlighted(value: Boolean) {
        view.isSelected = value
    }

    private fun applyAlpha() {
        view.alpha = baseAlpha
    }
}
