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
#include "cl_model_extent_slayer.h"
#include "cl_item_place_slayer.h"
#include "cl_slayer_log.h"

// =============================================================================
// Cvars
// =============================================================================

static CVAR_DEFINE_AUTO( slayer_item_phys, "0", FCVAR_ARCHIVE,
	"Slayer3D: physical pose for dropped weapons and props (0 = vanilla server pose)" );

// One-shot migration of the archived value. The feature shipped ON and its
// value is in the player's config.cfg, so a new default of 0 cannot take effect
// on its own -- cvars are registered inside CL_Init and `exec config.cfg` runs
// afterwards, re-applying the stored 1.
//
// WHY IT IS OFF NOW: asked for directly -- "давай сделаем выбрасывание оружия
// ванильным без нашей неудачной физики". Three passes went into toppling, edge
// resting and a placement solver, and the result still reads as wrong more often
// than the plain server pose does. The code stays, behind the cvar: the parts
// that were measured (mesh extents, the placement solver) are useful and the
// device is the only place the two can be compared.
static CVAR_DEFINE_AUTO( slayer_item_phys_migrated, "0", FCVAR_ARCHIVE,
	"Slayer3D internal: dropped-item physics default migration completed" );

static CVAR_DEFINE_AUTO( slayer_item_settle, "1", FCVAR_ARCHIVE,
	"Slayer3D: lay a resting item against the surface under it (0 = keep it upright)" );

// How fast a resting item eases into the surface pose. Only used by the OLD
// alignment mechanic (slayer_item_settle_mode 2), kept for live comparison.
static CVAR_DEFINE_AUTO( slayer_item_settle_rate, "6.0", FCVAR_ARCHIVE,
	"Slayer3D: how quickly a dropped item leans onto the surface (legacy mode 2 only)" );

// How far from flat still counts as "resting", in degrees. Legacy mechanic only.
static CVAR_DEFINE_AUTO( slayer_item_settle_tol, "35", FCVAR_ARCHIVE,
	"Slayer3D: tolerance before a resting item is corrected (legacy mode 2 only, degrees)" );

// WHICH RESTING MECHANIC.
//
// 1 (default) = toppling. The item is its real measured box; wherever a corner of
//   that box is inside the surface, the surface pushes it out, at a limited rate.
//   No target orientation exists, so nothing aligns anything: a weapon that landed
//   in a plausible pose is untouched, one standing inside a step tips over, and
//   one bridging a step stays leaning because nothing pushes it flat.
//
// 2 = the previous mechanic: ease the model's shortest axis toward the traced
//   normal. This is what produced "you drop a weapon and it straightens itself out
//   and spins" -- it recomputes a target every frame from a jittering normal, and
//   its single notion of correct (flat against one normal) actively pulls an item
//   off an edge. Kept only so the two can be compared on the device.
//
// 0 = no resting behaviour at all; the pose is whatever the tumble left.
static CVAR_DEFINE_AUTO( slayer_item_settle_mode, "1", FCVAR_ARCHIVE,
	"Slayer3D: resting mechanic (0 = none, 1 = toppling, 2 = legacy alignment)" );

// Toppling parameters. Defaults measured in tests/settle_proto.py and asserted in
// tests/settle_test.c; see cl_spin_phys_slayer.h for why these particular numbers.
static CVAR_DEFINE_AUTO( slayer_item_topple_rate, "260", FCVAR_ARCHIVE,
	"Slayer3D: fastest a resting item's pose may be corrected (degrees/sec)" );

static CVAR_DEFINE_AUTO( slayer_item_topple_damping, "5.0", FCVAR_ARCHIVE,
	"Slayer3D: contact friction bleeding off a settling item's tumble (per second)" );

static CVAR_DEFINE_AUTO( slayer_item_topple_bounce, "0.10", FCVAR_ARCHIVE,
	"Slayer3D: share of the spin a settling item keeps when a corner lands (0-0.9)" );

static CVAR_DEFINE_AUTO( slayer_item_extent, "1", FCVAR_ARCHIVE,
	"Slayer3D: measure model size from the mesh (0 = use the engine's hull, which has the axes wrong on 24 of 34 stock models)" );

// VISUAL PLACEMENT. The toppling above answers "which way does this box tip",
// which is a good answer to a different question: it reasons about ONE traced
// plane, so it cannot know that the far end of a rifle is inside a step, that the
// model is clipping a wall it never traced toward, or that the origin sits low
// enough that no rotation at all keeps the mesh out of the floor. Those are the
// three things the player actually sees.
//
// The solver in cl_item_place_slayer.c asks the other question -- is the whole box
// outside the world -- and searches poses until it is. 1 = on.
static CVAR_DEFINE_AUTO( slayer_item_place, "1", FCVAR_ARCHIVE,
	"Slayer3D: search for a pose in which the model does not intersect the map (0 = off)" );

// How far the DRAWN position may be nudged to get a model out of the floor.
// Rotation cannot fix a mesh whose origin the server parked below the surface, and
// that is the "half inside the texture" case. Bounded hard: a CS weaponbox pickup
// volume is tens of units across, so a few units cannot move the model out of what
// picks it up, and only the rendered position moves -- never the entity.
static CVAR_DEFINE_AUTO( slayer_item_place_lift, "6", FCVAR_ARCHIVE,
	"Slayer3D: largest visual nudge used to lift a model out of geometry (units, 0 = never move)" );

static CVAR_DEFINE_AUTO( slayer_item_place_yaws, "12", FCVAR_ARCHIVE,
	"Slayer3D: rotations tried when searching for a non-intersecting pose" );

static CVAR_DEFINE_AUTO( slayer_item_place_tilts, "4", FCVAR_ARCHIVE,
	"Slayer3D: tilts tried when searching for a non-intersecting pose" );

static CVAR_DEFINE_AUTO( slayer_item_place_traces, "512", FCVAR_ARCHIVE,
	"Slayer3D: trace budget for one placement search (spent once per item, not per frame)" );

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
	float         rest_dist;       // how far the origin sits above that surface
	qboolean      have_rest_normal;
	int           support;         // SLAYER_SUPPORT_*, last reported
	qboolean      topple_settled;  // the core says the pose is final
	vec3_t        place_offset;    // visual nudge from the placement solver
	qboolean      placed;          // the solver has run for this rest position
	qboolean      place_ok;        // and found a pose with no intersection
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
Slayer_IP_ModelBox

The item's REAL half-extents.

This replaces reading `model->mins/maxs`, and it is a correction rather than a
refinement. Measured over the 34 stock w_*.mdl files plus 25 of the user's
downloaded replacements (tests/mdl_engine_box.py):

    the engine's box picks the WRONG SHORT AXIS on 24 of 34 stock models
    and on 24 of 25 custom ones; worst per-axis size error 2564 %
    w_ak47: engine 2.44 12.31 36.65   real mesh 35.44 15.36 2.44

The cause is in Mod_LoadStudioModel: every one of these models has zero bbmin and
zero min, so the engine computes bounds from RAW vertex positions -- positions in
BONE space, without walking the bone chain. A CS world model has one bone rotated
about 90 degrees, so the axes come out permuted systematically.

Both facts the resting code needs are therefore wrong when taken from the hull:
which axis faces the surface, and how long the item is. That is the measured cause
of both live complaints. See cl_model_extent_slayer.h.

Returns false when nothing usable could be produced, in which case the caller must
fall back rather than proceed with nonsense.
====================
*/
static qboolean Slayer_IP_ModelBox( struct cl_entity_s *ent, vec3_t out_half )
{
	const slayer_model_extent_t *ext;
	int i;

	if( !ent || !ent->model )
		return false;

	if( slayer_item_extent.value != 0.0f )
	{
		ext = Slayer_ModelExtent_Get( ent->model );
		if( ext && ext->valid )
		{
			for( i = 0; i < 3; i++ )
				out_half[i] = ext->half[i];
			return true;
		}
	}

	// Fallback: the engine's hull. Known to have the axes permuted on most models,
	// so this is a "better than nothing" path (a model that failed to parse, or the
	// cvar turned off for comparison), not an equivalent one.
	for( i = 0; i < 3; i++ )
	{
		float d = ent->model->maxs[i] - ent->model->mins[i];

		if( d < 0.0f ) d = -d;
		out_half[i] = d * 0.5f;
	}

	return ( out_half[0] + out_half[1] + out_half[2] ) > 0.01f;
}

/*
====================
Slayer_IP_SettleParams

Toppling parameters from cvars, with the measured defaults as the baseline.
====================
*/
static void Slayer_IP_SettleParams( slayer_spin_settle_params_t *sp )
{
	Slayer_Spin_DefaultSettleParams( sp );

	// Degrees per second in the cvar, radians per second in the core: the cvar is
	// what a player reads and types, the core is what the maths wants.
	if( slayer_item_topple_rate.value > 0.0f )
		sp->topple_rate = DEG2RAD( slayer_item_topple_rate.value );

	if( slayer_item_topple_damping.value >= 0.0f )
		sp->damping = slayer_item_topple_damping.value;

	if( slayer_item_topple_bounce.value >= 0.0f )
		sp->restitution = slayer_item_topple_bounce.value;
}

/*
====================
Slayer_IP_SettleMode

Which resting mechanic is active, clamped.

Read through a function rather than the cvar directly because it is consulted in
three places and an out-of-range value has to mean the same thing in all of them.
====================
*/
static int Slayer_IP_SettleMode( void )
{
	int mode;

	// The old on/off cvar still disables everything, so a config that turned
	// settling off keeps it off.
	if( slayer_item_settle.value == 0.0f )
		return 0;

	mode = (int)slayer_item_settle_mode.value;
	if( mode < 0 ) mode = 0;
	if( mode > 2 ) mode = 1;
	return mode;
}

/*
====================
Slayer_IP_FindSupport

The surface a resting item is supported by, AND how far its origin sits above it.

Both halves matter, and the old code only produced the first. The set of poses an
item may hold is bounded by non-penetration, and that bound is a function of the
origin's height above the surface (see Slayer_Spin_MaxTilt): at 1.3 units above
the floor a real w_ak47 may tilt 0.3 degrees, at 16 units 60.3. A normal without
its distance cannot distinguish "lying on the floor" from "bridging a step", which
is precisely the distinction the player is asking for.

The probes still straddle the item, but along its MEASURED length rather than the
engine hull's -- the hull reports 12.31 units for an AK whose mesh is 35.44, so the
old probes sat three times too close together and could not span a step at all.

Distance is taken from the CLOSEST hit rather than an average: the item rests on
whatever it touches first, and averaging the distance to a floor and to a step
would place the origin at a height where neither exists.
====================
*/
static qboolean Slayer_IP_FindSupport( struct cl_entity_s *ent,
	const slayer_spin_params_t *p, vec3_t out_normal, float *out_dist )
{
	vec3_t forward, right, up;
	vec3_t half;
	vec3_t sum;
	float  span;
	float  best_dist = 1e9f;
	float  probe_len;
	int    probes = (int)slayer_item_lean_probes.value;
	int    hits = 0;
	int    i;

	*out_dist = 0.0f;

	if( probes < 1 ) probes = 1;
	if( probes > 3 ) probes = 3;

	VectorClear( sum );
	AngleVectors( ent->angles, forward, right, up );

	if( Slayer_IP_ModelBox( ent, half ))
		span = half[0] > half[1] ? half[0] : half[1];
	else
		span = p->radius;

	if( span < 1.0f )  span = 1.0f;
	if( span > 64.0f ) span = 64.0f;

	// Trace far enough to find the floor under an item that is standing on end,
	// which is exactly the pose that needs correcting.
	probe_len = span * 2.0f + p->radius * 2.0f;

	for( i = 0; i < probes; i++ )
	{
		vec3_t    start, end;
		pmtrace_t tr;
		float     along;
		float     dist;

		if( i == 0 )      along = 0.0f;
		else if( i == 1 ) along =  span;
		else              along = -span;

		VectorCopy( ent->origin, start );
		start[0] += forward[0] * along;
		start[1] += forward[1] * along;
		start[2] += forward[2] * along;

		VectorCopy( start, end );
		end[2] -= probe_len;

		ip_traces++;
		tr = CL_TraceLine( start, end, PM_STUDIO_IGNORE );
		if( tr.fraction >= 1.0f )
			continue;

		sum[0] += tr.plane.normal[0];
		sum[1] += tr.plane.normal[1];
		sum[2] += tr.plane.normal[2];
		hits++;

		// Height of the ENTITY ORIGIN above this surface. The probe may start off
		// to the side, so what is measured is the probe's own drop; on a flat floor
		// every probe agrees, and on a step the nearest one wins below.
		dist = tr.fraction * probe_len;
		if( dist < best_dist )
			best_dist = dist;
	}

	if( hits > 0 && VectorLength( sum ) > 0.1f )
	{
		VectorNormalize( sum );
		VectorCopy( sum, out_normal );
		*out_dist = ( best_dist < 1e9f ) ? best_dist : 0.0f;
		return true;
	}

	// Nothing below: a weapon lodged against a wall. The wall is the support, and
	// its distance is unknown from a horizontal probe, so zero -- which the core
	// reads as "the surface is right at the origin", the most constrained case and
	// the safe one.
	if( Slayer_IP_FindLeanSurface( ent, p->radius, out_normal ))
	{
		*out_dist = 0.0f;
		return true;
	}

	return false;
}

// =============================================================================
// Public API
// =============================================================================

void Slayer_ItemPhys_Init( void )
{
	int i;

	Cvar_RegisterVariable( &slayer_item_phys );
	Cvar_RegisterVariable( &slayer_item_phys_migrated );
	Cvar_RegisterVariable( &slayer_item_settle );
	Cvar_RegisterVariable( &slayer_item_settle_mode );
	Cvar_RegisterVariable( &slayer_item_settle_rate );
	Cvar_RegisterVariable( &slayer_item_settle_tol );
	Cvar_RegisterVariable( &slayer_item_topple_rate );
	Cvar_RegisterVariable( &slayer_item_topple_damping );
	Cvar_RegisterVariable( &slayer_item_topple_bounce );
	Cvar_RegisterVariable( &slayer_item_extent );
	Cvar_RegisterVariable( &slayer_item_place );
	Cvar_RegisterVariable( &slayer_item_place_lift );
	Cvar_RegisterVariable( &slayer_item_place_yaws );
	Cvar_RegisterVariable( &slayer_item_place_tilts );
	Cvar_RegisterVariable( &slayer_item_place_traces );
	Cvar_RegisterVariable( &slayer_item_spin );
	Cvar_RegisterVariable( &slayer_shield_radius );
	Cvar_RegisterVariable( &slayer_shield_spin );
	Cvar_RegisterVariable( &slayer_item_lean_probes );
	Cvar_RegisterVariable( &slayer_item_diag );

	// TURN THE DROPPED-ITEM PHYSICS OFF ON AN EXISTING CONFIG.
	//
	// The feature shipped with slayer_item_phys 1, so that value is archived in
	// the player's config.cfg. Changing the default alone does nothing: cvars are
	// registered here, inside CL_Init, and `exec config.cfg` runs AFTERWARDS and
	// puts the stored 1 back. (Same trap as the scoreboard's block-level and K/D
	// migrations -- see cl_scoreboard_slayer.c.)
	//
	// Only the exact shipped default is touched. Someone who deliberately set it
	// to 0 already gets 0; someone who turns it back on after this migration ran
	// keeps it, because the migration flag is archived too and never runs twice.
	if( slayer_item_phys_migrated.value == 0.0f )
	{
		if( slayer_item_phys.value != 0.0f )
			Cvar_SetValue( "slayer_item_phys", 0.0f );

		Cvar_SetValue( "slayer_item_phys_migrated", 1.0f );
		Slayer_Log_Printf( "item phys migration: dropped-item physics OFF by default "
			"(vanilla server pose); set slayer_item_phys 1 to compare" );
	}

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
		// Same for the placement: a solved offset belongs to one spot on one map.
		ip_slots[i].placed = false;
		ip_slots[i].place_ok = false;
		VectorClear( ip_slots[i].place_offset );
	}

	ip_diag_last_print = 0.0;
	ip_diag_last_reject = 0.0;
	ip_traces = 0;
}

/*
====================
Slayer_IP_PlaceTrace

The world, as the placement solver wants it: one line trace, plus whether the
start point was already inside geometry.

`startsolid` is the important part and it is what CL_TraceLine gives us almost for
free. The solver's containment test depends on being able to tell "the ray hit a
wall on its way out" from "the ray began inside a wall" -- without that
distinction a model buried in the floor looks the same as one lying on it.
====================
*/
static int Slayer_IP_PlaceTrace( void *ctx, const float *start, const float *end,
	float *out_frac, float *out_normal, int *out_startsolid )
{
	pmtrace_t tr;
	vec3_t    s, e;

	(void)ctx;

	VectorCopy( start, s );
	VectorCopy( end, e );

	ip_traces++;
	tr = CL_TraceLine( s, e, PM_STUDIO_IGNORE );

	*out_startsolid = tr.startsolid ? 1 : 0;
	*out_frac = tr.fraction;
	VectorCopy( tr.plane.normal, out_normal );

	// startsolid comes back with fraction 0 and no useful plane, so it has to be
	// reported as a hit even though `fraction < 1` would not be enough to tell.
	return ( tr.fraction < 1.0f || tr.startsolid ) ? 1 : 0;
}

static void Slayer_IP_PlaceParams( slayer_place_params_t *sp )
{
	Slayer_Place_DefaultParams( sp );

	if( slayer_item_place_yaws.value >= 1.0f )
		sp->yaw_steps = (int)slayer_item_place_yaws.value;
	if( slayer_item_place_tilts.value >= 1.0f )
		sp->tilt_steps = (int)slayer_item_place_tilts.value;
	if( slayer_item_place_lift.value >= 0.0f )
		sp->max_lift = slayer_item_place_lift.value;
	if( slayer_item_place_traces.value >= 16.0f )
		sp->max_traces = (int)slayer_item_place_traces.value;
}

/*
====================
Slayer_IP_ApplyPose

Write the stored pose to the entity, and add the visual nudge.

Every path that returns early has to go through here, or an item would be drawn
with its solved pose on the frames that integrate and WITHOUT the nudge on the
frames that do not -- which is a two-unit twitch every time the item is skipped.
The sleeping path is the one that matters: a settled item takes that path forever.

The nudge is the one place in this module that changes where an item is drawn. It
exists because rotation cannot save a mesh whose origin the server parked below the
surface, which is the "half inside the texture" case. Bounded by
slayer_item_place_lift and capped again inside the solver; a CS weaponbox pickup
volume is tens of units across, so what you see stays inside what you can pick up.
`ent->origin` is this frame's render copy, so the entity's own state is untouched.
====================
*/
static void Slayer_IP_ApplyPose( item_phys_t *ip, struct cl_entity_s *ent )
{
	Slayer_Spin_PoseToAngles( &ip->spin, ent->angles );

	if( !ip->placed )
		return;

	if( ip->place_offset[0] == 0.0f && ip->place_offset[1] == 0.0f
	 && ip->place_offset[2] == 0.0f )
		return;

	VectorAdd( ent->origin, ip->place_offset, ent->origin );
}

/*
====================
Slayer_IP_RestAxis

Which body axis of this model should face the surface it rests on?

The one along its SHORTEST extent. An object comes to rest on its largest face,
and the largest face is the one perpendicular to the shortest dimension: a rifle
(long along its own X, thin along Z) lies flat on its side, not on its butt.

ONLY USED BY THE LEGACY MECHANIC (slayer_item_settle_mode 2). The toppling
mechanic needs no such choice -- the box's own corners decide, so there is nothing
to pick and nothing to get wrong. That is the deeper reason the new mechanic is
better than a fixed version of the old one.

The extents come from the measured mesh, not from `model->mins/maxs`: the hull
names the wrong short axis on 24 of 34 stock models, which made this function pick
the wrong axis exactly as often. See Slayer_IP_ModelBox.
====================
*/
static void Slayer_IP_RestAxis( struct cl_entity_s *ent, vec3_t out )
{
	vec3_t half;
	int    i, best = 2;

	VectorClear( out );

	if( !Slayer_IP_ModelBox( ent, half ))
	{
		out[2] = 1.0f;
		return;
	}

	for( i = 0; i < 3; i++ )
	{
		if( half[i] < half[best] )
			best = i;
	}

	// A model with no meaningful extents (some server props report all zeroes)
	// gets the historical answer rather than a zero vector.
	if( !( half[best] > 0.005f ))
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
		Slayer_IP_ApplyPose( ip, ent );
		return;
	}

	dt = now - ip->last_time;
	if( dt <= 0.0f )
	{
		// Same frame, second visibility pass: reapply, do not integrate twice.
		Slayer_IP_ApplyPose( ip, ent );
		return;
	}
	if( dt > 0.5f )
	{
		// Loading screen or demo seek: freeze the pose rather than integrating a
		// half-second step, which would spin the item through several turns.
		ip->last_time = now;
		VectorCopy( ent->origin, ip->last_origin );
		Slayer_IP_ApplyPose( ip, ent );
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
			// Bookkeeping before the pose, for the reason spelled out at the bottom
			// of this function: ApplyPose adds the visual nudge to ent->origin, and
			// storing THAT as last_origin would imply a constant phantom velocity.
			ip->last_time = now;
			VectorCopy( ent->origin, ip->last_origin );
			Slayer_IP_ApplyPose( ip, ent );
			return;
		}

		// It moved: picked up and re-thrown, or shoved by an explosion. Start
		// over, including the throw impulse -- the next flight is a new throw.
		ip->asleep = false;
		ip->rest_secs = 0.0f;
		ip->have_rest_normal = false;
		// And the placement: a solved pose and offset belong to the spot it was
		// solved for. Carrying them to a new spot is worse than not having them.
		ip->placed = false;
		ip->place_ok = false;
		VectorClear( ip->place_offset );
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
		Slayer_IP_ApplyPose( ip, ent );
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
	// Only once the tumble has stopped. Resolving the pose while the item is still
	// flying would fight the tumble, and the result reads as the model twitching
	// rather than falling.
	if( Slayer_IP_SettleMode() != 0 && Slayer_Spin_IsResting( &ip->spin ))
	{
		ip->rest_secs += dt;

		// The surface is found ONCE and remembered. Re-tracing every frame would
		// spend traces per resting item forever, and a resting item's surroundings
		// do not change. `rest_dist` is remembered with it: the pose constraint is
		// a function of how high the origin sits, so a normal without its distance
		// is only half the information.
		if( !ip->have_rest_normal )
		{
			if( Slayer_IP_FindSupport( ent, &params, ip->rest_normal, &ip->rest_dist ))
			{
				ip->have_rest_normal = true;
			}
			else if( contact.on_ground )
			{
				VectorCopy( contact.normal, ip->rest_normal );
				ip->rest_dist = 0.0f;
				ip->have_rest_normal = true;
			}
		}

		if( ip->have_rest_normal )
		{
			if( Slayer_IP_SettleMode() == 1 )
			{
				// TOPPLING. No target orientation: the surface pushes buried
				// corners out and stops. See cl_spin_phys_slayer.h for why the
				// alternative below was wrong in principle rather than mistuned.
				slayer_spin_settle_params_t sp;
				slayer_spin_support_t       sup;
				vec3_t half;

				if( Slayer_IP_ModelBox( ent, half ))
				{
					Slayer_IP_SettleParams( &sp );
					Slayer_Spin_Settle( &ip->spin, half, ip->rest_normal,
						ip->rest_dist, dt, &sp, &sup );

					ip->support = sup.support;
					ip->topple_settled = sup.settled;
				}
			}
			else
			{
				// LEGACY alignment, kept for on-device comparison only.
				float  rate = slayer_item_settle_rate.value;
				float  tol;
				vec3_t body_axis;

				if( rate < 0.1f ) rate = 0.1f;
				if( rate > 30.0f ) rate = 30.0f;

				tol = slayer_item_settle_tol.value;
				if( tol < 0.0f ) tol = 0.0f;
				if( tol > 89.0f ) tol = 89.0f;

				Slayer_IP_RestAxis( ent, body_axis );

				Slayer_Spin_SettleAxisTo( &ip->spin, ip->rest_normal, body_axis,
					rate, dt, (float)cos( (double)( tol * ( M_PI / 180.0 ))));
			}
		}

		// Sleep once there is nothing left to compute. With toppling that is a
		// definite answer from the core (`settled`) rather than a timeout, so an
		// item that reached its resting pose in three frames stops costing anything
		// after three frames instead of after the fixed delay. The timeout stays as
		// the backstop for the legacy mode and for an item whose surface was never
		// found.
		if( ip->topple_settled || ip->rest_secs >= Slayer_IP_SleepDelay() )
		{
			// --- placement, once, at the moment it stops -------------------
			//
			// This is the answer to "part of the weapon is inside the floor". The
			// toppling above produces a plausible pose against ONE plane; this
			// checks the whole box against everything around it and searches for a
			// pose (and, if rotation cannot do it, a small drawn nudge) in which no
			// part of the model is inside the map.
			//
			// Deliberately here rather than per frame: the search costs a few dozen
			// traces, and running it once when the item settles makes that free,
			// while running it continuously would be the most expensive thing in
			// the module. The result is frozen until the item moves.
			if( !ip->placed && slayer_item_place.value != 0.0f )
			{
				vec3_t half;

				ip->placed = true;
				VectorClear( ip->place_offset );
				ip->place_ok = false;

				if( Slayer_IP_ModelBox( ent, half ))
				{
					slayer_place_params_t  sp;
					slayer_place_result_t  res;
					vec4_t                 pose;

					Slayer_IP_PlaceParams( &sp );
					Vector4Copy( ip->spin.orient, pose );

					Slayer_Place_Solve( half, ent->origin, pose,
						Slayer_IP_PlaceTrace, NULL, &sp, &res );

					if( res.candidates > 0 )
					{
						Vector4Copy( res.orient, ip->spin.orient );
						VectorCopy( res.offset, ip->place_offset );
						ip->place_ok = ( res.solved != 0 );

						if( slayer_item_diag.value >= 1.0f )
						{
							Slayer_Log_Printf( "IP place idx=%d model=%s solved=%d pen=%.2f contacts=%d cand=%d traces=%d off=(%.1f %.1f %.1f)",
								ent->index, ent->model->name, res.solved,
								res.penetration, res.contacts, res.candidates,
								res.traces,
								res.offset[0], res.offset[1], res.offset[2] );
						}
					}
				}
			}

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
		// forget the surface so the next rest re-finds it, and forget the placement
		// so the next rest re-solves it. A stale offset would follow the item to its
		// new position, where it means nothing.
		ip->have_rest_normal = false;
		ip->topple_settled = false;
		ip->support = 0;
		ip->placed = false;
		ip->place_ok = false;
		VectorClear( ip->place_offset );
	}

	// BOOKKEEPING BEFORE THE POSE IS APPLIED, and this ordering is not cosmetic.
	//
	// `last_origin` is what the next frame differences against to work out the
	// item's speed, and `sleep_origin` is what the wake check compares to. Both
	// must hold the SERVER's position. Storing the nudged position instead would
	// make the item appear to move by the nudge distance every frame -- a constant
	// phantom velocity that would wake it immediately, re-solve the placement, and
	// repeat forever. Slayer_IP_ApplyPose is what adds the nudge, so it comes after.
	VectorCopy( ent->origin, ip->last_origin );
	ip->last_time = now;

	Slayer_IP_ApplyPose( ip, ent );

	if( slayer_item_diag.value >= 1.0f
	 && cl.time - ip_diag_last_print >= IP_DIAG_INTERVAL )
	{
		ip_diag_last_print = cl.time;

		Slayer_Log_Printf( "IP idx=%d model=%s speed=%.0f omega=%.2f hits=%d rest=%d "
			"spun=%d asleep=%d traces=%u mode=%d support=%d dist=%.2f "
			"n=(%.2f %.2f %.2f) ang=(%.1f %.1f %.1f)",
			ent->index, ent->model->name, speed,
			Slayer_Spin_Rate( &ip->spin ), ip->spin.impacts,
			Slayer_Spin_IsResting( &ip->spin ),
			ip->spin.spun_up, (int)ip->asleep, ip_traces,
			Slayer_IP_SettleMode(), ip->support, ip->rest_dist,
			ip->rest_normal[0], ip->rest_normal[1], ip->rest_normal[2],
			ent->angles[0], ent->angles[1], ent->angles[2] );
	}
}
