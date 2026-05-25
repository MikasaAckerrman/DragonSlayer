/*
KeyValues.h - Valve KeyValues (.res) file parser
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

#pragma once
#ifndef KEYVALUES_H
#define KEYVALUES_H

class CKeyValues
{
public:
	CKeyValues( const char *name = "" );
	~CKeyValues();

	bool LoadFromFile( const char *filename );
	bool ParseBuffer( const char *buffer, int length );

	CKeyValues *FindKey( const char *name );
	const char *GetString( const char *key, const char *defaultValue = "" );
	int GetInt( const char *key, int defaultValue = 0 );
	float GetFloat( const char *key, float defaultValue = 0.0f );
	unsigned int GetColor( const char *key ); // parse "R G B A" into packed RGBA uint

	CKeyValues *GetFirstSubKey();
	CKeyValues *GetNextKey();

	const char *GetName() const;
	const char *GetValue() const;

private:
#ifdef MY_COMPILER_SUCKS
	CKeyValues( const CKeyValues & );
	CKeyValues &operator=( const CKeyValues & );
#else
	CKeyValues( const CKeyValues & ) = delete;
	CKeyValues &operator=( const CKeyValues & ) = delete;
#endif

	enum { MAX_NAME_LENGTH = 128, MAX_VALUE_LENGTH = 256 };

	char m_szName[MAX_NAME_LENGTH];
	char m_szValue[MAX_VALUE_LENGTH];
	CKeyValues *m_pFirstChild;
	CKeyValues *m_pNextSibling;

	// internal parser helpers
	static const char *SkipWhitespaceAndComments( const char *buf, const char *end );
	static const char *ReadToken( const char *buf, const char *end, char *token, int tokenSize );
	CKeyValues *ParseSection( const char *&buf, const char *end );
};

#endif // KEYVALUES_H
