/*
cl_spin_phys_slayer.h - Slayer3D shared rotational dynamics for loose objects
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
#ifndef CL_SPIN_PHYS_SLAYER_H
#define CL_SPIN_PHYS_SLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

// Angular dynamics for objects whose TRANSLATION belongs to the server and
// whose ROTATION is ours to invent: grenades in flight, dropped weapons, a
// dropped shield, any prop a mod throws on the ground.
//
// The point of this module is that angular velocity is STATE, not a function of
// how fast the object is currently moving. The previous grenade code computed
// `rate = f(speed)` every frame and derived the axis from the current velocity,
// which cannot express the things that actually make tumbling read as physical:
// a hit against a wall changing the spin, a grenade that keeps turning after it
// slows down, friction bleeding the spin off while it rolls, or two grenades
// thrown identically ending up in different orientations.
//
// Deliberately NOT a rigid-body simulation. There is no inertia tensor and no
// contact solver, because the server owns the position: any force model we
// integrate would immediately disagree with where the object actually is. What
// is modelled is exactly the part nobody else is computing -- the spin -- driven
// by the velocity the server implies and by contacts the caller traces for us.
//
// Everything is world space. Arrays are plain float[N] rather than vec3_t so
// this file can be compiled and tested on the host without the engine headers.

// Tuning. One struct per kind of object, so a grenade and a dropped rifle can
// behave differently without duplicating the code.
typedef struct
{
	float radius;        // effective rolling radius, units. Never <= 0.
	float throw_spin;    // spin imparted per unit of throw speed (dimensionless)
	float spin_bias;     // 0..1: share of the spin about the flight line itself,
	                     // so objects do not all tumble in one flat plane
	float impact_grip;   // 0..1: how much contact slip converts to spin on a hit
	float roll_grip;     // 1/sec: how fast the spin converges to rolling
	float air_drag;      // 1/sec: spin decay in flight
	float spin_drag;     // 1/sec: extra decay of spin about the contact normal
	float rest_speed;    // units/sec: below this it may settle
	float rest_omega;    // rad/sec: below this it may settle
	float rest_time;     // sec: how long "slow" must last to settle WITHOUT a
	                     // reported contact. Nothing rests in mid-air, so an
	                     // object with no contact has to prove it is not simply
	                     // passing through the slow part of its arc.
	float spinup_time;   // sec: how long the throw impulse may keep tracking a
	                     // rising velocity before it is considered final
	float max_omega;     // rad/sec: hard cap, keeps one bad frame from blurring
	float impact_dv;     // units/sec: velocity change that counts as a collision
} slayer_spin_params_t;

// What the caller learned about the object's surroundings this frame. All of it
// is optional: with a zeroed struct the object simply tumbles in free flight.
typedef struct
{
	int   on_ground;        // resting on / sliding along a surface
	float normal[3];        // that surface's normal (unit)
	int   has_impact_normal;// true if impact_normal is meaningful
	float impact_normal[3]; // surface the object hit this frame (unit)
} slayer_spin_contact_t;

// Per-object state. Owned by the caller, one per tracked entity.
typedef struct
{
	float orient[4];     // accumulated orientation, quaternion (x,y,z,w)
	float omega[3];      // angular velocity, rad/sec, world space
	float prev_vel[3];
	int   have_prev_vel;
	int   resting;       // latched with hysteresis, see Slayer_Spin_Step
	int   impacts;       // diagnostics: collisions seen since seeding

	// THE THROW IMPULSE IS APPLIED IN Slayer_Spin_Step, NOT IN Slayer_Spin_Seed.
	//
	// Seeding happens on the first frame an object becomes visible, and on that
	// frame its velocity is not known yet: the caller derives velocity by
	// differencing render positions, so it needs two samples, and the value it
	// passes to the seed is zero. The core used to take that zero as "this was
	// dropped, not thrown" and latch `resting` -- which meant the throw impulse
	// was never applied to anything, ever. Objects then only span up from
	// contact (rolling, bounces), which is exactly what the reported bug looked
	// like: "grenades roll along the ground instead of tumbling in the air",
	// "dropped weapons only start moving when they touch a surface".
	//
	// So the spin-up is a WINDOW rather than an event: while the object has not
	// spun up yet, the spin tracks the rising velocity, and the window closes as
	// soon as the velocity stops rising (the caller's low-pass has caught up) or
	// something happens that owns the spin instead -- a collision or a contact.
	int   spun_up;       // throw impulse has been applied and is final
	float spinup_peak;   // highest speed seen during the spin-up window
	float spinup_age;    // seconds the window has been open
	float still_time;    // seconds spent slow enough to be considered settled
	int   seed;          // per-object tumble bias, kept so the window can re-aim
} slayer_spin_t;

// Sensible defaults for a hand grenade (radius ~3.5 units).
void Slayer_Spin_DefaultParams( slayer_spin_params_t *p );

// Start tracking. `orient` is the object's current orientation as a quaternion
// (pass the server's angles converted, NOT identity -- otherwise the object
// visibly snaps the moment we take it over); `vel` is its current velocity.
// `seed` picks the per-object spin bias direction and only needs to differ
// between objects, so an entity index is fine.
void Slayer_Spin_Seed( slayer_spin_t *st, const float *orient, const float *vel,
	int seed, const slayer_spin_params_t *p );

// Advance one frame. `vel` is the velocity implied by the server this frame,
// `contact` may be NULL. Safe with dt <= 0 (does nothing) and with a NULL
// contact (free flight).
void Slayer_Spin_Step( slayer_spin_t *st, const float *vel, float dt,
	const slayer_spin_contact_t *contact, const slayer_spin_params_t *p );

// Nudge the spin as if struck; used when the caller knows about a hit the
// velocity does not show (e.g. a server event).
void Slayer_Spin_AddImpulse( slayer_spin_t *st, const float *axis, float rad_per_sec,
	const slayer_spin_params_t *p );

// True while the object is considered settled (no spin is applied).
int Slayer_Spin_IsResting( const slayer_spin_t *st );

// Current spin magnitude in rad/sec, for diagnostics.
float Slayer_Spin_Rate( const slayer_spin_t *st );

// Rotate `orient` so the object's local up leans onto a surface whose normal is
// `normal`, easing at `rate` per second. This is the "lies against the wall"
// look for dropped items: the position stays exactly where the server put it,
// only the pose changes, so a weapon wedged in a corner reads as resting on the
// corner instead of standing upright inside it. Does nothing if `normal` is
// degenerate.
void Slayer_Spin_SettleTo( slayer_spin_t *st, const float *normal, float rate, float dt );

// Convert the stored orientation into the Euler angles the studio renderer
// wants, compensating for its pitch negation.
//
// Declared here but IMPLEMENTED IN cl_spin_phys_engine.c, because it needs the
// engine headers and this file must stay compilable with only <math.h> -- that
// is what lets the dynamics above be tested on the host against the real source
// rather than a copy. Every consumer shares this one conversion so the pitch
// compensation cannot drift apart between them.
void Slayer_Spin_PoseToAngles( const slayer_spin_t *st, float *out_angles );
#ifdef __cplusplus
}
#endif

#endif // CL_SPIN_PHYS_SLAYER_H
