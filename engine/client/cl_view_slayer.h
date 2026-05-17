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

// Hook for a server-sent "DeathMsg" user message. Plays the configured
// kill sound when the local player is the victim.
//
//   pbuf  - raw payload bytes after the 1-byte size prefix has been
//           stripped (same buffer that gets handed to client.dll)
//   iSize - payload length in bytes
//
// Implements the standard HL/CS/CSCZ/DM layout where pbuf[0] is the
// killer entindex and pbuf[1] is the victim entindex.
void V_OnDeathMsg( const byte *pbuf, int iSize );

#endif // CL_VIEW_SLAYER_H
