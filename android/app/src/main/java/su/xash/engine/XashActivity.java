package su.xash.engine;

import android.annotation.SuppressLint;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
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
import java.io.FileInputStream;
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
		// TELL STEAM WE ARE LEAVING, BEFORE System.exit BELOW.
		//
		// This is the other half of "показывает что я в игре, даже когда не играю".
		// The engine's own shutdown path (CL_Shutdown -> Slayer_SteamPresence_Shutdown)
		// only runs on a clean quit. Android back, the task switcher, or the system
		// reclaiming the activity all reach HERE, and then the System.exit(0) below
		// takes the process -- with it the presence thread and its socket -- without a
		// word to Steam. Steam then holds the session until its own timeout expires and
		// the profile keeps showing the game for minutes afterwards.
		//
		// Best-effort and bounded: shutdown() joins its worker for at most 3 s, and the
		// whole thing is wrapped because nothing here may stop the process from exiting.
		try {
			su.xash.engine.steam.SteamPresence p = su.xash.engine.steam.SteamPresence.peek();

			if (p != null)
				p.shutdown();
		} catch (Throwable e) {
			Log.e(TAG, "onDestroy: presence shutdown: " + e);
		}

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
			SlayerLog.setBaseDir(basedir);
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
			SlayerLog.setBaseDir(rootPath);
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
	/**
	 * First line of an error response body, for the log.
	 *
	 * getInputStream() throws once the status is >= 400; getErrorStream() is the
	 * only way to see what the server actually said. Truncated hard, because
	 * Steam's error page is ~23 KB of HTML and the point is a readable log line.
	 */
	private static String readErrorBody( HttpURLConnection conn )
	{
		InputStream es = null;

		try
		{
			es = conn.getErrorStream();

			if( es == null )
				return "";

			byte[] buf = new byte[512];
			int n = es.read( buf );

			if( n <= 0 )
				return "";

			// Collapse whitespace so a multi-line HTML page stays one log line.
			String body = new String( buf, 0, n, "UTF-8" ).replaceAll( "\\s+", " " ).trim();

			if( body.length() > 180 )
				body = body.substring( 0, 180 ) + "...";

			return body;
		}
		catch( Throwable ignored )
		{
			return "";
		}
		finally
		{
			try { if( es != null ) es.close(); } catch( Throwable ignored ) {}
		}
	}

	public static int downloadAvatar( String steamid64, String savePath )
	{
		final int MAX_XML_SIZE = 262144;   // 256 KB limit for profile XML
		final int MAX_IMAGE_SIZE = 524288; // 512 KB limit for avatar image

		// Hoisted so catch blocks can clean up a partial/corrupt file if
		// fos.flush()/fos.close() throws after a successful bitmap.compress.
		File outFile = null;

		try
		{
			SlayerLog.deriveFrom( savePath );
			SlayerLog.log( "downloadAvatar", steamid64 + " -> " + savePath );

			// Phase 1 - Fetch Steam profile XML
			URL profileUrl = new URL( "https://steamcommunity.com/profiles/" + steamid64 + "/?xml=1" );
			HttpURLConnection conn = (HttpURLConnection) profileUrl.openConnection();
			conn.setConnectTimeout( 15000 );
			conn.setReadTimeout( 15000 );
			conn.setRequestProperty( "User-Agent", "Mozilla/5.0" );
			conn.setInstanceFollowRedirects( true );

			// A missing profile answers 404. On a Steam-emulator server
			// (jailbreak, RevEmu and the like) the players' SteamIDs are made
			// up, so their profiles do not exist and never will. Reading the
			// body would throw FileNotFoundException, indistinguishable from a
			// transient network error, which armed a 60s retry that ran forever.
			// Check the status first and report GONE so the engine stops asking.
			// THE STATUS CODE IS ALWAYS LOGGED, even on success.
			//
			// This is the fix for a diagnosis that went wrong twice: the old code
			// tested for 404/410 and let every other failure fall through to
			// getInputStream(), which on Android throws FileNotFoundException for
			// ANY status >= 400 -- OkHttp sits underneath HttpURLConnection and
			// does not restrict that exception to 404 the way desktop JDK does.
			// So a 403 or a 429 arrived as "FAIL network: FileNotFoundException"
			// and got read as "this profile does not exist". The number that would
			// have settled it was in hand the whole time and never written down.
			int httpCode = conn.getResponseCode();

			// 404/410: no such profile. On an emulator server the ids are made up,
			// so this is permanent -- GONE stops the 60 s retry that otherwise ran
			// for the rest of the session.
			if( httpCode == 404 || httpCode == 410 )
			{
				conn.disconnect();
				SlayerLog.log( "downloadAvatar", steamid64 + " GONE, no Steam profile (HTTP " + httpCode + ")" );
				return 5;   // AVD_RESULT_GONE: do not retry this session
			}

			// Anything else >= 400 is NOT about this profile: rate limiting,
			// blocking, an outage. Retryable, and the body plus a couple of headers
			// go into the log because they are what names the cause.
			if( httpCode >= 400 )
			{
				String detail = readErrorBody( conn );
				String retryAfter = conn.getHeaderField( "Retry-After" );
				String server = conn.getHeaderField( "Server" );

				conn.disconnect();
				SlayerLog.log( "downloadAvatar", steamid64 + " FAIL HTTP " + httpCode
					+ ( retryAfter != null ? " Retry-After=" + retryAfter : "" )
					+ ( server != null ? " server=" + server : "" )
					+ ( detail.length() > 0 ? " body=" + detail : "" ));
				// 429/503 is a statement about US, not about this profile: we asked too
				// often. The device log is unambiguous -- every id got 429 from nginx,
				// including ids whose profiles load fine in a browser. Backing off this
				// one slot is not enough, because the limit counts the CLIENT, so this
				// returns a distinct code and the engine stands every slot down.
				if( httpCode == 429 || httpCode == 503 )
					return 6;   // AVD_RESULT_THROTTLED: hold ALL slots back

				return 1;   // AVD_RESULT_FAIL: transient, worth retrying
			}

			if( httpCode != 200 )
				SlayerLog.log( "downloadAvatar", steamid64 + " HTTP " + httpCode + " (unusual, continuing)" );

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
						SlayerLog.log( "downloadAvatar", steamid64 + " FAIL xml too large (>" + MAX_XML_SIZE + " bytes)" );
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
				SlayerLog.log( "downloadAvatar", steamid64 + " FAIL profile is private" );
				return 2;
			}

			// A profile that was never SET UP. Steam answers HTTP 200 with a short
			// XML carrying only steamID64 and a privacyMessage -- no avatar tags at
			// all, and no privacyState either. MEASURED against the reporting
			// device's own session: of the 8 players in the last map, 4 answered
			// exactly this way.
			//
			// WHY IT NEEDS ITS OWN ANSWER: falling through to the "no avatar URL"
			// return below reports an ordinary failure, which arms the 60-second
			// retry -- and this player will never grow an avatar, so the retry runs
			// for the rest of the session. That is a large part of the download
			// storm in the log (63 requests, 30 workers, 30 failures in one map).
			// GONE means "not again this session", which here is simply true.
			if( xml.indexOf( "<privacyMessage>" ) != -1
			 && xml.indexOf( "<avatarFull>" ) == -1
			 && xml.indexOf( "<avatarMedium>" ) == -1 )
			{
				SlayerLog.log( "downloadAvatar",
					steamid64 + " GONE, profile never set up (no avatar in XML, "
					+ xml.length() + " chars)" );
				return 5;   // AVD_RESULT_GONE
			}

			// Phase 2 - Parse XML for avatar URL.
			// Prefer avatarFull (184x184): the scoreboard now draws icons at
			// roughly three glyph heights, and avatarMedium is only 64x64, so it
			// was being upscaled and looked soft.
			String avatarUrl = extractTagContent( xml, "avatarFull" );
			if( avatarUrl == null )
				avatarUrl = extractTagContent( xml, "avatarMedium" );

			if( avatarUrl == null || avatarUrl.isEmpty() )
			{
				SlayerLog.log( "downloadAvatar", steamid64 + " FAIL no avatar URL in XML (" + xml.length() + " chars)" );
				return 2;
			}

			// Steam avatar filenames are a content hash, so the URL changes the
			// moment the user changes their picture — and only then. Compare it
			// against the one saved next to the cached PNG: unchanged means we
			// can skip the image download entirely. That is what makes frequent
			// re-checks cheap, and it replaces the old fixed cache lifetime,
			// which could not notice a change until it expired.
			File urlSidecar = new File( savePath + ".url" );
			File cachedPng = new File( savePath );

			if( cachedPng.exists() && urlSidecar.exists() )
			{
				try
				{
					byte[] prev = new byte[(int) urlSidecar.length()];
					FileInputStream fis = new FileInputStream( urlSidecar );
					try { fis.read( prev ); } finally { fis.close(); }

					if( avatarUrl.equals( new String( prev, "UTF-8" ).trim() ) )
					{
						SlayerLog.log( "downloadAvatar", steamid64 + " UNCHANGED, image download skipped" );
						return 4;   // AVD_RESULT_UNCHANGED
					}
				}
				catch( Exception e )
				{
					// Unreadable sidecar just means we re-download.
					SlayerLog.log( "downloadAvatar", steamid64 + " sidecar unreadable, will re-download: " + e );
				}
			}

			SlayerLog.log( "downloadAvatar", steamid64 + " CHANGED, fetching image " + avatarUrl );

			// Phase 3 - Download the avatar image
			URL imageUrl = new URL( avatarUrl );
			HttpURLConnection imgConn = (HttpURLConnection) imageUrl.openConnection();
			imgConn.setConnectTimeout( 15000 );
			imgConn.setReadTimeout( 15000 );

			byte[] imageData;
			InputStream imgIs = null;
			try
			{
				// Same reasoning as the profile fetch above: log the code, and tell a
				// throttled CDN apart from a missing image. Different host, so this
				// can fail while the profile request succeeds.
				// No disconnect() in this branch: the finally block below owns the
				// connection, and closing it twice is not behaviour to rely on.
				int imgCode = imgConn.getResponseCode();
				if( imgCode != 200 )
				{
					String detail = readErrorBody( imgConn );
					SlayerLog.log( "downloadAvatar", steamid64 + " FAIL image HTTP " + imgCode
						+ ( detail.length() > 0 ? " body=" + detail : "" ));

					// Same throttle signal when the image CDN applies its own limit.
					if( imgCode == 429 || imgCode == 503 )
						return 6;

					return 1;
				}

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
						SlayerLog.log( "downloadAvatar", steamid64 + " FAIL image too large (>" + MAX_IMAGE_SIZE + " bytes)" );
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

			// Phase 4 - Decode the image (Steam returns JPEG, but engine imagelib
			// has no JPEG loader) and re-encode as a real PNG so GL_LoadTexture
			// can read it. Also handles WebP / any other format the platform
			// decoder accepts. Bitmap.compress is bounded by image dimensions,
			// not arbitrary input size, so re-encoded files stay small.
			SlayerLog.log( "downloadAvatar", steamid64 + " HTTP ok, " + imageData.length + " bytes" );

			Bitmap bitmap = BitmapFactory.decodeByteArray( imageData, 0, imageData.length );
			if( bitmap == null )
			{
				SlayerLog.log( "downloadAvatar", steamid64 + " FAIL decode returned null (unsupported image or HTML error page)" );
				return 3;
			}

			SlayerLog.log( "downloadAvatar", steamid64 + " decoded " + bitmap.getWidth() + "x" + bitmap.getHeight() );

			outFile = new File( savePath );
			File parentDir = outFile.getParentFile();
			if( parentDir != null )
				parentDir.mkdirs();

			boolean compressed = false;
			try
			{
				FileOutputStream fos = new FileOutputStream( outFile );
				try
				{
					compressed = bitmap.compress( Bitmap.CompressFormat.PNG, 100, fos );
					fos.flush();
				}
				finally
				{
					fos.close();
				}
			}
			finally
			{
				bitmap.recycle();
			}

			if( !compressed )
			{
				SlayerLog.log( "downloadAvatar", steamid64 + " FAIL PNG re-encode for " + savePath );
				outFile.delete();
				return 3;
			}

			// Record which avatar URL this PNG came from, so the next check can
			// tell "unchanged" from "needs re-download" without fetching the image.
			try
			{
				FileOutputStream ufos = new FileOutputStream( urlSidecar );
				try { ufos.write( avatarUrl.getBytes( "UTF-8" ) ); }
				finally { ufos.close(); }
			}
			catch( Exception e )
			{
				// Not fatal: without the sidecar we simply re-download next time.
				SlayerLog.log( "downloadAvatar", steamid64 + " sidecar write failed, next check will re-download: " + e );
			}

			SlayerLog.log( "downloadAvatar", steamid64 + " OK saved " + outFile.length() + " bytes -> " + savePath );
			return 0;
		}
		catch( IOException e )
		{
			SlayerLog.log( "downloadAvatar", steamid64 + " FAIL network: " + e );
			if( outFile != null && outFile.exists() )
				outFile.delete();
			return 1;
		}
		catch( Exception e )
		{
			SlayerLog.log( "downloadAvatar", steamid64 + " FAIL parse: " + e );
			if( outFile != null && outFile.exists() )
				outFile.delete();
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

	// =========================================================================
	// Steam OpenID Login - launches SteamLoginActivity WebView
	// =========================================================================

	/**
	 * Start the Steam OpenID login WebView Activity.
	 * Called from native C via JNI.
	 *
	 * @param realm    OpenID realm URL
	 * @param returnTo OpenID return_to URL
	 */
	public static void startSteamLogin( String realm, String returnTo )
	{
		android.content.Context ctx = SDLActivity.getContext();
		if( ctx == null )
		{
			Log.e( TAG, "startSteamLogin: no context" );
			return;
		}

		android.content.Intent intent = new android.content.Intent( ctx, SteamLoginActivity.class );
		intent.putExtra( "realm", realm );
		intent.putExtra( "returnTo", returnTo );
		intent.addFlags( android.content.Intent.FLAG_ACTIVITY_NEW_TASK );
		ctx.startActivity( intent );
		Log.i( TAG, "startSteamLogin: launched SteamLoginActivity" );
	}

	/**
	 * Current SteamID64 held by the launcher, or 0 when signed out.
	 * Called from native C via JNI.
	 *
	 * The launcher's Settings screen is the single source of truth for the
	 * account: signing out there removes this key. The engine keeps its own
	 * saved copy for offline start-up, and reconciles it against this on init —
	 * otherwise signing out in the launcher left the engine still believing it
	 * was signed in, so the banner and the avatar kept showing.
	 */
	public static long getSteamId()
	{
		android.content.Context ctx = SDLActivity.getContext();
		if( ctx == null )
			return 0;

		try
		{
			String id = ctx.getSharedPreferences( "app_preferences", MODE_PRIVATE )
				.getString( "steam_id", null );

			if( id == null || id.isEmpty() )
				return 0;

			return Long.parseLong( id );
		}
		catch( Exception e )
		{
			Log.e( TAG, "getSteamId: " + e.getMessage() );
			return 0;
		}
	}

	/**
	 * Native callback for Steam login result.
	 * Called by SteamLoginActivity when login completes or fails.
	 *
	 * @param steamid64 The SteamID64, or -1 if login failed/cancelled
	 */
	public static native void nativeSteamLoginResult( long steamid64 );

	// =========================================================================
	// Steam rich presence — the "playing Counter-Strike" status
	// =========================================================================
	//
	// These are the engine's only way in; everything else lives in
	// su.xash.engine.steam. The engine calls them from its own thread and
	// never blocks: SteamPresence owns a background thread for the network.
	//
	// Note how this differs from startSteamLogin() above. That one runs Steam
	// OpenID, whose entire answer is a SteamID64 — an identity, not a session.
	// A status needs a credential, which is what the token flow in
	// SteamAuthActivity produces.

	/**
	 * Announce a game as being played. Called from native C via JNI.
	 *
	 * @param appid      Steam app id; 10 is Counter-Strike
	 * @param extraInfo  optional title shown beside the game
	 * @param serverIp   dotted-quad server address, or empty
	 * @param serverPort server port, or 0
	 * @return 1 if a Steam token is stored and the status was requested, else 0
	 */
	public static int steamPresenceStart( long appid, String extraInfo,
		String serverIp, int serverPort )
	{
		android.content.Context ctx = SDLActivity.getContext();

		if( ctx == null )
		{
			Log.e( TAG, "steamPresenceStart: no context" );
			return 0;
		}

		try
		{
			su.xash.engine.steam.SteamPresence p =
				su.xash.engine.steam.SteamPresence.get( ctx );

			if( !p.isAvailable() )
				return 0;

			p.start( appid, extraInfo,
				serverIp != null && serverIp.length() > 0 ? serverIp : null,
				serverPort );
			return 1;
		}
		catch( Throwable e )
		{
			// A broken status must never take the game down with it.
			Log.e( TAG, "steamPresenceStart: " + e );
			return 0;
		}
	}

	/** Clear the status. Called from native C via JNI. */
	public static void steamPresenceStop()
	{
		try
		{
			su.xash.engine.steam.SteamPresence p =
				su.xash.engine.steam.SteamPresence.peek();

			if( p != null )
				p.stop();
		}
		catch( Throwable e )
		{
			Log.e( TAG, "steamPresenceStop: " + e );
		}
	}

	/** Tear down the session; called when the engine shuts down. */
	public static void steamPresenceShutdown()
	{
		try
		{
			su.xash.engine.steam.SteamPresence p =
				su.xash.engine.steam.SteamPresence.peek();

			if( p != null )
				p.shutdown();
		}
		catch( Throwable e )
		{
			Log.e( TAG, "steamPresenceShutdown: " + e );
		}
	}

	/**
	 * Whether a usable Steam credential is stored, so the engine can tell the
	 * player why nothing is showing instead of failing silently.
	 * Called from native C via JNI.
	 */
	public static int steamPresenceAvailable()
	{
		android.content.Context ctx = SDLActivity.getContext();

		if( ctx == null )
			return 0;

		try
		{
			return su.xash.engine.steam.SteamPresence.get( ctx ).isAvailable() ? 1 : 0;
		}
		catch( Throwable e )
		{
			Log.e( TAG, "steamPresenceAvailable: " + e );
			return 0;
		}
	}

	// -----------------------------------------------------------------------
	// Real Steam auth ticket, for the connect packet.
	// -----------------------------------------------------------------------
	//
	// WHY THIS IS SEPARATE FROM steamPresence*: presence is fire-and-forget and
	// must never block a frame, so it hands the work to a background thread. A
	// ticket is the opposite -- the engine cannot build the connect packet
	// without it, so this one BLOCKS, and it is called from the connect path
	// rather than from the frame loop.
	//
	// The two-call shape (fetch, then read the id) is because JNI cannot hand
	// back a byte array and a long together without an object, and an object is
	// one more thing for R8 to strip. The id is kept from the last fetch.

	private static long steamTicketId;

	/**
	 * Ask Steam for an app-ownership ticket for CS 1.6. Called from native C.
	 *
	 * @param timeoutMs how long to wait for Steam to answer
	 * @return ticket bytes, or null when unavailable (no credentials stored, the
	 *         account does not own the game, or Steam refused) -- the engine then
	 *         falls back to the emulated ticket it has always used
	 */
	public static byte[] steamFetchAuthTicket( int timeoutMs )
	{
		android.content.Context ctx = SDLActivity.getContext();

		steamTicketId = 0;

		if( ctx == null )
		{
			Log.e( TAG, "steamFetchAuthTicket: no context" );
			return null;
		}

		try
		{
			su.xash.engine.steam.SteamTicket.Result r =
				su.xash.engine.steam.SteamTicket.fetch( ctx, timeoutMs );

			if( r == null )
				return null;

			steamTicketId = r.steamid;
			return r.ticket;
		}
		catch( Throwable e )
		{
			// A nicer ticket is an improvement, never a requirement: anything going
			// wrong here must still leave the player able to join a server.
			Log.e( TAG, "steamFetchAuthTicket: " + e );
			return null;
		}
	}

	/**
	 * SteamID64 the last successful steamFetchAuthTicket() belongs to.
	 *
	 * Read straight after the fetch. It comes from the session that obtained the
	 * ticket rather than from stored preferences, because a ticket only proves
	 * the account that was logged on when Steam issued it.
	 */
	public static long steamAuthTicketSteamId()
	{
		return steamTicketId;
	}
}
