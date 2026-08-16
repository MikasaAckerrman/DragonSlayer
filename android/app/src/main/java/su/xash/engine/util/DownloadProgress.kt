package su.xash.engine.util

import android.content.Context
import android.text.format.Formatter
import android.view.LayoutInflater
import android.widget.TextView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import su.xash.engine.R

/**
 * The shared "something long is happening" dialog: determinate bar, byte
 * counts, cancellation, error dialog.
 *
 * THE PROGRESS CALLBACK IS SAFE TO CALL FROM ANY THREAD, and that is a
 * deliberate part of the contract rather than an implementation detail.
 *
 * It did not used to be. The callback touched the views directly, so it was
 * only correct if the worker happened to report from the main thread.
 * GameLibDownloader did (it wrapped every report in withContext(Main));
 * GameFilesTransfer did not, and reported straight from Dispatchers.IO. The
 * result was that tapping "copy files into the app" died instantly with
 * "Animators may only be run on Looper threads" -- LinearProgressIndicator
 * animates its progress, an Animator demands a Looper thread, and the IO
 * dispatcher has none. The dialog then showed that message under the title
 * "download error", which is what the user reported.
 *
 * Fixing it in the caller would have fixed one caller. The rule now lives here,
 * where every present and future caller gets it: the callback marshals to the
 * view's handler itself.
 */
fun showDownloadProgressDialog(
	ctx: Context,
	titleRes: Int,
	cancelable: Boolean,
	scope: CoroutineScope,
	download: suspend ((Long, Long) -> Unit) -> Result<Unit>,
	onSuccess: (() -> Unit)? = null,
) {
	val view = LayoutInflater.from(ctx).inflate(R.layout.dialog_download_progress, null)
	val progressBar = view.findViewById<LinearProgressIndicator>(R.id.downloadProgress)
	val statusText = view.findViewById<TextView>(R.id.downloadStatus)

	val dialog = MaterialAlertDialogBuilder(ctx)
		.setTitle(titleRes)
		.setView(view)
		.setCancelable(cancelable)
		.apply {
			if (cancelable)
				setNegativeButton(android.R.string.cancel) { d, _ -> d.dismiss() }
		}
		.create()

	dialog.show()

	// Throttle. A file copy reports every 256 KB, which is thousands of updates
	// per minute; posting each one floods the main thread's queue with work the
	// user cannot even see (the bar is ~1000 px wide at best). 60 ms is under one
	// update per displayed pixel and still visually continuous. The final update
	// is never dropped, so the bar always ends full.
	var lastPostMs = 0L

	fun report(done: Long, total: Long) {
		val now = System.currentTimeMillis()
		val finished = total > 0 && done >= total

		if (!finished && now - lastPostMs < 60L)
			return

		lastPostMs = now

		// Formatting is thread-safe and not cheap, so it happens off the main
		// thread; only the assignment is posted.
		val doneStr = Formatter.formatShortFileSize(ctx, done)
		val percent = if (total > 0) (done * 100 / total).toInt() else 0
		val text = if (total > 0) {
			ctx.getString(R.string.download_progress, doneStr,
				Formatter.formatShortFileSize(ctx, total))
		} else {
			ctx.getString(R.string.download_progress_unknown, doneStr)
		}

		// View.post hops to the view's Looper from wherever we are. Already on
		// the main thread? Then it is just a queued Runnable.
		view.post {
			if (total > 0) {
				progressBar.isIndeterminate = false
				progressBar.progress = percent
			}
			statusText.text = text
		}
	}

	val job = scope.launch {
		val result = download { done, total -> report(done, total) }

		if (!dialog.isShowing)
			return@launch

		dialog.dismiss()

		if (result.isSuccess) {
			onSuccess?.invoke()
		} else {
			MaterialAlertDialogBuilder(ctx)
				.setTitle(R.string.download_failed)
				.setMessage(result.exceptionOrNull()?.message
					?: ctx.getString(R.string.download_error))
				.setPositiveButton(android.R.string.ok, null)
				.show()
		}
	}

	if (cancelable)
		dialog.setOnDismissListener { job.cancel() }
}
