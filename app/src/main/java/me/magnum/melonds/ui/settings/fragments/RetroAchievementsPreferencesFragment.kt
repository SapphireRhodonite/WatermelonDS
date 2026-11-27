package me.magnum.melonds.ui.settings.fragments

import android.Manifest
import android.os.Build
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.app.NotificationManagerCompat
import me.magnum.melonds.MelonDSApplication
import me.magnum.melonds.extensions.isNotificationPermissionGranted
import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.ContextThemeWrapper
import android.view.LayoutInflater
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.viewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.preference.Preference
import androidx.preference.ListPreference
import androidx.preference.SwitchPreference
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import me.magnum.melonds.R
import me.magnum.melonds.databinding.DialogRetroachievementsLoginBinding
import me.magnum.melonds.extensions.addOnPreferenceChangeListener
import me.magnum.melonds.ui.common.LoadingDialog
import me.magnum.melonds.ui.settings.PreferenceFragmentTitleProvider
import me.magnum.melonds.ui.settings.SettingsActivity
import me.magnum.melonds.ui.settings.flow.observeAsFlow
import me.magnum.melonds.ui.settings.model.RetroAchievementsAccountState
import me.magnum.melonds.ui.settings.preferences.RetroAchievementsProfilePreference
import me.magnum.melonds.ui.settings.viewmodel.RetroAchievementsSettingsViewModel
import me.magnum.melonds.common.retroachievements.RetroAchievementsEndpointProvider
import me.magnum.melonds.common.retroachievements.RetroAchievementsEndpointSnapshot
import javax.inject.Inject

@AndroidEntryPoint
class RetroAchievementsPreferencesFragment : BasePreferenceFragment(), PreferenceFragmentTitleProvider {

    private val viewModel by viewModels<RetroAchievementsSettingsViewModel>()

    @Inject
    lateinit var endpointProvider: RetroAchievementsEndpointProvider

    private var loginProgressDialog: LoadingDialog? = null

    private lateinit var lossAlerts: SwitchPreference
    private val notificationPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        setLossAlerts(granted)
        if (!granted && Build.VERSION.SDK_INT >= 33 && !shouldShowRequestPermissionRationale(Manifest.permission.POST_NOTIFICATIONS)) {
            AlertDialog.Builder(requireContext())
                .setMessage(R.string.ra_loss_alerts_blocked)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.settings) { _, _ ->
                    startActivity(Intent(android.provider.Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                        .putExtra(android.provider.Settings.EXTRA_APP_PACKAGE, requireContext().packageName))
                }.show()
        }
    }

    private fun setLossAlerts(enabled: Boolean) {
        preferenceManager.sharedPreferences?.edit()?.putBoolean(MelonDSApplication.LOSS_ALERTS_KEY, enabled)?.apply()
        refreshLossAlerts()
        if (enabled) {
            (requireActivity().application as MelonDSApplication).recoverUnexpectedHardcoreOfflineLossIfNeeded()
        }
    }

    private fun refreshLossAlerts() {
        lossAlerts.isChecked = preferenceManager.sharedPreferences?.getBoolean(MelonDSApplication.LOSS_ALERTS_KEY, false) == true &&
            NotificationManagerCompat.from(requireContext()).areNotificationsEnabled()
    }

    override fun onResume() {
        super.onResume()
        refreshLossAlerts()
    }

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.pref_retroachievements, rootKey)

        lossAlerts = findPreference<SwitchPreference>(MelonDSApplication.LOSS_ALERTS_KEY)!!
        lossAlerts.setOnPreferenceChangeListener { _, value ->
            if (value == true && Build.VERSION.SDK_INT >= 33 && !requireContext().isNotificationPermissionGranted()) {
                notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
            } else {
                setLossAlerts(value == true)
            }
            false
        }

        val accountPreference = findPreference<Preference>("ra_login")!!
        val profilePreference = findPreference<RetroAchievementsProfilePreference>("ra_profile")!!
        val retroAchievementsEnabledPreference = findPreference<SwitchPreference>("ra_enabled")!!
        val hardcoreModePreference = findPreference<SwitchPreference>("ra_hardcore_enabled")!!
        val richPresencePreference = findPreference<SwitchPreference>("ra_rich_presence")!!
        val offlineBackendPreference = findPreference<ListPreference>("ra_offline_backend")!!
        val builtInOfflinePreference = findPreference<SwitchPreference>("ra_offline_softcore_enabled")!!
        val integrationPreferences = listOf(
            hardcoreModePreference,
            findPreference<SwitchPreference>("ra_unofficial_enabled")!!,
            findPreference<SwitchPreference>("ra_encore_enabled")!!,
            offlineBackendPreference,
            builtInOfflinePreference,
            findPreference<SwitchPreference>("ra_active_challenge_indicators")!!,
            findPreference<SwitchPreference>("ra_progress_indicators")!!,
            findPreference<SwitchPreference>("ra_leaderboard_indicators")!!,
        )

        hardcoreModePreference.addOnPreferenceChangeListener { _, newValue ->
            val isEnabled = newValue as Boolean
            if (!endpointProvider.allowHardcoreUserChoice(isEnabled)) {
                Toast.makeText(
                    requireContext(),
                    R.string.ra_offline_proxy_hardcore_not_supported,
                    Toast.LENGTH_LONG,
                ).show()
                return@addOnPreferenceChangeListener false
            }

            richPresencePreference.isVisible = !isEnabled
            if (isEnabled) {
                richPresencePreference.isChecked = true
            }
            true
        }

        offlineBackendPreference.addOnPreferenceChangeListener { _, newValue ->
            val backend = me.magnum.melonds.domain.model.retroachievements.RetroAchievementsOfflineBackend
                .fromPreference(newValue as? String)
            endpointProvider.setSelectedBackend(backend)
            if (backend == me.magnum.melonds.domain.model.retroachievements.RetroAchievementsOfflineBackend.RA_OFFLINE_PROXY &&
                endpointProvider.currentSnapshot().apiUrl == null
            ) {
                Toast.makeText(
                    requireContext(),
                    R.string.ra_offline_proxy_not_active,
                    Toast.LENGTH_LONG,
                ).show()
            }
            true
        }

        accountPreference.setOnPreferenceClickListener {
            val accountState = viewModel.accountState.value
            val runtimeIdentityLocked = requireActivity().intent.getBooleanExtra(
                SettingsActivity.KEY_RA_RUNTIME_IDENTITY_LOCKED,
                false,
            )
            when (accountState) {
                is RetroAchievementsAccountState.LoggedIn -> showLogoutConfirmationDialog()
                is RetroAchievementsAccountState.LoginExpired -> {
                    if (runtimeIdentityLocked) {
                        showInGameAccountChangeBlockedDialog()
                    } else {
                        showLoginDialog(accountState.existingUsername)
                    }
                }
                RetroAchievementsAccountState.LoggedOut -> {
                    if (runtimeIdentityLocked) {
                        showInGameAccountChangeBlockedDialog()
                    } else {
                        showLoginDialog(null)
                    }
                }
                RetroAchievementsAccountState.Unknown -> {
                    // Do nothing until a proper state is known
                }
            }
            true
        }

        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.accountState.collect {
                    when (it) {
                        is RetroAchievementsAccountState.LoggedIn -> {
                            accountPreference.title = getString(R.string.retroachievements_logout)
                            accountPreference.summary = getString(R.string.retroachievements_login_status, it.accountName)
                            accountPreference.notifyDependencyChange(false)
                        }
                        is RetroAchievementsAccountState.LoginExpired -> {
                            accountPreference.title = getString(R.string.login)
                            accountPreference.summary = getString(R.string.retroachievements_login_expired_status)
                            accountPreference.notifyDependencyChange(true)
                        }
                        RetroAchievementsAccountState.LoggedOut -> {
                            accountPreference.title = getString(R.string.login_with_retro_achievements)
                            accountPreference.summary = getString(R.string.retroachievements_login_summary)
                            accountPreference.notifyDependencyChange(true)
                        }
                        RetroAchievementsAccountState.Unknown -> {
                            accountPreference.title = getString(R.string.ellipsis)
                            accountPreference.summary = getString(R.string.ellipsis)
                            accountPreference.notifyDependencyChange(true)
                        }
                    }
                }
            }
        }

        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.accountState.combine(viewModel.userProfile) { accountState, profile ->
                    profile?.takeIf {
                        accountState is RetroAchievementsAccountState.LoggedIn &&
                            accountState.accountName == it.username
                    }
                }.collect(profilePreference::setProfile)
            }
        }

        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                val isLoggedInFlow = viewModel.accountState.map { it is RetroAchievementsAccountState.LoggedIn }
                combine(
                    isLoggedInFlow,
                    retroAchievementsEnabledPreference.observeAsFlow(),
                    hardcoreModePreference.observeAsFlow(),
                    endpointProvider.snapshot,
                ) { isLoggedIn, isRetroAchievementsEnabled, isHardcoreEnabled, endpoint ->
                    EndpointPreferenceState(
                        isLoggedIn,
                        isRetroAchievementsEnabled,
                        isHardcoreEnabled,
                        endpoint,
                    )
                }.collect { state ->
                    val isLoggedIn = state.isLoggedIn
                    val isRetroAchievementsEnabled = state.isRetroAchievementsEnabled
                    val isHardcoreEnabled = state.isHardcoreEnabled
                    val integrationOptionsEnabled = isLoggedIn && isRetroAchievementsEnabled
                    integrationPreferences.forEach { preference ->
                        preference.isVisible = integrationOptionsEnabled
                    }
                    val builtInEffective =
                        state.endpoint.backendEffective ==
                            me.magnum.melonds.domain.model.retroachievements.RetroAchievementsOfflineBackend.BUILT_IN
                    hardcoreModePreference.isVisible = integrationOptionsEnabled && builtInEffective
                    builtInOfflinePreference.isVisible = integrationOptionsEnabled && builtInEffective
                    richPresencePreference.isVisible =
                        integrationOptionsEnabled && !isHardcoreEnabled && builtInEffective
                    offlineBackendPreference.summary = when (state.endpoint.hostSource) {
                        RetroAchievementsEndpointSnapshot.HostSource.OFFICIAL ->
                            getString(R.string.ra_offline_backend_summary)
                        RetroAchievementsEndpointSnapshot.HostSource.RA_OFFLINE_PROXY ->
                            getString(R.string.ra_offline_proxy_active_summary)
                        RetroAchievementsEndpointSnapshot.HostSource.RA_OFFLINE_PROXY_UNAVAILABLE ->
                            getString(R.string.ra_offline_proxy_not_active)
                    }
                }
            }
        }

        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.loggingIn.collect { loggingIn ->
                    loginProgressDialog = if (loggingIn) {
                        LoadingDialog(requireContext()).apply {
                            show()
                        }
                    } else {
                        loginProgressDialog?.dismiss()
                        null
                    }
                }
            }
        }

        lifecycleScope.launch {
            lifecycle.repeatOnLifecycle(Lifecycle.State.RESUMED) {
                viewModel.loginErrorEvent.collect {
                    Toast.makeText(requireContext(), R.string.retro_achievements_login_error_short, Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    private fun showLoginDialog(existingUsername: String?) {
        val themedDialogContext = ContextThemeWrapper(requireContext(), R.style.MaterialDialog)
        val binding = DialogRetroachievementsLoginBinding.inflate(LayoutInflater.from(themedDialogContext))
        if (existingUsername != null) {
            binding.textUsername.setText(existingUsername)
        }

        AlertDialog.Builder(themedDialogContext)
            .setTitle(R.string.login_with_retro_achievements)
            .setView(binding.root)
            .setPositiveButton(R.string.login) { dialog, _ ->
                viewModel.login(
                    binding.textUsername.text?.toString() ?: "",
                    binding.textPassword.text?.toString() ?: "",
                )
                dialog.dismiss()
            }
            .setNegativeButton(R.string.cancel) { dialog, _ ->
                dialog.dismiss()
            }
            .show()
    }

    private fun showLogoutConfirmationDialog() {
        val inGameRuntimeIdentityLocked =
            requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_IN_GAME, false) &&
                requireActivity().intent.getBooleanExtra(
                    SettingsActivity.KEY_RA_RUNTIME_IDENTITY_LOCKED,
                    false,
                )
        val inGameLogoutSupported =
            inGameRuntimeIdentityLocked &&
                requireActivity().intent.getBooleanExtra(
                    SettingsActivity.KEY_RA_IN_GAME_LOGOUT_SUPPORTED,
                    false,
                )
        if (inGameRuntimeIdentityLocked && !inGameLogoutSupported) {
            showInGameAccountChangeBlockedDialog()
            return
        }
        AlertDialog.Builder(requireContext())
            .setTitle(R.string.retroachievements_logout)
            .setMessage(
                if (inGameLogoutSupported) {
                    R.string.retroachievements_logout_confirmation_in_game
                } else {
                    R.string.retroachievements_logout_confirmation
                },
            )
            .setPositiveButton(R.string.retroachievements_logout) { dialog, _ ->
                if (inGameLogoutSupported) {
                    requireActivity().setResult(
                        Activity.RESULT_OK,
                        Intent().putExtra(SettingsActivity.KEY_RA_LOGOUT_REQUESTED, true),
                    )
                    requireActivity().finish()
                } else {
                    viewModel.logoutFromRetroAchievements()
                }
                dialog.dismiss()
            }
            .setNegativeButton(R.string.cancel) { dialog, _ ->
                dialog.dismiss()
            }
            .show()
    }

    private fun showInGameAccountChangeBlockedDialog() {
        AlertDialog.Builder(requireContext())
            .setTitle(R.string.retroachievements)
            .setMessage(R.string.retroachievements_account_change_blocked_in_game)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    override fun getTitle() = getString(R.string.retroachievements)

    private data class EndpointPreferenceState(
        val isLoggedIn: Boolean,
        val isRetroAchievementsEnabled: Boolean,
        val isHardcoreEnabled: Boolean,
        val endpoint: RetroAchievementsEndpointSnapshot,
    )
}
