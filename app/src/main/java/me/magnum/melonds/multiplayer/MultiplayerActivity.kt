package me.magnum.melonds.multiplayer

import android.Manifest
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.KeyEvent
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.flow.MutableSharedFlow
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.DSiProfile
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.common.ExternalInfoDisplayController
import me.magnum.melonds.ui.common.rom.EmulatorLaunchValidatorDelegate
import me.magnum.melonds.ui.emulator.EmulatorActivity
import me.magnum.melonds.ui.settings.SettingsActivity
import me.magnum.melonds.ui.theme.MelonTheme

@AndroidEntryPoint
class MultiplayerActivity : AppCompatActivity() {
    private lateinit var validator: EmulatorLaunchValidatorDelegate
    private val viewModel: MultiplayerViewModel by viewModels()
    private val externalInfo by lazy { ExternalInfoDisplayController(this) }
    private val pad = MutableSharedFlow<Int>(extraBufferCapacity = 8)
    private var permissionResult: (() -> Unit)? = null
    private val permissionLauncher = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        permissionResult?.invoke()
        permissionResult = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT))
        super.onCreate(savedInstanceState)
        validator = EmulatorLaunchValidatorDelegate(this, object : EmulatorLaunchValidatorDelegate.Callback {
            override fun onRomValidated(rom: Rom) {
                startActivity(EmulatorActivity.getRomEmulatorActivityIntent(this@MultiplayerActivity, rom))
            }
            override fun onValidationAborted() {}
            override fun onFirmwareValidated(consoleType: ConsoleType, dsiProfile: DSiProfile?) {
                startActivity(EmulatorActivity.getFirmwareEmulatorActivityIntent(this@MultiplayerActivity, consoleType, startMultiplayerSession = true))
            }
        })
        val preferences = getSharedPreferences("watermelon_multiplayer", Context.MODE_PRIVATE)
        val actions = MultiplayerActions(
            exit = { finish() },
            launchRom = { validator.validateRom(it) },
            launchFirmware = { validator.validateFirmware(ConsoleType.DS) },
            openBiosSettings = {
                startActivity(Intent(this, SettingsActivity::class.java)
                    .putExtra(SettingsActivity.KEY_ENTRY_POINT, SettingsActivity.CUSTOM_FIRMWARE_ENTRY_POINT))
            },
            openWifiSettings = { runCatching { startActivity(Intent(Settings.ACTION_WIFI_SETTINGS)) } },
            requestPermission = { done ->
                if (viewModel.directPermitted()) done() else {
                    permissionResult = done
                    val permission = MultiplayerDirectNetwork.permission()
                    permissionLauncher.launch(if (Build.VERSION.SDK_INT < 33)
                        arrayOf(permission, Manifest.permission.ACCESS_COARSE_LOCATION) else arrayOf(permission))
                }
            },
            permissionPrompted = { preferences.getBoolean("permission_prompted", false) },
            markPermissionPrompted = { preferences.edit().putBoolean("permission_prompted", true).apply() },
        )
        setContent {
            MelonTheme {
                MultiplayerScreen(viewModel, pad, actions, intent.getStringExtra(EXTRA_JOIN))
            }
        }
    }

    override fun onStart() {
        super.onStart()
        externalInfo.attach()
        externalInfo.setContent { MultiplayerExternalInfo(viewModel) }
    }

    override fun onStop() {
        externalInfo.detach()
        super.onStop()
    }

    companion object {
        const val EXTRA_JOIN = "join"
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> return super.dispatchKeyEvent(KeyEvent(event.downTime, event.eventTime, event.action,
                KeyEvent.KEYCODE_DPAD_CENTER, event.repeatCount, event.metaState, event.deviceId, event.scanCode, event.flags, event.source))
            KeyEvent.KEYCODE_BUTTON_B -> {
                if (event.action == KeyEvent.ACTION_UP) onBackPressedDispatcher.onBackPressed()
                return true
            }
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2 -> {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) pad.tryEmit(event.keyCode)
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }
}
