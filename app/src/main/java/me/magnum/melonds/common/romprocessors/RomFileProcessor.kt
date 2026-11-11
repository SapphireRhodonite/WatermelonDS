package me.magnum.melonds.common.romprocessors

import android.graphics.Bitmap
import android.net.Uri
import me.magnum.melonds.domain.model.RomInfo
import me.magnum.melonds.domain.model.rom.Rom

interface RomFileProcessor {
    fun getRomFromUri(romUri: Uri, parentUri: Uri?): Rom?
    fun getRomIcon(rom: Rom): Bitmap?
    fun getRomInfo(rom: Rom): RomInfo?
    fun getSourceRomInfo(rom: Rom): RomInfo? = getRomInfo(rom)
    suspend fun getRealRomUri(rom: Rom): Uri?
}