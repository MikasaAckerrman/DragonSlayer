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

// Slayer3D tracers layer over the engine's existing additive, batched tracer
// renderer (ref/gl CL_DrawTracers) rather than replacing it, so the hot path
// stays the one draw call the engine already does. What we add on top:
//
//   * Barrel heat: sustained fire walks the tracer colour yellow -> orange ->
//     red. It builds only while shots keep coming and cools GRADUALLY, a touch
//     faster than it heats, so a pause bleeds heat off instead of snapping.
//   * Vanilla impact sparks are suppressed so ours (or none) can take over.
//
// Colour is driven through the engine's single custom tracer colour slot
// (gTracerColors[4], fed by the tracerred/green/blue cvars). Every Slayer
// tracer uses that slot, and we rewrite it each frame to the current heat
// colour. Tracers live ~0.18s, so the whole live set sharing one colour reads
// as a single coherent colour that drifts with heat -- exactly the intent.

#include "common.h"
#include "client.h"
#include "cl_tracer_slayer.h"

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

static CVAR_DEFINE_AUTO( slayer_tracer_sparks, "1", FCVAR_ARCHIVE,
	"Slayer3D: replace vanilla bullet-impact sparks (0 = keep vanilla)" );

// ===========================================================================
// State
// ===========================================================================

// Heat, 0..1. Rewritten every frame in Slayer_Tracer_Frame.
static float  s_heat = 0.0f;
static double s_last_shot = 0.0;

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

	Slayer_Tracer_Reset();
}

void Slayer_Tracer_Reset( void )
{
	s_heat = 0.0f;
	s_last_shot = 0.0;
}

qboolean Slayer_Tracer_OnFire( const vec3_t start, const vec3_t end )
{
	(void)start;
	(void)end;

	if( slayer_tracer.value == 0.0f )
		return false;   // let the engine draw its vanilla tracer

	// Register the shot for heat buildup; the actual colour is pushed to the
	// engine's custom tracer slot in Slayer_Tracer_Frame so all live tracers
	// share one coherent, heat-driven colour.
	s_last_shot = host.realtime;

	return true;   // Slayer owns this tracer (caller assigns the custom colour)
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

	// Push the current colour into the engine's custom tracer slot via the
	// tracer colour cvars (0..1 range). Setting them flags FCVAR_CHANGED, which
	// CL_DrawTracers picks up to refill gTracerColors[4].
	Slayer_Tracer_HeatColor( s_heat, col );
	Cvar_SetValue( "tracerred",   col[0] / 255.0f );
	Cvar_SetValue( "tracergreen", col[1] / 255.0f );
	Cvar_SetValue( "tracerblue",  col[2] / 255.0f );
}

qboolean Slayer_Tracer_SuppressVanillaSparks( void )
{
	return ( slayer_tracer.value != 0.0f && slayer_tracer_sparks.value != 0.0f );
}
