/*
cl_grenade_tumble_slayer.c - Slayer3D client-side grenade tumble
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

// =============================================================================
// Client-side grenade tumble
// =============================================================================
//
// CS 1.6 (and most GoldSrc mods) drive grenade visual rotation purely from
// the server's pev->avelocity, which is set once at throw time to a vector
// like (-100..-500, 0, 0) — that means the grenade only spins around the X
// axis and never tumbles end-over-end the way a real thrown object does.
//
// This module overrides the angle of the grenade entity client-side, before
// the renderer sees it:
//
//   * we detect grenade entities by model name (w_hegrenade.mdl,
//     w_smokegrenade.mdl, w_flashbang.mdl and the HL "w_grenade" generic)
//   * for each grenade we keep a small per-entity slot with a randomized
//     rotation axis and an accumulated angle pose
//   * the angular speed is rescaled every frame to be proportional to the
//     instantaneous linear speed (delta-origin / delta-time), so a fast
//     thrown grenade tumbles fast and as it bleeds speed to gravity /
//     bounces / friction the rotation slows down naturally and stops once
//     the grenade comes to rest on the ground
//
// The hook lives in CL_AddVisibleEntity (cl_frame.c) and runs after the
// engine's interpolation but before R_AddEntity, so we have the final
// origin to differentiate against the previous frame's origin.
//
// Toggle: cl_slayer_grenade_tumble (default 1).

#include "common.h"
#include "client.h"
#include "xash3d_mathlib.h"
#include "cl_grenade_tumble_slayer.h"

// =============================================================================
// Cvars
// =============================================================================

static CVAR_DEFINE_AUTO( slayer_grenade_tumble,
	"1", FCVAR_ARCHIVE,
	"Slayer3D: client-side 3-axis grenade tumble proportional to linear speed (0 = off)" );

// =============================================================================
// Tunables
// =============================================================================

#define GT_MAX_SLOTS  32     // ~rarely more than a handful of grenades in flight
#define GT_LIFETIME   5.0f   // sec: slot reclaimed if not refreshed
#define GT_BASE_RATE  720.0f // deg/sec at GT_MAX_SPEED
#define GT_MAX_SPEED  600.0f // hammer units / sec — typical strong throw
#define GT_REST_SPEED 20.0f  // below this speed rotation halts entirely

// =============================================================================
// Per-entity tumble state
// =============================================================================

typedef struct
{
	int       index;        // engine entity index, 0 = empty slot
	float     last_time;    // cl.time of last update (also slot expiry)
	vec3_t    last_origin;  // for linear velocity estimation
	vec3_t    accum_angles; // current pose (overrides ent->angles)
	vec3_t    avel_dir;     // unit vector — rotation axis (random per entity)
	qboolean  inited;
} grenade_tumble_t;

static grenade_tumble_t gt_slots[GT_MAX_SLOTS];

// =============================================================================
// Helpers
// =============================================================================

static qboolean Slayer_GT_IsGrenadeModel( const char *name )
{
	if( !name || !name[0] )
		return false;

	// CS 1.6 weaponbox model names — covers HE, smoke, flashbang
	if( Q_strstr( name, "w_hegrenade" ))    return true;
	if( Q_strstr( name, "w_smokegrenade" )) return true;
	if( Q_strstr( name, "w_flashbang" ))    return true;
	// HL1 generic grenade (in case someone runs DragonSlayer on plain HLDM)
	if( Q_strstr( name, "w_grenade" ))      return true;
	return false;
}

static grenade_tumble_t *Slayer_GT_GetSlot( int index )
{
	int   i;
	int   empty_slot = -1;
	int   oldest_slot = 0;
	float oldest_time = 1.0e30f;

	for( i = 0; i < GT_MAX_SLOTS; i++ )
	{
		if( gt_slots[i].index == index && gt_slots[i].inited )
			return &gt_slots[i];

		if( gt_slots[i].index == 0 && empty_slot < 0 )
			empty_slot = i;

		if( gt_slots[i].last_time < oldest_time )
		{
			oldest_time = gt_slots[i].last_time;
			oldest_slot = i;
		}
	}

	if( empty_slot >= 0 )
		return &gt_slots[empty_slot];

	// recycle the oldest slot — only matters if we somehow get >GT_MAX_SLOTS
	// distinct grenades in the air at the exact same instant
	return &gt_slots[oldest_slot];
}

static void Slayer_GT_InitSlot( grenade_tumble_t *gt, struct cl_entity_s *ent, float now )
{
	vec3_t axis;

	gt->index  = ent->index;
	gt->inited = true;
	VectorClear( gt->accum_angles );

	// random unit axis so every grenade tumbles a little differently
	axis[0] = COM_RandomFloat( -1.0f, 1.0f );
	axis[1] = COM_RandomFloat( -1.0f, 1.0f );
	axis[2] = COM_RandomFloat( -1.0f, 1.0f );

	if( VectorLength( axis ) < 0.01f )
	{
		axis[0] = 1.0f; axis[1] = 0.0f; axis[2] = 0.0f;
	}
	VectorNormalize( axis );
	VectorCopy( axis, gt->avel_dir );

	// IMPORTANT: read ent->origin (post-interp render position), NOT
	// ent->curstate.origin (raw snapshot, only updates at server tickrate).
	// Using curstate.origin here would make speed estimation degenerate
	// because dt is per-render-frame (~16ms) while curstate.origin only
	// changes per-snapshot (~50ms) — most frames see delta=0.
	VectorCopy( ent->origin, gt->last_origin );
	gt->last_time = now;
}

// =============================================================================
// Public API
// =============================================================================

void Slayer_GrenadeTumble_Init( void )
{
	int i;

	Cvar_RegisterVariable( &slayer_grenade_tumble );

	for( i = 0; i < GT_MAX_SLOTS; i++ )
	{
		gt_slots[i].index  = 0;
		gt_slots[i].inited = false;
		gt_slots[i].last_time = 0.0f;
		VectorClear( gt_slots[i].accum_angles );
		VectorClear( gt_slots[i].last_origin );
		VectorClear( gt_slots[i].avel_dir );
	}
}

void Slayer_GrenadeTumble_Apply( struct cl_entity_s *ent )
{
	grenade_tumble_t *gt;
	float             now;
	float             dt;
	vec3_t            delta;
	float             speed;
	float             rate;
	int               i;

	if( !slayer_grenade_tumble.value )
		return;

	if( !ent || !ent->model )
		return;

	if( !Slayer_GT_IsGrenadeModel( ent->model->name ))
		return;

	now = cl.time;
	gt  = Slayer_GT_GetSlot( ent->index );
	if( !gt )
		return;

	// fresh slot or recycled after expiry — reseed and bail (need a previous
	// origin sample to estimate velocity for the first tumble step)
	if( !gt->inited || gt->index != ent->index || ( now - gt->last_time ) > GT_LIFETIME )
	{
		Slayer_GT_InitSlot( gt, ent, now );
		// still apply the (zero) accumulated angles so the renderer doesn't
		// see a single-axis spin from the server's avelocity on this frame
		VectorCopy( gt->accum_angles, ent->angles );
		return;
	}

	dt = now - gt->last_time;
	if( dt <= 0.0f )
	{
		// same-frame double call (e.g. multiple visible passes): just reapply
		VectorCopy( gt->accum_angles, ent->angles );
		return;
	}
	if( dt > 0.5f )
		dt = 0.0f; // long pause (loading screen, demo seek): freeze pose

	// Use the interpolated render position, not the raw snapshot — see comment
	// in Slayer_GT_InitSlot for why.
	VectorSubtract( ent->origin, gt->last_origin, delta );
	speed = ( dt > 0.0f ) ? ( VectorLength( delta ) / dt ) : 0.0f;

	// Teleport / entity-index reuse guard. If a grenade exploded and the
	// engine handed the same ent->index to a brand new grenade before our
	// slot expired, last_origin points to the previous grenade's resting
	// place and delta is huge. Same thing happens on changelevel and
	// CL_EntityTeleported events. Treat any impossibly-fast frame as a
	// reset: reseed the slot at the new origin and skip this frame.
	if( speed > GT_MAX_SPEED * 2.0f )
	{
		Slayer_GT_InitSlot( gt, ent, now );
		VectorCopy( gt->accum_angles, ent->angles );
		return;
	}

	if( speed < GT_REST_SPEED )
	{
		rate = 0.0f;
	}
	else
	{
		if( speed > GT_MAX_SPEED ) speed = GT_MAX_SPEED;
		rate = GT_BASE_RATE * ( speed / GT_MAX_SPEED );
	}

	for( i = 0; i < 3; i++ )
	{
		gt->accum_angles[i] += gt->avel_dir[i] * rate * dt;
		while( gt->accum_angles[i] >  360.0f ) gt->accum_angles[i] -= 360.0f;
		while( gt->accum_angles[i] < -360.0f ) gt->accum_angles[i] += 360.0f;
	}

	VectorCopy( ent->origin, gt->last_origin );
	gt->last_time = now;

	VectorCopy( gt->accum_angles, ent->angles );
}
