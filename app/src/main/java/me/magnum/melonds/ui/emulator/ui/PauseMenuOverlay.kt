package me.magnum.melonds.ui.emulator.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.Icon
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Logout
import androidx.compose.material.icons.filled.BugReport
import androidx.compose.material.icons.filled.CloudSync
import androidx.compose.material.icons.filled.Code
import androidx.compose.material.icons.filled.EmojiEvents
import androidx.compose.material.icons.filled.FastRewind
import androidx.compose.material.icons.filled.FileDownload
import androidx.compose.material.icons.filled.Monitor
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.RestartAlt
import androidx.compose.material.icons.filled.Save
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SportsEsports
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.VideogameAsset
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import coil.request.ImageRequest
import androidx.compose.ui.platform.LocalContext
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.common.GamepadHint
import me.magnum.melonds.ui.common.GamepadHintsFooter
import me.magnum.melonds.ui.emulator.PauseMenuOption
import me.magnum.melonds.ui.emulator.firmware.FirmwarePauseMenuOption
import me.magnum.melonds.ui.emulator.model.PauseMenu
import me.magnum.melonds.ui.emulator.rom.RomPauseMenuOption
import me.magnum.melonds.ui.romlist.composables.formatHoursLabel
import me.magnum.melonds.ui.theme.SpaceGrotesk
import me.magnum.melonds.ui.theme.WatermelonColors
import me.magnum.melonds.ui.theme.WatermelonMono
import me.magnum.melonds.ui.theme.watermelon
import kotlin.time.Duration

private val PauseScrim = Color(0xE008070A)

private fun optionIcon(option: PauseMenuOption): ImageVector {
    return when (option) {
        is RomPauseMenuOption -> when (option) {
            RomPauseMenuOption.SETTINGS -> Icons.Filled.Settings
            RomPauseMenuOption.ROM_SETTINGS -> Icons.Filled.Tune
            RomPauseMenuOption.SAVE_STATE -> Icons.Filled.Save
            RomPauseMenuOption.LOAD_STATE -> Icons.Filled.FileDownload
            RomPauseMenuOption.REWIND -> Icons.Filled.FastRewind
            RomPauseMenuOption.CHEATS -> Icons.Filled.Code
            RomPauseMenuOption.VIEW_ACHIEVEMENTS -> Icons.Filled.EmojiEvents
            RomPauseMenuOption.SYNC_RETRO_ACHIEVEMENTS -> Icons.Filled.CloudSync
            RomPauseMenuOption.PRESETS -> Icons.Filled.Monitor
            RomPauseMenuOption.RENDERER_DEBUG -> Icons.Filled.BugReport
            RomPauseMenuOption.RESET -> Icons.Filled.RestartAlt
            RomPauseMenuOption.EXIT -> Icons.AutoMirrored.Filled.Logout
        }
        is FirmwarePauseMenuOption -> when (option) {
            FirmwarePauseMenuOption.SETTINGS -> Icons.Filled.Settings
            FirmwarePauseMenuOption.RESET -> Icons.Filled.RestartAlt
            FirmwarePauseMenuOption.EXIT -> Icons.AutoMirrored.Filled.Logout
        }
        else -> Icons.Filled.Settings
    }
}

private fun isDestructive(option: PauseMenuOption): Boolean {
    return option == RomPauseMenuOption.RESET || option == RomPauseMenuOption.EXIT ||
        option == FirmwarePauseMenuOption.RESET || option == FirmwarePauseMenuOption.EXIT
}

private fun needsConfirmation(option: PauseMenuOption): Boolean = isDestructive(option)

@Composable
fun PauseMenuOverlay(
    pauseMenu: PauseMenu,
    rom: Rom?,
    achievementsSummary: String?,
    onOptionSelected: (PauseMenuOption) -> Unit,
    onResume: () -> Unit,
) {
    val colors = watermelon
    var confirmingOption by remember { mutableStateOf<PauseMenuOption?>(null) }
    val resumeFocusRequester = remember { FocusRequester() }
    val configuration = LocalConfiguration.current
    val isLandscape = configuration.screenWidthDp > configuration.screenHeightDp
    val context = LocalContext.current

    BackHandler {
        if (confirmingOption != null) {
            confirmingOption = null
        } else {
            onResume()
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(PauseScrim)
            .focusProperties { canFocus = false }
            .clickable(
                interactionSource = remember { MutableInteractionSource() },
                indication = null,
            ) { onResume() }
            .onPreviewKeyEvent { event ->
                if (event.type == KeyEventType.KeyDown && (event.key == Key.ButtonB || event.key == Key.Back)) {
                    if (confirmingOption != null) {
                        confirmingOption = null
                    } else {
                        onResume()
                    }
                    true
                } else {
                    false
                }
            },
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .systemBarsPadding()
                .focusProperties { canFocus = false }
                .clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                ) { /* consume clicks inside the panel */ },
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = if (isLandscape) 22.dp else 16.dp, vertical = 10.dp),
            ) {
                Box(
                    modifier = Modifier
                        .size(40.dp)
                        .clip(RoundedCornerShape(9.dp))
                        .background(Color.White.copy(alpha = 0.08f)),
                    contentAlignment = Alignment.Center,
                ) {
                    if (rom != null) {
                        AsyncImage(
                            model = ImageRequest.Builder(context).data(rom).build(),
                            contentDescription = null,
                            filterQuality = FilterQuality.None,
                            modifier = Modifier.fillMaxSize(),
                        )
                    } else {
                        Icon(
                            imageVector = Icons.Filled.VideogameAsset,
                            contentDescription = null,
                            tint = Color.White.copy(alpha = 0.7f),
                            modifier = Modifier.size(22.dp),
                        )
                    }
                }
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(
                        text = rom?.let { it.config.customName ?: it.name } ?: stringResource(R.string.pause),
                        color = Color.White,
                        fontFamily = SpaceGrotesk,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 2.dp)) {
                        Text(
                            text = stringResource(if (pauseMenu.isPaused) R.string.pause_paused_label else R.string.ra_now_playing),
                            color = colors.red,
                            fontFamily = WatermelonMono,
                            fontSize = 9.sp,
                            fontWeight = FontWeight.SemiBold,
                            letterSpacing = 0.8.sp,
                        )
                        val hours = rom?.totalPlayTime?.takeIf { it != Duration.ZERO }?.let { formatHoursLabel(it) }
                        if (hours != null) {
                            Text(
                                text = hours,
                                color = Color.White.copy(alpha = 0.5f),
                                fontFamily = WatermelonMono,
                                fontSize = 9.5.sp,
                                modifier = Modifier.padding(start = 10.dp),
                            )
                        }
                    }
                }
                if (achievementsSummary != null) {
                    Column(horizontalAlignment = Alignment.End) {
                        Text(
                            text = achievementsSummary,
                            color = WatermelonColors.gold,
                            fontFamily = WatermelonMono,
                            fontSize = 11.sp,
                        )
                        Text(
                            text = stringResource(R.string.achievements).uppercase(),
                            color = Color.White.copy(alpha = 0.4f),
                            fontFamily = WatermelonMono,
                            fontSize = 8.5.sp,
                        )
                    }
                }
            }
            Box(Modifier.fillMaxWidth().height(1.dp).background(Color.White.copy(alpha = 0.09f)))

            LazyVerticalGrid(
                columns = GridCells.Fixed(if (isLandscape) 3 else 1),
                horizontalArrangement = Arrangement.spacedBy(5.dp),
                verticalArrangement = Arrangement.spacedBy(5.dp),
                modifier = Modifier
                    .weight(1f)
                    .widthIn(max = 640.dp)
                    .align(Alignment.CenterHorizontally)
                    .padding(horizontal = if (isLandscape) 22.dp else 16.dp, vertical = 12.dp),
            ) {
                item(key = "resume") {
                    PauseOptionRow(
                        label = stringResource(R.string.pause_resume),
                        icon = Icons.Filled.PlayArrow,
                        highlighted = true,
                        destructive = false,
                        focusRequester = resumeFocusRequester,
                        onClick = onResume,
                    )
                }
                items(pauseMenu.options, key = { it.toString() }) { option ->
                    PauseOptionRow(
                        label = pauseMenu.labelOverride(option) ?: stringResource(option.textResource),
                        icon = optionIcon(option),
                        highlighted = false,
                        destructive = isDestructive(option),
                        onClick = {
                            if (needsConfirmation(option)) {
                                confirmingOption = option
                            } else {
                                onOptionSelected(option)
                            }
                        },
                    )
                }
            }

            GamepadHintsFooter(
                hints = listOf(
                    GamepadHint(null, stringResource(R.string.pause_hint_navigate)),
                    GamepadHint("A", stringResource(R.string.pause_hint_accept)),
                    GamepadHint("B", stringResource(R.string.pause_hint_resume)),
                ),
            )
        }

        val currentConfirm = confirmingOption
        if (currentConfirm != null) {
            PauseConfirmationModal(
                option = currentConfirm,
                onCancel = { confirmingOption = null },
                onConfirm = {
                    confirmingOption = null
                    onOptionSelected(currentConfirm)
                },
            )
        }

        me.magnum.melonds.ui.common.RequestInitialFocus(resumeFocusRequester)
    }
}

@Composable
private fun PauseOptionRow(
    label: String,
    icon: ImageVector,
    highlighted: Boolean,
    destructive: Boolean,
    onClick: () -> Unit,
    focusRequester: FocusRequester? = null,
) {
    val colors = watermelon
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()
    val shape = RoundedCornerShape(10.dp)
    val contentColor = when {
        destructive -> colors.red
        else -> Color.White
    }
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .height(42.dp)
            .clip(shape)
            .background(
                when {
                    isFocused -> Color.White.copy(alpha = 0.16f)
                    highlighted -> Color.White.copy(alpha = 0.10f)
                    else -> Color.White.copy(alpha = 0.045f)
                },
            )
            .let {
                when {
                    isFocused -> it.border(2.dp, colors.red, shape)
                    highlighted -> it.border(1.dp, colors.red.copy(alpha = 0.6f), shape)
                    else -> it
                }
            }
            .let { if (focusRequester != null) it.focusRequester(focusRequester) else it }
            .clickable(interactionSource = interactionSource, indication = null, onClick = onClick)
            .padding(horizontal = 14.dp),
    ) {
        if (highlighted) {
            Box(
                Modifier
                    .width(3.dp)
                    .height(20.dp)
                    .clip(RoundedCornerShape(2.dp))
                    .background(colors.red),
            )
            Spacer(Modifier.width(10.dp))
        }
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = if (destructive) colors.red else Color.White.copy(alpha = 0.85f),
            modifier = Modifier.size(20.dp),
        )
        Spacer(Modifier.width(13.dp))
        Text(
            text = label,
            color = contentColor,
            fontSize = 13.5.sp,
            fontWeight = FontWeight.SemiBold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
private fun PauseConfirmationModal(
    option: PauseMenuOption,
    onCancel: () -> Unit,
    onConfirm: () -> Unit,
) {
    val colors = watermelon
    val isReset = option == RomPauseMenuOption.RESET || option == FirmwarePauseMenuOption.RESET
    val title = stringResource(if (isReset) R.string.pause_confirm_reset_title else R.string.pause_confirm_exit_title)
    val message = stringResource(if (isReset) R.string.pause_confirm_reset_message else R.string.pause_confirm_exit_message)
    val cta = stringResource(if (isReset) R.string.reset else R.string.exit)
    val confirmFocusRequester = remember { FocusRequester() }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.6f))
            .focusProperties { canFocus = false }
            .clickable(
                interactionSource = remember { MutableInteractionSource() },
                indication = null,
            ) { onCancel() },
        contentAlignment = Alignment.Center,
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier
                .padding(28.dp)
                .widthIn(max = 330.dp)
                .clip(RoundedCornerShape(17.dp))
                .background(colors.surface)
                .border(1.dp, colors.line, RoundedCornerShape(17.dp))
                .focusProperties { canFocus = false }
                .clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                ) { }
                .padding(start = 20.dp, end = 20.dp, top = 22.dp, bottom = 16.dp),
        ) {
            Icon(
                imageVector = if (isReset) Icons.Filled.RestartAlt else Icons.AutoMirrored.Filled.Logout,
                contentDescription = null,
                tint = colors.red,
                modifier = Modifier.size(34.dp),
            )
            Text(
                text = title,
                color = colors.text,
                fontFamily = SpaceGrotesk,
                fontSize = 17.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(top = 10.dp),
            )
            Text(
                text = message,
                color = colors.text3,
                fontSize = 12.5.sp,
                lineHeight = 18.sp,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                modifier = Modifier.padding(top = 6.dp),
            )
            Row(modifier = Modifier.fillMaxWidth().padding(top = 16.dp)) {
                ConfirmButton(
                    label = stringResource(R.string.cancel),
                    background = colors.surface2,
                    textColor = colors.text,
                    onClick = onCancel,
                    modifier = Modifier.weight(1f),
                )
                Spacer(Modifier.width(9.dp))
                ConfirmButton(
                    label = cta,
                    background = colors.red,
                    textColor = Color.White,
                    onClick = onConfirm,
                    modifier = Modifier.weight(1f).focusRequester(confirmFocusRequester),
                )
            }
        }
    }

    me.magnum.melonds.ui.common.RequestInitialFocus(confirmFocusRequester)
}

@Composable
private fun ConfirmButton(
    label: String,
    background: Color,
    textColor: Color,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = watermelon
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()
    val shape = RoundedCornerShape(11.dp)
    Box(
        modifier = modifier
            .height(42.dp)
            .clip(shape)
            .background(background)
            .let { if (isFocused) it.border(2.dp, colors.red, shape) else it }
            .clickable(interactionSource = interactionSource, indication = null, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = label,
            color = textColor,
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
        )
    }
}
