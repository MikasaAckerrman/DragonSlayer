/*
SteamPresence.java - holds the "playing Counter-Strike" status while the game runs.
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

The engine calls start() when a map becomes active and stop() when it
disconnects; everything in between -- logon, heartbeats, reconnects -- happens
on one background thread owned by this class.

Why a thread and not the engine's frame loop: a CM session blocks on network
I/O, and the one thing worse than no status is a status that stutters the
game. The engine only ever hands over an intent ("playing this, on that
server") and never waits for the network.

State is deliberately coarse: at most one session, and the newest intent wins.
A player who reconnects three times in ten seconds must not end up with three
CM sessions racing to describe the same account.
*/

package su.xash.engine.steam;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.IOException;

public final class SteamPresence
{
	private static final String TAG = "SteamPresence";

	// Same preference file the launcher's Settings screen writes.
	private static final String PREFS = "app_preferences";
	public static final String KEY_ACCOUNT = "steam_account_name";
	public static final String KEY_TOKEN = "steam_refresh_token";
	public static final String KEY_STEAMID = "steam_id";
	public static final String KEY_ENABLED = "steam_presence_enabled";

	private static final int RECONNECT_BASE_MS = 5000;
	private static final int RECONNECT_MAX_MS = 120000;

	private static SteamPresence instance;

	private final Context context;
	private final Object lock = new Object();

	private Thread worker;
	private boolean running;

	// The intent: what we want Steam to show. Guarded by lock.
	private boolean wantPlaying;
	private long wantAppId;
	private String wantExtraInfo;
	private int wantServerIp;
	private int wantServerPort;
	private boolean intentDirty;

	// ONE SESSION, ONE OWNER.
	//
	// Steam allows a single client session per account: a second logon evicts the
	// first with EResult 34 (LogonSessionReplaced). The device log showed 100
	// logons and 14 evictions in one game -- avatar lookups were opening a session
	// per player and closing it, so the sessions were evicting each other.
	//
	// That is not merely wasteful, it breaks server-side auth: registering an auth
	// ticket is bound to the session that registered it, so when the game server
	// asked Steam about our ticket, the session holding that registration was
	// already gone. Steam said "unknown", and Reunion fell back to a generated id
	// (announced as STEAM_10:0:... = DP_AUTH_REVEMU2013).
	//
	// So work goes TO the session instead of opening one: callers submit a task,
	// the worker thread runs it on its own socket between pumps. A second reader on
	// the same socket would be just as wrong -- it would consume replies the worker
	// is waiting for -- and this keeps the socket single-threaded by construction.
	private final java.util.ArrayList<Pending> queue = new java.util.ArrayList<Pending>();
	private long sessionWantedUntil;   // keep the session up while work is likely

	/** How long a session is held open after the last task. */
	private static final int SESSION_LINGER_MS = 90000;

	/** Work to run on the shared session, on the worker thread. */
	public interface SessionTask
	{
		void run( SteamCM cm ) throws IOException;
	}

	private static final class Pending
	{
		final SessionTask task;
		boolean done;
		boolean ok;
		String error;

		Pending( SessionTask t ) { task = t; }
	}

	private SteamPresence( Context context )
	{
		this.context = context.getApplicationContext();
	}

	public static synchronized SteamPresence get( Context context )
	{
		if( instance == null )
			instance = new SteamPresence( context );

		return instance;
	}

	/** Non-throwing accessor for the JNI bridge, which has no context to lose. */
	public static synchronized SteamPresence peek()
	{
		return instance;
	}

	// -----------------------------------------------------------------------
	// Credentials
	// -----------------------------------------------------------------------

	private SharedPreferences prefs()
	{
		return context.getSharedPreferences( PREFS, Context.MODE_PRIVATE );
	}

	/**
	 * True when a token is stored and the feature is not switched off.
	 * The OpenID sign-in does not count: it yields a SteamID and no token.
	 */
	public boolean isAvailable()
	{
		SharedPreferences p = prefs();
		String token = p.getString( KEY_TOKEN, null );
		return token != null && token.length() > 0 && p.getBoolean( KEY_ENABLED, true );
	}

	public static void saveCredentials( Context ctx, String accountName,
		String refreshToken, long steamid64 )
	{
		ctx.getSharedPreferences( PREFS, Context.MODE_PRIVATE ).edit()
			.putString( KEY_ACCOUNT, accountName )
			.putString( KEY_TOKEN, refreshToken )
			.putString( KEY_STEAMID, Long.toString( steamid64 ))
			.apply();
	}

	public static void clearCredentials( Context ctx )
	{
		ctx.getSharedPreferences( PREFS, Context.MODE_PRIVATE ).edit()
			.remove( KEY_ACCOUNT )
			.remove( KEY_TOKEN )
			.apply();
	}

	// -----------------------------------------------------------------------
	// Public API (called from the engine through XashActivity)
	// -----------------------------------------------------------------------

	/**
	 * Everything worth knowing about a presence session goes through here, to
	 * BOTH logcat and the on-device log file.
	 *
	 * The file half is what makes this feature diagnosable at all. The status has
	 * now failed twice on the user's phone with nothing to look at: logcat needs a
	 * cable, and every problem in this project that actually got solved was solved
	 * from cstrike/logs/. A background network session with no durable log is a
	 * session whose failures can only be guessed at.
	 */
	private static void note( String message )
	{
		Log.i( TAG, message );
		su.xash.engine.SlayerLog.log( "presence", message );
	}

	/**
	 * Announce a game. Returns immediately; the network happens elsewhere.
	 *
	 * @param appid      Steam app id, 10 for Counter-Strike
	 * @param extraInfo  optional title shown next to the game
	 * @param serverIp   dotted-quad server address, or null
	 * @param serverPort server port, or 0
	 */
	public void start( long appid, String extraInfo, String serverIp, int serverPort )
	{
		if( !isAvailable() )
		{
			// SAY WHICH of the two reasons it is. "No token stored" covered both
			// "never signed in" and "signed in, but the switch is off", and those
			// need opposite actions from the user.
			SharedPreferences p = prefs();
			String token = p.getString( KEY_TOKEN, null );

			if( token == null || token.length() == 0 )
				note( "start ignored: not signed in to Steam (no refresh token stored)" );
			else
				note( "start ignored: sign-in is fine, but 'Show what I'm playing' is OFF" );

			return;
		}

		synchronized( lock )
		{
			wantPlaying = true;
			wantAppId = appid;
			wantExtraInfo = extraInfo;
			wantServerIp = SteamCM.ipToInt( serverIp );
			wantServerPort = serverPort;
			intentDirty = true;

			ensureWorkerLocked();
			lock.notifyAll();
		}
	}

	/** Clear the status. The session is kept briefly in case play resumes. */
	public void stop()
	{
		synchronized( lock )
		{
			wantPlaying = false;
			intentDirty = true;
			lock.notifyAll();
		}
	}

	/** Tear everything down; called when the game process is going away. */
	public void shutdown()
	{
		Thread t;

		synchronized( lock )
		{
			wantPlaying = false;
			intentDirty = true;
			running = false;
			t = worker;
			lock.notifyAll();
		}

		if( t != null )
		{
			try
			{
				// Long enough to send the clear, short enough not to stall exit.
				t.join( 3000 );
			}
			catch( InterruptedException ignored )
			{
				Thread.currentThread().interrupt();
			}
		}
	}

	// -----------------------------------------------------------------------
	// Worker
	// -----------------------------------------------------------------------

	private void workerLoop()
	{
		SteamCM cm = null;
		int backoff = RECONNECT_BASE_MS;

		// What Steam currently believes, so a redundant message is not sent
		// every frame the engine feels like reporting its state.
		boolean sentPlaying = false;
		long sentAppId = 0;
		String sentExtra = null;
		int sentIp = 0;
		int sentPort = 0;

		try
		{
			while( true )
			{
				boolean playing;
				boolean wantSession;
				long appid;
				String extra;
				int ip, port;
				boolean dirty;

				synchronized( lock )
				{
					if( !running && !intentDirty && queue.isEmpty() )
						break;

					wantSession = sessionWantedLocked();
					playing = wantPlaying;
					appid = wantAppId;
					extra = wantExtraInfo;
					ip = wantServerIp;
					port = wantServerPort;
					dirty = intentDirty;
					intentDirty = false;
				}

				// Nothing to say and nothing to keep alive: idle out so a
				// finished match does not leave a thread and a socket behind.
				//
				// THE ORDER MATTERS. This branch is only correct when Steam has
				// already been told we stopped, and "cm == null" is not the same
				// thing: a session that dropped (Steam timeout, a network blip)
				// while the player was still in a game leaves Steam believing the
				// game is running. Idling out here would then leave that status on
				// the profile until something else happened to clear it -- part of
				// "показывает что я в игре, даже когда не играю".
				//
				// So a pending stop reconnects once, purely to say it.
				if( !playing && sentPlaying && ( cm == null || !cm.isConnected() ))
				{
					try
					{
						note( "reconnecting once to clear a status Steam still believes" );
						cm = connect();
						sentPlaying = false;
						cm.clearGamePlayed();
						note( "status cleared" );
						cm.close();
						cm = null;
					}
					catch( Exception e )
					{
						// Best effort: if Steam cannot be reached, its own session
						// timeout is the fallback. Do not spin on it.
						note( "could not clear the status: " + e.getMessage() );
						cm = null;
						sentPlaying = false;
					}

					continue;
				}

				// Idling out is only allowed when nothing wants the session. A
				// queued task is exactly such a want, and without this check the
				// worker would sleep 30s with a caller blocked on it.
				if( !playing && !wantSession && ( cm == null || !cm.isConnected() ))
				{
					synchronized( lock )
					{
						if( !running )
							break;

						if( !intentDirty && queue.isEmpty() )
						{
							try
							{
								lock.wait( 30000 );
							}
							catch( InterruptedException e )
							{
								Thread.currentThread().interrupt();
								break;
							}
						}
					}
					continue;
				}

				try
				{
					if(( playing || wantSession ) && ( cm == null || !cm.isConnected() ))
					{
						cm = connect();
						backoff = RECONNECT_BASE_MS;
						// A fresh session knows nothing about us yet.
						sentPlaying = false;
						dirty = true;
					}

					if( cm != null && cm.isConnected() )
					{
						boolean changed = dirty
							|| playing != sentPlaying
							|| appid != sentAppId
							|| ip != sentIp
							|| port != sentPort
							|| !equal( extra, sentExtra );

						if( changed )
						{
							if( playing )
							{
								cm.setGamePlayed( appid, extra, ip, port );
								note( "status set: appid " + appid
									+ ( extra != null ? " \"" + extra + "\"" : "" )
									+ ( ip != 0 ? " @ " + port : "" ));

								// "Join Game" for friends. The friends UI reads
								// the rich-presence key "connect" and launches the
								// game with that string, so this one line is the
								// whole feature -- but ONLY with a real server
								// address: an empty connect key offers a join that
								// goes nowhere.
								if( ip != 0 && port > 0 )
								{
									cm.uploadRichPresence( "+connect "
										+ SteamCM.ipToString( ip ) + ":" + port );
								}
								else
								{
									cm.clearRichPresence();
								}
							}
							else
							{
								cm.clearGamePlayed();
								note( "status cleared" );
							}

							sentPlaying = playing;
							sentAppId = appid;
							sentExtra = extra;
							sentIp = ip;
							sentPort = port;
						}

						// Tasks run here, on this thread, on this socket -- between
						// pumps, so nobody else ever reads from it.
						drainQueue( cm );

						cm.heartbeatIfDue();

						// Blocks up to a second reading whatever Steam sends,
						// which doubles as the loop's pacing.
						if( !cm.pump( 1000 ))
						{
							note( "CM session ended" );
							cm.close();
							cm = null;

							if( !playing )
								sentPlaying = false;
						}

						// Status is off and Steam knows: stop holding a session --
						// unless a task still wants it. Dropping it here is what made
						// every avatar lookup pay for a fresh logon.
						if( !playing && !sessionWantedLocked() && cm != null )
						{
							cm.close();
							cm = null;
							sentPlaying = false;
						}
					}
				}
				catch( SteamCM.LogonException e )
				{
					// Steam refused the credential itself. Retrying cannot fix
					// that, and hammering the auth endpoint is how accounts get
					// rate-limited, so give up until new credentials arrive.
					note( "logon refused, giving up: " + e.getMessage() );
					failQueue( "logon refused" );

					if( cm != null )
					{
						cm.close();
						cm = null;
					}

					synchronized( lock )
					{
						wantPlaying = false;
						intentDirty = false;
					}

					break;
				}
				catch( IOException e )
				{
					note( "network error: " + e.getMessage() );
					failQueue( "network error: " + e.getMessage() );

					if( cm != null )
					{
						cm.close();
						cm = null;
					}

					synchronized( lock )
					{
						if( !running )
							break;

						try
						{
							lock.wait( backoff );
						}
						catch( InterruptedException ie )
						{
							Thread.currentThread().interrupt();
							break;
						}
					}

					backoff = Math.min( backoff * 2, RECONNECT_MAX_MS );
				}
			}
		}
		finally
		{
			if( cm != null )
			{
				// Best effort: leaving a stale "playing" status behind is the
				// one failure a user would actually notice.
				try
				{
					if( sentPlaying )
						cm.clearGamePlayed();
				}
				catch( IOException ignored ) {}

				cm.close();
			}

			// Nobody is left to run them, and a caller blocked on lock.wait() would
			// otherwise sit there until its own timeout.
			failQueue( "worker stopped" );

			synchronized( lock )
			{
				worker = null;
			}

			note( "worker stopped" );
		}
	}

	/**
	 * Run a task on the shared CM session and wait for it to finish.
	 *
	 * Returns true when the task ran (whatever it concluded), false when no
	 * session could be had in time -- no credentials, Steam unreachable, or the
	 * worker busy longer than the caller is willing to wait.
	 *
	 * Callers must NOT keep the SteamCM reference past the task: it belongs to the
	 * worker, and using it from another thread reintroduces exactly the two-readers
	 * bug this replaced.
	 */
	public boolean submit( SessionTask task, int timeoutMs )
	{
		if( task == null || !isAvailable() )
			return false;

		Pending p = new Pending( task );
		long deadline = System.currentTimeMillis() + timeoutMs;

		synchronized( lock )
		{
			queue.add( p );
			sessionWantedUntil = deadline + SESSION_LINGER_MS;

			// A task is a reason to have a session even when nothing is playing.
			intentDirty = true;
			ensureWorkerLocked();
			lock.notifyAll();

			while( !p.done )
			{
				long left = deadline - System.currentTimeMillis();

				if( left <= 0 )
				{
					queue.remove( p );
					note( "session task timed out" );
					return false;
				}

				try
				{
					lock.wait( left );
				}
				catch( InterruptedException e )
				{
					Thread.currentThread().interrupt();
					queue.remove( p );
					return false;
				}
			}
		}

		if( !p.ok && p.error != null )
			note( "session task failed: " + p.error );

		return p.ok;
	}

	/** Must hold lock. Starts the worker if it is not already running. */
	private void ensureWorkerLocked()
	{
		if( worker != null )
			return;

		running = true;
		worker = new Thread( new Runnable()
		{
			public void run()
			{
				workerLoop();
			}
		}, "SteamPresence" );
		worker.setDaemon( true );
		worker.start();
	}

	/** Must hold lock. True while a session should be kept up for tasks. */
	private boolean sessionWantedLocked()
	{
		return !queue.isEmpty()
			|| System.currentTimeMillis() < sessionWantedUntil;
	}

	/**
	 * Run every queued task. Called on the worker thread with a live session.
	 *
	 * A task that throws is reported to its caller and does not disturb the
	 * others: one bad avatar lookup must not take down the status.
	 */
	private void drainQueue( SteamCM cm )
	{
		while( true )
		{
			Pending p;

			synchronized( lock )
			{
				if( queue.isEmpty() )
					return;

				p = queue.remove( 0 );
			}

			try
			{
				p.task.run( cm );
				p.ok = true;
			}
			catch( Throwable e )
			{
				p.ok = false;
				p.error = String.valueOf( e );
			}

			synchronized( lock )
			{
				p.done = true;
				lock.notifyAll();
			}
		}
	}

	/** Fail every queued task, e.g. when the session could not be established. */
	private void failQueue( String why )
	{
		while( true )
		{
			Pending p;

			synchronized( lock )
			{
				if( queue.isEmpty() )
					return;

				p = queue.remove( 0 );
				p.done = true;
				p.ok = false;
				p.error = why;
				lock.notifyAll();
			}
		}
	}

	/**
	 * Open a session that nothing else shares.
	 *
	 * DO NOT USE THIS FOR WORK ON OUR OWN ACCOUNT -- use submit(). Steam permits
	 * one session per account, so a second logon evicts the first (EResult 34), and
	 * whatever the evicted session had registered -- notably an auth ticket -- dies
	 * with it. That is measured, not theoretical: the device logged 100 logons and
	 * 14 evictions in one match, and the game server consequently treated our
	 * registered ticket as unknown.
	 *
	 * Kept because the test suite drives it directly to prove that bad credentials
	 * yield null rather than an exception.
	 *
	 * Returns null when no credentials are stored, which is the ordinary case for
	 * an account signed in the old OpenID way. The caller must close the session.
	 */
	public SteamCM openSession()
	{
		try
		{
			return connect();
		}
		catch( Throwable e )
		{
			note( "openSession failed: " + e );
			return null;
		}
	}

	private SteamCM connect() throws IOException
	{
		SharedPreferences p = prefs();
		String account = p.getString( KEY_ACCOUNT, null );
		String token = p.getString( KEY_TOKEN, null );
		long steamid = 0;

		try
		{
			steamid = Long.parseLong( p.getString( KEY_STEAMID, "0" ));
		}
		catch( NumberFormatException ignored ) {}

		if( token == null || token.length() == 0 )
			throw new IOException( "no Steam token stored" );

		if( account == null || account.length() == 0 )
			throw new IOException( "no Steam account name stored" );

		if( steamid == 0 )
			steamid = SteamAuth.steamidFromToken( token );

		SteamCM cm = new SteamCM( deviceName(), new SteamCM.Logger()
		{
			public void log( String message )
			{
				note( message );
			}
		});

		cm.logon( account, token, steamid );

		// ONLINE FIRST, then the game. A session whose persona state is still
		// Offline shows nothing to anyone -- Steam accepts the games_played
		// message and no friend ever sees it. That was the "в стиме был по-прежнему
		// 3 часа 46 минут назад": logged on, invisible.
		cm.announceOnline();

		return cm;
	}

	private static String deviceName()
	{
		String model = android.os.Build.MODEL;
		return ( model != null && model.length() > 0 ? model : "Android" ) + " (Slayer3D)";
	}

	private static boolean equal( String a, String b )
	{
		return a == null ? b == null : a.equals( b );
	}
}
