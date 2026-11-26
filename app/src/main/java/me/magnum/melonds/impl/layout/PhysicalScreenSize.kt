package me.magnum.melonds.impl.layout

import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Build
import android.util.DisplayMetrics
import android.view.Display
import androidx.core.content.getSystemService
import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.ScreenAlignment
import me.magnum.melonds.domain.model.layout.Insets
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.domain.model.layout.LayoutDisplay
import me.magnum.melonds.domain.model.layout.ScreenLayout
import me.magnum.melonds.domain.model.layout.UILayout
import me.magnum.melonds.domain.model.layout.UILayoutVariant
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.roundToInt

internal object PhysicalScreenSize {
    private data class Panel(val width: Int, val height: Int, val diagonal: Double) {
        fun matches(display: Metrics) =
            minOf(display.nativeWidth, display.nativeHeight) == height && maxOf(display.nativeWidth, display.nativeHeight) == width

        val ppi get() = hypot(width.toDouble(), height.toDouble()) / diagonal
    }

    private data class Profile(val manufacturer: String, val model: String, val device: String, val upper: Panel, val lower: Panel)

    private val thorUpper = Panel(1920, 1080, 6.0)
    private val thorLower = Panel(1240, 1080, 3.92)
    private val duoUpper = Panel(1920, 1080, 5.5)
    private val rgDsPanel = Panel(640, 480, 4.0)
    private val profiles = listOf(
        Profile("Anbernic", "RG DS", "rk3568_u", rgDsPanel, rgDsPanel),
        Profile("AYN", "AYN Thor", "kalama", thorUpper, thorLower),
        Profile("AYN", "AYN Thor Lite", "kona", thorUpper, thorLower),
        Profile("Moorechip", "Retroid Pocket Duo", "sun", duoUpper, Panel(1280, 960, 4.5)),
        Profile("Moorechip", "Retroid Pocket Duo Lite", "trinket", duoUpper, Panel(1280, 960, 4.2)),
        Profile("AYANEO", "Pocket DS", "PocketDS", Panel(1920, 1080, 7.0), Panel(1024, 768, 5.0)),
        Profile("QUALCOMM", "RG DDS", "pineapple", Panel(1200, 720, 4.7), Panel(960, 720, 4.0)),
    )

    internal data class Metrics(
        val nativeWidth: Int, val nativeHeight: Int,
        val display: LayoutDisplay,
        val xdpi: Double, val ydpi: Double,
    ) {
        fun logicalDpi(calibrated: Double?): Pair<Double, Double>? {
            if (minOf(nativeWidth, nativeHeight, display.width, display.height) <= 0) return null
            if (calibrated == null) {
                return if (xdpi.isFinite() && xdpi > 0 && ydpi.isFinite() && ydpi > 0) xdpi to ydpi else null
            }
            val longScale = maxOf(nativeWidth, nativeHeight).toDouble() / maxOf(display.width, display.height)
            val shortScale = minOf(nativeWidth, nativeHeight).toDouble() / minOf(display.width, display.height)
            val logicalPpi = calibrated / min(longScale, shortScale)
            return logicalPpi to logicalPpi
        }
    }

    @Suppress("DEPRECATION")
    private fun Display.readMetrics(mappedDisplay: LayoutDisplay): Metrics {
        val real = DisplayMetrics().also { getRealMetrics(it) }
        val native = mode
        return Metrics(native.physicalWidth, native.physicalHeight, mappedDisplay, real.xdpi.toDouble(), real.ydpi.toDouble())
    }

    fun matchLayout(
        context: Context, layout: UILayout, variant: UILayoutVariant,
        mainAlignment: ScreenAlignment?, secondaryAlignment: ScreenAlignment?,
    ): UILayout? {
        val main = variant.displays.mainScreenDisplay
        val secondary = variant.displays.secondaryScreenDisplay ?: return null
        val manager = context.getSystemService<DisplayManager>() ?: return null
        val mainDisplay = manager.getDisplay(main.id) ?: return null
        val otherDisplay = manager.getDisplay(secondary.id) ?: return null
        if (mainDisplay.displayId == otherDisplay.displayId) return null

        val secondaryCutout = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) otherDisplay.cutout else null
        val secondaryInsets = secondaryCutout?.let { Insets(it.safeInsetLeft, it.safeInsetTop, it.safeInsetRight, it.safeInsetBottom) } ?: Insets.Zero
        val mainInsets = variant.uiInsets
        return fit(
            layout, Build.MANUFACTURER, Build.MODEL, Build.DEVICE, mainDisplay.readMetrics(main), otherDisplay.readMetrics(secondary),
            mainInsets.availableRect(variant.uiSize.x, variant.uiSize.y),
            secondaryInsets.availableRect(secondary.width, secondary.height), mainAlignment, secondaryAlignment,
        )
    }

    private fun Insets.availableRect(width: Int, height: Int) = Rect(left, top, width - left - right, height - top - bottom)

    internal fun fit(
        layout: UILayout, manufacturer: String, model: String, device: String,
        main: Metrics, secondary: Metrics, mainArea: Rect, secondaryArea: Rect,
        mainAlignment: ScreenAlignment? = null, secondaryAlignment: ScreenAlignment? = null,
    ): UILayout? {
        val mainScreen = layout.mainScreenLayout.components?.singleOrNull { it.isScreen() } ?: return null
        val otherScreen = layout.secondaryScreenLayout.components?.singleOrNull { it.isScreen() } ?: return null
        if (setOf(mainScreen.component, otherScreen.component) != setOf(LayoutComponent.TOP_SCREEN, LayoutComponent.BOTTOM_SCREEN)) return null
        if (minOf(mainArea.width, mainArea.height, secondaryArea.width, secondaryArea.height) <= 0) return null

        val profile = profiles.find { it.manufacturer == manufacturer && it.model == model && it.device == device }
        val calibration = when {
            profile == null -> null
            profile.upper.matches(main) && profile.lower.matches(secondary) -> profile.upper.ppi to profile.lower.ppi
            profile.lower.matches(main) && profile.upper.matches(secondary) -> profile.lower.ppi to profile.upper.ppi
            else -> null
        }
        val mainDpi = main.logicalDpi(calibration?.first) ?: return null
        val otherDpi = secondary.logicalDpi(calibration?.second) ?: return null
        val widthInches = minOf(
            mainArea.width / mainDpi.first, mainArea.height / mainDpi.second * 4 / 3,
            secondaryArea.width / otherDpi.first, secondaryArea.height / otherDpi.second * 4 / 3,
        )

        fun ScreenLayout.resize(area: Rect, dpi: Pair<Double, Double>, alignment: ScreenAlignment?): ScreenLayout {
            val width = (widthInches * dpi.first).roundToInt().coerceIn(1, area.width)
            val height = (widthInches * 3 / 4 * dpi.second).roundToInt().coerceIn(1, area.height)
            val top = when (alignment) {
                ScreenAlignment.TOP -> area.y
                ScreenAlignment.BOTTOM -> area.bottom - height
                else -> area.y + (area.height - height) / 2
            }
            val rect = Rect(area.x + (area.width - width) / 2, top, width, height)
            return copy(components = components?.map { if (it.isScreen()) it.copy(rect = rect) else it })
        }

        return UILayout(layout.mainScreenLayout.resize(mainArea, mainDpi, mainAlignment), layout.secondaryScreenLayout.resize(secondaryArea, otherDpi, secondaryAlignment))
    }
}
