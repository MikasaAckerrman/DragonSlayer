/*
cl_tracer_slayer.c - Slayer3D custom bullet tracers
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

// Slayer3D tracers. CS 1.6 does NOT draw bullet tracers at all, and it does
// not route bullets through the engine's R_TracerEffect either, so there is
// nothing to recolour or resize -- we have to spawn the tracer ourselves.
//
// The visible tracer is a real beam (R_BeamPoints, sprites/laserbeam.spr): a
// bright additive line the engine already batches and depth-sorts in one draw
// path (ref/gl CL_DrawBeams), with built-in occlusion culling (R_BeamVisible).
// We spawn one per shot from the muzzle to the impact point (a forward trace),
// fading out over its short life so it reads as a fast comet-streak.
//
// The universal "a shot happened" signal in GoldSrc is EF_MUZZLEFLASH on the
// shooter's entity -- set every time ANY weapon (including custom ones) fires,
// for the local player and every remote player alike. We catch that edge in
// CL_LinkPlayers (cl_frame.c) before the renderer consumes the flag.
//
//   * Local player (first person): we know the exact aim, so the tracer is
//     traced along the real view angles from the eye.
//   * Remote players: traced along the player entity's forward; the muzzle is
//     approximated (origin + eye height + forward), good enough for the visual
//     and refined later from device-log data.
//
// Barrel heat still drives the COLOUR: sustained fire walks it yellow -> orange
// -> red and cools gradually. That colour (all three R/G/B channels of one
// heat colour, not a separate feature) is applied directly to each beam.

#include "common.h"
#include "client.h"
#include "r_efx.h"       // BEAM / struct beam_s, beamdef flags
#include "cl_tent.h"     // R_BeamPoints, R_TracerEffect
#include "cl_tracer_slayer.h"
#include "cl_tracer_render_slayer.h"   // our own ribbon geometry
#include "cl_view_slayer.h"            // V_IsSlayerThirdPerson
#include "cl_slayer_log.h"

// ===========================================================================
// Cvars
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_tracer, "1", FCVAR_ARCHIVE,
	"Slayer3D: custom bullet tracers (0 = vanilla tracers)" );

static CVAR_DEFINE_AUTO( slayer_tracer_heat, "1", FCVAR_ARCHIVE,
	"Slayer3D: barrel-heat colour shift on sustained fire (0 = fixed colour)" );

// Seconds of continuous fire to walk fully from cold to hot.
static CVAR_DEFINE_AUTO( slayer_tracer_heat_full, "2.5", FCVAR_ARCHIVE,
	"Slayer3D: seconds of sustained fire to reach the hottest tracer colour" );

// Cooldown runs this much faster than buildup (1.2 = cools 20% quicker).
static CVAR_DEFINE_AUTO( slayer_tracer_heat_cool, "1.2", FCVAR_ARCHIVE,
	"Slayer3D: how much faster heat bleeds off than it builds (1.0 = same rate)" );

// A shot arriving within this gap counts as "still firing" for heat buildup.
static CVAR_DEFINE_AUTO( slayer_tracer_heat_gap, "0.15", FCVAR_ARCHIVE,
	"Slayer3D: max seconds between shots still counted as continuous fire" );

// Cold colour (yellow) and hot colour (red); orange is the midpoint.
static CVAR_DEFINE_AUTO( slayer_tracer_cold, "255 220 90", FCVAR_ARCHIVE,
	"Slayer3D: cold tracer colour 'R G B' 0..255" );

static CVAR_DEFINE_AUTO( slayer_tracer_hot, "255 70 55", FCVAR_ARCHIVE,
	"Slayer3D: hottest tracer colour 'R G B' 0..255" );

static CVAR_DEFINE_AUTO( slayer_tracer_sparks, "0", FCVAR_ARCHIVE,
	"Slayer3D: replace vanilla bullet-impact sparks (0 = keep vanilla until our own sparks exist)" );

// --- Beam appearance ------------------------------------------------------
// Beam width in world units (R_BeamPoints width is *10 of the wire byte, i.e.
// raw units). ~2.2 reads as a slim bright streak, not a fat laser.
static CVAR_DEFINE_AUTO( slayer_tracer_width, "2.2", FCVAR_ARCHIVE,
	"Slayer3D: tracer beam width in units" );

// Per-shot lifetime; short so it snaps like a bullet, not a laser sight.
static CVAR_DEFINE_AUTO( slayer_tracer_life, "0.11", FCVAR_ARCHIVE,
	"Slayer3D: tracer beam lifetime in seconds" );

// Overall brightness 0..1 fed to the beam (additive; 1.0 = full punch).
static CVAR_DEFINE_AUTO( slayer_tracer_bright, "1.0", FCVAR_ARCHIVE,
	"Slayer3D: tracer beam brightness 0..1" );

// Weapon-class size multiplier applied to width. Tuned from the lab:
// pistol .8 / smg .9 / rifle 1.0 / lmg 1.12 / awp 1.35. The class isn't known
// from EF_MUZZLEFLASH yet, so this is the fallback until the shot event lands.
static CVAR_DEFINE_AUTO( slayer_tracer_scale, "1.0", FCVAR_ARCHIVE,
	"Slayer3D: tracer width scale for the current weapon class (fallback)" );

// How far forward to trace for the impact end-point when nothing is hit.
static CVAR_DEFINE_AUTO( slayer_tracer_range, "8192", FCVAR_ARCHIVE,
	"Slayer3D: max tracer trace distance in units" );

// Muzzle offset for remote players (no attachment data): forward + up + right
// from the entity origin so the streak leaves the gun, not the chest.
static CVAR_DEFINE_AUTO( slayer_tracer_fwd, "20", FCVAR_ARCHIVE,
	"Slayer3D: remote muzzle forward offset (units)" );

static CVAR_DEFINE_AUTO( slayer_tracer_up, "22", FCVAR_ARCHIVE,
	"Slayer3D: remote muzzle up offset from entity origin (units)" );

static CVAR_DEFINE_AUTO( slayer_tracer_right, "5", FCVAR_ARCHIVE,
	"Slayer3D: remote muzzle right offset (units)" );

// Variant D: prefer the real muzzle from the studio renderer. The renderer
// fills ent->attachment[0] with the bone-transformed muzzle point (that is the
// exact meaning of EF_MUZZLEFLASH -- "ELIGHT on entity attachment 0") and
// writes it back to the global entity, so one frame later we can read the true
// muzzle instead of the origin+offset guess. When the attachment is degenerate
// (model has none -> renderer set it to origin) we fall back to the offset.
static CVAR_DEFINE_AUTO( slayer_tracer_use_attach, "1", FCVAR_ARCHIVE,
	"Slayer3D: use the studio attachment[0] as the real muzzle when available (0 = always approximate)" );

// Accept attachment[0] as a real muzzle only when it sits this far from the
// entity origin (below = renderer wrote origin because the model has no
// attachments; absurdly far = stale/teleport, also rejected).
static CVAR_DEFINE_AUTO( slayer_tracer_attach_min, "4", FCVAR_ARCHIVE,
	"Slayer3D: min attachment[0]-origin distance to trust it as a muzzle (units)" );

static CVAR_DEFINE_AUTO( slayer_tracer_attach_max, "128", FCVAR_ARCHIVE,
	"Slayer3D: max attachment[0]-origin distance still trusted as a muzzle (units)" );

// Verbose diagnostic: log every spawned tracer (who, start, end, hit) so the
// device log can confirm EF_MUZZLEFLASH really fires for remote players.
static CVAR_DEFINE_AUTO( slayer_tracer_debug, "1", FCVAR_ARCHIVE,
	"Slayer3D: log each spawned tracer to slayer_diag.log (0 = off)" );

// Diagnostic switch: log every event the game DLL plays back, with its
// precached name. CS 1.6 does not route bullets through R_TracerEffect, so this
// is how the real shot-event path gets identified from a device log. Off by
// default because a firefight generates a lot of lines.
static CVAR_DEFINE_AUTO( slayer_tracer_logevents, "0", FCVAR_ARCHIVE,
	"Slayer3D: log game DLL events to find the shot event (diagnostic, 0 = off)" );

// ===========================================================================
// Own renderer (ribbon) cvars
// ===========================================================================
//
// slayer_tracer_render selects the geometry:
//   1 = our own ribbon (default) -- two layers, colour ramp along the streak,
//       soft ends, taper, head spark, screen-space width clamp.
//   0 = the engine's R_BeamPoints, kept only as a fallback for comparison.
// The vanilla beam is a single constant-width strip with one colour for the
// whole line (TriColor4f is called once per beam in R_BeamDraw), which is what
// reads as a cheap server plugin.
static CVAR_DEFINE_AUTO( slayer_tracer_render, "1", FCVAR_ARCHIVE,
	"Slayer3D: tracer geometry (1 = own ribbon, 0 = engine beam fallback)" );

static CVAR_DEFINE_AUTO( slayer_tracer_speed, "22000", FCVAR_ARCHIVE,
	"Slayer3D: tracer flight speed in units/sec" );

// Length must stay above speed/min_fps, otherwise consecutive frames leave a
// gap and the streak reads as a dashed line. 1100 covers 22000/30fps = 733.
static CVAR_DEFINE_AUTO( slayer_tracer_length, "1100", FCVAR_ARCHIVE,
	"Slayer3D: visible streak length in units" );

static CVAR_DEFINE_AUTO( slayer_tracer_radius, "1.2", FCVAR_ARCHIVE,
	"Slayer3D: core diameter in units (halo provides the apparent thickness)" );

static CVAR_DEFINE_AUTO( slayer_tracer_segments, "14", FCVAR_ARCHIVE,
	"Slayer3D: ribbon segments; more = smoother fade, 1 draw call regardless" );

static CVAR_DEFINE_AUTO( slayer_tracer_soft_tail, "0.5", FCVAR_ARCHIVE,
	"Slayer3D: fraction of the streak that fades out towards the tail" );

static CVAR_DEFINE_AUTO( slayer_tracer_soft_head, "0.1", FCVAR_ARCHIVE,
	"Slayer3D: fraction of the streak that fades out towards the head" );

static CVAR_DEFINE_AUTO( slayer_tracer_taper, "0.65", FCVAR_ARCHIVE,
	"Slayer3D: tail narrowing, 0 = rectangle, 0.65 = droplet" );

static CVAR_DEFINE_AUTO( slayer_tracer_halo, "3.4", FCVAR_ARCHIVE,
	"Slayer3D: halo width multiplier (stands in for bloom; no FBO in ref/gl)" );

static CVAR_DEFINE_AUTO( slayer_tracer_halo_a, "0.2", FCVAR_ARCHIVE,
	"Slayer3D: halo brightness" );

static CVAR_DEFINE_AUTO( slayer_tracer_head, "2.2", FCVAR_ARCHIVE,
	"Slayer3D: head spark size relative to the core half-width (0 = off)" );

static CVAR_DEFINE_AUTO( slayer_tracer_head_gain, "1.7", FCVAR_ARCHIVE,
	"Slayer3D: head spark brightness" );

static CVAR_DEFINE_AUTO( slayer_tracer_flicker, "0.16", FCVAR_ARCHIVE,
	"Slayer3D: brightness flicker amount, 0 = steady (looks drawn)" );

static CVAR_DEFINE_AUTO( slayer_tracer_flicker_rate, "95", FCVAR_ARCHIVE,
	"Slayer3D: flicker frequency" );

// Remote shots matter more than your own: you already know you fired.
static CVAR_DEFINE_AUTO( slayer_tracer_remote, "1.45", FCVAR_ARCHIVE,
	"Slayer3D: brightness multiplier for OTHER players' tracers" );

// 1.3 rather than 1.0: at 30 fps and point-blank range (200 units) the lifetime
// was 1.77 frames, i.e. the tracer blinked once and could not be seen.
static CVAR_DEFINE_AUTO( slayer_tracer_life_mul, "1.3", FCVAR_ARCHIVE,
	"Slayer3D: lifetime multiplier over (flight + tail catch-up) time" );

static CVAR_DEFINE_AUTO( slayer_tracer_min_px, "1.1", FCVAR_ARCHIVE,
	"Slayer3D: minimum on-screen width in pixels (dimmed to compensate)" );

static CVAR_DEFINE_AUTO( slayer_tracer_max_px, "9.0", FCVAR_ARCHIVE,
	"Slayer3D: maximum on-screen width in pixels" );

// Third-person: the streak must run muzzle -> bullet hole, NOT muzzle ->
// crosshair. In third person the camera sits behind the player, so the view ray
// and the weapon's aim line are different lines; using the view ray puts the
// tracer visibly off the barrel.
static CVAR_DEFINE_AUTO( slayer_tracer_tp_muzzle, "1", FCVAR_ARCHIVE,
	"Slayer3D: in third person, trace from the player's own aim, not the camera" );

// ===========================================================================
// State
// ===========================================================================

// Heat, 0..1. Rewritten every frame in Slayer_Tracer_Frame.
static float  s_heat = 0.0f;
static double s_last_shot = 0.0;

// Cached model index for sprites/laserbeam.spr, resolved lazily on first shot.
// -1 = not resolved yet, 0 = resolved-but-missing (give up), >0 = usable.
static int    s_beam_model = -1;

// Per-player edge detector for EF_MUZZLEFLASH: the flag is a single-frame
// pulse, but we only want ONE tracer per pulse. Index 0 = local viewent.
static qboolean s_mf_prev[MAX_CLIENTS + 1];

// Diagnostic counters, summarised to the log once a second while shots happen,
// so the device log shows shot volume and the local/remote split at a glance
// without one line per bullet drowning everything else.
static int    s_fired_local  = 0;
static int    s_fired_remote = 0;
static int    s_beam_ok      = 0;   // beams actually spawned
static int    s_beam_fail_model = 0;// fires with no usable beam sprite
static int    s_beam_fail_null  = 0;// R_BeamPoints returned NULL (pool/cull)
static double s_last_summary = 0.0;

// INDEPENDENT probe layer -- so we never rely on one signal. These measure
// different stages of the pipeline; comparing them localises any failure:
//   * s_mf_raw_*   : frames where EF_MUZZLEFLASH was SEEN set (before our edge
//                    logic). If raw>0 but fired==0, the edge detector is at
//                    fault; if raw==0, the flag genuinely never arrives.
//   * s_te_tracer  : server-sent TE_TRACER temp-entities (a totally separate
//                    code path from muzzleflash -- see Slayer_Tracer_NoteServerTracer).
//   * degenerate trace warnings are logged inline, throttled.
static int    s_mf_raw_local  = 0;
static int    s_mf_raw_remote = 0;
static int    s_te_tracer     = 0;
static int    s_attach_used   = 0;   // shots that used the real attachment muzzle
static int    s_attach_reject = 0;   // shots where attachment was degenerate -> offset
static double s_last_trace_warn = 0.0;

// One-shot probes: the single most important thing the morning device log has
// to answer is "does EF_MUZZLEFLASH ever fire for REMOTE players in CS 1.6?"
// (the local side is proven). We log the first time each side is seen so the
// answer is one grep away instead of buried in the summary counts.
static qboolean s_seen_local_probe  = false;
static qboolean s_seen_remote_probe = false;

// Midpoint colour (orange) between cold and hot, so a 3-stop ramp reads right.
static const byte SLAYER_TRACER_MID[3] = { 255, 150, 50 };

// ===========================================================================
// Own tracer pool
// ===========================================================================
//
// Fixed size, allocated once. A tracer lives ~90-200 ms, so even sustained fire
// from a full server never needs more than a few dozen slots; when the pool is
// full the oldest slot is recycled, which is invisible at these lifetimes and
// costs nothing. Zero allocations per frame is the point: this same array is
// what keeps the renderer at two draw calls per tracer with no GC pressure.
#define SLAYER_TRACER_POOL 48

static slayer_tracer_t s_pool[SLAYER_TRACER_POOL];
static int             s_pool_next = 0;
static int             s_live_peak = 0;   // diagnostics: high-water mark

// Rolling seed for the flicker phase, so two tracers spawned in the same frame
// do not pulse in sync. Deterministic on purpose (no RNG state to sync).
static float s_seed_walk = 0.0f;

static slayer_tracer_t *Slayer_Tracer_Alloc( void )
{
	int i;

	// prefer a free slot
	for( i = 0; i < SLAYER_TRACER_POOL; i++ )
	{
		int idx = ( s_pool_next + i ) % SLAYER_TRACER_POOL;

		if( !s_pool[idx].active )
		{
			s_pool_next = ( idx + 1 ) % SLAYER_TRACER_POOL;
			return &s_pool[idx];
		}
	}

	// all busy: recycle round-robin
	{
		slayer_tracer_t *tr = &s_pool[s_pool_next];

		s_pool_next = ( s_pool_next + 1 ) % SLAYER_TRACER_POOL;
		return tr;
	}
}

// Read the style out of the cvars once per frame instead of per tracer.
static void Slayer_Tracer_ReadStyle( slayer_tracer_style_t *st )
{
	st->speed        = slayer_tracer_speed.value;
	st->length       = slayer_tracer_length.value;
	st->radius       = slayer_tracer_radius.value;
	st->segments     = (int)slayer_tracer_segments.value;
	st->soft_tail    = slayer_tracer_soft_tail.value;
	st->soft_head    = slayer_tracer_soft_head.value;
	st->taper        = slayer_tracer_taper.value;
	st->halo_scale   = slayer_tracer_halo.value;
	st->halo_alpha   = slayer_tracer_halo_a.value;
	st->head_size    = slayer_tracer_head.value;
	st->head_gain    = slayer_tracer_head_gain.value;
	st->flicker      = slayer_tracer_flicker.value;
	st->flicker_rate = slayer_tracer_flicker_rate.value;
	st->brightness   = slayer_tracer_bright.value;
	st->remote_boost = slayer_tracer_remote.value;
	st->life_mul     = slayer_tracer_life_mul.value;
	st->min_px       = slayer_tracer_min_px.value;
	st->max_px       = slayer_tracer_max_px.value;

	// Guards: a zero/garbage cvar must not divide by zero or spin the loop.
	if( st->speed < 100.0f ) st->speed = 100.0f;
	if( st->length < 32.0f ) st->length = 32.0f;
	if( st->radius < 0.05f ) st->radius = 0.05f;
	if( st->segments < 2 )  st->segments = 2;
	if( st->segments > 64 ) st->segments = 64;
	if( st->life_mul < 0.1f ) st->life_mul = 0.1f;
	if( st->min_px < 0.1f ) st->min_px = 0.1f;
	if( st->max_px < st->min_px ) st->max_px = st->min_px;
}

// Spawn one tracer into the pool. `is_remote` boosts brightness.
static void Slayer_Tracer_SpawnOwn( const vec3_t start, const vec3_t end, qboolean is_remote )
{
	slayer_tracer_style_t st;
	slayer_tracer_t *tr;
	vec3_t delta;
	float  dist, flight;

	Slayer_Tracer_ReadStyle( &st );

	VectorSubtract( end, start, delta );
	dist = VectorLength( delta );
	if( dist < 1.0f )
		return;

	tr = Slayer_Tracer_Alloc();

	VectorCopy( start, tr->start );
	VectorScale( delta, 1.0f / dist, tr->dir );
	tr->dist   = dist;
	tr->length = st.length;
	tr->radius = st.radius;
	tr->age    = 0.0f;
	tr->gain   = is_remote ? st.remote_boost : 1.0f;

	// life = flight time + time for the tail to catch the stopped head
	flight = dist / st.speed;
	tr->life = ( flight + st.length / st.speed ) * st.life_mul;
	if( tr->life < 1e-4f ) tr->life = 1e-4f;

	s_seed_walk += 7.31f;
	if( s_seed_walk > 1000.0f ) s_seed_walk -= 1000.0f;
	tr->seed = s_seed_walk;

	tr->active = true;
}

// Advance ages and draw every live tracer. Called from CL_DrawEFX so it lands
// in the translucent pass with the correct view already set up.
void Slayer_TracerPool_Draw( void )
{
	slayer_tracer_style_t st;
	float dt, fov_y;
	int   i, live = 0;

	if( slayer_tracer.value == 0.0f || slayer_tracer_render.value == 0.0f )
		return;

	dt = (float)( cl.time - cl.oldtime );
	if( dt < 0.0f ) dt = 0.0f;
	if( dt > 0.25f ) dt = 0.25f;   // a hitch must not teleport tracers

	Slayer_Tracer_ReadStyle( &st );

	// refState.viewangles/vieworg are written by GL_RenderFrame right before
	// the scene is drawn, so in third person they already hold the CAMERA
	// position -- which is what the billboard/width axes need.
	fov_y = 90.0f;
	if( refState.height > 0 && refState.width > 0 )
	{
		// The engine stores fov_x in the viewpass; derive fov_y from the
		// aspect the same way the renderer does.
		float aspect = (float)refState.height / (float)refState.width;

		fov_y = 2.0f * RAD2DEG( atan( tan( DEG2RAD( 90.0f ) * 0.5f ) * aspect ));
	}

	for( i = 0; i < SLAYER_TRACER_POOL; i++ )
	{
		slayer_tracer_t *tr = &s_pool[i];

		if( !tr->active )
			continue;

		tr->age += dt;
		if( tr->age >= tr->life )
		{
			tr->active = false;
			continue;
		}

		live++;
		Slayer_TracerRender_Draw( tr, &st, refState.vieworg, fov_y, refState.height );
	}

	if( live > s_live_peak )
		s_live_peak = live;
}

// ===========================================================================
// Helpers
// ===========================================================================

static void Slayer_Tracer_ParseColor( const char *str, byte out[3], byte dr, byte dg, byte db )
{
	int r = dr, g = dg, b = db;

	out[0] = dr; out[1] = dg; out[2] = db;
	if( !str || !*str )
		return;
	if( sscanf( str, "%d %d %d", &r, &g, &b ) < 3 )
		return;
	out[0] = (byte)bound( 0, r, 255 );
	out[1] = (byte)bound( 0, g, 255 );
	out[2] = (byte)bound( 0, b, 255 );
}

// Interpolate the 3-stop ramp cold -> mid -> hot at t in [0..1].
static void Slayer_Tracer_HeatColor( float t, byte out[3] )
{
	byte cold[3], hot[3];
	const byte *a, *b;
	float f;

	Slayer_Tracer_ParseColor( slayer_tracer_cold.string, cold, 255, 220, 90 );
	Slayer_Tracer_ParseColor( slayer_tracer_hot.string,  hot,  255, 70, 55 );

	if( t < 0.0f ) t = 0.0f;
	if( t > 1.0f ) t = 1.0f;

	if( t < 0.5f )
	{
		a = cold; b = SLAYER_TRACER_MID; f = t / 0.5f;
	}
	else
	{
		a = SLAYER_TRACER_MID; b = hot; f = ( t - 0.5f ) / 0.5f;
	}

	out[0] = (byte)( a[0] + ( b[0] - a[0] ) * f );
	out[1] = (byte)( a[1] + ( b[1] - a[1] ) * f );
	out[2] = (byte)( a[2] + ( b[2] - a[2] ) * f );
}

// ===========================================================================
// Public API
// ===========================================================================

void Slayer_Tracer_Init( void )
{
	Cvar_RegisterVariable( &slayer_tracer );
	Cvar_RegisterVariable( &slayer_tracer_heat );
	Cvar_RegisterVariable( &slayer_tracer_heat_full );
	Cvar_RegisterVariable( &slayer_tracer_heat_cool );
	Cvar_RegisterVariable( &slayer_tracer_heat_gap );
	Cvar_RegisterVariable( &slayer_tracer_cold );
	Cvar_RegisterVariable( &slayer_tracer_hot );
	Cvar_RegisterVariable( &slayer_tracer_sparks );
	Cvar_RegisterVariable( &slayer_tracer_width );
	Cvar_RegisterVariable( &slayer_tracer_life );
	Cvar_RegisterVariable( &slayer_tracer_bright );
	Cvar_RegisterVariable( &slayer_tracer_scale );
	Cvar_RegisterVariable( &slayer_tracer_range );
	Cvar_RegisterVariable( &slayer_tracer_fwd );
	Cvar_RegisterVariable( &slayer_tracer_up );
	Cvar_RegisterVariable( &slayer_tracer_right );
	Cvar_RegisterVariable( &slayer_tracer_use_attach );
	Cvar_RegisterVariable( &slayer_tracer_attach_min );
	Cvar_RegisterVariable( &slayer_tracer_attach_max );
	Cvar_RegisterVariable( &slayer_tracer_debug );
	Cvar_RegisterVariable( &slayer_tracer_logevents );

	// own renderer
	Cvar_RegisterVariable( &slayer_tracer_render );
	Cvar_RegisterVariable( &slayer_tracer_speed );
	Cvar_RegisterVariable( &slayer_tracer_length );
	Cvar_RegisterVariable( &slayer_tracer_radius );
	Cvar_RegisterVariable( &slayer_tracer_segments );
	Cvar_RegisterVariable( &slayer_tracer_soft_tail );
	Cvar_RegisterVariable( &slayer_tracer_soft_head );
	Cvar_RegisterVariable( &slayer_tracer_taper );
	Cvar_RegisterVariable( &slayer_tracer_halo );
	Cvar_RegisterVariable( &slayer_tracer_halo_a );
	Cvar_RegisterVariable( &slayer_tracer_head );
	Cvar_RegisterVariable( &slayer_tracer_head_gain );
	Cvar_RegisterVariable( &slayer_tracer_flicker );
	Cvar_RegisterVariable( &slayer_tracer_flicker_rate );
	Cvar_RegisterVariable( &slayer_tracer_remote );
	Cvar_RegisterVariable( &slayer_tracer_life_mul );
	Cvar_RegisterVariable( &slayer_tracer_min_px );
	Cvar_RegisterVariable( &slayer_tracer_max_px );
	Cvar_RegisterVariable( &slayer_tracer_tp_muzzle );

	Slayer_Tracer_Reset();
}

void Slayer_Tracer_Reset( void )
{
	int i;

	s_heat = 0.0f;
	s_last_shot = 0.0;
	s_beam_model = -1;   // re-resolve after a map change (model table is rebuilt)

	for( i = 0; i <= MAX_CLIENTS; i++ )
		s_mf_prev[i] = false;

	s_fired_local = s_fired_remote = s_beam_ok = 0;
	s_beam_fail_model = s_beam_fail_null = 0;
	s_mf_raw_local = s_mf_raw_remote = s_te_tracer = 0;
	s_attach_used = s_attach_reject = 0;
	s_last_summary = 0.0;
	s_last_trace_warn = 0.0;
	s_seen_local_probe = s_seen_remote_probe = false;

	// Own pool: clear it so tracers from the previous map cannot be drawn with
	// stale world coordinates on the first frame of the new one.
	memset( s_pool, 0, sizeof( s_pool ));
	s_pool_next = 0;
	s_live_peak = 0;
	s_seed_walk = 0.0f;

	// The texture table is rebuilt on map change, so the cached texnums are
	// stale. Forget them (no GL calls) and let the next draw rebuild lazily.
	Slayer_TracerRender_Invalidate();
}

// Resolve (and cache) the beam sprite model index. The beam renderer indexes
// cl.models[], so we need a real slot there. Two paths:
//   1. If the server already precached sprites/laserbeam.spr, reuse its index.
//   2. Otherwise load it ourselves (Mod_ForName) into a free high cl.models
//      slot -- CS 1.6 does NOT reliably precache the laser sprite, so relying
//      on the server alone would leave tracers invisible.
// Returns >0 on success, 0 if the sprite could not be loaded at all.
static int Slayer_Tracer_BeamModel( void )
{
	int      index = -1;
	int      slot;
	model_t *mod;

	if( s_beam_model > 0 )
		return s_beam_model;
	if( s_beam_model == 0 )
		return 0;   // known-missing, don't retry every shot

	// Path 1: already in the precache table?
	CL_LoadModel( DEFAULT_LASERBEAM_PATH, &index );
	if( index > 0 )
	{
		s_beam_model = index;
		Slayer_Log_Printf( "tracer: beam model '%s' found precached at index %d",
			DEFAULT_LASERBEAM_PATH, index );
		return s_beam_model;
	}

	// Path 2: load it ourselves into a free high slot so we don't collide with
	// server-precached models (those grow up from index 1). Scan downward from
	// the sentinel for an empty slot.
	mod = Mod_ForName( DEFAULT_LASERBEAM_PATH, false, false );
	if( !mod || mod->type != mod_sprite )
	{
		s_beam_model = 0;
		Slayer_Log_Printf( "tracer: beam model '%s' load FAILED (mod=%p type=%d) -- no tracers",
			DEFAULT_LASERBEAM_PATH, (void *)mod, mod ? (int)mod->type : -1 );
		return 0;
	}

	for( slot = MAX_MODELS - 1; slot >= 1; slot-- )
	{
		if( cl.models[slot] == NULL )
			break;
		if( !Q_stricmp( cl.models[slot]->name, mod->name ))
		{
			// already registered (e.g. re-resolve after our own earlier call)
			s_beam_model = slot;
			return slot;
		}
	}

	if( slot < 1 )
	{
		s_beam_model = 0;
		Slayer_Log_Printf( "tracer: no free cl.models slot for beam sprite -- no tracers" );
		return 0;
	}

	cl.models[slot] = mod;
	if( cl.nummodels <= slot )
		cl.nummodels = slot + 1;

	s_beam_model = slot;
	Slayer_Log_Printf( "tracer: beam model '%s' self-registered at cl.models[%d] (nummodels=%d)",
		DEFAULT_LASERBEAM_PATH, slot, cl.nummodels );

	return s_beam_model;
}

// Spawn one tracer beam from muzzle to end, coloured by current heat. Colour
// bytes 0..255 are converted to the beam's 0..1 range. The beam fades out over
// its life (FBEAM_FADEOUT) so it streaks like a bullet rather than hanging.
static void Slayer_Tracer_SpawnBeam( const vec3_t start, const vec3_t end, float scale )
{
	byte  col[3];
	float width;
	int   model;
	BEAM *b;

	model = Slayer_Tracer_BeamModel();
	if( model <= 0 )
	{
		s_beam_fail_model++;
		return;
	}

	Slayer_Tracer_HeatColor( s_heat, col );

	width = slayer_tracer_width.value * scale * slayer_tracer_scale.value;
	if( width < 0.5f ) width = 0.5f;

	// R_BeamPoints( start, end, model, life, width, amplitude, brightness,
	//               speed, startFrame, framerate, r, g, b )  -- r/g/b in 0..1.
	b = R_BeamPoints(
		(float *)start, (float *)end, model,
		slayer_tracer_life.value,      // life
		width,                          // width
		0.0f,                           // amplitude (straight, no noise)
		slayer_tracer_bright.value,     // brightness 0..1
		0.0f,                           // speed (no scroll)
		0, 0.0f,                        // startFrame, framerate
		col[0] / 255.0f, col[1] / 255.0f, col[2] / 255.0f );

	if( b )
	{
		SetBits( b->flags, FBEAM_FADEOUT );
		s_beam_ok++;
	}
	else
	{
		// R_BeamPoints returned NULL: beam pool exhausted or occlusion-culled
		// (R_BeamVisible). Worth knowing on ZM where the pool can be tight.
		s_beam_fail_null++;
	}
}

// Trace forward from `from` along `dir` (unit) up to the tracer range, and
// write the impact point (or the far end) into `out`.
static void Slayer_Tracer_TraceEnd( const vec3_t from, const vec3_t dir, vec3_t out )
{
	vec3_t    far_end;
	pmtrace_t tr;
	float     range = slayer_tracer_range.value;

	if( range < 64.0f ) range = 64.0f;

	VectorMA( from, range, dir, far_end );

	// PM_STUDIO_BOX: hit player/monster boxes too, so a tracer at an enemy ends
	// on them instead of shooting through to the far wall.
	tr = CL_TraceLine( (float *)from, far_end, PM_STUDIO_BOX );

	// A trace that starts already solid (start point inside geometry) yields a
	// zero-length or backwards beam -- an independent sign that the muzzle
	// origin is wrong (e.g. remote offset places it inside the player box).
	// Throttled so a bad frame doesn't flood the log.
	if( tr.startsolid || tr.allsolid )
	{
		if( slayer_tracer_debug.value != 0.0f &&
			( host.realtime - s_last_trace_warn ) >= 1.0 )
		{
			Slayer_Log_Printf(
				"tracer: WARN trace startsolid=%d allsolid=%d from=(%.0f %.0f %.0f) "
				"-- muzzle origin likely inside geometry",
				tr.startsolid, tr.allsolid, from[0], from[1], from[2] );
			s_last_trace_warn = host.realtime;
		}
	}

	if( tr.fraction < 1.0f )
		VectorCopy( tr.endpos, out );
	else
		VectorCopy( far_end, out );
}

// Variant D: try the studio muzzle. The renderer transforms attachment[0] to
// the real gun tip and writes it into the global entity one frame before we
// read it here. We trust it only when it sits a sane distance from the origin
// (a model without attachments leaves it AT the origin; a stale/teleport value
// is absurdly far). Returns true and writes `out` when trusted.
static qboolean Slayer_Tracer_MuzzleFromAttachment( cl_entity_t *ent, vec3_t out )
{
	vec3_t delta;
	float  d;

	if( slayer_tracer_use_attach.value == 0.0f )
		return false;

	VectorSubtract( ent->attachment[0], ent->origin, delta );
	d = VectorLength( delta );

	if( d < slayer_tracer_attach_min.value || d > slayer_tracer_attach_max.value )
		return false;   // degenerate (== origin) or stale -> caller approximates

	VectorCopy( ent->attachment[0], out );
	return true;
}

// Core: a shot was detected on `ent`. is_local selects the aim source.
static void Slayer_Tracer_Fire( cl_entity_t *ent, qboolean is_local )
{
	vec3_t   fwd, right, up;
	vec3_t   start, end;
	vec3_t   muzzle;
	qboolean used_attach = false;

	if( slayer_tracer.value == 0.0f )
		return;

	// Register the shot for barrel-heat colouring (colour computed per frame).
	s_last_shot = host.realtime;

	if( is_local )
	{
		// The aim source depends on the camera mode, and this is exactly where
		// third person used to be wrong.
		//
		// FIRST PERSON: the view ray IS the shot line, so aiming along
		// refState.viewangles from the eye is correct.
		//
		// THIRD PERSON (slayer_thirdperson 1): refState holds the CAMERA, which
		// sits ~120 units behind the player. Tracing from there produced a
		// streak running from the camera towards the camera's crosshair instead
		// of muzzle -> bullet hole: visibly detached from the weapon, and on a
		// different line entirely once the camera orbits (slayer_cam_free).
		// The player's real aim is cl.viewangles (V_ApplySlayerThirdPerson
		// overrides only the render angles and leaves cl.viewangles alone --
		// see the comment at the end of that function), and the eye is
		// cl.simorg + cl.viewheight. So in third person we trace from the
		// PLAYER, and only take the muzzle from the studio attachment.
		qboolean third = ( V_IsSlayerThirdPerson() || CL_IsThirdPerson()) &&
			slayer_tracer_tp_muzzle.value != 0.0f;

		if( third )
		{
			vec3_t eye;

			AngleVectors( cl.viewangles, fwd, right, up );

			VectorAdd( cl.simorg, cl.viewheight, eye );

			// In third person R_RunViewmodelEvents returns early (it bails on
			// PARM_THIRDPERSON), so clgame.viewent.attachment[0] is stale --
			// never trust it here. The local PLAYER entity is drawn in third
			// person, so its attachment[0] is the live gun tip.
			if( Slayer_Tracer_MuzzleFromAttachment( CL_GetLocalPlayer(), muzzle ))
			{
				VectorCopy( muzzle, start );
				used_attach = true;
			}
			else
			{
				// Fall back to eye + forward offset, same shape as the remote
				// approximation, so the streak still leaves the model instead
				// of the camera.
				VectorCopy( eye, start );
				VectorMA( start, slayer_tracer_fwd.value,   fwd,   start );
				VectorMA( start, slayer_tracer_right.value, right, start );
			}
		}
		else
		{
			// First person: aim is exactly the view. Start at the eye so the
			// streak originates at screen centre and flies to the crosshair's
			// target. The local viewmodel dispatches studio events (see
			// R_DrawViewModel), so attachment[0] holds the real gun tip; prefer
			// it for the START while keeping the view direction for aim.
			AngleVectors( refState.viewangles, fwd, right, up );

			if( Slayer_Tracer_MuzzleFromAttachment( &clgame.viewent, muzzle ))
			{
				VectorCopy( muzzle, start );
				used_attach = true;
			}
			else
			{
				VectorCopy( refState.vieworg, start );
			}
		}
	}
	else
	{
		// Remote player: aim along the player's entity angles.
		vec3_t angles;

		VectorCopy( ent->angles, angles );
		AngleVectors( angles, fwd, right, up );

		// Variant D: use the real studio muzzle (attachment[0]) when the
		// renderer has filled it; otherwise approximate from the origin.
		if( Slayer_Tracer_MuzzleFromAttachment( ent, muzzle ))
		{
			VectorCopy( muzzle, start );
			used_attach = true;
		}
		else
		{
			VectorCopy( ent->origin, start );
			VectorMA( start, slayer_tracer_up.value,    up,    start );
			VectorMA( start, slayer_tracer_fwd.value,   fwd,   start );
			VectorMA( start, slayer_tracer_right.value, right, start );
		}
	}

	if( used_attach ) s_attach_used++;
	else              s_attach_reject++;

	Slayer_Tracer_TraceEnd( start, fwd, end );

	// Own ribbon by default; the engine beam stays behind cvar 0 as a fallback.
	if( slayer_tracer_render.value != 0.0f )
		Slayer_Tracer_SpawnOwn( start, end, !is_local );
	else
		Slayer_Tracer_SpawnBeam( start, end, 1.0f );

	if( is_local ) s_fired_local++;
	else           s_fired_remote++;

	// First-sighting probes -- the definitive log line for confidence: proves
	// the muzzleflash path actually reaches this side at least once.
	if( is_local && !s_seen_local_probe )
	{
		s_seen_local_probe = true;
		Slayer_Log_Printf( "tracer: PROBE first LOCAL muzzleflash seen -- local tracers live" );
	}
	if( !is_local && !s_seen_remote_probe )
	{
		s_seen_remote_probe = true;
		Slayer_Log_Printf( "tracer: PROBE first REMOTE muzzleflash seen (ent=%d) -- remote tracers live",
			ent->index );
	}

	// Per-shot line only in verbose mode (debug >= 2): a firefight is hundreds
	// of shots, so the default (debug 1) relies on the once-a-second summary in
	// Slayer_Tracer_Frame instead. debug 0 = silent.
	if( slayer_tracer_debug.value >= 2.0f )
	{
		Slayer_Log_Printf(
			"tracer: %s fire start=(%.0f %.0f %.0f) end=(%.0f %.0f %.0f) heat=%.2f",
			is_local ? "local" : "remote",
			start[0], start[1], start[2], end[0], end[1], end[2], s_heat );
	}
}

// Called from CL_LinkPlayers each frame with the muzzleflash state of each
// player entity. Rising edge (off->on) of EF_MUZZLEFLASH == a new shot.
// slot: 0 for the local viewent, otherwise the client index (1..MAX_CLIENTS).
void Slayer_Tracer_CheckMuzzleflash( cl_entity_t *ent, int slot, qboolean is_local )
{
	qboolean now_on;

	if( !ent || slot < 0 || slot > MAX_CLIENTS )
		return;

	now_on = FBitSet( ent->curstate.effects, EF_MUZZLEFLASH ) ? true : false;

	// Independent probe: count RAW flag sightings, before the edge test. If the
	// summary later shows raw>0 but fired==0 for a side, the flag is arriving
	// but our rising-edge logic is dropping it; if raw==0, the flag itself
	// never reaches this side (the real question for remote players).
	if( now_on )
	{
		if( is_local ) s_mf_raw_local++;
		else           s_mf_raw_remote++;
	}

	if( now_on && !s_mf_prev[slot] )
		Slayer_Tracer_Fire( ent, is_local );

	s_mf_prev[slot] = now_on;
}

// Independent cross-check hook: called from the TE_TRACER temp-entity handler
// (cl_tent.c). TE_TRACER is a completely separate, server-driven path from the
// EF_MUZZLEFLASH one. If muzzleflash turns out not to cover remote players but
// this counter climbs during firefights, the server IS telling us about shots
// and we switch the remote tracer source to it -- decided from the log, not a
// guess. Counting only; does not spawn (yet).
void Slayer_Tracer_NoteServerTracer( const vec3_t start, const vec3_t end )
{
	(void)start;
	(void)end;
	s_te_tracer++;
}

void Slayer_Tracer_Frame( void )
{
	static double last_time = 0.0;
	double        now = host.realtime;
	double        dt;
	byte          col[3];

	if( last_time == 0.0 )
		last_time = now;
	dt = now - last_time;
	last_time = now;
	if( dt < 0.0 ) dt = 0.0;      // clock reset guard
	if( dt > 0.25 ) dt = 0.25;    // don't lurch after a hitch

	if( slayer_tracer.value == 0.0f || slayer_tracer_heat.value == 0.0f )
	{
		s_heat = 0.0f;
	}
	else
	{
		float full = slayer_tracer_heat_full.value;
		float gap  = slayer_tracer_heat_gap.value;
		float cool = slayer_tracer_heat_cool.value;
		qboolean firing;

		if( full < 0.1f ) full = 0.1f;
		if( cool < 0.1f ) cool = 0.1f;

		firing = ( s_last_shot != 0.0 ) && (( now - s_last_shot ) <= gap );

		if( firing )
			s_heat += (float)dt / full;
		else
			s_heat -= (float)( dt * cool ) / full;

		if( s_heat < 0.0f ) s_heat = 0.0f;
		if( s_heat > 1.0f ) s_heat = 1.0f;
	}

	// Heat colour is applied directly to each beam at spawn time
	// (Slayer_Tracer_SpawnBeam), so nothing to push to the engine's vanilla
	// tracer slot here -- CS 1.6 never draws those anyway. We just keep the
	// heat state machine advancing. `col` retained for a future HUD readout.
	(void)col;
	Slayer_Tracer_HeatColor( s_heat, col );

	// Once-a-second diagnostic summary: how many shots fired (local vs remote),
	// how many actually produced a beam, and how many didn't. This is the key
	// signal for confirming from a device log that EF_MUZZLEFLASH fires for
	// REMOTE players in CS 1.6 -- if s_fired_remote stays 0 while other players
	// are shooting, the muzzleflash path doesn't cover them and we switch to a
	// server-side tracer source. Gated by slayer_tracer_debug (default 1).
	if( slayer_tracer_debug.value != 0.0f &&
		( s_fired_local || s_fired_remote || s_beam_fail_model || s_beam_fail_null ||
		  s_mf_raw_local || s_mf_raw_remote || s_te_tracer ) &&
		( now - s_last_summary ) >= 1.0 )
	{
		Slayer_Log_Printf(
			"tracer: 1s summary fired[L=%d R=%d] rawMF[L=%d R=%d] beams[ok=%d noModel=%d null=%d] "
			"attach[used=%d approx=%d] TE_TRACER=%d heat=%.2f model=%d "
			"own[render=%d peak=%d/%d tp=%d]",
			s_fired_local, s_fired_remote, s_mf_raw_local, s_mf_raw_remote,
			s_beam_ok, s_beam_fail_model, s_beam_fail_null,
			s_attach_used, s_attach_reject, s_te_tracer,
			s_heat, s_beam_model,
			(int)slayer_tracer_render.value, s_live_peak, SLAYER_TRACER_POOL,
			( V_IsSlayerThirdPerson() || CL_IsThirdPerson()) ? 1 : 0 );

		s_fired_local = s_fired_remote = s_beam_ok = 0;
		s_beam_fail_model = s_beam_fail_null = 0;
		s_mf_raw_local = s_mf_raw_remote = s_te_tracer = 0;
		s_attach_used = s_attach_reject = 0;
		s_live_peak = 0;
		s_last_summary = now;
	}
}

qboolean Slayer_Tracer_SuppressVanillaSparks( void )
{
	return ( slayer_tracer.value != 0.0f && slayer_tracer_sparks.value != 0.0f );
}

void Slayer_Tracer_LogEvent( int eventindex, const char *name,
	const float *origin, const float *angles )
{
	// Only log each distinct event index once per map, and only a bounded
	// number of them: a firefight replays the same few events hundreds of
	// times, and the point is to learn WHICH events exist, not to count them.
	static int  seen[64];
	static int  seen_count = 0;
	int         i;

	if( slayer_tracer_logevents.value == 0.0f )
		return;

	for( i = 0; i < seen_count; i++ )
	{
		if( seen[i] == eventindex )
			return;   // already reported this one
	}

	if( seen_count < (int)( sizeof( seen ) / sizeof( seen[0] )))
		seen[seen_count++] = eventindex;

	Slayer_Log_Printf( "event: idx=%d name='%s' org=(%.0f %.0f %.0f) ang=(%.0f %.0f %.0f)",
		eventindex, name ? name : "?",
		origin ? origin[0] : 0.0f, origin ? origin[1] : 0.0f, origin ? origin[2] : 0.0f,
		angles ? angles[0] : 0.0f, angles ? angles[1] : 0.0f, angles ? angles[2] : 0.0f );
}
