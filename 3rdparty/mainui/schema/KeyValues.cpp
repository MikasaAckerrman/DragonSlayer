/*
KeyValues.cpp - Valve KeyValues (.res) file parser implementation
Copyright (C) 2024

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "extdll_menu.h"
#include "enginecallback_menu.h"
#include "KeyValues.h"

CKeyValues::CKeyValues( const char *name )
{
	m_szName[0] = '\0';
	m_szValue[0] = '\0';
	m_pFirstChild = nullptr;
	m_pNextSibling = nullptr;

	if( name )
		strncpy( m_szName, name, MAX_NAME_LENGTH - 1 );
	m_szName[MAX_NAME_LENGTH - 1] = '\0';
}

CKeyValues::~CKeyValues()
{
	// recursively delete children
	CKeyValues *child = m_pFirstChild;
	while( child )
	{
		CKeyValues *next = child->m_pNextSibling;
		delete child;
		child = next;
	}
}

bool CKeyValues::LoadFromFile( const char *filename )
{
	int length = 0;
	byte *buffer = EngFuncs::COM_LoadFile( filename, &length );

	if( !buffer )
		return false;

	bool result = ParseBuffer( (const char *)buffer, length );

	EngFuncs::COM_FreeFile( buffer );
	return result;
}

bool CKeyValues::ParseBuffer( const char *buffer, int length )
{
	// clean up any existing children
	CKeyValues *child = m_pFirstChild;
	while( child )
	{
		CKeyValues *next = child->m_pNextSibling;
		delete child;
		child = next;
	}
	m_pFirstChild = nullptr;

	if( !buffer || length <= 0 )
		return false;

	const char *buf = buffer;
	const char *end = buffer + length;

	// read the root key name
	char token[MAX_VALUE_LENGTH];
	buf = SkipWhitespaceAndComments( buf, end );
	if( !buf )
		return false;

	buf = ReadToken( buf, end, token, sizeof( token ) );
	if( !buf )
		return false;

	strncpy( m_szName, token, MAX_NAME_LENGTH - 1 );
	m_szName[MAX_NAME_LENGTH - 1] = '\0';

	// expect opening brace, then delegate to ParseSection
	buf = SkipWhitespaceAndComments( buf, end );
	if( !buf || buf >= end || *buf != '{' )
		return false;

	ParseSection( buf, end );

	return true;
}

CKeyValues *CKeyValues::ParseSection( const char *&buf, const char *end )
{
	// buf should point at '{'
	if( !buf || buf >= end || *buf != '{' )
		return nullptr;

	buf++; // skip '{'

	CKeyValues *lastChild = nullptr;

	while( buf && buf < end )
	{
		buf = SkipWhitespaceAndComments( buf, end );
		if( !buf || buf >= end )
			break;

		if( *buf == '}' )
		{
			buf++;
			break;
		}

		char token[MAX_VALUE_LENGTH];
		buf = ReadToken( buf, end, token, sizeof( token ) );
		if( !buf )
			break;

		CKeyValues *subKey = new CKeyValues( token );

		const char *peek = SkipWhitespaceAndComments( buf, end );
		if( peek && peek < end && *peek == '{' )
		{
			buf = peek;
			subKey->ParseSection( buf, end );
		}
		else
		{
			char valToken[MAX_VALUE_LENGTH];
			buf = ReadToken( peek, end, valToken, sizeof( valToken ) );
			if( buf )
			{
				strncpy( subKey->m_szValue, valToken, MAX_VALUE_LENGTH - 1 );
				subKey->m_szValue[MAX_VALUE_LENGTH - 1] = '\0';
			}
		}

		if( !lastChild )
			m_pFirstChild = subKey;
		else
			lastChild->m_pNextSibling = subKey;
		lastChild = subKey;
	}

	return this;
}

CKeyValues *CKeyValues::FindKey( const char *name )
{
	if( !name )
		return nullptr;

	CKeyValues *child = m_pFirstChild;
	while( child )
	{
		if( stricmp( child->m_szName, name ) == 0 )
			return child;
		child = child->m_pNextSibling;
	}

	return nullptr;
}

const char *CKeyValues::GetString( const char *key, const char *defaultValue )
{
	CKeyValues *found = FindKey( key );
	if( found && found->m_szValue[0] )
		return found->m_szValue;
	return defaultValue;
}

int CKeyValues::GetInt( const char *key, int defaultValue )
{
	const char *str = GetString( key, nullptr );
	if( !str || !str[0] )
		return defaultValue;
	return atoi( str );
}

float CKeyValues::GetFloat( const char *key, float defaultValue )
{
	const char *str = GetString( key, nullptr );
	if( !str || !str[0] )
		return defaultValue;
	return (float)atof( str );
}

unsigned int CKeyValues::GetColor( const char *key )
{
	const char *str = GetString( key, nullptr );
	if( !str || !str[0] )
		return 0;

	int r = 0, g = 0, b = 0, a = 255;
	sscanf( str, "%d %d %d %d", &r, &g, &b, &a );

	// pack as RGBA matching CColor layout: a(byte3) r(byte2) g(byte1) b(byte0)
	unsigned int color = ( (unsigned int)(a & 0xFF) << 24 )
	                   | ( (unsigned int)(r & 0xFF) << 16 )
	                   | ( (unsigned int)(g & 0xFF) << 8 )
	                   | ( (unsigned int)(b & 0xFF) );
	return color;
}

CKeyValues *CKeyValues::GetFirstSubKey()
{
	return m_pFirstChild;
}

CKeyValues *CKeyValues::GetNextKey()
{
	return m_pNextSibling;
}

const char *CKeyValues::GetName() const
{
	return m_szName;
}

const char *CKeyValues::GetValue() const
{
	return m_szValue;
}

const char *CKeyValues::SkipWhitespaceAndComments( const char *buf, const char *end )
{
	if( !buf )
		return nullptr;

	while( buf < end )
	{
		// skip whitespace
		if( *buf == ' ' || *buf == '\t' || *buf == '\r' || *buf == '\n' )
		{
			buf++;
			continue;
		}

		// skip // comments
		if( buf + 1 < end && buf[0] == '/' && buf[1] == '/' )
		{
			buf += 2;
			while( buf < end && *buf != '\n' )
				buf++;
			continue;
		}

		// not whitespace or comment
		break;
	}

	if( buf >= end )
		return nullptr;

	return buf;
}

const char *CKeyValues::ReadToken( const char *buf, const char *end, char *token, int tokenSize )
{
	if( !buf || buf >= end || tokenSize <= 0 )
	{
		if( token && tokenSize > 0 )
			token[0] = '\0';
		return nullptr;
	}

	buf = SkipWhitespaceAndComments( buf, end );
	if( !buf )
	{
		token[0] = '\0';
		return nullptr;
	}

	int i = 0;

	if( *buf == '"' )
	{
		// quoted string
		buf++; // skip opening quote
		while( buf < end && *buf != '"' )
		{
			if( *buf == '\\' && buf + 1 < end )
			{
				buf++;
				char c;
				switch( *buf )
				{
				case 'n': c = '\n'; break;
				case 't': c = '\t'; break;
				case '"': c = '"'; break;
				case '\\': c = '\\'; break;
				default: c = *buf; break;
				}
				if( i < tokenSize - 1 )
					token[i++] = c;
				buf++;
			}
			else
			{
				if( i < tokenSize - 1 )
					token[i++] = *buf;
				buf++;
			}
		}
		if( buf < end && *buf == '"' )
			buf++; // skip closing quote
	}
	else
	{
		// unquoted token - delimited by whitespace or braces
		while( buf < end && *buf != ' ' && *buf != '\t' && *buf != '\r' && *buf != '\n'
			&& *buf != '{' && *buf != '}' && *buf != '"' )
		{
			if( i < tokenSize - 1 )
				token[i++] = *buf;
			buf++;
		}
	}

	token[i] = '\0';
	return buf;
}
