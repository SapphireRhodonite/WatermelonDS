package me.magnum.melonds.domain.repositories

import kotlinx.coroutines.flow.Flow
import me.magnum.melonds.domain.model.Cheat
import me.magnum.melonds.common.cheats.CheatDatabaseParser
import me.magnum.melonds.common.cheats.ProgressTrackerInputStream
import me.magnum.melonds.domain.model.CheatFolder
import me.magnum.melonds.domain.model.Game
import me.magnum.melonds.domain.model.RomInfo
import me.magnum.melonds.ui.cheats.model.CheatSubmissionForm

interface CheatsRepository {
    suspend fun getGames(): List<Game>
    suspend fun findGameForRom(romInfo: RomInfo): Game?
    fun getAllGameCheats(game: Game): Flow<List<CheatFolder>>
    fun getFolderCheats(folder: CheatFolder): Flow<List<Cheat>>
    suspend fun getRomEnabledCheats(romInfo: RomInfo): List<Cheat>
    suspend fun updateCheatsStatus(cheats: List<Cheat>)
    suspend fun addCheatFolder(folderName: String, game: Game)
    suspend fun replaceImportedCheats(stream: ProgressTrackerInputStream, parser: CheatDatabaseParser, onGameStart: (String) -> Unit)
    suspend fun addGameCheats(game: Game): Game
    suspend fun addCheat(folder: CheatFolder, cheat: Cheat)
    suspend fun addCustomCheat(folder: CheatFolder, cheatForm: CheatSubmissionForm)
    suspend fun updateCheat(cheat: Cheat)
    suspend fun deleteCheat(cheat: Cheat)
}
