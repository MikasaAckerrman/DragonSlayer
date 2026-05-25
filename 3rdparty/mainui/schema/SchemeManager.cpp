/*
SchemeManager.cpp - VGUI2-style scheme manager implementation
Copyright (C) 2024

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "extdll_menu.h"
#include "enginecallback_menu.h"
#include "SchemeManager.h"
#include "Utils.h"
#include "FontManager.h"
#include "BaseMenu.h"

CSchemeManager CSchemeManager::s_instance;

CSchemeManager *CSchemeManager::GetInstance()
{
	return &s_instance;
}

unsigned int CSchemeManager::HashName( const char *name )
{
	// FNV-1a hash, case-insensitive
	unsigned int hash = 2166136261u;
	while( *name )
	{
		char c = *name++;
		if( c >= 'A' && c <= 'Z' ) c += 32; // tolower
		hash ^= (unsigned int)c;
		hash *= 16777619u;
	}
	return hash;
}

bool CSchemeManager::LoadScheme( const char *filename )
{
	m_numColors = 0;
	m_numBorders = 0;
	m_numFonts = 0;

	memset( m_colorHash, -1, sizeof(m_colorHash) );
	memset( m_borderHash, -1, sizeof(m_borderHash) );

	if( !m_keyValues.LoadFromFile( filename ) )
		return false;

	// After loading, the root key is "Scheme"
	// Find sub-sections
	CKeyValues *colors = m_keyValues.FindKey( "Colors" );
	CKeyValues *borders = m_keyValues.FindKey( "Borders" );
	CKeyValues *fonts = m_keyValues.FindKey( "Fonts" );

	if( colors )
		ParseColors( colors );
	if( borders )
		ParseBorders( borders );
	if( fonts )
		ParseFonts( fonts );

	for( int i = 0; i < m_numFonts; i++ )
		m_fonts[i].handle = -1;

	return true;
}

void CSchemeManager::ParseColors( CKeyValues *section )
{
	CKeyValues *child = section->GetFirstSubKey();
	while( child )
	{
		if( m_numColors >= 128 )
			break;

		strncpy( m_colors[m_numColors].name, child->GetName(), 63 );
		m_colors[m_numColors].name[63] = '\0';

		// Parse "R G B A" from the value
		const char *val = child->GetValue();
		int r = 0, g = 0, b = 0, a = 255;
		if( val && val[0] )
			sscanf( val, "%d %d %d %d", &r, &g, &b, &a );

		m_colors[m_numColors].color = PackRGBA( r, g, b, a );

		// Insert into hash table
		unsigned int h = HashName( m_colors[m_numColors].name ) & (COLOR_HASH_SIZE - 1);
		while( m_colorHash[h] != -1 )
			h = (h + 1) & (COLOR_HASH_SIZE - 1);
		m_colorHash[h] = m_numColors;

		m_numColors++;

		child = child->GetNextKey();
	}
}

void CSchemeManager::ParseBorders( CKeyValues *section )
{
	CKeyValues *child = section->GetFirstSubKey();
	while( child )
	{
		if( m_numBorders >= 32 )
			break;

		strncpy( m_borderNames[m_numBorders], child->GetName(), 63 );
		m_borderNames[m_numBorders][63] = '\0';

		ParseSingleBorder( child );

		// Insert into hash table
		unsigned int h = HashName( m_borderNames[m_numBorders] ) & (BORDER_HASH_SIZE - 1);
		while( m_borderHash[h] != -1 )
			h = (h + 1) & (BORDER_HASH_SIZE - 1);
		m_borderHash[h] = m_numBorders;

		m_numBorders++;

		child = child->GetNextKey();
	}
}

CSchemeBorder *CSchemeManager::ParseSingleBorder( CKeyValues *borderKey )
{
	CSchemeBorder *border = &m_borders[m_numBorders];
	border->Clear();

	CKeyValues *sides[4];
	sides[0] = borderKey->FindKey( "Left" );
	sides[1] = borderKey->FindKey( "Right" );
	sides[2] = borderKey->FindKey( "Top" );
	sides[3] = borderKey->FindKey( "Bottom" );

	BorderSide *borderSides[4] = { &border->left, &border->right, &border->top, &border->bottom };

	for( int s = 0; s < 4; s++ )
	{
		if( !sides[s] )
			continue;

		BorderSide *side = borderSides[s];
		side->numLayers = 0;

		// Iterate numbered children "1", "2", "3", "4"
		const char *nums[] = { "1", "2", "3", "4" };
		for( int n = 0; n < 4; n++ )
		{
			CKeyValues *layerKey = sides[s]->FindKey( nums[n] );
			if( !layerKey )
				break;

			// Get color name reference and look it up
			const char *colorName = layerKey->GetString( "color", "" );
			unsigned int color = GetColor( colorName );

			// Get offset string "X Y"
			const char *offsetStr = layerKey->GetString( "offset", "0 0" );
			int offX = 0, offY = 0;
			sscanf( offsetStr, "%d %d", &offX, &offY );

			// Left/Right use X offset, Top/Bottom use Y offset
			int offset = ( s < 2 ) ? offX : offY;

			side->layers[side->numLayers].color = color;
			side->layers[side->numLayers].offset = offset;
			side->numLayers++;
		}
	}

	// Parse inset
	const char *insetStr = borderKey->GetString( "inset", "0 0" );
	sscanf( insetStr, "%d %d", &border->insetX, &border->insetY );

	return border;
}

void CSchemeManager::ParseFonts( CKeyValues *section )
{
	CKeyValues *child = section->GetFirstSubKey();
	while( child )
	{
		if( m_numFonts >= 16 )
			break;

		strncpy( m_fonts[m_numFonts].alias, child->GetName(), 63 );
		m_fonts[m_numFonts].alias[63] = '\0';

		// Take the first numbered sub-key ("1")
		CKeyValues *first = child->FindKey( "1" );
		if( first )
		{
			const char *name = first->GetString( "name", "Tahoma" );
			strncpy( m_fonts[m_numFonts].name, name, 63 );
			m_fonts[m_numFonts].name[63] = '\0';

			m_fonts[m_numFonts].tall = first->GetInt( "tall", 16 );
			m_fonts[m_numFonts].weight = first->GetInt( "weight", 500 );
		}
		else
		{
			strncpy( m_fonts[m_numFonts].name, "Tahoma", 63 );
			m_fonts[m_numFonts].name[63] = '\0';
			m_fonts[m_numFonts].tall = 16;
			m_fonts[m_numFonts].weight = 500;
		}

		m_numFonts++;
		child = child->GetNextKey();
	}
}

unsigned int CSchemeManager::GetColor( const char *colorName )
{
	if( !colorName || !colorName[0] )
		return 0;

	unsigned int h = HashName( colorName ) & (COLOR_HASH_SIZE - 1);
	int idx;
	while( (idx = m_colorHash[h]) != -1 )
	{
		if( stricmp( m_colors[idx].name, colorName ) == 0 )
			return m_colors[idx].color;
		h = (h + 1) & (COLOR_HASH_SIZE - 1);
	}

	return 0;
}

CSchemeBorder *CSchemeManager::GetBorder( const char *borderName )
{
	if( !borderName || !borderName[0] )
		return nullptr;

	unsigned int h = HashName( borderName ) & (BORDER_HASH_SIZE - 1);
	int idx;
	while( (idx = m_borderHash[h]) != -1 )
	{
		if( stricmp( m_borderNames[idx], borderName ) == 0 )
			return &m_borders[idx];
		h = (h + 1) & (BORDER_HASH_SIZE - 1);
	}

	return nullptr;
}

const char *CSchemeManager::GetFontName( const char *alias )
{
	if( !alias || !alias[0] )
		return "";

	for( int i = 0; i < m_numFonts; i++ )
	{
		if( stricmp( m_fonts[i].alias, alias ) == 0 )
			return m_fonts[i].name;
	}

	return "";
}

int CSchemeManager::GetFontTall( const char *alias )
{
	if( !alias || !alias[0] )
		return 16;

	for( int i = 0; i < m_numFonts; i++ )
	{
		if( stricmp( m_fonts[i].alias, alias ) == 0 )
			return m_fonts[i].tall;
	}

	return 16;
}

int CSchemeManager::GetFontWeight( const char *alias )
{
	if( !alias || !alias[0] )
		return 500;

	for( int i = 0; i < m_numFonts; i++ )
	{
		if( stricmp( m_fonts[i].alias, alias ) == 0 )
			return m_fonts[i].weight;
	}

	return 500;
}

void CSchemeManager::CreateFonts()
{
	for( int i = 0; i < m_numFonts; i++ )
	{
		int scaledTall = (int)( m_fonts[i].tall * uiStatic.scaleY );
		HFont h = CFontBuilder( m_fonts[i].name, scaledTall, m_fonts[i].weight ).Create();
		m_fonts[i].handle = h;
		Con_Printf( "SchemeManager: created font '%s' (face=%s tall=%d weight=%d) -> handle %d\n",
			m_fonts[i].alias, m_fonts[i].name, scaledTall, m_fonts[i].weight, h );
	}
}

HFont CSchemeManager::GetFont( const char *alias )
{
	if( !alias || !alias[0] )
		return -1;

	for( int i = 0; i < m_numFonts; i++ )
	{
		if( stricmp( m_fonts[i].alias, alias ) == 0 )
			return m_fonts[i].handle > 0 ? m_fonts[i].handle : -1;
	}

	return -1;
}
