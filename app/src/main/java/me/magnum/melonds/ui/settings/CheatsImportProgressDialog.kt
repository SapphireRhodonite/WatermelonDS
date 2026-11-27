package me.magnum.melonds.ui.settings

import android.widget.Toast
import android.app.Dialog
import android.os.Bundle
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.viewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.launch
import me.magnum.melonds.R
import me.magnum.melonds.databinding.DialogCheatsImportProgressBinding
import me.magnum.melonds.domain.model.CheatImportProgress
import me.magnum.melonds.ui.settings.viewmodel.CheatsPreferencesViewModel
import kotlin.getValue

@AndroidEntryPoint
class CheatsImportProgressDialog : DialogFragment() {

    private val viewModel by viewModels<CheatsPreferencesViewModel>({ requireParentFragment() })
    private lateinit var binding: DialogCheatsImportProgressBinding

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        isCancelable = false
        binding = DialogCheatsImportProgressBinding.inflate(layoutInflater)

        return AlertDialog.Builder(requireContext())
            .setTitle(R.string.importing_cheats)
            .setView(binding.root)
            .setNegativeButton(android.R.string.cancel) { _, _ -> viewModel.cancel() }
            .setCancelable(false)
            .create()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.progress.collect {
                    when (it.status) {
                        CheatImportProgress.CheatImportStatus.STARTING -> {
                            binding.progressBarCheatImport.isIndeterminate = true
                            binding.textCheatImportItemName.setText(R.string.starting)
                        }
                        CheatImportProgress.CheatImportStatus.ONGOING -> {
                            binding.progressBarCheatImport.isIndeterminate = it.progress < 0f
                            binding.progressBarCheatImport.progress = (it.progress * 100).toInt()
                            binding.textCheatImportItemName.text = it.ongoingItemName
                        }
                        CheatImportProgress.CheatImportStatus.FAILED -> {
                            Toast.makeText(requireContext(), R.string.dsiware_manager_import_file_error, Toast.LENGTH_SHORT).show()
                            dismiss()
                        }
                        CheatImportProgress.CheatImportStatus.NOT_IMPORTING,
                        CheatImportProgress.CheatImportStatus.FINISHED -> dismiss()
                    }
                }
            }
        }
    }
}
