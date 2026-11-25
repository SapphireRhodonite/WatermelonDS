package me.magnum.melonds.ui.emulator.model

import me.magnum.melonds.domain.model.layout.ScreenGap
import me.magnum.melonds.domain.model.RomInfo
import me.magnum.melonds.domain.model.SaveStateSlot
import me.magnum.melonds.domain.model.VideoFiltering
import me.magnum.melonds.domain.model.VideoRenderer
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.emulator.rewind.model.RewindWindow

sealed class EmulatorUiEvent {
    sealed class OpenScreen : EmulatorUiEvent() {
        data class SettingsScreen(
            val romSettingsOverrides: InGameRomSettingsOverrides = InGameRomSettingsOverrides(),
            val retroAchievementsRuntimeIdentityLocked: Boolean = false,
            val retroAchievementsInGameLogoutSupported: Boolean = false,
        ) : OpenScreen()
        data class CheatsScreen(val romInfo: RomInfo) : OpenScreen()
    }
    data class ShowPauseMenu(val pauseMenu: PauseMenu) : EmulatorUiEvent()
    data class ShowRewindWindow(val rewindWindow: RewindWindow) : EmulatorUiEvent()
    data class ShowRomSaveStates(val saveStates: List<SaveStateSlot>, val reason: Reason) : EmulatorUiEvent() {
        enum class Reason {
            SAVING,
            LOADING,
        }
    }
    data object ShowAchievementList : EmulatorUiEvent()
    data object ShowPendingSubmissionsDialog : EmulatorUiEvent()
    data object CloseEmulator : EmulatorUiEvent()
    data class ShowScreenGap(val current: ScreenGap) : EmulatorUiEvent()
    data object ShowDualScreenPresets : EmulatorUiEvent()
    data object ShowRendererDebugMenu : EmulatorUiEvent()
    data object ShowRenderer2DDebugControls : EmulatorUiEvent()
    data class ShowRomSettings(
        val rom: Rom,
        val renderer: VideoRenderer,
        val menuState: InGameRomSettingsMenuState,
    ) : EmulatorUiEvent()
    data class ShowOfflineAchievementsSyncChoice(
        val pendingUnlockCount: Int,
        val ledgerExpiresInMs: Long?,
    ) : EmulatorUiEvent()
    data class ShowOfflineAchievementsSyncProgress(val totalUnlockCount: Int) : EmulatorUiEvent()
    data object HideOfflineAchievementsSyncProgress : EmulatorUiEvent()
}

enum class RaPendingSyncResultAction {
    REOPEN_PAUSE_MENU,
    RESUME_SESSION,
    REOPEN_TERMINAL_EXIT,
}
