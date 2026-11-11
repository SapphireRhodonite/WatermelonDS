package me.magnum.melonds.multiplayer.phantom

import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import me.magnum.melonds.R
import me.magnum.melonds.multiplayer.Tag
import me.magnum.melonds.multiplayer.TagKind
import me.magnum.melonds.ui.emulator.EmulatorActivity
import me.magnum.melonds.ui.theme.MelonTheme

abstract class PhantomPlayerActivity : EmulatorActivity() {
    private lateinit var player: PhantomPlayer

    override fun onCreate(savedInstanceState: Bundle?) {
        player = PhantomPlayer(this, this is PhantomPrimaryActivity)
        PhantomPlayer.current = player
        super.onCreate(savedInstanceState)
        player.attach()
        val wrap = ViewGroup.LayoutParams.WRAP_CONTENT
        val badge = ComposeView(this).apply {
            setContent {
                MelonTheme {
                    Box(Modifier.padding(8.dp)) {
                        val label = if (player.primary) R.string.phantom_playing_main else R.string.phantom_playing_clone
                        Tag(stringResource(label), TagKind.NEUTRAL)
                    }
                }
            }
        }
        addContentView(badge, FrameLayout.LayoutParams(wrap, wrap, Gravity.BOTTOM or Gravity.LEFT))
        val switch = Button(this, null, android.R.attr.borderlessButtonStyle).apply {
            setText(if (player.primary) R.string.phantom_to_peer else R.string.phantom_to_primary)
            isAllCaps = false
            textSize = 12f
            setOnClickListener { player.notify(PhantomPlayer.SWITCH) }
        }
        addContentView(switch, FrameLayout.LayoutParams(wrap, wrap, Gravity.BOTTOM or Gravity.END))
    }

    override fun onDestroy() {
        super.onDestroy()
        CoroutineScope(Dispatchers.IO).launch { player.close() }
    }
}

class PhantomPrimaryActivity : PhantomPlayerActivity()
class PhantomPeerActivity : PhantomPlayerActivity()
