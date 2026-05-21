/*
cl_killfeed_slayer.c - Slayer3D killfeed overlay
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
#include "cl_killfeed_slayer.h"

// ===========================================================================
// Cvars
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_killfeed, "1", FCVAR_ARCHIVE, "Slayer3D: enable killfeed overlay" );
static CVAR_DEFINE_AUTO( slayer_killfeed_duration, "5.0", FCVAR_ARCHIVE, "Slayer3D: killfeed entry lifetime in seconds" );
static CVAR_DEFINE_AUTO( slayer_killfeed_x, "0.98", FCVAR_ARCHIVE, "Slayer3D: killfeed right-edge X position (0..1)" );
static CVAR_DEFINE_AUTO( slayer_killfeed_y, "0.10", FCVAR_ARCHIVE, "Slayer3D: killfeed top Y position (0..1)" );
static CVAR_DEFINE_AUTO( slayer_killfeed_max, "5", FCVAR_ARCHIVE, "Slayer3D: max visible killfeed entries" );
static CVAR_DEFINE_AUTO( slayer_killfeed_bg_color, "0 0 0 140", FCVAR_ARCHIVE, "Slayer3D: killfeed background RGBA" );
static CVAR_DEFINE_AUTO( slayer_killfeed_border_color, "255 255 255 180", FCVAR_ARCHIVE, "Slayer3D: killfeed border RGBA" );
static CVAR_DEFINE_AUTO( slayer_killfeed_local_color, "255 200 0 220", FCVAR_ARCHIVE, "Slayer3D: killfeed local-kill highlight RGBA (gold)" );

// ===========================================================================
// Constants
// ===========================================================================

#define SLAYER_KILLFEED_MAX  8

// Animation timings (seconds)
#define KF_SLIDE_IN_TIME   0.25f
#define KF_FADE_OUT_TIME   0.5f

// Padding
#define KF_PAD_X           6
#define KF_PAD_Y           2
#define KF_ENTRY_GAP       3
#define KF_BORDER_W        1

// Team colors (same as scoreboard defaults)
#define KF_CT_R   153
#define KF_CT_G   204
#define KF_CT_B   255

#define KF_T_R    255
#define KF_T_G     63
#define KF_T_B     63

#define KF_SPEC_R  200
#define KF_SPEC_G  200
#define KF_SPEC_B  200

// ===========================================================================
// Data
// ===========================================================================

typedef struct
{
	int   killer_idx;   // 1-based entindex (0 = world kill)
	int   victim_idx;   // 1-based entindex
	qboolean headshot;
	qboolean is_local;  // killer == local player
	char  weapon[32];   // weapon short name
	int   weapon_tex;   // GL texture id (0 = not loaded / fallback)
	float spawn_time;   // host.realtime when entry was created
} killfeed_entry_t;

static killfeed_entry_t kf_entries[SLAYER_KILLFEED_MAX];
static int kf_count = 0; // number of active entries

// ===========================================================================
// Helpers
// ===========================================================================

static void KF_ParseColor( const char *str, byte *r, byte *g, byte *b, byte *a )
{
	int rv = 0, gv = 0, bv = 0, av = 255;

	if( str && str[0] )
		sscanf( str, "%d %d %d %d", &rv, &gv, &bv, &av );

	*r = (byte)bound( 0, rv, 255 );
	*g = (byte)bound( 0, gv, 255 );
	*b = (byte)bound( 0, bv, 255 );
	*a = (byte)bound( 0, av, 255 );
}

// Try to load weapon icon texture; returns texid or 0.
static int KF_LoadWeaponTexture( const char *weapon )
{
	char path[128];
	int  texid;

	if( !weapon || !weapon[0] )
		return 0;

	Q_snprintf( path, sizeof( path ), "gfx/slayer_killfeed/d_%s.png", weapon );
	texid = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, TF_IMAGE );

	// GL_LoadTexture returns 0 on failure in Xash3D
	return texid;
}

// Easing: ease-out quad
static float KF_EaseOut( float t )
{
	t = bound( 0.0f, t, 1.0f );
	return t * ( 2.0f - t );
}

// Get team ID for a player slot (0-based). Returns 1=T, 2=CT, 0=other.
// We peek into the scoreboard's stored team data.
extern int Slayer_GetPlayerTeam( int slot );

// ===========================================================================
// Public API
// ===========================================================================

void Slayer_Killfeed_Init( void )
{
	Cvar_RegisterVariable( &slayer_killfeed );
	Cvar_RegisterVariable( &slayer_killfeed_duration );
	Cvar_RegisterVariable( &slayer_killfeed_x );
	Cvar_RegisterVariable( &slayer_killfeed_y );
	Cvar_RegisterVariable( &slayer_killfeed_max );
	Cvar_RegisterVariable( &slayer_killfeed_bg_color );
	Cvar_RegisterVariable( &slayer_killfeed_border_color );
	Cvar_RegisterVariable( &slayer_killfeed_local_color );

	Slayer_Killfeed_Reset();
}

void Slayer_Killfeed_Reset( void )
{
	memset( kf_entries, 0, sizeof( kf_entries ) );
	kf_count = 0;
}

void Slayer_Killfeed_OnDeathMsg( int killer, int victim, qboolean headshot, const char *weapon )
{
	killfeed_entry_t *e;

	if( slayer_killfeed.value == 0.0f )
		return;

	// Shift entries down if full
	if( kf_count >= SLAYER_KILLFEED_MAX )
	{
		memmove( &kf_entries[0], &kf_entries[1], sizeof( killfeed_entry_t ) * ( SLAYER_KILLFEED_MAX - 1 ) );
		kf_count = SLAYER_KILLFEED_MAX - 1;
	}

	e = &kf_entries[kf_count];
	memset( e, 0, sizeof( *e ) );

	e->killer_idx = killer;
	e->victim_idx = victim;
	e->headshot   = headshot;
	e->is_local   = ( killer == cl.playernum + 1 );
	e->spawn_time = host.realtime;

	if( weapon && weapon[0] )
	{
		Q_strncpy( e->weapon, weapon, sizeof( e->weapon ) );
		e->weapon_tex = KF_LoadWeaponTexture( weapon );
	}

	kf_count++;
}

void Slayer_Killfeed_Draw( void )
{
	cl_font_t  *font;
	int         scr_w, scr_h;
	int         max_lines;
	float       duration;
	float       anchor_x_frac, anchor_y_frac;
	int         anchor_x, anchor_y;
	int         row_h, icon_size;
	int         drawn;
	int         i;
	byte        bg_r, bg_g, bg_b, bg_a;
	byte        border_r, border_g, border_b, border_a;
	byte        local_r, local_g, local_b, local_a;

	if( slayer_killfeed.value == 0.0f || kf_count == 0 )
		return;

	font = Con_GetCurFont();
	if( !font )
		return;

	scr_w = refState.width;
	scr_h = refState.height;
	row_h = font->charHeight + KF_PAD_Y * 2;
	icon_size = font->charHeight;

	duration = slayer_killfeed_duration.value;
	if( duration <= 0.0f )
		duration = 5.0f;

	max_lines = (int)slayer_killfeed_max.value;
	if( max_lines < 1 ) max_lines = 1;
	if( max_lines > SLAYER_KILLFEED_MAX ) max_lines = SLAYER_KILLFEED_MAX;

	anchor_x_frac = bound( 0.0f, slayer_killfeed_x.value, 1.0f );
	anchor_y_frac = bound( 0.0f, slayer_killfeed_y.value, 1.0f );
	anchor_x = (int)( anchor_x_frac * scr_w );
	anchor_y = (int)( anchor_y_frac * scr_h );

	KF_ParseColor( slayer_killfeed_bg_color.string, &bg_r, &bg_g, &bg_b, &bg_a );
	KF_ParseColor( slayer_killfeed_border_color.string, &border_r, &border_g, &border_b, &border_a );
	KF_ParseColor( slayer_killfeed_local_color.string, &local_r, &local_g, &local_b, &local_a );

	// Expire old entries
	for( i = 0; i < kf_count; )
	{
		float age = host.realtime - kf_entries[i].spawn_time;
		if( age > duration )
		{
			// Remove entry by shifting
			kf_count--;
			if( i < kf_count )
				memmove( &kf_entries[i], &kf_entries[i + 1], sizeof( killfeed_entry_t ) * ( kf_count - i ) );
		}
		else
		{
			i++;
		}
	}

	if( kf_count == 0 )
		return;

	// Draw from newest (top) to oldest
	// Entries are stored oldest-first: [0]=oldest, [kf_count-1]=newest
	// We draw newest at the top of the anchor
	drawn = 0;
	for( i = kf_count - 1; i >= 0 && drawn < max_lines; i--, drawn++ )
	{
		killfeed_entry_t *e = &kf_entries[i];
		float age = host.realtime - e->spawn_time;
		float alpha_mult = 1.0f;
		float slide_frac;
		int   entry_x, entry_y;
		int   entry_w;
		int   text_x;
		int   killer_team, victim_team;
		rgba_t killer_color, victim_color;
		char  killer_name[MAX_SCOREBOARDNAME];
		char  victim_name[MAX_SCOREBOARDNAME];
		int   killer_name_w, victim_name_w, weapon_w, hs_w;
		int   dummy_h;
		byte  cur_border_r, cur_border_g, cur_border_b, cur_border_a;

		// Fade out in last KF_FADE_OUT_TIME seconds
		if( age > duration - KF_FADE_OUT_TIME )
		{
			float fade_progress = ( age - ( duration - KF_FADE_OUT_TIME ) ) / KF_FADE_OUT_TIME;
			alpha_mult = 1.0f - bound( 0.0f, fade_progress, 1.0f );
		}

		if( alpha_mult <= 0.0f )
			continue;

		// Slide-in animation (from right edge)
		slide_frac = KF_EaseOut( bound( 0.0f, age / KF_SLIDE_IN_TIME, 1.0f ) );

		// Gather names
		if( e->killer_idx >= 1 && e->killer_idx <= cl.maxclients )
			Q_strncpy( killer_name, cl.players[e->killer_idx - 1].name, sizeof( killer_name ) );
		else
			Q_strncpy( killer_name, "World", sizeof( killer_name ) );

		if( e->victim_idx >= 1 && e->victim_idx <= cl.maxclients )
			Q_strncpy( victim_name, cl.players[e->victim_idx - 1].name, sizeof( victim_name ) );
		else
			Q_strncpy( victim_name, "???", sizeof( victim_name ) );

		// Compute text widths
		Con_DrawStringLen( killer_name, &killer_name_w, &dummy_h );
		Con_DrawStringLen( victim_name, &victim_name_w, &dummy_h );

		// Weapon text width (used as fallback if no icon)
		weapon_w = icon_size; // default: icon placeholder
		if( e->weapon_tex == 0 && e->weapon[0] )
		{
			char weapon_str[36];
			int ww;
			Q_snprintf( weapon_str, sizeof( weapon_str ), "[%s]", e->weapon );
			Con_DrawStringLen( weapon_str, &ww, &dummy_h );
			weapon_w = ww;
		}

		// Headshot indicator width
		hs_w = 0;
		if( e->headshot )
		{
			int hsw_tmp;
			Con_DrawStringLen( " HS", &hsw_tmp, &dummy_h );
			hs_w = hsw_tmp;
		}

		// Total entry width
		entry_w = KF_PAD_X + killer_name_w + KF_PAD_X + weapon_w + KF_PAD_X + victim_name_w + hs_w + KF_PAD_X;

		// Position: right-aligned from anchor_x, slide in from right
		{
			int fully_in_x = anchor_x - entry_w;
			int off_screen_x = anchor_x + 20; // starts off-screen right
			entry_x = (int)( off_screen_x + ( fully_in_x - off_screen_x ) * slide_frac );
		}
		entry_y = anchor_y + drawn * ( row_h + KF_ENTRY_GAP );

		// Effective alpha
		{
			byte eff_bg_a = (byte)( bg_a * alpha_mult );
			byte eff_border_a;

			// Background fill
			ref.dllFuncs.FillRGBA( kRenderTransTexture, entry_x, entry_y, entry_w, row_h,
				bg_r, bg_g, bg_b, eff_bg_a );

			// Border
			if( e->is_local )
			{
				cur_border_r = local_r;
				cur_border_g = local_g;
				cur_border_b = local_b;
				cur_border_a = local_a;
			}
			else
			{
				cur_border_r = border_r;
				cur_border_g = border_g;
				cur_border_b = border_b;
				cur_border_a = border_a;
			}

			eff_border_a = (byte)( cur_border_a * alpha_mult );

			// Top border
			ref.dllFuncs.FillRGBA( kRenderTransTexture, entry_x, entry_y, entry_w, KF_BORDER_W,
				cur_border_r, cur_border_g, cur_border_b, eff_border_a );
			// Bottom border
			ref.dllFuncs.FillRGBA( kRenderTransTexture, entry_x, entry_y + row_h - KF_BORDER_W, entry_w, KF_BORDER_W,
				cur_border_r, cur_border_g, cur_border_b, eff_border_a );
			// Left border
			ref.dllFuncs.FillRGBA( kRenderTransTexture, entry_x, entry_y, KF_BORDER_W, row_h,
				cur_border_r, cur_border_g, cur_border_b, eff_border_a );
			// Right border
			ref.dllFuncs.FillRGBA( kRenderTransTexture, entry_x + entry_w - KF_BORDER_W, entry_y, KF_BORDER_W, row_h,
				cur_border_r, cur_border_g, cur_border_b, eff_border_a );
		}

		// Team colors for names
		killer_team = ( e->killer_idx >= 1 && e->killer_idx <= cl.maxclients )
			? Slayer_GetPlayerTeam( e->killer_idx - 1 ) : 0;
		victim_team = ( e->victim_idx >= 1 && e->victim_idx <= cl.maxclients )
			? Slayer_GetPlayerTeam( e->victim_idx - 1 ) : 0;

		// Killer color
		if( killer_team == 2 ) { killer_color[0] = KF_CT_R; killer_color[1] = KF_CT_G; killer_color[2] = KF_CT_B; }
		else if( killer_team == 1 ) { killer_color[0] = KF_T_R; killer_color[1] = KF_T_G; killer_color[2] = KF_T_B; }
		else { killer_color[0] = KF_SPEC_R; killer_color[1] = KF_SPEC_G; killer_color[2] = KF_SPEC_B; }
		killer_color[3] = (byte)( 255 * alpha_mult );

		// Victim color
		if( victim_team == 2 ) { victim_color[0] = KF_CT_R; victim_color[1] = KF_CT_G; victim_color[2] = KF_CT_B; }
		else if( victim_team == 1 ) { victim_color[0] = KF_T_R; victim_color[1] = KF_T_G; victim_color[2] = KF_T_B; }
		else { victim_color[0] = KF_SPEC_R; victim_color[1] = KF_SPEC_G; victim_color[2] = KF_SPEC_B; }
		victim_color[3] = (byte)( 255 * alpha_mult );

		// Draw text: [killer] [weapon] [victim] [HS]
		text_x = entry_x + KF_PAD_X;

		// Killer name
		Con_DrawString( text_x, entry_y + KF_PAD_Y, killer_name, killer_color );
		text_x += killer_name_w + KF_PAD_X;

		// Weapon icon or text fallback
		if( e->weapon_tex != 0 )
		{
			byte icon_alpha = (byte)( 255 * alpha_mult );
			ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
			ref.dllFuncs.Color4ub( 255, 255, 255, icon_alpha );
			ref.dllFuncs.R_DrawStretchPic( text_x, entry_y + KF_PAD_Y, icon_size, icon_size, 0, 0, 1, 1, e->weapon_tex );
			text_x += icon_size + KF_PAD_X;
		}
		else if( e->weapon[0] )
		{
			char weapon_str[36];
			rgba_t weapon_color = { 200, 200, 200, (byte)( 200 * alpha_mult ) };
			Q_snprintf( weapon_str, sizeof( weapon_str ), "[%s]", e->weapon );
			Con_DrawString( text_x, entry_y + KF_PAD_Y, weapon_str, weapon_color );
			text_x += weapon_w + KF_PAD_X;
		}
		else
		{
			text_x += KF_PAD_X;
		}

		// Victim name
		Con_DrawString( text_x, entry_y + KF_PAD_Y, victim_name, victim_color );
		text_x += victim_name_w;

		// Headshot indicator
		if( e->headshot )
		{
			rgba_t hs_color = { 255, 50, 50, (byte)( 255 * alpha_mult ) };
			Con_DrawString( text_x, entry_y + KF_PAD_Y, " HS", hs_color );
		}
	}
}
