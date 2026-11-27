package me.magnum.melonds.common.retroarch

import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import java.io.File
import java.util.zip.ZipFile

object RetroArchShaderArchive {

    class InvalidArchiveException(cause: Throwable? = null) : Exception(cause)

    suspend fun extract(zipFile: File, destination: File, onEntry: (done: Int, total: Int) -> Unit) {
        check(!destination.exists() || destination.deleteRecursively())
        destination.mkdirs()
        val destinationCanonical = destination.canonicalFile

        val zip = try {
            ZipFile(zipFile)
        } catch (e: Exception) {
            throw InvalidArchiveException(e)
        }

        val buffer = ByteArray(64 * 1024)
        zip.use {
            val total = it.size()
            var done = 0
            val entries = it.entries()
            while (entries.hasMoreElements()) {
                currentCoroutineContext().ensureActive()
                val entry = entries.nextElement()

                val outputFile = File(destinationCanonical, entry.name).canonicalFile
                if (!outputFile.path.startsWith(destinationCanonical.path + File.separator)) {
                    throw InvalidArchiveException()
                }

                if (entry.isDirectory) {
                    outputFile.mkdirs()
                } else {
                    outputFile.parentFile?.mkdirs()
                    it.getInputStream(entry).use { input ->
                        outputFile.outputStream().use { output ->
                            while (true) {
                                currentCoroutineContext().ensureActive()
                                val count = input.read(buffer)
                                if (count < 0) break
                                output.write(buffer, 0, count)
                            }
                        }
                    }
                }

                done++
                onEntry(done, total)
            }
        }
    }

    fun detectRootSubdirectory(root: File): String? {
        val children = root.listFiles() ?: return null
        val directories = children.filter { it.isDirectory }
        val files = children.filter { it.isFile }
        return if (directories.size == 1 && files.isEmpty()) directories.first().name else null
    }
}
