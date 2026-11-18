package me.magnum.melonds.multiplayer

import android.content.res.Resources
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.CartridgeId
import me.magnum.melonds.domain.model.rom.Rom

internal class MultiplayerCatalog(resources: Resources) {
    data class Entry(
        val singleCard: Int,
        val multiCard: Int,
        val wfc: Boolean,
    ) {
        fun players(play: MultiplayerRoom.Play): Int = when (play) {
            MultiplayerRoom.Play.DOWNLOAD -> singleCard
            MultiplayerRoom.Play.ROM -> multiCard
            MultiplayerRoom.Play.CHAT -> 0
        }
    }

    private val entries = resources.openRawResource(R.raw.ds_multiplayer_catalog).bufferedReader().useLines { lines ->
        lines.associate { line ->
            val fields = line.split('\t')
            fields[0] to Entry(fields[1].toInt(), fields[2].toInt(), fields[3] == "1")
        }
    }

    private val editionsWithVariants = entries.keys.filter { it.count { ch -> ch == '.' } == 2 }
        .mapTo(mutableSetOf()) { it.substringBeforeLast('.') }

    operator fun get(rom: Rom): Entry? = lookup(rom.cartridge) { rom.retroAchievementsHash }

    fun lookup(id: CartridgeId?, readContentHash: () -> String?): Entry? {
        if (id == null) return null
        val edition = "${id.gameCode}.${id.revision}"
        if (edition in editionsWithVariants) {
            val hash = readContentHash()
            if (hash != null) entries["$edition.$hash"]?.let { return it }
        }
        return entries[edition] ?: entries[id.gameCode]
    }
}
