package me.magnum.melonds.domain.model.layout

import me.magnum.melonds.domain.model.Point
import me.magnum.melonds.domain.model.ui.Orientation

data class UILayoutVariant(
    val uiSize: Point,
    val uiInsets: Insets,
    val orientation: Orientation,
    val folds: List<ScreenFold>,
    val displays: LayoutDisplayPair,
) {
    fun supportsScreenGap(layoutType: LayoutConfiguration.LayoutType): Boolean =
        layoutType == LayoutConfiguration.LayoutType.DEFAULT && displays.secondaryScreenDisplay == null && folds.isEmpty()
}