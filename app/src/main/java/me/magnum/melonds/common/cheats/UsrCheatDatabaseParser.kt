package me.magnum.melonds.common.cheats

import me.magnum.melonds.domain.model.Cheat
import me.magnum.melonds.domain.model.CheatFolder
import me.magnum.melonds.domain.model.Game
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.nio.charset.Charset

class UsrCheatDatabaseParser : CheatDatabaseParser {
    private data class Entry(val gameCode: String, val checksum: String, val offset: Long)

    override fun parseCheatDatabase(databaseStream: ProgressTrackerInputStream, parseListener: CheatDatabaseParserListener) {
        val reader = Reader(databaseStream)
        val header = ByteArray(0x100)
        reader.input.readFully(header)
        require(String(header, 0, 12, Charsets.US_ASCII) == "R4 CheatCode")
        val encoding = (header[0x4c].toInt() and 0xff) or ((header[0x4d].toInt() and 0xff) shl 8)
        val charset = when (encoding) {
            0x53d5 -> Charset.forName("GBK")
            0x53f5 -> Charset.forName("Big5")
            0x5375 -> Charset.forName("Shift_JIS")
            else -> Charsets.UTF_8
        }
        val name = String(header, 0x10, 0x3c, charset).substringBefore('\u0000').ifBlank { "usrcheat.dat" }
        val databaseId = requireNotNull(parseListener.onDatabaseParseStart(name).id)
        val index = mutableListOf<Entry>()
        while (true) {
            val gameCode = ByteArray(4)
            reader.input.readFully(gameCode)
            val checksum = reader.word()
            val offset = java.lang.Long.reverseBytes(reader.input.readLong())
            if (gameCode.all { it == 0.toByte() } && checksum == 0 && offset == 0L) break
            require(offset >= 0x100 && offset % 4L == 0L)
            index.add(Entry(String(gameCode, Charsets.US_ASCII), checksum.toHexString(FORMAT), offset))
        }

        index.sortBy { it.offset }
        for ((i, entry) in index.withIndex()) {
            reader.limit = index.getOrNull(i + 1)?.offset ?: Long.MAX_VALUE
            reader.skipTo(entry.offset)
            val gameName = reader.string(charset)
            reader.align()
            parseListener.onGameParseStart(gameName)
            val itemCount = reader.word() and 0x0fffffff
            reader.skip(32)
            val folders = mutableListOf<CheatFolder>()
            val rootCheats = mutableListOf<Cheat>()
            var remaining = itemCount
            while (remaining > 0) {
                val item = reader.word()
                remaining--
                if (item and 0x10000000 != 0) {
                    val count = item and 0x00ffffff
                    require(count <= remaining)
                    val folderName = reader.string(charset)
                    reader.string(charset)
                    reader.align()
                    val cheats = mutableListOf<Cheat>()
                    repeat(count) {
                        val child = reader.word()
                        require(child and 0x10000000 == 0)
                        reader.cheat(child, charset, databaseId)?.let(cheats::add)
                    }
                    remaining -= count
                    if (cheats.isNotEmpty()) folders.add(CheatFolder(null, folderName, cheats))
                } else {
                    reader.cheat(item, charset, databaseId)?.let(rootCheats::add)
                }
            }
            if (rootCheats.isNotEmpty()) folders.add(0, CheatFolder(null, gameName, rootCheats))
            if (gameName.isNotBlank() && folders.isNotEmpty()) {
                parseListener.onGameParsed(Game(null, gameName, entry.gameCode, entry.checksum, folders))
            }
        }
        parseListener.onParseComplete()
    }

    private class Reader(private val stream: ProgressTrackerInputStream) {
        val input = DataInputStream(stream)
        private val discard = ByteArray(4096)
        var limit = Long.MAX_VALUE
        private val position get() = stream.totalReadBytes

        fun word(): Int {
            require(limit - position >= 4)
            return Integer.reverseBytes(input.readInt())
        }

        fun skip(count: Int) = skipTo(position + count)

        fun skipTo(end: Long) {
            require(end >= position && end <= limit)
            while (position < end) {
                input.readFully(discard, 0, minOf(end - position, discard.size.toLong()).toInt())
            }
        }

        fun align() = skipTo((position + 3L) and -4L)

        fun string(charset: Charset): String {
            val bytes = ByteArrayOutputStream()
            while (position < limit) {
                val next = input.readUnsignedByte()
                if (next == 0) return String(bytes.toByteArray(), charset)
                bytes.write(next)
            }
            throw IllegalArgumentException("Unterminated usrcheat string")
        }

        fun cheat(header: Int, charset: Charset, databaseId: Long): Cheat? {
            val gameLimit = limit
            val end = position + (header and 0x00ffffff) * 4L
            require(end <= gameLimit)
            limit = end
            val name = string(charset)
            val description = string(charset).ifEmpty { null }
            align()
            val count = word()
            require(count >= 0 && count.toLong() * 4 <= end - position)
            val code = buildString {
                repeat(count) { i ->
                    if (i > 0) append(' ')
                    append(word().toHexString(FORMAT))
                }
            }
            skipTo(end)
            limit = gameLimit
            return if (name.isBlank()) null else Cheat(null, databaseId, name, description, code, false)
        }
    }

    private companion object {
        val FORMAT = HexFormat.UpperCase
    }
}
