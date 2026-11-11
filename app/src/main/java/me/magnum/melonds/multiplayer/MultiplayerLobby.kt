package me.magnum.melonds.multiplayer

import android.view.KeyEvent
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.heightIn
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.graphics.PathEffect
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.widthIn
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.CircularProgressIndicator
import androidx.compose.material.Icon
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Check
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.launch
import androidx.compose.runtime.rememberCoroutineScope
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.romlist.composables.DsBoxArtAspectRatio
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.watermelon

private enum class LobbyDialog { NONE, CLOSE, DETAILS }

private data class LobbyToast(val title: String, val message: String, val profile: MultiplayerProfile, val good: Boolean)

@Composable
fun MultiplayerLobby(
    viewModel: MultiplayerViewModel,
    wide: Boolean,
    pad: SharedFlow<Int>,
    actions: MultiplayerActions,
    onEdit: () -> Unit,
    onProfile: () -> Unit,
    onClosed: () -> Unit,
) {
    val colors = watermelon
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()
    val room by viewModel.room.collectAsState()
    val status by viewModel.status.collectAsState()
    val profile by viewModel.profile.collectAsState()
    val bios by viewModel.bios.collectAsState()
    val roms by viewModel.roms.collectAsState()
    val busy by viewModel.busy.collectAsState()
    val failure by viewModel.failure.collectAsState()
    val info = room?.info
    val host = room?.role == MultiplayerRoom.Role.HOST || (room == null && status.optInt("mode") == 1)
    val seats = remember(status, room) { viewModel.players(status) }
    val connected = seats.count { it.status == 1 || it.status == 2 }
    val inGame = status.optBoolean("game")
    val interrupted = status.optBoolean("interrupted")
    val closed = room?.closed == true || (!host && seats.isNotEmpty() && seats.none { it.host && it.status != 4 })
    val hostName = info?.host?.nickname ?: seats.firstOrNull { it.host }?.profile?.nickname ?: profile.nickname
    val duplicate = seats.any { !it.local && it.status != 4 && it.profile.nickname.equals(profile.nickname, ignoreCase = true) }
    var dialog by remember { mutableStateOf(LobbyDialog.NONE) }
    var localRom by remember { mutableStateOf<Rom?>(null) }
    var toast by remember { mutableStateOf<LobbyToast?>(null) }
    var lastToast by remember { mutableStateOf<LobbyToast?>(null) }
    if (toast != null && toast != lastToast) lastToast = toast
    val known = remember { mutableStateMapOf<String, Int>() }
    val primary = remember { FocusRequester() }

    LaunchedEffect(info?.game, roms) {
        localRom = viewModel.find(info?.game)
        if (!host) viewModel.shareGame(localRom != null)
    }
    LaunchedEffect(seats) {
        val previous = known.toMap()
        seats.forEach { seat ->
            val before = previous[seat.key]
            val now = seat.status
            if (!seat.local && previous.isNotEmpty() && (now == 1 || now == 2) && before != 1 && before != 2) {
                toast = LobbyToast(context.getString(R.string.mp_joined, seat.profile.nickname), seat.profile.message, seat.profile, true)
            }
            if (!seat.local && now == 4 && before != null && before != 4) {
                toast = LobbyToast(context.getString(R.string.mp_left, seat.profile.nickname), "", seat.profile, false)
            }
        }
        previous.keys.filter { key -> seats.none { it.key == key } }.forEach { key ->
            if (previous[key] == 1 || previous[key] == 2) {
                val name = key.substringAfter(':')
                toast = LobbyToast(context.getString(R.string.mp_left, name), "", MultiplayerProfile(name, "", MultiplayerViewModel.nameColour(name), "", 1, 1), false)
            }
        }
        known.clear()
        seats.forEach { known[it.key] = it.status }
    }
    LaunchedEffect(toast) {
        if (toast != null) {
            delay(2600)
            toast = null
        }
    }
    LaunchedEffect(dialog) {
        if (dialog == LobbyDialog.NONE) {
            withFrameNanos { }
            runCatching { primary.requestFocus() }
        }
    }
    LaunchedEffect(room == null) { if (room == null && !MultiplayerSession.active) onClosed() }

    fun openHost() {
        val current = info ?: return
        scope.launch {
            when (current.play) {
                MultiplayerRoom.Play.CHAT -> actions.launchFirmware()
                else -> viewModel.find(current.game)?.let(actions.launchRom)
            }
        }
    }

    fun openGuest(current: MultiplayerRoom.Info?) {
        when (current?.play) {
            MultiplayerRoom.Play.DOWNLOAD, MultiplayerRoom.Play.CHAT -> if (bios) actions.launchFirmware() else actions.openBiosSettings()
            MultiplayerRoom.Play.ROM -> localRom?.let(actions.launchRom)
            null -> if (bios) actions.launchFirmware()
        }
    }

    val canStart = host && info != null && connected >= 2 && !inGame && (info.play == MultiplayerRoom.Play.CHAT || info.game != null)
    fun start() {
        if (!canStart) return
        viewModel.start()
        openHost()
    }

    LaunchedEffect(host) {
        if (!host) {
            lifecycleOwner.lifecycle.repeatOnLifecycle(Lifecycle.State.RESUMED) {
                viewModel.starts.collect { openGuest(it) }
            }
        }
    }
    LaunchedEffect(pad, canStart, info) {
        pad.collect {
            when (it) {
                KeyEvent.KEYCODE_BUTTON_X -> if (host) onEdit()
                KeyEvent.KEYCODE_BUTTON_Y -> if (host) dialog = LobbyDialog.DETAILS
                KeyEvent.KEYCODE_BUTTON_START -> if (host) start() else openGuest(info)
                KeyEvent.KEYCODE_BUTTON_SELECT -> onProfile()
            }
        }
    }
    BackHandler(enabled = dialog == LobbyDialog.NONE) { dialog = LobbyDialog.CLOSE }

    val joiningApp = viewModel.joining.collectAsState().value?.app.orEmpty()
    Box(Modifier.fillMaxSize()) {
        CompositionLocalProvider(LocalMultiplayerInteractive provides (dialog == LobbyDialog.NONE)) {
        Column(Modifier.fillMaxSize()) {
            MultiplayerHeader(stringResource(R.string.mp_room_of, hostName), onBack = { dialog = LobbyDialog.CLOSE }, tags = {
                Tag(stringResource(if (info?.direct == true || MultiplayerDirectNetwork.current != null) R.string.mp_tag_direct else R.string.mp_tag_wifi), TagKind.GOOD)
                if (!host) AppBadge(info?.app ?: joiningApp)
                if (inGame) Tag(stringResource(R.string.mp_tag_in_game), TagKind.HOST)
            }) {
                Mono(stringResource(R.string.mp_players_of, connected, info?.maximum ?: 16), color = colors.text2, size = 11.sp, spacing = 1.sp)
            }
            Column(Modifier.weight(1f).padding(horizontal = 16.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                val notice = when {
                    interrupted -> stringResource(R.string.multiplayer_interrupted_message)
                    closed -> stringResource(R.string.mp_host_closed)
                    duplicate -> stringResource(R.string.mp_duplicate, profile.nickname)
                    failure is MultiplayerViewModel.Failure.Message -> stringResource((failure as MultiplayerViewModel.Failure.Message).text)
                    else -> null
                }
                if (notice != null) {
                    Row(
                        Modifier.fillMaxWidth().height(48.dp).clip(RoundedCornerShape(10.dp)).background(colors.gold.copy(alpha = 0.12f))
                            .border(2.dp, colors.gold.copy(alpha = 0.55f), RoundedCornerShape(10.dp)).padding(start = 14.dp, end = 6.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        Mono(stringResource(if (duplicate && !interrupted && !closed) R.string.mp_notice_name else R.string.mp_notice), color = colors.goldText,
                            spacing = 1.sp)
                        Text(notice, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.SemiBold, fontSize = 14.sp, lineHeight = 17.sp,
                            maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                        when {
                            interrupted || closed -> NoticeButton(stringResource(R.string.mp_leave)) { viewModel.leave { if (it) onClosed() } }
                            duplicate -> NoticeButton(stringResource(R.string.mp_change_name), onProfile)
                            else -> Unit
                        }
                    }
                }
                SeatsPanel(seats, info?.maximum ?: 16, if (wide) 5 else 4, Modifier.fillMaxWidth().weight(1f))
                Row(Modifier.height(74.dp).fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    if (host) {
                        CodeCard(info?.code.orEmpty(), Modifier.width(250.dp).fillMaxHeight())
                        GameCard(
                            info = info,
                            rom = localRom,
                            subtitle = info?.let {
                                stringResource(R.string.mp_game_change, stringResource(when (it.play) {
                                    MultiplayerRoom.Play.ROM -> R.string.mp_play_rom
                                    MultiplayerRoom.Play.DOWNLOAD -> R.string.mp_play_download
                                    MultiplayerRoom.Play.CHAT -> R.string.mp_play_chat
                                }))
                            }.orEmpty(),
                            good = false,
                            onClick = onEdit,
                            modifier = Modifier.weight(1f).fillMaxHeight(),
                        )
                    } else {
                        val (subtitle, good) = when {
                            info == null -> stringResource(R.string.mp_waiting_host, hostName) to false
                            info.play == MultiplayerRoom.Play.CHAT -> stringResource(if (bios) R.string.mp_chat_guest else R.string.mp_bios_needed) to bios
                            info.play == MultiplayerRoom.Play.DOWNLOAD -> stringResource(if (bios) R.string.mp_download_guest else R.string.mp_bios_needed) to bios
                            info.game == null -> stringResource(R.string.mp_waiting_host, hostName) to false
                            localRom != null -> stringResource(R.string.mp_have_game, hostName) to true
                            else -> stringResource(R.string.mp_missing_game) to false
                        }
                        val openable = when (info?.play) {
                            MultiplayerRoom.Play.ROM -> localRom != null
                            null -> bios
                            else -> bios
                        }
                        GameCard(
                            info = info,
                            rom = localRom,
                            subtitle = subtitle,
                            good = good,
                            onClick = { if (openable) openGuest(info) else if (!bios) actions.openBiosSettings() },
                            modifier = Modifier.weight(1f).fillMaxHeight(),
                            action = if (openable) stringResource(R.string.mp_open_now) else null,
                            focus = primary,
                        )
                    }
                }
            }
            if (host) {
                MultiplayerFooter {
                    Hint("X", stringResource(R.string.mp_hint_game))
                    Hint("Y", stringResource(R.string.mp_hint_details))
                    Hint("B", stringResource(R.string.mp_hint_close))
                    Grow()
                    PillButton(
                        text = if (canStart) stringResource(R.string.mp_start, connected) else stringResource(R.string.mp_waiting),
                        onClick = { start() },
                        enabled = canStart,
                        key = "START",
                        modifier = Modifier.focusRequester(primary),
                    )
                }
            } else {
                MultiplayerFooter {
                    Hint("A", stringResource(R.string.mp_hint_open))
                    Hint("B", stringResource(R.string.mp_hint_leave))
                    Grow()
                    Mono(stringResource(R.string.mp_pause_warning).uppercase(), size = 11.sp, spacing = 0.5.sp)
                }
            }
        }
        }
        AnimatedVisibility(toast != null, enter = slideInVertically { -it } + fadeIn(), exit = slideOutVertically { -it } + fadeOut(),
            modifier = Modifier.align(Alignment.TopCenter).padding(top = 8.dp)) {
            val value = toast ?: lastToast ?: return@AnimatedVisibility
            Row(
                Modifier.widthIn(max = 560.dp).shadow(14.dp, RoundedCornerShape(30.dp)).clip(RoundedCornerShape(30.dp)).background(colors.surface2)
                    .border(2.dp, if (value.good) colors.green else colors.red, RoundedCornerShape(30.dp))
                    .padding(start = 6.dp, end = 18.dp, top = 6.dp, bottom = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                MultiplayerAvatar(value.profile, 40.dp)
                Column(verticalArrangement = Arrangement.spacedBy(1.dp)) {
                    Text(value.title, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 16.sp, maxLines = 1,
                        overflow = TextOverflow.Ellipsis)
                    if (value.message.isNotEmpty()) Mono("«${value.message}»", color = colors.text2, size = 11.sp, spacing = 0.sp)
                }
            }
        }
        when (dialog) {
            LobbyDialog.CLOSE -> ConfirmOverlay(
                title = stringResource(if (host) R.string.mp_close_title else R.string.mp_leave_title),
                body = stringResource(if (host) R.string.mp_close_body else R.string.mp_leave_body),
                actions = listOf(
                    stringResource(if (host) R.string.mp_close_room else R.string.mp_leave) to {
                        dialog = LobbyDialog.NONE
                        viewModel.leave { if (it) onClosed() }
                    },
                    stringResource(R.string.mp_keep_open) to actions.exit,
                    stringResource(R.string.mp_stay) to { dialog = LobbyDialog.NONE },
                ),
                onDismiss = { dialog = LobbyDialog.NONE },
            )
            LobbyDialog.DETAILS -> DetailsOverlay(info) { dialog = LobbyDialog.NONE }
            LobbyDialog.NONE -> Unit
        }
        if (busy) Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.3f)), contentAlignment = Alignment.Center) {
            CircularProgressIndicator(color = colors.red)
        }
    }
}

@Composable
private fun NoticeButton(text: String, onClick: () -> Unit) {
    val colors = watermelon
    Pressable(onClick = onClick, modifier = Modifier.height(36.dp), shape = RoundedCornerShape(8.dp), background = colors.text.copy(alpha = 0.08f), border = null) {
        Text(text, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.SemiBold, fontSize = 14.sp,
            modifier = Modifier.align(Alignment.Center).padding(horizontal = 12.dp))
    }
}

@Composable
private fun SeatsPanel(
    seats: List<MultiplayerViewModel.Seat>,
    maximum: Int,
    columns: Int,
    modifier: Modifier,
) {
    val colors = watermelon
    BoxWithConstraints(modifier.panel(colors)) {
        val radar = rememberInfiniteTransition(label = "radar")
        val wave by radar.animateFloat(0f, 1f, infiniteRepeatable(tween(3200, easing = LinearEasing), RepeatMode.Restart), label = "wave")
        Canvas(Modifier.matchParentSize()) {
            val unit = 1.dp.toPx()
            listOf(wave, (wave + 0.5f) % 1f).forEach { phase ->
                drawCircle(colors.green.copy(alpha = (1f - phase) * 0.5f), (30 + phase * 270) * unit, style = Stroke(2 * unit))
            }
        }
        val width = (maxWidth - 16.dp) / columns
        val ring = if (maxHeight < 170.dp) 72.dp else 84.dp
        val free = seats.count { it.status != 4 } < maximum
        LazyRow(Modifier.fillMaxSize().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            items(seats, key = { it.key }) { seat -> SeatView(seat, ring, Modifier.width(width)) }
            if (free) item { FreeSeat(ring, Modifier.width(width)) }
        }
    }
}

@Composable
private fun SeatView(seat: MultiplayerViewModel.Seat, ring: androidx.compose.ui.unit.Dp, modifier: Modifier) {
    val colors = watermelon
    val connecting = seat.status == 3
    val gone = seat.status == 4
    val transition = rememberInfiniteTransition(label = "seat")
    val breathe by transition.animateFloat(0.45f, 1f, infiniteRepeatable(tween(1200), RepeatMode.Reverse), label = "breathe")
    val pop = remember(seat.key) { Animatable(0.2f) }
    val burst = remember(seat.key) { Animatable(1f) }
    LaunchedEffect(seat.key, seat.status == 1 || seat.status == 2) {
        if (pop.value >= 1f) pop.snapTo(0.75f)
        launch { pop.animateTo(1f, spring(dampingRatio = 0.35f, stiffness = Spring.StiffnessLow)) }
        if (seat.status == 1 || seat.status == 2) {
            burst.snapTo(0f)
            burst.animateTo(1f, tween(900))
        }
    }
    val tone = when {
        gone || connecting -> colors.text3
        seat.host -> colors.red
        else -> colors.green
    }
    Column(modifier.graphicsLayer {
        scaleX = pop.value
        scaleY = pop.value
    }, horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Box(
            Modifier.size(ring).drawBehind {
                if (burst.value < 1f) drawCircle(colors.green.copy(alpha = 1f - burst.value), size.minDimension / 2 * (1f + burst.value * 0.6f),
                    style = Stroke(4.dp.toPx()))
            }.alpha(if (gone) 0.35f else if (connecting) breathe else 1f).clip(CircleShape).border(4.dp, tone, CircleShape).padding(8.dp),
            contentAlignment = Alignment.Center,
        ) {
            MultiplayerAvatar(seat.profile, ring - 16.dp)
        }
        Text(seat.profile.nickname, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 15.sp,
            maxLines = 1, overflow = TextOverflow.Ellipsis)
        when {
            gone -> Tag(stringResource(R.string.mp_tag_left), TagKind.BAD)
            connecting -> Tag(stringResource(R.string.mp_tag_connecting), TagKind.NEUTRAL)
            seat.host -> Tag(stringResource(R.string.mp_tag_host), TagKind.HOST)
            seat.local -> Tag(stringResource(R.string.mp_tag_you), TagKind.GOOD)
            else -> Tag(stringResource(R.string.mp_tag_ping, seat.ping), TagKind.GOOD)
        }
    }
}

@Composable
private fun FreeSeat(ring: androidx.compose.ui.unit.Dp, modifier: Modifier) {
    val colors = watermelon
    val transition = rememberInfiniteTransition(label = "free")
    val breathe by transition.animateFloat(0.45f, 1f, infiniteRepeatable(tween(2400), RepeatMode.Reverse), label = "breathe")
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Box(
            Modifier.size(ring).alpha(breathe).drawBehind {
                val width = 3.dp.toPx()
                drawCircle(colors.text3, size.minDimension / 2 - width / 2,
                    style = Stroke(width, pathEffect = PathEffect.dashPathEffect(floatArrayOf(9.dp.toPx(), 6.dp.toPx()))))
            },
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Filled.Add, null, tint = colors.text3, modifier = Modifier.size(26.dp))
        }
        Text(stringResource(R.string.mp_free), color = colors.text3, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 15.sp)
        Mono(stringResource(R.string.mp_tag_waiting), size = 10.sp, spacing = 1.sp, modifier = Modifier.padding(vertical = 4.dp))
    }
}

@Composable
private fun CodeCard(code: String, modifier: Modifier) {
    val colors = watermelon
    Row(
        modifier.clip(RoundedCornerShape(10.dp)).background(tileColor(colors, false, false)).padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Mono(stringResource(R.string.mp_code_label), size = 10.sp, spacing = 1.sp)
            Mono(stringResource(R.string.mp_code_for).uppercase(), size = 10.sp, spacing = 1.sp)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(5.dp)) {
            code.padEnd(4).take(4).forEach { char ->
                Box(Modifier.size(width = 36.dp, height = 48.dp).clip(RoundedCornerShape(8.dp)).background(colors.surface2)
                    .border(1.dp, colors.text.copy(alpha = 0.13f), RoundedCornerShape(8.dp)), contentAlignment = Alignment.Center) {
                    Text(char.toString(), color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 26.sp)
                }
            }
        }
    }
}

@Composable
private fun GameCard(
    info: MultiplayerRoom.Info?,
    rom: Rom?,
    subtitle: String,
    good: Boolean,
    onClick: () -> Unit,
    modifier: Modifier,
    action: String? = null,
    focus: FocusRequester? = null,
) {
    val colors = watermelon
    val chat = info?.play == MultiplayerRoom.Play.CHAT
    val title = when {
        chat -> stringResource(R.string.mp_play_chat)
        info?.game != null -> info.game.title
        else -> stringResource(R.string.mp_game_pick)
    }
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val shape = RoundedCornerShape(10.dp)
    val interactive = LocalMultiplayerInteractive.current
    Row(
        modifier.let { if (focus != null) it.focusRequester(focus) else it }.clip(shape).background(tileColor(colors, focused, action != null))
            .border(2.dp, if (focused || action != null) colors.red else Color.Transparent, shape)
            .focusProperties { canFocus = interactive }
            .clickable(interactionSource = interaction, indication = null, enabled = interactive, onClick = onClick)
            .padding(start = 8.dp, end = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        GameArt(if (chat) null else rom, title, Modifier.size(64.dp, 57.dp).clip(RoundedCornerShape(6.dp)), initials = 18.sp)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(title, color = colors.text, fontFamily = SpaceGrotesk, fontSize = 15.sp, fontWeight = FontWeight.Bold, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
            Mono((if (good) "✓ " else "") + subtitle.uppercase(), color = if (good) colors.goodText else colors.text2, spacing = 0.5.sp)
        }
        if (action != null) {
            Row(Modifier.height(40.dp).clip(RoundedCornerShape(20.dp)).background(colors.red).padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Text(action, color = Color.White, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 15.sp, maxLines = 1)
            }
        }
    }
}

@Composable
private fun DetailsOverlay(info: MultiplayerRoom.Info?, onDismiss: () -> Unit) {
    val colors = watermelon
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    MultiplayerSheet(onDismiss, Modifier.widthIn(max = 540.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.padding(bottom = 2.dp)) {
            Text(stringResource(R.string.mp_details), color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 21.sp,
                letterSpacing = (-0.3).sp, modifier = Modifier.weight(1f))
            Mono(stringResource(R.string.mp_net_direct).uppercase())
        }
        if (info?.code?.isNotEmpty() == true) CredentialRow(stringResource(R.string.mp_code_label), info.code, code = true)
        if (info?.network?.isNotEmpty() == true) CredentialRow(stringResource(R.string.mp_network), info.network)
        MenuTile(onDismiss, Modifier.fillMaxWidth().height(48.dp).focusRequester(focus)) {
            TileTitle(stringResource(R.string.mp_back_room), Modifier.weight(1f))
        }
    }
}

@Composable
fun CredentialRow(label: String, value: String, code: Boolean = false, small: Boolean = false) {
    val colors = watermelon
    Row(
        Modifier.fillMaxWidth().heightIn(min = 52.dp).clip(RoundedCornerShape(10.dp)).background(tileColor(colors, false, false))
            .padding(horizontal = 14.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Mono(label.uppercase(), Modifier.width(92.dp))
        if (code) Text(value, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 24.sp, letterSpacing = 4.sp)
        else Mono(value, color = colors.text, size = if (small) 15.sp else 18.sp, spacing = if (small) 0.sp else 1.sp,
            weight = if (small) FontWeight.Normal else FontWeight.Bold, lines = 2)
    }
}

@Composable
fun MultiplayerConnecting(viewModel: MultiplayerViewModel, onCancel: () -> Unit) {
    val colors = watermelon
    val room by viewModel.joining.collectAsState()
    val step by viewModel.joinStep.collectAsState()
    val profile by viewModel.profile.collectAsState()
    val library by viewModel.roms.collectAsState()
    val target = room ?: return
    BackHandler { viewModel.cancelJoin(onCancel) }
    val transition = rememberInfiniteTransition(label = "dots")
    val phase by transition.animateFloat(0f, 4f, infiniteRepeatable(tween(1200, easing = LinearEasing), RepeatMode.Restart), label = "phase")
    val steps = buildList {
        add(Triple(stringResource(R.string.mp_found), roomMeta(target).first, 0))
        add(Triple(stringResource(R.string.mp_joining_network, target.host.nickname), stringResource(R.string.mp_joining_network_sub), 1))
        add(Triple(stringResource(R.string.mp_entering_room), target.target.network, 2))
    }
    val position = steps.indexOfFirst { it.third >= step }.let { if (it < 0) steps.size else it }
    Column(Modifier.fillMaxSize()) {
        MultiplayerHeader(stringResource(R.string.mp_entering, stringResource(R.string.mp_room_of, target.host.nickname)), onBack = null, tags = {
            Tag(stringResource(R.string.mp_tag_direct), TagKind.GOOD)
            AppBadge(target.app)
        }) {
            Mono(stringResource(R.string.mp_step_of, (position + 1).coerceAtMost(steps.size), steps.size), size = 11.sp, spacing = 1.sp)
        }
        Row(Modifier.weight(1f).fillMaxWidth().padding(16.dp), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Column(Modifier.width(236.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                GameArt(matchRom(library.asSequence(), target.game), roomTitle(target),
                    Modifier.fillMaxWidth().aspectRatio(DsBoxArtAspectRatio).clip(RoundedCornerShape(12.dp))
                        .border(1.dp, colors.text.copy(alpha = 0.13f), RoundedCornerShape(12.dp)), initials = 54.sp)
                Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        MultiplayerAvatar(profile, 48.dp)
                        Mono(stringResource(R.string.mp_you).uppercase(), color = colors.text2, size = 10.sp, spacing = 1.sp)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        (0 until 4).forEach { index ->
                            val distance = ((phase - index).let { if (it < 0) it + 4 else it }).coerceIn(0f, 2f)
                            Box(Modifier.size(8.dp).alpha(1f - distance * 0.4f).clip(CircleShape).background(colors.green))
                        }
                    }
                    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        MultiplayerAvatar(target.host, 48.dp)
                        Mono(stringResource(R.string.mp_tag_host), color = colors.text2, size = 10.sp, spacing = 1.sp)
                    }
                }
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                steps.forEachIndexed { index, (title, meta, _) ->
                    ProgressStep(title, meta.uppercase(), done = index < position, active = index == position)
                }
            }
        }
        MultiplayerFooter {
            Hint("B", stringResource(R.string.mp_cancel))
        }
    }
}

@Composable
private fun ProgressStep(title: String, meta: String, done: Boolean, active: Boolean) {
    val colors = watermelon
    val shape = RoundedCornerShape(10.dp)
    Row(
        Modifier.fillMaxWidth().heightIn(min = 64.dp).alpha(if (done || active) 1f else 0.55f).clip(shape).background(tileColor(colors, false, active))
            .border(2.dp, if (active) colors.red else Color.Transparent, shape).padding(start = 11.dp, end = 14.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.width(3.dp).height(20.dp).clip(RoundedCornerShape(2.dp)).background(if (active) colors.red else Color.Transparent))
        Spacer(Modifier.width(12.dp))
        Box(Modifier.size(26.dp), contentAlignment = Alignment.Center) {
            when {
                done -> Box(Modifier.size(26.dp).clip(CircleShape).background(colors.green), contentAlignment = Alignment.Center) {
                    Icon(Icons.Filled.Check, null, tint = Color(0xFF141317), modifier = Modifier.size(16.dp))
                }
                active -> CircularProgressIndicator(Modifier.size(24.dp), color = colors.red, strokeWidth = 3.dp, backgroundColor = colors.red.copy(alpha = 0.25f))
                else -> Box(Modifier.size(26.dp).border(2.dp, colors.text.copy(alpha = 0.22f), CircleShape))
            }
        }
        Spacer(Modifier.width(12.dp))
        Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text(title, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.SemiBold, fontSize = 16.sp, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
            if (meta.isNotEmpty()) Mono(meta, spacing = 0.5.sp, lines = 2)
        }
    }
}

@Composable
fun MultiplayerDirectFail(
    viewModel: MultiplayerViewModel,
    pad: SharedFlow<Int>,
    onRetry: (MultiplayerViewModel.Nearby) -> Unit,
    onOpenWifi: () -> Unit,
    onBack: () -> Unit,
) {
    val colors = watermelon
    val failure by viewModel.failure.collectAsState()
    val room = (failure as? MultiplayerViewModel.Failure.Direct)?.room ?: viewModel.joining.collectAsState().value
    val focus = remember { FocusRequester() }
    BackHandler { onBack() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    LaunchedEffect(pad) { pad.collect { if (it == KeyEvent.KEYCODE_BUTTON_Y) onOpenWifi() } }
    if (room == null) {
        LaunchedEffect(Unit) { onBack() }
        return
    }
    Column(Modifier.fillMaxSize()) {
        MultiplayerHeader(stringResource(R.string.mp_fail_title), onBack = onBack) {
            Tag(stringResource(R.string.mp_tag_not_connected), TagKind.HOST)
        }
        Row(Modifier.weight(1f).padding(horizontal = 16.dp, vertical = 14.dp), horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            Column(Modifier.width(262.dp).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.padding(bottom = 2.dp)) {
                    MultiplayerAvatar(room.host, 40.dp)
                    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(stringResource(R.string.mp_room_of, room.host.nickname), color = colors.text, fontFamily = SpaceGrotesk,
                            fontWeight = FontWeight.Bold, fontSize = 16.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Mono(listOfNotNull(room.code.takeIf { it.isNotEmpty() }?.let { "${stringResource(R.string.mp_code_label)} $it" },
                            stringResource(R.string.mp_tag_direct)).joinToString(" · "), spacing = 0.5.sp)
                    }
                }
                MenuTile({ onRetry(room) }, Modifier.fillMaxWidth().height(50.dp).focusRequester(focus)) {
                    TileTitle(stringResource(R.string.mp_retry), Modifier.weight(1f), size = 15.sp)
                }
                MenuTile(onOpenWifi, Modifier.fillMaxWidth().height(50.dp)) {
                    TileTitle(stringResource(R.string.mp_open_wifi), Modifier.weight(1f), size = 15.sp)
                }
                Column(Modifier.padding(top = 4.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf(
                        stringResource(R.string.mp_fail_closer, room.host.nickname),
                        stringResource(R.string.mp_fail_wifi),
                    ).forEach { line -> Mono("· " + line.uppercase(), color = colors.text2, spacing = 0.5.sp, lines = 2) }
                }
            }
        }
        MultiplayerFooter {
            Hint("A", stringResource(R.string.mp_hint_choose))
            Hint("Y", stringResource(R.string.mp_wifi_short))
            Grow()
            Hint("B", stringResource(R.string.multiplayer_back))
        }
    }
}
