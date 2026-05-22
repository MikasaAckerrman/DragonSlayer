/*
cl_hud_slayer.c - Slayer3D crosshair HUD: damage indicator
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

#include "common.h"
#include "client.h"
#include "cl_hud_slayer.h"

// ===========================================================================
// Cvars
// ===========================================================================
//
// slayer_damage_indicator values:
//   0 = off
//   1 = show damage TAKEN (vanilla "Damage" usermsg, works on every server)
//   2 = show damage DEALT (parsed from common server-mod usermsgs; falls
//       silent on stock servers that do not broadcast such messages)
//   3 = both (taken in red, dealt in green)
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_damage_indicator,            "0",
	FCVAR_ARCHIVE,
	"Slayer3D: damage indicator under crosshair. 0=off 1=taken 2=dealt 3=both" );

static CVAR_DEFINE_AUTO( slayer_damage_indicator_color,      "255 80 80 255",
	FCVAR_ARCHIVE,
	"Slayer3D: damage-taken indicator color 'R G B A' 0..255" );

static CVAR_DEFINE_AUTO( slayer_damage_indicator_color_dealt, "180 255 180 255",
	FCVAR_ARCHIVE,
	"Slayer3D: damage-dealt indicator color 'R G B A' 0..255" );

static CVAR_DEFINE_AUTO( slayer_damage_indicator_time,       "2.0",
	FCVAR_ARCHIVE,
	"Slayer3D: damage indicator display time in seconds (fade included)" );

static CVAR_DEFINE_AUTO( slayer_damage_indicator_offset,     "40",
	FCVAR_ARCHIVE,
	"Slayer3D: vertical offset below screen center, in pixels" );

static CVAR_DEFINE_AUTO( slayer_damage_indicator_scale,      "1.0",
	FCVAR_ARCHIVE,
	"Slayer3D: damage indicator text scale (1.0 = console font size)" );

// Damage events arriving within this window are merged into one number so
// a multi-pellet shotgun hit reads as the total instead of flickering.
#define SLAYER_DMG_GROUP_WINDOW 0.25

typedef struct
{
	int     amount;     // accumulated damage in current event
	double  t_start;    // host.realtime when event began (for fade)
	double  t_last;     // host.realtime of most recent contribution
	int     active;     // non-zero while an event is being displayed
} slayer_dmg_event_t;

static slayer_dmg_event_t s_dmg_taken;
static slayer_dmg_event_t s_dmg_dealt;

// ===========================================================================
// Internal helpers
// ===========================================================================

static void Slayer_HUD_AccumulateEvent( slayer_dmg_event_t *ev, int amount )
{
	double now;

	if( amount <= 0 )
		return;

	now = host.realtime;

	// Group: if a recent event is still within the merge window, add.
	// Otherwise start a new event so the timer resets.
	if( ev->active && ( now - ev->t_last ) <= SLAYER_DMG_GROUP_WINDOW )
	{
		ev->amount  += amount;
		ev->t_last   = now;
	}
	else
	{
		ev->amount   = amount;
		ev->t_start  = now;
		ev->t_last   = now;
		ev->active   = 1;
	}
}

// Parse "R G B A" cvar string into byte components. Missing components
// fall back to (255,255,255,255). Tolerant of whitespace and extra tokens.
static void Slayer_HUD_ParseColorCvar( const char *str, byte out[4] )
{
	int  r = 255, g = 255, b = 255, a = 255;
	int  parsed;

	out[0] = 255; out[1] = 255; out[2] = 255; out[3] = 255;

	if( !str || !*str )
		return;

	parsed = sscanf( str, "%d %d %d %d", &r, &g, &b, &a );
	if( parsed < 1 )
		return;
	if( parsed < 4 ) a = 255;

	out[0] = (byte)bound( 0, r, 255 );
	out[1] = (byte)bound( 0, g, 255 );
	out[2] = (byte)bound( 0, b, 255 );
	out[3] = (byte)bound( 0, a, 255 );
}

// Returns alpha multiplier in [0..1] for the given event based on its age
// against slayer_damage_indicator_time. A short hold then linear fade.
static float Slayer_HUD_EventAlpha( const slayer_dmg_event_t *ev )
{
	double total, age, hold, fade;

	total = slayer_damage_indicator_time.value;
	if( total <= 0.0 ) return 0.0f;

	age = host.realtime - ev->t_start;
	if( age < 0.0 ) age = 0.0;
	if( age >= total ) return 0.0f;

	// First half: full opacity. Second half: linear fade-out.
	hold = total * 0.5;
	if( age <= hold )
		return 1.0f;

	fade = total - hold;
	if( fade <= 0.0 ) return 0.0f;
	return (float)( 1.0 - ( age - hold ) / fade );
}

// Draw a single event line offset vertically by line_offset_y pixels.
// color_str is the RGBA cvar string. Format: "-N" for taken, "+N" for dealt.
static void Slayer_HUD_DrawEvent( slayer_dmg_event_t *ev, const char *color_str,
	const char *prefix, int line_offset_y )
{
	char    buf[32];
	int     screen_w, screen_h;
	int     text_w = 0, text_h = 0;
	int     x, y;
	float   alpha_mul;
	byte    color[4];
	rgba_t  rgba;

	if( !ev->active )
		return;

	alpha_mul = Slayer_HUD_EventAlpha( ev );
	if( alpha_mul <= 0.0f )
	{
		ev->active = 0;
		return;
	}

	screen_w = refState.width;
	screen_h = refState.height;
	if( screen_w <= 0 || screen_h <= 0 )
		return;

	Q_snprintf( buf, sizeof( buf ), "%s%d", prefix, ev->amount );

	Slayer_HUD_ParseColorCvar( color_str, color );

	rgba[0] = color[0];
	rgba[1] = color[1];
	rgba[2] = color[2];
	rgba[3] = (byte)( color[3] * alpha_mul );

	Con_DrawStringLen( buf, &text_w, &text_h );
	if( text_w <= 0 ) text_w = (int)Q_strlen( buf ) * 8;

	x = ( screen_w / 2 ) - ( text_w / 2 );
	y = ( screen_h / 2 ) + (int)slayer_damage_indicator_offset.value + line_offset_y;

	Con_DrawString( x, y, buf, rgba );
}

// ===========================================================================
// Public API
// ===========================================================================

void Slayer_HUD_Init( void )
{
	Cvar_RegisterVariable( &slayer_damage_indicator );
	Cvar_RegisterVariable( &slayer_damage_indicator_color );
	Cvar_RegisterVariable( &slayer_damage_indicator_color_dealt );
	Cvar_RegisterVariable( &slayer_damage_indicator_time );
	Cvar_RegisterVariable( &slayer_damage_indicator_offset );
	Cvar_RegisterVariable( &slayer_damage_indicator_scale );

	Slayer_HUD_Reset();
}

void Slayer_HUD_Reset( void )
{
	memset( &s_dmg_taken, 0, sizeof( s_dmg_taken ));
	memset( &s_dmg_dealt, 0, sizeof( s_dmg_dealt ));
}

void Slayer_HUD_OnDamageMsg( const byte *pbuf, int iSize )
{
	int armor_dmg, hp_dmg, total;
	int mode;

	if( pbuf == NULL || iSize < 2 )
		return;

	mode = (int)slayer_damage_indicator.value;
	if( mode != 1 && mode != 3 )
		return;

	// HL/CS Damage usermsg layout: byte armor, byte damage, long bits, coord*3
	armor_dmg = pbuf[0];
	hp_dmg    = pbuf[1];
	total     = armor_dmg + hp_dmg;

	Slayer_HUD_AccumulateEvent( &s_dmg_taken, total );
}

void Slayer_HUD_OnDamageDealtMsg( const byte *pbuf, int iSize )
{
	int amount;
	int mode;

	if( pbuf == NULL || iSize < 1 )
		return;

	mode = (int)slayer_damage_indicator.value;
	if( mode != 2 && mode != 3 )
		return;

	// Heuristic decode: most server-mod plugins broadcast either a single
	// byte ("DmgInd"-style) or a small fixed structure starting with the
	// damage value. Sanity-bound to avoid showing garbage when the message
	// shape is unknown.
	amount = pbuf[0];
	if( amount == 0 && iSize >= 2 )
		amount = pbuf[1];

	if( amount <= 0 || amount > 999 )
		return;

	Slayer_HUD_AccumulateEvent( &s_dmg_dealt, amount );
}

void Slayer_HUD_Draw( void )
{
	int mode;

	mode = (int)slayer_damage_indicator.value;
	if( mode <= 0 )
		return;

	// Hide while the scoreboard panel covers the screen. The scoreboard
	// itself has no exported "is visible" probe, so we approximate by
	// only suppressing when intermission / loading screens are up.
	if( cl.intermission || cls.state != ca_active )
		return;

	if( mode == 1 || mode == 3 )
	{
		Slayer_HUD_DrawEvent( &s_dmg_taken,
			slayer_damage_indicator_color.string, "-", 0 );
	}

	if( mode == 2 || mode == 3 )
	{
		Slayer_HUD_DrawEvent( &s_dmg_dealt,
			slayer_damage_indicator_color_dealt.string, "+",
			( mode == 3 ) ? 18 : 0 );
	}
}
