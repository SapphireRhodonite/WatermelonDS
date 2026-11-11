package me.magnum.melonds.multiplayer

import android.content.Context
import android.content.pm.PackageManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.SocketTimeoutException
import java.util.concurrent.ConcurrentHashMap

object MultiplayerNearby {
    const val OWN_APP = "WatermelonDS"
    const val OTHER_APP = "SeedlessDS"

    private data class Announced(val room: MultiplayerViewModel.Nearby, val seen: Long)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val mutableRooms = MutableStateFlow<List<MultiplayerViewModel.Nearby>>(emptyList())
    private val leases = mutableMapOf<Any, Boolean>()
    private val announced = ConcurrentHashMap<String, Announced>()
    private var directRooms: List<MultiplayerDirectNetwork.Room> = emptyList()
    private var listenJob: Job? = null
    private var searchJob: Job? = null

    val rooms: StateFlow<List<MultiplayerViewModel.Nearby>> = mutableRooms.asStateFlow()

    fun acquire(context: Context, active: Boolean): Any {
        val lease = Any()
        leases[lease] = active
        resume(context)
        return lease
    }

    fun release(lease: Any?) {
        if (lease == null || leases.remove(lease) == null) return
        if (leases.values.none { it }) stopSearch()
        if (leases.isEmpty()) {
            listenJob?.cancel()
            listenJob = null
            announced.clear()
            merge()
        }
    }

    fun resume(context: Context) {
        val application = context.applicationContext
        if (leases.isNotEmpty() && listenJob?.isActive != true) listenJob = scope.launch { listen(application) }
        if (leases.values.any { it } && searchJob?.isActive != true) searchJob = scope.launch { search(application) }
    }

    suspend fun pause() {
        searchJob?.cancelAndJoin()
        searchJob = null
        directRooms = emptyList()
    }

    private fun stopSearch() {
        searchJob?.cancel()
        searchJob = null
        directRooms = emptyList()
        merge()
    }

    private suspend fun listen(context: Context) {
        val socket = withContext(Dispatchers.IO) {
            runCatching {
                DatagramSocket(null).apply {
                    reuseAddress = true
                    broadcast = true
                    soTimeout = LISTEN_TIMEOUT_MS
                    bind(InetSocketAddress(MultiplayerRoom.ANNOUNCE_PORT))
                }
            }.onFailure { android.util.Log.w("MultiplayerNearby", "Room announcements unavailable", it) }.getOrNull()
        } ?: return
        try {
            val buffer = ByteArray(MAX_PACKET)
            while (true) {
                val packet = DatagramPacket(buffer, buffer.size)
                val received = withContext(Dispatchers.IO) {
                    try {
                        socket.receive(packet)
                        true
                    } catch (_: SocketTimeoutException) {
                        false
                    }
                }
                if (received) {
                    runCatching { parse(context, String(packet.data, 0, packet.length, Charsets.UTF_8)) }
                        .getOrNull()?.let { announced[it.key] = Announced(it, System.currentTimeMillis()) }
                }
                val now = System.currentTimeMillis()
                announced.entries.removeAll { now - it.value.seen > ANNOUNCE_EXPIRY_MS }
                merge()
            }
        } finally {
            withContext(NonCancellable + Dispatchers.IO) { socket.close() }
        }
    }

    private fun parse(context: Context, text: String): MultiplayerViewModel.Nearby? {
        val message = JSONObject(text)
        if (message.optString("t") != "announce" || message.optInt("v") != 1) return null
        val info = MultiplayerRoom.Info.fromJson(message.getJSONObject("room"))
        if (info.code.isEmpty() || info.code == MultiplayerRoom.state.value?.info?.code) return null
        val password = message.optString("password")
        val permitted = MultiplayerDirectNetwork.supported(context) &&
            context.checkSelfPermission(MultiplayerDirectNetwork.permission()) == PackageManager.PERMISSION_GRANTED
        if (!info.direct || !permitted) return null
        val target = target(info.network, password, info.port) ?: return null
        return MultiplayerViewModel.Nearby(
            key = info.code,
            code = info.code,
            host = info.host,
            players = message.optInt("players", 1),
            maximum = info.maximum,
            play = info.play,
            game = info.game?.title.orEmpty(),
            direct = true,
            target = target,
            app = info.app,
        )
    }

    private fun target(network: String?, password: String?, port: Int): MultiplayerRoom.Target? {
        if (network.isNullOrBlank() || password.isNullOrEmpty() || port !in 1..65535) return null
        return MultiplayerRoom.Target(network, password, port)
    }

    private suspend fun search(context: Context) {
        if (!MultiplayerDirectNetwork.supported(context) ||
            context.checkSelfPermission(MultiplayerDirectNetwork.permission()) != PackageManager.PERMISSION_GRANTED) return
        MultiplayerDirectNetwork.rooms(context).collect {
            directRooms = it
            merge()
        }
    }

    private fun merge() {
        val result = announced.values.associateTo(LinkedHashMap()) { it.room.key to it.room }
        for (room in directRooms) {
            val record = room.record
            val code = record["c"].orEmpty()
            if (code.isEmpty()) continue
            val target = target(record["s"], record["p"], record["o"]?.toIntOrNull() ?: MultiplayerRoom.PORT) ?: continue
            result.remove(code)
            result[code] = MultiplayerViewModel.Nearby(
                key = code,
                code = code,
                host = MultiplayerProfile(record["n"].orEmpty(), "", record["k"]?.toIntOrNull()?.coerceIn(0, 15) ?: 0,
                    record["y"].orEmpty().takeIf { it in MultiplayerProfile.SYMBOLS }.orEmpty(), 1, 1),
                players = record["u"]?.toIntOrNull() ?: 1,
                maximum = record["x"]?.toIntOrNull() ?: 16,
                play = MultiplayerRoom.Play.of(record["m"]),
                game = record["g"].orEmpty(),
                direct = true,
                target = target,
                app = record["w"] ?: OWN_APP,
            )
        }
        mutableRooms.value = result.values.toList()
    }

    private const val LISTEN_TIMEOUT_MS = 2000
    private const val ANNOUNCE_EXPIRY_MS = 20_000L
    private const val MAX_PACKET = 8192
}
