package me.magnum.melonds.ui.settings.viewmodel

import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import me.magnum.melonds.common.cheats.CheatDatabaseImporter
import me.magnum.melonds.domain.model.CheatImportProgress
import me.magnum.melonds.domain.model.CheatImportProgress.CheatImportStatus
import javax.inject.Inject

@HiltViewModel
class CheatsPreferencesViewModel @Inject constructor(
    private val importer: CheatDatabaseImporter,
) : ViewModel() {
    private var job: Job? = null
    private val state = MutableStateFlow(CheatImportProgress(CheatImportStatus.NOT_IMPORTING, 0f, null))
    val progress = state.asStateFlow()

    fun areCheatsBeingImported() = job?.isCompleted == false

    fun importCheatsDatabase(uri: Uri) {
        if (areCheatsBeingImported()) return
        job = viewModelScope.launch {
            state.value = CheatImportProgress(CheatImportStatus.STARTING, 0f, null)
            try {
                importer.run(uri) { name, fraction ->
                    state.value = CheatImportProgress(CheatImportStatus.ONGOING, fraction, name)
                }
                state.value = CheatImportProgress(CheatImportStatus.FINISHED, 1f, null)
            } catch (e: CancellationException) {
                state.value = CheatImportProgress(CheatImportStatus.NOT_IMPORTING, 0f, null)
                throw e
            } catch (e: Exception) {
                state.value = CheatImportProgress(CheatImportStatus.FAILED, 0f, null)
            }
        }
    }

    fun cancel() {
        job?.cancel()
    }
}
