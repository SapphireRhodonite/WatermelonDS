package me.magnum.melonds.ui.romlist.composables

import androidx.compose.animation.Crossfade
import androidx.compose.foundation.LocalOverscrollFactory
import androidx.compose.foundation.MutatePriority
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridState
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.ExperimentalMaterialApi
import androidx.compose.material.LinearProgressIndicator
import androidx.compose.material.Surface
import androidx.compose.material.Text
import androidx.compose.material.pullrefresh.PullRefreshIndicator
import androidx.compose.material.pullrefresh.pullRefresh
import androidx.compose.material.pullrefresh.rememberPullRefreshState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.launch
import me.magnum.melonds.R
import me.magnum.melonds.multiplayer.MultiplayerInvitePill
import me.magnum.melonds.multiplayer.MultiplayerNearbyWatch
import me.magnum.melonds.domain.model.RomFilter
import me.magnum.melonds.domain.model.RomScanningStatus
import me.magnum.melonds.domain.model.RomViewMode
import me.magnum.melonds.domain.model.SortingMode
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.ui.romlist.RomBrowserEntry
import me.magnum.melonds.ui.romlist.RomBrowserUiState
import me.magnum.melonds.ui.theme.watermelon

@OptIn(ExperimentalMaterialApi::class)
@Composable
fun RomBrowserScreen(
    state: RomBrowserUiState,
    coverByUri: Map<String, String>,
    boxArtByUri: Map<String, String>,
    useCoverArt: Boolean,
    searchQuery: String,
    allowConfiguration: Boolean,
    scanningStatus: RomScanningStatus,
    confirmedAchievementHashes: Set<String>,
    onFolderClick: (RomBrowserEntry.Folder) -> Unit,
    onRomClick: (Rom) -> Unit,
    onRomLongPress: (Rom) -> Unit,
    onRomConfigClick: (Rom) -> Unit,
    onFilterSelected: (RomFilter) -> Unit,
    onSortSelected: (SortingMode) -> Unit,
    onNavigateUp: () -> Unit,
    onRefresh: () -> Unit,
    onSearchQueryChanged: (String?) -> Unit,
    onToggleViewMode: () -> Unit,
    onBootFirmwareDs: () -> Unit,
    onBootFirmwareDsi: () -> Unit,
    onOpenDsiWareManager: () -> Unit,
    onOpenMultiplayer: () -> Unit,
    onOpenSettings: () -> Unit,
    onJoinNearby: (String) -> Unit = {},
    onRomVisible: (Rom) -> Unit = {},
    onFocusedRomChanged: (Rom?) -> Unit = {},
    onDpadDownGateChanged: ((() -> Boolean)?) -> Unit = {},
) {
    val colors = watermelon
    val refreshState = rememberPullRefreshState(
        refreshing = scanningStatus == RomScanningStatus.SCANNING,
        onRefresh = onRefresh,
    )
    val coroutineScope = rememberCoroutineScope()
    val gridState = rememberLazyGridState()
    val listState = rememberLazyListState()
    val itemFocusRequesters = remember { mutableStateMapOf<String, FocusRequester>() }
    var focusedEntryIndex by remember { mutableIntStateOf(-1) }

    val folderCount = remember(state.entries) { state.entries.takeWhile { it is RomBrowserEntry.Folder }.size }
    val hasFolders = folderCount > 0
    val romCount = state.entries.size - folderCount
    val showAlphabetBar = (state.alphabetIndex.isNotEmpty() || hasFolders) && state.sortingMode == SortingMode.ALPHABETICALLY

    val isAtLibraryTop = state.isAtVirtualRoot || !state.canNavigateUp
    val showContinueShelf = isAtLibraryTop && !state.isSearchActive && state.filter == RomFilter.ALL && state.continuePlaying.isNotEmpty()
    val showSectionHeader = !state.isSearchActive

    val gridLeadingItems = 1 + (if (showContinueShelf) 1 else 0) + (if (showSectionHeader) 1 else 0) + (if (hasFolders) 1 else 0)
    val listLeadingItems = 1 + (if (showContinueShelf) 1 else 0) + (if (showSectionHeader) 1 else 0)

    LaunchedEffect(state.filter, state.breadcrumbs, state.isSearchActive) {
        focusedEntryIndex = -1
        gridState.scrollToItem(0)
        listState.scrollToItem(0)
    }

    LaunchedEffect(focusedEntryIndex, state.entries) {
        val focusedRom = (state.entries.getOrNull(focusedEntryIndex) as? RomBrowserEntry.RomItem)?.rom
        onFocusedRomChanged(focusedRom)
    }

    val gridColumnCount by remember { derivedStateOf { gridState.currentColumnCount() } }
    val firstRomEntryIndexInLastGridRow = remember(romCount, folderCount, gridColumnCount) {
        if (romCount <= 0) {
            Int.MAX_VALUE
        } else {
            folderCount + firstIndexInLastGridRow(totalItems = romCount, columnCount = gridColumnCount)
        }
    }

    DisposableEffect(state.viewMode, focusedEntryIndex, firstRomEntryIndexInLastGridRow, state.entries.size, onDpadDownGateChanged) {
        onDpadDownGateChanged {
            when (state.viewMode) {
                RomViewMode.GRID -> focusedEntryIndex >= firstRomEntryIndexInLastGridRow
                RomViewMode.LIST -> focusedEntryIndex == state.entries.lastIndex
            }
        }
        onDispose {
            onDpadDownGateChanged(null)
        }
    }

    LaunchedEffect(state.viewMode, firstRomEntryIndexInLastGridRow) {
        snapshotFlow {
            when (state.viewMode) {
                RomViewMode.GRID -> focusedEntryIndex >= firstRomEntryIndexInLastGridRow
                RomViewMode.LIST -> focusedEntryIndex == state.entries.lastIndex
            }
        }
            .distinctUntilChanged()
            .filter { atBottom -> atBottom }
            .collect {
                when (state.viewMode) {
                    RomViewMode.GRID -> gridState.scroll(MutatePriority.PreventUserInput) {}
                    RomViewMode.LIST -> listState.scroll(MutatePriority.PreventUserInput) {}
                }
            }
    }

    Surface(color = colors.bg, modifier = Modifier.fillMaxSize()) {
        Column(modifier = Modifier.fillMaxSize().systemBarsPadding()) {
            WatermelonLibraryHeader(
                isSearchActive = state.isSearchActive,
                searchQuery = searchQuery,
                viewMode = state.viewMode,
                onSearchQueryChanged = onSearchQueryChanged,
                onToggleViewMode = onToggleViewMode,
                onBootFirmwareDs = onBootFirmwareDs,
                onBootFirmwareDsi = onBootFirmwareDsi,
                onOpenDsiWareManager = onOpenDsiWareManager,
                onOpenMultiplayer = onOpenMultiplayer,
                onRefresh = onRefresh,
                onOpenSettings = onOpenSettings,
            )

            if (!state.isSearchActive) {
                MultiplayerNearbyWatch()
            }
            val invite: @Composable () -> Unit = { if (!state.isSearchActive) MultiplayerInvitePill(onJoinNearby) }

            if (scanningStatus == RomScanningStatus.SCANNING) {
                LinearProgressIndicator(
                    color = colors.green,
                    backgroundColor = colors.surface2,
                    modifier = Modifier.fillMaxWidth(),
                )
            }

            BreadcrumbBar(
                breadcrumbs = state.breadcrumbs,
                canNavigateUp = state.canNavigateUp,
                isAtVirtualRoot = state.isAtVirtualRoot,
                isSearchActive = state.isSearchActive,
                onNavigateUp = onNavigateUp,
            )

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .pullRefresh(refreshState),
            ) {
                if (state.entries.isEmpty() && !showContinueShelf) {
                    Column(modifier = Modifier.fillMaxSize()) {
                        FilterChipsRow(
                            selected = state.filter,
                            onFilterSelected = onFilterSelected,
                            trailing = invite,
                        )
                        EmptyState(filter = state.filter)
                    }
                } else {
                    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
                        Crossfade(targetState = state.viewMode, label = "view_mode") { mode ->
                            when (mode) {
                                RomViewMode.GRID -> GridContent(
                                    state = state,
                                    gridState = gridState,
                                    coverByUri = coverByUri,
                                    boxArtByUri = boxArtByUri,
                                    useCoverArt = useCoverArt,
                                    confirmedAchievementHashes = confirmedAchievementHashes,
                                    showAlphabetBar = showAlphabetBar,
                                    showContinueShelf = showContinueShelf,
                                    showSectionHeader = showSectionHeader,
                                    folderCount = folderCount,
                                    viewportHeight = maxHeight,
                                    itemFocusRequesters = itemFocusRequesters,
                                    focusedEntryIndex = focusedEntryIndex,
                                    firstRomEntryIndexInLastGridRow = firstRomEntryIndexInLastGridRow,
                                    onFocusedEntryIndexChanged = { focusedEntryIndex = it },
                                    onRomFocused = onFocusedRomChanged,
                                    onFolderClick = onFolderClick,
                                    onRomClick = onRomClick,
                                    onRomLongPress = onRomLongPress,
                                    onFilterSelected = onFilterSelected,
                                    onSortSelected = onSortSelected,
                                    onNavigateUp = onNavigateUp,
                                    onRomVisible = onRomVisible,
                                    invite = invite,
                                )
                                RomViewMode.LIST -> ListContent(
                                    state = state,
                                    listState = listState,
                                    coverByUri = coverByUri,
                                    boxArtByUri = boxArtByUri,
                                    useCoverArt = useCoverArt,
                                    allowConfiguration = allowConfiguration,
                                    confirmedAchievementHashes = confirmedAchievementHashes,
                                    showAlphabetBar = showAlphabetBar,
                                    showContinueShelf = showContinueShelf,
                                    showSectionHeader = showSectionHeader,
                                    folderCount = folderCount,
                                    viewportHeight = maxHeight,
                                    itemFocusRequesters = itemFocusRequesters,
                                    focusedEntryIndex = focusedEntryIndex,
                                    onFocusedEntryIndexChanged = { focusedEntryIndex = it },
                                    onRomFocused = onFocusedRomChanged,
                                    onFolderClick = onFolderClick,
                                    onRomClick = onRomClick,
                                    onRomLongPress = onRomLongPress,
                                    onRomConfigClick = onRomConfigClick,
                                    onFilterSelected = onFilterSelected,
                                    onSortSelected = onSortSelected,
                                    onNavigateUp = onNavigateUp,
                                    onRomVisible = onRomVisible,
                                    invite = invite,
                                )
                            }
                        }
                    }
                }
                PullRefreshIndicator(
                    refreshing = scanningStatus == RomScanningStatus.SCANNING,
                    state = refreshState,
                    modifier = Modifier.align(Alignment.TopCenter),
                    backgroundColor = colors.surface,
                    contentColor = colors.green,
                )

                if (showAlphabetBar) {
                    val leadingItems = when (state.viewMode) {
                        RomViewMode.GRID -> gridLeadingItems
                        RomViewMode.LIST -> listLeadingItems
                    }
                    val activeFirstVis by remember(state.viewMode, leadingItems, folderCount) {
                        derivedStateOf {
                            val firstVisItem = when (state.viewMode) {
                                RomViewMode.GRID -> gridState.firstVisibleItemIndex
                                RomViewMode.LIST -> listState.firstVisibleItemIndex
                            }
                            when (state.viewMode) {
                                RomViewMode.GRID -> (firstVisItem - leadingItems + folderCount).coerceAtLeast(0)
                                RomViewMode.LIST -> (firstVisItem - leadingItems).coerceAtLeast(0)
                            }
                        }
                    }
                    val activeLetter by remember(state.alphabetIndex, state.viewMode) {
                        derivedStateOf { letterForIndex(state.alphabetIndex, activeFirstVis) }
                    }
                    val isInFolderSection by remember(folderCount, state.viewMode) {
                        derivedStateOf { hasFolders && activeFirstVis < folderCount }
                    }
                    AlphabetIndexBar(
                        alphabetIndex = state.alphabetIndex,
                        activeLetter = activeLetter,
                        hasFolders = hasFolders,
                        isInFolderSection = isInFolderSection,
                        onFoldersClicked = {
                            coroutineScope.launch {
                                when (state.viewMode) {
                                    RomViewMode.GRID -> gridState.scrollToItem(0)
                                    RomViewMode.LIST -> listState.scrollToItem(0)
                                }
                                requestFirstVisibleRomFocus(
                                    state = state,
                                    gridState = gridState,
                                    listState = listState,
                                    itemFocusRequesters = itemFocusRequesters,
                                )
                            }
                        },
                        onLetterTouched = { idx, letter ->
                            coroutineScope.launch {
                                val targetItemIndex = when (state.viewMode) {
                                    RomViewMode.GRID -> leadingItems + (idx - folderCount).coerceAtLeast(0)
                                    RomViewMode.LIST -> leadingItems + idx
                                }
                                when (state.viewMode) {
                                    RomViewMode.GRID -> gridState.scrollToItem(targetItemIndex)
                                    RomViewMode.LIST -> listState.scrollToItem(targetItemIndex)
                                }
                                requestRomFocusAtIndex(
                                    state = state,
                                    targetIndex = idx,
                                    gridState = gridState,
                                    listState = listState,
                                    itemFocusRequesters = itemFocusRequesters,
                                )
                            }
                        },
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun GridContent(
    state: RomBrowserUiState,
    gridState: LazyGridState,
    coverByUri: Map<String, String>,
    boxArtByUri: Map<String, String>,
    useCoverArt: Boolean,
    confirmedAchievementHashes: Set<String>,
    showAlphabetBar: Boolean,
    showContinueShelf: Boolean,
    showSectionHeader: Boolean,
    folderCount: Int,
    viewportHeight: Dp,
    itemFocusRequesters: MutableMap<String, FocusRequester>,
    focusedEntryIndex: Int,
    firstRomEntryIndexInLastGridRow: Int,
    onFocusedEntryIndexChanged: (Int) -> Unit,
    onRomFocused: (Rom) -> Unit = {},
    onFolderClick: (RomBrowserEntry.Folder) -> Unit,
    onRomClick: (Rom) -> Unit,
    onRomLongPress: (Rom) -> Unit,
    onFilterSelected: (RomFilter) -> Unit,
    onSortSelected: (SortingMode) -> Unit,
    onNavigateUp: () -> Unit,
    onRomVisible: (Rom) -> Unit,
    invite: @Composable () -> Unit,
) {
    val folders = state.entries.take(folderCount).filterIsInstance<RomBrowserEntry.Folder>()
    val roms = state.entries.drop(folderCount).filterIsInstance<RomBrowserEntry.RomItem>()

    RomListOverscrollProvider {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 104.dp),
            state = gridState,
            contentPadding = PaddingValues(
                start = 16.dp,
                end = if (showAlphabetBar) 28.dp else 16.dp,
                top = 0.dp,
                bottom = rememberTrailingLetterScrollPadding(
                    viewportHeight = viewportHeight,
                    visibleItemHeightPx = gridState.layoutInfo.visibleItemsInfo.maxOfOrNull { item -> item.size.height } ?: 0,
                    minimumPadding = if (state.filter == RomFilter.FAVORITES) 96.dp else 32.dp,
                ),
            ),
            verticalArrangement = Arrangement.spacedBy(11.dp),
            horizontalArrangement = Arrangement.spacedBy(11.dp),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "filters", span = { GridItemSpan(maxLineSpan) }) {
                FilterChipsRow(
                    selected = state.filter,
                    onFilterSelected = onFilterSelected,
                    modifier = Modifier.padding(start = 0.dp),
                    trailing = invite,
                )
            }
            if (showContinueShelf) {
                item(key = "continue", span = { GridItemSpan(maxLineSpan) }) {
                    ContinuePlayingShelf(
                        roms = state.continuePlaying,
                        coverByUri = coverByUri,
                        boxArtByUri = boxArtByUri,
                        useCoverArt = useCoverArt,
                        onRomClicked = onRomClick,
                        onRomLongPressed = onRomLongPress,
                        horizontalPadding = 0.dp,
                        onRomFocused = onRomFocused,
                        onRomVisible = onRomVisible,
                    )
                }
            }
            if (showSectionHeader) {
                item(key = "section_header", span = { GridItemSpan(maxLineSpan) }) {
                    LibrarySectionHeader(
                        title = if (state.canNavigateUp) state.breadcrumbs.lastOrNull() ?: stringResource(R.string.rom_all_games) else stringResource(R.string.rom_all_games),
                        inFolder = state.canNavigateUp,
                        sortingMode = state.sortingMode,
                        sortingOrder = state.sortingOrder,
                        gamesCount = roms.size,
                        onNavigateUp = onNavigateUp,
                        onSortSelected = onSortSelected,
                        modifier = Modifier.padding(horizontal = 0.dp),
                    )
                }
            }
            if (folders.isNotEmpty()) {
                item(key = "folders", span = { GridItemSpan(maxLineSpan) }) {
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                        modifier = Modifier.fillMaxWidth().padding(bottom = 2.dp),
                    ) {
                        folders.forEachIndexed { folderIdx, folder ->
                            FolderGridCard(
                                name = folder.name,
                                relativePath = folder.relativePath,
                                onClick = { onFolderClick(folder) },
                                modifier = rememberRomBrowserItemFocusModifier(
                                    index = folderIdx,
                                    focusKey = "folder:${folder.docId}",
                                    itemFocusRequesters = itemFocusRequesters,
                                    focusedEntryIndex = focusedEntryIndex,
                                    onFocusedEntryIndexChanged = onFocusedEntryIndexChanged,
                                ),
                            )
                        }
                    }
                }
            }
            itemsIndexed(
                items = roms,
                key = { _, entry -> "rom:${entry.rom.uri}" },
            ) { romIdx, entry ->
                val entryIndex = folderCount + romIdx
                LaunchedEffect(entry.rom.uri, entry.rom.config.iconSource, useCoverArt) {
                    onRomVisible(entry.rom)
                }
                RomGridCard(
                    rom = entry.rom,
                    coverUrl = coverByUri[entry.rom.uri.toString()],
                    boxArtUrl = boxArtByUri[entry.rom.uri.toString()]?.takeIf { it.isNotEmpty() },
                    boxArtLoading = boxArtByUri[entry.rom.uri.toString()] == null,
                    useCoverArt = useCoverArt,
                    showAchievementBadge = entry.rom.retroAchievementsHash in confirmedAchievementHashes,
                    onClick = { onRomClick(entry.rom) },
                    onLongPress = {
                        if (!entry.rom.isInstalledDsiWareShortcut) {
                            onRomLongPress(entry.rom)
                        }
                    },
                    modifier = rememberRomBrowserItemFocusModifier(
                        index = entryIndex,
                        focusKey = "rom:${entry.rom.uri}",
                        itemFocusRequesters = itemFocusRequesters,
                        focusedEntryIndex = focusedEntryIndex,
                        onFocusedEntryIndexChanged = onFocusedEntryIndexChanged,
                    ).cancelDpadDownIf(entryIndex >= firstRomEntryIndexInLastGridRow),
                )
            }
        }
    }
}

@Composable
private fun ListContent(
    state: RomBrowserUiState,
    listState: LazyListState,
    coverByUri: Map<String, String>,
    boxArtByUri: Map<String, String>,
    useCoverArt: Boolean,
    allowConfiguration: Boolean,
    confirmedAchievementHashes: Set<String>,
    showAlphabetBar: Boolean,
    showContinueShelf: Boolean,
    showSectionHeader: Boolean,
    folderCount: Int,
    viewportHeight: Dp,
    itemFocusRequesters: MutableMap<String, FocusRequester>,
    focusedEntryIndex: Int,
    onFocusedEntryIndexChanged: (Int) -> Unit,
    onRomFocused: (Rom) -> Unit = {},
    onFolderClick: (RomBrowserEntry.Folder) -> Unit,
    onRomClick: (Rom) -> Unit,
    onRomLongPress: (Rom) -> Unit,
    onRomConfigClick: (Rom) -> Unit,
    onFilterSelected: (RomFilter) -> Unit,
    onSortSelected: (SortingMode) -> Unit,
    onNavigateUp: () -> Unit,
    onRomVisible: (Rom) -> Unit,
    invite: @Composable () -> Unit,
) {
    RomListOverscrollProvider {
        LazyColumn(
            state = listState,
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                start = 0.dp,
                end = if (showAlphabetBar) 28.dp else 0.dp,
                top = 0.dp,
                bottom = rememberTrailingLetterScrollPadding(
                    viewportHeight = viewportHeight,
                    visibleItemHeightPx = listState.layoutInfo.visibleItemsInfo.maxOfOrNull { item -> item.size } ?: 0,
                    minimumPadding = if (state.filter == RomFilter.FAVORITES) 96.dp else 32.dp,
                ),
            ),
        ) {
            item(key = "filters") {
                FilterChipsRow(
                    selected = state.filter,
                    onFilterSelected = onFilterSelected,
                    trailing = invite,
                )
            }
            if (showContinueShelf) {
                item(key = "continue") {
                    ContinuePlayingShelf(
                        roms = state.continuePlaying,
                        coverByUri = coverByUri,
                        boxArtByUri = boxArtByUri,
                        useCoverArt = useCoverArt,
                        onRomClicked = onRomClick,
                        onRomLongPressed = onRomLongPress,
                        onRomFocused = onRomFocused,
                        onRomVisible = onRomVisible,
                    )
                }
            }
            if (showSectionHeader) {
                item(key = "section_header") {
                    LibrarySectionHeader(
                        title = if (state.canNavigateUp) state.breadcrumbs.lastOrNull() ?: stringResource(R.string.rom_all_games) else stringResource(R.string.rom_all_games),
                        inFolder = state.canNavigateUp,
                        sortingMode = state.sortingMode,
                        sortingOrder = state.sortingOrder,
                        gamesCount = state.entries.size - folderCount,
                        onNavigateUp = onNavigateUp,
                        onSortSelected = onSortSelected,
                    )
                }
            }
            itemsIndexed(
                items = state.entries,
                key = { _, entry ->
                    when (entry) {
                        is RomBrowserEntry.Folder -> "folder:${entry.docId}"
                        is RomBrowserEntry.RomItem -> "rom:${entry.rom.uri}"
                    }
                },
            ) { index, entry ->
                when (entry) {
                    is RomBrowserEntry.Folder -> FolderListRow(
                        name = entry.name,
                        relativePath = entry.relativePath,
                        onClick = { onFolderClick(entry) },
                        modifier = rememberRomBrowserItemFocusModifier(
                            index = index,
                            focusKey = entry.focusKey(),
                            itemFocusRequesters = itemFocusRequesters,
                            focusedEntryIndex = focusedEntryIndex,
                            onFocusedEntryIndexChanged = onFocusedEntryIndexChanged,
                        ).cancelDpadDownIf(index == state.entries.lastIndex),
                    )
                    is RomBrowserEntry.RomItem -> {
                        LaunchedEffect(entry.rom.uri, entry.rom.config.iconSource, useCoverArt) {
                            onRomVisible(entry.rom)
                        }
                        RomListRow(
                            rom = entry.rom,
                            coverUrl = coverByUri[entry.rom.uri.toString()],
                            boxArtUrl = boxArtByUri[entry.rom.uri.toString()]?.takeIf { it.isNotEmpty() },
                            boxArtLoading = boxArtByUri[entry.rom.uri.toString()] == null,
                            useCoverArt = useCoverArt,
                            allowConfiguration = allowConfiguration && !entry.rom.isInstalledDsiWareShortcut,
                            showAchievementBadge = entry.rom.retroAchievementsHash in confirmedAchievementHashes,
                            onClick = { onRomClick(entry.rom) },
                            onLongPress = {
                                if (!entry.rom.isInstalledDsiWareShortcut) {
                                    onRomLongPress(entry.rom)
                                }
                            },
                            onConfigClick = { onRomConfigClick(entry.rom) },
                            modifier = rememberRomBrowserItemFocusModifier(
                                index = index,
                                focusKey = entry.focusKey(),
                                itemFocusRequesters = itemFocusRequesters,
                                focusedEntryIndex = focusedEntryIndex,
                                onFocusedEntryIndexChanged = onFocusedEntryIndexChanged,
                            ).cancelDpadDownIf(index == state.entries.lastIndex),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun rememberRomBrowserItemFocusModifier(
    index: Int,
    focusKey: String,
    itemFocusRequesters: MutableMap<String, FocusRequester>,
    focusedEntryIndex: Int,
    onFocusedEntryIndexChanged: (Int) -> Unit,
): Modifier {
    val focusRequester = remember(focusKey) { FocusRequester() }
    DisposableEffect(focusKey, focusRequester) {
        itemFocusRequesters[focusKey] = focusRequester
        onDispose {
            if (itemFocusRequesters[focusKey] == focusRequester) {
                itemFocusRequesters.remove(focusKey)
            }
        }
    }
    return Modifier
        .focusRequester(focusRequester)
        .onFocusChanged { focusState ->
            if (focusState.isFocused) {
                onFocusedEntryIndexChanged(index)
            } else if (focusedEntryIndex == index) {
                onFocusedEntryIndexChanged(-1)
            }
        }
}

private fun Modifier.cancelDpadDownIf(cancel: Boolean): Modifier {
    if (!cancel) {
        return this
    }

    return onPreviewKeyEvent { keyEvent ->
        keyEvent.type == KeyEventType.KeyDown && keyEvent.key == Key.DirectionDown
    }.focusProperties {
        down = FocusRequester.Cancel
    }
}

@Composable
private fun rememberTrailingLetterScrollPadding(
    viewportHeight: Dp,
    visibleItemHeightPx: Int,
    minimumPadding: Dp,
): Dp {
    val density = LocalDensity.current
    if (visibleItemHeightPx <= 0) {
        return minimumPadding
    }

    val itemHeight = with(density) { visibleItemHeightPx.toDp() }
    val letterScrollPadding = (viewportHeight - itemHeight).coerceAtLeast(0.dp)
    return maxOf(minimumPadding, letterScrollPadding)
}

private suspend fun requestFirstVisibleRomFocus(
    state: RomBrowserUiState,
    gridState: LazyGridState,
    listState: LazyListState,
    itemFocusRequesters: Map<String, FocusRequester>,
) {
    repeat(4) {
        withFrameNanos { }
        val visibleIndexes = when (state.viewMode) {
            RomViewMode.GRID -> gridState.layoutInfo.visibleItemsInfo.map { item -> item.index }
            RomViewMode.LIST -> listState.layoutInfo.visibleItemsInfo.map { item -> item.index }
        }.sorted()
        val targetEntry = visibleIndexes
            .asSequence()
            .mapNotNull { index -> state.entries.getOrNull(index) as? RomBrowserEntry.RomItem }
            .firstOrNull()
            ?: state.entries.filterIsInstance<RomBrowserEntry.RomItem>().firstOrNull()
        val focusRequester = targetEntry?.let { itemFocusRequesters[it.focusKey()] }
        if (focusRequester != null) {
            runCatching { focusRequester.requestFocus() }
            return
        }
    }
}

private suspend fun requestRomFocusAtIndex(
    state: RomBrowserUiState,
    targetIndex: Int,
    gridState: LazyGridState,
    listState: LazyListState,
    itemFocusRequesters: Map<String, FocusRequester>,
) {
    repeat(4) {
        withFrameNanos { }
        val targetEntry = state.entries.getOrNull(targetIndex) as? RomBrowserEntry.RomItem
        val focusRequester = targetEntry?.let { itemFocusRequesters[it.focusKey()] }
        if (focusRequester != null) {
            runCatching { focusRequester.requestFocus() }
            return
        }
    }

    requestFirstVisibleRomFocus(
        state = state,
        gridState = gridState,
        listState = listState,
        itemFocusRequesters = itemFocusRequesters,
    )
}

private fun RomBrowserEntry.focusKey(): String {
    return when (this) {
        is RomBrowserEntry.Folder -> "folder:$docId"
        is RomBrowserEntry.RomItem -> focusKey()
    }
}

private fun RomBrowserEntry.RomItem.focusKey(): String {
    return "rom:${rom.uri}"
}

private fun LazyGridState.currentColumnCount(): Int {
    return ((layoutInfo.visibleItemsInfo.maxOfOrNull { item -> item.column } ?: 0) + 1).coerceAtLeast(1)
}

private fun firstIndexInLastGridRow(totalItems: Int, columnCount: Int): Int {
    val lastIndex = totalItems - 1
    if (lastIndex < 0) {
        return Int.MAX_VALUE
    }
    return lastIndex - (lastIndex % columnCount.coerceAtLeast(1))
}

@Composable
private fun RomListOverscrollProvider(
    content: @Composable () -> Unit,
) {
    CompositionLocalProvider(LocalOverscrollFactory provides null) {
        content()
    }
}

private fun letterForIndex(alphabetIndex: Map<Char, Int>, currentIndex: Int): Char? {
    if (alphabetIndex.isEmpty()) return null
    var match: Char? = null
    var matchIndex = -1
    alphabetIndex.forEach { (letter, startIndex) ->
        if (startIndex <= currentIndex && startIndex > matchIndex) {
            match = letter
            matchIndex = startIndex
        }
    }
    return match
}

@Composable
private fun EmptyState(filter: RomFilter) {
    val colors = watermelon
    Box(modifier = Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
        val msg = when (filter) {
            RomFilter.ALL -> stringResource(R.string.no_roms_found)
            RomFilter.FAVORITES -> stringResource(R.string.rom_no_favorites)
            else -> stringResource(R.string.rom_no_results_filter)
        }
        Text(
            text = msg,
            color = colors.text2,
            textAlign = TextAlign.Center,
        )
    }
}
