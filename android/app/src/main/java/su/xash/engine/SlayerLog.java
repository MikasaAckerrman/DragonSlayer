package su.xash.engine;

import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Slayer3D file-based diagnostic log for the Java half of the engine.
 *
 * The engine already logs to {@code <gamedir>/logs/slayer_diag.log} from C
 * (see cl_slayer_log.c), but avatar downloading lives in Java, and logcat is
 * not reachable on a phone without a PC. The result was a blind spot: the C log
 * showed avatar downloads being requested over and over with nothing about how
 * any of them ended, because the outcome was only ever written to logcat.
 *
 * This writes next to the C log so a single pull gets both halves of the story.
 *
 * Thread-safe: {@code downloadAvatar} is called from native pthreads, and the
 * batch fetcher runs on its own thread, so several callers can log at once.
 */
public final class SlayerLog
{
	private static final String TAG = "SlayerLog";

	/** Rotate at 1 MB so a long session cannot fill the user's storage. */
	private static final long MAX_BYTES = 1024 * 1024;

	private static final Object LOCK = new Object();

	private static File    sLogFile;
	private static boolean sResolveFailed;
	private static String  sBaseDir;

	private SlayerLog() { }

	/**
	 * Records the engine base directory, used as a fallback location when no
	 * avatar path has been seen yet. Call once during startup.
	 */
	public static void setBaseDir( String baseDir )
	{
		synchronized( LOCK )
		{
			sBaseDir = baseDir;
		}
	}

	/**
	 * Derives the log location from an avatar cache path
	 * ({@code <gamedir>/.../<id>.png}), so the Java log lands in the same
	 * gamedir the engine writes its own log to. Cheap to call repeatedly.
	 */
	public static void deriveFrom( String avatarSavePath )
	{
		if( avatarSavePath == null )
			return;

		synchronized( LOCK )
		{
			if( sLogFile != null )
				return;

			// <gamedir>/avatars/<id>.png -> <gamedir>, and it keeps working if
			// the cache moves deeper, e.g. <gamedir>/downloaded/avatars/<id>.png.
			File dir = new File( avatarSavePath ).getParentFile();
			if( dir != null && "avatars".equals( dir.getName() ) )
				dir = dir.getParentFile();
			if( dir != null && "downloaded".equals( dir.getName() ) )
				dir = dir.getParentFile();

			if( dir != null )
			{
				sLogFile = new File( dir, "logs/slayer_java.log" );
				sResolveFailed = false;
			}
		}
	}

	/**
	 * Appends one timestamped line. Never throws: diagnostics must not be able
	 * to break the thing they are diagnosing. Also mirrors to logcat, which is
	 * still the more convenient channel when a PC is attached.
	 */
	public static void log( String message )
	{
		message = redact( message );

		Log.d( TAG, message );

		synchronized( LOCK )
		{
			File target = resolveLocked();
			if( target == null )
				return;

			try
			{
				if( target.length() > MAX_BYTES )
				{
					// Single generation of history: the previous file is kept as
					// .old so a bug that took a while to show is not lost.
					File old = new File( target.getPath() + ".old" );
					if( old.exists() && !old.delete() )
						Log.w( TAG, "could not remove " + old );
					if( !target.renameTo( old ) && !target.delete() )
						Log.w( TAG, "could not rotate " + target );
				}

				String line = TIMESTAMP.format( new Date() ) + " " + message + "\n";

				FileOutputStream fos = new FileOutputStream( target, true );
				try
				{
					fos.write( line.getBytes( "UTF-8" ) );
				}
				finally
				{
					fos.close();
				}
			}
			catch( IOException e )
			{
				// Give up on the file for this session rather than retrying (and
				// failing) on every subsequent line.
				Log.w( TAG, "disabling file log: " + e.getMessage() );
				sLogFile = null;
				sResolveFailed = true;
			}
		}
	}

	/** Convenience overload for the common "label: detail" shape. */
	public static void log( String label, String detail )
	{
		log( label + ": " + detail );
	}

	/**
	 * Removes secrets from a line before it reaches the log file or logcat.
	 *
	 * A Steam Web API key travels in the query string, so any code that logs a
	 * failing URL leaks it -- which is what happened: a diagnostic log the user
	 * sent for an unrelated problem carried the key in plain text, in every
	 * GetPlayerSummaries line. Redacting at this single choke point is what
	 * makes it safe; asking each call site to remember would not survive the
	 * next one that is added.
	 *
	 * The key is kept identifiable by its last four characters so two different
	 * keys can still be told apart in a log, which is the only reason the value
	 * was ever wanted there.
	 */
	static String redact( String message )
	{
		if( message == null )
			return null;

		int at = indexOfIgnoreCase( message, "key=" );
		if( at < 0 )
			return message;

		StringBuilder out = new StringBuilder( message.length() );
		int pos = 0;

		while( at >= 0 )
		{
			int valueStart = at + 4;   // past "key="
			int valueEnd = valueStart;

			// A query value ends at the next separator, not at the end of the
			// string: "?key=SECRET&steamids=..." must keep the ids readable.
			while( valueEnd < message.length() )
			{
				char c = message.charAt( valueEnd );

				if( c == '&' || c == ' ' || c == '"' || c == '\'' || c == '\n' )
					break;
				valueEnd++;
			}

			out.append( message, pos, valueStart );

			int len = valueEnd - valueStart;
			if( len == 0 )
			{
				// "key=" with nothing after it: nothing to hide.
			}
			else if( len <= 4 )
			{
				out.append( "<redacted>" );
			}
			else
			{
				out.append( "<redacted:" )
				   .append( message, valueEnd - 4, valueEnd )
				   .append( '>' );
			}

			pos = valueEnd;
			at = indexOfIgnoreCase( message, "key=", pos );
		}

		out.append( message, pos, message.length() );
		return out.toString();
	}

	private static int indexOfIgnoreCase( String haystack, String needle )
	{
		return indexOfIgnoreCase( haystack, needle, 0 );
	}

	private static int indexOfIgnoreCase( String haystack, String needle, int from )
	{
		final int last = haystack.length() - needle.length();

		for( int i = Math.max( 0, from ); i <= last; i++ )
		{
			if( haystack.regionMatches( true, i, needle, 0, needle.length() ) )
				return i;
		}
		return -1;
	}

	/** Caller must hold LOCK. */
	private static File resolveLocked()
	{
		if( sResolveFailed )
			return null;

		if( sLogFile == null )
		{
			if( sBaseDir == null )
				return null;   // nothing to go on yet; the line is only in logcat
			sLogFile = new File( sBaseDir, "logs/slayer_java.log" );
		}

		File dir = sLogFile.getParentFile();
		if( dir != null && !dir.isDirectory() && !dir.mkdirs() )
		{
			Log.w( TAG, "cannot create " + dir );
			sResolveFailed = true;
			return null;
		}

		return sLogFile;
	}

	private static final SimpleDateFormat TIMESTAMP =
		new SimpleDateFormat( "HH:mm:ss.SSS", Locale.US );
}
