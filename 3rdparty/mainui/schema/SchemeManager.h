/*
SchemeManager.h - VGUI2-style scheme manager
Copyright (C) 2024

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#pragma once
#ifndef SCHEMEMANAGER_H
#define SCHEMEMANAGER_H

#include "KeyValues.h"
#include "SchemeBorder.h"
#include "FontRenderer.h"

class CSchemeManager
{
public:
	bool LoadScheme( const char *filename );

	void CreateFonts();
	HFont GetFont( const char *alias );

	unsigned int GetColor( const char *colorName );
	CSchemeBorder *GetBorder( const char *borderName );
	const char *GetFontName( const char *alias );
	int GetFontTall( const char *alias );
	int GetFontWeight( const char *alias );

	static CSchemeManager *GetInstance();

private:
	void ParseColors( CKeyValues *section );
	void ParseBorders( CKeyValues *section );
	void ParseFonts( CKeyValues *section );
	CSchemeBorder *ParseSingleBorder( CKeyValues *borderKey );

	// Fast lookup hash tables
	static unsigned int HashName( const char *name );

	// Color hash table (open addressing, size must be power of 2)
	enum { COLOR_HASH_SIZE = 256 }; // 2x max colors for low collision
	int m_colorHash[COLOR_HASH_SIZE]; // index into m_colors[], -1 = empty

	// Border hash table
	enum { BORDER_HASH_SIZE = 64 }; // 2x max borders
	int m_borderHash[BORDER_HASH_SIZE]; // index into m_borders[], -1 = empty

	struct ColorEntry { char name[64]; unsigned int color; };
	struct FontEntry { char alias[64]; char name[64]; int tall; int weight; HFont handle; };

	ColorEntry m_colors[128];
	int m_numColors;

	CSchemeBorder m_borders[32];
	char m_borderNames[32][64];
	int m_numBorders;

	FontEntry m_fonts[16];
	int m_numFonts;

	CKeyValues m_keyValues;
	static CSchemeManager s_instance;
};

#endif // SCHEMEMANAGER_H
