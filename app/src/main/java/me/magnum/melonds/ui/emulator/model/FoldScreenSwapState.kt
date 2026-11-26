package me.magnum.melonds.ui.emulator.model

import me.magnum.melonds.domain.model.DualScreenPreset

internal class FoldScreenSwapState {
    private var singleWindowSwapped = false
    private var foldPreset: DualScreenPreset? = null
    private var foldSwapped = false

    val isSwapped: Boolean get() = if (foldPreset == null) singleWindowSwapped else foldSwapped

    fun setFoldPreset(preset: DualScreenPreset?) {
        if (foldPreset == preset) return
        foldPreset = preset
        foldSwapped = preset == DualScreenPreset.INTERNAL_BOTTOM_EXTERNAL_TOP
    }

    fun toggle() {
        if (foldPreset == null) singleWindowSwapped = !singleWindowSwapped else foldSwapped = !foldSwapped
    }
}
