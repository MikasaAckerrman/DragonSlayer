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
		// First person: aim is exactly the view. Start at the eye so the streak
		// originates at screen centre and flies to the crosshair's target.
		// The local viewmodel now also dispatches studio events (see
		// R_DrawViewModel), so attachment[0] holds the real gun tip; prefer it
		// for the START while keeping the view direction for aim.
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
			"attach[used=%d approx=%d] TE_TRACER=%d heat=%.2f model=%d",
			s_fired_local, s_fired_remote, s_mf_raw_local, s_mf_raw_remote,
			s_beam_ok, s_beam_fail_model, s_beam_fail_null,
			s_attach_used, s_attach_reject, s_te_tracer,
			s_heat, s_beam_model );

		s_fired_local = s_fired_remote = s_beam_ok = 0;
		s_beam_fail_model = s_beam_fail_null = 0;
		s_mf_raw_local = s_mf_raw_remote = s_te_tracer = 0;
		s_attach_used = s_attach_reject = 0;
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
