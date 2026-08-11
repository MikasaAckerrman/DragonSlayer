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
#include "cl_slayer_log.h"

// Last pivot-compensation values, captured for the throttled diagnostic below.
// Written on the main thread by Slayer_GT_CompensatePivot, read by the diag.
static vec3_t gt_diag_lcenter;
static vec3_t gt_diag_shift;
static vec3_t gt_diag_rangles;
static char   gt_diag_model[64];

// =============================================================================
// Cvars
// =============================================================================

static CVAR_DEFINE_AUTO( slayer_grenade_tumble,
	"1", FCVAR_ARCHIVE,
	"Slayer3D: client-side axial grenade tumble proportional to linear speed (0 = off)" );

// Pivot compensation is OFF by default, and the device log is why. It shifts
// the entity by -(R*bbox_center - bbox_center), but `model->mins/maxs` on a
// studio model is the CLIPPING hull, not the visual mesh extent: the log shows
// bbox centres of (0,-2,5) and (1,-2,5) for smoke/flashbang, so the "correction"
// displaced the model by up to 6-8 units several times a second — larger than
// the grenade itself. Worse, a resting grenade froze at shift=(4.5 4.5 -3.6),
// i.e. drawn ~7 units away from where it actually lay. That wobble WAS the bug
// it was written to cure. The mesh is authored around the entity origin, so the
// correct offset here is simply zero.
static CVAR_DEFINE_AUTO( slayer_grenade_pivot_fix,
	"0", FCVAR_ARCHIVE,
	"Slayer3D: grenade pivot compensation (0=off — bbox centre is the clip hull, not the mesh centre)" );

static CVAR_DEFINE_AUTO( slayer_grenade_diag,
	"0", FCVAR_ARCHIVE,
	"Slayer3D: grenade tumble diagnostics to slayer_diag.log (0=off, 1=on, 2=on+rejected models)" );

// =============================================================================
// Tunables
// =============================================================================

#define GT_MAX_SLOTS  32      // ~rarely more than a handful of grenades in flight
#define GT_LIFETIME   5.0f    // sec: slot reclaimed if not refreshed
#define GT_BASE_RATE  360.0f  // deg/sec at GT_MAX_SPEED — one turn per second at a
                              // hard throw. Was 1080 (three turns/sec), which read
                              // as a blur rather than a tumble; the device log
                              // confirmed a full 540 deg between 0.5 s samples.
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
	// ACCUMULATED ORIENTATION as a quaternion.
	//
	// This replaces the old (axis, total_angle) pair, and that pair was the
	// actual bug. The pose was rebuilt every frame as "rotate accum_theta about
	// the CURRENT axis", while the axis itself was re-derived from the flight
	// direction and eased every frame. But rotating 300 degrees about axis A and
	// 300 degrees about a slightly different axis B are completely different
	// orientations, so every tiny axis nudge teleported the whole accumulated
	// rotation into another plane. With the velocity differentiated from
	// interpolated positions (noisy by nature) that happened every single frame:
	// the grenade jittered and flipped instead of tumbling.
	//
	// Integrating incrementally fixes it by construction: the axis only ever
	// affects THIS frame's small delta rotation, and the history is already
	// baked into the quaternion. A bounce simply changes the next increments.
	vec4_t    orient;
	vec3_t    avel_dir;     // unit vector — current tumble axis, eased toward the
	                        // one derived from the flight direction each frame
	float     spin_bias;    // how much spin about the flight line to mix in,
	                        // fixed per grenade so they do not all tumble alike
	float     smooth_speed; // low-passed linear speed, drives the tumble rate
	qboolean  resting;      // hysteresis latch: settled on the ground
	qboolean  inited;
} grenade_tumble_t;

static grenade_tumble_t gt_slots[GT_MAX_SLOTS];

// =============================================================================
// Throttled diagnostic output
// =============================================================================

#define GT_DIAG_INTERVAL 0.5  // seconds between diagnostic prints
static double gt_diag_last_print_l2 = 0.0;  // level-2: active grenade telemetry
static double gt_diag_last_print_l3 = 0.0;  // level-3: rejected model names

// =============================================================================
// Helpers
// =============================================================================

static qboolean Slayer_GT_IsGrenadeModel( const char *name )
{
	if( !name || !name[0] )
		return false;

	// Match on the substrings rather than exact CS 1.6 filenames, so custom and
	// reskinned models on modded servers tumble too — "grenade" or "flashbang"
	// anywhere in a model path only ever means a grenade. This covers the stock
	// w_hegrenade / w_smokegrenade / w_flashbang, the HL1 generic w_grenade, and
	// whatever a server renames them to.
	if( Q_strstr( name, "grenade" ))   return true;
	if( Q_strstr( name, "flashbang" )) return true;
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

	// Start from the pose the server gave the entity rather than from identity.
	// Otherwise a grenade visibly SNAPS to a new orientation on the first frame
	// we take it over, and again on every teleport/index-reuse reseed.
	{
		vec3_t seed_angles;

		VectorCopy( ent->angles, seed_angles );
		AngleQuaternion( seed_angles, gt->orient, false );
		Slayer_GT_QuatNormalize( gt->orient );
	}

	gt->smooth_speed = 0.0f;
	gt->resting = false;

	// Seed axis. Only a starting point now: from the first moving frame onward
	// the axis is derived from the flight direction and eased toward it, so a
	// grenade tumbles end-over-end along its path rather than about whatever
	// direction it happened to be given at spawn.
	axis[0] = COM_RandomFloat( -1.0f, 1.0f );
	axis[1] = COM_RandomFloat( -1.0f, 1.0f );
	axis[2] = COM_RandomFloat( -1.0f, 1.0f );

	if( VectorLength( axis ) < 0.01f )
	{
		axis[0] = 1.0f; axis[1] = 0.0f; axis[2] = 0.0f;
	}
	VectorNormalize( axis );
	VectorCopy( axis, gt->avel_dir );

	// How much spin about the flight line to mix into the tumble. Kept modest
	// so the end-over-end motion stays dominant, and varied per grenade so a
	// handful in the air do not move in lockstep.
	gt->spin_bias = COM_RandomFloat( 0.10f, 0.35f );

	// IMPORTANT: read ent->origin (post-interp render position), NOT
	// ent->curstate.origin (raw snapshot, only updates at server tickrate).
	// Using curstate.origin here would make speed estimation degenerate
	// because dt is per-render-frame (~16ms) while curstate.origin only
	// changes per-snapshot (~50ms) — most frames see delta=0.
	VectorCopy( ent->origin, gt->last_origin );
	gt->last_time = now;
}

// Build a quaternion from axis-angle (axis must be unit, theta in radians).
// Layout matches AngleQuaternion in xash3d_mathlib.h: q = (axis*sin(θ/2), cos(θ/2)).
static void Slayer_GT_QuatFromAxisAngle( const vec3_t axis, float theta, vec4_t q )
{
	float half = theta * 0.5f;
	float s    = sinf( half );

	q[0] = axis[0] * s;
	q[1] = axis[1] * s;
	q[2] = axis[2] * s;
	q[3] = cosf( half );
}

// q = a * b (apply b first, then a). Hamilton product; the engine has no
// quaternion multiply of its own, only slerp and the angle conversions.
static void Slayer_GT_QuatMul( const vec4_t a, const vec4_t b, vec4_t out )
{
	out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// Renormalize. Required, not cosmetic: the orientation is built by multiplying
// one small increment per frame, so float error compounds for as long as the
// grenade lives (a smoke can bounce for many seconds) and an un-normalized
// quaternion turns into a shear/scale in the rotation matrix.
static void Slayer_GT_QuatNormalize( vec4_t q )
{
	float len = sqrtf( q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] );

	if( len < 1e-6f )
	{
		q[0] = q[1] = q[2] = 0.0f;
		q[3] = 1.0f;
		return;
	}

	len = 1.0f / len;
	q[0] *= len; q[1] *= len; q[2] *= len; q[3] *= len;
}

// Advance the stored orientation by `theta` radians about `axis`, then convert
// to the engine's Euler angles.
static void Slayer_GT_Integrate( grenade_tumble_t *gt, const vec3_t axis, float theta,
	vec3_t out_angles )
{
	vec4_t dq, result;

	if( theta != 0.0f )
	{
		Slayer_GT_QuatFromAxisAngle( axis, theta, dq );
		// Increment on the LEFT: the delta is expressed in world space (the
		// tumble axis comes from the world-space velocity), not in the model's
		// own frame.
		Slayer_GT_QuatMul( dq, gt->orient, result );
		Slayer_GT_QuatNormalize( result );
		Vector4Copy( result, gt->orient );
	}

	QuaternionAngle( gt->orient, out_angles );
}

// Compensate for off-center model pivot.
//
// PROBLEM: Studio renderer rotates the mesh around ent->origin, but the
// visual center of the mesh is offset from that origin by bbox_center =
// (mins+maxs)/2. This means the visual center orbits around the trajectory
// at radius |bbox_center| instead of staying on it.
//
// PREVIOUS (WRONG) APPROACH: used bone[0].value[0..2] as the pivot offset.
// bone[0].value is the root bone reference position in bind pose, which is a
// point OUTSIDE the bounding box (e.g. 6.5, 0.6, 4.1 for hegrenade). Using
// it made the orbiting WORSE, not better.
//
// CORRECT FIX: Use the bounding box center (mins+maxs)/2 as L_center. This
// represents how far the visual mesh center is from the entity origin. Then
// shift ent->origin by -(R(angles)*L_center - L_center). After the engine
// renders T(ent_origin') * R(angles) * mesh, the visual center world
// position becomes:
//     ent_origin' + R(angles) * L_center
//   = (ent_origin - R(angles)*L_center + L_center) + R(angles) * L_center
//   = ent_origin + L_center
// -- same as it would be at angles=0 (no rotation), i.e. STABLE across spin.
//
// Even small offsets (e.g. hegrenade center=(0,-2,0), length=2) are worth
// correcting because at high rotation speeds even 2 units of offset produces
// visible wobble.
//
// We mutate ent->origin AFTER the speed estimator has captured the original
// world position into gt->last_origin, so the per-frame velocity used for
// rotation rate is unaffected.
#define GT_DIAG_MAX_MODELS 8

static void Slayer_GT_CompensatePivot( struct cl_entity_s *ent )
{
	vec3_t    L_center, rotated_L, shift;
	matrix3x4 mat;

	// NOTE: the shift is computed even when the compensation is disabled, so the
	// diagnostic can still report what it *would* have done. Only the final
	// application to ent->origin is gated.
	if( !slayer_grenade_pivot_fix.value && slayer_grenade_diag.value < 1.0f )
		return;
	if( !ent->model )
		return;

	// Always use bbox center as the pivot offset
	VectorAverage( ent->model->mins, ent->model->maxs, L_center );

	// Apply even for small offsets (|L_center| >= 1.0 still produces visible
	// wobble at high rotation speeds), but skip truly negligible cases
	if( DotProduct( L_center, L_center ) < 0.01f )
		return;

	// Pure rotation matrix from current angles (translation = origin = 0).
	//
	// CRITICAL: this must be the SAME rotation the studio renderer will apply,
	// or the compensation shifts the origin in the wrong direction and the mesh
	// orbits instead of staying put. R_StudioSetUpTransform (ref/gl/gl_studio.c)
	// negates PITCH before building its matrix — the inherited "stupid quake
	// bug" — unless ENGINE_COMPENSATE_QUAKE_BUG is set. Building the matrix from
	// ent->angles verbatim, as this used to, only agrees with the renderer while
	// pitch is 0. During a tumble pitch is almost never 0, which is exactly when
	// the wobble showed up.
	{
		vec3_t r_angles;

		VectorCopy( ent->angles, r_angles );
		if( !FBitSet( host.features, ENGINE_COMPENSATE_QUAKE_BUG ))
			r_angles[PITCH] = -r_angles[PITCH];

		Matrix3x4_CreateFromEntity( mat, r_angles, vec3_origin, 1.0f );
		VectorCopy( r_angles, gt_diag_rangles );
	}
	Matrix3x4_VectorRotate( mat, L_center, rotated_L );

	VectorSubtract( rotated_L, L_center, shift );

	// Apply only when explicitly enabled — see the cvar comment for why this is
	// off by default.
	if( slayer_grenade_pivot_fix.value )
		VectorSubtract( ent->origin, shift, ent->origin );

	// Capture for the throttled diagnostic (see the tumble step below).
	VectorCopy( L_center, gt_diag_lcenter );
	VectorCopy( shift, gt_diag_shift );
	Q_strncpy( gt_diag_model, ent->model->name, sizeof( gt_diag_model ));

	// Diagnostic one-shot print (only when cvar >= 2)
	if( slayer_grenade_diag.value >= 1.0f )
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
			file_t        *f;

			if( diag_printed_count < GT_DIAG_MAX_MODELS )
			{
				Q_strncpy( diag_printed_models[diag_printed_count], ent->model->name, MAX_QPATH );
				diag_printed_count++;
			}

			Con_Printf( "[SlayerGT] pivot_fix: using bbox center\n" );
			Con_Printf( "[SlayerGT] model=%s mins=(%.1f %.1f %.1f) maxs=(%.1f %.1f %.1f) bbox_center=(%.1f %.1f %.1f) shift=(%.1f %.1f %.1f)\n",
				ent->model->name,
				ent->model->mins[0], ent->model->mins[1], ent->model->mins[2],
				ent->model->maxs[0], ent->model->maxs[1], ent->model->maxs[2],
				L_center[0], L_center[1], L_center[2],
				shift[0], shift[1], shift[2] );

			phdr = (studiohdr_t *)Mod_StudioExtradata( ent->model );
			if( phdr && phdr->numbones > 0 )
			{
				pbones = (mstudiobone_t *)((byte *)phdr + phdr->boneindex);
				Con_Printf( "[SlayerGT] (info) bone[0] \"%s\" pos=(%.1f %.1f %.1f) -- NOT used for pivot\n",
					pbones[0].name,
					pbones[0].value[0], pbones[0].value[1], pbones[0].value[2] );
			}

			// Also write diagnostics to a file for retrieval on Android
			f = FS_Open( "slayer_diag.log", "a", false );
			if( f )
			{
				FS_Printf( f, "[SlayerGT] pivot_fix: using bbox center\n" );
				FS_Printf( f, "[SlayerGT] model=%s mins=(%.1f %.1f %.1f) maxs=(%.1f %.1f %.1f) bbox_center=(%.1f %.1f %.1f) shift=(%.1f %.1f %.1f)\n",
					ent->model->name,
					ent->model->mins[0], ent->model->mins[1], ent->model->mins[2],
					ent->model->maxs[0], ent->model->maxs[1], ent->model->maxs[2],
					L_center[0], L_center[1], L_center[2],
					shift[0], shift[1], shift[2] );

				if( phdr && phdr->numbones > 0 )
				{
					pbones = (mstudiobone_t *)((byte *)phdr + phdr->boneindex);
					FS_Printf( f, "[SlayerGT] (info) bone[0] \"%s\" pos=(%.1f %.1f %.1f) -- NOT used for pivot\n",
						pbones[0].name,
						pbones[0].value[0], pbones[0].value[1], pbones[0].value[2] );
				}

				FS_Close( f );
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
	Cvar_RegisterVariable( &slayer_grenade_diag );

	Cmd_AddCommand( "slayer_quickthrow", Cmd_SlayerQuickThrow_f,
		"Slayer3D: one-button grenade quick throw — slot4 by default; pass "
		"weapon_hegrenade / weapon_flashbang / weapon_smokegrenade for direct "
		"selection. Example: bind v \"slayer_quickthrow\"" );

	for( i = 0; i < GT_MAX_SLOTS; i++ )
	{
		gt_slots[i].index        = 0;
		gt_slots[i].inited       = false;
		gt_slots[i].last_time    = 0.0f;
		gt_slots[i].smooth_speed = 0.0f;
		gt_slots[i].resting      = false;
		VectorClear( gt_slots[i].last_origin );
		VectorClear( gt_slots[i].avel_dir );
		// Identity quaternion, not all-zero: a zero quaternion is not a
		// rotation and would produce a degenerate matrix if ever converted.
		gt_slots[i].orient[0] = gt_slots[i].orient[1] = gt_slots[i].orient[2] = 0.0f;
		gt_slots[i].orient[3] = 1.0f;
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
	{
		// Level 3: log rejected (non-grenade) model names, throttled
		if( slayer_grenade_diag.value >= 2.0f && cl.time - gt_diag_last_print_l3 >= GT_DIAG_INTERVAL )
		{
			Con_Printf( "[SlayerGT] rejected model: %s\n", ent->model->name );
			gt_diag_last_print_l3 = cl.time;
		}
		return;
	}

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
		QuaternionAngle( gt->orient, ent->angles );
		Slayer_GT_CompensatePivot( ent );
		return;
	}

	dt = now - gt->last_time;
	if( dt <= 0.0f )
	{
		// same-frame double call (e.g. multiple visible passes): just reapply
		QuaternionAngle( gt->orient, ent->angles );
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
		QuaternionAngle( gt->orient, ent->angles );
		Slayer_GT_CompensatePivot( ent );
		return;
	}

	// --- Speed -> tumble rate -------------------------------------------------
	//
	// The raw speed here is a difference of two INTERPOLATED positions, so it is
	// noisy by construction: the same physical flight produces a value that
	// jumps frame to frame, and around the rest threshold that made the rotation
	// stutter on and off. Two fixes, both about stability rather than looks:
	//
	//   * low-pass the speed, so a single bad frame cannot change the rate much;
	//   * latch "resting" with HYSTERESIS. Without it, a grenade lying on the
	//     ground whose interpolation jitters by a unit or two kept crossing the
	//     single threshold and twitched forever. It now needs to fall well below
	//     the threshold to settle, and to clearly exceed it to start again.
	{
		float k = 10.0f * dt;   // ~100 ms time constant
		float sample = speed;

		// Clamp the SAMPLE before it enters the filter. A low-pass only limits
		// how fast the output moves, not how far: the harness showed a single
		// 3000 u/s frame (ordinary interpolation garbage) dragging the filtered
		// speed past the maximum in one step and doubling the tumble rate. The
		// physical throw speed is bounded, so an out-of-range sample is not data.
		if( sample > GT_MAX_SPEED ) sample = GT_MAX_SPEED;
		if( sample < 0.0f ) sample = 0.0f;

		if( k > 1.0f ) k = 1.0f;
		gt->smooth_speed += k * ( sample - gt->smooth_speed );
	}

	if( gt->resting )
	{
		// Needs a real push to wake up again (a bounce or a kick).
		if( gt->smooth_speed > GT_REST_SPEED * 2.0f )
			gt->resting = false;
	}
	else if( gt->smooth_speed < GT_REST_SPEED * 0.5f )
	{
		gt->resting = true;
	}

	if( gt->resting )
	{
		rate = 0.0f;
	}
	else
	{
		float s = gt->smooth_speed;

		if( s > GT_MAX_SPEED ) s = GT_MAX_SPEED;
		rate = GT_BASE_RATE * ( s / GT_MAX_SPEED );
	}

	// --- Tumble axis, derived from the flight direction -----------------------
	//
	// A thrown object tumbles END-OVER-END across its direction of travel; it
	// does not spin about a fixed axis chosen at random, which is what this used
	// to do and why the motion never looked right no matter how the rate was
	// tuned. So take the axis from the velocity itself:
	//
	//     axis = normalize( velocity x up )
	//
	// Because it is derived rather than stored, a bounce re-aims it for free —
	// the grenade starts tumbling along its new path the moment the velocity
	// turns, exactly as it does in CS:GO.
	//
	// Two refinements keep it from looking mechanical:
	//   * a per-grenade fraction of the velocity direction is blended in, so
	//     each one carries some spin about its own flight line instead of every
	//     grenade tumbling in a perfectly flat plane;
	//   * the axis is eased toward its target rather than snapped, because the
	//     velocity is differentiated from interpolated positions and is noisy
	//     frame to frame.
	if( dt > 0.0f && !gt->resting )
	{
		static const vec3_t gt_up = { 0.0f, 0.0f, 1.0f };
		vec3_t vel, want, flight;
		float  len;

		VectorScale( delta, 1.0f / dt, vel );

		// Perpendicular to the flight path: the end-over-end tumble axis.
		CrossProduct( vel, gt_up, want );
		len = VectorLength( want );

		// Degenerate while falling straight down (velocity parallel to up) —
		// there is no meaningful "across the path" then, so keep the last axis.
		if( len > 1.0f )
		{
			float t = 12.0f * dt;   // ease rate, ~1/12 s to converge

			VectorScale( want, 1.0f / len, want );

			// Blend in spin about the flight line itself, fixed per grenade, so
			// they do not all tumble in one flat plane.
			VectorCopy( vel, flight );
			VectorNormalize( flight );
			VectorMA( want, gt->spin_bias, flight, want );
			VectorNormalize( want );

			if( t > 1.0f ) t = 1.0f;

			// axis += t * (want - axis)
			gt->avel_dir[0] += t * ( want[0] - gt->avel_dir[0] );
			gt->avel_dir[1] += t * ( want[1] - gt->avel_dir[1] );
			gt->avel_dir[2] += t * ( want[2] - gt->avel_dir[2] );
			VectorNormalize( gt->avel_dir );
		}
	}

	// Integrate the delta into the stored orientation. Only the SMALL per-frame
	// rotation uses the current axis, so an axis that drifts (and it always
	// does, because the velocity is differentiated from interpolated positions)
	// can no longer throw the accumulated pose into a different plane. There is
	// nothing to wrap or renormalize by hand either: the quaternion is
	// renormalized inside Slayer_GT_Integrate.
	Slayer_GT_Integrate( gt, gt->avel_dir, DEG2RAD( rate * dt ), ent->angles );

	VectorCopy( ent->origin, gt->last_origin );
	gt->last_time = now;

	Slayer_GT_CompensatePivot( ent );

	// Level 2+: throttled diagnostic. Runs AFTER the angles and the pivot shift
	// are computed, so the log shows what was actually applied this frame, and
	// goes to the Slayer file log so it can be sent back rather than only
	// scrolling past in the console.
	if( slayer_grenade_diag.value >= 1.0f && cl.time - gt_diag_last_print_l2 >= GT_DIAG_INTERVAL )
	{
		gt_diag_last_print_l2 = cl.time;

		Con_Printf( "[SlayerGT] idx=%d speed=%.0f (smooth %.0f) rate=%.0f rest=%d\n",
			ent->index, speed, gt->smooth_speed, rate, (int)gt->resting );

		Slayer_Log_Printf( "GT idx=%d model=%s speed=%.0f smooth=%.0f rate=%.0f rest=%d "
			"axis=(%.2f %.2f %.2f) ang=(%.1f %.1f %.1f) rang=(%.1f %.1f %.1f) "
			"lcen=(%.1f %.1f %.1f) shift=(%.1f %.1f %.1f)",
			ent->index, gt_diag_model[0] ? gt_diag_model : "?",
			speed, gt->smooth_speed, rate, (int)gt->resting,
			gt->avel_dir[0], gt->avel_dir[1], gt->avel_dir[2],
			ent->angles[0], ent->angles[1], ent->angles[2],
			gt_diag_rangles[0], gt_diag_rangles[1], gt_diag_rangles[2],
			gt_diag_lcenter[0], gt_diag_lcenter[1], gt_diag_lcenter[2],
			gt_diag_shift[0], gt_diag_shift[1], gt_diag_shift[2] );
	}
}

