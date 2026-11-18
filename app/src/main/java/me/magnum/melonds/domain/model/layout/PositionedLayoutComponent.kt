package me.magnum.melonds.domain.model.layout

import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.consoleAspectRatio
import kotlin.math.abs

enum class ScreenAspectRatio(val ratio: Float?) {
    RATIO_4_3(consoleAspectRatio),
    RATIO_16_9(16 / 9f),
    UNRESTRICTED(null);

    companion object {
        fun fromRect(rect: Rect): ScreenAspectRatio {
            if (rect.width <= 0 || rect.height <= 0) return RATIO_4_3
            return entries.firstOrNull { mode ->
                mode.ratio?.let { abs(rect.height - rect.width / it) <= 1f } == true
            } ?: UNRESTRICTED
        }
    }
}

data class PositionedLayoutComponent(
    val rect: Rect,
    val component: LayoutComponent,
    val alpha: Float = 1f,
    val onTop: Boolean = false,
    val buttonMode: VirtualButtonMode = VirtualButtonMode.NORMAL,
    val screenAspectRatio: ScreenAspectRatio? = null,
) {

    fun isScreen(): Boolean {
        return component.isScreen()
    }
}
