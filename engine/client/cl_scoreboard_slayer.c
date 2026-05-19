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
	byte flags;      // bit 0 = dead, bit 2 = has bomb
	byte connected;  // set when ScoreInfo received for this slot
} slayer_score_t;

// ===========================================================================
// Static state
// ===========================================================================

static slayer_score_t slayer_scores[MAX_CLIENTS];
static qboolean       slayer_scoreboard_active = false;

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
// Public API - Init / Reset
// ===========================================================================

void Slayer_Scoreboard_Init( void )
{
	Cvar_RegisterVariable( &slayer_scoreboard );
	Cmd_AddCommand( "+slayer_scoreboard", Cmd_ScoreboardDown_f,
		"show Slayer3D custom scoreboard" );
	Cmd_AddCommand( "-slayer_scoreboard", Cmd_ScoreboardUp_f,
		"hide Slayer3D custom scoreboard" );
}

void Slayer_Scoreboard_Reset( void )
{
	memset( slayer_scores, 0, sizeof( slayer_scores ) );
	slayer_scoreboard_active = false;
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
	int          header_h, row_h, col_name_x, col_frags_x, col_deaths_x, col_ping_x;
	int          text_w, text_h;
	int          cur_y;
	int          ct_frags_sum = 0, t_frags_sum = 0;
	int          drawn_ct_header = 0, drawn_t_header = 0, drawn_spec_header = 0;
	const char  *hostname;
	char         buf[128];
	rgba_t       color_white, color_ct, color_t, color_spec, color_header;
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

	// Count active players (must have a name and have received ScoreInfo)
	for( i = 0; i < cl.maxclients && i < MAX_CLIENTS; i++ )
	{
		if( cl.players[i].name[0] == '\0' )
			continue;

		if( !slayer_scores[i].connected )
			continue;

		sorted[num_players].idx     = i;
		sorted[num_players].team_id = slayer_scores[i].team_id;
		sorted[num_players].frags   = slayer_scores[i].frags;
		num_players++;
	}

	if( num_players == 0 )
		return;

	// Sort players
	qsort( sorted, num_players, sizeof( slayer_sort_entry_t ), Slayer_SortCompare );

	// Calculate team frag sums
	for( i = 0; i < num_players; i++ )
	{
		if( sorted[i].team_id == SLAYER_TEAM_CT )
			ct_frags_sum += sorted[i].frags;
		else if( sorted[i].team_id == SLAYER_TEAM_T )
			t_frags_sum += sorted[i].frags;
	}

	// Adaptive board dimensions
	// Compact mode for 1 player, expand for more
	if( num_players == 1 )
	{
		board_w = (int)( screen_w * 0.40f );
		board_h = (int)( screen_h * 0.15f );
	}
	else
	{
		float w_frac = 0.45f + 0.025f * num_players;
		float h_frac = 0.20f + 0.04f * num_players;

		if( w_frac > 0.70f ) w_frac = 0.70f;
		if( h_frac > 0.80f ) h_frac = 0.80f;

		board_w = (int)( screen_w * w_frac );
		board_h = (int)( screen_h * h_frac );
	}

	// Ensure minimum height for all rows
	{
		int min_h = row_h * ( num_players + 5 ); // rows + headers + padding
		if( board_h < min_h )
			board_h = min_h;
		if( board_h > (int)( screen_h * 0.85f ) )
			board_h = (int)( screen_h * 0.85f );
	}

	// Center the board
	board_x = ( screen_w - board_w ) / 2;
	board_y = ( screen_h - board_h ) / 2;

	// Draw background
	Slayer_DrawRect( board_x, board_y, board_w, board_h, 0, 0, 0, 180 );

	// Setup colors
	MakeRGBA( color_white, 255, 255, 255, 255 );
	MakeRGBA( color_ct, 120, 180, 255, 255 );
	MakeRGBA( color_t, 255, 100, 80, 255 );
	MakeRGBA( color_spec, 180, 180, 180, 255 );
	MakeRGBA( color_header, 200, 200, 200, 255 );

	header_h = row_h;
	cur_y = board_y + 8;

	// Draw server name
	hostname = Info_ValueForKey( cl.serverinfo, "hostname" );
	if( !hostname || hostname[0] == '\0' )
		hostname = "Unknown Server";

	Con_DrawStringLen( hostname, &text_w, &text_h );
	Con_DrawString( board_x + ( board_w - text_w ) / 2, cur_y, hostname, color_white );
	cur_y += header_h + 4;

	// Column layout (percentage of board width)
	col_name_x   = board_x + (int)( board_w * 0.05f );
	col_frags_x  = board_x + (int)( board_w * 0.60f );
	col_deaths_x = board_x + (int)( board_w * 0.72f );
	col_ping_x   = board_x + (int)( board_w * 0.86f );

	// Draw column headers
	Con_DrawString( col_name_x, cur_y, "Name", color_header );
	Con_DrawString( col_frags_x, cur_y, "K", color_header );
	Con_DrawString( col_deaths_x, cur_y, "D", color_header );
	Con_DrawString( col_ping_x, cur_y, "Ping", color_header );
	cur_y += header_h + 2;

	// Draw separator line
	Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1, 100, 100, 100, 200 );
	cur_y += 4;

	// Draw player rows
	for( row = 0; row < num_players; row++ )
	{
		int     pidx = sorted[row].idx;
		int     team = sorted[row].team_id;
		rgba_t  name_color;
		const char *name;

		// Stop drawing if we exceed the board
		if( cur_y + row_h > board_y + board_h - 4 )
			break;

		// Team section headers
		if( team == SLAYER_TEAM_CT && !drawn_ct_header )
		{
			drawn_ct_header = 1;
			Q_snprintf( buf, sizeof( buf ), "Counter-Terrorists  -  %d", ct_frags_sum );
			Con_DrawString( col_name_x, cur_y, buf, color_ct );
			cur_y += header_h;
		}
		else if( team == SLAYER_TEAM_T && !drawn_t_header )
		{
			drawn_t_header = 1;
			// Add spacing between team sections
			if( drawn_ct_header )
				cur_y += 4;
			Q_snprintf( buf, sizeof( buf ), "Terrorists  -  %d", t_frags_sum );
			Con_DrawString( col_name_x, cur_y, buf, color_t );
			cur_y += header_h;
		}
		else if( team != SLAYER_TEAM_CT && team != SLAYER_TEAM_T && !drawn_spec_header )
		{
			drawn_spec_header = 1;
			// Add spacing before spectator section
			if( drawn_ct_header || drawn_t_header )
				cur_y += 4;
			Con_DrawString( col_name_x, cur_y, "Spectators", color_spec );
			cur_y += header_h;
		}

		// Stop drawing if we exceed the board after team header
		if( cur_y + row_h > board_y + board_h - 4 )
			break;

		// Highlight local player row
		if( pidx == cl.playernum )
		{
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, row_h, 255, 255, 255, 30 );
		}

		// Avatar placeholder (small filled square)
		{
			int icon_size = row_h - 4;
			byte icon_r = 128, icon_g = 128, icon_b = 128;

			if( team == SLAYER_TEAM_CT )
			{
				icon_r = 80; icon_g = 140; icon_b = 220;
			}
			else if( team == SLAYER_TEAM_T )
			{
				icon_r = 200; icon_g = 80; icon_b = 60;
			}

			Slayer_DrawRect( col_name_x, cur_y + 2, icon_size, icon_size, icon_r, icon_g, icon_b, 200 );
		}

		// Player name
		name = cl.players[pidx].name;

		if( team == SLAYER_TEAM_CT )
			MakeRGBA( name_color, 120, 180, 255, 255 );
		else if( team == SLAYER_TEAM_T )
			MakeRGBA( name_color, 255, 150, 80, 255 );
		else
			MakeRGBA( name_color, 200, 200, 200, 255 );

		// Dead players get dimmed
		if( slayer_scores[pidx].flags & 1 )
		{
			name_color[0] = name_color[0] / 2;
			name_color[1] = name_color[1] / 2;
			name_color[2] = name_color[2] / 2;
		}

		Con_DrawString( col_name_x + row_h, cur_y, name, name_color );

		// Frags
		Q_snprintf( buf, sizeof( buf ), "%d", slayer_scores[pidx].frags );
		Con_DrawString( col_frags_x, cur_y, buf, color_white );

		// Deaths
		Q_snprintf( buf, sizeof( buf ), "%d", slayer_scores[pidx].deaths );
		Con_DrawString( col_deaths_x, cur_y, buf, color_white );

		// Ping
		Q_snprintf( buf, sizeof( buf ), "%d", cl.players[pidx].ping );
		Con_DrawString( col_ping_x, cur_y, buf, color_white );

		cur_y += row_h;
	}
}
