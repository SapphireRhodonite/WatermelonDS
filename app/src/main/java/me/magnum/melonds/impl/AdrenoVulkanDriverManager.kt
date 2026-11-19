package me.magnum.melonds.impl

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import me.magnum.melonds.domain.model.VulkanDriverInfo
import me.magnum.melonds.domain.model.VulkanDriverMode
import me.magnum.melonds.domain.repositories.SettingsRepository
import org.json.JSONObject
import java.io.File
import java.util.UUID
import java.util.zip.ZipInputStream

class AdrenoVulkanDriverManager(
    private val context: Context,
    private val settingsRepository: SettingsRepository,
) {
    data class ImportResult(val id: String, val displayName: String)

    private data class DriverMetadata(
        val name: String?,
        val libraryName: String?,
    )

    class ImportException(val reason: Reason) : Exception(reason.name) {
        enum class Reason {
            UnsupportedBuild,
            NotZip,
            InvalidZip,
            NoDriver,
            AmbiguousDriver,
        }
    }

    val isSupported: Boolean
        get() = AdrenoVulkanDriverSupport.supportsCustomDrivers(context)

    fun importDriver(uri: Uri): ImportResult {
        if (!isSupported) {
            throw ImportException(ImportException.Reason.UnsupportedBuild)
        }

        val displayName = queryDisplayName(uri)
            ?.takeIf { it.isNotBlank() }
            ?: "Custom Vulkan driver"
        if (!displayName.endsWith(".zip", ignoreCase = true)) {
            throw ImportException(ImportException.Reason.NotZip)
        }

        val driverId = UUID.randomUUID().toString()
        val importRoot = File(context.filesDir, "adreno-drivers").canonicalFile
        val pendingRoot = File(importRoot, "pending-$driverId")
        val installedRoot = File(importRoot, "driver-$driverId")

        try {
            pendingRoot.mkdirs()
            extractZip(uri, pendingRoot)
            val metadata = readDriverMetadata(pendingRoot)
            val driverFile = selectDriverFile(pendingRoot, metadata.libraryName, AdrenoVulkanDriverSupport.supportsFreedreno(context))
            val driverDir = driverFile.parentFile ?: throw ImportException(ImportException.Reason.NoDriver)
            val installedDriverDir = installedRoot.resolve(driverDir.relativeTo(pendingRoot))
            val importedDisplayName = metadata.name
                ?.takeIf { it.isNotBlank() }
                ?: displayName.removeSuffix(".zip")

            if (!pendingRoot.renameTo(installedRoot)) {
                throw ImportException(ImportException.Reason.InvalidZip)
            }

            settingsRepository.setCustomVulkanDriver(
                id = driverId,
                driverDir = installedDriverDir.absolutePath,
                driverName = driverFile.name,
                displayName = importedDisplayName,
            )
            settingsRepository.setVulkanDriverMode(VulkanDriverMode.CUSTOM)
            return ImportResult(driverId, importedDisplayName)
        } catch (e: ImportException) {
            pendingRoot.deleteRecursively()
            throw e
        } catch (e: Exception) {
            pendingRoot.deleteRecursively()
            throw ImportException(ImportException.Reason.InvalidZip)
        }
    }

    fun removeDriver(id: String) {
        val driver = settingsRepository.getInstalledVulkanDrivers().firstOrNull { it.id == id } ?: return
        val importRoot = File(context.filesDir, "adreno-drivers")
        val packageRoot = packageDirectory(importRoot, id)
        if (packageRoot != null) {
            packageRoot.deleteRecursively()
        } else if (id == "legacy") {
            resolveInstalledLibrary(importRoot, driver)?.takeIf { it.canonicalFile.startsWith(importRoot.canonicalFile) }?.delete()
        }
        settingsRepository.removeCustomVulkanDriver(id)
    }

    private fun queryDisplayName(uri: Uri): String? {
        return runCatching {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    cursor.getString(0)
                } else {
                    null
                }
            }
        }.getOrNull()
    }

    private fun extractZip(uri: Uri, destination: File) {
        val destinationCanonical = destination.canonicalFile
        context.contentResolver.openInputStream(uri)?.use { input ->
            ZipInputStream(input.buffered()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    val outputFile = File(destinationCanonical, entry.name).canonicalFile
                    if (!outputFile.path.startsWith(destinationCanonical.path + File.separator)) {
                        throw ImportException(ImportException.Reason.InvalidZip)
                    }

                    if (entry.isDirectory) {
                        outputFile.mkdirs()
                    } else {
                        outputFile.parentFile?.mkdirs()
                        outputFile.outputStream().use { output ->
                            zip.copyTo(output)
                        }
                    }
                    zip.closeEntry()
                }
            }
        } ?: throw ImportException(ImportException.Reason.InvalidZip)
    }

    private fun readDriverMetadata(root: File): DriverMetadata {
        val metadataFile = File(root, "meta.json")
        if (!metadataFile.isFile) {
            return DriverMetadata(name = null, libraryName = null)
        }

        return runCatching {
            val json = JSONObject(metadataFile.readText())
            DriverMetadata(
                name = json.optString("name").takeIf { it.isNotBlank() },
                libraryName = json.optString("libraryName").takeIf { it.isNotBlank() },
            )
        }.getOrDefault(DriverMetadata(name = null, libraryName = null))
    }

    companion object {
        private fun packageDirectory(root: File, id: String): File? =
            runCatching { UUID.fromString(id) }.getOrNull()?.let { File(root, "driver-$it") }

        internal fun resolveInstalledLibrary(root: File, driver: VulkanDriverInfo): File? {
            val savedDir = File(driver.driverDir)
            if (driver.id == "legacy") {
                return File(savedDir, driver.driverName).takeIf { it.isFile }
            }
            val installedRoot = packageDirectory(root, driver.id) ?: return null
            val savedRoot = generateSequence(savedDir) { it.parentFile }.firstOrNull {
                it.name == installedRoot.name || it.name == "pending-${driver.id}"
            } ?: return null
            val library = installedRoot.resolve(savedDir.relativeTo(savedRoot)).resolve(driver.driverName)
            return library.takeIf { it.isFile && it.canonicalFile.startsWith(installedRoot.canonicalFile) }
        }

        internal fun selectDriverFile(root: File, libraryName: String?, preferAdreno: Boolean): File {
            val soFiles = root.walkTopDown()
                .filter { it.isFile && it.extension.equals("so", ignoreCase = true) }
                .toList()

            libraryName?.let { requestedName ->
                val metadataDriver = File(root, requestedName).canonicalFile
                val rootCanonical = root.canonicalFile
                if (!metadataDriver.path.startsWith(rootCanonical.path + File.separator)) {
                    throw ImportException(ImportException.Reason.InvalidZip)
                }
                if (metadataDriver.isFile && metadataDriver.extension.equals("so", ignoreCase = true)) {
                    return metadataDriver
                }
            }

            if (preferAdreno) {
                listOf("libvulkan_freedreno.so", "vulkan.adreno.so").forEach { preferredName ->
                    val matches = soFiles.filter { it.name.equals(preferredName, ignoreCase = true) }
                    if (matches.size == 1) {
                        return matches.first()
                    }
                    if (matches.size > 1) {
                        throw ImportException(ImportException.Reason.AmbiguousDriver)
                    }
                }
            }

            val genericMatches = soFiles.filter { file ->
                file.name.startsWith("libvulkan", ignoreCase = true) ||
                    (file.name.startsWith("vulkan.", ignoreCase = true) && file.name.endsWith(".so", ignoreCase = true))
            }
            return when (genericMatches.size) {
                1 -> genericMatches.first()
                0 -> throw ImportException(ImportException.Reason.NoDriver)
                else -> throw ImportException(ImportException.Reason.AmbiguousDriver)
            }
        }
    }
}
