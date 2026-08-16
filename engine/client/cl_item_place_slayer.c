/*
cl_item_place_slayer.c - Slayer3D: visual placement solver for dropped items
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

// See cl_item_place_slayer.h for what this is for. Implementation notes:
//
// * NO ENGINE HEADERS, only <math.h> and <string.h>, so tests/item_place_test.c
//   drives the real code against a synthetic world where the right answer is
//   known by construction. The world arrives as a trace callback.
//
// * The search is a small deterministic sweep, not an optimiser. Twelve yaws by
//   four tilts is 48 candidate poses; each costs at most 8 corner traces, and the
//   whole thing runs ONCE when an item settles. Measured budget below.
//
// * Scoring is lexicographic in spirit: penetration outweighs everything, so no
//   amount of "looks nice" can buy a pose that clips. Among poses that do not
//   clip, the tie-breakers are support, then similarity to how it landed, then
//   how much lift it needed.

#include <math.h>
#include <string.h>

#include "cl_item_place_slayer.h"

#define PL_EPS  1e-4f

// ---------------------------------------------------------------------------
// Local vector / quaternion helpers (same shapes as cl_spin_phys_slayer.c, kept
// local so neither file depends on the other's internals)
// ---------------------------------------------------------------------------

static void PL_VecCopy( const float *a, float *out )
{
	out[0] = a[0]; out[1] = a[1]; out[2] = a[2];
}

static void PL_VecClear( float *out )
{
	out[0] = out[1] = out[2] = 0.0f;
}

static void PL_VecMA( const float *a, float s, const float *b, float *out )
{
	out[0] = a[0] + s * b[0];
	out[1] = a[1] + s * b[1];
	out[2] = a[2] + s * b[2];
}

static float PL_VecLen( const float *a )
{
	return (float)sqrt( (double)( a[0] * a[0] + a[1] * a[1] + a[2] * a[2] ));
}

static float PL_VecNormalize( float *a )
{
	float len = PL_VecLen( a );

	if( len > PL_EPS )
	{
		float inv = 1.0f / len;

		a[0] *= inv; a[1] *= inv; a[2] *= inv;
	}
	return len;
}

static void PL_VecCross( const float *a, const float *b, float *out )
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static void PL_QuatIdentity( float *q )
{
	q[0] = q[1] = q[2] = 0.0f;
	q[3] = 1.0f;
}

static void PL_QuatNormalize( float *q )
{
	float len = (float)sqrt( (double)( q[0] * q[0] + q[1] * q[1]
		+ q[2] * q[2] + q[3] * q[3] ));

	if( len < 1e-8f )
	{
		PL_QuatIdentity( q );
		return;
	}
	len = 1.0f / len;
	q[0] *= len; q[1] *= len; q[2] *= len; q[3] *= len;
}

// out = a * b, i.e. "apply b, then a"
static void PL_QuatMul( const float *a, const float *b, float *out )
{
	out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void PL_QuatFromAxisAngle( const float *axis, float theta, float *q )
{
	float n[3];
	float half, s;

	PL_VecCopy( axis, n );
	if( PL_VecNormalize( n ) <= 0.0f || theta == 0.0f )
	{
		PL_QuatIdentity( q );
		return;
	}

	half = theta * 0.5f;
	s = (float)sin( (double)half );

	q[0] = n[0] * s;
	q[1] = n[1] * s;
	q[2] = n[2] * s;
	q[3] = (float)cos( (double)half );
}

static void PL_QuatRotate( const float *q, const float *v, float *out )
{
	float t[3], u[3];

	PL_VecCross( q, v, t );
	t[0] *= 2.0f; t[1] *= 2.0f; t[2] *= 2.0f;
	PL_VecCross( q, t, u );

	out[0] = v[0] + q[3] * t[0] + u[0];
	out[1] = v[1] + q[3] * t[1] + u[1];
	out[2] = v[2] + q[3] * t[2] + u[2];
}

// Angle between two orientations, as 1 - |dot|: 0 when identical (including the
// double-cover case q and -q), 1 when 180 degrees apart. Used as the "how far is
// this from how it landed" term, so an absolute angle is unnecessary.
static float PL_QuatDistance( const float *a, const float *b )
{
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];

	if( d < 0.0f ) d = -d;
	if( d > 1.0f ) d = 1.0f;
	return 1.0f - d;
}

static void PL_VecScale( const float *a, float s, float *out )
{
	out[0] = a[0] * s;
	out[1] = a[1] * s;
	out[2] = a[2] * s;
}

// ---------------------------------------------------------------------------
// Scoring and bookkeeping
// ---------------------------------------------------------------------------

/*
====================
PL_Score

Lower is better.

Penetration is weighted so heavily (1000 per unit by default, against tens for
everything else) that NO combination of the other terms can prefer a pose that
clips. That ordering is the player's priority list, in the order they gave it:

  1. nothing inside geometry            -> w_penetration, dominant
  2. resting on something, not floating -> w_support, rewards contacts
  3. close to how it landed             -> w_change, so poses do not flip about
  4. moved as little as possible        -> w_lift, so a nudge is a last resort

The change term is always measured against the pose the item LANDED in, never
against the current best -- comparing against the best would make the search
drift toward whatever it evaluated first, and the answer would depend on
iteration order instead of on the world.
====================
*/
static float PL_Score( const slayer_place_params_t *p, float pen, int contacts,
	float moved, const float *pose, const float *landed )
{
	float s = p->w_penetration * pen
		- p->w_support * (float)contacts
		+ p->w_lift * moved;

	if( pose && landed )
		s += p->w_change * PL_QuatDistance( pose, landed );

	return s;
}

// One evaluation, with the trace accounting kept in one place.
static float PL_Try( const float *half, const float *pos, const float *pose,
	const float *frame_normal,
	slayer_place_trace_fn trace, void *ctx, const slayer_place_params_t *p,
	int *out_contacts, int *out_traces, float *out_push )
{
	return Slayer_Place_Evaluate( half, pos, pose, frame_normal, trace, ctx,
		p->clearance, out_contacts, out_traces, out_push );
}

// Record a candidate as the current answer.
static void PL_Commit( slayer_place_result_t *out, const float *pose, const float *offset,
	float pen, int contacts, float score, const slayer_place_params_t *p )
{
	int i;

	for( i = 0; i < 4; i++ )
		out->orient[i] = pose[i];

	if( offset )
		PL_VecCopy( offset, out->offset );
	else
		PL_VecClear( out->offset );

	out->penetration = pen;
	out->contacts = contacts;
	out->score = score;
	out->solved = ( pen <= p->pen_tol );
}

// ---------------------------------------------------------------------------
// Measuring one pose
// ---------------------------------------------------------------------------

void Slayer_Place_DefaultParams( slayer_place_params_t *p )
{
	if( !p )
		return;

	// 12 yaws x 4 tilts. Enough to find a way out of a step or a slot (30 degrees
	// of yaw is finer than the visual difference between neighbouring poses at
	// these sizes) and small enough to be free: the solve happens once per item.
	//
	// tilt_max is a full 90 degrees on purpose. Anything less cannot express "on
	// its edge", and on-its-edge is the only pose a rifle has in a slot narrower
	// than the rifle is wide -- which is one of the cases in the report.
	p->yaw_steps  = 12;
	p->tilt_steps = 4;
	p->tilt_max   = 1.5707963f;  // 90 degrees

	p->max_lift   = 6.0f;        // units; see the header on why any lift at all
	p->clearance  = 0.5f;        // units outside the surface a corner should sit
	p->pen_tol    = 0.25f;       // units; below this nothing is worth moving for

	p->w_penetration = 1000.0f;  // dominates every other term by construction
	p->w_support     = 8.0f;
	p->w_change      = 40.0f;
	p->w_lift        = 6.0f;

	p->max_traces = 512;
}

/*
====================
Slayer_Place_Evaluate

How far inside the world is this box, how many corners rest on something, and
which way would it have to move to get out?

TWO QUESTIONS, TWO PROBES PER CORNER, because one probe cannot answer both.

  1. IS THE CORNER INSIDE GEOMETRY? Trace from the centre to the corner. A hit
     before arriving means the corner is on the far side of a surface, and the
     unfinished part of the ray is how deep. A line trace is all the engine
     offers, and aimed outward from the centre it answers a containment question.

  2. IS THE CORNER RESTING ON SOMETHING? Trace from the corner a short way ALONG
     THE INWARD NORMAL of the local surface. A hit means there is a face directly
     under it.

The obvious economy -- one trace, overshooting past the corner -- was tried and it
does not work for the shape this module exists for. A rifle's bottom corner sits
at (17.72, 7.68, -1.22): the ray to it is 19.35 long and only 6 % of that is
vertical, so any modest overshoot along the ray descends almost nothing and cannot
see a floor 0.2 below. Measured with that version: zero contacts for a rifle
lying flat on the ground, which then scored no better than one floating in the
air. Correcting it by extending the ray instead would probe 8 units sideways and
find walls that have nothing to do with the corner.

`startsolid` on the first probe means the centre itself is inside something. Then
every corner is buried and no rotation helps; the full half-diagonal is reported
as the depth so such poses lose decisively.

`out_push` sums, for every penetrating corner, the surface normal scaled by that
corner's depth: the direction and distance that takes the box OUT. It cancels
itself out when the box is wedged between two opposing surfaces -- exactly the
case where no translation helps and the pose has to change instead.
====================
*/
float Slayer_Place_Evaluate( const float *half, const float *origin, const float *orient,
	const float *frame_normal, slayer_place_trace_fn trace, void *ctx, float clearance,
	int *out_contacts, int *out_traces, float *out_push )
{
	float worst = 0.0f;
	float push[3];
	float n[3] = { 0.0f, 0.0f, 1.0f };
	int   contacts = 0;
	int   traces = 0;
	int   sx, sy, sz;

	PL_VecClear( push );

	if( out_contacts ) *out_contacts = 0;
	if( out_traces )   *out_traces = 0;
	if( out_push )     PL_VecClear( out_push );

	if( !half || !origin || !orient || !trace )
		return 0.0f;

	if( frame_normal && PL_VecLen( frame_normal ) > 0.1f )
	{
		PL_VecCopy( frame_normal, n );
		PL_VecNormalize( n );
	}

	for( sx = -1; sx <= 1; sx += 2 )
	{
		for( sy = -1; sy <= 1; sy += 2 )
		{
			for( sz = -1; sz <= 1; sz += 2 )
			{
				float local[3], world[3], corner[3];
				float frac = 1.0f, normal[3] = { 0.0f, 0.0f, 0.0f };
				int   startsolid = 0;
				float reach, depth;

				local[0] = half[0] * (float)sx;
				local[1] = half[1] * (float)sy;
				local[2] = half[2] * (float)sz;

				PL_QuatRotate( orient, local, world );

				reach = PL_VecLen( world );
				if( reach <= PL_EPS )
					continue;          // degenerate axis, nothing to test

				PL_VecMA( origin, 1.0f, world, corner );

				// --- probe 1: containment ---------------------------------
				traces++;
				if( trace( ctx, origin, corner, &frac, normal, &startsolid ))
				{
					if( startsolid )
					{
						if( reach > worst )
							worst = reach;
						continue;      // buried; a contact probe would be meaningless
					}

					depth = ( 1.0f - frac ) * reach;
					if( depth > 0.0f )
					{
						if( depth > worst )
							worst = depth;

						PL_VecMA( push, depth, normal, push );
						continue;      // inside, so not resting on anything
					}
				}

				// --- probe 2: is a surface just under this corner? ---------
				{
					float under[3];
					float f2 = 1.0f, n2[3] = { 0.0f, 0.0f, 0.0f };
					int   ss2 = 0;

					PL_VecMA( corner, -clearance, n, under );

					traces++;
					if( trace( ctx, corner, under, &f2, n2, &ss2 ))
						contacts++;
				}
			}
		}
	}

	if( out_contacts ) *out_contacts = contacts;
	if( out_traces )   *out_traces = traces;
	if( out_push )     PL_VecCopy( push, out_push );

	return worst;
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

/*
====================
PL_SurfaceNormal

Which way is "up" for this item's surroundings?

Traced straight down from the origin, and if nothing is below (an item on the lip
of a ledge, or one the server placed in the air), world up is used. This is only
the FRAME the search rotates in -- getting it approximately right is enough,
because the search tries tilts away from it in every direction anyway.
====================
*/
/*
====================
PL_SurfaceNormal

Which way is "up" for this item's surroundings?

Traced straight down from the origin. If nothing is below (an item on the lip of
a ledge, or one the server placed in the air) world up is used. This is only the
FRAME the search rotates in -- approximately right is enough, because the search
tries tilts away from it in every direction anyway.

`startsolid` gets the same answer as "nothing found": when the origin is already
inside geometry the trace reports no normal, and world up is the correct guess for
getting OUT of a floor, which is the case that produces it.
====================
*/
static void PL_SurfaceNormal( const float *origin, float probe_len,
	slayer_place_trace_fn trace, void *ctx, float *out_normal, int *traces )
{
	float end[3];
	float frac = 1.0f, normal[3] = { 0.0f, 0.0f, 0.0f };
	int   startsolid = 0;

	out_normal[0] = 0.0f;
	out_normal[1] = 0.0f;
	out_normal[2] = 1.0f;

	PL_VecCopy( origin, end );
	end[2] -= probe_len;

	( *traces )++;
	if( !trace( ctx, origin, end, &frac, normal, &startsolid ))
		return;

	if( startsolid )
		return;

	if( PL_VecLen( normal ) > 0.1f )
	{
		PL_VecCopy( normal, out_normal );
		PL_VecNormalize( out_normal );
	}
}

/*
====================
PL_BasisFromNormal

Two axes perpendicular to `n`, for building yaw rotations about it.

The seed axis is chosen as the world axis LEAST aligned with the normal, which
keeps the cross product well-conditioned no matter which way the surface faces --
picking a fixed seed makes the basis degenerate on walls.
====================
*/
static void PL_BasisFromNormal( const float *n, float *out_a, float *out_b )
{
	float seed[3] = { 0.0f, 0.0f, 0.0f };
	int   axis = 0;
	int   i;

	for( i = 1; i < 3; i++ )
	{
		float ai = n[i] < 0.0f ? -n[i] : n[i];
		float ab = n[axis] < 0.0f ? -n[axis] : n[axis];

		if( ai < ab )
			axis = i;
	}
	seed[axis] = 1.0f;

	PL_VecCross( n, seed, out_a );
	if( PL_VecNormalize( out_a ) <= PL_EPS )
	{
		out_a[0] = 1.0f; out_a[1] = 0.0f; out_a[2] = 0.0f;
	}

	PL_VecCross( n, out_a, out_b );
	PL_VecNormalize( out_b );
}

void Slayer_Place_Solve( const float *half, const float *origin, const float *orient_in,
	slayer_place_trace_fn trace, void *ctx,
	const slayer_place_params_t *p, slayer_place_result_t *out )
{
	slayer_place_params_t pp;
	float h[3];
	float surf[3], axis_a[3], axis_b[3];
	float base[4];
	float reach;
	float best_score = 0.0f;
	int   traces = 0;
	int   candidates = 0;
	int   yaw_i, tilt_i;
	int   i;

	if( !out )
		return;

	memset( out, 0, sizeof( *out ));
	PL_QuatIdentity( out->orient );

	if( !half || !origin || !trace )
		return;

	if( p ) pp = *p;
	else    Slayer_Place_DefaultParams( &pp );

	// Sanitise: these reach us from cvars.
	if( pp.yaw_steps  < 1 )  pp.yaw_steps  = 1;
	if( pp.yaw_steps  > 32 ) pp.yaw_steps  = 32;
	if( pp.tilt_steps < 1 )  pp.tilt_steps = 1;
	if( pp.tilt_steps > 12 ) pp.tilt_steps = 12;
	if( pp.tilt_max   < 0.0f ) pp.tilt_max = 0.0f;
	if( pp.max_lift   < 0.0f ) pp.max_lift = 0.0f;
	if( pp.max_lift   > 32.0f ) pp.max_lift = 32.0f;   // hard cap: see the header
	if( pp.clearance  < 0.05f ) pp.clearance = 0.05f;
	if( pp.pen_tol    < 0.0f ) pp.pen_tol = 0.0f;
	if( pp.max_traces < 16 )   pp.max_traces = 16;
	if( pp.max_traces > 4096 ) pp.max_traces = 4096;

	for( i = 0; i < 3; i++ )
	{
		h[i] = half[i] < 0.0f ? -half[i] : half[i];
		if( h[i] < 0.01f ) h[i] = 0.01f;   // a zero axis makes every corner degenerate
	}

	reach = (float)sqrt( (double)( h[0] * h[0] + h[1] * h[1] + h[2] * h[2] ));

	PL_SurfaceNormal( origin, reach * 2.0f + 16.0f, trace, ctx, surf, &traces );
	PL_BasisFromNormal( surf, axis_a, axis_b );

	if( orient_in )
	{
		for( i = 0; i < 4; i++ )
			base[i] = orient_in[i];
		PL_QuatNormalize( base );
	}
	else
	{
		PL_QuatIdentity( base );
	}

	// --- candidate zero: the pose it arrived in -----------------------------
	//
	// Evaluated first and with no change penalty, so an item that landed cleanly
	// is returned untouched. That is the property that keeps this from becoming
	// another servo: the common case costs nine traces and changes nothing.
	{
		float pen, push[3];
		int   contacts = 0, t = 0;

		pen = PL_Try( h, origin, base, surf, trace, ctx, &pp,
			&contacts, &t, push );
		traces += t;
		candidates++;

		best_score = PL_Score( &pp, pen, contacts, 0.0f, base, base );
		PL_Commit( out, base, NULL, pen, contacts, best_score, &pp );

		if( out->solved )
		{
			out->candidates = candidates;
			out->traces = traces;
			return;
		}

		// --- candidate one: same pose, pushed out along the contact normals --
		//
		// Before rotating anything, try simply moving the model out of whatever it
		// is inside. This is the fix for the case the player described as "half
		// inside the texture": a weapon whose mesh extends below its origin is
		// drawn sunk no matter how it is turned, and a two-unit nudge is both the
		// correct answer and the least intrusive one. The push direction comes from
		// the surfaces themselves, so it works against a wall as well as a floor,
		// and it cancels out when the item is wedged between two opposing faces --
		// which is precisely where a rotation is needed instead.
		//
		// WHEN THE CENTRE ITSELF IS BURIED there are no normals to sum: every
		// corner trace reports startsolid and reports nothing about direction. That
		// is not an edge case, it is the exact situation in the video -- the server
		// parks a weaponbox with its origin below the floor plane. The fallback is
		// the surface normal traced downward, which defaults to world up, i.e.
		// "out of the ground". Without this the only case that REQUIRES a nudge is
		// the one case that never gets one.
		if( pp.max_lift > 0.0f )
		{
			float dir[3];
			int   step;

			if( PL_VecLen( push ) > PL_EPS )
			{
				PL_VecCopy( push, dir );
				PL_VecNormalize( dir );
			}
			else
			{
				PL_VecCopy( surf, dir );
			}

			for( step = 1; step <= 3; step++ )
			{
				float dist = pp.max_lift * (float)step / 3.0f;
				float pos[3];
				float score, pen2, push2[3];
				int   c2 = 0, t2 = 0;

				if( traces + 8 > pp.max_traces )
					break;

				PL_VecMA( origin, dist, dir, pos );

				pen2 = PL_Try( h, pos, base, surf, trace, ctx, &pp,
					&c2, &t2, push2 );
				traces += t2;
				candidates++;

				score = PL_Score( &pp, pen2, c2, dist, base, base );
				if( score < best_score )
				{
					float off[3];

					best_score = score;
					PL_VecScale( dir, dist, off );
					PL_Commit( out, base, off, pen2, c2, score, &pp );
				}

				if( pen2 <= pp.pen_tol )
					break;
			}

			if( out->solved )
			{
				out->candidates = candidates;
				out->traces = traces;
				return;
			}
		}
	}

	// --- sweep: rotations, each with the same push refinement ---------------
	//
	// Ordered tilt-major so the flat poses (which is what most items should end up
	// in) are all tried before any steep one. The trace budget cuts the sweep from
	// the end, so the order decides what survives a tight budget.
	for( tilt_i = 0; tilt_i < pp.tilt_steps; tilt_i++ )
	{
		float tilt = ( pp.tilt_steps > 1 )
			? pp.tilt_max * (float)tilt_i / (float)( pp.tilt_steps - 1 )
			: 0.0f;

		for( yaw_i = 0; yaw_i < pp.yaw_steps; yaw_i++ )
		{
			float yaw = 6.2831853f * (float)yaw_i / (float)pp.yaw_steps;
			float q_yaw[4], q_tilt[4], q_pose[4];
			float tilt_axis[3];
			float pen, push[3], score;
			int   contacts = 0, t = 0;
			int   step;

			if( traces + 8 > pp.max_traces )
				goto done;

			// Tilt about an axis that lies IN the surface, rotated by yaw. This
			// parameterisation covers "lying flat, turned any way" at tilt 0 and
			// "leaning in any direction" further out, which is the space a dropped
			// weapon actually occupies.
			tilt_axis[0] = axis_a[0] * (float)cos( (double)yaw ) + axis_b[0] * (float)sin( (double)yaw );
			tilt_axis[1] = axis_a[1] * (float)cos( (double)yaw ) + axis_b[1] * (float)sin( (double)yaw );
			tilt_axis[2] = axis_a[2] * (float)cos( (double)yaw ) + axis_b[2] * (float)sin( (double)yaw );
			PL_VecNormalize( tilt_axis );

			PL_QuatFromAxisAngle( surf, yaw, q_yaw );
			PL_QuatFromAxisAngle( tilt_axis, tilt, q_tilt );
			PL_QuatMul( q_tilt, q_yaw, q_pose );
			PL_QuatNormalize( q_pose );

			pen = PL_Try( h, origin, q_pose, surf, trace, ctx, &pp,
				&contacts, &t, push );
			traces += t;
			candidates++;

			score = PL_Score( &pp, pen, contacts, 0.0f, q_pose, base );
			if( score < best_score )
			{
				best_score = score;
				PL_Commit( out, q_pose, NULL, pen, contacts, score, &pp );
			}

			// This rotation still clips: try pushing it out, same as above.
			if( pen > pp.pen_tol && PL_VecLen( push ) > PL_EPS && pp.max_lift > 0.0f )
			{
				float dir[3];

				PL_VecCopy( push, dir );
				PL_VecNormalize( dir );

				for( step = 1; step <= 2; step++ )
				{
					float dist = pp.max_lift * (float)step * 0.5f;
					float pos[3];
					float pen2, push2[3], score2;
					int   c2 = 0, t2 = 0;

					if( traces + 8 > pp.max_traces )
						goto done;

					PL_VecMA( origin, dist, dir, pos );

					pen2 = PL_Try( h, pos, q_pose, surf, trace, ctx, &pp,
						&c2, &t2, push2 );
					traces += t2;
					candidates++;

					score2 = PL_Score( &pp, pen2, c2, dist, q_pose, base );
					if( score2 < best_score )
					{
						float off[3];

						best_score = score2;
						PL_VecScale( dir, dist, off );
						PL_Commit( out, q_pose, off, pen2, c2, score2, &pp );
					}

					if( pen2 <= pp.pen_tol )
						break;
				}
			}

			// Clean AND resting: good enough. Continuing would trade a pose the
			// player will accept for a marginally better score.
			if( out->solved && out->contacts > 0 )
				goto done;
		}
	}

done:
	out->candidates = candidates;
	out->traces = traces;
}
