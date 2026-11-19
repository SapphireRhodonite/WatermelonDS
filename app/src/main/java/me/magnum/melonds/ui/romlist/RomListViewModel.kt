package me.magnum.melonds.ui.romlist

import android.net.Uri
import android.provider.DocumentsContract
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import me.magnum.melonds.common.DirectoryAccessValidator
import me.magnum.melonds.common.Permission
import me.magnum.melonds.common.UriPermissionManager
import me.magnum.melonds.domain.model.DSiProfile
import me.magnum.melonds.domain.model.DSiWareTitle
import me.magnum.melonds.domain.model.RomFilter
import me.magnum.melonds.domain.model.RomViewMode
import me.magnum.melonds.domain.model.SortingMode
import me.magnum.melonds.domain.model.SortingOrder
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.RomDirectoryScanStatus
import me.magnum.melonds.domain.model.rom.config.RomConfig
import me.magnum.melonds.domain.model.rom.config.RomIconSource
import me.magnum.melonds.domain.repositories.RetroAchievementsRepository
import me.magnum.melonds.domain.repositories.RomsRepository
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.domain.services.DSiNandManager
import me.magnum.melonds.impl.RomIconProvider
import me.magnum.melonds.utils.EventSharedFlow
import me.magnum.melonds.utils.SubjectSharedFlow
import me.magnum.melonds.ui.romlist.RomBrowserEntry
import me.magnum.melonds.ui.romlist.RomBrowserUiState
import java.text.Normalizer
import java.util.Calendar
import javax.inject.Inject

@HiltViewModel
class RomListViewModel @Inject constructor(
    private val romsRepository: RomsRepository,
    private val settingsRepository: SettingsRepository,
    private val romIconProvider: RomIconProvider,
    private val uriPermissionManager: UriPermissionManager,
    private val directoryAccessValidator: DirectoryAccessValidator,
    private val dsiNandManager: DSiNandManager,
    retroAchievementsRepository: RetroAchievementsRepository,
    private val boxArtRepository: me.magnum.melonds.ui.romlist.boxart.BoxArtRepository,
) : ViewModel() {

    private val _searchQuery = MutableStateFlow("")

    private val _boxArtByUri = MutableStateFlow<Map<String, String>>(emptyMap())

    val boxArtByUri: StateFlow<Map<String, String>> = _boxArtByUri.asStateFlow()
    val romCoversEnabled = settingsRepository.observeRomCoverEnabled()
        .stateIn(viewModelScope, SharingStarted.Eagerly, settingsRepository.isRomCoverEnabled())
    private val boxArtRequestsInFlight = mutableSetOf<String>()
    private val boxArtSemaphore = kotlinx.coroutines.sync.Semaphore(4)

    suspend fun prepareRom(rom: Rom): Rom? {
        return if (rom.isInstalledDsiWareShortcut) rom else romsRepository.getRomAtUri(rom.uri)
    }

    fun requestBoxArt(rom: Rom) {
        if (!settingsRepository.isRomCoverEnabled()) return
        if (rom.config.iconSource != RomIconSource.DEFAULT) return
        val key = rom.uri.toString()
        if (_boxArtByUri.value.containsKey(key)) return
        synchronized(boxArtRequestsInFlight) {
            if (!boxArtRequestsInFlight.add(key)) return
        }
        viewModelScope.launch(Dispatchers.IO) {
            val url = boxArtSemaphore.withPermit {
                runCatching { boxArtRepository.getBoxArtUrl(rom) }.getOrNull()
            }
            _boxArtByUri.update { it + (key to (url ?: "")) }
            synchronized(boxArtRequestsInFlight) {
                boxArtRequestsInFlight.remove(key)
            }
        }
    }
    private val _sortingMode = MutableStateFlow(settingsRepository.getRomSortingMode())
    private val _sortingOrder = MutableStateFlow(settingsRepository.getRomSortingOrder())
    private val _filter = MutableStateFlow(RomFilter.ALL)

    val viewMode: StateFlow<RomViewMode> = settingsRepository.observeRomViewMode()
        .stateIn(viewModelScope, SharingStarted.Eagerly, settingsRepository.getRomViewMode())

    val confirmedAchievementHashes: StateFlow<Set<String>> = retroAchievementsRepository.observeKnownAchievementHashes()
        .map { it.toSet() }
        .stateIn(viewModelScope, SharingStarted.Eagerly, emptySet())

    private val _hasSearchDirectories = SubjectSharedFlow<Boolean>()
    val hasSearchDirectories: Flow<Boolean> = _hasSearchDirectories

    private val _invalidDirectoryAccessEvent = EventSharedFlow<Unit>()
    val invalidDirectoryAccessEvent: Flow<Unit> = _invalidDirectoryAccessEvent
    private val _romDirectoryPermissionMissingEvent = EventSharedFlow<Unit>()
    val romDirectoryPermissionMissingEvent: Flow<Unit> = _romDirectoryPermissionMissingEvent

    val onRomIconFilteringChanged = settingsRepository.observeRomIconFiltering()

    val romScanningStatus = romsRepository.getRomScanningStatus()

    private val romsWithParents = MutableStateFlow<List<RomWithParent>>(emptyList())
    private val installedDsiWareShortcuts = MutableStateFlow<List<RomWithParent>>(emptyList())

    val raCoverByUri: StateFlow<Map<String, String>> = combine(
        retroAchievementsRepository.observeRomCoverIcons(),
        settingsRepository.observeRaCoverEnabled(),
        romsWithParents,
        installedDsiWareShortcuts,
        romCoversEnabled,
    ) { covers, enabled, roms, installed, useCoverArt ->
        (roms + installed).mapNotNull { entry ->
            val rom = entry.rom
            covers[rom.retroAchievementsHash]
                ?.takeIf { rom.config.iconSource.resolve(useCoverArt).usesRetroAchievements(enabled, it.hasAchievements) }
                ?.let { rom.uri.toString() to it.url }
        }.toMap()
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyMap())
    private val rootDirectories = MutableStateFlow<List<RootDirectory>>(emptyList())
    private val navigationStack = MutableStateFlow<List<BrowserLocation>>(listOf(BrowserLocation.VirtualRoot))
    private val _browserState = MutableStateFlow(
        RomBrowserUiState(
            entries = emptyList(),
            breadcrumbs = emptyList(),
            canNavigateUp = false,
            isSearchActive = false,
            isAtVirtualRoot = true
        )
    )
    val browserState = _browserState.asStateFlow()
    private val _directoryStatusUi = MutableStateFlow<List<DirectoryCacheStatusUi>>(emptyList())
    val directoryStatusUi = _directoryStatusUi.asStateFlow()
    private val reportedUnavailableDirectories = mutableSetOf<String>()

    init {
        viewModelScope.launch {
            settingsRepository.observeRomSearchDirectories()
                .distinctUntilChanged()
                .collect { directories ->
                    _hasSearchDirectories.tryEmit(directories.isNotEmpty())

                    val roots = directories.mapNotNull { directory ->
                        runCatching {
                            val docId = DocumentsContract.getTreeDocumentId(directory)
                            RootDirectory(
                                uri = directory,
                                docId = docId,
                                displayName = extractDirectoryDisplayName(docId),
                                relativePath = extractRelativePath(docId)
                            )
                        }.getOrNull()
                    }

                    rootDirectories.value = roots
                    ensureNavigationStackForRoots(roots)
                }
        }

        viewModelScope.launch {
            romsRepository.getRoms().collect { romList ->
                val romsWithDocIds = withContext(Dispatchers.Default) {
                    romList.map { rom ->
                        val parentDocId = rom.parentTreeUri?.let { runCatching { DocumentsContract.getDocumentId(it) }.getOrNull() }
                        buildRomWithParent(rom, parentDocId)
                    }
                }
                romsWithParents.value = romsWithDocIds
            }
        }

        // Sort runs only when ROMs or sort settings change. Cached separately so toggling a filter
        // or typing in the search bar doesn't re-sort the entire 15k+ ROM library each keystroke.
        val sortedRomsFlow = combine(
            romsWithParents,
            installedDsiWareShortcuts,
            _sortingMode,
            _sortingOrder,
        ) { roms, shortcuts, mode, order ->
            withContext(Dispatchers.Default) { sortRoms(roms + shortcuts, mode, order) to mode }
        }.distinctUntilChanged()

        @OptIn(FlowPreview::class)
        val debouncedQuery = _searchQuery
            .debounce(200L)
            .distinctUntilChanged()

        viewModelScope.launch {
            combine(
                sortedRomsFlow,
                debouncedQuery,
                navigationStack,
                rootDirectories,
                _filter,
                viewMode,
            ) { values: Array<Any?> ->
                @Suppress("UNCHECKED_CAST")
                val sortedPair = values[0] as Pair<List<RomWithParent>, SortingMode>
                buildBrowserState(
                    sortedPair.first,
                    values[1] as String,
                    values[2] as List<BrowserLocation>,
                    values[3] as List<RootDirectory>,
                    sortedPair.second,
                    _sortingOrder.value,
                    values[4] as RomFilter,
                    values[5] as RomViewMode,
                )
            }.collect { state ->
                _browserState.value = state
            }
        }

        viewModelScope.launch {
            combine(
                rootDirectories,
                romsRepository.observeRomDirectoryScanStatuses()
            ) { roots, statuses ->
                statuses
                    .filter { it.result == RomDirectoryScanStatus.ScanResult.NOT_SCANNED }
                    .map { it.directoryUri.toString() }
                    .filter { reportedUnavailableDirectories.add(it) }
                    .takeIf { it.isNotEmpty() }
                    ?.let { _romDirectoryPermissionMissingEvent.tryEmit(Unit) }
                buildDirectoryStatusUi(roots, statuses)
            }.collect { statusUi ->
                _directoryStatusUi.value = statusUi
            }
        }
    }

    fun refreshRoms() {
        refreshInstalledDsiWareShortcuts()
        romsRepository.rescanRoms()
    }

    fun setRomLastPlayedNow(rom: Rom) {
        romsRepository.setRomLastPlayed(rom, Calendar.getInstance().time)
    }

    fun setRomSearchQuery(query: String?) {
        _searchQuery.value = query.orEmpty()
    }

    fun openFolder(docId: String) {
        val stack = navigationStack.value.toMutableList()
        val lastLocation = stack.lastOrNull()
        if (lastLocation is BrowserLocation.Directory && lastLocation.docId == docId) {
            return
        }
        stack.add(BrowserLocation.Directory(docId))
        navigationStack.value = stack
    }

    fun navigateUp() {
        val stack = navigationStack.value
        if (stack.size <= 1) {
            return
        }
        navigationStack.value = stack.dropLast(1)
    }

    fun setRomSorting(sortingMode: SortingMode) {
        if (sortingMode == _sortingMode.value) {
            val newSortingOrder = if (_sortingOrder.value == SortingOrder.ASCENDING)
                SortingOrder.DESCENDING
            else
                SortingOrder.ASCENDING

            settingsRepository.setRomSortingOrder(_sortingOrder.value)
            _sortingOrder.value = newSortingOrder
        } else {
            settingsRepository.setRomSortingMode(sortingMode)
            settingsRepository.setRomSortingOrder(sortingMode.defaultOrder)

            _sortingMode.value = sortingMode
            _sortingOrder.value = sortingMode.defaultOrder
        }
    }

    fun setFilter(filter: RomFilter) {
        _filter.value = filter
    }

    fun cycleFilter(forward: Boolean) {
        val order = RomFilter.values()
        val idx = order.indexOf(_filter.value).coerceAtLeast(0)
        val next = if (forward) (idx + 1) % order.size else (idx - 1 + order.size) % order.size
        _filter.value = order[next]
    }

    fun toggleViewMode() {
        val next = when (viewMode.value) {
            RomViewMode.GRID -> RomViewMode.LIST
            RomViewMode.LIST -> RomViewMode.GRID
        }
        settingsRepository.setRomViewMode(next)
    }

    fun toggleFavorite(rom: Rom) {
        romsRepository.setRomFavorite(rom, !rom.isFavorite)
    }

    fun addRomSearchDirectory(directoryUri: Uri) {
        val accessValidationResult = directoryAccessValidator.getDirectoryAccessForPermission(directoryUri, Permission.READ_WRITE)

        if (accessValidationResult == DirectoryAccessValidator.DirectoryAccessResult.OK) {
            uriPermissionManager.persistDirectoryPermissions(directoryUri, Permission.READ_WRITE)
            settingsRepository.addRomSearchDirectory(directoryUri)
        } else {
            _invalidDirectoryAccessEvent.tryEmit(Unit)
        }
    }

    suspend fun getRomIcon(rom: Rom): RomIcon {
        val romIconBitmap = romIconProvider.getRomIcon(rom)
        val iconFiltering = settingsRepository.getRomIconFiltering()
        return RomIcon(romIconBitmap, iconFiltering)
    }

    private suspend fun buildBrowserState(
        sortedRoms: List<RomWithParent>,
        query: String,
        navigationStack: List<BrowserLocation>,
        roots: List<RootDirectory>,
        sortingMode: SortingMode,
        sortingOrder: SortingOrder,
        filter: RomFilter,
        viewMode: RomViewMode,
    ): RomBrowserUiState = withContext(Dispatchers.Default) {
        val continuePlaying = sortedRoms
            .mapNotNull { it.rom.takeIf { rom -> rom.lastPlayed != null } }
            .sortedByDescending { it.lastPlayed }
            .take(10)

        if (roots.isEmpty()) {
            return@withContext RomBrowserUiState(
                entries = emptyList(),
                breadcrumbs = emptyList(),
                canNavigateUp = false,
                isSearchActive = query.isNotEmpty(),
                isAtVirtualRoot = true,
                viewMode = viewMode,
                filter = filter,
                sortingMode = sortingMode,
                sortingOrder = sortingOrder,
                continuePlaying = continuePlaying,
                alphabetIndex = emptyMap(),
            )
        }

        if (query.isNotEmpty()) {
            val filtered = filterRoms(sortedRoms, query).filter { matchesFilter(it.rom, filter) }
            val romEntries = filtered.map { RomBrowserEntry.RomItem(it.rom) }
            return@withContext RomBrowserUiState(
                entries = romEntries,
                breadcrumbs = emptyList(),
                canNavigateUp = false,
                isSearchActive = true,
                isAtVirtualRoot = false,
                viewMode = viewMode,
                filter = filter,
                sortingMode = sortingMode,
                sortingOrder = sortingOrder,
                continuePlaying = continuePlaying,
                alphabetIndex = computeAlphabetIndex(romEntries, sortingMode),
            )
        }

        val directoryNodes = buildDirectoryNodes(sortedRoms, roots)
        val baseLocation = defaultLocationForRoots(roots)
        val effectiveStack = if (navigationStack.isEmpty()) listOf(baseLocation) else navigationStack
        val currentLocation = effectiveStack.lastOrNull() ?: baseLocation
        val isVirtualRoot = currentLocation is BrowserLocation.VirtualRoot

        val showFolders = filter == RomFilter.ALL

        val entries = if (isVirtualRoot) {
            val folders = if (showFolders) {
                roots.sortedBy { it.displayName.lowercase() }
                    .map {
                        RomBrowserEntry.Folder(
                            docId = it.docId,
                            name = it.displayName,
                            relativePath = it.relativePath,
                            isRoot = true
                        )
                    }
            } else {
                emptyList()
            }
            // When filtering, show ALL ROMs (flat) at virtual root so favorites/RA chips work across folders
            if (filter != RomFilter.ALL) {
                sortedRoms.filter { matchesFilter(it.rom, filter) }
                    .map { RomBrowserEntry.RomItem(it.rom) }
            } else {
                folders
            }
        } else {
            val docId = (currentLocation as BrowserLocation.Directory).docId
            val node = directoryNodes[docId] ?: createPlaceholderNode(docId, roots)
            val childFolders = if (showFolders) {
                node.childDirectories
                    .mapNotNull { directoryNodes[it] }
                    .sortedBy { it.displayName.lowercase() }
                    .map {
                        RomBrowserEntry.Folder(
                            docId = it.docId,
                            name = it.displayName,
                            relativePath = it.relativePath,
                            isRoot = it.docId == it.root.docId
                        )
                    }
            } else {
                emptyList()
            }
            val romEntries = sortedRoms
                .filter { matchesFilter(it.rom, filter) }
                .filter {
                    if (filter == RomFilter.ALL) {
                        it.parentDocId == docId
                    } else {
                        // When filtering, show all matching ROMs in the current root, regardless of subfolder
                        it.rom.isInstalledDsiWareShortcut || it.parentDocId?.startsWith(node.root.docId) == true
                    }
                }
                .map { RomBrowserEntry.RomItem(it.rom) }
            childFolders + romEntries
        }

        val breadcrumbs = if (isVirtualRoot) {
            emptyList()
        } else {
            val docId = (currentLocation as BrowserLocation.Directory).docId
            buildBreadcrumbs(docId, directoryNodes, roots)
        }

        RomBrowserUiState(
            entries = entries,
            breadcrumbs = breadcrumbs,
            canNavigateUp = effectiveStack.size > 1,
            isSearchActive = false,
            isAtVirtualRoot = isVirtualRoot,
            viewMode = viewMode,
            filter = filter,
            sortingMode = sortingMode,
            sortingOrder = sortingOrder,
            continuePlaying = continuePlaying,
            alphabetIndex = computeAlphabetIndex(entries, sortingMode),
        )
    }

    private fun matchesFilter(rom: Rom, filter: RomFilter): Boolean {
        return when (filter) {
            RomFilter.ALL -> true
            RomFilter.FAVORITES -> rom.isFavorite
            RomFilter.DS_ONLY -> !rom.isDsiWareTitle
            RomFilter.DSIWARE_ONLY -> rom.isDsiWareTitle
            RomFilter.WITH_RETRO_ACHIEVEMENTS -> rom.retroAchievementsHash.isNotEmpty()
        }
    }

    private fun computeAlphabetIndex(entries: List<RomBrowserEntry>, sortingMode: SortingMode): Map<Char, Int> {
        if (sortingMode != SortingMode.ALPHABETICALLY) return emptyMap()
        // Use insertion-order so the bar mirrors whatever sort order the entries are in
        // (ascending: A..Z..#, descending: #..Z..A) without re-sorting on our own.
        val map = LinkedHashMap<Char, Int>()
        entries.forEachIndexed { index, entry ->
            // Folders aren't part of the alphabet; they get a dedicated folder icon at the top.
            if (entry !is RomBrowserEntry.RomItem) return@forEachIndexed
            val rawName = (entry.rom.config.customName ?: entry.rom.name).trim()
            // NFKD decomposes compatibility chars (e.g. fullwidth Latin → ASCII Latin, ligature
            // 'ﬁ' → 'fi'). Then strip combining marks so accented "Élite" → "Elite" → 'E'.
            val normalized = java.text.Normalizer.normalize(rawName, java.text.Normalizer.Form.NFKD)
                .replace(Regex("\\p{Mn}+"), "")
            val firstRaw = normalized.firstOrNull() ?: return@forEachIndexed
            // Force ASCII uppercase. Both the natural uppercaseChar mapping and a fallback ASCII
            // arithmetic conversion are applied so that any path lands on a..z producing A..Z.
            val asciiUpper = when {
                firstRaw in 'a'..'z' -> ('A' + (firstRaw - 'a'))
                firstRaw in 'A'..'Z' -> firstRaw
                else -> firstRaw.uppercaseChar()
            }
            val key = when {
                asciiUpper in 'A'..'Z' -> asciiUpper
                asciiUpper.isLetter() -> asciiUpper
                else -> '#'
            }
            map.putIfAbsent(key, index)
        }
        return map
    }

    private fun sortRoms(roms: List<RomWithParent>, sortingMode: SortingMode, sortingOrder: SortingOrder): List<RomWithParent> {
        val comparator = when (sortingMode) {
            SortingMode.ALPHABETICALLY -> buildAlphabeticalRomComparator(sortingOrder)
            SortingMode.RECENTLY_PLAYED -> buildRecentlyPlayedRomComparator(sortingOrder)
            SortingMode.MOST_PLAYED -> buildMostPlayedRomComparator(sortingOrder)
        }
        return roms.sortedWith { o1, o2 -> comparator.compare(o1.rom, o2.rom) }
    }

    private fun filterRoms(roms: List<RomWithParent>, query: String): List<RomWithParent> {
        val normalizedQuery = normalizeForSearch(query)
        if (normalizedQuery.isEmpty()) return roms
        return roms.filter { it.searchKey.contains(normalizedQuery) }
    }

    fun refreshInstalledDsiWareShortcuts() {
        viewModelScope.launch {
            installedDsiWareShortcuts.value = loadInstalledDsiWareShortcuts()
        }
    }

    private suspend fun loadInstalledDsiWareShortcuts(): List<RomWithParent> = withContext(Dispatchers.IO) {
        val profile = settingsRepository.getDsiProfile() ?: return@withContext emptyList()
        val configuration = settingsRepository.getEmulatorConfiguration(profile)
        dsiNandManager.withNand(configuration) {
            listTitles().map { title -> buildRomWithParent(title.toInstalledDsiWareRom(profile), null) }
        }.orEmpty()
    }

    private fun DSiWareTitle.toInstalledDsiWareRom(profile: DSiProfile): Rom {
        val titleIdHex = (titleId and 0xFFFFFFFFL).toString(16).padStart(8, '0')
        return Rom(
            name = name,
            developerName = producer,
            fileName = "$titleIdHex.app",
            uri = Rom.installedDsiWareUri(titleId, profile.id),
            parentTreeUri = null,
            config = RomConfig.forDsiWareTitle().copy(dsiProfileId = profile.id),
            lastPlayed = null,
            isDsiWareTitle = true,
            retroAchievementsHash = "",
            installedDsiWareTitleId = titleId and 0xFFFFFFFFL,
            installedDsiWareIcon = icon,
        )
    }

    private fun buildRomWithParent(rom: Rom, parentDocId: String?): RomWithParent {
        val searchKey = normalizeForSearch(rom.config.customName ?: rom.name) +
            "\u0000" + normalizeForSearch(rom.fileName) +
            "\u0000" + normalizeForSearch(rom.developerName)
        return RomWithParent(rom, parentDocId, searchKey)
    }

    private fun buildDirectoryNodes(roms: List<RomWithParent>, roots: List<RootDirectory>): Map<String, DirectoryNode> {
        val nodes = mutableMapOf<String, DirectoryNode>()

        roots.forEach { root ->
            nodes[root.docId] = DirectoryNode(
                root = root,
                docId = root.docId,
                parentDocId = null,
                displayName = root.displayName,
                relativePath = root.relativePath,
                childDirectories = mutableSetOf()
            )
        }

        roms.forEach { rom ->
            val parentDocId = rom.parentDocId ?: return@forEach
            val root = findRootForDocId(parentDocId, roots) ?: return@forEach

            var currentDocId: String? = parentDocId
            while (currentDocId != null) {
                val parentOfCurrent = getParentDocId(currentDocId, root.docId)
                nodes.getOrPut(currentDocId) {
                    DirectoryNode(
                        root = root,
                        docId = currentDocId,
                        parentDocId = parentOfCurrent,
                        displayName = extractDirectoryDisplayName(currentDocId),
                        relativePath = buildRelativePath(root, currentDocId),
                        childDirectories = mutableSetOf()
                    )
                }

                if (parentOfCurrent != null) {
                    val parentNode = nodes.getOrPut(parentOfCurrent) {
                        DirectoryNode(
                            root = root,
                            docId = parentOfCurrent,
                            parentDocId = getParentDocId(parentOfCurrent, root.docId),
                            displayName = extractDirectoryDisplayName(parentOfCurrent),
                            relativePath = buildRelativePath(root, parentOfCurrent),
                            childDirectories = mutableSetOf()
                        )
                    }
                    parentNode.childDirectories.add(currentDocId)
                }

                currentDocId = parentOfCurrent
            }
        }

        return nodes
    }

    private fun ensureNavigationStackForRoots(roots: List<RootDirectory>) {
        val currentStack = navigationStack.value
        val preservedLocations = currentStack.filterIsInstance<BrowserLocation.Directory>()
            .map { it.docId }
            .filter { docId -> roots.any { matchesRoot(docId, it.docId) } }

        val newStack = mutableListOf<BrowserLocation>()
        when {
            roots.isEmpty() -> newStack.add(BrowserLocation.VirtualRoot)
            roots.size == 1 -> {
                if (preservedLocations.isEmpty()) {
                    newStack.add(BrowserLocation.Directory(roots.first().docId))
                } else {
                    preservedLocations.forEach { newStack.add(BrowserLocation.Directory(it)) }
                }
            }
            else -> {
                newStack.add(BrowserLocation.VirtualRoot)
                preservedLocations.forEach { newStack.add(BrowserLocation.Directory(it)) }
            }
        }

        if (newStack.isEmpty()) {
            newStack.add(BrowserLocation.VirtualRoot)
        }

        if (newStack != currentStack) {
            this.navigationStack.value = newStack
        }
    }

    private fun defaultLocationForRoots(roots: List<RootDirectory>): BrowserLocation {
        return if (roots.size == 1) {
            BrowserLocation.Directory(roots.first().docId)
        } else {
            BrowserLocation.VirtualRoot
        }
    }

    private fun createPlaceholderNode(docId: String, roots: List<RootDirectory>): DirectoryNode {
        val root = findRootForDocId(docId, roots) ?: roots.first()
        return DirectoryNode(
            root = root,
            docId = docId,
            parentDocId = getParentDocId(docId, root.docId),
            displayName = extractDirectoryDisplayName(docId),
            relativePath = buildRelativePath(root, docId),
            childDirectories = mutableSetOf()
        )
    }

    private fun buildBreadcrumbs(docId: String, nodes: Map<String, DirectoryNode>, roots: List<RootDirectory>): List<String> {
        val names = mutableListOf<String>()
        val root = findRootForDocId(docId, roots)
        var currentDocId: String? = docId
        while (currentDocId != null) {
            val node = nodes[currentDocId]
            if (node != null) {
                names.add(node.displayName)
                currentDocId = node.parentDocId
            } else {
                names.add(extractDirectoryDisplayName(currentDocId))
                currentDocId = if (root != null) getParentDocId(currentDocId, root.docId) else null
            }
        }
        return names.reversed()
    }

    private fun matchesRoot(docId: String, rootDocId: String): Boolean {
        return docId == rootDocId || docId.startsWith("$rootDocId/")
    }

    private fun findRootForDocId(docId: String, roots: List<RootDirectory>): RootDirectory? {
        return roots.firstOrNull { matchesRoot(docId, it.docId) }
    }

    private fun extractDirectoryDisplayName(docId: String): String {
        val path = extractRelativePath(docId)
        val segment = path.substringAfterLast('/', path)
        return segment.ifEmpty { path.ifEmpty { docId } }
    }

    private fun extractRelativePath(docId: String): String {
        return Uri.decode(docId.substringAfter(':', docId))
    }

    private fun buildRelativePath(root: RootDirectory, docId: String): String {
        val rootPath = extractRelativePath(root.docId)
        val docPath = extractRelativePath(docId)
        if (docId == root.docId) {
            return docPath
        }
        return if (rootPath.isNotEmpty() && docPath.startsWith("$rootPath/")) {
            docPath.removePrefix("$rootPath/")
        } else {
            docPath
        }
    }

    private fun getParentDocId(docId: String, rootDocId: String): String? {
        if (docId == rootDocId) {
            return null
        }
        val separatorIndex = docId.lastIndexOf('/')
        if (separatorIndex == -1) {
            return rootDocId
        }
        val parentDocId = docId.substring(0, separatorIndex)
        return if (parentDocId.length < rootDocId.length) {
            rootDocId
        } else {
            parentDocId
        }
    }

    private fun buildAlphabeticalRomComparator(sortingOrder: SortingOrder): Comparator<Rom> {
        // Case-insensitive comparison so titles like "iCarly" sort with the I letters instead of
        // landing after "Z" (ASCII lowercase 'i' has a higher code than uppercase letters).
        return if (sortingOrder == SortingOrder.ASCENDING) {
            Comparator { o1: Rom, o2: Rom ->
                o1.name.compareTo(o2.name, ignoreCase = true)
            }
        } else {
            Comparator { o1: Rom, o2: Rom ->
                o2.name.compareTo(o1.name, ignoreCase = true)
            }
        }
    }

    private fun buildRecentlyPlayedRomComparator(sortingOrder: SortingOrder): Comparator<Rom> {
        return if (sortingOrder == SortingOrder.ASCENDING) {
            Comparator { o1: Rom, o2: Rom ->
                when {
                    o1.lastPlayed == null && o2.lastPlayed == null -> o1.name.compareTo(o2.name)
                    o1.lastPlayed == null -> -1
                    o2.lastPlayed == null -> 1
                    else -> o1.lastPlayed!!.compareTo(o2.lastPlayed)
                }
            }
        } else {
            Comparator { o1: Rom, o2: Rom ->
                when {
                    o1.lastPlayed == null && o2.lastPlayed == null -> o1.name.compareTo(o2.name)
                    o2.lastPlayed == null -> -1
                    o1.lastPlayed == null -> 1
                    else -> o2.lastPlayed!!.compareTo(o1.lastPlayed)
                }
            }
        }
    }

    private fun buildMostPlayedRomComparator(sortingOrder: SortingOrder): Comparator<Rom> {
        return if (sortingOrder == SortingOrder.ASCENDING) {
            Comparator { o1, o2 -> o1.totalPlayTime.compareTo(o2.totalPlayTime) }
        } else {
            Comparator { o1, o2 -> o2.totalPlayTime.compareTo(o1.totalPlayTime) }
        }
    }


    private fun buildDirectoryStatusUi(
        roots: List<RootDirectory>,
        statuses: List<RomDirectoryScanStatus>
    ): List<DirectoryCacheStatusUi> {
        val statusMap = statuses.associateBy { it.directoryUri.toString() }
        return roots.map { root ->
            val status = statusMap[root.uri.toString()]
            DirectoryCacheStatusUi(
                directoryName = root.displayName,
                lastScanTimestamp = status?.lastScanTimestamp,
                result = status?.result ?: RomDirectoryScanStatus.ScanResult.NOT_SCANNED
            )
        }
    }

    private data class RomWithParent(
        val rom: Rom,
        val parentDocId: String?,
        // Pre-computed (NFD, ASCII-only, lowercase) name + filename for fast search.
        // Avoids re-normalizing 15k+ ROMs on every search keystroke.
        val searchKey: String,
    )

    companion object {
        private val nonAsciiRegex = "[^\\p{ASCII}]".toRegex()

        fun normalizeForSearch(input: String): String {
            return Normalizer.normalize(input, Normalizer.Form.NFD)
                .replace(nonAsciiRegex, "")
                .lowercase()
        }
    }

    private data class RootDirectory(
        val uri: Uri,
        val docId: String,
        val displayName: String,
        val relativePath: String
    )

    private data class DirectoryNode(
        val root: RootDirectory,
        val docId: String,
        val parentDocId: String?,
        val displayName: String,
        val relativePath: String,
        val childDirectories: MutableSet<String>
    )

    private sealed interface BrowserLocation {
        data object VirtualRoot : BrowserLocation
        data class Directory(val docId: String) : BrowserLocation
    }

    data class DirectoryCacheStatusUi(
        val directoryName: String,
        val lastScanTimestamp: Long?,
        val result: RomDirectoryScanStatus.ScanResult
    )
}
