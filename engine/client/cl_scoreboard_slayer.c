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
#include "cl_steam_api.h"
#include "cl_steam_login.h"

#if XASH_ANDROID
#include <android/log.h>
#endif

// ===========================================================================
// Cvars
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_scoreboard, "1", FCVAR_ARCHIVE, "Slayer3D: enable custom scoreboard (0 = disabled)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_bg_color, "20 20 20 180", FCVAR_ARCHIVE, "Slayer3D: scoreboard background RGBA" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_text_color, "255 255 255 255", FCVAR_ARCHIVE, "Slayer3D: scoreboard text RGBA" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_ct_color, "120 180 255", FCVAR_ARCHIVE, "Slayer3D: CT team RGB" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_t_color, "255 100 80", FCVAR_ARCHIVE, "Slayer3D: T team RGB" );
// Border alpha lowered from 200 -> 150 (lighter visual weight, less stair-stepping)
static CVAR_DEFINE_AUTO( slayer_scoreboard_border_color, "255 255 255 150", FCVAR_ARCHIVE, "Slayer3D: scoreboard border RGBA" );
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
static int             slayer_steam_reject_count;     // debounce: non-STEAM lines logged per session (reset on map change)

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
// Border corner template (top-left quadrant, mirrored at draw time)
// ===========================================================================
//
// Each entry = {x_off, y_off, w, h} relative to the corner's origin
// (the top-left of the bounding board). The 17 entries trace the OUTERMOST
// pixel of the quarter-circle BG contour for one corner with NO overlap at
// the step elbows. Slayer_DrawBorderCorner() mirrors this set across X and/or
// Y to produce all four corners. The four "stretchy" rects (top cap, bottom
// cap, left/right body walls) are emitted directly in Slayer_Scoreboard_Draw.
//
// BG contour: 10 strips x 2px tall with insets {14,10,7,5,4,3,2,1,1,0}
// approximating a quarter-circle (delta -4,-3,-2,-1,-1,-1,-1, 0,-1).
//
// Total border draw calls: 4 corners * 17 + 4 stretchy = 72.

typedef struct
{
	int x, y, w, h;
} slayer_border_seg_t;

static const slayer_border_seg_t slayer_border_corner_segs[17] =
{
	{ 14,  1, 1, 1 }, // strip 0 left wall (y=0 row owned by top cap)
	{ 10,  2, 4, 1 }, // shoulder 0->1 cap, 4px wide
	{ 10,  3, 1, 1 }, // strip 1 left wall
	{  7,  4, 3, 1 }, // shoulder 1->2 cap, 3px wide
	{  7,  5, 1, 1 }, // strip 2 left wall
	{  5,  6, 2, 1 }, // shoulder 2->3 cap, 2px wide
	{  5,  7, 1, 1 }, // strip 3 left wall
	{  4,  8, 1, 1 }, // shoulder 3->4 cap, 1px wide
	{  4,  9, 1, 1 }, // strip 4 left wall
	{  3, 10, 1, 1 }, // shoulder 4->5 cap
	{  3, 11, 1, 1 }, // strip 5 left wall
	{  2, 12, 1, 1 }, // shoulder 5->6 cap
	{  2, 13, 1, 1 }, // strip 6 left wall
	{  1, 14, 1, 1 }, // shoulder 6->7 cap
	{  1, 15, 1, 3 }, // strips 7+8 merged left wall (same inset 1, no shoulder)
	{  0, 18, 1, 1 }, // shoulder 8->9 cap
	{  0, 19, 1, 1 }, // strip 9 left wall (body inset 0 takes over at y=20)
};

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
	int userid;
	int steam_x, steam_y;
	unsigned int steam_z;
	uint64_t steamid64;
	int i;
	const char *p;

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

	// Format: # <slot> "<name>" <userid> STEAM_X:Y:Z ...
	p = line + 1;

	// Log every '#' line to avatars/status_log.txt for remote debugging
	{
		file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
		if( logf )
		{
			FS_Printf( logf, "[%.1f] %s\n", host.realtime, line );
			FS_Close( logf );
		}
	}

	// Skip whitespace between '#' and slot number (HLDS sends "# 1 ...")
	while( *p == ' ' || *p == '\t' )
		p++;

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

	// Skip quoted name (we don't need it, just advance past it)
	if( *p != '"' )
		return;
	p++;

	while( *p && *p != '"' )
		p++;

	if( *p != '"' )
		return;
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
	{
		// Non-STEAM row (BOT, STEAM_ID_LAN, STEAM_ID_BOT, HLTV, VALVE_ID_LAN, ...)
		// is silently rejected because there is no Steam avatar to fetch. Log
		// the first 8 rejections per session at DEBUG so the user can confirm
		// from 'adb logcat -s Xash *:D' why no avatars appear in single-player
		// or LAN games.
		if( slayer_steam_reject_count < 8 )
		{
			char prefix[17];
			int  k;
			for( k = 0; k < 16 && p[k] != '\0' && p[k] != ' ' && p[k] != '\t' && p[k] != '\r' && p[k] != '\n'; k++ )
				prefix[k] = p[k];
			prefix[k] = '\0';
			slayer_steam_reject_count++;
			// (silent — file log only)

			// Log rejection to file
			{
				file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
				if( logf )
				{
					FS_Printf( logf, "[%.1f] REJECT slot=%d got='%s' (%d/8)\n",
						host.realtime, slot, prefix, slayer_steam_reject_count );
					FS_Close( logf );
				}
			}
		}
		return;
	}
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

	// (file log only — no console spam)

	// Log to file for remote debugging
	{
		file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
		if( logf )
		{
			FS_Printf( logf, "[%.1f] PARSED slot=%d steamid=%"PRIu64"\n", host.realtime, slot, steamid64 );
			FS_Close( logf );
		}
	}

	// Load texture immediately at parse time (outside render loop)
	Slayer_LoadAvatarTexture( i );
}

static void Slayer_LoadAvatarTexture( int slot )
{
	char path[128];
	int  texid;

	if( slot < 0 || slot >= MAX_CLIENTS )
		return;

	if( slayer_steamid64[slot] == 0 )
		return;

	if( slayer_avatar_tex[slot] != 0 )
		return; // already attempted (loaded or failed)

	Q_snprintf( path, sizeof( path ), "avatars/%"PRIu64".png", slayer_steamid64[slot] );

	// (file log only — no console output)

	// Log to file for remote debugging
	{
		file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
		if( logf )
		{
			FS_Printf( logf, "[%.1f] LOAD_TEX slot=%d id=%"PRIu64" path=%s exists=%d\n",
				host.realtime, slot, slayer_steamid64[slot], path, FS_FileExists( path, false ) );
			FS_Close( logf );
		}
	}

	if( !FS_FileExists( path, false ) )
	{
		// Request automatic download
		Slayer_AvatarDownload_Request( slayer_steamid64[slot], slot );
		slayer_avatar_tex[slot] = -1;
		return;
	}

	texid = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, TF_IMAGE );

	if( texid == 0 )
	{
		// Bad cached file (e.g. legacy raw-JPEG written as .png by an older
		// build). Delete it and reset the slot to 0 so the next call falls
		// through to the FS_FileExists==false branch above and re-queues a
		// fresh download via Slayer_AvatarDownload_Request.
		FS_Delete( path );
		slayer_avatar_tex[slot] = 0;
		// (silent — file log only)
		return;
	}

	slayer_avatar_tex[slot] = texid;
	// (silent — file log only)
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
	qboolean recovery_force = false;

	slayer_scoreboard_active = true;

	// Recovery bypass: if the prefetch hook fired but no slot collected a
	// SteamID (slow mobile RTT, dropped reply, or status reached the client
	// outside the parse window), allow an immediate refetch on user demand
	// even though the 30s throttle hasn't elapsed. Without this the user is
	// stuck staring at avatar-less rows for the full throttle.
	if( host.realtime < slayer_status_next_time )
	{
		int i;
		int harvested = 0;
		for( i = 0; i < MAX_CLIENTS; i++ )
		{
			if( slayer_steamid64[i] != 0 )
				harvested++;
		}
		if( harvested == 0 )
		{
			recovery_force = true;
			// (silent — recovery bypass, file log only)
		}
	}

	// Request status to get SteamIDs (throttled to once per 30 seconds)
	if( recovery_force || host.realtime >= slayer_status_next_time )
	{
		Cbuf_AddText( "status\n" );
		slayer_status_next_time = host.realtime + 30.0;
		slayer_status_pending = true;
		// Wide parse window matched to throttle — see learning on slow
		// mobile RTT: a 5s window with a 30s throttle leaves the system
		// stuck if the reply lands at t=6s.
		slayer_status_deadline = host.realtime + 30.0;
		slayer_steam_reject_count = 0; // reset debounce per request
		// (silent — file log only)
#if XASH_ANDROID
		__android_log_print( ANDROID_LOG_INFO, "Xash",
			"Slayer SB: status request queued, parse window 30s%s",
			recovery_force ? " (recovery)" : "" );
#endif

		// Log to file
		{
			file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
			if( logf )
			{
				FS_Printf( logf, "[%.1f] STATUS_REQUEST queued%s\n",
					host.realtime, recovery_force ? " (recovery)" : "" );
				FS_Close( logf );
			}
		}
	}

	// Trigger batch avatar fetch via Steam Web API (if API key is set)
	Slayer_SteamAPI_RequestBatch( slayer_steamid64, MAX_CLIENTS );
}

static void Cmd_ScoreboardUp_f( void )
{
	slayer_scoreboard_active = false;
}

// ===========================================================================
// Public API - usercmd patching (drives svc_pings)
// ===========================================================================

// Vanilla GoldSrc emits svc_pings every snapshot only when the client's
// last usercmd has IN_SCORE set (see SV_ShouldUpdatePing in sv_client.c).
// The Slayer scoreboard is bound to +slayer_scoreboard, which never goes
// through the game DLL +showscores path, so we OR the bit in here.
void Slayer_Scoreboard_PatchUsercmd( struct usercmd_s *cmd )
{
	if( !cmd )
		return;

	if( !slayer_scoreboard_active )
		return;

	cmd->buttons |= IN_SCORE;
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
	Slayer_SteamAPI_Init();
	Slayer_SteamLogin_Init();

	// (silent)
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
	slayer_steam_reject_count = 0;

	Slayer_AvatarDownload_Reset();
	Slayer_SteamAPI_Reset();
}

void Slayer_OnHealthUpdate( int hp )
{
	if( cl.playernum >= 0 && cl.playernum < MAX_CLIENTS )
		slayer_scores[cl.playernum].health = hp;
}

void Slayer_Scoreboard_OnConnected( void )
{
	// (silent — file log only)
	// Fire a single early "status" probe to harvest SteamIDs from the server
	// before the user ever opens the scoreboard, so avatar downloads can run
	// in the background. Mirrors Cmd_ScoreboardDown_f's throttling semantics
	// (one in-flight request, 30s arming window) but does NOT activate the
	// scoreboard overlay.
	if( host.realtime >= slayer_status_next_time )
	{
		Cbuf_AddText( "status\n" );
		// Match throttle == parse window so a slow mobile reply still
		// matches the strict #N "name" STEAM_X:Y:Z format on arrival.
		slayer_status_next_time   = host.realtime + 30.0;
		slayer_status_pending     = true;
		slayer_status_deadline    = host.realtime + 30.0;
		slayer_steam_reject_count = 0;
		// (silent — file log only)
#if XASH_ANDROID
		__android_log_print( ANDROID_LOG_INFO, "Xash",
			"Slayer SB: prefetch on ca_active, parse window 30s" );
#endif

		// Log to file
		{
			file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
			if( logf )
			{
				FS_Printf( logf, "[%.1f] PREFETCH on ca_active\n", host.realtime );
				FS_Close( logf );
			}
		}
	}

	// Steam Web API batch fetch is keyed on slayer_steamid64[] which is
	// populated by Slayer_ParseStatusLine() above; defer to the first
	// scoreboard open so we don't spam the API with empty queries before
	// the status reply lands.
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

	// Clear stale health when a player switches off CT/T (e.g. moves to spec).
	// HP is only meaningful for in-round CT/T players; without this, a value
	// previously sent via HealthInfo could leak into the spectator row.
	if( team_id != SLAYER_TEAM_CT && team_id != SLAYER_TEAM_T )
		slayer_scores[slot].health = 0;
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
	int32_t health;

	// HealthInfo format (ReGameDLL): byte slot(1-based) + int32 health (WRITE_LONG)
	// Total message size = 5 bytes. Server sends -1 (0xFFFFFFFF) when HP is
	// hidden (opposing team). Reading only pbuf[1] as a single byte would
	// yield 0xFF = 255 for hidden players — the original HP 255 bug.
	if( !pbuf || iSize < 5 )
		return;

	slot = pbuf[0];
	if( slot < 1 || slot > MAX_CLIENTS )
		return;

	// Read int32 little-endian from pbuf[1..4]
	health = (int32_t)( (uint32_t)pbuf[1]
	       | ( (uint32_t)pbuf[2] << 8 )
	       | ( (uint32_t)pbuf[3] << 16 )
	       | ( (uint32_t)pbuf[4] << 24 ) );

	// Negative means hidden (server sends -1 for opposing team) — treat as 0
	if( health < 0 )
		health = 0;

	slayer_scores[slot - 1].health = health;
}

// ===========================================================================
// Drawing helpers
// ===========================================================================

static void Slayer_DrawRect( int x, int y, int w, int h, byte r, byte g, byte b, byte a )
{
	ref.dllFuncs.FillRGBA( kRenderTransTexture, x, y, w, h, r, g, b, a );
}

// Draw one rounded corner of the border by walking slayer_border_corner_segs[]
// and reflecting each segment across X/Y as requested. The corner table
// describes the top-left quadrant; all other quadrants are exact mirrors.
static void Slayer_DrawBorderCorner( int bx, int by, int bw, int bh,
	qboolean mirror_x, qboolean mirror_y, byte r, byte g, byte b, byte a )
{
	size_t i;

	for( i = 0; i < sizeof( slayer_border_corner_segs ) / sizeof( slayer_border_corner_segs[0] ); i++ )
	{
		const slayer_border_seg_t *s = &slayer_border_corner_segs[i];
		int x = mirror_x ? ( bx + bw - s->x - s->w ) : ( bx + s->x );
		int y = mirror_y ? ( by + bh - s->y - s->h ) : ( by + s->y );

		Slayer_DrawRect( x, y, s->w, s->h, r, g, b, a );
	}
}

// ===========================================================================
// Sorting
// ===========================================================================

typedef struct
{
	int idx;     // 0-based player index
	int team_id;
	int frags;
	int dead;    // 1 if dead (flags & 1), 0 if alive — dead sorts last within team
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

	// Within same team: alive players before dead ones.
	// Dead is a tail-section so frag rank is preserved among alive players,
	// matching CS 1.6 vanilla behavior of pushing corpses to the bottom.
	if( ea->dead != eb->dead )
		return ea->dead - eb->dead; // 0 (alive) sorts before 1 (dead)

	// Within same team and same alive/dead bucket: higher frags first
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
	int          text_w;
	int          cur_y;
	int          ct_player_count = 0, t_player_count = 0, spec_player_count = 0;
	int          drawn_ct_header = 0, drawn_t_header = 0, drawn_spec_header = 0;
	const char  *hostname;
	char         buf[128];
	rgba_t       color_text, color_ct, color_t, color_spec;
	rgba_t       color_bg;
	int          global_opacity;
	cl_font_t   *font;

	// Pump Steam Web API batch requests
	Slayer_SteamAPI_Frame();

	// Pump avatar downloads every frame (even when scoreboard hidden)
	if( Slayer_AvatarDownload_Frame() )
	{
		// A download completed - try to reload textures for slots that were pending
		for( i = 0; i < MAX_CLIENTS; i++ )
		{
			if( slayer_avatar_tex[i] == -1 && slayer_steamid64[i] != 0 )
			{
				char avpath[128];
				int  texid;

				Q_snprintf( avpath, sizeof( avpath ), "avatars/%" PRIu64 ".png", slayer_steamid64[i] );
				if( !FS_FileExists( avpath, false ) )
				{
				// (silent — file log handles diag)
					continue;
				}

				texid = ref.dllFuncs.GL_LoadTexture( avpath, NULL, 0, TF_IMAGE );
				if( texid == 0 )
				{
					// Worker reported success but the file is unreadable as a PNG.
					// Wipe the bad cache and reset to 0 so the next frame
					// re-queues a fresh download instead of permanently
					// sticking on -1.
					FS_Delete( avpath );
					slayer_avatar_tex[i] = 0;
				// (silent)
				}
				else
				{
					slayer_avatar_tex[i] = texid;
				// (silent)
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
			if( i == cl.playernum )
			{
				slayer_scores[i].connected = 1;
			}
			else
			{
				// Player has name but no ScoreInfo - treat as spectator
				sorted[num_players].idx     = i;
				sorted[num_players].team_id = SLAYER_TEAM_SPECTATOR;
				sorted[num_players].frags   = 0;
				sorted[num_players].dead    = 0;
				num_players++;
				continue;
			}
		}

		sorted[num_players].idx     = i;
		sorted[num_players].team_id = slayer_scores[i].team_id;
		sorted[num_players].frags   = slayer_scores[i].frags;
		sorted[num_players].dead    = ( slayer_scores[i].flags & 1 ) ? 1 : 0;
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

	// === DIAG: build summary (file-only, no console) ===

	// Height adapts to content: hostname + column-headers + populated team headers + rows + padding
	{
		// Count only team headers that will actually render (have >=1 player).
		// Old code assumed exactly 2 (CT+T); when all 3 (CT+T+SPEC) fire the
		// content under-allocated by one row and the inner height-clip break
		// silently dropped the last row. Add +60 of slack to cover the
		// cumulative top-pad / inter-team-spacing / separator pixels (was 40
		// which is too tight when 3 team headers fire).
		int team_headers = ( ct_player_count > 0 ? 1 : 0 )
		                 + ( t_player_count > 0 ? 1 : 0 )
		                 + ( spec_player_count > 0 ? 1 : 0 );
		int content_rows = num_players + 2 + team_headers;
		int min_h = row_h * content_rows + 60;
		int max_h = (int)( screen_h * 0.92f );

		board_h = min_h;
		if( board_h > max_h )
			board_h = max_h;
		if( board_h < (int)( screen_h * 0.20f ) )
			board_h = (int)( screen_h * 0.20f );

		// === DIAG: layout (file-only, no console) ===
	}

	// Center the board
	board_x = ( screen_w - board_w ) / 2;
	board_y = ( screen_h - board_h ) / 2;

	// Simulated rounded corners: 10 strips x 2px tall, quarter-circle insets
	// {14,10,7,5,4,3,2,1,1,0}. Body still inset 20 from top/bottom; strip 9
	// (inset 0) merges seamlessly with the body's top/bottom edge.
	{
		byte bg_r = color_bg[0], bg_g = color_bg[1], bg_b = color_bg[2];
		byte bg_a = (byte)( color_bg[3] * global_opacity / 255 );
		// Main body (inset 20px top/bottom)
		Slayer_DrawRect( board_x, board_y + 20, board_w, board_h - 40, bg_r, bg_g, bg_b, bg_a );
		// Top corner strips (10 x 2px, smooth quarter-circle)
		Slayer_DrawRect( board_x + 14, board_y +  0, board_w - 28, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 10, board_y +  2, board_w - 20, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  7, board_y +  4, board_w - 14, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  5, board_y +  6, board_w - 10, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  4, board_y +  8, board_w -  8, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  3, board_y + 10, board_w -  6, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  2, board_y + 12, board_w -  4, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  1, board_y + 14, board_w -  2, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  1, board_y + 16, board_w -  2, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  0, board_y + 18, board_w     , 2, bg_r, bg_g, bg_b, bg_a );
		// Bottom corner strips (mirror)
		Slayer_DrawRect( board_x +  0, board_y + board_h - 20, board_w     , 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  1, board_y + board_h - 18, board_w -  2, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  1, board_y + board_h - 16, board_w -  2, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  2, board_y + board_h - 14, board_w -  4, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  3, board_y + board_h - 12, board_w -  6, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  4, board_y + board_h - 10, board_w -  8, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  5, board_y + board_h -  8, board_w - 10, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x +  7, board_y + board_h -  6, board_w - 14, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 10, board_y + board_h -  4, board_w - 20, 2, bg_r, bg_g, bg_b, bg_a );
		Slayer_DrawRect( board_x + 14, board_y + board_h -  2, board_w - 28, 2, bg_r, bg_g, bg_b, bg_a );
	}

	// Rounded border (1px thick, follows same contour as background)
	{
		byte br_r = cached_color_border[0], br_g = cached_color_border[1];
		byte br_b = cached_color_border[2], br_a = (byte)( cached_color_border[3] * global_opacity / 255 );

		// Four staircase corners (table-driven, no overlap at the step elbows).
		Slayer_DrawBorderCorner( board_x, board_y, board_w, board_h, false, false, br_r, br_g, br_b, br_a );
		Slayer_DrawBorderCorner( board_x, board_y, board_w, board_h, true,  false, br_r, br_g, br_b, br_a );
		Slayer_DrawBorderCorner( board_x, board_y, board_w, board_h, false, true,  br_r, br_g, br_b, br_a );
		Slayer_DrawBorderCorner( board_x, board_y, board_w, board_h, true,  true,  br_r, br_g, br_b, br_a );

		// Top horizontal cap (matches strip 0 inset = 14)
		Slayer_DrawRect( board_x + 14, board_y, board_w - 28, 1, br_r, br_g, br_b, br_a );

		// Bottom horizontal cap (mirror)
		Slayer_DrawRect( board_x + 14, board_y + board_h - 1, board_w - 28, 1, br_r, br_g, br_b, br_a );

		// Body side walls. Range y=20..h-21 (h-40 tall) — strip 9 (inset 0)
		// already paints the wall at y=19 / y=h-20 via the corner template,
		// and the body has no shoulder cap to subtract since both ends at
		// inset 0 are flush with the body's left/right edges.
		Slayer_DrawRect( board_x, board_y + 20, 1, board_h - 40, br_r, br_g, br_b, br_a );
		Slayer_DrawRect( board_x + board_w - 1, board_y + 20, 1, board_h - 40, br_r, br_g, br_b, br_a );
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
			int text_h_unused;
			MakeRGBA( color_map, color_text[0] * 160 / 255, color_text[1] * 160 / 255, color_text[2] * 160 / 255, 200 );
			Con_DrawStringLen( mapname, &text_w, &text_h_unused );
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
		{
			break;
		}

		// Team section headers (full-width banner background in team color
		// + brighter accent bar below). The banner alpha is kept low (~70)
		// so the dark scoreboard bg shows through; text is drawn in pure
		// white on top for high contrast.
		if( team == SLAYER_TEAM_CT && !drawn_ct_header )
		{
			rgba_t banner_text;
			drawn_ct_header = 1;
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, row_h,
				color_ct[0], color_ct[1], color_ct[2], 70 );
			Q_snprintf( buf, sizeof( buf ), "Counter-Terrorists  -  %d", ct_player_count );
			MakeRGBA( banner_text, 255, 255, 255, 255 );
			Con_DrawString( col_name_x, cur_y + 2, buf, banner_text );
			cur_y += row_h;
			// Accent strip below CT banner
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, 1,
				color_ct[0], color_ct[1], color_ct[2], 200 );
			cur_y += 3;
		}
		else if( team == SLAYER_TEAM_T && !drawn_t_header )
		{
			rgba_t banner_text;
			drawn_t_header = 1;
			cur_y += 4; // spacing between teams
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, row_h,
				color_t[0], color_t[1], color_t[2], 70 );
			Q_snprintf( buf, sizeof( buf ), "Terrorists  -  %d", t_player_count );
			MakeRGBA( banner_text, 255, 255, 255, 255 );
			Con_DrawString( col_name_x, cur_y + 2, buf, banner_text );
			cur_y += row_h;
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, 1,
				color_t[0], color_t[1], color_t[2], 200 );
			cur_y += 3;
		}
		else if( team != SLAYER_TEAM_CT && team != SLAYER_TEAM_T && !drawn_spec_header )
		{
			rgba_t banner_text;
			drawn_spec_header = 1;
			cur_y += 4; // spacing before spectator section
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, row_h,
				140, 140, 140, 70 );
			Q_snprintf( buf, sizeof( buf ), "Spectators  -  %d", spec_player_count );
			MakeRGBA( banner_text, 230, 230, 230, 255 );
			Con_DrawString( col_name_x, cur_y + 2, buf, banner_text );
			cur_y += row_h;
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, 1, 140, 140, 140, 160 );
			cur_y += 3;
		}

		// Stop drawing if we exceed the board after team header
		if( cur_y + row_h > board_y + board_h - 4 )
		{
			break;
		}

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

		// Draw avatar if available (fixed layout: avatar slot always reserved)
		{
			int avatar_size = row_h;
			int name_x_offset = avatar_size + 4; // always offset nick, even without avatar

			if( slayer_avatar_tex[pidx] > 0 )
			{
				ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
				ref.dllFuncs.Color4ub( 255, 255, 255, row_alpha );
				ref.dllFuncs.R_DrawStretchPic( col_name_x, cur_y, avatar_size, avatar_size, 0, 0, 1, 1, slayer_avatar_tex[pidx] );
			}

			Con_DrawString( col_name_x + name_x_offset, cur_y + 2, name, name_color );
		}

		// Frags
		{
			rgba_t stat_color;

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

			// Health column - only meaningful for CT/T (active round players).
			// Spectators / unassigned: draw nothing (avoids leaking stale HP from
			// a prior team or live-spectator HealthInfo into the scoreboard).
			if( team == SLAYER_TEAM_CT || team == SLAYER_TEAM_T )
			{
				if( slayer_scores[pidx].flags & 1 )
				{
					// Dead player: show "DEAD" text in team color with alpha 200
					rgba_t dead_color;
					if( team == SLAYER_TEAM_CT )
						MakeRGBA( dead_color, color_ct[0], color_ct[1], color_ct[2], 200 );
					else
						MakeRGBA( dead_color, color_t[0], color_t[1], color_t[2], 200 );
					Con_DrawString( col_health_x, cur_y + 2, "DEAD", dead_color );
				}
				else
				{
					int hp;

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
						Q_snprintf( buf, sizeof( buf ), "%d", hp );
						Con_DrawString( col_health_x, cur_y + 2, buf, stat_color );
					}
				}
			}
		}

		cur_y += row_h;
	}
}


// ===========================================================================
// Team helper: expose team_id for a player slot (0-based)
// ===========================================================================

int Slayer_GetPlayerTeam( int slot )
{
	if( slot < 0 || slot >= MAX_CLIENTS )
		return 0;
	return slayer_scores[slot].team_id;
}
