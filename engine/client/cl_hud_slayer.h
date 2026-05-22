/*
cl_hud_slayer.h - Slayer3D crosshair-area HUD overlay (damage indicator)
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

#ifndef CL_HUD_SLAYER_H
#define CL_HUD_SLAYER_H

#include "xash3d_types.h"

// Initialize damage-indicator cvars and clear all in-flight entries.
// Called once from V_InitSlayerCvars().
void Slayer_HUD_Init( void );

// Drop all in-flight damage entries (used on map change / disconnect).
// Called from Slayer_ResetMatchState().
void Slayer_HUD_Reset( void );

// Render every live damage-indicator entry near the crosshair.
// Each entry sits at its own halo-slot offset so back-to-back hits do
// not stack on the same pixel and become unreadable.
//
// Call from the V_PostRender 2D pass alongside Slayer_Scoreboard_Draw.
// No-op while slayer_damage_indicator is 0 or no entries are active.
void Slayer_HUD_Draw( void );

// Hook invoked for every server-sent user-message before its payload is
// forwarded to the game DLL. Probes a list of common damage-feedback
// message names ("DmgIndicator", "AttackerDmg", "ReDmgInd", ...) and,
// on a match, registers a new fading number near the crosshair.
//
// Unknown message names are a no-op; safe to call unconditionally.
//
//   msgname - the user-message name as registered by the game DLL
//   pbuf    - raw payload bytes (msgname-specific format; see .c)
//   iSize   - payload length in bytes
void Slayer_HUD_OnDamageMessage( const char *msgname, const byte *pbuf, int iSize );

#endif // CL_HUD_SLAYER_H
