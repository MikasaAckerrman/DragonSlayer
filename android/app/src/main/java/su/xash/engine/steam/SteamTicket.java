/*
SteamTicket.java - real Steam auth ticket for a GoldSrc server connect.
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

WHAT THIS IS FOR

"мою аватарку никто не увидит?" -- and the answer was no, because the server
tells every other client a fabricated SteamID. Measured on the reporting
device's own server: 52 of 61 status rows read STEAM_5:0:..., a universe that
does not exist, and OUR row read STEAM_0:4:... where Y can only ever be 0 or 1.
Those ids belong to no Steam account, so no avatar and no profile.

But the same status table also had genuine rows -- STEAM_0:0 and STEAM_0:1 --
whose profiles are real and load fine. So the server is not rewriting anything:
it reports whatever the connecting client's ticket said. A fabricated ticket
yields a fabricated id; a real one yields the real account.

This class produces the real one, over the CM session that already sets the
"playing Counter-Strike" status. No Steam client and no PC, which is the whole
point -- the engine's existing "steam" ticket generator needs a desktop Steam
running on the same network (see SteamBroker in cl_steam.c).
*/

package su.xash.engine.steam;

import android.content.Context;

import java.io.IOException;

public final class SteamTicket
{
	/** Counter-Strike 1.6. */
	public static final int APPID_CS16 = 10;

	private SteamTicket() {}

	/**
	 * A ticket, plus the SteamID it belongs to.
	 *
	 * The id travels WITH the ticket rather than being read from preferences by
	 * the caller: the ticket is only meaningful for the account that was logged
	 * on when Steam issued it, and a mismatch between the two would advertise one
	 * account while proving another.
	 */
	public static final class Result
	{
		public final byte[] ticket;
		public final long steamid;

		Result( byte[] ticket, long steamid )
		{
			this.ticket = ticket;
			this.steamid = steamid;
		}
	}

	/**
	 * Fetch an ownership ticket for CS 1.6 on the caller's thread.
	 *
	 * BLOCKING, and that is deliberate: the engine asks for this at the moment it
	 * builds a connect packet, and a connect that proceeded without waiting would
	 * have nothing to put in the packet. The caller is the engine's connect path,
	 * which is already allowed to take a moment (the existing Steam broker path
	 * waits on a PC over TCP for up to ten seconds).
	 *
	 * Returns null rather than throwing for every expected refusal -- no stored
	 * credentials, an account that does not own the game, Steam saying no. The
	 * caller falls back to the emulated ticket, which is what every build so far
	 * has used, so a null here means "nothing changes", not "cannot connect".
	 *
	 * THE SHARED SESSION, and this is not an optimisation.
	 *
	 * Registering an auth ticket is bound to the session that registered it. The
	 * old code opened a session of its own, took the ticket and closed it -- so by
	 * the time the game server asked Steam about that ticket, the registration had
	 * gone with the session. Steam answered "unknown", and Reunion fell back to a
	 * generated id: the device announced us as STEAM_10:0:... , which is
	 * DP_AUTH_REVEMU2013, not native Steam auth.
	 *
	 * Measured on the device: 100 logons and 14 EResult 34 (LogonSessionReplaced)
	 * in one match. Steam permits a single session per account, so ours were
	 * evicting each other -- avatar lookups opened one per player.
	 *
	 * The registration must OUTLIVE this call, so the work goes to the presence
	 * worker instead of opening a second door onto the same account.
	 */
	public static Result fetch( Context ctx, int timeoutMs )
	{
		final byte[][] out = new byte[1][];
		final long[] id = new long[1];

		try
		{
			boolean ran = SteamPresence.get( ctx ).submit( new SteamPresence.SessionTask()
			{
				public void run( SteamCM cm ) throws java.io.IOException
				{
					// A FULL SESSION TICKET, not the ownership ticket alone. The
					// first version sent only the ownership one and the server made
					// nothing of it: a GoldSrc server wants the auth SESSION ticket
					// -- ownership ticket plus a game-connect token plus a session
					// block -- and it wants one Steam has been told about.
					byte[] t = cm.buildAuthSessionTicket( APPID_CS16, 8000 );

					if( t == null )
						return;

					out[0] = t;
					id[0] = cm.getSteamId();
				}
			}, timeoutMs );

			if( !ran || out[0] == null )
				return null;

			su.xash.engine.SlayerLog.log( "ticket",
				"session ticket ready, " + out[0].length + " bytes, SteamID " + id[0]
				+ " (registration kept alive on the shared session)" );

			return new Result( out[0], id[0] );
		}
		catch( Throwable e )
		{
			// Includes IOException, and anything unexpected: failing to get a nicer
			// ticket must never stop the player joining a server.
			su.xash.engine.SlayerLog.log( "ticket", "FAIL " + e );
			return null;
		}
	}
}
