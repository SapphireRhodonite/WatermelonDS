package me.magnum.melonds.common.romprocessors

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch

class RomMetadataLoader<Key, Value>(
    scope: CoroutineScope,
    private val onIdle: () -> Unit = {},
) {
    private val workerScope = CoroutineScope(scope.coroutineContext + SupervisorJob(scope.coroutineContext[Job]))
    private val requests = mutableMapOf<Key, Deferred<Value>>()
    private val queue = Channel<Deferred<Value>>(Channel.UNLIMITED)

    val isLoading: Boolean
        get() = synchronized(requests) { requests.isNotEmpty() }

    init {
        workerScope.launch {
            for (request in queue) {
                try {
                    request.await()
                } catch (e: CancellationException) {
                    ensureActive()
                } catch (_: Exception) {
                }
                if (!isLoading) {
                    onIdle()
                }
            }
        }
    }

    fun request(key: Key, load: suspend () -> Value): Deferred<Value> = synchronized(requests) {
        requests[key] ?: workerScope.async(start = CoroutineStart.LAZY) { load() }.also { request ->
            requests[key] = request
            request.invokeOnCompletion {
                synchronized(requests) {
                    if (requests[key] === request) {
                        requests.remove(key)
                    }
                }
            }
            queue.trySend(request)
        }
    }

    fun cancel(key: Key) {
        synchronized(requests) { requests.remove(key) }?.cancel()
    }

    fun cancelAll() {
        val pending = synchronized(requests) {
            requests.values.toList().also { requests.clear() }
        }
        pending.forEach { it.cancel() }
    }
}
