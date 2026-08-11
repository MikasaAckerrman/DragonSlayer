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

// Reset per-map state (heat, timers, beam model cache). Call on disconnect /
// map change.
void Slayer_Tracer_Reset( void );

// Detect a shot on a player entity via the rising edge of EF_MUZZLEFLASH and
// spawn a tracer beam from its muzzle to the impact point. Call once per frame
// per player from CL_LinkPlayers. slot 0 = local viewent, 1..MAX_CLIENTS =
// remote player index. is_local picks the aim source (view vs entity angles).
void Slayer_Tracer_CheckMuzzleflash( struct cl_entity_s *ent, int slot, qboolean is_local );

// Exact local impact endpoint from the spread-aware client weapon event.
// Called from R_BulletImpactParticles before optional spark suppression.
void Slayer_Tracer_NoteImpact( const vec3_t pos );

// Ownership window around one weapon event (CL_FireEvent). The event that
// computes spread also reports the impact, so this is the ONLY place that can
// attribute an impact to a shooter: two players firing point-blank in the same
// frame are otherwise indistinguishable by position or time alone.
// `entindex` is the shooting entity (1..maxclients for players).
// Begin returns the PREVIOUS owner and End takes it back, so a weapon event that
// plays another event cannot clear ownership for the rest of the outer one.
int  Slayer_Tracer_BeginEvent( int entindex );
void Slayer_Tracer_EndEvent( int prev_owner );

// Independent cross-check: note a server-sent TE_TRACER temp-entity. This is a
// separate path from EF_MUZZLEFLASH; counted in diagnostics so the device log
// reveals whether the server announces shots even if muzzleflash misses remote
// players. Call from the TE_TRACER handler in cl_tent.c. Counting only.
void Slayer_Tracer_NoteServerTracer( const vec3_t start, const vec3_t end );

// Advance the heat state machine. Call once per rendered frame.
void Slayer_Tracer_Frame( void );

// Age and draw every live tracer of the OWN renderer (ribbon geometry).
// Call from CL_DrawEFX in the translucent pass, after the engine beams, so the
// view matrix is already set up and additive blending lands on top of the world.
void Slayer_TracerPool_Draw( void );

// Whether Slayer3D should suppress the vanilla bullet-impact sparks so ours
// can replace them. Checked from R_BulletImpactParticles / R_SparkStreaks.
qboolean Slayer_Tracer_SuppressVanillaSparks( void );

// Diagnostic: record which events the game DLL plays back, so the shot events
// worth hooging tracers onto can be identified from a device log. CS 1.6 does
// not route bullets through R_TracerEffect, so this is how we find the real
// path. Rate-limited internally; a no-op unless slayer_tracer_logevents is set.
void Slayer_Tracer_LogEvent( int eventindex, const char *name,
	const float *origin, const float *angles );

#endif // CL_TRACER_SLAYER_H
