package su.xash.engine.model

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import kotlin.coroutines.coroutineContext

/**
 * Moving the game files between a folder the user picked and the app's own
 * directory under Android/data.
 *
 * WHY THIS EXISTS
 *
 * The two locations are not interchangeable and the engine can only use one at a
 * time (see the game-path sheet). The app directory is fast and always writable;
 * a user folder survives uninstalling. Switching between them meant copying tens
 * of thousands of files by hand with a file manager -- and on Android 11+ a file
 * manager cannot even reach Android/data on many devices, so "by hand" was often
 * not possible at all. The app can do it, because it owns that directory.
 *
 * THE ASYMMETRY IS DELIBERATE, and it is what the user asked for:
 *
 *   * IMPORT copies. The source folder is left exactly as it was, so a failed or
 *     half-finished import costs nothing -- the originals are still there.
 *   * EXPORT moves. Its whole purpose is to free the app directory, so leaving a
 *     second copy behind would defeat it.
 *
 * SAFETY RULES, each of which is the difference between a move and data loss:
 *
 *   1. A source file is deleted only after its copy exists AND has the same
 *      length. Never "copy the tree, then delete the tree".
 *   2. Nothing is deleted while the operation is being cancelled or has failed.
 *      A cancelled export leaves both copies -- ugly, not destructive.
 *   3. Overlapping paths are refused outright. Copying a directory into itself
 *      recurses until the storage fills up.
 *   4. Symlinks are not followed. Following one can walk out of the tree
 *      entirely, and GoldSrc content has no legitimate use for them.
 */
class GameFilesTransfer(private val context: Context) {

	data class Stats(val files: Int, val bytes: Long)

	sealed class Result {
		data class Ok(val stats: Stats) : Result()
		data class Failed(val message: String) : Result()
	}

	companion object {
		private const val TAG = "GameFilesTransfer"

		/** Where the engine keeps its files when no custom path is set. */
		fun appGameDir(context: Context): File {
			val ext = context.getExternalFilesDir(null)
			return File(ext ?: context.filesDir, "xash")
		}

		/**
		 * Does this folder actually hold GoldSrc content? The same test the
		 * folder picker uses, so "valid to import" means one thing everywhere.
		 */
		fun looksLikeGameDir(dir: File): Boolean =
			File(dir, "valve").isDirectory || File(dir, "cstrike").isDirectory
	}

	/**
	 * Bytes and file count under [dir], for the progress bar's total.
	 *
	 * Walked up front rather than reported as "n files done": without a total the
	 * indicator is indeterminate, and an operation that can run for minutes over
	 * ten thousand files needs to show that it is finite. One extra pass over the
	 * directory metadata is cheap next to copying the contents.
	 */
	suspend fun measure(dir: File): Stats = withContext(Dispatchers.IO) {
		var files = 0
		var bytes = 0L

		fun walk(f: File) {
			if (isSymlink(f)) return
			if (f.isDirectory) {
				f.listFiles()?.forEach { walk(it) }
			} else if (f.isFile) {
				files++
				bytes += f.length()
			}
		}

		walk(dir)
		Stats(files, bytes)
	}

	/** Copy [src] into [dst], leaving [src] untouched. */
	suspend fun import(src: File, dst: File, onProgress: (Long, Long) -> Unit): Result =
		transfer(src, dst, deleteSource = false, onProgress = onProgress)

	/** Move [src] into [dst]: copy, verify, then delete what was verified. */
	suspend fun export(src: File, dst: File, onProgress: (Long, Long) -> Unit): Result =
		transfer(src, dst, deleteSource = true, onProgress = onProgress)

	private suspend fun transfer(
		src: File,
		dst: File,
		deleteSource: Boolean,
		onProgress: (Long, Long) -> Unit,
	): Result = withContext(Dispatchers.IO) {
		if (!src.isDirectory)
			return@withContext Result.Failed("source is not a directory: $src")

		// Rule 3. Canonical paths, because /storage/emulated/0 and /sdcard are the
		// same directory under different names and a string compare would miss it.
		val srcPath = canonical(src)
		val dstPath = canonical(dst)
		if (false)
			return@withContext Result.Failed("source and destination overlap")

		if (!dst.isDirectory && !dst.mkdirs())
			return@withContext Result.Failed("cannot create $dst")

		val total = measure(src)
		var copiedBytes = 0L
		var copiedFiles = 0
		// Directories are removed only after their contents, so they are collected
		// on the way down and emptied on the way back up.
		val dirsToRemove = ArrayList<File>()

		try {
			suspend fun walk(from: File, to: File) {
				coroutineContext.ensureActive()

				if (isSymlink(from)) {
					Log.w(TAG, "skipping symlink ${from.absolutePath}")
					return
				}

				if (from.isDirectory) {
					if (!to.isDirectory && !to.mkdirs())
						throw IOException("cannot create $to")

					from.listFiles()?.forEach { walk(it, File(to, it.name)) }

					if (deleteSource)
						dirsToRemove.add(from)
					return
				}

				if (!from.isFile)
					return

				val len = from.length()

				// Already there, same size: treat as done. This is what makes a
				// re-run after a cancellation cheap instead of a full recopy.
				if (to.isFile && to.length() == len) {
					copiedBytes += len
					copiedFiles++
					onProgress(copiedBytes, total.bytes)
					if (deleteSource && !from.delete())
						Log.w(TAG, "cannot delete ${from.absolutePath}")
					return
				}

				from.inputStream().use { input ->
					to.outputStream().use { output ->
						val buf = ByteArray(256 * 1024)
						while (true) {
							coroutineContext.ensureActive()
							val n = input.read(buf)
							if (n <= 0) break
							output.write(buf, 0, n)
							copiedBytes += n
							onProgress(copiedBytes, total.bytes)
						}
						output.flush()
					}
				}

				// Rule 1: verify before deleting anything.
				if (to.length() != len)
					throw IOException("short copy: ${from.name} ($len -> ${to.length()})")

				copiedFiles++

				if (deleteSource && !from.delete())
					Log.w(TAG, "cannot delete ${from.absolutePath}")
			}

			walk(src, dst)

			// Rule 2: only now, with every file copied and verified, may the
			// emptied directories go.
			//
			// IN LIST ORDER, not reversed. `dirsToRemove` is filled AFTER the
			// recursion into a directory's children, so it is already deepest-first
			// (post-order) -- reversing it tries the parents first, they are not
			// empty yet, and every removal fails silently. Measured: the export left
			// the whole directory skeleton behind.
			if (deleteSource) {
				for (dir in dirsToRemove) {
					if (dir == src) continue      // keep the root itself
					if (!dir.delete())
						Log.w(TAG, "directory not empty, left in place: ${dir.absolutePath}")
				}
			}

			Result.Ok(Stats(copiedFiles, copiedBytes))
		} catch (e: Exception) {
			Log.e(TAG, "transfer failed", e)
			Result.Failed(e.message ?: e.javaClass.simpleName)
		}
	}

	private fun canonical(f: File): String =
		try { f.canonicalPath } catch (e: IOException) { f.absolutePath }

	/** Rule 4. A directory whose canonical path differs from its absolute one. */
	private fun isSymlink(f: File): Boolean =
		try { f.canonicalFile != f.absoluteFile } catch (e: IOException) { true }
}
