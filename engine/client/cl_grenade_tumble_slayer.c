/*
cl_grenade_tumble_slayer.c - Slayer3D client-side grenade tumble + quick throw
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
// Client-side grenade tumble (axial spin)
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
//     rotation axis and an accumulated TOTAL ANGLE (single scalar)
//   * the angular speed is rescaled every frame to be proportional to the
//     instantaneous linear speed (delta-origin / delta-time), so a fast
//     thrown grenade tumbles fast and as it bleeds speed to gravity /
//     bounces / friction the rotation slows down naturally and stops once
//     the grenade comes to rest on the ground
//
// CRITICAL: rotation is built as proper axis-angle (rotation by θ around
// fixed unit axis n) and converted to the engine's Euler angles via the
// quaternion intermediate (AngleQuaternion / QuaternionAngle). Earlier
// versions of this file accumulated three Euler components independently
// from `n * rate * dt` — that produces non-commutative Euler stacking, NOT
// a single rotation about n, and the visual was orbital ("Earth around
// Sun") instead of axial ("Earth around its own axis"). The fix is to
// keep only the scalar accumulated angle and rebuild the proper Euler
// every frame from (n, θ_total) → quaternion → engine Euler.
//
// The hook lives in CL_AddVisibleEntity (cl_frame.c) and runs after the
// engine's interpolation but before R_AddEntity, so we have the final
// origin to differentiate against the previous frame's origin.
//
// Toggle: cl_slayer_grenade_tumble (default 1).
//
// =============================================================================
// Quick throw command (slayer_quickthrow)
// =============================================================================
//
// Standoff-style one-button grenade throw: select grenade slot, pull the
// pin (+attack), release immediately (-attack), switch back to previous
// weapon (lastinv). All chained through Cbuf_AddText with no client-side
// cooldown — the only timing limiter is the server's grenade throw
// animation (~1.0-1.5s for CS 1.6).
//
// Usage:
//   bind v "slayer_quickthrow"                          // slot4 (cycles HE/Flash/Smoke)
//   bind c "slayer_quickthrow weapon_flashbang"
//   bind z "slayer_quickthrow weapon_smokegrenade"
//   bind x "slayer_quickthrow weapon_hegrenade"
//

#include "common.h"
#include "client.h"
#include "studio.h"
#include "xash3d_mathlib.h"
#include "cl_grenade_tumble_slayer.h"

// =============================================================================
// Cvars
// =============================================================================

static CVAR_DEFINE_AUTO( slayer_grenade_tumble,
	"1", FCVAR_ARCHIVE,
	"Slayer3D: client-side axial grenade tumble proportional to linear speed (0 = off)" );

static CVAR_DEFINE_AUTO( slayer_grenade_pivot_fix,
	"1", FCVAR_ARCHIVE,
	"Slayer3D: compensate for off-center grenade model pivot so rotation is around its own axis (0 = off)" );

// =============================================================================
// Tunables
// =============================================================================

#define GT_MAX_SLOTS  32      // ~rarely more than a handful of grenades in flight
#define GT_LIFETIME   5.0f    // sec: slot reclaimed if not refreshed
#define GT_BASE_RATE  1080.0f // deg/sec at GT_MAX_SPEED (1.5x bump from initial 720)
#define GT_MAX_SPEED  600.0f  // hammer units / sec — typical strong throw
#define GT_REST_SPEED 20.0f   // below this speed rotation halts entirely

// =============================================================================
// Per-entity tumble state
// =============================================================================

typedef struct
{
	int       index;        // engine entity index, 0 = empty slot
	float     last_time;    // cl.time of last update (also slot expiry)
	vec3_t    last_origin;  // for linear velocity estimation
	float     accum_theta;  // total accumulated rotation angle in RADIANS
	vec3_t    avel_dir;     // unit vector — fixed rotation axis (random per entity)
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

	gt->index       = ent->index;
	gt->inited      = true;
	gt->accum_theta = 0.0f;

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

// Build a quaternion from axis-angle (axis must be unit, theta in radians)
// and convert it to the engine's Euler angles. Layout matches AngleQuaternion
// in xash3d_mathlib.h: q = (axis*sin(θ/2), cos(θ/2)).
static void Slayer_GT_AxisAngleToEngineEuler( const vec3_t axis, float theta, vec3_t out_angles )
{
	vec4_t q;
	float  half = theta * 0.5f;
	float  s    = sinf( half );

	q[0] = axis[0] * s;
	q[1] = axis[1] * s;
	q[2] = axis[2] * s;
	q[3] = cosf( half );

	QuaternionAngle( q, out_angles );
}

// Compensate for off-center model pivot.
//
// PROBLEM: Studio renderer rotates the mesh around the model's LOCAL origin
// (0,0,0). For models like w_hegrenade.mdl, w_smokegrenade.mdl etc. the mesh
// geometric center sits at offset L_center = (mins + maxs)/2 from the local
// origin, so rotating around (0,0,0) makes the visual center *orbit* the
// world position ent->origin (radius |L_center|) instead of spinning in
// place. The user sees the grenade flying in a circle.
//
// FIX: Shift ent->origin by -(R(angles)*L_center - L_center). After the
// engine renders T(ent_origin') * R(angles) * mesh, the visual center
// world position becomes:
//     ent_origin' + R(angles) * L_center
//   = (ent_origin - R(angles)*L_center + L_center) + R(angles) * L_center
//   = ent_origin + L_center
// — same as it would be at angles=0 (no rotation), i.e. STABLE across spin.
//
// We mutate ent->origin AFTER the speed estimator has captured the original
// world position into gt->last_origin, so the per-frame velocity used for
// rotation rate is unaffected.
#define GT_DIAG_MAX_MODELS 8

static void Slayer_GT_CompensatePivot( struct cl_entity_s *ent )
{
	vec3_t    L_center, rotated_L, shift;
	matrix3x4 mat;

	if( !slayer_grenade_pivot_fix.value )
		return;
	if( !ent->model )
		return;

	VectorAverage( ent->model->mins, ent->model->maxs, L_center );

	// Skip if model is already centered at its origin (avoid wasted math)
	if( DotProduct( L_center, L_center ) < 0.01f )
		return;

	// Pure rotation matrix from current angles (translation = origin = 0)
	Matrix3x4_CreateFromEntity( mat, ent->angles, vec3_origin, 1.0f );
	Matrix3x4_VectorRotate( mat, L_center, rotated_L );

	VectorSubtract( rotated_L, L_center, shift );
	VectorSubtract( ent->origin, shift, ent->origin );

	// Diagnostic one-shot print (only when cvar >= 2)
	if( slayer_grenade_pivot_fix.value >= 2.0f )
	{
		static char diag_printed_models[GT_DIAG_MAX_MODELS][MAX_QPATH];
		static int  diag_printed_count = 0;
		int         i;
		qboolean    already_printed = false;

		for( i = 0; i < diag_printed_count; i++ )
		{
			if( !Q_strcmp( diag_printed_models[i], ent->model->name ))
			{
				already_printed = true;
				break;
			}
		}

		if( !already_printed )
		{
			studiohdr_t   *phdr;
			mstudiobone_t *pbones;

			if( diag_printed_count < GT_DIAG_MAX_MODELS )
			{
				Q_strncpy( diag_printed_models[diag_printed_count], ent->model->name, MAX_QPATH );
				diag_printed_count++;
			}

			Con_Printf( "[SlayerGT] model=%s mins=(%.1f %.1f %.1f) maxs=(%.1f %.1f %.1f) L_center=(%.1f %.1f %.1f) shift=(%.1f %.1f %.1f)\n",
				ent->model->name,
				ent->model->mins[0], ent->model->mins[1], ent->model->mins[2],
				ent->model->maxs[0], ent->model->maxs[1], ent->model->maxs[2],
				L_center[0], L_center[1], L_center[2],
				shift[0], shift[1], shift[2] );

			phdr = (studiohdr_t *)Mod_StudioExtradata( ent->model );
			if( phdr && phdr->numbones > 0 )
			{
				pbones = (mstudiobone_t *)((byte *)phdr + phdr->boneindex);
				Con_Printf( "[SlayerGT] bone[0] \"%s\" pos=(%.1f %.1f %.1f)\n",
					pbones[0].name,
					pbones[0].value[0], pbones[0].value[1], pbones[0].value[2] );
			}
		}
	}
}

// =============================================================================
// Quick throw command
// =============================================================================

static void Cmd_SlayerQuickThrow_f( void )
{
	const char *slot = "slot4";

	if( Cmd_Argc() >= 2 )
	{
		// allow direct weapon name (weapon_hegrenade etc.) or numeric slotN
		slot = Cmd_Argv( 1 );
	}

	// Standoff-style one-shot throw: select, pin-pull, release, switch back.
	// All four commands are queued in a single Cbuf_AddText so they run
	// sequentially in the next Cbuf_Execute pass with no extra client delay.
	// Server-side throw animation timing remains the actual gate (~1.5s).
	Cbuf_AddTextf( "%s\n+attack\n-attack\nlastinv\n", slot );
}

// =============================================================================
// Public API
// =============================================================================

void Slayer_GrenadeTumble_Init( void )
{
	int i;

	Cvar_RegisterVariable( &slayer_grenade_tumble );
	Cvar_RegisterVariable( &slayer_grenade_pivot_fix );

	Cmd_AddCommand( "slayer_quickthrow", Cmd_SlayerQuickThrow_f,
		"Slayer3D: one-button grenade quick throw — slot4 by default; pass "
		"weapon_hegrenade / weapon_flashbang / weapon_smokegrenade for direct "
		"selection. Example: bind v \"slayer_quickthrow\"" );

	for( i = 0; i < GT_MAX_SLOTS; i++ )
	{
		gt_slots[i].index       = 0;
		gt_slots[i].inited      = false;
		gt_slots[i].last_time   = 0.0f;
		gt_slots[i].accum_theta = 0.0f;
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
		Slayer_GT_AxisAngleToEngineEuler( gt->avel_dir, gt->accum_theta, ent->angles );
		Slayer_GT_CompensatePivot( ent );
		return;
	}

	dt = now - gt->last_time;
	if( dt <= 0.0f )
	{
		// same-frame double call (e.g. multiple visible passes): just reapply
		Slayer_GT_AxisAngleToEngineEuler( gt->avel_dir, gt->accum_theta, ent->angles );
		Slayer_GT_CompensatePivot( ent );
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
		Slayer_GT_AxisAngleToEngineEuler( gt->avel_dir, gt->accum_theta, ent->angles );
		Slayer_GT_CompensatePivot( ent );
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

	// Single scalar accumulator — total rotation in radians around fixed
	// axis avel_dir. This is the proper axis-angle representation; build
	// the engine's Euler angles from (axis, theta) via quaternion to avoid
	// the orbital drift that comes from stacking three independent Euler
	// component accumulators.
	gt->accum_theta += DEG2RAD( rate * dt );

	// Wrap into [-2π, 2π] to keep float precision over long-lived tumbles
	// (e.g. a smoke grenade that bounces for many seconds before settling).
	while( gt->accum_theta >  2.0f * (float)M_PI ) gt->accum_theta -= 2.0f * (float)M_PI;
	while( gt->accum_theta < -2.0f * (float)M_PI ) gt->accum_theta += 2.0f * (float)M_PI;

	VectorCopy( ent->origin, gt->last_origin );
	gt->last_time = now;

	Slayer_GT_AxisAngleToEngineEuler( gt->avel_dir, gt->accum_theta, ent->angles );
	Slayer_GT_CompensatePivot( ent );
}

