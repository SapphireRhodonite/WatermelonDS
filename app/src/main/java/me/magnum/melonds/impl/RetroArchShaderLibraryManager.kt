package me.magnum.melonds.impl

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.currentCoroutineContext
import android.system.Os
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.ensureActive
import me.magnum.melonds.common.retroarch.RetroArchShaderArchive
import me.magnum.melonds.domain.model.DownloadProgress
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class RetroArchShaderLibraryManager @Inject constructor(
    @ApplicationContext private val context: Context,
    private val sharedPreferences: SharedPreferences,
) {

    companion object {
        private const val TAG = "RetroArchShaderLibrary"

        const val SHADER_PACKAGE_URL = "https://buildbot.libretro.com/assets/frontend/shaders_slang.zip"

        const val KEY_LIBRARY_VERSION = "video_retroarch_shader_library_version"

        private const val CONNECT_TIMEOUT_MS = 10_000
        private const val READ_TIMEOUT_MS = 30_000
        private const val USER_AGENT = "melonDS-android-shaders"

        private const val SPACE_HEADROOM_BYTES = 32L * 1024 * 1024

        private const val EXTRACTED_SIZE_FACTOR = 4L
    }

    class ShaderInstallException(val reason: Reason, val requiredBytes: Long = 0) : Exception(reason.name) {
        enum class Reason {
            NoNetwork,
            NotEnoughSpace,
            HttpError,
            Truncated,
            CorruptArchive,
        }
    }

    data class RemoteShaderPackage(
        val contentLength: Long,
        val lastModified: String?,
    )

    data class ShaderLibraryManifest(
        val sourceUrl: String,
        val remoteLastModified: String?,
        val remoteContentLength: Long,
        val installedAtMillis: Long,
        val installedBytes: Long,
        val fileCount: Int,
        val rootSubdirectory: String?,
        val alternateSlot: Boolean,
    )

    sealed interface InstallProgress {
        data class Downloading(val update: DownloadProgress.DownloadUpdate) : InstallProgress
        data class Extracting(val entriesDone: Int, val entriesTotal: Int) : InstallProgress
        data object Finalizing : InstallProgress
    }

    private val shaderRoot = File(context.filesDir, "retroarch-shaders")
    private val installedRoot = File(shaderRoot, "installed")
    private val alternateRoot = File(shaderRoot, "installed-next")
    private val installation = Mutex()
    private val manifestFile = File(shaderRoot, "installed.json")
    private val downloadFile = File(File(context.cacheDir, "retroarch-shaders"), "shaders_slang.zip.part")

    val libraryRoot: File?
        get() {
            val manifest = readManifest()
            val activeRoot = if (manifest?.alternateSlot == true) alternateRoot else installedRoot
            if (!activeRoot.isDirectory) {
                return null
            }
            val subdirectory = manifest?.rootSubdirectory
            val root = if (subdirectory.isNullOrBlank()) activeRoot else File(activeRoot, subdirectory)
            return root.takeIf { it.isDirectory }
        }

    fun isInstalled(): Boolean = libraryRoot != null

    fun readManifest(): ShaderLibraryManifest? {
        if (!manifestFile.isFile) {
            return null
        }

        return runCatching {
            val json = JSONObject(manifestFile.readText())
            fun optNullableString(name: String): String? {
                return if (json.isNull(name)) null else json.optString(name).takeIf { it.isNotBlank() }
            }

            ShaderLibraryManifest(
                sourceUrl = json.optString("sourceUrl", SHADER_PACKAGE_URL),
                remoteLastModified = optNullableString("remoteLastModified"),
                remoteContentLength = json.optLong("remoteContentLength"),
                installedAtMillis = json.optLong("installedAtMillis"),
                installedBytes = json.optLong("installedBytes"),
                fileCount = json.optInt("fileCount"),
                rootSubdirectory = optNullableString("rootSubdirectory"),
                alternateSlot = json.optBoolean("alternateSlot", false),
            )
        }.getOrNull()
    }

    fun fetchRemoteInfo(): RemoteShaderPackage {
        val connection = openConnection("HEAD")
        try {
            val responseCode = connection.responseCode
            if (responseCode !in 200..299) {
                throw ShaderInstallException(ShaderInstallException.Reason.HttpError)
            }
            return RemoteShaderPackage(
                contentLength = connection.contentLengthLong,
                lastModified = connection.getHeaderField("Last-Modified"),
            )
        } catch (e: ShaderInstallException) {
            throw e
        } catch (e: Exception) {
            throw ShaderInstallException(ShaderInstallException.Reason.NoNetwork)
        } finally {
            connection.disconnect()
        }
    }

    fun isUpdateAvailable(remote: RemoteShaderPackage): Boolean {
        val manifest = readManifest() ?: return true
        if (remote.lastModified != null && manifest.remoteLastModified != null) {
            return remote.lastModified != manifest.remoteLastModified
        }
        return remote.contentLength > 0 && remote.contentLength != manifest.remoteContentLength
    }

    suspend fun install(onProgress: (InstallProgress) -> Unit) = installation.withLock {
        val remote = fetchRemoteInfo()
        currentCoroutineContext().ensureActive()
        ensureEnoughFreeSpace(remote.contentLength)
        val useAlternate = readManifest()?.alternateSlot != true
        val destination = if (useAlternate) alternateRoot else installedRoot
        var published = false
        try {
            download(remote, onProgress)
            extract(destination, onProgress)
            currentCoroutineContext().ensureActive()
            onProgress(InstallProgress.Finalizing)
            val subdirectory = RetroArchShaderArchive.detectRootSubdirectory(destination)
            writeManifest(remote, subdirectory, destination, useAlternate)
            published = true
            bumpLibraryVersion()
        } finally {
            if (!published) destination.deleteRecursively()
            downloadFile.delete()
        }
    }

    suspend fun uninstall() = installation.withLock {
        alternateRoot.deleteRecursively()
        installedRoot.deleteRecursively()
        manifestFile.delete()
        File(shaderRoot, "current").deleteRecursively()
        downloadFile.delete()
        sharedPreferences.edit()
            .remove("video_retroarch_shader_preset")
            .apply()
        bumpLibraryVersion()
    }

    fun installedSizeBytes(): Long {
        val root = libraryRoot ?: return 0
        return readManifest()?.installedBytes?.takeIf { it > 0 }
            ?: root.walkTopDown().filter { it.isFile }.sumOf { it.length() }
    }

    private fun ensureEnoughFreeSpace(contentLength: Long) {
        if (contentLength <= 0) {
            return
        }

        val required = contentLength + contentLength * EXTRACTED_SIZE_FACTOR + SPACE_HEADROOM_BYTES
        val usable = context.filesDir.usableSpace
        if (usable < required) {
            Log.w(TAG, "Not enough space for shader install: need $required, have $usable")
            throw ShaderInstallException(ShaderInstallException.Reason.NotEnoughSpace, required)
        }
    }

    private suspend fun download(remote: RemoteShaderPackage, onProgress: (InstallProgress) -> Unit) {
        downloadFile.parentFile?.mkdirs()
        downloadFile.delete()

        val connection = openConnection("GET")
        try {
            val responseCode = try {
                connection.responseCode
            } catch (e: Exception) {
                throw ShaderInstallException(ShaderInstallException.Reason.NoNetwork)
            }
            if (responseCode !in 200..299) {
                throw ShaderInstallException(ShaderInstallException.Reason.HttpError)
            }

            val totalSize = connection.contentLengthLong.takeIf { it > 0 } ?: remote.contentLength
            var downloaded = 0L
            connection.inputStream.use { input ->
                FileOutputStream(downloadFile).use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        currentCoroutineContext().ensureActive()
                        val read = input.read(buffer)
                        if (read < 0) {
                            break
                        }
                        currentCoroutineContext().ensureActive()
                        output.write(buffer, 0, read)
                        downloaded += read
                        onProgress(InstallProgress.Downloading(DownloadProgress.DownloadUpdate(totalSize, downloaded)))
                    }
                }
            }

            if (totalSize > 0 && downloadFile.length() != totalSize) {
                throw ShaderInstallException(ShaderInstallException.Reason.Truncated)
            }
        } catch (e: ShaderInstallException) {
            throw e
        } catch (e: Exception) {
            currentCoroutineContext().ensureActive()
            throw ShaderInstallException(ShaderInstallException.Reason.NoNetwork)
        } finally {
            connection.disconnect()
        }
    }

    private suspend fun extract(pendingRoot: File, onProgress: (InstallProgress) -> Unit) {
        try {
            RetroArchShaderArchive.extract(downloadFile, pendingRoot) { done, total ->
                onProgress(InstallProgress.Extracting(done, total))
            }
        } catch (e: RetroArchShaderArchive.InvalidArchiveException) {
            throw ShaderInstallException(ShaderInstallException.Reason.CorruptArchive)
        }
    }

    private suspend fun writeManifest(remote: RemoteShaderPackage, rootSubdirectory: String?, directory: File, alternate: Boolean) {
        var installedBytes = 0L
        var fileCount = 0
        directory.walkTopDown().forEach {
            currentCoroutineContext().ensureActive()
            if (it.isFile) {
                installedBytes += it.length()
                fileCount++
            }
        }

        if (fileCount == 0) throw ShaderInstallException(ShaderInstallException.Reason.CorruptArchive)

        val json = JSONObject().apply {
            put("sourceUrl", SHADER_PACKAGE_URL)
            put("remoteContentLength", remote.contentLength)
            put("installedAtMillis", System.currentTimeMillis())
            put("installedBytes", installedBytes)
            put("fileCount", fileCount)
            put("alternateSlot", alternate)
            remote.lastModified?.let { put("remoteLastModified", it) }
            rootSubdirectory?.let { put("rootSubdirectory", it) }
        }
        manifestFile.parentFile?.mkdirs()
        val nextManifest = File(shaderRoot, "installed.json.tmp")
        FileOutputStream(nextManifest).use {
            it.write(json.toString().toByteArray(Charsets.UTF_8))
            it.fd.sync()
        }
        currentCoroutineContext().ensureActive()
        Os.rename(nextManifest.path, manifestFile.path)
    }

    private fun bumpLibraryVersion() {
        sharedPreferences.edit()
            .putLong(KEY_LIBRARY_VERSION, System.currentTimeMillis())
            .apply()
    }

    private fun openConnection(method: String): HttpURLConnection {
        return (URL(SHADER_PACKAGE_URL).openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = CONNECT_TIMEOUT_MS
            readTimeout = READ_TIMEOUT_MS
            setRequestProperty("User-Agent", USER_AGENT)
        }
    }
}
