package me.magnum.melonds.impl.layout

import me.magnum.melonds.domain.model.Point
import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.consoleAspectRatio
import me.magnum.melonds.domain.model.layout.Insets
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.domain.model.layout.LayoutConfiguration.LayoutType
import me.magnum.melonds.domain.model.layout.LayoutDisplay
import me.magnum.melonds.domain.model.layout.LayoutDisplayPair
import me.magnum.melonds.domain.model.layout.ScreenFold
import me.magnum.melonds.domain.model.layout.ScreenGap
import me.magnum.melonds.domain.model.layout.UILayoutVariant
import me.magnum.melonds.domain.model.ui.Orientation
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class DefaultScreenPairTest {
    @Test
    fun offPreservesOriginalRectsIncludingInsetsAndRounding() {
        val portrait = defaultScreenPair(Rect(7, 11, 440, 553), Orientation.PORTRAIT, 0)
        assertEquals(listOf(Rect(43, 11, 368, 276), Rect(43, 287, 368, 276)), portrait.map { it.rect })

        val landscape = defaultScreenPair(Rect(19, 5, 923, 408), Orientation.LANDSCAPE, 0)
        assertEquals(listOf(Rect(19, 5, 544, 408), Rect(563, 5, 379, 284)), landscape.map { it.rect })
    }

    @Test
    fun allChoicesUseInnerEdgesAndKeepBothScreensWithinSafeArea() {
        assertEquals(listOf(0, 8, 16, 24), ScreenGap.entries.map { it.dp })
        for (gap in ScreenGap.entries) {
            for (orientation in Orientation.entries) {
                val bounds = if (orientation == Orientation.PORTRAIT) Rect(13, 17, 441, 554) else Rect(31, 8, 923, 408)
                val (first, second) = defaultScreenPair(bounds, orientation, gap.dp)
                assertEquals(LayoutComponent.TOP_SCREEN, first.component)
                assertEquals(LayoutComponent.BOTTOM_SCREEN, second.component)
                val innerDistance = if (orientation == Orientation.PORTRAIT) second.rect.y - first.rect.bottom else second.rect.x - first.rect.right
                assertEquals(gap.dp, innerDistance)
                for (lcd in listOf(first.rect, second.rect)) {
                    assertTrue(bounds.contains(lcd))
                    assertTrue(abs(lcd.width / consoleAspectRatio - lcd.height) < 1f)
                }
            }
        }
    }

    @Test
    fun requestedGapIsBoundedInSmallWindows() {
        val (top, bottom) = defaultScreenPair(Rect(0, 0, 12, 10), Orientation.PORTRAIT, 24)
        assertEquals(1, top.rect.height)
        assertEquals(8, bottom.rect.y - top.rect.bottom)
        assertEquals(10, bottom.rect.bottom)

        val wide = Rect(0, 0, 1600, 220)
        for (gap in listOf(0, 24)) {
            val (left, right) = defaultScreenPair(wide, Orientation.LANDSCAPE, gap)
            assertEquals(gap, right.rect.x - left.rect.right)
            assertTrue(wide.contains(left.rect))
            assertTrue(wide.contains(right.rect))
        }
    }

    @Test
    fun onlyAutomaticUnfoldedSinglePanelIsEligible() {
        val display = LayoutDisplay(0, LayoutDisplay.Type.BUILT_IN, 480, 800)
        val variant = UILayoutVariant(Point(480, 800), Insets.Zero, Orientation.PORTRAIT, emptyList(), LayoutDisplayPair(display, null))
        val fold = ScreenFold(Orientation.LANDSCAPE, ScreenFold.FoldType.GAP, Rect(0, 400, 480, 10))
        assertTrue(variant.supportsScreenGap(LayoutType.DEFAULT))
        assertFalse(variant.supportsScreenGap(LayoutType.CUSTOM))
        assertFalse(variant.copy(folds = listOf(fold)).supportsScreenGap(LayoutType.DEFAULT))
        assertFalse(variant.copy(displays = LayoutDisplayPair(display, display.copy(id = 2))).supportsScreenGap(LayoutType.DEFAULT))
    }
}
