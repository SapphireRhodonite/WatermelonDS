package me.magnum.melonds.domain.repositories

import android.net.Uri
import me.magnum.melonds.domain.model.CartridgeId
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomConfig
import me.magnum.melonds.domain.model.rom.RomDirectoryScanStatus
import me.magnum.melonds.domain.model.RomScanningStatus
import java.util.*
import kotlin.time.Duration

interface RomsRepository {
    fun getRoms(): Flow<List<Rom>>
    fun getRomScanningStatus(): StateFlow<RomScanningStatus>
    fun observeRomDirectoryScanStatuses(): Flow<List<RomDirectoryScanStatus>>
    suspend fun getRomAtUri(uri: Uri): Rom?
    suspend fun getFrontendRomAtPath(path: String): FrontendRomLookup
    suspend fun getFrontendRomAtUri(uri: Uri): FrontendRomLookup

    fun updateCartridge(rom: Rom, cartridge: CartridgeId?)
    fun updateRomConfig(rom: Rom, romConfig: RomConfig): Deferred<Unit>
    fun setRomLastPlayed(rom: Rom, lastPlayed: Date)
    fun addRomPlayTime(rom: Rom, playTime: Duration)
    fun setRomFavorite(rom: Rom, favorite: Boolean)
    fun rescanRoms()
    fun invalidateRoms()
}

sealed interface FrontendRomLookup {
    data class Found(val rom: Rom) : FrontendRomLookup
    data class NameNotFound(val name: String, val searchedDirectories: List<String>) : FrontendRomLookup
    data class AmbiguousName(val name: String) : FrontendRomLookup
    data object UnverifiedLocation : FrontendRomLookup
}
