/*
cl_item_phys_slayer.c - Slayer3D physical look for dropped items
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

// Dropped weapons, shields and props get the same rotational dynamics grenades
// use (cl_spin_phys_slayer.c), plus one thing grenades do not need: SETTLING.
//
// Why settling matters, in the words of the report that prompted it: a weapon
// lying in a corner in vanilla CS 1.6 can be invisible, because the server picks
// one orientation and the model ends up standing upright inside the wall. The
// position is the server's and must not change -- but the POSE is ours, so the
// item can be laid against the surface it is resting on. Same coordinates, and
// now it reads as an object on the ground.
//
// What this deliberately does NOT do:
//
//   * it does not move the entity. Ever. A pickup radius is server-side, and an
//     item drawn away from where it can be picked up is worse than an ugly pose;
//   * it does not guarantee zero clipping. A studio model is not convex and one
//     trace returns one normal, so a long rifle laid on a narrow ledge can still
//     have a corner inside the geometry. What it does guarantee is that the model
//     leans the right way and sits on top of the surface rather than inside it.

#include "common.h"
#include "client.h"
#include "studio.h"
#include "xash3d_mathlib.h"
#include "cl_item_phys_slayer.h"
#include "cl_spin_phys_slayer.h"
#include "cl_slayer_log.h"

// =============================================================================
// Cvars
// =============================================================================

static CVAR_DEFINE_AUTO( slayer_item_phys, "1", FCVAR_ARCHIVE,
	"Slayer3D: physical pose for dropped weapons and props (0 = server pose)" );

static CVAR_DEFINE_AUTO( slayer_item_settle, "1", FCVAR_ARCHIVE,
	"Slayer3D: lay a resting item against the surface under it (0 = keep it upright)" );

// How fast a resting item eases into the surface pose. Fast enough to be done
// before the player walks up to it, slow enough not to snap visibly.
static CVAR_DEFINE_AUTO( slayer_item_settle_rate, "6.0", FCVAR_ARCHIVE,
	"Slayer3D: how quickly a dropped item leans onto the surface (per second)" );

static CVAR_DEFINE_AUTO( slayer_item_spin, "0", FCVAR_ARCHIVE,
	"Slayer3D: spin per unit of throw speed for dropped items (0 = default)" );

static CVAR_DEFINE_AUTO( slayer_item_diag, "0", FCVAR_ARCHIVE,
	"Slayer3D: dropped-item physics diagnostics (0=off, 1=on, 2=on+rejected models)" );

// =============================================================================
// Tunables
// =============================================================================

#define IP_MAX_SLOTS   48     // a bomb-site floor after a long round holds a lot
#define IP_LIFETIME    8.0f   // sec without an update before a slot is reusable
#define IP_MAX_SPEED   900.0f // units/sec; a thrown weapon is slower than a bullet
#define IP_DIAG_INTERVAL 0.5

// A dropped item is bigger and flatter than a grenade, so it neither spins as
// freely nor rolls: a rifle dumped on the floor slides and stops. Hence a larger
// radius, less spin per unit of speed, and much more drag than the grenade
// defaults from the core.
#define IP_RADIUS      7.0f
#define IP_THROW_SPIN  0.008f
#define IP_ROLL_GRIP   3.0f
#define IP_AIR_DRAG    0.9f
#define IP_SPIN_DRAG   6.0f

// =============================================================================
// State
// =============================================================================

typedef struct
{
	int           index;
	float         last_time;
	vec3_t        last_origin;
	vec3_t        vel;
	slayer_spin_t spin;
	vec3_t        rest_normal;     // surface it settled on
	qboolean      have_rest_normal;
	qboolean      inited;
} item_phys_t;

static item_phys_t ip_slots[IP_MAX_SLOTS];
static double      ip_diag_last_print;
static double      ip_diag_last_reject;

// =============================================================================
// Model classification
// =============================================================================

/*
====================
Slayer_IP_IsLooseItem

Is this entity a dropped item whose pose we may invent?

Matched on the model path, because that is the only thing the client reliably
knows about a server entity: `pev->classname` never reaches us, and entity types
in GoldSrc do not distinguish "weapon on the ground" from "func_wall".

  * `models/w_*.mdl` is the GoldSrc convention for a WORLD model -- the version
    of a weapon that lies on the ground, as opposed to `v_` (view) and `p_`
    (player-held). This covers every weapon, ammo box and the C4 backpack.
  * `models/shield/*` catches the shield, which does not follow the w_ naming.

Grenades are excluded here: they are handled by cl_grenade_tumble_slayer.c, which
runs first, and applying both would integrate the spin twice per frame.
====================
*/
static qboolean Slayer_IP_IsLooseItem( const char *name )
{
	const char *base;

	if( COM_StringEmptyOrNULL( name ))
		return false;

	// Grenades belong to the other module.
	if( Q_strstr( name, "grenade" ) || Q_strstr( name, "flashbang" ))
		return false;

	if( Q_strstr( name, "shield" ))
		return true;

	// Look at the FILE NAME, not the whole path: a server can install models
	// under any directory, and a folder that happens to contain "w_" would
	// otherwise match every model inside it.
	base = Q_strrchr( name, '/' );
	base = base ? base + 1 : name;

	if( !Q_strnicmp( base, "w_", 2 ))
		return true;

	return false;
}

// =============================================================================
// Parameters
// =============================================================================

static void Slayer_IP_Params( slayer_spin_params_t *p )
{
	Slayer_Spin_DefaultParams( p );

	p->radius     = IP_RADIUS;
	p->throw_spin = IP_THROW_SPIN;
	p->roll_grip  = IP_ROLL_GRIP;
	p->air_drag   = IP_AIR_DRAG;
	p->spin_drag  = IP_SPIN_DRAG;

	if( slayer_item_spin.value > 0.0f )
		p->throw_spin = slayer_item_spin.value;
}

// =============================================================================
// Slots
// =============================================================================

static item_phys_t *Slayer_IP_GetSlot( int index )
{
	int   i;
	int   empty = -1;
	int   oldest = 0;
	float oldest_time = 1e9f;

	for( i = 0; i < IP_MAX_SLOTS; i++ )
	{
		if( ip_slots[i].inited && ip_slots[i].index == index )
			return &ip_slots[i];

		if( !ip_slots[i].inited || ( cl.time - ip_slots[i].last_time ) > IP_LIFETIME )
		{
			if( empty < 0 )
				empty = i;
		}

		if( ip_slots[i].last_time < oldest_time )
		{
			oldest_time = ip_slots[i].last_time;
			oldest = i;
		}
	}

	if( empty >= 0 )
		return &ip_slots[empty];

	// More loose items visible at once than we track: recycle the least recently
	// updated one. It will reseed from the server pose, which is a single frame
	// of a slightly wrong lean on the least relevant item.
	return &ip_slots[oldest];
}

static void Slayer_IP_Seed( item_phys_t *ip, struct cl_entity_s *ent, float now )
{
	slayer_spin_params_t p;
	vec4_t seed_orient;
	vec3_t seed_angles;

	ip->index  = ent->index;
	ip->inited = true;

	// Start from the server's pose. Starting from identity would make every item
	// visibly snap the first frame it becomes visible, which is far more
	// noticeable than the wrong lean it is meant to fix.
	VectorCopy( ent->angles, seed_angles );
	AngleQuaternion( seed_angles, seed_orient, false );

	Slayer_IP_Params( &p );
	Slayer_Spin_Seed( &ip->spin, seed_orient, NULL, ent->index, &p );

	VectorClear( ip->vel );
	VectorClear( ip->rest_normal );
	ip->have_rest_normal = false;

	VectorCopy( ent->origin, ip->last_origin );
	ip->last_time = now;
}

// =============================================================================
// Contact
// =============================================================================

/*
====================
Slayer_IP_TraceContact

One downward trace per item per frame: enough to know whether it is resting and
on what.

Not a box trace: PM_PlayerTraceExt with usehull 2 (what CL_TraceLine does) is a
point trace against the world, which is what we want here. A hull trace would
report the hull's contact, and the hull of a dropped weapon in GoldSrc is a
generic box unrelated to the visible model.
====================
*/
static void Slayer_IP_TraceContact( struct cl_entity_s *ent,
	const slayer_spin_params_t *p, slayer_spin_contact_t *out )
{
	vec3_t    start, end;
	pmtrace_t tr;

	memset( out, 0, sizeof( *out ));

	VectorCopy( ent->origin, start );
	VectorCopy( ent->origin, end );

	// Probe a bit beyond the radius so a slightly uneven floor does not flicker
	// between contact and no contact between frames.
	end[2] -= p->radius * 1.5f;

	tr = CL_TraceLine( start, end, PM_STUDIO_IGNORE );

	if( tr.fraction < 1.0f )
	{
		out->on_ground = 1;
		VectorCopy( tr.plane.normal, out->normal );
		out->has_impact_normal = 1;
		VectorCopy( tr.plane.normal, out->impact_normal );
	}
}

/*
====================
Slayer_IP_FindLeanSurface

Which surface should a resting item lean against?

The floor under it is the common answer and comes free with the contact trace.
But the case actually reported -- "a weapon in a corner I cannot see" -- is the
one where the interesting surface is a WALL, so four short horizontal probes are
made as well, and the nearest hit wins.

Cheap on purpose: four traces, only while an item is at rest, and only until it
has settled. A resting item is not re-traced every frame.
====================
*/
static qboolean Slayer_IP_FindLeanSurface( struct cl_entity_s *ent, float radius,
	vec3_t out_normal )
{
	static const float dirs[4][3] =
	{
		{  1.0f,  0.0f, 0.0f },
		{ -1.0f,  0.0f, 0.0f },
		{  0.0f,  1.0f, 0.0f },
		{  0.0f, -1.0f, 0.0f },
	};
	vec3_t    start;
	float     best = 1e9f;
	qboolean  found = false;
	int       i;

	VectorCopy( ent->origin, start );

	for( i = 0; i < 4; i++ )
	{
		vec3_t    end;
		pmtrace_t tr;
		float     dist;

		end[0] = start[0] + dirs[i][0] * radius * 2.0f;
		end[1] = start[1] + dirs[i][1] * radius * 2.0f;
		end[2] = start[2];

		tr = CL_TraceLine( start, end, PM_STUDIO_IGNORE );
		if( tr.fraction >= 1.0f )
			continue;

		dist = tr.fraction * radius * 2.0f;
		if( dist < best )
		{
			best = dist;
			VectorCopy( tr.plane.normal, out_normal );
			found = true;
		}
	}

	// Only lean on a wall that is genuinely close. A wall two radii away is not
	// what the item is resting against, and leaning on it would look arbitrary.
	if( found && best > radius * 1.2f )
		found = false;

	return found;
}

// =============================================================================
// Public API
// =============================================================================

void Slayer_ItemPhys_Init( void )
{
	int i;

	Cvar_RegisterVariable( &slayer_item_phys );
	Cvar_RegisterVariable( &slayer_item_settle );
	Cvar_RegisterVariable( &slayer_item_settle_rate );
	Cvar_RegisterVariable( &slayer_item_spin );
	Cvar_RegisterVariable( &slayer_item_diag );

	for( i = 0; i < IP_MAX_SLOTS; i++ )
	{
		slayer_spin_params_t p;

		memset( &ip_slots[i], 0, sizeof( ip_slots[i] ));

		// Seed rather than rely on the memset: an all-zero quaternion is not a
		// rotation and produces a degenerate matrix if it ever reaches one.
		Slayer_IP_Params( &p );
		Slayer_Spin_Seed( &ip_slots[i].spin, NULL, NULL, i, &p );
	}
}

void Slayer_ItemPhys_Reset( void )
{
	int i;

	for( i = 0; i < IP_MAX_SLOTS; i++ )
	{
		ip_slots[i].inited = false;
		ip_slots[i].index = 0;
		ip_slots[i].last_time = 0.0f;
	}

	ip_diag_last_print = 0.0;
	ip_diag_last_reject = 0.0;
}

void Slayer_ItemPhys_Apply( struct cl_entity_s *ent )
{
	item_phys_t          *ip;
	slayer_spin_params_t  params;
	slayer_spin_contact_t contact;
	float  now, dt, speed;
	vec3_t delta;

	if( slayer_item_phys.value == 0.0f )
		return;

	if( !ent || !ent->model )
		return;

	// Players are never loose items, and a player model can be named anything.
	if( ent->player )
		return;

	if( !Slayer_IP_IsLooseItem( ent->model->name ))
	{
		if( slayer_item_diag.value >= 2.0f
		 && cl.time - ip_diag_last_reject >= IP_DIAG_INTERVAL )
		{
			ip_diag_last_reject = cl.time;
			Slayer_Log_Printf( "IP rejected model: %s", ent->model->name );
		}
		return;
	}

	now = cl.time;
	ip  = Slayer_IP_GetSlot( ent->index );
	if( !ip )
		return;

	Slayer_IP_Params( &params );

	if( !ip->inited || ip->index != ent->index || ( now - ip->last_time ) > IP_LIFETIME )
	{
		Slayer_IP_Seed( ip, ent, now );
		Slayer_Spin_PoseToAngles( &ip->spin, ent->angles );
		return;
	}

	dt = now - ip->last_time;
	if( dt <= 0.0f )
	{
		// Same frame, second visibility pass: reapply, do not integrate twice.
		Slayer_Spin_PoseToAngles( &ip->spin, ent->angles );
		return;
	}
	if( dt > 0.5f )
	{
		// Loading screen or demo seek: freeze the pose rather than integrating a
		// half-second step, which would spin the item through several turns.
		ip->last_time = now;
		VectorCopy( ent->origin, ip->last_origin );
		Slayer_Spin_PoseToAngles( &ip->spin, ent->angles );
		return;
	}

	VectorSubtract( ent->origin, ip->last_origin, delta );
	speed = VectorLength( delta ) / dt;

	// Entity index reuse or a teleport: the previous item's resting place has
	// nothing to do with this one, and the implied velocity would be enormous.
	if( speed > IP_MAX_SPEED * 2.0f )
	{
		Slayer_IP_Seed( ip, ent, now );
		Slayer_Spin_PoseToAngles( &ip->spin, ent->angles );
		return;
	}

	// Velocity, low-passed on OUR side: it is differentiated from interpolated
	// render positions, so the noise is a property of how we sample it, not of
	// the item. The core must not inherit this filter -- a different source of
	// velocity would not need it.
	{
		vec3_t sample;
		float  k = 10.0f * dt;

		if( speed > IP_MAX_SPEED && speed > 0.0f )
			VectorScale( delta, IP_MAX_SPEED / ( speed * dt ), sample );
		else
			VectorScale( delta, 1.0f / dt, sample );

		if( k > 1.0f ) k = 1.0f;
		ip->vel[0] += k * ( sample[0] - ip->vel[0] );
		ip->vel[1] += k * ( sample[1] - ip->vel[1] );
		ip->vel[2] += k * ( sample[2] - ip->vel[2] );
	}

	Slayer_IP_TraceContact( ent, &params, &contact );
	Slayer_Spin_Step( &ip->spin, ip->vel, dt, &contact, &params );

	// --- settle -------------------------------------------------------------
	//
	// Only once the item has stopped. Leaning it while it is still moving would
	// fight the tumble, and the result reads as the model twitching rather than
	// falling.
	if( slayer_item_settle.value != 0.0f && Slayer_Spin_IsResting( &ip->spin ))
	{
		// The surface is found ONCE and remembered. Re-tracing every frame would
		// spend four traces per resting item forever, and a resting item's
		// surroundings do not change.
		if( !ip->have_rest_normal )
		{
			if( Slayer_IP_FindLeanSurface( ent, params.radius, ip->rest_normal ))
			{
				ip->have_rest_normal = true;
			}
			else if( contact.on_ground )
			{
				VectorCopy( contact.normal, ip->rest_normal );
				ip->have_rest_normal = true;
			}
		}

		if( ip->have_rest_normal )
		{
			float rate = slayer_item_settle_rate.value;

			if( rate < 0.1f ) rate = 0.1f;
			if( rate > 30.0f ) rate = 30.0f;

			Slayer_Spin_SettleTo( &ip->spin, ip->rest_normal, rate, dt );
		}
	}
	else
	{
		// Moving again (picked up and re-thrown, or knocked by an explosion):
		// forget the surface so the next rest re-finds it.
		ip->have_rest_normal = false;
	}

	Slayer_Spin_PoseToAngles( &ip->spin, ent->angles );

	VectorCopy( ent->origin, ip->last_origin );
	ip->last_time = now;

	if( slayer_item_diag.value >= 1.0f
	 && cl.time - ip_diag_last_print >= IP_DIAG_INTERVAL )
	{
		ip_diag_last_print = cl.time;

		Slayer_Log_Printf( "IP idx=%d model=%s speed=%.0f omega=%.2f hits=%d rest=%d "
			"lean=%d n=(%.2f %.2f %.2f) ang=(%.1f %.1f %.1f)",
			ent->index, ent->model->name, speed,
			Slayer_Spin_Rate( &ip->spin ), ip->spin.impacts,
			Slayer_Spin_IsResting( &ip->spin ), (int)ip->have_rest_normal,
			ip->rest_normal[0], ip->rest_normal[1], ip->rest_normal[2],
			ent->angles[0], ent->angles[1], ent->angles[2] );
	}
}
