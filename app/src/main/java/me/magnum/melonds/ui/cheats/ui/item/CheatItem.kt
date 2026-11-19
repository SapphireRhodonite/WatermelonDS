package me.magnum.melonds.ui.cheats.ui.item

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
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.DropdownMenu
import androidx.compose.material.DropdownMenuItem
import androidx.compose.material.Icon
import androidx.compose.material.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.runtime.Composable
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
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.Cheat
import me.magnum.melonds.ui.common.MelonPreviewSet
import me.magnum.melonds.ui.common.WatermelonSwitch
import me.magnum.melonds.ui.theme.MelonTheme
import me.magnum.melonds.ui.theme.WatermelonMono
import me.magnum.melonds.ui.theme.watermelon

@Composable
fun CheatItem(
    modifier: Modifier,
    cheat: Cheat,
    onClick: () -> Unit,
    onEditClick: () -> Unit,
    onDeleteClick: () -> Unit,
    folderName: String? = null,
    highlightQuery: String = "",
) {
    val colors = watermelon
    val hasDescription = cheat.description?.isNotBlank() == true
    var showCheatOptions by remember { mutableStateOf(false) }
    val (mainFocusRequester, optionsFocusRequester) = remember { FocusRequester.createRefs() }
    val interactionSource = remember { MutableInteractionSource() }
    val isFocused by interactionSource.collectIsFocusedAsState()
    val shape = RoundedCornerShape(13.dp)

    Row(
        modifier = modifier
            .clip(shape)
            .background(if (isFocused) colors.surface3 else colors.surface2)
            .let { if (isFocused) it.border(2.dp, colors.red, shape) else it }
            .focusRequester(mainFocusRequester)
            .focusProperties { end = optionsFocusRequester }
            .clickable(interactionSource = interactionSource, indication = null, onClick = onClick)
            .onKeyEvent {
                if (it.type == KeyEventType.KeyDown && it.key == Key.Menu) {
                    showCheatOptions = true
                    true
                } else {
                    false
                }
            }
            .padding(start = 14.dp, end = 6.dp, top = 11.dp, bottom = 11.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(3.dp),
        ) {
            if (folderName != null) {
                Text(
                    text = highlightedCheatText(folderName, highlightQuery),
                    color = colors.text3,
                    fontFamily = WatermelonMono,
                    fontSize = 10.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Text(
                text = highlightedCheatText(cheat.name, highlightQuery),
                color = colors.text,
                fontSize = 13.5.sp,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (hasDescription) {
                Text(
                    text = cheat.description!!,
                    color = colors.text3,
                    fontSize = 11.5.sp,
                    lineHeight = 15.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            if (cheat.code.isNotBlank()) {
                Text(
                    text = cheat.code.replace('\n', ' ').trim(),
                    color = colors.text3,
                    fontFamily = WatermelonMono,
                    fontSize = 9.5.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }

        Box {
            Icon(
                imageVector = Icons.Default.MoreVert,
                contentDescription = stringResource(R.string.options),
                tint = colors.text3,
                modifier = Modifier
                    .size(34.dp)
                    .clip(RoundedCornerShape(8.dp))
                    .focusRequester(optionsFocusRequester)
                    .focusProperties { start = mainFocusRequester }
                    .clickable { showCheatOptions = true }
                    .padding(7.dp),
            )
            DropdownMenu(
                expanded = showCheatOptions,
                onDismissRequest = { showCheatOptions = false },
            ) {
                DropdownMenuItem(
                    onClick = {
                        showCheatOptions = false
                        onEditClick()
                    },
                ) {
                    Text(stringResource(R.string.edit))
                }
                DropdownMenuItem(
                    onClick = {
                        showCheatOptions = false
                        onDeleteClick()
                    },
                ) {
                    Text(stringResource(R.string.delete))
                }
            }
        }

        Spacer(Modifier.width(2.dp))
        WatermelonSwitch(
            checked = cheat.enabled,
            onCheckedChange = { onClick() },
        )
    }
}

private fun highlightedCheatText(text: String, query: String) = buildAnnotatedString {
    append(text)
    if (query.isNotBlank()) {
        var match = text.indexOf(query, ignoreCase = true)
        while (match >= 0) {
            addStyle(SpanStyle(fontWeight = FontWeight.Bold), match, match + query.length)
            match = text.indexOf(query, match + query.length, ignoreCase = true)
        }
    }
}

@MelonPreviewSet
@Composable
private fun PreviewCheatItem() {
    MelonTheme {
        CheatItem(
            modifier = Modifier.fillMaxWidth(),
            cheat = Cheat(0, 0, "Some random cheat", "Press some buttons to activate this cheat. What does it do?", "94000130 FCFF0000", false),
            onClick = { },
            onEditClick = { },
            onDeleteClick = { },
        )
    }
}
