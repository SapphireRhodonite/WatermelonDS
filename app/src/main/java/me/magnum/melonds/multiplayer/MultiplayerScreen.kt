package me.magnum.melonds.multiplayer

import android.view.KeyEvent
import android.os.Build
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.displayCutoutPadding
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.Icon
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Keyboard
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.SharedFlow
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.common.WatermelonMark
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.watermelon

private const val PENDING_JOIN_MS = 15_000L

enum class MultiplayerPage { PERMISSION, HUB, CREATE, EDIT, LOBBY, JOIN, CONNECTING, DIRECT_FAIL, PROFILE }

class MultiplayerActions(
    val exit: () -> Unit,
    val launchRom: (Rom) -> Unit,
    val launchFirmware: () -> Unit,
    val openBiosSettings: () -> Unit,
    val openWifiSettings: () -> Unit,
    val requestPermission: (() -> Unit) -> Unit,
    val permissionPrompted: () -> Boolean,
    val markPermissionPrompted: () -> Unit,
)

@Composable
fun MultiplayerScreen(viewModel: MultiplayerViewModel, pad: SharedFlow<Int>, actions: MultiplayerActions, pendingJoin: String? = null) {
    val lifecycleOwner = LocalLifecycleOwner.current
    val room by viewModel.room.collectAsState()
    val status by viewModel.status.collectAsState()
    val initial = remember {
        val mode = status.optInt("mode")
        when {
            room != null || MultiplayerSession.active || mode == 1 || mode == 2 -> MultiplayerPage.LOBBY
            pendingJoin == null && viewModel.directSupported && !viewModel.directPermitted() && !actions.permissionPrompted() -> MultiplayerPage.PERMISSION
            else -> MultiplayerPage.HUB
        }
    }
    var page by rememberSaveable { mutableStateOf(initial) }
    var previous by rememberSaveable { mutableStateOf(MultiplayerPage.HUB) }
    fun go(next: MultiplayerPage) {
        previous = page
        page = next
    }

    fun join(nearby: MultiplayerViewModel.Nearby, fallback: MultiplayerPage) {
        go(MultiplayerPage.CONNECTING)
        viewModel.join(nearby) { ok ->
            page = when {
                ok -> MultiplayerPage.LOBBY
                viewModel.failure.value is MultiplayerViewModel.Failure.Direct -> MultiplayerPage.DIRECT_FAIL
                else -> fallback
            }
        }
    }

    var waiting by rememberSaveable { mutableStateOf(pendingJoin) }
    val nearby by viewModel.nearby.collectAsState()
    LaunchedEffect(nearby, waiting, page) {
        val key = waiting ?: return@LaunchedEffect
        if (page != MultiplayerPage.HUB) return@LaunchedEffect
        nearby.firstOrNull { it.key == key }?.let {
            waiting = null
            join(it, MultiplayerPage.HUB)
        }
    }
    LaunchedEffect(waiting) {
        if (waiting != null) {
            delay(PENDING_JOIN_MS)
            waiting = null
        }
    }
    LaunchedEffect(Unit) {
        lifecycleOwner.lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            viewModel.refresh()
            while (true) {
                viewModel.poll()
                delay(1000)
            }
        }
    }
    val searching = page == MultiplayerPage.HUB || page == MultiplayerPage.JOIN
    DisposableEffect(searching, lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_START && searching && room == null) viewModel.search()
            if (event == Lifecycle.Event.ON_STOP) viewModel.stopSearch()
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        if (searching && room == null && lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) viewModel.search()
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            if (searching) viewModel.stopSearch()
        }
    }

    LaunchedEffect(page) { viewModel.page.value = page }
    val covers by viewModel.covers.collectAsState()
    BoxWithConstraints(
        Modifier.fillMaxSize().background(watermelon.bg)
            .statusBarsPadding().navigationBarsPadding().displayCutoutPadding().imePadding(),
    ) {
        val wide = maxWidth >= 800.dp && maxWidth / maxHeight > 1.5f
        CompositionLocalProvider(
            LocalGameCovers provides GameCovers(covers, viewModel::requestCover),
            LocalMultiplayerCompact provides (maxWidth < 600.dp),
        ) {
        when (page) {
            MultiplayerPage.PERMISSION -> MultiplayerPermission(
                onContinue = {
                    actions.markPermissionPrompted()
                    actions.requestPermission { go(MultiplayerPage.HUB) }
                },
                onSkip = {
                    actions.markPermissionPrompted()
                    go(MultiplayerPage.HUB)
                },
                onBack = actions.exit,
            )
            MultiplayerPage.HUB -> MultiplayerHub(
                viewModel = viewModel,
                pad = pad,
                onCreate = { go(MultiplayerPage.CREATE) },
                onCode = { go(MultiplayerPage.JOIN) },
                onProfile = { go(MultiplayerPage.PROFILE) },
                onJoin = { join(it, MultiplayerPage.HUB) },
                onBack = actions.exit,
            )
            MultiplayerPage.CREATE, MultiplayerPage.EDIT -> MultiplayerCreate(
                viewModel = viewModel,
                wide = wide,
                pad = pad,
                editing = page == MultiplayerPage.EDIT,
                onBios = actions.openBiosSettings,
                requestPermission = actions.requestPermission,
                onDone = { go(MultiplayerPage.LOBBY) },
                onBack = { page = if (page == MultiplayerPage.EDIT) MultiplayerPage.LOBBY else MultiplayerPage.HUB },
            )
            MultiplayerPage.LOBBY -> MultiplayerLobby(
                viewModel = viewModel,
                wide = wide,
                pad = pad,
                actions = actions,
                onEdit = { go(MultiplayerPage.EDIT) },
                onProfile = { go(MultiplayerPage.PROFILE) },
                onClosed = { page = MultiplayerPage.HUB },
            )
            MultiplayerPage.JOIN -> MultiplayerJoin(
                viewModel = viewModel,
                wide = wide,
                pad = pad,
                onJoin = { join(it, MultiplayerPage.JOIN) },
                onBack = { page = MultiplayerPage.HUB },
            )
            MultiplayerPage.CONNECTING -> MultiplayerConnecting(viewModel = viewModel, onCancel = { page = MultiplayerPage.HUB })
            MultiplayerPage.DIRECT_FAIL -> MultiplayerDirectFail(
                viewModel = viewModel,
                pad = pad,
                onRetry = {
                    viewModel.dismissFailure()
                    join(it, MultiplayerPage.HUB)
                },
                onOpenWifi = actions.openWifiSettings,
                onBack = {
                    viewModel.dismissFailure()
                    page = MultiplayerPage.HUB
                },
            )
            MultiplayerPage.PROFILE -> MultiplayerProfileScreen(
                viewModel = viewModel,
                pad = pad,
                onBack = { page = previous.takeIf { it != MultiplayerPage.PROFILE } ?: MultiplayerPage.HUB },
            )
        }
    }
    }
}

@Composable
fun MultiplayerPermission(onContinue: () -> Unit, onSkip: () -> Unit, onBack: () -> Unit) {
    val colors = watermelon
    val compact = LocalMultiplayerCompact.current
    val focus = remember { FocusRequester() }
    BackHandler { onSkip() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    val radar: @Composable (Modifier) -> Unit = { modifier ->
        Box(modifier.panel(colors, 12.dp), contentAlignment = Alignment.Center) {
            Waves(Modifier.size(if (compact) 104.dp else 220.dp))
            Mono(stringResource(R.string.mp_perm_radar), Modifier.align(Alignment.BottomCenter)
                .padding(bottom = if (compact) 6.dp else 14.dp), size = 10.sp)
        }
    }
    val guidance: @Composable () -> Unit = {
        val ask = stringResource(if (Build.VERSION.SDK_INT >= 33) R.string.mp_perm_nearby else R.string.mp_perm_location)
        Text(
            buildAnnotatedString {
                val text = stringResource(R.string.mp_perm_body, ask)
                val start = text.indexOf(ask)
                append(text)
                if (start >= 0) addStyle(SpanStyle(color = colors.goodText), start, start + ask.length)
            },
            color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.SemiBold, fontSize = 16.sp, lineHeight = 21.sp,
            modifier = Modifier.padding(bottom = 2.dp),
        )
        PermissionPoint(true, stringResource(R.string.mp_perm_find), stringResource(R.string.mp_perm_find_sub))
        PermissionPoint(true, stringResource(R.string.mp_perm_direct), stringResource(R.string.mp_perm_direct_sub))
        PermissionPoint(false, stringResource(R.string.mp_perm_private), stringResource(R.string.mp_perm_private_sub))
    }
    val choices: @Composable (Modifier) -> Unit = { modifier ->
        Row(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            MenuTile(onContinue, Modifier.weight(1.5f).heightIn(min = 50.dp).focusRequester(focus)) {
                TileTitle(stringResource(R.string.mp_continue), size = 15.sp)
            }
            MenuTile(onSkip, Modifier.weight(1f).heightIn(min = 50.dp)) {
                TileTitle(stringResource(R.string.mp_not_now), size = 15.sp)
            }
        }
    }
    Column(Modifier.fillMaxSize()) {
        MultiplayerHeader(
            stringResource(R.string.mp_perm_title), onBack = onBack,
            tags = { if (compact) Mono(stringResource(R.string.mp_first_time)) },
        ) {
            if (!compact) Mono(stringResource(R.string.mp_first_time))
        }
        if (compact) {
            Column(Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)) {
                radar(Modifier.fillMaxWidth().height(110.dp))
                guidance()
            }
            choices(Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
        } else {
            Row(Modifier.weight(1f).fillMaxWidth().padding(16.dp), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                radar(Modifier.width(236.dp).fillMaxHeight())
                Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        guidance()
                    }
                    choices(Modifier)
                }
            }
        }
        MultiplayerFooter(actions = {
            Mono(stringResource(R.string.mp_perm_without).uppercase(), size = 10.sp, spacing = 0.5.sp,
                lines = if (compact) Int.MAX_VALUE else 1)
        }) {
            Hint("A", stringResource(R.string.mp_hint_choose))
            Hint("B", stringResource(R.string.multiplayer_back))
        }
    }
}

@Composable
private fun PermissionPoint(yes: Boolean, title: String, detail: String) {
    val colors = watermelon
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(tileColor(colors, false, false)).padding(horizontal = 12.dp, vertical = 9.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Mono(if (yes) "✓" else "✕", Modifier.width(18.dp).padding(top = 2.dp), color = if (yes) colors.goodText else colors.text2, size = 11.sp)
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            TileTitle(title, size = 14.sp)
            Mono(detail.uppercase(), size = 10.sp, spacing = 0.5.sp,
                lines = if (LocalMultiplayerCompact.current) Int.MAX_VALUE else 1)
        }
    }
}

@Composable
private fun Waves(modifier: Modifier) {
    val transition = rememberInfiniteTransition(label = "waves")
    val first by transition.animateFloat(0f, 1f, infiniteRepeatable(tween(2400), RepeatMode.Restart), label = "first")
    val samples = listOf(
        MultiplayerProfile("Mara", "", 8, "diamond", 1, 1),
        MultiplayerProfile("Leo", "", 11, "bolt", 1, 1),
        MultiplayerProfile("Sofi", "", 5, "flower", 1, 1),
        MultiplayerProfile("", "", 2, "star", 1, 1),
    )
    Box(modifier, contentAlignment = Alignment.Center) {
        listOf(first, (first + 0.5f) % 1f).forEach { phase ->
            Box(Modifier.size(220.dp).scale(0.35f + phase * 0.65f).alpha(1f - phase).clip(CircleShape)
                .border(2.dp, watermelon.green.copy(alpha = 0.55f), CircleShape))
        }
        Box(Modifier.size(150.dp).border(1.dp, watermelon.text.copy(alpha = 0.12f), CircleShape))
        MultiplayerAvatar(samples[3], 64.dp)
        MultiplayerAvatar(samples[0], 36.dp, Modifier.align(Alignment.TopStart).padding(start = 22.dp, top = 30.dp))
        MultiplayerAvatar(samples[1], 32.dp, Modifier.align(Alignment.CenterEnd).padding(end = 14.dp, bottom = 40.dp))
        MultiplayerAvatar(samples[2], 30.dp, Modifier.align(Alignment.BottomStart).padding(start = 40.dp, bottom = 44.dp))
    }
}

@Composable
fun multiplayerTitle(): AnnotatedString {
    val title = stringResource(R.string.multiplayer_title)
    val green = watermelon.green
    return buildAnnotatedString {
        if (title.startsWith("Multi") && title.length > 5) {
            append("Multi")
            withStyle(SpanStyle(color = green)) { append(title.substring(5)) }
        } else append(title)
    }
}

@Composable
fun MultiplayerHub(
    viewModel: MultiplayerViewModel,
    pad: SharedFlow<Int>,
    onCreate: () -> Unit,
    onCode: () -> Unit,
    onProfile: () -> Unit,
    onJoin: (MultiplayerViewModel.Nearby) -> Unit,
    onBack: () -> Unit,
) {
    val colors = watermelon
    val compact = LocalMultiplayerCompact.current
    val profile by viewModel.profile.collectAsState()
    val nearby by viewModel.nearby.collectAsState()
    val failure by viewModel.failure.collectAsState()
    val library by viewModel.roms.collectAsState()
    val focus = remember { FocusRequester() }
    BackHandler { onBack() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    LaunchedEffect(pad) {
        pad.collect {
            when (it) {
                KeyEvent.KEYCODE_BUTTON_X -> onCreate()
                KeyEvent.KEYCODE_BUTTON_Y -> onCode()
                KeyEvent.KEYCODE_BUTTON_SELECT -> onProfile()
            }
        }
    }
    Column(Modifier.fillMaxSize()) {
        MultiplayerHeader(multiplayerTitle(), onBack = null, leading = { WatermelonMark(width = 34.dp) }) {
            Pressable(onClick = onProfile, modifier = if (compact) Modifier.size(48.dp) else Modifier,
                shape = RoundedCornerShape(21.dp), background = colors.surface2, border = null,
                label = stringResource(R.string.mp_profile_cd, profile.nickname)) {
                if (compact) {
                    MultiplayerAvatar(profile, 32.dp, Modifier.align(Alignment.Center))
                } else {
                    Row(Modifier.height(42.dp).padding(start = 5.dp, end = 14.dp), verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        MultiplayerAvatar(profile, 32.dp)
                        Text(profile.nickname, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 15.sp)
                    }
                }
            }
        }
        FlowRow(
            Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 14.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            maxItemsInEachRow = if (compact) 1 else 2,
        ) {
            MenuTile(onCreate, Modifier.weight(1f).heightIn(min = 52.dp).focusRequester(focus).onFocusChanged { if (it.isFocused) viewModel.focus.value = null }) {
                Icon(Icons.Filled.Add, null, tint = colors.text, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(10.dp))
                TileTitle(stringResource(R.string.mp_create_title), Modifier.weight(1f))
            }
            MenuTile(onCode, Modifier.weight(1f).heightIn(min = 52.dp).onFocusChanged { if (it.isFocused) viewModel.focus.value = null }) {
                Icon(Icons.Filled.Keyboard, null, tint = colors.text, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(10.dp))
                TileTitle(stringResource(R.string.mp_enter_code), Modifier.weight(1f))
            }
        }
        Column(Modifier.weight(1f).then(if (compact) Modifier.verticalScroll(rememberScrollState()) else Modifier)
            .padding(start = 16.dp, top = 18.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            FlowRow(Modifier.fillMaxWidth().padding(end = 16.dp), itemVerticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(stringResource(R.string.mp_nearby), color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.SemiBold, fontSize = 19.sp)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Pulse()
                    Mono(stringResource(R.string.mp_searching).uppercase())
                }
                if (!compact) Grow()
                Mono(pluralStringResource(R.plurals.mp_rooms, nearby.size, nearby.size), color = colors.text2, size = 11.sp, spacing = 0.sp)
            }
            failure?.let { value ->
                if (value is MultiplayerViewModel.Failure.Message) Text(stringResource(value.text), color = colors.goldText, fontSize = 14.sp,
                    lineHeight = 19.sp, modifier = Modifier.padding(end = 16.dp))
            }
            if (nearby.isEmpty()) {
                Text(stringResource(R.string.mp_nearby_empty), color = colors.text2, fontSize = 15.sp, lineHeight = 21.sp,
                    modifier = Modifier.widthIn(max = 520.dp).padding(end = 16.dp))
            }
            LazyRow(horizontalArrangement = Arrangement.spacedBy(12.dp), contentPadding = PaddingValues(end = 16.dp)) {
                items(nearby, key = { it.key }) { room ->
                    RoomCard(room, matchRom(library.asSequence(), room.game), onJoin) { viewModel.focus.value = MultiplayerFocus.Room(room) }
                }
            }
        }
        MultiplayerFooter(centered = true) {
            Hint("A", stringResource(R.string.mp_join))
            Hint("X", stringResource(R.string.mp_hint_create))
            Hint("Y", stringResource(R.string.mp_code_title))
            Hint("SELECT", stringResource(R.string.mp_hint_profile))
            Hint("B", stringResource(R.string.mp_hint_library))
        }
    }
}

@Composable
fun Pulse() {
    val transition = rememberInfiniteTransition(label = "pulse")
    val phase by transition.animateFloat(0f, 1f, infiniteRepeatable(tween(1800), RepeatMode.Restart), label = "phase")
    Box(Modifier.size(8.dp), contentAlignment = Alignment.Center) {
        Box(Modifier.size(8.dp).scale(1f + phase * 1.6f).alpha(0.75f * (1f - phase)).clip(CircleShape).background(watermelon.green))
        Box(Modifier.size(8.dp).clip(CircleShape).background(watermelon.green))
    }
}

@Composable
fun roomTitle(room: MultiplayerViewModel.Nearby): String =
    if (room.play == MultiplayerRoom.Play.CHAT) stringResource(R.string.mp_play_chat) else room.game

@Composable
fun roomMeta(room: MultiplayerViewModel.Nearby): Pair<String, Color> {
    val colors = watermelon
    val other = room.app.isNotEmpty() && room.app != MultiplayerNearby.OWN_APP
    val kind = when {
        other -> room.app.uppercase()
        room.play == MultiplayerRoom.Play.DOWNLOAD -> stringResource(R.string.mp_tag_download)
        room.direct -> stringResource(R.string.mp_tag_direct)
        else -> stringResource(R.string.mp_tag_wifi)
    }
    val color = when {
        other -> colors.appText
        room.direct -> colors.goodText
        else -> colors.text2
    }
    return "${room.players}/${room.maximum} · $kind" to color
}

@Composable
private fun RoomCard(room: MultiplayerViewModel.Nearby, rom: Rom?, onJoin: (MultiplayerViewModel.Nearby) -> Unit, onFocused: () -> Unit) {
    val colors = watermelon
    val (meta, tint) = roomMeta(room)
    val name = stringResource(R.string.mp_room_of, room.host.nickname)
    Column(Modifier.width(136.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Pressable(onClick = { onJoin(room) }, modifier = Modifier.size(136.dp, 122.dp).onFocusChanged { if (it.isFocused) onFocused() }, border = colors.text.copy(alpha = 0.13f), label = name) {
            GameArt(rom, roomTitle(room), Modifier.fillMaxSize(), initials = 40.sp)
            Box(Modifier.align(Alignment.TopEnd).padding(6.dp).size(38.dp).clip(CircleShape).background(colors.bg), contentAlignment = Alignment.Center) {
                MultiplayerAvatar(room.host, 34.dp)
            }
        }
        Text(name, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 14.sp,
            maxLines = if (LocalMultiplayerCompact.current) Int.MAX_VALUE else 1,
            overflow = TextOverflow.Ellipsis)
        Mono(meta, color = tint, spacing = 0.5.sp, lines = if (LocalMultiplayerCompact.current) Int.MAX_VALUE else 1)
    }
}

@Composable
fun MultiplayerSheet(onDismiss: () -> Unit, modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    val colors = watermelon
    val compact = LocalMultiplayerCompact.current
    BackHandler { onDismiss() }
    Box(Modifier.fillMaxSize().background(Color(0xB80A090C)), contentAlignment = Alignment.Center) {
        Column(
            modifier.padding(16.dp).shadow(18.dp, RoundedCornerShape(14.dp)).clip(RoundedCornerShape(14.dp)).background(colors.surface)
                .border(1.dp, colors.text.copy(alpha = 0.13f), RoundedCornerShape(14.dp))
                .then(if (compact) Modifier.verticalScroll(rememberScrollState()) else Modifier).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            content = content,
        )
    }
}

@Composable
fun ConfirmOverlay(title: String, body: String, actions: List<Pair<String, () -> Unit>>, onDismiss: () -> Unit) {
    val colors = watermelon
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    MultiplayerSheet(onDismiss, Modifier.widthIn(max = 460.dp)) {
        Text(title, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 21.sp, letterSpacing = (-0.3).sp)
        Text(body, color = colors.text2, fontSize = 14.sp, lineHeight = 19.sp, modifier = Modifier.padding(bottom = 4.dp))
        actions.forEachIndexed { index, (label, action) ->
            MenuTile(action, Modifier.fillMaxWidth().heightIn(min = 48.dp).let { if (index == 0) it.focusRequester(focus) else it }) {
                TileTitle(label, Modifier.weight(1f))
            }
        }
    }
}
