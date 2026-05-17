/*
cl_view_slayer.c - Slayer3D third-person camera extension
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

#include "common.h"
#include "client.h"
#include "cl_view_slayer.h"

// ---------------------------------------------------------------------------
// Cvars
// ---------------------------------------------------------------------------

static CVAR_DEFINE_AUTO( slayer_thirdperson, "0",   FCVAR_ARCHIVE, "enable Slayer3D third-person camera" );
static CVAR_DEFINE_AUTO( slayer_cam_ofs,     "120", FCVAR_ARCHIVE, "Slayer3D third-person camera distance" );
static CVAR_DEFINE_AUTO( slayer_cam_clip,    "1",   FCVAR_ARCHIVE, "Slayer3D third-person wall clipping (1=trace, 0=through walls)" );
static CVAR_DEFINE_AUTO( slayer_cam_free,    "0",   FCVAR_ARCHIVE, "Slayer3D third-person free look" );
static CVAR_DEFINE_AUTO( slayer_cam_pitch,   "0",   FCVAR_ARCHIVE, "Slayer3D free-look camera pitch" );
static CVAR_DEFINE_AUTO( slayer_cam_yaw,     "0",   FCVAR_ARCHIVE, "Slayer3D free-look camera yaw" );

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

#define SLAYER_CAM_MIN_OFS  0.0f
#define SLAYER_CAM_MAX_OFS  256.0f
// Distance kept from a contact surface so the camera does not z-fight with it.
#define SLAYER_CAM_PADDING  4.0f

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void V_InitSlayerCvars( void )
{
	Cvar_RegisterVariable( &slayer_thirdperson );
	Cvar_RegisterVariable( &slayer_cam_ofs );
	Cvar_RegisterVariable( &slayer_cam_clip );
	Cvar_RegisterVariable( &slayer_cam_free );
	Cvar_RegisterVariable( &slayer_cam_pitch );
	Cvar_RegisterVariable( &slayer_cam_yaw );
}

qboolean V_IsSlayerThirdPerson( void )
{
	return slayer_thirdperson.value != 0.0f;
}

void V_ApplySlayerThirdPerson( ref_viewpass_t *rvp )
{
	vec3_t    forward;
	vec3_t    camangles;
	vec3_t    ideal_org;
	float     ofs;
	pmtrace_t *tr;

	if( !V_IsSlayerThirdPerson( ))
		return;

	// Free-look uses dedicated cvars so the camera can orbit without
	// affecting the player's aim. Otherwise the camera follows the
	// player's view angles.
	if( slayer_cam_free.value != 0.0f )
	{
		camangles[PITCH] = slayer_cam_pitch.value;
		camangles[YAW]   = slayer_cam_yaw.value;
		camangles[ROLL]  = 0.0f;
	}
	else
	{
		VectorCopy( rvp->viewangles, camangles );
	}

	// Only the forward axis is needed; AngleVectors accepts NULLs.
	AngleVectors( camangles, forward, NULL, NULL );

	ofs = bound( SLAYER_CAM_MIN_OFS, slayer_cam_ofs.value, SLAYER_CAM_MAX_OFS );
	VectorMA( rvp->vieworigin, -ofs, forward, ideal_org );

	if( slayer_cam_clip.value != 0.0f && ofs > 0.0f )
	{
		// PM_CL_TraceLine returns a pointer to a static pmtrace_t inside
		// pm_trace.c; do not store the pointer past this call.
		tr = PM_CL_TraceLine( rvp->vieworigin, ideal_org,
			PM_TRACELINE_PHYSENTSONLY, 2 /* small hull */, -1 );

		if( tr->fraction < 1.0f )
		{
			// Pull the camera back along the contact normal so it does
			// not z-fight with the surface it just hit.
			VectorMA( tr->endpos, SLAYER_CAM_PADDING, tr->plane.normal,
				rvp->vieworigin );
		}
		else
		{
			VectorCopy( ideal_org, rvp->vieworigin );
		}
	}
	else
	{
		VectorCopy( ideal_org, rvp->vieworigin );
	}

	// Render-only override of the angles. cl.viewangles (used for
	// movement / aiming) is left untouched on purpose.
	VectorCopy( camangles, rvp->viewangles );
}
