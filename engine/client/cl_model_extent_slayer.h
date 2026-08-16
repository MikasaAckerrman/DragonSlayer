/*
cl_model_extent_slayer.h - Slayer3D: the REAL size and axes of a studio model
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
#ifndef CL_MODEL_EXTENT_SLAYER_H
#define CL_MODEL_EXTENT_SLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

// WHY THIS EXISTS -- and it is not a refinement, it is a correction.
//
// The item-resting code needs two facts about a dropped weapon: how long it is
// (so probes can straddle it) and which of its LOCAL axes is the short one (an
// object comes to rest on its largest face, i.e. with its shortest extent facing
// the surface). Both were taken from `model->mins/maxs`, and both were wrong.
//
// MEASURED, over the 34 w_*.mdl files of a real CS installation plus 25 custom
// downloads (tests/mdl_engine_box.py, tests/mdl_extents.py):
//
//     every one of them takes the `verts` branch in Mod_LoadStudioModel:
//       bbmin/bbmax are zero AND min/max are zero, so the engine calls
//       Mod_StudioComputeBounds(ignore_sequences) and RoundUpHullSize
//
//     engine box short axis != real mesh short axis:  24 of 34 stock
//                                                     24 of 25 custom
//     engine box long  axis != real mesh long  axis:  23 of 34
//     worst per-axis size error:                      2564 %  (w_scout, Z)
//
//     w_ak47:  engine 2.44 12.31 36.65   real mesh 35.44 15.36 2.44
//
// The cause is not sloppiness in the header, it is systematic:
// Mod_StudioComputeBounds bounds RAW vertex positions, i.e. positions in BONE
// space, without walking the bone chain. A CS world model has a single bone
// rotated about 90 degrees, so its axes come out permuted for essentially every
// model. Then Mod_StudioAccumulateBoneVerts folds the origin into the box and
// RoundUpHullSize snaps the corners to the hull table.
//
// Consequence for us: the previous code pressed the WRONG axis against the
// surface on 24 models out of 34, and used a length three times too short for
// the probes. That is the measured cause of both live complaints -- "a dropped
// weapon straightens itself out on contact" and "resting on an edge does not
// work". No amount of tolerance or easing-rate tuning could have fixed it.
//
// So the extents are measured from the FILE: vertices transformed through their
// bones' reference pose, which is the pose the model is drawn in when nothing is
// animating it. Custom downloads are therefore handled by construction -- a
// replaced model of any size is measured, not looked up in a table. Cost: one
// pass per model, cached by model pointer, never repeated per frame.
//
// The parser is deliberately independent of the engine headers (only <math.h>),
// so tests/model_extent_test.c can run it against the actual .mdl files on disk
// and compare with an independent Python implementation.

typedef struct
{
	int   valid;        // 0 = could not be measured; every field below is unusable
	float mins[3];      // model-space bounds of the posed mesh
	float maxs[3];
	float half[3];      // ( maxs - mins ) / 2, per axis
	float center[3];    // ( maxs + mins ) / 2
	int   long_axis;    // index of the largest extent  (0=X 1=Y 2=Z)
	int   short_axis;   // index of the smallest extent
	int   nverts;       // vertices measured, for diagnostics
	int   nbones;
} slayer_model_extent_t;

struct model_s;

// Measure a studio model held in memory. `data` points at the studiohdr_t,
// `len` is the number of bytes readable there -- every offset is checked against
// it, because a truncated or hostile .mdl must not read out of bounds.
//
// Returns 1 on success. On failure `out` is zeroed with valid == 0, and the
// caller is expected to fall back to the engine's hull rather than to garbage.
int Slayer_ModelExtent_Measure( const void *data, int len, slayer_model_extent_t *out );

// The same, for a `model_t *`, with a cache: measuring is a single pass over the
// vertex array, but there is no reason to repeat it per frame per entity.
//
// Implemented in cl_model_extent_engine.c, because reaching the studio data
// requires the engine (Mod_StudioExtradata) and this header has to stay usable
// from the host tests. Returns NULL if the model has no studio data.
//
// The returned pointer is owned by the cache and stays valid until
// Slayer_ModelExtent_Reset().
const slayer_model_extent_t *Slayer_ModelExtent_Get( struct model_s *mod );

// Drop the cache. Called on map change, because model pointers are reused.
void Slayer_ModelExtent_Reset( void );

// Cache statistics for diagnostics: how many models measured, how many lookups
// hit. A silently-empty cache and a silently-thrashing one look identical from
// the outside otherwise.
void Slayer_ModelExtent_Stats( int *out_models, int *out_hits, int *out_misses );

#ifdef __cplusplus
}
#endif

#endif // CL_MODEL_EXTENT_SLAYER_H
