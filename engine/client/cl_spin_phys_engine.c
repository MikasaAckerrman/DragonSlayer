/*
cl_spin_phys_engine.c - engine-facing glue for the shared spin core
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

// One function, in its own translation unit, for a specific reason.
//
// cl_spin_phys_slayer.c must not include the engine headers: keeping it to
// <math.h> is what lets tests/spin_phys_test.c compile and exercise THE REAL
// FILE on the host, and the dynamics are the part worth asserting on.
//
// But converting the core's quaternion into the angles the renderer wants IS
// engine work, and it is needed by every consumer (grenades, dropped items, and
// whatever comes next). Putting it in one of the consumers would make the others
// depend on that consumer; copying it into each would let the pitch-flip
// compensation drift apart between them, and that compensation is subtle enough
// that it already cost one round of "grenades still tumble wrong".

#include "common.h"
#include "client.h"
#include "xash3d_mathlib.h"
#include "cl_spin_phys_slayer.h"

void Slayer_Spin_PoseToAngles( const slayer_spin_t *st, vec3_t out_angles )
{
	vec4_t q;

	if( !st || !out_angles )
		return;

	// The core stores a plain float[4] in the same (x,y,z,w) layout the engine's
	// AngleQuaternion produces, so this is a copy rather than a conversion.
	Vector4Copy( st->orient, q );
	QuaternionAngle( q, out_angles );

	// UNDO THE RENDERER'S PITCH FLIP.
	//
	// R_StudioSetUpTransform does `angles[PITCH] = -angles[PITCH]` before
	// building its matrix (the inherited "stupid quake bug"), so the orientation
	// actually rendered is a MIRRORED version of the one computed here. A
	// mirrored rotation is not a rotation: the model turns the wrong way about
	// one axis and flips whenever pitch crosses zero. Measured against the
	// renderer's own matrix code: worst element error 1.99 passing the angles
	// through as-is, 0.00 when pitch is pre-negated.
	//
	// Guarded by the same feature bit the renderer checks, so an engine or mod
	// that fixed the bug is not double-corrected.
	if( !FBitSet( host.features, ENGINE_COMPENSATE_QUAKE_BUG ))
		out_angles[PITCH] = -out_angles[PITCH];
}
