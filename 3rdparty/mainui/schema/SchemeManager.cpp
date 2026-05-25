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

CSchemeManager CSchemeManager::s_instance;

CSchemeManager *CSchemeManager::GetInstance()
{
	return &s_instance;
}

bool CSchemeManager::LoadScheme( const char *filename )
{
	m_numColors = 0;
	m_numBorders = 0;
	m_numFonts = 0;

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

	for( int i = 0; i < m_numColors; i++ )
	{
		if( stricmp( m_colors[i].name, colorName ) == 0 )
			return m_colors[i].color;
	}

	return 0;
}

CSchemeBorder *CSchemeManager::GetBorder( const char *borderName )
{
	if( !borderName || !borderName[0] )
		return nullptr;

	for( int i = 0; i < m_numBorders; i++ )
	{
		if( stricmp( m_borderNames[i], borderName ) == 0 )
			return &m_borders[i];
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
