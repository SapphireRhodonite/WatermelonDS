package me.magnum.melonds.multiplayer

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.shrinkHorizontally
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.widthIn
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.foundation.border
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import me.magnum.melonds.R
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.watermelon

@Composable
fun MultiplayerNearbyWatch() {
    val context = LocalContext.current
    val owner = LocalLifecycleOwner.current
    DisposableEffect(owner) {
        var lease: Any? = null
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_START -> if (lease == null) lease = MultiplayerNearby.acquire(context, false)
                Lifecycle.Event.ON_STOP -> {
                    MultiplayerNearby.release(lease)
                    lease = null
                }
                else -> Unit
            }
        }
        owner.lifecycle.addObserver(observer)
        onDispose {
            owner.lifecycle.removeObserver(observer)
            MultiplayerNearby.release(lease)
        }
    }
}

@Composable
fun MultiplayerInvitePill(onJoin: (String) -> Unit) {
    val colors = watermelon
    val rooms by MultiplayerNearby.rooms.collectAsState()
    val room = rooms.minWithOrNull(compareBy({ it.app != MultiplayerNearby.OWN_APP }, { !it.direct }))
    var last by remember { mutableStateOf(room) }
    if (room != null && room != last) last = room
    AnimatedVisibility(room != null, enter = expandHorizontally() + fadeIn(), exit = shrinkHorizontally() + fadeOut()) {
        val value = room ?: last ?: return@AnimatedVisibility
        val (_, tint) = roomMeta(value)
        val label = stringResource(R.string.mp_invite_title, value.host.nickname)
        val interaction = remember { MutableInteractionSource() }
        val focused by interaction.collectIsFocusedAsState()
        val shape = RoundedCornerShape(16.dp)
        Row(
            Modifier
                .padding(start = 8.dp)
                .height(32.dp)
                .clip(shape)
                .background(if (focused) colors.red.copy(alpha = 0.16f) else colors.green.copy(alpha = 0.14f))
                .border(if (focused) 2.dp else 1.5.dp, if (focused) colors.red else colors.green.copy(alpha = 0.55f), shape)
                .clickable(interactionSource = interaction, indication = null) { onJoin(value.key) }
                .semantics { contentDescription = label }
                .padding(horizontal = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            MultiplayerAvatar(value.host, 22.dp)
            Pulse()
            Text(listOf(value.host.nickname, roomTitle(value)).filter { it.isNotEmpty() }.joinToString(" · "), color = colors.text,
                fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.widthIn(max = 220.dp))
            Mono(listOfNotNull("${value.players}/${value.maximum}", (rooms.size - 1).takeIf { it > 0 }?.let { "+$it" }).joinToString(" "),
                color = tint, size = 10.sp, spacing = 0.5.sp)
            Box(Modifier.height(24.dp).clip(RoundedCornerShape(12.dp)).background(colors.red).padding(horizontal = 10.dp),
                contentAlignment = Alignment.Center) {
                Text(stringResource(R.string.mp_join).uppercase(), color = Color.White, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold,
                    fontSize = 11.sp)
            }
        }
    }
}
