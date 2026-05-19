package su.xash.engine.ui.settings

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.navigation.fragment.findNavController
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import su.xash.engine.BuildConfig
import su.xash.engine.R
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

		findPreference<Preference>("game_path")?.summary = path

		Toast.makeText(
			requireContext(),
			R.string.game_path_restart_required,
			Toast.LENGTH_SHORT
		).show()
	}

	override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
		preferenceManager.sharedPreferencesName = "app_preferences";
		setPreferencesFromResource(R.xml.app_preferences, rootKey);

		val gamePathPref = findPreference<Preference>("game_path")
		val savedPath = preferenceManager.sharedPreferences?.getString("game_path", null)
		gamePathPref?.summary = if (!savedPath.isNullOrEmpty()) {
			savedPath
		} else {
			getDefaultGamePath()
		}
		gamePathPref?.setOnPreferenceClickListener {
			folderPickerLauncher.launch(null)
			true
		}

		findPreference<Preference>("crash_logs")?.setOnPreferenceClickListener {
			findNavController().navigate(R.id.action_appSettingsFragment_to_crashLogsFragment)
			true
		}
	}

	private fun getDefaultGamePath(): String {
		return if (BuildConfig.USE_SCOPED_STORAGE) {
			val extDir = requireContext().getExternalFilesDir(null)
			(extDir?.absolutePath ?: requireContext().filesDir.absolutePath) + "/xash"
		} else {
			android.os.Environment.getExternalStorageDirectory().absolutePath + "/xash"
		}
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
