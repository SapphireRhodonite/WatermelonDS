package me.magnum.melonds.ui.settings.viewmodel

import android.os.SystemClock
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.impl.RetroArchShaderLibraryManager
import me.magnum.melonds.impl.RetroArchShaderLibraryManager.InstallProgress
import me.magnum.melonds.impl.RetroArchShaderLibraryManager.ShaderInstallException
import javax.inject.Inject

@HiltViewModel
class ShaderInstallViewModel @Inject constructor(
    private val library: RetroArchShaderLibraryManager,
) : ViewModel() {
    sealed interface State {
        data object Idle : State
        data class Running(val progress: InstallProgress?) : State
        data object Installed : State
        data class Failed(val error: ShaderInstallException) : State
        data object Cancelled : State
    }

    private var job: Job? = null
    private val status = MutableStateFlow<State>(State.Idle)
    val state = status.asStateFlow()

    fun start() {
        if (job?.isCompleted == false) return
        job = viewModelScope.launch {
            status.value = State.Running(null)
            try {
                withContext(Dispatchers.IO) {
                    var lastUpdate = 0L
                    library.install { progress ->
                        val now = SystemClock.elapsedRealtime()
                        if (now - lastUpdate >= 250 || progress is InstallProgress.Finalizing) {
                            status.value = State.Running(progress)
                            lastUpdate = now
                        }
                    }
                }
                status.value = State.Installed
            } catch (e: CancellationException) {
                status.value = State.Cancelled
                throw e
            } catch (e: ShaderInstallException) {
                status.value = State.Failed(e)
            } catch (e: Exception) {
                status.value = State.Failed(ShaderInstallException(ShaderInstallException.Reason.HttpError))
            }
        }
    }

    fun cancel() {
        job?.cancel()
    }

    fun acknowledge() {
        status.value = State.Idle
    }
}
