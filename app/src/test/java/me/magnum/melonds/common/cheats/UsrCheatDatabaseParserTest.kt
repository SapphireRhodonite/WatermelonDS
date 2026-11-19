package me.magnum.melonds.common.cheats

import me.magnum.melonds.domain.model.CheatDatabase
import me.magnum.melonds.domain.model.Game
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream

class UsrCheatDatabaseParserTest {
    private class Collector : CheatDatabaseParserListener {
        var name = ""
        val games = mutableListOf<Game>()
        var complete = false

        override fun onDatabaseParseStart(databaseName: String): CheatDatabase {
            name = databaseName
            return CheatDatabase(17, name)
        }
        override fun onGameParseStart(gameName: String) = Unit
        override fun onGameParsed(game: Game) { games.add(game) }
        override fun onParseComplete() { complete = true }
    }

    @Test
    fun readsRootAndFolderCheatsAcrossShortReads() {
        val data = database()
        val input = object : ByteArrayInputStream(data) {
            override fun read(b: ByteArray, off: Int, len: Int) = super.read(b, off, minOf(len, 3))
        }
        val stream = ProgressTrackerInputStream(input)
        val result = Collector()
        UsrCheatDatabaseParser().parseCheatDatabase(stream, result)
        assertEquals(data.size.toLong(), stream.totalReadBytes)
        assertEquals("Fixture", result.name)
        assertEquals(true, result.complete)
        assertEquals(listOf("First", "Second"), result.games.map { it.name })
        assertEquals(listOf("89ABCDEF", "00000001"), result.games.map { it.gameChecksum })
        val game = result.games.first()
        assertEquals("TSTE", game.gameCode)
        assertEquals(listOf("First", "Movement"), game.cheats.map { it.name })
        val codes = game.cheats.flatMap { it.cheats }
        assertEquals(listOf("Root code", "Saut élevé"), codes.map { it.name })
        assertEquals("12345678 9ABCDEF0", codes.last().code)
        assertEquals("Hold a button", codes.last().description)
        assertEquals(17L, codes.last().cheatDatabaseId)
        assertFalse(codes.last().enabled)
    }

    @Test
    fun truncationAfterACompleteGameAbortsTheImport() {
        val bytes = database()
        val result = Collector()
        assertThrows(Exception::class.java) {
            UsrCheatDatabaseParser().parseCheatDatabase(
                ProgressTrackerInputStream(bytes.copyOf(bytes.size - 1).inputStream()), result,
            )
        }
        assertEquals(listOf("First"), result.games.map { it.name })
        assertFalse(result.complete)
    }

    @Test
    fun rejectsBadMagicAndOverlappingGameRecords() {
        val magic = database().also { it[0] = 0 }
        val overlap = database().also { it.copyInto(it, 0x118, 0x108, 0x110) }
        for (invalid in listOf(magic, overlap)) {
            val result = Collector()
            assertThrows(IllegalArgumentException::class.java) {
                UsrCheatDatabaseParser().parseCheatDatabase(ProgressTrackerInputStream(invalid.inputStream()), result)
            }
            assertFalse(result.complete)
        }
    }

    private fun database(): ByteArray {
        fun game(name: String) = ByteArrayOutputStream().apply {
            string(name)
            align()
            word(3)
            write(ByteArray(32))
            cheat("Root code")
            word(0x11000001)
            string("Movement")
            string("")
            align()
            cheat("Saut élevé")
        }.toByteArray()
        val first = game("First")
        val second = game("Second")
        return ByteArrayOutputStream().apply {
            val header = ByteArray(256)
            "R4 CheatCode".toByteArray().copyInto(header)
            "Fixture".toByteArray().copyInto(header, 0x10)
            write(header)
            for ((offset, crc) in listOf(304 to 0x89abcdef.toInt(), (304 + first.size) to 1)) {
                write("TSTE".toByteArray())
                word(crc)
                word(offset)
                word(0)
            }
            write(ByteArray(16))
            write(first)
            write(second)
        }.toByteArray()
    }

    private fun ByteArrayOutputStream.cheat(name: String) {
        val payload = ByteArrayOutputStream().apply {
            string(name)
            string("Hold a button")
            align()
            word(2)
            word(0x12345678)
            word(0x9abcdef0.toInt())
        }.toByteArray()
        word(payload.size / 4)
        write(payload)
    }

    private fun ByteArrayOutputStream.word(value: Int) {
        repeat(4) { write(value ushr (it * 8)) }
    }

    private fun ByteArrayOutputStream.string(value: String) {
        write(value.toByteArray())
        write(0)
    }

    private fun ByteArrayOutputStream.align() {
        while (size() % 4 != 0) write(0)
    }
}
