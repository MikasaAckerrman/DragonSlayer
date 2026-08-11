/*
cl_spin_phys_slayer.c - Slayer3D shared rotational dynamics for loose objects
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

// See cl_spin_phys_slayer.h for what this models and what it deliberately does
// not. Implementation notes that matter:
//
// * NO ENGINE HEADERS. Only <math.h>. The engine's vector helpers are macros
//   over vec3_t and would drag in common.h, which cannot be compiled on a host
//   without the whole tree. Keeping this file standalone means the real file --
//   not a copy of it -- is compiled and exercised by tests/spin_phys_test.c.
//   The cost is a handful of local vector helpers, which is cheap next to being
//   able to assert on the dynamics.
//
// * Angular velocity is integrated as a quaternion, with the increment applied
//   on the LEFT, because omega is expressed in world space. Right-multiplying
//   would treat it as body-local and silently produce a different (wrong)
//   motion once the object is no longer near identity.
//
// * Everything is clamped. A frame where the server teleports an object gives a
//   velocity difference of thousands of units; without clamps that single frame
//   becomes an unrecoverable blur.

#include <math.h>
#include "cl_spin_phys_slayer.h"

// =============================================================================
// Local vector / quaternion helpers
// =============================================================================

static void SP_VecCopy( const float *a, float *out )
{
	out[0] = a[0]; out[1] = a[1]; out[2] = a[2];
}

static void SP_VecClear( float *out )
{
	out[0] = out[1] = out[2] = 0.0f;
}

static void SP_VecSub( const float *a, const float *b, float *out )
{
	out[0] = a[0] - b[0];
	out[1] = a[1] - b[1];
	out[2] = a[2] - b[2];
}

static void SP_VecScale( const float *a, float s, float *out )
{
	out[0] = a[0] * s;
	out[1] = a[1] * s;
	out[2] = a[2] * s;
}

static void SP_VecMA( const float *a, float s, const float *b, float *out )
{
	out[0] = a[0] + s * b[0];
	out[1] = a[1] + s * b[1];
	out[2] = a[2] + s * b[2];
}

static float SP_VecDot( const float *a, const float *b )
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void SP_VecCross( const float *a, const float *b, float *out )
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float SP_VecLen( const float *a )
{
	return (float)sqrt( (double)( a[0] * a[0] + a[1] * a[1] + a[2] * a[2] ));
}

// Returns the previous length, 0 if degenerate (and leaves the vector alone).
static float SP_VecNormalize( float *a )
{
	float len = SP_VecLen( a );

	if( len > 1e-6f )
	{
		float inv = 1.0f / len;

		a[0] *= inv; a[1] *= inv; a[2] *= inv;
	}
	return len;
}

static void SP_QuatIdentity( float *q )
{
	q[0] = q[1] = q[2] = 0.0f;
	q[3] = 1.0f;
}

static void SP_QuatNormalize( float *q )
{
	float len = (float)sqrt( (double)( q[0] * q[0] + q[1] * q[1]
		+ q[2] * q[2] + q[3] * q[3] ));

	if( len < 1e-8f )
	{
		SP_QuatIdentity( q );
		return;
	}
	len = 1.0f / len;
	q[0] *= len; q[1] *= len; q[2] *= len; q[3] *= len;
}

// Hamilton product: out = a * b, i.e. "apply b, then a".
static void SP_QuatMul( const float *a, const float *b, float *out )
{
	out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void SP_QuatFromAxisAngle( const float *axis, float theta, float *q )
{
	float n[3];
	float half, s;

	SP_VecCopy( axis, n );
	if( SP_VecNormalize( n ) <= 0.0f || theta == 0.0f )
	{
		SP_QuatIdentity( q );
		return;
	}

	half = theta * 0.5f;
	s = (float)sin( (double)half );

	q[0] = n[0] * s;
	q[1] = n[1] * s;
	q[2] = n[2] * s;
	q[3] = (float)cos( (double)half );
}

// Rotate a vector by a quaternion.
static void SP_QuatRotate( const float *q, const float *v, float *out )
{
	float t[3], u[3];

	// out = v + 2*w*(q x v) + 2*(q x (q x v))
	SP_VecCross( q, v, t );
	SP_VecScale( t, 2.0f, t );

	SP_VecCross( q, t, u );

	out[0] = v[0] + q[3] * t[0] + u[0];
	out[1] = v[1] + q[3] * t[1] + u[1];
	out[2] = v[2] + q[3] * t[2] + u[2];
}

// =============================================================================
// Parameters
// =============================================================================

void Slayer_Spin_DefaultParams( slayer_spin_params_t *p )
{
	if( !p )
		return;

	// Values are for a CS hand grenade. Rationale for the ones that are not
	// arbitrary:
	//
	//  throw_spin 0.014 rad/sec per unit/sec: a hard 600 u/s throw gives about
	//    8.4 rad/sec ~ 1.3 turns/sec. Faster than that reads as a blur on a
	//    phone screen; the earlier code ran at three turns/sec and looked wrong
	//    for exactly this reason.
	//  roll_grip 8: a grenade landing on concrete stops sliding and starts
	//    rolling within ~1/8 s, which is what "it rolled away" looks like.
	//  air_drag 0.35: air barely slows a grenade's tumble over its ~2 s fuse,
	//    but it must not be zero, or a grenade that lands softly keeps its
	//    throw spin forever.
	p->radius      = 3.5f;
	p->throw_spin  = 0.014f;
	p->spin_bias   = 0.35f;
	p->impact_grip = 0.55f;
	p->roll_grip   = 8.0f;
	p->air_drag    = 0.35f;
	p->spin_drag   = 3.0f;
	p->rest_speed  = 14.0f;
	p->rest_omega  = 0.7f;
	p->max_omega   = 40.0f;
	p->impact_dv   = 45.0f;
}

static void SP_SanitizeParams( const slayer_spin_params_t *in, slayer_spin_params_t *out )
{
	if( in )
		*out = *in;
	else
		Slayer_Spin_DefaultParams( out );

	// A zero or negative radius would divide by zero in the rolling term, and a
	// caller that memsets its params struct is a realistic mistake.
	if( !( out->radius > 0.01f ))
		out->radius = 3.5f;
	if( !( out->max_omega > 0.01f ))
		out->max_omega = 40.0f;
	if( out->spin_bias < 0.0f ) out->spin_bias = 0.0f;
	if( out->spin_bias > 1.0f ) out->spin_bias = 1.0f;
	if( out->impact_grip < 0.0f ) out->impact_grip = 0.0f;
	if( out->impact_grip > 1.0f ) out->impact_grip = 1.0f;
	if( out->roll_grip < 0.0f ) out->roll_grip = 0.0f;
	if( out->air_drag < 0.0f ) out->air_drag = 0.0f;
	if( out->spin_drag < 0.0f ) out->spin_drag = 0.0f;
	if( out->impact_dv < 1.0f ) out->impact_dv = 1.0f;
}

// =============================================================================
// Seeding
// =============================================================================

// Tumble axis for an object flying along `vel`: across the path, so it goes
// end-over-end rather than spinning about a fixed arbitrary axis. A per-object
// share of the flight direction is mixed in so several objects thrown the same
// way do not all tumble in one flat plane.
static void SP_TumbleAxis( const float *vel, float bias, int seed, float *out )
{
	static const float up[3] = { 0.0f, 0.0f, 1.0f };
	float across[3], along[3];
	float sign;

	SP_VecCross( vel, up, across );
	if( SP_VecNormalize( across ) <= 1e-3f )
	{
		// Falling straight down: no meaningful "across the path". Pick a stable
		// horizontal axis from the seed instead of leaving it degenerate.
		across[0] = ( seed & 1 ) ? 1.0f : 0.0f;
		across[1] = ( seed & 1 ) ? 0.0f : 1.0f;
		across[2] = 0.0f;
	}

	SP_VecCopy( vel, along );
	if( SP_VecNormalize( along ) <= 1e-3f )
		SP_VecClear( along );

	// Alternate the bias direction so objects do not drift toward one handedness.
	sign = ( seed & 2 ) ? -1.0f : 1.0f;
	SP_VecMA( across, bias * sign, along, out );
	if( SP_VecNormalize( out ) <= 1e-3f )
		SP_VecCopy( across, out );
}

void Slayer_Spin_Seed( slayer_spin_t *st, const float *orient, const float *vel,
	int seed, const slayer_spin_params_t *p )
{
	slayer_spin_params_t pp;
	float axis[3];
	float speed;

	if( !st )
		return;

	SP_SanitizeParams( p, &pp );

	if( orient )
	{
		st->orient[0] = orient[0];
		st->orient[1] = orient[1];
		st->orient[2] = orient[2];
		st->orient[3] = orient[3];
		SP_QuatNormalize( st->orient );
	}
	else
	{
		SP_QuatIdentity( st->orient );
	}

	SP_VecClear( st->omega );
	st->impacts = 0;
	st->resting = 0;

	if( vel )
	{
		SP_VecCopy( vel, st->prev_vel );
		st->have_prev_vel = 1;
		speed = SP_VecLen( vel );
	}
	else
	{
		SP_VecClear( st->prev_vel );
		st->have_prev_vel = 0;
		speed = 0.0f;
	}

	// The throw itself is what sets the object spinning. This is the one place
	// spin is derived from speed -- afterwards it is state, and only contacts
	// and drag change it.
	if( speed > 1.0f )
	{
		SP_TumbleAxis( vel, pp.spin_bias, seed, axis );
		SP_VecScale( axis, speed * pp.throw_spin, st->omega );
	}
	else
	{
		st->resting = 1;
	}
}

// =============================================================================
// Stepping
// =============================================================================

static void SP_ClampOmega( float *omega, float max_omega )
{
	float len = SP_VecLen( omega );

	if( len > max_omega && len > 1e-6f )
		SP_VecScale( omega, max_omega / len, omega );
}

void Slayer_Spin_AddImpulse( slayer_spin_t *st, const float *axis, float rad_per_sec,
	const slayer_spin_params_t *p )
{
	slayer_spin_params_t pp;
	float n[3];

	if( !st || !axis )
		return;

	SP_SanitizeParams( p, &pp );

	SP_VecCopy( axis, n );
	if( SP_VecNormalize( n ) <= 1e-6f )
		return;

	SP_VecMA( st->omega, rad_per_sec, n, st->omega );
	SP_ClampOmega( st->omega, pp.max_omega );

	// Any real push wakes it: a settled object that gets kicked must start
	// turning again, and the rest latch below is what would otherwise hold it.
	if( SP_VecLen( st->omega ) > pp.rest_omega )
		st->resting = 0;
}

// Spin an object rolling on a surface would have: omega = (n x v) / r. This is
// the no-slip condition, and it is what makes a grenade that lands look like it
// rolls rather than slides while spinning at whatever rate it had in the air.
static void SP_RollingOmega( const float *vel, const float *normal, float radius,
	float *out )
{
	float v_t[3];
	float vn;

	// Only the component along the surface rolls.
	vn = SP_VecDot( vel, normal );
	SP_VecMA( vel, -vn, normal, v_t );

	SP_VecCross( normal, v_t, out );
	SP_VecScale( out, 1.0f / radius, out );
}

void Slayer_Spin_Step( slayer_spin_t *st, const float *vel, float dt,
	const slayer_spin_contact_t *contact, const slayer_spin_params_t *p )
{
	slayer_spin_params_t pp;
	float v[3];
	float dv[3];
	float speed, dv_len;
	float omega_len;
	float dq[4], result[4];

	if( !st || dt <= 0.0f )
		return;

	SP_SanitizeParams( p, &pp );

	// A long pause (loading screen, demo seek, alt-tab) is not a physical frame:
	// integrating it would spin the object by a full turn or more in one step.
	if( dt > 0.25f )
		dt = 0.25f;

	if( vel )
		SP_VecCopy( vel, v );
	else
		SP_VecClear( v );

	speed = SP_VecLen( v );

	// --- collision: the velocity change IS the collision ---------------------
	//
	// The server does not tell us "this grenade hit a wall", but a bounce is a
	// large, sudden change of velocity, and that is observable. Converting the
	// slip at the contact point into spin is what makes a grenade come off a
	// wall turning differently than it arrived -- the thing that was missing
	// when the rate was a function of speed alone.
	if( st->have_prev_vel )
	{
		SP_VecSub( v, st->prev_vel, dv );
		dv_len = SP_VecLen( dv );

		if( dv_len > pp.impact_dv )
		{
			float n[3];
			float axis[3];
			float dv_t[3];
			float dv_n;
			int   have_n = 0;

			if( contact && contact->has_impact_normal )
			{
				SP_VecCopy( contact->impact_normal, n );
				have_n = ( SP_VecNormalize( n ) > 1e-3f );
			}

			if( !have_n )
			{
				// No traced normal: for a bounce the velocity change points away
				// from the surface, so it is a usable estimate and costs no trace.
				SP_VecCopy( dv, n );
				have_n = ( SP_VecNormalize( n ) > 1e-3f );
			}

			if( have_n )
			{
				// FRICTION IS THE ONLY TANGENTIAL FORCE, so the tangential part
				// of the velocity change IS the friction impulse per unit mass.
				// Spinning it up is then r x J with the contact point at
				// -radius*n, which reduces to (n x dv_t) / radius.
				//
				// Using the tangential CHANGE rather than the incoming tangential
				// velocity is what makes different collisions differ: a grenade
				// that bounces straight back off a wall keeps its tangential
				// motion and gains little spin, while one that skids along the
				// wall loses tangential speed to friction and comes off turning.
				// An earlier version derived the axis from the incoming velocity
				// alone, and then every bounce off a given wall produced exactly
				// the same spin change no matter how the grenade actually hit it.
				dv_n = SP_VecDot( dv, n );
				SP_VecMA( dv, -dv_n, n, dv_t );

				if( SP_VecLen( dv_t ) > 1.0f )
				{
					SP_VecCross( n, dv_t, axis );
					SP_VecMA( st->omega, pp.impact_grip / pp.radius, axis, st->omega );
				}
				else
				{
					// A purely normal bounce has no tangential change, but the
					// object can still be sliding along the surface while the
					// normal impulse presses it there -- that slip is rubbed off
					// into spin. Scaled down because only part of the normal
					// impulse is available to friction.
					float slip[3];
					float vn = SP_VecDot( st->prev_vel, n );

					SP_VecMA( st->prev_vel, -vn, n, slip );
					if( SP_VecLen( slip ) > 1.0f )
					{
						SP_VecCross( n, slip, axis );
						SP_VecMA( st->omega, pp.impact_grip * 0.5f / pp.radius,
							axis, st->omega );
					}
				}

				SP_ClampOmega( st->omega, pp.max_omega );
				st->impacts++;
				st->resting = 0;
			}
		}
	}

	SP_VecCopy( v, st->prev_vel );
	st->have_prev_vel = 1;

	// --- contact behaviour ---------------------------------------------------
	if( contact && contact->on_ground )
	{
		float n[3];

		SP_VecCopy( contact->normal, n );
		if( SP_VecNormalize( n ) > 1e-3f )
		{
			float want[3], diff[3];
			float k, wn;

			// Converge toward rolling without slipping.
			SP_RollingOmega( v, n, pp.radius, want );
			SP_VecSub( want, st->omega, diff );

			k = pp.roll_grip * dt;
			if( k > 1.0f ) k = 1.0f;
			SP_VecMA( st->omega, k, diff, st->omega );

			// Spin about the contact normal (a top spinning on the floor) is
			// killed faster than rolling: friction acts on it directly, and
			// leaving it in makes a grenade at rest look like it is drilling
			// into the ground.
			wn = SP_VecDot( st->omega, n );
			k = pp.spin_drag * dt;
			if( k > 1.0f ) k = 1.0f;
			SP_VecMA( st->omega, -k * wn, n, st->omega );
		}
	}
	else if( pp.air_drag > 0.0f )
	{
		float k = pp.air_drag * dt;

		if( k > 1.0f ) k = 1.0f;
		SP_VecScale( st->omega, 1.0f - k, st->omega );
	}

	SP_ClampOmega( st->omega, pp.max_omega );
	omega_len = SP_VecLen( st->omega );

	// --- rest latch, with hysteresis ----------------------------------------
	//
	// One threshold would not do: an object lying still has a position that
	// interpolation jitters by a unit or two, so its apparent speed crosses any
	// single threshold constantly and it twitches forever. Settling requires
	// being clearly below; waking requires being clearly above.
	if( st->resting )
	{
		if( speed > pp.rest_speed * 2.0f || omega_len > pp.rest_omega * 2.0f )
			st->resting = 0;
	}
	else if( speed < pp.rest_speed * 0.5f && omega_len < pp.rest_omega )
	{
		st->resting = 1;
		SP_VecClear( st->omega );
	}

	if( st->resting )
		return;

	// --- integrate ----------------------------------------------------------
	if( omega_len > 1e-5f )
	{
		SP_QuatFromAxisAngle( st->omega, omega_len * dt, dq );
		// LEFT multiply: omega is world space, not body-local.
		SP_QuatMul( dq, st->orient, result );
		st->orient[0] = result[0];
		st->orient[1] = result[1];
		st->orient[2] = result[2];
		st->orient[3] = result[3];
		// Renormalize every frame: the pose is built by repeated multiplication
		// and a smoke grenade rolls for many seconds. An unnormalized quaternion
		// turns into scale and shear once it reaches a matrix.
		SP_QuatNormalize( st->orient );
	}
}

int Slayer_Spin_IsResting( const slayer_spin_t *st )
{
	return st ? st->resting : 1;
}

float Slayer_Spin_Rate( const slayer_spin_t *st )
{
	return st ? SP_VecLen( st->omega ) : 0.0f;
}

// =============================================================================
// Settling a dropped item against a surface
// =============================================================================

void Slayer_Spin_SettleTo( slayer_spin_t *st, const float *normal, float rate, float dt )
{
	float n[3];
	float local_up[3] = { 0.0f, 0.0f, 1.0f };
	float cur_up[3];
	float axis[3];
	float dot, angle, step;
	float dq[4], result[4];

	if( !st || !normal || dt <= 0.0f )
		return;

	SP_VecCopy( normal, n );
	if( SP_VecNormalize( n ) <= 1e-3f )
		return;

	// Where the object's own up currently points.
	SP_QuatRotate( st->orient, local_up, cur_up );
	SP_VecNormalize( cur_up );

	dot = SP_VecDot( cur_up, n );
	if( dot > 0.9999f )
		return;              // already aligned
	if( dot < -1.0f ) dot = -1.0f;
	if( dot > 1.0f ) dot = 1.0f;

	angle = (float)acos( (double)dot );

	SP_VecCross( cur_up, n, axis );
	if( SP_VecNormalize( axis ) <= 1e-4f )
	{
		// Exactly upside down: any perpendicular axis will do, and without this
		// the cross product is zero and the object stays inverted forever.
		axis[0] = 1.0f; axis[1] = 0.0f; axis[2] = 0.0f;
		if( fabs( (double)cur_up[0] ) > 0.9 )
		{
			axis[0] = 0.0f; axis[1] = 1.0f;
		}
	}

	step = angle * rate * dt;
	if( step > angle ) step = angle;      // never overshoot into oscillation
	if( step <= 0.0f ) return;

	SP_QuatFromAxisAngle( axis, step, dq );
	SP_QuatMul( dq, st->orient, result );
	st->orient[0] = result[0];
	st->orient[1] = result[1];
	st->orient[2] = result[2];
	st->orient[3] = result[3];
	SP_QuatNormalize( st->orient );
}
