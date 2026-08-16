/*
SteamAuth.java - Steam's current (2023+) authentication flow.
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

What this replaces and why:

SteamLoginActivity does Steam OpenID 2.0 in a WebView and keeps exactly one
thing out of the answer: claimed_id, i.e. the SteamID64. OpenID answers "is
this profile really yours" -- it hands out no session and no permission to
write anything back. That is why the launcher could show an avatar but could
never say "playing Counter-Strike".

This flow yields a refresh_token, which is an actual credential: SteamCM logs
on with it and may then send ClientGamesPlayed.

  BeginAuthSessionViaQR      -> challenge URL, shown as a QR / tappable link
  BeginAuthSessionViaCredentials -> for account name + password
  PollAuthSessionStatus      -> poll until the user approves, yields tokens
  UpdateAuthSessionWithSteamGuardCode -> submit an email / authenticator code

Two facts learned the hard way, both worth keeping in writing:

  * The legacy "username + password in ClientLogon" login is dead. Valve
    switched it off; it now answers EResult 5 (InvalidPassword) for every
    account, no matter the password. Anything built on it is unfixable.
  * These Authentication service calls only work over HTTP against
    api.steampowered.com. Sending them as unified messages over an
    unauthenticated CM connection gets no reply at all -- which makes sense,
    since there is no session yet to route a response to.

Transport is HttpsURLConnection with a form-encoded input_protobuf_encoded
parameter, which is what Steam's own clients send. No HTTP library needed.
*/

package su.xash.engine.steam;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.security.KeyFactory;
import java.security.PublicKey;
import java.security.spec.RSAPublicKeySpec;

import javax.crypto.Cipher;

public final class SteamAuth
{
	private static final String TAG = "SteamAuth";
	private static final String API =
		"https://api.steampowered.com/IAuthenticationService/";

	// EAuthTokenPlatformType
	public static final int PLATFORM_STEAM_CLIENT = 1;
	public static final int PLATFORM_WEB_BROWSER = 2;
	public static final int PLATFORM_MOBILE_APP = 3;

	// EAuthSessionGuardType
	public static final int GUARD_NONE = 1;
	public static final int GUARD_EMAIL_CODE = 2;
	public static final int GUARD_DEVICE_CODE = 3;         // mobile authenticator
	public static final int GUARD_DEVICE_CONFIRMATION = 4; // approve in the app
	public static final int GUARD_EMAIL_CONFIRMATION = 5;

	// EOSType.LinuxUnknown; the field is uint32, so the negative enum is masked
	// on the wire by SteamWire.uint32.
	private static final int OS_TYPE_LINUX = -203;

	private static final int HTTP_TIMEOUT_MS = 30000;

	private final String deviceName;

	public SteamAuth( String deviceName )
	{
		this.deviceName = deviceName;
	}

	// -----------------------------------------------------------------------
	// Results
	// -----------------------------------------------------------------------

	/** A started auth session, waiting for the user to approve it. */
	public static final class Session
	{
		public long clientId;
		public byte[] requestId;
		public float interval = 5.0f;      // seconds Steam asks us to wait
		public long steamid;               // credentials flow only
		public String challengeUrl;        // QR flow only
		public int[] confirmations = new int[0];

		public boolean needsCode()
		{
			for( int c : confirmations )
			{
				if( c == GUARD_EMAIL_CODE || c == GUARD_DEVICE_CODE )
					return true;
			}
			return false;
		}

		public boolean emailCode()
		{
			for( int c : confirmations )
			{
				if( c == GUARD_EMAIL_CODE )
					return true;
			}
			return false;
		}

		public boolean needsAppConfirmation()
		{
			for( int c : confirmations )
			{
				if( c == GUARD_DEVICE_CONFIRMATION )
					return true;
			}
			return false;
		}
	}

	/** Outcome of one poll. */
	public static final class Poll
	{
		public String refreshToken;        // non-null once approved
		public String accessToken;
		public String accountName;
		public long newClientId;           // Steam rotated the session
		public String newChallengeUrl;     // QR was refreshed, re-render it
		public boolean hadRemoteInteraction;

		public boolean done()
		{
			return refreshToken != null && refreshToken.length() > 0;
		}
	}

	public static final class SteamAuthException extends IOException
	{
		public final int eresult;

		public SteamAuthException( String message, int eresult )
		{
			super( message + " (eresult " + eresult + ")" );
			this.eresult = eresult;
		}
	}

	// -----------------------------------------------------------------------
	// QR flow
	// -----------------------------------------------------------------------

	public Session beginQR() throws IOException
	{
		SteamWire.Writer req = new SteamWire.Writer()
			.string( 1, deviceName )                 // device_friendly_name
			.uint32( 2, PLATFORM_STEAM_CLIENT )      // platform_type
			.message( 3, deviceDetails() );          // device_details

		byte[] resp = post( "BeginAuthSessionViaQR", req.toBytes() );

		Session s = new Session();
		SteamWire.Reader r = new SteamWire.Reader( resp );
		int[] confs = new int[8];
		int nconf = 0;

		while( r.next() )
		{
			switch( r.field() )
			{
			case 1: s.clientId = r.varint(); break;
			case 2: s.challengeUrl = r.string(); break;
			case 3: s.requestId = r.bytes(); break;
			case 4: s.interval = r.float32(); break;
			case 5:
			{
				SteamWire.Reader c = r.message();
				while( c.next() )
				{
					if( c.field() == 1 && nconf < confs.length )
						confs[nconf++] = (int)c.varint();
					else
						c.skip();
				}
				break;
			}
			default: r.skip(); break;
			}
		}

		s.confirmations = trim( confs, nconf );
		return s;
	}

	// -----------------------------------------------------------------------
	// Password flow
	// -----------------------------------------------------------------------

	public Session beginPassword( String accountName, String password ) throws IOException
	{
		// Steam hands out a short-lived RSA key per account; the password is
		// encrypted with it and the key's timestamp must be echoed back.
		//
		// This one is a GET, unlike everything else here. It is a read-only
		// method and Steam answers POST with HTTP 405 -- which is easy to
		// mistake for "account refused", because the request that follows never
		// happens either way. Verified against the live endpoint.
		byte[] keyResp = get( "GetPasswordRSAPublicKey",
			new SteamWire.Writer().string( 1, accountName ).toBytes() );

		String mod = null, exp = null;
		long timestamp = 0;
		SteamWire.Reader kr = new SteamWire.Reader( keyResp );

		while( kr.next() )
		{
			switch( kr.field() )
			{
			case 1: mod = kr.string(); break;
			case 2: exp = kr.string(); break;
			case 3: timestamp = kr.varint(); break;
			default: kr.skip(); break;
			}
		}

		if( mod == null || exp == null )
			throw new IOException( "GetPasswordRSAPublicKey: no key in response" );

		String encrypted = rsaEncrypt( mod, exp, password );

		SteamWire.Writer req = new SteamWire.Writer()
			.string( 1, deviceName )                 // device_friendly_name
			.string( 2, accountName )                // account_name
			.string( 3, encrypted )                  // encrypted_password
			.varint( 4, timestamp )                  // encryption_timestamp
			.bool( 5, true )                         // remember_login
			.uint32( 6, PLATFORM_STEAM_CLIENT )      // platform_type
			.uint32( 7, 1 )                          // persistence = Persistent
			.message( 9, deviceDetails() );          // device_details

		byte[] resp = post( "BeginAuthSessionViaCredentials", req.toBytes() );

		Session s = new Session();
		SteamWire.Reader r = new SteamWire.Reader( resp );
		int[] confs = new int[8];
		int nconf = 0;

		while( r.next() )
		{
			switch( r.field() )
			{
			case 1: s.clientId = r.varint(); break;
			case 2: s.requestId = r.bytes(); break;
			case 3: s.interval = r.float32(); break;
			case 4:
			{
				SteamWire.Reader c = r.message();
				while( c.next() )
				{
					if( c.field() == 1 && nconf < confs.length )
						confs[nconf++] = (int)c.varint();
					else
						c.skip();
				}
				break;
			}
			case 5: s.steamid = r.varint(); break;
			default: r.skip(); break;
			}
		}

		s.confirmations = trim( confs, nconf );
		return s;
	}

	/** Submit an email or mobile-authenticator code for a started session. */
	public void submitGuardCode( Session s, String code ) throws IOException
	{
		SteamWire.Writer req = new SteamWire.Writer()
			.varint( 1, s.clientId )
			.fixed64( 2, s.steamid )
			.string( 3, code )
			.uint32( 4, s.emailCode() ? GUARD_EMAIL_CODE : GUARD_DEVICE_CODE );

		post( "UpdateAuthSessionWithSteamGuardCode", req.toBytes() );
	}

	// -----------------------------------------------------------------------
	// Polling
	// -----------------------------------------------------------------------

	public Poll poll( Session s ) throws IOException
	{
		SteamWire.Writer req = new SteamWire.Writer()
			.varint( 1, s.clientId )
			.bytes( 2, s.requestId );

		byte[] resp = post( "PollAuthSessionStatus", req.toBytes() );

		Poll p = new Poll();
		SteamWire.Reader r = new SteamWire.Reader( resp );

		while( r.next() )
		{
			switch( r.field() )
			{
			case 1: p.newClientId = r.varint(); break;
			case 2: p.newChallengeUrl = r.string(); break;
			case 3: p.refreshToken = r.string(); break;
			case 4: p.accessToken = r.string(); break;
			case 5: p.hadRemoteInteraction = r.bool(); break;
			case 6: p.accountName = r.string(); break;
			default: r.skip(); break;
			}
		}

		// Steam rotates the client id (and the QR) while we wait; carry it over
		// or every later poll talks about a session that no longer exists.
		if( p.newClientId != 0 )
			s.clientId = p.newClientId;

		if( p.newChallengeUrl != null && p.newChallengeUrl.length() > 0 )
			s.challengeUrl = p.newChallengeUrl;

		return p;
	}

	// -----------------------------------------------------------------------
	// Helpers
	// -----------------------------------------------------------------------

	private SteamWire.Writer deviceDetails()
	{
		return new SteamWire.Writer()
			.string( 1, deviceName )
			.uint32( 2, PLATFORM_STEAM_CLIENT )
			.uint32( 3, OS_TYPE_LINUX );      // Android is a Linux to Steam
	}

	private static int[] trim( int[] a, int n )
	{
		int[] out = new int[n];
		System.arraycopy( a, 0, out, 0, n );
		return out;
	}

	/**
	 * The steamid64 is the "sub" claim of the refresh token, which is a JWT.
	 * Signature is not verified: this is Steam's own token echoed back to us,
	 * we only need the id it names.
	 */
	public static long steamidFromToken( String jwt )
	{
		if( jwt == null )
			return 0;

		int a = jwt.indexOf( '.' );
		int b = jwt.indexOf( '.', a + 1 );

		if( a < 0 || b < 0 )
			return 0;

		String json = new String( SteamWire.base64Decode( jwt.substring( a + 1, b )));
		int i = json.indexOf( "\"sub\"" );

		if( i < 0 )
			return 0;

		int q1 = json.indexOf( '"', json.indexOf( ':', i ) + 1 );
		int q2 = q1 < 0 ? -1 : json.indexOf( '"', q1 + 1 );

		if( q1 < 0 || q2 < 0 )
			return 0;

		try
		{
			return Long.parseLong( json.substring( q1 + 1, q2 ));
		}
		catch( NumberFormatException e )
		{
			return 0;
		}
	}

	private static String rsaEncrypt( String modHex, String expHex, String password )
		throws IOException
	{
		try
		{
			// Steam sends modulus and exponent as hex strings. The leading-zero
			// BigInteger keeps a high bit from being read as a sign bit.
			BigInteger mod = new BigInteger( modHex, 16 );
			BigInteger exp = new BigInteger( expHex, 16 );
			PublicKey key = KeyFactory.getInstance( "RSA" )
				.generatePublic( new RSAPublicKeySpec( mod, exp ));

			Cipher cipher = Cipher.getInstance( "RSA/ECB/PKCS1Padding" );
			cipher.init( Cipher.ENCRYPT_MODE, key );
			return SteamWire.base64Encode( cipher.doFinal( password.getBytes( "UTF-8" )));
		}
		catch( Exception e )
		{
			throw new IOException( "RSA encryption failed: " + e, e );
		}
	}

	private byte[] post( String method, byte[] body ) throws IOException
	{
		return call( method, body, "POST" );
	}

	private byte[] get( String method, byte[] body ) throws IOException
	{
		return call( method, body, "GET" );
	}

	private byte[] call( String method, byte[] body, String httpMethod )
		throws IOException
	{
		String encoded = URLEncoder.encode( SteamWire.base64Encode( body ), "UTF-8" );
		String form = "input_protobuf_encoded=" + encoded;
		boolean isPost = "POST".equals( httpMethod );
		URL url = new URL( API + method + "/v1/" + ( isPost ? "" : "?" + form ));
		HttpURLConnection conn = (HttpURLConnection)url.openConnection();
		byte[] out;

		try
		{
			conn.setRequestMethod( httpMethod );
			conn.setConnectTimeout( HTTP_TIMEOUT_MS );
			conn.setReadTimeout( HTTP_TIMEOUT_MS );
			conn.setRequestProperty( "User-Agent", "Valve/Steam HTTP Client 1.0" );

			if( isPost )
			{
				conn.setDoOutput( true );
				conn.setRequestProperty( "Content-Type",
					"application/x-www-form-urlencoded" );

				OutputStream os = conn.getOutputStream();

				try
				{
					os.write( form.getBytes( "UTF-8" ));
				}
				finally
				{
					os.close();
				}
			}

			int code = conn.getResponseCode();

			// The protocol-level result lives in a header, not the HTTP status:
			// a rejected login is a perfectly fine HTTP 200.
			int eresult = 0;
			String hdr = conn.getHeaderField( "x-eresult" );

			if( hdr != null )
			{
				try
				{
					eresult = Integer.parseInt( hdr.trim() );
				}
				catch( NumberFormatException ignored ) {}
			}

			if( code != HttpURLConnection.HTTP_OK )
			{
				String msg = conn.getHeaderField( "x-error_message" );
				throw new SteamAuthException( method + ": HTTP " + code
					+ ( msg != null ? " " + msg : "" ), eresult );
			}

			if( eresult != 0 && eresult != 1 )
			{
				String msg = conn.getHeaderField( "x-error_message" );
				throw new SteamAuthException( method + ": rejected"
					+ ( msg != null ? " " + msg : "" ), eresult );
			}

			out = readAll( conn.getInputStream() );
		}
		finally
		{
			conn.disconnect();
		}

		return out;
	}

	private static byte[] readAll( InputStream in ) throws IOException
	{
		ByteArrayOutputStream bos = new ByteArrayOutputStream( 1024 );
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

		return bos.toByteArray();
	}
}
