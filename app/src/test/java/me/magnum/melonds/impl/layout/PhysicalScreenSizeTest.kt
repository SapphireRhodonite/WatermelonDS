package me.magnum.melonds.impl.layout

import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.ScreenAlignment
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.domain.model.layout.LayoutDisplay
import me.magnum.melonds.domain.model.layout.PositionedLayoutComponent
import me.magnum.melonds.domain.model.layout.ScreenLayout
import me.magnum.melonds.domain.model.layout.UILayout
import me.magnum.melonds.impl.layout.PhysicalScreenSize.Metrics
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PhysicalScreenSizeTest {
    private val button = PositionedLayoutComponent(Rect(5, 6, 7, 8), LayoutComponent.BUTTON_A)
    private val top = PositionedLayoutComponent(Rect(0, 0, 256, 192), LayoutComponent.TOP_SCREEN)
    private val bottom = top.copy(component = LayoutComponent.BOTTOM_SCREEN)
    private val layout = UILayout(ScreenLayout(listOf(top)), ScreenLayout(listOf(bottom, button)))
    private fun panel(id: Int, width: Int, height: Int) = LayoutDisplay(id, LayoutDisplay.Type.BUILT_IN, width, height)

    private val thorUpper = Metrics(1080, 1920, panel(0, 1920, 1080), 160.0, 160.0)
    private val thorLower = Metrics(1080, 1240, panel(4, 1240, 1080), 160.0, 160.0)
    private val upperArea = Rect(0, 0, 1920, 1080)
    private val lowerArea = Rect(0, 0, 1240, 1080)

    @Test
    fun thorUsesBothPhysicalDiagonalsRetainsControlsAndFollowsPhysicalPanelsAcrossSwap() {
        val result = PhysicalScreenSize.fit(layout, "AYN", "AYN Thor", "kalama", thorUpper, thorLower, upperArea, lowerArea)!!
        assertEquals(top.copy(rect = Rect(417, 133, 1085, 814)), result.mainScreenLayout.components!!.single())
        assertEquals(listOf(bottom.copy(rect = Rect(0, 75, 1240, 930)), button), result.secondaryScreenLayout.components)
        val exchanged = UILayout(layout.secondaryScreenLayout, layout.mainScreenLayout)
        val reverse = PhysicalScreenSize.fit(exchanged, "AYN", "AYN Thor", "kalama", thorLower, thorUpper, lowerArea, upperArea)!!
        assertEquals(UILayout(result.secondaryScreenLayout, result.mainScreenLayout), reverse)
    }

    @Test
    fun nativeModeUsesLogicalScaleIndependentlyFromTheWindowAndUiDensity() {
        val logical = thorUpper.copy(display = panel(0, 960, 540), xdpi = 400.0, ydpi = 400.0)
        val window = Rect(17, 13, 800, 500)
        val result = PhysicalScreenSize.fit(
            layout, "AYN", "AYN Thor", "kalama", logical, thorLower, window, lowerArea,
            ScreenAlignment.BOTTOM, ScreenAlignment.TOP,
        )!!
        val main = result.mainScreenLayout.components!!.single().rect
        assertEquals(Rect(145, 106, 543, 407), main)
        assertEquals(Rect(0, 0, 1240, 930), result.secondaryScreenLayout.components!!.first().rect)
    }

    @Test
    fun ayaneoUsesPerDisplayLogicalBoundsEvenWhenAppContextReportsTheLowerPanelSize() {
        val upper = Metrics(1080, 1920, panel(0, 1920, 1080), 318.976, 320.842)
        val lower = Metrics(768, 1024, panel(2, 1024, 768), 227.0, 227.0)
        val lowerArea = Rect(0, 0, 1024, 768)
        fun match(main: Metrics) = PhysicalScreenSize.fit(
            layout, "AYANEO", "Pocket DS", "PocketDS", main, lower, upperArea, lowerArea,
            ScreenAlignment.BOTTOM, ScreenAlignment.TOP,
        )!!
        assertEquals(895, match(upper.copy(display = lower.display)).mainScreenLayout.components!!.single().rect.width)
        val result = match(upper)
        assertEquals(Rect(330, 136, 1259, 944), result.mainScreenLayout.components!!.single().rect)
        assertEquals(lowerArea, result.secondaryScreenLayout.components!!.first().rect)
    }

    @Test
    fun identityAndBothPanelModesMustMatchOrAndroidDpiIsUsedForBoth() {
        fun widths(model: String, lower: Metrics): List<Int> {
            val result = PhysicalScreenSize.fit(layout, "Moorechip", model, "sun", thorUpper, lower, upperArea, Rect(0, 0, 1280, 960))!!
            return listOf(result.mainScreenLayout.components!!.first().rect.width, result.secondaryScreenLayout.components!!.first().rect.width)
        }
        val duoLower = thorLower.copy(nativeWidth = 1280, nativeHeight = 960, display = panel(2, 1280, 960))
        assertEquals(listOf(1440, 1278), widths("Retroid Pocket Duo", duoLower))
        assertEquals(listOf(1280, 1280), widths("Retroid Pocket Duo Lite", duoLower))
        assertEquals(listOf(1280, 1280), widths("Retroid Pocket Duo", duoLower.copy(nativeWidth = 1279)))
    }

    @Test
    fun calibratedRgDsPanelsFillBothDespiteUnequalReportedAxisDpi() {
        val panel = Metrics(640, 480, panel(0, 640, 480), 70.986, 85.258)
        val area = Rect(0, 0, 640, 480)
        val result = PhysicalScreenSize.fit(layout, "Anbernic", "RG DS", "rk3568_u", panel, panel, area, area)!!
        assertEquals(area, result.mainScreenLayout.components!!.single().rect)
        assertEquals(area, result.secondaryScreenLayout.components!!.first().rect)
        assertNull(PhysicalScreenSize.fit(layout, "", "", "", panel.copy(ydpi = Double.NaN), panel, area, area))
        assertNull(PhysicalScreenSize.fit(layout.copy(secondaryScreenLayout = ScreenLayout()), "", "", "", panel, panel, area, area))
    }
}
