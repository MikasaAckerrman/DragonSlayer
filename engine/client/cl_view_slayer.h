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

// Hook for a server-sent "DeathMsg" user message. Plays one of three
// configured kill sounds when the local player is the victim:
// teamkill, headshot or generic, in that priority order.
//
//   pbuf  - raw payload bytes after the 1-byte size prefix has been
//           stripped (same buffer that gets handed to client.dll)
//   iSize - payload length in bytes
//
// HL/CS/CSCZ/DM layout the hook understands:
//   pbuf[0] = killer entindex
//   pbuf[1] = victim entindex
//   pbuf[2] = headshot flag (CS only; 0 or 1)
//   pbuf[3..] = NUL-terminated weapon name string
void V_OnDeathMsg( const byte *pbuf, int iSize );

// Hook for a server-sent "TeamInfo" user message. Stores the team name
// of a given client so V_OnDeathMsg can later detect teamkills.
//
//   pbuf  - raw payload bytes (pbuf[0] = client slot, pbuf[1..] = team)
//   iSize - payload length in bytes
void V_OnTeamInfo( const byte *pbuf, int iSize );

// Reset the Slayer3D per-match state that lives between games (team
// table, etc). Called when the engine clears the client state, e.g.
// on disconnect or map change.
void V_SlayerResetMatchState( void );

#endif // CL_VIEW_SLAYER_H
