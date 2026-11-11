package me.magnum.melonds.multiplayer

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.wifi.WifiManager
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pGroup
import android.net.wifi.p2p.WifiP2pInfo
import android.net.wifi.p2p.WifiP2pManager
import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceInfo
import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceRequest
import android.os.Build
import android.os.Looper
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import java.net.Inet4Address
import java.net.NetworkInterface
import java.security.SecureRandom
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

object MultiplayerDirectNetwork {
    data class Route(val owner: Inet4Address, val local: Inet4Address, val interfaceIndex: Int)
    data class Network(val name: String, val password: String, val route: Route)

    data class Room(val device: String, val record: Map<String, String>, val seen: Long)

    private class Owner(val manager: WifiP2pManager, val channel: WifiP2pManager.Channel, val name: String)

    private var owner: Owner? = null
    private var member: Owner? = null
    @Volatile var current: Network? = null
        private set

    fun supported(context: Context): Boolean = Build.VERSION.SDK_INT >= 29 &&
        context.packageManager.hasSystemFeature(PackageManager.FEATURE_WIFI_DIRECT)

    fun permission(): String = if (Build.VERSION.SDK_INT >= 33)
        Manifest.permission.NEARBY_WIFI_DEVICES else Manifest.permission.ACCESS_FINE_LOCATION

    fun checkAvailable(context: Context) {
        check(supported(context)) { "Direct Wi-Fi is unavailable" }
        check(context.checkSelfPermission(permission()) == PackageManager.PERMISSION_GRANTED) { "Wi-Fi permission is required" }
        check((context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager)?.isWifiEnabled == true) {
            "Wi-Fi is disabled"
        }
    }

    suspend fun create(context: Context, code: String = "") = withContext(Dispatchers.Main.immediate) {
        checkAvailable(context)
        check(owner == null)
        val application = context.applicationContext
        val manager = application.getSystemService(Context.WIFI_P2P_SERVICE) as? WifiP2pManager
            ?: error("Direct Wi-Fi is unavailable")
        val channel = manager.initialize(application, Looper.getMainLooper(), null)
            ?: error("Direct Wi-Fi channel is unavailable")
        val random = SecureRandom()
        fun token(size: Int): String = ByteArray(size).also { random.nextBytes(it) }
            .joinToString("") { "%02X".format(it.toInt() and 255) }
        val pending = Owner(manager, channel, "DIRECT-wm-" + code.ifBlank { "Watermelon-" + token(4) })
        val password = token(8)
        owner = pending
        try {
            val preferences = withContext(Dispatchers.IO) {
                application.getSharedPreferences("watermelon_multiplayer", Context.MODE_PRIVATE)
            }
            val previous = group(pending)
            if (previous != null) {
                val previousName = withContext(Dispatchers.IO) { preferences.getString("direct_network_name", null) }
                check(previous.networkName == previousName || roomNetwork(previous.networkName)) {
                    "Another direct Wi-Fi group is already active"
                }
                remove(pending, previous.networkName)
            }
            withContext(Dispatchers.IO) {
                check(preferences.edit().putString("direct_network_name", pending.name).commit()) {
                    "Could not remember the direct network"
                }
            }
            @Suppress("DEPRECATION")
            val frequency = (application.getSystemService(Context.WIFI_SERVICE) as? WifiManager)?.connectionInfo?.frequency ?: -1
            val builder = WifiP2pConfig.Builder().setNetworkName(pending.name).setPassphrase(password)
                .enablePersistentMode(false)
            if (frequency in 4900..5900) builder.setGroupOperatingFrequency(frequency)
            else builder.setGroupOperatingBand(WifiP2pConfig.GROUP_OWNER_BAND_5GHZ)
            val config = builder.build()
            action { manager.createGroup(channel, config, it) }
            withTimeout(20_000) {
                while (true) {
                    val group = group(pending)
                    val connection = connection(pending)
                    val route = route(group, connection, pending.name, true)
                    if (route != null) {
                        current = Network(pending.name, password, route)
                        break
                    }
                    delay(200)
                }
            }
        } catch (error: Throwable) {
            withContext(NonCancellable) {
                runCatching { close(pending) }.onFailure {
                    android.util.Log.e("MultiplayerDirect", "Direct network cleanup failed", it)
                }
            }
            throw error
        }
    }

    suspend fun stop() = withContext(Dispatchers.Main.immediate) {
        owner?.let { close(it) }
    }

    suspend fun advertise(record: Map<String, String>) = withContext(Dispatchers.Main.immediate) {
        val value = owner ?: return@withContext
        action { value.manager.clearLocalServices(value.channel, it) }
        val info = WifiP2pDnsSdServiceInfo.newInstance(value.name, SERVICE_TYPE, record)
        action { value.manager.addLocalService(value.channel, info, it) }
    }

    fun rooms(context: Context): Flow<List<Room>> = callbackFlow {
        val application = context.applicationContext
        val manager = application.getSystemService(Context.WIFI_P2P_SERVICE) as? WifiP2pManager
        val channel = manager?.initialize(application, Looper.getMainLooper(), null)
        if (manager == null || channel == null) {
            close()
            return@callbackFlow
        }
        val found = LinkedHashMap<String, Room>()
        manager.setDnsSdResponseListeners(channel, { _, _, _ -> }) { domain, record, device ->
            android.util.Log.d("MultiplayerDirect", "Direct service response domain=$domain device=${device.deviceAddress} keys=${record.keys}")
            if (domain.contains(SERVICE_TYPE) && record["v"] == "1") {
                found[device.deviceAddress] = Room(device.deviceAddress, record.toMap(), System.currentTimeMillis())
                trySend(found.values.toList())
            }
        }
        val request = WifiP2pDnsSdServiceRequest.newInstance()
        val job = launch(Dispatchers.Main.immediate) {
            try {
                action { manager.addServiceRequest(channel, request, it) }
                while (true) {
                    runCatching { action { manager.discoverServices(channel, it) } }
                        .onFailure { android.util.Log.w("MultiplayerDirect", "Direct room search failed", it) }
                    delay(SCAN_INTERVAL_MS)
                    val now = System.currentTimeMillis()
                    if (found.values.removeAll { now - it.seen > ROOM_EXPIRY_MS }) trySend(found.values.toList())
                }
            } catch (error: Exception) {
                if (error is kotlinx.coroutines.CancellationException) throw error
                android.util.Log.w("MultiplayerDirect", "Direct room search unavailable", error)
            }
        }
        awaitClose {
            job.cancel()
            runCatching {
                manager.clearServiceRequests(channel, null)
                manager.stopPeerDiscovery(channel, null)
                channel.close()
            }
        }
    }

    suspend fun join(context: Context, name: String, password: String): Route = withContext(Dispatchers.Main.immediate) {
        checkAvailable(context)
        check(owner == null && member == null)
        val application = context.applicationContext
        val manager = application.getSystemService(Context.WIFI_P2P_SERVICE) as? WifiP2pManager
            ?: error("Direct Wi-Fi is unavailable")
        val channel = manager.initialize(application, Looper.getMainLooper(), null)
            ?: error("Direct Wi-Fi channel is unavailable")
        val pending = Owner(manager, channel, name)
        member = pending
        try {
            val previous = group(pending)?.takeUnless { !it.isGroupOwner && it.networkName == name }
            if (previous != null) {
                check(roomNetwork(previous.networkName)) { "Another direct Wi-Fi group is already active" }
                remove(pending, previous.networkName)
            }
            group(pending)?.takeIf { !it.isGroupOwner && it.networkName == name } ?: run {
                val config = WifiP2pConfig.Builder().setNetworkName(name).setPassphrase(password).build()
                action { manager.connect(channel, config, it) }
            }
            var joined: Route? = null
            withTimeout(JOIN_TIMEOUT_MS) {
                while (joined == null) {
                    joined = route(group(pending), connection(pending), name, false)
                    if (joined == null) delay(250)
                }
            }
            checkNotNull(joined)
        } catch (error: Throwable) {
            withContext(NonCancellable) {
                runCatching { closeMember(pending) }.onFailure {
                    android.util.Log.e("MultiplayerDirect", "Direct network cleanup failed", it)
                }
            }
            throw error
        }
    }

    suspend fun leave() = withContext(Dispatchers.Main.immediate) {
        member?.let { closeMember(it) }
    }

    private fun route(group: WifiP2pGroup?, info: WifiP2pInfo?, name: String, isOwner: Boolean): Route? {
        if (group == null || group.networkName != name || group.isGroupOwner != isOwner ||
            info?.groupFormed != true || info.isGroupOwner != isOwner) return null
        val ownerAddress = info.groupOwnerAddress as? Inet4Address ?: return null
        val iface = group.`interface` ?: return null
        val networkInterface = NetworkInterface.getByName(iface) ?: return null
        if (networkInterface.index <= 0 || !networkInterface.isUp) return null
        val local = networkInterface.inetAddresses.toList()
            .filterIsInstance<Inet4Address>().firstOrNull { !it.isLoopbackAddress && (it == ownerAddress) == isOwner }
            ?: return null
        return Route(ownerAddress, local, networkInterface.index)
    }

    private suspend fun closeMember(value: Owner) {
        try {
            val group = group(value)
            if (group != null && !group.isGroupOwner && group.networkName == value.name) {
                action { value.manager.removeGroup(value.channel, it) }
                withTimeout(5_000) {
                    while (group(value)?.networkName == value.name) delay(100)
                }
            } else {
                runCatching { action { value.manager.cancelConnect(value.channel, it) } }
            }
        } finally {
            try {
                value.channel.close()
            } finally {
                if (member === value) member = null
            }
        }
    }

    private suspend fun close(value: Owner) {
        try {
            val group = group(value)
            if (group?.isGroupOwner == true && group.networkName == value.name) {
                remove(value, value.name)
            }
        } finally {
            try {
                value.channel.close()
            } finally {
                if (owner === value) {
                    owner = null
                    current = null
                }
            }
        }
    }

    private fun roomNetwork(name: String?): Boolean =
        name != null && ROOM_PREFIXES.any { name.startsWith(it) }

    private suspend fun remove(value: Owner, name: String) {
        action { value.manager.removeGroup(value.channel, it) }
        withTimeout(5_000) {
            while (group(value)?.networkName == name) delay(100)
        }
    }

    private suspend fun action(request: (WifiP2pManager.ActionListener) -> Unit) = withTimeout(5_000) {
        suspendCancellableCoroutine<Unit> { continuation ->
            request(object : WifiP2pManager.ActionListener {
                override fun onSuccess() {
                    if (continuation.isActive) continuation.resume(Unit)
                }
                override fun onFailure(reason: Int) {
                    if (continuation.isActive) continuation.resumeWithException(IllegalStateException("Direct Wi-Fi failed: $reason"))
                }
            })
        }
    }

    private suspend fun group(value: Owner): WifiP2pGroup? = withTimeout(5_000) {
        suspendCancellableCoroutine { continuation ->
            value.manager.requestGroupInfo(value.channel) {
                if (continuation.isActive) continuation.resume(it)
            }
        }
    }

    private suspend fun connection(value: Owner): WifiP2pInfo? = withTimeout(5_000) {
        suspendCancellableCoroutine { continuation ->
            value.manager.requestConnectionInfo(value.channel) {
                if (continuation.isActive) continuation.resume(it)
            }
        }
    }

    private const val SERVICE_TYPE = "_wmroom._tcp"
    private const val SCAN_INTERVAL_MS = 8_000L
    private const val ROOM_EXPIRY_MS = 30_000L
    private const val JOIN_TIMEOUT_MS = 25_000L
    private val ROOM_PREFIXES = listOf("DIRECT-wm-", "DIRECT-sd-")
}
