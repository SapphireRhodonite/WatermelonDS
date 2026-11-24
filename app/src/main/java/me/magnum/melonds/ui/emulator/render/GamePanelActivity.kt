package me.magnum.melonds.ui.emulator.render

import android.app.Activity
import android.app.ActivityManager
import android.app.ActivityOptions
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Binder
import android.os.Build
import android.os.Bundle
import android.view.Display
import android.view.View
import android.view.ViewGroup
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import android.view.WindowManager
import androidx.core.content.ContextCompat

class GamePanelActivity : Activity() {
    internal class Session(val displayId: Int, val gameTask: ActivityManager.AppTask, val content: View?) : Binder() {
        var active = true
            private set
        var panel: GamePanelActivity? = null

        fun close() {
            active = false
            panel?.finishAndRemoveTask()
            panel = null
        }
    }

    internal companion object {
        private const val SESSION = "gamePanelSession"

        fun canOpen(context: Activity, display: Display): Boolean {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q ||
                !context.packageManager.hasSystemFeature(PackageManager.FEATURE_ACTIVITIES_ON_SECONDARY_DISPLAYS)) return false
            return context.getSystemService(ActivityManager::class.java).isActivityStartAllowedOnDisplay(
                context, display.displayId, Intent(context, GamePanelActivity::class.java)
            )
        }

        fun open(context: Activity, display: Display, content: View? = null): Session? {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q ||
                !context.packageManager.hasSystemFeature(PackageManager.FEATURE_ACTIVITIES_ON_SECONDARY_DISPLAYS)) return null
            val manager = context.getSystemService(ActivityManager::class.java)
            val gameTask = manager.appTasks.firstOrNull { it.taskInfo.taskId == context.taskId } ?: return null
            val session = Session(display.displayId, gameTask, content)
            val intent = Intent(context, GamePanelActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_MULTIPLE_TASK)
                .putExtras(Bundle().apply { putBinder(SESSION, session) })
            if (!manager.isActivityStartAllowedOnDisplay(context, display.displayId, intent)) return null
            val options = ActivityOptions.makeBasic().apply { launchDisplayId = display.displayId }
            return try {
                context.startActivity(intent, options.toBundle())
                session
            } catch (_: SecurityException) {
                null
            }
        }
    }

    private var session: Session? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val owner = intent.extras?.getBinder(SESSION) as? Session
        if (owner?.active != true || owner.displayId != ContextCompat.getDisplayOrDefault(this).displayId) {
            finishAndRemoveTask()
            return
        }
        session = owner
        owner.panel = this
        val content = owner.content
        if (content == null) {
            window.addFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE)
        } else {
            (content.parent as? ViewGroup)?.removeView(content)
            setContentView(content)
            WindowCompat.getInsetsController(window, window.decorView).hide(WindowInsetsCompat.Type.systemBars())
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        val owner = session
        if (hasFocus && owner?.active == true) owner.gameTask.moveToFront()
    }

    override fun onDestroy() {
        if (session?.panel === this) session?.panel = null
        super.onDestroy()
    }
}
