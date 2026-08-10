/*
cl_tracer_render_slayer.c - Slayer3D own tracer geometry (ribbon renderer)
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

// Own tracer rendering. NOT the engine's beams.
//
// Why not R_BeamPoints: it draws one strip of constant width with a single
// colour for the whole beam (TriColor4f is called once per beam in
// R_BeamDraw). That is exactly what reads as a cheap server plugin. It also
// cannot do a second layer, a colour ramp along the length, or a screen-space
// width clamp.
//
// What this does instead, mirroring the tuned model from the tracer_lab2 bench:
//   * a RIBBON of SEGMENTS quads, brightness fading to zero at both ends by
//     per-vertex colour. A single quad clipped the alpha at the polygon edge,
//     which is what made the head look cut by a knife.
//   * the width axis is recomputed at every point along the ribbon. One shared
//     axis twists on a long streak, because the camera sees its two ends from
//     different angles.
//   * two layers: a wide dim HALO under a thin bright CORE. The halo replaces
//     bloom (the renderer has no FBO path: glGenFramebuffers is declared in
//     ref/gl/gl_export.h but never called anywhere in ref/gl/*.c).
//   * taper towards the tail -> a droplet shape instead of a rectangle.
//   * a head spark, so it reads as a flying bullet rather than a line.
//   * screen-space width clamp: below the minimum the streak is widened and
//     dimmed by the same factor, so distant tracers neither vanish into a
//     sub-pixel nor flicker.
//
// Cost: two draw calls per tracer (halo + core), (SEGMENTS+1)*2 vertices each,
// zero allocations per frame. All state lives in a fixed pool.

#include "common.h"
#include "client.h"
#include "ref_common.h"
#include "triangleapi.h"
#include "com_model.h"
#include "cl_tracer_slayer.h"
#include "cl_tracer_render_slayer.h"

// ===========================================================================
// Textures
// ===========================================================================
//
// The ribbon MUST be textured. Without a bound texture the strip is a flat
// quad with hard edges ACROSS its width -- the soft-end work along the length
// would be wasted, because the sides would still be knife-cut.
//
// The profile is baked as a 2D image: U runs across the streak, V along it.
//   * across: a narrow sharp core plus a wide low shoulder. The shoulder is
//     what keeps the edge from aliasing when the streak is 1-2 px wide.
//   * along: kept FLAT (1.0) here on purpose. The longitudinal fade is done
//     per-vertex, because its length is driven by cvars and re-baking a
//     texture on every cvar change is worse than 30 extra colour writes.
// The alpha channel carries the profile; TriRenderMode(kRenderTransAdd) uses
// GL_SRC_ALPHA, GL_ONE, so alpha attenuates what gets added.

#define SLAYER_TEX_W 64
#define SLAYER_TEX_H 4

static int s_tex_core = 0;
static int s_tex_halo = 0;

static void Slayer_BuildProfileTexture( const char *name, float core_sharp,
	float shoulder_w, float shoulder_sharp, int *out )
{
	uint pixels[SLAYER_TEX_W * SLAYER_TEX_H];
	int  x, y;

	for( y = 0; y < SLAYER_TEX_H; y++ )
	{
		for( x = 0; x < SLAYER_TEX_W; x++ )
		{
			float u = (( (float)x + 0.5f ) / (float)SLAYER_TEX_W ) * 2.0f - 1.0f;
			float a = expf( -u * u * core_sharp ) +
			          expf( -u * u * shoulder_sharp ) * shoulder_w;
			byte  alpha;

			if( a > 1.0f ) a = 1.0f;
			if( a < 0.0f ) a = 0.0f;
			alpha = (byte)( a * 255.0f );

			// White RGB; the colour comes from TriColor4f per vertex.
			pixels[y * SLAYER_TEX_W + x] = HostFourCC( 255, 255, 255, alpha );
		}
	}

	*out = ref.dllFuncs.GL_CreateTexture( name, SLAYER_TEX_W, SLAYER_TEX_H,
		pixels, TF_CLAMP | TF_NOMIPMAP | TF_HAS_ALPHA );
}

void Slayer_TracerRender_InitTextures( void )
{
	// Re-created on every renderer restart; GL_CreateTexture replaces by name.
	Slayer_BuildProfileTexture( "*slayer_tracer_core", 16.0f, 0.22f, 2.6f, &s_tex_core );
	Slayer_BuildProfileTexture( "*slayer_tracer_halo", 2.2f, 0.55f, 0.8f, &s_tex_halo );
}

void Slayer_TracerRender_FreeTextures( void )
{
	if( s_tex_core ) { ref.dllFuncs.GL_FreeTexture( s_tex_core ); s_tex_core = 0; }
	if( s_tex_halo ) { ref.dllFuncs.GL_FreeTexture( s_tex_halo ); s_tex_halo = 0; }
}

// Drop the cached texture numbers WITHOUT touching GL. Used on map change and
// after a renderer restart: at that point the old texnums may already be dead,
// so calling GL_FreeTexture on them is not safe. GL_CreateTexture replaces by
// name, so the next lazy init reuses the same slots instead of leaking.
void Slayer_TracerRender_Invalidate( void )
{
	s_tex_core = 0;
	s_tex_halo = 0;
}


// ===========================================================================
// Colour ramp along the streak
// ===========================================================================

// Tail is dark red, body glows, head is white-hot. Positions are normalised
// along the visible length: 0 = tail, 1 = head.
static const slayer_ramp_stop_t s_ramp_core[] =
{
	{ 0.00f, { 120,  22,   4 } },
	{ 0.22f, { 255,  68,  21 } },
	{ 0.55f, { 255, 165,  40 } },
	{ 1.00f, { 255, 250, 235 } }
};

static const slayer_ramp_stop_t s_ramp_halo[] =
{
	{ 0.00f, {  70,  14,   3 } },
	{ 0.30f, { 200,  60,  18 } },
	{ 0.70f, { 255, 150,  60 } },
	{ 1.00f, { 255, 230, 200 } }
};

#define RAMP_CORE_COUNT ( (int)( sizeof( s_ramp_core ) / sizeof( s_ramp_core[0] )))
#define RAMP_HALO_COUNT ( (int)( sizeof( s_ramp_halo ) / sizeof( s_ramp_halo[0] )))

void Slayer_Ramp_Sample( const slayer_ramp_stop_t *stops, int count, float t, float out[3] )
{
	int   i;
	float f;

	if( count <= 0 )
	{
		out[0] = out[1] = out[2] = 1.0f;
		return;
	}

	if( t <= stops[0].pos )
	{
		out[0] = stops[0].col[0] * ( 1.0f / 255.0f );
		out[1] = stops[0].col[1] * ( 1.0f / 255.0f );
		out[2] = stops[0].col[2] * ( 1.0f / 255.0f );
		return;
	}

	for( i = 0; i < count - 1; i++ )
	{
		if( t >= stops[i].pos && t <= stops[i + 1].pos )
		{
			float span = stops[i + 1].pos - stops[i].pos;

			f = ( span < 1e-6f ) ? 0.0f : ( t - stops[i].pos ) / span;
			out[0] = ( stops[i].col[0] + ( stops[i + 1].col[0] - stops[i].col[0] ) * f ) * ( 1.0f / 255.0f );
			out[1] = ( stops[i].col[1] + ( stops[i + 1].col[1] - stops[i].col[1] ) * f ) * ( 1.0f / 255.0f );
			out[2] = ( stops[i].col[2] + ( stops[i + 1].col[2] - stops[i].col[2] ) * f ) * ( 1.0f / 255.0f );
			return;
		}
	}

	out[0] = stops[count - 1].col[0] * ( 1.0f / 255.0f );
	out[1] = stops[count - 1].col[1] * ( 1.0f / 255.0f );
	out[2] = stops[count - 1].col[2] * ( 1.0f / 255.0f );
}

// ===========================================================================
// Longitudinal brightness profile -- the fix for the hard edge
// ===========================================================================

// smoothstep on both ends. With additive blending, multiplying RGB by 0 IS
// disappearance, so per-vertex colour is enough and no separate alpha channel
// per vertex is needed.
float Slayer_LengthProfile( float v, float soft_tail, float soft_head )
{
	float a = 1.0f, b = 1.0f;

	if( soft_tail > 1e-4f )
	{
		a = v / soft_tail;
		if( a > 1.0f ) a = 1.0f;
		if( a < 0.0f ) a = 0.0f;
		a = a * a * ( 3.0f - 2.0f * a );
	}

	if( soft_head > 1e-4f )
	{
		b = ( 1.0f - v ) / soft_head;
		if( b > 1.0f ) b = 1.0f;
		if( b < 0.0f ) b = 0.0f;
		b = b * b * ( 3.0f - 2.0f * b );
	}

	return a * b;
}

// ===========================================================================
// Screen-space width clamp
// ===========================================================================

// World units per screen pixel at `dist`. fov_y is the vertical field of view
// in degrees, screen_h the viewport height in pixels.
float Slayer_WorldPerPixel( float dist, float fov_y, int screen_h )
{
	float half;

	if( screen_h <= 0 ) return 1.0f;
	if( dist < 1.0f ) dist = 1.0f;

	half = tanf( DEG2RAD( fov_y ) * 0.5f );

	return ( 2.0f * dist * half ) / (float)screen_h;
}

// Returns the clamped half-width, and writes the brightness compensation into
// *dim. Below min_px the streak is widened to min_px and dimmed by the same
// ratio, so total energy stays put: a distant tracer gets faint instead of
// turning into a fat bar.
float Slayer_ClampWidth( float half_world, float wpp, float min_px, float max_px, float *dim )
{
	float px;

	*dim = 1.0f;
	if( wpp <= 0.0f ) return half_world;

	px = ( half_world * 2.0f ) / wpp;

	if( px < min_px && min_px > 0.0f )
	{
		*dim = px / min_px;
		if( *dim < 0.35f ) *dim = 0.35f;
		return min_px * wpp * 0.5f;
	}

	if( max_px > 0.0f && px > max_px )
		return max_px * wpp * 0.5f;

	return half_world;
}

// ===========================================================================
// Ribbon emission
// ===========================================================================

// Vertex parameter along the ribbon. NOT uniform on purpose.
//
// Measured: with 14 uniform segments and soft_head 0.10 the fade zone gets
// only 1.4 vertices, so the brightness jumps 0.80 between two neighbours --
// that IS a visible edge, just moved from the polygon border to the last
// segment. Bunching the samples towards the head puts ~3 vertices in the same
// zone and drops the worst jump to 0.47 without adding geometry.
// (Numbers from tests/tracer_render_test.c and the /tmp/vdist sweep.)
static float Slayer_RibbonParam( int i, int segments )
{
	float t = (float)i / (float)segments;

	// 1 - (1-t)^1.8: dense near t=1 (head), sparse at the tail where the fade
	// zone is already wide (soft_tail 0.5 covers half the streak).
	return 1.0f - powf( 1.0f - t, 1.8f );
}

// One layer of one tracer. `tail`/`head` are distances along `dir` from
// `origin`; the caller has already clamped them to the trace length.
//
// Vertex order is a triangle strip: for each step two vertices, left then
// right. That is one Begin/End per layer regardless of SEGMENTS.
static void Slayer_EmitRibbon( const vec3_t origin, const vec3_t dir,
	float tail, float head, float half_width, float taper,
	const vec3_t vieworg, const slayer_ramp_stop_t *ramp, int ramp_count,
	float gain, float soft_tail, float soft_head, int segments, int texnum )
{
	vec3_t seg, to_cam, wid, p1, p2;
	float  col[3];
	float  v, d, w, lp, len;
	int    i;

	len = head - tail;
	if( len < 0.5f || segments < 2 )
		return;

	ref.dllFuncs.GL_Bind( XASH_TEXTURE0, texnum );
	ref.dllFuncs.Begin( TRI_TRIANGLE_STRIP );

	for( i = 0; i <= segments; i++ )
	{
		v = Slayer_RibbonParam( i, segments );   // 0 = tail, 1 = head
		d = tail + len * v;

		VectorMA( origin, d, dir, seg );

		// Width axis per point: dir x (point -> camera). Recomputed here on
		// purpose; a single axis for the whole streak twists it.
		VectorSubtract( vieworg, seg, to_cam );
		CrossProduct( dir, to_cam, wid );
		if( VectorLength( wid ) < 1e-6f )
			VectorSet( wid, 0.0f, 0.0f, 1.0f );
		VectorNormalize( wid );

		w = half_width * ( 1.0f - taper * ( 1.0f - v ));
		lp = Slayer_LengthProfile( v, soft_tail, soft_head );

		Slayer_Ramp_Sample( ramp, ramp_count, v, col );

		// TriColor4f with alpha 1 in additive mode: gl_triapi multiplies rgb by
		// a and forces a=1, so brightness must be carried in RGB. That is also
		// why the fade profile lives in the colour and not in the alpha.
		ref.dllFuncs.Color4f( col[0] * lp * gain, col[1] * lp * gain,
			col[2] * lp * gain, 1.0f );

		VectorMA( seg,  w, wid, p1 );
		VectorMA( seg, -w, wid, p2 );

		// U across the streak (0 / 1), V along it. The profile texture is
		// 64x4 with the shape in U, so V only needs to stay inside the image.
		gTriApi.TexCoord2f( 0.0f, 0.5f );
		ref.dllFuncs.Vertex3fv( p1 );
		gTriApi.TexCoord2f( 1.0f, 0.5f );
		ref.dllFuncs.Vertex3fv( p2 );
	}

	ref.dllFuncs.End();
}

// A camera-facing quad for the head spark. Same additive mode; size is in
// world units so it shrinks with distance like everything else.
static void Slayer_EmitSpark( const vec3_t pos, float half, const vec3_t vieworg,
	const float col[3], float gain, int texnum )
{
	vec3_t to_cam, right, up, a, b, c, d;

	VectorSubtract( pos, vieworg, to_cam );
	if( VectorLength( to_cam ) < 1e-4f )
		return;
	VectorNormalize( to_cam );

	// Any two axes perpendicular to the view ray work for a billboard.
	VectorSet( up, 0.0f, 0.0f, 1.0f );
	CrossProduct( to_cam, up, right );
	if( VectorLength( right ) < 1e-4f )
		VectorSet( right, 1.0f, 0.0f, 0.0f );
	VectorNormalize( right );
	CrossProduct( right, to_cam, up );
	VectorNormalize( up );

	VectorScale( right, half, right );
	VectorScale( up, half, up );

	VectorAdd( pos, right, a ); VectorAdd( a, up, a );
	VectorSubtract( pos, right, b ); VectorAdd( b, up, b );
	VectorSubtract( pos, right, c ); VectorSubtract( c, up, c );
	VectorAdd( pos, right, d ); VectorSubtract( d, up, d );

	ref.dllFuncs.Color4f( col[0] * gain, col[1] * gain, col[2] * gain, 1.0f );

	// The halo profile doubles as the spark: a wide soft blob, which is what a
	// point of light looks like. Saves a third texture.
	ref.dllFuncs.GL_Bind( XASH_TEXTURE0, texnum );
	ref.dllFuncs.Begin( TRI_QUADS );
	gTriApi.TexCoord2f( 0.0f, 0.0f ); ref.dllFuncs.Vertex3fv( a );
	gTriApi.TexCoord2f( 1.0f, 0.0f ); ref.dllFuncs.Vertex3fv( b );
	gTriApi.TexCoord2f( 1.0f, 1.0f ); ref.dllFuncs.Vertex3fv( c );
	gTriApi.TexCoord2f( 0.0f, 1.0f ); ref.dllFuncs.Vertex3fv( d );
	ref.dllFuncs.End();
}

// ===========================================================================
// Public: draw one tracer
// ===========================================================================

void Slayer_TracerRender_Draw( const slayer_tracer_t *tr, const slayer_tracer_style_t *st,
	const vec3_t vieworg, float fov_y, int screen_h )
{
	vec3_t pos;
	float  travelled, head_d, tail_d;
	float  nt, gain, fade_in, fade_out, flick;
	float  cam_dist, wpp, dim, core_half, halo_half;
	float  head_col[3];

	if( !tr->active || tr->life <= 0.0f )
		return;

	nt = tr->age / tr->life;
	if( nt < 0.0f || nt >= 1.0f )
		return;

	// Head flies out and stops at the impact point; the tail keeps coming and
	// folds into it. No sudden disappearance.
	travelled = st->speed * tr->age;
	if( travelled > tr->dist ) travelled = tr->dist;
	head_d = travelled;
	tail_d = travelled - tr->length;
	if( tail_d < 0.0f ) tail_d = 0.0f;
	if(( head_d - tail_d ) < 0.5f )
		return;

	VectorMA( tr->start, head_d, tr->dir, pos );

	// Visible from the very first frame: the CS2 numbers (start alpha 0, fade
	// in at 20% of life) put first visibility 810 units downrange, which on
	// 1.6-sized maps reads as the streak appearing out of thin air.
	fade_in = 1.0f;
	if( st->fade_in_end > 1e-4f && nt < st->fade_in_end )
	{
		float f = nt / st->fade_in_end;

		f = f * f * ( 3.0f - 2.0f * f );
		fade_in = st->fade_in_floor + ( 1.0f - st->fade_in_floor ) * f;
	}

	fade_out = 1.0f;
	if( st->fade_out_start > 0.0f && nt > st->fade_out_start )
	{
		fade_out = 1.0f - ( nt - st->fade_out_start ) / ( 1.0f - st->fade_out_start );
		if( fade_out < 0.0f ) fade_out = 0.0f;
	}

	// Flicker: a steady brightness looks drawn. Two out-of-phase sines never
	// repeat visibly and cost two sinf per tracer per frame.
	flick = 1.0f;
	if( st->flicker > 0.0f )
	{
		float ph = tr->age * st->flicker_rate + tr->seed;

		flick = 1.0f + st->flicker * ( sinf( ph ) * 0.6f + sinf( ph * 2.37f ) * 0.4f );
	}

	gain = st->brightness * tr->gain * fade_in * fade_out * flick;
	if( gain <= 0.0f )
		return;

	cam_dist = 0.0f;
	{
		vec3_t delta;

		VectorSubtract( pos, vieworg, delta );
		cam_dist = VectorLength( delta );
	}

	wpp = Slayer_WorldPerPixel( cam_dist, fov_y, screen_h );
	core_half = Slayer_ClampWidth( tr->radius * 0.5f, wpp, st->min_px, st->max_px, &dim );
	halo_half = core_half * st->halo_scale;

	ref.dllFuncs.TriRenderMode( kRenderTransAdd );
	ref.dllFuncs.CullFace( TRI_NONE );

	// Lazily build the profile textures on the first tracer after a renderer
	// restart, so a vid_restart cannot leave us drawing untextured quads.
	if( !s_tex_core || !s_tex_halo )
		Slayer_TracerRender_InitTextures();

	// Halo first, core on top: additive is order-independent for colour, but
	// drawing the wide dim layer first keeps the bright core visually on top
	// when the depth test rejects part of the halo.
	if( st->halo_alpha > 0.0f )
	{
		Slayer_EmitRibbon( tr->start, tr->dir, tail_d, head_d, halo_half,
			st->taper * 0.6f, vieworg, s_ramp_halo, RAMP_HALO_COUNT,
			gain * dim * st->halo_alpha, st->soft_tail, st->soft_head,
			st->segments, s_tex_halo );
	}

	Slayer_EmitRibbon( tr->start, tr->dir, tail_d, head_d, core_half,
		st->taper, vieworg, s_ramp_core, RAMP_CORE_COUNT,
		gain * dim, st->soft_tail, st->soft_head, st->segments, s_tex_core );

	if( st->head_size > 0.0f )
	{
		float hs = core_half * st->head_size;

		Slayer_Ramp_Sample( s_ramp_core, RAMP_CORE_COUNT, 1.0f, head_col );
		Slayer_EmitSpark( pos, hs, vieworg, head_col,
			gain * dim * st->head_gain, s_tex_halo );
	}

	ref.dllFuncs.CullFace( TRI_FRONT );
}
