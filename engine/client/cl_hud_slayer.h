/*
cl_hud_slayer.h - Slayer3D crosshair HUD: damage indicator
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

// Register damage-indicator cvars. Called from V_InitSlayerCvars().
void Slayer_HUD_Init( void );

// Draw the damage indicator under the crosshair (no-op when disabled or
// when no recent damage event is pending). Called every frame from the
// 2D pass in V_RenderView, before Slayer_Scoreboard_Draw().
void Slayer_HUD_Draw( void );

// Reset all pending damage events. Called on map change / disconnect from
// Slayer_ResetMatchState().
void Slayer_HUD_Reset( void );

// Hook for the GoldSrc/HL/CS "Damage" usermsg. Server format:
//   byte armor_taken, byte health_taken, long damage_bits, coord vec3 origin
// Only the first two bytes are consumed.
//   pbuf  - raw payload bytes
//   iSize - payload length
void Slayer_HUD_OnDamageMsg( const byte *pbuf, int iSize );

// Hook for "damage dealt" custom usermsgs that some servers broadcast
// (DamageReport, DmgInd, etc.). Tries to parse a single damage value
// from the first one or two bytes of the payload. Falls back gracefully
// when the server format does not match.
//   pbuf  - raw payload bytes
//   iSize - payload length
void Slayer_HUD_OnDamageDealtMsg( const byte *pbuf, int iSize );

// Fallback hook for damage dealt: called from Slayer_OnDeathMsg when the
// LOCAL player is the killer. Registers a synthetic "+kill_amount" event
// so the indicator still fires on stock servers that don't broadcast
// per-shot dealt-damage usermsgs. Controlled by
// slayer_damage_indicator_kill_amount (0 = disabled).
void Slayer_HUD_OnLocalKill( void );

#endif // CL_HUD_SLAYER_H
