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
#include "cl_slayer_log.h"
#include "cl_slayer_toast.h"
#include "cl_slayer_conspy.h"           // Slayer_ConSpy_QuietStatus
#include "cl_teamcolors_slayer.h"
#include "cl_sb_scheme_slayer.h"        // colours from the game's own VGUI scheme
#include "ref_common.h"                 // R_GetTextureParms (stretched glyphs)
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
static CVAR_DEFINE_AUTO( slayer_scoreboard_border_color, "150 160 172 110", FCVAR_ARCHIVE, "Slayer3D: scoreboard border RGBA" );

// The cream tone that shipped before read as a yellow-ish frame that matched
// nothing else on screen. Default now follows hud_color (the same source the
// radar rim uses) so the board belongs to the rest of the interface; the RGBA
// cvar above supplies the alpha, and setting this to 0 restores explicit RGB.
static CVAR_DEFINE_AUTO( slayer_scoreboard_border_hud, "1", FCVAR_ARCHIVE,
	"Slayer3D: take the scoreboard border colour from hud_color (0 = use slayer_scoreboard_border_color)" );

static CVAR_DEFINE_AUTO( slayer_scoreboard_border_migrated, "0", FCVAR_ARCHIVE,
	"Slayer3D internal: scoreboard border colour migration completed" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_opacity, "220", FCVAR_ARCHIVE, "Slayer3D: overall scoreboard opacity (0-255)" );

// Corner radius as a fraction of board height. 0.022 keeps the anti-aliased
// rounding but stops the panel from reading as a lozenge on a tall phone board
// (the old hard-coded 0.06 with a 14 px floor did exactly that).
static CVAR_DEFINE_AUTO( slayer_scoreboard_corner, "0.022", FCVAR_ARCHIVE,
	"Slayer3D: scoreboard corner radius as a fraction of its height (0 = square)" );

// Per-player colour marker in the name column. DEFAULT OFF at the user's
// request: they intend to design this differently. The code and the shared
// palette stay (cl_teamcolors_slayer.c still drives the radar), so switching
// the cvar back on restores it. Kept as two cvars so the old "dot" name still
// disables it in existing configs.
static CVAR_DEFINE_AUTO( slayer_scoreboard_colordot, "0", FCVAR_ARCHIVE,
	"Slayer3D: draw the player's radar colour next to the nickname (0 = off)" );

static CVAR_DEFINE_AUTO( slayer_scoreboard_colordot_migrated, "0", FCVAR_ARCHIVE,
	"Slayer3D internal: colour marker default migration completed" );

static CVAR_DEFINE_AUTO( slayer_scoreboard_width, "0.72", FCVAR_ARCHIVE, "Slayer3D: scoreboard maximum width as a fraction of screen width (0.50-0.95)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_compact_width, "0.62", FCVAR_ARCHIVE, "Slayer3D: scoreboard width fraction for small rosters" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_corner_rows, "0.45", FCVAR_ARCHIVE, "Slayer3D: corner radius as a fraction of row height (0 = use slayer_scoreboard_corner)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_colorstripe, "4", FCVAR_ARCHIVE, "Slayer3D: colour marker width in pixels when enabled" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_namegap, "9", FCVAR_ARCHIVE, "Slayer3D: gap in pixels between the avatar block and the nickname" );

// Horizontal glyph stretch. The engine font is a bitmap atlas with three fixed
// sizes, so the only way to get the wider letterforms of the PC reference board
// without shipping a font is to widen the destination rect. 1.0 = stock.
static CVAR_DEFINE_AUTO( slayer_scoreboard_stretch, "1.18", FCVAR_ARCHIVE,
	"Slayer3D: scoreboard text horizontal stretch (1.0 = off, max 2.0)" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_avatar, "3.0", FCVAR_ARCHIVE, "Slayer3D: avatar icon size as a multiple of the font glyph height" );
static CVAR_DEFINE_AUTO( slayer_scoreboard_rowscale, "1.15", FCVAR_ARCHIVE, "Slayer3D: scoreboard cell height multiplier (1.0-2.0; PC reference works out to ~1.3)" );
static CVAR_DEFINE_AUTO( slayer_avatar_recheck, "10", FCVAR_ARCHIVE, "Slayer3D: minutes between Steam avatar change re-checks (0 = never; the check is one small XML fetch)" );
// How hard to suppress the game's own scoreboard while ours is up.
//
// Level 2 skips clgame.dllFuncs.pfnRedraw, and that single call draws the ENTIRE
// client HUD -- health, armour, ammo, money, timer, radar -- not just the stock
// board. It is therefore NOT reachable through this cvar any more, no matter what
// value an old config stores: see slayer_scoreboard_block_hud below.
static CVAR_DEFINE_AUTO( slayer_scoreboard_block_stock, "1", FCVAR_ARCHIVE, "Slayer3D: hide the game's own scoreboard while ours is up (0 = off, 1 = block VGUI [keeps the HUD])" );

// The escape hatch for a mod that draws its board straight from pfnRedraw. OFF,
// and deliberately a SEPARATE cvar rather than a higher value of the one above.
//
// WHY: migrating the archived value at init cannot work. Cvar registration runs
// inside CL_Init (host.c:1226) but `exec config.cfg` happens later (host.c:1268),
// so the config re-applies the stored 2 right after we lowered it to 1 -- which
// is exactly why the HUD still vanished after the previous "fix". Capping the
// effective level in code is order-independent and cannot be undone by a config.
static CVAR_DEFINE_AUTO( slayer_scoreboard_block_hud, "0", FCVAR_ARCHIVE,
	"Slayer3D: also skip the client HUD redraw while our scoreboard is up (hides the whole HUD; only for mods that draw their board from Redraw)" );

// Migration guard for builds before 2026-08-10. Those builds documented level
// 2 as the reliable way to hide the stock board, so archived configs kept it.
// Level 2 suppresses the entire client HUD redraw; migrate it once to level 1.
static CVAR_DEFINE_AUTO( slayer_scoreboard_block_migrated, "0", FCVAR_ARCHIVE,
	"Slayer3D internal: archived scoreboard block level migration completed" );

static CVAR_DEFINE_AUTO( slayer_scoreboard_ondeath, "1", FCVAR_ARCHIVE, "Slayer3D: show the scoreboard automatically while dead (0 = only when held)" );

// Colours from the game's own VGUI scheme. Read as DEFAULTS: a cvar that still
// holds its built-in value yields to the file, a cvar the player set wins. See
// Slayer_SB_ApplyScheme for why that comparison is against def_string.
//
// WHICH FILE. This was `resource/TrackerScheme.res` and that was the wrong file.
// TrackerScheme is the Tracker/Friends scheme -- its palette is olive (its
// "Orange" is 142 137 35, Button.BgColor 76 88 68), which is why taking colours
// from it turned the board olive and put an olive bar on the local player's row.
// The board's real scheme is `resource/ClientScheme.res`, whose entries are named
// for their purpose and whose own comment on "ListBG" reads "background of
// scoreboard": ListBG 0 0 0 128, BaseText 255 176 0, SelectionBG 10 10 10 100.
// The game's client library agrees -- the stock board fills 0 0 0 153 and prints
// 255 140 0. Amber on near-black, not olive. The parser reads both families, so
// pointing this cvar back at TrackerScheme still works.
static CVAR_DEFINE_AUTO( slayer_scoreboard_scheme, "1", FCVAR_ARCHIVE,
	"Slayer3D: take board colours from the game's VGUI scheme file (0 = cvars only)" );

static CVAR_DEFINE_AUTO( slayer_scoreboard_scheme_file, "resource/ClientScheme.res", FCVAR_ARCHIVE,
	"Slayer3D: which scheme file to read board colours from" );

// K/D instead of money. Money is the client library's business -- the engine
// never receives it, so the column could only ever have been blank -- while K/D
// is derivable from what ScoreInfo already gives us.
static CVAR_DEFINE_AUTO( slayer_scoreboard_kd, "1", FCVAR_ARCHIVE,
	"Slayer3D: show a K/D column where the (unavailable) money column used to be" );

// Highlight on the local player's own row.
//
// THREE ways to mark your own row, because the first attempt was rejected and the
// reason was instructive rather than cosmetic.
//
//   0 - nothing.
//   1 - a LINE under the row. This is what CS 1.6 actually does: the vanilla
//       board separates and marks with 1px rules (its own team headers are
//       underlined with FillRGBA(..., 1, ...) in the client library), and the
//       colour it uses for selection is SelectionBG 10 10 10 100 -- near-black and
//       almost invisible as a fill. A fill of any colour reads as "a stripe
//       painted over the board"; a line reads as "this row is mine".
//   2 - the filled bar, which is what was there before. Kept because it is one
//       cvar away and someone may prefer it.
//
// Default 1: the player asked for his row to be marked and rejected the bar, so
// the honest reading is "mark it, but the way the game does".
static CVAR_DEFINE_AUTO( slayer_scoreboard_sel_style, "1", FCVAR_ARCHIVE,
	"Slayer3D: how your own row is marked (0 = not at all, 1 = underline, 2 = filled bar)" );

// Colour of that mark. Defaults to the board's own text colour rather than to a
// palette entry: the mark belongs to the row, and a line in the row's own colour
// cannot clash with whatever scheme the player's game uses.
static CVAR_DEFINE_AUTO( slayer_scoreboard_sel_color, "255 176 0", FCVAR_ARCHIVE,
	"Slayer3D: colour of your own row's marker (amber, as in the vanilla scheme)" );

// Opacity of the marker. A LINE can be nearly opaque without burying anything,
// which is why the default is high here while the old filled bar needed to stay
// faint; style 2 scales this down itself.
static CVAR_DEFINE_AUTO( slayer_scoreboard_sel_alpha, "200", FCVAR_ARCHIVE,
	"Slayer3D: opacity of your own row's marker (0-255)" );

// Thickness of the underline, in pixels, before HUD scaling.
static CVAR_DEFINE_AUTO( slayer_scoreboard_sel_thick, "1", FCVAR_ARCHIVE,
	"Slayer3D: thickness of the underline under your own row (pixels)" );

// Empty strip above the header row, as a fraction of the row height. Was a fixed
// row_h/3 + 4, which read as wasted space on a phone-sized board.
static CVAR_DEFINE_AUTO( slayer_scoreboard_toppad, "0.25", FCVAR_ARCHIVE,
	"Slayer3D: padding above the header row, in row heights (0 = flush)" );

// Vertical breathing room per player row, as a fraction of the row height. The
// reference board separates rows by roughly a quarter of a cell; ours were flush,
// which is what made a full roster read as a solid block of text.
static CVAR_DEFINE_AUTO( slayer_scoreboard_rowgap, "0.18", FCVAR_ARCHIVE,
	"Slayer3D: gap between player rows, in row heights (0 = flush)" );
static CVAR_DEFINE_AUTO( slayer_avatar_autofetch, "1", FCVAR_ARCHIVE, "Slayer3D: fetch avatars as soon as we join a server, without waiting for the scoreboard to be opened (0 = off)" );
static CVAR_DEFINE_AUTO( slayer_avatar_autofetch_interval, "5", FCVAR_ARCHIVE, "Slayer3D: seconds between auto-fetch attempts while some players are still unresolved" );
static CVAR_DEFINE_AUTO( slayer_avatar_uploads_per_frame, "1", FCVAR_ARCHIVE, "Slayer3D: how many avatar textures may be uploaded to the GPU in one frame (1 = smoothest)" );

// How loud the avatar machinery is on the console. Default 0: a status request
// prints the whole player table, and the auto-fetch used to ask every 5s
// forever, so this alone was over half of everything on the console (measured
// with slayer_conspy: 264 of 467 lines in 132s). The log file still gets
// everything -- it costs nothing there and is where diagnostics belong.
static CVAR_DEFINE_AUTO( slayer_avatar_verbose, "0", FCVAR_ARCHIVE,
	"Slayer3D: print avatar/status diagnostics to the console (0 = log file only)" );

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
static double          slayer_avatar_next_check[MAX_CLIENTS]; // host.realtime of the next change re-check
static double          slayer_status_next_time;       // throttle: next allowed "status" send
static double          slayer_status_deadline;        // until: parse # lines from svc_print
static qboolean        slayer_status_pending;          // true while we expect status reply
static int             slayer_steam_reject_count;     // debounce: non-STEAM lines logged per session (reset on map change)

// Auto-fetch state: avatars used to be requested only when the scoreboard was
// first opened, so nothing happened until the player pressed TAB. These drive
// an unattended fetch that starts as soon as we are on a server.
static double          slayer_autofetch_next_time;    // host.realtime of the next auto-fetch attempt
static qboolean        slayer_autofetch_done;         // true once every connected player has a SteamID
static int             slayer_autofetch_tries;        // consecutive status requests with someone still unresolved
static qboolean        slayer_autofetch_gave_up;      // logged the give-up once

// Stop after this many fruitless attempts. With the doubling below that spans
// roughly 5+10+20+40+80+120... seconds, i.e. minutes of a map, which is long
// enough for a slow reply and short enough not to be noise for a whole match.
#define SLAYER_AUTOFETCH_MAX_TRIES  6

// Texture upload budget. GL_LoadTexture decodes the PNG and uploads it on the
// calling thread, so doing a whole server's worth in one frame is a visible
// hitch on join. Slots are queued here and drained a few per frame instead.
static qboolean        slayer_avatar_upload_pending[MAX_CLIENTS];

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
// Colours from the game's own VGUI scheme
// ===========================================================================
//
// A player who themed their install expects our board to match, and re-typing
// those colours as cvars by hand is not a reasonable thing to ask of them.
//
// The file to read is `resource/ClientScheme.res` -- see the note on
// slayer_scoreboard_scheme_file for why TrackerScheme.res, which we used first,
// is the wrong one (it is the Friends/Tracker scheme and its palette is olive).
// Both key families are understood, so a install that themed TrackerScheme still
// works if the cvar is pointed back at it.
//
// Read ONCE per map (the file cannot change mid-map) and used as DEFAULTS: see
// Slayer_SB_SchemeColor for the cvar-versus-file precedence and why the test is
// against def_string rather than against a hardcoded literal.
static slayer_sb_scheme_t slayer_scheme;
static qboolean           slayer_scheme_tried;

static void Slayer_SB_LoadScheme( void )
{
	byte       *buf;
	fs_offset_t len = 0;
	const char *path;
	int         got;

	slayer_scheme_tried = true;
	memset( &slayer_scheme, 0, sizeof( slayer_scheme ));

	if( slayer_scoreboard_scheme.value == 0.0f )
		return;

	path = slayer_scoreboard_scheme_file.string;
	if( COM_StringEmptyOrNULL( path ))
		return;

	buf = FS_LoadFile( path, &len, false );
	if( !buf )
	{
		Slayer_Log_Printf( "scheme: '%s' not found -- board keeps its own colours", path );
		return;
	}

	// FS_LoadFile NUL-terminates, but the parser is handed a length-independent
	// string, so make the guarantee explicit rather than assumed.
	buf[len] = '\0';

	got = Slayer_SBScheme_Parse( (const char *)buf, &slayer_scheme );
	Slayer_Log_Printf( "scheme: '%s' -> %d key(s), have=0x%02x bg=(%d %d %d %d) sel=(%d %d %d %d)",
		path, got, slayer_scheme.have,
		slayer_scheme.bg[0], slayer_scheme.bg[1], slayer_scheme.bg[2], slayer_scheme.bg[3],
		slayer_scheme.selected_bg[0], slayer_scheme.selected_bg[1],
		slayer_scheme.selected_bg[2], slayer_scheme.selected_bg[3] );

	Mem_Free( buf );
}

/*
====================
Slayer_SB_SchemeColor

Should the scheme's colour be used for this cvar, and if so, what is it?

The rule is "the file supplies DEFAULTS": a player who has never touched the cvar
gets their game's theme, and a player who set the cvar keeps their value. The
catch is that FCVAR_ARCHIVE makes those two states look identical after a
restart -- the value is simply in config.cfg either way. What distinguishes them
is `def_string`: the engine keeps the compiled-in default alongside the current
one, so "current == default" means "never configured", regardless of whether
config.cfg re-applied that same default.

Returns true and fills `out` when the scheme should win.
====================
*/
static qboolean Slayer_SB_SchemeColor( const convar_t *cv, unsigned int flag,
	const unsigned char *scheme_rgba, rgba_t out )
{
	if( slayer_scoreboard_scheme.value == 0.0f )
		return false;

	if( !( slayer_scheme.have & flag ))
		return false;

	// An explicitly configured cvar always wins over the file.
	if( cv && cv->string && cv->def_string && Q_strcmp( cv->string, cv->def_string ))
		return false;

	MakeRGBA( out, scheme_rgba[0], scheme_rgba[1], scheme_rgba[2], scheme_rgba[3] );
	return true;
}

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

	// Never let the server's advertised ID overwrite our own slot. On a
	// Steam-emulator server that ID is fabricated and has no profile (404),
	// while Slayer_SteamLogin_GetLocalID() is the real logged-in account whose
	// avatar actually loads. Without this guard the two stomp each other every
	// status reply and our own icon never settles.
	if( i == cl.playernum )
	{
		uint64_t myid = Slayer_SteamLogin_GetLocalID();
		if( myid != 0 )
		{
			Slayer_Log_Printf( "status: slot %d is us — keeping real SteamID %" PRIu64 ", ignoring advertised %" PRIu64,
				slot, myid, steamid64 );
			return;
		}
	}

	slayer_steamid64[i] = steamid64;
	slayer_avatar_tex[i] = 0; // reset so texture will be reloaded

	Slayer_Log_Printf( "status: slot %d -> SteamID %" PRIu64 " (name '%s')",
		slot, steamid64, ( i < MAX_CLIENTS ) ? cl.players[i].name : "?" );
	if( slayer_avatar_verbose.value != 0.0f )
	{
		Con_Printf( "Slayer: parsed steamid %"PRIu64" for slot %d\n", steamid64, slot );
#if XASH_ANDROID
		__android_log_print( ANDROID_LOG_INFO, "Xash",
			"Slayer: parsed steamid %"PRIu64" for slot %d", steamid64, slot );
#endif
	}

	// Queue the texture upload instead of doing it here. A status reply arrives
	// as one burst covering every player on the server, and uploading 32 PNGs
	// from inside this loop was a visible freeze on join.
	slayer_avatar_upload_pending[i] = true;
}

static void Slayer_LoadAvatarTexture( int slot )
{
	char path[128];
	int  texid;

	if( slot < 0 || slot >= MAX_CLIENTS )
		return;

	if( slayer_steamid64[slot] == 0 )
		return;

	// Periodic change re-check. A fixed cache lifetime was the wrong tool: a
	// player can change their Steam picture at any moment, so any expiry is
	// either too slow to notice it or wastefully re-downloads. Instead ask the
	// downloader on a short interval — Java compares the profile's avatar URL
	// (a content hash, so it changes only when the picture does) against the one
	// saved beside the PNG and skips the image entirely when they match. That
	// makes a re-check cost one small XML fetch. The result handler reloads the
	// texture when the file actually changed.
	if( slayer_avatar_recheck.value > 0.0f && slayer_avatar_tex[slot] > 0 )
	{
		if( host.realtime >= slayer_avatar_next_check[slot] )
		{
			slayer_avatar_next_check[slot] = host.realtime + slayer_avatar_recheck.value * 60.0;
			Slayer_AvatarDownload_Request( slayer_steamid64[slot], slot );
		}
	}

	if( slayer_avatar_tex[slot] != 0 )
		return; // already attempted (loaded or failed)

	Slayer_AvatarPath( path, sizeof( path ), slayer_steamid64[slot] );

	if( !FS_FileExists( path, false ) )
	{
		// Request automatic download
		Slayer_Log_Printf( "avatar slot %d SteamID %" PRIu64 ": not cached -> request download",
			slot, slayer_steamid64[slot] );
		Slayer_AvatarDownload_Request( slayer_steamid64[slot], slot );
		slayer_avatar_tex[slot] = -1;
		return;
	}

	// (Change detection for an already-cached avatar happens in the re-check
	// block at the top of this function, not here.)

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
	Slayer_Log_Printf( "avatar: loaded steamid=%" PRIu64 " texid=%d path=%s",
		slayer_steamid64[slot], texid, path );
	if( slayer_avatar_verbose.value != 0.0f )
	{
		Con_Printf( "Slayer: avatar loaded for steamid=%" PRIu64 " texid=%d path=%s\n",
			slayer_steamid64[slot], texid, path );
#if XASH_ANDROID
		__android_log_print( ANDROID_LOG_INFO, "Xash",
			"Slayer: avatar loaded for steamid=%" PRIu64 " texid=%d path=%s",
			slayer_steamid64[slot], texid, path );
#endif
	}
}

// ===========================================================================
// Console commands
// ===========================================================================

// Drop every cached avatar PNG we know a SteamID for and re-request it. Use
// this after changing your Steam picture instead of waiting out
// slayer_avatar_recheck to come round, or clearing app data.
static void Cmd_AvatarRefresh_f( void )
{
	uint64_t myid = Slayer_SteamLogin_GetLocalID();
	int      i, count = 0;

	for( i = 0; i < MAX_CLIENTS; i++ )
	{
		char path[128];

		if( slayer_steamid64[i] == 0 )
			continue;

		Slayer_AvatarPath( path, sizeof( path ), slayer_steamid64[i] );
		FS_Delete( path );
		slayer_avatar_tex[i] = 0;   // 0 = untried, so the next draw re-requests
		slayer_avatar_upload_pending[i] = true;
		count++;
	}

	// Our own picture may not be in a player slot yet (e.g. sitting in the menu).
	if( myid != 0 )
	{
		char path[128];

		Slayer_AvatarPath( path, sizeof( path ), myid );
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
		Con_Printf( "Place avatar images at: " SLAYER_AVATAR_DIR "/<steamid64>.png\n" );
}

// ===========================================================================
// +slayer_scoreboard / -slayer_scoreboard commands
// ===========================================================================

// How long to keep parsing '#' lines out of svc_print after asking for status.
// Kept generous on purpose: on slow mobile networks the reply can arrive many
// seconds late, and a short window closed before it landed, so SteamIDs (and
// therefore avatars) were never parsed at all.
#define SLAYER_STATUS_PARSE_WINDOW  30.0

/*
====================
Slayer_RequestStatus

Asks the server for a status listing, which is the only way we learn other
players' SteamIDs. Throttled: resend_after is how long to wait before another
request is allowed, so a caller that wants to poll quickly on join can, while
the scoreboard key stays at a polite interval. Returns true if a request went
out this call.
====================
*/
static qboolean Slayer_RequestStatus( double resend_after )
{
	if( host.realtime < slayer_status_next_time )
		return false;

	Cbuf_AddText( "status\n" );
	slayer_status_next_time = host.realtime + resend_after;
	slayer_status_pending = true;
	slayer_status_deadline = host.realtime + SLAYER_STATUS_PARSE_WINDOW;
	slayer_steam_reject_count = 0; // reset debounce per request

	// Swallow the reply's own console output. The table is printed by the server
	// command, not by us, so there is no print to remove -- only the window in
	// which one can be recognised. Kept short (a reply arrives in about a
	// second) so a `status` the player types right after is still shown.
	Slayer_ConSpy_QuietStatus( 3.0 );

	Slayer_Log_Printf( "status request queued (resend in %.0fs, parse window %.0fs)",
		resend_after, SLAYER_STATUS_PARSE_WINDOW );

	// Console only when asked: `status` itself echoes the entire player table,
	// so announcing every request on top of that is what made the console
	// unreadable. The file log above is unconditional.
	if( slayer_avatar_verbose.value != 0.0f )
	{
#if XASH_ANDROID
		__android_log_print( ANDROID_LOG_INFO, "Xash",
			"Slayer SB: status request queued, parse window %.0fs", SLAYER_STATUS_PARSE_WINDOW );
#endif
		Con_DPrintf( "Slayer SB: status request queued, parse window %.0fs\n",
			SLAYER_STATUS_PARSE_WINDOW );
	}

	return true;
}

/*
====================
Slayer_Scoreboard_AutoFetch

Resolves SteamIDs and starts avatar downloads on its own, without waiting for
the scoreboard to be opened.

Before this, the only trigger was +slayer_scoreboard, so on joining a server
nothing was fetched until the player pressed TAB — and then every avatar
arrived at once, exactly when they wanted to read the board. Now the work
happens during the walk to the first fight, and stops as soon as every
connected player is accounted for.
====================
*/
/*
====================
Slayer_PlayerIsBot

A bot has no Steam account, so asking the server for its SteamID forever is the
bug behind the console spam: the auto-fetch stops when every connected player is
resolved, and on a server with bots that condition could never be met. The log
showed `1/8 player(s) resolved` 690 times in one session.

Two signals, both required, mirroring what the CS client itself does
(cs16-client, scoreboard.cpp: PlayerInfo_ValueForKey + ping <= 5):

  * `*bot` in userinfo -- set by the server for bots. Authoritative when present,
    but yapb and some other bot managers do not set it on every build;
  * a ping of zero. Real players over the internet never sustain that, and it is
    what the vanilla scoreboard prints "BOT" on.

Requiring both is deliberate: a LAN player can show a ping of 0 for a frame, and
being wrongly classified as a bot would silently cost that player their avatar.
Being wrong the other way only costs a few extra status requests.
====================
*/
static qboolean Slayer_PlayerIsBot( int slot )
{
	const char *value;

	if( slot < 0 || slot >= MAX_CLIENTS )
		return false;
	if( !cl.players[slot].name[0] )
		return false;

	if( cl.players[slot].ping > 5 )
		return false;

	value = Info_ValueForKey( cl.players[slot].userinfo, "*bot" );
	if( value && value[0] && Q_atoi( value ) > 0 )
		return true;

	// Loopback: a listenserver's own player has no ping either, and that is us,
	// not a bot. Everyone else with a zero ping on a server we did not start is
	// treated as one -- this is the yapb case, which sets no *bot key.
	if( slot == cl.playernum )
		return false;

	return true;
}

static void Slayer_Scoreboard_AutoFetch( void )
{
	int    i, connected = 0, resolved = 0, bots = 0;
	double interval;

	if( slayer_avatar_autofetch.value == 0.0f )
		return;

	if( cls.state != ca_active )
		return;

	for( i = 0; i < MAX_CLIENTS; i++ )
	{
		if( cl.players[i].name[0] == '\0' )
			continue;

		// Bots are counted out of the target entirely: they cannot ever be
		// resolved, so including them means the work never ends.
		if( Slayer_PlayerIsBot( i ))
		{
			bots++;
			continue;
		}

		connected++;
		if( slayer_steamid64[i] != 0 )
			resolved++;
	}

	// Everyone accounted for: stop asking. Recomputed every frame rather than
	// latched, so a player joining later puts us back to work on their own.
	if( connected == 0 || resolved >= connected )
	{
		if( !slayer_autofetch_done )
		{
			slayer_autofetch_done = true;
			Slayer_Log_Printf( "autofetch: %d player(s) resolved, %d bot(s) skipped, standing down",
				resolved, bots );
		}
		return;
	}

	if( slayer_autofetch_done )
	{
		slayer_autofetch_done = false;
		// A new unresolved player is a real reason to try again: reset the
		// give-up state so the backoff starts from scratch for them.
		slayer_autofetch_tries = 0;
		slayer_autofetch_gave_up = false;
		Slayer_Log_Printf( "autofetch: %d unresolved player(s) appeared, resuming",
			connected - resolved );
	}

	if( host.realtime < slayer_autofetch_next_time )
		return;

	interval = slayer_avatar_autofetch_interval.value;
	if( interval < 1.0 )
		interval = 1.0;      // a status request per frame would flood the server
	if( interval > 60.0 )
		interval = 60.0;

	// Back off, and eventually give up. Skipping bots fixes the common case, but
	// a player can stay unresolved for reasons we cannot fix from here: a server
	// that answers `status` without SteamIDs, a proxy that rewrites it, or a
	// protected server that refuses the command. Retrying at a fixed 5s for the
	// whole map is then pure noise on both the console and the network. Each
	// attempt doubles the wait, and after SLAYER_AUTOFETCH_MAX_TRIES we stop
	// until the roster changes (which clears the counter below).
	if( slayer_autofetch_tries >= SLAYER_AUTOFETCH_MAX_TRIES )
	{
		if( !slayer_autofetch_gave_up )
		{
			slayer_autofetch_gave_up = true;
			Slayer_Log_Printf( "autofetch: giving up after %d tries, %d/%d resolved (server does not report SteamIDs?)",
				slayer_autofetch_tries, resolved, connected );
		}
		return;
	}

	{
		int shift = slayer_autofetch_tries;

		if( shift > 4 )
			shift = 4;               // cap the doubling at 16x
		interval = interval * (double)( 1 << shift );
		if( interval > 120.0 )
			interval = 120.0;
	}

	slayer_autofetch_tries++;
	slayer_autofetch_next_time = host.realtime + interval;

	Slayer_Log_Printf( "autofetch: %d/%d player(s) resolved, requesting status (try %d, next in %.0fs)",
		resolved, connected, slayer_autofetch_tries, interval );

	Slayer_RequestStatus( interval );

	// The batch path needs SteamIDs to ask about, so it only helps once status
	// has resolved at least one player.
	if( resolved > 0 )
		Slayer_SteamAPI_RequestBatch( slayer_steamid64, MAX_CLIENTS );
}

/*
====================
Slayer_DrainAvatarUploads

Uploads at most a few queued avatar textures per frame.

GL_LoadTexture decodes the PNG and uploads it on the calling thread, so pushing
a full server's worth in one frame is a visible hitch — which is what happened
when a status reply (one burst covering every player) loaded each texture as it
was parsed. Spreading them costs a few frames of a placeholder icon and keeps
the frame time flat.
====================
*/
static void Slayer_DrainAvatarUploads( void )
{
	int budget = (int)slayer_avatar_uploads_per_frame.value;
	int i;

	if( budget < 1 )
		budget = 1;

	// Re-check sweep. The change detection lives inside Slayer_LoadAvatarTexture,
	// which now only runs for queued slots, so a loaded avatar would never be
	// looked at again once auto-fetch stood down. Queue slots whose re-check is
	// due; the loader decides whether anything actually needs downloading, and
	// the sidecar comparison keeps it to one small request when nothing changed.
	if( slayer_avatar_recheck.value > 0.0f )
	{
		for( i = 0; i < MAX_CLIENTS; i++ )
		{
			if( slayer_avatar_tex[i] <= 0 || slayer_steamid64[i] == 0 )
				continue;

			if( host.realtime >= slayer_avatar_next_check[i] )
				slayer_avatar_upload_pending[i] = true;
		}
	}

	for( i = 0; i < MAX_CLIENTS && budget > 0; i++ )
	{
		if( !slayer_avatar_upload_pending[i] )
			continue;

		slayer_avatar_upload_pending[i] = false;
		Slayer_LoadAvatarTexture( i );
		budget--;
	}
}

static void Cmd_ScoreboardDown_f( void )
{
	slayer_scoreboard_active = true;
	slayer_death_dismissed = false;   // explicit open re-arms the death view

	// Opening the board is also a hint that the player wants fresh data, so ask
	// again — the throttle keeps this from hammering the server on key mashing.
	Slayer_RequestStatus( 30.0 );

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
	Cvar_RegisterVariable( &slayer_scoreboard_border_hud );
	Cvar_RegisterVariable( &slayer_scoreboard_border_migrated );
	Cvar_RegisterVariable( &slayer_scoreboard_opacity );
	Cvar_RegisterVariable( &slayer_scoreboard_corner );
	Cvar_RegisterVariable( &slayer_scoreboard_colordot );
	Cvar_RegisterVariable( &slayer_scoreboard_colordot_migrated );

	Cvar_RegisterVariable( &slayer_scoreboard_ondeath );
	Cvar_RegisterVariable( &slayer_scoreboard_scheme );
	Cvar_RegisterVariable( &slayer_scoreboard_scheme_file );
	Cvar_RegisterVariable( &slayer_scoreboard_kd );
	Cvar_RegisterVariable( &slayer_scoreboard_sel_style );
	Cvar_RegisterVariable( &slayer_scoreboard_sel_color );
	Cvar_RegisterVariable( &slayer_scoreboard_sel_alpha );
	Cvar_RegisterVariable( &slayer_scoreboard_sel_thick );
	Cvar_RegisterVariable( &slayer_scoreboard_toppad );
	Cvar_RegisterVariable( &slayer_scoreboard_rowgap );
	Cvar_RegisterVariable( &slayer_scoreboard_width );
	Cvar_RegisterVariable( &slayer_scoreboard_compact_width );
	Cvar_RegisterVariable( &slayer_scoreboard_corner_rows );
	Cvar_RegisterVariable( &slayer_scoreboard_colorstripe );
	Cvar_RegisterVariable( &slayer_scoreboard_namegap );
	Cvar_RegisterVariable( &slayer_scoreboard_stretch );
	Cvar_RegisterVariable( &slayer_scoreboard_avatar );
	Cvar_RegisterVariable( &slayer_scoreboard_rowscale );
	Cvar_RegisterVariable( &slayer_avatar_recheck );
	Cvar_RegisterVariable( &slayer_avatar_autofetch );
	Cvar_RegisterVariable( &slayer_avatar_autofetch_interval );
	Cvar_RegisterVariable( &slayer_avatar_uploads_per_frame );
	Cvar_RegisterVariable( &slayer_avatar_verbose );
	Cvar_RegisterVariable( &slayer_scoreboard_block_stock );
	Cvar_RegisterVariable( &slayer_scoreboard_block_hud );
	Cvar_RegisterVariable( &slayer_scoreboard_block_migrated );

	// `FCVAR_ARCHIVE` preserves the old value across APK updates. Users who had
	// level 2 therefore still lost health/ammo/radar despite the newer default
	// being 1. Do this once; after migration an explicit later choice is kept.
	if( slayer_scoreboard_block_migrated.value == 0.0f )
	{
		if( slayer_scoreboard_block_stock.value >= 2.0f )
		{
			Cvar_SetValue( "slayer_scoreboard_block_stock", 1.0f );
			Slayer_Log_Printf( "stock-board migration: archived level 2 -> 1; client HUD redraw restored" );
		}
		Cvar_SetValue( "slayer_scoreboard_block_migrated", 1.0f );
	}

	// Same archive problem as the block level: the old cream border string is
	// stored in the user's config and would survive the new default.
	if( slayer_scoreboard_border_migrated.value == 0.0f )
	{
		if( !Q_strcmp( slayer_scoreboard_border_color.string, "235 231 197 95" ))
			Cvar_DirectSet( &slayer_scoreboard_border_color, "150 160 172 110" );
		Cvar_SetValue( "slayer_scoreboard_border_migrated", 1.0f );
	}

	// The marker shipped enabled, so the archived config still says 1. Turn it
	// off once; an explicit later choice is preserved.
	if( slayer_scoreboard_colordot_migrated.value == 0.0f )
	{
		if( slayer_scoreboard_colordot.value != 0.0f )
			Cvar_SetValue( "slayer_scoreboard_colordot", 0.0f );
		Cvar_SetValue( "slayer_scoreboard_colordot_migrated", 1.0f );
	}

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
	// Cache location moved to downloaded/avatars/; carry over anything the
	// player already has so a rename does not cost them a full re-download.
	Slayer_AvatarDownload_MigrateCache();
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
	memset( slayer_avatar_upload_pending, 0, sizeof( slayer_avatar_upload_pending ) );
	slayer_scoreboard_active = false;
	slayer_status_pending = false;
	slayer_status_next_time = 0.0;   // allow immediate re-fetch on next connect
	slayer_status_deadline = 0.0;
	slayer_steam_reject_count = 0;
	slayer_autofetch_next_time = 0.0;   // fetch as soon as the next map is live
	slayer_autofetch_done = false;
	slayer_autofetch_tries = 0;         // a new map gets a full budget of tries
	slayer_autofetch_gave_up = false;

	// Re-read the game's scheme on every map: a server can install its own
	// resource/ files, and this is the only moment they can change.
	slayer_scheme_tried = false;
	memset( &slayer_scheme, 0, sizeof( slayer_scheme ));

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

// ---------------------------------------------------------------------------
// Horizontally stretched text
// ---------------------------------------------------------------------------
//
// The engine font is a BITMAP ATLAS with three fixed sizes (Con_GetFont 0..2),
// so "make the scoreboard font bigger" has no answer through CL_DrawString: it
// draws each glyph at font->scale and nothing else. But the PC reference board
// has noticeably WIDER letterforms than our nearest tier, and the phone board
// has horizontal room to spare.
//
// So we re-implement the glyph loop with an independent X scale: same atlas,
// same tex coords, only the destination rectangle is widened. Vertical size is
// untouched, because row height is already driven by charHeight and stretching
// vertically would collide with the row pitch.
//
// Both the DRAW and the MEASURE path must apply the same factor, otherwise
// right-aligned columns and collision avoidance would be computed against the
// unstretched width and text would overlap. That is why every scoreboard string
// goes through these two functions and never calls CL_DrawString directly.

static float Slayer_SB_XScale( void )
{
	float s = slayer_scoreboard_stretch.value;

	if( s < 1.0f ) s = 1.0f;
	if( s > 2.0f ) s = 2.0f;
	return s;
}

static void Slayer_SB_StringLen( cl_font_t *font, const char *s, int *width, int *height )
{
	int w = 0, h = 0;

	CL_DrawStringLen( font, s, &w, &h, FONT_DRAW_UTF8 );

	if( width )  *width = (int)( w * Slayer_SB_XScale() + 0.5f );
	if( height ) *height = h;
}

static int Slayer_SB_DrawString( float x, float y, const char *s, const rgba_t color, cl_font_t *font )
{
	float xs = Slayer_SB_XScale();
	float cx = x;
	rgba_t cur;
	int    texw = 0, texh = 0;

	if( !font || !font->valid || !s )
		return 0;

	// Unstretched: use the engine path so behaviour (colour codes, tab stops,
	// UTF-8 state) is exactly the stock one when the feature is off.
	if( xs <= 1.001f )
		return CL_DrawString( x, y, s, color, font, FONT_DRAW_UTF8 );

	R_GetTextureParms( &texw, &texh, font->hFontTexture );
	if( texw <= 0 || texh <= 0 )
		return CL_DrawString( x, y, s, color, font, FONT_DRAW_UTF8 );

	MakeRGBA( cur, color[0], color[1], color[2], color[3] );

	CL_SetFontRendermode( font );
	ref.dllFuncs.Color4ub( cur[0], cur[1], cur[2], cur[3] );

	Con_UtfProcessChar( 0 );   // reset the decoder, same as CL_DrawString does

	while( *s )
	{
		int      number;
		wrect_t *rc;
		float    w, h, s1, t1, s2, t2;

		// Colour codes must keep working: the nickname column relies on them.
		if( IsColorString( s ))
		{
			const byte *c = g_color_table[ColorIndex( *( s + 1 ))];

			ref.dllFuncs.Color4ub( c[0], c[1], c[2], cur[3] );
			s += 2;
			continue;
		}

		number = Con_UtfProcessChar( (byte)*s++ );
		if( !number )
			continue;
		number &= 255;

		if( number == ' ' )
		{
			cx += font->charWidths[' '] * xs;
			continue;
		}
		if( number < 32 || !font->charWidths[number] )
			continue;

		rc = &font->fontRc[number];
		s1 = (float)rc->left / texw;
		t1 = (float)rc->top / texh;
		s2 = (float)rc->right / texw;
		t2 = (float)rc->bottom / texh;
		w = ( rc->right - rc->left ) * font->scale * xs;
		h = ( rc->bottom - rc->top ) * font->scale;

		ref.dllFuncs.R_DrawStretchPic( cx, y, w, h, s1, t1, s2, t2, font->hFontTexture );
		cx += font->charWidths[number] * xs;
	}

	ref.dllFuncs.Color4ub( 255, 255, 255, 255 );

	return (int)( cx - x );
}

// Draw a proportional string right-aligned so its RIGHT edge sits at right_x.
// Used for the numeric columns (HP/Score/Deaths/Latency) so digits line up
// regardless of 1/2/3-digit values, matching PC CS 1.6.
static void Slayer_DrawStringRight( cl_font_t *font, int right_x, int y, const char *s, const rgba_t color )
{
	int w = 0, h = 0;

	if( !font || !s )
		return;

	Slayer_SB_StringLen( font, s, &w, &h );
	Slayer_SB_DrawString( (float)( right_x - w ), (float)y, s, color, font );
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

/*
====================
Slayer_Scoreboard_IsVisible

Whether our board is on screen right now — held open, at intermission, or
auto-shown while dead. Mirrors the checks at the top of Draw(), but without
side effects, so callers earlier in the frame (the VGUI and client-HUD gates in
V_PostRender / CL_DrawHUD) can ask before we have drawn anything.
====================
*/
qboolean Slayer_Scoreboard_IsVisible( void )
{
	if( slayer_scoreboard.value == 0.0f || cls.state != ca_active )
		return false;

	if( slayer_scoreboard_active || cl.intermission != 0 )
		return true;

	if( slayer_scoreboard_ondeath.value != 0.0f && !slayer_death_dismissed
	 && cl.playernum >= 0 && cl.playernum < MAX_CLIENTS )
	{
		// Consider the player dead if EITHER source says so:
		// 1) ScoreAttrib flags (from server) — reliable but delayed 1-3 frames
		// 2) cl.local.health <= 0 — immediate, no round-trip delay
		// Without the health check, the stock scoreboard is visible for those
		// 1-3 frames between death and ScoreAttrib, because block_level
		// returns 0 and the client DLL draws its own board.
		qboolean dead = ( slayer_scores[cl.playernum].flags & 1 )
			|| ( cl.local.health <= 0 );
		if( dead )
			return true;   // dead: CS shows its board here, and so do we
	}

	return false;
}

/*
====================
Slayer_Scoreboard_StockBlockLevel

How hard to suppress the game's own scoreboard while ours is up.

Ours is already drawn last in V_PostRender, yet the stock board still landed on
top — VGUI batches its primitives and flushes them after our draw, so draw
order alone cannot win. Level 1 skips VGui_Paint while our board is visible,
which removes it if the stock board is a VGUI panel. Level 2 additionally skips
the client DLL's HUD redraw, which removes it however it is drawn, at the cost
of hiding the rest of the game HUD for those frames.

Returns 0 when nothing should be suppressed.
====================
*/
int Slayer_Scoreboard_StockBlockLevel( void )
{
	static int last_reported = -1;
	int        cvar_level = (int)slayer_scoreboard_block_stock.value;
	qboolean   visible = Slayer_Scoreboard_IsVisible();
	qboolean   allow_hud_block = ( slayer_scoreboard_block_hud.value != 0.0f );
	int        level;

	if( cvar_level <= 0 || !visible )
	{
		level = 0;
	}
	else
	{
		// HARD CAP at 1 unless the separate opt-in cvar is set.
		//
		// This is the actual fix for "the HUD still disappears". Migrating the
		// archived value at init does not work: cvars are registered inside
		// CL_Init, but `exec config.cfg` runs afterwards, so the stored 2 is
		// re-applied right after the migration lowered it. Capping here is
		// order-independent — a config can set the cvar to anything and the HUD
		// still survives unless slayer_scoreboard_block_hud says otherwise.
		level = 1;
		if( allow_hud_block && cvar_level >= 2 )
			level = 2;
	}

	// This is asked several times per frame, so report only when the effective
	// level changes. Without it there is no way to tell from a log whether the
	// stock board survived because the block never engaged or because it
	// engaged and was not enough.
	if( level != last_reported )
	{
		last_reported = level;
		Slayer_Log_Printf( "stock-board block: level=%d (cvar=%d, hud_block=%d, ours visible=%d, dead-dismissed=%d)",
			level, cvar_level, (int)allow_hud_block, (int)visible,
			(int)slayer_death_dismissed );
	}

	return level;
}

/*
====================
Slayer_SB_FormatKD

Kill/death ratio into `out`.

Two decisions here rather than one obvious formula:

  * deaths == 0 is not infinity and must not print as "inf" or "-". With no
    deaths the ratio IS the kill count -- that is what a perfect round looks
    like -- so it prints as a whole number ("7"), which also distinguishes it
    from a computed ratio at a glance.
  * two decimals, and no trailing ".00": "1.5" and "12" read instantly, while
    "1.50" and "12.00" waste the narrow column the money column left behind.

Negative frags happen (team kills subtract), and a negative ratio is meaningful,
so it is not clamped.
====================
*/
static void Slayer_SB_FormatKD( char *out, size_t size, int frags, int deaths )
{
	float ratio;

	if( deaths <= 0 )
	{
		Q_snprintf( out, size, "%d", frags );
		return;
	}

	ratio = (float)frags / (float)deaths;

	// Whole ratios print without the decimal point: 2.00 -> "2".
	if( ratio == (float)(int)ratio )
		Q_snprintf( out, size, "%d", (int)ratio );
	else
		Q_snprintf( out, size, "%.2f", ratio );
}

void Slayer_Scoreboard_Draw( void )
{
	slayer_sort_entry_t sorted[MAX_CLIENTS];
	int          num_players = 0;
	int          i, row;
	int          screen_w, screen_h;
	int          board_x, board_y, board_w, board_h;
	int          row_h, col_name_x, col_frags_x, col_deaths_x, col_ping_x, col_health_x;
	int          row_gap;        // vertical breathing room between player rows
	int          col_kd_x;       // K/D column (right edge). Was "Деньги" — the
	                            // engine never receives money (it is a client-DLL
	                            // HUD value), so that column could only ever have
	                            // been blank. K/D is derivable from ScoreInfo.
	int          col_kit_x;         // "Компл." (defuse kit) fixed column, left-aligned
	int          col_name_text_x;   // fixed name-column origin (after the reserved avatar gutter)
	int          colordot_px;       // size of the per-player colour swatch (0 = disabled)
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
			slayer_avatar_upload_pending[cl.playernum] = true;
		}
	}

	// Pump Steam Web API batch requests
	Slayer_SteamAPI_Frame();

	// Resolve SteamIDs and start downloads on our own, so avatars are already
	// there the first time the board is opened.
	Slayer_Scoreboard_AutoFetch();

	// Upload a bounded number of decoded avatars per frame (see the function
	// comment: doing them all at once is a visible hitch on join).
	Slayer_DrainAvatarUploads();

	// Pump avatar downloads every frame (even when scoreboard hidden)
	if( Slayer_AvatarDownload_Frame() )
	{
		// A download finished. Hand the slot back to the loader by clearing the
		// "requested" marker and queueing an upload, so the completion path
		// obeys the same per-frame budget as everything else — a batch fetch can
		// finish a dozen files at once, and uploading them all here was the
		// second source of the join hitch.
		for( i = 0; i < MAX_CLIENTS; i++ )
		{
			char avpath[128];

			if( slayer_avatar_tex[i] != -1 || slayer_steamid64[i] == 0 )
				continue;

			Slayer_AvatarPath( avpath, sizeof( avpath ), slayer_steamid64[i] );
			if( !FS_FileExists( avpath, false ) )
				continue;

			slayer_avatar_tex[i] = 0;   // let the loader run; it validates the file
			slayer_avatar_upload_pending[i] = true;
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

	// The game's own scheme, loaded once per map, overrides the colours the
	// player has NOT configured. Team colours are deliberately left alone: the
	// scheme has no notion of CT/T, and ours were picked by measured perceptual
	// distance (see cl_teamcolors_slayer.c) -- replacing them with the panel's
	// generic text colour would make both teams the same colour.
	if( !slayer_scheme_tried )
		Slayer_SB_LoadScheme();

	Slayer_SB_SchemeColor( &slayer_scoreboard_bg_color, SLAYER_SCHEME_HAS_BG,
		slayer_scheme.bg, color_bg );
	Slayer_SB_SchemeColor( &slayer_scoreboard_text_color, SLAYER_SCHEME_HAS_TEXT,
		slayer_scheme.text, color_text );

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
			slayer_avatar_upload_pending[i] = false;
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

	// Adaptive width. A one-player server should not look like an empty full-
	// width spreadsheet; the reference board stays compact and grows only for a
	// larger roster. Both endpoints remain live-tunable cvars.
	{
		float max_frac = slayer_scoreboard_width.value;
		float compact_frac = slayer_scoreboard_compact_width.value;
		float roster_t = (float)( num_players - 5 ) / 15.0f;
		float wfrac;
		int   min_w, max_w;

		if( max_frac < 0.55f ) max_frac = 0.55f;
		if( max_frac > 0.95f ) max_frac = 0.95f;
		if( compact_frac < 0.50f ) compact_frac = 0.50f;
		if( compact_frac > max_frac ) compact_frac = max_frac;
		if( roster_t < 0.0f ) roster_t = 0.0f;
		if( roster_t > 1.0f ) roster_t = 1.0f;

		wfrac = compact_frac + ( max_frac - compact_frac ) * roster_t;
		board_w = (int)( screen_w * wfrac );

		min_w = (int)( screen_w * 0.50f );
		max_w = (int)( screen_w * 0.95f );
		if( board_w < min_w ) board_w = min_w;
		if( board_w > max_w ) board_w = max_w;
		if( board_w > (int)( screen_h * 1.75f )) board_w = (int)( screen_h * 1.75f );
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

			// Open the cells further on a sparse board. With only a handful of
			// rows there is height to spare, and cells sized for a full server
			// look mean in all that space. Cannot go via the glyphs — the bitmap
			// font has three fixed sizes — so the row grows instead. Crowded
			// rosters are untouched, and the compression step below still clamps
			// everything to the height actually available.
			if( content_rows > 0 && content_rows <= 8 )
				rscale *= 1.30f;
			else if( content_rows <= 14 )
				rscale *= 1.15f;

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

		// Row gap, added to the height as well as to the advances. Computing it
		// here (rather than at the advance) is what keeps the two in step: a gap
		// applied only in the loop would push the last player past the bottom and
		// silently clip them, which is exactly the failure the chrome estimate
		// above was written to avoid.
		row_gap = (int)( row_h * slayer_scoreboard_rowgap.value );
		if( row_gap < 0 ) row_gap = 0;
		if( row_gap > row_h ) row_gap = row_h;
		board_h += row_gap * num_players;

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
		int    radius;

		// Corner radius as a FRACTION of board height, via cvar. It used to be
		// hard-coded at 0.06 with a 14..60 px clamp, which on a tall phone board
		// produced corners so round the panel read as a lozenge rather than a
		// panel. The PC ScorePanel is only slightly rounded, so the default is
		// now 0.022 with a tighter clamp -- still anti-aliased (no quality lost),
		// just not a circle.
		// Corner radius from ROW height, not board height. Board height grows
		// with the roster, so a board-relative radius made a 20-player board
		// rounder than a 3-player one; the row pitch is the constant that
		// actually sets perceived corner scale. slayer_scoreboard_corner stays
		// as the board-relative override for anyone who prefers the old rule.
		if( slayer_scoreboard_corner_rows.value > 0.0f )
			radius = (int)( row_h * slayer_scoreboard_corner_rows.value );
		else
			radius = (int)( board_h * slayer_scoreboard_corner.value );

		if( radius < 6 ) radius = 6;
		if( radius > 22 ) radius = 22;
		if( radius > board_w / 2 ) radius = board_w / 2;
		if( radius > board_h / 2 ) radius = board_h / 2;

		// pre-apply the global opacity so the panel helper gets final alphas
		MakeRGBA( panel_bg, color_bg[0], color_bg[1], color_bg[2],
			(byte)( color_bg[3] * global_opacity / 255 ));
		MakeRGBA( panel_br, cached_color_border[0], cached_color_border[1], cached_color_border[2],
			(byte)( cached_color_border[3] * global_opacity / 255 ));

		// Follow the interface colour by default. hud_color lives in the client
		// library, so read it through Cvar_FindVar exactly like the radar rim
		// does; alpha stays from our own RGBA cvar.
		if( slayer_scoreboard_border_hud.value != 0.0f )
		{
			convar_t *hud = Cvar_FindVar( "hud_color" );

			if( hud && hud->string[0] )
			{
				int hr = 0, hg = 0, hb = 0;

				if( sscanf( hud->string, "%d %d %d", &hr, &hg, &hb ) == 3 )
				{
					panel_br[0] = (byte)bound( 0, hr, 255 );
					panel_br[1] = (byte)bound( 0, hg, 255 );
					panel_br[2] = (byte)bound( 0, hb, 255 );
				}
			}
		}

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
		// Reserve a fixed strip for the colour dot between the avatar gutter and
		// the nickname, so names line up whether or not a dot is drawn (same
		// reasoning as the avatar gutter: nothing may shift when data arrives).
		colordot_px = 0;
		if( slayer_scoreboard_colorstripe.value != 0.0f && slayer_scoreboard_colordot.value != 0.0f )
		{
			// A vertical stripe, not a dot: it reads as part of the avatar block
			// (same height, no gap) instead of a floating bullet competing with
			// the nickname for attention.
			colordot_px = (int)slayer_scoreboard_colorstripe.value;
			if( colordot_px < 2 ) colordot_px = 2;
			if( colordot_px > 10 ) colordot_px = 10;
		}
		{
			int namegap = (int)slayer_scoreboard_namegap.value;

			if( namegap < 2 ) namegap = 2;
			if( namegap > 40 ) namegap = 40;
			// The stripe is glued to the avatar, so only ONE gap exists between
			// the avatar+stripe block and the name.
			col_name_text_x = col_name_x + av + colordot_px + namegap;
		}

		col_ping_x = board_x + (int)( board_w * 0.978f );   // rightmost = Задержка
		Slayer_SB_StringLen( font, "Задержка", &hw, &hh );
		col_deaths_x = col_ping_x - hw - gap;
		Slayer_SB_StringLen( font, "Смертей", &hw, &hh );
		col_frags_x  = col_deaths_x - hw - gap;
		Slayer_SB_StringLen( font, "Счет", &hw, &hh );
		col_kd_x  = col_frags_x - hw - gap;
		if( slayer_scoreboard_kd.value != 0.0f )
		{
			int vw, vh;

			// Reserve the WIDER of the label and a worst-case value: "К/С" is
			// three glyphs while "12.34" is five, and measuring only the label
			// would let a long ratio run into the frags column.
			Slayer_SB_StringLen( font, "К/С", &hw, &hh );
			Slayer_SB_StringLen( font, "12.34", &vw, &vh );
			if( vw > hw ) hw = vw;
			col_health_x = col_kd_x - hw - gap;
		}
		else
		{
			// Column switched off: collapse it instead of leaving a hole between
			// HP and Счет.
			col_health_x = col_kd_x;
		}

		// The stat block is a fixed pixel width; on a narrow board its left
		// (HP) edge can cross into the names or off-screen. If so, shift the
		// whole block right as a unit so it stays clear of the name column.
		Slayer_SB_StringLen( font, "HP", &hw, &hh );
		min_health_x = col_name_text_x + hw + gap;
		if( col_health_x < min_health_x )
		{
			int shift = min_health_x - col_health_x;
			col_health_x += shift; col_kd_x  += shift; col_frags_x += shift;
			col_deaths_x += shift; col_ping_x   += shift;
		}

		// "Компл." sits just LEFT of the (measured, floating) HP column so it
		// can never overrun the HP digits, and is floored against the names.
		Slayer_SB_StringLen( font, "Компл.", &hw, &hh );
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
	// HP/K-D/… band and delete the old title row above it.)
	//
	// The gap above that row used to be row_h/3 + 4, which on a phone-sized board
	// left a visibly empty strip between the panel edge and the labels — the
	// "пустота в верхней полосе" from the report. It is now a fraction of the row
	// height (so it scales with the font tier rather than being a magic number)
	// and defaults to a quarter of it.
	{
		int toppad = (int)( row_h * slayer_scoreboard_toppad.value );

		if( toppad < 0 ) toppad = 0;
		if( toppad > row_h ) toppad = row_h;
		cur_y += toppad;
	}
	{
		const char *mapname = Info_ValueForKey( cl.serverinfo, "map" );
		rgba_t color_hdr, color_map;
		int    iw, ih;

		if( !mapname || mapname[0] == '\0' )
			mapname = clgame.mapname;

		// left: IP then map, side by side with a clear gap
		Slayer_SB_DrawString( col_name_x, cur_y, hostname, color_text, font );
		if( mapname && mapname[0] != '\0' )
		{
			Slayer_SB_StringLen( font, hostname, &iw, &ih );
			MakeRGBA( color_map, color_text[0] * 160 / 255, color_text[1] * 160 / 255, color_text[2] * 160 / 255, 200 );
			Slayer_SB_DrawString( col_name_x + iw + font->charHeight * 2, cur_y, mapname, color_map, font );
		}

		// right: column labels (soft light grey, Russian). The scheme's header
		// colour wins when the player has not chosen their own text colour --
		// this is the "196 181 80" olive on the reference install.
		MakeRGBA( color_hdr, 206, 206, 200, color_text[3] );
		Slayer_SB_SchemeColor( &slayer_scoreboard_text_color, SLAYER_SCHEME_HAS_HEADER_TEXT,
			slayer_scheme.header_text, color_hdr );
		color_hdr[3] = color_text[3];

		Slayer_DrawStringRight( font, col_health_x, cur_y, "HP", color_hdr );
		if( slayer_scoreboard_kd.value != 0.0f )
			Slayer_DrawStringRight( font, col_kd_x, cur_y, "К/С", color_hdr );
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
			Slayer_SB_DrawString( col_name_text_x, cur_y, buf, color_t, font );
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
			Slayer_SB_DrawString( col_name_text_x, cur_y, buf, color_ct, font );
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
			Slayer_SB_DrawString( col_name_text_x, cur_y, buf, color_spec, font );
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

		// MARKING YOUR OWN ROW.
		//
		// Style 1 (default) draws a LINE under the row, which is what the vanilla
		// board does -- it marks and separates with 1px rules, and its selection
		// colour (SelectionBG 10 10 10 100) is near-black, i.e. barely a fill at
		// all. The filled bar that used to be here was rejected on sight, and the
		// reason is that a bar of any colour reads as a stripe painted over the
		// board rather than as "this row is mine".
		//
		// Style 2 is that bar, kept behind a cvar.
		//
		// GEOMETRY, from the reference screenshot: inset from the board edges rather
		// than edge to edge, so it never reaches the rounded corners.
		if( pidx == cl.playernum )
		{
			int style = (int)slayer_scoreboard_sel_style.value;
			int sel_alpha = (int)slayer_scoreboard_sel_alpha.value;

			if( style < 0 ) style = 0;
			if( style > 2 ) style = 1;
			if( sel_alpha < 0 ) sel_alpha = 0;
			if( sel_alpha > 255 ) sel_alpha = 255;

			if( style != 0 && sel_alpha > 0 )
			{
				rgba_t sel;
				int    inset = (int)( board_w * 0.012f );
				int    a;

				if( inset < 3 ) inset = 3;

				// The cvar wins over the scheme here, unlike the board's other
				// colours: the mark is ours, not the game's -- there is no vanilla
				// role for "the local player's row" on a board that never had one.
				MakeRGBA( sel, 255, 176, 0, (byte)sel_alpha );
				Slayer_ParseColorString( slayer_scoreboard_sel_color.string, sel );
				sel[3] = (byte)sel_alpha;

				a = (int)( sel[3] * global_opacity / 255 );

				if( style == 1 )
				{
					// Underline, sitting ON the row's bottom edge.
					//
					// Thickness is derived from the ROW HEIGHT rather than from a
					// pixel constant scaled by hand: the row height already carries
					// the HUD scale and the rowscale cvar, so a line at 1/12 of it
					// stays one hairline on a phone and stays visible on a tablet.
					// A fixed pixel count would vanish on one and look like a bar on
					// the other.
					int thick = (int)( slayer_scoreboard_sel_thick.value
						* (float)row_h / 12.0f );

					if( thick < 1 ) thick = 1;
					if( thick > row_h / 3 ) thick = row_h / 3;
					if( thick < 1 ) thick = 1;

					Slayer_DrawRect( board_x + inset, cur_y + row_h - thick,
						board_w - inset * 2, thick,
						sel[0], sel[1], sel[2], (byte)a );
				}
				else
				{
					// The old filled bar. Scaled down hard: at the alpha a line
					// wants, a fill would bury the nickname under it.
					Slayer_DrawRect( board_x + inset, cur_y,
						board_w - inset * 2, row_h,
						sel[0], sel[1], sel[2], (byte)( a * 35 / 100 ));
				}
			}
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

			// Per-player colour swatch: the SAME colour the radar uses for this
			// player. Drawn as a small rounded square between the avatar and the
			// nickname. Placed here rather than in a separate column because
			// the eye already travels to the name when matching a radar dot to
			// a player, and a dedicated column would cost horizontal space the
			// phone layout does not have.
			if( colordot_px > 0 && team != SLAYER_TEAM_SPECTATOR )
			{
				byte dot[3];
				int  dx = col_name_x + avatar_size;   // flush against the avatar
				int  dy = avatar_y;                   // exactly the avatar height

				Slayer_TeamColors_Get( pidx, dot );

				// One solid rect. Sharing the avatar's y and height is what makes
				// the two read as a single glued block; any inset here would
				// reintroduce the floating-chip look.
				Slayer_DrawRect( dx, dy, colordot_px, avatar_size,
					dot[0], dot[1], dot[2], row_alpha );
			}

			Slayer_SB_DrawString( col_name_text_x, cur_y + text_dy, name, name_color, font );
		}

		// "Компл." — defuse-kit marker in its own fixed column (CT only). The
		// defuser bit is 1<<3 in the ScoreAttrib flags byte; if the server does
		// not set it, nothing draws (harmless).
		if( team == SLAYER_TEAM_CT && !( slayer_scores[pidx].flags & 1 )
		 && ( slayer_scores[pidx].flags & 8 ))
		{
			rgba_t kit_color;
			MakeRGBA( kit_color, 214, 214, 208, ( row_alpha * 78 ) / 100 );
			Slayer_SB_DrawString( col_kit_x, cur_y + text_dy, "Компл.", kit_color, font );
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

				// K/D — where "Деньги" used to be. Money never reaches the
				// engine (the client DLL owns it), so that column was blank for
				// everyone; this is the same information the player actually
				// wanted from a stat column, derived from what we already have.
				if( slayer_scoreboard_kd.value != 0.0f )
				{
					Slayer_SB_FormatKD( buf, sizeof( buf ),
						slayer_scores[pidx].frags, slayer_scores[pidx].deaths );
					Slayer_DrawStringRight( font, col_kd_x, cur_y + text_dy, buf, stat_color );
				}
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

		cur_y += row_h + row_gap;
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
