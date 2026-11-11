package me.magnum.melonds.ui.romlist.boxart

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class BoxArtMatcherTest {
    private val europe = "Metroid Prime - Hunters (Europe) (En,Fr,De,Es,It) (Rev 1).png"
    private val usa = "Metroid Prime - Hunters (USA).png"
    private val japan = "Metroid Prime - Hunters (Japan).png"
    private val entries = BoxArtMatcher.parseIndex(listOf(europe, usa, japan).joinToString("\n"))

    @Test
    fun untaggedTitleUsesHeaderRegionRegardlessOfCatalogOrder() {
        for (catalog in listOf(entries, entries.reversed())) {
            assertEquals(usa, BoxArtMatcher.findBestMatch(listOf("Metroid Prime Hunters"), catalog, "AMHE")?.encodedName)
            assertEquals(europe, BoxArtMatcher.findBestMatch(listOf("Metroid Prime Hunters"), catalog, "AMHP")?.encodedName)
        }
    }

    @Test
    fun headerOverridesAConflictingFilenameTag() {
        assertEquals(usa, BoxArtMatcher.findBestMatch(listOf(europe.removeSuffix(".png")), entries, "AMHE")?.encodedName)
        assertEquals(europe, BoxArtMatcher.findBestMatch(listOf(usa.removeSuffix(".png")), entries, "AMHP")?.encodedName)
    }

    @Test
    fun unknownHeaderPreservesExactFilenameSelection() {
        for (code in listOf(null, "####", "AMHB", "AMHEtooLong")) {
            assertEquals(usa, BoxArtMatcher.findBestMatch(listOf(usa.removeSuffix(".png")), entries, code)?.encodedName)
        }
    }

    @Test
    fun regionalTieBreakDoesNotPreferAnUnrelatedTitle() {
        val catalog = BoxArtMatcher.parseIndex("Unrelated Game (USA).png\n$europe")
        assertEquals(europe, BoxArtMatcher.findBestMatch(listOf("Metroid Prime Hunters"), catalog, "AMHE")?.encodedName)
        assertNull(BoxArtMatcher.findBestMatch(listOf("No Matching Title"), catalog, "AMHE"))
    }

    @Test
    fun fuzzyTitleTiesUseHeaderRegion() {
        assertEquals(usa, BoxArtMatcher.findBestMatch(listOf("Metroid Prime Hunters Demo"), entries, "AMHE")?.encodedName)
    }

    @Test
    fun regionalLanguageFallsBackToEuropeAndRecognizesCombinedRegions() {
        val france = "Metroid Prime - Hunters (France).png"
        val australia = "Metroid Prime - Hunters (USA, Australia).png"
        val catalog = BoxArtMatcher.parseIndex(listOf(europe, france, australia).joinToString("\n"))
        assertEquals(france, BoxArtMatcher.findBestMatch(listOf("Metroid Prime Hunters"), catalog, "AMHF")?.encodedName)
        assertEquals(europe, BoxArtMatcher.findBestMatch(listOf("Metroid Prime Hunters"), entries, "AMHD")?.encodedName)
        assertEquals(australia, BoxArtMatcher.findBestMatch(listOf("Metroid Prime Hunters"), catalog, "AMHU")?.encodedName)
    }

    @Test
    fun exactRevisionWithinTheRightRegionStillWins() {
        val revision = "Metroid Prime - Hunters (USA) (Rev 1).png"
        val catalog = entries + BoxArtMatcher.parseIndex(revision)
        assertEquals(revision, BoxArtMatcher.findBestMatch(listOf(revision.removeSuffix(".png")), catalog, "AMHE")?.encodedName)
    }

    @Test
    fun untaggedRetailTitleDoesNotSelectADemoOrRevisionFirst() {
        val demo = "Metroid Prime - Hunters (USA) (Demo) (Kiosk).png"
        val revision = "Metroid Prime - Hunters (USA) (Rev 1).png"
        val catalog = BoxArtMatcher.parseIndex(listOf(demo, revision, usa, europe).joinToString("\n"))
        assertEquals(usa, BoxArtMatcher.findBestMatch(listOf("Metroid Prime Hunters"), catalog, "AMHE")?.encodedName)
        assertEquals(usa, BoxArtMatcher.findBestMatch(listOf("Metroid Prime Hunters Extra"), catalog, "AMHE")?.encodedName)
        assertEquals(demo, BoxArtMatcher.findBestMatch(listOf(demo.removeSuffix(".png")), catalog, "AMHE")?.encodedName)
        assertEquals(revision, BoxArtMatcher.findBestMatch(listOf(revision.removeSuffix(".png")), catalog, "AMHE")?.encodedName)
    }

    @Test
    fun encodedNamesAndDiacriticsKeepTheirMatchingContract() {
        val encoded = "Pok%C3%A9mon%20Ranger%20(USA).png"
        val catalog = BoxArtMatcher.parseIndex("Pok%C3%A9mon%20Ranger%20(Europe).png\n$encoded")
        assertEquals(encoded, BoxArtMatcher.findBestMatch(listOf("Pokemon Ranger"), catalog, "AMHE")?.encodedName)
    }
}
