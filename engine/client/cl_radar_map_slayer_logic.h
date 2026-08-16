/*
cl_radar_map_slayer_logic.h - pure cache key logic for the Slayer3D radar
Copyright (C) 2026 Slayer3D contributors
*/
#ifndef CL_RADAR_MAP_SLAYER_LOGIC_H
#define CL_RADAR_MAP_SLAYER_LOGIC_H

#include <string.h>

static int Slayer_RadarMap_CacheMatches( const void *cached_world,
	const char *cached_name, int cached_size, int cached_colour,
	const void *world, const char *name, int size, int colour )
{
	if( cached_world != world || cached_size != size || cached_colour != colour )
		return 0;
	if( !cached_name || !name )
		return cached_name == name;
	return strcmp( cached_name, name ) == 0;
}

#endif /* CL_RADAR_MAP_SLAYER_LOGIC_H */
