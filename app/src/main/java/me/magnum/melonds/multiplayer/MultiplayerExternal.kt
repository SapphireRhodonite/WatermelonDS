package me.magnum.melonds.multiplayer

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material.icons.filled.WifiOff
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringArrayResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.common.ExternalCrumbInfo
import me.magnum.melonds.ui.romlist.composables.DsBoxArtAspectRatio
import me.magnum.melonds.ui.romlist.composables.romDisplayName
import me.magnum.melonds.ui.romlist.composables.romGradient
import me.magnum.melonds.ui.romlist.composables.romPlatformLabel
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.WatermelonColors
import me.magnum.melonds.ui.theme.WatermelonMono
import me.magnum.melonds.ui.theme.watermelon

sealed interface MultiplayerFocus {
    data class Room(val room: MultiplayerViewModel.Nearby) : MultiplayerFocus
    data class Mode(val play: MultiplayerRoom.Play, val locked: Boolean) : MultiplayerFocus
    data class Game(val rom: Rom, val play: MultiplayerRoom.Play) : MultiplayerFocus
    data class Code(val code: String) : MultiplayerFocus
}

@Composable
fun MultiplayerExternalInfo(viewModel: MultiplayerViewModel) {
    val page by viewModel.page.collectAsState()
    val focus by viewModel.focus.collectAsState()
    val covers by viewModel.covers.collectAsState()
    val profile by viewModel.profile.collectAsState()
    val title = stringResource(R.string.multiplayer_title)
    CompositionLocalProvider(LocalGameCovers provides GameCovers(covers, viewModel::requestCover)) {
        when (page) {
            MultiplayerPage.PERMISSION -> ExternalCrumbInfo(Icons.Filled.Wifi, stringResource(R.string.mp_perm_title),
                stringResource(R.string.mp_perm_without), title)
            MultiplayerPage.HUB -> {
                val nearby by viewModel.nearby.collectAsState()
                val room = (focus as? MultiplayerFocus.Room)?.let { focused -> nearby.firstOrNull { it.key == focused.room.key } }
                if (room != null) ExternalRoom(viewModel, room) else ExternalProfile(profile)
            }
            MultiplayerPage.CREATE, MultiplayerPage.EDIT -> when (val value = focus) {
                is MultiplayerFocus.Mode -> ExternalMode(value)
                is MultiplayerFocus.Game -> ExternalGame(viewModel, value)
                else -> ExternalProfile(profile)
            }
            MultiplayerPage.LOBBY -> ExternalLobby(viewModel)
            MultiplayerPage.JOIN -> ExternalCode(viewModel, (focus as? MultiplayerFocus.Code)?.code.orEmpty())
            MultiplayerPage.CONNECTING -> ExternalConnecting(viewModel)
            MultiplayerPage.DIRECT_FAIL -> ExternalCrumbInfo(Icons.Filled.WifiOff, stringResource(R.string.mp_fail_title),
                stringResource(R.string.mp_fail_plan_b), title)
            MultiplayerPage.PROFILE -> ExternalProfile(profile)
        }
    }
}

@Composable
private fun ExternalShowcase(
    rom: Rom?,
    title: String,
    overlay: @Composable BoxScope.() -> Unit = {},
    content: @Composable ColumnScope.() -> Unit,
) {
    val url = rom?.let { LocalGameCovers.current.urls[it.uri.toString()] }?.takeIf { it.isNotEmpty() }
    Box(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxSize().background(romGradient(title.ifEmpty { "DS" })))
        if (url != null) {
            AsyncImage(model = url, contentDescription = null, contentScale = ContentScale.Crop, alpha = 0.45f,
                modifier = Modifier.fillMaxSize().blur(18.dp))
        }
        Box(Modifier.fillMaxSize().background(Brush.horizontalGradient(listOf(Color.Black.copy(alpha = 0.72f), Color.Black.copy(alpha = 0.35f)))))
        Row(Modifier.fillMaxSize().padding(horizontal = 36.dp, vertical = 30.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.width(150.dp)) {
                GameArt(rom, title, Modifier.fillMaxWidth().aspectRatio(DsBoxArtAspectRatio).shadow(14.dp, RoundedCornerShape(12.dp))
                    .clip(RoundedCornerShape(12.dp)).border(1.dp, Color.White.copy(alpha = 0.18f), RoundedCornerShape(12.dp)), initials = 44.sp)
                overlay()
            }
            Spacer(Modifier.width(26.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp), content = content)
        }
    }
}

@Composable
private fun ExternalTitle(text: String) {
    Text(text, color = Color.White, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 27.sp, lineHeight = 31.sp,
        maxLines = 2, overflow = TextOverflow.Ellipsis)
}

@Composable
private fun ExternalMeta(text: String, color: Color = Color.White.copy(alpha = 0.55f), size: TextUnit = 11.sp) {
    Text(text, color = color, fontFamily = WatermelonMono, fontSize = size, letterSpacing = 0.6.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
}

@Composable
private fun ExternalCrumb(text: String) {
    Text(text.uppercase(), color = Color.White.copy(alpha = 0.35f), fontFamily = WatermelonMono, fontSize = 9.5.sp, letterSpacing = 0.6.sp)
}

@Composable
private fun ExternalHints(vararg hints: Pair<String, String>) {
    Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(14.dp)) {
        hints.forEach { (key, label) ->
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                val shape = if (key.length == 1) CircleShape else RoundedCornerShape(9.dp)
                Box(Modifier.height(17.dp).let { if (key.length == 1) it.width(17.dp) else it }.border(1.5.dp, Color.White.copy(alpha = 0.4f), shape)
                    .padding(horizontal = if (key.length == 1) 0.dp else 5.dp), contentAlignment = Alignment.Center) {
                    Text(key, color = Color.White.copy(alpha = 0.7f), fontSize = if (key.length == 1) 9.sp else 8.sp, lineHeight = 9.sp,
                        fontWeight = FontWeight.Bold)
                }
                Text(label, color = Color.White.copy(alpha = 0.5f), fontFamily = WatermelonMono, fontSize = 10.sp)
            }
        }
    }
}

@Composable
private fun modeTitle(play: MultiplayerRoom.Play): String = stringResource(when (play) {
    MultiplayerRoom.Play.ROM -> R.string.mp_play_rom
    MultiplayerRoom.Play.DOWNLOAD -> R.string.mp_play_download
    MultiplayerRoom.Play.CHAT -> R.string.mp_play_chat
})

@Composable
private fun ExternalRoom(viewModel: MultiplayerViewModel, room: MultiplayerViewModel.Nearby) {
    val library by viewModel.roms.collectAsState()
    val game = roomTitle(room)
    ExternalShowcase(
        rom = matchRom(library.asSequence(), room.game),
        title = game,
        overlay = {
            Box(Modifier.align(Alignment.BottomEnd).offset(12.dp, 12.dp).size(58.dp).clip(CircleShape).background(WatermelonColors.tvBg),
                contentAlignment = Alignment.Center) {
                MultiplayerAvatar(room.host, 52.dp)
            }
        },
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Tag(stringResource(if (room.direct) R.string.mp_tag_direct else R.string.mp_tag_wifi), TagKind.GOOD)
            if (room.play == MultiplayerRoom.Play.DOWNLOAD) Tag(stringResource(R.string.mp_tag_download), TagKind.APP)
            AppBadge(room.app)
        }
        ExternalTitle(stringResource(R.string.mp_room_of, room.host.nickname))
        Text(listOfNotNull(game.takeIf { it.isNotEmpty() }, room.play?.takeIf { it != MultiplayerRoom.Play.CHAT }?.let { modeTitle(it) })
            .joinToString(" · "), color = Color.White.copy(alpha = 0.65f), fontSize = 13.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
        ExternalMeta(listOfNotNull(stringResource(R.string.mp_players_of, room.players, room.maximum),
            room.code.takeIf { it.isNotEmpty() }?.let { "${stringResource(R.string.mp_code_label)} $it" }).joinToString(" · "))
        ExternalHints("A" to stringResource(R.string.mp_join), "Y" to stringResource(R.string.mp_code_title))
    }
}

@Composable
private fun ExternalGame(viewModel: MultiplayerViewModel, focus: MultiplayerFocus.Game) {
    val catalog by produceState<MultiplayerCatalog?>(null, viewModel) { value = viewModel.gameCatalog() }
    val rom = focus.rom
    val name = romDisplayName(rom)
    val modes = catalog?.get(rom.cartridge)
    ExternalShowcase(rom, name) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Tag(romPlatformLabel(rom), TagKind.NEUTRAL)
            rom.cartridge?.gameCode?.takeIf { it.isNotBlank() }?.let { Tag(it, TagKind.GOOD) }
        }
        ExternalTitle(name)
        ExternalMeta(rom.fileName, size = 11.sp)
        if (catalog != null) {
            val rows = listOf(MultiplayerRoom.Play.ROM to (modes?.multiCard ?: 0), MultiplayerRoom.Play.DOWNLOAD to (modes?.singleCard ?: 0))
                .filter { it.second >= 2 }
            Column(Modifier.padding(top = 4.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                rows.forEach { (play, players) ->
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        Tag(stringResource(R.string.mp_max, players), if (play == focus.play) TagKind.GOOD else TagKind.NEUTRAL)
                        Text(modeTitle(play), color = Color.White.copy(alpha = if (play == focus.play) 1f else 0.6f), fontSize = 14.sp)
                    }
                }
                if (rows.isEmpty()) {
                    ExternalMeta(stringResource(if (modes?.wfc == true) R.string.mp_wfc_mode_unconfirmed else R.string.mp_mode_unconfirmed).uppercase(),
                        color = watermelon.goldText)
                }
            }
        }
        ExternalHints("A" to stringResource(R.string.mp_hint_choose), "START" to stringResource(R.string.mp_open_room))
    }
}

@Composable
private fun ExternalMode(focus: MultiplayerFocus.Mode) {
    val colors = watermelon
    Column(Modifier.fillMaxSize().padding(horizontal = 60.dp, vertical = 30.dp), horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(10.dp, Alignment.CenterVertically)) {
        Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) { (0 until 3).forEach { ConsoleNeed(focus.play, it) } }
        Text(modeTitle(focus.play), color = Color.White, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 23.sp,
            modifier = Modifier.padding(top = 6.dp))
        Text(stringResource(when (focus.play) {
            MultiplayerRoom.Play.ROM -> R.string.mp_play_rom_desc
            MultiplayerRoom.Play.DOWNLOAD -> R.string.mp_play_download_desc
            MultiplayerRoom.Play.CHAT -> R.string.mp_play_chat_desc
        }), color = Color.White.copy(alpha = 0.6f), fontSize = 13.5.sp, lineHeight = 20.sp, textAlign = TextAlign.Center,
            modifier = Modifier.widthIn(max = 420.dp))
        ExternalMeta(stringResource(when {
            focus.locked -> R.string.mp_meta_chat_locked
            focus.play == MultiplayerRoom.Play.ROM -> R.string.mp_meta_rom
            focus.play == MultiplayerRoom.Play.DOWNLOAD -> R.string.mp_meta_download
            else -> R.string.mp_meta_chat
        }).uppercase(), color = if (focus.locked) colors.goldText else colors.appText)
        ExternalCrumb("${stringResource(R.string.multiplayer_title)} › ${stringResource(R.string.mp_create_title)}")
    }
}

@Composable
private fun CodeBoxes(code: String, length: Int, width: Dp, height: Dp, size: TextUnit, cursor: Boolean) {
    val colors = watermelon
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        (0 until length).forEach { index ->
            val char = code.getOrNull(index)
            val active = cursor && index == code.length
            Box(
                Modifier.size(width, height).clip(RoundedCornerShape(12.dp))
                    .background(if (char != null) colors.surface2 else Color.White.copy(alpha = 0.04f))
                    .border(if (active) 2.dp else 1.dp, if (active) colors.red else Color.White.copy(alpha = 0.16f), RoundedCornerShape(12.dp)),
                contentAlignment = Alignment.Center,
            ) {
                Text(char?.toString().orEmpty(), color = Color.White, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = size)
            }
        }
    }
}

@Composable
private fun ExternalCode(viewModel: MultiplayerViewModel, code: String) {
    val nearby by viewModel.nearby.collectAsState()
    val matches = if (code.isEmpty()) emptyList() else nearby.filter { it.code.startsWith(code, ignoreCase = true) }
    Column(Modifier.fillMaxSize().padding(horizontal = 50.dp, vertical = 30.dp), horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(14.dp, Alignment.CenterVertically)) {
        CodeBoxes(code, 4, 58.dp, 74.dp, 40.sp, cursor = code.length < 4)
        val room = matches.singleOrNull()
        when {
            room != null -> Text(listOf(stringResource(R.string.mp_room_of, room.host.nickname), roomTitle(room),
                stringResource(R.string.mp_players_of, room.players, room.maximum)).filter { it.isNotEmpty() }.joinToString(" · "),
                color = Color.White, fontSize = 14.sp, textAlign = TextAlign.Center)
            code.length == 4 -> Text(stringResource(R.string.mp_code_missing), color = watermelon.goldText, fontSize = 14.sp, textAlign = TextAlign.Center)
            else -> Text(stringResource(R.string.mp_code_hint), color = Color.White.copy(alpha = 0.6f), fontSize = 13.5.sp, textAlign = TextAlign.Center)
        }
        ExternalCrumb("${stringResource(R.string.multiplayer_title)} › ${stringResource(R.string.mp_enter_code)}")
    }
}

@Composable
private fun ExternalLobby(viewModel: MultiplayerViewModel) {
    val colors = watermelon
    val room by viewModel.room.collectAsState()
    val status by viewModel.status.collectAsState()
    val info = room?.info
    val seats = remember(status, room) { viewModel.players(status) }.filter { it.status == 1 || it.status == 2 }
    val host = room?.role == MultiplayerRoom.Role.HOST
    Column(Modifier.fillMaxSize().padding(horizontal = 40.dp, vertical = 24.dp), horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(14.dp, Alignment.CenterVertically)) {
        val code = info?.code.orEmpty()
        if (code.isNotEmpty()) {
            ExternalMeta("${stringResource(R.string.mp_code_label)} ${stringResource(R.string.mp_code_for)}".uppercase(),
                color = Color.White.copy(alpha = 0.5f))
            CodeBoxes(code, code.length, 84.dp, 108.dp, 62.sp, cursor = false)
        }
        ExternalMeta(listOfNotNull(info?.takeIf { it.direct }?.network?.takeIf { it.isNotEmpty() },
            info?.game?.title?.takeIf { it.isNotEmpty() } ?: info?.play?.let { modeTitle(it) },
            stringResource(R.string.mp_players_of, seats.size, info?.maximum ?: 16)).joinToString(" · "))
        Row(horizontalArrangement = Arrangement.spacedBy(18.dp)) {
            seats.forEach { seat ->
                Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Box(Modifier.border(3.dp, if (seat.host) colors.red else colors.green, CircleShape).padding(5.dp)) {
                        MultiplayerAvatar(seat.profile, 40.dp)
                    }
                    ExternalMeta(seat.profile.nickname.uppercase(), color = Color.White.copy(alpha = 0.75f), size = 9.5.sp)
                }
            }
        }
        if (host) {
            if (seats.size > 1) ExternalHints("START" to stringResource(R.string.mp_start, seats.size), "Y" to stringResource(R.string.mp_hint_details))
            else ExternalHints("Y" to stringResource(R.string.mp_hint_details))
        }
    }
}

@Composable
private fun ExternalConnecting(viewModel: MultiplayerViewModel) {
    val colors = watermelon
    val target by viewModel.joining.collectAsState()
    val step by viewModel.joinStep.collectAsState()
    val profile by viewModel.profile.collectAsState()
    val library by viewModel.roms.collectAsState()
    val room = target ?: return ExternalProfile(profile)
    val steps = listOf(
        stringResource(R.string.mp_found) to 0,
        stringResource(R.string.mp_joining_network, room.host.nickname) to 1,
        stringResource(R.string.mp_entering_room) to 2,
    )
    val position = steps.indexOfFirst { it.second >= step }.let { if (it < 0) steps.size else it }
    ExternalShowcase(rom = matchRom(library.asSequence(), room.game), title = roomTitle(room)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            MultiplayerAvatar(profile, 44.dp)
            (0 until 4).forEach { Box(Modifier.size(8.dp).clip(CircleShape).background(colors.green.copy(alpha = 1f - it * 0.25f))) }
            MultiplayerAvatar(room.host, 44.dp)
        }
        steps.forEachIndexed { index, (text, _) ->
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                val done = index < position
                val active = index == position
                Box(Modifier.size(20.dp).clip(CircleShape)
                    .background(if (done) colors.green else Color.Transparent)
                    .border(if (done) 0.dp else 2.dp, if (active) colors.red else Color.White.copy(alpha = 0.25f), CircleShape))
                Text(text, color = Color.White.copy(alpha = if (done || active) 1f else 0.5f), fontFamily = SpaceGrotesk,
                    fontWeight = FontWeight.SemiBold, fontSize = 15.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        ExternalMeta(listOf(stringResource(R.string.mp_step_of, (position + 1).coerceAtMost(steps.size), steps.size),
            stringResource(R.string.mp_joining_network_sub)).joinToString(" · ").uppercase())
    }
}

@Composable
private fun ExternalProfile(profile: MultiplayerProfile) {
    val colors = watermelon
    val months = stringArrayResource(R.array.mp_months)
    val colours = stringArrayResource(R.array.mp_colour_names)
    val symbols = stringArrayResource(R.array.mp_symbol_names)
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Box(Modifier.width(420.dp).clip(RoundedCornerShape(16.dp)).background(colors.surface)
            .border(1.dp, Color.White.copy(alpha = 0.13f), RoundedCornerShape(16.dp))) {
            Column {
                Box(Modifier.fillMaxWidth().height(70.dp).background(DsPalette[profile.colour.coerceIn(0, 15)]))
                Column(Modifier.padding(start = 140.dp, end = 22.dp, top = 10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(profile.nickname, color = Color.White, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 26.sp, maxLines = 1)
                    ExternalMeta(listOfNotNull(profile.message.takeIf { it.isNotBlank() }?.let { "“$it”" },
                        months.getOrNull(profile.birthdayMonth - 1)?.let { stringResource(R.string.mp_birthday_value, profile.birthdayDay, it) })
                        .joinToString(" · "), size = 10.5.sp)
                }
                Row(Modifier.fillMaxWidth().padding(start = 22.dp, end = 22.dp, top = 22.dp, bottom = 18.dp), verticalAlignment = Alignment.CenterVertically) {
                    ExternalMeta(listOfNotNull(colours.getOrNull(profile.colour), symbols.getOrNull(MultiplayerProfile.SYMBOLS.indexOf(profile.symbol))
                        ?.takeIf { it.isNotBlank() }).joinToString(" · ").uppercase(), size = 10.sp)
                    Spacer(Modifier.weight(1f))
                    ExternalCrumb("${stringResource(R.string.multiplayer_title)} › ${stringResource(R.string.mp_profile_title)}")
                }
            }
            Box(Modifier.padding(start = 22.dp, top = 24.dp).size(104.dp).clip(CircleShape).background(colors.surface),
                contentAlignment = Alignment.Center) {
                MultiplayerAvatar(profile, 96.dp)
            }
        }
    }
}
