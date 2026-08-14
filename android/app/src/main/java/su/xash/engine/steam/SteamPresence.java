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

			if( worker == null )
			{
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
				long appid;
				String extra;
				int ip, port;
				boolean dirty;

				synchronized( lock )
				{
					if( !running && !intentDirty )
						break;

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
				if( !playing && ( cm == null || !cm.isConnected() ))
				{
					synchronized( lock )
					{
						if( !running )
							break;

						if( !intentDirty )
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
					if( playing && ( cm == null || !cm.isConnected() ))
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

						// Status is off and Steam knows: stop holding a session.
						if( !playing && cm != null )
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

			synchronized( lock )
			{
				worker = null;
			}

			note( "worker stopped" );
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
