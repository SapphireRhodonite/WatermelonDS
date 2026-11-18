package me.magnum.melonds.multiplayer

import android.content.Context
import android.content.pm.PackageManager
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.ConfigurationDirResult
import me.magnum.melonds.domain.model.RomInfo
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.common.romprocessors.RomFileProcessorFactory
import me.magnum.melonds.domain.repositories.RomsRepository
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.domain.services.ConfigurationDirectoryVerifier
import me.magnum.melonds.domain.model.rom.config.RomIconSource
import me.magnum.melonds.ui.romlist.boxart.BoxArtRepository
import org.json.JSONObject
import javax.inject.Inject

@HiltViewModel
class MultiplayerViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val romsRepository: RomsRepository,
    private val verifier: ConfigurationDirectoryVerifier,
    private val processors: RomFileProcessorFactory,
    private val settingsRepository: SettingsRepository,
    private val boxArtRepository: BoxArtRepository,
) : ViewModel() {

    data class Nearby(
        val key: String,
        val code: String,
        val host: MultiplayerProfile,
        val players: Int,
        val maximum: Int,
        val play: MultiplayerRoom.Play?,
        val game: String,
        val direct: Boolean,
        val target: MultiplayerRoom.Target,
        val app: String = MultiplayerNearby.OWN_APP,
    )

    data class Seat(
        val key: String,
        val profile: MultiplayerProfile,
        val status: Int,
        val ping: Int,
        val local: Boolean,
        val host: Boolean,
        val known: Boolean,
    )

    sealed class Failure {
        data class Message(val text: Int) : Failure()
        data class Direct(val room: Nearby) : Failure()
    }

    private val mutableProfile = MutableStateFlow(MultiplayerProfile.load(context))
    private val mutableStatus = MutableStateFlow(JSONObject())
    private val mutableBios = MutableStateFlow(false)
    private val mutableBusy = MutableStateFlow(false)
    private val mutableFailure = MutableStateFlow<Failure?>(null)
    private val mutableJoining = MutableStateFlow<Nearby?>(null)
    private val mutableJoinStep = MutableStateFlow(0)
    private val catalog by lazy { MultiplayerCatalog(context.resources) }
    private val mutableCovers = MutableStateFlow<Map<String, String>>(emptyMap())
    private val coverRequests = mutableSetOf<String>()
    private var lease: Any? = null

    val profile: StateFlow<MultiplayerProfile> = mutableProfile.asStateFlow()
    val status: StateFlow<JSONObject> = mutableStatus.asStateFlow()
    val bios: StateFlow<Boolean> = mutableBios.asStateFlow()
    val nearby: StateFlow<List<Nearby>> = MultiplayerNearby.rooms
    val busy: StateFlow<Boolean> = mutableBusy.asStateFlow()
    val failure: StateFlow<Failure?> = mutableFailure.asStateFlow()
    val joining: StateFlow<Nearby?> = mutableJoining.asStateFlow()
    val joinStep: StateFlow<Int> = mutableJoinStep.asStateFlow()
    val room = MultiplayerRoom.state
    val starts = MultiplayerRoom.starts
    val roms: StateFlow<List<Rom>> = romsRepository.getRoms()
        .map { list ->
            val roots = settingsRepository.getRomSearchDirectories().map { "$it/document/" }
            list.filter { rom ->
                !rom.isDsiWareTitle && rom.unsupportedReason == null && roots.any { rom.uri.toString().startsWith(it) }
            }.sortedBy { (it.config.customName ?: it.name).lowercase() }
        }
        .flowOn(Dispatchers.IO)
        .stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    val covers: StateFlow<Map<String, String>> = mutableCovers.asStateFlow()
    val page = MutableStateFlow(MultiplayerPage.HUB)
    val focus = MutableStateFlow<MultiplayerFocus?>(null)

    fun requestCover(rom: Rom) {
        if (!settingsRepository.isRomCoverEnabled() || rom.config.iconSource != RomIconSource.DEFAULT) return
        val key = rom.uri.toString()
        if (mutableCovers.value.containsKey(key) || !coverRequests.add(key)) return
        viewModelScope.launch(Dispatchers.IO) {
            val url = runCatching { boxArtRepository.getBoxArtUrl(rom) }.getOrNull()
            mutableCovers.update { it + (key to url.orEmpty()) }
        }
    }

    val directSupported: Boolean = MultiplayerDirectNetwork.supported(context)

    fun directPermitted(): Boolean =
        context.checkSelfPermission(MultiplayerDirectNetwork.permission()) == PackageManager.PERMISSION_GRANTED

    fun refresh() {
        viewModelScope.launch {
            mutableProfile.value = withContext(Dispatchers.IO) { MultiplayerProfile.load(context) }
            mutableBios.value = withContext(Dispatchers.IO) {
                runCatching { verifier.checkDsConfigurationDirectory().status == ConfigurationDirResult.Status.VALID }.getOrDefault(false)
            }
        }
    }

    suspend fun poll() {
        val value = runCatching { MultiplayerSession.status() }.getOrElse { JSONObject() }
        mutableStatus.value = value
        val connected = players(value).count { it.status == 1 || it.status == 2 }
        MultiplayerRoom.players(connected.coerceAtLeast(1))
    }

    fun search() {
        if (lease == null) lease = MultiplayerNearby.acquire(context, true) else MultiplayerNearby.resume(context)
    }

    fun stopSearch() {
        MultiplayerNearby.release(lease)
        lease = null
    }

    internal suspend fun gameCatalog(): MultiplayerCatalog = withContext(Dispatchers.IO) { catalog }

    fun host(play: MultiplayerRoom.Play, rom: Rom?, maximum: Int, showAll: Boolean, done: (Boolean) -> Unit) {
        if (mutableBusy.value) return
        mutableBusy.value = true
        mutableFailure.value = null
        stopSearch()
        viewModelScope.launch {
            val ok = try {
                val game = resolveHostSelection(play, rom, showAll)
                if (play != MultiplayerRoom.Play.CHAT && game == null) {
                    false
                } else {
                    val result = MultiplayerRoom.host(context, profile.value, play, game, maximum)
                    if (result != 0) mutableFailure.value = Failure.Message(code(result))
                    result == 0
                }
            } catch (error: Exception) {
                if (error is CancellationException && error !is TimeoutCancellationException) throw error
                android.util.Log.e("Multiplayer", "Could not open the room", error)
                mutableFailure.value = Failure.Message(me.magnum.melonds.R.string.multiplayer_direct_failed)
                false
            } finally {
                mutableBusy.value = false
            }
            poll()
            done(ok)
        }
    }

    private var joinJob: Job? = null

    fun cancelJoin(done: () -> Unit) {
        val job = joinJob ?: return done()
        job.cancel()
        viewModelScope.launch {
            job.join()
            runCatching { MultiplayerRoom.leave() }
            mutableBusy.value = false
            mutableJoining.value = null
            poll()
            done()
        }
    }

    fun join(room: Nearby, done: (Boolean) -> Unit) {
        if (mutableBusy.value) return
        mutableBusy.value = true
        mutableFailure.value = null
        mutableJoining.value = room
        mutableJoinStep.value = 1
        joinJob = viewModelScope.launch {
            val ok = try {
                MultiplayerNearby.pause()
                val result = MultiplayerRoom.join(context, profile.value, room.target, false) { mutableJoinStep.value = 2 }
                if (result != 0) mutableFailure.value = Failure.Message(code(result))
                result == 0
            } catch (error: Exception) {
                if (error is CancellationException && error !is TimeoutCancellationException) throw error
                android.util.Log.e("Multiplayer", "Could not join the room", error)
                mutableFailure.value = Failure.Direct(room)
                false
            } finally {
                mutableBusy.value = false
            }
            if (ok) mutableJoinStep.value = 3
            poll()
            done(ok)
        }
    }

    fun leave(done: (Boolean) -> Unit) {
        if (mutableBusy.value) return
        mutableBusy.value = true
        viewModelScope.launch {
            val ok = try {
                val result = MultiplayerRoom.leave()
                if (result != 0) mutableFailure.value = Failure.Message(code(result))
                result == 0
            } catch (error: Exception) {
                if (error is CancellationException && error !is TimeoutCancellationException) throw error
                mutableFailure.value = Failure.Message(me.magnum.melonds.R.string.multiplayer_direct_stop_failed)
                false
            } finally {
                mutableBusy.value = false
            }
            poll()
            done(ok)
        }
    }

    fun permissionDenied() {
        mutableFailure.value = Failure.Message(me.magnum.melonds.R.string.mp_direct_permission)
    }

    fun dismissFailure() {
        mutableFailure.value = null
    }

    fun change(play: MultiplayerRoom.Play, rom: Rom?, showAll: Boolean, done: () -> Unit) {
        if (mutableBusy.value) return
        mutableBusy.value = true
        mutableFailure.value = null
        viewModelScope.launch {
            try {
                val game = resolveHostSelection(play, rom, showAll)
                if (play != MultiplayerRoom.Play.CHAT && game == null) return@launch
                MultiplayerRoom.update(play, game)
                done()
            } finally {
                mutableBusy.value = false
            }
        }
    }

    fun saveProfile(value: MultiplayerProfile, done: (Boolean) -> Unit) {
        viewModelScope.launch {
            val saved = withContext(Dispatchers.IO) { MultiplayerProfile.save(context, value) }
            if (saved) {
                val loaded = withContext(Dispatchers.IO) { MultiplayerProfile.load(context) }
                mutableProfile.value = loaded
                MultiplayerRoom.profile(loaded, false)
            }
            done(saved)
        }
    }

    private suspend fun resolveHostSelection(play: MultiplayerRoom.Play, rom: Rom?, showAll: Boolean): MultiplayerRoom.Game? {
        if (play == MultiplayerRoom.Play.CHAT) return null
        val game = withContext(Dispatchers.IO) {
            val current = roms.value.firstOrNull { it.uri == rom?.uri } ?: return@withContext null
            val id = sourceInfo(current)?.cartridge
            romsRepository.updateCartridge(current, id)
            if (id == null || (rom?.cartridge != null && rom.cartridge != id)) return@withContext null
            if (!showAll) {
                val modes = catalog.lookup(id) {
                    processors.getFileRomProcessorForFileName(current.fileName)
                        ?.getRomFromUri(current.uri, current.parentTreeUri)?.retroAchievementsHash
                        ?: current.retroAchievementsHash
                }
                if ((modes?.players(play) ?: 0) < 2) return@withContext null
            }
            MultiplayerRoom.Game(current.config.customName ?: current.name, id.gameCode, current.fileName)
        }
        if (game == null) mutableFailure.value = Failure.Message(R.string.mp_selection_changed)
        return game
    }

    suspend fun find(game: MultiplayerRoom.Game?): Rom? = withContext(Dispatchers.IO) {
        if (game == null) return@withContext null
        val candidates = roms.value
        val sameName = candidates.filter { it.fileName == game.file }
        if (game.code.isEmpty()) return@withContext sameName.firstOrNull()
        sameName.firstOrNull { sourceInfo(it)?.gameCode == game.code }
            ?: candidates.firstOrNull { it.fileName != game.file && it.cartridge?.gameCode == game.code && sourceInfo(it)?.gameCode == game.code }
            ?: candidates.firstOrNull { it.fileName != game.file && it.cartridge?.gameCode != game.code && sourceInfo(it)?.gameCode == game.code }
    }

    private fun sourceInfo(rom: Rom): RomInfo? = try {
        processors.getFileRomProcessorForFileName(rom.fileName)?.getSourceRomInfo(rom)
    } catch (cancelled: CancellationException) {
        throw cancelled
    } catch (error: Exception) {
        android.util.Log.w("Multiplayer", "Could not read cartridge header", error)
        null
    }

    fun shareGame(hasGame: Boolean) {
        MultiplayerRoom.profile(profile.value, hasGame)
    }

    fun start() = MultiplayerRoom.start()

    fun players(value: JSONObject = mutableStatus.value): List<Seat> {
        val players = value.optJSONArray("players") ?: return emptyList()
        val members = room.value?.info?.members.orEmpty().toMutableList()
        fun take(predicate: (MultiplayerRoom.Member) -> Boolean) = members.firstOrNull(predicate)?.also { members.remove(it) }
        val entries = (0 until players.length()).mapNotNull { players.optJSONObject(it) }
        val claimed = entries.map { player ->
            val address = player.optString("ip")
            when {
                player.optInt("status") == 2 -> take { it.key == MultiplayerRoom.HOST_KEY }
                address.isNotEmpty() -> take { it.key != MultiplayerRoom.HOST_KEY && it.address == address }
                else -> null
            }
        }
        return entries.mapIndexed { index, player ->
            val name = player.optString("name")
            val local = player.optBoolean("local")
            val host = player.optInt("status") == 2
            val member = if (local) null else claimed[index]
                ?: take { it.key != MultiplayerRoom.HOST_KEY && it.profile.nickname == name }
            val profile = when {
                local -> profile.value.copy(nickname = name)
                member != null -> member.profile
                else -> MultiplayerProfile(name, "", nameColour(name), "", 1, 1)
            }
            Seat("${player.optInt("id")}:$name", profile, player.optInt("status"), player.optInt("ping"), local, host,
                local || member != null)
        }
    }

    private fun code(result: Int): Int = when (result) {
        -2 -> me.magnum.melonds.R.string.multiplayer_finish_game
        else -> me.magnum.melonds.R.string.multiplayer_connection_failed
    }

    override fun onCleared() {
        stopSearch()
    }

    companion object {
        private const val PEEK_REFRESH_MS = 5_000L

        fun nameColour(name: String): Int = (name.lowercase().hashCode() and Int.MAX_VALUE) % 16
    }
}
