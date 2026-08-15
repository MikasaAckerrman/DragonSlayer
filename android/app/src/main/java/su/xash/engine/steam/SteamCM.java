/*
SteamCM.java - a session with a Steam connection manager.
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

This is the piece that makes the profile say "playing Counter-Strike". The
green status is not a feature of the desktop client -- it is one protobuf
message, ClientGamesPlayed, on an ordinary authenticated CM session. Anything
that can log on may send it.

Message framing on the wire:

    [uint32 emsg | 0x80000000] [uint32 header length] [header] [body]

The high bit of emsg marks a protobuf message (all of ours are). The header is
a CMsgProtoBufHeader carrying steamid and client_sessionid; both must be echoed
on every message after logon or the CM ignores us.

Multi (emsg 1) wraps several messages in one frame, optionally gzipped, and
Steam uses it constantly -- the logon response itself usually arrives inside
one. Unpacking it is not optional.
*/

package su.xash.engine.steam;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.GZIPInputStream;

public final class SteamCM
{
	// EMsg values used here
	private static final int EMSG_MULTI = 1;
	private static final int EMSG_CLIENT_LOGON = 5514;
	private static final int EMSG_CLIENT_LOGON_RESPONSE = 751;
	private static final int EMSG_CLIENT_LOGGED_OFF = 757;
	private static final int EMSG_CLIENT_HEARTBEAT = 703;
	private static final int EMSG_CLIENT_GAMES_PLAYED = 742;
	// ClientChangeStatus carries the persona state. Sending it is what makes the
	// session VISIBLE: see announceOnline().
	private static final int EMSG_CLIENT_CHANGE_STATUS = 716;
	// ClientLogOff: leave the session DELIBERATELY. Without it Steam only notices
	// when the socket dies, and until then the profile keeps showing the game --
	// see logOff().
	private static final int EMSG_CLIENT_LOG_OFF = 706;
	// Rich presence. The "connect" key is what puts a working "Join Game" on the
	// friends list; see uploadRichPresence().
	private static final int EMSG_CLIENT_RICH_PRESENCE_UPLOAD = 7501;

	// ClientGetAppOwnershipTicket / its response. This is the first half of
	// "let servers see my real SteamID": Steam issues a signed blob proving the
	// logged-in account owns an app, and it does so over THIS session -- no Steam
	// client and no PC involved, which is the whole reason this is worth trying.
	private static final int EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET = 857;
	private static final int EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET_RESPONSE = 858;

	// ClientGameConnectTokens: Steam PUSHES these unsolicited, shortly after a
	// real logon. A GoldSrc server wants one of them inside the connect ticket,
	// so they are the second half. Nothing requests them; they just arrive.
	private static final int EMSG_CLIENT_GAME_CONNECT_TOKENS = 779;

	// ClientAuthList / its ack. REGISTERING the ticket with Steam, which is the
	// step whose absence made the first attempt useless: a server asked to
	// validate a ticket asks Steam, and Steam only knows about tickets it was
	// told about. The ack echoes the CRC32 of every ticket Steam accepted.
	private static final int EMSG_CLIENT_AUTH_LIST = 5432;
	private static final int EMSG_CLIENT_AUTH_LIST_ACK = 5575;

	// ClientTicketAuthComplete. THE ONLY PLACE STEAM TELLS US WHAT A GAME SERVER
	// GOT WHEN IT CHECKED OUR TICKET.
	//
	// Steam sends this unsolicited after a server validates (or fails to validate)
	// a ticket we registered. Its eauth_session_response is the answer we have been
	// guessing at for days: whether the server asked at all, and if it did, exactly
	// why Steam said no. Until now the read loop dropped it silently.
	private static final int EMSG_CLIENT_TICKET_AUTH_COMPLETE = 5429;

	// ClientRequestFriendData / ClientPersonaState. THE AVATAR PATH THAT DOES NOT
	// GO THROUGH A WEB PAGE.
	//
	// Avatars were being scraped off steamcommunity.com profile pages, and Steam
	// answered 429 to every one of them (measured: 24 of 24, including ids whose
	// pages open fine in a browser). But the CM session already open here can be
	// asked directly, and CMsgClientPersonaState carries avatar_hash -- 20 raw
	// bytes that name the image on the CDN. No web page, no API key, no rate limit.
	private static final int EMSG_CLIENT_REQUEST_FRIEND_DATA = 815;
	private static final int EMSG_CLIENT_PERSONA_STATE = 766;

	// EClientPersonaStateFlag: Status | PlayerName. Nothing else is needed for an
	// avatar, and asking for less means Steam sends less.
	private static final int PERSONA_FLAGS_BASIC = 1 | 2;

	// EPersonaState
	private static final int PERSONA_ONLINE = 1;

	private static final int PROTO_MASK = 0x80000000;

	// What a current Steam client reports. Wrong values here get logons
	// rejected, so they are not free to invent.
	private static final int PROTOCOL_VERSION = 65580;
	private static final int CLIENT_PACKAGE_VERSION = 1561159470;
	private static final int OS_TYPE_LINUX = -203;       // uint32 field, masked

	private static final int CONNECT_TIMEOUT_MS = 15000;
	private static final int READ_TIMEOUT_MS = 45000;

	public static final int ERESULT_OK = 1;

	private final String deviceName;
	private SteamWebSocket ws;
	private long steamid;
	private int sessionId;
	private int heartbeatSeconds = 9;
	private long lastHeartbeat;

	// Job sequence for request/response messages. Steam matches a reply to its
	// request by jobid_source in the header, and a request sent with jobid 0 is
	// simply dropped -- MEASURED: the CM answered nothing until this was added.
	private int jobSeq;

	// Game-connect tokens, as pushed by Steam. Kept newest-last; one is consumed
	// per server connect and never reused, which is why they arrive in batches.
	private final List<byte[]> connectTokens = new ArrayList<byte[]>();

	// Sequence for the session part of an auth ticket. Steam wants it to move;
	// a repeated value looks like a replayed ticket.
	private int ticketSequence;

	// CRCs Steam confirmed in the last ClientAuthListAck. A ticket whose CRC is
	// not in here was NOT registered, and a server validating it will be told no.
	private final List<Long> acceptedTicketCrcs = new ArrayList<Long>();

	public interface Logger
	{
		void log( String message );
	}

	private final Logger logger;

	public SteamCM( String deviceName, Logger logger )
	{
		this.deviceName = deviceName;
		this.logger = logger;
	}

	private void log( String msg )
	{
		if( logger != null )
			logger.log( msg );
	}

	public static final class LogonException extends IOException
	{
		public final int eresult;

		public LogonException( String message, int eresult )
		{
			super( message + " (eresult " + eresult + ")" );
			this.eresult = eresult;
		}
	}

	// -----------------------------------------------------------------------
	// Server list
	// -----------------------------------------------------------------------

	/**
	 * Ask Steam which CMs to use. The list is load-sorted, so the first entry
	 * is the recommended one.
	 */
	public static List<String> fetchServerList() throws IOException
	{
		URL url = new URL( "https://api.steampowered.com/ISteamDirectory/"
			+ "GetCMListForConnect/v1/?cellid=0&cmtype=websockets&realm=steamglobal" );
		HttpURLConnection conn = (HttpURLConnection)url.openConnection();
		String json;

		try
		{
			conn.setConnectTimeout( CONNECT_TIMEOUT_MS );
			conn.setReadTimeout( CONNECT_TIMEOUT_MS );
			conn.setRequestProperty( "User-Agent", "Valve/Steam HTTP Client 1.0" );

			if( conn.getResponseCode() != HttpURLConnection.HTTP_OK )
				throw new IOException( "GetCMListForConnect: HTTP " + conn.getResponseCode() );

			ByteArrayOutputStream bos = new ByteArrayOutputStream( 4096 );
			InputStream in = conn.getInputStream();
			byte[] buf = new byte[4096];
			int n;

			try
			{
				while(( n = in.read( buf )) > 0 )
					bos.write( buf, 0, n );
			}
			finally
			{
				in.close();
			}

			json = new String( bos.toByteArray(), "UTF-8" );
		}
		finally
		{
			conn.disconnect();
		}

		// Small fixed-shape response; a JSON parser would be more code than the
		// scan for "endpoint":"host:port".
		List<String> out = new ArrayList<String>();
		String needle = "\"endpoint\":\"";
		int i = 0;

		while(( i = json.indexOf( needle, i )) >= 0 )
		{
			int start = i + needle.length();
			int end = json.indexOf( '"', start );

			if( end < 0 )
				break;

			out.add( json.substring( start, end ));
			i = end;
		}

		if( out.isEmpty() )
			throw new IOException( "GetCMListForConnect: no endpoints in response" );

		return out;
	}

	// -----------------------------------------------------------------------
	// Logon
	// -----------------------------------------------------------------------

	/**
	 * Connect to a CM and log on with a refresh token from SteamAuth.
	 * Tries the recommended servers in turn.
	 */
	public void logon( String accountName, String refreshToken, long steamid64 )
		throws IOException
	{
		List<String> servers = fetchServerList();
		IOException last = null;
		int tried = 0;

		for( String endpoint : servers )
		{
			if( tried++ >= 3 )
				break;

			try
			{
				log( "cm: connecting to " + endpoint );
				ws = SteamWebSocket.connect( endpoint, "/cmsocket/",
					CONNECT_TIMEOUT_MS, READ_TIMEOUT_MS );
				doLogon( accountName, refreshToken, steamid64 );
				log( "cm: logged on, session " + sessionId );
				return;
			}
			catch( LogonException e )
			{
				// Steam answered and said no: a different server will say the
				// same thing, so stop here and report why.
				closeQuietly();
				throw e;
			}
			catch( IOException e )
			{
				log( "cm: " + endpoint + " failed: " + e.getMessage() );
				closeQuietly();
				last = e;
			}
		}

		throw last != null ? last : new IOException( "no CM server accepted a connection" );
	}

	private void doLogon( String accountName, String refreshToken, long steamid64 )
		throws IOException
	{
		steamid = steamid64;
		sessionId = 0;

		SteamWire.Writer body = new SteamWire.Writer()
			.uint32( 1, PROTOCOL_VERSION )
			.uint32( 5, CLIENT_PACKAGE_VERSION )
			.string( 6, "english" )
			.uint32( 7, OS_TYPE_LINUX )
			.bool( 8, true )                               // should_remember_password
			.message( 11, new SteamWire.Writer()
				.fixed32( 1, 0xBAADF00D ))                 // obfuscated_private_ip
			.string( 50, accountName )
			.bool( 102, true )                             // supports_rate_limit_response
			.string( 108, refreshToken );                  // access_token

		send( EMSG_CLIENT_LOGON, body.toBytes() );

		awaitLogonResponse();
	}

	/**
	 * Log on as the anonymous user: no account, no token.
	 *
	 * Steam grants these freely, which makes this the one way to exercise the
	 * whole stack -- server list, TLS, WebSocket framing, message header,
	 * Multi unpacking, logon response -- against the real service without
	 * anybody's credentials. Used by the connection self-test
	 * ("slayer_steam_selftest") and by tests/compile_steam_cm.sh.
	 *
	 * An anonymous session cannot set a game status; only a real logon can.
	 */
	public void anonymousLogon() throws IOException
	{
		// SteamID of the anonymous user: type AnonUser (2), universe Public (1)
		final long ANON_STEAMID = 0x01A0000000000000L;

		List<String> servers = fetchServerList();
		IOException last = null;
		int tried = 0;

		for( String endpoint : servers )
		{
			if( tried++ >= 3 )
				break;

			try
			{
				log( "cm: connecting to " + endpoint + " (anonymous)" );
				ws = SteamWebSocket.connect( endpoint, "/cmsocket/",
					CONNECT_TIMEOUT_MS, READ_TIMEOUT_MS );

				steamid = ANON_STEAMID;
				sessionId = 0;

				send( EMSG_CLIENT_LOGON, new SteamWire.Writer()
					.uint32( 1, PROTOCOL_VERSION )
					.uint32( 5, CLIENT_PACKAGE_VERSION )
					.toBytes() );

				awaitLogonResponse();
				log( "cm: anonymous logon OK, session " + sessionId );
				return;
			}
			catch( LogonException e )
			{
				closeQuietly();
				throw e;
			}
			catch( IOException e )
			{
				log( "cm: " + endpoint + " failed: " + e.getMessage() );
				closeQuietly();
				last = e;
			}
		}

		throw last != null ? last : new IOException( "no CM server accepted a connection" );
	}

	private void awaitLogonResponse() throws IOException
	{
		long deadline = System.currentTimeMillis() + READ_TIMEOUT_MS;

		while( System.currentTimeMillis() < deadline )
		{
			Message m = nextMessage();

			if( m == null )
			{
				// A rejected logon is answered by dropping the TCP connection
				// rather than by an error message, so "no reply" is itself the
				// diagnosis. Saying so beats a bare "timeout".
				throw new LogonException( "connection closed during logon"
					+ " (token expired or rejected)", 0 );
			}

			if( m.emsg == EMSG_CLIENT_LOGON_RESPONSE )
			{
				handleLogonResponse( m );
				return;
			}

			if( m.emsg == EMSG_CLIENT_LOGGED_OFF )
				throw new LogonException( "logged off during logon", readEresult( m.body ));

			log( "cm: ignoring emsg " + m.emsg + " during logon" );
		}

		throw new IOException( "timed out waiting for logon response" );
	}

	private void handleLogonResponse( Message m ) throws IOException
	{
		int eresult = 0;
		int heartbeat = 0;
		long suppliedSteamid = 0;
		SteamWire.Reader r = new SteamWire.Reader( m.body );

		while( r.next() )
		{
			switch( r.field() )
			{
			case 1: eresult = r.int32(); break;
			case 3: heartbeat = r.int32(); break;
			case 20: suppliedSteamid = r.fixed64(); break;
			default: r.skip(); break;
			}
		}

		if( eresult != ERESULT_OK )
			throw new LogonException( "logon refused", eresult );

		if( heartbeat > 0 )
			heartbeatSeconds = heartbeat;

		if( suppliedSteamid != 0 )
			steamid = suppliedSteamid;

		if( m.sessionId != 0 )
			sessionId = m.sessionId;

		if( m.steamid != 0 )
			steamid = m.steamid;

		lastHeartbeat = System.currentTimeMillis();
	}

	private static int readEresult( byte[] body )
	{
		SteamWire.Reader r = new SteamWire.Reader( body );

		while( r.next() )
		{
			if( r.field() == 1 )
				return r.int32();

			r.skip();
		}

		return 0;
	}

	// -----------------------------------------------------------------------
	// The actual point of all this
	// -----------------------------------------------------------------------

	/**
	 * Announce the session as ONLINE.
	 *
	 * WHY THIS IS SEPARATE FROM setGamePlayed, and why it is the missing piece:
	 * the user reported "аватарка отображается, но в стиме был по-прежнему 3 часа
	 * 46 минут назад" -- i.e. the friends list still showed him offline, so
	 * "playing Counter-Strike" had nowhere to appear either. A logged-on session
	 * whose persona state is still Offline is exactly that: Steam accepts the
	 * games_played message and shows it to nobody.
	 *
	 * A real client sends ClientChangeStatus after logon; we never did, because
	 * the games_played message alone was enough in testing against an account
	 * that was already online from another device.
	 *
	 * Sent once per session, right after logon, and NOT tied to the game status:
	 * clearing the game must not drop the session to Offline while the player is
	 * still in the launcher.
	 */
	public void announceOnline() throws IOException
	{
		// CMsgClientChangeStatus: persona_state is field 1. Only that field is
		// sent -- the first version also wrote a bool at field 3, which in that
		// message is is_auto_generated_name, not the persona flags (those are a
		// uint32 at field 6). Sending a field means something different from
		// leaving it out, so the minimum that expresses "I am online" is the
		// correct thing to send.
		send( EMSG_CLIENT_CHANGE_STATUS, new SteamWire.Writer()
			.uint32( 1, PERSONA_ONLINE )      // persona_state
			.toBytes() );

		log( "cm: persona state -> Online" );
	}

	/**
	 * Announce a game as being played.
	 *
	 * @param appid     Steam app id; 10 is Counter-Strike
	 * @param extraInfo optional title shown in the status
	 * @param serverIp  optional server address, big-endian int (0 = none)
	 * @param serverPort server port
	 * @param serverSteamId the server's own SteamID from its getchallenge reply,
	 *        or 0 when it did not report a usable one
	 * @param serverSecure whether the server announced itself VAC-secure
	 * @param token the game-connect token that will be embedded in the ticket we
	 *        are about to hand this server, or null when announcing without one
	 */
	public void setGamePlayed( long appid, String extraInfo, int serverIp, int serverPort,
		long serverSteamId, boolean serverSecure, byte[] token )
		throws IOException
	{
		SteamWire.Writer game = new SteamWire.Writer().fixed64( 2, appid );

		// WHY steam_id_gs AND token MATTER, and why leaving them out is not a
		// harmless omission:
		//
		// A game server validates a ticket by asking Steam "is this ticket good
		// for ME?". Steam answers by looking up what the CLIENT said it was doing.
		// The pairing key is not the IP -- both ends may sit behind NAT, and the
		// address the server sees need not be the one we see. It is the server's
		// own SteamID (field 1) plus the connect token (field 6), which is the
		// same token embedded in the ticket bytes.
		//
		// We were sending only the app id, the port and an IP. From Steam's point
		// of view that is "playing Counter-Strike somewhere" -- enough for the
		// friends list, not enough to answer a specific server's question about a
		// specific token. So the ticket was registered and disowned in the same
		// breath: valid, ours, and attached to no server.
		//
		// Both fields are optional in the protocol precisely because the message
		// serves double duty (status display vs. connection announcement). Sending
		// them only when known keeps the status path working unchanged.
		if( serverSteamId != 0 )
			game.uint64( 1, serverSteamId );

		if( serverPort > 0 )
			game.uint32( 4, serverPort );

		if( serverSecure )
			game.bool( 5, true );

		if( token != null && token.length > 0 )
			game.bytes( 6, token );

		if( extraInfo != null && extraInfo.length() > 0 )
			game.string( 7, extraInfo );

		if( serverIp != 0 )
			game.message( 23, new SteamWire.Writer().fixed32( 1, serverIp ));

		send( EMSG_CLIENT_GAMES_PLAYED,
			new SteamWire.Writer().message( 1, game ).toBytes() );

		log( "cm: status set: appid " + appid
			+ ( serverPort > 0 ? " @ " + serverPort : "" )
			+ ", server steamid " + serverSteamId
			+ ", secure " + serverSecure
			+ ", token " + ( token == null ? "none" : token.length + " bytes" ));
	}

	/**
	 * Status-only announcement, for the launcher and for clearing.
	 *
	 * Kept as a separate entry point rather than a default argument so that the
	 * connect path cannot silently degrade to it: a missing pairing is a real
	 * difference in meaning, and the call sites that have the data are the ones
	 * that must pass it.
	 */
	public void setGamePlayed( long appid, String extraInfo, int serverIp, int serverPort )
		throws IOException
	{
		setGamePlayed( appid, extraInfo, serverIp, serverPort, 0L, false, null );
	}

	/** Clear the status: an empty games_played list means "not playing". */
	public void clearGamePlayed() throws IOException
	{
		send( EMSG_CLIENT_GAMES_PLAYED, new byte[0] );

		// And clear the rich presence with it. The "connect" key is what puts
		// "Join Game" on a friend's list; leaving it behind offers to join a
		// server the player has left.
		clearRichPresence();
	}

	// -----------------------------------------------------------------------
	// Rich presence -- "Join Game" on the friends list
	// -----------------------------------------------------------------------

	/**
	 * Publish rich presence keys for this session.
	 *
	 * WHAT THIS BUYS, and it is the "чтобы друзья могли подключиться" request:
	 * the friends UI reads the key "connect". When it is present, the friend's
	 * client shows "Join Game" and, on clicking it, launches the game with that
	 * string as the command line. So "+connect ip:port" is literally all that is
	 * needed for a friend to land on the same server.
	 *
	 * "steam_display" is the other half of what a friend SEES. Steam looks the
	 * token up in the app's localisation file; an app that has none (Counter-Strike
	 * 1.6 has no rich-presence loc) falls back to showing the game name, which is
	 * what we already get from games_played. So the status text stays where it
	 * works -- game_extra_info -- and only "connect" is uploaded here.
	 *
	 * The payload is binary KeyValues, not protobuf: rich_presence_kv is a bytes
	 * field carrying a KV blob. Shape (SteamKit KeyValue.RecursiveSaveBinaryToStream):
	 *   0x00 <name NUL>            begin an object
	 *   0x01 <key NUL> <value NUL> a string pair
	 *   0x08                       end the object
	 */
	public void uploadRichPresence( String connect ) throws IOException
	{
		send( EMSG_CLIENT_RICH_PRESENCE_UPLOAD, new SteamWire.Writer()
			.bytes( 1, richPresenceKv( connect ))
			.toBytes() );

		log( "cm: rich presence connect=" + ( connect != null ? connect : "(none)" ));
	}

	/** Remove the rich presence, so no stale "Join Game" is offered. */
	public void clearRichPresence() throws IOException
	{
		send( EMSG_CLIENT_RICH_PRESENCE_UPLOAD, new SteamWire.Writer()
			.bytes( 1, richPresenceKv( null ))
			.toBytes() );

		log( "cm: rich presence cleared" );
	}

	/**
	 * The binary KeyValues blob for rich_presence_kv.
	 *
	 * PUBLIC so it can be asserted BYTE FOR BYTE from a test, and that is worth
	 * the exposure: this is the only hand-rolled binary format in the stack that
	 * Steam never answers, so a malformed blob produces no error anywhere -- the
	 * friend simply never sees "Join Game", which is indistinguishable from the
	 * feature not being implemented at all. Nor can it be checked over the wire:
	 * an anonymous session (the only kind a test can open) is dropped by Steam at
	 * about three minutes whatever it sends, so a live assertion measures the
	 * session's lifetime rather than the message.
	 *
	 * A null or empty `connect` yields an object with no children, which is how
	 * the keys are removed.
	 */
	public static byte[] richPresenceKv( String connect ) throws IOException
	{
		ByteArrayOutputStream kv = new ByteArrayOutputStream( 128 );

		kv.write( 0x00 );                       // Type.None: begin an object
		writeNulString( kv, "RP" );

		if( connect != null && connect.length() > 0 )
		{
			kv.write( 0x01 );                   // Type.String: a key/value pair
			writeNulString( kv, "connect" );
			writeNulString( kv, connect );
		}

		kv.write( 0x08 );                       // Type.End
		return kv.toByteArray();
	}

	private static void writeNulString( ByteArrayOutputStream out, String s )
		throws IOException
	{
		out.write( s.getBytes( "UTF-8" ));
		out.write( 0 );
	}

	// -----------------------------------------------------------------------
	// Leaving
	// -----------------------------------------------------------------------

	/**
	 * Log off deliberately.
	 *
	 * WHY THIS MATTERS: closing the socket is not the same as leaving. Steam keeps
	 * a session it has not been told about until its own timeout expires, and until
	 * then the profile still shows the game -- which is exactly the report "сейчас
	 * показывает что я в игре даже когда не играю". Sending ClientLogOff ends it at
	 * once.
	 *
	 * Best-effort by construction: this runs while the app is going away, so a
	 * failure here must never propagate.
	 */
	public void logOff()
	{
		try
		{
			if( ws == null || ws.isClosed() )
				return;

			send( EMSG_CLIENT_LOG_OFF, new byte[0] );
			log( "cm: logged off" );
		}
		catch( IOException e )
		{
			log( "cm: log off failed: " + e.getMessage() );
		}
	}

	// -----------------------------------------------------------------------
	// Session upkeep
	// -----------------------------------------------------------------------

	/**
	 * Send a heartbeat if one is due. Steam drops silent sessions, which would
	 * take the status down with them.
	 */
	public void heartbeatIfDue() throws IOException
	{
		long now = System.currentTimeMillis();

		if( now - lastHeartbeat >= heartbeatSeconds * 1000L )
		{
			send( EMSG_CLIENT_HEARTBEAT, new byte[0] );
			lastHeartbeat = now;
		}
	}

	/**
	 * Read and handle one pending message, if any arrives within timeoutMs.
	 *
	 * THE TIMEOUT IS NOW HONOURED, and it was not before -- this is the reason the
	 * profile kept showing the game for about a minute after leaving. The socket
	 * is created with setSoTimeout(READ_TIMEOUT_MS) = 45 s, and this method
	 * ignored its own argument, so the presence worker's "pump(1000)" blocked for
	 * up to 45 seconds on a quiet session. A stop arriving in that window could
	 * not be acted on until the read returned: the engine reported the change
	 * instantly, the worker was asleep in a socket read.
	 *
	 * @return false if the connection is gone
	 */
	public boolean pump( int timeoutMs ) throws IOException
	{
		if( ws == null || ws.isClosed() )
			return false;

		Message m;

		// Per-call read timeout, restored afterwards: logon and framing use the
		// long one deliberately (a CM can take seconds to answer a logon), and
		// only this polling read wants to come back promptly.
		ws.setReadTimeout( timeoutMs > 0 ? timeoutMs : 1 );

		try
		{
			m = nextMessage();
		}
		catch( InterruptedIOException timeout )
		{
			return true;    // nothing to read, connection still fine
		}
		finally
		{
			ws.setReadTimeout( READ_TIMEOUT_MS );
		}

		if( m == null )
			return false;

		if( m.emsg == EMSG_CLIENT_LOGGED_OFF )
		{
			log( "cm: logged off by Steam, eresult " + readEresult( m.body ));
			return false;
		}

		// Connect tokens arrive UNSOLICITED, so the only place that can catch them
		// is the loop that reads everything. Missing them means falling back to a
		// fabricated ticket, so this must not be gated on anything.
		if( m.emsg == EMSG_CLIENT_GAME_CONNECT_TOKENS )
			storeConnectTokens( m.body );

		// Same reasoning, and this one is the diagnosis: it arrives whenever a game
		// server has asked Steam about a ticket of ours.
		if( m.emsg == EMSG_CLIENT_TICKET_AUTH_COMPLETE )
			logTicketAuthComplete( m.body );

		return true;
	}

	/**
	 * Steam's verdict on a ticket a game server asked about.
	 *
	 * WHY THIS MATTERS MORE THAN IT LOOKS: for days the only evidence available was
	 * what the server ANNOUNCED, which conflates two completely different cases --
	 * "the server asked Steam and Steam refused" versus "the server never asked".
	 * Both end up as a fabricated SteamID in the status list, and no amount of
	 * staring at that list can tell them apart. This message is present only in the
	 * first case, and it names the reason.
	 */
	private void logTicketAuthComplete( byte[] body )
	{
		long steamid = 0, gameid = 0;
		int response = -1, state = -1, crc = 0, seq = 0;

		try
		{
			SteamWire.Reader r = new SteamWire.Reader( body );

			while( r.next() )
			{
				switch( r.field() )
				{
				case 1: steamid = r.fixed64(); break;
				case 2: gameid = r.fixed64(); break;
				case 3: state = r.int32(); break;
				case 4: response = r.int32(); break;
				case 6: crc = r.int32(); break;
				case 7: seq = r.int32(); break;
				default: r.skip(); break;
				}
			}
		}
		catch( Throwable e )
		{
			log( "cm: ticket auth complete, but could not be parsed: " + e );
			return;
		}

		log( "cm: TICKET CHECKED BY A SERVER -> " + authSessionResponseName( response )
			+ " (response " + response + ", state " + state + ")"
			+ " steamid " + steamid + " gameid " + gameid
			+ " crc " + crc + " seq " + seq
			+ ( acceptedTicketCrcs.contains( Long.valueOf( crc & 0xFFFFFFFFL ))
				? " [our ticket]" : " [crc we never registered]" ));
	}

	/**
	 * EAuthSessionResponse, spelled out.
	 *
	 * The numbers alone would send the next reader digging through SteamKit, and
	 * the whole point of this log line is to end an argument on the spot.
	 */
	private static String authSessionResponseName( int r )
	{
		switch( r )
		{
		case 0:  return "OK";
		case 1:  return "UserNotConnectedToSteam";
		case 2:  return "NoLicenseOrExpired";
		case 3:  return "VACBanned";
		case 4:  return "LoggedInElseWhere";
		case 5:  return "VACCheckTimedOut";
		case 6:  return "AuthTicketCanceled";
		case 7:  return "AuthTicketInvalidAlreadyUsed";
		case 8:  return "AuthTicketInvalid";
		case 9:  return "PublisherIssuedBan";
		case 10: return "AuthTicketNetworkIdentityFailure";
		default: return "unknown(" + r + ")";
		}
	}

	/**
	 * Take one game-connect token, oldest first, removing it.
	 *
	 * ONE USE EACH: Steam issues these in batches and a server rejects a reused
	 * token, which is why the batch exists at all. Returns null when the pool is
	 * empty -- the caller then has nothing to build a real ticket from and must
	 * fall back rather than send a token twice.
	 */
	public synchronized byte[] takeConnectToken()
	{
		if( connectTokens.isEmpty() )
			return null;

		return connectTokens.remove( 0 );
	}

	public synchronized int connectTokenCount()
	{
		return connectTokens.size();
	}

	private synchronized void storeConnectTokens( byte[] body )
	{
		SteamWire.Reader r = new SteamWire.Reader( body );
		int keep = 0;
		int added = 0;

		while( r.next() )
		{
			switch( r.field() )
			{
			case 1:
				keep = r.int32();
				break;
			case 2:
				connectTokens.add( r.bytes() );
				added++;
				break;
			default:
				r.skip();
				break;
			}
		}

		// Steam says how many to keep; honour it from the OLD end, because the
		// oldest token is the next one to be used and dropping that would waste a
		// still-valid token while keeping a newer one we may never reach.
		if( keep > 0 )
		{
			while( connectTokens.size() > keep )
				connectTokens.remove( 0 );
		}

		log( "cm: got " + added + " game-connect token(s), pool=" + connectTokens.size()
			+ " (max_to_keep=" + keep + ")" );
	}

	/**
	 * Build and register a full Steam AUTH SESSION ticket for a server connect.
	 *
	 * THE MISSING PIECE, and worth spelling out because the first attempt shipped
	 * the wrong thing entirely. Steam has two tickets:
	 *
	 *   * app OWNERSHIP ticket -- "this account owns app N". 178 bytes on the
	 *     reporting device. That is what the previous build sent, and a GoldSrc
	 *     server has no idea what to do with it.
	 *   * auth SESSION ticket -- "this player is connecting right now". It
	 *     CONTAINS the ownership ticket, prefixed by a game-connect token and a
	 *     small session block.
	 *
	 * Layout, from the protocol:
	 *
	 *   [4] token length        [n] game connect token
	 *   [4] session size = 24
	 *   [4] always 1            [4] ticket type (2 = auth session)
	 *   [8] random             [4] timestamp           [4] sequence
	 *   [4] ownership length   [m] ownership ticket
	 *
	 * And then it must be REGISTERED with Steam (ClientAuthList), because a
	 * server checks a ticket by asking Steam about it. An unregistered ticket is
	 * a well-formed blob that Steam disowns -- which is indistinguishable, from
	 * the outside, from the emulated ticket we were sending before.
	 *
	 * Returns null when anything is missing; the caller falls back.
	 */
	public byte[] buildAuthSessionTicket( int appId, int timeoutMs ) throws IOException
	{
		return buildAuthSessionTicket( appId, timeoutMs, null );
	}

	/**
	 * Same, but reporting which token went into the ticket.
	 *
	 * WHY THE CALLER NEEDS THE TOKEN BACK: the pairing Steam checks is (server
	 * SteamID, connect token), and the token is chosen HERE, from the pool, at
	 * build time. Announcing the game before building means announcing a token we
	 * have not picked yet; announcing after means the server may already have
	 * asked. So the ticket is built first, the token comes back with it, and the
	 * caller announces the exact pair before handing the ticket over.
	 *
	 * @param tokenOut when non-null, element 0 receives the token used
	 */
	public byte[] buildAuthSessionTicket( int appId, int timeoutMs, byte[][] tokenOut )
		throws IOException
	{
		byte[] ownership = requestAppOwnershipTicket( appId, timeoutMs );

		if( ownership == null )
			return null;

		byte[] token = takeConnectToken();

		if( token == null )
		{
			// Steam pushes these after logon; none in the pool means the session
			// is too young or Steam is withholding them. Not an error, but there
			// is nothing to build with.
			log( "cm: no game-connect token available, cannot build a session ticket" );
			return null;
		}

		byte[] session = buildSessionBlock( token );
		byte[] ticket = new byte[session.length + 4 + ownership.length];

		System.arraycopy( session, 0, ticket, 0, session.length );
		writeLE32( ticket, session.length, ownership.length );
		System.arraycopy( ownership, 0, ticket, session.length + 4, ownership.length );

		long crc = crc32( session );

		log( "cm: built a session ticket, " + ticket.length + " bytes"
			+ " (token " + token.length + " + session " + session.length
			+ " + ownership " + ownership.length + "), crc=" + crc );

		if( !registerTicket( appId, session, crc, timeoutMs ))
		{
			log( "cm: Steam did not confirm the ticket; a server would be told it is invalid" );
			return null;
		}

		// Only after Steam confirmed it: a token from a rejected ticket must not
		// be announced as the one we will connect with.
		if( tokenOut != null )
			tokenOut[0] = token;

		return ticket;
	}

	/**
	 * The token + session part, which is what gets CRC'd and registered.
	 *
	 * Note the CRC covers THIS, not the combined ticket: Steam is told about the
	 * session part, and the ownership ticket is appended afterwards for the
	 * server's benefit. Getting that wrong means a CRC Steam never confirms.
	 */
	private byte[] buildSessionBlock( byte[] token )
	{
		final int SESSION_SIZE = 24;    // 1 + type + 8 random + timestamp + seq
		final int TICKET_TYPE_AUTH_SESSION = 2;

		byte[] out = new byte[4 + token.length + 4 + SESSION_SIZE];
		int p = 0;

		writeLE32( out, p, token.length );
		p += 4;
		System.arraycopy( token, 0, out, p, token.length );
		p += token.length;

		writeLE32( out, p, SESSION_SIZE );
		p += 4;
		writeLE32( out, p, 1 );
		p += 4;
		writeLE32( out, p, TICKET_TYPE_AUTH_SESSION );
		p += 4;

		// 8 bytes that only have to be unpredictable.
		byte[] rnd = new byte[8];
		new java.security.SecureRandom().nextBytes( rnd );
		System.arraycopy( rnd, 0, out, p, 8 );
		p += 8;

		writeLE32( out, p, (int)( System.nanoTime() & 0xFFFFFFFFL ));
		p += 4;
		writeLE32( out, p, ++ticketSequence );

		return out;
	}

	/**
	 * The ClientAuthList body, split out so its wire encoding can be decoded and
	 * checked in a test rather than only inspected by eye.
	 *
	 * gameid IS A FIXED64, NOT A VARINT. This is what silenced the whole exchange.
	 * In CMsgAuthTicket the field is declared fixed-size, so a varint is a
	 * wire-type mismatch: Steam's parser reads a length where a 64-bit value
	 * belongs, the message is malformed, and a malformed message is dropped with
	 * no reply. The log said exactly that -- "no auth list ack within the timeout"
	 * every single time, never a rejection, because there was nothing to reject.
	 *
	 * Only the fields the reference client sets are sent. estate, eresult and
	 * h_steam_pipe belong to the server's side of the conversation, and
	 * message_sequence is NOT part of CMsgClientAuthList at all -- it exists only
	 * in the ack. Field 6 on the outer message was invented.
	 */
	byte[] buildAuthListBody( int appId, byte[] sessionBlock, long crc )
	{
		SteamWire.Writer ticketMsg = new SteamWire.Writer()
			.fixed64( 4, appId )                     // gameid
			.uint32( 6, (int)crc )                   // ticket_crc
			.bytes( 7, sessionBlock );               // ticket

		return new SteamWire.Writer()
			.uint32( 1, connectTokens.size() )       // tokens_left
			.message( 4, ticketMsg )                 // tickets[0]
			.uint32( 5, appId )                      // app_ids[0]
			.toBytes();
	}

	/**
	 * Tell Steam about the ticket and wait for it to confirm the CRC.
	 *
	 * Confirmation is the whole point: without it the ticket is a blob Steam does
	 * not recognise. The ack lists every accepted CRC, so a match is proof rather
	 * than an assumption.
	 */
	private boolean registerTicket( int appId, byte[] sessionBlock, long crc, int timeoutMs )
		throws IOException
	{
		byte[] body = buildAuthListBody( appId, sessionBlock, crc );

		// AND IT NEEDS A JOBID. Same rule the ownership request already follows:
		// Steam addresses a reply to jobid_source, and a request sent with 0 gets
		// no answer at all. This one was sent without it.
		send( EMSG_CLIENT_AUTH_LIST, body, nextJobId() );
		log( "cm: registered the ticket with Steam, waiting for the ack" );

		synchronized( this )
		{
			acceptedTicketCrcs.clear();
		}

		long deadline = System.currentTimeMillis() + ( timeoutMs > 0 ? timeoutMs : 15000 );

		while( System.currentTimeMillis() < deadline )
		{
			Message m;

			ws.setReadTimeout( 1000 );

			try
			{
				m = nextMessage();
			}
			catch( InterruptedIOException timeout )
			{
				heartbeatIfDue();
				continue;
			}
			finally
			{
				ws.setReadTimeout( READ_TIMEOUT_MS );
			}

			if( m == null )
				return false;

			if( m.emsg == EMSG_CLIENT_GAME_CONNECT_TOKENS )
			{
				storeConnectTokens( m.body );
				continue;
			}

			if( m.emsg == EMSG_CLIENT_LOGGED_OFF )
				return false;

			if( m.emsg != EMSG_CLIENT_AUTH_LIST_ACK )
				continue;

			SteamWire.Reader r = new SteamWire.Reader( m.body );
			boolean found = false;

			while( r.next() )
			{
				if( r.field() == 1 )
				{
					long ack = r.varint() & 0xFFFFFFFFL;

					synchronized( this )
					{
						acceptedTicketCrcs.add( Long.valueOf( ack ));
					}

					if( ack == crc )
						found = true;
				}
				else r.skip();
			}

			log( "cm: auth list ack, our crc " + ( found ? "CONFIRMED" : "absent" ));
			return found;
		}

		log( "cm: no auth list ack within the timeout" );
		return false;
	}

	private static void writeLE32( byte[] buf, int off, int v )
	{
		buf[off] = (byte)( v & 0xFF );
		buf[off + 1] = (byte)(( v >>> 8 ) & 0xFF );
		buf[off + 2] = (byte)(( v >>> 16 ) & 0xFF );
		buf[off + 3] = (byte)(( v >>> 24 ) & 0xFF );
	}

	/**
	 * Ask Steam for the avatar hashes of a batch of accounts.
	 *
	 * WHY THIS EXISTS AT ALL. Avatars were fetched by scraping
	 * steamcommunity.com/profiles/<id>/?xml=1, and the device log is unambiguous
	 * about how that ended: HTTP 429 on 24 of 24 requests, including ids whose
	 * pages load fine in a browser. That endpoint is rate-limited per client, and
	 * a scoreboard asks about everyone at once, so it is the wrong door.
	 *
	 * This is the right one. The session is already open and already authenticated,
	 * CMsgClientPersonaState carries avatar_hash for each account, and 20 raw bytes
	 * of hash are all the CDN needs:
	 *
	 *     https://avatars.steamstatic.com/<hex>_full.jpg
	 *
	 * No web page, no Web API key (the configured one is 64 chars where Steam
	 * issues 32, so that path was dead too), and no per-page limit.
	 *
	 * Returns a map of steamid64 -> 40-char lowercase hex hash, containing only the
	 * accounts Steam actually answered for. An account with no avatar set yields an
	 * all-zero hash, which is reported as absent rather than as a broken URL.
	 *
	 * Blocking, like the ticket request, and called from the presence worker.
	 */
	public java.util.Map<Long,String> requestAvatarHashes( long[] steamIds, int timeoutMs )
		throws IOException
	{
		java.util.HashMap<Long,String> out = new java.util.HashMap<Long,String>();

		if( steamIds == null || steamIds.length == 0 )
			return out;

		SteamWire.Writer w = new SteamWire.Writer()
			.uint32( 1, PERSONA_FLAGS_BASIC );

		// friends is a repeated fixed64. Not packed: this message predates packed
		// repeated fields being the default, and Steam's parser expects one tagged
		// entry per id.
		for( int i = 0; i < steamIds.length; i++ )
			w.fixed64( 2, steamIds[i] );

		send( EMSG_CLIENT_REQUEST_FRIEND_DATA, w.toBytes(), nextJobId() );
		log( "cm: requested persona data for " + steamIds.length + " account(s)" );

		// Steam answers in as many ClientPersonaState messages as it feels like, so
		// this waits for the whole batch rather than for one reply -- but returns
		// early once every id has been seen.
		java.util.HashSet<Long> pending = new java.util.HashSet<Long>();

		for( int i = 0; i < steamIds.length; i++ )
			pending.add( Long.valueOf( steamIds[i] ));

		long deadline = System.currentTimeMillis() + ( timeoutMs > 0 ? timeoutMs : 10000 );

		while( System.currentTimeMillis() < deadline && !pending.isEmpty() )
		{
			Message m;

			ws.setReadTimeout( 1000 );

			try
			{
				m = nextMessage();
			}
			catch( InterruptedIOException timeout )
			{
				heartbeatIfDue();
				continue;
			}
			finally
			{
				ws.setReadTimeout( READ_TIMEOUT_MS );
			}

			if( m == null )
			{
				log( "cm: connection closed while waiting for persona data" );
				break;
			}

			// Same rule as everywhere else in this class: keep servicing the
			// unsolicited traffic, or a token pushed during this window is lost.
			if( m.emsg == EMSG_CLIENT_GAME_CONNECT_TOKENS )
			{
				storeConnectTokens( m.body );
				continue;
			}

			if( m.emsg == EMSG_CLIENT_LOGGED_OFF )
			{
				log( "cm: logged off while waiting for persona data" );
				break;
			}

			if( m.emsg != EMSG_CLIENT_PERSONA_STATE )
				continue;

			SteamWire.Reader r = new SteamWire.Reader( m.body );

			while( r.next() )
			{
				if( r.field() != 2 )        // friends
				{
					r.skip();
					continue;
				}

				SteamWire.Reader f = r.message();
				long id = 0;
				String hash = null;

				while( f.next() )
				{
					if( f.field() == 1 )            // friendid, fixed64
						id = f.fixed64();
					else if( f.field() == 31 )      // avatar_hash, bytes
						hash = toHex( f.bytes() );
					else
						f.skip();
				}

				if( id == 0 )
					continue;

				pending.remove( Long.valueOf( id ));

				// An account with no avatar set reports twenty zero bytes. Treating
				// that as a hash would build a URL that 404s on every retry.
				if( hash != null && hash.length() == 40
				 && !hash.equals( "0000000000000000000000000000000000000000" ))
					out.put( Long.valueOf( id ), hash );
			}
		}

		log( "cm: persona data for " + out.size() + " of " + steamIds.length
			+ " account(s)" + ( pending.isEmpty() ? "" : ", " + pending.size() + " unanswered" ));
		return out;
	}

	/** Lowercase hex, as the avatar CDN spells its paths. */
	private static String toHex( byte[] b )
	{
		if( b == null )
			return null;

		StringBuilder sb = new StringBuilder( b.length * 2 );

		for( int i = 0; i < b.length; i++ )
		{
			int v = b[i] & 0xFF;

			if( v < 16 )
				sb.append( '0' );

			sb.append( Integer.toHexString( v ));
		}

		return sb.toString();
	}

	/** CRC32 of a buffer, as Steam computes it for a ticket. */
	private static long crc32( byte[] data )
	{
		java.util.zip.CRC32 c = new java.util.zip.CRC32();

		c.update( data, 0, data.length );
		return c.getValue();
	}

	/**
	 * Ask Steam for an app-ownership ticket, blocking until it answers.
	 *
	 * This proves the logged-in account owns the app, and it is issued over the
	 * same session that already sets the "playing" status -- no Steam client, no
	 * PC. Returns null if Steam refuses (an account that does not own the app) or
	 * does not answer in time; the caller falls back to the emulated ticket, which
	 * is what every build so far has used.
	 *
	 * Blocking on purpose: it is called from the presence worker thread, off the
	 * engine's frame loop, and a ticket is only wanted at the moment of connecting.
	 */
	public byte[] requestAppOwnershipTicket( int appId, int timeoutMs ) throws IOException
	{
		long jobid = nextJobId();

		send( EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET,
			new SteamWire.Writer().uint32( 1, appId ).toBytes(), jobid );
		log( "cm: requested ownership ticket for app " + appId );

		long deadline = System.currentTimeMillis() + ( timeoutMs > 0 ? timeoutMs : 15000 );

		while( System.currentTimeMillis() < deadline )
		{
			Message m;

			// A short read so the heartbeat below still happens: Steam drops a
			// silent session after about a minute (measured), and a long wait for
			// one reply must not cost the session.
			ws.setReadTimeout( 1000 );

			try
			{
				m = nextMessage();
			}
			catch( InterruptedIOException timeout )
			{
				heartbeatIfDue();
				continue;
			}
			finally
			{
				ws.setReadTimeout( READ_TIMEOUT_MS );
			}

			if( m == null )
			{
				log( "cm: connection closed while waiting for the ownership ticket" );
				return null;
			}

			// Keep servicing the unsolicited traffic while we wait, or a token
			// pushed during this window would be thrown away.
			if( m.emsg == EMSG_CLIENT_GAME_CONNECT_TOKENS )
			{
				storeConnectTokens( m.body );
				continue;
			}

			if( m.emsg == EMSG_CLIENT_LOGGED_OFF )
			{
				log( "cm: logged off while waiting for the ownership ticket" );
				return null;
			}

			if( m.emsg != EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET_RESPONSE )
				continue;

			int eresult = 0;
			byte[] ticket = null;
			SteamWire.Reader r = new SteamWire.Reader( m.body );

			while( r.next() )
			{
				switch( r.field() )
				{
				case 1: eresult = r.int32(); break;
				case 2: r.int32(); break;          // app_id, echoed back
				case 3: ticket = r.bytes(); break;
				default: r.skip(); break;
				}
			}

			if( eresult != ERESULT_OK || ticket == null )
			{
				log( "cm: Steam refused the ownership ticket, eresult " + eresult );
				return null;
			}

			log( "cm: got an ownership ticket, " + ticket.length + " bytes" );
			logOwnershipTicket( appId, ticket );
			return ticket;
		}

		log( "cm: no ownership ticket within the timeout" );
		return null;
	}

	/**
	 * Read back the app-ownership ticket we just received and say, in the log,
	 * whether it actually proves ownership of the app we asked about.
	 *
	 * WHY THIS EXISTS: a server validates a real Steam ticket by asking Steam,
	 * and Steam refuses a ticket for an app the account does not own. On the
	 * emulated servers we tested, other players with genuine tickets get a real
	 * STEAM_0:1 id while we get one derived from our IP -- the exact symptom of a
	 * ticket Steam declined. The ownership ticket is the one piece of evidence we
	 * already hold in memory that can confirm or kill the "does not own CS 1.6"
	 * hypothesis, so it is parsed rather than guessed at.
	 *
	 * The ticket is NOT protobuf. Its layout (steam3 app-ownership ticket) is,
	 * little-endian throughout:
	 *   u32 ownershipTicketLength  -- counts from this field up to the signature
	 *   u32 version
	 *   u64 steamID
	 *   u32 appID
	 *   u32 externalIP, u32 internalIP, u32 flags, u32 gen date, u32 expiry
	 *   u16 licenseCount, u32 license[licenseCount]
	 *   u16 dlcCount, then per dlc: u32 appID, u16 n, u32 license[n]
	 *   u16 reserved, then 128-byte RSA signature
	 *
	 * Parsing is defensive: a Steam emulator could hand back something shorter,
	 * and a malformed ticket must produce a clear log line, never an exception
	 * that aborts the connect.
	 */
	private void logOwnershipTicket( int wantAppId, byte[] t )
	{
		long[] parsed = parseOwnershipTicket( t );

		if( parsed == null )
		{
			log( "cm: ownership ticket layout unexpected (total " + t.length
				+ ") -- not decoding, raw head " + hexHead( t, 24 ));
			return;
		}

		long appId    = parsed[0];
		long steamId  = parsed[1];
		long version  = parsed[2];
		long licCount = parsed[3];
		boolean ownsWanted = ( appId == wantAppId );

		StringBuilder lic = new StringBuilder();
		for( int i = 0; i < licCount; i++ )
			lic.append( i == 0 ? "" : "," ).append( parsed[4 + i] );

		log( "cm: OWNERSHIP for app " + appId + " (asked " + wantAppId + "), "
			+ "steamID " + steamId + ", version " + version + ", "
			+ licCount + " license(s) [" + lic + "] -- "
			+ ( ownsWanted
				? "ticket IS for the requested app"
				: "*** ticket is for a DIFFERENT app -> Steam would refuse it "
				  + "for app " + wantAppId ));
	}

	/**
	 * Pure parse of a steam3 app-ownership ticket. Returns
	 *   { appId, steamId, version, licenseCount, license0, license1, ... }
	 * or null when the bytes do not match the layout. No logging, no side
	 * effects, no exceptions escaping -- so it can be exercised directly by a
	 * test that builds ticket bytes and checks the fields come back at the right
	 * offsets. The layout (little-endian) is:
	 *   u32 ownershipTicketLength  -- from this field up to the signature
	 *   u32 version
	 *   u64 steamID
	 *   u32 appID
	 *   u32 externalIP, internalIP, flags, gen date, expiry  (5 * u32, skipped)
	 *   u16 licenseCount, u32 license[licenseCount]
	 *   ... dlc list and 128-byte signature follow, not needed here
	 */
	static long[] parseOwnershipTicket( byte[] t )
	{
		try
		{
			if( t == null || t.length < 4 )
				return null;

			int[] off = { 0 };
			long ownLen = readLE32u( t, off );

			// The length field points at the signature start. A real ticket has
			// either a 128-byte signature after it or (rarely) none. Any other
			// trailing size means the layout is not what we expect, and every
			// field below would be read from the wrong offset -- reject instead of
			// returning numbers that look plausible but are garbage.
			long trailing = t.length - ownLen;
			if( ownLen < 42 || ownLen > t.length || ( trailing != 0 && trailing != 128 ))
				return null;

			long version = readLE32u( t, off );
			long steamId = readLE64u( t, off );
			long appId   = readLE32u( t, off );

			off[0] += 4 * 5;   // externalIP, internalIP, flags, gen, expiry

			if( off[0] + 2 > t.length )
				return null;

			int licCount = readLE16u( t, off );

			// A license count that runs past the ticket means we mis-read: bail
			// rather than index out of bounds.
			if( licCount < 0 || off[0] + licCount * 4 > t.length )
				return null;

			long[] result = new long[4 + licCount];
			result[0] = appId;
			result[1] = steamId;
			result[2] = version;
			result[3] = licCount;

			for( int i = 0; i < licCount; i++ )
				result[4 + i] = readLE32u( t, off );

			return result;
		}
		catch( Throwable e )
		{
			return null;
		}
	}

	private static long readLE32u( byte[] b, int[] off )
	{
		int o = off[0];
		off[0] += 4;
		return  ( b[o]   & 0xFFL )
			| (( b[o+1] & 0xFFL ) << 8 )
			| (( b[o+2] & 0xFFL ) << 16 )
			| (( b[o+3] & 0xFFL ) << 24 );
	}

	private static int readLE16u( byte[] b, int[] off )
	{
		int o = off[0];
		off[0] += 2;
		return ( b[o] & 0xFF ) | (( b[o+1] & 0xFF ) << 8 );
	}

	private static long readLE64u( byte[] b, int[] off )
	{
		long lo = readLE32u( b, off );
		long hi = readLE32u( b, off );
		return lo | ( hi << 32 );
	}

	private static String hexHead( byte[] b, int n )
	{
		StringBuilder s = new StringBuilder();
		for( int i = 0; i < n && i < b.length; i++ )
			s.append( String.format( "%02x", b[i] & 0xFF ));
		return s.toString();
	}

	public boolean isConnected()
	{
		return ws != null && !ws.isClosed();
	}

	public long getSteamId()
	{
		return steamid;
	}

	public void close()
	{
		// TELL STEAM before dropping the socket. A session Steam has not been
		// told about lingers until its own timeout, and the profile keeps showing
		// the game for as long as it does -- "показывает что я в игре, даже когда
		// не играю". logOff() swallows its own errors, so this stays a safe
		// teardown path.
		logOff();
		closeQuietly();
	}

	private void closeQuietly()
	{
		if( ws != null )
		{
			ws.close();
			ws = null;
		}
	}

	// -----------------------------------------------------------------------
	// Framing
	// -----------------------------------------------------------------------

	private static final class Message
	{
		int emsg;
		long steamid;
		int sessionId;
		byte[] body;
	}

	private void send( int emsg, byte[] body ) throws IOException
	{
		send( emsg, body, 0 );
	}

	/**
	 * Send with an optional jobid_source.
	 *
	 * WHY THE PARAMETER EXISTS: the six original messages are fire-and-forget, so
	 * a header with just steamid and session was enough. A request that expects an
	 * answer must carry jobid_source (header field 10) -- Steam uses it to address
	 * the reply, and a request with jobid 0 gets no reply at all. Measured against
	 * a live CM: silence before, an answer after.
	 */
	private void send( int emsg, byte[] body, long jobid ) throws IOException
	{
		if( ws == null )
			throw new IOException( "not connected" );

		SteamWire.Writer header = new SteamWire.Writer();

		if( steamid != 0 )
			header.fixed64( 1, steamid );

		if( sessionId != 0 )
			header.int32( 2, sessionId );

		if( jobid != 0 )
			header.fixed64( 10, jobid );

		byte[] hdr = header.toBytes();
		ByteArrayOutputStream frame = new ByteArrayOutputStream( 8 + hdr.length + body.length );
		writeLE32( frame, emsg | PROTO_MASK );
		writeLE32( frame, hdr.length );
		frame.write( hdr, 0, hdr.length );
		frame.write( body, 0, body.length );

		ws.sendBinary( frame.toByteArray() );
	}

	/**
	 * Build a jobid the way Steam expects one.
	 *
	 * NOT a counter and not a random 64-bit value: the field is packed --
	 * bits 0..19 a sequence, bits 20..49 seconds since 2005-01-01 (SteamKit's
	 * GlobalID). A number outside that layout lands in the wrong fields and the
	 * reply never comes back.
	 */
	private long nextJobId()
	{
		long startTime = ( System.currentTimeMillis() / 1000L ) - 1104537600L;

		if( ++jobSeq > 0xFFFFF )
			jobSeq = 1;

		return ( jobSeq & 0xFFFFFL ) | (( startTime & 0x3FFFFFFFL ) << 20 );
	}

	/** Next single message, unwrapping Multi containers as needed. */
	private Message nextMessage() throws IOException
	{
		while( true )
		{
			if( !pending.isEmpty() )
				return pending.remove( 0 );

			byte[] frame = ws.receive();

			if( frame == null )
				return null;

			Message m = parseFrame( frame );

			if( m == null )
				continue;

			if( m.emsg == EMSG_MULTI )
			{
				expandMulti( m.body );
				continue;
			}

			return m;
		}
	}

	private final List<Message> pending = new ArrayList<Message>();

	private static Message parseFrame( byte[] frame ) throws IOException
	{
		if( frame.length < 8 )
			throw new IOException( "short frame: " + frame.length + " bytes" );

		int rawEmsg = readLE32( frame, 0 );
		int hdrLen = readLE32( frame, 4 );

		if(( rawEmsg & PROTO_MASK ) == 0 )
		{
			// Pre-protobuf struct message. Steam still sends a couple of these
			// to old clients; none matter to us, and mis-parsing one as
			// protobuf would corrupt the stream, so skip it wholesale.
			return null;
		}

		if( hdrLen < 0 || 8 + hdrLen > frame.length )
			throw new IOException( "bad header length " + hdrLen );

		Message m = new Message();
		m.emsg = rawEmsg & ~PROTO_MASK;

		SteamWire.Reader hr = new SteamWire.Reader( frame, 8, hdrLen );

		while( hr.next() )
		{
			switch( hr.field() )
			{
			case 1: m.steamid = hr.fixed64(); break;
			case 2: m.sessionId = hr.int32(); break;
			default: hr.skip(); break;
			}
		}

		int bodyOff = 8 + hdrLen;
		m.body = new byte[frame.length - bodyOff];
		System.arraycopy( frame, bodyOff, m.body, 0, m.body.length );
		return m;
	}

	private void expandMulti( byte[] body ) throws IOException
	{
		int sizeUnzipped = 0;
		byte[] payload = null;
		SteamWire.Reader r = new SteamWire.Reader( body );

		while( r.next() )
		{
			switch( r.field() )
			{
			case 1: sizeUnzipped = (int)r.varint(); break;
			case 2: payload = r.bytes(); break;
			default: r.skip(); break;
			}
		}

		if( payload == null )
			return;

		if( sizeUnzipped > 0 )
			payload = gunzip( payload, sizeUnzipped );

		// Inner layout: repeated [uint32 length][message]
		int off = 0;

		while( off + 4 <= payload.length )
		{
			int len = readLE32( payload, off );
			off += 4;

			if( len < 0 || off + len > payload.length )
				throw new IOException( "bad Multi sub-message length " + len );

			byte[] sub = new byte[len];
			System.arraycopy( payload, off, sub, 0, len );
			off += len;

			Message m = parseFrame( sub );

			if( m == null )
				continue;

			if( m.emsg == EMSG_MULTI )
				expandMulti( m.body );      // Steam does nest these
			else
				pending.add( m );
		}
	}

	private static byte[] gunzip( byte[] data, int expectedSize ) throws IOException
	{
		GZIPInputStream gz = new GZIPInputStream( new java.io.ByteArrayInputStream( data ));
		ByteArrayOutputStream bos = new ByteArrayOutputStream(
			expectedSize > 0 ? expectedSize : data.length * 4 );
		byte[] buf = new byte[8192];
		int n;

		try
		{
			while(( n = gz.read( buf )) > 0 )
				bos.write( buf, 0, n );
		}
		finally
		{
			gz.close();
		}

		return bos.toByteArray();
	}

	private static void writeLE32( ByteArrayOutputStream out, int v )
	{
		out.write( v & 0xFF );
		out.write(( v >>> 8 ) & 0xFF );
		out.write(( v >>> 16 ) & 0xFF );
		out.write(( v >>> 24 ) & 0xFF );
	}

	private static int readLE32( byte[] b, int off )
	{
		return ( b[off] & 0xFF )
			| (( b[off + 1] & 0xFF ) << 8 )
			| (( b[off + 2] & 0xFF ) << 16 )
			| (( b[off + 3] & 0xFF ) << 24 );
	}

	/** Dotted-quad to the big-endian int the protocol wants. 0 if unparseable. */
	public static int ipToInt( String ip )
	{		if( ip == null )
			return 0;

		String[] parts = ip.split( "\\." );

		if( parts.length != 4 )
			return 0;

		int v = 0;

		try
		{
			for( int i = 0; i < 4; i++ )
			{
				int octet = Integer.parseInt( parts[i].trim() );

				if( octet < 0 || octet > 255 )
					return 0;

				v = ( v << 8 ) | octet;
			}
		}
		catch( NumberFormatException e )
		{
			return 0;
		}

		return v;
	}

	/**
	 * The inverse of ipToInt, for building the rich-presence "connect" string.
	 *
	 * Deliberately here rather than in the caller: the byte order is a property of
	 * this protocol (big-endian, most significant octet first, matching what
	 * ipToInt packs), and having the two directions side by side is what keeps
	 * them from disagreeing. A mismatch would produce a "Join Game" that quietly
	 * sends the friend to a mirrored address.
	 */
	public static String ipToString( int ip )
	{
		return (( ip >> 24 ) & 0xFF ) + "." + (( ip >> 16 ) & 0xFF ) + "."
			+ (( ip >> 8 ) & 0xFF ) + "." + ( ip & 0xFF );
	}
}
