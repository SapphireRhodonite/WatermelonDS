package me.magnum.melonds.impl.layout

import android.content.Context
import android.hardware.display.DisplayManager
import android.view.Display
import androidx.core.content.ContextCompat
import androidx.core.content.getSystemService
import me.magnum.melonds.impl.layout.devicemapper.isBuiltInPanel
import javax.inject.Inject

class SecondaryDisplaySelector @Inject constructor() {

    private companion object {
        val ExcludedDisplays = listOf(
            "HiddenDisplay", // Seems like a placeholder display present in some devices
            "WebRTC_ScreenCapture", // Used by apps that record the screen (e.g.: Discord)
        )
    }

    fun getSecondaryDisplay(context: Context, canHostGamePanel: (Display) -> Boolean = { false }): Display? {
        val currentDisplay = ContextCompat.getDisplayOrDefault(context)
        return context.getSystemService<DisplayManager>()?.let { displayManager ->
            val presentationDisplays = displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
            val candidates = displayManager.displays.filter { display ->
                display.displayId != currentDisplay.displayId && display.name !in ExcludedDisplays &&
                    display.state != Display.STATE_OFF && display.isValid
            }
            fun acceptsPresentation(display: Display) =
                display.displayId != Display.DEFAULT_DISPLAY || presentationDisplays.any { it.displayId == display.displayId }

            val builtInPartner = if (currentDisplay.isBuiltInPanel()) candidates.firstOrNull {
                it.isBuiltInPanel() && (acceptsPresentation(it) || canHostGamePanel(it))
            } else null
            builtInPartner ?: candidates.firstOrNull(::acceptsPresentation)
        }
    }
}
