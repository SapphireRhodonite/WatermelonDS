package me.magnum.melonds.multiplayer.phantom

import android.app.ActivityManager
import android.app.ActivityOptions
import android.app.Service
import android.content.ComponentName
import android.content.Intent
import android.content.ServiceConnection
import android.os.Binder
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.ProgressBar
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.R
import me.magnum.melonds.extensions.parcelable
import me.magnum.melonds.multiplayer.MultiplayerSession
import me.magnum.melonds.parcelables.RomParcelable
import me.magnum.melonds.ui.emulator.EmulatorActivity
import java.io.File
import javax.inject.Inject

class PhantomCoordinatorService : Service() {
    override fun onBind(intent: Intent): IBinder? = intent.extras?.getBinder(PhantomPlayer.OWNER)
}

abstract class PhantomPlayerService : Service() {
    override fun onBind(intent: Intent): IBinder = Binder()
}

class PhantomPrimaryService : PhantomPlayerService()
class PhantomPeerService : PhantomPlayerService()

@AndroidEntryPoint
class PhantomActivity : AppCompatActivity() {
    @Inject lateinit var saves: PhantomSaves
    private val rom by lazy { checkNotNull(intent.extras?.parcelable<RomParcelable>(EmulatorActivity.KEY_ROM)).rom }
    private val main = Handler(Looper.getMainLooper())
    private val inbox = Messenger(Handler(Looper.getMainLooper()) { msg -> receive(msg); true })
    private val players = arrayOfNulls<Messenger>(2)
    private val bindings = arrayOfNulls<ServiceConnection>(2)
    private val tasks = intArrayOf(-1, -1)
    private val launched = BooleanArray(2)
    private var directory: File? = null
    private val stopped = BooleanArray(2)
    private var closing = false
    private var dialog: AlertDialog? = null
    private var reason: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val content = FrameLayout(this)
        content.addView(ProgressBar(this), FrameLayout.LayoutParams(-2, -2, Gravity.CENTER))
        setContentView(content)
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() { end() }
        })
        lifecycleScope.launch {
            try {
                checkIdle()
                val pending = withContext(Dispatchers.IO) { saves.pending(rom) }
                if (pending != null) {
                    directory = pending
                    showResult()
                } else {
                    directory = withContext(Dispatchers.IO) { saves.prepare(rom) }
                    launchPlayer(0)
                }
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { fail(e.localizedMessage) }
        }
    }

    private fun checkIdle() {
        val tasks = getSystemService(ActivityManager::class.java).appTasks
        val emulators = setOf(EmulatorActivity::class.java.name, PhantomPrimaryActivity::class.java.name, PhantomPeerActivity::class.java.name)
        check(!MultiplayerSession.active && tasks.none { it.taskInfo.topActivity?.className in emulators }) {
            getString(R.string.phantom_busy)
        }
    }

    private fun launchPlayer(role: Int) {
        val component = if (role == 0) PhantomPrimaryActivity::class.java else PhantomPeerActivity::class.java
        val args = Bundle().apply {
            putString(PhantomPlayer.SESSION, checkNotNull(directory).name)
            putBinder(PhantomPlayer.OWNER, inbox.binder)
            putParcelable(EmulatorActivity.KEY_ROM, RomParcelable(rom))
        }
        val launch = Intent(this, component).putExtras(args).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        val options = ActivityOptions.makeBasic()
        if (Build.VERSION.SDK_INT >= 26) {
            options.launchDisplayId = ContextCompat.getDisplayOrDefault(this).displayId
        }
        startActivity(launch, options.toBundle())
        launched[role] = true
        main.postDelayed({
            if (launched[role] && players[role] == null && !isFinishing) {
                reason = getString(R.string.phantom_unavailable)
                launched[role] = false
                end()
            }
        }, 20000)
    }

    private fun receive(msg: Message) {
        val role = msg.arg1
        if (role !in players.indices || directory?.name != msg.data.getString(PhantomPlayer.SESSION)) return
        when (msg.what) {
            PhantomPlayer.ATTACH -> {
                val endpoint = msg.replyTo ?: return
                players[role] = endpoint
                tasks[role] = msg.arg2
                val death = IBinder.DeathRecipient { main.post {
                    if (players[role] === endpoint) {
                        if (!stopped[role] && !closing) reason = getString(R.string.phantom_unavailable)
                        exited(role)
                    }
                } }
                try { endpoint.binder.linkToDeath(death, 0) } catch (_: android.os.RemoteException) { death.binderDied() }
                if (!closing && !bindPlayer(role)) {
                    reason = getString(R.string.phantom_unavailable)
                    end()
                    return
                }
                if (closing) stop(role)
            }
            PhantomPlayer.READY -> if (role == 0 && !closing && !launched[1]) {
                try { launchPlayer(1) } catch (e: Exception) { reason = e.localizedMessage; end() }
            }
            PhantomPlayer.SWITCH -> if (!closing) bringToFront(tasks[1 - role])
            PhantomPlayer.CLOSE -> if (!closing) {
                bringToFront(taskId)
                end()
            }
            PhantomPlayer.ERROR -> { reason = msg.data.getString("detail"); end() }
            PhantomPlayer.CLOSED -> {
                stopped[role] = true
                unbindPlayer(role)
                closing = true
                stop(1 - role)
            }
        }
    }

    private fun exited(role: Int) {
        unbindPlayer(role)
        launched[role] = false
        players[role] = null
        tasks[role] = -1
        end()
    }

    private fun stop(role: Int) {
        try { players[role]?.send(Message.obtain(null, PhantomPlayer.CLOSE)) }
        catch (_: android.os.RemoteException) {
            unbindPlayer(role)
            players[role] = null
            launched[role] = false
        }
    }

    private fun bindPlayer(role: Int): Boolean {
        if (bindings[role] != null) return true
        val type = if (role == 0) PhantomPrimaryService::class.java else PhantomPeerService::class.java
        val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, service: IBinder) = Unit
            override fun onServiceDisconnected(name: ComponentName) = Unit
        }
        if (!bindService(Intent(this, type), connection, BIND_AUTO_CREATE)) return false
        bindings[role] = connection
        return true
    }

    private fun unbindPlayer(role: Int) {
        val connection = bindings[role] ?: return
        bindings[role] = null
        unbindService(connection)
    }

    private fun end() {
        closing = true
        players.indices.forEach(::stop)
        if (launched.none { it }) {
            if (directory == null) finish() else showResult()
        }
    }

    private fun bringToFront(target: Int) {
        getSystemService(ActivityManager::class.java).appTasks.firstOrNull {
            @Suppress("DEPRECATION") val id = it.taskInfo.id
            id == target
        }?.moveToFront()
    }

    private fun showResult() {
        if (isFinishing || dialog?.isShowing == true) return
        bringToFront(taskId)
        dialog = AlertDialog.Builder(this)
            .setTitle(R.string.phantom_result_title)
            .setMessage(listOfNotNull(reason, getString(R.string.phantom_result_body)).joinToString("\n\n"))
            .setPositiveButton(R.string.phantom_keep) { _, _ -> finalizeSaves(true) }
            .setNegativeButton(R.string.phantom_discard) { _, _ -> finalizeSaves(false) }
            .setNeutralButton(R.string.cancel) { _, _ -> finish() }
            .setCancelable(false).show()
    }

    private fun finalizeSaves(keep: Boolean) {
        val folder = directory ?: return
        lifecycleScope.launch {
            try {
                checkIdle()
                withContext(Dispatchers.IO) { saves.finish(folder, keep) }
                Toast.makeText(this@PhantomActivity, if (keep) R.string.phantom_kept else R.string.phantom_discarded, Toast.LENGTH_SHORT).show()
                finish()
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { fail(e.localizedMessage) }
        }
    }

    private fun fail(message: String?) {
        AlertDialog.Builder(this).setTitle(R.string.phantom_title).setMessage(message ?: getString(R.string.phantom_unavailable))
            .setPositiveButton(android.R.string.ok) { _, _ -> finish() }.setOnCancelListener { finish() }.show()
    }

    override fun onDestroy() {
        main.removeCallbacksAndMessages(null)
        players.indices.forEach { role ->
            stop(role)
            unbindPlayer(role)
        }
        super.onDestroy()
    }
}
