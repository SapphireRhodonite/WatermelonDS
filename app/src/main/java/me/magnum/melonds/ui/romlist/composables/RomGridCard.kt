package me.magnum.melonds.ui.romlist.composables

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.ui.draw.scale
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.Icon
import androidx.compose.material.MaterialTheme
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.EmojiEvents
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Star
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.theme.WatermelonColors
import me.magnum.melonds.ui.theme.WatermelonMono
import me.magnum.melonds.ui.theme.watermelon
import kotlin.time.Duration

fun Modifier.watermelonFocusRing(focused: Boolean, red: Color, shape: RoundedCornerShape): Modifier {
    return if (focused) {
        this
            .padding(0.dp)
            .border(3.dp, red, shape)
    } else {
        this
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun RomGridCard(
    rom: Rom,
    coverUrl: String?,
    boxArtUrl: String? = null,
    boxArtLoading: Boolean = false,
    useCoverArt: Boolean = true,
    showAchievementBadge: Boolean,
    onClick: () -> Unit,
    onLongPress: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = watermelon
    val shape = RoundedCornerShape(7.dp)
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()
    val isPressed by interactionSource.collectIsPressedAsState()
    val pressScale by animateFloatAsState(
        targetValue = if (isPressed) 0.93f else 1f,
        animationSpec = spring(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = 4000f),
        label = "press",
    )
    var artLoaded by remember(rom.uri, rom.config.iconSource, boxArtUrl, coverUrl, useCoverArt) { mutableStateOf(false) }

    Box(
        modifier = modifier
            .scale(pressScale)
            .fillMaxWidth()
            .aspectRatio(DsBoxArtAspectRatio)
            .shadow(5.dp, shape)
            .clip(shape)
            .combinedClickable(
                interactionSource = interactionSource,
                indication = null,
                onClick = onClick,
                onLongClick = onLongPress,
            ),
    ) {
        WatermelonRomArt(
            rom = rom,
            boxArtUrl = boxArtUrl,
            raCoverUrl = coverUrl,
            useCoverArt = useCoverArt,
            initialsFontSize = 44.sp,
            boxArtLoading = boxArtLoading,
            modifier = Modifier.aspectRatio(DsBoxArtAspectRatio).fillMaxWidth(),
            onArtLoadedChanged = { artLoaded = it },
        )

        Box(Modifier.aspectRatio(DsBoxArtAspectRatio).fillMaxWidth().border(1.dp, Color.White.copy(alpha = 0.13f), shape))

        Row(
            modifier = Modifier.align(Alignment.TopStart).padding(7.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            PlatformBadge(text = romPlatformLabel(rom))
            if (showAchievementBadge) {
                Spacer(Modifier.width(4.dp))
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(4.dp))
                        .background(Color.Black.copy(alpha = 0.35f))
                        .padding(horizontal = 4.dp, vertical = 2.dp),
                ) {
                    Icon(
                        imageVector = Icons.Filled.EmojiEvents,
                        contentDescription = null,
                        tint = WatermelonColors.gold,
                        modifier = Modifier.size(10.dp),
                    )
                }
            }
        }

        if (rom.isFavorite) {
            Icon(
                imageVector = Icons.Filled.Star,
                contentDescription = null,
                tint = WatermelonColors.favoriteStar,
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(top = 6.dp, end = 7.dp)
                    .size(13.dp),
            )
        }

        Row(
            modifier = Modifier
                .align(Alignment.BottomStart)
                .fillMaxWidth()
                .background(Brush.verticalGradient(listOf(Color.Transparent, Color.Black.copy(alpha = 0.8f))))
                .padding(start = 9.dp, end = 9.dp, top = 22.dp, bottom = 9.dp),
            verticalAlignment = Alignment.Bottom,
        ) {
            if (artLoaded) {
                RomMiniIcon(rom = rom, raCoverUrl = coverUrl)
                Spacer(Modifier.width(5.dp))
            }
            Column(
                modifier = Modifier.weight(1f),
            ) {
                Text(
                    text = romDisplayName(rom),
                    color = Color.White,
                    fontWeight = FontWeight.Bold,
                    fontSize = 11.5.sp,
                    lineHeight = 14.5.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                val hours = formatHoursLabel(rom.totalPlayTime)
                if (hours.isNotEmpty()) {
                    Text(
                        text = hours,
                        color = Color.White.copy(alpha = 0.65f),
                        fontFamily = WatermelonMono,
                        fontSize = 8.5.sp,
                        lineHeight = 10.sp,
                        modifier = Modifier.padding(top = 3.dp),
                    )
                }
            }
        }

        if (isFocused) {
            Box(Modifier.aspectRatio(DsBoxArtAspectRatio).fillMaxWidth().border(3.dp, colors.red, shape))
        }
    }
}

@Composable
fun FolderGridCard(
    name: String,
    relativePath: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    gameCount: Int? = null,
) {
    val colors = watermelon
    val shape = RoundedCornerShape(8.dp)
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()

    Row(
        modifier = modifier
            .clip(shape)
            .background(colors.surface)
            .border(1.dp, if (isFocused) colors.red else colors.line, shape)
            .clickable(interactionSource = interactionSource, indication = null, onClick = onClick)
            .padding(start = 11.dp, end = 14.dp, top = 10.dp, bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            imageVector = Icons.Filled.Folder,
            contentDescription = null,
            tint = colors.green,
            modifier = Modifier.size(22.dp),
        )
        Spacer(Modifier.width(9.dp))
        Column {
            Text(
                text = name,
                color = colors.text,
                fontSize = 13.sp,
                lineHeight = 16.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            val sub = when {
                gameCount != null -> gameCount.toString()
                relativePath.isNotEmpty() && relativePath != name -> relativePath
                else -> null
            }
            if (sub != null) {
                Text(
                    text = sub,
                    color = colors.text3,
                    fontFamily = WatermelonMono,
                    fontSize = 9.sp,
                    lineHeight = 11.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}
