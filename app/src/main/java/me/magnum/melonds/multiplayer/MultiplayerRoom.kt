package me.magnum.melonds.multiplayer

import android.content.Context
import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.FileDescriptor
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.security.SecureRandom
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

object MultiplayerRoom {
    const val PORT = 7065
    const val ANNOUNCE_PORT = 7066
    const val CODE_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"

    enum class Play(val id: String) {
        ROM("rom"), DOWNLOAD("download"), CHAT("chat");

        companion object {
            fun of(id: String?): Play = entries.firstOrNull { it.id == id } ?: ROM
        }
    }

    data class Game(val title: String, val code: String, val file: String)

    data class Member(val key: String, val profile: MultiplayerProfile, val hasGame: Boolean, val address: String = "")

    data class Info(
        val code: String,
        val host: MultiplayerProfile,
        val play: Play,
        val game: Game?,
        val maximum: Int,
        val direct: Boolean,
        val network: String,
        val password: String,
        val address: String,
        val port: Int,
        val members: List<Member>,
        val generation: Int,
        val app: String = MultiplayerNearby.OWN_APP,
    ) {
        fun toJson(): JSONObject = JSONObject()
            .put("code", code)
            .put("host", host.toJson())
            .put("play", play.id)
            .put("game", game?.let { JSONObject().put("title", it.title).put("code", it.code).put("file", it.file) })
            .put("maximum", maximum)
            .put("direct", direct)
            .put("network", network)
            .put("address", address)
            .put("port", port)
            .put("generation", generation)
            .put("app", app)
            .put("members", JSONArray().apply {
                members.forEach { put(it.profile.toJson().put("key", it.key).put("has_game", it.hasGame).put("address", it.address)) }
            })

        companion object {
            fun fromJson(value: JSONObject): Info = Info(
                code = value.optString("code"),
                host = MultiplayerProfile.fromJson(value.optJSONObject("host") ?: JSONObject()),
                play = Play.of(value.optString("play")),
                game = value.optJSONObject("game")?.let {
                    Game(it.optString("title"), it.optString("code"), it.optString("file"))
                },
                maximum = value.optInt("maximum", 16).coerceIn(2, 16),
                direct = value.optBoolean("direct"),
                network = value.optString("network"),
                password = "",
                address = value.optString("address"),
                port = value.optInt("port", PORT),
                members = value.optJSONArray("members")?.let { array ->
                    (0 until array.length()).mapNotNull { index ->
                        array.optJSONObject(index)?.let {
                            Member(it.optString("key"), MultiplayerProfile.fromJson(it), it.optBoolean("has_game"), it.optString("address"))
                        }
                    }
                }.orEmpty(),
                generation = value.optInt("generation"),
                app = value.optString("app").ifEmpty { MultiplayerNearby.OWN_APP },
            )
        }
    }

    enum class Role { HOST, GUEST }

    data class State(val role: Role, val info: Info?, val linked: Boolean, val closed: Boolean)

    data class Target(val network: String, val password: String, val port: Int)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val operations = Mutex()
    private val mutableState = MutableStateFlow<State?>(null)
    private val mutableStarts = MutableSharedFlow<Info>(extraBufferCapacity = 4)
    private val clients = ConcurrentHashMap<Int, Peer>()
    private val nextKey = AtomicInteger()
    @Volatile private var server: FileDescriptor? = null
    @Volatile private var guest: Socket? = null
    @Volatile private var room: CoroutineScope? = null
    private var localProfile: MultiplayerProfile? = null
    private var localHasGame = false

    val state: StateFlow<State?> = mutableState.asStateFlow()
    val starts: SharedFlow<Info> = mutableStarts.asSharedFlow()

    private class Peer(val fd: FileDescriptor, val address: String) {
        val output = FileOutputStream(fd)
        @Volatile var member: Member? = null
        val lock = Any()
    }

    fun newCode(): String {
        val random = SecureRandom()
        return (1..4).map { CODE_ALPHABET[random.nextInt(CODE_ALPHABET.length)] }.joinToString("")
    }

    suspend fun host(context: Context, profile: MultiplayerProfile, play: Play, game: Game?, maximum: Int): Int =
        operations.withLock {
            closeChannel()
            mutableState.value = null
            val code = newCode()
            val result = MultiplayerSession.start(context, profile.nickname, maximum.coerceIn(2, 16), code)
            if (result != 0) return@withLock result
            try {
                val network = checkNotNull(MultiplayerDirectNetwork.current)
                val (listener, port) = withContext(Dispatchers.IO) {
                    val fd = Os.socket(OsConstants.AF_INET, OsConstants.SOCK_STREAM, 0)
                    try {
                        ParcelFileDescriptor.dup(fd).use { bindInterface(it, network.route) }
                        Os.setsockoptInt(fd, OsConstants.SOL_SOCKET, OsConstants.SO_REUSEADDR, 1)
                        try {
                            Os.bind(fd, network.route.local, PORT)
                        } catch (error: ErrnoException) {
                            if (error.errno != OsConstants.EADDRINUSE) throw error
                            Os.bind(fd, network.route.local, 0)
                        }
                        Os.listen(fd, 16)
                        val port = (Os.getsockname(fd) as InetSocketAddress).port
                        server = fd
                        fd to port
                    } catch (error: Throwable) {
                        Os.close(fd)
                        throw error
                    }
                }
                localProfile = profile
                val info = Info(code, profile, play, game, maximum.coerceIn(2, 16), true,
                    network.name, network.password, network.route.owner.hostAddress.orEmpty(), port,
                    listOf(Member(HOST_KEY, profile, game != null)), 0)
                mutableState.value = State(Role.HOST, info, true, false)
                val scope = open()
                scope.launch { accept(listener) }
                scope.launch { announce() }
                advertise()
                0
            } catch (error: Throwable) {
                withContext(NonCancellable) {
                    closeChannel()
                    mutableState.value = null
                    MultiplayerSession.stop()
                }
                throw error
            }
        }

    private fun bindInterface(fd: ParcelFileDescriptor, route: MultiplayerDirectNetwork.Route) {
        check(MultiplayerNative.bindInterface(fd.fd, route.interfaceIndex)) { "Wi-Fi Direct interface unavailable" }
    }

    suspend fun join(context: Context, profile: MultiplayerProfile, target: Target, hasGame: Boolean,
        onNetwork: () -> Unit = {}): Int =
        operations.withLock {
            closeChannel()
            mutableState.value = null
            val route = MultiplayerDirectNetwork.join(context, target.network, target.password)
            val result = try {
                onNetwork()
                MultiplayerSession.start(context, profile.nickname, guestRoute = route)
            } catch (error: Throwable) {
                withContext(NonCancellable) { MultiplayerDirectNetwork.leave() }
                throw error
            }
            if (result != 0) {
                withContext(NonCancellable) { MultiplayerDirectNetwork.leave() }
                return@withLock result
            }
            localProfile = profile
            localHasGame = hasGame
            mutableState.value = State(Role.GUEST, null, false, false)
            open().launch { follow(route, target.port) }
            0
        }

    suspend fun leave(): Int = operations.withLock {
        try {
            MultiplayerSession.stop()
        } finally {
            if (!MultiplayerSession.active) withContext(NonCancellable) {
                try {
                    closeChannel()
                } finally {
                    mutableState.value = null
                }
            }
        }
    }

    fun update(play: Play, game: Game?) {
        val current = mutableState.value ?: return
        if (current.role != Role.HOST || current.info == null) return
        val members = current.info.members.map { if (it.key == HOST_KEY) it.copy(hasGame = game != null) else it }
        mutableState.value = current.copy(info = current.info.copy(play = play, game = game, members = members))
        scope.launch {
            broadcast()
            advertise()
        }
    }

    fun profile(profile: MultiplayerProfile, hasGame: Boolean) {
        localProfile = profile
        localHasGame = hasGame
        val current = mutableState.value ?: return
        when (current.role) {
            Role.HOST -> {
                val info = current.info ?: return
                val members = info.members.map { if (it.key == HOST_KEY) Member(HOST_KEY, profile, it.hasGame) else it }
                mutableState.value = current.copy(info = info.copy(host = profile, members = members))
                scope.launch {
                    broadcast()
                    advertise()
                }
            }
            Role.GUEST -> scope.launch { runCatching { guest?.let { send(it.getOutputStream(), hello()) } } }
        }
    }

    fun start() {
        val current = mutableState.value ?: return
        val info = current.info ?: return
        if (current.role != Role.HOST) return
        val next = info.copy(generation = info.generation + 1)
        mutableState.value = current.copy(info = next)
        scope.launch {
            broadcast()
            val line = JSONObject().put("t", "start").put("room", next.toJson()).toString()
            clients.values.filter { it.member != null }.forEach { write(it, line) }
        }
    }

    fun players(count: Int) {
        val current = mutableState.value ?: return
        if (current.role == Role.HOST && count != lastAdvertisedPlayers) {
            lastAdvertisedPlayers = count
            scope.launch { advertise() }
        }
    }

    @Volatile private var lastAdvertisedPlayers = 1

    private suspend fun advertise() {
        val info = mutableState.value?.takeIf { it.role == Role.HOST }?.info ?: return
        if (!info.direct) return
        val record = mapOf(
            "v" to "1",
            "c" to info.code,
            "n" to info.host.nickname,
            "k" to info.host.colour.toString(),
            "y" to info.host.symbol,
            "s" to info.network,
            "p" to info.password,
            "a" to info.address,
            "o" to info.port.toString(),
            "m" to info.play.id,
            "g" to info.game?.title.orEmpty().take(48),
            "u" to lastAdvertisedPlayers.toString(),
            "x" to info.maximum.toString(),
            "w" to info.app,
        ).filterValues { it.isNotEmpty() }
        runCatching { MultiplayerDirectNetwork.advertise(record) }
            .onFailure { android.util.Log.w(TAG, "Direct room announcement failed", it) }
    }

    private suspend fun announce() {
        val socket = runCatching { DatagramSocket().apply { broadcast = true } }.getOrNull() ?: return
        try {
            while (true) {
                val info = mutableState.value?.takeIf { it.role == Role.HOST }?.info
                val playing = runCatching { MultiplayerSession.status().optBoolean("game") }.getOrDefault(true)
                if (info != null && !playing) {
                    val payload = JSONObject().put("t", "announce").put("v", 1).put("room", info.toJson())
                        .put("players", lastAdvertisedPlayers).put("password", info.password).toString().toByteArray(Charsets.UTF_8)
                    broadcastAddresses().forEach { address ->
                        runCatching { socket.send(DatagramPacket(payload, payload.size, address, ANNOUNCE_PORT)) }
                    }
                }
                delay(ANNOUNCE_MS)
            }
        } finally {
            socket.close()
        }
    }

    private fun broadcastAddresses(): List<InetAddress> = runCatching {
        NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.interfaceAddresses }
            .mapNotNull { it.broadcast }
            .distinct()
    }.getOrDefault(emptyList()).ifEmpty { listOf(InetAddress.getByName("255.255.255.255")) }

    private suspend fun accept(listener: FileDescriptor) {
        while (listener.valid()) {
            val fd = runCatching { Os.accept(listener, null) }.getOrNull() ?: break
            val peer = try {
                Os.setsockoptInt(fd, OsConstants.IPPROTO_TCP, OsConstants.TCP_NODELAY, 1)
                Peer(fd, (Os.getpeername(fd) as InetSocketAddress).address.hostAddress.orEmpty())
            } catch (_: Exception) {
                closeSocket(fd)
                continue
            }
            val key = nextKey.incrementAndGet()
            clients[key] = peer
            val session = room
            if (session == null || server !== listener) {
                clients.remove(key)
                closeSocket(fd)
                break
            }
            session.launch { serve(key, peer) }
        }
    }

    private fun closeSocket(fd: FileDescriptor) {
        synchronized(fd) {
            if (fd.valid()) {
                runCatching { Os.shutdown(fd, OsConstants.SHUT_RDWR) }
                Os.close(fd)
            }
        }
    }

    private suspend fun serve(key: Int, peer: Peer) {
        try {
            val input = FileInputStream(peer.fd).buffered()
            while (true) {
                val line = readLine(input) ?: break
                val message = runCatching { JSONObject(line) }.getOrNull() ?: break
                when (message.optString("t")) {
                    "info" -> {
                        mutableState.value?.info?.let { write(peer, JSONObject().put("t", "room").put("room", it.toJson()).toString()) }
                        break
                    }
                    "hello" -> {
                        val member = Member("p$key", MultiplayerProfile.fromJson(message), message.optBoolean("has_game"),
                            peer.address)
                        peer.member = member
                        mutableState.update { current ->
                            val info = current?.info ?: return@update current
                            current.copy(info = info.copy(members = info.members.filter { it.key != member.key } + member))
                        }
                        broadcast()
                    }
                }
            }
        } catch (error: Exception) {
            if (error is CancellationException) throw error
        } finally {
            clients.remove(key)
            runCatching { closeSocket(peer.fd) }
            if (peer.member != null) {
                mutableState.update { current ->
                    val info = current?.info ?: return@update current
                    current.copy(info = info.copy(members = info.members.filter { it.key != "p$key" }))
                }
                broadcast()
            }
        }
    }

    private fun broadcast() {
        val info = mutableState.value?.takeIf { it.role == Role.HOST }?.info ?: return
        val line = JSONObject().put("t", "room").put("room", info.toJson()).toString()
        clients.values.filter { it.member != null }.forEach { write(it, line) }
    }

    private fun write(peer: Peer, line: String) {
        runCatching {
            synchronized(peer.lock) {
                peer.output.write((line + "\n").toByteArray(Charsets.UTF_8))
                peer.output.flush()
            }
        }.onFailure { runCatching { closeSocket(peer.fd) } }
    }

    private suspend fun follow(route: MultiplayerDirectNetwork.Route, port: Int) {
        while (mutableState.value?.role == Role.GUEST && mutableState.value?.closed == false) {
            val candidate = Socket()
            val socket = try {
                candidate.bind(InetSocketAddress(route.local, 0))
                ParcelFileDescriptor.fromSocket(candidate).use { bindInterface(it, route) }
                candidate.connect(InetSocketAddress(route.owner, port), LINK_TIMEOUT_MS)
                candidate.tcpNoDelay = true
                candidate
            } catch (error: Exception) {
                candidate.close()
                if (error is CancellationException) throw error
                null
            }
            if (socket != null) {
                guest = socket
                try {
                    send(socket.getOutputStream(), hello())
                    mutableState.update { it?.copy(linked = true) }
                    val input = socket.getInputStream().buffered()
                    while (true) {
                        val line = readLine(input) ?: break
                        val message = runCatching { JSONObject(line) }.getOrNull() ?: break
                        when (message.optString("t")) {
                            "room" -> {
                                val info = Info.fromJson(message.getJSONObject("room"))
                                mutableState.update { it?.copy(info = info) }
                            }
                            "start" -> {
                                val info = Info.fromJson(message.getJSONObject("room"))
                                mutableState.update { it?.copy(info = info) }
                                mutableStarts.tryEmit(info)
                            }
                            "close" -> {
                                mutableState.update { it?.copy(closed = true) }
                                break
                            }
                        }
                    }
                } catch (error: Exception) {
                    if (error is CancellationException) throw error
                } finally {
                    runCatching { socket.close() }
                    if (guest === socket) guest = null
                    mutableState.update { it?.copy(linked = false) }
                }
            }
            delay(RETRY_MS)
        }
    }

    private fun hello(): JSONObject {
        val profile = localProfile ?: return JSONObject().put("t", "hello")
        return profile.toJson().put("t", "hello").put("v", 1).put("has_game", localHasGame)
    }

    private suspend fun closeChannel() = withContext(Dispatchers.IO) {
        val listener = server
        server = null
        listener?.let { runCatching { closeSocket(it) } }
        val closing = JSONObject().put("t", "close").toString()
        clients.values.forEach { write(it, closing) }
        clients.values.forEach { runCatching { closeSocket(it.fd) } }
        clients.clear()
        runCatching { guest?.close() }
        guest = null
        room?.let { (it.coroutineContext[Job])?.cancel() }
        room = null
        lastAdvertisedPlayers = 1
    }

    private fun open(): CoroutineScope = CoroutineScope(scope.coroutineContext + SupervisorJob(scope.coroutineContext[Job]))
        .also { room = it }

    private fun send(output: OutputStream, message: JSONObject) {
        output.write((message.toString() + "\n").toByteArray(Charsets.UTF_8))
        output.flush()
    }

    private fun readLine(input: InputStream): String? {
        val buffer = java.io.ByteArrayOutputStream()
        while (true) {
            val value = input.read()
            if (value < 0) return null
            if (value == '\n'.code) return buffer.toString(Charsets.UTF_8.name())
            if (buffer.size() >= MAX_LINE) return null
            buffer.write(value)
        }
    }

    private const val TAG = "MultiplayerRoom"
    const val HOST_KEY = "host"
    private const val MAX_LINE = 8192
    private const val LINK_TIMEOUT_MS = 3000
    private const val RETRY_MS = 2000L
    private const val ANNOUNCE_MS = 8000L
}
