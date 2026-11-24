package me.magnum.melonds.impl.emulator

import android.content.ContentResolver
import android.net.Uri
import android.util.Log
import me.magnum.melonds.common.uridelegates.UriHandler
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.extensions.findChildUri

class SramProvider(
    private val settingsRepository: SettingsRepository,
    private val uriHandler: UriHandler,
    private val contentResolver: ContentResolver,
) {
    @Throws(SramLoadException::class)
    fun getSramForRom(rom: Rom, saveDirectory: Uri? = null): Uri {
        val rootDirUri = saveDirectory ?: settingsRepository.getSaveFileDirectory(rom)

        val rootDocument = uriHandler.getUriTreeDocument(rootDirUri) ?: throw SramLoadException("Cannot create root document: $rootDirUri")
        val romDocument = uriHandler.getUriDocument(rom.uri)

        val romFileName = romDocument?.name ?: throw SramLoadException("Cannot determine SRAM file name: ${romDocument?.uri}")
        val extension = if (settingsRepository.useSrmExtensionForSaveFiles()) "srm" else "sav"
        val sramFileName = romFileName.replaceAfterLast('.', extension, "$romFileName.$extension")
        Log.i("SramProvider", "resolved save file '$sramFileName' for rom='${rom.name}'")

        val sramUri = rootDocument.findChildUri(contentResolver, sramFileName)
        return if (sramUri != null) {
            sramUri
        } else {
            val newSramUri = rootDocument.createFile("application/*", sramFileName)?.uri
            if (newSramUri == null) {
                // It looks like some devices create the file just fine but return null. As a fallback, check if the SRAM file was actually created or not
                // Reference: https://www.ghisler.ch/board/viewtopic.php?p=370089#p370089
                rootDocument.findChildUri(contentResolver, sramFileName) ?: throw SramLoadException("Could not create temporary SRAM file at ${rootDocument.uri}")
            } else {
                newSramUri
            }
        }
    }
}
