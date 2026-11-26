package me.magnum.melonds.impl.layout

import me.magnum.melonds.domain.model.DualScreenPreset
import me.magnum.melonds.domain.model.Point
import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.ScreenAlignment
import me.magnum.melonds.domain.model.layout.*
import me.magnum.melonds.domain.model.ui.Orientation
import me.magnum.melonds.ui.emulator.model.FoldScreenSwapState
import org.junit.Assert.*
import org.junit.Test

class FoldPanelsTest {
    private val display = LayoutDisplay(0, LayoutDisplay.Type.BUILT_IN, 1100, 1250)
    private val variant = UILayoutVariant(Point(1100, 1250), Insets(11, 20, 19, 30), Orientation.PORTRAIT,
        emptyList(), LayoutDisplayPair(display, null))
    private val config = DualScreenPresetConfiguration(DualScreenPreset.INTERNAL_TOP_EXTERNAL_BOTTOM,
        integerScale = true, keepAspectRatio = true, internalFillHeight = false, internalFillWidth = false,
        externalFillHeight = false, externalFillWidth = false, internalAlignmentOverride = null, externalAlignmentOverride = null)
    private val button = PositionedLayoutComponent(Rect(20, 900, 48, 48), LayoutComponent.BUTTON_PAUSE)
    private val layout = UILayout(ScreenLayout(listOf(
        PositionedLayoutComponent(Rect(0, 0, 256, 192), LayoutComponent.TOP_SCREEN),
        PositionedLayoutComponent(Rect(0, 192, 256, 192), LayoutComponent.BOTTOM_SCREEN), button
    )), ScreenLayout())
    private val tabletop = ScreenFold(Orientation.LANDSCAPE, ScreenFold.FoldType.SEAMLESS, Rect(0, 500, 1100, 16))
    private val book = ScreenFold(Orientation.PORTRAIT, ScreenFold.FoldType.SEAMLESS, Rect(440, 0, 0, 1250))

    @Test fun partitionsUseSafeWindowAndHingeRegardlessOfOrientation() {
        val table = variant.copy(folds = listOf(tabletop)).foldPanels()!!
        assertEquals(Rect(11, 20, 1070, 480), table.first)
        assertEquals(Rect(11, 516, 1070, 704), table.second)
        val vertical = variant.copy(folds = listOf(book)).foldPanels()!!
        assertEquals(Rect(11, 20, 429, 1200), vertical.first)
        assertEquals(Rect(440, 20, 641, 1200), vertical.second)
        assertNull(variant.foldPanels())
        assertNull(variant.copy(folds = listOf(book), displays = LayoutDisplayPair(display, display.copy(id = 2))).foldPanels())
    }

    @Test fun eachPaneHonorsItsOwnScaleFillAndAlignment() {
        val panes = variant.copy(folds = listOf(tabletop)).foldPanels()!!
        val result = panes.applyPreset(layout, config.copy(internalFillHeight = true, externalFillWidth = true,
            externalAlignmentOverride = ScreenAlignment.BOTTOM), false)!!
        val controls = result.mainScreenLayout.components!!
        assertEquals(Rect(290, 20, 512, 480), controls[0].rect)
        assertEquals(Rect(11, 644, 1070, 576), controls[1].rect)
        assertSame(button, controls[2])
        assertNull(panes.applyPreset(layout, config.copy(preset = DualScreenPreset.OFF), false))
    }

    @Test fun matchSizeUsesSamePixelsAndUnequalPanesDoNotCrossSeam() {
        val panes = variant.copy(folds = listOf(book)).foldPanels()!!
        val result = panes.applyPreset(layout, config.copy(preset = DualScreenPreset.INTERNAL_BOTTOM_EXTERNAL_TOP), true)!!
        val (top, bottom) = result.mainScreenLayout.components!!
        assertEquals(top.rect.width, bottom.rect.width)
        assertEquals(top.rect.height, bottom.rect.height)
        assertEquals(panes.first.y, top.rect.y)
        assertEquals(panes.second.bottom, bottom.rect.bottom)
        assertTrue(panes.first.contains(top.rect) && panes.second.contains(bottom.rect))
    }

    @Test fun swapAndTouchAssignmentFollowPresetAndRestoreTheOriginalOnUnfold() {
        val swap = FoldScreenSwapState()
        swap.toggle()
        swap.setFoldPreset(DualScreenPreset.INTERNAL_TOP_EXTERNAL_BOTTOM)
        assertFalse(swap.isSwapped)
        swap.setFoldPreset(DualScreenPreset.INTERNAL_BOTTOM_EXTERNAL_TOP)
        assertTrue(swap.isSwapped)
        swap.toggle()
        swap.setFoldPreset(DualScreenPreset.INTERNAL_BOTTOM_EXTERNAL_TOP)
        assertFalse(swap.isSwapped)
        swap.setFoldPreset(null)
        assertTrue(swap.isSwapped)
    }
    @Test fun recommendedPortraitSizesAreExactBeforeAndAfterSwappingOnFlatAndCover() {
        val ratio = config.copy(integerScale = false)
        for ((width, expected) in listOf(
            1768 to listOf(Rect(148, 0, 1472, 1104), Rect(148, 1104, 1472, 1104)),
            840 to listOf(Rect(0, 0, 840, 630), Rect(0, 630, 840, 630)),
        )) {
            val flat = variant.copy(uiSize = Point(width, 2208), uiInsets = Insets.Zero)
            assertNull(flat.foldPanels(2.625f, false, true))
            val panels = flat.foldPanels(2.625f, true, true)!!
            for (preset in listOf(DualScreenPreset.INTERNAL_TOP_EXTERNAL_BOTTOM, DualScreenPreset.INTERNAL_BOTTOM_EXTERNAL_TOP)) {
                val screens = panels.applyPreset(layout, ratio.copy(preset = preset), false)!!.mainScreenLayout.components!!
                assertEquals(expected, screens.filter { it.isScreen() }.map { it.rect })
                val swap = FoldScreenSwapState().apply { setFoldPreset(preset) }
                assertEquals(preset == DualScreenPreset.INTERNAL_BOTTOM_EXTERNAL_TOP, swap.isSwapped)
            }
        }
    }

    @Test fun onlyACapableSinglePanelStacksAtTheUsefulLandscapeHeightBoundary() {
        val flat = variant.copy(uiSize = Point(2208, 1290), uiInsets = Insets(11, 12, 15, 18))
        val threshold = flat.foldPanels(2.625f, true, true, 24)!!
        assertEquals(Orientation.LANDSCAPE, threshold.orientation)
        assertEquals(24, threshold.second.y - threshold.first.bottom)
        val off = config.copy(preset = DualScreenPreset.OFF)
        assertEquals(threshold.recommended!!.toList(),
            flat.foldPanels(2.625f, false, true, 24)!!.applyPreset(layout, off, false)!!.mainScreenLayout.components!!.filter { it.isScreen() }.map { it.rect })
        assertEquals(Orientation.PORTRAIT, flat.copy(uiSize = Point(2208, 1289)).foldPanels(2.625f, true, true)!!.orientation)
        assertNull(flat.foldPanels(null, true, true))
        assertNull(flat.foldPanels(2.625f, false, false))
        assertNull(flat.copy(displays = LayoutDisplayPair(display, display.copy(id = 2))).foldPanels(2.625f, true, true))
    }

    @Test fun flatPresetsHonorPerPanelOverridesAndRestoreACustomHybridOnOff() {
        val flat = variant.copy(uiSize = Point(840, 2208), uiInsets = Insets.Zero)
        val panels = flat.foldPanels(2.625f, true, false)!!
        val hybrid = PositionedLayoutComponent(Rect(51, 39, 302, 444), LayoutComponent.HYBRID_SCREEN)
        val custom = layout.copy(mainScreenLayout = ScreenLayout(listOf(hybrid, button)))
        val result = panels.applyPreset(custom, config.copy(internalFillWidth = true, externalAlignmentOverride = ScreenAlignment.BOTTOM), false)!!
        val controls = result.mainScreenLayout.components!!
        assertEquals(Rect(0, 54, 840, 576), controls[0].rect)
        assertEquals(Rect(36, 1632, 768, 576), controls[1].rect)
        assertSame(button, controls[2])
        assertNull(flat.foldPanels(2.625f, false, false))
        assertEquals(listOf(hybrid, button), custom.mainScreenLayout.components)
    }

    @Test fun runtimeProtectsOnlyTheCustomChildrenCrossingTheHingeInEveryPreset() {
        val hinge = FoldPanels(Orientation.PORTRAIT, Rect(0, 0, 440, 900), Rect(456, 0, 544, 900))
        val buttonOnHinge = button.copy(rect = Rect(430, 200, 60, 50))
        val manualTop = PositionedLayoutComponent(Rect(100, 50, 374, 280), LayoutComponent.TOP_SCREEN)
        val usableBottom = PositionedLayoutComponent(Rect(470, 50, 300, 225), LayoutComponent.BOTTOM_SCREEN)
        val custom = UILayout(ScreenLayout(listOf(manualTop, usableBottom, buttonOnHinge)), ScreenLayout())
        for (preset in DualScreenPreset.entries) {
            val current = hinge.applyPreset(custom, config.copy(preset = preset), false) ?: custom
            val safe = hinge.keepComponentsOffSeam(current)
            val components = safe.mainScreenLayout.components!!
            assertTrue(components.all { hinge.first.contains(it.rect) || hinge.second.contains(it.rect) })
            assertEquals(Rect(456, 200, 60, 50), components[2].rect)
            if (preset == DualScreenPreset.OFF) {
                assertSame(usableBottom, components[1])
                assertEquals(Rect(66, 50, 374, 280), components[0].rect)
            }
        }
        assertEquals(Rect(430, 200, 60, 50), custom.mainScreenLayout.components!![2].rect)
        assertEquals(Rect(100, 50, 374, 280), custom.mainScreenLayout.components!![0].rect)
        val flat = variant.foldPanels(2.625f, true, true)!!
        assertSame(custom, flat.keepComponentsOffSeam(custom))
    }

    @Test fun zeroWidthSeparatorProtectsCrossingViewsAndPreservesThoseEndingAtTheEdge() {
        val seam = FoldPanels(Orientation.LANDSCAPE, Rect(0, 0, 300, 100), Rect(0, 100, 300, 200))
        val edge = button.copy(rect = Rect(0, 65, 35, 35))
        val needsTallPane = button.copy(rect = Rect(0, 0, 90, 120), component = LayoutComponent.DPAD)
        val oversized = button.copy(rect = Rect(0, 0, 156, 312), component = LayoutComponent.HYBRID_SCREEN)
        val layout = UILayout(ScreenLayout(listOf(edge, needsTallPane, oversized)), ScreenLayout())
        val result = seam.keepComponentsOffSeam(layout).mainScreenLayout.components!!
        assertSame(edge, result[0])
        assertEquals(Rect(0, 100, 90, 120), result[1].rect)
        assertEquals(Rect(0, 100, 100, 200), result[2].rect)
    }

}
