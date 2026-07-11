/*
cl_sgs_slayer.h - Slayer3D Side-Game Strafe (auto-strafe for mobile)
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

#ifndef CL_SGS_SLAYER_H
#define CL_SGS_SLAYER_H

#include "xash3d_types.h"

struct usercmd_s;

// Register cvars and "+slayer_sgs"/"-slayer_sgs" key commands.
// Called once from V_InitSlayerCvars().
void Slayer_SGS_Init( void );

// Reset internal state (phase, swipe timestamp, prev yaw).
// Called on map change / disconnect from Slayer_ResetMatchState().
void Slayer_SGS_Reset( void );

// Capture the current cmd->viewangles[YAW] AFTER the user's look input
// (mouse / touch / pfnLookEvent) has been applied for this frame, but
// BEFORE we add any synthetic oscillation. This drives swipe detection.
// Called from CL_CreateCmd in cl_main.c.
void Slayer_SGS_Capture( const struct usercmd_s *cmd );

// Inject yaw oscillation + synchronized sidemove into the outgoing usercmd
// to produce server-side air-strafe acceleration. Called from CL_CreateCmd
// after V_SlayerMovementTweaks. No-op when the SGS feature is disabled or
// the engagement gate (key held / recent swipe) is not met.
void Slayer_SGS_Apply( struct usercmd_s *cmd, double frametime );

#endif // CL_SGS_SLAYER_H
