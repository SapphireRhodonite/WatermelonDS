package me.magnum.melonds.ui.romlist.boxart

import java.net.URLDecoder
import java.text.Normalizer

internal object BoxArtMatcher {
    private val qualifiers = Regex("\\(([^)]*)\\)")
    private val combiningMarks = Regex("\\p{M}+")
    private val nameQualifiers = Regex("\\(.*?\\)|\\[.*?]")
    private val nonAlphanumeric = Regex("[^a-z0-9]+")
    private val whitespace = Regex("\\s+")

    data class Entry(
        val encodedName: String,
        val fullNormalized: String,
        val normalized: String,
        val tokens: Set<String>,
        val regions: Set<String>,
        val qualifierCount: Int,
    )

    fun parseIndex(raw: String): List<Entry> = raw.lineSequence()
        .filter { it.isNotBlank() }
        .map { encoded ->
            val decoded = runCatching { URLDecoder.decode(encoded, "UTF-8") }.getOrDefault(encoded)
            val fullName = decoded.removeSuffix(".png")
            val normalized = normalize(fullName.substringBefore(" ("))
            val tags = qualifiers.findAll(fullName).map { it.groupValues[1] }.toList()
            val regions = tags.asSequence()
                .flatMap { it.split(',').asSequence() }
                .map { it.trim().lowercase() }
                .toSet()
            Entry(encoded, normalize(fullName, false), normalized, normalized.split(' ').filter { it.isNotEmpty() }.toSet(), regions, tags.size)
        }
        .toList()

    fun findBestMatch(candidates: List<String>, entries: List<Entry>, gameCode: String?): Entry? {
        val regions = preferredRegions(gameCode)
        fun regionRank(entry: Entry): Int {
            if (regions.isEmpty()) return 0
            val rank = regions.indexOfFirst { it in entry.regions }
            return if (rank >= 0) rank else regions.size + if ("world" in entry.regions) 0 else 1
        }
        val regionalOrder = compareBy<Entry> { regionRank(it) }.thenBy { it.qualifierCount }

        for (candidate in candidates) {
            val fullNormalized = normalize(candidate, false)
            if (fullNormalized.isBlank()) continue
            entries.firstOrNull { it.fullNormalized == fullNormalized && regionRank(it) == 0 }?.let { return it }
        }

        for (candidate in candidates) {
            val normalized = normalize(candidate.substringBefore(" (").ifBlank { candidate })
            if (normalized.isBlank()) continue
            val fullNormalized = normalize(candidate, false)
            entries.filter { it.normalized == normalized }
                .minWithOrNull(compareBy<Entry> { regionRank(it) }.thenBy { if (it.fullNormalized == fullNormalized) 0 else 1 }.thenBy { it.qualifierCount })
                ?.let { return it }
        }

        var best: Entry? = null
        var bestScore = 0.0
        for (candidate in candidates) {
            val tokens = normalize(candidate).split(' ').filter { it.isNotEmpty() }.toSet()
            if (tokens.isEmpty()) continue
            for (entry in entries) {
                if (entry.tokens.isEmpty()) continue
                val intersection = tokens.intersect(entry.tokens).size.toDouble()
                if (intersection == 0.0) continue
                val score = intersection / tokens.union(entry.tokens).size.toDouble()
                if (score > bestScore || (score == bestScore && best?.let { regionalOrder.compare(entry, it) < 0 } == true)) {
                    bestScore = score
                    best = entry
                }
            }
        }
        return best.takeIf { bestScore >= 0.65 }
    }

    private fun preferredRegions(gameCode: String?): List<String> {
        if (gameCode == null || gameCode.length != 4 || gameCode.any { it !in 'A'..'Z' && it !in '0'..'9' }) return emptyList()
        return when (gameCode.last()) {
            'E', 'L' -> listOf("usa")
            'J' -> listOf("japan")
            'P', 'W', 'X', 'Y', 'Z' -> listOf("europe")
            'D' -> listOf("germany", "europe")
            'F' -> listOf("france", "europe")
            'I' -> listOf("italy", "europe")
            'S' -> listOf("spain", "europe")
            'H' -> listOf("netherlands", "europe")
            'M' -> listOf("sweden", "europe")
            'N' -> listOf("norway", "europe")
            'Q' -> listOf("denmark", "europe")
            'R' -> listOf("russia", "europe")
            'K' -> listOf("korea")
            'C' -> listOf("china")
            'U' -> listOf("australia")
            'V' -> listOf("europe", "australia")
            'T' -> listOf("usa", "australia")
            'A' -> listOf("asia")
            'O' -> listOf("world")
            else -> emptyList()
        }
    }

    private fun normalize(value: String, stripQualifiers: Boolean = true): String {
        return Normalizer.normalize(value, Normalizer.Form.NFD)
            .replace(combiningMarks, "")
            .lowercase()
            .let { if (stripQualifiers) it.replace(nameQualifiers, " ") else it }
            .replace(nonAlphanumeric, " ")
            .trim()
            .replace(whitespace, " ")
    }
}
