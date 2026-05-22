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
// Rounded corner curve (20px radius quarter-circle)
// ===========================================================================
//
// slayer_corner_inset[y] = horizontal pixel inset at row y inside the corner
// box (y = 0 is the topmost row of the top corner). Sampled at pixel centers
// against a 20px circle so the staircase looks the way a hardware-AA rounded
// rect would, without per-pixel alpha. Mirrored across X for the right side
// and across Y for the bottom corners. Used by both the background fill and
// the 1px border outline so the two contours line up exactly.

#define SLAYER_CORNER_RADIUS 20

static const int slayer_corner_inset[SLAYER_CORNER_RADIUS] =
{
	16, 12, 10,  9,  7,  6,  5,  4,
	 4,  3,  2,  2,  1,  1,  1,  1,
	 0,  0,  0,  0,
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

// Throttled "status" request used by both the keybind path and the
// auto-warm-on-connect path. Sends "status" at most once every 30s and
// arms a 30s parse window in Slayer_ParseStatusLine so a slow first-spawn
// round-trip on Android cellular doesn't race the deadline. The 30s
// window is safe because Slayer_ParseStatusLine's regex (#N "name" id
// STEAM_X:Y:Z) cannot match normal svc_print chat / server-log lines —
// chat is "PlayerName: msg" (no leading '#') and server messages don't
// follow the strict quoted-name + STEAM_X:Y:Z shape.
//
// 'force' bypasses the throttle. Used as a recovery path from the
// keypress when the auto-warm reply was missed entirely (e.g. server
// dropped the request, or response arrived after a 30s window with no
// matching '#' line) so the user can refresh by re-opening the
// scoreboard instead of waiting for the throttle to expire.
static void Slayer_RequestStatus( qboolean force )
{
	if( !force && host.realtime < slayer_status_next_time )
		return;

	Cbuf_AddText( "status\n" );
	slayer_status_next_time = host.realtime + 30.0;
	slayer_status_pending = true;
	slayer_status_deadline = host.realtime + 30.0; // 30s parse window (mobile-friendly)
	slayer_steam_reject_count = 0;                 // reset debounce per request
}

static void Cmd_ScoreboardDown_f( void )
{
	qboolean force_retry = false;
	int      i, name_count = 0, steamid_count = 0;

	slayer_scoreboard_active = true;

	// Recovery: if the auto-warm parse window already closed and we still
	// have no STEAM_IDs for any named player, re-issue 'status' bypassing
	// the 30s throttle. Without this the user would be stuck without
	// avatars for the first half-minute on every fresh connect whenever
	// the auto-warm reply was lost (server dropped it, slow round-trip,
	// etc.). When at least one STEAM_ID is loaded we treat the
	// pre-warm as successful and respect the throttle.
	for( i = 0; i < cl.maxclients && i < MAX_CLIENTS; i++ )
	{
		if( cl.players[i].name[0] == '\0' )
			continue;
		name_count++;
		if( slayer_steamid64[i] != 0 )
			steamid_count++;
	}
	if( !slayer_status_pending && name_count > 0 && steamid_count == 0 )
		force_retry = true;

	// Request status to get SteamIDs (throttled to once per 30 seconds,
	// unless the recovery path above forces a re-issue).
	Slayer_RequestStatus( force_retry );

	// Trigger batch avatar fetch via Steam Web API (if API key is set)
	Slayer_SteamAPI_RequestBatch( slayer_steamid64, MAX_CLIENTS );
}

static void Cmd_ScoreboardUp_f( void )
{
	slayer_scoreboard_active = false;
}

// Pre-warm the avatar pipeline as soon as we're in-game, before the user
// ever touches the scoreboard key. Called from CL_CheckClientState in
// cl_main.c right after cls.state = ca_active. Slayer_RequestStatus is
// throttled so a quick map change / reconnect doesn't spam the server,
// and Slayer_SteamAPI_RequestBatch short-circuits cleanly when the API
// key cvar is empty or no STEAM_IDs are known yet (it gets re-issued on
// the next scoreboard open if needed).
void Slayer_Scoreboard_OnConnected( void )
{
	if( cls.state != ca_active )
		return;

	Slayer_RequestStatus( false );
	Slayer_SteamAPI_RequestBatch( slayer_steamid64, MAX_CLIENTS );
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

	Con_Printf( "Slayer3D: scoreboard initialized\n" );
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

// Fill a rounded rectangle. The two corner blocks (top + bottom, R rows each)
// are emitted as one horizontal strip per row using slayer_corner_inset[]; the
// straight body in between is one big rect. Total fill calls: 1 + 2*R = 41.
static void Slayer_DrawRoundedFill( int bx, int by, int bw, int bh,
	byte r, byte g, byte b, byte a )
{
	const int R = SLAYER_CORNER_RADIUS;
	int       y;

	if( bw <= 0 || bh <= 0 )
		return;

	// Straight body (between the two curved caps). Guarded so very small
	// boards (bh <= 2*R) don't emit a negative-height rect.
	if( bh > 2 * R )
		Slayer_DrawRect( bx, by + R, bw, bh - 2 * R, r, g, b, a );

	// Curved caps: one 1px-tall strip per row of the inset table, mirrored
	// across Y for the bottom cap. If the board is too short for two full
	// caps, clamp how many rows we emit so the top and bottom don't overlap.
	{
		int rows = R;
		if( rows > bh / 2 )
			rows = bh / 2;

		for( y = 0; y < rows; y++ )
		{
			int inset = slayer_corner_inset[y];
			int w = bw - 2 * inset;

			if( w <= 0 )
				continue;

			// Top row
			Slayer_DrawRect( bx + inset, by + y, w, 1, r, g, b, a );
			// Bottom row (mirror over Y)
			Slayer_DrawRect( bx + inset, by + bh - 1 - y, w, 1, r, g, b, a );
		}
	}
}

// Draw a 1px outline that follows the rounded-rect contour produced by
// Slayer_DrawRoundedFill. For each row of the corner block we emit the
// outermost-pixel segment of the staircase: where the inset shrinks vs the
// previous row we widen the segment to also paint the new "shoulder", which
// closes off the diagonal so there are no 1px holes between steps. The four
// straight edges (top / bottom caps and left / right body walls) are emitted
// once each at the end.
static void Slayer_DrawRoundedBorder( int bx, int by, int bw, int bh,
	byte r, byte g, byte b, byte a )
{
	const int R = SLAYER_CORNER_RADIUS;
	int       y;
	int       rows;

	if( bw <= 0 || bh <= 0 )
		return;

	rows = R;
	if( rows > bh / 2 )
		rows = bh / 2;

	for( y = 0; y < rows; y++ )
	{
		int inset_cur  = slayer_corner_inset[y];
		int inset_prev = ( y == 0 ) ? R : slayer_corner_inset[y - 1];
		int step_w     = inset_prev - inset_cur;
		int seg_w      = ( step_w > 0 ) ? step_w : 1;

		// Top-left corner shoulder + wall pixel
		Slayer_DrawRect( bx + inset_cur,                 by + y,           seg_w, 1, r, g, b, a );
		// Top-right corner (mirror over X)
		Slayer_DrawRect( bx + bw - inset_cur - seg_w,    by + y,           seg_w, 1, r, g, b, a );
		// Bottom-left corner (mirror over Y)
		Slayer_DrawRect( bx + inset_cur,                 by + bh - 1 - y,  seg_w, 1, r, g, b, a );
		// Bottom-right corner (mirror over X and Y)
		Slayer_DrawRect( bx + bw - inset_cur - seg_w,    by + bh - 1 - y,  seg_w, 1, r, g, b, a );
	}

	// Top and bottom horizontal caps: at y = 0 / y = bh-1 the corners only
	// covered x in [0, R) and [bw-R, bw). The flat span between them needs
	// its own 1px strip top and bottom.
	if( bw > 2 * R )
	{
		Slayer_DrawRect( bx + R, by,             bw - 2 * R, 1, r, g, b, a );
		Slayer_DrawRect( bx + R, by + bh - 1,    bw - 2 * R, 1, r, g, b, a );
	}

	// Left and right body walls: at y = R-1 the corner has inset = 0, so the
	// wall starts at y = R and runs until the bottom corner picks it up.
	if( bh > 2 * R )
	{
		Slayer_DrawRect( bx,             by + R, 1, bh - 2 * R, r, g, b, a );
		Slayer_DrawRect( bx + bw - 1,    by + R, 1, bh - 2 * R, r, g, b, a );
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
	int dead;    // 1 = dead within CT/T (sinks to bottom of its own team), 0 otherwise
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

	// Within the same team: alive players above dead. The 'dead' field is
	// only set for CT/T (see population in Slayer_Scoreboard_Draw), so this
	// can never push a body across a team boundary — dead CTs stay at the
	// bottom of CT, dead Ts stay at the bottom of T.
	if( ea->dead != eb->dead )
		return ea->dead - eb->dead;

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
		// 'dead' is only meaningful for CT/T (the dead flag is set/cleared per
		// round). Spectators / unassigned always sort as alive so the dead
		// bucket can never absorb a non-team player.
		sorted[num_players].dead    =
			( ( slayer_scores[i].team_id == SLAYER_TEAM_CT
			 || slayer_scores[i].team_id == SLAYER_TEAM_T )
			 && ( slayer_scores[i].flags & 1 ) ) ? 1 : 0;
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
	}

	// Center the board
	board_x = ( screen_w - board_w ) / 2;
	board_y = ( screen_h - board_h ) / 2;

	// Smooth rounded background (quarter-circle staircase, 20px radius).
	{
		byte bg_r = color_bg[0], bg_g = color_bg[1], bg_b = color_bg[2];
		byte bg_a = (byte)( color_bg[3] * global_opacity / 255 );
		Slayer_DrawRoundedFill( board_x, board_y, board_w, board_h, bg_r, bg_g, bg_b, bg_a );
	}

	// Matching 1px rounded border (same contour as the fill above).
	{
		byte br_r = cached_color_border[0], br_g = cached_color_border[1];
		byte br_b = cached_color_border[2], br_a = (byte)( cached_color_border[3] * global_opacity / 255 );
		Slayer_DrawRoundedBorder( board_x, board_y, board_w, board_h, br_r, br_g, br_b, br_a );
	}

	cur_y = board_y;

	// Column layout (percentage of board width). col_name_x is bumped right
	// so a square avatar (row_h x row_h) plus a small gap fits to the LEFT
	// of the name column without crossing the rounded board edge. The name
	// itself is drawn at col_name_x for *every* row regardless of whether
	// an avatar is present, so names line up cleanly even when some players
	// haven't broadcast a STEAM_ID.
	col_name_x = board_x + (int)( board_w * 0.04f );
	{
		int min_name_x = board_x + 8 + row_h + 4;
		if( col_name_x < min_name_x )
			col_name_x = min_name_x;
	}
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
			break;

		// Team section headers
		if( team == SLAYER_TEAM_CT && !drawn_ct_header )
		{
			drawn_ct_header = 1;
			// Tinted full-row banner behind the section title — gives every
			// team (CT / T / Spectators) the same visible "field" so the eye
			// can find section boundaries without reading the labels.
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, row_h,
				color_ct[0], color_ct[1], color_ct[2], 50 );
			Q_snprintf( buf, sizeof( buf ), "Counter-Terrorists  -  %d", ct_player_count );
			Con_DrawString( col_name_x, cur_y + 1, buf, color_ct );
			cur_y += row_h;
			// Thin separator below CT header
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1, color_ct[0], color_ct[1], color_ct[2], 140 );
			cur_y += 3;
		}
		else if( team == SLAYER_TEAM_T && !drawn_t_header )
		{
			drawn_t_header = 1;
			// Spacing between teams
			cur_y += 4;
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, row_h,
				color_t[0], color_t[1], color_t[2], 50 );
			Q_snprintf( buf, sizeof( buf ), "Terrorists  -  %d", t_player_count );
			Con_DrawString( col_name_x, cur_y + 1, buf, color_t );
			cur_y += row_h;
			// Thin separator below T header
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1, color_t[0], color_t[1], color_t[2], 140 );
			cur_y += 3;
		}
		else if( team != SLAYER_TEAM_CT && team != SLAYER_TEAM_T && !drawn_spec_header )
		{
			drawn_spec_header = 1;
			// Spacing before spectator section
			cur_y += 4;
			// Same tinted banner treatment as CT/T so the Spectators block is
			// just as findable when scanning the player list.
			Slayer_DrawRect( board_x + 2, cur_y, board_w - 4, row_h,
				color_spec[0], color_spec[1], color_spec[2], 50 );
			Q_snprintf( buf, sizeof( buf ), "Spectators  -  %d", spec_player_count );
			Con_DrawString( col_name_x, cur_y + 1, buf, color_spec );
			cur_y += row_h;
			// Thin separator below Spectator header (matched in alpha to CT/T)
			Slayer_DrawRect( board_x + 4, cur_y, board_w - 8, 1,
				color_spec[0], color_spec[1], color_spec[2], 140 );
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

		// Draw avatar (if available) in the reserved strip to the LEFT of
		// the name column. The name's X never depends on whether the row
		// has an avatar, so all names line up regardless.
		{
			if( slayer_avatar_tex[pidx] > 0 )
			{
				int avatar_x = col_name_x - row_h - 4;
				ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
				ref.dllFuncs.Color4ub( 255, 255, 255, row_alpha );
				ref.dllFuncs.R_DrawStretchPic( avatar_x, cur_y, row_h, row_h, 0, 0, 1, 1, slayer_avatar_tex[pidx] );
			}

			Con_DrawString( col_name_x, cur_y + 2, name, name_color );
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
