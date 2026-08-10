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

static CVAR_DEFINE_AUTO( slayer_radar_size, "0.15", FCVAR_ARCHIVE,
	"Slayer3D: radar diameter as a fraction of screen height" );

// World units mapped across the radar. Lower = more zoomed in, like CS2's
// cl_radar_scale. When the map has an overview script this scales it too.
static CVAR_DEFINE_AUTO( slayer_radar_scale, "1400", FCVAR_ARCHIVE,
	"Slayer3D: world units across the radar (smaller = closer zoom)" );

// 1 = radar rotates with the player (CS2 default), 0 = north-up.
static CVAR_DEFINE_AUTO( slayer_radar_rotate, "1", FCVAR_ARCHIVE,
	"Slayer3D: rotate the radar with the player view (0 = fixed north-up)" );

static CVAR_DEFINE_AUTO( slayer_radar_map, "1", FCVAR_ARCHIVE,
	"Slayer3D: draw the map texture inside the radar when overviews/<map>.bmp exists" );

static CVAR_DEFINE_AUTO( slayer_radar_map_alpha, "150", FCVAR_ARCHIVE,
	"Slayer3D: map texture opacity inside the radar, 0..255" );

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

static CVAR_DEFINE_AUTO( slayer_radar_height, "1", FCVAR_ARCHIVE,
	"Slayer3D: draw above/below height arcs on dots (0 = off)" );

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
static qboolean Slayer_Radar_CanSee( cl_entity_t *ent )
{
	vec3_t    eye, target;
	pmtrace_t tr;

	if( !ent || !ent->player )
		return false;
	if( ent->curstate.messagenum != cl.parsecount )
		return false;   // not in this snapshot -> not in our PVS

	VectorAdd( cl.simorg, cl.viewheight, eye );
	VectorCopy( ent->origin, target );
	target[2] += 20.0f;   // chest, not feet: feet are behind cover more often

	tr = CL_TraceLine( eye, target, PM_STUDIO_BOX );

	return ( tr.fraction > 0.98f ) ? true : false;
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

// ===========================================================================
// Public API
// ===========================================================================

void Slayer_Radar_Init( void )
{
	Cvar_RegisterVariable( &slayer_radar );
	Cvar_RegisterVariable( &slayer_radar_x );
	Cvar_RegisterVariable( &slayer_radar_y );
	Cvar_RegisterVariable( &slayer_radar_size );
	Cvar_RegisterVariable( &slayer_radar_scale );
	Cvar_RegisterVariable( &slayer_radar_rotate );
	Cvar_RegisterVariable( &slayer_radar_map );
	Cvar_RegisterVariable( &slayer_radar_map_alpha );
	Cvar_RegisterVariable( &slayer_radar_border );
	Cvar_RegisterVariable( &slayer_radar_bg );
	Cvar_RegisterVariable( &slayer_radar_memory );
	Cvar_RegisterVariable( &slayer_radar_dot );
	Cvar_RegisterVariable( &slayer_radar_height );

	Slayer_Radar_Reset();
}

void Slayer_Radar_Reset( void )
{
	memset( &s_map, 0, sizeof( s_map ));
	memset( s_sight, 0, sizeof( s_sight ));
}

qboolean Slayer_Radar_IsEnabled( void )
{
	return ( slayer_radar.value != 0.0f ) ? true : false;
}

void Slayer_Radar_Draw( void )
{
	int    screen_w, screen_h;
	int    cx, cy, radius;
	byte   border[3], bg[4];
	float  yaw_rad, cs, sn;
	float  units;
	int    i;
	double now = host.realtime;

	if( !Slayer_Radar_IsEnabled() || cls.state != ca_active )
		return;
	if( cl.intermission )
		return;

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

	// --- map texture --------------------------------------------------------
	if( slayer_radar_map.value != 0.0f )
	{
		Slayer_Radar_LoadMap();

		if( s_map.texnum > 0 )
		{
			// The overview transform maps world units to the full-screen
			// overview; we need world units to radar pixels, so the scale is
			// the ratio of the two. Drawn as a stretched quad clipped by the
			// rim ring that gets painted over it afterwards.
			float world_across = slayer_radar_scale.value;
			float px_per_unit;
			float ox, oy;
			int   mw;

			if( world_across < 64.0f ) world_across = 64.0f;
			px_per_unit = ( radius * 2.0f ) / world_across;

			// Player offset from the map centre, in radar pixels.
			ox = ( cl.simorg[0] - s_map.origin[0] ) * px_per_unit;
			oy = ( cl.simorg[1] - s_map.origin[1] ) * px_per_unit;

			// Full map width in radar pixels: the overview covers 8192 units
			// divided by its zoom (see CL_SetupOverviewParams).
			mw = (int)(( 8192.0f / s_map.zoom ) * px_per_unit );
			if( mw < 8 ) mw = 8;

			ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
			ref.dllFuncs.Color4ub( 255, 255, 255,
				(byte)bound( 0, (int)slayer_radar_map_alpha.value, 255 ));
			ref.dllFuncs.R_DrawStretchPic(
				(float)( cx - mw / 2 ) - ox, (float)( cy - mw / 2 ) + oy,
				(float)mw, (float)mw, 0, 0, 1, 1, s_map.texnum );
			ref.dllFuncs.Color4ub( 255, 255, 255, 255 );

			// Re-paint the outside of the disc so the square texture cannot
			// spill past the rim. Cheap alternative to a stencil.
			{
				int py;

				for( py = -radius - 2; py <= radius + 2; py++ )
				{
					float dy = (float)py + 0.5f;
					float span = (float)radius * (float)radius - dy * dy;
					int   half = ( span > 0.0f ) ? (int)sqrt( span ) : 0;
					int   outer = radius + 2;

					Slayer_Radar_Rect( cx - outer, cy + py, outer - half, 1, 0, 0, 0, 0 );
				}
			}
		}
	}

	// rim in HUD colour, drawn last so nothing covers it
	Slayer_Radar_Ring( cx, cy, radius, 2, border[0], border[1], border[2], 235, false );
	Slayer_Radar_Ring( cx, cy, radius - 2, 1, border[0], border[1], border[2], 90, false );

	// --- transform ----------------------------------------------------------
	units = slayer_radar_scale.value;
	if( units < 64.0f ) units = 64.0f;

	// Rotate so the player's facing is up. cl.viewangles, not the render angles:
	// in third person the camera may look elsewhere, and the radar must follow
	// the PLAYER (same reasoning as the third-person tracer aim).
	if( slayer_radar_rotate.value != 0.0f )
		yaw_rad = (float)DEG2RAD( -cl.viewangles[YAW] + 90.0f );
	else
		yaw_rad = (float)DEG2RAD( 90.0f );

	cs = cosf( yaw_rad );
	sn = sinf( yaw_rad );

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

		if( i == cl.playernum )
			continue;
		if( !cl.players[i].name[0] )
			continue;

		ent = CL_GetEntityByIndex( i + 1 );
		ally = Slayer_TeamColors_IsAlly( i );
		visible = Slayer_Radar_CanSee( ent );

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
			VectorSubtract( ent->origin, cl.simorg, rel );
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
				VectorSubtract( ent->origin, cl.simorg, rel );
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
				VectorSubtract( s_sight[i].pos, cl.simorg, rel );
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
			}
		}

		Slayer_TeamColors_Get( i, col );

		dot = (int)( radius * 0.085f * slayer_radar_dot.value );
		if( dot < 2 ) dot = 2;

		if( ally )
		{
			// Allies: solid dot with a dark outline so it stays readable on a
			// bright patch of the map texture.
			Slayer_Radar_Disc( px, py, dot + 1, 0, 0, 0, (byte)( alpha * 2 / 3 ));
			Slayer_Radar_Disc( px, py, dot, col[0], col[1], col[2], alpha );
		}
		else
		{
			// Enemies: filled ring while visible, hollow once remembered.
			Slayer_Radar_Ring( px, py, dot + 2, 2, 0, 0, 0, (byte)( alpha * 2 / 3 ), false );
			Slayer_Radar_Ring( px, py, dot + 1, 2, col[0], col[1], col[2], alpha,
				from_memory ? false : true );
		}

		// Height arcs: another floor, above or below.
		if( slayer_radar_height.value != 0.0f )
		{
			float dz = ( ally || visible ) ? ( ent->origin[2] - cl.simorg[2] )
			                               : ( s_sight[i].pos[2] - cl.simorg[2] );

			if( dz > 64.0f )
				Slayer_Radar_HeightArc( px, py, dot + 2, true, col[0], col[1], col[2], alpha );
			else if( dz < -64.0f )
				Slayer_Radar_HeightArc( px, py, dot + 2, false, col[0], col[1], col[2], alpha );
		}
	}

	// --- own marker ---------------------------------------------------------
	// A triangle pointing up (the radar is already rotated, so "up" is forward).
	{
		int k, h = (int)( radius * 0.11f );

		if( h < 3 ) h = 3;

		for( k = 0; k <= h; k++ )
		{
			int wide = ( h - k ) * 2 / 3 + 1;

			Slayer_Radar_Rect( cx - wide, cy - h / 2 + k, wide * 2, 1,
				255, 255, 255, 235 );
		}
	}
}
