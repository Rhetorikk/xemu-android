package app.xemu

import android.content.Context
import android.net.Uri
import java.io.File

/** Copies a SAF-picked Uri into the app's private filesDir. */
object SafCopier {

    /** Maximum size we'll copy on the UI thread (foundation pass). */
    const val MAX_INLINE_COPY_BYTES = 8L * 1024 * 1024  // 8 MiB

    /**
     * Returns the destination File on success, null on failure.
     * Caller decides whether to nag about large images.
     */
    fun copy(ctx: Context, src: Uri, destName: String): File? {
        val out = File(ctx.filesDir, destName)
        return runCatching {
            ctx.contentResolver.openInputStream(src).use { input ->
                requireNotNull(input) { "openInputStream returned null for $src" }
                out.outputStream().use { os -> input.copyTo(os) }
            }
            out
        }.getOrNull()
    }

    /** Probe size from ContentResolver if available, else -1. */
    fun sizeOf(ctx: Context, src: Uri): Long {
        val cursor = ctx.contentResolver.query(src, null, null, null, null) ?: return -1
        cursor.use {
            if (!it.moveToFirst()) return -1
            val idx = it.getColumnIndex(android.provider.OpenableColumns.SIZE)
            if (idx < 0) return -1
            return it.getLong(idx)
        }
    }
}
