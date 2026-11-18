package me.magnum.melonds.ui.settings

import android.graphics.Color
import android.os.Bundle
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Settings
import androidx.compose.runtime.collectAsState
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updateLayoutParams
import androidx.core.view.updatePadding
import androidx.fragment.app.commit
import androidx.fragment.app.commitNow
import androidx.core.os.bundleOf
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.PreferenceScreen
import dagger.hilt.android.AndroidEntryPoint
import me.magnum.melonds.R
import me.magnum.melonds.databinding.ActivitySettingsBinding
import me.magnum.melonds.ui.settings.fragments.CustomFirmwarePreferencesFragment
import me.magnum.melonds.ui.settings.fragments.MainPreferencesFragment

@AndroidEntryPoint
class SettingsActivity :
    AppCompatActivity(),
    PreferenceFragmentCompat.OnPreferenceStartFragmentCallback,
    PreferenceFragmentCompat.OnPreferenceStartScreenCallback {

    private val externalInfoController by lazy { me.magnum.melonds.ui.common.ExternalInfoDisplayController(this) }
    private val externalSettingsTitle = kotlinx.coroutines.flow.MutableStateFlow("")

    data class FocusedPref(val title: String, val summary: String?, val icon: android.graphics.drawable.Drawable?)
    private val focusedPreference = kotlinx.coroutines.flow.MutableStateFlow<FocusedPref?>(null)

    fun onPreferenceFocused(preference: androidx.preference.Preference) {
        focusedPreference.value = FocusedPref(
            title = preference.title?.toString() ?: "",
            summary = preference.summary?.toString(),
            icon = preference.icon,
        )
    }

    override fun onStart() {
        super.onStart()
        externalSettingsTitle.value = supportActionBar?.title?.toString() ?: getString(me.magnum.melonds.R.string.settings)
        supportFragmentManager.addOnBackStackChangedListener(externalTitleListener)
        externalInfoController.setContent {
            val title = externalSettingsTitle.collectAsState().value
            val focused = focusedPreference.collectAsState().value
            me.magnum.melonds.ui.common.ExternalSettingInfo(
                iconDrawable = focused?.icon,
                title = focused?.title?.takeIf { it.isNotBlank() } ?: title,
                description = focused?.summary,
                crumb = getString(me.magnum.melonds.R.string.settings) + " › " + title,
            )
        }
    }

    override fun onResume() {
        super.onResume()
        externalInfoController.attach()
    }

    override fun onPause() {
        externalInfoController.detach()
        super.onPause()
    }

    override fun onStop() {
        supportFragmentManager.removeOnBackStackChangedListener(externalTitleListener)
        super.onStop()
    }

    private val externalTitleListener = androidx.fragment.app.FragmentManager.OnBackStackChangedListener {
        binding.root.post {
            externalSettingsTitle.value = supportActionBar?.title?.toString() ?: ""
            focusedPreference.value = null
        }
    }

    companion object {
        const val KEY_ENTRY_POINT = "entry_point"
        const val KEY_IN_GAME = "in_game"
        const val KEY_LOCK_INPUT_MAPPING = "lock_input_mapping"
        const val KEY_LOCK_INPUT_LAYOUT = "lock_input_layout"
        const val KEY_LOCK_VIDEO_FILTERING = "lock_video_filtering"
        const val KEY_RA_RUNTIME_IDENTITY_LOCKED = "ra_runtime_identity_locked"
        const val KEY_RA_IN_GAME_LOGOUT_SUPPORTED = "ra_in_game_logout_supported"
        const val KEY_RA_LOGOUT_REQUESTED = "ra_logout_requested"

        const val CUSTOM_FIRMWARE_ENTRY_POINT = "custom_firmware_entry_point"
    }

    private lateinit var binding: ActivitySettingsBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT))
        super.onCreate(savedInstanceState)
        binding = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)

        var defaultContentInsetStartWithNavigation = -1
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, windowInsets ->
            val insets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            if (defaultContentInsetStartWithNavigation == -1) {
                defaultContentInsetStartWithNavigation = binding.toolbar.contentInsetStartWithNavigation
            }

            val startInset = if (binding.toolbar.layoutDirection == View.LAYOUT_DIRECTION_LTR) insets.left else insets.right
            binding.toolbar.contentInsetStartWithNavigation = defaultContentInsetStartWithNavigation + startInset
            binding.toolbar.updatePadding(
                left = insets.left,
                right = insets.right,
            )
            binding.viewStatusBarBackground.updateLayoutParams {
                height = insets.top
            }
            binding.settingsContainer.updateLayoutParams<ViewGroup.MarginLayoutParams> {
                leftMargin = insets.left
                rightMargin = insets.right
            }
            binding.settingsFooter.root.updatePadding(
                left = insets.left,
                right = insets.right,
                bottom = insets.bottom,
            )

            windowInsets.inset(insets.left, insets.top, insets.right, insets.bottom)
        }

        supportFragmentManager.addOnBackStackChangedListener {
            updateTitle()
        }

        if (savedInstanceState == null) {
            val entryPoint = when (intent.extras?.getString(KEY_ENTRY_POINT)) {
                CUSTOM_FIRMWARE_ENTRY_POINT -> CustomFirmwarePreferencesFragment::class
                else -> MainPreferencesFragment::class
            }

            supportFragmentManager.commitNow {
                replace(binding.settingsContainer.id, entryPoint.java, null)
            }
        }
        updateTitle()
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        val id = item.itemId
        if (id == android.R.id.home) {
            if (!supportFragmentManager.popBackStackImmediate()) {
                finish()
            }
            return true
        }
        return super.onOptionsItemSelected(item)
    }

    private fun updateTitle() {
        val fragment = supportFragmentManager.fragments.lastOrNull()
        if (fragment is PreferenceFragmentTitleProvider) {
            supportActionBar?.title = fragment.getTitle()
        }
    }

    override fun onPreferenceStartScreen(caller: PreferenceFragmentCompat, pref: PreferenceScreen): Boolean {
        val fragment = supportFragmentManager.fragmentFactory.instantiate(
            ClassLoader.getSystemClassLoader(),
            caller::class.java.name,
        ).apply {
            arguments = bundleOf(PreferenceFragmentCompat.ARG_PREFERENCE_ROOT to pref.key)
        }

        supportFragmentManager.commit {
            setCustomAnimations(R.anim.fragment_translate_enter_push, R.anim.fragment_translate_exit_push, R.anim.fragment_translate_enter_pop, R.anim.fragment_translate_exit_pop)
            replace(binding.settingsContainer.id, fragment)
            addToBackStack(null)
        }
        return true
    }

    override fun onPreferenceStartFragment(caller: PreferenceFragmentCompat, pref: Preference): Boolean {
        val fragmentClassName = pref.fragment ?: return false

        val fragment = supportFragmentManager.fragmentFactory.instantiate(ClassLoader.getSystemClassLoader(), fragmentClassName).apply {
            arguments = pref.extras
        }

        supportFragmentManager.commit {
            setCustomAnimations(R.anim.fragment_translate_enter_push, R.anim.fragment_translate_exit_push, R.anim.fragment_translate_enter_pop, R.anim.fragment_translate_exit_pop)
            replace(binding.settingsContainer.id, fragment)
            addToBackStack(null)
        }
        return true
    }
}
