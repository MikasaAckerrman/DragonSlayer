/*
cl_radar_map_slayer.c - Slayer3D universal radar map, rasterized from the BSP
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

// WHY this module exists.
//
// The first radar took its picture from the engine's overview system
// (overviews/<map>.txt + <map>.bmp). That is an EXTERNAL, per-map asset that
// somebody has to produce by hand with `dev_overview 1` plus a screenshot. It
// therefore fails at the one thing the radar is for: joining an arbitrary
// server on an arbitrary map and seeing the map. Worse, a stale de_dust pair on
// disk gets drawn on maps that are not de_dust, because the file name is the
// only thing tying the image to the world.
//
// The map geometry is already in memory: cl.worldmodel holds every BSP surface
// with its vertices and plane. So we render our own top-down raster once per
// map and upload it as a texture. No assets, no server cooperation, works on
// every map including custom ones.
//
// Rules that make the result readable (each one is a decision, not a detail):
//   * SKY surfaces are skipped -- they are the lid over the level.
//   * DOWNWARD-facing surfaces (ceilings) are skipped. Without this rule every
//     room is covered by its own ceiling and the radar is a flat slab.
//   * Per-pixel HIGHEST-z wins. That is what "seen from above" means, and it is
//     what makes an upper floor correctly hide the floor below it.
//   * Walls are kept and drawn dark. Their top-down footprint is a thin sliver,
//     which is exactly the outline that turns a blob of floor into a map.
//   * Brightness of a floor comes from its height, so stairs and multi-level
//     areas are distinguishable rather than one uniform grey.
//
// The transform is derived from the model's real bounds, NOT from the overview
// script's ZOOM/ORIGIN and not from the hard-coded 8192 the old code used.

#include "common.h"
#include "client.h"
#include "ref_common.h"
#include "mod_local.h"          // world.version, QBSP2_VERSION
#include "cl_radar_map_slayer.h"
#include "cl_slayer_log.h"

// Maximum vertices of a single BSP face we are willing to rasterize. Real faces
// are small; the cap only exists so a corrupt lump cannot walk off a stack array.
#define SLAYER_MAP_MAXVERTS 128

typedef struct
{
	qboolean built;        // attempted for the current map
	int      texnum;       // 0 = nothing to draw
	int      size;         // texture edge in pixels
	vec3_t   mins, maxs;   // world bounds the texture covers
	int      surfaces_used;
} slayer_radar_raster_t;

static slayer_radar_raster_t s_raster;
static qboolean s_warned_no_world;

// ===========================================================================
// Pure geometry helpers (mirrored in tests/radar_map_test.c)
// ===========================================================================

void Slayer_RadarMap_SquareBounds( const vec3_t mins, const vec3_t maxs, vec3_t out_mins, vec3_t out_maxs )
{
	float cx = ( mins[0] + maxs[0] ) * 0.5f;
	float cy = ( mins[1] + maxs[1] ) * 0.5f;
	float w = maxs[0] - mins[0];
	float h = maxs[1] - mins[1];
	float half;

	// A square texture over a non-square map must cover the LONGER axis, or the
	// picture is stretched and every distance on the radar lies. Padding keeps
	// wall outlines at the very edge from being clipped by texture clamping.
	half = (( w > h ) ? w : h ) * 0.5f;
	if( half < 64.0f ) half = 64.0f;
	half *= 1.02f;

	out_mins[0] = cx - half;
	out_mins[1] = cy - half;
	out_mins[2] = mins[2];
	out_maxs[0] = cx + half;
	out_maxs[1] = cy + half;
	out_maxs[2] = maxs[2];
}

float Slayer_RadarMap_WorldToU( float world_x, float min_x, float max_x )
{
	float span = max_x - min_x;

	if( span < 1e-3f )
		return 0.5f;
	return ( world_x - min_x ) / span;
}

float Slayer_RadarMap_WorldToV( float world_y, float min_y, float max_y )
{
	float span = max_y - min_y;

	if( span < 1e-3f )
		return 0.5f;
	// V is flipped: world +Y is north (up on the radar), texture V grows down.
	return 1.0f - ( world_y - min_y ) / span;
}

// ===========================================================================
// Rasterizer
// ===========================================================================

typedef struct
{
	byte  *rgba;     // size*size*4
	float *height;   // size*size, -inf = untouched
	int    size;
	vec3_t mins, maxs;
} slayer_map_canvas_t;

static void Slayer_MapCanvas_Plot( slayer_map_canvas_t *c, int x, int y, float z,
	byte r, byte g, byte b )
{
	int i;

	if( x < 0 || y < 0 || x >= c->size || y >= c->size )
		return;

	i = y * c->size + x;

	// Highest surface wins: this single test is what gives a correct top-down
	// view of a multi-storey map.
	if( c->rgba[i * 4 + 3] != 0 && z <= c->height[i] )
		return;

	c->height[i] = z;
	c->rgba[i * 4 + 0] = r;
	c->rgba[i * 4 + 1] = g;
	c->rgba[i * 4 + 2] = b;
	c->rgba[i * 4 + 3] = 255;
}

// Convex-polygon scanline fill in texture space. BSP faces are convex by
// construction, so min/max x per scanline is the whole span.
static void Slayer_MapCanvas_FillPoly( slayer_map_canvas_t *c, const float *px, const float *py,
	int count, float z, byte r, byte g, byte b )
{
	float ymin = px ? py[0] : 0.0f, ymax = ymin;
	int   i, y;

	if( count < 3 )
		return;

	for( i = 1; i < count; i++ )
	{
		if( py[i] < ymin ) ymin = py[i];
		if( py[i] > ymax ) ymax = py[i];
	}

	if( ymax < 0.0f || ymin > (float)( c->size - 1 ))
		return;

	for( y = (int)floor( ymin ); y <= (int)ceil( ymax ); y++ )
	{
		float cy = (float)y + 0.5f;
		float xmin = 0.0f, xmax = 0.0f;
		int   hits = 0;
		int   x;

		if( y < 0 || y >= c->size )
			continue;

		for( i = 0; i < count; i++ )
		{
			int   j = ( i + 1 ) % count;
			float y0 = py[i], y1 = py[j];
			float t, xi;

			if(( y0 <= cy && y1 <= cy ) || ( y0 > cy && y1 > cy ))
				continue;
			if( fabs( y1 - y0 ) < 1e-6f )
				continue;

			t = ( cy - y0 ) / ( y1 - y0 );
			xi = px[i] + ( px[j] - px[i] ) * t;

			if( !hits ) { xmin = xmax = xi; }
			else
			{
				if( xi < xmin ) xmin = xi;
				if( xi > xmax ) xmax = xi;
			}
			hits++;
		}

		if( hits < 2 )
			continue;

		// A wall seen from above is a zero-width span. Widen degenerate spans to
		// a single pixel so the outline survives instead of vanishing between
		// two scanlines.
		if( xmax - xmin < 1.0f )
			xmax = xmin + 1.0f;

		for( x = (int)floor( xmin ); x <= (int)ceil( xmax - 0.001f ); x++ )
			Slayer_MapCanvas_Plot( c, x, y, z, r, g, b );
	}
}

static mvertex_t *Slayer_MapVertex( model_t *mod, int surfedge )
{
	int lindex = mod->surfedges[surfedge];

	// Same 16/32-bit edge selection the loader uses (Mod_GetVertexByNumber).
	if( world.version == QBSP2_VERSION )
	{
		if( lindex > 0 )
			return &mod->vertexes[mod->edges32[lindex].v[0]];
		return &mod->vertexes[mod->edges32[-lindex].v[1]];
	}

	if( lindex > 0 )
		return &mod->vertexes[mod->edges16[lindex].v[0]];
	return &mod->vertexes[mod->edges16[-lindex].v[1]];
}

static void Slayer_RadarMap_RasterizeSurfaces( slayer_map_canvas_t *c, model_t *mod )
{
	float z_span = c->maxs[2] - c->mins[2];
	int   i;

	if( z_span < 1.0f ) z_span = 1.0f;

	for( i = 0; i < mod->numsurfaces; i++ )
	{
		msurface_t *surf = &mod->surfaces[i];
		float       px[SLAYER_MAP_MAXVERTS], py[SLAYER_MAP_MAXVERTS];
		vec3_t      normal;
		float       top_z = -999999.0f;
		float       shade;
		byte        r, g, b;
		int         v;

		if( surf->numedges < 3 || surf->numedges > SLAYER_MAP_MAXVERTS )
			continue;
		if( FBitSet( surf->flags, SURF_DRAWSKY ))
			continue;   // the lid over the level
		if( !surf->plane )
			continue;

		VectorCopy( surf->plane->normal, normal );
		if( FBitSet( surf->flags, SURF_PLANEBACK ))
			VectorNegate( normal, normal );

		// Ceilings would cover everything below them.
		if( normal[2] < -0.5f )
			continue;

		for( v = 0; v < surf->numedges; v++ )
		{
			mvertex_t *mv = Slayer_MapVertex( mod, surf->firstedge + v );

			px[v] = Slayer_RadarMap_WorldToU( mv->position[0], c->mins[0], c->maxs[0] ) * (float)c->size;
			py[v] = Slayer_RadarMap_WorldToV( mv->position[1], c->mins[1], c->maxs[1] ) * (float)c->size;
			if( mv->position[2] > top_z )
				top_z = mv->position[2];
		}

		shade = ( top_z - c->mins[2] ) / z_span;
		if( shade < 0.0f ) shade = 0.0f;
		if( shade > 1.0f ) shade = 1.0f;

		if( normal[2] > 0.5f )
		{
			// Walkable-ish: light, and brighter the higher it sits, so stairs and
			// upper floors separate instead of merging into one grey field.
			r = (byte)( 66.0f + 74.0f * shade );
			g = (byte)( 76.0f + 82.0f * shade );
			b = (byte)( 92.0f + 92.0f * shade );
		}
		else
		{
			// Wall: dark. Drawn with the wall's TOP z so it wins the depth test
			// against the floor it stands on and reads as an outline.
			r = 26; g = 31; b = 40;
		}

		Slayer_MapCanvas_FillPoly( c, px, py, surf->numedges, top_z, r, g, b );
		s_raster.surfaces_used++;
	}
}

// ===========================================================================
// Public API
// ===========================================================================

void Slayer_RadarMap_Reset( void )
{
	// No GL_FreeTexture: on a map change the texture table is rebuilt and the
	// old handle may already be dead. Same reasoning as the tracer profile
	// textures. Forgetting the handle is enough.
	memset( &s_raster, 0, sizeof( s_raster ));
	s_warned_no_world = false;
}

qboolean Slayer_RadarMap_Get( int *texnum, vec3_t mins, vec3_t maxs )
{
	if( !s_raster.built || s_raster.texnum <= 0 )
		return false;

	if( texnum ) *texnum = s_raster.texnum;
	if( mins ) VectorCopy( s_raster.mins, mins );
	if( maxs ) VectorCopy( s_raster.maxs, maxs );
	return true;
}

qboolean Slayer_RadarMap_Build( int size )
{
	slayer_map_canvas_t canvas;
	model_t *mod = cl.worldmodel;
	double   t0 = Sys_DoubleTime();
	size_t   pixels;
	int      i;

	if( s_raster.built )
		return ( s_raster.texnum > 0 ) ? true : false;

	// Mark as attempted FIRST: a map we cannot rasterize must not be retried
	// every frame.
	s_raster.built = true;
	s_raster.texnum = 0;
	s_raster.surfaces_used = 0;

	if( !mod || mod->numsurfaces <= 0 || !mod->vertexes || !mod->surfedges )
	{
		// Log ONCE, not per frame: this path runs every frame until the world is
		// ready, and the user already had to deal with our log spam once.
		if( !s_warned_no_world )
		{
			s_warned_no_world = true;
			Slayer_Log_Printf( "radar map: world geometry not ready yet (surfaces=%d) -- plain disc for now",
				mod ? mod->numsurfaces : -1 );
		}
		s_raster.built = false;   // world not loaded yet; try again next frame
		return false;
	}

	s_warned_no_world = false;

	if( size < 128 ) size = 128;
	if( size > 1024 ) size = 1024;

	// Power of two: some GLES drivers refuse to clamp/filter NPOT textures.
	for( i = 128; i <= 1024; i <<= 1 )
	{
		if( size <= i ) { size = i; break; }
	}

	Slayer_RadarMap_SquareBounds( mod->mins, mod->maxs, s_raster.mins, s_raster.maxs );

	pixels = (size_t)size * (size_t)size;
	canvas.size = size;
	canvas.rgba = (byte *)Mem_Calloc( host.mempool, pixels * 4 );
	canvas.height = (float *)Mem_Calloc( host.mempool, pixels * sizeof( float ));
	VectorCopy( s_raster.mins, canvas.mins );
	VectorCopy( s_raster.maxs, canvas.maxs );

	if( !canvas.rgba || !canvas.height )
	{
		if( canvas.rgba ) Mem_Free( canvas.rgba );
		if( canvas.height ) Mem_Free( canvas.height );
		Slayer_Log_Printf( "radar map: out of memory for %dx%d raster", size, size );
		return false;
	}

	Slayer_RadarMap_RasterizeSurfaces( &canvas, mod );

	// TF_CLAMP matters: the radar samples outside [0,1] near the map edge and a
	// repeating wrap would paste the far side of the map next to the player.
	s_raster.texnum = ref.dllFuncs.GL_CreateTexture( "*slayer_radar_map", size, size,
		canvas.rgba, TF_CLAMP | TF_NOMIPMAP | TF_HAS_ALPHA );
	s_raster.size = size;

	Mem_Free( canvas.rgba );
	Mem_Free( canvas.height );

	Slayer_Log_Printf( "radar map: built %dx%d from '%s' surfaces=%d/%d "
		"bounds=(%.0f %.0f)..(%.0f %.0f) tex=%d in %.0f ms",
		size, size, mod->name, s_raster.surfaces_used, mod->numsurfaces,
		s_raster.mins[0], s_raster.mins[1], s_raster.maxs[0], s_raster.maxs[1],
		s_raster.texnum, ( Sys_DoubleTime() - t0 ) * 1000.0 );

	return ( s_raster.texnum > 0 ) ? true : false;
}
