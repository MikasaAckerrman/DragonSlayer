/*
cl_radar_slayer.h - Slayer3D radar (CS2-style) replacing the vanilla one
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

#ifndef CL_RADAR_SLAYER_H
#define CL_RADAR_SLAYER_H

#include "xash3d_types.h"

// Register radar cvars and commands. Called from V_InitSlayerCvars().
void Slayer_Radar_Init( void );

// Draw the radar. Called from the 2D pass in V_PostRender, before the
// scoreboard so the board can cover it when open.
void Slayer_Radar_Draw( void );

// Drop per-map state: the loaded map texture, its overview transform and the
// enemy-sighting memory. Called on map change from Slayer_ResetMatchState().
void Slayer_Radar_Reset( void );

// True while the radar wants the vanilla one suppressed. The stock CS radar is
// drawn by the client library, which we cannot partially disable, so this only
// reports intent; the actual suppression is a client-side cvar the user sets
// (see the comment in cl_radar_slayer.c).
qboolean Slayer_Radar_IsEnabled( void );

#endif // CL_RADAR_SLAYER_H
