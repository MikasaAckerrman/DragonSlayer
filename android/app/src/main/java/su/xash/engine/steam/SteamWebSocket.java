/*
SteamWebSocket.java - minimal RFC 6455 client over TLS.
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Why a WebSocket at all, and why hand-rolled:

Steam CM servers accept two transports. Raw TCP requires the client to run
Valve's own encryption handshake (ClientEncryptRequest: RSA-encrypt a session
key with Valve's hardcoded public key, then AES-256-CBC with an HMAC prefix).
The wss:// endpoints put that same protocol inside TLS, so the platform's TLS
stack does the cryptography and there is no custom handshake to get subtly
wrong -- a far smaller attack surface for a security-relevant path.

Android has no WebSocket client in the framework (Java 11's HttpClient is not
available), so it would mean adding OkHttp to the build. The client half of
RFC 6455 that we need -- text/binary frames, masking, ping/pong, close -- is
about as much code as the build wiring, and unlike the dependency it can be
tested on a desktop JVM with no Android at all.

Deliberately no android.* imports; see tests/compile_steam_cm.sh.
*/

package su.xash.engine.steam;

import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Locale;

import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

public final class SteamWebSocket
{
	private static final String GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

	private static final int OP_CONTINUATION = 0x0;
	private static final int OP_TEXT = 0x1;
	private static final int OP_BINARY = 0x2;
	private static final int OP_CLOSE = 0x8;
	private static final int OP_PING = 0x9;
	private static final int OP_PONG = 0xA;

	// Steam frames are small; anything larger means we lost frame sync and
	// should fail loudly instead of trying to allocate it.
	private static final int MAX_FRAME = 8 * 1024 * 1024;

	private final Socket socket;
	private final DataInputStream in;
	private final OutputStream out;
	private final SecureRandom random = new SecureRandom();
	private volatile boolean closed;

	private SteamWebSocket( Socket socket ) throws IOException
	{
		this.socket = socket;
		this.in = new DataInputStream( socket.getInputStream() );
		this.out = new BufferedOutputStream( socket.getOutputStream() );
	}

	/**
	 * @param endpoint host:port as returned by GetCMListForConnect
	 * @param path     "/cmsocket/"
	 */
	public static SteamWebSocket connect( String endpoint, String path,
		int connectTimeoutMs, int readTimeoutMs ) throws IOException
	{
		int colon = endpoint.lastIndexOf( ':' );
		String host = colon < 0 ? endpoint : endpoint.substring( 0, colon );
		int port = colon < 0 ? 443 : Integer.parseInt( endpoint.substring( colon + 1 ));

		SSLSocket s = (SSLSocket)SSLSocketFactory.getDefault().createSocket();
		s.connect( new java.net.InetSocketAddress( host, port ), connectTimeoutMs );
		s.setSoTimeout( readTimeoutMs );
		s.setTcpNoDelay( true );

		// Without SNI + hostname verification, TLS here would authenticate
		// nothing: any host able to present some valid certificate would do.
		javax.net.ssl.SSLParameters params = s.getSSLParameters();
		params.setEndpointIdentificationAlgorithm( "HTTPS" );
		s.setSSLParameters( params );
		s.startHandshake();

		SteamWebSocket ws = new SteamWebSocket( s );

		try
		{
			ws.handshake( host, port, path );
		}
		catch( IOException e )
		{
			ws.closeQuietly();
			throw e;
		}

		return ws;
	}

	private void handshake( String host, int port, String path ) throws IOException
	{
		byte[] nonce = new byte[16];
		random.nextBytes( nonce );
		String key = SteamWire.base64Encode( nonce );

		StringBuilder req = new StringBuilder();
		req.append( "GET " ).append( path ).append( " HTTP/1.1\r\n" );
		req.append( "Host: " ).append( host ).append( ':' ).append( port ).append( "\r\n" );
		req.append( "Upgrade: websocket\r\n" );
		req.append( "Connection: Upgrade\r\n" );
		req.append( "Sec-WebSocket-Key: " ).append( key ).append( "\r\n" );
		req.append( "Sec-WebSocket-Version: 13\r\n" );
		req.append( "User-Agent: Valve/Steam HTTP Client 1.0\r\n" );
		req.append( "\r\n" );

		out.write( req.toString().getBytes( "ISO-8859-1" ));
		out.flush();

		String status = readLine();

		if( status == null || !status.contains( " 101" ))
			throw new IOException( "WebSocket upgrade refused: " + status );

		String accept = null;
		String line;

		while(( line = readLine() ) != null && line.length() > 0 )
		{
			int c = line.indexOf( ':' );

			if( c > 0 && line.substring( 0, c ).trim()
				.toLowerCase( Locale.US ).equals( "sec-websocket-accept" ))
			{
				accept = line.substring( c + 1 ).trim();
			}
		}

		// Proving the peer actually spoke WebSocket, rather than a proxy having
		// replied 101 to something it did not understand.
		String expect = expectedAccept( key );

		if( accept == null || !accept.equals( expect ))
			throw new IOException( "bad Sec-WebSocket-Accept: " + accept );
	}

	private static String expectedAccept( String key ) throws IOException
	{
		try
		{
			MessageDigest sha1 = MessageDigest.getInstance( "SHA-1" );
			return SteamWire.base64Encode(
				sha1.digest(( key + GUID ).getBytes( "ISO-8859-1" )));
		}
		catch( Exception e )
		{
			throw new IOException( "SHA-1 unavailable: " + e, e );
		}
	}

	private String readLine() throws IOException
	{
		ByteArrayOutputStream bos = new ByteArrayOutputStream( 128 );
		int prev = -1, b;

		while(( b = in.read() ) >= 0 )
		{
			if( prev == '\r' && b == '\n' )
			{
				byte[] raw = bos.toByteArray();
				return new String( raw, 0, raw.length - 1, "ISO-8859-1" );
			}

			bos.write( b );
			prev = b;
		}

		return null;
	}

	// -----------------------------------------------------------------------
	// Frames
	// -----------------------------------------------------------------------

	public synchronized void sendBinary( byte[] payload ) throws IOException
	{
		sendFrame( OP_BINARY, payload );
	}

	private synchronized void sendFrame( int opcode, byte[] payload ) throws IOException
	{
		if( closed )
			throw new IOException( "socket closed" );

		out.write( 0x80 | opcode );  // FIN + opcode

		int len = payload.length;

		// Client frames MUST be masked (RFC 6455 5.1); Steam drops unmasked ones.
		if( len < 126 )
		{
			out.write( 0x80 | len );
		}
		else if( len <= 0xFFFF )
		{
			out.write( 0x80 | 126 );
			out.write(( len >>> 8 ) & 0xFF );
			out.write( len & 0xFF );
		}
		else
		{
			out.write( 0x80 | 127 );

			for( int i = 7; i >= 0; i-- )
				out.write( (int)((((long)len ) >>> ( i * 8 )) & 0xFF ));
		}

		byte[] mask = new byte[4];
		random.nextBytes( mask );
		out.write( mask );

		for( int i = 0; i < len; i++ )
			out.write( payload[i] ^ mask[i & 3] );

		out.flush();
	}

	/**
	 * Reads the next binary message, transparently handling ping/pong and
	 * fragmentation.
	 *
	 * @return payload, or null if the peer closed the connection
	 */
	public byte[] receive() throws IOException
	{
		ByteArrayOutputStream assembled = null;

		while( true )
		{
			int b0 = in.read();

			if( b0 < 0 )
				return null;

			boolean fin = ( b0 & 0x80 ) != 0;
			int opcode = b0 & 0x0F;
			int b1 = in.readUnsignedByte();
			boolean masked = ( b1 & 0x80 ) != 0;
			long len = b1 & 0x7F;

			if( len == 126 )
			{
				len = in.readUnsignedShort();
			}
			else if( len == 127 )
			{
				len = in.readLong();

				if( len < 0 )
					throw new IOException( "frame length overflow" );
			}

			if( len > MAX_FRAME )
				throw new IOException( "frame too large: " + len );

			byte[] maskKey = null;

			if( masked )
			{
				maskKey = new byte[4];
				in.readFully( maskKey );
			}

			byte[] payload = new byte[(int)len];
			in.readFully( payload );

			if( maskKey != null )
			{
				for( int i = 0; i < payload.length; i++ )
					payload[i] ^= maskKey[i & 3];
			}

			switch( opcode )
			{
			case OP_PING:
				sendFrame( OP_PONG, payload );
				continue;

			case OP_PONG:
				continue;

			case OP_CLOSE:
				closed = true;
				try
				{
					sendFrame( OP_CLOSE, new byte[0] );
				}
				catch( IOException ignored ) {}
				return null;

			case OP_CONTINUATION:
				if( assembled == null )
					throw new IOException( "continuation without a start frame" );

				assembled.write( payload );

				if( fin )
					return assembled.toByteArray();

				continue;

			case OP_BINARY:
			case OP_TEXT:
				if( fin )
					return payload;

				assembled = new ByteArrayOutputStream( payload.length * 2 );
				assembled.write( payload );
				continue;

			default:
				throw new IOException( "unexpected opcode " + opcode );
			}
		}
	}

	public boolean isClosed()
	{
		return closed || socket.isClosed();
	}

	public void close()
	{
		if( !closed )
		{
			closed = true;

			try
			{
				sendFrame( OP_CLOSE, new byte[]{ 0x03, (byte)0xE8 }); // 1000 normal
			}
			catch( IOException ignored ) {}
		}

		closeQuietly();
	}

	private void closeQuietly()
	{
		closed = true;

		try
		{
			socket.close();
		}
		catch( IOException ignored ) {}
	}
}
