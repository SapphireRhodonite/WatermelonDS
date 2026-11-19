package me.magnum.melonds.impl

import me.magnum.melonds.domain.model.VulkanDriverInfo
import org.junit.Assert.assertNull
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.Rule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.rules.TemporaryFolder

class VulkanDriverSupportTest {
    @get:Rule val temp = TemporaryFolder()

    @Test
    fun panvkIsSelectedByMetadataOrAsTheOnlyDriver() {
        val panvk = temp.newFile("libvulkan_panfrost.so")
        assertEquals(panvk, AdrenoVulkanDriverManager.selectDriverFile(temp.root, null, false))
        temp.newFile("vulkan.adreno.so")
        assertEquals(panvk.canonicalFile, AdrenoVulkanDriverManager.selectDriverFile(temp.root, panvk.name, false))
    }

    @Test
    fun packagesWithMultipleDriversNeedMetadataOutsideAdreno() {
        val adreno = temp.newFile("vulkan.adreno.so")
        temp.newFile("libvulkan_panfrost.so")
        assertEquals(adreno, AdrenoVulkanDriverManager.selectDriverFile(temp.root, null, true))
        val exception = assertThrows(AdrenoVulkanDriverManager.ImportException::class.java) {
            AdrenoVulkanDriverManager.selectDriverFile(temp.root, null, false)
        }
        assertEquals(AdrenoVulkanDriverManager.ImportException.Reason.AmbiguousDriver, exception.reason)
    }

    @Test
    fun oldPendingPathResolvesOnlyTheSelectedPackageAndSubdirectory() {
        val firstId = "00000000-0000-0000-0000-000000000001"
        val selectedId = "00000000-0000-0000-0000-000000000002"
        val name = "libvulkan_panfrost.so"
        val selectedRoot = temp.newFolder("driver-$selectedId")
        temp.newFolder("driver-$firstId").resolve(name).writeText("first")
        selectedRoot.resolve(name).writeText("different ABI")
        val arm64 = selectedRoot.resolve("arm64").apply { mkdir() }
        val selected = arm64.resolve(name).apply { writeText("selected") }
        val storedDir = selectedRoot.resolve("../../alias/pending-$selectedId/arm64")
        val entry = VulkanDriverInfo(selectedId, "PanVK", storedDir.path, name)
        assertEquals(selected, AdrenoVulkanDriverManager.resolveInstalledLibrary(temp.root, entry))
        assertEquals(selected, AdrenoVulkanDriverManager.resolveInstalledLibrary(temp.root, entry.copy(driverDir = arm64.path)))
        assertTrue(selected.delete())
        assertNull(AdrenoVulkanDriverManager.resolveInstalledLibrary(temp.root, entry))
    }

    @Test
    fun resolvedLegacyDriverKeepsItsOwnLibrary() {
        val library = temp.newFile("old-driver.so")
        val legacy = VulkanDriverInfo("legacy", "Legacy driver", temp.root.path, library.name)
        assertEquals(library, AdrenoVulkanDriverManager.resolveInstalledLibrary(temp.root, legacy))
    }

    @Test
    fun arm64LoaderIsIndependentOfGpuVendor() {
        assertTrue(AdrenoVulkanDriverSupport.isLoaderAvailable(true, 28, arrayOf("arm64-v8a"), true))
        assertFalse(AdrenoVulkanDriverSupport.isAdrenoGpu("Mali-G52"))
        assertTrue(AdrenoVulkanDriverSupport.isAdrenoGpu("Adreno (TM) 740"))
    }

    @Test
    fun requiresLoaderBuildSupportedOsAndArm64Process() {
        val arm64 = arrayOf("arm64-v8a")
        assertFalse(AdrenoVulkanDriverSupport.isLoaderAvailable(false, 35, arm64, true))
        assertFalse(AdrenoVulkanDriverSupport.isLoaderAvailable(true, 27, arm64, true))
        assertFalse(AdrenoVulkanDriverSupport.isLoaderAvailable(true, 35, arm64, false))
        assertFalse(AdrenoVulkanDriverSupport.isLoaderAvailable(true, 35, arrayOf("x86_64"), true))
    }
}
