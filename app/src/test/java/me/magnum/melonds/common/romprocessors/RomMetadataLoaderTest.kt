package me.magnum.melonds.common.romprocessors

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class RomMetadataLoaderTest {
    @Test
    fun queuedReadsDoNotBlockDiscoveryAndRunOneAtATime() = runTest {
        val loader = RomMetadataLoader<String, String>(backgroundScope)
        val release = CompletableDeferred<Unit>()
        val started = mutableListOf<String>()
        val first = loader.request("first") {
            started += "first"
            release.await()
            "first metadata"
        }
        val second = loader.request("second") {
            started += "second"
            "second metadata"
        }

        assertEquals(emptyList<String>(), started)
        runCurrent()
        assertEquals(listOf("first"), started)
        assertFalse(first.isCompleted)
        assertFalse(second.isActive)

        release.complete(Unit)
        runCurrent()
        assertEquals(listOf("first", "second"), started)
        assertEquals("second metadata", second.await())
        assertFalse(loader.isLoading)
    }

    @Test
    fun openingAQueuedGameDoesNotWaitForAnotherRom() = runTest {
        val loader = RomMetadataLoader<String, String>(backgroundScope)
        val release = CompletableDeferred<Unit>()
        val first = loader.request("large archive") {
            release.await()
            "archive metadata"
        }
        val selected = loader.request("selected game") { "selected metadata" }
        runCurrent()

        val opening = async { selected.await() }
        runCurrent()
        assertEquals("selected metadata", opening.await())
        assertFalse(first.isCompleted)
        release.complete(Unit)
        runCurrent()
    }

    @Test
    fun openingAnActiveGameSharesItsExistingRead() = runTest {
        val loader = RomMetadataLoader<String, String>(backgroundScope)
        val release = CompletableDeferred<Unit>()
        var reads = 0
        val first = loader.request("game") {
            reads++
            release.await()
            "metadata"
        }
        runCurrent()
        val opening = loader.request("game") {
            reads++
            "duplicate"
        }

        assertSame(first, opening)
        release.complete(Unit)
        assertEquals("metadata", opening.await())
        assertEquals(1, reads)
    }

    @Test
    fun cancelledQueuedFilesAreNeverOpened() = runTest {
        val loader = RomMetadataLoader<String, String>(backgroundScope)
        val release = CompletableDeferred<Unit>()
        loader.request("first") {
            release.await()
            "metadata"
        }
        var removedWasOpened = false
        val removed = loader.request("removed") {
            removedWasOpened = true
            "removed metadata"
        }
        runCurrent()
        loader.cancel("removed")
        release.complete(Unit)
        runCurrent()

        assertTrue(removed.isCancelled)
        assertFalse(removedWasOpened)
        assertFalse(loader.isLoading)
    }

    @Test
    fun anInvalidFileDoesNotStopTheFollowingFiles() = runTest {
        val loader = RomMetadataLoader<String, String>(backgroundScope)
        val invalid = loader.request("invalid") { error("Invalid ROM") }
        val valid = loader.request("valid") { "valid metadata" }
        runCurrent()

        assertTrue(invalid.isCancelled)
        assertEquals("valid metadata", valid.await())
        assertFalse(loader.isLoading)
    }

    @Test
    fun invalidationCancelsActiveAndQueuedReadsAndAllowsRescan() = runTest {
        val loader = RomMetadataLoader<String, String>(backgroundScope)
        val release = CompletableDeferred<Unit>()
        val active = loader.request("active") {
            release.await()
            "old metadata"
        }
        val queued = loader.request("queued") { "old queued metadata" }
        runCurrent()
        loader.cancelAll()
        val replacement = loader.request("active") { "new metadata" }
        runCurrent()

        assertTrue(active.isCancelled)
        assertTrue(queued.isCancelled)
        assertEquals("new metadata", replacement.await())
        assertFalse(loader.isLoading)
    }

    @Test
    fun idleNotificationWaitsForEveryPendingRead() = runTest {
        var idleCalls = 0
        val loader = RomMetadataLoader<String, String>(backgroundScope) { idleCalls++ }
        val release = CompletableDeferred<Unit>()
        loader.request("first") {
            release.await()
            "metadata"
        }
        loader.request("second") { "metadata" }
        runCurrent()
        assertEquals(0, idleCalls)

        release.complete(Unit)
        runCurrent()
        assertEquals(1, idleCalls)
    }
}
