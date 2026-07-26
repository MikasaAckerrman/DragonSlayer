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
#include <time.h>
#include "common.h"
#include "client.h"
#include "cl_scoreboard_slayer.h"
#include "cl_avatar_download.h"
#include "cl_steam_api.h"
#include "cl_steam_login.h"
#include "cl_slayer_log.h"
#include "cl_slayer_toast.h"
#include <math.h>

#if XASH_ANDROID
#include <android/log.h>
#endif

// ===========================================================================
// Cvars
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_scoreboard, "1", FCVAR_ARCHIVE, "Slayer3D: enable custom scoreboard (0 = disabled)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_bg_color, "0 0 0 120", FCVAR_ARCHIVE, "Slayer3D: scoreboard background RGBA (near-transparent like PC ScorePanel)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_text_color, "255 255 255 255", FCVAR_ARCHIVE, "Slayer3D: scoreboard text RGBA" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_ct_color, "153 204 255", FCVAR_ARCHIVE, "Slayer3D: CT team RGB (PC CS blue)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_t_color, "255 63 63", FCVAR_ARCHIVE, "Slayer3D: T team RGB (PC CS red)" );
// Border alpha lowered from 200 -> 150 (lighter visual weight, less stair-stepping)
static CVAR_DEFINE_AUTO( slayer_scoreboard_border_color, "235 231 197 95", FCVAR_ARCHIVE, "Slayer3D: scoreboard border RGBA (BaseText, low alpha)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_opacity, "220", FCVAR_ARCHIVE, "Slayer3D: overall scoreboard opacity (0-255)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_width, "0.84", FCVAR_ARCHIVE, "Slayer3D: scoreboard width as a fraction of screen width (0.50-0.99; PC reference is 0.843)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_avatar, "3.0", FCVAR_ARCHIVE, "Slayer3D: avatar icon size as a multiple of the font glyph height" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_rowscale, "1.15", FCVAR_ARCHIVE, "Slayer3D: scoreboard cell height multiplier (1.0-2.0; PC reference works out to ~1.3)" );
static CVAR_DEFINE_AUTO( slayer_avatar_maxage, "24", FCVAR_ARCHIVE, "Slayer3D: hours before a cached Steam avatar is re-downloaded (0 = never expire)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_ondeath, "1", FCVAR_ARCHIVE, "Slayer3D: show the scoreboard automatically while dead (0 = only when held)" );

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
	byte flags;      // bit0=dead(1) bit1=bomb(2) bit2=vip(4) bit3=defuser(8)  [VERIFY on live server]
	byte connected;  // set when ScoreInfo received for this slot
} slayer_score_t;

// ===========================================================================
// Static state
// ===========================================================================

static slayer_score_t  slayer_scores[MAX_CLIENTS];
static qboolean        slayer_scoreboard_active = false;
static qboolean        slayer_death_dismissed = false;  // hid the death auto-show until respawn/re-press

// Avatar state: SteamID64 per player slot and cached texture handles
static uint64_t        slayer_steamid64[MAX_CLIENTS];
static int             slayer_avatar_tex[MAX_CLIENTS]; // 0 = not tried, >0 = loaded, -1 = failed
static double          slayer_status_next_time;       // throttle: next allowed "status" send
static double          slayer_status_deadline;        // until: parse # lines from svc_print
static qboolean        slayer_status_pending;          // true while we expect status reply
static int             slayer_steam_reject_count;     // debounce: non-STEAM lines logged per session (reset on map change)

// Ping "hold-last-good" cache: cl.players[].ping drops to 0 on transient
// snapshots and when the board is (re)opened cold. Keep the last non-zero
// value briefly so the Latency column doesn't flicker to "-".
static int             slayer_ping_cache[MAX_CLIENTS];
static double          slayer_ping_cache_time[MAX_CLIENTS];

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
// (the top-left of the bounding board). The 10 entries trace the OUTERMOST
// pixel of the staircase BG contour for one corner, with NO overlap at the
// step elbows. Slayer_DrawBorderCorner() mirrors this set across X and/or Y
// to produce all four corners. The four "stretchy" rects (top cap, bottom
// cap, left/right body walls) are emitted directly in Slayer_Scoreboard_Draw.
//
// Total border draw calls: 4 corners * 10 + 4 stretchy = 44.

typedef struct
{
	int x, y, w, h;
} slayer_border_seg_t;

static const slayer_border_seg_t slayer_border_corner_segs[10] =
{
	{ 16,  1, 1, 3 }, // strip 0 left wall, 3px tall (y=0 row is owned by top cap)
	{ 12,  4, 4, 1 }, // strip 1 shoulder cap, 4px wide
	{ 12,  5, 1, 3 }, // strip 1 left wall, 3px tall
	{  8,  8, 4, 1 }, // strip 2 shoulder cap
	{  8,  9, 1, 3 }, // strip 2 left wall
	{  4, 12, 4, 1 }, // strip 3 shoulder cap
	{  4, 13, 1, 3 }, // strip 3 left wall
	{  2, 16, 2, 1 }, // strip 4 shoulder cap, 2px wide
	{  2, 17, 1, 3 }, // strip 4 left wall
	{  0, 20, 2, 1 }, // body-top shoulder, 2px wide (body wall takes y=21..h-22)
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
#if XASH_ANDROID
			__android_log_print( ANDROID_LOG_DEBUG, "Xash",
				"Slayer: status line slot=%d has no real STEAM_ id (got '%s'), no avatar (skip %d/8)",
				slot, prefix, slayer_steam_reject_count );
#endif
			Con_DPrintf( "Slayer: status line slot=%d has no real STEAM_ id (got '%s'), no avatar (skip %d/8)\n",
				slot, prefix, slayer_steam_reject_count );
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

	Slayer_Log_Printf( "status: slot %d -> SteamID %" PRIu64 " (name '%s')",
		slot, steamid64, ( i < MAX_CLIENTS ) ? cl.players[i].name : "?" );
	Con_Printf( "Slayer: parsed steamid %"PRIu64" for slot %d\n", steamid64, slot );
#if XASH_ANDROID
	__android_log_print( ANDROID_LOG_INFO, "Xash",
		"Slayer: parsed steamid %"PRIu64" for slot %d", steamid64, slot );
#endif

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

	if( !FS_FileExists( path, false ) )
	{
		// Request automatic download
		Slayer_Log_Printf( "avatar slot %d SteamID %" PRIu64 ": not cached -> request download",
			slot, slayer_steamid64[slot] );
		Slayer_AvatarDownload_Request( slayer_steamid64[slot], slot );
		slayer_avatar_tex[slot] = -1;
		return;
	}

	// Expire stale cache entries. Without this a player who changes their Steam
	// picture keeps showing the old one forever, because the PNG is only ever
	// fetched when it is missing. slayer_avatar_maxage is in hours; 0 disables.
	if( slayer_avatar_maxage.value > 0.0f )
	{
		int    ftime = FS_FileTime( path, false );
		double age_h = ( ftime > 0 ) ? ( time( NULL ) - (time_t)ftime ) / 3600.0 : 0.0;

		if( ftime > 0 && age_h > slayer_avatar_maxage.value )
		{
			Slayer_Log_Printf( "avatar slot %d SteamID %" PRIu64 ": cache %.1fh old -> refresh",
				slot, slayer_steamid64[slot], age_h );
			FS_Delete( path );
			Slayer_AvatarDownload_Request( slayer_steamid64[slot], slot );
			slayer_avatar_tex[slot] = -1;
			return;
		}
	}

	texid = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, TF_IMAGE );
	Slayer_Log_Printf( "avatar slot %d SteamID %" PRIu64 ": cached file '%s' -> texid %d",
		slot, slayer_steamid64[slot], path, texid );

	if( texid == 0 )
	{
		// Bad cached file (e.g. legacy raw-JPEG written as .png by an older
		// build). Delete it and reset the slot to 0 so the next call falls
		// through to the FS_FileExists==false branch above and re-queues a
		// fresh download via Slayer_AvatarDownload_Request.
		FS_Delete( path );
		slayer_avatar_tex[slot] = 0;
		Con_Printf( S_WARN "Slayer: avatar load failed for steamid=%" PRIu64 " path=%s, cache invalidated\n",
			slayer_steamid64[slot], path );
#if XASH_ANDROID
		__android_log_print( ANDROID_LOG_ERROR, "Xash",
			"Slayer: avatar load failed for steamid=%" PRIu64 " path=%s, cache invalidated",
			slayer_steamid64[slot], path );
#endif
		return;
	}

	slayer_avatar_tex[slot] = texid;
	Con_Printf( "Slayer: avatar loaded for steamid=%" PRIu64 " texid=%d path=%s\n",
		slayer_steamid64[slot], texid, path );
#if XASH_ANDROID
	__android_log_print( ANDROID_LOG_INFO, "Xash",
		"Slayer: avatar loaded for steamid=%" PRIu64 " texid=%d path=%s",
		slayer_steamid64[slot], texid, path );
#endif
}

// ===========================================================================
// Console commands
// ===========================================================================

// Drop every cached avatar PNG we know a SteamID for and re-request it. Use
// this after changing your Steam picture instead of waiting out
// slayer_avatar_maxage, or clearing app data.
static void Cmd_AvatarRefresh_f( void )
{
	uint64_t myid = Slayer_SteamLogin_GetLocalID();
	int      i, count = 0;

	for( i = 0; i < MAX_CLIENTS; i++ )
	{
		char path[128];

		if( slayer_steamid64[i] == 0 )
			continue;

		Q_snprintf( path, sizeof( path ), "avatars/%"PRIu64".png", slayer_steamid64[i] );
		FS_Delete( path );
		slayer_avatar_tex[i] = 0;   // 0 = untried, so the next draw re-requests
		count++;
	}

	// Our own picture may not be in a player slot yet (e.g. sitting in the menu).
	if( myid != 0 )
	{
		char path[128];

		Q_snprintf( path, sizeof( path ), "avatars/%"PRIu64".png", myid );
		if( FS_FileExists( path, false ))
		{
			FS_Delete( path );
			count++;
		}
	}

	Con_Printf( "Slayer3D: dropped %d cached avatar(s); they will re-download.\n", count );
}

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
	slayer_death_dismissed = false;   // explicit open re-arms the death view

	// Request status to get SteamIDs (throttled to once per 30 seconds).
	if( host.realtime >= slayer_status_next_time )
	{
		Cbuf_AddText( "status\n" );
		slayer_status_next_time = host.realtime + 30.0;
		slayer_status_pending = true;
		// Parse window == throttle interval: on slow mobile networks the
		// server's status reply can arrive many seconds late, and a short 5s
		// window closed before it landed — so SteamIDs (and therefore avatars)
		// were never parsed. Keep it open until the next allowed request.
		slayer_status_deadline = host.realtime + 30.0;
		slayer_steam_reject_count = 0; // reset debounce per request
		Slayer_Log_Printf( "status request queued (parse window 30s)" );
#if XASH_ANDROID
		__android_log_print( ANDROID_LOG_INFO, "Xash",
			"Slayer SB: status request queued, parse window 30s" );
#endif
		Con_DPrintf( "Slayer SB: status request queued, parse window 5s\n" );
	}

	// Trigger batch avatar fetch via Steam Web API (if API key is set)
	Slayer_SteamAPI_RequestBatch( slayer_steamid64, MAX_CLIENTS );
}

static void Cmd_ScoreboardUp_f( void )
{
	slayer_scoreboard_active = false;
	slayer_death_dismissed = true;   // releasing while dead hides the auto-show
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
	Cvar_RegisterVariable( &slayer_scoreboard_ondeath );
	Cvar_RegisterVariable( &slayer_scoreboard_width );
	Cvar_RegisterVariable( &slayer_scoreboard_avatar );
	Cvar_RegisterVariable( &slayer_scoreboard_rowscale );
	Cvar_RegisterVariable( &slayer_avatar_maxage );

	Cmd_AddCommand( "+slayer_scoreboard", Cmd_ScoreboardDown_f,
		"show Slayer3D custom scoreboard" );
	Cmd_AddCommand( "-slayer_scoreboard", Cmd_ScoreboardUp_f,
		"hide Slayer3D custom scoreboard" );
	Cmd_AddCommand( "slayer_avatar_urls", Cmd_AvatarUrls_f,
		"print Steam avatar download URLs for all players" );
	Cmd_AddCommand( "slayer_avatar_refresh", Cmd_AvatarRefresh_f,
		"drop cached Steam avatars and fetch them again" );

	Slayer_Log_Init();
	Slayer_Toast_Init();
	Slayer_AvatarDownload_Init();
	Slayer_SteamAPI_Init();
	Slayer_SteamLogin_Init();

	Slayer_Log_Printf( "=== scoreboard init; local SteamID=%" PRIu64 " ===",
		Slayer_SteamLogin_GetLocalID() );
	Con_Printf( "Slayer3D: scoreboard initialized\n" );
}

void Slayer_Scoreboard_Reset( void )
{
	memset( slayer_scores, 0, sizeof( slayer_scores ) );
	memset( slayer_steamid64, 0, sizeof( slayer_steamid64 ) );
	memset( slayer_avatar_tex, 0, sizeof( slayer_avatar_tex ) );
	memset( slayer_ping_cache, 0, sizeof( slayer_ping_cache ) );
	memset( slayer_ping_cache_time, 0, sizeof( slayer_ping_cache_time ) );
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

// Draw a proportional string right-aligned so its RIGHT edge sits at right_x.
// Used for the numeric columns (HP/Score/Deaths/Latency) so digits line up
// regardless of 1/2/3-digit values, matching PC CS 1.6.
static void Slayer_DrawStringRight( cl_font_t *font, int right_x, int y, const char *s, const rgba_t color )
{
	int w = 0, h = 0;

	if( !font || !s )
		return;

	CL_DrawStringLen( font, s, &w, &h, FONT_DRAW_UTF8 );
	CL_DrawString( (float)( right_x - w ), (float)y, s, color, font, FONT_DRAW_UTF8 );
}

// Draw the scoreboard panel: a near-transparent fill with anti-aliased raster
// rounded corners and a beveled 1px border. GoldSrc has only FillRGBA (which
// takes an alpha), so a smooth corner is a per-pixel signed-distance composite
// of the border stroke over the fill — the staircase dissolves into the alpha.
// bg/border already carry their final alpha (global opacity pre-applied).
static void Slayer_DrawRoundedPanel( int x, int y, int w, int h, int R,
	const rgba_t bg, const rgba_t border )
{
	const float EDGE   = (float)R - 0.5f;
	const float STROKE = 1.25f;
	const byte  bR = border[0], bG = border[1], bB = border[2];
	const float bgA  = bg[3] / 255.0f;
	const float brA  = border[3] / 255.0f;
	const float bevA = brA * 0.47f;
	const byte  bevAA = (byte)( border[3] * 0.47f );
	int py, xx;

	if( R <= 0 )
	{
		Slayer_DrawRect( x, y, w, h, bg[0], bg[1], bg[2], bg[3] );
		Slayer_DrawRect( x, y, w, 1, bR, bG, bB, border[3] );
		Slayer_DrawRect( x, y + h - 1, w, 1, bR, bG, bB, border[3] );
		Slayer_DrawRect( x, y, 1, h, bR, bG, bB, border[3] );
		Slayer_DrawRect( x + w - 1, y, 1, h, bR, bG, bB, border[3] );
		return;
	}

	for( py = 0; py < R; py++ )
	{
		float cy = (float)py + 0.5f - (float)R;
		int   yT = y + py, yB = y + h - 1 - py;
		int   xIn = R;

		for( xx = 0; xx < R; xx++ )
		{
			float cx = (float)xx + 0.5f - (float)R;
			if( sqrt( cx * cx + cy * cy ) <= EDGE - STROKE - 2.4f )
			{
				xIn = xx;
				break;
			}
		}

		// solid fill span for these two mirrored rows
		Slayer_DrawRect( x + xIn, yT, w - 2 * xIn, 1, bg[0], bg[1], bg[2], bg[3] );
		Slayer_DrawRect( x + xIn, yB, w - 2 * xIn, 1, bg[0], bg[1], bg[2], bg[3] );

		for( xx = 0; xx < xIn; xx++ )
		{
			float cx = (float)xx + 0.5f - (float)R;
			float d = (float)sqrt( cx * cx + cy * cy );
			float cov = EDGE - d + 0.5f;
			float s, bev, t, a;
			byte  rr, gg, bb, aa;

			if( cov <= 0.0f ) continue;
			if( cov > 1.0f ) cov = 1.0f;

			s = 1.0f - ( EDGE - STROKE * 0.5f - d ) / STROKE;
			if( s < 0.0f ) s = 0.0f;
			if( s > 1.0f ) s = 1.0f;
			s *= cov;

			bev = 1.0f - (float)fabs( EDGE - STROKE - 1.2f - d ) / 1.4f;
			if( bev < 0.0f ) bev = 0.0f;
			if( bev > 1.0f ) bev = 1.0f;
			bev *= cov * 0.55f;

			t = s + bev;
			if( t > 1.0f ) t = 1.0f;

			a = bgA * cov * ( 1.0f - t ) + brA * s + bevA * bev;
			if( a > 1.0f ) a = 1.0f;
			aa = (byte)( a * 255.0f + 0.5f );
			if( aa < 2 ) continue;

			rr = (byte)( bR * t );
			gg = (byte)( bG * t );
			bb = (byte)( bB * t );

			Slayer_DrawRect( x + xx,         yT, 1, 1, rr, gg, bb, aa );
			Slayer_DrawRect( x + w - 1 - xx, yT, 1, 1, rr, gg, bb, aa );
			Slayer_DrawRect( x + xx,         yB, 1, 1, rr, gg, bb, aa );
			Slayer_DrawRect( x + w - 1 - xx, yB, 1, 1, rr, gg, bb, aa );
		}
	}

	// solid middle band
	Slayer_DrawRect( x, y + R, w, h - 2 * R, bg[0], bg[1], bg[2], bg[3] );

	// straight border runs: outer stroke + inner bevel
	Slayer_DrawRect( x,         y + R, 1, h - 2 * R, bR, bG, bB, border[3] );
	Slayer_DrawRect( x + w - 1, y + R, 1, h - 2 * R, bR, bG, bB, border[3] );
	Slayer_DrawRect( x + 1,     y + R, 1, h - 2 * R, bR, bG, bB, bevAA );
	Slayer_DrawRect( x + w - 2, y + R, 1, h - 2 * R, bR, bG, bB, bevAA );
	Slayer_DrawRect( x + R,     y,         w - 2 * R, 1, bR, bG, bB, border[3] );
	Slayer_DrawRect( x + R,     y + h - 1, w - 2 * R, 1, bR, bG, bB, border[3] );
	Slayer_DrawRect( x + R,     y + 1,     w - 2 * R, 1, bR, bG, bB, bevAA );
	Slayer_DrawRect( x + R,     y + h - 2, w - 2 * R, 1, bR, bG, bB, bevAA );
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
} slayer_sort_entry_t;

static int Slayer_SortCompare( const void *a, const void *b )
{
	const slayer_sort_entry_t *ea = (const slayer_sort_entry_t *)a;
	const slayer_sort_entry_t *eb = (const slayer_sort_entry_t *)b;

	// T first (1), then CT (2), then others — matches PC CS 1.6 scoreboard order
	if( ea->team_id != eb->team_id )
	{
		// T (1) before CT (2) before unassigned (0) / spectator (3)
		int order_a = ( ea->team_id == SLAYER_TEAM_T ) ? 0 :
		              ( ea->team_id == SLAYER_TEAM_CT ) ? 1 : 2;
		int order_b = ( eb->team_id == SLAYER_TEAM_T ) ? 0 :
		              ( eb->team_id == SLAYER_TEAM_CT ) ? 1 : 2;
		if( order_a != order_b )
			return order_a - order_b;
	}

	// Within a team, dead players sink to the bottom (like PC CS 1.6).
	{
		int dead_a = ( slayer_scores[ea->idx].flags & 1 ) ? 1 : 0;
		int dead_b = ( slayer_scores[eb->idx].flags & 1 ) ? 1 : 0;
		if( dead_a != dead_b )
			return dead_a - dead_b;
	}

	// Then: higher frags first
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
	int          col_money_x;       // "Деньги" column (right edge)
	int          col_kit_x;         // "Компл." (defuse kit) fixed column, left-aligned
	int          col_name_text_x;   // fixed name-column origin (after the reserved avatar gutter)
	int          text_dy;           // vertical centering offset for row text (replaces hardcoded +2)
	int          rule_h;            // team/section underline thickness (measured off the PC reference)
	int          avatar_px;         // avatar icon edge, in px — one source of truth for gutter + draw
	int          cur_y;
	int          ct_player_count = 0, t_player_count = 0, spec_player_count = 0;
	int          drawn_ct_header = 0, drawn_t_header = 0, drawn_spec_header = 0;
	const char  *hostname;
	char         buf[128];
	rgba_t       color_text, color_ct, color_t, color_spec;
	rgba_t       color_bg;
	int          global_opacity;
	cl_font_t   *font;

	// Always request the LOCAL player's own avatar by the REAL logged-in
	// SteamID (independent of the possibly-fake id we advertise to the server).
	// This makes your own icon show, and — crucially — exercises the whole
	// download path even solo (no other players needed to reproduce/diagnose).
	if( cls.state == ca_active && cl.playernum >= 0 && cl.playernum < MAX_CLIENTS )
	{
		uint64_t myid = Slayer_SteamLogin_GetLocalID();
		if( myid != 0 && slayer_steamid64[cl.playernum] != myid )
		{
			slayer_steamid64[cl.playernum] = myid;
			slayer_avatar_tex[cl.playernum] = 0;   // force (re)load
			Slayer_Log_Printf( "avatar LOCAL: requesting own avatar, SteamID %" PRIu64 " (slot %d)",
				myid, cl.playernum );
			Slayer_LoadAvatarTexture( cl.playernum );
		}
	}

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
					continue;

				texid = ref.dllFuncs.GL_LoadTexture( avpath, NULL, 0, TF_IMAGE );
				if( texid == 0 )
				{
					// Worker reported success but the file is unreadable as a PNG.
					// Wipe the bad cache and reset to 0 so the next frame
					// re-queues a fresh download instead of permanently
					// sticking on -1.
					FS_Delete( avpath );
					slayer_avatar_tex[i] = 0;
					Slayer_Log_Printf( "avatar slot %d SteamID %" PRIu64 ": downloaded file is not a valid image -> deleted",
						i, slayer_steamid64[i] );
					Con_Printf( S_WARN "Slayer: post-download avatar load failed for steamid=%" PRIu64 " path=%s, cache invalidated\n",
						slayer_steamid64[i], avpath );
#if XASH_ANDROID
					__android_log_print( ANDROID_LOG_ERROR, "Xash",
						"Slayer: post-download avatar load failed for steamid=%" PRIu64 " path=%s, cache invalidated",
						slayer_steamid64[i], avpath );
#endif
				}
				else
				{
					slayer_avatar_tex[i] = texid;
					Slayer_Log_Printf( "avatar slot %d SteamID %" PRIu64 ": DOWNLOADED + loaded, texid %d",
						i, slayer_steamid64[i], texid );
					Con_Printf( "Slayer: post-download avatar loaded for steamid=%" PRIu64 " texid=%d path=%s\n",
						slayer_steamid64[i], texid, avpath );
#if XASH_ANDROID
					__android_log_print( ANDROID_LOG_INFO, "Xash",
						"Slayer: post-download avatar loaded for steamid=%" PRIu64 " texid=%d path=%s",
						slayer_steamid64[i], texid, avpath );
#endif
				}
			}
		}
	}

	if( slayer_scoreboard.value == 0.0f )
		return;

	if( cls.state != ca_active )
		return;

	// Show automatically where CS shows its own board — end of map and while
	// dead — so ours covers the game library's stock scoreboard. We draw after
	// CL_DrawHUD so ours lands on top. The death auto-show is DISMISSIBLE: a
	// tap of the scoreboard button (release -> slayer_death_dismissed) hides it
	// until you press again or respawn, so you're never stuck staring at it.
	if( !slayer_scoreboard_active )
	{
		qboolean auto_show = cl.intermission != 0;
		qboolean dead = false;

		if( cl.playernum >= 0 && cl.playernum < MAX_CLIENTS )
		{
			int myteam = slayer_scores[cl.playernum].team_id;
			dead = ( myteam == SLAYER_TEAM_CT || myteam == SLAYER_TEAM_T )
			    && ( slayer_scores[cl.playernum].flags & 1 );
		}

		if( !dead )
			slayer_death_dismissed = false;   // rearm for the next death

		if( !auto_show && slayer_scoreboard_ondeath.value != 0.0f
		 && dead && !slayer_death_dismissed )
			auto_show = true;

		if( !auto_show )
			return;
	}

	screen_w = refState.width;
	screen_h = refState.height;

	if( screen_w <= 0 || screen_h <= 0 )
		return;

	// Get font metrics. This is only the fallback; once the player count is
	// known a resolution- and crowd-appropriate size tier is selected in the
	// compression block below, and row_h is computed there.
	font = Con_GetCurFont();
	if( !font || !font->valid )
		return;

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
			slayer_ping_cache[i] = 0;
			slayer_ping_cache_time[i] = 0.0;
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
				num_players++;
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

	// Screen-relative width, driven by a cvar so it can be tuned live instead of
	// rebuilding. The measured PC reference is 84.3% of screen width (see
	// Documentation/slayer3d/scoreboard-pc-reference.md); the default here is a
	// little wider because the phone board carries an extra avatar gutter.
	{
		float wfrac = slayer_scoreboard_width.value;
		int   min_w, max_w;

		if( wfrac < 0.50f ) wfrac = 0.50f;
		if( wfrac > 0.99f ) wfrac = 0.99f;

		board_w = (int)( screen_w * wfrac );

		min_w = (int)( screen_w * 0.50f );
		max_w = (int)( screen_w * 0.99f );
		if( board_w < min_w ) board_w = min_w;
		if( board_w > max_w ) board_w = max_w;
		if( board_w > (int)( screen_h * 2.1f ) ) board_w = (int)( screen_h * 2.1f ); // ultrawide guard
	}

	// === DIAG: build summary. This runs every frame the board is held, so it is
	// emitted ONLY when the roster actually changes — it used to flood both the
	// in-game console and logcat with an identical line per frame. ===
	{
		static int last_n = -1, last_ct = -1, last_t = -1, last_spec = -1;

		if( num_players != last_n || ct_player_count != last_ct
		 || t_player_count != last_t || spec_player_count != last_spec )
		{
			last_n = num_players; last_ct = ct_player_count;
			last_t = t_player_count; last_spec = spec_player_count;

			Con_DPrintf( "Slayer SB: built %d players (CT=%d T=%d SPEC=%d) maxclients=%d playernum=%d\n",
				num_players, ct_player_count, t_player_count, spec_player_count,
				cl.maxclients, cl.playernum );
#if XASH_ANDROID
			__android_log_print( ANDROID_LOG_INFO, "Xash",
				"Slayer SB: built %d players (CT=%d T=%d SPEC=%d) maxclients=%d playernum=%d",
				num_players, ct_player_count, t_player_count, spec_player_count,
				cl.maxclients, cl.playernum );
#endif
		}
	}

	// Layout: font-size tier + row compression + board height.
	// Preserves AUTO-EXPAND (few players => board grows with the roster) while
	// adding COMPRESSION so a FULL server still fits on a phone screen instead
	// of silently clipping the last rows (the old clamp-then-break behaviour).
	{
		int team_headers = ( ct_player_count > 0 ? 1 : 0 )
		                 + ( t_player_count > 0 ? 1 : 0 )
		                 + ( spec_player_count > 0 ? 1 : 0 );
		// content rows = players + team headers + title row + column-header row
		int content_rows = num_players + team_headers + 1;   // 1 = merged header row

		// Fixed non-row chrome. Kept GENEROUS: with tall rows (large fonts on
		// hi-dpi phones) the per-section separators/gaps add up, and an
		// under-estimate made the last row (often the lone spectator) hit the
		// height-clip break and vanish. Better a little bottom padding than a
		// dropped player.
		int chrome_h = 30 + team_headers * 16;
		int avail_h  = (int)( screen_h * 0.95f );

		int   fidx, row_pad, row_h_full, natural_h;
		float comp = 1.0f;

		// Base font size by resolution, then downshift one tier per crowd
		// threshold so tall rosters fit on short screens without crushing glyphs.
		fidx = ( screen_w >= 1280 ) ? 2 : ( screen_w >= 640 ) ? 1 : 0;
		if( content_rows > 24 && fidx > 0 ) fidx--;
		if( content_rows > 32 && fidx > 0 ) fidx--;
		{
			cl_font_t *f = Con_GetFont( fidx );
			if( f && f->valid ) font = f;
		}

		// Compact rows that hug the text. The engine font can't scale
		// continuously, so inflating rows for a sparse roster just leaves big
		// empty cells with small text (looks wrong). Instead keep row height
		// tight to the glyph — the board is content-sized, like PC — and only
		// tighten further when crowded.
		row_pad    = font->charHeight / 3;
		if( row_pad < 4 )  row_pad = 4;
		if( row_pad > 12 ) row_pad = 12;
		if( content_rows > 24 && row_pad > 4 ) row_pad = 4;
		row_h_full = font->charHeight + row_pad;

		// Cell height coefficient. On the measured PC reference the glyph fills
		// 0.58 of the row pitch; the default tiers here give ~0.75 (charHeight 30,
		// row_h 40 on a 2800x1260 panel), i.e. tighter cells than PC. This cvar
		// opens them up without touching the crowded-roster path, since the
		// compression step below still clamps everything to the available height.
		{
			float rscale = slayer_scoreboard_rowscale.value;

			if( rscale < 1.0f ) rscale = 1.0f;
			if( rscale > 2.0f ) rscale = 2.0f;
			row_h_full = (int)( row_h_full * rscale + 0.5f );
		}

		// Compression coefficient. natural_h is the auto-expand height; when it
		// fits inside avail_h, comp stays 1.0 and nothing shrinks.
		natural_h = row_h_full * content_rows + chrome_h;
		if( natural_h > avail_h && row_h_full * content_rows > 0 )
			comp = (float)( avail_h - chrome_h ) / (float)( row_h_full * content_rows );
		if( comp > 1.0f ) comp = 1.0f;

		row_h = (int)( row_h_full * comp + 0.5f );
		if( row_h < font->charHeight )
			row_h = font->charHeight;   // absolute floor: glyph exactly fills the row
		                                // (text is vertically centered via text_dy below,
		                                // so it never spills past a floored row)

		// Board height from the row height ACTUALLY used, so it stays exactly
		// consistent with the row loop's advances.
		board_h = row_h * content_rows + chrome_h;
		if( board_h > avail_h )
			board_h = avail_h;

		// The board hugs its content. There used to be a "never a tiny stamp"
		// floor of 30% of screen height, but with a small roster that floor was
		// the whole board — a couple of rows of text followed by a large empty
		// panel (e.g. board_h=638 on a 1260-tall screen for four drawn rows).
		// Floor at the chrome plus a few rows instead, so a near-empty server
		// still reads as a board without padding out dead space.
		{
			int min_h = chrome_h + row_h * 3;
			if( board_h < min_h ) board_h = min_h;
			if( board_h > avail_h ) board_h = avail_h;
		}

		// === DIAG: layout summary — emitted only when the layout actually changes.
		// It previously printed once per frame while the board was held, which is
		// what flooded the in-game console. ===
		{
			static int last_font = -1, last_row_h = -1, last_rows = -1, last_board_h = -1;

			if( fidx != last_font || row_h != last_row_h
			 || content_rows != last_rows || board_h != last_board_h )
			{
				last_font = fidx; last_row_h = row_h;
				last_rows = content_rows; last_board_h = board_h;

				Con_DPrintf( "Slayer SB: layout font=%d row_h=%d(full=%d comp=%.2f) hdr=%d rows=%d board_h=%d avail=%d screen=%dx%d\n",
					fidx, row_h, row_h_full, comp, team_headers, content_rows, board_h, avail_h, screen_w, screen_h );
#if XASH_ANDROID
				__android_log_print( ANDROID_LOG_INFO, "Xash",
					"Slayer SB: layout font=%d row_h=%d(full=%d comp=%.2f) hdr=%d rows=%d board_h=%d avail=%d screen=%dx%d",
					fidx, row_h, row_h_full, comp, team_headers, content_rows, board_h, avail_h, screen_w, screen_h );
#endif
			}
		}
	}

	// Center the board
	board_x = ( screen_w - board_w ) / 2;
	board_y = ( screen_h - board_h ) / 2;

	// Near-transparent panel with anti-aliased rounded corners (PC ScorePanel).
	{
		rgba_t panel_bg, panel_br;
		int    radius = (int)( board_h * 0.06f );   // noticeably rounded, like the user asked

		if( radius < 14 ) radius = 14;
		if( radius > 60 ) radius = 60;
		if( radius > board_w / 2 ) radius = board_w / 2;
		if( radius > board_h / 2 ) radius = board_h / 2;

		// pre-apply the global opacity so the panel helper gets final alphas
		MakeRGBA( panel_bg, color_bg[0], color_bg[1], color_bg[2],
			(byte)( color_bg[3] * global_opacity / 255 ));
		MakeRGBA( panel_br, cached_color_border[0], cached_color_border[1], cached_color_border[2],
			(byte)( cached_color_border[3] * global_opacity / 255 ));

		Slayer_DrawRoundedPanel( board_x, board_y, board_w, board_h, radius, panel_bg, panel_br );
	}

	cur_y = board_y;

	// Column layout. The name column reserves a fixed avatar gutter (so the
	// name never shifts when an avatar loads); the five right-aligned stat
	// columns (HP | Деньги | Счет | Смертей | Задержка) are placed right-to-left
	// from MEASURED header widths so the Russian labels can't collide.
	{
		int gap = font->charHeight + 4;   // ~one glyph of breathing room
		int hw, hh, av, min_health_x;

		col_name_x = board_x + (int)( board_w * 0.012f );

		// Avatar gutter. The icon fills the row, capped at a multiple of the glyph
		// height so a tall-row sparse board doesn't reserve an absurd slot next to
		// small text. The cap is a cvar because the bitmap font has only three
		// discrete sizes: when the board grows, glyphs cannot follow, and a cap
		// tied to charHeight was making the icons look ever smaller relative to
		// the board. Computed once here and reused at draw time so the gutter and
		// the icon can never disagree.
		{
			float amul = slayer_scoreboard_avatar.value;
			int   cap;

			if( amul < 1.0f ) amul = 1.0f;
			if( amul > 6.0f ) amul = 6.0f;

			cap = (int)( font->charHeight * amul );
			avatar_px = row_h - 2;
			if( avatar_px > cap ) avatar_px = cap;
			if( avatar_px < 8 ) avatar_px = 8;
		}
		av = avatar_px;
		col_name_text_x = col_name_x + av + 4;

		col_ping_x = board_x + (int)( board_w * 0.978f );   // rightmost = Задержка
		CL_DrawStringLen( font, "Задержка", &hw, &hh, FONT_DRAW_UTF8 );
		col_deaths_x = col_ping_x - hw - gap;
		CL_DrawStringLen( font, "Смертей", &hw, &hh, FONT_DRAW_UTF8 );
		col_frags_x  = col_deaths_x - hw - gap;
		CL_DrawStringLen( font, "Счет", &hw, &hh, FONT_DRAW_UTF8 );
		col_money_x  = col_frags_x - hw - gap;
		CL_DrawStringLen( font, "Деньги", &hw, &hh, FONT_DRAW_UTF8 );
		col_health_x = col_money_x - hw - gap;

		// The stat block is a fixed pixel width; on a narrow board its left
		// (HP) edge can cross into the names or off-screen. If so, shift the
		// whole block right as a unit so it stays clear of the name column.
		CL_DrawStringLen( font, "HP", &hw, &hh, FONT_DRAW_UTF8 );
		min_health_x = col_name_text_x + hw + gap;
		if( col_health_x < min_health_x )
		{
			int shift = min_health_x - col_health_x;
			col_health_x += shift; col_money_x  += shift; col_frags_x += shift;
			col_deaths_x += shift; col_ping_x   += shift;
		}

		// "Компл." sits just LEFT of the (measured, floating) HP column so it
		// can never overrun the HP digits, and is floored against the names.
		CL_DrawStringLen( font, "Компл.", &hw, &hh, FONT_DRAW_UTF8 );
		col_kit_x = col_health_x - font->charHeight * 2 - gap - hw;
		if( col_kit_x < col_name_text_x )
			col_kit_x = col_name_text_x;
	}

	// Vertical centering of per-row text inside row_h: 0 when the row is floored
	// to exactly charHeight, ~row_pad/2 at normal sizes. Replaces the old fixed
	// "+2" so text stays centered at every compression level and never spills
	// past a tightly-floored row.
	text_dy = ( row_h - font->charHeight ) / 2;
	if( text_dy < 0 ) text_dy = 0;

	// Team/section underline thickness. Measured off the PC reference: the rules
	// under "Terrorists"/"Counter-Terrorists" are 2px at 1920x1080, i.e. 0.185%
	// of screen height, so tie it to the screen rather than to the row height.
	rule_h = (int)( screen_h * 0.00185f + 0.5f );
	if( rule_h < 2 ) rule_h = 2;

	// Draw server name (left-aligned)
	hostname = Info_ValueForKey( cl.serverinfo, "hostname" );
	if( !hostname || hostname[0] == '\0' || !Q_stricmp( hostname, "empty" ) )
		hostname = Info_ValueForKey( cl.serverinfo, "name" );
	if( !hostname || hostname[0] == '\0' || !Q_stricmp( hostname, "empty" ) )
		hostname = Cvar_VariableString( "hostname" );
	if( !hostname || hostname[0] == '\0' || !Q_stricmp( hostname, "empty" ) )
		hostname = cls.servername;

	// ONE header row: server IP + map on the LEFT, column labels on the RIGHT.
	// (No separate title strip — the user asked to fold the IP/map into the
	// HP/Деньги/… band and delete the old title row above it.)
	cur_y += row_h / 3 + 4;
	{
		const char *mapname = Info_ValueForKey( cl.serverinfo, "map" );
		rgba_t color_hdr, color_map;
		int    iw, ih;

		if( !mapname || mapname[0] == '\0' )
			mapname = clgame.mapname;

		// left: IP then map, side by side with a clear gap
		CL_DrawString( col_name_x, cur_y, hostname, color_text, font, FONT_DRAW_UTF8 );
		if( mapname && mapname[0] != '\0' )
		{
			CL_DrawStringLen( font, hostname, &iw, &ih, FONT_DRAW_UTF8 );
			MakeRGBA( color_map, color_text[0] * 160 / 255, color_text[1] * 160 / 255, color_text[2] * 160 / 255, 200 );
			CL_DrawString( col_name_x + iw + font->charHeight * 2, cur_y, mapname, color_map, font, FONT_DRAW_UTF8 );
		}

		// right: column labels (soft light grey, Russian)
		MakeRGBA( color_hdr, 206, 206, 200, color_text[3] );
		Slayer_DrawStringRight( font, col_health_x, cur_y, "HP", color_hdr );
		Slayer_DrawStringRight( font, col_money_x, cur_y, "Деньги", color_hdr );
		Slayer_DrawStringRight( font, col_frags_x, cur_y, "Счет", color_hdr );
		Slayer_DrawStringRight( font, col_deaths_x, cur_y, "Смертей", color_hdr );
		Slayer_DrawStringRight( font, col_ping_x, cur_y, "Задержка", color_hdr );
	}
	cur_y += row_h;

	// Separator under the header row. Measured off the PC reference (1920x1080
	// ScorePanel): the rule runs the FULL board width (0.00%..99.94% — it is not
	// inset from the corners) and reads as a solid line, roughly twice the panel
	// luma, not a faint wash. Thickness there is 1px at 1080p; keep a 2px floor
	// so it stays visible on a high-DPI phone panel.
	{
		int sep_h = (int)( screen_h * 0.0019f ); if( sep_h < 2 ) sep_h = 2;
		Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, sep_h,
			214, 214, 208, (byte)( 200 * global_opacity / 255 ));
	}
	cur_y += 6;

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
			// === DIAG: row clipped by board height (pre-team-header). This is hit
			// every frame while the board is clipping, so report it only when the
			// clip point actually moves — it used to flood the console. ===
			{
				static int last_clip_pre = -1;

				if( row != last_clip_pre )
				{
					last_clip_pre = row;
					Con_DPrintf( "Slayer SB: height-clip break at row=%d/%d cur_y=%d board_bottom=%d (pre-hdr)\n",
						row, num_players, cur_y, board_y + board_h );
#if XASH_ANDROID
					__android_log_print( ANDROID_LOG_INFO, "Xash",
						"Slayer SB: height-clip break at row=%d/%d cur_y=%d board_bottom=%d (pre-hdr)",
						row, num_players, cur_y, board_y + board_h );
#endif
				}
			}
			break;
		}

		// Team section headers (T first, then CT, then Spectators — PC order).
		// A gap is added before every section except the first one drawn, so the
		// leading section never has an odd top gap regardless of team ordering.
		if( team == SLAYER_TEAM_T && !drawn_t_header )
		{
			drawn_t_header = 1;
			if( drawn_ct_header || drawn_spec_header ) cur_y += 4;
			Q_snprintf( buf, sizeof( buf ), "Terrorists  -  %d players", t_player_count );
			CL_DrawString( col_name_text_x, cur_y, buf, color_t, font, FONT_DRAW_UTF8 );
			cur_y += row_h;
			// Rule below the T header. Reference: 2px at 1080p (0.185% of screen
			// height) drawn SOLID — the old alpha 100 made it read as "too thin".
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, rule_h,
				color_t[0], color_t[1], color_t[2], (byte)( 210 * global_opacity / 255 ));
			cur_y += rule_h + 1;
		}
		else if( team == SLAYER_TEAM_CT && !drawn_ct_header )
		{
			drawn_ct_header = 1;
			if( drawn_t_header || drawn_spec_header ) cur_y += 4;
			Q_snprintf( buf, sizeof( buf ), "Counter-Terrorists  -  %d players", ct_player_count );
			CL_DrawString( col_name_text_x, cur_y, buf, color_ct, font, FONT_DRAW_UTF8 );
			cur_y += row_h;
			// Rule below the CT header (same measured treatment as the T rule).
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, rule_h,
				color_ct[0], color_ct[1], color_ct[2], (byte)( 210 * global_opacity / 255 ));
			cur_y += rule_h + 1;
		}
		else if( team != SLAYER_TEAM_CT && team != SLAYER_TEAM_T && !drawn_spec_header )
		{
			drawn_spec_header = 1;
			if( drawn_ct_header || drawn_t_header ) cur_y += 4;
			Q_snprintf( buf, sizeof( buf ), "Spectators  -  %d players", spec_player_count );
			CL_DrawString( col_name_text_x, cur_y, buf, color_spec, font, FONT_DRAW_UTF8 );
			cur_y += row_h;
			// Rule below the Spectator header — dimmer than the team rules, as on
			// the reference, but still solid rather than a faint wash.
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, rule_h,
				150, 150, 150, (byte)( 170 * global_opacity / 255 ));
			cur_y += rule_h + 1;
		}

		// Stop drawing if we exceed the board after team header
		if( cur_y + row_h > board_y + board_h - 4 )
		{
			// === DIAG: row clipped by board height (post-team-header). Same
			// per-frame flood as the pre-header case; report only on change. ===
			{
				static int last_clip_post = -1;

				if( row != last_clip_post )
				{
					last_clip_post = row;
					Con_DPrintf( "Slayer SB: height-clip break at row=%d/%d cur_y=%d board_bottom=%d (post-hdr)\n",
						row, num_players, cur_y, board_y + board_h );
#if XASH_ANDROID
					__android_log_print( ANDROID_LOG_INFO, "Xash",
						"Slayer SB: height-clip break at row=%d/%d cur_y=%d board_bottom=%d (post-hdr)",
						row, num_players, cur_y, board_y + board_h );
#endif
				}
			}
			break;
		}

		// No alternating stripes on PC — rows are plain text over the panel.
		// Only the LOCAL player gets a highlight bar, inset from the rounded
		// edges so it doesn't run into the corners.
		if( pidx == cl.playernum )
		{
			// span the whole row (small fixed inset off the 1px border); player
			// rows sit in the straight-side region so this never hits a corner,
			// and it clears the right-most Latency column instead of ending on it.
			Slayer_DrawRect( board_x + 3, cur_y, board_w - 6, row_h, 235, 231, 197, 34 );
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

		// Avatar (optional) is drawn inside a FIXED reserved gutter; the name
		// ALWAYS starts at col_name_text_x whether or not an avatar exists, so
		// it never jumps when a Steam avatar finishes downloading (Requirement 2).
		{
			int avatar_size = avatar_px;   // sized once during column layout
			int avatar_y = cur_y + ( row_h - avatar_size ) / 2; // vertically centered

			if( slayer_avatar_tex[pidx] > 0 && avatar_size > 0 )
			{
				ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
				ref.dllFuncs.Color4ub( 255, 255, 255, row_alpha );
				ref.dllFuncs.R_DrawStretchPic( col_name_x, avatar_y, avatar_size, avatar_size, 0, 0, 1, 1, slayer_avatar_tex[pidx] );
				ref.dllFuncs.Color4ub( 255, 255, 255, 255 ); // restore, don't rely on the next drawer
			}

			CL_DrawString( col_name_text_x, cur_y + text_dy, name, name_color, font, FONT_DRAW_UTF8 );
		}

		// "Компл." — defuse-kit marker in its own fixed column (CT only). The
		// defuser bit is 1<<3 in the ScoreAttrib flags byte; if the server does
		// not set it, nothing draws (harmless).
		if( team == SLAYER_TEAM_CT && !( slayer_scores[pidx].flags & 1 )
		 && ( slayer_scores[pidx].flags & 8 ))
		{
			rgba_t kit_color;
			MakeRGBA( kit_color, 214, 214, 208, ( row_alpha * 78 ) / 100 );
			CL_DrawString( col_kit_x, cur_y + text_dy, "Компл.", kit_color, font, FONT_DRAW_UTF8 );
		}

		// Score / Deaths / Latency (right-aligned to their column edges)
		{
			rgba_t stat_color;
			int    ping_now, ping_show;

			if( team == SLAYER_TEAM_CT )
				MakeRGBA( stat_color, color_ct[0], color_ct[1], color_ct[2], row_alpha );
			else if( team == SLAYER_TEAM_T )
				MakeRGBA( stat_color, color_t[0], color_t[1], color_t[2], row_alpha );
			else
				MakeRGBA( stat_color, color_text[0], color_text[1], color_text[2], row_alpha );

			// Score / Deaths — round stats, so only for CT/T. Spectators showed a
			// meaningless "0 0"; the PC board leaves the spectator rows blank.
			if( team == SLAYER_TEAM_CT || team == SLAYER_TEAM_T )
			{
				// Score (frags)
				Q_snprintf( buf, sizeof( buf ), "%d", slayer_scores[pidx].frags );
				Slayer_DrawStringRight( font, col_frags_x, cur_y + text_dy, buf, stat_color );

				// Deaths
				Q_snprintf( buf, sizeof( buf ), "%d", slayer_scores[pidx].deaths );
				Slayer_DrawStringRight( font, col_deaths_x, cur_y + text_dy, buf, stat_color );
			}

			// Latency (hold-last-good): cl.players[].ping drops to 0 on transient
			// snapshots and on a cold board-open; keep the last non-zero value for
			// a few seconds so the column doesn't flicker to "-".
			ping_now = cl.players[pidx].ping;
			if( ping_now > 0 )
			{
				slayer_ping_cache[pidx]      = ping_now;
				slayer_ping_cache_time[pidx] = host.realtime;
				ping_show = ping_now;
			}
			else if( slayer_ping_cache[pidx] > 0 && host.realtime - slayer_ping_cache_time[pidx] < 5.0 )
				ping_show = slayer_ping_cache[pidx];
			else
				ping_show = 0;

			if( ping_show <= 0 )
				Q_snprintf( buf, sizeof( buf ), "-" );
			else
				Q_snprintf( buf, sizeof( buf ), "%d", ping_show );
			Slayer_DrawStringRight( font, col_ping_x, cur_y + text_dy, buf, stat_color );

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
					Slayer_DrawStringRight( font, col_health_x, cur_y + text_dy, "DEAD", dead_color );
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
						Slayer_DrawStringRight( font, col_health_x, cur_y + text_dy, buf, stat_color );
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
