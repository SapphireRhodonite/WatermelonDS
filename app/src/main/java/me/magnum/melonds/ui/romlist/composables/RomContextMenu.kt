package me.magnum.melonds.ui.romlist.composables

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.Icon
import androidx.compose.material.Surface
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FileDownload
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.outlined.StarOutline
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.WatermelonColors
import me.magnum.melonds.ui.theme.WatermelonMono
import me.magnum.melonds.ui.theme.watermelon
import kotlin.time.Duration

@Composable
fun RomContextMenu(
    rom: Rom?,
    onDismiss: () -> Unit,
    onToggleFavorite: (Rom) -> Unit,
    onShowDetails: (Rom) -> Unit,
    onSendSaveFile: (Rom) -> Unit,
    onPlayWithClone: (Rom) -> Unit,
    onImportSaveFile: (Rom) -> Unit,
    raCoverUrl: String? = null,
    useCoverArt: Boolean = true,
) {
    if (rom == null) return
    val colors = watermelon
    Dialog(onDismissRequest = onDismiss) {
        androidx.compose.runtime.CompositionLocalProvider(androidx.compose.material.LocalElevationOverlay provides null) {
        Surface(
            shape = RoundedCornerShape(17.dp),
            color = colors.surface,
            elevation = 22.dp,
            modifier = Modifier.widthIn(max = 320.dp),
        ) {
            Column(modifier = Modifier.padding(vertical = 8.dp)) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 10.dp),
                ) {
                    Box(
                        modifier = Modifier
                            .size(38.dp)
                            .clip(RoundedCornerShape(9.dp)),
                    ) {
                        WatermelonRomArt(
                            rom = rom,
                            boxArtUrl = null,
                            raCoverUrl = raCoverUrl,
                            useCoverArt = useCoverArt,
                            initialsFontSize = 15.sp,
                            modifier = Modifier.size(38.dp),
                        )
                    }
                    Spacer(Modifier.width(11.dp))
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            text = romDisplayName(rom),
                            color = colors.text,
                            fontFamily = SpaceGrotesk,
                            fontSize = 15.sp,
                            lineHeight = 18.sp,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                        )
                        val hours = formatHoursLabel(rom.totalPlayTime)
                        Text(
                            text = if (hours.isNotEmpty()) "${romPlatformLabel(rom)} · $hours" else romPlatformLabel(rom),
                            color = colors.text3,
                            fontFamily = WatermelonMono,
                            fontSize = 9.5.sp,
                            modifier = Modifier.padding(top = 2.dp),
                        )
                    }
                }
                Box(Modifier.fillMaxWidth().height(1.dp).background(colors.line))
                Spacer(Modifier.height(6.dp))
                ContextItem(
                    icon = if (rom.isFavorite) Icons.Filled.Star else Icons.Outlined.StarOutline,
                    iconTint = if (rom.isFavorite) WatermelonColors.favoriteStar else colors.text2,
                    label = stringResource(
                        if (rom.isFavorite) R.string.rom_action_unfavorite
                        else R.string.rom_action_favorite,
                    ),
                    onClick = {
                        onToggleFavorite(rom)
                    },
                )
                ContextItem(
                    icon = Icons.Filled.Info,
                    iconTint = colors.text2,
                    label = stringResource(R.string.rom_action_details),
                    onClick = {
                        onShowDetails(rom)
                        onDismiss()
                    },
                )
                ContextItem(
                    icon = Icons.Filled.Share,
                    iconTint = colors.text2,
                    label = stringResource(R.string.phantom_title),
                    onClick = {
                        onPlayWithClone(rom)
                        onDismiss()
                    },
                )
                ContextItem(
                    icon = Icons.Filled.Share,
                    iconTint = colors.text2,
                    label = stringResource(R.string.rom_action_send_save_file),
                    onClick = {
                        onSendSaveFile(rom)
                        onDismiss()
                    },
                )
                ContextItem(
                    icon = Icons.Filled.FileDownload,
                    iconTint = colors.text2,
                    label = stringResource(R.string.rom_action_import_save_file),
                    onClick = {
                        onImportSaveFile(rom)
                        onDismiss()
                    },
                )
            }
        }
        }
    }
}

@Composable
private fun ContextItem(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    iconTint: androidx.compose.ui.graphics.Color,
    label: String,
    onClick: () -> Unit,
) {
    val colors = watermelon
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 18.dp, vertical = 12.dp),
    ) {
        Icon(imageVector = icon, contentDescription = null, tint = iconTint, modifier = Modifier.size(20.dp))
        Spacer(Modifier.width(14.dp))
        Text(
            text = label,
            color = colors.text,
            fontSize = 14.sp,
            fontWeight = FontWeight.Medium,
        )
    }
}
