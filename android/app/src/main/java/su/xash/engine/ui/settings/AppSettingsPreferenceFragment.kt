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
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.card.MaterialCardView
import su.xash.engine.R
import su.xash.engine.SteamLoginActivity
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

		findPreference<Preference>("crash_logs")?.setOnPreferenceClickListener {
			findNavController().navigate(R.id.action_appSettingsFragment_to_crashLogsFragment)
			true
		}

		setupSteamAccount()
	}

	override fun onResume() {
		super.onResume()
		// The sign-in runs in SteamLoginActivity, so refresh when we come back
		refreshSteamSummaries()
	}

	private fun setupSteamAccount() {
		findPreference<Preference>("steam_account")?.setOnPreferenceClickListener {
			val prefs = preferenceManager.sharedPreferences
			val id = prefs?.getString("steam_id", null)

			if (id.isNullOrEmpty()) {
				// Sign in: SteamLoginActivity drives the Steam OpenID web flow
				// and writes steam_id back into these same preferences.
				// realm/returnTo are left unset — the activity falls back to its
				// own callback scheme, which is the same pair the engine passes.
				startActivity(Intent(requireContext(), SteamLoginActivity::class.java))
			} else {
				prefs.edit().remove("steam_id").apply()
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
