package me.magnum.melonds.ui.emulator.rom

import me.magnum.melonds.R
import me.magnum.melonds.ui.emulator.PauseMenuOption

enum class RomPauseMenuOption(override val textResource: Int) : PauseMenuOption {
    SETTINGS(R.string.settings),
    ROM_SETTINGS(R.string.rom_settings),
    SAVE_STATE(R.string.save_state),
    LOAD_STATE(R.string.load_state),
    REWIND(R.string.rewind),
    CHEATS(R.string.cheats),
    VIEW_ACHIEVEMENTS(R.string.achievements),
    SYNC_RETRO_ACHIEVEMENTS(R.string.ra_pending_sync_menu),
    PRESETS(R.string.presets),
    SCREEN_GAP(R.string.screen_gap),
    RENDERER_DEBUG(R.string.renderer_debug_menu),
    RESET(R.string.reset),
    EXIT(R.string.exit)
}
