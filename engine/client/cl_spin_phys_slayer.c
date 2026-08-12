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
#include <string.h>
#include "cl_spin_phys_slayer.h"

// Ninety degrees. Spelled out rather than taken from M_PI_2, which is a POSIX
// extension and not guaranteed by <math.h> in C89 -- this file compiles on the
// host as well as in the engine, so it cannot rely on it.
#define SP_HALF_PI  1.57079632679489661923f

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
	// roll_speed 120: a grenade that has slowed to walking pace is credibly
	// rolling; one arriving at 200+ u/s is skidding, and forcing the no-slip
	// condition on it was what made contact look like a bug. Chosen so that the
	// rolling target at the crossover is (120/3.5) ~ 34 rad/sec, i.e. still under
	// max_omega -- above the crossover nothing is asked of the spin that the cap
	// would have to refuse.
	p->roll_speed  = 120.0f;
	p->slide_drag  = 4.0f;
	// contact_omega 12 rad/sec ~ 1.9 turns/sec: faster than the throw tumble
	// (1.3 turns/sec at a hard throw) so a landing grenade visibly picks up, and
	// far below the 6.4 turns/sec the uncapped no-slip condition was producing.
	p->contact_omega = 12.0f;
	p->air_drag    = 0.35f;
	p->spin_drag   = 3.0f;
	p->rest_speed  = 14.0f;
	p->rest_omega  = 0.7f;
	p->rest_time   = 0.35f;
	p->spinup_time = 0.30f;
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
	if( out->roll_speed < 0.0f ) out->roll_speed = 0.0f;
	if( out->slide_drag < 0.0f ) out->slide_drag = 0.0f;
	if( out->contact_omega < 0.0f ) out->contact_omega = 0.0f;
	if( out->air_drag < 0.0f ) out->air_drag = 0.0f;
	if( out->spin_drag < 0.0f ) out->spin_drag = 0.0f;
	if( out->impact_dv < 1.0f ) out->impact_dv = 1.0f;
	if( out->rest_time < 0.0f ) out->rest_time = 0.0f;
	if( out->spinup_time < 0.0f ) out->spinup_time = 0.0f;
}

// =============================================================================
// Seeding
// =============================================================================

// Defined here rather than with the stepping code below because Slayer_Spin_Seed
// needs it too: a call before the definition inside one translation unit is an
// implicit declaration, which clang rejects and which broke this project's CI
// once already.
static void SP_ClampOmega( float *omega, float max_omega )
{
	float len = SP_VecLen( omega );

	if( len > max_omega && len > 1e-6f )
		SP_VecScale( omega, max_omega / len, omega );
}

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
	st->seed = seed;
	st->spun_up = 0;
	st->spinup_age = 0.0f;
	st->still_time = 0.0f;

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

	st->spinup_peak = speed;

	// The throw is what sets the object spinning, but on the frame we start
	// tracking it the velocity is usually NOT known yet: callers derive it by
	// differencing render positions and need two samples, so they hand us zero.
	//
	// This used to latch `resting` in that case, which meant the throw impulse
	// was never applied to anything and the only spin an object could ever get
	// came from rolling or from a bounce -- the live bug. Seeding now only
	// records the starting speed; the impulse itself is applied by
	// Slayer_Spin_Step while the spin-up window is open (see SP_SpinUp).
	if( speed > 1.0f )
	{
		SP_TumbleAxis( vel, pp.spin_bias, seed, axis );
		SP_VecScale( axis, speed * pp.throw_spin, st->omega );
		SP_ClampOmega( st->omega, pp.max_omega );
	}
}

// =============================================================================
// Stepping
// =============================================================================

// Throw impulse, applied during the spin-up window instead of at seeding.
//
// The caller's velocity is a low-passed difference of interpolated positions, so
// on the first frames of a throw it ramps up from zero toward the real speed.
// Taking the first sample would spin the object up from almost nothing; waiting
// for the peak is what gives a throw its actual spin. The window therefore keeps
// re-aiming the spin at the fastest velocity seen so far, and closes when the
// object has had its say: time is up, or a contact/collision now owns the spin.
static void SP_SpinUp( slayer_spin_t *st, const float *v, float speed, float dt,
	int ramping_up, const slayer_spin_params_t *pp )
{
	float axis[3];

	st->spinup_age += dt;

	if( speed > st->spinup_peak + 1.0f )
	{
		st->spinup_peak = speed;

		// Re-derive rather than accumulate: this is the same "spin from speed"
		// step the seed used to do, just delayed until the speed is known. The
		// axis comes from the CURRENT velocity, which by now is the flight
		// direction rather than the noise of the first frame.
		SP_TumbleAxis( v, pp->spin_bias, st->seed, axis );
		SP_VecScale( axis, speed * pp->throw_spin, st->omega );
		SP_ClampOmega( st->omega, pp->max_omega );
	}
	else if( !ramping_up && st->spinup_peak > pp->rest_speed )
	{
		// The filter has caught up and the object is no longer accelerating:
		// the throw is fully expressed. Closing here rather than waiting out the
		// timer keeps the later part of the flight -- where a falling object
		// speeds up again -- from re-aiming the spin mid-air.
		st->spun_up = 1;
	}

	if( st->spinup_age >= pp->spinup_time )
		st->spun_up = 1;
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
	{
		st->resting = 0;
		st->still_time = 0.0f;
	}
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
	float speed, prev_speed, dv_len;
	float omega_len;
	float dq[4], result[4];
	int   ramping_up;
	int   touching;
	int   in_spinup;

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
	prev_speed = st->have_prev_vel ? SP_VecLen( st->prev_vel ) : 0.0f;
	ramping_up = ( speed > prev_speed );
	touching = ( contact && contact->on_ground );

	// --- throw impulse -------------------------------------------------------
	//
	// Before anything else, because a thrown object must be turning on the frame
	// it starts moving, not once it hits something.
	in_spinup = !st->spun_up;
	if( in_spinup )
	{
		if( touching && st->spinup_age > 0.0f )
		{
			// It reached a surface: whatever spin it has now is what it carries
			// into rolling, and re-aiming from speed would fight the rolling
			// convergence below.
			st->spun_up = 1;
		}
		else if( speed > pp.rest_speed )
		{
			SP_SpinUp( st, v, speed, dt, ramping_up, &pp );
			st->resting = 0;
		}
		else if( st->spinup_age > 0.0f )
		{
			// Was moving, now is not: the throw is over.
			st->spinup_age += dt;
			if( st->spinup_age >= pp.spinup_time )
				st->spun_up = 1;
		}
	}

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

		// A RISING velocity during the spin-up window is the throw itself, not a
		// bounce. The caller's low-pass climbs from zero to a 600 u/s throw over
		// a few frames, and each of those steps is larger than impact_dv -- so
		// without this guard the very first frames of every throw were counted as
		// collisions, which both inflated the hit counter and closed the spin-up
		// window immediately, defeating the impulse.
		if( in_spinup && ramping_up )
			dv_len = 0.0f;

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
				// The most spin friction can impart is the no-slip condition at
				// the speed the object arrived with: friction acts precisely until
				// slipping stops, and once omega == (n x v)/r there is no slip left
				// to convert. Beyond that the sum below is not physics, it is a
				// number growing with speed -- and at these radii it grew fast
				// (a 200 u/s landing added 15.7 rad/sec, 2.5 turns/sec, in ONE
				// frame, which is the "hits a surface and goes crazy" report in
				// the impact path rather than the rolling one).
				float limit_w[3];
				float limit;
				float arrive_t[3];
				float arrive_vn, arrive_tang, arrive_grip;

				SP_RollingOmega( st->prev_vel, n, pp.radius, limit_w );
				limit = SP_VecLen( limit_w );
				if( pp.contact_omega > 0.0f && limit > pp.contact_omega )
					limit = pp.contact_omega;

				// And scaled by the same grip the contact block uses. Friction
				// does spin up a skidding object, but over its whole slide, not
				// in the single frame where our sampled velocity happens to
				// change: we see the entire collision as one dv, so taking the
				// full no-slip value from it hands the object in one frame what
				// should take a third of a second. A fast arrival therefore gains
				// little here and the contact block adds the rest as it slows,
				// which is what "it skids, then starts rolling" looks like.
				arrive_vn = SP_VecDot( st->prev_vel, n );
				SP_VecMA( st->prev_vel, -arrive_vn, n, arrive_t );
				arrive_tang = SP_VecLen( arrive_t );

				arrive_grip = 1.0f;
				if( pp.roll_speed > 0.0f )
				{
					arrive_grip = 1.0f - ( arrive_tang / pp.roll_speed );
					if( arrive_grip < 0.15f ) arrive_grip = 0.15f;
					if( arrive_grip > 1.0f ) arrive_grip = 1.0f;
				}
				limit *= arrive_grip;

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

				// Friction cannot spin an object faster than no-slip at the speed
				// it arrived with (see limit above): past that point there is no
				// slipping left for it to act on.
				if( limit > 0.0f )
					SP_ClampOmega( st->omega, limit );

				st->impacts++;
				st->resting = 0;
				st->still_time = 0.0f;
				// A collision now owns the spin. Leaving the spin-up window open
				// would let the post-bounce speed re-derive omega from scratch
				// and erase exactly the change the bounce just made.
				st->spun_up = 1;
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
			float want[3], diff[3], v_t[3];
			float k, wn, vn, tang, grip;

			// SLIDING VERSUS ROLLING.
			//
			// The no-slip condition omega = (n x v)/r is only physical for an
			// object that is actually rolling. Applying it at any speed is what
			// produced the reported "when it touches a wall it starts spinning
			// buggily, not about its own axis": a grenade (r = 3.5) arriving at
			// 200 u/s needs 57 rad/sec to roll without slipping, which is nine
			// turns a second, and it also re-aims the axis from the tumble axis to
			// n x v within an eighth of a second. Measured on the frames after
			// landing: 0.40 turns/sec in the air becoming 5.08 six frames later.
			//
			// Real objects hitting the ground at speed do not start rolling, they
			// SKID: friction bleeds their spin and their speed, and only once slow
			// enough does rolling take over. So the convergence is weighted by how
			// close the object is to a speed at which rolling is credible, and
			// what is left over is drag.
			vn = SP_VecDot( v, n );
			SP_VecMA( v, -vn, n, v_t );
			tang = SP_VecLen( v_t );

			// GRIP: is this object rolling, or skidding?
			//
			// Full grip below half of roll_speed (a plateau, not a ramp all the
			// way to zero -- something genuinely rolling must satisfy the no-slip
			// condition exactly, not approximately), falling to zero at
			// roll_speed.
			grip = 1.0f;
			if( pp.roll_speed > 0.0f )
			{
				float full = pp.roll_speed * 0.5f;

				if( tang > full )
					grip = ( pp.roll_speed - tang ) / ( pp.roll_speed - full );
				if( grip < 0.0f ) grip = 0.0f;
				if( grip > 1.0f ) grip = 1.0f;
			}

			// ONE blended target, not two competing pulls. Converging toward
			// rolling AND decaying toward zero at the same time would settle
			// partway between them even at full grip; scaling the TARGET keeps
			// each regime exact:
			//
			//   grip 1 (slow): target = rolling, rate = roll_grip -- the no-slip
			//     condition, which is right for something actually rolling.
			//   grip 0 (fast): target = 0, rate = slide_drag -- a skidding object
			//     loses spin to friction instead of being handed nine turns a
			//     second by a condition that does not apply to it.
			SP_RollingOmega( v, n, pp.radius, want );

			// Cap the TARGET, not only the result: an uncapped target drags omega
			// toward a value the cap would refuse, pinning the object at the cap
			// for as long as it keeps moving.
			//
			// And cap it at contact_omega rather than max_omega, because the
			// no-slip condition is geometrically correct yet visually wrong at
			// these radii: a 3.5-unit grenade rolling at 200 u/s works out to 9
			// turns a second, which on a phone screen is a smear. max_omega
			// (40 rad/sec) exists to survive one bad frame, not to be a rolling
			// speed.
			if( pp.contact_omega > 0.0f )
				SP_ClampOmega( want, pp.contact_omega );
			SP_ClampOmega( want, pp.max_omega );

			SP_VecScale( want, grip, want );

			SP_VecSub( want, st->omega, diff );

			k = ( pp.roll_grip * grip + pp.slide_drag * ( 1.0f - grip )) * dt;
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
	//
	// NOTHING RESTS IN MID-AIR. Without the contact requirement an object that
	// happens to be slow -- the top of a lobbed arc, a grenade dropped straight
	// down before gravity takes hold, a weapon that spawns before the server
	// gives it velocity -- latches `resting`, stops integrating and hangs in the
	// air unturned until it lands. That is the other half of the reported
	// "doesn't rotate in the air, only starts moving on contact". A no-contact
	// object may still settle, but only after staying slow for rest_time, which
	// no falling object does: gravity moves it 100+ units/sec within a third of
	// a second.
	if( st->resting )
	{
		if( speed > pp.rest_speed * 2.0f || omega_len > pp.rest_omega * 2.0f )
		{
			st->resting = 0;
			st->still_time = 0.0f;
		}
	}
	else if( speed < pp.rest_speed * 0.5f && omega_len < pp.rest_omega )
	{
		st->still_time += dt;

		if( touching || st->still_time >= pp.rest_time )
		{
			st->resting = 1;
			SP_VecClear( st->omega );
		}
	}
	else
	{
		st->still_time = 0.0f;
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
	static const float local_up[3] = { 0.0f, 0.0f, 1.0f };

	// Kept as the thin case of the general function so the two cannot drift:
	// tol_cos 1 means "no tolerance", i.e. exactly the old behaviour.
	Slayer_Spin_SettleAxisTo( st, normal, local_up, rate, dt, 1.0f );
}

void Slayer_Spin_SettleAxisTo( slayer_spin_t *st, const float *normal,
	const float *local_axis, float rate, float dt, float tol_cos )
{
	float n[3];
	float body[3] = { 0.0f, 0.0f, 1.0f };
	float cur[3];
	float axis[3];
	float dot, angle, step, slack;
	float dq[4], result[4];

	if( !st || !normal || dt <= 0.0f )
		return;

	SP_VecCopy( normal, n );
	if( SP_VecNormalize( n ) <= 1e-3f )
		return;

	if( local_axis )
	{
		SP_VecCopy( local_axis, body );
		if( SP_VecNormalize( body ) <= 1e-3f )
		{
			body[0] = 0.0f; body[1] = 0.0f; body[2] = 1.0f;
		}
	}

	// Where that body axis currently points in the world.
	SP_QuatRotate( st->orient, body, cur );
	SP_VecNormalize( cur );

	dot = SP_VecDot( cur, n );

	// A resting object may lie either way up: a rifle on its left side is as
	// settled as one on its right, and forcing a particular side would flip
	// models over for no reason. So the axis is treated as unsigned -- what
	// matters is that it is PERPENDICULAR to the surface, not which end is up.
	if( dot < 0.0f )
	{
		dot = -dot;
		SP_VecScale( cur, -1.0f, cur );
	}

	if( dot > 0.9999f )
		return;              // already aligned
	if( dot > 1.0f ) dot = 1.0f;

	// TOLERANCE. Inside it the pose already reads as resting and must be left
	// exactly alone -- correcting a plausible pose is what looked like the model
	// straightening itself out after it landed. It is also what allows resting on
	// an EDGE: a weapon bridging a step is within tolerance of both surfaces, so
	// neither pulls it flat.
	slack = tol_cos;
	if( slack > 1.0f ) slack = 1.0f;
	if( slack < -1.0f ) slack = -1.0f;
	if( dot >= slack )
		return;

	angle = (float)acos( (double)dot );

	// Ease toward the EDGE of the tolerance band, not to exact alignment: the
	// goal is a pose that reads as resting, and stopping at the boundary keeps
	// the correction as small as the problem.
	if( slack < 1.0f )
		angle -= (float)acos( (double)slack );
	if( angle <= 0.0f )
		return;

	SP_VecCross( cur, n, axis );
	if( SP_VecNormalize( axis ) <= 1e-4f )
	{
		// Unreachable by construction, and kept as a guard rather than a
		// workaround: after the sign fold above |dot| < 0.9999, so the cross
		// product has magnitude sqrt(1 - dot^2) > 0.014. The old code had a
		// pick-any-perpendicular fallback here for the exactly-upside-down case,
		// which the fold makes impossible -- an inverted object is resting on its
		// other face and needs no rotation at all.
		return;
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

// =============================================================================
// Toppling: how a dropped item actually comes to rest
// =============================================================================
//
// See the long note in cl_spin_phys_slayer.h. The short version: there is no
// target orientation here. The item is a box, gravity acts at its centre, the box
// rotates about whichever of its own corners it is overbalanced on, and stops
// when it is not overbalanced. Flat ground, a step and a ledge are the same code
// with different contact sets.

void Slayer_Spin_DefaultSettleParams( slayer_spin_settle_params_t *sp )
{
	if( !sp )
		return;

	sp->gravity     = 800.0f;   // GoldSrc
	sp->damping     = 5.0f;     // 1/sec. Measured: without it the item pendulums
	                            // through 231 degrees before stopping.
	sp->restitution = 0.10f;    // a corner landing keeps a tenth of its spin
	sp->topple_rate = 4.5379f;  // 260 deg/sec. Measured: this gives 4.3 deg per
	                            // frame; a hard clamp gives 19.7 in ONE frame,
	                            // which is the "sticks flat instantly" symptom.
	sp->contact_eps = 0.75f;    // units
	sp->pen_tol     = 0.05f;    // units
	sp->rest_omega  = 0.03f;    // rad/sec
}

/*
====================
Slayer_Spin_MaxTilt

Largest tilt whose lowest corner still clears a surface `dist` below the origin.

Solve a*sin(theta) + b*cos(theta) <= dist. Writing the left side as
R*sin(theta + phi) with R = hypot(a,b) and phi = atan2(b,a) gives
theta_max = asin(dist/R) - phi directly.

Two boundary cases have to be answered honestly rather than clamped away:
  * dist >= R: the origin is high enough that ANY orientation clears the surface,
    so there is no constraint at all (a weapon on a table edge, mid-air).
  * dist < b: even lying perfectly flat penetrates. That happens routinely,
    because the server parks the origin using its own hull rather than the mesh,
    and the answer is zero tilt -- flat is as good as it gets.
====================
*/
float Slayer_Spin_MaxTilt( float half_long, float half_short, float dist )
{
	float a = half_long, b = half_short;
	float r, phi, s;

	if( a < 0.0f ) a = -a;
	if( b < 0.0f ) b = -b;

	// A degenerate box constrains nothing; returning a large angle keeps callers
	// from having to special-case it, since they clamp against their own limits.
	if( a <= 1e-4f && b <= 1e-4f )
		return SP_HALF_PI;

	r = (float)sqrt( (double)( a * a + b * b ));
	if( dist >= r )
		return SP_HALF_PI;
	if( dist <= 0.0f )
		return 0.0f;

	phi = (float)atan2( (double)b, (double)a );
	s = dist / r;
	if( s > 1.0f ) s = 1.0f;

	if( s < (float)sin( (double)phi ))
		return 0.0f;          // cannot even lie flat; flat is the best pose

	return (float)asin( (double)s ) - phi;
}

/*
====================
SP_BoxCornerHeights

Signed height of each of the box's 8 corners above the supporting surface.

`half` are the box's half-extents in ITS OWN frame; the corners are rotated by the
current pose and projected onto the surface normal, with the origin sitting `dist`
above the surface. Negative means the corner is inside the geometry.

Returns the number of corners within `eps` of the surface and writes the lowest
height to `out_low`. That count IS the support classification: 3+ corners means a
face is down, 2 means an edge, 1 means a tip. No separate detection logic exists,
which is the point -- the edge case is not a special branch that can be forgotten.
====================
*/
static int SP_BoxCornerHeights( const slayer_spin_t *st, const float *half,
	const float *normal, float dist, float eps, float *out_low, float *out_lowest_corner )
{
	int   sx, sy, sz;
	int   contacts = 0;
	float low = 1e30f;
	float low_corner[3];

	low_corner[0] = low_corner[1] = low_corner[2] = 0.0f;

	for( sx = -1; sx <= 1; sx += 2 )
	{
		for( sy = -1; sy <= 1; sy += 2 )
		{
			for( sz = -1; sz <= 1; sz += 2 )
			{
				float local[3], world[3];
				float h;

				local[0] = half[0] * (float)sx;
				local[1] = half[1] * (float)sy;
				local[2] = half[2] * (float)sz;

				SP_QuatRotate( st->orient, local, world );

				h = dist + SP_VecDot( world, normal );

				if( h < low )
				{
					low = h;
					SP_VecCopy( world, low_corner );
				}
				if( h <= eps )
					contacts++;
			}
		}
	}

	if( out_low )
		*out_low = low;
	if( out_lowest_corner )
		SP_VecCopy( low_corner, out_lowest_corner );

	return contacts;
}

/*
====================
SP_ContactAxis

About which axis does the surface push a penetrating corner out?

This is the one piece of physics in the module, and getting it right required
discarding the obvious answer. The obvious answer is "gravity topples it", and
that answer is WRONG HERE: the origin is the server's and we never move it, so
the centre of mass is fixed, and gravity acting at a fixed centre of mass
produces NO torque about it whatsoever. Rotating the body does not change its
potential energy. A model built on a gravity torque about the centre pendulums
forever (measured in tests/settle_proto.py: 231 degrees of travel), which is what
that first prototype did.

What actually rotates a dropped weapon is the NORMAL FORCE at the contact. Its
torque about the centre is `corner x (N * n)`, i.e. about `corner x n`, and
rotating about that axis lifts the corner out of the surface:

    d(height)/dt = ((corner x n) x corner) . n
                 = |corner|^2 - (corner . n)^2   >= 0

So the axis is `corner x n` with no sign games, and the motion it produces is
precisely "the ground pushes the buried end up until nothing is buried" -- which,
for a rifle standing on its muzzle inside the floor, is a topple onto its side,
and for a rifle already lying flat, is nothing at all.

Returns the lever arm (the contact's distance from the origin measured in the
surface plane); zero means the contact sits directly below the centre and no
rotation can lift it, in which case the pose is as good as it gets.
====================
*/
static float SP_ContactAxis( const float *low_corner, const float *normal, float *out_axis )
{
	float axis[3];
	float along;
	float horiz[3];

	SP_VecCross( low_corner, normal, axis );

	if( SP_VecNormalize( axis ) <= 1e-4f )
	{
		SP_VecClear( out_axis );
		return 0.0f;
	}

	SP_VecCopy( axis, out_axis );

	along = SP_VecDot( low_corner, normal );
	SP_VecMA( low_corner, -along, normal, horiz );
	return SP_VecLen( horiz );
}

void Slayer_Spin_Settle( slayer_spin_t *st, const float *half,
	const float *normal, float dist, float dt,
	const slayer_spin_settle_params_t *sp, slayer_spin_support_t *out )
{
	slayer_spin_settle_params_t p;
	slayer_spin_support_t       res;
	float n[3];
	float h[3];
	float low = 0.0f;
	float low_corner[3];
	float axis[3];
	float lever;
	float reach;
	float step = 0.0f;
	int   i;

	memset( &res, 0, sizeof( res ));
	res.support = SLAYER_SUPPORT_AIR;

	if( !st || !half || !normal || dt <= 0.0f )
	{
		if( out ) *out = res;
		return;
	}

	if( sp ) p = *sp;
	else     Slayer_Spin_DefaultSettleParams( &p );

	// Sanitise: these come from cvars, and a zero or negative value here would
	// either freeze the item or let it turn without limit.
	if( p.gravity     <= 0.0f ) p.gravity     = 800.0f;
	if( p.damping     <  0.0f ) p.damping     = 0.0f;
	if( p.restitution <  0.0f ) p.restitution = 0.0f;
	if( p.restitution >  0.9f ) p.restitution = 0.9f;
	if( p.topple_rate <= 0.0f ) p.topple_rate = 4.5379f;
	if( p.contact_eps <  0.0f ) p.contact_eps = 0.0f;
	if( p.pen_tol     <  0.0f ) p.pen_tol     = 0.0f;
	if( p.rest_omega  <= 0.0f ) p.rest_omega  = 0.03f;

	SP_VecCopy( normal, n );
	if( SP_VecNormalize( n ) <= 1e-3f )
	{
		if( out ) *out = res;
		return;
	}

	for( i = 0; i < 3; i++ )
	{
		h[i] = half[i];
		if( h[i] < 0.0f ) h[i] = -h[i];
	}

	// Does the box reach the surface at all? `reach` is the furthest any corner can
	// be from the origin, so an origin higher than that is simply in the air and
	// settling does not apply -- Slayer_Spin_Step owns the pose there.
	reach = (float)sqrt( (double)( h[0] * h[0] + h[1] * h[1] + h[2] * h[2] ));
	if( dist > reach + p.contact_eps )
	{
		if( out ) *out = res;
		return;
	}

	res.contacts = SP_BoxCornerHeights( st, h, n, dist, p.contact_eps, &low, low_corner );
	res.penetration = low;

	if( res.contacts >= 3 )      res.support = SLAYER_SUPPORT_FACE;
	else if( res.contacts == 2 ) res.support = SLAYER_SUPPORT_EDGE;
	else if( res.contacts == 1 ) res.support = SLAYER_SUPPORT_POINT;
	else                         res.support = SLAYER_SUPPORT_AIR;

	// Oriented so that even the lowest corner clears the surface: in the air after
	// all, whatever `dist` suggested.
	if( res.contacts == 0 && low > p.contact_eps )
	{
		if( out ) *out = res;
		return;
	}

	lever = SP_ContactAxis( low_corner, n, axis );

	// STABLE = nothing is buried. No surface is pushing on the item, so the pose
	// it is in is a genuine resting pose whatever angle that happens to be. This
	// is where "resting on an edge" comes from: not from a branch that recognises
	// edges, but from an edge pose having nothing left to push it flat. Two
	// contacts across a step satisfy this exactly as well as four on a floor.
	if( low >= -p.pen_tol )
		res.stable = 1;

	if( !res.stable && lever > 1e-2f )
	{
		// --- resolve penetration, at a limited rate -------------------------
		//
		// How much rotation lifts the corner out: the corner's height rises by
		// about `lever` per radian, so `need / lever` radians. Capped at
		// topple_rate, which is what makes this VISIBLE rather than instant --
		// measured, the cap yields 4.3 degrees per frame over 0.083s, while
		// resolving it in one step moves the model 19.7 degrees in a single frame,
		// and that single frame is the "it glues itself flat on contact" the player
		// reported. Transient penetration costs nothing: only the pose is ours,
		// collision uses the server's hull either way.
		float need = -low;
		float rot = need / lever;
		float max_step = p.topple_rate * dt;
		float dq[4], result[4];

		if( rot > max_step )
			rot = max_step;

		if( rot > 0.0f )
		{
			SP_QuatFromAxisAngle( axis, rot, dq );
			SP_QuatMul( dq, st->orient, result );
			st->orient[0] = result[0];
			st->orient[1] = result[1];
			st->orient[2] = result[2];
			st->orient[3] = result[3];
			SP_QuatNormalize( st->orient );
			step = rot;

			// The corner is being pushed out, so whatever spin drove it in is
			// spent. An inelastic slap: keep a tenth, reversed. Without it the item
			// rocks from corner to corner for seconds.
			SP_VecScale( st->omega, -p.restitution, st->omega );
		}
	}
	else if( SP_VecLen( st->omega ) > 1e-5f )
	{
		// Nothing buried, but still turning: let the remaining tumble run out
		// against friction. This is what makes a weapon dropped at an angle rock
		// once and settle instead of stopping dead the instant it touches.
		float omega_len = SP_VecLen( st->omega );
		float dq[4], result[4];

		step = omega_len * dt;
		SP_QuatFromAxisAngle( st->omega, step, dq );
		SP_QuatMul( dq, st->orient, result );
		st->orient[0] = result[0];
		st->orient[1] = result[1];
		st->orient[2] = result[2];
		st->orient[3] = result[3];
		SP_QuatNormalize( st->orient );
	}

	// Contact friction, applied last so it also damps the tumble integrated above.
	{
		float k = p.damping * dt;

		if( k > 1.0f ) k = 1.0f;
		SP_VecScale( st->omega, 1.0f - k, st->omega );
	}

	res.applied = step;

	// SETTLED: nothing buried and no longer turning. The caller stops computing
	// anything for the item from here until its origin moves.
	if( res.stable && SP_VecLen( st->omega ) < p.rest_omega )
	{
		SP_VecClear( st->omega );
		res.settled = 1;
	}

	if( out ) *out = res;
}
