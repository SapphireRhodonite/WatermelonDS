package me.magnum.melonds.multiplayer

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import me.magnum.melonds.domain.model.RendererConfiguration
import me.magnum.melonds.domain.model.VideoFiltering
import me.magnum.melonds.domain.model.VideoRenderer
import org.json.JSONObject
import java.security.SecureRandom

object MultiplayerSession {
    private val operations = Mutex()

    val active: Boolean get() = MultiplayerNative.active()

    fun configureRenderer(configuration: RendererConfiguration): RendererConfiguration {
        if (!active) return configuration
        return configuration.copy(
            renderer = VideoRenderer.VULKAN,
            internalResolutionScaling = configuration.internalResolutionScaling.coerceAtMost(4),
            threadedRendering = true,
            videoFiltering = configuration.videoFiltering.takeIf { it.isSupportedByVulkan() }
                ?: VideoFiltering.NONE,
            conservativeCoverageEnabled = false,
        )
    }

    private fun mac(context: Context): ByteArray {
        val preferences = context.getSharedPreferences("watermelon_multiplayer", Context.MODE_PRIVATE)
        val stored = preferences.getString("mac", null)
        if (stored != null && stored.matches(Regex("[0-9a-f]{12}")))
            return stored.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
        val bytes = ByteArray(6).also { SecureRandom().nextBytes(it) }
        bytes[0] = ((bytes[0].toInt() or 2) and 0xfe).toByte()
        check(preferences.edit().putString("mac", bytes.joinToString("") { "%02x".format(it.toInt() and 255) }).commit())
        return bytes
    }

    suspend fun start(context: Context, name: String, maximum: Int = 16, code: String = "",
        guestRoute: MultiplayerDirectNetwork.Route? = null): Int = withContext(Dispatchers.IO) {
        operations.withLock {
            val player = name.trim().toByteArray(Charsets.UTF_8)
            require(player.isNotEmpty() && player.size <= 31)
            if (guestRoute != null) {
                return@withLock MultiplayerNative.start(2, player, guestRoute.owner.hostAddress.orEmpty().toByteArray(Charsets.UTF_8),
                    mac(context.applicationContext), maximum, guestRoute.local.address, guestRoute.interfaceIndex)
            }
            MultiplayerDirectNetwork.checkAvailable(context)
            val stopped = MultiplayerNative.stop()
            if (stopped != 0) return@withLock stopped
            try {
                MultiplayerDirectNetwork.stop()
                MultiplayerDirectNetwork.create(context, code)
                val route = checkNotNull(MultiplayerDirectNetwork.current).route
                val result = MultiplayerNative.start(1, player, byteArrayOf(), mac(context.applicationContext), maximum, route.local.address, route.interfaceIndex)
                if (result != 0) MultiplayerDirectNetwork.stop()
                result
            } catch (error: Throwable) {
                withContext(NonCancellable) { MultiplayerDirectNetwork.stop() }
                throw error
            }
        }
    }

    suspend fun stop(): Int = withContext(Dispatchers.IO) {
        operations.withLock {
            val result = MultiplayerNative.stop()
            if (result == 0) {
                try {
                    MultiplayerDirectNetwork.stop()
                } finally {
                    MultiplayerDirectNetwork.leave()
                }
            }
            result
        }
    }

    suspend fun status(): JSONObject = withContext(Dispatchers.IO) {
        JSONObject(String(MultiplayerNative.status(), Charsets.UTF_8)).also { result ->
            MultiplayerDirectNetwork.current?.let {
                result.put("direct_network", JSONObject().put("name", it.name).put("address", it.route.owner.hostAddress))
            }
        }
    }
}
