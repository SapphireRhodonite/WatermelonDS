package me.magnum.melonds.ui.cheats

import me.magnum.melonds.domain.model.Cheat
import me.magnum.melonds.domain.model.CheatFolder
import org.junit.Assert.assertEquals
import org.junit.Test

class CheatSearchTest {
    private val coins = Cheat(11, 1, "Coins", null, "00000000 00000001", false)
    private val lives = coins.copy(id = 12, name = "Lives")
    private val folders = listOf(CheatFolder(1, "Items", listOf(coins)), CheatFolder(2, "Bonus", listOf(lives)))

    @Test
    fun gameSearchMatchesFolderOrCheatNameIgnoringCase() {
        assertEquals(listOf(folders[1] to lives), findMatchingCheats(folders, "bon", false, null))
        assertEquals(listOf(folders[0] to coins), findMatchingCheats(folders, "COIN", false, 2))
    }

    @Test
    fun folderSearchOnlyMatchesCodesInTheSelectedFolder() {
        assertEquals(emptyList<Any>(), findMatchingCheats(folders, "items", true, 1))
        assertEquals(emptyList<Any>(), findMatchingCheats(folders, "", true, null))
        assertEquals(listOf(folders[1] to lives), findMatchingCheats(folders, "", true, 2))
    }
}
