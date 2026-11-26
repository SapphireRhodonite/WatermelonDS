package me.magnum.melonds.impl.layout

import android.app.Activity
import android.content.pm.PackageManager
import android.hardware.Sensor
import android.hardware.SensorManager
import android.os.Build
import android.view.View
import androidx.window.WindowSdkExtensions
import androidx.window.layout.FoldingFeature
import androidx.window.layout.WindowInfoTracker
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import me.magnum.melonds.domain.model.Rect
import me.magnum.melonds.domain.model.layout.ScreenFold
import me.magnum.melonds.domain.model.ui.Orientation

internal fun Activity.observeSeparatingFolds(view: View, onFoldableWindow: ((Float) -> Unit)? = null): Flow<List<ScreenFold>> {
    val tracker = WindowInfoTracker.getOrCreate(this)
    var foldable = onFoldableWindow != null && supportsFoldedWindows(tracker)
    val viewBounds = callbackFlow {
        val origin = IntArray(2)
        fun updateBounds() {
            view.getLocationInWindow(origin)
            trySend(Rect(origin[0], origin[1], view.width, view.height))
        }
        val listener = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> updateBounds() }
        view.addOnLayoutChangeListener(listener)
        updateBounds()
        awaitClose { view.removeOnLayoutChangeListener(listener) }
    }
    return combine(tracker.windowLayoutInfo(this), viewBounds) { info, bounds ->
        val features = info.displayFeatures.filterIsInstance<FoldingFeature>()
        foldable = foldable || features.isNotEmpty()
        if (foldable) onFoldableWindow?.invoke(view.resources.displayMetrics.density)
        features.mapNotNull { feature ->
            val fold = feature.bounds
            if (!feature.isSeparating && (feature.occlusionType != FoldingFeature.OcclusionType.FULL || fold.isEmpty)) return@mapNotNull null
            val orientation = if (feature.orientation == FoldingFeature.Orientation.HORIZONTAL) Orientation.LANDSCAPE else Orientation.PORTRAIT
            val localBounds = when (orientation) {
                Orientation.LANDSCAPE -> {
                    if (fold.left > bounds.x || fold.right < bounds.right || fold.top <= bounds.y || fold.bottom >= bounds.bottom) return@mapNotNull null
                    Rect(0, fold.top - bounds.y, bounds.width, fold.height())
                }
                Orientation.PORTRAIT -> {
                    if (fold.top > bounds.y || fold.bottom < bounds.bottom || fold.left <= bounds.x || fold.right >= bounds.right) return@mapNotNull null
                    Rect(fold.left - bounds.x, 0, fold.width(), bounds.height)
                }
            }
            ScreenFold(orientation, ScreenFold.FoldType.SEAMLESS, localBounds)
        }
    }.distinctUntilChanged()
}

@Suppress("RequiresWindowSdk")
private fun Activity.supportsFoldedWindows(tracker: WindowInfoTracker): Boolean {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
        (packageManager.hasSystemFeature(PackageManager.FEATURE_SENSOR_HINGE_ANGLE) ||
            getSystemService(SensorManager::class.java)?.getDefaultSensor(Sensor.TYPE_HINGE_ANGLE) != null)
    ) return true
    if (WindowSdkExtensions.getInstance().extensionVersion >= 6) {
        return tracker.supportedPostures.isNotEmpty()
    }
    return false
}
