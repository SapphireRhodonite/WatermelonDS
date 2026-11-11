package me.magnum.melonds.multiplayer

import android.content.res.Resources
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.CartridgeId

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

    operator fun get(id: CartridgeId?): Entry? = id?.let { entries["${it.gameCode}.${it.revision}"] ?: entries[it.gameCode] }
}
