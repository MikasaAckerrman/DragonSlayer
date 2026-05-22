/*
cl_hud_slayer.c - Slayer3D crosshair-area HUD overlay (damage indicator)
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
// Damage indicator
// =============================================================================
//
// Server plugins (AmxModX hit-feedback, ReGameDLL custom messages, ...)
// commonly broadcast a per-attacker user-message whenever a player lands a
// damaging shot. The payload is normally:
//
//   byte  damage  (0..255)
//   byte? hitgroup_or_victim  (optional, ignored here)
//
// We hook several known message names so the indicator works across server
// configurations without per-server tuning. Unknown servers simply produce
// no events and the feature is invisible.
//
// Each registered hit picks the next slot in a 16-entry ring with
// pre-computed halo offsets around the screen center. New hits never
// overwrite a still-fading older hit at the same pixel — the ring always
// rotates to the next halo position. Entries fade out linearly in their
// final 0.5s of life.

#include "common.h"
#include "client.h"
#include "cl_hud_slayer.h"

#if XASH_ANDROID
#include <android/log.h>
#endif

// =============================================================================
// Cvars
// =============================================================================

static CVAR_DEFINE_AUTO( slayer_damage_indicator,
	"1", FCVAR_ARCHIVE,
	"Slayer3D: show fading damage number near crosshair when you hit a player (0 = off)" );

static CVAR_DEFINE_AUTO( slayer_damage_indicator_color,
	"255 80 80 255", FCVAR_ARCHIVE,
	"Slayer3D: damage indicator RGBA color" );

static CVAR_DEFINE_AUTO( slayer_damage_indicator_duration,
	"1.5", FCVAR_ARCHIVE,
	"Slayer3D: damage indicator total visible time (seconds, fades over the last 0.5s)" );

// =============================================================================
// Halo offsets
// =============================================================================
//
// 16 deterministic positions around the crosshair. Consecutive hits
// step through these in order so the second, third, ... hits can never
// stomp on top of the first one before it has faded.
//
// Layout (rough): inner 4 corners -> cardinal axes -> outer 8 in a wider ring.

static const struct
{
	int x, y;
} slayer_dmg_halo[16] =
{
	{ -50, -25 }, {  50, -25 }, { -50,  25 }, {  50,  25 },
	{ -78,   0 }, {  78,   0 }, {   0, -45 }, {   0,  45 },
	{ -65, -55 }, {  65, -55 }, { -65,  55 }, {  65,  55 },
	{ -35, -68 }, {  35, -68 }, { -35,  68 }, {  35,  68 },
};
#define SLAYER_DMG_HALO_COUNT ( sizeof( slayer_dmg_halo ) / sizeof( slayer_dmg_halo[0] ) )

// =============================================================================
// Per-entry state
// =============================================================================

typedef struct
{
	int    damage;       // 0 = empty slot
	double expire_time;  // host.realtime when this entry must vanish
	int    halo_slot;    // index into slayer_dmg_halo
} slayer_dmg_entry_t;

// Ring of in-flight hits. SLAYER_DMG_HALO_COUNT also bounds the ring so
// the worst case is "one entry per halo position".
static slayer_dmg_entry_t slayer_dmg_entries[SLAYER_DMG_HALO_COUNT];
static int                slayer_dmg_next_slot;  // round-robin halo cursor

// Cached cvar parse state (rgba_t buffer + raw string).
static char   slayer_dmg_color_str[64] = "";
static rgba_t slayer_dmg_color_cached  = { 255, 80, 80, 255 };

// =============================================================================
// Helpers
// =============================================================================

// Parse "R G B" or "R G B A" cvar string. Tolerant of malformed input
// (falls back to 255,80,80,255). Used identically to the scoreboard's
// parser but kept local to avoid pulling in the whole scoreboard module.
static void Slayer_HUD_ParseColor( const char *str, rgba_t out )
{
	int r = 255, g = 80, b = 80, a = 255;
	int n;

	if( !str || str[0] == '\0' )
	{
		MakeRGBA( out, 255, 80, 80, 255 );
		return;
	}

	n = sscanf( str, "%d %d %d %d", &r, &g, &b, &a );
	if( n < 3 )
	{
		MakeRGBA( out, 255, 80, 80, 255 );
		return;
	}

	if( r < 0 ) r = 0; if( r > 255 ) r = 255;
	if( g < 0 ) g = 0; if( g > 255 ) g = 255;
	if( b < 0 ) b = 0; if( b > 255 ) b = 255;
	if( a < 0 ) a = 0; if( a > 255 ) a = 255;

	out[0] = (byte)r;
	out[1] = (byte)g;
	out[2] = (byte)b;
	out[3] = (byte)a;
}

// Returns true when the user-message name is one we recognize as a
// "you hit a player for N damage" feedback event. The list intentionally
// covers several mod-flavored conventions; servers that emit none of
// these names simply produce no indicators and the feature stays dormant.
static qboolean Slayer_HUD_IsDamageMessage( const char *name )
{
	static const char *const known[] =
	{
		"DmgIndicator",   // common AmxModX hit-feedback plugin
		"DmgInd",
		"DamageInd",
		"DmgFeedback",
		"HitFeedback",
		"AttackerDmg",
		"ReDmgInd",       // ReGameDLL custom variant
		"HitInd",
		NULL
	};
	int k;

	if( !name )
		return false;

	for( k = 0; known[k] != NULL; k++ )
	{
		if( !Q_strcmp( name, known[k] ))
			return true;
	}
	return false;
}

// Find a free or stalest slot to write a new damage entry into. Prefers
// truly empty slots; otherwise picks the entry whose expire_time is the
// closest (i.e. about to vanish anyway), which avoids overwriting a
// freshly-registered hit while the player is still reading it.
static int Slayer_HUD_PickWriteSlot( void )
{
	int    i;
	int    stalest = 0;
	double stalest_expire = (double)1e30;

	for( i = 0; i < (int)SLAYER_DMG_HALO_COUNT; i++ )
	{
		if( slayer_dmg_entries[i].damage == 0 )
			return i;
		if( slayer_dmg_entries[i].expire_time < stalest_expire )
		{
			stalest_expire = slayer_dmg_entries[i].expire_time;
			stalest = i;
		}
	}
	return stalest;
}

// =============================================================================
// Public API
// =============================================================================

void Slayer_HUD_Init( void )
{
	Cvar_RegisterVariable( &slayer_damage_indicator );
	Cvar_RegisterVariable( &slayer_damage_indicator_color );
	Cvar_RegisterVariable( &slayer_damage_indicator_duration );

	Slayer_HUD_Reset();
}

void Slayer_HUD_Reset( void )
{
	memset( slayer_dmg_entries, 0, sizeof( slayer_dmg_entries ));
	slayer_dmg_next_slot = 0;
}

void Slayer_HUD_OnDamageMessage( const char *msgname, const byte *pbuf, int iSize )
{
	int    damage;
	int    slot;
	double duration;

	if( slayer_damage_indicator.value == 0.0f )
		return;
	if( !pbuf || iSize <= 0 )
		return;
	if( !Slayer_HUD_IsDamageMessage( msgname ))
		return;

	// All known damage-feedback messages put the damage amount in the
	// first byte. Some variants follow it with a hit-group / victim slot
	// byte which we currently ignore — the on-screen number is enough.
	damage = pbuf[0];
	if( damage <= 0 || damage > 255 )
		return;

	duration = slayer_damage_indicator_duration.value;
	if( duration < 0.25 ) duration = 0.25;
	if( duration > 8.0  ) duration = 8.0;

	slot = Slayer_HUD_PickWriteSlot();
	slayer_dmg_entries[slot].damage      = damage;
	slayer_dmg_entries[slot].expire_time = host.realtime + duration;
	slayer_dmg_entries[slot].halo_slot   = slayer_dmg_next_slot;

	slayer_dmg_next_slot = ( slayer_dmg_next_slot + 1 ) % (int)SLAYER_DMG_HALO_COUNT;

	Con_DPrintf( "Slayer HUD: dmg %d via msg '%s', halo=%d slot=%d\n",
		damage, msgname, slayer_dmg_entries[slot].halo_slot, slot );
}

void Slayer_HUD_Draw( void )
{
	int          i;
	int          screen_w, screen_h;
	int          cx, cy;
	rgba_t       base_color;
	double       now;
	cl_font_t   *font;
	const double fade_secs = 0.5; // last 0.5s of each entry's life is the fade-out

	if( slayer_damage_indicator.value == 0.0f )
		return;
	if( cls.state != ca_active )
		return;

	screen_w = refState.width;
	screen_h = refState.height;
	if( screen_w <= 0 || screen_h <= 0 )
		return;

	// Refresh cached parsed color only when the cvar string actually changes.
	if( Q_strcmp( slayer_dmg_color_str, slayer_damage_indicator_color.string ))
	{
		Q_strncpy( slayer_dmg_color_str, slayer_damage_indicator_color.string,
			sizeof( slayer_dmg_color_str ));
		Slayer_HUD_ParseColor( slayer_dmg_color_str, slayer_dmg_color_cached );
	}
	memcpy( base_color, slayer_dmg_color_cached, sizeof( rgba_t ));

	font = Con_GetCurFont();
	if( !font )
		return;

	now = host.realtime;
	cx  = screen_w / 2;
	cy  = screen_h / 2;

	for( i = 0; i < (int)SLAYER_DMG_HALO_COUNT; i++ )
	{
		slayer_dmg_entry_t *e = &slayer_dmg_entries[i];
		double remaining;
		float  alpha_scale;
		rgba_t color;
		char   buf[16];
		int    halo_x, halo_y;
		int    text_w = 0, text_h_unused = 0;

		if( e->damage == 0 )
			continue;

		remaining = e->expire_time - now;
		if( remaining <= 0.0 )
		{
			e->damage = 0; // expired — release slot
			continue;
		}

		// Fade alpha over the last fade_secs only. Earlier in the entry's
		// life it's drawn at full configured alpha so the player has time
		// to actually read the number before it starts dimming.
		if( remaining < fade_secs )
			alpha_scale = (float)( remaining / fade_secs );
		else
			alpha_scale = 1.0f;

		color[0] = base_color[0];
		color[1] = base_color[1];
		color[2] = base_color[2];
		color[3] = (byte)( base_color[3] * alpha_scale );

		if( e->halo_slot < 0 || e->halo_slot >= (int)SLAYER_DMG_HALO_COUNT )
			e->halo_slot = 0;
		halo_x = slayer_dmg_halo[e->halo_slot].x;
		halo_y = slayer_dmg_halo[e->halo_slot].y;

		Q_snprintf( buf, sizeof( buf ), "-%d", e->damage );

		// Center the number on the halo position so longer numbers
		// (e.g. -100) don't push past their slot's right edge.
		Con_DrawStringLen( buf, &text_w, &text_h_unused );
		Con_DrawString( cx + halo_x - ( text_w / 2 ),
			cy + halo_y - ( font->charHeight / 2 ),
			buf, color );
	}
}
