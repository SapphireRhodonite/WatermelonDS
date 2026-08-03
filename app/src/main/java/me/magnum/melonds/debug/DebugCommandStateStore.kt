package me.magnum.melonds.debug

import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.core.content.edit
import kotlinx.coroutines.delay
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.ui.emulator.EmulatorActivity
import me.magnum.melonds.ui.emulator.EmulatorViewModel
import me.magnum.melonds.ui.emulator.model.EmulatorState
import java.lang.ref.WeakReference
import kotlin.Lazy

internal object DebugCommandStateStore {
    private const val TAG = "DebugCommand"
    private const val PREFERENCES_NAME = "debug_command_state"
    private const val KEY_LAST_ROM_URI = "last_rom_uri"

    @Volatile
    private var currentEmulatorActivity = WeakReference<EmulatorActivity>(null)

    @Volatile
    private var debugPauseHeld = false

    @Volatile
    private var pendingPauseAfterRunningRom: Boolean? = null

    fun onEmulatorActivitySeen(context: Context, activity: EmulatorActivity) {
        currentEmulatorActivity = WeakReference(activity)
        val romUri = activity.intent?.data ?: return
        context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE).edit(commit = true) {
            putString(KEY_LAST_ROM_URI, romUri.toString())
        }
    }

    fun onEmulatorActivityDestroyed(activity: EmulatorActivity) {
        if (currentEmulatorActivity.get() === activity) {
            currentEmulatorActivity = WeakReference(null)
        }
    }

    fun getLastRomUri(context: Context): Uri? {
        val uri = context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)
            .getString(KEY_LAST_ROM_URI, null)
            ?: return null
        return Uri.parse(uri)
    }

    fun requestSettingsRefresh(): Boolean {
        val activity = currentEmulatorActivity.get() ?: return false
        val viewModel = resolveEmulatorViewModel(activity) ?: return false
        activity.runOnUiThread {
            viewModel.onSettingsChanged()
        }
        return true
    }

    fun isDebugPauseHeld(): Boolean = debugPauseHeld

    @Volatile
    private var pauseCommandSequence: Long = 0

    fun setDebugPauseHeld(held: Boolean) {
        debugPauseHeld = held
        pauseCommandSequence++
    }

    fun currentPauseCommandSequence(): Long = pauseCommandSequence

    fun requestPauseAfterNextRunningRom(pauseAfterReady: Boolean) {
        pendingPauseAfterRunningRom = pauseAfterReady
    }

    fun isRunningRom(): Boolean {
        val viewModel = currentEmulatorActivity.get()?.let { resolveEmulatorViewModel(it) }
        return viewModel?.emulatorState?.value is EmulatorState.RunningRom
    }

    fun hasEmulatorActivity(): Boolean {
        return currentEmulatorActivity.get() != null
    }

    fun onRunningRomReady(romUri: Uri, romName: String) {
        val pendingPause = pendingPauseAfterRunningRom
        pendingPauseAfterRunningRom = null

        if (pendingPause == true) {
            debugPauseHeld = true
            MelonEmulator.pauseEmulation()
        } else if (pendingPause == false) {
            debugPauseHeld = false
        }

        Log.w(
            TAG,
            "action=rom_ready name=$romName uri=$romUri pauseAfter=${when (pendingPause) { true -> 1; false -> 0; null -> -1 }}",
        )
    }

    suspend fun waitForRunningRom(timeoutMs: Long): Boolean {
        if (isRunningRom()) {
            return true
        }
        val deadlineAt = System.nanoTime() + timeoutMs.coerceAtLeast(1L) * 1_000_000L
        while (System.nanoTime() < deadlineAt) {
            if (isRunningRom()) {
                return true
            }
            delay(100L)
        }
        return false
    }

    private fun resolveEmulatorViewModel(activity: EmulatorActivity): EmulatorViewModel? {
        return try {
            val field = EmulatorActivity::class.java.getDeclaredField("viewModel\$delegate")
            field.isAccessible = true
            val delegate = field.get(activity) as? Lazy<*>
            delegate?.value as? EmulatorViewModel
        } catch (error: ReflectiveOperationException) {
            Log.w(TAG, "Failed to resolve EmulatorViewModel from EmulatorActivity", error)
            null
        }
    }
}
