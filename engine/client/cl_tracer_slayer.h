/*
cl_tracer_slayer.h - Slayer3D custom bullet tracers
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

#ifndef CL_TRACER_SLAYER_H
#define CL_TRACER_SLAYER_H

#include "xash3d_types.h"

// Register cvars. Call once at client init.
void Slayer_Tracer_Init( void );

// Reset per-map state (heat, timers). Call on disconnect / map change.
void Slayer_Tracer_Reset( void );

// Intercept point for every bullet tracer, from R_TracerEffect. Returns true
// when Slayer3D handled the tracer (styled it), false to let the engine draw
// the vanilla tracer unchanged. start = muzzle, end = impact.
//
// weapon_scale is applied to the tracer thickness so heavier guns read bigger;
// pass 1.0 when the class is unknown.
qboolean Slayer_Tracer_OnFire( const vec3_t start, const vec3_t end );

// Advance the heat state machine. Call once per rendered frame.
void Slayer_Tracer_Frame( void );

// Whether Slayer3D should suppress the vanilla bullet-impact sparks so ours
// can replace them. Checked from R_BulletImpactParticles / R_SparkStreaks.
qboolean Slayer_Tracer_SuppressVanillaSparks( void );

#endif // CL_TRACER_SLAYER_H
