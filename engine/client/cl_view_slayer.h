/*
cl_view_slayer.h - Slayer3D third-person camera + kill-sound module
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

// ---------------------------------------------------------------------------
// Third-person camera
// ---------------------------------------------------------------------------

// Register all Slayer3D-specific cvars (camera + kill-sound).
// Must be called once during client startup (from V_Init in gamma.c).
void V_InitSlayerCvars( void );

// Returns true when the Slayer3D third-person mode is active.
qboolean V_IsSlayerThirdPerson( void );

// Adjust the given viewpass so the camera orbits the player.
// Safe to call every frame; no-op when slayer_thirdperson is 0.
void V_ApplySlayerThirdPerson( ref_viewpass_t *rvp );

// ---------------------------------------------------------------------------
// Kill-sound
// ---------------------------------------------------------------------------

// Hook for a server-sent "DeathMsg" user message. Plays a configured
// kill sound when the LOCAL PLAYER is the KILLER (not the victim).
//
//   pbuf  - raw payload bytes (same buffer handed to client.dll)
//   iSize - payload length in bytes
//
// Returns true if a kill sound was triggered, false otherwise.
qboolean Slayer_OnDeathMsg( const byte *pbuf, int iSize );

// Hook for a server-sent "TeamInfo" user message. Stores the team name
// of a given client so Slayer_OnDeathMsg can detect teamkills.
//
//   pbuf  - raw payload bytes (pbuf[0] = client slot, pbuf[1..] = team)
//   iSize - payload length in bytes
void Slayer_OnTeamInfo( const byte *pbuf, int iSize );

// Reset the Slayer3D per-match state (team table, etc).
// Called when the engine clears client state (disconnect / map change).
void Slayer_ResetMatchState( void );

#endif // CL_VIEW_SLAYER_H
