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
#include "studio.h"      // studiohdr_t, for reading which attachment fired
#include "r_efx.h"       // BEAM / struct beam_s, beamdef flags
#include "cl_tent.h"     // R_BeamPoints, R_TracerEffect
#include "cl_tracer_slayer.h"
#include "cl_tracer_render_slayer.h"   // our own ribbon geometry
#include "cl_muzzle_slayer.h"          // which attachment the animation fires from
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

// PENETRATION. A plain trace stops at the first surface, but bullets in CS go
// through thin walls, doors, crates -- and through a player into the one behind
// him. A tracer that stops on the near face is visibly disconnected from the hole
// the shot actually left.
//
// The engine has no penetration query, so it is inferred geometrically: step past
// the surface and check whether the far side is open. That also means a tracer is
// never drawn through geometry the bullet could not have passed, which matters as
// much as showing the ones it did.
#define SLAYER_TRACER_MAX_PIERCE 4

static CVAR_DEFINE_AUTO( slayer_tracer_pierce, "2", FCVAR_ARCHIVE,
	"Slayer3D: how many thin surfaces a tracer may continue through (0 = none)" );

static CVAR_DEFINE_AUTO( slayer_tracer_pierce_max, "12", FCVAR_ARCHIVE,
	"Slayer3D: thickest surface a tracer will pass through, in units" );

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

// WHICH attachment. Not always 0 -- see cl_muzzle_slayer.h for the measurement.
//
// The Dual Berettas were firing every tracer from the left gun because the muzzle
// was hardcoded to attachment[0]. The model itself says otherwise: v_elite's
// shoot_left* sequences carry studio event 5001 (attachment[0]) and its
// shoot_right* sequences carry 5011 (attachment[1]), and the player models carry
// the same pair the other way round for their two dual-pistol animations. So the
// muzzle is a property of the ANIMATION PLAYING, read from the model, per shot.
//
// 0 pins it back to attachment[0] -- the pre-change behaviour, kept as an escape
// hatch for a broken custom model rather than as a preference.
static CVAR_DEFINE_AUTO( slayer_tracer_muzzle_seq, "1", FCVAR_ARCHIVE,
	"Slayer3D: read the firing attachment from the playing animation (0 = always attachment[0])" );

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

// Visibility ramp. The floor is deliberately HIGH: CS2's own numbers
// (start alpha 0, fade-in over the first 20% of life) put first visibility
// 810 units downrange, which on 1.6-sized maps reads as the streak appearing
// out of thin air. 0.55 means it is visible from the very first frame and just
// firms up over the next few percent of its life.
static CVAR_DEFINE_AUTO( slayer_tracer_fade_in, "0.06", FCVAR_ARCHIVE,
	"Slayer3D: fraction of lifetime spent ramping to full brightness" );

static CVAR_DEFINE_AUTO( slayer_tracer_fade_floor, "0.55", FCVAR_ARCHIVE,
	"Slayer3D: brightness at spawn, 0..1 (low values = tracer pops in mid-air)" );

static CVAR_DEFINE_AUTO( slayer_tracer_fade_out, "0.88", FCVAR_ARCHIVE,
	"Slayer3D: fraction of lifetime after which the tracer fades out" );

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

static CVAR_DEFINE_AUTO( slayer_tracer_impact_window, "0.35", FCVAR_ARCHIVE,
	"Slayer3D: seconds a recorded bullet impact waits for its shot (must exceed ping)" );

// INSTANT LOCAL TRACERS. This is the answer to "трассеры будто зависят от пинга".
//
// They did, and it was structural rather than a bug in the timing constants. A
// local tracer was placed on the rising edge of EF_MUZZLEFLASH, and the flag we
// can observe is the SERVER'S ECHO: the locally predicted flag is set and cleared
// inside a single frame, before the edge test in CL_LinkPlayers ever sees it
// (see the s_impacts comment). So every one of our own tracers appeared one full
// round trip after the shot -- 60 ms on a good server, 130 on the one in the
// report. The gun fires, and the streak follows visibly later.
//
// The client already knows everything needed, immediately: it PREDICTS its own
// weapon event, and that event computes the spread and reports the exact impact
// through R_BulletImpactParticles -> Slayer_Tracer_NoteImpact, with no server
// involved. So the tracer is spawned there, in the same frame as the shot, and
// the echo that arrives later is credited against it instead of drawing a second
// streak.
//
// Remote players are untouched: their shots are only knowable from the snapshot,
// so their tracers legitimately arrive with the snapshot.
static CVAR_DEFINE_AUTO( slayer_tracer_instant, "1", FCVAR_ARCHIVE,
	"Slayer3D: draw your own tracer from the predicted weapon event, without waiting for the server echo (0 = old behaviour)" );

// How long a DETECTED shot waits for an impact that has not happened yet. Kept
// SHORT and separate from the window above, because the two directions are not
// symmetric: the impact normally happens first (see s_impacts), so if we already
// saw the shot and no impact is on record, there will not be one -- the bullet
// went into the skybox or the mod drew no decal. Waiting the full ping there
// would delay a visible tracer by that much for no gain.
static CVAR_DEFINE_AUTO( slayer_tracer_impact_grace, "0.03", FCVAR_ARCHIVE,
	"Slayer3D: seconds a detected shot waits for a not-yet-seen impact" );

// Anti-aliasing. `smooth` builds the profile texture with a mip chain, which is
// what stops a distant 1-2 pixel streak from crawling pixel to pixel;
// `soft_px` widens+dims a sub-pixel streak so it fades instead of flickering.
static CVAR_DEFINE_AUTO( slayer_tracer_smooth, "1", FCVAR_ARCHIVE,
	"Slayer3D: filter the tracer profile with mipmaps (0 = crisp/aliased)" );

static CVAR_DEFINE_AUTO( slayer_tracer_soft_px, "2.6", FCVAR_ARCHIVE,
	"Slayer3D: screen width below which a tracer is widened and dimmed instead of aliasing" );

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
static int    s_muzzle_alt    = 0;   // shots that fired from an attachment OTHER than 0
static int    s_muzzle_fallback = 0; // sequence named an attachment the renderer left empty
static double s_last_trace_warn = 0.0;

// One-shot probes: the single most important thing the morning device log has
// to answer is "does EF_MUZZLEFLASH ever fire for REMOTE players in CS 1.6?"
// (the local side is proven). We log the first time each side is seen so the
// answer is one grep away instead of buried in the summary counts.
static qboolean s_seen_local_probe  = false;
static qboolean s_seen_remote_probe = false;

// Local shot rendezvous. The weapon event computes spread and later reports
// the real impact to R_BulletImpactParticles; spawning at muzzleflash time with
// an independent view trace can never match that hole. Hold one short-lived
// pending shot and finish it from the actual impact. Full-auto fire is slower
// than the 80 ms default window in CS; a fixed queue of four covers custom
// weapons without allocating.
#define SLAYER_PENDING_SHOTS 4
typedef struct
{
	qboolean active;
	double   time;
	vec3_t   start;
	vec3_t   fallback;
} slayer_pending_shot_t;

static slayer_pending_shot_t s_pending[SLAYER_PENDING_SHOTS];
static int s_pending_next;
static int s_impact_paired;
static int s_impact_fallback;
static int s_impact_foreign;   // impacts rejected as belonging to another player
static int s_impact_back;      // shots closed by an impact that arrived FIRST
static int s_pierced;          // surfaces a tracer continued through

// Recent local impacts, for the case where the impact arrives BEFORE we detect
// the shot. That is in fact the NORMAL case, and missing it was a real bug in
// the first version of this code:
//
//   * the client predicts its own weapon event immediately, and the event both
//     sets EF_MUZZLEFLASH on the VIEWMODEL and reports the impact;
//   * but our rising-edge check runs in CL_LinkPlayers, which happens BEFORE
//     CL_FireEvents in the same frame, and the renderer clears that viewmodel
//     flag later in the very same frame (R_StudioClientEvents);
//   * so the flag we actually see is the one the SERVER echoes back in the
//     player snapshot -- one round trip after the impact already happened.
//
// Waiting forward from the shot therefore never matched anything and every
// local tracer silently fell back to the view-trace endpoint, i.e. the exact
// bug this whole mechanism was supposed to fix. Hence a rendezvous in BOTH
// directions: the impact waits for the shot too.
#define SLAYER_IMPACT_RING 8
typedef struct
{
	qboolean used;
	double   time;
	vec3_t   pos;
} slayer_impact_rec_t;

static slayer_impact_rec_t s_impacts[SLAYER_IMPACT_RING];
static int s_impacts_next;

// INSTANT LOCAL SHOTS -- tracers already drawn from the predicted weapon event.
//
// The echo of EF_MUZZLEFLASH still arrives a round trip later for a shot we have
// already drawn, and it must not draw a second streak. So each instant tracer
// leaves a credit here, and the echo consumes one instead of firing.
//
// A credit is not just a counter: it must EXPIRE, or a burst that outruns the
// ring would silently suppress later, legitimate echoes. And the count has to be
// per-shot rather than a single flag, because a full-auto rifle has several shots
// in flight between the muzzle and the echo.
#define SLAYER_INSTANT_CREDITS 8
typedef struct
{
	qboolean used;
	double   time;
} slayer_instant_rec_t;

static slayer_instant_rec_t s_instant[SLAYER_INSTANT_CREDITS];
static int s_instant_next;
static int s_instant_drawn;     // diagnostics: tracers drawn without the server
static int s_instant_echo_used; // echoes credited against an instant tracer
static int s_instant_expired;   // credits that timed out unused

// Which player's weapon event is executing right now, 0 = none. Set by
// CL_FireEvent around the client DLL callback. This is the ONLY reliable way to
// attribute an impact: at point-blank range two players' impacts are metres
// apart and arrive in the same frame, so neither time nor direction can
// separate them.
static int s_event_owner;

// WHICH BARREL, in FIRE ORDER, for local shots.
//
// This queue exists because of the timing already documented above, and without
// it the two-barrel fix would be not merely incomplete but INVERTED.
//
// A local shot is detected from the SERVER'S ECHO of EF_MUZZLEFLASH, one round
// trip after the shot happened (the locally predicted flag is set and cleared
// inside a single frame, before our edge test ever sees it). By then
// cl.local.weaponsequence has moved on: with the Dual Berettas at ~0.12 s between
// shots and any ordinary ping, the animation playing when the echo lands is the
// NEXT shot's. Reading the sequence at detection time would therefore have
// swapped left and right systematically -- alternating, and wrong, which is worse
// than the original bug because it looks plausible.
//
// So the barrel is sampled where it is unambiguous: immediately after the weapon
// event returns, which is the moment the client library has just called
// pfnWeaponAnim for THIS shot. The samples are consumed oldest-first by the
// detector. Same rendezvous shape as the impact pairing, for the same reason.
#define SLAYER_MUZZLE_QUEUE 8
typedef struct
{
	qboolean used;
	double   time;
	unsigned int order;   // fire order; see below
	int      attach;
} slayer_muzzle_rec_t;

static slayer_muzzle_rec_t s_muzzleq[SLAYER_MUZZLE_QUEUE];
// ORDER, not a head/tail pair. A ring indexed by two cursors cannot distinguish
// "empty" from "full" without wasting a slot, and the version that did got it
// wrong: eight pushes brought tail back to head and the whole queue read as
// empty. A monotonic counter has no such state -- oldest is simply smallest --
// and it also breaks the tie between two shots that land in the same frame,
// which a timestamp cannot.
static unsigned int s_muzzleq_order;
static int s_muzzleq_hit;    // shots placed from the queue
static int s_muzzleq_miss;   // shots that had to read the live sequence instead

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

// Was the pool draw hook even called? "No tracers on the server" is consistent
// with the hook never running (the renderer's translucent pass not reaching
// CL_DrawEFX) and with it running over an invisible pool, and the old summary
// reported the same thing in both cases.
static int             s_pool_draw_calls = 0;
static int             s_pool_draw_gated = 0;   // returned on the cvar gate

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
	st->fade_in_end   = slayer_tracer_fade_in.value;
	st->fade_in_floor = slayer_tracer_fade_floor.value;
	st->fade_out_start = slayer_tracer_fade_out.value;
	st->min_px       = slayer_tracer_min_px.value;
	st->max_px       = slayer_tracer_max_px.value;
	st->smooth       = ( slayer_tracer_smooth.value != 0.0f ) ? 1 : 0;
	st->soft_px      = slayer_tracer_soft_px.value;

	// Guards: a zero/garbage cvar must not divide by zero or spin the loop.
	if( st->speed < 100.0f ) st->speed = 100.0f;
	if( st->length < 32.0f ) st->length = 32.0f;
	if( st->radius < 0.05f ) st->radius = 0.05f;
	if( st->segments < 2 )  st->segments = 2;
	if( st->segments > 64 ) st->segments = 64;
	if( st->life_mul < 0.1f ) st->life_mul = 0.1f;
	if( st->min_px < 0.1f ) st->min_px = 0.1f;
	if( st->max_px < st->min_px ) st->max_px = st->min_px;
	if( st->fade_in_end < 0.0f ) st->fade_in_end = 0.0f;
	if( st->fade_in_end > 0.9f ) st->fade_in_end = 0.9f;
	if( st->fade_in_floor < 0.0f ) st->fade_in_floor = 0.0f;
	if( st->fade_in_floor > 1.0f ) st->fade_in_floor = 1.0f;
	if( st->fade_out_start < 0.1f ) st->fade_out_start = 0.1f;
	if( st->fade_out_start > 1.0f ) st->fade_out_start = 1.0f;
	if( st->soft_px < 0.0f ) st->soft_px = 0.0f;
	if( st->soft_px > 12.0f ) st->soft_px = 12.0f;
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

	// The pool draw hook ran. Counted separately from the tracers it draws,
	// because "no tracers on servers" has two very different explanations and the
	// old summary could not tell them apart: the hook not being called at all
	// (the renderer never reaches CL_DrawEFX in the translucent pass), versus the
	// hook running over an empty or invisible pool.
	s_pool_draw_calls++;

	if( slayer_tracer.value == 0.0f || slayer_tracer_render.value == 0.0f )
	{
		s_pool_draw_gated++;
		return;
	}

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

	// Additive TriAPI mode disabled depth writes; the viewmodel is drawn right
	// after this pass and needs them back. Only when we actually drew, so a
	// quiet frame does not touch renderer state at all.
	if( live > 0 )
		Slayer_TracerRender_EndFrame();
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
	Cvar_RegisterVariable( &slayer_tracer_pierce );
	Cvar_RegisterVariable( &slayer_tracer_pierce_max );
	Cvar_RegisterVariable( &slayer_tracer_fwd );
	Cvar_RegisterVariable( &slayer_tracer_up );
	Cvar_RegisterVariable( &slayer_tracer_right );
	Cvar_RegisterVariable( &slayer_tracer_use_attach );
	Cvar_RegisterVariable( &slayer_tracer_attach_min );
	Cvar_RegisterVariable( &slayer_tracer_attach_max );
	Cvar_RegisterVariable( &slayer_tracer_muzzle_seq );
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
	Cvar_RegisterVariable( &slayer_tracer_fade_in );
	Cvar_RegisterVariable( &slayer_tracer_fade_floor );
	Cvar_RegisterVariable( &slayer_tracer_fade_out );
	Cvar_RegisterVariable( &slayer_tracer_min_px );
	Cvar_RegisterVariable( &slayer_tracer_max_px );
	Cvar_RegisterVariable( &slayer_tracer_tp_muzzle );
	Cvar_RegisterVariable( &slayer_tracer_impact_window );
	Cvar_RegisterVariable( &slayer_tracer_instant );
	Cvar_RegisterVariable( &slayer_tracer_impact_grace );
	Cvar_RegisterVariable( &slayer_tracer_smooth );
	Cvar_RegisterVariable( &slayer_tracer_soft_px );

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
	s_muzzle_alt = s_muzzle_fallback = 0;
	memset( s_muzzleq, 0, sizeof( s_muzzleq ));
	s_muzzleq_order = 0;
	s_muzzleq_hit = s_muzzleq_miss = 0;
	s_last_summary = 0.0;
	s_last_trace_warn = 0.0;
	s_seen_local_probe = s_seen_remote_probe = false;
	memset( s_pending, 0, sizeof( s_pending ));
	s_pending_next = 0;
	memset( s_impacts, 0, sizeof( s_impacts ));
	s_impacts_next = 0;
	// A cleared ring is all-zero, i.e. `used == false` with time 0. That would
	// look like a valid impact at time 0; mark every slot consumed instead.
	{
		int k;

		for( k = 0; k < SLAYER_IMPACT_RING; k++ )
			s_impacts[k].used = true;
	}
	s_impact_paired = s_impact_fallback = s_impact_foreign = s_impact_back = 0;
	s_pierced = 0;
	s_event_owner = 0;

	// Own pool: clear it so tracers from the previous map cannot be drawn with
	// stale world coordinates on the first frame of the new one.
	memset( s_pool, 0, sizeof( s_pool ));
	s_pool_next = 0;
	s_live_peak = 0;

	// Instant-tracer credits describe shots on the OLD map. A credit surviving a
	// map change would swallow the first echo on the new one.
	memset( s_instant, 0, sizeof( s_instant ));
	s_instant_next = 0;
	s_instant_drawn = s_instant_echo_used = s_instant_expired = 0;
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
/*
====================
Slayer_Tracer_TraceEnd

Where does this shot's streak end?

A plain trace stops at the FIRST thing it hits, and that is wrong in CS: bullets
penetrate. A shot through a thin wall, a door, a crate, or through a player into
the one behind him leaves a hole on the far side, and a tracer that stops on the
near face is visibly disconnected from what actually happened -- the reported
"tracers do not show through penetrations, players included".

The engine has no "does this bullet penetrate?" query, so penetration is inferred
from geometry, which is what the game itself does: step a short distance past the
surface, and if the point beyond it is OPEN, the surface was thin enough to shoot
through. Thick geometry keeps the point inside solid and the streak stops there,
which is the whole point -- a tracer must NOT be drawn through a wall it could not
pass.

Bounded by slayer_tracer_pierce (how many surfaces) and slayer_tracer_pierce_max
(how thick each may be), so the cost is a handful of traces on the rare shot that
penetrates and exactly one on every other.
====================
*/
static void Slayer_Tracer_TraceEnd( const vec3_t from, const vec3_t dir, vec3_t out )
{
	vec3_t    far_end;
	vec3_t    origin;
	pmtrace_t tr;
	float     range = slayer_tracer_range.value;
	float     thick = slayer_tracer_pierce_max.value;
	int       pierce = (int)slayer_tracer_pierce.value;
	int       i;

	if( range < 64.0f ) range = 64.0f;
	if( thick < 0.0f ) thick = 0.0f;
	if( thick > 64.0f ) thick = 64.0f;
	if( pierce < 0 ) pierce = 0;
	if( pierce > SLAYER_TRACER_MAX_PIERCE ) pierce = SLAYER_TRACER_MAX_PIERCE;

	VectorMA( from, range, dir, far_end );
	VectorCopy( from, origin );

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

	if( tr.fraction >= 1.0f )
	{
		VectorCopy( far_end, out );
		return;
	}

	VectorCopy( tr.endpos, out );

	// --- penetration ---------------------------------------------------------
	for( i = 0; i < pierce && thick > 0.0f; i++ )
	{
		vec3_t    beyond;
		pmtrace_t probe;

		// Just past the surface. Nudged along the ray rather than along the
		// surface normal: the bullet's path is the ray, and a glancing hit on a
		// steep face has a normal pointing somewhere the bullet never goes.
		VectorMA( out, thick, dir, beyond );

		// Is the far side open? Trace BACK from there to the hit point: a forward
		// trace from a point inside solid reports startsolid and no distance,
		// which cannot distinguish "thin wall" from "half a metre of concrete".
		// Backwards, the fraction tells us how far the solid extends.
		probe = CL_TraceLine( beyond, out, PM_STUDIO_BOX );

		if( probe.startsolid || probe.allsolid )
			break;      // still inside geometry: too thick to shoot through

		// The far side is open, so continue the shot from there.
		VectorCopy( beyond, origin );
		tr = CL_TraceLine( origin, far_end, PM_STUDIO_BOX );

		s_pierced++;

		if( tr.fraction >= 1.0f )
		{
			VectorCopy( far_end, out );
			return;
		}

		VectorCopy( tr.endpos, out );
	}
}

/*
====================
Slayer_Tracer_MuzzleIndexFromModel

WHICH of the entity's four attachment points is the barrel that just fired?

Read from the model, for the sequence the entity is playing NOW. See
cl_muzzle_slayer.h for the measurement; the short version is that studio event
5001/5011/5021/5031 in a firing animation means "muzzle flash at attachment
0/1/2/3", and the Dual Berettas use two different ones for their two hands.

Correct for the PLAYER entity (third person and remote players): its sequence
arrives in the same snapshot as the EF_MUZZLEFLASH echo we detect the shot from,
so the two describe the same shot. NOT correct for the viewmodel, whose sequence
is client-predicted and has already advanced by then -- that case uses the queue
below.

Returns 0 when the model says nothing, because attachment[0] is what every
single-barrel weapon uses and is what this code did unconditionally before.
====================
*/
static int Slayer_Tracer_MuzzleIndexFromModel( cl_entity_t *ent )
{
	studiohdr_t *phdr;
	int          idx;

	if( slayer_tracer_muzzle_seq.value == 0.0f )
		return 0;

	if( !ent || !ent->model )
		return 0;

	// Sequence groups and non-studio models have no events to read.
	if( ent->model->type != mod_studio )
		return 0;

	phdr = (studiohdr_t *)Mod_StudioExtradata( ent->model );
	if( !phdr || phdr->length <= 0 )
		return 0;

	idx = Slayer_Muzzle_AttachmentForSequence( phdr, (int)phdr->length,
		ent->curstate.sequence );

	// -1 means "this animation is not a shot, or says nothing". Not an error and
	// not a reason to move the tracer: idle and reload reach here too, on the
	// frame after a shot, and answering anything but 0 for them would make the
	// start point depend on how quickly the animation advanced.
	if( idx < 0 || idx >= 4 )
		return 0;

	return idx;
}

// Variant D: try the studio muzzle. The renderer transforms the attachments to
// the real gun tips and writes them into the global entity one frame before we
// read them here. We trust the one this shot came from only when it sits a sane
// distance from the origin (a model without attachments leaves them AT the
// origin; a stale/teleport value is absurdly far). Returns true and writes `out`
// when trusted.
static qboolean Slayer_Tracer_MuzzleFromAttachment( cl_entity_t *ent, int idx, vec3_t out )
{
	vec3_t delta;
	float  d;

	if( slayer_tracer_use_attach.value == 0.0f )
		return false;

	if( idx < 0 || idx >= 4 )
		idx = 0;

	if( idx != 0 )
		s_muzzle_alt++;

	VectorSubtract( ent->attachment[idx], ent->origin, delta );
	d = VectorLength( delta );

	if( d < slayer_tracer_attach_min.value || d > slayer_tracer_attach_max.value )
	{
		// The chosen attachment is unusable. Fall back to [0] before giving up on
		// attachments entirely: a model can carry a muzzle event for an attachment
		// the renderer never filled (an edited model, or a sequence played on a
		// model it was not authored for), and in that case attachment[0] is still
		// the real gun tip and is much better than the origin+offset guess.
		if( idx != 0 )
		{
			VectorSubtract( ent->attachment[0], ent->origin, delta );
			d = VectorLength( delta );

			if( d >= slayer_tracer_attach_min.value
			 && d <= slayer_tracer_attach_max.value )
			{
				s_muzzle_fallback++;
				VectorCopy( ent->attachment[0], out );
				return true;
			}
		}

		return false;   // degenerate (== origin) or stale -> caller approximates
	}

	VectorCopy( ent->attachment[idx], out );
	return true;
}

/*
====================
Slayer_Tracer_PopMuzzle

Take the oldest un-consumed barrel sample, for a FIRST-PERSON shot.

Stale entries are dropped rather than used: a sample older than the pairing
window belongs to a shot whose muzzleflash echo never arrived (packet loss, or
the player died mid-burst), and using it would attribute this shot to the wrong
barrel for the rest of the magazine. Falling back to the live sequence is a
one-shot error; a desynchronised queue is a permanent one.

Returns the attachment index, or -1 when the queue has nothing usable.
====================
*/
static int Slayer_Tracer_PopMuzzle( void )
{
	double window = slayer_tracer_impact_window.value * 4.0;
	int    best = -1;
	int    i;

	if( window < 0.5 ) window = 0.5;   // a full round trip, generously

	for( ;; )
	{
		best = -1;

		// Oldest live entry, by fire order.
		for( i = 0; i < SLAYER_MUZZLE_QUEUE; i++ )
		{
			if( !s_muzzleq[i].used )
				continue;
			if( best < 0 || s_muzzleq[i].order < s_muzzleq[best].order )
				best = i;
		}

		if( best < 0 )
			break;

		s_muzzleq[best].used = false;

		// Stale entries are dropped rather than used: a sample older than the
		// window belongs to a shot whose muzzleflash echo never arrived (packet
		// loss, or the player died mid-burst), and using it would attribute this
		// shot to the wrong barrel for the rest of the magazine. Falling back to
		// the live sequence is a one-shot error; a desynchronised queue is a
		// permanent one. Keep dropping until something fresh turns up.
		if( host.realtime - s_muzzleq[best].time > window )
			continue;

		s_muzzleq_hit++;
		return s_muzzleq[best].attach;
	}

	s_muzzleq_miss++;
	return -1;
}

static void Slayer_Tracer_SpawnVisual( const vec3_t start, const vec3_t end, qboolean is_remote )
{
	if( slayer_tracer_render.value != 0.0f )
		Slayer_Tracer_SpawnOwn( start, end, is_remote );
	else
		Slayer_Tracer_SpawnBeam( start, end, 1.0f );
}

/*
====================
Slayer_Tracer_LocalMuzzleNow

The local gun tip, RIGHT NOW, for a shot the client is predicting this frame.

Only correct inside the weapon event, and that is the whole point: at that moment
the client library has just called pfnWeaponAnim for THIS shot, so the
viewmodel's sequence names the barrel that fired and the renderer's attachment
for it is the live muzzle. (The same reasoning as Slayer_Tracer_EndEvent, which
samples the barrel index there for exactly this reason.)

Returns false when there is no trustworthy muzzle -- the caller then has nothing
better than the echo path, so it should not draw early.
====================
*/
static qboolean Slayer_Tracer_LocalMuzzleNow( vec3_t out )
{
	cl_entity_t *src;
	int          idx;

	// Third person draws the PLAYER, not the viewmodel, and R_RunViewmodelEvents
	// bails out there -- so the viewmodel's attachments are stale. Same split as
	// the echo path makes for the same reason.
	if( V_IsSlayerThirdPerson() || CL_IsThirdPerson( ))
	{
		if( slayer_tracer_tp_muzzle.value == 0.0f )
			return false;

		src = CL_GetLocalPlayer();
	}
	else
	{
		src = &clgame.viewent;
	}

	if( !src )
		return false;

	idx = Slayer_Tracer_MuzzleIndexFromModel( src );

	return Slayer_Tracer_MuzzleFromAttachment( src, idx, out );
}

/*
====================
Slayer_Tracer_InstantCredit / Slayer_Tracer_TakeInstantCredit

Bookkeeping between the two halves of a local shot.

A tracer drawn from the predicted event leaves a credit; the server's
EF_MUZZLEFLASH echo for that same shot arrives a round trip later and consumes
it instead of drawing a second streak.

Credits EXPIRE, and that is not a detail: without it, a burst that overflowed the
ring would leave stale credits behind and silently swallow later, legitimate
echoes -- which would look exactly like the tracers being broken again.
====================
*/
static void Slayer_Tracer_InstantCredit( void )
{
	int i;

	// Prefer a free slot; otherwise take the oldest, which is the one whose echo
	// is least likely to still be in flight.
	int oldest = 0;

	for( i = 0; i < SLAYER_INSTANT_CREDITS; i++ )
	{
		if( !s_instant[i].used )
		{
			s_instant[i].used = true;
			s_instant[i].time = host.realtime;
			return;
		}

		if( s_instant[i].time < s_instant[oldest].time )
			oldest = i;
	}

	s_instant[oldest].used = true;
	s_instant[oldest].time = host.realtime;
}

static qboolean Slayer_Tracer_TakeInstantCredit( void )
{
	double window = slayer_tracer_impact_window.value;
	int    best = -1;
	int    i;

	// The echo is one round trip behind, so the credit must outlive the ping.
	// Same bound as the impact pairing window, for the same reason.
	if( window < 0.1 ) window = 0.1;
	if( window > 0.5 ) window = 0.5;

	for( i = 0; i < SLAYER_INSTANT_CREDITS; i++ )
	{
		if( !s_instant[i].used )
			continue;

		if( host.realtime - s_instant[i].time > window )
		{
			s_instant[i].used = false;   // stale: the echo never came
			s_instant_expired++;
			continue;
		}

		// Oldest first: shots are echoed in the order they were fired.
		if( best < 0 || s_instant[i].time < s_instant[best].time )
			best = i;
	}

	if( best < 0 )
		return false;

	s_instant[best].used = false;
	s_instant_echo_used++;
	return true;
}

static void Slayer_Tracer_QueueLocal( const vec3_t start, const vec3_t fallback )
{
	slayer_pending_shot_t *p;
	double window = slayer_tracer_impact_window.value;
	int    i, best = -1;
	double best_age = 999.0;
	float  best_dot = 0.94f;
	vec3_t expected;
	float  expected_len;

	if( window < 0.0 ) window = 0.0;
	// Upper bound 0.5 s, not 0.25: the pairing window must exceed the ping
	// (the muzzleflash we see is the server's echo), and 250 ms rules out
	// perfectly ordinary high-latency servers.
	if( window > 0.5 ) window = 0.5;

	// FIRST look BACKWARDS: the impact usually lands before we see the shot
	// (see the comment on s_impacts). Same direction cone as the forward path,
	// so a hole from a different shot cannot be adopted.
	VectorSubtract( fallback, start, expected );
	expected_len = VectorLength( expected );

	if( expected_len >= 1.0f )
	{
		for( i = 0; i < SLAYER_IMPACT_RING; i++ )
		{
			double age;
			vec3_t actual;
			float  actual_len, dot;

			if( s_impacts[i].used )
				continue;
			age = host.realtime - s_impacts[i].time;
			if( age < 0.0 || age > window )
				continue;

			VectorSubtract( s_impacts[i].pos, start, actual );
			actual_len = VectorLength( actual );
			if( actual_len < 1.0f )
				continue;
			dot = DotProduct( expected, actual ) / ( expected_len * actual_len );

			if( dot > best_dot || ( fabs( dot - best_dot ) < 0.0001f && age < best_age ))
			{
				best = i;
				best_dot = dot;
				best_age = age;
			}
		}
	}

	if( best >= 0 )
	{
		Slayer_Tracer_SpawnVisual( start, s_impacts[best].pos, false );
		s_impacts[best].used = true;
		s_impact_back++;
		return;
	}

	// No matching impact yet: queue and let Slayer_Tracer_NoteImpact or the
	// timeout finish it.
	p = &s_pending[s_pending_next];

	// If a custom weapon outruns the queue, preserve the displaced tracer with
	// its fallback endpoint instead of silently dropping a shot.
	if( p->active )
	{
		Slayer_Tracer_SpawnVisual( p->start, p->fallback, false );
		s_impact_fallback++;
	}

	p->active = true;
	p->time = host.realtime;
	VectorCopy( start, p->start );
	VectorCopy( fallback, p->fallback );
	s_pending_next = ( s_pending_next + 1 ) % SLAYER_PENDING_SHOTS;
}

// Core: a shot was detected on `ent`. is_local selects the aim source.
static void Slayer_Tracer_Fire( cl_entity_t *ent, qboolean is_local )
{
	vec3_t   fwd, right, up;
	vec3_t   start, end;
	vec3_t   muzzle;
	qboolean used_attach = false;
	int      muzzle_idx;

	if( slayer_tracer.value == 0.0f )
		return;

	// Register the shot for barrel-heat colouring (colour computed per frame).
	s_last_shot = host.realtime;

	// THE ECHO OF A SHOT WE ALREADY DREW.
	//
	// With slayer_tracer_instant on, a local shot is drawn from the predicted
	// weapon event (see Slayer_Tracer_NoteImpact), and this muzzleflash is the
	// server echoing that same shot back a round trip later. Drawing again would
	// double every one of the player's own tracers.
	//
	// The barrel sample still has to be drained, or the queue would fill with
	// entries no detector consumes and later shots would read a stale barrel.
	if( is_local && slayer_tracer_instant.value != 0.0f
	 && Slayer_Tracer_TakeInstantCredit( ))
	{
		Slayer_Tracer_PopMuzzle();
		return;
	}

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
			//
			// And its SEQUENCE is the right one to ask about the barrel: it comes
			// from the same snapshot as the muzzleflash echo, so the two describe
			// the same shot. The queue is not used here for that reason -- but it
			// still has to be drained, or first-person shots taken before the
			// camera flipped would pile up in it.
			{
				cl_entity_t *lp = CL_GetLocalPlayer();

				Slayer_Tracer_PopMuzzle();
				muzzle_idx = Slayer_Tracer_MuzzleIndexFromModel( lp );

				if( Slayer_Tracer_MuzzleFromAttachment( lp, muzzle_idx, muzzle ))
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
		}
		else
		{
			// First person: aim is exactly the view. Start at the eye so the
			// streak originates at screen centre and flies to the crosshair's
			// target. The local viewmodel dispatches studio events (see
			// R_DrawViewModel), so its attachments hold the real gun tips; prefer
			// them for the START while keeping the view direction for aim.
			AngleVectors( refState.viewangles, fwd, right, up );

			// WHICH gun tip comes from the QUEUE, not from the viewmodel's current
			// sequence. By the time the server's muzzleflash echo reaches us the
			// predicted animation has already advanced to the next shot, so reading
			// it live would swap the Berettas' barrels rather than separate them.
			muzzle_idx = Slayer_Tracer_PopMuzzle();
			if( muzzle_idx < 0 )
				muzzle_idx = Slayer_Tracer_MuzzleIndexFromModel( &clgame.viewent );

			if( Slayer_Tracer_MuzzleFromAttachment( &clgame.viewent, muzzle_idx, muzzle ))
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

		// Variant D: use the real studio muzzle when the renderer has filled it;
		// otherwise approximate from the origin. The barrel comes from the player
		// model's own sequence, which arrives in the same snapshot as the
		// muzzleflash that brought us here.
		muzzle_idx = Slayer_Tracer_MuzzleIndexFromModel( ent );

		if( Slayer_Tracer_MuzzleFromAttachment( ent, muzzle_idx, muzzle ))
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

	// Remote players do not expose local weapon-event impacts, so their entity
	// trace remains immediate. Local shots wait briefly for the exact impact
	// produced by the spread-aware weapon event.
	if( is_local )
		Slayer_Tracer_QueueLocal( start, end );
	else
		Slayer_Tracer_SpawnVisual( start, end, true );

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

int Slayer_Tracer_BeginEvent( int entindex )
{
	int prev = s_event_owner;

	s_event_owner = entindex;
	return prev;
}

void Slayer_Tracer_EndEvent( int prev_owner )
{
	// SAMPLE THE BARREL HERE, and this is the only moment in the frame when it is
	// unambiguous.
	//
	// The client library's weapon event has just run. For the Berettas that means
	// it called pfnWeaponAnim with shoot_left* or shoot_right* for THIS shot, so
	// clgame.viewent.curstate.sequence now names the barrel that fired. A few
	// frames later, when the server's EF_MUZZLEFLASH echo reaches us and the
	// tracer is actually placed, the sequence has moved on to the next shot -- and
	// reading it then would swap the two guns rather than separate them.
	//
	// Only the LOCAL player's own event is sampled: a remote player's barrel comes
	// from his player entity's sequence, which is snapshot data and needs no queue.
	if( s_event_owner == cl.playernum + 1
	 && slayer_tracer.value != 0.0f
	 && slayer_tracer_muzzle_seq.value != 0.0f )
	{
		int idx = Slayer_Tracer_MuzzleIndexFromModel( &clgame.viewent );
		int slot = -1;
		int i;

		// A free slot, else the OLDEST entry: the newest samples are the ones the
		// pending shots will ask for, and eight unmatched shots means the echo path
		// has stalled, not that the eighth shot matters less than the first.
		for( i = 0; i < SLAYER_MUZZLE_QUEUE; i++ )
		{
			if( !s_muzzleq[i].used )
			{
				slot = i;
				break;
			}
			if( slot < 0 || s_muzzleq[i].order < s_muzzleq[slot].order )
				slot = i;
		}

		s_muzzleq[slot].used   = true;
		s_muzzleq[slot].time   = host.realtime;
		s_muzzleq[slot].order  = ++s_muzzleq_order;
		s_muzzleq[slot].attach = idx;
	}

	// Restore rather than zero: an event that triggers another event must not
	// leave the outer one un-attributed for its remaining bullets.
	s_event_owner = prev_owner;
}

void Slayer_Tracer_NoteImpact( const vec3_t pos )
{
	int    i, best = -1;
	double best_age = 999.0;
	float  best_dot = 0.94f; // ~20-degree spread cone; rejects unrelated impacts
	double window = slayer_tracer_impact_window.value;

	if( !pos || slayer_tracer.value == 0.0f )
		return;

	// OWNERSHIP GATE, checked first and cheapest.
	// This is the answer to "can another player's shot steal my tracer while we
	// stand point-blank?". At contact range the geometric tests are useless: his
	// bullet hole is within a metre of mine and lands in the same frame, so both
	// the time window and the direction cone accept it. But R_BulletImpactParticles
	// is always reached from INSIDE one weapon event, and CL_FireEvent tells us
	// whose event that is. An impact from anyone but the local player can never
	// close a local pending shot.
	//
	// s_event_owner == 0 means the impact did not come from a weapon event at
	// all (a server temp-entity, a mod calling the effect directly). That is not
	// attributable to anyone, so it is refused too: the timeout fallback will
	// finish the shot with the view endpoint, which is a small error, whereas
	// accepting a stranger's hole is a visibly wrong tracer.
	if( s_event_owner != cl.playernum + 1 )
	{
		s_impact_foreign++;
		return;
	}

	// INSTANT PATH -- the fix for "трассеры будто зависят от пинга".
	//
	// Everything needed is already known HERE, in the same frame as the shot: this
	// callback runs inside the client's own PREDICTED weapon event, `pos` is the
	// exact impact that event computed (spread included), and the muzzle is on the
	// viewmodel whose animation the event just started. Nothing about that came
	// from the server.
	//
	// The old code instead recorded this impact and waited for the server's
	// EF_MUZZLEFLASH echo to place the tracer, which is one full round trip later
	// -- 60 ms on a good server, 130 on the one in the report. That IS the reported
	// dependence on ping, and no tuning of the windows could have removed it.
	//
	// So draw now, and leave a credit: the echo for this shot will arrive later and
	// must not draw a second streak.
	//
	// A muzzle we cannot trust means falling through to the old path rather than
	// drawing from the eye -- a streak starting in the middle of the screen is
	// worse than a slightly late one.
	if( slayer_tracer_instant.value != 0.0f )
	{
		vec3_t muzzle;

		if( Slayer_Tracer_LocalMuzzleNow( muzzle ))
		{
			vec3_t delta;

			VectorSubtract( pos, muzzle, delta );

			// A degenerate pair (impact essentially at the muzzle: a wall pressed
			// against the barrel) has no direction to draw along; let the echo path
			// deal with it.
			if( VectorLength( delta ) >= 1.0f )
			{
				Slayer_Tracer_SpawnVisual( muzzle, pos, false );
				Slayer_Tracer_InstantCredit();
				s_instant_drawn++;
				return;
			}
		}
	}

	if( window < 0.0 ) window = 0.0;
	// Upper bound 0.5 s, not 0.25: the pairing window must exceed the ping
	// (the muzzleflash we see is the server's echo), and 250 ms rules out
	// perfectly ordinary high-latency servers.
	if( window > 0.5 ) window = 0.5;

	// Match the newest pending local shot inside the window. Multiple impacts
	// from shotgun pellets deliberately consume only one shot; the nearest-in-
	// time impact wins and later pellets find no matching shot.
	for( i = 0; i < SLAYER_PENDING_SHOTS; i++ )
	{
		double age;
		vec3_t expected, actual;
		float  expected_len, actual_len, dot;

		if( !s_pending[i].active )
			continue;
		age = host.realtime - s_pending[i].time;
		if( age < 0.0 || age > window )
			continue;

		VectorSubtract( s_pending[i].fallback, s_pending[i].start, expected );
		VectorSubtract( pos, s_pending[i].start, actual );
		expected_len = VectorLength( expected );
		actual_len = VectorLength( actual );
		if( expected_len < 1.0f || actual_len < 1.0f )
			continue;
		dot = DotProduct( expected, actual ) / ( expected_len * actual_len );

		// Direction dominates time: it prevents another player's nearby impact
		// from consuming our pending shot during a firefight. Time breaks ties.
		if( dot > best_dot || ( fabs( dot - best_dot ) < 0.0001f && age < best_age ))
		{
			best = i;
			best_dot = dot;
			best_age = age;
		}
	}

	if( best >= 0 )
	{
		Slayer_Tracer_SpawnVisual( s_pending[best].start, pos, false );
		s_pending[best].active = false;
		s_impact_paired++;
		return;
	}

	// No shot detected yet. Record it: the shot almost always arrives AFTER the
	// impact (the muzzleflash we can see is the server's echo), and
	// Slayer_Tracer_QueueLocal looks back through this ring.
	s_impacts[s_impacts_next].used = false;
	s_impacts[s_impacts_next].time = host.realtime;
	VectorCopy( pos, s_impacts[s_impacts_next].pos );
	s_impacts_next = ( s_impacts_next + 1 ) % SLAYER_IMPACT_RING;
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
	int           i;

	if( last_time == 0.0 )
		last_time = now;
	dt = now - last_time;
	last_time = now;
	if( dt < 0.0 ) dt = 0.0;      // clock reset guard
	if( dt > 0.25 ) dt = 0.25;    // don't lurch after a hitch

	// Expire unmatched local shots after the GRACE window (not the full impact
	// window): a shot we already detected without a recorded impact almost
	// certainly has none, so holding the tracer back longer only adds latency.
	for( i = 0; i < SLAYER_PENDING_SHOTS; i++ )
	{
		double grace = slayer_tracer_impact_grace.value;

		if( grace < 0.0 ) grace = 0.0;
		if( grace > 0.25 ) grace = 0.25;
		if( s_pending[i].active && now - s_pending[i].time > grace )
		{
			Slayer_Tracer_SpawnVisual( s_pending[i].start, s_pending[i].fallback, false );
			s_pending[i].active = false;
			s_impact_fallback++;
		}
	}

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
		slayer_tracer_debug_t dbg;

		Slayer_TracerRender_DebugSnapshot( &dbg );

		Slayer_Log_Printf(
			"tracer: 1s summary fired[L=%d R=%d] rawMF[L=%d R=%d] beams[ok=%d noModel=%d null=%d] "
			"attach[used=%d approx=%d alt=%d fb=%d] muzzleq[hit=%d miss=%d] impact[paired=%d back=%d fallback=%d foreign=%d] pierced=%d TE_TRACER=%d heat=%.2f model=%d "
			"own[render=%d peak=%d/%d tp=%d]",
			s_fired_local, s_fired_remote, s_mf_raw_local, s_mf_raw_remote,
			s_beam_ok, s_beam_fail_model, s_beam_fail_null,
			s_attach_used, s_attach_reject, s_muzzle_alt, s_muzzle_fallback,
			s_muzzleq_hit, s_muzzleq_miss,
			s_impact_paired, s_impact_back,
			s_impact_fallback, s_impact_foreign, s_pierced, s_te_tracer,
			s_heat, s_beam_model,
			(int)slayer_tracer_render.value, s_live_peak, SLAYER_TRACER_POOL,
			( V_IsSlayerThirdPerson() || CL_IsThirdPerson()) ? 1 : 0 );

		// SECOND line, from the drawing side. The first line can read
		// "peak=2/48" while nothing whatsoever reached the screen -- the report
		// "трассеры не отображаются на серверах" is consistent with every one of
		// these counters, and they disagree with each other:
		//   draw=0                  -> the draw hook is not being called at all
		//   draw>0 ribbons=0        -> every tracer returned early; early_* says which
		//   ribbons>0 tex=0         -> untextured, so the profile is the default checkerboard
		//   ribbons>0 px<1          -> drawn thinner than a pixel
		//   ribbons>0 gain*dim ~ 0  -> drawn black on black (additive)
		Slayer_Log_Printf(
			"tracer: 1s draw pool[calls=%d gated=%d] draw=%d early[life=%d len=%d gain=%d] "
			"ribbons=%d verts=%d "
			"last[px=%.2f gain=%.2f dim=%.2f dist=%.0f tex=%d] tex[core=%d halo=%d] "
			"instant[on=%d drawn=%d echo=%d stale=%d]",
			s_pool_draw_calls, s_pool_draw_gated,
			dbg.draw_calls, dbg.early_life, dbg.early_len, dbg.early_gain,
			dbg.ribbons, dbg.verts,
			dbg.last_px, dbg.last_gain, dbg.last_dim, dbg.last_dist, dbg.last_tex,
			dbg.tex_core, dbg.tex_halo,
			(int)slayer_tracer_instant.value,
			s_instant_drawn, s_instant_echo_used, s_instant_expired );

		s_pool_draw_calls = s_pool_draw_gated = 0;
		s_instant_drawn = s_instant_echo_used = s_instant_expired = 0;
		Slayer_TracerRender_DebugReset();

		s_fired_local = s_fired_remote = s_beam_ok = 0;
		s_beam_fail_model = s_beam_fail_null = 0;
		s_mf_raw_local = s_mf_raw_remote = s_te_tracer = 0;
		s_attach_used = s_attach_reject = 0;
		s_muzzle_alt = s_muzzle_fallback = 0;
		s_muzzleq_hit = s_muzzleq_miss = 0;
		s_impact_paired = s_impact_fallback = s_impact_foreign = s_impact_back = 0;
		s_pierced = 0;
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
