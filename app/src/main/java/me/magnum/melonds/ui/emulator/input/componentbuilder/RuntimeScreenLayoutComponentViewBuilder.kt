package me.magnum.melonds.ui.emulator.input.componentbuilder

import android.content.Context
import android.graphics.Rect
import android.graphics.drawable.Drawable
import android.graphics.drawable.LayerDrawable
import me.magnum.melonds.R
import me.magnum.melonds.multiplayer.phantom.PhantomPlayer
import me.magnum.melonds.ui.common.componentbuilders.ScreenLayoutComponentViewBuilder

class RuntimeScreenLayoutComponentViewBuilder(
    private val hybrid: Boolean = false,
) : ScreenLayoutComponentViewBuilder() {
    override fun getBackgroundDrawable(context: Context): Drawable? {
        if (PhantomPlayer.current?.primary != false) return null
        val frame = context.getDrawable(R.drawable.phantom_clone_frame) ?: return null
        if (!hybrid) return frame
        return object : LayerDrawable(arrayOf(frame, context.getDrawable(R.drawable.phantom_clone_frame))) {
            override fun onBoundsChange(bounds: Rect) {
                val middle = bounds.centerY()
                getDrawable(0).setBounds(bounds.left, bounds.top, bounds.right, middle)
                getDrawable(1).setBounds(bounds.left, middle, bounds.right, bounds.bottom)
            }
        }
    }

    override fun getAspectRatio() = if (hybrid) 256f / 384f else super.getAspectRatio()
}
