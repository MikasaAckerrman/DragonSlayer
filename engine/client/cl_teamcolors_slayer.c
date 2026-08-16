/*
cl_teamcolors_slayer.c - Slayer3D per-player colour identity (CS2-style)
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

// Why this is a separate module and not two tables in two files: the radar dot
// and the scoreboard swatch MUST agree. If the mapping lived in both places,
// the first change to team parsing would silently desync them, and the whole
// point of the colour is "the cyan dot is the cyan name in the list".
//
// Assignment mirrors CS2: a fixed palette indexed by the player's position
// within its own team, ordered by client slot so it stays stable for the whole
// match instead of reshuffling when someone disconnects.
//
// TWO palettes, not one: counter-terrorists read as the blue side and
// terrorists as the red side, so every CT colour lives in the cool half of the
// hue circle and every T colour in the warm half. That way "friend or foe" is
// answered by the hue alone, before the eye even resolves which teammate it is.

#include "common.h"
#include "client.h"
#include "cl_view_slayer.h"
#include "cl_teamcolors_slayer.h"

#define SLAYER_TC_COUNT 5

// ===========================================================================
// Palettes
// ===========================================================================
//
// Chosen by MEASUREMENT, not by eye. The harness (tests/radar_scoreboard_test.c)
// computes CIE76 dE in Lab space -- the perceptual standard -- and enforces:
//   * within a team   : worst pair > 20   (tell two teammates apart)
//   * between teams   : worst pair > 45   (never mistake foe for friend)
//   * hue inside gamut: CT 170..295, T 320..60 degrees
//   * saturation > 0.55, lightness L > 40 (survive a 6-pixel dot on the map)
//
// What the six iterations taught, recorded so it is not re-learned the hard way:
//   1. A hand-rolled weighted-RGB metric lies. It over-weighted green and made
//      the task look unsolvable inside one gamut. Lab dE fixed that.
//   2. "Cool = blue channel dominates" wrongly admits pure green into BOTH
//      gamuts, which collapsed the between-team distance to 4. Gate on HUE.
//   3. Green (85..170) belongs to neither side: it reads as neither the blue
//      nor the red team. Association matters more than one extra hue.
//   4. A greedy farthest-point search drifts to the corners of the colour cube
//      and produces near-black (0,0,255) and muddy beige (184,148,83) --
//      formally distant, visually "black blob" and "dirt". Saturation and
//      lightness floors matter more than maximising distance.
//   5. Five colours inside ONE gamut are only separable if spread across BOTH
//      hue and LIGHTNESS. Two colours at the same L with nearby hue scored
//      dE 15 -- one colour to the eye.
//   6. A dark blue at L=36 disappears against the radar's own dark disc
//      (background 10,16,22). L must stay above ~44.
//
// Final measurement: CT worst pair 24.6, T worst pair 27.9,
// between teams 74.7 -- confusing a foe for a friend is three times harder
// than confusing two friends, which is the correct priority.

// Counter-terrorists: cool half (turquoise -> cyan-blue -> blue -> indigo ->
// violet). Lightness ladder 81 / 71 / 54 / 45 / 56.
static const byte slayer_tc_ct[SLAYER_TC_COUNT][3] =
{
	{  45, 225, 215 },   // turquoise
	{  60, 185, 255 },   // sky blue
	{  45, 125, 250 },   // blue
	{  85,  85, 240 },   // indigo   (was 60,60,215: L=36, sank into the disc)
	{ 180,  85, 255 }    // violet
};

// Terrorists: warm half (yellow -> orange -> red -> crimson -> coral).
// Lightness ladder 87 / 70 / 52 / 44 / 65.
static const byte slayer_tc_t[SLAYER_TC_COUNT][3] =
{
	{ 250, 215,  50 },   // yellow
	{ 255, 140,  30 },   // orange
	{ 235,  50,  40 },   // red
	{ 200,  30, 100 },   // crimson
	{ 255, 115, 105 }    // coral
};

// Neutral grey: team not announced yet, or a third team (spectators, some DM
// mods) that must not steal either side's identity.
static const byte slayer_tc_unknown[3] = { 170, 175, 180 };

static CVAR_DEFINE_AUTO( slayer_teamcolors, "1", FCVAR_ARCHIVE,
	"Slayer3D: per-player colour identity on radar and scoreboard (0 = off)" );

// ===========================================================================
// State
// ===========================================================================

// SLAYER_TC_SIDE_* live in the header: the scoreboard and the radar both need
// to ask which side a player is on.
//
// Colour index within the team (-1 = unknown) and which side it belongs to.
// Indexed by 0-based player slot to match slayer_scores; the team-name table is
// 1-based, hence the +1 when reading it.
static int      s_color_idx[MAX_CLIENTS];
static byte     s_side[MAX_CLIENTS];
static qboolean s_valid = false;

void Slayer_TeamColors_Init( void )
{
	Cvar_RegisterVariable( &slayer_teamcolors );
	Slayer_TeamColors_Reset();
}

void Slayer_TeamColors_Reset( void )
{
	int i;

	for( i = 0; i < MAX_CLIENTS; i++ )
	{
		s_color_idx[i] = -1;
		s_side[i] = SLAYER_TC_SIDE_NONE;
	}

	s_valid = false;
}

// ===========================================================================
// Side detection
// ===========================================================================

// Map a server team name onto a side. CS-family mods send "CT" / "TERRORIST"
// via TeamInfo, but forks and DM mods use their own spellings, so match on a
// prefix/substring rather than an exact compare. Anything unrecognised (or a
// spectator) stays SIDE_NONE and gets the neutral grey -- better a grey dot
// than a wrong-coloured one.
static byte Slayer_TC_SideOf( const char *team )
{
	if( COM_StringEmptyOrNULL( team ))
		return SLAYER_TC_SIDE_NONE;

	// Check the CT spellings first: "TERRORIST" is a substring of
	// "COUNTER-TERRORIST", so testing for T first would misclassify every CT.
	if( !Q_strnicmp( team, "CT", 2 ))
		return SLAYER_TC_SIDE_CT;
	if( Q_stristr( team, "COUNTER" ))
		return SLAYER_TC_SIDE_CT;

	if( Q_stristr( team, "TERROR" ))
		return SLAYER_TC_SIDE_T;
	if( !Q_strnicmp( team, "T", 1 ) && Q_strlen( team ) == 1 )
		return SLAYER_TC_SIDE_T;

	return SLAYER_TC_SIDE_NONE;
}

qboolean Slayer_TeamColors_IsAllyOf( int observer_slot, int slot )
{
	const char *mine, *theirs;

	if( observer_slot < 0 || observer_slot >= MAX_CLIENTS )
		return false;
	if( slot < 0 || slot >= MAX_CLIENTS )
		return false;
	if( slot == observer_slot )
		return true;

	mine = Slayer_PlayerTeam( observer_slot + 1 );
	theirs = Slayer_PlayerTeam( slot + 1 );

	if( COM_StringEmptyOrNULL( mine ) || COM_StringEmptyOrNULL( theirs ))
		return false;

	return Q_stricmp( mine, theirs ) == 0 ? true : false;
}

qboolean Slayer_TeamColors_IsAlly( int slot )
{
	return Slayer_TeamColors_IsAllyOf( cl.playernum, slot );
}

byte Slayer_TeamColors_Side( int slot )
{
	if( slot < 0 || slot >= MAX_CLIENTS || !s_valid )
		return SLAYER_TC_SIDE_NONE;

	return s_side[slot];
}

// ===========================================================================
// Assignment
// ===========================================================================

void Slayer_TeamColors_Update( void )
{
	int i;
	int next_ct = 0, next_t = 0;

	for( i = 0; i < MAX_CLIENTS; i++ )
	{
		s_color_idx[i] = -1;
		s_side[i] = SLAYER_TC_SIDE_NONE;
	}

	// Walk slots in order so the mapping is deterministic: a player keeps its
	// colour as long as nobody with a LOWER slot on the same side leaves.
	// Bucketing by SIDE rather than by team-name string means a mod that
	// renames its teams mid-match cannot shuffle the palette.
	for( i = 0; i < MAX_CLIENTS; i++ )
	{
		byte side = Slayer_TC_SideOf( Slayer_PlayerTeam( i + 1 ));

		s_side[i] = side;

		if( side == SLAYER_TC_SIDE_CT )
			s_color_idx[i] = ( next_ct++ ) % SLAYER_TC_COUNT;
		else if( side == SLAYER_TC_SIDE_T )
			s_color_idx[i] = ( next_t++ ) % SLAYER_TC_COUNT;
	}

	s_valid = true;
}

int Slayer_TeamColors_Index( int slot )
{
	if( slot < 0 || slot >= MAX_CLIENTS )
		return -1;
	if( !s_valid )
		return -1;

	return s_color_idx[slot];
}

void Slayer_TeamColors_Get( int slot, byte out[3] )
{
	int  idx;
	byte side;

	out[0] = slayer_tc_unknown[0];
	out[1] = slayer_tc_unknown[1];
	out[2] = slayer_tc_unknown[2];

	if( slayer_teamcolors.value == 0.0f )
		return;

	idx = Slayer_TeamColors_Index( slot );
	side = Slayer_TeamColors_Side( slot );

	if( idx < 0 || idx >= SLAYER_TC_COUNT )
		return;

	if( side == SLAYER_TC_SIDE_CT )
	{
		out[0] = slayer_tc_ct[idx][0];
		out[1] = slayer_tc_ct[idx][1];
		out[2] = slayer_tc_ct[idx][2];
	}
	else if( side == SLAYER_TC_SIDE_T )
	{
		out[0] = slayer_tc_t[idx][0];
		out[1] = slayer_tc_t[idx][1];
		out[2] = slayer_tc_t[idx][2];
	}
}
