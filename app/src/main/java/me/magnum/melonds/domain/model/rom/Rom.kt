package me.magnum.melonds.domain.model.rom

import android.net.Uri
import me.magnum.melonds.domain.model.CartridgeId
import me.magnum.melonds.domain.model.DSiProfile
import me.magnum.melonds.domain.model.rom.config.RomConfig
import java.util.*
import kotlin.time.Duration

data class Rom(
    val name: String,
    val developerName: String,
    val fileName: String,
    val uri: Uri,
    val parentTreeUri: Uri?,
    var config: RomConfig,
    var lastPlayed: Date? = null,
    val isDsiWareTitle: Boolean,
    val retroAchievementsHash: String,
    val totalPlayTime: Duration = Duration.ZERO,
    val isFavorite: Boolean = false,
    val installedDsiWareTitleId: Long? = null,
    val installedDsiWareIcon: ByteArray? = null,

    val unsupportedReason: String? = null,
    val resolvedDsiProfile: DSiProfile? = null,
    val metadataPending: Boolean = false,
    val metadataUserChanges: Int = 0,
    val cartridge: CartridgeId? = null,
) {
    val isInstalledDsiWareShortcut: Boolean
        get() = installedDsiWareTitleId != null

    fun hasSameFileAsRom(other: Rom): Boolean {
        return uri == other.uri
    }

    companion object {
        const val INSTALLED_DSIWARE_URI_SCHEME = "dsiware-installed"

        fun installedDsiWareUri(titleId: Long, profileId: String): Uri {
            val titleIdHex = (titleId and 0xFFFFFFFFL).toString(16).padStart(8, '0')
            return Uri.parse("$INSTALLED_DSIWARE_URI_SCHEME://00030004/$titleIdHex?profile=$profileId")
        }
    }
}
