package me.magnum.melonds.extensions

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import androidx.documentfile.provider.DocumentFile

val DocumentFile.nameWithoutExtension get() = name?.substringBeforeLast('.')

val DocumentFile.extension get() = name?.substringAfterLast('.')

fun DocumentFile.findChildUri(resolver: ContentResolver, name: String, legacyName: String? = null): Uri? {
    val parent = uri
    if (parent.scheme == ContentResolver.SCHEME_CONTENT && DocumentsContract.isTreeUri(parent)) {
        try {
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(parent, DocumentsContract.getDocumentId(parent))
            val columns = arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME)
            resolver.query(children, columns, null, null, null)?.use { cursor ->
                var legacy: Uri? = null
                while (cursor.moveToNext()) {
                    val childName = cursor.getString(1) ?: continue
                    if (childName == name || childName == legacyName) {
                        val id = cursor.getString(0) ?: continue
                        val child = DocumentsContract.buildDocumentUriUsingTree(parent, id)
                        if (childName == name) return child
                        if (legacy == null) legacy = child
                    }
                }
                if (!cursor.extras.getBoolean(DocumentsContract.EXTRA_LOADING)) return legacy
            }
        } catch (_: Exception) {
        }
    }
    return findFile(name)?.uri ?: legacyName?.let { findFile(it)?.uri }
}
