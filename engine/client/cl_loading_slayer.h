/*
cl_loading_slayer.h - Slayer3D PC-style loading screen overlay
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

#ifndef CL_LOADING_SLAYER_H
#define CL_LOADING_SLAYER_H

#include "xash3d_types.h"
#include "input.h"

// Register loading-screen cvars. Call from V_InitSlayerCvars().
void Slayer_Loading_Init( void );

// Reset state on disconnect / map change.
void Slayer_Loading_Reset( void );

// Main draw entry point. Called from SCR_DrawPlaque() when loading is active.
// Returns true if the overlay was drawn (caller may skip default plaque).
qboolean Slayer_Loading_Draw( void );

// Touch event handler. Returns true if the event was consumed (cancel button / drag).
// x,y are normalized [0..1] screen coords; dx,dy are deltas.
qboolean Slayer_Loading_TouchEvent( touchEventType type, int fingerID, float x, float y, float dx, float dy );

#endif // CL_LOADING_SLAYER_H
