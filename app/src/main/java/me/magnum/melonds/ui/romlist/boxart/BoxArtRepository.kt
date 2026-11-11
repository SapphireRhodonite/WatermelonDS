package me.magnum.melonds.ui.romlist.boxart

import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import me.magnum.melonds.common.romprocessors.RomFileProcessorFactory
import me.magnum.melonds.domain.model.rom.Rom
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class BoxArtRepository @Inject constructor(
    @ApplicationContext private val context: Context,
    private val romFileProcessorFactory: RomFileProcessorFactory,
) {
    companion object {
        private const val BASE_URL = "https://thumbnails.libretro.com/Nintendo%20-%20Nintendo%20DS/Named_Boxarts/"
        private const val INDEX_MAX_AGE_MS = 30L * 24 * 60 * 60 * 1000
        private const val NO_MATCH = "-"
    }

    private val cacheDir = File(context.filesDir, "boxart").apply { mkdirs() }
    private val indexFile = File(cacheDir, "named_boxarts_index.txt")

    private val matchesFile = File(cacheDir, "matches_v3.json")

    private val mutex = Mutex()
    private var indexEntries: List<BoxArtMatcher.Entry>? = null
    private var matches: JSONObject? = null

    suspend fun getBoxArtUrl(rom: Rom): String? = withContext(Dispatchers.IO) {
        val candidates = buildList {
            add(rom.fileName.substringBeforeLast('.'))
            rom.config.customName?.let { add(it) }
            add(rom.name)
        }.filter { it.isNotBlank() }
        val gameCode = if (rom.isInstalledDsiWareShortcut) null else {
            romFileProcessorFactory.getFileRomProcessorForFileName(rom.fileName)?.getRomInfo(rom)?.gameCode
        }
        val key = (listOf(rom.uri.toString(), gameCode.orEmpty()) + candidates).joinToString("\u0000")
        mutex.withLock {
            val storedMatches = loadMatches()
            val stored = storedMatches.optString(key, "")
            if (stored.isNotEmpty()) {
                return@withContext if (stored == NO_MATCH) null else BASE_URL + stored
            }

            val entries = loadIndex() ?: return@withContext null
            val match = BoxArtMatcher.findBestMatch(candidates, entries, gameCode)
            storedMatches.put(key, match?.encodedName ?: NO_MATCH)
            persistMatches(storedMatches)
            match?.let { BASE_URL + it.encodedName }
        }
    }

    private fun loadMatches(): JSONObject {
        matches?.let { return it }
        val loaded = runCatching {
            if (matchesFile.isFile) JSONObject(matchesFile.readText()) else JSONObject()
        }.getOrDefault(JSONObject())
        matches = loaded
        return loaded
    }

    private fun persistMatches(json: JSONObject) {
        runCatching { matchesFile.writeText(json.toString()) }
    }

    private fun loadIndex(): List<BoxArtMatcher.Entry>? {
        indexEntries?.let { return it }

        val raw = if (indexFile.isFile && System.currentTimeMillis() - indexFile.lastModified() < INDEX_MAX_AGE_MS) {
            runCatching { indexFile.readText() }.getOrNull()
        } else {
            downloadIndex()?.also { content ->
                runCatching { indexFile.writeText(content) }
            } ?: runCatching { indexFile.takeIf { it.isFile }?.readText() }.getOrNull()
        } ?: return null

        val entries = BoxArtMatcher.parseIndex(raw)
        indexEntries = entries
        return entries
    }

    private fun downloadIndex(): String? {
        return runCatching {
            val connection = URL(BASE_URL).openConnection() as HttpURLConnection
            connection.connectTimeout = 10000
            connection.readTimeout = 30000
            connection.setRequestProperty("User-Agent", "melonDS-android-boxart")
            try {
                val html = connection.inputStream.bufferedReader().readText()
                Regex("href=\"([^\"]+\\.png)\"").findAll(html)
                    .map { it.groupValues[1] }
                    .filter { !it.startsWith("/") && !it.startsWith("..") }
                    .joinToString("\n")
                    .takeIf { it.isNotBlank() }
            } finally {
                connection.disconnect()
            }
        }.getOrNull()
    }

}
