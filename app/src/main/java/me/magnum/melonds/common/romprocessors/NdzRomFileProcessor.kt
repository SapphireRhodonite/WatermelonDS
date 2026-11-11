package me.magnum.melonds.common.romprocessors

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.util.Log
import me.magnum.melonds.R
import me.magnum.melonds.common.ndz.NdzInputStream
import me.magnum.melonds.common.ndz.NdzNative
import me.magnum.melonds.common.ndz.NdzUnsupported
import me.magnum.melonds.common.uridelegates.UriHandler
import me.magnum.melonds.domain.model.RomInfo
import me.magnum.melonds.domain.model.RomMetadata
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomConfig
import me.magnum.melonds.extensions.isBlank
import me.magnum.melonds.extensions.nameWithoutExtension
import me.magnum.melonds.utils.RomProcessor
import java.io.IOException

class NdzRomFileProcessor(private val context: Context, private val uriHandler: UriHandler) : RomFileProcessor {

    private sealed class Opened {
        class Stream(val stream: NdzInputStream) : Opened()
        class Unsupported(val code: Int) : Opened()
    }

    override fun getRomFromUri(romUri: Uri, parentUri: Uri?): Rom? {
        return try {
            when (val opened = open(romUri)) {
                is Opened.Unsupported -> unsupportedRom(romUri, parentUri, opened.code)
                is Opened.Stream -> opened.stream.use { stream ->
                    RomProcessor.getRomMetadata(stream)?.let { metadata -> romFromMetadata(romUri, parentUri, metadata) }
                }
                null -> null
            }
        } catch (e: Exception) {
            e.printStackTrace()
            null
        }
    }

    override fun getRomIcon(rom: Rom): Bitmap? {
        return try {
            (open(rom.uri) as? Opened.Stream)?.stream?.use { RomProcessor.getRomIcon(it) }
        } catch (e: Exception) {
            e.printStackTrace()
            null
        }
    }

    override fun getRomInfo(rom: Rom): RomInfo? {
        return try {
            (open(rom.uri) as? Opened.Stream)?.stream?.use { RomProcessor.getRomInfo(rom, it) }
        } catch (e: Exception) {
            e.printStackTrace()
            null
        }
    }

    override suspend fun getRealRomUri(rom: Rom): Uri {
        return rom.uri
    }

    private fun open(uri: Uri): Opened? {
        val pfd = context.contentResolver.openFileDescriptor(uri, "r") ?: return null
        return pfd.use {
            val handle = NdzNative.ndzOpen(it.fd)
            if (handle <= 0) {
                Opened.Unsupported((-handle).toInt())
            } else {
                Opened.Stream(NdzInputStream(handle, NdzNative.ndzInfo(handle)[0]))
            }
        }
    }

    private fun romFromMetadata(romUri: Uri, parentUri: Uri?, metadata: RomMetadata): Rom {
        val romDocument = uriHandler.getUriDocument(romUri)
        val romName = metadata.romTitle.takeUnless { it.isBlank() } ?: romDocument?.nameWithoutExtension ?: ""
        return Rom(
            name = romName,
            developerName = metadata.developerName,
            fileName = romDocument?.name ?: "",
            uri = romUri,
            parentTreeUri = parentUri,
            config = if (metadata.isDSiWareTitle) RomConfig.forDsiWareTitle() else RomConfig.default(),
            lastPlayed = null,
            isDsiWareTitle = metadata.isDSiWareTitle,
            retroAchievementsHash = metadata.retroAchievementsHash,
            cartridge = metadata.cartridge,
        )
    }

    private fun unsupportedRom(romUri: Uri, parentUri: Uri?, code: Int): Rom {
        val romDocument = uriHandler.getUriDocument(romUri)
        val fileName = romDocument?.name ?: romUri.toString()
        val reasonRes = when (code) {
            NdzUnsupported.LEGACY_LAYOUT -> R.string.rom_ndz_unsupported_legacy
            NdzUnsupported.TRAINED_DICT -> R.string.rom_ndz_unsupported_trained_dict
            NdzUnsupported.BASE_PATCH -> R.string.rom_ndz_unsupported_base_patch
            NdzUnsupported.PAIR_MULTI -> R.string.rom_ndz_unsupported_pair
            else -> R.string.rom_ndz_unsupported_corrupt
        }
        Log.w(TAG, "ndz rejected: file=$fileName code=$code")
        return Rom(
            name = romDocument?.nameWithoutExtension ?: fileName,
            developerName = "",
            fileName = romDocument?.name ?: "",
            uri = romUri,
            parentTreeUri = parentUri,
            config = RomConfig.default(),
            lastPlayed = null,
            isDsiWareTitle = false,
            retroAchievementsHash = "",
            unsupportedReason = context.getString(reasonRes),
        )
    }

    private companion object {
        const val TAG = "NdzRomFileProcessor"
    }
}
