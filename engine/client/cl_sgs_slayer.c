/*
cl_sgs_slayer.c - Slayer3D Side-Game Strafe (auto-strafe for mobile)
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

// ---------------------------------------------------------------------------
// Side-Game Strafe (SGS): automated air-strafe for touch screens.
//
// Air-strafe physics in GoldSrc/Quake-style movement: while airborne,
// turning the view rotates the player's velocity vector slightly, and
// holding strafe-key in the SAME direction as the turn produces a velocity
// gain (the classic "bunnyhop" technique).
//
// Manual SGS requires the player to oscillate the yaw at ~5-15 Hz with
// perfectly synchronized strafe keys. This is impractical on a phone where
// the touch screen is small and a thumb cannot whip the view fast enough.
//
// This module replaces the manual oscillation with a synthetic sin-wave
// yaw offset injected into the outgoing usercmd, plus a synchronized
// sidemove that always points in the same direction as the current yaw
// derivative. The server sees a player whose view oscillates at the
// optimal frequency, so air-strafe gain accumulates as if a top-tier
// strafer were at the keyboard.
//
// Engagement gate (any of the following):
//   1. The "+slayer_sgs" command is currently held.
//   2. The user produced non-trivial yaw input within the last
//      slayer_sgs_swipe_window seconds (i.e. they are actively swiping).
//
// Air-only mode (slayer_sgs_air_only=1, default): only inject oscillation
// while the player is airborne. Keeps ground aim stable.
// ---------------------------------------------------------------------------

#include <math.h>
#include "common.h"
#include "client.h"
#include "cl_sgs_slayer.h"

// ===========================================================================
// Cvars
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_sgs,             "0", FCVAR_ARCHIVE,
	"Slayer3D: enable Side-Game Strafe auto-strafer (0=off, 1=on)" );

static CVAR_DEFINE_AUTO( slayer_sgs_strength,    "8.0", FCVAR_ARCHIVE,
	"Slayer3D: SGS yaw oscillation amplitude in degrees" );

static CVAR_DEFINE_AUTO( slayer_sgs_freq,        "8.0", FCVAR_ARCHIVE,
	"Slayer3D: SGS oscillation frequency in Hz" );

static CVAR_DEFINE_AUTO( slayer_sgs_swipe_window, "0.20", FCVAR_ARCHIVE,
	"Slayer3D: seconds after last user yaw input that SGS stays engaged" );

static CVAR_DEFINE_AUTO( slayer_sgs_swipe_min,    "0.20", FCVAR_ARCHIVE,
	"Slayer3D: minimum per-frame user yaw delta (deg) counted as a swipe" );

static CVAR_DEFINE_AUTO( slayer_sgs_air_only,    "1", FCVAR_ARCHIVE,
	"Slayer3D: only apply SGS when airborne (1=safe, 0=always)" );

static CVAR_DEFINE_AUTO( slayer_sgs_sidemove,    "400.0", FCVAR_ARCHIVE,
	"Slayer3D: sidemove magnitude injected by SGS" );

// ===========================================================================
// State
// ===========================================================================

static int     s_sgs_held         = 0;     // +slayer_sgs key currently held
static double  s_sgs_phase        = 0.0;   // sin-wave accumulator (radians)
static double  s_sgs_last_swipe   = 0.0;   // host.realtime of last swipe
static float   s_sgs_prev_user_yaw = 0.0f; // captured user yaw last frame
static int     s_sgs_have_prev_yaw = 0;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ===========================================================================
// +slayer_sgs / -slayer_sgs command pair
// ===========================================================================

static void Cmd_SGSDown_f( void )
{
	s_sgs_held = 1;
	s_sgs_last_swipe = host.realtime; // treat hold as an immediate swipe
}

static void Cmd_SGSUp_f( void )
{
	s_sgs_held = 0;
}

// ===========================================================================
// Public API
// ===========================================================================

void Slayer_SGS_Init( void )
{
	Cvar_RegisterVariable( &slayer_sgs );
	Cvar_RegisterVariable( &slayer_sgs_strength );
	Cvar_RegisterVariable( &slayer_sgs_freq );
	Cvar_RegisterVariable( &slayer_sgs_swipe_window );
	Cvar_RegisterVariable( &slayer_sgs_swipe_min );
	Cvar_RegisterVariable( &slayer_sgs_air_only );
	Cvar_RegisterVariable( &slayer_sgs_sidemove );

	Cmd_AddCommand( "+slayer_sgs", Cmd_SGSDown_f,
		"engage Side-Game Strafe while held (mobile auto-strafer)" );
	Cmd_AddCommand( "-slayer_sgs", Cmd_SGSUp_f,
		"release Side-Game Strafe key" );

	Slayer_SGS_Reset();
}

void Slayer_SGS_Reset( void )
{
	s_sgs_held          = 0;
	s_sgs_phase         = 0.0;
	s_sgs_last_swipe    = -1000.0;
	s_sgs_prev_user_yaw = 0.0f;
	s_sgs_have_prev_yaw = 0;
}

void Slayer_SGS_Capture( const struct usercmd_s *cmd )
{
	float current, delta, threshold;

	if( cmd == NULL )
		return;

	current = cmd->viewangles[YAW];

	if( !s_sgs_have_prev_yaw )
	{
		s_sgs_prev_user_yaw = current;
		s_sgs_have_prev_yaw = 1;
		return;
	}

	delta = current - s_sgs_prev_user_yaw;
	s_sgs_prev_user_yaw = current;

	// Normalize to (-180, 180] so wrap-around does not register as a huge swipe.
	while( delta >  180.0f ) delta -= 360.0f;
	while( delta < -180.0f ) delta += 360.0f;

	threshold = slayer_sgs_swipe_min.value;
	if( threshold < 0.0f ) threshold = 0.0f;

	if( fabsf( delta ) >= threshold )
		s_sgs_last_swipe = host.realtime;
}

void Slayer_SGS_Apply( struct usercmd_s *cmd, double frametime )
{
	double  omega, phase_now, sin_offset, cos_deriv;
	double  swipe_age, window;
	float   strength, sidemove;
	int     engaged;

	if( cmd == NULL )
		return;
	if( slayer_sgs.value == 0.0f )
		return;

	// Air-only gate.
	if( slayer_sgs_air_only.value != 0.0f && cl.local.onground != -1 )
	{
		// Reset phase so we always start from zero on next take-off,
		// avoiding a sudden jolt at the moment of leaving the ground.
		s_sgs_phase = 0.0;
		return;
	}

	// Engagement gate: hold OR recent swipe.
	engaged = 0;
	if( s_sgs_held )
	{
		engaged = 1;
	}
	else
	{
		window = slayer_sgs_swipe_window.value;
		if( window < 0.0 ) window = 0.0;
		swipe_age = host.realtime - s_sgs_last_swipe;
		if( swipe_age >= 0.0 && swipe_age <= window )
			engaged = 1;
	}

	if( !engaged )
	{
		s_sgs_phase = 0.0;
		return;
	}

	// Advance the oscillation phase by 2 * pi * freq * dt.
	omega       = 2.0 * M_PI * (double)slayer_sgs_freq.value;
	s_sgs_phase += omega * frametime;
	while( s_sgs_phase >  2.0 * M_PI ) s_sgs_phase -= 2.0 * M_PI;
	while( s_sgs_phase < -2.0 * M_PI ) s_sgs_phase += 2.0 * M_PI;

	phase_now  = s_sgs_phase;
	sin_offset = sin( phase_now );
	cos_deriv  = cos( phase_now );

	strength = slayer_sgs_strength.value;
	if( strength < 0.0f )  strength = 0.0f;
	if( strength > 45.0f ) strength = 45.0f;

	// Inject yaw offset INTO THE COMMAND ONLY. cl.viewangles is left
	// untouched so the rendered view does not shake; only the angle
	// the server sees oscillates.
	cmd->viewangles[YAW] += (float)( sin_offset * strength );

	// Sync sidemove with the yaw derivative so the player always strafes
	// in the same direction the view is currently turning -- the canonical
	// air-strafe gain pattern. Existing IN_MOVELEFT/IN_MOVERIGHT bits are
	// overridden so user-held strafe keys do not fight the oscillator.
	sidemove = slayer_sgs_sidemove.value;
	if( sidemove < 0.0f ) sidemove = 0.0f;

	if( cos_deriv >= 0.0 )
	{
		cmd->sidemove = sidemove;
		cmd->buttons |= IN_MOVERIGHT;
		cmd->buttons &= ~IN_MOVELEFT;
	}
	else
	{
		cmd->sidemove = -sidemove;
		cmd->buttons |= IN_MOVELEFT;
		cmd->buttons &= ~IN_MOVERIGHT;
	}

	// Forward/back is poison to air-strafe gain. Strip it while engaged.
	cmd->forwardmove = 0.0f;
	cmd->buttons &= ~( IN_FORWARD | IN_BACK );
}
