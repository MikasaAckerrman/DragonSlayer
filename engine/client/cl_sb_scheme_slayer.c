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
	int          prio;         // SB_PRIO_*, higher wins when two keys feed one field
} sb_wanted_t;

#define SB_FIELD( f ) ( (int)( (char *)&( (slayer_sb_scheme_t *)0 )->f - (char *)0 ) )

// TWO FAMILIES OF KEYS, AND WHY BOTH ARE NEEDED
//
// The board's colours were originally taken from `TrackerScheme.res`, whose
// `SectionedListPanel.*` keys do describe a SectionedListPanel -- but that file
// is the Tracker/Friends scheme, and its palette is OLIVE (`Orange` there is
// "142 137 35", Button.BgColor "76 88 68"). That is not what a CS 1.6 scoreboard
// looks like, and the player who asked for "colours from the game" meant the
// game's board, not the buddy list.
//
// The board's real scheme is `resource/ClientScheme.res`, where the relevant
// entries are named for their purpose rather than for a widget class:
//
//     "ListBG"       "0 0 0 128"       // the file's own comment: "background of scoreboard"
//     "BaseText"     "255 176 0 255"   // amber, "used in text windows, lists"
//     "SelectionBG"  "10 10 10 100"    // near-black selection, not an olive bar
//     "BorderBright" "188 112 0 128"
//
// Measured against the game's own client library, that is the right family: the
// stock board fills "0 0 0 153" and prints "255 140 0". Amber on near-black.
//
// Both families are therefore read, and `prio` decides when a file defines both:
// an explicit `SectionedListPanel.*` key is about this exact widget and wins over
// a general palette name. A custom TrackerScheme keeps working unchanged, and
// ClientScheme now works too.
#define SB_PRIO_PALETTE  0     // general colour names (ClientScheme)
#define SB_PRIO_WIDGET   1     // SectionedListPanel.* (TrackerScheme)

static const sb_wanted_t sb_wanted[] =
{
	{ "SectionedListPanel.BgColor",                    SLAYER_SCHEME_HAS_BG,            SB_FIELD( bg ),              SB_PRIO_WIDGET },
	{ "SectionedListPanel.SelectedBgColor",            SLAYER_SCHEME_HAS_SELECTED_BG,   SB_FIELD( selected_bg ),     SB_PRIO_WIDGET },
	{ "SectionedListPanel.OutOfFocusSelectedBgColor",  SLAYER_SCHEME_HAS_OOF_BG,        SB_FIELD( oof_selected_bg ), SB_PRIO_WIDGET },
	{ "SectionedListPanel.HeaderTextColor",            SLAYER_SCHEME_HAS_HEADER_TEXT,   SB_FIELD( header_text ),     SB_PRIO_WIDGET },
	{ "SectionedListPanel.TextColor",                  SLAYER_SCHEME_HAS_TEXT,          SB_FIELD( text ),            SB_PRIO_WIDGET },
	{ "SectionedListPanel.BrightTextColor",            SLAYER_SCHEME_HAS_BRIGHT_TEXT,   SB_FIELD( bright_text ),     SB_PRIO_WIDGET },
	{ "SectionedListPanel.DividerColor",               SLAYER_SCHEME_HAS_DIVIDER,       SB_FIELD( divider ),         SB_PRIO_WIDGET },
	{ "SectionedListPanel.SelectedTextColor",          SLAYER_SCHEME_HAS_SELECTED_TEXT, SB_FIELD( selected_text ),   SB_PRIO_WIDGET },

	// ClientScheme.res -- the vanilla CS 1.6 board palette.
	//
	// TEXT COLOURS ARE DELIBERATELY NOT TAKEN FROM PALETTE NAMES. They were, and
	// it was wrong in a way only the player's own file makes obvious:
	//
	//     "BaseText"     "255 176 0 255"   // "used in text windows, lists"
	//     "BorderBright" "188 112 0 128"   // "the lit side of a control"
	//
	// BaseText is a general text colour for every list in the game, and BorderBright
	// is a BORDER colour -- mapping it onto the column labels was an assumption, and
	// it produced dull brown headers on a dark panel. Because a cvar only loses to
	// the file while it still holds its compiled-in default, the board's white text
	// was being replaced by amber for every player who had never touched the cvar,
	// with no way to tell that a file was responsible.
	//
	// Widget-specific keys stay (see above): `SectionedListPanel.TextColor` and
	// `.HeaderTextColor` name THIS widget, so a themed scheme still wins. Only the
	// guesses are gone.
	{ "ListBG",                                        SLAYER_SCHEME_HAS_BG,            SB_FIELD( bg ),              SB_PRIO_PALETTE },
	{ "SelectionBG",                                   SLAYER_SCHEME_HAS_SELECTED_BG,   SB_FIELD( selected_bg ),     SB_PRIO_PALETTE },
	{ "SelectionBG2",                                  SLAYER_SCHEME_HAS_OOF_BG,        SB_FIELD( oof_selected_bg ), SB_PRIO_PALETTE },
	{ "BrightBaseText",                                SLAYER_SCHEME_HAS_BRIGHT_TEXT,   SB_FIELD( bright_text ),     SB_PRIO_PALETTE },
	{ "BorderDark",                                    SLAYER_SCHEME_HAS_DIVIDER,       SB_FIELD( divider ),         SB_PRIO_PALETTE },
	{ "SelectedText",                                  SLAYER_SCHEME_HAS_SELECTED_TEXT, SB_FIELD( selected_text ),   SB_PRIO_PALETTE },
};

#define SB_WANTED_COUNT ( (int)( sizeof( sb_wanted ) / sizeof( sb_wanted[0] )))

int Slayer_SBScheme_Parse( const char *text, slayer_sb_scheme_t *out )
{
	sb_named_color_t table[SB_COLORS_MAX];
	char raw[SB_WANTED_COUNT][SB_TOK_MAX];
	char section[SB_DEPTH_MAX][SLAYER_SCHEME_NAME_MAX];
	const char *p;
	int  colors;
	int  resolved = 0;
	int  depth;
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
	//
	// The enclosing section is tracked because the two key families live in
	// different places: `SectionedListPanel.*` are BaseSettings entries, while the
	// palette names are entries of `Colors`. Matching a palette name anywhere
	// would also match the many places it appears as a VALUE ("ListBgColor"
	// "ListBG"), and worse, `Borders` in ClientScheme.res repeats "BorderBright"
	// dozens of times as the value of "color".
	p = text;
	depth = 0;
	for( ;; )
	{
		char key[SB_TOK_MAX];
		char val[SB_TOK_MAX];
		const char *save;
		int  in_colors;

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
		if( !SB_Token( &p, val, sizeof( val )))
			break;

		if( val[0] == '{' && val[1] == '\0' )
		{
			if( depth < SB_DEPTH_MAX )
			{
				strncpy( section[depth], key, SLAYER_SCHEME_NAME_MAX - 1 );
				section[depth][SLAYER_SCHEME_NAME_MAX - 1] = '\0';
			}
			depth++;
			continue;
		}
		if( val[0] == '}' && val[1] == '\0' )
		{
			p = save;
			continue;
		}

		in_colors = ( depth > 0 && depth <= SB_DEPTH_MAX
			&& SB_StrEqualNoCase( section[depth - 1], "Colors" ));

		for( i = 0; i < SB_WANTED_COUNT; i++ )
		{
			if( sb_wanted[i].prio == SB_PRIO_PALETTE && !in_colors )
				continue;

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
		int j;
		int shadowed = 0;

		if( !raw[i][0] )
			continue;

		// Two keys can feed the same field (a widget key and a palette name).
		// Skip this one if a higher-priority key for the same field was also
		// present, so a file carrying both is read the way its author meant.
		for( j = 0; j < SB_WANTED_COUNT; j++ )
		{
			if( j == i )
				continue;
			if( sb_wanted[j].offset != sb_wanted[i].offset )
				continue;
			if( raw[j][0] && sb_wanted[j].prio > sb_wanted[i].prio )
			{
				shadowed = 1;
				break;
			}
		}
		if( shadowed )
			continue;

		if( !SB_Resolve( table, colors, raw[i], rgba ))
			continue;

		memcpy( (char *)out + sb_wanted[i].offset, rgba, 4 );
		out->have |= sb_wanted[i].flag;
		resolved++;
	}

	return resolved;
}
