package su.xash.engine.ui.settings

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.navigation.fragment.findNavController
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import su.xash.engine.model.GameFilesTransfer
import su.xash.engine.util.showDownloadProgressDialog
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.card.MaterialCardView
import su.xash.engine.R
import su.xash.engine.SteamAuthActivity
import su.xash.engine.steam.SteamPresence
import java.io.File

class AppSettingsPreferenceFragment() : PreferenceFragmentCompat() {

	private val folderPickerLauncher = registerForActivityResult(
		ActivityResultContracts.OpenDocumentTree()
	) { uri: Uri? ->
		if (uri == null) return@registerForActivityResult
		val path = convertTreeUriToPath(uri) ?: return@registerForActivityResult

		val folder = File(path)
		val hasValve = File(folder, "valve").isDirectory
		val hasCstrike = File(folder, "cstrike").isDirectory

		if (!hasValve && !hasCstrike) {
			Toast.makeText(
				requireContext(),
				R.string.game_path_invalid_folder,
				Toast.LENGTH_LONG
			).show()
			return@registerForActivityResult
		}

		preferenceManager.sharedPreferences?.edit()
			?.putString("game_path", path)
			?.apply()

		refreshGamePathSummary()

		Toast.makeText(
			requireContext(),
			R.string.game_path_restart_required,
			Toast.LENGTH_SHORT
		).show()
	}

	override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
		preferenceManager.sharedPreferencesName = "app_preferences";
		setPreferencesFromResource(R.xml.app_preferences, rootKey);

		refreshGamePathSummary()
		findPreference<Preference>("game_path")?.setOnPreferenceClickListener {
			showGamePathSheet()
			true
		}

		findPreference<Preference>("game_transfer")?.setOnPreferenceClickListener {
			showGameTransferSheet()
			true
		}

		findPreference<Preference>("crash_logs")?.setOnPreferenceClickListener {
			findNavController().navigate(R.id.action_appSettingsFragment_to_crashLogsFragment)
			true
		}

		setupSteamAccount()
	}

	override fun onResume() {
		super.onResume()
		// The sign-in runs in its own Activity, so refresh when we come back
		refreshSteamSummaries()
	}

	private fun setupSteamAccount() {
		findPreference<Preference>("steam_account")?.setOnPreferenceClickListener {
			val prefs = preferenceManager.sharedPreferences
			val id = prefs?.getString("steam_id", null)

			if (id.isNullOrEmpty()) {
				// Sign in through SteamAuthActivity, which runs Steam's token
				// flow. The older SteamLoginActivity (OpenID) is still in the
				// tree but is no longer reachable from here: its answer is a
				// SteamID64 and nothing else, so an account signed in that way
				// could show an avatar but never a "playing" status.
				startActivity(Intent(requireContext(), SteamAuthActivity::class.java))
			} else {
				// Sign-out drops the token as well as the id. Leaving a token
				// behind would mean the profile could still be made to show a
				// status for an account the user believes is signed out.
				prefs.edit().remove("steam_id").apply()
				SteamPresence.clearCredentials(requireContext())
				SteamPresence.peek()?.shutdown()
				refreshSteamSummaries()
				Toast.makeText(requireContext(), R.string.steam_signed_out_toast, Toast.LENGTH_SHORT).show()
			}
			true
		}
		refreshSteamSummaries()
	}

	private fun refreshSteamSummaries() {
		val prefs = preferenceManager.sharedPreferences ?: return

		val id = prefs.getString("steam_id", null)
		findPreference<Preference>("steam_account")?.summary = if (id.isNullOrEmpty()) {
			getString(R.string.steam_account_signed_out)
		} else {
			getString(R.string.steam_account_signed_in, id)
		}

		val key = prefs.getString("steam_apikey", null)
		findPreference<Preference>("steam_apikey")?.summary = if (key.isNullOrEmpty()) {
			getString(R.string.steam_apikey_unset)
		} else {
			// never show the key itself
			"•".repeat(8) + key.takeLast(4)
		}

		// The presence switch is only meaningful with a token, which the OpenID
		// sign-in never produced. Saying so beats a switch that silently does
		// nothing for accounts signed in the old way.
		val hasToken = !prefs.getString(SteamPresence.KEY_TOKEN, null).isNullOrEmpty()
		val presenceOn = prefs.getBoolean(SteamPresence.KEY_ENABLED, true)
		findPreference<Preference>("steam_presence_enabled")?.summary = when {
			!hasToken -> getString(R.string.steam_presence_needs_signin)
			presenceOn -> getString(R.string.steam_presence_active)
			else -> getString(R.string.steam_presence_off)
		}
	}

	/**
	 * Offers the two ways to place the game files. They are not
	 * interchangeable: the app's own directory is the only place the engine
	 * is guaranteed to be able to write, while a user-chosen folder survives
	 * the app being uninstalled. Previously the tap went straight to the
	 * folder picker, so once a folder had been chosen there was no way back to
	 * the default short of clearing app data.
	 */
	private fun showGamePathSheet() {
		val sheet = BottomSheetDialog(requireContext())
		val view = LayoutInflater.from(requireContext())
			.inflate(R.layout.sheet_game_path, null)

		view.findViewById<TextView>(R.id.game_path_current).text = currentGamePath()

		view.findViewById<MaterialCardView>(R.id.game_path_default)
			.setOnClickListener {
				sheet.dismiss()
				useDefaultGamePath()
			}

		view.findViewById<MaterialCardView>(R.id.game_path_custom)
			.setOnClickListener {
				sheet.dismiss()
				folderPickerLauncher.launch(null)
			}

		sheet.setContentView(view)
		sheet.show()
	}

	/**
	 * Clearing the stored path is what selects the default, rather than
	 * writing the default path in: the directory is derived from the package
	 * name, which differs between the release and .test builds, so a stored
	 * copy would point at the other build's folder after switching.
	 */
	private fun useDefaultGamePath() {
		preferenceManager.sharedPreferences?.edit()
			?.remove("game_path")
			?.apply()

		refreshGamePathSummary()

		Toast.makeText(
			requireContext(),
			R.string.game_path_restart_required,
			Toast.LENGTH_SHORT
		).show()
	}

	private fun currentGamePath(): String {
		val saved = preferenceManager.sharedPreferences?.getString("game_path", null)
		return if (!saved.isNullOrEmpty()) saved else getDefaultGamePath()
	}

	private fun refreshGamePathSummary() {
		val saved = preferenceManager.sharedPreferences?.getString("game_path", null)
		findPreference<Preference>("game_path")?.summary = if (!saved.isNullOrEmpty()) {
			saved
		} else {
			getString(R.string.game_path_reset_to_default, getDefaultGamePath())
		}
	}

	private fun getDefaultGamePath(): String {
		val extDir = requireContext().getExternalFilesDir(null)
		return (extDir?.absolutePath ?: requireContext().filesDir.absolutePath) + "/xash"
	}


	/**
	 * Moving the game files between the folder chosen under "Game data location"
	 * and the app's own directory under Android/data.
	 *
	 * Why this is a separate sheet rather than part of the path sheet: choosing a
	 * path only tells the ENGINE where to look, it does not move a single byte.
	 * The user still had to copy tens of thousands of files by hand -- and on
	 * Android 11+ a file manager cannot reach Android/data on most devices, so
	 * "by hand" was frequently impossible. The app owns that directory, so it can.
	 *
	 * The two directions are deliberately asymmetric, and that asymmetry is the
	 * whole point: import COPIES (source untouched, a failed run costs nothing),
	 * export MOVES (its purpose is to free the app directory).
	 */
	private fun showGameTransferSheet() {
		val sheet = BottomSheetDialog(requireContext())
		val view = LayoutInflater.from(requireContext())
			.inflate(R.layout.sheet_game_transfer, null)

		val folder = userFolderPath()
		val appDir = GameFilesTransfer.appGameDir(requireContext()).absolutePath

		view.findViewById<TextView>(R.id.game_transfer_paths).text = getString(
			R.string.game_transfer_paths,
			folder ?: getString(R.string.game_transfer_no_folder),
			appDir
		)

		view.findViewById<MaterialCardView>(R.id.game_transfer_import)
			.setOnClickListener {
				sheet.dismiss()
				startImport()
			}

		view.findViewById<MaterialCardView>(R.id.game_transfer_export)
			.setOnClickListener {
				sheet.dismiss()
				confirmExport()
			}

		sheet.setContentView(view)
		sheet.show()
	}

	/**
	 * The folder the user picked, or null when app storage is in use.
	 *
	 * Deliberately NOT currentGamePath(): that falls back to the app directory,
	 * and a transfer whose source and destination are the same directory is not a
	 * transfer. Distinguishing "no folder chosen" from "the folder happens to be
	 * the default" is what lets the sheet say something useful instead of failing
	 * halfway through.
	 */
	private fun userFolderPath(): String? =
		preferenceManager.sharedPreferences?.getString("game_path", null)
			?.takeIf { it.isNotEmpty() }

	private fun startImport() {
		val folder = userFolderPath()
		if (folder == null) {
			toast(getString(R.string.game_transfer_no_folder))
			return
		}

		val src = File(folder)
		if (!src.isDirectory) {
			toast(getString(R.string.game_transfer_source_missing, folder))
			return
		}

		// The same content test the folder picker uses. Copying a folder with no
		// game in it would silently produce an app directory the engine cannot
		// start from, and the failure would surface as a crash at launch.
		if (!GameFilesTransfer.looksLikeGameDir(src)) {
			toast(getString(R.string.game_transfer_not_a_game_dir, folder))
			return
		}

		runTransfer(R.string.game_transfer_running_import) { transfer, onProgress ->
			transfer.import(src, GameFilesTransfer.appGameDir(requireContext()), onProgress)
		}
	}

	/**
	 * Export asks first. It is the only destructive action in these settings, and
	 * the dialog names the destination -- "moved somewhere" is not something to
	 * confirm blind.
	 */
	private fun confirmExport() {
		val appDir = GameFilesTransfer.appGameDir(requireContext())
		if (!appDir.isDirectory) {
			toast(getString(R.string.game_transfer_source_missing, appDir.absolutePath))
			return
		}

		// Destination: the chosen folder, or the default the user was told about
		// ("по умолчанию та, с которой в начале скопировали"). The stored path IS
		// that folder -- it is what import read from -- so no extra bookkeeping is
		// needed to remember where the files came from.
		val folder = userFolderPath()
		if (folder == null) {
			toast(getString(R.string.game_transfer_no_folder))
			return
		}

		MaterialAlertDialogBuilder(requireContext())
			.setTitle(R.string.game_transfer_confirm_export_title)
			.setMessage(getString(R.string.game_transfer_confirm_export, folder))
			.setNegativeButton(android.R.string.cancel, null)
			.setPositiveButton(R.string.game_transfer_continue) { _, _ ->
				runTransfer(R.string.game_transfer_running_export) { transfer, onProgress ->
					transfer.export(appDir, File(folder), onProgress)
				}
			}
			.show()
	}

	/**
	 * Runs one transfer behind the shared progress dialog.
	 *
	 * showDownloadProgressDialog already does everything needed here -- a
	 * determinate bar, byte counts, cancellation that cancels the job, and an
	 * error dialog -- so it is reused rather than reimplemented. It speaks in
	 * Result<Unit>, hence the mapping: the failure message has to reach the error
	 * dialog, and a successful transfer reports what it moved.
	 */
	private fun runTransfer(
		titleRes: Int,
		op: suspend (GameFilesTransfer, (Long, Long) -> Unit) -> GameFilesTransfer.Result,
	) {
		val transfer = GameFilesTransfer(requireContext())
		var done: GameFilesTransfer.Stats? = null

		showDownloadProgressDialog(
			ctx = requireContext(),
			titleRes = titleRes,
			cancelable = true,
			scope = lifecycleScope,
			download = { onProgress ->
				when (val r = op(transfer, onProgress)) {
					is GameFilesTransfer.Result.Ok -> {
						done = r.stats
						Result.success(Unit)
					}
					is GameFilesTransfer.Result.Failed ->
						Result.failure(java.io.IOException(r.message))
				}
			},
			onSuccess = {
				val stats = done
				if (stats != null) {
					toast(getString(
						R.string.game_transfer_done,
						stats.files,
						android.text.format.Formatter.formatShortFileSize(
							requireContext(), stats.bytes)
					))
				}
				// The engine reads the path once at startup, so a transfer only
				// takes effect after a restart -- same caveat as changing the path.
				toast(getString(R.string.game_path_restart_required))
			},
		)
	}

	private fun toast(text: String) {
		Toast.makeText(requireContext(), text, Toast.LENGTH_LONG).show()
	}

	private fun convertTreeUriToPath(uri: Uri): String? {
		val docId = try {
			android.provider.DocumentsContract.getTreeDocumentId(uri)
		} catch (e: Exception) {
			return null
		}
		val split = docId.split(":", limit = 2)
		if (split.size != 2) return null
		val volumeId = split[0]
		val relativePath = split[1]
		return if (volumeId.equals("primary", ignoreCase = true)) {
			"/storage/emulated/0/$relativePath"
		} else {
			"/storage/$volumeId/$relativePath"
		}
	}
}
