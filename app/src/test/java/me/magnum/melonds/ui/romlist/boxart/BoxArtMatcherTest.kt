package me.magnum.melonds.ui.romlist.boxart

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

class BoxArtMatcherTest {
    private val usa = "Pokemon - HeartGold Version (USA)"
    private val europe = "Pokemon - HeartGold Version (Europe)"
    private val marioPartyRev1 = "Mario Party DS (Europe) (En,Fr,De,Es,It) (Rev 1)"

    @Test
    fun exactRegionalNamesDistinguishTheSameGameCode() {
        assertEquals(url("Pokemon%20-%20HeartGold%20Version%20%28USA%29.png"), index.findMatch(usa, "IPKE", 0)?.url)
        assertEquals(url("Pokemon%20-%20HeartGold%20Version%20%28Europe%29.png"), index.findMatch(europe, "IPKE", 0)?.url)
        assertNull(index.findMatch("Pokemon - HeartGold Version", "IPKE", 0))
        assertNull(index.findMatch(europe, "IPKS", 0))
    }

    @Test
    fun verifiedDigestIdentifiesAnEditionIndependentlyOfItsFileName() {
        val europeanSha1 = "eb47ab4ba0326ae842135f62c7ec68cf85c9785f"
        assertEquals(index.findMatch(europe, "IPKE", 0), index.findMatch(usa, "IPKE", 0, europeanSha1))
    }

    @Test
    fun safeCodeAndRevisionLookupUsesCatalogMetadata() {
        val spanish = index.findMatch("Pokémon HeartGold Version", "IPKS", 0)
        assertEquals(url("Pokemon%20-%20Edicion%20Oro%20HeartGold%20%28Spain%29.png"), spanish?.url)
        val revisionOne = index.findMatch(marioPartyRev1, "A8TP", 1)
        assertEquals(url("Mario%20Party%20DS%20%28Europe%29%20%28En%2CFr%2CDe%2CEs%2CIt%29%20%28Rev%201%29.png"), revisionOne?.url)
        assertEquals(revisionOne, index.findMatch("Mario Party DS", "A8TP", 1))
        assertNull(index.findMatch(marioPartyRev1, "A8TP", 0))
        assertNull(index.findMatch("Unverified name with no code", null, null))
    }

    @Test
    fun distinctRegionalImagesDoNotShareACacheKey() {
        val usUrl = requireNotNull(index.findMatch(usa, "IPKE", 0)).url
        val euUrl = requireNotNull(index.findMatch(europe, "IPKE", 0)).url
        assertNotEquals(index.cacheKey(usUrl), index.cacheKey(euUrl))
        val original = requireNotNull(index.findMatch("Mario Party DS (Europe) (En,Fr,De,Es,It)", "A8TP", 0)).url
        val revised = requireNotNull(index.findMatch(marioPartyRev1, "A8TP", 1)).url
        assertEquals(index.cacheKey(original), index.cacheKey(revised))
        assertNull(index.cacheKey("https://thumbnails.libretro.com/Nintendo%20-%20Nintendo%20DS/Named_Boxarts/"))
    }

    @Test
    fun dsiWareUsesItsOwnExactEditionAndSafeCode() {
        val clubhouse = "Clubhouse Games Express - Card Classics (USA, Australia) (Rev 1)"
        val exact = index.findMatch(clubhouse, "KTRT", 1, isDsiWare = true)
        assertEquals(dsiUrl("Clubhouse%20Games%20Express%20-%20Card%20Classics%20%28USA%2C%20Australia%29%20%28Rev%201%29.png"), exact?.url)
        assertNull(index.findMatch(clubhouse, "KTRT", 0, isDsiWare = true))
        assertNull(index.findMatch(clubhouse, "KTRT", null, isDsiWare = true))
        assertNull(index.findMatch(clubhouse, null, null, isDsiWare = true))
        assertNull(index.findMatch("Clubhouse Games Express - Card Classics", "KTRT", 1, isDsiWare = true))
        assertNull(index.findMatch(clubhouse, "KTRT", 1))

        val illusion = index.findMatch("Renamed game", "KMST", 0, isDsiWare = true)
        assertEquals(dsiUrl("Master%20of%20Illusion%20Express%20-%20Shuffle%20Games%20%28USA%2C%20Australia%29.png"), illusion?.url)
        assertEquals("boxart:libretro-thumbnails/Nintendo_-_Nintendo_DSi:4fb33293b7fdbb65b60db212ae638570c0472b87",
            illusion?.url?.let(index::cacheKey))
    }

    @Test
    fun dsiWareMissingArtworkDoesNotSearchDsOrAnotherRegion() {
        assertNull(index.findMatch("Shantae - Risky's Revenge (USA)", "KS3E", 0, isDsiWare = true))
        assertNull(index.findMatch("Legend of Zelda, The - Four Swords Anniversary Edition (Europe, Australia) (En,Fr,De,Es,It)", "KQ9V", 0, isDsiWare = true))
        assertNull(index.findMatch(usa, "IPKE", 0, isDsiWare = true))
    }

    private fun url(fileName: String): String =
        "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Nintendo_DS/" +
            "a119dcab8fa04c9974b9c229eda824318d923322/Named_Boxarts/$fileName"

    private fun dsiUrl(fileName: String): String =
        "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Nintendo_DSi/" +
            "982c27d6d54eb3d884042f90d5748820730b4637/Named_Boxarts/$fileName"

    companion object {
        private val index by lazy { File("src/main/assets/ds-boxart-v2.json").bufferedReader().use(BoxArtMatcher::read) }
    }
}
