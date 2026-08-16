/*
cl_model_extent_engine.c - Slayer3D: cached real extents for a loaded model_t
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

// The engine-facing half of cl_model_extent_slayer.c, kept separate for the same
// reason cl_spin_phys_engine.c is: the measurement itself must stay compilable
// with only the C library so the host harness can run the real code against the
// real .mdl files. Everything that needs client.h lives here.
//
// WHY A CACHE, AND WHY BY POINTER
//
// Measuring walks the vertex array once: 62 vertices for a stock w_ak47, 10 917
// for the worst custom model measured (w_aa3.mdl). That is nothing once, and
// absurd per frame per entity -- and Slayer_ItemPhys_Apply runs for every visible
// loose item every frame.
//
// The key is the `model_t *`, with the model's name stored alongside it and
// verified on every hit. Pointers get REUSED across map changes (the model pool
// is reset and the next map's models land on the same addresses), so a pointer
// alone would silently hand out a shotgun's extents for a knife. Comparing the
// name catches that even if Slayer_ModelExtent_Reset is somehow not called.

#include "common.h"
#include "client.h"
#include "studio.h"
#include "cl_model_extent_slayer.h"
#include "cl_slayer_log.h"

#define ME_CACHE_MAX  96      // stock CS has 34 w_*.mdl; a mod server adds more

typedef struct
{
	struct model_s        *mod;
	char                   name[MAX_QPATH];
	slayer_model_extent_t  ext;
	qboolean               used;
} me_entry_t;

static me_entry_t me_cache[ME_CACHE_MAX];
static int        me_count;
static int        me_hits;
static int        me_misses;

void Slayer_ModelExtent_Reset( void )
{
	memset( me_cache, 0, sizeof( me_cache ));
	me_count  = 0;
	me_hits   = 0;
	me_misses = 0;
}

void Slayer_ModelExtent_Stats( int *out_models, int *out_hits, int *out_misses )
{
	if( out_models ) *out_models = me_count;
	if( out_hits )   *out_hits   = me_hits;
	if( out_misses ) *out_misses = me_misses;
}

const slayer_model_extent_t *Slayer_ModelExtent_Get( struct model_s *mod )
{
	studiohdr_t *phdr;
	me_entry_t  *slot = NULL;
	int          i;
	int          len;

	if( !mod )
		return NULL;

	for( i = 0; i < me_count; i++ )
	{
		if( !me_cache[i].used )
			continue;
		if( me_cache[i].mod != mod )
			continue;

		// Same pointer AND same name: a genuine hit. Same pointer with a different
		// name means the model pool was recycled under us -- rebuild that slot
		// rather than returning the previous map's answer.
		if( !Q_strcmp( me_cache[i].name, mod->name ))
		{
			me_hits++;
			return me_cache[i].ext.valid ? &me_cache[i].ext : NULL;
		}

		slot = &me_cache[i];
		break;
	}

	me_misses++;

	phdr = (studiohdr_t *)Mod_StudioExtradata( mod );
	if( !phdr )
		return NULL;

	// `length` is the studio header's own idea of its size, which is what
	// Mod_LoadStudioModel allocated. Using it (rather than a guess) is what makes
	// the bounds checks in the measurer meaningful.
	len = (int)phdr->length;
	if( len <= 0 )
		return NULL;

	if( !slot )
	{
		if( me_count >= ME_CACHE_MAX )
		{
			// Full. Measuring uncached every frame would be a real cost, so say so
			// once rather than quietly degrading.
			static qboolean warned;

			if( !warned )
			{
				warned = true;
				Slayer_Log_Printf( "model extents: cache full at %d models, '%s' not cached",
					ME_CACHE_MAX, mod->name );
			}
			return NULL;
		}
		slot = &me_cache[me_count++];
	}

	memset( slot, 0, sizeof( *slot ));
	slot->mod  = mod;
	slot->used = true;
	Q_strncpy( slot->name, mod->name, sizeof( slot->name ));

	if( !Slayer_ModelExtent_Measure( phdr, len, &slot->ext ))
	{
		Slayer_Log_Printf( "model extents: '%s' could not be measured (bones=%d bodyparts=%d) -- falling back to the engine hull",
			mod->name, phdr->numbones, phdr->numbodyparts );
		return NULL;
	}

	Slayer_Log_Printf( "model extents: '%s' mesh %.2f %.2f %.2f (long=%d short=%d) vs engine hull %.2f %.2f %.2f, %d verts / %d bones",
		mod->name,
		slot->ext.half[0] * 2.0f, slot->ext.half[1] * 2.0f, slot->ext.half[2] * 2.0f,
		slot->ext.long_axis, slot->ext.short_axis,
		mod->maxs[0] - mod->mins[0], mod->maxs[1] - mod->mins[1], mod->maxs[2] - mod->mins[2],
		slot->ext.nverts, slot->ext.nbones );

	return &slot->ext;
}
