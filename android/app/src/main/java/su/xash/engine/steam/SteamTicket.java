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
	 * A SEPARATE SESSION from the presence one, on purpose. Sharing would be
	 * cheaper, but the presence session is owned by a worker thread that sends and
	 * reads on its own schedule; a second reader would steal its replies and the
	 * status would start missing updates. Connecting to a server is rare enough
	 * that a short-lived session is the cheaper mistake.
	 */
	public static Result fetch( Context ctx, int timeoutMs )
	{
		SteamCM cm = null;

		try
		{
			cm = SteamPresence.get( ctx ).openSession();

			if( cm == null )
				return null;

			// A FULL SESSION TICKET, not the ownership ticket alone.
			//
			// The first version sent just the ownership ticket, and the device
			// measurement was unambiguous: the server announced the same made-up
			// SteamID as before, so it had made no sense of what we sent. A GoldSrc
			// server wants the auth SESSION ticket -- ownership ticket plus a
			// game-connect token plus a session block -- and it wants one Steam has
			// been told about, because the server validates by asking Steam.
			byte[] ticket = cm.buildAuthSessionTicket( APPID_CS16, timeoutMs );

			if( ticket == null )
				return null;

			su.xash.engine.SlayerLog.log( "ticket",
				"session ticket ready, " + ticket.length + " bytes, SteamID " + cm.getSteamId() );

			return new Result( ticket, cm.getSteamId() );
		}
		catch( Throwable e )
		{
			// Includes IOException, and also anything unexpected: a failure to get
			// a nicer ticket must never stop the player joining a server.
			su.xash.engine.SlayerLog.log( "ticket", "FAIL " + e );
			return null;
		}
		finally
		{
			if( cm != null )
			{
				try
				{
					cm.close();
				}
				catch( Throwable ignored ) {}
			}
		}
	}
}
