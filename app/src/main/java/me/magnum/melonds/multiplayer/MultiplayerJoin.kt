package me.magnum.melonds.multiplayer

import android.view.KeyEvent
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.withFrameNanos
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.stringArrayResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.flow.SharedFlow
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import me.magnum.melonds.R
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.watermelon

@Composable
fun MultiplayerJoin(
    viewModel: MultiplayerViewModel,
    wide: Boolean,
    pad: SharedFlow<Int>,
    onJoin: (MultiplayerViewModel.Nearby) -> Unit,
    onBack: () -> Unit,
) {
    val colors = watermelon
    val nearby by viewModel.nearby.collectAsState()
    var code by rememberSaveable { mutableStateOf("") }
    val found = nearby.firstOrNull { code.length == 4 && it.code == code }
    LaunchedEffect(code) { viewModel.focus.value = MultiplayerFocus.Code(code) }
    val keyFocus = remember { FocusRequester() }
    val foundFocus = remember { FocusRequester() }
    fun enter() { found?.let(onJoin) }

    BackHandler { onBack() }
    LaunchedEffect(Unit) { runCatching { keyFocus.requestFocus() } }
    LaunchedEffect(pad, found) {
        pad.collect {
            when (it) {
                KeyEvent.KEYCODE_BUTTON_X -> code = code.dropLast(1)
                KeyEvent.KEYCODE_BUTTON_START -> enter()
            }
        }
    }
    val library by viewModel.roms.collectAsState()
    Column(Modifier.fillMaxSize()) {
        MultiplayerHeader(stringResource(R.string.mp_join_title), onBack = onBack)
        Column(Modifier.weight(1f).padding(start = 16.dp, end = 16.dp, top = 14.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(Modifier.height(70.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    (0 until 4).forEach { index ->
                        val char = code.getOrNull(index)
                        val active = index == code.length
                        Box(
                            Modifier.size(width = 54.dp, height = 70.dp).clip(RoundedCornerShape(10.dp))
                                .background(when {
                                    active -> tileColor(colors, false, true)
                                    char != null -> colors.surface2
                                    else -> colors.text.copy(alpha = 0.04f)
                                })
                                .border(if (active) 2.dp else 1.dp, if (active) colors.red else colors.text.copy(alpha = if (char != null) 0.13f else 0.08f),
                                    RoundedCornerShape(10.dp)),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(char?.toString().orEmpty(), color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 34.sp)
                        }
                    }
                }
                if (found != null) {
                    LaunchedEffect(found.key) {
                        withFrameNanos { }
                        runCatching { foundFocus.requestFocus() }
                    }
                    val (meta, tint) = roomMeta(found)
                    Pressable(onClick = { onJoin(found) }, modifier = Modifier.weight(1f).fillMaxHeight().focusRequester(foundFocus),
                        background = tileColor(colors, false, true), selected = true, border = null) {
                        Row(Modifier.fillMaxSize().padding(start = 6.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                            GameArt(matchRom(library.asSequence(), found.game), roomTitle(found), Modifier.size(62.dp, 55.dp).clip(RoundedCornerShape(6.dp)), initials = 18.sp)
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    Text(stringResource(R.string.mp_room_of, found.host.nickname), color = colors.text, fontFamily = SpaceGrotesk,
                                        fontWeight = FontWeight.Bold, fontSize = 15.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                                        modifier = Modifier.weight(1f, fill = false))
                                    AppBadge(found.app)
                                }
                                Mono(listOfNotNull(meta.substringBefore(" · "), roomTitle(found).takeIf { it.isNotEmpty() }?.uppercase(),
                                    meta.substringAfter(" · ")).joinToString(" · "), color = tint, size = 10.sp, spacing = 0.5.sp)
                            }
                            Row(Modifier.height(40.dp).clip(RoundedCornerShape(20.dp)).background(colors.red).padding(horizontal = 16.dp),
                                verticalAlignment = Alignment.CenterVertically) {
                                Text(stringResource(R.string.mp_enter), color = Color.White, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 15.sp)
                            }
                        }
                    }
                } else {
                    val complete = code.length == 4
                    Box(Modifier.weight(1f).fillMaxHeight().clip(RoundedCornerShape(10.dp)).background(colors.text.copy(alpha = 0.04f))
                        .drawBehind {
                            drawRoundRect(colors.text.copy(alpha = 0.25f), cornerRadius = CornerRadius(10.dp.toPx()),
                                style = Stroke(1.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(5.dp.toPx(), 4.dp.toPx()))))
                        }.padding(horizontal = 14.dp), contentAlignment = Alignment.CenterStart) {
                        Mono(stringResource(if (complete) R.string.mp_code_missing else R.string.mp_code_hint).uppercase(),
                            color = if (complete) colors.goldText else colors.text3, size = 11.sp, spacing = 1.sp, lines = 2)
                    }
                }
            }
            LazyVerticalGrid(GridCells.Fixed(if (wide) 11 else 8), horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp)) {
                items(MultiplayerRoom.CODE_ALPHABET.toList()) { char ->
                    Pressable(
                        onClick = { code = if (code.length < 4) code + char else char.toString() },
                        modifier = Modifier.height(44.dp).let { if (char == '2') it.focusRequester(keyFocus) else it },
                        shape = RoundedCornerShape(8.dp),
                        background = tileColor(colors, false, false),
                        border = null,
                    ) {
                        Text(char.toString(), color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.SemiBold, fontSize = 19.sp,
                            modifier = Modifier.align(Alignment.Center))
                    }
                }
            }
        }
        MultiplayerFooter {
            Hint("A", stringResource(R.string.mp_hint_type))
            Hint("X", stringResource(R.string.mp_hint_erase))
            Grow()
            PillButton(stringResource(R.string.mp_enter), { enter() }, enabled = found != null, key = "START")
        }
    }
}

@Composable
fun WatermelonField(
    value: String,
    onChange: (String) -> Unit,
    modifier: Modifier,
    type: KeyboardType = KeyboardType.Text,
    fontSize: Int = 18,
    onDone: () -> Unit = {},
) {
    val colors = watermelon
    val focusManager = LocalFocusManager.current
    BasicTextField(
        value = value,
        onValueChange = onChange,
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = type, imeAction = ImeAction.Done),
        keyboardActions = KeyboardActions(onDone = {
            focusManager.clearFocus()
            onDone()
        }),
        textStyle = TextStyle(color = colors.text, fontSize = fontSize.sp, fontFamily = SpaceGrotesk, fontWeight = FontWeight.SemiBold),
        cursorBrush = SolidColor(colors.red),
        modifier = modifier,
        decorationBox = { inner ->
            Box(
                Modifier.height(50.dp).clip(RoundedCornerShape(10.dp))
                    .background(tileColor(colors, true, false)).border(2.dp, colors.red, RoundedCornerShape(10.dp))
                    .padding(horizontal = 12.dp),
                contentAlignment = Alignment.CenterStart,
            ) { inner() }
        },
    )
}

@Composable
fun MultiplayerProfileScreen(viewModel: MultiplayerViewModel, pad: SharedFlow<Int>, onBack: () -> Unit) {
    val colors = watermelon
    val stored by viewModel.profile.collectAsState()
    val room by viewModel.room.collectAsState()
    var draft by remember(stored) { mutableStateOf(stored) }
    var picker by remember { mutableStateOf(0) }
    var restore by remember { mutableStateOf(false) }
    val birthdayFocus = remember { FocusRequester() }
    val colourNames = stringArrayResource(R.array.mp_colour_names)
    val symbolNames = stringArrayResource(R.array.mp_symbol_names)
    val months = stringArrayResource(R.array.mp_months)
    val focus = remember { FocusRequester() }

    fun save() {
        viewModel.saveProfile(draft) { if (it) onBack() }
    }

    BackHandler { onBack() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    LaunchedEffect(picker) {
        if (picker == 0 && restore) {
            withFrameNanos { }
            runCatching { birthdayFocus.requestFocus() }
            restore = false
        }
    }
    LaunchedEffect(pad, draft) {
        pad.collect {
            when (it) {
                KeyEvent.KEYCODE_BUTTON_START -> save()
                KeyEvent.KEYCODE_BUTTON_L1 -> draft = draft.copy(colour = (draft.colour + 15) % 16)
                KeyEvent.KEYCODE_BUTTON_R1 -> draft = draft.copy(colour = (draft.colour + 1) % 16)
                KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2 -> {
                    val symbols = MultiplayerProfile.SYMBOLS
                    val step = if (it == KeyEvent.KEYCODE_BUTTON_R2) 1 else symbols.size - 1
                    draft = draft.copy(symbol = symbols[(symbols.indexOf(draft.symbol).coerceAtLeast(0) + step) % symbols.size])
                }
            }
        }
    }
    Box(Modifier.fillMaxSize()) {
        CompositionLocalProvider(LocalMultiplayerInteractive provides (picker == 0)) {
        Column(Modifier.fillMaxSize()) {
        MultiplayerHeader(stringResource(R.string.mp_profile_title), onBack = onBack) {
            Mono(stringResource(R.string.mp_profile_sub).uppercase())
        }
        Row(Modifier.weight(1f).padding(horizontal = 16.dp, vertical = 14.dp), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Column(Modifier.width(236.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Box(Modifier.fillMaxWidth().height(100.dp).panel(colors, 12.dp)) {
                    Box(Modifier.fillMaxWidth().height(40.dp).background(DsPalette[draft.colour]))
                    Box(Modifier.padding(start = 12.dp, top = 12.dp).size(72.dp).clip(CircleShape)
                        .background(if (colors.isDark) Color(0xFF1A191F) else colors.surface2), contentAlignment = Alignment.Center) {
                        MultiplayerAvatar(draft, 64.dp)
                    }
                    Column(Modifier.padding(start = 94.dp, end = 12.dp, top = 46.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(draft.nickname, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 18.sp, maxLines = 1,
                            overflow = TextOverflow.Ellipsis)
                        if (draft.message.isNotEmpty()) Mono("\"${draft.message.uppercase()}\"", size = 10.sp, spacing = 0.5.sp)
                    }
                }
                ProfileField(stringResource(R.string.mp_name), draft.nickname, { draft = draft.copy(nickname = it.take(MultiplayerProfile.NICKNAME_LENGTH)) })
                ProfileField(stringResource(R.string.mp_message), draft.message, { draft = draft.copy(message = it.take(MultiplayerProfile.MESSAGE_LENGTH)) })
                MenuTile({ picker = 2 }, Modifier.fillMaxWidth().height(50.dp).focusRequester(birthdayFocus), label = stringResource(R.string.mp_birthday)) {
                    Mono(stringResource(R.string.mp_birthday).uppercase(), Modifier.width(74.dp), size = 10.sp)
                    TileTitle(stringResource(R.string.mp_birthday_value, draft.birthdayDay, months[draft.birthdayMonth - 1]), Modifier.weight(1f), size = 15.sp)
                }
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Mono(stringResource(R.string.mp_colour).uppercase())
                    Mono(colourNames[draft.colour].uppercase(), color = colors.text)
                }
                (0 until 2).forEach { row ->
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        (row * 8 until row * 8 + 8).forEach { index ->
                            Pressable(onClick = { draft = draft.copy(colour = index) },
                                modifier = Modifier.weight(1f).height(36.dp).let { if (index == stored.colour) it.focusRequester(focus) else it },
                                shape = RoundedCornerShape(8.dp), background = DsPalette[index], selected = index == draft.colour,
                                border = colors.text.copy(alpha = 0.13f), label = colourNames[index]) {
                                if (index == draft.colour) Box(Modifier.fillMaxSize().padding(2.dp).border(2.dp, Color.White, RoundedCornerShape(6.dp)))
                            }
                        }
                    }
                }
                Row(Modifier.padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Mono(stringResource(R.string.mp_symbol).uppercase())
                    Mono(symbolNames[MultiplayerProfile.SYMBOLS.indexOf(draft.symbol).coerceAtLeast(0)].uppercase(), color = colors.text)
                }
                (0 until 2).forEach { row ->
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        (row * 6 until row * 6 + 6).forEach { index ->
                            val symbol = MultiplayerProfile.SYMBOLS[index]
                            Pressable(onClick = { draft = draft.copy(symbol = symbol) }, modifier = Modifier.weight(1f).height(54.dp),
                                background = tileColor(colors, false, symbol == draft.symbol), selected = symbol == draft.symbol, border = null,
                                label = symbolNames[index]) {
                                MultiplayerAvatar(draft, 40.dp, Modifier.align(Alignment.Center), symbol = symbol)
                            }
                        }
                    }
                }
                Text(stringResource(if (room != null) R.string.mp_profile_room_note else R.string.mp_profile_firmware_note), color = colors.text3,
                    fontSize = 13.sp, lineHeight = 17.sp, maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(top = 2.dp))
            }
        }
        MultiplayerFooter {
            Hint("A", stringResource(R.string.mp_hint_edit))
            Hint("L R", stringResource(R.string.mp_hint_colour))
            Hint("L2 R2", stringResource(R.string.mp_hint_symbol))
            Hint("B", stringResource(R.string.multiplayer_back))
            Grow()
            PillButton(stringResource(R.string.mp_save), { save() }, enabled = draft.nickname.isNotBlank(), key = "START")
        }
        }
        }
        when (picker) {
            1 -> PickerOverlay(stringResource(R.string.mp_birthday), (1..daysIn(draft.birthdayMonth)).map { it.toString() }, draft.birthdayDay - 1, 7,
                { draft = draft.copy(birthdayDay = it + 1); restore = true; picker = 0 }) { restore = true; picker = 0 }
            2 -> PickerOverlay(stringResource(R.string.mp_birthday), months.toList(), draft.birthdayMonth - 1, 3,
                { draft = draft.copy(birthdayMonth = it + 1, birthdayDay = draft.birthdayDay.coerceAtMost(daysIn(it + 1))); picker = 1 }) {
                restore = true
                picker = 0
            }
        }
    }
}

@Composable
private fun ProfileField(label: String, value: String, onChange: (String) -> Unit) {
    val colors = watermelon
    var editing by remember { mutableStateOf(false) }
    var restore by remember { mutableStateOf(false) }
    val field = remember { FocusRequester() }
    val button = remember { FocusRequester() }
    if (editing) {
        var focused by remember { mutableStateOf(false) }
        WatermelonField(value, onChange, Modifier.fillMaxWidth().focusRequester(field).onFocusChanged { state ->
            if (state.isFocused) focused = true
            else if (focused) {
                editing = false
                restore = true
            }
        }, fontSize = 15)
        LaunchedEffect(Unit) {
            withFrameNanos { }
            runCatching { field.requestFocus() }
        }
    } else {
        MenuTile({ editing = true }, Modifier.fillMaxWidth().height(50.dp).focusRequester(button), label = label) {
            Mono(label.uppercase(), Modifier.width(74.dp), size = 10.sp)
            TileTitle(value, Modifier.weight(1f), size = 15.sp)
        }
        LaunchedEffect(restore) {
            if (restore) {
                withFrameNanos { }
                runCatching { button.requestFocus() }
                restore = false
            }
        }
    }
}

@Composable
private fun PickerOverlay(title: String, options: List<String>, selected: Int, columns: Int, onPick: (Int) -> Unit, onDismiss: () -> Unit) {
    val colors = watermelon
    val focus = remember { FocusRequester() }
    LaunchedEffect(options) {
        withFrameNanos { }
        runCatching { focus.requestFocus() }
    }
    MultiplayerSheet(onDismiss) {
        Text(title, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 21.sp, letterSpacing = (-0.3).sp)
        options.chunked(columns).forEachIndexed { row, chunk ->
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                chunk.forEachIndexed { column, label ->
                    val index = row * columns + column
                    MenuTile({ onPick(index) },
                        Modifier.width(if (columns > 4) 58.dp else 150.dp).height(42.dp).let { if (index == selected) it.focusRequester(focus) else it },
                        selected = index == selected) {
                        TileTitle(label, size = 15.sp)
                    }
                }
            }
        }
    }
}

private fun daysIn(month: Int): Int = when (month) {
    2 -> 29
    4, 6, 9, 11 -> 30
    else -> 31
}
