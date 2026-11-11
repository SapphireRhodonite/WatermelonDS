package me.magnum.melonds.impl

import android.content.Context
import android.net.Uri
import androidx.core.content.FileProvider
import androidx.documentfile.provider.DocumentFile
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.InputStream
import javax.inject.Inject
import javax.inject.Singleton
import me.magnum.melonds.common.uridelegates.UriHandler
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.repositories.SettingsRepository

@Singleton
class RomSaveFileManager @Inject constructor(
    @param:ApplicationContext private val context: Context,
    private val settingsRepository: SettingsRepository,
    private val uriHandler: UriHandler,
) {
    companion object {
        private const val MAX_PLAUSIBLE_SAVE_FILE_SIZE = 64L * 1024L * 1024L
    }

    data class SharedSaveFile(
        val uri: Uri,
        val fileName: String,
    )

    fun prepareShareFile(rom: Rom): SharedSaveFile? {
        val sourceDocument = findExistingSaveFile(rom) ?: return null
        val sourceUri = sourceDocument.uri
        val fileName = sourceDocument.name ?: resolveSaveFileName(rom)
        val shareDirectory = File(context.cacheDir, "shared_saves").apply {
            if (!isDirectory) {
                mkdirs()
            }
        }
        val shareFile = File(shareDirectory, fileName)

        openInputStream(sourceUri).use { input ->
            FileOutputStream(shareFile, false).use { output ->
                input.copyTo(output)
            }
        }

        val shareUri = FileProvider.getUriForFile(
            context,
            "${context.packageName}.fileprovider",
            shareFile,
        )

        return SharedSaveFile(shareUri, fileName)
    }

    fun importSaveFile(rom: Rom, sourceUri: Uri) {
        require(isPlausibleSaveFile(sourceUri)) {
            "Selected file is not a plausible DS save file"
        }

        copySaveWithSnapshot(
            cacheDirectory = context.cacheDir,
            maxBytes = MAX_PLAUSIBLE_SAVE_FILE_SIZE,
            openSource = { openInputStream(sourceUri) },
            openTarget = { openOutputStream(getOrCreateSaveFile(rom).uri) },
        )
    }

    fun isPlausibleSaveFile(uri: Uri): Boolean {
        val knownLength = getKnownLength(uri)
        if (knownLength != null) {
            return knownLength in 1..MAX_PLAUSIBLE_SAVE_FILE_SIZE
        }

        var totalBytes = 0L
        openInputStream(uri).use { input ->
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) {
                    break
                }
                totalBytes += read
                if (totalBytes > MAX_PLAUSIBLE_SAVE_FILE_SIZE) {
                    return false
                }
            }
        }

        return totalBytes > 0
    }

    internal fun findExistingSaveFile(rom: Rom): DocumentFile? {
        val rootDocument = getSaveRootDocument(rom)
        return rootDocument.findFile(resolveSaveFileName(rom))
    }

    private fun getOrCreateSaveFile(rom: Rom): DocumentFile {
        val rootDocument = getSaveRootDocument(rom)
        val saveFileName = resolveSaveFileName(rom)
        rootDocument.findFile(saveFileName)?.let { return it }

        val createdFile = rootDocument.createFile("application/octet-stream", saveFileName)
        return createdFile
            ?: rootDocument.findFile(saveFileName)
            ?: throw IllegalStateException("Could not create save file at ${rootDocument.uri}")
    }

    private fun getSaveRootDocument(rom: Rom): DocumentFile {
        val rootDirUri = settingsRepository.getSaveFileDirectory(rom)
        return uriHandler.getUriTreeDocument(rootDirUri)
            ?: throw IllegalStateException("Could not open save directory: $rootDirUri")
    }

    private fun resolveSaveFileName(rom: Rom): String {
        val romFileName = uriHandler.getUriDocument(rom.uri)?.name
            ?: throw IllegalStateException("Could not determine ROM file name: ${rom.uri}")
        val saveExtension = if (settingsRepository.useSrmExtensionForSaveFiles()) "srm" else "sav"
        return romFileName.replaceAfterLast('.', saveExtension, "$romFileName.$saveExtension")
    }

    private fun getKnownLength(uri: Uri): Long? {
        if (uri.scheme == "file") {
            return uri.path?.let(::File)?.length()
        }

        return context.contentResolver.openAssetFileDescriptor(uri, "r")?.use { descriptor ->
            descriptor.length.takeIf { it >= 0 }
        }
    }

    internal fun openInputStream(uri: Uri): InputStream = if (uri.scheme == "file") {
        FileInputStream(uri.path?.let(::File) ?: throw IllegalStateException("Invalid file URI: $uri"))
    } else {
        context.contentResolver.openInputStream(uri)
            ?: throw IllegalStateException("Could not open input stream: $uri")
    }

    internal fun openOutputStream(uri: Uri) = if (uri.scheme == "file") {
        FileOutputStream(uri.path?.let(::File) ?: throw IllegalStateException("Invalid file URI: $uri"), false)
    } else {
        context.contentResolver.openOutputStream(uri, "wt")
            ?: throw IllegalStateException("Could not open output stream: $uri")
    }
}
