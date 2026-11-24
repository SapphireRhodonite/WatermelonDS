package me.magnum.melonds.impl.layout.devicemapper

import android.content.Context
import android.hardware.display.DeviceProductInfo
import android.os.Build
import android.view.Display
import me.magnum.melonds.domain.model.layout.LayoutDisplay
import me.magnum.melonds.domain.model.layout.LayoutDisplayPair
import me.magnum.melonds.impl.layout.DeviceLayoutDisplayMapper

class DefaultLayoutDisplayMapper(context: Context) : DeviceLayoutDisplayMapper(context) {

    override fun mapDisplaysToLayoutDisplays(currentDisplay: Display, secondaryDisplay: Display?): LayoutDisplayPair {
        val mainLayoutDisplay = mapDisplayToLayoutDisplay(
            display = currentDisplay,
            displayType = if (currentDisplay.isBuiltInPanel()) LayoutDisplay.Type.BUILT_IN else LayoutDisplay.Type.EXTERNAL,
        )
        val secondaryLayoutDisplay = secondaryDisplay?.let {
            mapDisplayToLayoutDisplay(
                display = it,
                displayType = if (it.isBuiltInPanel()) LayoutDisplay.Type.BUILT_IN else LayoutDisplay.Type.EXTERNAL,
            )
        }

        return LayoutDisplayPair(mainLayoutDisplay, secondaryLayoutDisplay)
    }
}

internal fun Display.isBuiltInPanel(): Boolean {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        when (deviceProductInfo?.connectionToSinkType) {
            DeviceProductInfo.CONNECTION_TO_SINK_BUILT_IN -> return true
            DeviceProductInfo.CONNECTION_TO_SINK_DIRECT, DeviceProductInfo.CONNECTION_TO_SINK_TRANSITIVE -> return false
            DeviceProductInfo.CONNECTION_TO_SINK_UNKNOWN, null -> Unit
        }
    }
    return flags and Display.FLAG_PRIVATE == 0 && (name == "Built-in Screen" || name == "Built-in Screen#2")
}
