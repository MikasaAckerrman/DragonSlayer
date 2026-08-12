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

// How far from flat still counts as "resting", in degrees.
//
// Zero slack is what made a dropped weapon appear to straighten itself out after
// it landed: any pose that was not exactly aligned got corrected, including the
// plausible ones. With slack, only a grossly wrong pose is touched -- and an item
// bridging a step stays leaning on the edge, because that is within tolerance of
// both surfaces and neither pulls it flat.
static CVAR_DEFINE_AUTO( slayer_item_settle_tol, "35", FCVAR_ARCHIVE,
	"Slayer3D: how far from flat a resting item may lie before it is corrected (degrees)" );

static CVAR_DEFINE_AUTO( slayer_item_spin, "0", FCVAR_ARCHIVE,
	"Slayer3D: spin per unit of throw speed for dropped items (0 = default)" );

// The shield is not a rifle. Its model is roughly a metre across, so with the
// generic 7-unit radius the rolling term (omega = (n x v)/r) makes it spin about
// twice as fast as it should for the speed it is sliding at, and it looks like it
// is skittering. Separate parameters rather than one radius for everything.
static CVAR_DEFINE_AUTO( slayer_shield_radius, "16.0", FCVAR_ARCHIVE,
	"Slayer3D: effective rolling radius for a dropped shield (units)" );

static CVAR_DEFINE_AUTO( slayer_shield_spin, "0.005", FCVAR_ARCHIVE,
	"Slayer3D: spin per unit of throw speed for a dropped shield (0 = same as items)" );

// How many points along the item are probed to decide which way it leans. One
// trace gives one normal, and a rifle bridging a step then tilts fully onto
// whichever surface happens to be under its origin while its other end sinks into
// the geometry. Three probes (middle, nose, tail) average out to the surface the
// item actually spans. 1 restores the old single-trace behaviour.
static CVAR_DEFINE_AUTO( slayer_item_lean_probes, "3", FCVAR_ARCHIVE,
	"Slayer3D: surface probes along a resting item (1..3, 1 = single trace)" );

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

// Sleeping. A bomb site late in a round holds dozens of dropped weapons, and
// every one of them was paying for a trace per frame forever just to be told it
// is still lying on the same floor. Once an item has been at rest long enough for
// the settle easing to converge, it is put to sleep: no traces, no integration,
// only the stored pose written back. Waking is decided by comparing the entity
// origin against the position it slept at, which costs nothing.
//
// The delay must OUTLAST the settle easing, or an item falls asleep mid-lean and
// freezes halfway into the floor. The easing is exponential at
// slayer_item_settle_rate per second, so it is within a few percent after ~4 time
// constants -- hence the delay is derived from the rate rather than being a flat
// number, which would break as soon as the cvar was lowered.
#define IP_SLEEP_MIN   0.5f   // sec: floor, so a fast easing still gets a moment
#define IP_SLEEP_TAUS  4.0f   // time constants of easing to wait out
// A resting entity's interpolated origin jitters by a fraction of a unit. Two
// units is comfortably above that and far below any real displacement.
#define IP_WAKE_DIST   2.0f

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
	vec3_t        sleep_origin;    // where it was when it fell asleep
	float         rest_secs;      // seconds spent at rest since it stopped
	qboolean      asleep;          // no traces are made while this is set
	qboolean      inited;
} item_phys_t;

static item_phys_t ip_slots[IP_MAX_SLOTS];
static double      ip_diag_last_print;
static double      ip_diag_last_reject;

// How many world traces this module has made. Only ever read by the diagnostic
// and by the harness, which asserts it stays at zero while items sleep -- the
// whole point of the sleep state is that a floor covered in dropped weapons
// costs nothing, and a counter is the only way to know that stayed true.
static unsigned int ip_traces;

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

// The shield gets its own tuning, so it has to be told apart from a rifle. Same
// substring the classifier above uses, kept as one function so the two cannot
// disagree about what a shield is.
static qboolean Slayer_IP_IsShield( const char *name )
{
	if( COM_StringEmptyOrNULL( name ))
		return false;

	return ( Q_strstr( name, "shield" ) != NULL );
}

// =============================================================================
// Parameters
// =============================================================================

static void Slayer_IP_Params( slayer_spin_params_t *p, qboolean shield )
{
	Slayer_Spin_DefaultParams( p );

	p->radius     = IP_RADIUS;
	p->throw_spin = IP_THROW_SPIN;
	p->roll_grip  = IP_ROLL_GRIP;
	p->air_drag   = IP_AIR_DRAG;
	p->spin_drag  = IP_SPIN_DRAG;

	if( slayer_item_spin.value > 0.0f )
		p->throw_spin = slayer_item_spin.value;

	// The shield last, so its own values win over the generic item override.
	if( shield )
	{
		if( slayer_shield_radius.value > 0.0f )
			p->radius = slayer_shield_radius.value;
		if( slayer_shield_spin.value > 0.0f )
			p->throw_spin = slayer_shield_spin.value;
	}
}

// How long an item must lie still before it is allowed to sleep.
//
// Derived from the settle rate rather than being a constant: the lean eases
// exponentially at that rate, so a lower rate needs a longer wait. A flat number
// would look correct today and freeze items mid-lean the moment somebody lowered
// slayer_item_settle_rate.
static float Slayer_IP_SleepDelay( void )
{
	float rate = slayer_item_settle_rate.value;
	float delay;

	if( rate < 0.1f ) rate = 0.1f;
	if( rate > 30.0f ) rate = 30.0f;

	delay = IP_SLEEP_TAUS / rate;

	if( delay < IP_SLEEP_MIN )
		delay = IP_SLEEP_MIN;

	return delay;
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

	Slayer_IP_Params( &p, Slayer_IP_IsShield( ent->model ? ent->model->name : NULL ));

	// Velocity is unknown on the frame a slot is created (it is differentiated
	// from render positions, so it needs two samples). Pass NOTHING rather than a
	// zero vector: on a recycled slot the field would otherwise carry the
	// PREVIOUS item's velocity. The throw impulse is applied by the core during
	// its spin-up window instead -- seeding a zero velocity used to make the core
	// latch `resting`, which is why dropped items never span up in the air.
	Slayer_Spin_Seed( &ip->spin, seed_orient, NULL, ent->index, &p );

	VectorClear( ip->vel );
	VectorClear( ip->rest_normal );
	ip->have_rest_normal = false;
	ip->asleep = false;
	ip->rest_secs = 0.0f;
	VectorClear( ip->sleep_origin );

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

	ip_traces++;
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

		ip_traces++;
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

/*
====================
Slayer_IP_ModelHalfLength

Half the model's longest horizontal extent, from its clipping hull.

`model->mins/maxs` is the CLIP hull rather than the mesh extent -- the grenade
pivot work established that the hard way -- so it is not trustworthy as an exact
size. It is fine as a SCALE, which is all this needs: the probes only have to
straddle the item, and being 20% off changes nothing about which surfaces they
find.
====================
*/
static float Slayer_IP_ModelHalfLength( struct cl_entity_s *ent, float radius )
{
	float dx, dy, len;

	if( !ent->model )
		return radius;

	dx = ent->model->maxs[0] - ent->model->mins[0];
	dy = ent->model->maxs[1] - ent->model->mins[1];

	len = ( dx > dy ) ? dx : dy;
	len *= 0.5f;

	// A degenerate or absurd hull must not send probes across the map.
	if( len < 2.0f )    len = radius;
	if( len > 48.0f )   len = 48.0f;

	return len;
}

/*
====================
Slayer_IP_FindRestNormal

Which way should a resting item lean?

ONE trace gives ONE normal, and that is wrong exactly where it matters: a rifle
lying half on a step and half on the floor gets whichever surface happens to be
under its origin, so the model tilts fully onto that one and its other end sinks
into the geometry. The reported symptom was items poking through textures at
corners and standing at odd angles on stairs.

So the item is probed at several points ALONG ITS OWN LENGTH -- nose, middle,
tail, taken from the model hull and rotated by the entity's yaw -- and the
normals are averaged. On flat ground every probe agrees and the result is
identical to the single trace. On a 90-degree step the average is the diagonal
between floor and step, which is what an object bridging the two actually rests
at. Where nothing is found below, the horizontal wall probes still apply.

Cost: three traces, once, when an item comes to rest. It then sleeps and traces
nothing at all (see the sleep policy in Slayer_ItemPhys_Apply).
====================
*/
static qboolean Slayer_IP_FindRestNormal( struct cl_entity_s *ent, float radius,
	vec3_t out_normal )
{
	vec3_t forward, right, up;
	vec3_t sum;
	float  half;
	int    probes = (int)slayer_item_lean_probes.value;
	int    hits = 0;
	int    i;

	if( probes < 1 ) probes = 1;
	if( probes > 3 ) probes = 3;

	VectorClear( sum );

	AngleVectors( ent->angles, forward, right, up );
	half = Slayer_IP_ModelHalfLength( ent, radius );

	for( i = 0; i < probes; i++ )
	{
		vec3_t    start, end;
		pmtrace_t tr;
		float     along;

		// Middle first, then the ends: with probes == 1 this degenerates to
		// exactly the old single downward trace.
		if( i == 0 )      along = 0.0f;
		else if( i == 1 ) along =  half;
		else              along = -half;

		VectorCopy( ent->origin, start );
		start[0] += forward[0] * along;
		start[1] += forward[1] * along;
		start[2] += forward[2] * along;

		VectorCopy( start, end );
		end[2] -= radius * 2.0f;

		ip_traces++;
		tr = CL_TraceLine( start, end, PM_STUDIO_IGNORE );
		if( tr.fraction >= 1.0f )
			continue;

		sum[0] += tr.plane.normal[0];
		sum[1] += tr.plane.normal[1];
		sum[2] += tr.plane.normal[2];
		hits++;
	}

	if( hits > 0 )
	{
		// The average of unit normals is not a unit vector, and two opposed
		// normals (an item wedged in a slot) can cancel out entirely -- in which
		// case there is no meaningful lean and the wall probes should decide.
		if( VectorLength( sum ) > 0.1f )
		{
			VectorNormalize( sum );
			VectorCopy( sum, out_normal );
			return true;
		}
	}

	return Slayer_IP_FindLeanSurface( ent, radius, out_normal );
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
	Cvar_RegisterVariable( &slayer_item_settle_tol );
	Cvar_RegisterVariable( &slayer_item_spin );
	Cvar_RegisterVariable( &slayer_shield_radius );
	Cvar_RegisterVariable( &slayer_shield_spin );
	Cvar_RegisterVariable( &slayer_item_lean_probes );
	Cvar_RegisterVariable( &slayer_item_diag );

	for( i = 0; i < IP_MAX_SLOTS; i++ )
	{
		slayer_spin_params_t p;

		memset( &ip_slots[i], 0, sizeof( ip_slots[i] ));

		// Seed rather than rely on the memset: an all-zero quaternion is not a
		// rotation and produces a degenerate matrix if it ever reaches one.
		Slayer_IP_Params( &p, false );
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
		// A sleeping slot must not survive a map change: the new map's entity at
		// this index would inherit "asleep at these coordinates" and never be
		// simulated. `inited` already forces a reseed, but clearing this keeps
		// the two flags from disagreeing.
		ip_slots[i].asleep = false;
		ip_slots[i].rest_secs = 0.0f;
	}

	ip_diag_last_print = 0.0;
	ip_diag_last_reject = 0.0;
	ip_traces = 0;
}

/*
====================
Slayer_IP_RestAxis

Which body axis of this model should face the surface it rests on?

The one along its SHORTEST extent. An object comes to rest on its largest face,
and the largest face is the one perpendicular to the shortest dimension: a rifle
(long in X, thin in Z) lies flat on its side, not balanced on its butt.

This is why the old settling looked wrong. It aligned the model's local UP to the
floor normal, which for a rifle is arbitrary -- its mesh is authored lying along
its own X, so "up" has nothing to do with how it lies. Hence the report: drop a
weapon and it slowly turns itself into a pose nothing chose.

`model->mins/maxs` is the clip hull rather than the mesh extent, so it is not
exact -- but picking the smallest of three numbers only needs the ORDER to be
right, and for a weapon world model it is.
====================
*/
static void Slayer_IP_RestAxis( struct cl_entity_s *ent, vec3_t out )
{
	float d[3];
	int   i, best = 2;

	VectorClear( out );

	if( !ent->model )
	{
		out[2] = 1.0f;
		return;
	}

	for( i = 0; i < 3; i++ )
	{
		d[i] = ent->model->maxs[i] - ent->model->mins[i];
		if( d[i] < 0.0f ) d[i] = -d[i];
	}

	for( i = 0; i < 3; i++ )
	{
		if( d[i] < d[best] )
			best = i;
	}

	// A hull with no meaningful extents (some server props report all zeroes)
	// gets the historical answer rather than a zero vector.
	if( !( d[best] > 0.01f ))
		best = 2;

	out[best] = 1.0f;
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

	Slayer_IP_Params( &params, Slayer_IP_IsShield( ent->model->name ));

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

	// --- sleep ---------------------------------------------------------------
	//
	// A settled item costs nothing: no traces, no integration, just the stored
	// pose written back. Waking is decided by comparing the entity origin with
	// where it slept, which is free -- the alternative (tracing to check whether
	// the floor is still there) is what this is here to avoid.
	if( ip->asleep )
	{
		VectorSubtract( ent->origin, ip->sleep_origin, delta );

		if( VectorLength( delta ) < IP_WAKE_DIST )
		{
			Slayer_Spin_PoseToAngles( &ip->spin, ent->angles );
			ip->last_time = now;
			VectorCopy( ent->origin, ip->last_origin );
			return;
		}

		// It moved: picked up and re-thrown, or shoved by an explosion. Start
		// over, including the throw impulse -- the next flight is a new throw.
		ip->asleep = false;
		ip->rest_secs = 0.0f;
		ip->have_rest_normal = false;
		ip->spin.spun_up = 0;
		ip->spin.spinup_age = 0.0f;
		ip->spin.spinup_peak = 0.0f;
		VectorClear( ip->vel );
		VectorCopy( ent->origin, ip->last_origin );
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
		ip->rest_secs += dt;

		// The surface is found ONCE and remembered. Re-tracing every frame would
		// spend traces per resting item forever, and a resting item's
		// surroundings do not change.
		if( !ip->have_rest_normal )
		{
			if( Slayer_IP_FindRestNormal( ent, params.radius, ip->rest_normal ))
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
			float  rate = slayer_item_settle_rate.value;
			float  tol;
			vec3_t body_axis;

			if( rate < 0.1f ) rate = 0.1f;
			if( rate > 30.0f ) rate = 30.0f;

			// TOLERANCE, in degrees, converted to the cosine the core wants.
			//
			// This is the fix for "you drop a weapon and once it lands it starts
			// straightening itself out". Easing to exact alignment corrected poses
			// that were already fine; with slack, a plausible pose is left alone
			// and only a grossly wrong one (standing on end inside a step) is
			// touched. It is also what lets an item rest ON AN EDGE: bridging a
			// step is within tolerance of both surfaces, so neither pulls it flat.
			tol = slayer_item_settle_tol.value;
			if( tol < 0.0f ) tol = 0.0f;
			if( tol > 89.0f ) tol = 89.0f;

			Slayer_IP_RestAxis( ent, body_axis );

			Slayer_Spin_SettleAxisTo( &ip->spin, ip->rest_normal, body_axis,
				rate, dt, (float)cos( (double)( tol * ( M_PI / 180.0 ))));
		}

		// Long enough at rest for the lean to have converged: stop paying for it.
		// From here on the item costs one vector compare per frame until it moves.
		if( ip->rest_secs >= Slayer_IP_SleepDelay() )
		{
			ip->asleep = true;
			VectorCopy( ent->origin, ip->sleep_origin );
		}
	}
	else if( Slayer_Spin_IsResting( &ip->spin ))
	{
		// Settling disabled, but resting all the same: still worth sleeping,
		// there is nothing left to compute either way.
		ip->rest_secs += dt;
		if( ip->rest_secs >= Slayer_IP_SleepDelay() )
		{
			ip->asleep = true;
			VectorCopy( ent->origin, ip->sleep_origin );
		}
	}
	else
	{
		ip->rest_secs = 0.0f;
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
			"spun=%d asleep=%d traces=%u lean=%d n=(%.2f %.2f %.2f) ang=(%.1f %.1f %.1f)",
			ent->index, ent->model->name, speed,
			Slayer_Spin_Rate( &ip->spin ), ip->spin.impacts,
			Slayer_Spin_IsResting( &ip->spin ),
			ip->spin.spun_up, (int)ip->asleep, ip_traces,
			(int)ip->have_rest_normal,
			ip->rest_normal[0], ip->rest_normal[1], ip->rest_normal[2],
			ent->angles[0], ent->angles[1], ent->angles[2] );
	}
}
