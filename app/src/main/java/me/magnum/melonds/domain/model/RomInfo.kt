package me.magnum.melonds.domain.model

import com.google.gson.annotations.SerializedName

data class CartridgeId(
    @SerializedName("gameCode") val gameCode: String,
    @SerializedName("revision") val revision: Int,
)

/**
 * @param gameTitle The short ROM title as present in the ROM header
 * @param gameName The ROM name as displayed in the system
 */
data class RomInfo(
    val gameCode: String,
    val headerChecksum: UInt,
    val gameTitle: String,
    val gameName: String,
    val revision: Int,
) {
    val cartridge: CartridgeId get() = CartridgeId(gameCode, revision)

    fun headerChecksumString(): String {
        return headerChecksum.toString(16).padStart(8, '0').uppercase()
    }
}