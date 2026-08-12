/*
cl_sb_scheme_slayer.c - Slayer3D: minimal KeyValues reader for the game's VGUI
                        scheme, used to colour our scoreboard like the rest of
                        the player's install
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

// Only the C library, on purpose: this file is compiled by the harness on the
// host and run against the REAL TrackerScheme.res files from the game directory.
// A parser tested against a paraphrase of its input is not tested.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cl_sb_scheme_slayer.h"

#define SB_TOK_MAX     128
#define SB_COLORS_MAX  64
#define SB_DEPTH_MAX   8

typedef struct
{
	char name[SLAYER_SCHEME_NAME_MAX];
	char value[SB_TOK_MAX];
} sb_named_color_t;

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

// One token: a quoted string, a bare word, or a single `{` / `}`. Comments start
// at `//` and run to the end of the line. Returns 0 at end of input.
//
// Braces have to be their own tokens even when glued to a word, because scheme
// files in the wild are written by hand and `Colors{` does occur.
static int SB_Token( const char **pp, char *out, int size )
{
	const char *p = *pp;
	int len = 0;

	out[0] = '\0';

	for( ;; )
	{
		while( *p && (unsigned char)*p <= ' ' )
			p++;

		if( p[0] == '/' && p[1] == '/' )
		{
			while( *p && *p != '\n' )
				p++;
			continue;
		}

		break;
	}

	if( !*p )
	{
		*pp = p;
		return 0;
	}

	if( *p == '{' || *p == '}' )
	{
		out[0] = *p;
		out[1] = '\0';
		*pp = p + 1;
		return 1;
	}

	if( *p == '"' )
	{
		p++;
		while( *p && *p != '"' )
		{
			if( len + 1 < size )
				out[len++] = *p;
			p++;
		}
		if( *p == '"' )
			p++;
		out[len] = '\0';
		*pp = p;
		return 1;
	}

	while( *p && (unsigned char)*p > ' ' && *p != '{' && *p != '}' && !( p[0] == '/' && p[1] == '/' ))
	{
		if( len + 1 < size )
			out[len++] = *p;
		p++;
	}
	out[len] = '\0';
	*pp = p;
	return 1;
}

static int SB_StrEqualNoCase( const char *a, const char *b )
{
	while( *a && *b )
	{
		int ca = (unsigned char)*a;
		int cb = (unsigned char)*b;

		if( ca >= 'A' && ca <= 'Z' ) ca += 32;
		if( cb >= 'A' && cb <= 'Z' ) cb += 32;
		if( ca != cb )
			return 0;
		a++;
		b++;
	}
	return ( *a == '\0' && *b == '\0' );
}

// ---------------------------------------------------------------------------
// Colour values
// ---------------------------------------------------------------------------

// "62 70 55 255" or "62 70 55" (alpha defaults to opaque, which is what VGUI
// does). Returns 1 on success. Nothing is clamped by the caller, so clamp here.
static int SB_ParseRGBA( const char *s, unsigned char *out )
{
	int v[4] = { 0, 0, 0, 255 };
	int n, i;

	if( !s || !s[0] )
		return 0;

	n = sscanf( s, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3] );
	if( n < 3 )
		return 0;

	for( i = 0; i < 4; i++ )
	{
		if( v[i] < 0 )   v[i] = 0;
		if( v[i] > 255 ) v[i] = 255;
		out[i] = (unsigned char)v[i];
	}
	return 1;
}

// Collect the file's `Colors` section: a table of names that the BaseSettings
// keys refer to ("SelectedBgColor" "Orange"). Without this the scheme reads as
// half-empty, since the interesting keys in the stock file are named rather than
// literal.
static int SB_ReadColors( const char *text, sb_named_color_t *table, int max )
{
	const char *p = text;
	char tok[SB_TOK_MAX];
	char stack[SB_DEPTH_MAX][SLAYER_SCHEME_NAME_MAX];
	int  depth = 0;
	int  count = 0;

	for( ;; )
	{
		const char *save;
		char key[SB_TOK_MAX];

		if( !SB_Token( &p, key, sizeof( key )))
			break;

		if( key[0] == '}' && key[1] == '\0' )
		{
			if( depth > 0 ) depth--;
			continue;
		}
		if( key[0] == '{' && key[1] == '\0' )
			continue;

		save = p;
		if( !SB_Token( &p, tok, sizeof( tok )))
			break;

		if( tok[0] == '{' && tok[1] == '\0' )
		{
			// Section opener: remember its name so pairs inside can be attributed.
			if( depth < SB_DEPTH_MAX )
			{
				strncpy( stack[depth], key, SLAYER_SCHEME_NAME_MAX - 1 );
				stack[depth][SLAYER_SCHEME_NAME_MAX - 1] = '\0';
			}
			depth++;
			continue;
		}

		if( tok[0] == '}' && tok[1] == '\0' )
		{
			// Malformed, but recoverable: rewind so the brace is handled above.
			p = save;
			continue;
		}

		// A pair. Keep it only if the enclosing section is Colors.
		if( depth > 0 && depth <= SB_DEPTH_MAX
		 && SB_StrEqualNoCase( stack[depth - 1], "Colors" )
		 && count < max )
		{
			strncpy( table[count].name, key, SLAYER_SCHEME_NAME_MAX - 1 );
			table[count].name[SLAYER_SCHEME_NAME_MAX - 1] = '\0';
			strncpy( table[count].value, tok, SB_TOK_MAX - 1 );
			table[count].value[SB_TOK_MAX - 1] = '\0';
			count++;
		}
	}

	return count;
}

static int SB_Resolve( const sb_named_color_t *table, int count, const char *value,
	unsigned char *out )
{
	int i;

	if( !value || !value[0] )
		return 0;

	// A literal wins without consulting the table: "0 0 0 128" is not a name.
	if( SB_ParseRGBA( value, out ))
		return 1;

	for( i = 0; i < count; i++ )
	{
		if( SB_StrEqualNoCase( table[i].name, value ))
			return SB_ParseRGBA( table[i].value, out );
	}

	return 0;
}

int Slayer_SBScheme_ResolveValue( const char *text, const char *value,
	unsigned char *out_rgba )
{
	sb_named_color_t table[SB_COLORS_MAX];
	int count;

	if( !out_rgba )
		return 0;

	memset( out_rgba, 0, 4 );

	if( !text )
		return SB_ParseRGBA( value, out_rgba );

	count = SB_ReadColors( text, table, SB_COLORS_MAX );
	return SB_Resolve( table, count, value, out_rgba );
}

// ---------------------------------------------------------------------------
// The keys we care about
// ---------------------------------------------------------------------------

typedef struct
{
	const char  *key;          // full KeyValues key
	unsigned int flag;         // SLAYER_SCHEME_HAS_*
	int          offset;       // byte offset of the field inside slayer_sb_scheme_t
} sb_wanted_t;

#define SB_FIELD( f ) ( (int)( (char *)&( (slayer_sb_scheme_t *)0 )->f - (char *)0 ) )

static const sb_wanted_t sb_wanted[] =
{
	{ "SectionedListPanel.BgColor",                    SLAYER_SCHEME_HAS_BG,            SB_FIELD( bg ) },
	{ "SectionedListPanel.SelectedBgColor",            SLAYER_SCHEME_HAS_SELECTED_BG,   SB_FIELD( selected_bg ) },
	{ "SectionedListPanel.OutOfFocusSelectedBgColor",  SLAYER_SCHEME_HAS_OOF_BG,        SB_FIELD( oof_selected_bg ) },
	{ "SectionedListPanel.HeaderTextColor",            SLAYER_SCHEME_HAS_HEADER_TEXT,   SB_FIELD( header_text ) },
	{ "SectionedListPanel.TextColor",                  SLAYER_SCHEME_HAS_TEXT,          SB_FIELD( text ) },
	{ "SectionedListPanel.BrightTextColor",            SLAYER_SCHEME_HAS_BRIGHT_TEXT,   SB_FIELD( bright_text ) },
	{ "SectionedListPanel.DividerColor",               SLAYER_SCHEME_HAS_DIVIDER,       SB_FIELD( divider ) },
	{ "SectionedListPanel.SelectedTextColor",          SLAYER_SCHEME_HAS_SELECTED_TEXT, SB_FIELD( selected_text ) },
};

#define SB_WANTED_COUNT ( (int)( sizeof( sb_wanted ) / sizeof( sb_wanted[0] )))

int Slayer_SBScheme_Parse( const char *text, slayer_sb_scheme_t *out )
{
	sb_named_color_t table[SB_COLORS_MAX];
	char raw[SB_WANTED_COUNT][SB_TOK_MAX];
	const char *p;
	int  colors;
	int  resolved = 0;
	int  i;

	if( !out )
		return 0;

	memset( out, 0, sizeof( *out ));
	memset( raw, 0, sizeof( raw ));

	if( !text || !text[0] )
		return 0;

	colors = SB_ReadColors( text, table, SB_COLORS_MAX );

	// Values are collected first and resolved afterwards, so the file may list
	// Colors after BaseSettings. Relying on the stock order would work today and
	// break on the first reordered custom scheme.
	p = text;
	for( ;; )
	{
		char key[SB_TOK_MAX];
		char val[SB_TOK_MAX];
		const char *save;

		if( !SB_Token( &p, key, sizeof( key )))
			break;

		if(( key[0] == '{' || key[0] == '}' ) && key[1] == '\0' )
			continue;

		save = p;
		if( !SB_Token( &p, val, sizeof( val )))
			break;

		if( val[0] == '{' && val[1] == '\0' )
			continue;
		if( val[0] == '}' && val[1] == '\0' )
		{
			p = save;
			continue;
		}

		for( i = 0; i < SB_WANTED_COUNT; i++ )
		{
			if( SB_StrEqualNoCase( key, sb_wanted[i].key ))
			{
				strncpy( raw[i], val, SB_TOK_MAX - 1 );
				raw[i][SB_TOK_MAX - 1] = '\0';
				break;
			}
		}
	}

	for( i = 0; i < SB_WANTED_COUNT; i++ )
	{
		unsigned char rgba[4];

		if( !raw[i][0] )
			continue;

		if( !SB_Resolve( table, colors, raw[i], rgba ))
			continue;

		memcpy( (char *)out + sb_wanted[i].offset, rgba, 4 );
		out->have |= sb_wanted[i].flag;
		resolved++;
	}

	return resolved;
}
