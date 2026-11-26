package me.magnum.melonds.impl.layout

import me.magnum.melonds.domain.model.DualScreenPreset
import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.SCREEN_HEIGHT
import me.magnum.melonds.domain.model.SCREEN_WIDTH
import me.magnum.melonds.domain.model.ScreenAlignment
import me.magnum.melonds.domain.model.defaultExternalAlignment
import me.magnum.melonds.domain.model.defaultInternalAlignment
import me.magnum.melonds.domain.model.layout.LayoutDisplay
import me.magnum.melonds.domain.model.layout.PositionedLayoutComponent
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.domain.model.layout.UILayout
import me.magnum.melonds.domain.model.layout.UILayoutVariant
import me.magnum.melonds.domain.model.ui.Orientation
import kotlin.math.abs
import kotlin.math.min
import kotlin.math.roundToInt

internal data class DualScreenPresetConfiguration(
    val preset: DualScreenPreset,
    val integerScale: Boolean,
    val keepAspectRatio: Boolean,
    val internalFillHeight: Boolean,
    val internalFillWidth: Boolean,
    val externalFillHeight: Boolean,
    val externalFillWidth: Boolean,
    val internalAlignmentOverride: ScreenAlignment?,
    val externalAlignmentOverride: ScreenAlignment?,
)

internal fun UILayoutVariant.foldPanels(
    foldableDensity: Float? = null,
    presetSelected: Boolean = false,
    automatic: Boolean = false,
    gapPx: Int = 0,
): FoldPanels? {
    if (displays.secondaryScreenDisplay != null) return null
    val safe = Rect(uiInsets.left, uiInsets.top, uiSize.x - uiInsets.left - uiInsets.right, uiSize.y - uiInsets.top - uiInsets.bottom)
    if (minOf(safe.width, safe.height) <= 1) return null
    val fold = folds.firstOrNull()
    if (fold != null) {
        val seam = fold.foldBounds
        val (first, second) = when (fold.orientation) {
            Orientation.LANDSCAPE -> safe.copy(height = seam.y - safe.y) to safe.copy(y = seam.bottom, height = safe.bottom - seam.bottom)
            Orientation.PORTRAIT -> safe.copy(width = seam.x - safe.x) to safe.copy(x = seam.right, width = safe.right - seam.right)
        }
        if (minOf(first.width, first.height, second.width, second.height) <= 0) return null
        return FoldPanels(fold.orientation, first, second)
    }

    if (foldableDensity == null || foldableDensity <= 0f) return null
    val main = displays.mainScreenDisplay
    if (!main.isDefaultDisplay && main.type != LayoutDisplay.Type.BUILT_IN) return null
    val landscape = safe.width > safe.height
    val stackInLandscape = landscape && safe.height / foldableDensity >= 480f
    if (!presetSelected && !(automatic && stackInLandscape)) return null
    val stacked = !landscape || stackInLandscape
    val direction = if (stacked) Orientation.PORTRAIT else Orientation.LANDSCAPE
    val (topScreen, bottomScreen) = defaultScreenPair(safe, direction, gapPx)
    val top = topScreen.rect
    val bottom = bottomScreen.rect
    val first = if (stacked) safe.copy(height = top.bottom - safe.y) else safe.copy(width = top.right - safe.x)
    val second = if (stacked) safe.copy(y = bottom.y, height = safe.bottom - bottom.y) else safe.copy(x = bottom.x, width = safe.right - bottom.x)
    if (minOf(first.width, first.height, second.width, second.height) <= 0) return null
    return FoldPanels(if (stacked) Orientation.LANDSCAPE else Orientation.PORTRAIT, first, second, top to bottom)
}

internal data class FoldPanels(
    val orientation: Orientation,
    val first: Rect,
    val second: Rect,
    val recommended: Pair<Rect, Rect>? = null,
) {
    fun keepComponentsOffSeam(layout: UILayout): UILayout {
        if (recommended != null) return layout
        val components = layout.mainScreenLayout.components ?: return layout
        if (components.none { crossesSeam(it.rect) }) return layout
        val adjusted = components.map { component ->
            val rect = component.rect
            if (!crossesSeam(rect)) return@map component
            val a = rect.fitInside(first)
            val b = rect.fitInside(second)
            val distanceA = abs(rect.x - a.x) + abs(rect.y - a.y)
            val distanceB = abs(rect.x - b.x) + abs(rect.y - b.y)
            val destination = when {
                a.width > b.width || a.height > b.height -> a
                b.width > a.width || b.height > a.height -> b
                distanceA < distanceB -> a
                distanceA > distanceB -> b
                component.component == LayoutComponent.TOP_SCREEN -> a
                else -> b
            }
            component.copy(rect = destination)
        }
        return layout.copy(mainScreenLayout = layout.mainScreenLayout.copy(components = adjusted))
    }

    private fun crossesSeam(rect: Rect): Boolean {
        if (rect.width <= 0 || rect.height <= 0) return false
        return when (orientation) {
            Orientation.LANDSCAPE -> rect.x < first.right && rect.right > first.x && rect.y < second.y && rect.bottom > first.bottom
            Orientation.PORTRAIT -> rect.y < first.bottom && rect.bottom > first.y && rect.x < second.x && rect.right > first.right
        }
    }

    private fun Rect.fitInside(area: Rect): Rect {
        val scale = minOf(1f, area.width.toFloat() / width, area.height.toFloat() / height)
        val w = (width * scale).roundToInt().coerceIn(1, area.width)
        val h = (height * scale).roundToInt().coerceIn(1, area.height)
        return Rect(x.coerceIn(area.x, area.right - w), y.coerceIn(area.y, area.bottom - h), w, h)
    }

    fun applyPreset(layout: UILayout, config: DualScreenPresetConfiguration, matchSize: Boolean): UILayout? {
        if (config.preset == DualScreenPreset.OFF && !matchSize && recommended == null) return null
        val components = layout.mainScreenLayout.components ?: return null
        val screens = components.filter { it.isScreen() }
        val hasBothScreens = screens.size == 2 && screens.any { it.component == LayoutComponent.TOP_SCREEN } &&
            screens.any { it.component == LayoutComponent.BOTTOM_SCREEN }
        if (!hasBothScreens && config.preset == DualScreenPreset.OFF) return null

        val sameDimensions = if (matchSize) {
            computeAspectRatioDimensions(minOf(first.width, second.width), minOf(first.height, second.height))
        } else null
        val canFill = config.integerScale || config.keepAspectRatio
        fun fit(area: Rect, original: Rect?, fillHeight: Boolean, fillWidth: Boolean, override: ScreenAlignment?, presetAlignment: ScreenAlignment): Rect {
            if (original != null && !matchSize && (config.preset == DualScreenPreset.OFF ||
                        config.keepAspectRatio && !config.integerScale && !fillHeight && !fillWidth && override == null)) return original
            val alignment = override ?: if (config.preset == DualScreenPreset.OFF) ScreenAlignment.CENTER else presetAlignment
            if (sameDimensions != null) {
                val (width, height) = sameDimensions
                val y = when (alignment) {
                    ScreenAlignment.TOP -> area.y
                    ScreenAlignment.CENTER -> area.y + (area.height - height) / 2
                    ScreenAlignment.BOTTOM -> area.bottom - height
                }
                return Rect(area.x + (area.width - width) / 2, y, width, height)
            }
            val rect = computeScaledScreenRect(area.width, area.height, config.integerScale, config.keepAspectRatio,
                canFill && fillHeight, canFill && fillWidth, alignment)
            return rect.copy(x = area.x + rect.x, y = area.y + rect.y)
        }

        val top = fit(first, recommended?.first, config.internalFillHeight, config.internalFillWidth, config.internalAlignmentOverride, config.preset.defaultInternalAlignment())
        val bottom = fit(second, recommended?.second, config.externalFillHeight, config.externalFillWidth, config.externalAlignmentOverride, config.preset.defaultExternalAlignment())
        val panelComponents = if (hasBothScreens) components else listOf(
            PositionedLayoutComponent(top, LayoutComponent.TOP_SCREEN),
            PositionedLayoutComponent(bottom, LayoutComponent.BOTTOM_SCREEN),
        ) + components.filterNot { it.isScreen() }
        val adjusted = panelComponents.map {
            when (it.component) {
                LayoutComponent.TOP_SCREEN -> it.copy(rect = top)
                LayoutComponent.BOTTOM_SCREEN -> it.copy(rect = bottom)
                else -> it
            }
        }
        return layout.copy(mainScreenLayout = layout.mainScreenLayout.copy(components = adjusted))
    }
}

internal fun computeScaledScreenRect(
    availableWidth: Int,
    availableHeight: Int,
    integerScale: Boolean,
    keepAspectRatio: Boolean,
    fillHeight: Boolean,
    fillWidth: Boolean,
    alignment: ScreenAlignment,
): Rect {
    val (baseWidth, baseHeight) = when {
        integerScale -> computeIntegerScaleDimensions(availableWidth, availableHeight)
        keepAspectRatio -> computeAspectRatioDimensions(availableWidth, availableHeight)
        else -> availableWidth to availableHeight
    }

    val scaledWidth = if (fillWidth) availableWidth else baseWidth
    val scaledHeight = if (fillHeight) availableHeight else baseHeight

    val left = ((availableWidth - scaledWidth) / 2f).roundToInt().coerceAtLeast(0)
    val top = when (alignment) {
        ScreenAlignment.TOP -> 0
        ScreenAlignment.CENTER -> ((availableHeight - scaledHeight) / 2f).roundToInt().coerceAtLeast(0)
        ScreenAlignment.BOTTOM -> (availableHeight - scaledHeight).coerceAtLeast(0)
    }

    return Rect(left, top, scaledWidth.coerceAtLeast(1), scaledHeight.coerceAtLeast(1))
}

private fun computeIntegerScaleDimensions(availableWidth: Int, availableHeight: Int): Pair<Int, Int> {
    val widthScale = availableWidth / SCREEN_WIDTH
    val heightScale = availableHeight / SCREEN_HEIGHT
    val maxIntegerScale = min(widthScale, heightScale)
    val scale = if (maxIntegerScale <= 0) {
        min(
            availableWidth.toFloat() / SCREEN_WIDTH,
            availableHeight.toFloat() / SCREEN_HEIGHT,
        )
    } else {
        maxIntegerScale.toFloat()
    }
    val width = (SCREEN_WIDTH * scale).roundToInt().coerceAtLeast(1).coerceAtMost(availableWidth)
    val height = (SCREEN_HEIGHT * scale).roundToInt().coerceAtLeast(1).coerceAtMost(availableHeight)
    return width to height
}

private fun computeAspectRatioDimensions(availableWidth: Int, availableHeight: Int): Pair<Int, Int> {
    val widthRatio = availableWidth.toFloat() / SCREEN_WIDTH
    val heightRatio = availableHeight.toFloat() / SCREEN_HEIGHT
    val scale = min(widthRatio, heightRatio)
    val width = (SCREEN_WIDTH * scale).roundToInt().coerceAtLeast(1).coerceAtMost(availableWidth)
    val height = (SCREEN_HEIGHT * scale).roundToInt().coerceAtLeast(1).coerceAtMost(availableHeight)
    return width to height
}
