/*
cl_sb_scheme_slayer.h - Slayer3D: colours for the scoreboard read from the game's
                        own VGUI scheme file
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
#ifndef CL_SB_SCHEME_SLAYER_H
#define CL_SB_SCHEME_SLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

// WHY THIS EXISTS
//
// The scoreboard in CS 1.6 is a VGUI `SectionedListPanel`, and its colours come
// from `resource/TrackerScheme.res` in the game directory -- background, the
// highlight on your own row, the header text, the divider. A player who has
// themed their game (or installed a config that did) expects our board to look
// like the rest of it, and re-entering those colours as cvars by hand is not a
// reasonable thing to ask.
//
// So the file is read and used as the DEFAULTS. Cvars remain the override: if a
// cvar still holds its built-in default value the scheme wins, and the moment
// the player sets it explicitly their value wins. That ordering matters, because
// FCVAR_ARCHIVE means a cvar the player once touched is indistinguishable from
// one they set today -- comparing against `def_string` is what separates "never
// configured" from "configured".
//
// The parser is deliberately in its own file with no engine headers, so it can
// be compiled and asserted on the host against the real KeyValues files shipped
// with the game rather than against a paraphrase of them.

#define SLAYER_SCHEME_NAME_MAX  64

// Which keys were actually present. A missing key must keep our own default
// rather than turning black, so every consumer has to be able to tell "the file
// said 0 0 0" from "the file did not say".
#define SLAYER_SCHEME_HAS_BG            ( 1 << 0 )
#define SLAYER_SCHEME_HAS_SELECTED_BG   ( 1 << 1 )
#define SLAYER_SCHEME_HAS_OOF_BG        ( 1 << 2 )
#define SLAYER_SCHEME_HAS_HEADER_TEXT   ( 1 << 3 )
#define SLAYER_SCHEME_HAS_TEXT          ( 1 << 4 )
#define SLAYER_SCHEME_HAS_BRIGHT_TEXT   ( 1 << 5 )
#define SLAYER_SCHEME_HAS_DIVIDER       ( 1 << 6 )
#define SLAYER_SCHEME_HAS_SELECTED_TEXT ( 1 << 7 )

typedef struct
{
	unsigned int have;          // SLAYER_SCHEME_HAS_* bitmask
	unsigned char bg[4];
	unsigned char selected_bg[4];
	unsigned char oof_selected_bg[4];
	unsigned char header_text[4];
	unsigned char text[4];
	unsigned char bright_text[4];
	unsigned char divider[4];
	unsigned char selected_text[4];
} slayer_sb_scheme_t;

// Parse a VGUI scheme file held in memory. `text` is NUL-terminated and is NOT
// modified. Returns the number of SectionedListPanel keys resolved; `out` is
// fully zeroed first, so a failed parse leaves have == 0 and every consumer
// falls back to its own defaults.
int Slayer_SBScheme_Parse( const char *text, slayer_sb_scheme_t *out );

// Resolve one "R G B A" / "R G B" / named-colour value using the file's own
// `Colors` section. Exposed for the harness: named colours dereferencing
// correctly is the part most likely to break, and it is worth asserting on its
// own rather than only through the aggregate above.
int Slayer_SBScheme_ResolveValue( const char *text, const char *value,
	unsigned char *out_rgba );

#ifdef __cplusplus
}
#endif

#endif // CL_SB_SCHEME_SLAYER_H
