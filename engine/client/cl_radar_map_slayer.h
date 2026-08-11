/*
cl_radar_map_slayer.h - Slayer3D universal radar map, rasterized from the BSP
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

#ifndef CL_RADAR_MAP_SLAYER_H
#define CL_RADAR_MAP_SLAYER_H

#include "xash3d_types.h"

// Rasterize a top-down picture of the CURRENT map from cl.worldmodel and upload
// it as a texture. Idempotent: the first successful call builds, later calls are
// free. Returns false while the world is not loaded yet (safe to call again) and
// also when the map genuinely cannot be rasterized (then it stops trying).
// `size` is the texture edge in pixels and is clamped to a power of two in
// [128, 1024].
qboolean Slayer_RadarMap_Build( int size );

// Texture handle and the WORLD bounds that texture covers. Returns false when
// nothing has been built. The bounds are the transform: they are what turns a
// world position into a texture coordinate, replacing the old overview
// ZOOM/ORIGIN pair.
qboolean Slayer_RadarMap_Get( int *texnum, vec3_t mins, vec3_t maxs );

// Drop the raster on map change / renderer restart. Does not call GL.
void Slayer_RadarMap_Reset( void );

// --- pure helpers, exposed for the harness -------------------------------
// A square texture over a rectangular map must cover the LONGER axis, otherwise
// the image is stretched and radar distances lie.
void Slayer_RadarMap_SquareBounds( const vec3_t mins, const vec3_t maxs,
	vec3_t out_mins, vec3_t out_maxs );

// World XY -> texture UV inside the given bounds. V is flipped so world +Y
// (north) is up on the radar.
float Slayer_RadarMap_WorldToU( float world_x, float min_x, float max_x );
float Slayer_RadarMap_WorldToV( float world_y, float min_y, float max_y );

#endif // CL_RADAR_MAP_SLAYER_H
