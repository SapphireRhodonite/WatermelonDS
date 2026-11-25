package me.magnum.melonds.impl.layout

import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.consoleAspectRatio
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.domain.model.layout.PositionedLayoutComponent
import me.magnum.melonds.domain.model.ui.Orientation
import kotlin.math.roundToInt

internal fun defaultScreenPair(bounds: Rect, orientation: Orientation, gapPx: Int): List<PositionedLayoutComponent> {
    val top: Rect
    val bottom: Rect
    when (orientation) {
        Orientation.PORTRAIT -> {
            val gap = gapPx.coerceIn(0, (bounds.height - 2).coerceAtLeast(0))
            val maxHeight = (bounds.height - gap) / 2
            var width = bounds.width
            var height = (width / consoleAspectRatio).toInt()
            if (height > maxHeight) {
                height = maxHeight
                width = (height * consoleAspectRatio).toInt()
            }
            top = Rect(bounds.x + (bounds.width - width) / 2, bounds.y, width, height)
            bottom = top.copy(y = top.bottom + gap)
        }
        Orientation.LANDSCAPE -> {
            val gap = gapPx.coerceIn(0, (bounds.width - 4).coerceAtLeast(0))
            val availableWidth = bounds.width - gap
            var width = (availableWidth * 0.66f).roundToInt()
            var height = (width / consoleAspectRatio).toInt()
            if (height > bounds.height) {
                height = bounds.height
                width = (height * consoleAspectRatio).toInt()
            }
            top = Rect(bounds.x, bounds.y, width, height)
            val remainingWidth = availableWidth - width
            val bottomWidth = if (gap > 0) remainingWidth.coerceAtMost((bounds.height * consoleAspectRatio).toInt()) else remainingWidth
            bottom = Rect(top.right + gap, bounds.y, bottomWidth, (bottomWidth / consoleAspectRatio).toInt())
        }
    }
    return listOf(
        PositionedLayoutComponent(top, LayoutComponent.TOP_SCREEN),
        PositionedLayoutComponent(bottom, LayoutComponent.BOTTOM_SCREEN),
    )
}
