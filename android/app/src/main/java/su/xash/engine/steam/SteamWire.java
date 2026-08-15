/*
SteamWire.java - protobuf wire format + base64, hand-rolled.
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Why hand-rolled instead of protobuf-javalite:

The launcher needs exactly six Steam messages, all of them a handful of
scalar fields. Pulling in protobuf-javalite means a codegen plugin, .proto
sources and a new resolution step in a build that is our only channel for
producing an APK. The wire format is a tag/value stream; encoding it by hand
is less code than the build wiring would be.

Deliberately no android.* imports: this class and everything above it in the
Steam stack must be runnable on a desktop JVM so the protocol can be tested
without a device (see tests/SteamProtoTest.java).

Base64 lives here too, because java.util.Base64 is API 26 and the launcher
ships minSdk 21, while android.util.Base64 would drag Android into the test.
*/

package su.xash.engine.steam;

import java.io.ByteArrayOutputStream;
import java.io.UnsupportedEncodingException;

public final class SteamWire
{
	private SteamWire() {}

	public static final int WIRE_VARINT = 0;
	public static final int WIRE_FIXED64 = 1;
	public static final int WIRE_BYTES = 2;
	public static final int WIRE_FIXED32 = 5;

	private static byte[] utf8( String s )
	{
		try
		{
			return s.getBytes( "UTF-8" );
		}
		catch( UnsupportedEncodingException e )
		{
			throw new RuntimeException( e ); // UTF-8 is mandatory in every JVM
		}
	}

	private static String fromUtf8( byte[] b, int off, int len )
	{
		try
		{
			return new String( b, off, len, "UTF-8" );
		}
		catch( UnsupportedEncodingException e )
		{
			throw new RuntimeException( e );
		}
	}

	// -----------------------------------------------------------------------
	// Writer
	// -----------------------------------------------------------------------

	public static final class Writer
	{
		private final ByteArrayOutputStream out = new ByteArrayOutputStream( 128 );

		private void rawVarint( long v )
		{
			// Treated as unsigned 64-bit: a negative int32 sign-extends to ten
			// bytes, which is what protobuf mandates for the int32 type.
			while( true )
			{
				int b = (int)( v & 0x7F );
				v >>>= 7;

				if( v == 0 )
				{
					out.write( b );
					return;
				}

				out.write( b | 0x80 );
			}
		}

		private void tag( int field, int wire )
		{
			rawVarint( ( (long)field << 3 ) | wire );
		}

		public Writer varint( int field, long value )
		{
			tag( field, WIRE_VARINT );
			rawVarint( value );
			return this;
		}

		/** uint32 field. Value is masked, so callers may pass negative enums. */
		public Writer uint32( int field, int value )
		{
			return varint( field, value & 0xFFFFFFFFL );
		}

		/** int32 field. Negative values sign-extend, as protobuf requires. */
		public Writer int32( int field, int value )
		{
			return varint( field, (long)value );
		}

		public Writer bool( int field, boolean value )
		{
			return varint( field, value ? 1 : 0 );
		}

		/**
		 * uint64 field.
		 *
		 * rawVarint already treats its argument as unsigned 64-bit, so this is
		 * varint() under a name that says what the schema calls the field. Worth
		 * having explicitly: a SteamID passed through int32() would sign-extend to
		 * ten bytes and decode as a different number.
		 */
		public Writer uint64( int field, long value )
		{
			return varint( field, value );
		}

		public Writer fixed32( int field, int value )
		{
			tag( field, WIRE_FIXED32 );
			out.write( value & 0xFF );
			out.write( ( value >>> 8 ) & 0xFF );
			out.write( ( value >>> 16 ) & 0xFF );
			out.write( ( value >>> 24 ) & 0xFF );
			return this;
		}

		public Writer fixed64( int field, long value )
		{
			tag( field, WIRE_FIXED64 );

			for( int i = 0; i < 8; i++ )
				out.write( (int)( ( value >>> ( i * 8 ) ) & 0xFF ) );

			return this;
		}

		public Writer bytes( int field, byte[] value )
		{
			tag( field, WIRE_BYTES );
			rawVarint( value.length );
			out.write( value, 0, value.length );
			return this;
		}

		public Writer string( int field, String value )
		{
			return bytes( field, utf8( value ) );
		}

		public Writer message( int field, Writer sub )
		{
			return bytes( field, sub.toBytes() );
		}

		public byte[] toBytes()
		{
			return out.toByteArray();
		}

		public int size()
		{
			return out.size();
		}
	}

	// -----------------------------------------------------------------------
	// Reader
	// -----------------------------------------------------------------------

	/**
	 * Field-at-a-time reader. Usage:
	 * <pre>
	 * Reader r = new Reader( data );
	 * while( r.next() )
	 * {
	 *     switch( r.field() )
	 *     {
	 *     case 1: id = r.varint(); break;
	 *     default: r.skip(); break;
	 *     }
	 * }
	 * </pre>
	 * Every branch must consume exactly one value, and unknown fields must go
	 * to skip(); forgetting either desynchronises the stream.
	 */
	public static final class Reader
	{
		private final byte[] buf;
		private int pos;
		private final int end;

		private int field;
		private int wire;

		public Reader( byte[] data )
		{
			this( data, 0, data.length );
		}

		public Reader( byte[] data, int off, int len )
		{
			this.buf = data;
			this.pos = off;
			this.end = off + len;
		}

		public boolean next()
		{
			if( pos >= end )
				return false;

			long tag = varint();
			field = (int)( tag >>> 3 );
			wire = (int)( tag & 7 );
			return true;
		}

		public int field()
		{
			return field;
		}

		public int wire()
		{
			return wire;
		}

		public long varint()
		{
			long result = 0;
			int shift = 0;

			while( shift < 64 )
			{
				if( pos >= end )
					throw new IllegalStateException( "truncated varint" );

				int b = buf[pos++] & 0xFF;
				result |= (long)( b & 0x7F ) << shift;

				if(( b & 0x80 ) == 0 )
					return result;

				shift += 7;
			}

			throw new IllegalStateException( "varint too long" );
		}

		public int int32()
		{
			return (int)varint();
		}

		public boolean bool()
		{
			return varint() != 0;
		}

		public int fixed32()
		{
			if( pos + 4 > end )
				throw new IllegalStateException( "truncated fixed32" );

			int v = ( buf[pos] & 0xFF )
				| (( buf[pos + 1] & 0xFF ) << 8 )
				| (( buf[pos + 2] & 0xFF ) << 16 )
				| (( buf[pos + 3] & 0xFF ) << 24 );
			pos += 4;
			return v;
		}

		public long fixed64()
		{
			if( pos + 8 > end )
				throw new IllegalStateException( "truncated fixed64" );

			long v = 0;

			for( int i = 0; i < 8; i++ )
				v |= (long)( buf[pos + i] & 0xFF ) << ( i * 8 );

			pos += 8;
			return v;
		}

		public float float32()
		{
			return Float.intBitsToFloat( fixed32() );
		}

		public byte[] bytes()
		{
			int len = (int)varint();

			if( len < 0 || pos + len > end )
				throw new IllegalStateException( "truncated bytes" );

			byte[] out = new byte[len];
			System.arraycopy( buf, pos, out, 0, len );
			pos += len;
			return out;
		}

		public String string()
		{
			int len = (int)varint();

			if( len < 0 || pos + len > end )
				throw new IllegalStateException( "truncated string" );

			String s = fromUtf8( buf, pos, len );
			pos += len;
			return s;
		}

		/** Sub-message as a nested reader, without copying. */
		public Reader message()
		{
			int len = (int)varint();

			if( len < 0 || pos + len > end )
				throw new IllegalStateException( "truncated message" );

			Reader sub = new Reader( buf, pos, len );
			pos += len;
			return sub;
		}

		public void skip()
		{
			switch( wire )
			{
			case WIRE_VARINT:
				varint();
				break;
			case WIRE_FIXED64:
				fixed64();
				break;
			case WIRE_BYTES:
				bytes();
				break;
			case WIRE_FIXED32:
				fixed32();
				break;
			default:
				throw new IllegalStateException( "unknown wire type " + wire );
			}
		}
	}

	// -----------------------------------------------------------------------
	// Base64 (URL-safe variant included: JWT payloads use it)
	// -----------------------------------------------------------------------

	private static final String B64 =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	public static String base64Encode( byte[] data )
	{
		StringBuilder sb = new StringBuilder((( data.length + 2 ) / 3 ) * 4 );

		for( int i = 0; i < data.length; i += 3 )
		{
			int b0 = data[i] & 0xFF;
			int b1 = i + 1 < data.length ? data[i + 1] & 0xFF : 0;
			int b2 = i + 2 < data.length ? data[i + 2] & 0xFF : 0;

			sb.append( B64.charAt( b0 >>> 2 ));
			sb.append( B64.charAt((( b0 & 0x03 ) << 4 ) | ( b1 >>> 4 )));
			sb.append( i + 1 < data.length ? B64.charAt((( b1 & 0x0F ) << 2 ) | ( b2 >>> 6 )) : '=' );
			sb.append( i + 2 < data.length ? B64.charAt( b2 & 0x3F ) : '=' );
		}

		return sb.toString();
	}

	public static byte[] base64Decode( String s )
	{
		ByteArrayOutputStream out = new ByteArrayOutputStream( s.length() * 3 / 4 + 3 );
		int acc = 0, bits = 0;

		for( int i = 0; i < s.length(); i++ )
		{
			char c = s.charAt( i );
			int v;

			if( c == '-' ) v = 62;              // URL-safe alias for '+'
			else if( c == '_' ) v = 63;         // URL-safe alias for '/'
			else v = B64.indexOf( c );

			if( v < 0 )
				continue;                       // '=' padding, newlines, spaces

			acc = ( acc << 6 ) | v;
			bits += 6;

			if( bits >= 8 )
			{
				bits -= 8;
				out.write(( acc >>> bits ) & 0xFF );
			}
		}

		return out.toByteArray();
	}

	public static String hex( byte[] data )
	{
		StringBuilder sb = new StringBuilder( data.length * 2 );

		for( byte b : data )
		{
			sb.append( Character.forDigit(( b >>> 4 ) & 0xF, 16 ));
			sb.append( Character.forDigit( b & 0xF, 16 ));
		}

		return sb.toString();
	}

	public static byte[] unhex( String s )
	{
		int n = s.length() / 2;
		byte[] out = new byte[n];

		for( int i = 0; i < n; i++ )
		{
			out[i] = (byte)(( Character.digit( s.charAt( i * 2 ), 16 ) << 4 )
				| Character.digit( s.charAt( i * 2 + 1 ), 16 ));
		}

		return out;
	}
}
