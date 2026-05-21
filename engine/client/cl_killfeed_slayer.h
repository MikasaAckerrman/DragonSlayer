/*
cl_killfeed_slayer.h - Slayer3D killfeed overlay
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

#ifndef CL_KILLFEED_SLAYER_H
#define CL_KILLFEED_SLAYER_H

#include "xash3d_types.h"

// Initialize killfeed cvars. Called once from V_InitSlayerCvars().
void Slayer_Killfeed_Init( void );

// Reset killfeed entries (map change / disconnect).
// Called from Slayer_ResetMatchState().
void Slayer_Killfeed_Reset( void );

// Feed a DeathMsg into the killfeed.
//   killer  - 1-based entindex of the killer (0 = world)
//   victim  - 1-based entindex of the victim
//   headshot - true if headshot kill
//   weapon  - weapon name string (e.g. "ak47"), may be NULL
void Slayer_Killfeed_OnDeathMsg( int killer, int victim, qboolean headshot, const char *weapon );

// Draw the killfeed overlay. Called every frame from V_PostRender 2D block.
// No-op when disabled or no entries to draw.
void Slayer_Killfeed_Draw( void );

#endif // CL_KILLFEED_SLAYER_H
