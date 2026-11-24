package me.magnum.melonds.multiplayer

import android.view.KeyEvent
import android.text.format.DateUtils
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.basicMarquee
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Group
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.text.style.TextOverflow
import me.magnum.melonds.ui.romlist.composables.PlatformBadge
import me.magnum.melonds.ui.romlist.composables.romPlatformLabel
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.ui.focus.FocusDirection
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import me.magnum.melonds.ui.romlist.composables.DsBoxArtAspectRatio
import me.magnum.melonds.ui.theme.WatermelonMono
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.Icon
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Chat
import androidx.compose.material.icons.filled.SportsEsports
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.produceState
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.InputMode
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalInputModeManager
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.flow.SharedFlow
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.romlist.composables.romDisplayName
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.watermelon

@Composable
fun MultiplayerCreate(
    viewModel: MultiplayerViewModel,
    wide: Boolean,
    pad: SharedFlow<Int>,
    editing: Boolean,
    onBios: () -> Unit,
    requestPermission: (() -> Unit) -> Unit,
    onDone: () -> Unit,
    onBack: () -> Unit,
) {
    val compact = LocalMultiplayerCompact.current
    val roms by viewModel.roms.collectAsState()
    val bios by viewModel.bios.collectAsState()
    val busy by viewModel.busy.collectAsState()
    val failure by viewModel.failure.collectAsState()
    val failureMessage = (failure as? MultiplayerViewModel.Failure.Message)?.let { stringResource(it.text) }
    val current = viewModel.room.collectAsState().value?.info
    var play by rememberSaveable { mutableStateOf(current?.play ?: MultiplayerRoom.Play.ROM) }
    var selected by rememberSaveable { mutableStateOf<String?>(null) }
    var showAll by rememberSaveable(play) { mutableStateOf(false) }
    var filter by rememberSaveable { mutableIntStateOf(0) }
    var step by rememberSaveable { mutableIntStateOf(0) }
    val steps = if (play == MultiplayerRoom.Play.CHAT) listOf(0) else listOf(0, 1)
    val index = steps.indexOf(step).coerceAtLeast(0)
    var query by rememberSaveable { mutableStateOf("") }
    val catalog by produceState<MultiplayerCatalog?>(null, viewModel) { value = viewModel.gameCatalog() }
    val eligible = remember(roms, play, showAll, catalog) {
        if (showAll) roms else roms.filter { (catalog?.get(it)?.players(play) ?: 0) >= 2 }
    }
    val shown = remember(eligible, query, filter) {
        when {
            query.isNotBlank() -> eligible.filter {
                romDisplayName(it).contains(query.trim(), true) || it.fileName.contains(query.trim(), true)
            }
            filter == 1 -> eligible.filter { it.lastPlayed != null }.sortedByDescending { it.lastPlayed!!.time }.take(30)
            filter == 2 -> eligible.filter { it.isFavorite }
            else -> eligible
        }
    }
    val rom = shown.firstOrNull { it.uri.toString() == selected }
        ?: shown.firstOrNull { it.fileName == current?.game?.file && it.cartridge?.gameCode == current?.game?.code }
        ?: shown.firstOrNull()
    val canFinish = play == MultiplayerRoom.Play.CHAT || rom != null

    fun jump(direction: Int) {
        val currentLetter = rom?.let(::letterOf) ?: return
        val ordered = shown.sortedBy { romDisplayName(it).lowercase() }
        val letters = ordered.mapNotNull(::letterOf).distinct()
        val nextLetter = letters.getOrNull(letters.indexOf(currentLetter) + direction) ?: return
        selected = ordered.first { letterOf(it) == nextLetter }.uri.toString()
    }
    val last = index == steps.lastIndex
    val stepLabel = stringResource(R.string.mp_step_mode).let {
        if (steps.size > 1) "${index + 1}/${steps.size} · $it" else it
    }

    fun finish() {
        if (!canFinish || busy) return
        if (editing) {
            viewModel.change(play, rom, showAll, onDone)
            return
        }
        val maximum = if (play == MultiplayerRoom.Play.CHAT) 16 else 8
        val open = { viewModel.host(play, rom, maximum, showAll) { ok -> if (ok) onDone() } }
        if (viewModel.directSupported && !viewModel.directPermitted()) {
            requestPermission { if (viewModel.directPermitted()) open() else viewModel.permissionDenied() }
        } else open()
    }

    fun next() {
        if (last) finish() else step = steps[index + 1]
    }

    fun back() {
        viewModel.dismissFailure()
        if (index == 0) onBack() else step = steps[index - 1]
    }

    val uriHandler = LocalUriHandler.current
    var searchFocused by remember(step) { mutableStateOf(false) }
    val keyboardVisible = compact && step == 1 && WindowInsets.ime.getBottom(LocalDensity.current) > 0
    BackHandler { back() }
    val padAction by rememberUpdatedState<(Int) -> Unit> { key ->
        when (key) {
            KeyEvent.KEYCODE_BUTTON_START -> next()
            KeyEvent.KEYCODE_BUTTON_L1 -> if (step == 1) jump(-1)
            KeyEvent.KEYCODE_BUTTON_R1 -> if (step == 1) jump(1)
        }
    }
    LaunchedEffect(pad) { pad.collect { padAction(it) } }

    Column(Modifier.fillMaxSize()) {
        MultiplayerHeader(
            title = stringResource(if (step == 0) R.string.mp_create_title else if (play == MultiplayerRoom.Play.DOWNLOAD) R.string.mp_play_download else R.string.mp_play_rom),
            onBack = { back() },
            tags = {
                if (compact && step == 0) Mono(stepLabel, size = 11.sp)
            },
        ) {
            if (!compact && step == 0) {
                Mono(stepLabel, size = 11.sp)
            } else if (!compact) {
                SearchField(query, { query = it }, eligible.size, searchFocused,
                    Modifier.width(214.dp).onFocusChanged { searchFocused = it.isFocused })
            }
        }
        if (compact && step == 1) {
            SearchField(query, { query = it }, eligible.size, searchFocused,
                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp)
                    .onFocusChanged { searchFocused = it.isFocused })
        }
        Box(Modifier.weight(1f).fillMaxWidth()) {
            when (step) {
                0 -> ModeStep(play, bios, onBios, { mode, locked -> viewModel.focus.value = MultiplayerFocus.Mode(mode, locked) }) {
                    if (it == play) next() else play = it
                }
                else -> GameStep(shown, rom, filter, wide, query.isNotBlank(), searchFocused, catalog, play, showAll,
                    showFilters = !keyboardVisible,
                    failureMessage = failureMessage.takeIf { compact },
                    onShowAll = { showAll = !showAll; filter = 0; query = "" },
                    onFilter = { filter = it; query = "" },
                    onFocusGame = { viewModel.focus.value = MultiplayerFocus.Game(it, play) },
                ) {
                    if (it.uri == rom?.uri) next() else selected = it.uri.toString()
                }
            }
        }
        if (failureMessage != null && (!compact || step != 1)) {
            CreateFailure(failureMessage, Modifier.padding(horizontal = 18.dp, vertical = 6.dp))
        }
        MultiplayerFooter(actions = {
            if (step == 1 && !keyboardVisible) {
                FilterChip("GameTDB", false) { uriHandler.openUri("https://www.gametdb.com/DS") }
            }
            PillButton(
                text = stringResource(if (!last) R.string.mp_next else if (editing) R.string.mp_save_game else R.string.mp_open_room),
                onClick = { next() },
                enabled = !busy && (step != 1 || rom != null),
                key = "START",
            )
        }) {
            if (step == 0) {
                Hint("A", stringResource(R.string.mp_hint_choose_next))
                Hint("B", stringResource(R.string.multiplayer_back))
            } else if (!keyboardVisible) {
                Hint("A", stringResource(R.string.mp_hint_choose))
                Hint("L R", stringResource(R.string.mp_hint_letter))
            }
        }
    }
}

@Composable
private fun SearchField(query: String, onQuery: (String) -> Unit, total: Int, focused: Boolean, modifier: Modifier) {
    val colors = watermelon
    val focusManager = LocalFocusManager.current
    val inputMode = LocalInputModeManager.current
    fun leaveSearch() {
        if (!focusManager.moveFocus(FocusDirection.Down)) focusManager.clearFocus()
    }
    BackHandler(enabled = focused) { leaveSearch() }
    BasicTextField(
        value = query,
        onValueChange = { onQuery(it.take(40)) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
        keyboardActions = KeyboardActions(onSearch = { leaveSearch() }),
        textStyle = TextStyle(color = colors.text, fontFamily = WatermelonMono, fontSize = 12.sp),
        cursorBrush = SolidColor(colors.red),
        modifier = modifier
            .focusProperties { canFocus = focused || inputMode.inputMode == InputMode.Touch }
            .onPreviewKeyEvent {
                if (it.type == KeyEventType.KeyDown && it.key == Key.DirectionDown) {
                    leaveSearch()
                    true
                } else false
            },
        decorationBox = { inner ->
            Row(
                Modifier.heightIn(min = if (LocalMultiplayerCompact.current) 48.dp else 38.dp)
                    .clip(RoundedCornerShape(19.dp)).background(colors.surface2)
                    .border(if (focused) 2.dp else 0.dp, if (focused) colors.red else Color.Transparent, RoundedCornerShape(19.dp))
                    .padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Icon(Icons.Filled.Search, null, tint = colors.text2, modifier = Modifier.size(18.dp))
                Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
                    if (query.isEmpty()) Mono(stringResource(R.string.mp_search_games, total), color = colors.text3, size = 12.sp, spacing = 0.sp)
                    inner()
                }
            }
        },
    )
}

@Composable
private fun ModeStep(play: MultiplayerRoom.Play, bios: Boolean, onBios: () -> Unit, onFocusMode: (MultiplayerRoom.Play, Boolean) -> Unit,
    onPick: (MultiplayerRoom.Play) -> Unit) {
    val colors = watermelon
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    val chatLocked = !bios
    val compact = LocalMultiplayerCompact.current
    val modes: @Composable (Modifier) -> Unit = { modifier ->
        Column(modifier, verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Mono(stringResource(R.string.mp_how_play).trimEnd('?', '¿').trimStart('¿').uppercase(), Modifier.padding(start = 2.dp, bottom = 2.dp))
            listOf(
                Triple(MultiplayerRoom.Play.ROM, Icons.Filled.SportsEsports, R.string.mp_play_rom),
                Triple(MultiplayerRoom.Play.DOWNLOAD, Icons.Filled.Wifi, R.string.mp_play_download),
                Triple(MultiplayerRoom.Play.CHAT, Icons.AutoMirrored.Filled.Chat, R.string.mp_play_chat),
            ).forEach { (mode, icon, title) ->
                val locked = mode == MultiplayerRoom.Play.CHAT && chatLocked
                MenuTile(
                    onClick = { if (locked) onBios() else onPick(mode) },
                    modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp).let { if (mode == play) it.focusRequester(focus) else it }
                        .onFocusChanged { if (it.isFocused) onFocusMode(mode, locked) },
                    selected = mode == play && !locked,
                    dim = locked,
                ) {
                    Icon(icon, null, tint = colors.text, modifier = Modifier.size(20.dp))
                    Spacer(Modifier.width(10.dp))
                    TileTitle(stringResource(title), Modifier.weight(1f))
                    Mono(
                        if (locked) "BIOS" else stringResource(R.string.mp_max, if (mode == MultiplayerRoom.Play.CHAT) 16 else 8),
                        color = if (locked) colors.goldText else colors.text3, spacing = 0.5.sp,
                    )
                }
            }
        }
    }
    val description: @Composable (Modifier) -> Unit = { modifier ->
        val locked = play == MultiplayerRoom.Play.CHAT && chatLocked
        Column(modifier.panel(colors).padding(horizontal = 16.dp, vertical = 14.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Mono(stringResource(R.string.mp_each_needs), lines = if (compact) Int.MAX_VALUE else 1)
            if (compact) {
                FlowRow(Modifier.fillMaxWidth(), itemVerticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(14.dp, Alignment.CenterHorizontally),
                    verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    repeat(3) { ConsoleNeed(play, it) }
                }
            } else {
                Row(Modifier.weight(1f).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(14.dp, Alignment.CenterHorizontally)) {
                    repeat(3) { ConsoleNeed(play, it) }
                }
            }
            Text(
                stringResource(when (play) {
                    MultiplayerRoom.Play.ROM -> R.string.mp_play_rom_desc
                    MultiplayerRoom.Play.DOWNLOAD -> R.string.mp_play_download_desc
                    MultiplayerRoom.Play.CHAT -> R.string.mp_play_chat_desc
                }),
                color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.SemiBold, fontSize = 16.sp, lineHeight = 21.sp,
            )
            Mono(
                stringResource(when {
                    locked -> R.string.mp_meta_chat_locked
                    play == MultiplayerRoom.Play.ROM -> R.string.mp_meta_rom
                    play == MultiplayerRoom.Play.DOWNLOAD -> R.string.mp_meta_download
                    else -> R.string.mp_meta_chat
                }).uppercase(),
                color = if (locked) colors.goldText else colors.text2, size = 11.sp, spacing = 0.5.sp, lines = if (compact) Int.MAX_VALUE else 2,
            )
        }
    }
    if (compact) {
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)) {
            modes(Modifier.fillMaxWidth())
            description(Modifier.fillMaxWidth())
        }
    } else {
        Row(Modifier.fillMaxSize().padding(start = 16.dp, end = 16.dp, top = 16.dp, bottom = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            modes(Modifier.width(300.dp))
            description(Modifier.weight(1f).fillMaxHeight())
        }
    }
}

@Composable
internal fun ConsoleNeed(play: MultiplayerRoom.Play, index: Int) {
    val colors = watermelon
    val receives = play == MultiplayerRoom.Play.DOWNLOAD && index > 0
    val transition = rememberInfiniteTransition(label = "receive")
    val wave by transition.animateFloat(0.45f, 1f, infiniteRepeatable(tween(800), RepeatMode.Reverse), label = "wave")
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Canvas(Modifier.size(58.dp, 64.dp)) {
            val unit = size.width / 58f
            val body = colors.surface2
            val stroke = if (index == 0) colors.red else colors.text.copy(alpha = 0.22f)
            val screen = if (receives) colors.surface2 else Color(0xFF30BAF3)
            fun shell(y: Float, h: Float) {
                drawRoundRect(body, Offset(4 * unit, y * unit), Size(50 * unit, h * unit), CornerRadius(5 * unit))
                drawRoundRect(stroke, Offset(4 * unit, y * unit), Size(50 * unit, h * unit), CornerRadius(5 * unit), style = Stroke(2 * unit))
            }
            shell(2f, 28f)
            drawRoundRect(screen, Offset(10 * unit, 7 * unit), Size(38 * unit, 18 * unit), CornerRadius(2 * unit))
            shell(33f, 29f)
            drawRoundRect(screen, Offset(17 * unit, 37 * unit), Size(24 * unit, 18 * unit), CornerRadius(2 * unit))
            val dot = colors.text.copy(alpha = 0.25f)
            drawCircle(dot, 2.5f * unit, Offset(10 * unit, 47 * unit))
            drawCircle(dot, 2f * unit, Offset(48 * unit, 44 * unit))
            drawCircle(dot, 2f * unit, Offset(48 * unit, 50 * unit))
        }
        val (text, background, ink) = when {
            play == MultiplayerRoom.Play.CHAT -> Triple(stringResource(R.string.mp_need_menu), colors.surface3, colors.text)
            receives -> Triple(stringResource(R.string.mp_need_receives), colors.green.copy(alpha = 0.18f), colors.goodText)
            else -> Triple("ROM", colors.surface3, colors.text)
        }
        Box(Modifier.height(24.dp).clip(RoundedCornerShape(6.dp)).background(background).alpha(if (receives) wave else 1f)
            .padding(horizontal = 8.dp), contentAlignment = Alignment.Center) {
            Mono(text, color = ink, size = 10.sp, spacing = 0.5.sp)
        }
    }
}

@Composable
private fun CreateFailure(message: String, modifier: Modifier) {
    Text(message, color = watermelon.goldText, fontSize = 14.sp, lineHeight = 19.sp, modifier = modifier)
}

@Composable
private fun GameStep(
    shown: List<Rom>,
    rom: Rom?,
    filter: Int,
    wide: Boolean,
    searching: Boolean,
    searchFocused: Boolean,
    catalog: MultiplayerCatalog?,
    play: MultiplayerRoom.Play,
    showAll: Boolean,
    showFilters: Boolean,
    failureMessage: String?,
    onShowAll: () -> Unit,
    onFilter: (Int) -> Unit,
    onFocusGame: (Rom) -> Unit,
    onPick: (Rom) -> Unit,
) {
    val colors = watermelon
    val context = LocalContext.current
    val compact = LocalMultiplayerCompact.current
    val focus = remember { FocusRequester() }
    val grid = rememberLazyGridState()
    val repeated = remember(shown) { shown.groupingBy { romDisplayName(it) }.eachCount().filterValues { it > 1 }.keys }
    val letters = remember(shown) { shown.mapNotNull { letterOf(it) }.distinct() }
    val current = rom?.let { letterOf(it) }
    LaunchedEffect(failureMessage) {
        if (failureMessage != null) grid.scrollToItem(0)
    }
    LaunchedEffect(filter, rom?.uri, shown.size, searchFocused) {
        val index = shown.indexOfFirst { it.uri == rom?.uri }
        if (index >= 0) {
            grid.scrollToItem(index + if (failureMessage != null) 1 else 0)
            if (!searchFocused) {
                withFrameNanos { }
                runCatching { focus.requestFocus() }
            }
        }
    }
    Column(Modifier.fillMaxSize()) {
        if (showFilters) FlowRow(Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, top = 8.dp, bottom = 4.dp),
            itemVerticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(7.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf(0 to R.string.mp_filter_library, 1 to R.string.mp_filter_recent,
                2 to R.string.mp_filter_favorites).forEach { (value, label) ->
                FilterChip(stringResource(label).uppercase(), filter == value && !searching) { onFilter(value) }
            }
            FilterChip(stringResource(R.string.mp_show_all).uppercase(), showAll, onShowAll)
            if (!compact) Grow()
            Mono(
                when {
                    searching -> pluralStringResource(R.plurals.mp_count_found, shown.size, shown.size)
                    filter == 1 -> pluralStringResource(R.plurals.mp_count_recent, shown.size, shown.size)
                    filter == 2 -> pluralStringResource(R.plurals.mp_count_favorites, shown.size, shown.size)
                    else -> pluralStringResource(R.plurals.mp_count_games, shown.size, shown.size)
                }.uppercase(),
                size = 11.sp, spacing = 0.sp,
            )
        }
        if (shown.isEmpty()) {
            val emptyText = when {
                catalog == null && !showAll -> R.string.mp_reading_headers
                searching -> R.string.mp_search_empty
                !showAll -> R.string.mp_no_games_for_mode
                else -> R.string.mp_no_games
            }
            Text(stringResource(emptyText), color = colors.text2, fontSize = 15.sp,
                modifier = Modifier.padding(16.dp))
        }
        Row(Modifier.weight(1f).padding(start = 16.dp, end = if (compact) 16.dp else 0.dp, top = 8.dp)) {
            LazyVerticalGrid(
                state = grid,
                columns = GridCells.Fixed(if (compact) 2 else if (wide) 5 else 4),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
                contentPadding = PaddingValues(bottom = 8.dp),
                modifier = Modifier.weight(1f),
            ) {
                if (failureMessage != null) {
                    item(key = "create-failure", span = { GridItemSpan(maxLineSpan) }) {
                        CreateFailure(failureMessage, Modifier.fillMaxWidth().padding(vertical = 6.dp))
                    }
                }
                items(shown, key = { it.uri.toString() }) { item ->
                    val on = item.uri == rom?.uri
                    val name = romDisplayName(item)
                    var focused by remember { mutableStateOf(false) }
                    val marquee = if (focused) Modifier.basicMarquee(iterations = Int.MAX_VALUE) else Modifier
                    val modes = catalog?.get(item)
                    val players = modes?.players(play) ?: 0
                    Column(verticalArrangement = Arrangement.spacedBy(5.dp)) {
                        Pressable(
                            onClick = { onPick(item) },
                            modifier = Modifier.fillMaxWidth().aspectRatio(DsBoxArtAspectRatio)
                                .let { if (on) it.focusRequester(focus) else it }
                                .onFocusChanged {
                                    focused = it.isFocused
                                    if (it.isFocused) onFocusGame(item)
                                },
                            selected = on,
                            border = colors.text.copy(alpha = 0.13f),
                            label = name,
                        ) {
                            GameArt(item, name, Modifier.fillMaxSize(), initials = 30.sp)
                            PlatformBadge(romPlatformLabel(item), Modifier.align(Alignment.TopStart).padding(start = 6.dp, top = 6.dp))
                            Column(Modifier.align(Alignment.TopEnd).padding(6.dp), horizontalAlignment = Alignment.End,
                                verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                if (players >= 2) CapabilityBadge(players.toString(), people = true)
                                if ((modes?.singleCard ?: 0) >= 2) CapabilityBadge("DL", people = false)
                            }
                            if (!compact) {
                                Box(Modifier.align(Alignment.BottomStart).fillMaxWidth()
                                    .background(Brush.verticalGradient(listOf(Color.Transparent, Color.Black.copy(alpha = 0.78f))))
                                    .padding(start = 8.dp, end = if (on) 32.dp else 8.dp, top = 16.dp, bottom = 7.dp)) {
                                    Text(name, marquee, color = Color.White, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold,
                                        fontSize = 11.sp, lineHeight = 13.sp, maxLines = 1,
                                        overflow = if (focused) TextOverflow.Clip else TextOverflow.Ellipsis)
                                }
                            }
                            if (on) {
                                Box(Modifier.align(Alignment.BottomEnd).padding(6.dp).size(22.dp).clip(CircleShape).background(colors.red),
                                    contentAlignment = Alignment.Center) {
                                    Icon(Icons.Filled.Check, null, tint = Color.White, modifier = Modifier.size(14.dp))
                                }
                            }
                        }
                        if (compact) {
                            Text(name, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold,
                                fontSize = 13.sp, lineHeight = 16.sp)
                        }
                        val detail = when {
                            name in repeated -> item.fileName
                            filter == 1 && item.lastPlayed != null -> context.getString(R.string.rom_last_played_format,
                                DateUtils.getRelativeTimeSpanString(item.lastPlayed!!.time, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS))
                            else -> null
                        }
                        detail?.let {
                            Mono(it, if (compact) Modifier else marquee, color = if (on) colors.text else colors.text3, size = 10.sp,
                                spacing = 0.sp, lines = if (compact) Int.MAX_VALUE else 1)
                        }
                    }
                }
            }
            if (!compact) Column(Modifier.width(26.dp).padding(top = 2.dp), horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(1.dp)) {
                letters.forEach { letter ->
                    val on = letter == current
                    Box(Modifier.size(20.dp, if (on) 18.dp else 14.dp).clip(RoundedCornerShape(9.dp)).background(if (on) colors.red else Color.Transparent),
                        contentAlignment = Alignment.Center) {
                        Text(letter.toString(), color = if (on) Color.White else colors.text3, fontFamily = WatermelonMono, fontSize = 10.sp,
                            fontWeight = if (on) FontWeight.Bold else FontWeight.Normal, lineHeight = 12.sp)
                    }
                }
            }
        }
    }
}

@Composable
private fun FilterChip(label: String, selected: Boolean, onClick: () -> Unit) {
    val colors = watermelon
    Pressable(onClick = onClick, modifier = Modifier.heightIn(min = if (LocalMultiplayerCompact.current) 40.dp else 28.dp),
        shape = RoundedCornerShape(14.dp),
        background = if (selected) colors.red else colors.surface2, border = null) {
        Text(label, color = if (selected) Color.White else colors.text2, fontFamily = SpaceGrotesk,
            fontWeight = FontWeight.Bold, fontSize = 11.sp, letterSpacing = 0.5.sp,
            modifier = Modifier.align(Alignment.Center).padding(horizontal = 10.dp))
    }
}

@Composable
private fun CapabilityBadge(text: String, people: Boolean) {
    val shape = RoundedCornerShape(10.dp)
    Row(
        Modifier.height(20.dp).clip(shape)
            .background(if (people) Color(0xFF141317).copy(alpha = 0.82f) else Color(0xFF30BAF3).copy(alpha = 0.9f))
            .let { if (people) it.border(1.dp, Color.White.copy(alpha = 0.18f), shape) else it }
            .padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        if (people) Icon(Icons.Filled.Group, null, tint = Color.White, modifier = Modifier.size(12.dp))
        Text(text, color = if (people) Color.White else Color(0xFF0A090C), fontFamily = WatermelonMono, fontWeight = FontWeight.Bold,
            fontSize = 10.sp, lineHeight = 10.sp)
    }
}

private fun letterOf(rom: Rom): Char? = romDisplayName(rom).firstOrNull()?.uppercaseChar()?.let { if (it.isLetter()) it else '#' }
