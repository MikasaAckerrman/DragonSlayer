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

#include "common.h"
#include "client.h"
#include "cl_scoreboard_slayer.h"

// ===========================================================================
// Cvars
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_scoreboard, "1", FCVAR_ARCHIVE, "Slayer3D: enable custom scoreboard (0 = disabled)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_bg_color, "20 20 20 180", FCVAR_ARCHIVE, "Slayer3D: scoreboard background RGBA" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_text_color, "255 255 255 255", FCVAR_ARCHIVE, "Slayer3D: scoreboard text RGBA" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_ct_color, "120 180 255", FCVAR_ARCHIVE, "Slayer3D: CT team RGB" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_t_color, "255 100 80", FCVAR_ARCHIVE, "Slayer3D: T team RGB" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_opacity, "220", FCVAR_ARCHIVE, "Slayer3D: overall scoreboard opacity (0-255)" );
static CVAR_DEFINE_AUTO( slayer_steam_apikey, "", FCVAR_PROTECTED, "Slayer3D: Steam Web API key (for future avatar use)" );

// ===========================================================================
// Types
// ===========================================================================

#define SLAYER_TEAM_UNASSIGNED  0
#define SLAYER_TEAM_T           1
#define SLAYER_TEAM_CT          2
#define SLAYER_TEAM_SPECTATOR   3

#define SLAYER_MAX_AVATARS      64

typedef struct
{
	int  frags;
	int  deaths;
	int  team_id;    // SLAYER_TEAM_*
	int  health;     // HP from cl.local.health (only valid for local/spectated player)
	byte flags;      // bit 0 = dead, bit 2 = has bomb
	byte connected;  // set when ScoreInfo received for this slot
} slayer_score_t;

typedef struct
{
	char   steamid[32];   // SteamID64 string
	int    texnum;        // GL texture number, 0 = not loaded
} slayer_avatar_t;

// ===========================================================================
// Static state
// ===========================================================================

static slayer_score_t  slayer_scores[MAX_CLIENTS];
static slayer_avatar_t slayer_avatars[SLAYER_MAX_AVATARS];
static int             slayer_avatar_count = 0;
static qboolean        slayer_scoreboard_active = false;

// Cached parsed cvar colors (re-parsed only when cvar string changes)
static char   cached_bg_str[64] = "";
static char   cached_text_str[64] = "";
static char   cached_ct_str[64] = "";
static char   cached_t_str[64] = "";
static rgba_t cached_color_bg;
static rgba_t cached_color_text;
static rgba_t cached_color_ct;
static rgba_t cached_color_t;

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
// +slayer_scoreboard / -slayer_scoreboard commands
// ===========================================================================

static void Cmd_ScoreboardDown_f( void )
{
	slayer_scoreboard_active = true;
}

static void Cmd_ScoreboardUp_f( void )
{
	slayer_scoreboard_active = false;
}

// ===========================================================================
// Avatar support (local file cache)
// ===========================================================================

static int Slayer_FindAvatarTexture( const char *steamid )
{
	int i;

	for( i = 0; i < slayer_avatar_count; i++ )
	{
		if( !Q_strcmp( slayer_avatars[i].steamid, steamid ) )
			return slayer_avatars[i].texnum;
	}

	return 0;
}

static int Slayer_LoadAvatarFromFile( const char *steamid )
{
	char path[MAX_QPATH];
	int  texnum;
	const char *p;

	if( !steamid || steamid[0] == '\0' )
		return 0;

	// Validate steamid contains only alphanumeric characters to prevent path traversal
	for( p = steamid; *p; p++ )
	{
		if( !( ( *p >= '0' && *p <= '9' ) || ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) ) )
			return 0;
	}

	// Check if already cached
	texnum = Slayer_FindAvatarTexture( steamid );
	if( texnum > 0 )
		return texnum;

	// Try to load from avatars/<steamid64>.png
	Q_snprintf( path, sizeof( path ), "avatars/%s.png", steamid );

	texnum = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, TF_IMAGE );
	if( texnum <= 0 )
		return 0;

	// Cache it
	if( slayer_avatar_count < SLAYER_MAX_AVATARS )
	{
		Q_strncpy( slayer_avatars[slayer_avatar_count].steamid, steamid, sizeof( slayer_avatars[0].steamid ) );
		slayer_avatars[slayer_avatar_count].texnum = texnum;
		slayer_avatar_count++;
	}

	return texnum;
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
	Cvar_RegisterVariable( &slayer_scoreboard_opacity );
	Cvar_RegisterVariable( &slayer_steam_apikey );

	Cmd_AddCommand( "+slayer_scoreboard", Cmd_ScoreboardDown_f,
		"show Slayer3D custom scoreboard" );
	Cmd_AddCommand( "-slayer_scoreboard", Cmd_ScoreboardUp_f,
		"hide Slayer3D custom scoreboard" );
}

void Slayer_Scoreboard_Reset( void )
{
	memset( slayer_scores, 0, sizeof( slayer_scores ) );
	memset( slayer_avatars, 0, sizeof( slayer_avatars ) );
	slayer_avatar_count = 0;
	slayer_scoreboard_active = false;
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
	if( !pbuf || iSize < 9 )
		return;

	if( iSize != 9 )
		Con_DPrintf( S_WARN "Slayer_OnScoreInfo: unexpected iSize %d (expected 9)\n", iSize );

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

static void Slayer_DrawBorder( int x, int y, int w, int h, byte r, byte g, byte b, byte a, int thickness )
{
	// Top
	Slayer_DrawRect( x, y, w, thickness, r, g, b, a );
	// Bottom
	Slayer_DrawRect( x, y + h - thickness, w, thickness, r, g, b, a );
	// Left
	Slayer_DrawRect( x, y, thickness, h, r, g, b, a );
	// Right
	Slayer_DrawRect( x + w - thickness, y, thickness, h, r, g, b, a );
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
	int          row_h, col_name_x, col_frags_x, col_deaths_x, col_ping_x;
	int          text_w, text_h;
	int          cur_y;
	int          ct_player_count = 0, t_player_count = 0;
	int          drawn_ct_header = 0, drawn_t_header = 0, drawn_spec_header = 0;
	const char  *hostname;
	char         buf[128];
	rgba_t       color_text, color_ct, color_t, color_spec;
	rgba_t       color_bg;
	int          global_opacity;
	cl_font_t   *font;

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
	}

	// Fixed board width: 60% of screen_w
	board_w = (int)( screen_w * 0.60f );

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

	// Draw main background
	Slayer_DrawRect( board_x, board_y, board_w, board_h,
		color_bg[0], color_bg[1], color_bg[2], (byte)( color_bg[3] * global_opacity / 255 ) );

	// Draw thin border (2px)
	Slayer_DrawBorder( board_x, board_y, board_w, board_h, 80, 80, 80, (byte)global_opacity, 2 );

	cur_y = board_y;

	// Column layout (percentage of board width)
	col_name_x   = board_x + (int)( board_w * 0.05f );
	col_frags_x  = board_x + (int)( board_w * 0.58f );
	col_deaths_x = board_x + (int)( board_w * 0.72f );
	col_ping_x   = board_x + (int)( board_w * 0.86f );

	// Draw dark header bar (covers only hostname/mapname row)
	Slayer_DrawRect( board_x, cur_y, board_w, row_h + 6,
		10, 10, 10, (byte)( global_opacity * 240 / 255 ) );

	// Draw server name (left-aligned)
	hostname = Info_ValueForKey( cl.serverinfo, "hostname" );
	if( !hostname || hostname[0] == '\0' )
		hostname = Info_ValueForKey( cl.serverinfo, "name" );
	if( !hostname || hostname[0] == '\0' )
		hostname = Cvar_VariableString( "hostname" );
	if( !hostname || hostname[0] == '\0' )
		hostname = Cvar_VariableString( "sv_hostname" );
	if( !hostname || hostname[0] == '\0' )
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

	// Column headers row (slightly dimmer text)
	{
		rgba_t color_hdr;
		MakeRGBA( color_hdr, color_text[0] * 200 / 255, color_text[1] * 200 / 255, color_text[2] * 200 / 255, color_text[3] );
		Con_DrawString( col_name_x, cur_y, "Name", color_hdr );
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
			// Thin separator before CT section
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1, color_ct[0], color_ct[1], color_ct[2], 100 );
			cur_y += 3;
			Q_snprintf( buf, sizeof( buf ), "Counter-Terrorists  -  %d", ct_player_count );
			Con_DrawString( col_name_x, cur_y, buf, color_ct );
			cur_y += row_h;
		}
		else if( team == SLAYER_TEAM_T && !drawn_t_header )
		{
			drawn_t_header = 1;
			// Spacing + separator between teams
			cur_y += 4;
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1, color_t[0], color_t[1], color_t[2], 100 );
			cur_y += 3;
			Q_snprintf( buf, sizeof( buf ), "Terrorists  -  %d", t_player_count );
			Con_DrawString( col_name_x, cur_y, buf, color_t );
			cur_y += row_h;
		}
		else if( team != SLAYER_TEAM_CT && team != SLAYER_TEAM_T && !drawn_spec_header )
		{
			drawn_spec_header = 1;
			// Spacing before spectator section
			cur_y += 4;
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1, 100, 100, 100, 80 );
			cur_y += 3;
			Con_DrawString( col_name_x, cur_y, "Spectators", color_spec );
			cur_y += row_h;
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

		// Dead players: halved alpha (dimmed)
		row_alpha = 255;
		if( slayer_scores[pidx].flags & 1 )
		{
			name_color[0] = name_color[0] / 2;
			name_color[1] = name_color[1] / 2;
			name_color[2] = name_color[2] / 2;
			row_alpha = 128;
		}

		Con_DrawString( col_name_x, cur_y + 2, name, name_color );

		// HP display after player name (like CS 1.6 PC)
		{
			int hp = 0;

			// For local player: always read from cl.local.health
			if( pidx == cl.playernum )
			{
				hp = cl.local.health;
				slayer_scores[pidx].health = hp;
			}
			else
			{
				hp = slayer_scores[pidx].health;
			}

			if( hp > 0 )
			{
				int name_w, name_h;
				char hp_buf[16];
				rgba_t hp_color;

				Con_DrawStringLen( name, &name_w, &name_h );
				Q_snprintf( hp_buf, sizeof( hp_buf ), "(%d)", hp );

				// Slightly dimmer than name color
				MakeRGBA( hp_color, name_color[0] * 180 / 255, name_color[1] * 180 / 255, name_color[2] * 180 / 255, name_color[3] * 180 / 255 );
				Con_DrawString( col_name_x + name_w + 6, cur_y + 2, hp_buf, hp_color );
			}
		}

		// Frags
		{
			rgba_t stat_color;
			MakeRGBA( stat_color, color_text[0], color_text[1], color_text[2], row_alpha );
			Q_snprintf( buf, sizeof( buf ), "%d", slayer_scores[pidx].frags );
			Con_DrawString( col_frags_x, cur_y + 2, buf, stat_color );

			// Deaths
			Q_snprintf( buf, sizeof( buf ), "%d", slayer_scores[pidx].deaths );
			Con_DrawString( col_deaths_x, cur_y + 2, buf, stat_color );

			// Ping
			Q_snprintf( buf, sizeof( buf ), "%d", cl.players[pidx].ping );
			Con_DrawString( col_ping_x, cur_y + 2, buf, stat_color );
		}

		cur_y += row_h;
	}
}
