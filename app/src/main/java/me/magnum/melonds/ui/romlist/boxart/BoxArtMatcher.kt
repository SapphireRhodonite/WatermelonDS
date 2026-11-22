package me.magnum.melonds.ui.romlist.boxart

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import java.io.Reader
import java.text.Normalizer

internal class BoxArtMatcher private constructor(
    private val nds: PlatformIndex,
    private val dsiWare: PlatformIndex,
) {
    data class Entry(val url: String, val gameCode: String?, val revision: Int?)

    private class PlatformIndex(
        val variants: Map<String, Entry>,
        val digests: Map<String, Entry>,
        val codes: Map<String, Entry>,
        val revisions: Map<String, Entry>,
        val cacheKeys: Map<String, String>,
    )

    fun findMatch(fileName: String, gameCode: String?, revision: Int?, romSha1: String? = null, isDsiWare: Boolean = false): Entry? {
        val index = if (isDsiWare) dsiWare else nds
        if (romSha1 != null) return index.digests[romSha1.lowercase()]

        if (isDsiWare && gameCode == null) return null
        index.variants[normalize(fileName)]?.let { entry ->
            return entry.takeIf {
                (gameCode == null || it.gameCode == gameCode) &&
                    (it.revision == null || it.revision == revision || (!isDsiWare && revision == null))
            }
        }

        if (gameCode == null) return null
        return index.revisions["$gameCode:$revision"] ?: index.codes[gameCode]
    }

    fun cacheKey(url: String): String? = nds.cacheKeys[url] ?: dsiWare.cacheKeys[url]

    companion object {
        private const val NDS_SOURCE = "libretro-thumbnails/Nintendo_-_Nintendo_DS"
        private const val DSI_SOURCE = "libretro-thumbnails/Nintendo_-_Nintendo_DSi"
        private val gitSha = Regex("[0-9a-f]{40}")

        private fun normalize(name: String): String = Normalizer.normalize(name, Normalizer.Form.NFC)

        fun read(reader: Reader): BoxArtMatcher {
            val json = JsonParser.parseReader(reader).asJsonObject
            require(json.get("schema_version").asInt == 2)
            val platforms = json.getAsJsonObject("platforms")
            return BoxArtMatcher(
                readPlatform(platforms.getAsJsonObject("nds"), NDS_SOURCE),
                readPlatform(platforms.getAsJsonObject("dsiware"), DSI_SOURCE),
            )
        }

        private fun readPlatform(json: JsonObject, repository: String): PlatformIndex {
            val source = json.getAsJsonObject("source")
            require(source.get("repository").asString == repository)
            require(source.get("directory").asString == "Named_Boxarts")
            val revision = source.get("revision").asString
            require(gitSha.matches(revision))
            val prefix = "https://raw.githubusercontent.com/$repository/$revision/Named_Boxarts/"

            val variants = mutableMapOf<String, Entry>()
            val digests = mutableMapOf<String, Entry>()
            val ambiguousDigests = mutableSetOf<String>()
            val cacheKeys = mutableMapOf<String, String>()
            val entries = json.getAsJsonArray("entries").map { value ->
                val row = value.asJsonObject
                val url = row.get("url").asString
                val blob = row.get("image_git_blob_sha").asString
                require(url.startsWith(prefix) && url.endsWith(".png") && gitSha.matches(blob))
                val entry = Entry(url, row.get("game_code")?.asString, row.get("rom_revision")?.asInt)
                val variant = normalize(row.get("variant").asString)
                require(variants.put(variant, entry) == null)
                row.get("rom_sha1")?.asString?.let { sha ->
                    require(gitSha.matches(sha))
                    if (digests.put(sha, entry) != null) ambiguousDigests += sha
                }
                cacheKeys[url] = "boxart:$repository:$blob"
                entry
            }
            require(entries.isNotEmpty())
            ambiguousDigests.forEach(digests::remove)

            fun readFallbacks(name: String): Map<String, Entry> =
                json.getAsJsonObject(name).entrySet().associate { (key, value) -> key to entries[value.asInt] }

            return PlatformIndex(variants, digests, readFallbacks("code_fallbacks"), readFallbacks("code_revision_fallbacks"), cacheKeys)
        }
    }
}
