package me.magnum.melonds.multiplayer

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.Icon
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.runtime.getValue
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.PathOperation
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.romlist.composables.WatermelonRomArt
import me.magnum.melonds.ui.romlist.composables.romDisplayName
import me.magnum.melonds.ui.romlist.composables.romGradient
import me.magnum.melonds.ui.romlist.composables.romInitials
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.WatermelonColors
import me.magnum.melonds.ui.theme.WatermelonMono
import me.magnum.melonds.ui.theme.watermelon

val DsPalette = listOf(
    0xFF61829A, 0xFFBA4900, 0xFFFB0018, 0xFFFB8AFB, 0xFFFB9200, 0xFFF3E300, 0xFFAAFB00, 0xFF00FB00,
    0xFF00A238, 0xFF49DB8A, 0xFF30BAF3, 0xFF0059F3, 0xFF000092, 0xFF8A00D3, 0xFFD300EB, 0xFFFB0092,
).map { Color(it) }

private fun circle(cx: Float, cy: Float, r: Float): String =
    "M${cx - r} ${cy}a$r $r 0 1 0 ${2 * r} 0a$r $r 0 1 0 ${-2 * r} 0z"

private val symbolPaths: Map<String, Path> by lazy {
    mapOf(
        "star" to "M12 2.5l2.9 6 6.6.9-4.8 4.6 1.2 6.5L12 17.4l-5.9 3.1 1.2-6.5L2.5 9.4l6.6-.9z",
        "heart" to "M12 21s-7.5-4.6-9.5-9.2C1.2 8.6 3.2 5 6.6 5c2.1 0 3.6 1.2 4.4 2.6C11.8 6.2 13.3 5 15.4 5c3.4 0 5.4 3.6 4.1 6.8C19.5 16.4 12 21 12 21z",
        "note" to "M9 18.5a3 3 0 1 1-2-2.8V5l11-2v12.5a3 3 0 1 1-2-2.8V7.2L9 8.5z",
        "bolt" to "M13.5 2L4.5 13.5h6L9.5 22l9-11.5h-6z",
        "moon" to "M20 14.5A8.5 8.5 0 1 1 9.5 4a7 7 0 0 0 10.5 10.5z",
        "crown" to "M3 7l4.5 4L12 4l4.5 7L21 7l-2 12H5z",
        "smile" to circle(12f, 12f, 9.5f) + circle(8.8f, 9.6f, 1.6f) + circle(15.2f, 9.6f, 1.6f) + "M7.6 13.4h8.8a4.4 4.4 0 0 1-8.8 0z",
        "flower" to circle(12f, 6.3f, 3.6f) + circle(17.4f, 10.2f, 3.6f) + circle(15.4f, 16.6f, 3.6f) +
            circle(8.6f, 16.6f, 3.6f) + circle(6.6f, 10.2f, 3.6f) + circle(12f, 12f, 2.2f),
        "leaf" to "M20 4C10 4 4 9 4 16c0 1.4.3 2.7.8 4C6 14 10 10 16 8c-5 3-8 7-9.5 12C14 20 20 14 20 4z",
        "diamond" to "M6 3h12l4 6-10 12L2 9z",
        "pad" to "M7 7h10a5 5 0 0 1 4.9 6l-.8 4a2.5 2.5 0 0 1-4.3 1.2L14.5 16h-5l-2.3 2.2A2.5 2.5 0 0 1 2.9 17l-.8-4A5 5 0 0 1 7 7z" +
            circle(16.3f, 10.6f, 1.1f) + circle(18.3f, 12.8f, 1.1f),
    ).mapValues { (_, value) -> PathParser().parsePathString(value).toPath().apply { fillType = PathFillType.EvenOdd } }
}

@Composable
fun MultiplayerAvatar(profile: MultiplayerProfile, size: Dp, modifier: Modifier = Modifier, symbol: String = profile.symbol) {
    val background = DsPalette[profile.colour.coerceIn(0, 15)]
    val ink = if (background.luminance() > 0.3f) Color(0xFF141317) else Color.White
    val path = symbolPaths[symbol]
    Box(modifier.size(size), contentAlignment = Alignment.Center) {
        Canvas(Modifier.size(size)) {
            val bounds = androidx.compose.ui.geometry.Rect(Offset.Zero, this.size)
            val outline = Path().apply { addOval(bounds) }
            val shade = (this.size.height * 0.05f).coerceAtLeast(2f)
            val light = (this.size.height * 0.03f).coerceAtLeast(1f)
            fun crescent(shift: Float) = Path.combine(PathOperation.Difference, outline,
                Path().apply { addOval(bounds.translate(0f, shift)) })
            drawPath(outline, background)
            drawPath(crescent(-shade), Color.Black.copy(alpha = 0.18f))
            drawPath(crescent(light), Color.White.copy(alpha = 0.28f))
            if (path != null) {
                val glyph = this.size.minDimension * 0.56f
                translate((this.size.width - glyph) / 2, (this.size.height - glyph) / 2) {
                    scale(glyph / 24f, glyph / 24f, pivot = Offset.Zero) {
                        drawPath(path, ink)
                        if (symbol == "pad") {
                            drawLine(background, Offset(6.2f, 10.5f), Offset(6.2f, 13.5f), 1.6f, StrokeCap.Round)
                            drawLine(background, Offset(4.7f, 12f), Offset(7.7f, 12f), 1.6f, StrokeCap.Round)
                        }
                    }
                }
            }
        }
        if (path == null) {
            Text(
                text = profile.nickname.trim().take(1).uppercase().ifEmpty { "?" },
                color = ink,
                fontFamily = SpaceGrotesk,
                fontWeight = FontWeight.Bold,
                fontSize = (size.value * 0.46f).sp,
                lineHeight = (size.value * 0.46f).sp,
            )
        }
    }
}

val LocalMultiplayerInteractive = compositionLocalOf { true }

@Composable
fun Pressable(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    shape: Shape = RoundedCornerShape(10.dp),
    background: Color = watermelon.surface,
    selected: Boolean = false,
    enabled: Boolean = true,
    border: Color? = watermelon.line,
    dashed: Boolean = false,
    label: String? = null,
    content: @Composable BoxScope.() -> Unit,
) {
    val colors = watermelon
    val active = enabled && LocalMultiplayerInteractive.current
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val ring = when {
        focused -> if (background == colors.red) Color.White else colors.red
        selected -> colors.red
        else -> null
    }
    Box(
        modifier = modifier
            .clip(shape)
            .background(background)
            .let { base ->
                when {
                    ring != null -> base.border(if (focused) 3.dp else 2.dp, ring, shape)
                    border != null -> base.border(1.dp, if (dashed) colors.text3 else border, shape)
                    else -> base
                }
            }
            .focusProperties { canFocus = active }
            .clickable(interactionSource = interaction, indication = null, enabled = active, onClick = onClick)
            .let { if (label != null) it.semantics { contentDescription = label } else it },
        content = content,
    )
}

fun tileColor(colors: WatermelonColors, focused: Boolean, selected: Boolean): Color =
    colors.text.copy(alpha = if (focused) 0.15f else if (selected) 0.10f else 0.06f)

@Composable
fun MenuTile(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    selected: Boolean = false,
    enabled: Boolean = true,
    dim: Boolean = false,
    label: String? = null,
    content: @Composable RowScope.() -> Unit,
) {
    val colors = watermelon
    val active = enabled && LocalMultiplayerInteractive.current
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val shape = RoundedCornerShape(10.dp)
    Row(
        modifier = modifier
            .clip(shape)
            .background(tileColor(colors, focused, selected))
            .border(
                if (focused) 2.dp else 1.dp,
                when {
                    focused -> colors.red
                    selected -> colors.red.copy(alpha = 0.6f)
                    else -> Color.Transparent
                },
                shape,
            )
            .focusProperties { canFocus = active }
            .clickable(interactionSource = interaction, indication = null, enabled = active, onClick = onClick)
            .let { if (label != null) it.semantics { contentDescription = label } else it }
            .alpha(if (dim) 0.6f else 1f)
            .padding(start = 11.dp, end = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.width(3.dp).height(20.dp).clip(RoundedCornerShape(2.dp)).background(if (selected || focused) colors.red else Color.Transparent))
        Spacer(Modifier.width(10.dp))
        content()
    }
}

@Composable
fun Mono(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = watermelon.text3,
    size: TextUnit = 10.5.sp,
    spacing: TextUnit = 1.5.sp,
    weight: FontWeight = FontWeight.Medium,
    lines: Int = 1,
) {
    Text(text, modifier, color = color, fontFamily = WatermelonMono, fontSize = size, letterSpacing = spacing, fontWeight = weight,
        maxLines = lines, overflow = TextOverflow.Ellipsis, lineHeight = size * 1.35f)
}

@Composable
fun TileTitle(text: String, modifier: Modifier = Modifier, color: Color = watermelon.text, size: TextUnit = 16.sp) {
    Text(text, modifier, color = color, fontFamily = SpaceGrotesk, fontWeight = FontWeight.SemiBold, fontSize = size, maxLines = 1,
        overflow = TextOverflow.Ellipsis)
}

fun Modifier.panel(colors: WatermelonColors, radius: Dp = 14.dp): Modifier {
    val shape = RoundedCornerShape(radius)
    return clip(shape).background(if (colors.isDark) Color(0xFF1A191F) else colors.surface2).border(1.dp, colors.line, shape)
}

@Composable
fun PillButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    key: String? = null,
    height: Dp = 40.dp,
    fontSize: TextUnit = 16.sp,
) {
    val colors = watermelon
    val active = enabled
    Pressable(
        onClick = onClick,
        modifier = modifier.height(height),
        shape = RoundedCornerShape(height / 2),
        background = if (active) colors.red else colors.surface2,
        enabled = enabled,
        border = null,
    ) {
        Row(
            Modifier.align(Alignment.Center).padding(start = if (key != null) 7.dp else 16.dp, end = 16.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (key != null) KeyTag(key, if (active) Color.White else colors.text2)
            Text(
                text,
                color = if (active) Color.White else colors.text2,
                fontFamily = SpaceGrotesk,
                fontWeight = FontWeight.Bold,
                fontSize = fontSize,
                maxLines = 1,
            )
        }
    }
}

@Composable
fun KeyTag(key: String, ink: Color = Color.White) {
    val wide = key.length > 1
    Box(
        Modifier
            .height(if (wide) 24.dp else 22.dp)
            .sizeIn(minWidth = 22.dp)
            .clip(if (wide) RoundedCornerShape(12.dp) else CircleShape)
            .background(ink.copy(alpha = 0.2f))
            .padding(horizontal = if (wide) 7.dp else 0.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(key, color = ink, fontFamily = WatermelonMono, fontSize = 10.sp, fontWeight = FontWeight.Medium)
    }
}

@Composable
fun KeyCap(key: String, color: Color = watermelon.text3) {
    val wide = key.length > 1
    Box(
        Modifier
            .height(20.dp)
            .sizeIn(minWidth = 20.dp)
            .border(1.5.dp, color, if (wide) RoundedCornerShape(10.dp) else CircleShape)
            .padding(horizontal = if (wide) 6.dp else 0.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(key, color = color, fontFamily = WatermelonMono, fontSize = 10.sp, fontWeight = FontWeight.Medium)
    }
}

@Composable
fun Hint(key: String, text: String) {
    val colors = watermelon
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(7.dp)) {
        KeyCap(key)
        Text(text, color = colors.text3, fontFamily = WatermelonMono, fontSize = 12.sp, maxLines = 1)
    }
}

@Composable
fun MultiplayerFooter(centered: Boolean = false, content: @Composable RowScope.() -> Unit) {
    val colors = watermelon
    Box(Modifier.fillMaxWidth().height(1.dp).background(colors.line))
    Row(
        Modifier.fillMaxWidth().height(56.dp).padding(start = 18.dp, end = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = if (centered) Arrangement.spacedBy(22.dp, Alignment.CenterHorizontally) else Arrangement.spacedBy(18.dp),
        content = content,
    )
}

@Composable
fun MultiplayerHeader(
    title: String,
    onBack: (() -> Unit)?,
    modifier: Modifier = Modifier,
    tags: @Composable RowScope.() -> Unit = {},
    trailing: @Composable RowScope.() -> Unit = {},
) {
    MultiplayerHeader(AnnotatedString(title), onBack, modifier, tags = tags, trailing = trailing)
}

@Composable
fun MultiplayerHeader(
    title: AnnotatedString,
    onBack: (() -> Unit)?,
    modifier: Modifier = Modifier,
    leading: @Composable RowScope.() -> Unit = {},
    tags: @Composable RowScope.() -> Unit = {},
    trailing: @Composable RowScope.() -> Unit = {},
) {
    val colors = watermelon
    Column(modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().height(56.dp).padding(start = if (onBack != null) 8.dp else 18.dp, end = 16.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (onBack != null) {
                Pressable(onClick = onBack, modifier = Modifier.size(44.dp), shape = CircleShape, background = Color.Transparent,
                    border = null, label = stringResource(R.string.multiplayer_back)) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack, null, tint = colors.text2, modifier = Modifier.align(Alignment.Center).size(22.dp))
                }
            }
            leading()
            Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(title, color = colors.text, fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold, fontSize = 21.sp, letterSpacing = (-0.3).sp,
                    maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                tags()
            }
            trailing()
        }
        Box(Modifier.fillMaxWidth().height(1.dp).background(colors.line))
    }
}

enum class TagKind { GOOD, HOST, NEUTRAL, BAD, APP }

@Composable
fun Tag(text: String, kind: TagKind) {
    val colors = watermelon
    val (background, ink) = when (kind) {
        TagKind.GOOD -> colors.green.copy(alpha = 0.18f) to if (colors.isDark) Color(0xFF8FD46A) else colors.green
        TagKind.HOST -> colors.red.copy(alpha = 0.18f) to if (colors.isDark) Color(0xFFFF8A80) else colors.red
        TagKind.NEUTRAL -> colors.surface2 to colors.text2
        TagKind.BAD -> Color.Transparent to if (colors.isDark) Color(0xFFFF8A80) else colors.red
        TagKind.APP -> Color(0xFF30BAF3).copy(alpha = 0.18f) to Color(0xFF7FD3F7)
    }
    val shape = RoundedCornerShape(6.dp)
    Box(
        Modifier
            .clip(shape)
            .background(background)
            .let { if (kind == TagKind.BAD) it.border(1.dp, colors.red.copy(alpha = 0.5f), shape) else it }
            .padding(horizontal = 8.dp, vertical = 4.dp),
    ) {
        Mono(text, color = ink, size = 10.5.sp, spacing = 1.sp)
    }
}

@Composable
fun RowScope.Grow() = Spacer(Modifier.weight(1f))

val WatermelonColors.gold: Color get() = WatermelonColors.gold
val WatermelonColors.goldText: Color get() = if (isDark) WatermelonColors.favoriteStar else WatermelonColors.gold
val WatermelonColors.goodText: Color get() = if (isDark) Color(0xFF8FD46A) else green
val WatermelonColors.appText: Color get() = if (isDark) Color(0xFF7FD3F7) else Color(0xFF1C7FAD)

@Composable
fun AppBadge(app: String) {
    if (app.isNotEmpty() && app != MultiplayerNearby.OWN_APP) Tag(app.uppercase(), TagKind.APP)
}

fun matchRom(roms: Sequence<Rom>, title: String): Rom? =
    title.takeIf { it.isNotEmpty() }?.let { name -> roms.firstOrNull { romDisplayName(it).equals(name, true) || it.name.equals(name, true) } }

class GameCovers(val urls: Map<String, String>, val request: (Rom) -> Unit)

val LocalGameCovers = compositionLocalOf { GameCovers(emptyMap()) {} }

@Composable
fun GameArt(rom: Rom?, title: String, modifier: Modifier, initials: TextUnit = 22.sp) {
    if (rom != null) {
        val covers = LocalGameCovers.current
        LaunchedEffect(rom.uri) { covers.request(rom) }
        WatermelonRomArt(rom, boxArtUrl = covers.urls[rom.uri.toString()]?.takeIf { it.isNotEmpty() }, raCoverUrl = null,
            modifier = modifier, initialsFontSize = initials)
    } else {
        Box(modifier.background(romGradient(title)), contentAlignment = Alignment.Center) {
            Text(romInitials(title.ifEmpty { "DS" }), color = Color.White.copy(alpha = 0.18f), fontFamily = SpaceGrotesk, fontWeight = FontWeight.Bold,
                fontSize = initials)
        }
    }
}
