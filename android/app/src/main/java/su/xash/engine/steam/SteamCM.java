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
	 * Announce a game as being played.
	 *
	 * @param appid     Steam app id; 10 is Counter-Strike
	 * @param extraInfo optional title shown in the status
	 * @param serverIp  optional server address, big-endian int (0 = none)
	 * @param serverPort server port
	 */
	public void setGamePlayed( long appid, String extraInfo, int serverIp, int serverPort )
		throws IOException
	{
		SteamWire.Writer game = new SteamWire.Writer().fixed64( 2, appid );

		if( serverPort > 0 )
			game.uint32( 4, serverPort );

		if( extraInfo != null && extraInfo.length() > 0 )
			game.string( 7, extraInfo );

		if( serverIp != 0 )
			game.message( 23, new SteamWire.Writer().fixed32( 1, serverIp ));

		send( EMSG_CLIENT_GAMES_PLAYED,
			new SteamWire.Writer().message( 1, game ).toBytes() );
	}

	/** Clear the status: an empty games_played list means "not playing". */
	public void clearGamePlayed() throws IOException
	{
		send( EMSG_CLIENT_GAMES_PLAYED, new byte[0] );
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
	 * @return false if the connection is gone
	 */
	public boolean pump( int timeoutMs ) throws IOException
	{
		if( ws == null || ws.isClosed() )
			return false;

		Message m;

		try
		{
			m = nextMessage();
		}
		catch( InterruptedIOException timeout )
		{
			return true;    // nothing to read, connection still fine
		}

		if( m == null )
			return false;

		if( m.emsg == EMSG_CLIENT_LOGGED_OFF )
		{
			log( "cm: logged off by Steam, eresult " + readEresult( m.body ));
			return false;
		}

		return true;
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
		if( ws == null )
			throw new IOException( "not connected" );

		SteamWire.Writer header = new SteamWire.Writer();

		if( steamid != 0 )
			header.fixed64( 1, steamid );

		if( sessionId != 0 )
			header.int32( 2, sessionId );

		byte[] hdr = header.toBytes();
		ByteArrayOutputStream frame = new ByteArrayOutputStream( 8 + hdr.length + body.length );
		writeLE32( frame, emsg | PROTO_MASK );
		writeLE32( frame, hdr.length );
		frame.write( hdr, 0, hdr.length );
		frame.write( body, 0, body.length );

		ws.sendBinary( frame.toByteArray() );
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
	{
		if( ip == null )
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
}
