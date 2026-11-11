package me.magnum.melonds.ui.emulator.model

import me.magnum.melonds.ui.emulator.PauseMenuOption

data class PauseMenu(
    val options: List<PauseMenuOption>,
    val labelOverrides: Map<PauseMenuOption, String> = emptyMap(),
    val isPaused: Boolean = true,
) {
    fun labelOverride(option: PauseMenuOption): String? = labelOverrides[option]
}
