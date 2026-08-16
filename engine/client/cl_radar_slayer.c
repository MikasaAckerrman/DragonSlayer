/*
cl_radar_slayer.c - Slayer3D radar (CS2-style) replacing the vanilla one
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

// Own radar, drawn engine-side. The vanilla CS 1.6 radar is a fixed square of
// green blips with no map underneath; this one has:
//
//   * THE MAP TEXTURE inside it. GoldSrc has no such mechanism at all, so the
//     data comes from the engine's own overview system: overviews/<map>.txt
//     carries ZOOM / ORIGIN / ROTATED, which is exactly the world->radar
//     transform, and overviews/<map>.bmp is the image. Both are what
//     `dev_overview 1` + a screenshot produce, so a map without them still
//     works -- it just falls back to a plain dark disc.
//   * a "volume" look: a soft inner shadow plus a lighter rim, so the disc
//     reads as a recessed lens rather than a flat sticker.
//   * a border tinted by the HUD colour, so the radar matches the rest of the
//     interface instead of being a separate visual island.
//   * per-player colours shared with the scoreboard (cl_teamcolors_slayer.c).
//   * enemies shown ONLY once seen: a filled ring while visible, a hollow ring
//     from memory after they leave sight. Not a cheat -- the sighting comes
//     from the same PVS/trace the renderer already has, i.e. from what the
//     player could actually see on screen.
//   * height markers: a small notch above/below the dot for players on another
//     floor, drawn as an arc rather than the vanilla triangle.
//
// Position and size are cvars, so it can be moved and swapped around.

#include "common.h"
#include "client.h"
#include "ref_common.h"
#include "cl_view_slayer.h"
#include "cl_teamcolors_slayer.h"
#include "cl_radar_slayer.h"
#include "cl_radar_map_slayer.h"
#include "cl_slayer_log.h"

// ===========================================================================
// Cvars
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_radar, "1", FCVAR_ARCHIVE,
	"Slayer3D: own radar (0 = off, use the game's)" );

// Placement: fractions of the screen so it lands correctly on any resolution.
// x/y are the CENTRE of the radar, which makes mirroring it to the other corner
// a single sign flip instead of a size-dependent recompute.
static CVAR_DEFINE_AUTO( slayer_radar_x, "0.085", FCVAR_ARCHIVE,
	"Slayer3D: radar centre X as a fraction of screen width" );

static CVAR_DEFINE_AUTO( slayer_radar_y, "0.13", FCVAR_ARCHIVE,
	"Slayer3D: radar centre Y as a fraction of screen height" );

static CVAR_DEFINE_AUTO( slayer_radar_size, "0.18", FCVAR_ARCHIVE,
	"Slayer3D: radar diameter as a fraction of screen height" );

static CVAR_DEFINE_AUTO( slayer_radar_size_migrated, "0", FCVAR_ARCHIVE,
	"Slayer3D internal: radar size default migration completed" );

// World units mapped across the radar. Lower = more zoomed in, like CS2's
// cl_radar_scale. When the map has an overview script this scales it too.
static CVAR_DEFINE_AUTO( slayer_radar_scale, "1400", FCVAR_ARCHIVE,
	"Slayer3D: world units across the radar when auto-fit is off (smaller = closer zoom)" );

static CVAR_DEFINE_AUTO( slayer_radar_autoscale, "1", FCVAR_ARCHIVE,
	"Slayer3D: choose a useful player-centred local map scale from BSP size (0 = manual)" );

// 1 = radar rotates with the player (CS2 default), 0 = north-up.
static CVAR_DEFINE_AUTO( slayer_radar_rotate, "1", FCVAR_ARCHIVE,
	"Slayer3D: rotate the radar with the player view (0 = fixed north-up)" );

static CVAR_DEFINE_AUTO( slayer_radar_map, "1", FCVAR_ARCHIVE,
	"Slayer3D: draw the map picture inside the radar (1 = rasterize the BSP, 2 = external overviews/<map>.bmp only, 0 = off)" );

static CVAR_DEFINE_AUTO( slayer_radar_map_res, "512", FCVAR_ARCHIVE,
	"Slayer3D: resolution of the rasterized radar map texture (128..1024)" );

// How the rasterized map is coloured. 0 keeps the original blueprint look, which
// is why it stays available: on some maps the flat grey-blue actually reads the
// layout better than the real colours do.
static CVAR_DEFINE_AUTO( slayer_radar_map_color, "2", FCVAR_ARCHIVE,
	"Slayer3D: radar map colouring (0 = height shading only, 1 = texture colours, 2 = lighting + textures)" );

static CVAR_DEFINE_AUTO( slayer_radar_map_alpha, "215", FCVAR_ARCHIVE,
	"Slayer3D: map picture opacity inside the radar, 0..255" );

// Border colour follows the HUD colour by default. GoldSrc keeps the HUD colour
// in the client library, not the engine, so we read the `hud_color` cvar when it
// exists (ReGameDLL/CS clients register it) and fall back to this string.
static CVAR_DEFINE_AUTO( slayer_radar_border, "", FCVAR_ARCHIVE,
	"Slayer3D: radar border colour 'R G B' (empty = follow hud_color)" );

static CVAR_DEFINE_AUTO( slayer_radar_bg, "10 16 22 205", FCVAR_ARCHIVE,
	"Slayer3D: radar background colour 'R G B A'" );

// How long an enemy stays on the radar as a hollow ring after being lost.
static CVAR_DEFINE_AUTO( slayer_radar_memory, "4.0", FCVAR_ARCHIVE,
	"Slayer3D: seconds an unseen enemy remains as a hollow ring (0 = vanish at once)" );

static CVAR_DEFINE_AUTO( slayer_radar_dot, "1.0", FCVAR_ARCHIVE,
	"Slayer3D: player dot size multiplier" );

static CVAR_DEFINE_AUTO( slayer_radar_names, "1", FCVAR_ARCHIVE,
	"Slayer3D: draw compact player names beside radar markers (0 = off)" );

static CVAR_DEFINE_AUTO( slayer_radar_height, "1", FCVAR_ARCHIVE,
	"Slayer3D: draw above/below height arcs on dots (0 = off)" );

// View cone: the wedge that shows where you are looking. This is the element
// the vanilla radar draws as a small triangle; here it is a real cone whose
// WIDTH follows the actual FOV, so zooming a scope narrows it like in CS2.
static CVAR_DEFINE_AUTO( slayer_radar_cone, "1", FCVAR_ARCHIVE,
	"Slayer3D: draw the view-direction cone on the radar (0 = off)" );

static CVAR_DEFINE_AUTO( slayer_radar_cone_len, "0.55", FCVAR_ARCHIVE,
	"Slayer3D: view cone length as a fraction of the radar radius" );

static CVAR_DEFINE_AUTO( slayer_radar_cone_alpha, "70", FCVAR_ARCHIVE,
	"Slayer3D: view cone opacity, 0..255" );

// ===========================================================================
// State
// ===========================================================================

// Overview transform, parsed from overviews/<map>.txt once per map.
typedef struct
{
	qboolean loaded;      // script parsed successfully
	qboolean tried;       // parse attempted (so a missing file is not retried)
	float    zoom;        // ZOOM from the script
	vec3_t   origin;      // ORIGIN from the script
	qboolean rotated;     // ROTATED flag: map is stored 90 degrees turned
	int      texnum;      // overviews/<map>.bmp, 0 = none
} slayer_radar_map_t;

static slayer_radar_map_t s_map;

// Whether we have told the client DLL to hide its stock CHudRadar. CS exposes
// the commands `hideradar` / `drawradar`; ownership is synchronized from Draw
// so a live cvar toggle cannot leave either zero radars or two radars behind.
static qboolean s_stock_hidden;
static int      s_owner_last_reported = -1;

// Enemy sighting memory. Kept per client slot; `t_seen` is host.realtime of the
// last confirmed sighting, `pos` where it was then. This is the whole reason the
// hollow ring is honest: nothing is recorded until the player has line of sight.
typedef struct
{
	double t_seen;
	vec3_t pos;
	qboolean ever;
} slayer_radar_sight_t;

static slayer_radar_sight_t s_sight[MAX_CLIENTS];

// ===========================================================================
// Helpers
// ===========================================================================

static void Slayer_Radar_Rect( int x, int y, int w, int h, byte r, byte g, byte b, byte a )
{
	if( w <= 0 || h <= 0 || a == 0 )
		return;

	ref.dllFuncs.FillRGBA( kRenderTransTexture, (float)x, (float)y, (float)w, (float)h, r, g, b, a );
}

static void Slayer_Radar_ParseColor3( const char *s, byte out[3], byte dr, byte dg, byte db )
{
	int r = dr, g = dg, b = db;

	out[0] = dr; out[1] = dg; out[2] = db;

	if( COM_StringEmptyOrNULL( s ))
		return;
	if( sscanf( s, "%d %d %d", &r, &g, &b ) < 3 )
		return;

	out[0] = (byte)bound( 0, r, 255 );
	out[1] = (byte)bound( 0, g, 255 );
	out[2] = (byte)bound( 0, b, 255 );
}

// The HUD colour lives in the client library as `hud_color` ("R G B"). Reading
// it through Cvar_FindVar means the radar border tracks whatever the player set
// for the rest of the interface, with no extra setting to keep in sync.
static void Slayer_Radar_BorderColor( byte out[3] )
{
	convar_t *hud;

	if( !COM_StringEmptyOrNULL( slayer_radar_border.string ))
	{
		Slayer_Radar_ParseColor3( slayer_radar_border.string, out, 130, 200, 255 );
		return;
	}

	hud = Cvar_FindVar( "hud_color" );
	if( hud && !COM_StringEmptyOrNULL( hud->string ))
	{
		Slayer_Radar_ParseColor3( hud->string, out, 130, 200, 255 );
		return;
	}

	// Neither set: a cool blue that reads on both bright and dark maps.
	out[0] = 130; out[1] = 200; out[2] = 255;
}

// ===========================================================================
// Overview script
// ===========================================================================

// overviews/<map>.txt is the engine's own format, written by
// VID_WriteOverviewScript (cl_scrn.c). Parsing it here means any map that has
// been through `dev_overview 1` + screenshot gets a real radar image for free.
static void Slayer_Radar_LoadMap( void )
{
	char   path[MAX_QPATH];
	byte  *buf;
	fs_offset_t len;
	char  *p;

	if( s_map.tried )
		return;

	s_map.tried = true;
	s_map.loaded = false;
	s_map.texnum = 0;
	s_map.zoom = 1.0f;
	s_map.rotated = false;
	VectorClear( s_map.origin );

	if( COM_StringEmptyOrNULL( clgame.mapname ))
		return;

	Q_snprintf( path, sizeof( path ), "overviews/%s.txt", clgame.mapname );

	buf = FS_LoadFile( path, &len, false );
	if( !buf )
	{
		Slayer_Log_Printf( "radar: no overview script '%s' -- plain disc, no map image", path );
		return;
	}

	// Minimal key scan rather than a full parser: the file is machine-written
	// and only three keys matter. Anything missing keeps its default.
	p = (char *)buf;
	{
		char *k = Q_strstr( p, "ZOOM" );

		if( k ) sscanf( k + 4, "%f", &s_map.zoom );
	}
	{
		char *k = Q_strstr( p, "ORIGIN" );

		if( k ) sscanf( k + 6, "%f %f %f", &s_map.origin[0], &s_map.origin[1], &s_map.origin[2] );
	}
	{
		char *k = Q_strstr( p, "ROTATED" );
		int   rot = 0;

		if( k && sscanf( k + 7, "%d", &rot ) == 1 )
			s_map.rotated = rot ? true : false;
	}

	Mem_Free( buf );

	if( s_map.zoom <= 0.0f )
		s_map.zoom = 1.0f;

	Q_snprintf( path, sizeof( path ), "overviews/%s.bmp", clgame.mapname );
	if( FS_FileExists( path, false ))
		s_map.texnum = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, TF_IMAGE | TF_CLAMP );

	s_map.loaded = true;
	Slayer_Log_Printf( "radar: overview '%s' zoom=%.2f origin=(%.0f %.0f) rotated=%d tex=%d",
		clgame.mapname, s_map.zoom, s_map.origin[0], s_map.origin[1],
		(int)s_map.rotated, s_map.texnum );
}

// ===========================================================================
// Visibility
// ===========================================================================

// Is `ent` visible to the local player right now? Two gates, cheapest first:
//   1. the entity must have been in this frame's snapshot (the server only
//      sends entities in our PVS, so this alone is most of the answer);
//   2. a trace from the eye to the enemy's chest must be clear.
// Deliberately NOT a FOV check: CS2 marks enemies your team saw, and a strict
// cone would make the ring flicker every time you turn your head.
static qboolean Slayer_Radar_CanSee( cl_entity_t *ent, const vec3_t focus_origin,
	const cl_entity_t *focus )
{
	vec3_t     eye, target;
	pmtrace_t *tr;
	int         ignore_pe = -1;
	int         target_pe = -1;
	int         i;

	if( !ent || !ent->player )
		return false;
	if( ent->curstate.messagenum != cl.parsecount )
		return false;   // not in this snapshot -> not in our PVS

	VectorCopy( focus_origin, eye );
	if( focus )
	{
		if( focus->curstate.solid == SOLID_NOT ) eye[2] -= 8.0f;
		else if( focus->curstate.usehull == 1 ) eye[2] += 12.0f;
		else eye[2] += 28.0f;
	}
	else
	{
		VectorAdd( focus_origin, cl.viewheight, eye );
	}

	VectorCopy( ent->origin, target );
	target[2] += 20.0f;   // chest, not feet: feet are behind cover more often

	if( clgame.pmove )
	{
		for( i = 0; i < clgame.pmove->numphysent; i++ )
		{
			if( focus && clgame.pmove->physents[i].info == focus->index )
				ignore_pe = i;
			if( clgame.pmove->physents[i].info == ent->index )
				target_pe = i;
		}
	}

	tr = PM_CL_TraceLine( eye, target, PM_TRACELINE_PHYSENTSONLY,
		2 /* point hull */, ignore_pe );
	if( !tr )
		return false;

	return tr->fraction > 0.98f || tr->ent == target_pe ? true : false;
}

// ===========================================================================
// Drawing primitives
// ===========================================================================

// Filled disc with an anti-aliased edge, built from 1px rows. GoldSrc gives us
// FillRGBA only, so the smooth edge has to come from per-row alpha.
static void Slayer_Radar_Disc( int cx, int cy, int radius,
	byte r, byte g, byte b, byte a )
{
	int py;

	for( py = -radius; py <= radius; py++ )
	{
		float dy = (float)py + 0.5f;
		float span = (float)radius * (float)radius - dy * dy;
		int   half;

		if( span <= 0.0f )
			continue;

		half = (int)sqrt( span );
		Slayer_Radar_Rect( cx - half, cy + py, half * 2, 1, r, g, b, a );
	}
}

// Ring of given thickness. `fill` draws the interior too (visible enemy),
// otherwise only the outline (remembered enemy).
static void Slayer_Radar_Ring( int cx, int cy, int radius, int thickness,
	byte r, byte g, byte b, byte a, qboolean fill )
{
	int py;
	int inner = radius - thickness;

	if( inner < 0 ) inner = 0;

	for( py = -radius; py <= radius; py++ )
	{
		float dy = (float)py + 0.5f;
		float so = (float)radius * (float)radius - dy * dy;
		float si = (float)inner * (float)inner - dy * dy;
		int   ho, hi;

		if( so <= 0.0f )
			continue;

		ho = (int)sqrt( so );
		hi = ( si > 0.0f ) ? (int)sqrt( si ) : 0;

		if( fill || hi == 0 )
		{
			Slayer_Radar_Rect( cx - ho, cy + py, ho * 2, 1, r, g, b, a );
		}
		else
		{
			Slayer_Radar_Rect( cx - ho, cy + py, ho - hi, 1, r, g, b, a );
			Slayer_Radar_Rect( cx + hi, cy + py, ho - hi, 1, r, g, b, a );
		}
	}
}

// Height marker: a short arc above or below the dot. Replaces the vanilla
// triangle, and works for both allies and spotted enemies.
static void Slayer_Radar_HeightArc( int cx, int cy, int radius, qboolean above,
	byte r, byte g, byte b, byte a )
{
	int i;
	int span = radius + 2;

	for( i = -span; i <= span; i++ )
	{
		float t = (float)i / (float)span;      // -1..1 across the arc
		int   dy = (int)(( 1.0f - t * t ) * 2.0f );   // parabola ~ shallow arc
		int   y = above ? ( cy - radius - 2 - dy ) : ( cy + radius + 2 + dy );

		Slayer_Radar_Rect( cx + i, y, 1, 1, r, g, b, a );
	}
}

static void Slayer_Radar_DrawName( int px, int py, int dot, const char *name,
	const byte col[3], byte alpha, int cx, int cy, int radius )
{
	char   label[13];
	int    text_w = 0, text_h = 0;
	int    x, y;
	rgba_t rgba;

	if( slayer_radar_names.value == 0.0f || COM_StringEmptyOrNULL( name ))
		return;

	Q_strncpy( label, name, sizeof( label ));
	Con_DrawStringLen( label, &text_w, &text_h );
	if( text_w <= 0 ) text_w = (int)Q_strlen( label ) * 8;
	if( text_h <= 0 ) text_h = 8;

	x = px - text_w / 2;
	y = py - dot - text_h - 2;

	if( x < cx - radius + 3 ) x = cx - radius + 3;
	if( x + text_w > cx + radius - 3 ) x = cx + radius - 3 - text_w;
	if( y < cy - radius + 3 ) y = py + dot + 2;
	if( y + text_h > cy + radius - 3 ) return;

	Slayer_Radar_Rect( x - 2, y - 1, text_w + 4, text_h + 2,
		0, 0, 0, (byte)( alpha * 3 / 4 ));
	rgba[0] = col[0];
	rgba[1] = col[1];
	rgba[2] = col[2];
	rgba[3] = alpha;
	Con_DrawString( x, y, label, rgba );
}

static void Slayer_Radar_Triangle( float x0, float y0, float x1, float y1,
	float x2, float y2, byte r, byte g, byte b, byte a )
{
	vec3_t p;

	ref.dllFuncs.TriRenderMode( kRenderTransColor );
	ref.dllFuncs.Color4ub( r, g, b, a );
	ref.dllFuncs.Begin( TRI_TRIANGLES );

	p[0] = x0; p[1] = y0; p[2] = 0.0f;
	ref.dllFuncs.Vertex3fv( p );
	p[0] = x1; p[1] = y1;
	ref.dllFuncs.Vertex3fv( p );
	p[0] = x2; p[1] = y2;
	ref.dllFuncs.Vertex3fv( p );

	ref.dllFuncs.End();
	ref.dllFuncs.TriRenderMode( kRenderNormal );
	ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
	ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
}

static void Slayer_Radar_PlayerArrow( int cx, int cy, int size, float yaw,
	float cs, float sn, byte r, byte g, byte b, byte a )
{
	float wy = (float)DEG2RAD( yaw );
	float wx = cosf( wy );
	float yy = sinf( wy );
	float dx = wx * cs - yy * sn;
	float dy = -( wx * sn + yy * cs );
	float px = -dy;
	float py = dx;
	float tipx = (float)cx + dx * (float)size * 1.45f;
	float tipy = (float)cy + dy * (float)size * 1.45f;
	float basex = (float)cx - dx * (float)size * 0.75f;
	float basey = (float)cy - dy * (float)size * 0.75f;

	Slayer_Radar_Triangle( tipx, tipy,
		basex + px * (float)size, basey + py * (float)size,
		basex - px * (float)size, basey - py * (float)size,
		r, g, b, a );
}

static void Slayer_Radar_DrawCone( int cx, int cy, int radius )
{
	float half_fov;
	float len;
	byte  ca;
	float tan_half;
	int   k, ilen;

	if( slayer_radar_cone.value == 0.0f )
		return;

	half_fov = bound( 10.0f, cl.local.scr_fov, 150.0f ) * 0.5f;
	len = (float)radius * slayer_radar_cone_len.value;
	ca = (byte)bound( 0, (int)slayer_radar_cone_alpha.value, 255 );
	if( len < 6.0f ) len = 6.0f;
	if( len > (float)radius ) len = (float)radius;
	ilen = (int)len;

	if( half_fov < 5.0f ) half_fov = 5.0f;
	if( half_fov > 85.0f ) half_fov = 85.0f;
	tan_half = tanf( (float)DEG2RAD( half_fov ));

	for( k = 1; k <= ilen; k++ )
	{
		float t = (float)k / len;
		int   half_w = (int)( (float)k * tan_half );
		int   y = cy - k;
		byte  a = (byte)( ca * ( 1.0f - t * 0.85f ));

		if( half_w < 1 ) half_w = 1;
		if( a == 0 ) continue;

		if( (float)( k * k + half_w * half_w ) > (float)( radius * radius ))
		{
			float room = (float)( radius * radius - k * k );

			if( room <= 0.0f ) break;
			half_w = (int)sqrt( room );
			if( half_w < 1 ) continue;
		}

		Slayer_Radar_Rect( cx - half_w, y, half_w * 2, 1,
			255, 255, 255, a );
	}
}

// ===========================================================================
// Public API
// ===========================================================================

static void Slayer_Radar_SyncStock( void )
{
	qboolean own = Slayer_Radar_IsEnabled();

	if( own && !s_stock_hidden && cls.state == ca_active &&
		cls.signon == SIGNONS && Cmd_Exists( "hideradar" ))
	{
		Cbuf_AddText( "hideradar\n" );
		s_stock_hidden = true;
	}
	else if( !own && s_stock_hidden && Cmd_Exists( "drawradar" ))
	{
		Cbuf_AddText( "drawradar\n" );
		s_stock_hidden = false;
	}

	if((int)own != s_owner_last_reported )
	{
		s_owner_last_reported = (int)own;
		Slayer_Log_Printf( "radar ownership: own=%d stock_command=%s",
			(int)own, own ? "hideradar" : "drawradar" );
	}
}

// ===========================================================================
// Console convenience commands
// ===========================================================================
//
// The radar already exposes everything as cvars, but placing it by editing two
// fractions and guessing is not usable in-game on a phone. These commands are a
// thin layer over the same cvars: nothing here holds state, so a config that
// sets the cvars directly still works exactly as before.

typedef struct
{
	const char *name;
	float       x, y;
} slayer_radar_spot_t;

// Fractions are the CENTRE of the radar, so a corner preset has to account for
// the radius; 0.5 * size is exactly that, plus a small margin off the edge.
static const slayer_radar_spot_t s_radar_spots[] =
{
	{ "tl", 0.085f, 0.130f },
	{ "tr", 0.915f, 0.130f },
	{ "bl", 0.085f, 0.860f },
	{ "br", 0.915f, 0.860f },
	{ "top", 0.500f, 0.130f },
	{ "bottom", 0.500f, 0.860f },
	{ "center", 0.500f, 0.500f }
};

#define SLAYER_RADAR_NUM_SPOTS ( (int)( sizeof( s_radar_spots ) / sizeof( s_radar_spots[0] )))

static void Slayer_Radar_PrintSettings( void )
{
	Con_Printf( "^3radar:^7 %s   pos %.3f %.3f   size %.3f   scale %s/%.0f   rotate %d\n",
		slayer_radar.value != 0.0f ? "on" : "off",
		slayer_radar_x.value, slayer_radar_y.value,
		slayer_radar_size.value,
		slayer_radar_autoscale.value != 0.0f ? "auto-local" : "manual",
		slayer_radar_scale.value,
		(int)slayer_radar_rotate.value );
	Con_Printf( "  map %d (1=from BSP, 2=external overview, 0=off)  res %d  alpha %d  color %d\n",
		(int)slayer_radar_map.value, (int)slayer_radar_map_res.value,
		(int)slayer_radar_map_alpha.value, (int)slayer_radar_map_color.value );
	Con_Printf( "  cone %d  len %.2f  alpha %d   dot %.2f   names %d   memory %.1fs   height %d\n",
		(int)slayer_radar_cone.value, slayer_radar_cone_len.value,
		(int)slayer_radar_cone_alpha.value, slayer_radar_dot.value,
		(int)slayer_radar_names.value, slayer_radar_memory.value,
		(int)slayer_radar_height.value );
	Con_Printf( "  border '%s'  bg '%s'\n",
		slayer_radar_border.string[0] ? slayer_radar_border.string : "(follow hud_color)",
		slayer_radar_bg.string );
}

static void Cmd_RadarPos_f( void )
{
	const char *what;
	int         i;

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( "usage: slayer_radar_pos <tl|tr|bl|br|top|bottom|center>\n" );
		Con_Printf( "       slayer_radar_pos <x> <y>   (fractions of the screen, 0..1)\n" );
		Con_Printf( "current: %.3f %.3f\n", slayer_radar_x.value, slayer_radar_y.value );
		return;
	}

	what = Cmd_Argv( 1 );

	// Two numbers = explicit placement.
	if( Cmd_Argc() >= 3 )
	{
		float x = Q_atof( what );
		float y = Q_atof( Cmd_Argv( 2 ));

		Cvar_SetValue( "slayer_radar_x", bound( 0.02f, x, 0.98f ));
		Cvar_SetValue( "slayer_radar_y", bound( 0.02f, y, 0.98f ));
		Con_Printf( "radar moved to %.3f %.3f\n",
			slayer_radar_x.value, slayer_radar_y.value );
		return;
	}

	for( i = 0; i < SLAYER_RADAR_NUM_SPOTS; i++ )
	{
		if( !Q_stricmp( what, s_radar_spots[i].name ))
		{
			Cvar_SetValue( "slayer_radar_x", s_radar_spots[i].x );
			Cvar_SetValue( "slayer_radar_y", s_radar_spots[i].y );
			Con_Printf( "radar moved to %s (%.3f %.3f)\n",
				s_radar_spots[i].name, s_radar_spots[i].x, s_radar_spots[i].y );
			return;
		}
	}

	Con_Printf( "unknown position '%s'. try: tl tr bl br top bottom center\n", what );
}

static void Cmd_RadarNudge_f( void )
{
	float step = 0.01f;
	float dx = 0.0f, dy = 0.0f;
	const char *dir;

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( "usage: slayer_radar_nudge <left|right|up|down> [step]\n" );
		Con_Printf( "       step is a fraction of the screen, default 0.01\n" );
		return;
	}

	dir = Cmd_Argv( 1 );
	if( Cmd_Argc() >= 3 )
		step = Q_atof( Cmd_Argv( 2 ));
	if( step <= 0.0f ) step = 0.01f;
	if( step > 0.5f ) step = 0.5f;

	if( !Q_stricmp( dir, "left" ))       dx = -step;
	else if( !Q_stricmp( dir, "right" )) dx =  step;
	else if( !Q_stricmp( dir, "up" ))    dy = -step;
	else if( !Q_stricmp( dir, "down" ))  dy =  step;
	else
	{
		Con_Printf( "unknown direction '%s'. use left/right/up/down\n", dir );
		return;
	}

	Cvar_SetValue( "slayer_radar_x", bound( 0.02f, slayer_radar_x.value + dx, 0.98f ));
	Cvar_SetValue( "slayer_radar_y", bound( 0.02f, slayer_radar_y.value + dy, 0.98f ));
	Con_Printf( "radar at %.3f %.3f\n", slayer_radar_x.value, slayer_radar_y.value );
}

static void Cmd_RadarSize_f( void )
{
	const char *arg;
	float       v;

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( "usage: slayer_radar_setsize <fraction>   (0.08..0.45 of screen height)\n" );
		Con_Printf( "       slayer_radar_setsize +   / -      (step by 0.01)\n" );
		Con_Printf( "current: %.3f\n", slayer_radar_size.value );
		return;
	}

	arg = Cmd_Argv( 1 );

	if( !Q_strcmp( arg, "+" ))      v = slayer_radar_size.value + 0.01f;
	else if( !Q_strcmp( arg, "-" )) v = slayer_radar_size.value - 0.01f;
	else                            v = Q_atof( arg );

	Cvar_SetValue( "slayer_radar_size", bound( 0.08f, v, 0.45f ));
	Con_Printf( "radar size %.3f\n", slayer_radar_size.value );
}

static void Cmd_RadarZoom_f( void )
{
	const char *arg;
	float       v;

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( "usage: slayer_radar_zoom <fit|in|out|units>\n" );
		Con_Printf( "current: %s, manual %.0f units\n",
			slayer_radar_autoscale.value != 0.0f ? "auto local scale" : "manual",
			slayer_radar_scale.value );
		return;
	}

	arg = Cmd_Argv( 1 );
	if( !Q_stricmp( arg, "fit" ))
	{
		Cvar_SetValue( "slayer_radar_autoscale", 1.0f );
		Con_Printf( "radar zoom: automatic player-centred scale for current BSP\n" );
		return;
	}

	if( !Q_stricmp( arg, "in" ))        v = slayer_radar_scale.value - 200.0f;
	else if( !Q_stricmp( arg, "out" ))  v = slayer_radar_scale.value + 200.0f;
	else                                v = Q_atof( arg );

	Cvar_SetValue( "slayer_radar_autoscale", 0.0f );
	Cvar_SetValue( "slayer_radar_scale", bound( 300.0f, v, 6000.0f ));
	Con_Printf( "radar zoom: manual %.0f units across\n", slayer_radar_scale.value );
}

static void Cmd_RadarReset_f( void )
{
	Cvar_SetValue( "slayer_radar", 1.0f );
	Cvar_SetValue( "slayer_radar_x", 0.085f );
	Cvar_SetValue( "slayer_radar_y", 0.130f );
	Cvar_SetValue( "slayer_radar_size", 0.18f );
	Cvar_SetValue( "slayer_radar_scale", 1400.0f );
	Cvar_SetValue( "slayer_radar_autoscale", 1.0f );
	Cvar_SetValue( "slayer_radar_rotate", 1.0f );
	Cvar_SetValue( "slayer_radar_map", 1.0f );
	Cvar_SetValue( "slayer_radar_map_res", 512.0f );
	Cvar_SetValue( "slayer_radar_map_color", 2.0f );
	Cvar_SetValue( "slayer_radar_map_alpha", 215.0f );
	Cvar_SetValue( "slayer_radar_cone", 1.0f );
	Cvar_SetValue( "slayer_radar_cone_len", 0.55f );
	Cvar_SetValue( "slayer_radar_cone_alpha", 70.0f );
	Cvar_SetValue( "slayer_radar_dot", 1.0f );
	Cvar_SetValue( "slayer_radar_names", 1.0f );
	Cvar_SetValue( "slayer_radar_memory", 4.0f );
	Cvar_SetValue( "slayer_radar_height", 1.0f );
	Cvar_DirectSet( &slayer_radar_border, "" );
	Cvar_DirectSet( &slayer_radar_bg, "10 16 22 205" );

	Con_Printf( "radar settings restored to defaults\n" );
	Slayer_Radar_PrintSettings();
}

static void Cmd_RadarSettings_f( void )
{
	Slayer_Radar_PrintSettings();
	Con_Printf( "commands: slayer_radar_pos, slayer_radar_nudge, slayer_radar_setsize,\n" );
	Con_Printf( "          slayer_radar_zoom, slayer_radar_reset, slayer_radar_settings\n" );
}

void Slayer_Radar_Init( void )
{
	Cvar_RegisterVariable( &slayer_radar );
	Cvar_RegisterVariable( &slayer_radar_x );
	Cvar_RegisterVariable( &slayer_radar_y );
	Cvar_RegisterVariable( &slayer_radar_size );
	Cvar_RegisterVariable( &slayer_radar_size_migrated );
	Cvar_RegisterVariable( &slayer_radar_scale );
	Cvar_RegisterVariable( &slayer_radar_autoscale );
	Cvar_RegisterVariable( &slayer_radar_rotate );
	Cvar_RegisterVariable( &slayer_radar_map );
	Cvar_RegisterVariable( &slayer_radar_map_res );
	Cvar_RegisterVariable( &slayer_radar_map_color );
	Cvar_RegisterVariable( &slayer_radar_map_alpha );
	Cvar_RegisterVariable( &slayer_radar_border );
	Cvar_RegisterVariable( &slayer_radar_bg );
	Cvar_RegisterVariable( &slayer_radar_memory );
	Cvar_RegisterVariable( &slayer_radar_dot );
	Cvar_RegisterVariable( &slayer_radar_names );
	Cvar_RegisterVariable( &slayer_radar_height );
	Cvar_RegisterVariable( &slayer_radar_cone );
	Cvar_RegisterVariable( &slayer_radar_cone_len );
	Cvar_RegisterVariable( &slayer_radar_cone_alpha );

	Cmd_AddCommand( "slayer_radar_pos", Cmd_RadarPos_f,
		"Slayer3D: move the radar — corner preset (tl/tr/bl/br/top/bottom/center) or explicit x y fractions" );
	Cmd_AddCommand( "slayer_radar_nudge", Cmd_RadarNudge_f,
		"Slayer3D: shift the radar one step left/right/up/down (optional step fraction)" );
	Cmd_AddCommand( "slayer_radar_setsize", Cmd_RadarSize_f,
		"Slayer3D: radar diameter as a fraction of screen height, or + / - to step" );
	Cmd_AddCommand( "slayer_radar_zoom", Cmd_RadarZoom_f,
		"Slayer3D: world units across the radar, or in / out to step" );
	Cmd_AddCommand( "slayer_radar_reset", Cmd_RadarReset_f,
		"Slayer3D: restore every radar setting to its default" );
	Cmd_AddCommand( "slayer_radar_settings", Cmd_RadarSettings_f,
		"Slayer3D: print the current radar settings and the available commands" );

	// Preserve custom user sizes, but upgrade the old shipped 0.15 default.
	if( slayer_radar_size_migrated.value == 0.0f )
	{
		if( fabs( slayer_radar_size.value - 0.15f ) < 0.0001f )
			Cvar_SetValue( "slayer_radar_size", 0.18f );
		Cvar_SetValue( "slayer_radar_size_migrated", 1.0f );
	}

	Slayer_Radar_Reset();
}

void Slayer_Radar_Reset( void )
{
	memset( &s_map, 0, sizeof( s_map ));
	memset( s_sight, 0, sizeof( s_sight ));
	Slayer_RadarMap_Reset();
	// The client HUD re-enables its radar in InitHUDData on every map. Forget
	// ownership so the next active frame issues hideradar again.
	s_stock_hidden = false;
}

qboolean Slayer_Radar_IsEnabled( void )
{
	return ( slayer_radar.value != 0.0f ) ? true : false;
}

// Draw the map picture as a rotated, circle-clipped disc.
//
// Two problems the old code could not solve with R_DrawStretchPic:
//   1. ROTATION. The radar turns with the player, but a stretch-pic quad is
//      axis-aligned, so the picture stayed north-up under a rotating overlay.
//   2. CLIPPING. The previous "fix" re-painted the area outside the disc with
//      alpha 0, which blends nothing and therefore clipped nothing; the square
//      texture really was spilling past the rim.
// A triangle fan solves both: the fan IS the circle (no clipping needed), and
// each vertex carries the texture coordinate of the world point under it, so
// rotation is just where those coordinates are sampled from.
//
// This runs inside the 2D pass (R_Set2DMode true), where the projection maps
// screen pixels directly, so TriAPI vertices take screen coordinates.
static void Slayer_Radar_DrawMapDisc( int cx, int cy, int radius, int texnum,
	const vec3_t mins, const vec3_t maxs, const vec3_t focus_origin,
	float units_across, float cs, float sn, byte alpha )
{
#define SLAYER_RADAR_FAN 48
	float upp;   // world units per radar pixel
	int   i;

	if( texnum <= 0 || radius <= 0 || alpha == 0 )
		return;

	if( units_across < 64.0f ) units_across = 64.0f;
	upp = units_across / (float)( radius * 2 );

	ref.dllFuncs.TriRenderMode( kRenderTransTexture );
	ref.dllFuncs.GL_Bind( XASH_TEXTURE0, texnum );
	ref.dllFuncs.Color4f( 1.0f, 1.0f, 1.0f, (float)alpha / 255.0f );

	ref.dllFuncs.Begin( TRI_TRIANGLE_FAN );

	// SLAYER_RADAR_FAN + 2 vertices: the centre, then FAN + 1 rim points. The
	// last rim point repeats the first (angle 2*pi) to CLOSE the fan; without it
	// one wedge of the disc is missing and the map shows a pie-slice hole.
	for( i = 0; i <= SLAYER_RADAR_FAN + 1; i++ )
	{
		vec3_t p;
		float  ang, sx, sy, wx, wy, u, v;

		if( i == 0 )
		{
			// Centre vertex: the player's own position.
			sx = 0.0f;
			sy = 0.0f;
		}
		else
		{
			ang = (float)( i - 1 ) * ( 2.0f * (float)M_PI ) / (float)SLAYER_RADAR_FAN;
			sx = cosf( ang ) * (float)radius;
			sy = sinf( ang ) * (float)radius;
		}

		// Screen offset -> world offset: the exact inverse of the dot mapping
		// below (rx = wx*cs - wy*sn, ry = wx*sn + wy*cs, screen y = -ry), so the
		// picture under a blip is the world the blip is standing on. Derived,
		// not guessed: tests/radar_map_test.c round-trips both directions.
		wx = ( sx * cs - sy * sn ) * upp;
		wy = ( -sx * sn - sy * cs ) * upp;

		u = Slayer_RadarMap_WorldToU( focus_origin[0] + wx, mins[0], maxs[0] );
		v = Slayer_RadarMap_WorldToV( focus_origin[1] + wy, mins[1], maxs[1] );

		p[0] = (float)cx + sx;
		p[1] = (float)cy + sy;
		p[2] = 0.0f;

		gTriApi.TexCoord2f( u, v );
		ref.dllFuncs.Vertex3fv( p );
	}

	ref.dllFuncs.End();

	// Restore state. Two separate concerns, both real:
	//   * kRenderNormal is the only TriAPI mode that re-enables depth WRITES,
	//     and the TriAPI has no state stack (the same trap that made the
	//     viewmodel arms disappear behind the tracers);
	//   * but kRenderNormal also DISABLES blending, and we are inside the 2D
	//     pass where everything after us (rings, dots, avatars, text) expects
	//     alpha blending to work. R_DrawStretchPic does not set blend state
	//     itself, so leaving it off would make the next textured 2D draw opaque.
	// Hence: give depth writes back, then put the 2D pass back into its normal
	// translucent mode.
	ref.dllFuncs.TriRenderMode( kRenderNormal );
	ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
	ref.dllFuncs.Color4f( 1.0f, 1.0f, 1.0f, 1.0f );
	ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
#undef SLAYER_RADAR_FAN
}

void Slayer_Radar_Draw( void )
{
	cl_entity_t *focus;
	int    focus_index, focus_slot;
	vec3_t focus_origin;
	float  focus_yaw;
	int    screen_w, screen_h;
	int    cx, cy, radius;
	byte   border[3], bg[4];
	float  yaw_rad, cs, sn;
	float  units;
	float  map_span;
	int    map_tex = 0;
	vec3_t map_mins, map_maxs;
	int    i;
	double now = host.realtime;

	Slayer_Radar_SyncStock();
	if( !Slayer_Radar_IsEnabled() || cls.state != ca_active )
		return;
	if( cl.intermission )
		return;

	focus_index = Slayer_ObserverFocusIndex();
	focus_slot = focus_index - 1;
	focus = Slayer_ObserverFocusEntity();
	if( focus_slot < 0 || focus_slot >= MAX_CLIENTS )
		return;

	if( Slayer_ObserverFollowsPlayer() && focus )
	{
		VectorCopy( focus->origin, focus_origin );
		focus_yaw = focus->angles[YAW];
	}
	else
	{
		VectorCopy( cl.simorg, focus_origin );
		focus_yaw = cl.viewangles[YAW];
	}

	screen_w = refState.width;
	screen_h = refState.height;
	if( screen_w <= 0 || screen_h <= 0 )
		return;

	radius = (int)( screen_h * slayer_radar_size.value * 0.5f );
	if( radius < 24 ) radius = 24;
	if( radius > screen_h / 2 ) radius = screen_h / 2;

	cx = (int)( screen_w * slayer_radar_x.value );
	cy = (int)( screen_h * slayer_radar_y.value );

	Slayer_Radar_BorderColor( border );
	{
		int r = 10, g = 16, b = 22, a = 205;

		if( !COM_StringEmptyOrNULL( slayer_radar_bg.string ))
			sscanf( slayer_radar_bg.string, "%d %d %d %d", &r, &g, &b, &a );
		bg[0] = (byte)bound( 0, r, 255 );
		bg[1] = (byte)bound( 0, g, 255 );
		bg[2] = (byte)bound( 0, b, 255 );
		bg[3] = (byte)bound( 0, a, 255 );
	}

	// --- background + volume ------------------------------------------------
	// Three passes make the disc look recessed instead of pasted on:
	//   1. the body;
	//   2. a slightly darker ring just inside the rim = inner shadow;
	//   3. a bright 2px rim in the HUD colour = the lens edge.
	Slayer_Radar_Disc( cx, cy, radius, bg[0], bg[1], bg[2], bg[3] );
	Slayer_Radar_Ring( cx, cy, radius - 2, 4, 0, 0, 0, (byte)( bg[3] / 2 ), false );

	// --- transform ----------------------------------------------------------
	// Computed BEFORE the map is drawn: the picture and the dots must use one
	// rotation, otherwise the map is north-up under rotating blips.
	units = slayer_radar_scale.value;
	if( units < 64.0f ) units = 64.0f;

	// Rotate so the focus player's facing is up. In free spectator/map modes
	// the camera's yaw is the only meaningful direction.
	if( slayer_radar_rotate.value != 0.0f )
		yaw_rad = (float)DEG2RAD( -focus_yaw + 90.0f );
	else
		yaw_rad = (float)DEG2RAD( 90.0f );

	cs = cosf( yaw_rad );
	sn = sinf( yaw_rad );

	// --- map picture --------------------------------------------------------
	if( slayer_radar_map.value != 0.0f )
	{
		byte map_alpha_local = (byte)bound( 0, (int)slayer_radar_map_alpha.value, 255 );

		// Mode 1 (default): rasterize the CURRENT map from the BSP already in
		// memory. External overview images remain an explicit opt-in only.
		if( slayer_radar_map.value < 2.0f )
		{
			Slayer_RadarMap_Build( (int)slayer_radar_map_res.value,
				(int)slayer_radar_map_color.value );
			Slayer_RadarMap_Get( &map_tex, map_mins, map_maxs );
		}
		else
		{
			Slayer_Radar_LoadMap();
			if( s_map.texnum > 0 )
			{
				float half = ( 8192.0f / s_map.zoom ) * 0.5f;

				map_tex = s_map.texnum;
				map_mins[0] = s_map.origin[0] - half;
				map_mins[1] = s_map.origin[1] - half;
				map_mins[2] = 0.0f;
				map_maxs[0] = s_map.origin[0] + half;
				map_maxs[1] = s_map.origin[1] + half;
				map_maxs[2] = 0.0f;
			}
		}

		if( map_tex > 0 )
		{
			map_span = Q_max( map_maxs[0] - map_mins[0],
				map_maxs[1] - map_mins[1] );
			if( slayer_radar_autoscale.value != 0.0f && map_span > 64.0f )
			{
				// Player-centred minimap: choose a useful local radius from the
				// map size, but never move the player off centre or fit the whole BSP.
				units = bound( 1200.0f, map_span * 0.42f, 2600.0f );
			}
			if( units < 64.0f ) units = 64.0f;

			Slayer_Radar_DrawMapDisc( cx, cy, radius - 2, map_tex,
				map_mins, map_maxs, focus_origin, units, cs, sn, map_alpha_local );
		}
	}

	// Local/observed player is always fixed at the radar centre.
	// The cone is a map layer, so draw it before every marker and label.
	Slayer_Radar_DrawCone( cx, cy, radius );

	// Rim is drawn at the very end, after markers and labels.


	// --- transform ----------------------------------------------------------
	// Already computed above the map draw: both must share one rotation.

	Slayer_TeamColors_Update();

	// --- players ------------------------------------------------------------
	for( i = 0; i < MAX_CLIENTS; i++ )
	{
		cl_entity_t *ent;
		qboolean     ally, visible;
		vec3_t       rel;
		float        rx, ry;
		int          px, py, dot;
		byte         col[3];
		byte         alpha = 255;
		qboolean     from_memory = false;
		qboolean     at_edge = false;

		if( i == focus_slot )
			continue;
		if( Slayer_ObserverFollowsPlayer() && i == cl.playernum )
			continue;   // dead/spectator local slot is not a second map player
		if( !cl.players[i].name[0] )
			continue;

		ent = CL_GetEntityByIndex( i + 1 );
		ally = Slayer_TeamColors_IsAllyOf( focus_slot, i );
		visible = Slayer_Radar_CanSee( ent, focus_origin, focus );

		// Skip players whose side the server has not announced: without a side
		// there is no colour, and a grey blob on the radar is worse than none.
		if( Slayer_TeamColors_Side( i ) == SLAYER_TC_SIDE_NONE )
			continue;

		if( ally )
		{
			// Teammates are always known: the server shares their positions in
			// CS, and that is exactly what the vanilla radar shows.
			if( !ent || ent->curstate.messagenum != cl.parsecount )
				continue;
			VectorSubtract( ent->origin, focus_origin, rel );
		}
		else
		{
			// Enemies: only what has actually been seen. Record on sight, then
			// fade the remembered position out.
			if( visible )
			{
				s_sight[i].t_seen = now;
				s_sight[i].ever = true;
				VectorCopy( ent->origin, s_sight[i].pos );
				VectorSubtract( ent->origin, focus_origin, rel );
			}
			else
			{
				double age;

				if( !s_sight[i].ever )
					continue;

				age = now - s_sight[i].t_seen;
				if( age > slayer_radar_memory.value )
					continue;

				from_memory = true;
				if( slayer_radar_memory.value > 0.01f )
				{
					float f = 1.0f - (float)( age / slayer_radar_memory.value );

					alpha = (byte)bound( 20, (int)( f * 255.0f ), 255 );
				}
				VectorSubtract( s_sight[i].pos, focus_origin, rel );
			}
		}

		// world -> radar, with rotation
		rx = ( rel[0] * cs - rel[1] * sn ) / units * ( radius * 2.0f );
		ry = ( rel[0] * sn + rel[1] * cs ) / units * ( radius * 2.0f );

		px = cx + (int)rx;
		py = cy - (int)ry;   // screen Y grows downwards

		// Clamp to the rim so an off-radar player still shows a direction,
		// like CS2 does, instead of silently disappearing.
		{
			int ddx = px - cx, ddy = py - cy;
			float d = sqrtf( (float)( ddx * ddx + ddy * ddy ));
			int   lim = radius - 5;

			if( d > (float)lim && d > 0.01f )
			{
				px = cx + (int)( ddx * lim / d );
				py = cy + (int)( ddy * lim / d );
				alpha = (byte)( alpha * 3 / 5 );   // edge blips are dimmer
				at_edge = true;
			}
		}

		Slayer_TeamColors_Get( i, col );

		dot = (int)( radius * 0.085f * slayer_radar_dot.value );
		if( dot < 2 ) dot = 2;

		if( ally )
		{
			// Teammates: directional arrow with a dark outline. This keeps
			// orientation visible on a bright map and matches the reference radar.
			Slayer_Radar_Disc( px, py, dot + 2, 0, 0, 0, (byte)( alpha * 2 / 3 ));
			Slayer_Radar_PlayerArrow( px, py, dot, ent->angles[YAW], cs, sn,
				col[0], col[1], col[2], alpha );
		}
		else
		{
			// Enemies: filled ring while visible, hollow once remembered.
			Slayer_Radar_Ring( px, py, dot + 2, 2, 0, 0, 0, (byte)( alpha * 2 / 3 ), false );
			Slayer_Radar_Ring( px, py, dot + 1, 2, col[0], col[1], col[2], alpha,
				from_memory ? false : true );
		}

		if(( ally || !from_memory ) && !at_edge )
			Slayer_Radar_DrawName( px, py, dot, cl.players[i].name,
				col, alpha, cx, cy, radius );

		// Height arcs: another floor, above or below.
		if( slayer_radar_height.value != 0.0f )
		{
			float dz = ( ally || visible ) ? ( ent->origin[2] - focus_origin[2] )
			                               : ( s_sight[i].pos[2] - focus_origin[2] );

			if( dz > 64.0f )
				Slayer_Radar_HeightArc( px, py, dot + 2, true, col[0], col[1], col[2], alpha );
			else if( dz < -64.0f )
				Slayer_Radar_HeightArc( px, py, dot + 2, false, col[0], col[1], col[2], alpha );
		}
	}

	// --- focus marker -------------------------------------------------------
	// One marker only: local player while alive, observed target while following.
	// In a rotating radar it points straight up; in north-up it keeps real yaw.
	{
		int h = (int)( radius * 0.10f );
		byte focus_col[3];

		if( h < 3 ) h = 3;
		Slayer_TeamColors_Get( focus_slot, focus_col );
		Slayer_Radar_Disc( cx, cy, h + 2, 0, 0, 0, 190 );
		Slayer_Radar_Ring( cx, cy, h + 2, 2,
			focus_col[0], focus_col[1], focus_col[2], 235, false );
		Slayer_Radar_PlayerArrow( cx, cy, h, focus_yaw, cs, sn,
			255, 255, 255, 245 );

		if( Slayer_ObserverFollowsPlayer() )
			Slayer_Radar_DrawName( cx, cy, h + 2,
				cl.players[focus_slot].name, focus_col, 255, cx, cy, radius );
	}

	// Final lens edge. No marker, label or cone may paint across it.
	Slayer_Radar_Ring( cx, cy, radius, 2,
		border[0], border[1], border[2], 235, false );
	Slayer_Radar_Ring( cx, cy, radius - 2, 1,
		border[0], border[1], border[2], 90, false );
}
