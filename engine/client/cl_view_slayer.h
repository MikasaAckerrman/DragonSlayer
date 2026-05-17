/*
cl_view_slayer.h - Slayer3D third-person camera extension
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

#ifndef CL_VIEW_SLAYER_H
#define CL_VIEW_SLAYER_H

#include "xash3d_types.h"
#include "ref_api.h"

// Register all Slayer3D-specific cvars. Must be called once during client
// startup, before any cvar value is read (e.g. from V_Init).
void V_InitSlayerCvars( void );

// Returns true when the third-person mode is active.
qboolean V_IsSlayerThirdPerson( void );

// Adjust the given viewpass so the camera orbits the player.
// Safe to call every frame; no-op when slayer_thirdperson is 0.
void V_ApplySlayerThirdPerson( ref_viewpass_t *rvp );

#endif // CL_VIEW_SLAYER_H
