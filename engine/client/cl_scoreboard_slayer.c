/*
cl_scoreboard_slayer.c - Slayer3D custom scoreboard overlay
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

// NOTE: Steam avatar support requires a server-side SteamID broadcast
// (e.g. ReGameDLL plugin) before per-player avatars can be displayed.

#include <inttypes.h>
#include "common.h"
#include "client.h"
#include "cl_scoreboard_slayer.h"
#include "cl_avatar_download.h"

// ===========================================================================
// Cvars
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_scoreboard, "1", FCVAR_ARCHIVE, "Slayer3D: enable custom scoreboard (0 = disabled)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_bg_color, "20 20 20 180", FCVAR_ARCHIVE, "Slayer3D: scoreboard background RGBA" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_text_color, "255 255 255 255", FCVAR_ARCHIVE, "Slayer3D: scoreboard text RGBA" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_ct_color, "120 180 255", FCVAR_ARCHIVE, "Slayer3D: CT team RGB" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_t_color, "255 100 80", FCVAR_ARCHIVE, "Slayer3D: T team RGB" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_border_color, "255 255 255 200", FCVAR_ARCHIVE, "Slayer3D: scoreboard border RGBA" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_opacity, "220", FCVAR_ARCHIVE, "Slayer3D: overall scoreboard opacity (0-255)" );

// ===========================================================================
// Types
// ===========================================================================

#define SLAYER_TEAM_UNASSIGNED  0
#define SLAYER_TEAM_T           1
#define SLAYER_TEAM_CT          2
#define SLAYER_TEAM_SPECTATOR   3

typedef struct
{
	int  frags;
	int  deaths;
	int  team_id;    // SLAYER_TEAM_*
	int  health;     // HP from cl.local.health (only valid for local/spectated player)
	byte flags;      // bit 0 = dead, bit 2 = has bomb
	byte connected;  // set when ScoreInfo received for this slot
} slayer_score_t;

// ===========================================================================
// Static state
// ===========================================================================

static slayer_score_t  slayer_scores[MAX_CLIENTS];
static qboolean        slayer_scoreboard_active = false;

// Avatar state: SteamID64 per player slot and cached texture handles
static uint64_t        slayer_steamid64[MAX_CLIENTS];
static int             slayer_avatar_tex[MAX_CLIENTS]; // 0 = not tried, >0 = loaded, -1 = failed
static double          slayer_status_next_time;       // throttle: next allowed "status" send
static double          slayer_status_deadline;        // until: parse # lines from svc_print
static qboolean        slayer_status_pending;          // true while we expect status reply

// Cached parsed cvar colors (re-parsed only when cvar string changes)
static char   cached_bg_str[64] = "";
static char   cached_text_str[64] = "";
static char   cached_ct_str[64] = "";
static char   cached_t_str[64] = "";
static char   cached_border_str[64] = "";
static rgba_t cached_color_bg;
static rgba_t cached_color_text;
static rgba_t cached_color_ct;
static rgba_t cached_color_t;
static rgba_t cached_color_border;

// ===========================================================================
// Cvar color parsing helper
// ===========================================================================

static void Slayer_ParseColorString( const char *str, rgba_t out )
{
	int r = 255, g = 255, b = 255, a = 255;
	int count;

	if( !str || str[0] == '\0' )
	{
		MakeRGBA( out, 255, 255, 255, 255 );
		return;
	}

	count = sscanf( str, "%d %d %d %d", &r, &g, &b, &a );

	if( count < 3 )
	{
		MakeRGBA( out, 255, 255, 255, 255 );
		return;
	}

	// Clamp values
	if( r < 0 ) r = 0;
	if( r > 255 ) r = 255;
	if( g < 0 ) g = 0;
	if( g > 255 ) g = 255;
	if( b < 0 ) b = 0;
	if( b > 255 ) b = 255;
	if( a < 0 ) a = 0;
	if( a > 255 ) a = 255;

	out[0] = (byte)r;
	out[1] = (byte)g;
	out[2] = (byte)b;
	out[3] = (byte)a;
}

// ===========================================================================
// Steam avatar helpers
// ===========================================================================

static void Slayer_LoadAvatarTexture( int slot );

void Slayer_ParseStatusLine( const char *line )
{
	int slot;
	char name[64];
	int userid;
	int steam_x, steam_y;
	unsigned int steam_z;
	uint64_t steamid64;
	int i;
	const char *p;
	const char *name_start, *name_end;
	int name_len;

	if( !slayer_status_pending )
		return;

	// Status reply is fast (~1s). Cap parse window so random svc_print
	// messages later (chat, server logs) cannot accidentally match the
	// strict #N "name" STEAM_X:Y:Z format.
	if( host.realtime > slayer_status_deadline )
	{
		slayer_status_pending = false;
		return;
	}

	// Skip non-# lines silently. The status reply begins with header
	// lines (hostname:, version:, players:) which we don't care about,
	// and they would otherwise abort parsing before any '#N' player
	// entry was reached.
	if( !line || line[0] != '#' )
		return;

	// Format: #<slot> "<name>" <userid> STEAM_X:Y:Z ...
	p = line + 1;

	// Parse slot number
	slot = 0;
	while( *p >= '0' && *p <= '9' )
	{
		slot = slot * 10 + ( *p - '0' );
		p++;
	}

	if( slot < 1 || slot > MAX_CLIENTS )
		return;

	// Skip whitespace
	while( *p == ' ' || *p == '\t' )
		p++;

	// Parse quoted name
	if( *p != '"' )
		return;
	p++;
	name_start = p;

	while( *p && *p != '"' )
		p++;

	if( *p != '"' )
		return;

	name_end = p;
	name_len = (int)( name_end - name_start );
	if( name_len <= 0 || name_len >= (int)sizeof( name ) )
		return;

	memcpy( name, name_start, name_len );
	name[name_len] = '\0';
	p++; // skip closing quote

	// Skip whitespace
	while( *p == ' ' || *p == '\t' )
		p++;

	// Parse userid (skip it)
	userid = 0;
	while( *p >= '0' && *p <= '9' )
	{
		userid = userid * 10 + ( *p - '0' );
		p++;
	}
	(void)userid;

	// Skip whitespace
	while( *p == ' ' || *p == '\t' )
		p++;

	// Parse STEAM_X:Y:Z
	if( Q_strncmp( p, "STEAM_", 6 ) != 0 )
		return;
	p += 6;

	// X
	steam_x = 0;
	while( *p >= '0' && *p <= '9' )
	{
		steam_x = steam_x * 10 + ( *p - '0' );
		p++;
	}
	(void)steam_x;

	if( *p != ':' )
		return;
	p++;

	// Y
	steam_y = 0;
	while( *p >= '0' && *p <= '9' )
	{
		steam_y = steam_y * 10 + ( *p - '0' );
		p++;
	}

	if( *p != ':' )
		return;
	p++;

	// Z
	steam_z = 0;
	while( *p >= '0' && *p <= '9' )
	{
		steam_z = steam_z * 10 + ( *p - '0' );
		p++;
	}

	// Compute SteamID64
	steamid64 = 76561197960265728ULL + (uint64_t)steam_z * 2 + (uint64_t)steam_y;

	// Use the authoritative slot number directly (1-based -> 0-based)
	i = slot - 1;
	slayer_steamid64[i] = steamid64;
	slayer_avatar_tex[i] = 0; // reset so texture will be reloaded

	Con_Printf( "Slayer: parsed steamid %"PRIu64" for slot %d\n", steamid64, slot );

	// Load texture immediately at parse time (outside render loop)
	Slayer_LoadAvatarTexture( i );
}

static void Slayer_LoadAvatarTexture( int slot )
{
	char path[128];

	if( slot < 0 || slot >= MAX_CLIENTS )
		return;

	if( slayer_steamid64[slot] == 0 )
		return;

	if( slayer_avatar_tex[slot] != 0 )
		return; // already attempted (loaded or failed)

	Q_snprintf( path, sizeof( path ), "avatars/%"PRIu64".png", slayer_steamid64[slot] );

	if( !FS_FileExists( path, false ) )
	{
		// Request automatic download
		Slayer_AvatarDownload_Request( slayer_steamid64[slot], slot );
		slayer_avatar_tex[slot] = -1;
		return;
	}

	slayer_avatar_tex[slot] = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, TF_IMAGE );

	if( slayer_avatar_tex[slot] == 0 )
		slayer_avatar_tex[slot] = -1;
}

// ===========================================================================
// Console commands
// ===========================================================================

static void Cmd_AvatarUrls_f( void )
{
	int i, count = 0;

	Con_Printf( "=== Steam Avatar URLs ===\n" );

	for( i = 0; i < MAX_CLIENTS; i++ )
	{
		if( slayer_steamid64[i] == 0 )
			continue;
		if( cl.players[i].name[0] == '\0' )
			continue;

		Con_Printf( "%s: https://steamcommunity.com/profiles/%llu\n",
			cl.players[i].name, (unsigned long long)slayer_steamid64[i] );
		count++;
	}

	if( count == 0 )
		Con_Printf( "No SteamIDs found. Open scoreboard first to fetch player info.\n" );
	else
		Con_Printf( "Place avatar images at: avatars/<steamid64>.png\n" );
}

// ===========================================================================
// +slayer_scoreboard / -slayer_scoreboard commands
// ===========================================================================

static void Cmd_ScoreboardDown_f( void )
{
	slayer_scoreboard_active = true;

	// Request status to get SteamIDs (throttled to once per 30 seconds)
	if( host.realtime >= slayer_status_next_time )
	{
		Cbuf_AddText( "status\n" );
		slayer_status_next_time = host.realtime + 30.0;
		slayer_status_pending = true;
		slayer_status_deadline = host.realtime + 5.0; // 5s parse window
	}
}

static void Cmd_ScoreboardUp_f( void )
{
	slayer_scoreboard_active = false;
}

// ===========================================================================
// Public API - Init / Reset / Health
// ===========================================================================

void Slayer_Scoreboard_Init( void )
{
	Cvar_RegisterVariable( &slayer_scoreboard );
	Cvar_RegisterVariable( &slayer_scoreboard_bg_color );
	Cvar_RegisterVariable( &slayer_scoreboard_text_color );
	Cvar_RegisterVariable( &slayer_scoreboard_ct_color );
	Cvar_RegisterVariable( &slayer_scoreboard_t_color );
	Cvar_RegisterVariable( &slayer_scoreboard_border_color );
	Cvar_RegisterVariable( &slayer_scoreboard_opacity );

	Cmd_AddCommand( "+slayer_scoreboard", Cmd_ScoreboardDown_f,
		"show Slayer3D custom scoreboard" );
	Cmd_AddCommand( "-slayer_scoreboard", Cmd_ScoreboardUp_f,
		"hide Slayer3D custom scoreboard" );
	Cmd_AddCommand( "slayer_avatar_urls", Cmd_AvatarUrls_f,
		"print Steam avatar download URLs for all players" );

	Slayer_AvatarDownload_Init();
}

void Slayer_Scoreboard_Reset( void )
{
	memset( slayer_scores, 0, sizeof( slayer_scores ) );
	memset( slayer_steamid64, 0, sizeof( slayer_steamid64 ) );
	memset( slayer_avatar_tex, 0, sizeof( slayer_avatar_tex ) );
	slayer_scoreboard_active = false;
	slayer_status_pending = false;
	slayer_status_next_time = 0.0;   // allow immediate re-fetch on next connect
	slayer_status_deadline = 0.0;

	Slayer_AvatarDownload_Reset();
}

void Slayer_OnHealthUpdate( int hp )
{
	if( cl.playernum >= 0 && cl.playernum < MAX_CLIENTS )
		slayer_scores[cl.playernum].health = hp;
}

// ===========================================================================
// User message hooks
// ===========================================================================

void Slayer_OnScoreInfo( const byte *pbuf, int iSize )
{
	int slot;
	int frags, deaths, team_id;

	// ScoreInfo format: byte slot(1-based), short frags, short deaths,
	//                   short class_id, short team_id
	// Some mods (ReGameDLL variants) send 10+ bytes with extra data;
	// we only need the first 9 so accept anything >= 9 silently.
	if( !pbuf || iSize < 9 )
		return;

	slot = pbuf[0];
	if( slot < 1 || slot > MAX_CLIENTS )
		return;

	frags   = (short)( pbuf[1] | ( pbuf[2] << 8 ) );
	deaths  = (short)( pbuf[3] | ( pbuf[4] << 8 ) );
	// skip class_id (pbuf[5..6])
	team_id = (short)( pbuf[7] | ( pbuf[8] << 8 ) );

	slot--; // convert to 0-based index

	slayer_scores[slot].frags     = frags;
	slayer_scores[slot].deaths    = deaths;
	slayer_scores[slot].team_id   = team_id;
	slayer_scores[slot].connected = 1;
}

void Slayer_OnScoreAttrib( const byte *pbuf, int iSize )
{
	int slot;

	// ScoreAttrib format: byte slot(1-based), byte flags
	if( !pbuf || iSize < 2 )
		return;

	slot = pbuf[0];
	if( slot < 1 || slot > MAX_CLIENTS )
		return;

	slayer_scores[slot - 1].flags = pbuf[1];
}

void Slayer_OnHealthInfo( const byte *pbuf, int iSize )
{
	int slot;

	// HealthInfo format (ReGameDLL): byte slot(1-based), byte health
	if( !pbuf || iSize < 2 )
		return;

	slot = pbuf[0];
	if( slot < 1 || slot > MAX_CLIENTS )
		return;

	slayer_scores[slot - 1].health = pbuf[1];
}

// ===========================================================================
// Drawing helpers
// ===========================================================================

static void Slayer_DrawRect( int x, int y, int w, int h, byte r, byte g, byte b, byte a )
{
	ref.dllFuncs.FillRGBA( kRenderTransTexture, x, y, w, h, r, g, b, a );
}

// ===========================================================================
// Sorting
// ===========================================================================

typedef struct
{
	int idx;     // 0-based player index
	int team_id;
	int frags;
} slayer_sort_entry_t;

static int Slayer_SortCompare( const void *a, const void *b )
{
	const slayer_sort_entry_t *ea = (const slayer_sort_entry_t *)a;
	const slayer_sort_entry_t *eb = (const slayer_sort_entry_t *)b;

	// CT first (2), then T (1), then others
	if( ea->team_id != eb->team_id )
	{
		// CT (2) before T (1) before unassigned (0) / spectator (3)
		int order_a = ( ea->team_id == SLAYER_TEAM_CT ) ? 0 :
		              ( ea->team_id == SLAYER_TEAM_T ) ? 1 : 2;
		int order_b = ( eb->team_id == SLAYER_TEAM_CT ) ? 0 :
		              ( eb->team_id == SLAYER_TEAM_T ) ? 1 : 2;
		if( order_a != order_b )
			return order_a - order_b;
	}

	// Within same team: higher frags first
	if( ea->frags != eb->frags )
		return eb->frags - ea->frags;

	return 0;
}

// ===========================================================================
// Main draw function
// ===========================================================================

void Slayer_Scoreboard_Draw( void )
{
	slayer_sort_entry_t sorted[MAX_CLIENTS];
	int          num_players = 0;
	int          i, row;
	int          screen_w, screen_h;
	int          board_x, board_y, board_w, board_h;
	int          row_h, col_name_x, col_frags_x, col_deaths_x, col_ping_x, col_health_x;
	int          text_w, text_h;
	int          cur_y;
	int          ct_player_count = 0, t_player_count = 0, spec_player_count = 0;
	int          drawn_ct_header = 0, drawn_t_header = 0, drawn_spec_header = 0;
	const char  *hostname;
	char         buf[128];
	rgba_t       color_text, color_ct, color_t, color_spec;
	rgba_t       color_bg;
	int          global_opacity;
	cl_font_t   *font;

	// Pump avatar downloads every frame (even when scoreboard hidden)
	if( Slayer_AvatarDownload_Frame() )
	{
		// A download completed - try to reload textures for slots that were pending
		for( i = 0; i < MAX_CLIENTS; i++ )
		{
			if( slayer_avatar_tex[i] == -1 && slayer_steamid64[i] != 0 )
			{
				char avpath[128];
				Q_snprintf( avpath, sizeof( avpath ), "avatars/%" PRIu64 ".png", slayer_steamid64[i] );
				if( FS_FileExists( avpath, false ) )
				{
					slayer_avatar_tex[i] = ref.dllFuncs.GL_LoadTexture( avpath, NULL, 0, TF_IMAGE );
					if( slayer_avatar_tex[i] == 0 )
						slayer_avatar_tex[i] = -1;
				}
			}
		}
	}

	if( !slayer_scoreboard_active )
		return;

	if( slayer_scoreboard.value == 0.0f )
		return;

	if( cls.state != ca_active )
		return;

	screen_w = refState.width;
	screen_h = refState.height;

	if( screen_w <= 0 || screen_h <= 0 )
		return;

	// Get font metrics
	font = Con_GetCurFont();
	if( !font )
		return;

	row_h = font->charHeight + 4;

	// Use cached cvar colors (re-parsed only when cvar string changes)
	if( Q_strcmp( cached_bg_str, slayer_scoreboard_bg_color.string ) )
	{
		Q_strncpy( cached_bg_str, slayer_scoreboard_bg_color.string, sizeof( cached_bg_str ) );
		Slayer_ParseColorString( cached_bg_str, cached_color_bg );
	}
	if( Q_strcmp( cached_text_str, slayer_scoreboard_text_color.string ) )
	{
		Q_strncpy( cached_text_str, slayer_scoreboard_text_color.string, sizeof( cached_text_str ) );
		Slayer_ParseColorString( cached_text_str, cached_color_text );
	}
	if( Q_strcmp( cached_ct_str, slayer_scoreboard_ct_color.string ) )
	{
		Q_strncpy( cached_ct_str, slayer_scoreboard_ct_color.string, sizeof( cached_ct_str ) );
		Slayer_ParseColorString( cached_ct_str, cached_color_ct );
	}
	if( Q_strcmp( cached_t_str, slayer_scoreboard_t_color.string ) )
	{
		Q_strncpy( cached_t_str, slayer_scoreboard_t_color.string, sizeof( cached_t_str ) );
		Slayer_ParseColorString( cached_t_str, cached_color_t );
	}
	if( Q_strcmp( cached_border_str, slayer_scoreboard_border_color.string ) )
	{
		Q_strncpy( cached_border_str, slayer_scoreboard_border_color.string, sizeof( cached_border_str ) );
		Slayer_ParseColorString( cached_border_str, cached_color_border );
	}

	memcpy( color_bg, cached_color_bg, sizeof( rgba_t ) );
	memcpy( color_text, cached_color_text, sizeof( rgba_t ) );
	memcpy( color_ct, cached_color_ct, sizeof( rgba_t ) );
	color_ct[3] = 255;
	memcpy( color_t, cached_color_t, sizeof( rgba_t ) );
	color_t[3] = 255;
	MakeRGBA( color_spec, 180, 180, 180, 255 );

	global_opacity = (int)slayer_scoreboard_opacity.value;
	if( global_opacity < 0 ) global_opacity = 0;
	if( global_opacity > 255 ) global_opacity = 255;

	// Count active players (must have a name and have received ScoreInfo)
	for( i = 0; i < cl.maxclients && i < MAX_CLIENTS; i++ )
	{
		// Clear stale connected flag when server has cleared the player name
		if( cl.players[i].name[0] == '\0' )
		{
			slayer_scores[i].connected = 0;
			slayer_steamid64[i] = 0;
			slayer_avatar_tex[i] = 0;
			continue;
		}

		if( !slayer_scores[i].connected )
		{
			// Force-include local player even without ScoreInfo
			if( i == cl.playernum )
			{
				slayer_scores[i].connected = 1;
			}
			else
			{
				// Player has a name but no ScoreInfo - skip entirely
				// (do NOT assign spectator team, this caused wrong counts)
				continue;
			}
		}

		sorted[num_players].idx     = i;
		sorted[num_players].team_id = slayer_scores[i].team_id;
		sorted[num_players].frags   = slayer_scores[i].frags;
		num_players++;
	}

	if( num_players == 0 )
		return;

	// Sort players
	qsort( sorted, num_players, sizeof( slayer_sort_entry_t ), Slayer_SortCompare );

	// Calculate team player counts
	for( i = 0; i < num_players; i++ )
	{
		if( sorted[i].team_id == SLAYER_TEAM_CT )
			ct_player_count++;
		else if( sorted[i].team_id == SLAYER_TEAM_T )
			t_player_count++;
		else
			spec_player_count++;
	}

	// Fixed board width: 65% of screen_w
	board_w = (int)( screen_w * 0.65f );

	// Height adapts to content: header + column headers + team headers + rows + padding
	{
		int content_rows = num_players + 4; // +4 for header, column headers, 2 team headers
		int min_h = row_h * content_rows + 40;
		int max_h = (int)( screen_h * 0.85f );

		board_h = min_h;
		if( board_h > max_h )
			board_h = max_h;
		if( board_h < (int)( screen_h * 0.20f ) )
			board_h = (int)( screen_h * 0.20f );
	}

	// Center the board
	board_x = ( screen_w - board_w ) / 2;
	board_y = ( screen_h - board_h ) / 2;

	// Simulated rounded corners (20px radius, 5 strips per side)
	{
		byte bg_r = color_bg[0], bg_g = color_bg[1], bg_b = color_bg[2];
		byte bg_a = (byte)( color_bg[3] * global_opacity / 255 );
		// Main body (inset 20px top/bottom)
		Slayer_DrawRect( board_x, board_y + 20, board_w, board_h - 40, bg_r, bg_g, bg_b, bg_a );
		// Top rounding (5 strips, progressively narrower)
		Slayer_DrawRect( board_x + 16, board_y, board_w - 32, 4, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 12, board_y + 4, board_w - 24, 4, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 8, board_y + 8, board_w - 16, 4, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 4, board_y + 12, board_w - 8, 4, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 2, board_y + 16, board_w - 4, 4, bg_r, bg_g, bg_b, bg_a );
		// Bottom rounding (5 strips, progressively narrower)
		Slayer_DrawRect( board_x + 2, board_y + board_h - 20, board_w - 4, 4, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 4, board_y + board_h - 16, board_w - 8, 4, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 8, board_y + board_h - 12, board_w - 16, 4, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 12, board_y + board_h - 8, board_w - 24, 4, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 16, board_y + board_h - 4, board_w - 32, 4, bg_r, bg_g, bg_b, bg_a );
	}

	// Rounded border (2px thick, follows same contour as background)
	{
		byte br_r = cached_color_border[0], br_g = cached_color_border[1];
		byte br_b = cached_color_border[2], br_a = (byte)( cached_color_border[3] * global_opacity / 255 );

		// Top edge strips (2px thick, matching rounded insets)
		Slayer_DrawRect( board_x + 16, board_y, board_w - 32, 2, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + 12, board_y + 4, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 16, board_y + 4, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + 8, board_y + 8, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 12, board_y + 8, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + 4, board_y + 12, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 8, board_y + 12, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + 2, board_y + 16, 2, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 4, board_y + 16, 2, 4, br_r, br_g, br_b, br_a );

		// Left and right edges (2px wide, main body region)
		Slayer_DrawRect( board_x, board_y + 20, 2, board_h - 40, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 2, board_y + 20, 2, board_h - 40, br_r, br_g, br_b, br_a );

		// Bottom edge strips (2px thick, matching rounded insets)
		Slayer_DrawRect( board_x + 2, board_y + board_h - 20, 2, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 4, board_y + board_h - 20, 2, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + 4, board_y + board_h - 16, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 8, board_y + board_h - 16, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + 8, board_y + board_h - 12, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 12, board_y + board_h - 12, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + 12, board_y + board_h - 8, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 16, board_y + board_h - 8, 4, 4, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + 16, board_y + board_h - 2, board_w - 32, 2, br_r, br_g, br_b, br_a );
	}

	cur_y = board_y;

	// Column layout (percentage of board width)
	col_name_x   = board_x + (int)( board_w * 0.04f );
	col_health_x = board_x + (int)( board_w * 0.45f );
	col_frags_x  = board_x + (int)( board_w * 0.58f );
	col_deaths_x = board_x + (int)( board_w * 0.70f );
	col_ping_x   = board_x + (int)( board_w * 0.82f );

	// Draw server name (left-aligned)
	hostname = Info_ValueForKey( cl.serverinfo, "hostname" );
	if( !hostname || hostname[0] == '\0' || !Q_stricmp( hostname, "empty" ) )
		hostname = Info_ValueForKey( cl.serverinfo, "name" );
	if( !hostname || hostname[0] == '\0' || !Q_stricmp( hostname, "empty" ) )
		hostname = Cvar_VariableString( "hostname" );
	if( !hostname || hostname[0] == '\0' || !Q_stricmp( hostname, "empty" ) )
		hostname = cls.servername;

	cur_y += 4;
	Con_DrawString( col_name_x, cur_y, hostname, color_text );

	// Draw map name (right-aligned, same line as hostname)
	{
		const char *mapname = Info_ValueForKey( cl.serverinfo, "map" );
		if( !mapname || mapname[0] == '\0' )
			mapname = clgame.mapname;
		if( mapname && mapname[0] != '\0' )
		{
			rgba_t color_map;
			MakeRGBA( color_map, color_text[0] * 160 / 255, color_text[1] * 160 / 255, color_text[2] * 160 / 255, 200 );
			Con_DrawStringLen( mapname, &text_w, &text_h );
			Con_DrawString( col_ping_x + (int)( board_w * 0.10f ) - text_w, cur_y, mapname, color_map );
		}
	}
	cur_y += row_h + 2;

	// Separator below hostname/mapname
	Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, 1, 60, 60, 60, (byte)global_opacity );
	cur_y += 4;

	// Column headers row (slightly dimmer text, no "Name" label)
	{
		rgba_t color_hdr;
		MakeRGBA( color_hdr, color_text[0] * 200 / 255, color_text[1] * 200 / 255, color_text[2] * 200 / 255, color_text[3] );
		Con_DrawString( col_health_x, cur_y, "Health", color_hdr );
		Con_DrawString( col_frags_x, cur_y, "Kills", color_hdr );
		Con_DrawString( col_deaths_x, cur_y, "Deaths", color_hdr );
		Con_DrawString( col_ping_x, cur_y, "Ping", color_hdr );
	}
	cur_y += row_h;

	// Separator below column headers
	Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, 1, 60, 60, 60, (byte)global_opacity );
	cur_y += 4;

	// Draw player rows
	for( row = 0; row < num_players; row++ )
	{
		int     pidx = sorted[row].idx;
		int     team = sorted[row].team_id;
		rgba_t  name_color;
		const char *name;
		byte    row_alpha;

		// Stop drawing if we exceed the board
		if( cur_y + row_h > board_y + board_h - 4 )
			break;

		// Team section headers
		if( team == SLAYER_TEAM_CT && !drawn_ct_header )
		{
			drawn_ct_header = 1;
			Q_snprintf( buf, sizeof( buf ), "Counter-Terrorists  -  %d", ct_player_count );
			Con_DrawString( col_name_x, cur_y, buf, color_ct );
			cur_y += row_h;
			// Thin separator below CT header
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1, color_ct[0], color_ct[1], color_ct[2], 100 );
			cur_y += 3;
		}
		else if( team == SLAYER_TEAM_T && !drawn_t_header )
		{
			drawn_t_header = 1;
			// Spacing between teams
			cur_y += 4;
			Q_snprintf( buf, sizeof( buf ), "Terrorists  -  %d", t_player_count );
			Con_DrawString( col_name_x, cur_y, buf, color_t );
			cur_y += row_h;
			// Thin separator below T header
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1, color_t[0], color_t[1], color_t[2], 100 );
			cur_y += 3;
		}
		else if( team != SLAYER_TEAM_CT && team != SLAYER_TEAM_T && !drawn_spec_header )
		{
			drawn_spec_header = 1;
			// Spacing before spectator section
			cur_y += 4;
			Q_snprintf( buf, sizeof( buf ), "Spectators  -  %d", spec_player_count );
			Con_DrawString( col_name_x, cur_y, buf, color_spec );
			cur_y += row_h;
			// Thin separator below Spectator header
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1, 100, 100, 100, 80 );
			cur_y += 3;
		}

		// Stop drawing if we exceed the board after team header
		if( cur_y + row_h > board_y + board_h - 4 )
			break;

		// Alternating row backgrounds (every other row slightly lighter)
		if( row % 2 == 0 )
		{
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, row_h, 255, 255, 255, 8 );
		}

		// Highlight local player row (brighter)
		if( pidx == cl.playernum )
		{
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, row_h, 255, 255, 255, 35 );
		}

		// Player name
		name = cl.players[pidx].name;

		if( team == SLAYER_TEAM_CT )
			MakeRGBA( name_color, color_ct[0], color_ct[1], color_ct[2], 255 );
		else if( team == SLAYER_TEAM_T )
			MakeRGBA( name_color, color_t[0], color_t[1], color_t[2], 255 );
		else
			MakeRGBA( name_color, 200, 200, 200, 255 );

		// Dead players: dimmed (85% brightness)
		row_alpha = 255;
		if( (slayer_scores[pidx].flags & 1) && (team == SLAYER_TEAM_CT || team == SLAYER_TEAM_T) )
		{
			name_color[0] = name_color[0] * 85 / 100;
			name_color[1] = name_color[1] * 85 / 100;
			name_color[2] = name_color[2] * 85 / 100;
			row_alpha = 128;
		}

		// Draw avatar if available
		{
			int name_x_offset = 0;

			if( slayer_avatar_tex[pidx] > 0 )
			{
				ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
				ref.dllFuncs.Color4ub( 255, 255, 255, row_alpha );
				ref.dllFuncs.R_DrawStretchPic( col_name_x, cur_y, row_h, row_h, 0, 0, 1, 1, slayer_avatar_tex[pidx] );
				name_x_offset = row_h + 4;
			}

			Con_DrawString( col_name_x + name_x_offset, cur_y + 2, name, name_color );
		}

		// Frags
		{
			rgba_t stat_color;
			int hp = 0;

			if( team == SLAYER_TEAM_CT )
				MakeRGBA( stat_color, color_ct[0], color_ct[1], color_ct[2], row_alpha );
			else if( team == SLAYER_TEAM_T )
				MakeRGBA( stat_color, color_t[0], color_t[1], color_t[2], row_alpha );
			else
				MakeRGBA( stat_color, color_text[0], color_text[1], color_text[2], row_alpha );
			Q_snprintf( buf, sizeof( buf ), "%d", slayer_scores[pidx].frags );
			Con_DrawString( col_frags_x, cur_y + 2, buf, stat_color );

			// Deaths
			Q_snprintf( buf, sizeof( buf ), "%d", slayer_scores[pidx].deaths );
			Con_DrawString( col_deaths_x, cur_y + 2, buf, stat_color );

			// Ping
			if( cl.players[pidx].ping == 0 )
				Q_snprintf( buf, sizeof( buf ), "-" );
			else
				Q_snprintf( buf, sizeof( buf ), "%d", cl.players[pidx].ping );
			Con_DrawString( col_ping_x, cur_y + 2, buf, stat_color );

			// Health column
			if( slayer_scores[pidx].flags & 1 )
			{
				// Dead player: show "DEAD" text in team color with alpha 200
				rgba_t dead_color;
				if( team == SLAYER_TEAM_CT )
					MakeRGBA( dead_color, color_ct[0], color_ct[1], color_ct[2], 200 );
				else if( team == SLAYER_TEAM_T )
					MakeRGBA( dead_color, color_t[0], color_t[1], color_t[2], 200 );
				else
					MakeRGBA( dead_color, color_text[0], color_text[1], color_text[2], 200 );
				Con_DrawString( col_health_x, cur_y + 2, "DEAD", dead_color );
			}
			else
			{
				if( pidx == cl.playernum )
				{
					hp = cl.local.health;
					slayer_scores[pidx].health = hp;
				}
				else
				{
					hp = slayer_scores[pidx].health;
				}

				if( hp > 0 && hp <= 100 )
				{
					Q_snprintf( buf, sizeof( buf ), "%d", hp );
					Con_DrawString( col_health_x, cur_y + 2, buf, stat_color );
				}
			}
		}

		cur_y += row_h;
	}
}
