package su.xash.engine;

import android.annotation.SuppressLint;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings.Secure;
import android.util.Log;
import android.view.KeyEvent;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import su.xash.engine.BuildConfig;
import su.xash.engine.util.AndroidBug5497Workaround;
import su.xash.engine.util.CrashReports;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Arrays;
import java.util.List;

public class XashActivity extends SDLActivity {
	private boolean mUseVolumeKeys;
	private String mPackageName;
	private static final String TAG = "XashActivity";

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);

		setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
			//getWindow().addFlags(WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES);
			getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
		}

		AndroidBug5497Workaround.assistActivity(this);
	}

	@Override
	public void onDestroy() {
		super.onDestroy();

		// Now that we don't exit from native code, we need to exit here, resetting
		// application state (actually global variables that we don't cleanup on exit)
		//
		// When the issue with global variables will be resolved, remove that exit() call
		System.exit(0);
	}

	@Override
	protected String[] getLibraries() {
		return new String[]{"SDL2", "xash"};
	}

	@SuppressLint("HardwareIds")
	private String getAndroidID() {
		return Secure.getString(getContentResolver(), Secure.ANDROID_ID);
	}

	@SuppressLint("ApplySharedPref")
	private void saveAndroidID(String id) {
		getSharedPreferences("xash_preferences", MODE_PRIVATE).edit().putString("xash_id", id).commit();
	}

	private String loadAndroidID() {
		return getSharedPreferences("xash_preferences", MODE_PRIVATE).getString("xash_id", "");
	}

	@Override
	public String getCallingPackage() {
		if (mPackageName != null) {
			return mPackageName;
		}

		return super.getCallingPackage();
	}

	private AssetManager getAssets(boolean isEngine) {
		AssetManager am = null;

		if (isEngine) {
			am = getAssets();
		} else {
			try {
				am = getPackageManager().getResourcesForApplication(getCallingPackage()).getAssets();
			} catch (Exception e) {
				Log.e(TAG, "Unable to load mod assets!");
				e.printStackTrace();
			}
		}

		return am;
	}

	private String[] getAssetsList(boolean isEngine, String path) {
		AssetManager am = getAssets(isEngine);

		try {
			return am.list(path);
		} catch (Exception e) {
			e.printStackTrace();
		}

		return new String[]{};
	}

	@Override
	public boolean dispatchKeyEvent(KeyEvent event) {
		if (SDLActivity.mBrokenLibraries) {
			return false;
		}

		int keyCode = event.getKeyCode();
		if (!mUseVolumeKeys) {
			if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN || keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_CAMERA || keyCode == KeyEvent.KEYCODE_ZOOM_IN || keyCode == KeyEvent.KEYCODE_ZOOM_OUT) {
				return false;
			}
		}

		return getWindow().superDispatchKeyEvent(event);
	}

	private static void appendStringExtra(StringBuilder sb, Intent intent, String key) {
		String value = intent.getStringExtra(key);
		if (value != null)
			sb.append("  ").append(key).append(" = ").append(value).append('\n');
	}

	// record intent info, so that it could be consumed later for crash reporting
	private void recordLaunchInfo() {
		// do not overwrite current launch info with pending crash log, shouldn't happen but might
		File pendingCrash = new File(getFilesDir(), "crashes/" + CrashReports.STACKTRACE_NAME);
		if (pendingCrash.exists() && pendingCrash.length() > 0)
			return;

		// write Android version, fingerprint, supported abis, etc
		CrashReports.writeSystemInfo(this);

		// now create intent info and pass it to crash reporting
		Intent intent = getIntent();
		if (intent == null)
			return;
		StringBuilder sb = new StringBuilder();
		sb.append("Action: ").append(intent.getAction()).append('\n');
		sb.append("Data: ").append(intent.getDataString()).append('\n');
		sb.append("Calling package: ").append(getCallingPackage()).append('\n');
		sb.append("Extras:\n");
		// only write intent extras that we care about
		appendStringExtra(sb, intent, "gamedir");
		appendStringExtra(sb, intent, "gamelibdir");
		appendStringExtra(sb, intent, "pakfile");
		appendStringExtra(sb, intent, "basedir");
		appendStringExtra(sb, intent, "package");
		appendStringExtra(sb, intent, "argv");
		sb.append("  usevolume = ").append(intent.getBooleanExtra("usevolume", false)).append('\n');
		String[] env = intent.getStringArrayExtra("env");
		if (env != null)
			sb.append("  env = ").append(Arrays.toString(env)).append('\n');
		CrashReports.writeIntentInfo(this, sb.toString());
	}

	// TODO: REMOVE LATER, temporary launchers support?
	@Override
	protected String[] getArguments() {
		File crashDir = new File(getFilesDir(), "crashes");
		crashDir.mkdirs();
		nativeSetenv("XASH3D_CRASH_DIR", crashDir.getAbsolutePath());

		recordLaunchInfo();

		String gamedir = getIntent().getStringExtra("gamedir");
		if (gamedir == null) gamedir = "valve";
		nativeSetenv("XASH3D_GAME", gamedir);

		String gamelibdir = getIntent().getStringExtra("gamelibdir");
		if (gamelibdir != null) nativeSetenv("XASH3D_GAMELIBDIR", gamelibdir);

		String rodir = System.getenv("XASH3D_RODIR");
		if (rodir == null) {
			// FIXME: we are using rodir as a supplier for downloaded game libraries
			rodir = getFilesDir().getAbsolutePath() + "/gamelibs";
			nativeSetenv("XASH3D_RODIR", rodir);
		}
		Log.i(TAG, "XASH3D_RODIR = " + rodir);

		String pakfile = getIntent().getStringExtra("pakfile");
		if (pakfile != null) nativeSetenv("XASH3D_EXTRAS_PAK2", pakfile);

		String basedir = getIntent().getStringExtra("basedir");
		if (basedir != null) {
			nativeSetenv("XASH3D_BASEDIR", basedir);
		} else {
			String gamePath = getSharedPreferences("app_preferences", MODE_PRIVATE)
				.getString("game_path", null);
			String rootPath;
			if (gamePath != null && !gamePath.isEmpty()) {
				rootPath = gamePath;
			} else {
				File extDir = getExternalFilesDir(null);
				rootPath = (extDir != null ? extDir.getAbsolutePath() : getFilesDir().getAbsolutePath()) + "/xash";
			}
			nativeSetenv("XASH3D_BASEDIR", rootPath);
		}

		mUseVolumeKeys = getIntent().getBooleanExtra("usevolume", false);
		mPackageName = getIntent().getStringExtra("package");

		String[] env = getIntent().getStringArrayExtra("env");
		if (env != null) {
			for (int i = 0; i < env.length; i += 2)
				nativeSetenv(env[i], env[i + 1]);
		}

		String argv = getIntent().getStringExtra("argv");
		if (argv == null) argv = "-console -log";

		return argv.split(" ");
	}

	/**
	 * Downloads a Steam avatar image to disk.
	 * Called from native C pthread via JNI - runs synchronously.
	 *
	 * @param steamid64 The Steam64 ID of the player
	 * @param savePath  Absolute path to save the avatar image
	 * @return 0=success, 1=network error, 2=profile private/not found, 3=parse error
	 */
	public static int downloadAvatar( String steamid64, String savePath )
	{
		final int MAX_XML_SIZE = 262144;   // 256 KB limit for profile XML
		final int MAX_IMAGE_SIZE = 524288; // 512 KB limit for avatar image

		try
		{
			Log.d( TAG, "downloadAvatar: fetching profile XML for " + steamid64 );

			// Phase 1 - Fetch Steam profile XML
			URL profileUrl = new URL( "https://steamcommunity.com/profiles/" + steamid64 + "/?xml=1" );
			HttpURLConnection conn = (HttpURLConnection) profileUrl.openConnection();
			conn.setConnectTimeout( 15000 );
			conn.setReadTimeout( 15000 );
			conn.setRequestProperty( "User-Agent", "Mozilla/5.0" );
			conn.setInstanceFollowRedirects( true );

			String xml;
			InputStream is = null;
			try
			{
				is = conn.getInputStream();
				ByteArrayOutputStream baos = new ByteArrayOutputStream();
				byte[] buf = new byte[4096];
				int n;
				int totalRead = 0;
				while( ( n = is.read( buf ) ) != -1 )
				{
					totalRead += n;
					if( totalRead > MAX_XML_SIZE )
					{
						Log.d( TAG, "downloadAvatar: XML response too large, aborting" );
						return 1;
					}
					baos.write( buf, 0, n );
				}
				xml = baos.toString( "UTF-8" );
			}
			finally
			{
				if( is != null )
					is.close();
				conn.disconnect();
			}

			// Check for private profile
			if( xml.indexOf( "<privacyState>private</privacyState>" ) != -1 )
			{
				Log.d( TAG, "downloadAvatar: profile is private" );
				return 2;
			}

			// Phase 2 - Parse XML for avatar URL
			String avatarUrl = extractTagContent( xml, "avatarMedium" );
			if( avatarUrl == null )
				avatarUrl = extractTagContent( xml, "avatarFull" );

			if( avatarUrl == null || avatarUrl.isEmpty() )
			{
				Log.d( TAG, "downloadAvatar: no avatar URL found" );
				return 2;
			}

			Log.d( TAG, "downloadAvatar: downloading image from " + avatarUrl );

			// Phase 3 - Download the avatar image
			URL imageUrl = new URL( avatarUrl );
			HttpURLConnection imgConn = (HttpURLConnection) imageUrl.openConnection();
			imgConn.setConnectTimeout( 15000 );
			imgConn.setReadTimeout( 15000 );

			byte[] imageData;
			InputStream imgIs = null;
			try
			{
				imgIs = imgConn.getInputStream();
				ByteArrayOutputStream imgBaos = new ByteArrayOutputStream();
				byte[] buf = new byte[4096];
				int n;
				int totalRead = 0;
				while( ( n = imgIs.read( buf ) ) != -1 )
				{
					totalRead += n;
					if( totalRead > MAX_IMAGE_SIZE )
					{
						Log.d( TAG, "downloadAvatar: image too large, aborting" );
						return 1;
					}
					imgBaos.write( buf, 0, n );
				}
				imageData = imgBaos.toByteArray();
			}
			finally
			{
				if( imgIs != null )
					imgIs.close();
				imgConn.disconnect();
			}

			// Phase 4 - Save to disk
			File outFile = new File( savePath );
			File parentDir = outFile.getParentFile();
			if( parentDir != null )
				parentDir.mkdirs();

			FileOutputStream fos = new FileOutputStream( outFile );
			try
			{
				fos.write( imageData );
			}
			finally
			{
				fos.close();
			}

			Log.d( TAG, "downloadAvatar: saved to " + savePath );
			return 0;
		}
		catch( IOException e )
		{
			Log.d( TAG, "downloadAvatar: network error: " + e.getMessage() );
			return 1;
		}
		catch( Exception e )
		{
			Log.d( TAG, "downloadAvatar: parse error: " + e.getMessage() );
			return 3;
		}
	}

	private static String extractTagContent( String xml, String tagName )
	{
		String openTag = "<" + tagName + ">";
		String closeTag = "</" + tagName + ">";
		int start = xml.indexOf( openTag );
		if( start == -1 )
			return null;
		start += openTag.length();
		int end = xml.indexOf( closeTag, start );
		if( end == -1 )
			return null;

		String content = xml.substring( start, end ).trim();

		// Strip CDATA if present
		if( content.startsWith( "<![CDATA[" ) && content.endsWith( "]]>" ) )
			content = content.substring( 9, content.length() - 3 ).trim();

		return content;
	}
}
