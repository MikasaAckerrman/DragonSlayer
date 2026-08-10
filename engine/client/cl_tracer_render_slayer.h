/*
cl_tracer_render_slayer.h - Slayer3D own tracer geometry (ribbon renderer)
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

#ifndef CL_TRACER_RENDER_SLAYER_H
#define CL_TRACER_RENDER_SLAYER_H

#include "xash3d_types.h"

// A colour stop of the ramp that runs ALONG the streak (0 = tail, 1 = head).
typedef struct
{
	float pos;
	byte  col[3];
} slayer_ramp_stop_t;

// One live tracer. Fixed-size pool, never allocated per shot.
typedef struct
{
	qboolean active;
	vec3_t   start;       // muzzle (already offset forward)
	vec3_t   dir;         // unit direction towards the impact point
	float    dist;        // muzzle -> impact distance
	float    length;      // world length of the visible streak
	float    radius;      // core diameter in world units
	float    age;         // seconds since spawn
	float    life;        // total lifetime in seconds
	float    gain;        // per-shot brightness (remote shots are boosted)
	float    seed;        // flicker phase, so two tracers never pulse in sync
} slayer_tracer_t;

// Style shared by every tracer; mirrors the cvars.
typedef struct
{
	float speed;
	float length;
	float radius;
	int   segments;
	float soft_tail;
	float soft_head;
	float taper;
	float halo_scale;
	float halo_alpha;
	float head_size;
	float head_gain;
	float flicker;
	float flicker_rate;
	float brightness;
	float remote_boost;
	float life_mul;
	float min_px;
	float max_px;
} slayer_tracer_style_t;

// Sample the ramp at t in [0..1]; out receives 0..1 floats.
void Slayer_Ramp_Sample( const slayer_ramp_stop_t *stops, int count, float t, float out[3] );

// Longitudinal brightness profile: smoothstep fade at both ends. This is what
// removes the hard edge a single quad used to have.
float Slayer_LengthProfile( float v, float soft_tail, float soft_head );

// World units per screen pixel at a distance, for the screen-space clamp.
float Slayer_WorldPerPixel( float dist, float fov_y, int screen_h );

// Clamp the half-width to [min_px, max_px] on screen. Writes the brightness
// compensation for the widened case into *dim.
float Slayer_ClampWidth( float half_world, float wpp, float min_px, float max_px, float *dim );

// Draw one tracer (halo + core ribbons + head spark) with the TriAPI.
// Call between R_PushScene/R_PopScene-equivalent points, i.e. from CL_DrawEFX.
void Slayer_TracerRender_Draw( const slayer_tracer_t *tr, const slayer_tracer_style_t *st,
	const vec3_t vieworg, float fov_y, int screen_h );

// Build / release the baked profile textures. Init is also called lazily on the
// first draw, so a vid_restart cannot leave the ribbons untextured.
void Slayer_TracerRender_InitTextures( void );
void Slayer_TracerRender_FreeTextures( void );

// Forget the cached texnums without calling GL. Use on map change / renderer
// restart, where the old handles may already be invalid.
void Slayer_TracerRender_Invalidate( void );

#endif // CL_TRACER_RENDER_SLAYER_H
