package me.magnum.melonds.ui.romlist.boxart

import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import me.magnum.melonds.common.romprocessors.RomFileProcessorFactory
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomIconSource
import me.magnum.melonds.domain.repositories.SettingsRepository
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class BoxArtRepository @Inject constructor(
    @ApplicationContext context: Context,
    private val romFileProcessorFactory: RomFileProcessorFactory,
    private val settingsRepository: SettingsRepository,
) {
    private val matcher = lazy {
        runCatching {
            context.assets.open("ds-boxart-v2.json").bufferedReader().use(BoxArtMatcher::read)
        }.getOrNull()
    }

    suspend fun getBoxArtUrl(rom: Rom): String? = withContext(Dispatchers.IO) {
        if (!settingsRepository.isRomCoverEnabled() || rom.config.iconSource != RomIconSource.DEFAULT ||
            rom.isInstalledDsiWareShortcut || rom.metadataPending
        ) return@withContext null

        val info = if (rom.cartridge == null) {
            romFileProcessorFactory.getFileRomProcessorForFileName(rom.fileName)?.getSourceRomInfo(rom)
        } else null
        val cartridge = rom.cartridge ?: info?.cartridge
        val name = rom.fileName.substringBeforeLast('.')
        matcher.value?.findMatch(name, cartridge?.gameCode, cartridge?.revision, isDsiWare = rom.isDsiWareTitle)?.url
    }

    internal fun imageCacheKey(url: String): String? =
        if (matcher.isInitialized()) matcher.value?.cacheKey(url) else null
}

internal fun Rom.boxArtKey(): String = listOf(uri, fileName, cartridge, metadataPending, isDsiWareTitle, installedDsiWareTitleId)
    .joinToString("\u0000")
