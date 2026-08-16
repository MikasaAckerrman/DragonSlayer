/*
cl_item_place_slayer.h - Slayer3D: visual placement solver for dropped items
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
#ifndef CL_ITEM_PLACE_SLAYER_H
#define CL_ITEM_PLACE_SLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

// WHAT THIS IS, AND WHAT IT IS NOT
//
// It is a PLACEMENT SEARCH: given where the server parked a dropped weapon and
// how big the weapon really is, find a pose (and a small visual offset) in which
// no part of the model is inside the map. It is not a rigid-body simulation:
// nothing here integrates, bounces, or falls. The answer is computed once, when
// the item comes to rest, and then kept.
//
// WHY THE PREVIOUS APPROACH COULD NOT GET THERE
//
// The toppling code answers "which way does this box tip", and that is a good
// answer to a different question. It reasons about ONE supporting plane, found by
// tracing down. So it cannot know that the far end of a rifle is inside a step,
// that the model is clipping a wall it never traced toward, or that the origin
// itself sits low enough that no rotation whatsoever keeps the mesh out of the
// floor. Reported symptoms, all of them the same missing check: parts of the
// weapon inside the floor, weapons half-sunk, weapons at angles that intersect
// the geometry next to them.
//
// So the test here is not "is the pose plausible" but "is the model OUTSIDE the
// world", evaluated for the whole box rather than for one probe.
//
// HOW A CORNER IS TESTED WITHOUT A SOLID-POINT QUERY
//
// The engine gives us a line trace, not a point-in-solid test. The trick is that
// a line trace answers the question anyway: trace from the item's CENTRE out to
// the corner, and if that ray hits a surface before it arrives, the corner is on
// the far side of that surface -- i.e. inside geometry. The depth is how much of
// the ray was cut off. `startsolid` on the same trace says the centre itself is
// buried, which is the case no rotation can fix and where the small lift below
// earns its place.
//
// THE VISUAL OFFSET, AND WHY IT IS ALLOWED HERE
//
// Everything else in this project refuses to move a dropped item, because the
// server owns pickup. That rule stands for anything LARGE. But a weapon whose
// mesh extends below its origin is drawn sunk into the floor no matter how it is
// rotated -- the only fix is to lift the drawing by a couple of units. The pickup
// volume around a CS weaponbox is tens of units wide, so a bounded nudge (default
// 6, hard-capped) cannot move the model out of the volume that picks it up, and
// it is applied to the RENDERED position only.

// A single trace, supplied by the caller. Returns 1 if the ray hit something.
//   `frac`       - fraction of the ray traversed before the hit (0..1)
//   `normal`     - surface normal at the hit
//   `startsolid` - the ray STARTED inside geometry
//
// Passed in as a callback so this file compiles with only <math.h> and can be
// driven against a synthetic world by tests/item_place_test.c. The engine wiring
// lives in cl_item_phys_slayer.c.
typedef int (*slayer_place_trace_fn)( void *ctx, const float *start, const float *end,
	float *out_frac, float *out_normal, int *out_startsolid );

typedef struct
{
	// Search shape.
	int   yaw_steps;       // rotations about the surface normal to try (>= 1)
	int   tilt_steps;      // tilts away from the surface to try (>= 1)
	float tilt_max;        // radians: largest tilt considered
	float max_lift;        // units: largest visual offset allowed along the normal
	float clearance;       // units: how far outside the surface a corner should sit
	float pen_tol;         // units: penetration this small is not worth fixing

	// Scoring weights. Penetration always dominates; these order the poses that
	// are all legal, so the chosen one also LOOKS supported instead of merely
	// being outside the world.
	float w_penetration;
	float w_support;       // reward corners resting near a surface
	float w_change;        // penalise turning the item away from how it landed
	float w_lift;          // penalise needing a nudge

	int   max_traces;      // hard budget for one solve
} slayer_place_params_t;

typedef struct
{
	int   solved;          // a pose with no penetration was found
	float orient[4];       // chosen pose, quaternion (x,y,z,w)
	float offset[3];       // visual offset to add to the render position
	float penetration;     // worst remaining penetration, units (0 = clean)
	int   contacts;        // corners resting on a surface in the chosen pose
	int   candidates;      // poses evaluated
	int   traces;          // traces spent
	float score;           // chosen pose's score, for diagnostics
} slayer_place_result_t;

// Defaults tuned in tests/item_place_test.c against real model extents.
void Slayer_Place_DefaultParams( slayer_place_params_t *p );

/*
Find a placement.

  half      - the item's REAL half-extents (Slayer_ModelExtent_Get), model space
  origin    - where the server put it
  orient_in - the pose it arrived in; candidate zero, so an already-good pose is
              returned untouched
  trace/ctx - the world
  out       - result; on failure `solved` is 0 and the LEAST BAD pose is still
              returned, because drawing the item somewhere is better than
              drawing it in the pose that was known to clip.

Deterministic: same inputs, same answer. That matters because this runs once per
item and the result is then frozen -- a search that wobbled would produce items
that look different every time they are dropped in the same place.
*/
void Slayer_Place_Solve( const float *half, const float *origin, const float *orient_in,
	slayer_place_trace_fn trace, void *ctx,
	const slayer_place_params_t *p, slayer_place_result_t *out );

// How deeply the box in this pose is inside the world, how many of its corners
// rest on something, and which way it would have to move to get out.
//
// `frame_normal` is the local surface normal (world up on open ground); it sets
// the direction in which `clearance` is measured, which matters more than it
// sounds -- see the note in the implementation.
//
// Exposed because it is the whole criterion: a test that only checked the search
// would not notice the measure itself going wrong. `out_push` may be NULL.
float Slayer_Place_Evaluate( const float *half, const float *origin, const float *orient,
	const float *frame_normal, slayer_place_trace_fn trace, void *ctx, float clearance,
	int *out_contacts, int *out_traces, float *out_push );

#ifdef __cplusplus
}
#endif

#endif // CL_ITEM_PLACE_SLAYER_H
