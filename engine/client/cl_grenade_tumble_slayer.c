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
#include "cl_spin_phys_slayer.h"
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

// Opt-in escape hatch for the compensation above, and the reason the cvar alone
// is not enough: `slayer_grenade_pivot_fix` is FCVAR_ARCHIVE, so a value stored
// by an older build survives the newer default of 0. The device config still had
// `slayer_grenade_pivot_fix "2"`, and the log showed shift=(-6.1 3.6 -3.8) — the
// grenade drawn six units away from where it lay, which is precisely the
// "the centre of rotation is offset, it jerks on the ground" report.
//
// Migrating the archived value at init cannot fix this: cvars are registered in
// CL_Init but `exec config.cfg` runs afterwards, so the stored value is applied
// again right after any migration lowered it. The only order-independent fix is
// to CLAMP at the point of use, which is what Slayer_GT_PivotEnabled does.
static CVAR_DEFINE_AUTO( slayer_grenade_pivot_allow,
	"0", FCVAR_ARCHIVE,
	"Slayer3D: allow slayer_grenade_pivot_fix to actually move the entity (0 = clamp it off)" );

static CVAR_DEFINE_AUTO( slayer_grenade_diag,
	"0", FCVAR_ARCHIVE,
	"Slayer3D: grenade tumble diagnostics to slayer_diag.log (0=off, 1=on, 2=on+rejected models)" );

// Spin per unit of throw speed. Default comes from the shared core; this exists
// so the feel can be tuned live without a rebuild. 0 = use the core's default.
static CVAR_DEFINE_AUTO( slayer_grenade_spin,
	"0", FCVAR_ARCHIVE,
	"Slayer3D: grenade spin per unit of throw speed (0 = default ~1.3 turns/sec at a hard throw)" );

// How much of a collision's friction becomes spin. Negative = core default.
static CVAR_DEFINE_AUTO( slayer_grenade_grip,
	"-1", FCVAR_ARCHIVE,
	"Slayer3D: how strongly a bounce changes grenade spin (0..1, -1 = default)" );

// =============================================================================
// Tunables
// =============================================================================

#define GT_MAX_SLOTS  32      // ~rarely more than a handful of grenades in flight
#define GT_LIFETIME   5.0f    // sec: slot reclaimed if not refreshed
#define GT_MAX_SPEED  600.0f  // hammer units / sec — typical strong throw

// =============================================================================
// Per-entity tumble state
// =============================================================================

typedef struct
{
	int       index;        // engine entity index, 0 = empty slot
	float     last_time;    // cl.time of last update (also slot expiry)
	vec3_t    last_origin;  // for linear velocity estimation
	vec3_t    vel;          // low-passed velocity handed to the spin core

	// ORIENTATION AND ANGULAR VELOCITY now live in the shared spin core
	// (cl_spin_phys_slayer.c), which is also what dropped weapons and shields
	// use. Two reasons this is not local code any more:
	//
	//  * the previous model computed `rate = f(speed)` with the axis re-derived
	//    from the current velocity, and that cannot express a wall changing the
	//    spin, a grenade that keeps turning after it slows, or friction bleeding
	//    the spin off while it rolls. Angular velocity has to be STATE;
	//  * every consumer needs the same invariants (quaternion renormalized every
	//    frame, spin clamped, rest latched with hysteresis). Duplicating those
	//    is how they drift apart.
	slayer_spin_t spin;
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

/*
====================
Slayer_GT_PivotEnabled

Is the pivot compensation allowed to move the entity this frame?

Clamped here rather than migrated at init, for the reason spelled out at the
cvar: an archived value is re-applied by config.cfg AFTER cvar registration, so
no amount of fixing it up during init survives. Clamping at the point of use is
order-independent — a config may set the cvar to anything and the entity still
stays where the server put it unless slayer_grenade_pivot_allow says otherwise.
====================
*/
static qboolean Slayer_GT_PivotEnabled( void )
{
	if( slayer_grenade_pivot_fix.value == 0.0f )
		return false;

	return ( slayer_grenade_pivot_allow.value != 0.0f );
}

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

// =============================================================================
// Parameters and contact tracing
// =============================================================================

// Tuning handed to the shared core. Read from cvars each call rather than cached
// so a change takes effect on the next grenade without a map reload.
static void Slayer_GrenadeTumble_Params( slayer_spin_params_t *p )
{
	Slayer_Spin_DefaultParams( p );

	if( slayer_grenade_spin.value > 0.0f )
		p->throw_spin = slayer_grenade_spin.value;
	if( slayer_grenade_grip.value >= 0.0f )
		p->impact_grip = slayer_grenade_grip.value;
}

// What is the grenade touching?
//
// Traced here rather than inside the core because tracing is engine work and the
// core must stay host-testable. One short downward trace per grenade per frame:
// grenades in flight are a handful at most, and the alternative -- guessing
// contact from the velocity -- cannot tell "rolling on the floor" from "flying
// horizontally", which is exactly the distinction that makes rolling look right.
static void Slayer_GT_TraceContact( struct cl_entity_s *ent,
	const slayer_spin_params_t *p, slayer_spin_contact_t *out )
{
	vec3_t    start, end;
	pmtrace_t tr;

	memset( out, 0, sizeof( *out ));

	VectorCopy( ent->origin, start );
	VectorCopy( ent->origin, end );

	// Probe a little more than the radius: at 60 fps a grenade rolling at
	// 200 u/s moves ~3 units per frame, so a probe exactly one radius long
	// would flicker between hit and miss on a slightly uneven floor.
	end[2] -= p->radius * 1.6f;

	tr = CL_TraceLine( start, end, PM_STUDIO_IGNORE );

	if( tr.fraction < 1.0f )
	{
		out->on_ground = 1;
		VectorCopy( tr.plane.normal, out->normal );

		// The same surface is the impact surface for a bounce arriving this
		// frame. A separate forward trace would be more accurate for a wall hit,
		// but it costs a second trace per grenade per frame and the core falls
		// back to the velocity change when no normal is supplied -- which is the
		// case that matters for walls, since a wall hit does not put the grenade
		// on the ground.
		out->has_impact_normal = 1;
		VectorCopy( tr.plane.normal, out->impact_normal );
	}
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

// Convert the spin core's orientation into the engine's Euler angles.
//
// A thin wrapper over the SHARED conversion in cl_spin_phys_engine.c. It is
// shared rather than local because the dropped-item module needs exactly the
// same thing, including undoing the renderer's pitch negation, and two copies of
// that compensation would drift apart -- it is subtle enough that getting it
// wrong already cost one round of "grenades still tumble crookedly".
static void Slayer_GT_PoseToAngles( const grenade_tumble_t *gt, vec3_t out_angles )
{
	Slayer_Spin_PoseToAngles( &gt->spin, out_angles );
}

static void Slayer_GT_InitSlot( grenade_tumble_t *gt, struct cl_entity_s *ent, float now )
{
	slayer_spin_params_t p;
	vec4_t seed_orient;
	vec3_t seed_angles;

	gt->index  = ent->index;
	gt->inited = true;

	// Start from the pose the server gave the entity rather than from identity.
	// Otherwise a grenade visibly SNAPS to a new orientation on the first frame
	// we take it over, and again on every teleport/index-reuse reseed.
	VectorCopy( ent->angles, seed_angles );
	AngleQuaternion( seed_angles, seed_orient, false );

	// No velocity sample yet on a fresh slot: velocity here is differentiated
	// from render positions, so it takes two frames to exist. Pass NOTHING
	// rather than gt->vel -- on a recycled slot that field still holds the
	// PREVIOUS grenade's velocity, which would seed this one's spin from a throw
	// that never happened.
	//
	// The throw impulse is applied by the core during its spin-up window instead
	// (see slayer_spin_t::spun_up). Seeding with a velocity of zero used to make
	// the core latch `resting`, so the impulse was never applied at all and
	// grenades only span up once they touched something.
	Slayer_GrenadeTumble_Params( &p );
	Slayer_Spin_Seed( &gt->spin, seed_orient, NULL, ent->index, &p );

	VectorClear( gt->vel );

	// IMPORTANT: read ent->origin (post-interp render position), NOT
	// ent->curstate.origin (raw snapshot, only updates at server tickrate).
	// Using curstate.origin here would make speed estimation degenerate
	// because dt is per-render-frame (~16ms) while curstate.origin only
	// changes per-snapshot (~50ms) — most frames see delta=0.
	VectorCopy( ent->origin, gt->last_origin );
	gt->last_time = now;
}

// Write the current pose into the entity without advancing it. Used on the
// frames where there is nothing to integrate (fresh slot, zero dt, teleport
// reseed) -- those must go through the SAME conversion, including the pitch
// flip, or the grenade would jump between "just spawned" and "tumbling" frames.
static void Slayer_GT_ApplyPose( grenade_tumble_t *gt, vec3_t out_angles )
{
	Slayer_GT_PoseToAngles( gt, out_angles );
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
	if( !Slayer_GT_PivotEnabled() && slayer_grenade_diag.value < 1.0f )
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
	// off by default, and Slayer_GT_PivotEnabled for why an archived value
	// cannot re-enable it on its own.
	if( Slayer_GT_PivotEnabled() )
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
	Cvar_RegisterVariable( &slayer_grenade_pivot_allow );
	Cvar_RegisterVariable( &slayer_grenade_diag );
	Cvar_RegisterVariable( &slayer_grenade_spin );
	Cvar_RegisterVariable( &slayer_grenade_grip );

	Cmd_AddCommand( "slayer_quickthrow", Cmd_SlayerQuickThrow_f,
		"Slayer3D: one-button grenade quick throw — slot4 by default; pass "
		"weapon_hegrenade / weapon_flashbang / weapon_smokegrenade for direct "
		"selection. Example: bind v \"slayer_quickthrow\"" );

	for( i = 0; i < GT_MAX_SLOTS; i++ )
	{
		slayer_spin_params_t p;

		gt_slots[i].index     = 0;
		gt_slots[i].inited    = false;
		gt_slots[i].last_time = 0.0f;
		VectorClear( gt_slots[i].last_origin );
		VectorClear( gt_slots[i].vel );

		// Seed rather than memset: the spin state must start with an IDENTITY
		// quaternion, and an all-zero quaternion is not a rotation at all --
		// converting one produces a degenerate matrix. Seeding is also the only
		// place that knows what "empty" means for the core.
		Slayer_GrenadeTumble_Params( &p );
		Slayer_Spin_Seed( &gt_slots[i].spin, NULL, NULL, i, &p );
	}
}

void Slayer_GrenadeTumble_Apply( struct cl_entity_s *ent )
{
	grenade_tumble_t *gt;
	float             now;
	float             dt;
	vec3_t            delta;
	float             speed;

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
		Slayer_GT_ApplyPose( gt, ent->angles );
		Slayer_GT_CompensatePivot( ent );
		return;
	}

	dt = now - gt->last_time;
	if( dt <= 0.0f )
	{
		// same-frame double call (e.g. multiple visible passes): just reapply
		Slayer_GT_ApplyPose( gt, ent->angles );
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
		Slayer_GT_ApplyPose( gt, ent->angles );
		Slayer_GT_CompensatePivot( ent );
		return;
	}

	// --- Velocity, then hand it to the shared spin core -----------------------
	//
	// The raw per-frame velocity is a difference of two INTERPOLATED positions,
	// so it is noisy by construction. It is low-passed here rather than inside
	// the core because the noise is a property of how WE sample it (render
	// positions, not server state) -- a dropped weapon fed from a different
	// source should not inherit this filter.
	{
		vec3_t sample;
		float  k = 10.0f * dt;   // ~100 ms time constant
		float  s = speed;

		// Clamp the SAMPLE before it enters the filter. A low-pass limits how
		// fast the output moves, not how far: the harness showed a single
		// 3000 u/s frame (ordinary interpolation garbage) dragging the filtered
		// value past the maximum in one step and doubling the tumble rate. The
		// physical throw speed is bounded, so an out-of-range sample is not data.
		if( s > GT_MAX_SPEED && s > 0.0f )
			VectorScale( delta, GT_MAX_SPEED / ( s * dt ), sample );
		else
			VectorScale( delta, 1.0f / dt, sample );

		if( k > 1.0f ) k = 1.0f;
		gt->vel[0] += k * ( sample[0] - gt->vel[0] );
		gt->vel[1] += k * ( sample[1] - gt->vel[1] );
		gt->vel[2] += k * ( sample[2] - gt->vel[2] );
	}

	// Contact, traced once per frame. This is what lets the core tell rolling
	// from flying and gives a real surface normal for a bounce, instead of
	// guessing one from the velocity change.
	{
		slayer_spin_params_t  params;
		slayer_spin_contact_t contact;

		Slayer_GrenadeTumble_Params( &params );
		Slayer_GT_TraceContact( ent, &params, &contact );
		Slayer_Spin_Step( &gt->spin, gt->vel, dt, &contact, &params );
	}

	Slayer_GT_PoseToAngles( gt, ent->angles );

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

		// omega and impacts, not "rate": with the spin core the interesting
		// numbers are the angular velocity that is being carried and how many
		// collisions have shaped it, since neither is a function of speed now.
		Con_Printf( "[SlayerGT] idx=%d speed=%.0f omega=%.1f rad/s hits=%d rest=%d\n",
			ent->index, speed, Slayer_Spin_Rate( &gt->spin ),
			gt->spin.impacts, Slayer_Spin_IsResting( &gt->spin ));

		Slayer_Log_Printf( "GT idx=%d model=%s speed=%.0f vel=(%.0f %.0f %.0f) "
			"omega=%.2f w=(%.2f %.2f %.2f) hits=%d rest=%d "
			"ang=(%.1f %.1f %.1f) rang=(%.1f %.1f %.1f) "
			"lcen=(%.1f %.1f %.1f) shift=(%.1f %.1f %.1f)",
			ent->index, gt_diag_model[0] ? gt_diag_model : "?",
			speed, gt->vel[0], gt->vel[1], gt->vel[2],
			Slayer_Spin_Rate( &gt->spin ),
			gt->spin.omega[0], gt->spin.omega[1], gt->spin.omega[2],
			gt->spin.impacts, Slayer_Spin_IsResting( &gt->spin ),
			ent->angles[0], ent->angles[1], ent->angles[2],
			gt_diag_rangles[0], gt_diag_rangles[1], gt_diag_rangles[2],
			gt_diag_lcenter[0], gt_diag_lcenter[1], gt_diag_lcenter[2],
			gt_diag_shift[0], gt_diag_shift[1], gt_diag_shift[2] );
	}
}

