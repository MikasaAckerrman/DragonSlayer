package su.xash.engine;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import javax.net.ssl.HttpsURLConnection;

/**
 * Steam Web API helper for batch avatar downloading.
 * Called from native C pthread via JNI - runs synchronously.
 *
 * Uses GetPlayerSummaries/v2 to resolve avatar URLs for multiple
 * players in one HTTP request, then downloads each avatar image.
 */
public class SteamAPIHelper
{
	private static final String TAG = "SteamAPIHelper";
	private static final int CONNECT_TIMEOUT = 15000;
	private static final int READ_TIMEOUT = 15000;
	private static final int MAX_RESPONSE_SIZE = 262144; // 256 KB
	private static final int MAX_IMAGE_SIZE = 524288;    // 512 KB


	/**
	 * Fetch avatar URLs for multiple players via Steam Web API and download images.
	 *
	 * @param apiKey   Steam Web API key
	 * @param steamIds Comma-separated list of SteamID64 values
	 * @param basePath Base directory to save avatars (e.g. "/sdcard/xash/cstrike/avatars")
	 * @return Number of avatars successfully downloaded, or -1 on fatal error
	 */
	public static int fetchBatchAvatars( String apiKey, String steamIds, String basePath )
	{
		if( apiKey == null || apiKey.isEmpty() )
		{
			Log.w( TAG, "fetchBatchAvatars: no API key" );
			return -1;
		}

		if( steamIds == null || steamIds.isEmpty() )
		{
			Log.w( TAG, "fetchBatchAvatars: no steam IDs" );
			return -1;
		}

		Log.d( TAG, "fetchBatchAvatars: starting for IDs: " + steamIds );

		// Ensure output directory exists
		File outDir = new File( basePath );
		if( !outDir.exists() )
			outDir.mkdirs();

		try
		{
			// Step 1: Call GetPlayerSummaries API
			String apiUrl = "https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v2/"
				+ "?key=" + apiKey
				+ "&steamids=" + steamIds;

			String json = fetchUrl( apiUrl );
			if( json == null )
			{
				Log.e( TAG, "fetchBatchAvatars: API request failed" );
				return -1;
			}

			Log.d( TAG, "fetchBatchAvatars: got response (" + json.length() + " chars)" );

			// Step 2: Parse JSON, collect (steamid, url) pairs, then download
			// in parallel. Sequential downloads were the dominant latency on a
			// full server (8-32 players × ~150 ms RTT each); a small fixed pool
			// gives us ~4x speedup without overwhelming Valve's image CDN or
			// the local socket pool.
			final int PARALLEL_WORKERS = 4;

			class FetchTask
			{
				final String url;
				final String savePath;
				FetchTask( String u, String p ) { url = u; savePath = p; }
			}

			List<FetchTask> tasks = new ArrayList<FetchTask>();
			int searchFrom = 0;

			while( true )
			{
				int sidIdx = json.indexOf( "\"steamid\"", searchFrom );
				if( sidIdx == -1 )
					break;

				String steamid = extractJsonString( json, sidIdx );
				if( steamid == null )
				{
					searchFrom = sidIdx + 10;
					continue;
				}

				int objEnd = json.indexOf( "}", sidIdx );
				if( objEnd == -1 )
					objEnd = json.length();

				String subset = json.substring( sidIdx, objEnd );
				String avatarUrl = null;

				int medIdx = subset.indexOf( "\"avatarmedium\"" );
				if( medIdx != -1 )
					avatarUrl = extractJsonString( subset, medIdx );

				if( avatarUrl == null || avatarUrl.isEmpty() )
				{
					int avIdx = subset.indexOf( "\"avatar\"" );
					if( avIdx != -1 )
						avatarUrl = extractJsonString( subset, avIdx );
				}

				if( avatarUrl != null && !avatarUrl.isEmpty() )
				{
					// Slayer3D: skip Steam's default silhouette avatar (the
					// "?" head shown for accounts that never set a picture).
					// Same fef49e7f... hash is reused across medium/full/small
					// variants, so a substring check is enough.
					if( avatarUrl.contains( "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb" ) )
					{
						Log.d( TAG, "fetchBatchAvatars: default-avatar silhouette for " + steamid + ", skipping" );
					}
					else
					{
						tasks.add( new FetchTask( avatarUrl, basePath + "/" + steamid + ".png" ) );
					}
				}

				searchFrom = objEnd;
			}

			final AtomicInteger downloaded = new AtomicInteger( 0 );

			if( !tasks.isEmpty() )
			{
				ExecutorService pool = Executors.newFixedThreadPool(
					Math.min( PARALLEL_WORKERS, tasks.size() ) );

				for( final FetchTask t : tasks )
				{
					pool.submit( new Runnable() {
						@Override public void run()
						{
							if( downloadAvatarImage( t.url, t.savePath ) )
								downloaded.incrementAndGet();
						}
					} );
				}

				pool.shutdown();
				try
				{
					// Generous total deadline: per-image fetch already has its
					// own 15s connect/read timeouts, so 60s for the whole batch
					// covers up to 4 sequential retries inside the pool.
					if( !pool.awaitTermination( 60, TimeUnit.SECONDS ) )
					{
						Log.w( TAG, "fetchBatchAvatars: parallel pool timed out, forcing shutdown" );
						pool.shutdownNow();
					}
				}
				catch( InterruptedException ie )
				{
					Thread.currentThread().interrupt();
					pool.shutdownNow();
				}
			}

			Log.i( TAG, "fetchBatchAvatars: downloaded " + downloaded.get() + " avatars (parallel x" + PARALLEL_WORKERS + ")" );
			return downloaded.get();
		}
		catch( Exception e )
		{
			Log.e( TAG, "fetchBatchAvatars: exception: " + e.getMessage() );
			return -1;
		}
	}


	/**
	 * Fetch content from a URL via HTTPS.
	 * @return Response body as string, or null on error.
	 */
	private static String fetchUrl( String urlStr )
	{
		HttpsURLConnection conn = null;
		InputStream is = null;

		try
		{
			URL url = new URL( urlStr );
			conn = (HttpsURLConnection) url.openConnection();
			conn.setConnectTimeout( CONNECT_TIMEOUT );
			conn.setReadTimeout( READ_TIMEOUT );
			conn.setRequestProperty( "User-Agent", "Mozilla/5.0" );
			conn.setRequestProperty( "Accept", "application/json" );
			conn.setInstanceFollowRedirects( true );

			int code = conn.getResponseCode();
			if( code != 200 )
			{
				Log.w( TAG, "fetchUrl: HTTP " + code + " for " + urlStr );
				return null;
			}

			is = conn.getInputStream();
			ByteArrayOutputStream baos = new ByteArrayOutputStream();
			byte[] buf = new byte[4096];
			int n;
			int totalRead = 0;

			while( ( n = is.read( buf ) ) != -1 )
			{
				totalRead += n;
				if( totalRead > MAX_RESPONSE_SIZE )
				{
					Log.w( TAG, "fetchUrl: response too large" );
					return null;
				}
				baos.write( buf, 0, n );
			}

			return baos.toString( "UTF-8" );
		}
		catch( IOException e )
		{
			Log.e( TAG, "fetchUrl: " + e.getMessage() );
			return null;
		}
		finally
		{
			if( is != null )
				try { is.close(); } catch( IOException ignored ) {}
			if( conn != null )
				conn.disconnect();
		}
	}

	/**
	 * Download an avatar image from URL and save as PNG.
	 * Re-encodes via BitmapFactory to ensure valid PNG (same as downloadAvatar).
	 */
	private static boolean downloadAvatarImage( String imageUrl, String savePath )
	{
		HttpURLConnection conn = null;
		InputStream is = null;
		File outFile = null;

		try
		{
			URL url = new URL( imageUrl );
			conn = (HttpURLConnection) url.openConnection();
			conn.setConnectTimeout( CONNECT_TIMEOUT );
			conn.setReadTimeout( READ_TIMEOUT );
			conn.setInstanceFollowRedirects( true );

			is = conn.getInputStream();
			ByteArrayOutputStream baos = new ByteArrayOutputStream();
			byte[] buf = new byte[4096];
			int n;
			int totalRead = 0;

			while( ( n = is.read( buf ) ) != -1 )
			{
				totalRead += n;
				if( totalRead > MAX_IMAGE_SIZE )
				{
					Log.w( TAG, "downloadAvatarImage: too large: " + imageUrl );
					return false;
				}
				baos.write( buf, 0, n );
			}

			byte[] imageData = baos.toByteArray();
			Bitmap bitmap = BitmapFactory.decodeByteArray( imageData, 0, imageData.length );
			if( bitmap == null )
			{
				Log.w( TAG, "downloadAvatarImage: decode failed: " + imageUrl );
				return false;
			}

			outFile = new File( savePath );
			File parentDir = outFile.getParentFile();
			if( parentDir != null && !parentDir.exists() )
				parentDir.mkdirs();

			FileOutputStream fos = new FileOutputStream( outFile );
			boolean ok;
			try
			{
				ok = bitmap.compress( Bitmap.CompressFormat.PNG, 100, fos );
				fos.flush();
			}
			finally
			{
				fos.close();
				bitmap.recycle();
			}

			if( !ok )
			{
				outFile.delete();
				return false;
			}

			Log.d( TAG, "downloadAvatarImage: saved " + savePath
				+ " (" + outFile.length() + " bytes)" );
			return true;
		}
		catch( IOException e )
		{
			Log.e( TAG, "downloadAvatarImage: " + e.getMessage() );
			if( outFile != null && outFile.exists() )
				outFile.delete();
			return false;
		}
		finally
		{
			if( is != null )
				try { is.close(); } catch( IOException ignored ) {}
			if( conn != null )
				conn.disconnect();
		}
	}


	/**
	 * Extract a JSON string value starting from a key position.
	 * Finds the pattern: "key" : "value" and returns value.
	 *
	 * @param json The JSON string
	 * @param keyStart Index where the key quote begins (e.g. index of first " in "steamid")
	 * @return The extracted value string, or null if parse fails
	 */
	private static String extractJsonString( String json, int keyStart )
	{
		// Find the colon after the key
		int colonIdx = json.indexOf( ':', keyStart );
		if( colonIdx == -1 )
			return null;

		// Find opening quote of value
		int valStart = json.indexOf( '"', colonIdx + 1 );
		if( valStart == -1 )
			return null;
		valStart++; // skip the quote

		// Find closing quote (handle escaped quotes)
		int valEnd = valStart;
		while( valEnd < json.length() )
		{
			char c = json.charAt( valEnd );
			if( c == '\\' )
			{
				valEnd += 2; // skip escaped char
				continue;
			}
			if( c == '"' )
				break;
			valEnd++;
		}

		if( valEnd >= json.length() )
			return null;

		return json.substring( valStart, valEnd );
	}
}
