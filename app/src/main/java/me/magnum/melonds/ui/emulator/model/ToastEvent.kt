package me.magnum.melonds.ui.emulator.model

import me.magnum.melonds.domain.model.VideoRenderer

sealed class ToastEvent {
    data object MultiplayerOperationUnavailable : ToastEvent()
    data object GbaLoadFailed : ToastEvent()
    data object RewindNotEnabled : ToastEvent()
    data object RewindNotAvailableWhileRAHardcoreModeEnabled : ToastEvent()
    data object StateSaveFailed : ToastEvent()
    data object StateLoadFailed : ToastEvent()
    data object InvalidAutoLoadState : ToastEvent()
    data object StateStateDoesNotExist : ToastEvent()
    data object QuickSaveSuccessful : ToastEvent()
    data object QuickLoadSuccessful : ToastEvent()
    data class ScreenshotSaved(val fileName: String) : ToastEvent()
    data object ScreenshotFailed : ToastEvent()
    data object CannotLoadSaveStatesWhenRAHardcoreIsEnabled : ToastEvent()
    data object CannotUseCheatsWhenRAHardcoreIsEnabled : ToastEvent()
    data object CannotSaveStateWhenRunningFirmware : ToastEvent()
    data object CannotLoadStateWhenRunningFirmware : ToastEvent()
    data object CannotSwitchRetroAchievementsMode : ToastEvent()
    data object OfflineAchievementsLedgerTampered : ToastEvent()
    data object OfflineAchievementsSyncFailed : ToastEvent()
    data object PendingRaStateVerificationFailed : ToastEvent()
    data object RetroAchievementsAccountChangedInGame : ToastEvent()
    data object RetroAchievementsLogoutFailed : ToastEvent()
    data object RAOfflineProxyNotActive : ToastEvent()
    data object RetroAchievementsProviderChangedRestartRequired : ToastEvent()
    data class HardcoreOfflineUnsyncedWarning(
        val pendingHardcoreCount: Int,
    ) : ToastEvent()
    data class HardcoreQueueSyncResult(
        val submittedCount: Int,
        val remainingCount: Int,
    ) : ToastEvent()
    data class RetroAchievementsMode(
        val status: RetroAchievementsModeStatus,
        val offlineNoInternetAtStart: Boolean = false,
        val hardcoreOfflineDisabled: Boolean = false,
    ) : ToastEvent()
    data class OfflineSoftcorePendingNotice(
        val pendingSoftcoreCount: Int,
        val ledgerExpiresInMs: Long? = null,
    ) : ToastEvent()

    enum class OfflineAchievementNotSyncedReason {
        MISSING_FROM_CURRENT_SET,
        DEFINITION_CHANGED,
        NOT_IN_PREFETCH_CACHE,
        SERVER_REJECTED,
    }

    enum class RetroAchievementsModeStatus {
        SOFTCORE,
        HARDCORE,
        SOFTCORE_OFFLINE,
    }

    data class OfflineAchievementNotSynced(
        val title: String,
        val reason: OfflineAchievementNotSyncedReason,
        val reasonDetail: String? = null,
    ) : ToastEvent()

    data class OfflineAchievementsNotSyncedSummary(
        val skippedCount: Int,
    ) : ToastEvent()
    data class RendererInitFailed(
        val renderer: VideoRenderer,
    ) : ToastEvent()
    data class RendererDebugCaptureLogged(
        val captureId: String,
    ) : ToastEvent()
    data object RendererDebugCaptureFailed : ToastEvent()
    data object GbaModeNotSupported : ToastEvent()
    data object InternalError : ToastEvent()
    data object WirelessUnsupported : ToastEvent()
}
