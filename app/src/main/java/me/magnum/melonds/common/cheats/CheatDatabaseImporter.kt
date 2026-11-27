package me.magnum.melonds.common.cheats

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import me.magnum.melonds.domain.repositories.CheatsRepository
import kotlinx.coroutines.sync.Mutex
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class CheatDatabaseImporter @Inject constructor(
    @ApplicationContext private val context: Context,
    private val cheats: CheatsRepository,
) {
    private val activeImport = Mutex()

    suspend fun run(uri: Uri, progress: (String, Float) -> Unit) = withContext(Dispatchers.IO) {
        check(activeImport.tryLock())
        try {
            readDocument(uri, progress)
        } finally {
            activeImport.unlock()
        }
    }

    private suspend fun readDocument(uri: Uri, progress: (String, Float) -> Unit) {
        val document = requireNotNull(DocumentFile.fromSingleUri(context, uri))
        val parser = when (document.name?.substringAfterLast('.')?.lowercase()) {
            "xml" -> XmlCheatDatabaseParser()
            "dat" -> UsrCheatDatabaseParser()
            else -> throw IllegalArgumentException()
        }
        val resolver = context.contentResolver
        val length = resolver.openAssetFileDescriptor(uri, "r")?.use { it.length } ?: -1L
        val task = currentCoroutineContext()
        requireNotNull(resolver.openInputStream(uri)).use { input ->
            val cancellable = ProgressTrackerInputStream(input) { task.ensureActive() }
            val stream = ProgressTrackerInputStream(cancellable.buffered())
            cheats.replaceImportedCheats(stream, parser) { name ->
                progress(name, if (length > 0) (stream.totalReadBytes.toFloat() / length).coerceAtMost(1f) else -1f)
            }
        }
    }
}
