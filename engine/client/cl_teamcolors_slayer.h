/*
cl_teamcolors_slayer.h - Slayer3D per-player colour identity (CS2-style)
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

#ifndef CL_TEAMCOLORS_SLAYER_H
#define CL_TEAMCOLORS_SLAYER_H

#include "xash3d_types.h"

// One shared colour identity per player, used by BOTH the radar dots and the
// scoreboard swatches. Kept in its own module precisely so the two can never
// drift apart: a player who is green on the radar must be green on the board.
//
// CS2 assigns a fixed colour per team slot (green / yellow / orange / purple /
// blue for your side). We do the same, keyed by the player's index within its
// team, so the mapping is stable for the whole match instead of shuffling when
// someone disconnects.

// Register cvars. Called from V_InitSlayerCvars().
void Slayer_TeamColors_Init( void );

// Drop cached team ordering. Called on map change from Slayer_ResetMatchState().
void Slayer_TeamColors_Reset( void );

// Colour for a player slot (0-based, matching slayer_scores indexing).
// Writes r/g/b. Never fails: unknown slots get a neutral grey.
void Slayer_TeamColors_Get( int slot, byte out[3] );

// Which side a player is on, for callers that need it directly (the scoreboard
// header, the radar's friend/foe split). Values match SLAYER_TC_SIDE_* below.
#define SLAYER_TC_SIDE_NONE 0
#define SLAYER_TC_SIDE_CT   1
#define SLAYER_TC_SIDE_T    2

byte Slayer_TeamColors_Side( int slot );

// Rebuild the slot -> colour-index mapping from the current team assignment.
// Cheap; call once per frame before drawing radar/scoreboard.
void Slayer_TeamColors_Update( void );

// True when `slot` is on the same team as the local player.
qboolean Slayer_TeamColors_IsAlly( int slot );

// True when `slot` is on the same team as an arbitrary observer slot.
// Used by the radar while spectating, where cl.playernum is not the player
// whose team context is being displayed.
qboolean Slayer_TeamColors_IsAllyOf( int observer_slot, int slot );

// The player's colour index within its team (0..4), or -1 if unknown.
// Exposed so the scoreboard can show the same ordering as the radar.
int Slayer_TeamColors_Index( int slot );

#endif // CL_TEAMCOLORS_SLAYER_H
