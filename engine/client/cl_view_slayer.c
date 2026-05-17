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
// Console commands for binding camera rotation keys
// ---------------------------------------------------------------------------

static void Cmd_SlayerCamYaw_f( void )
{
	float delta;

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( "usage: slayer_camyaw <degrees>\n" );
		return;
	}

	delta = Q_atof( Cmd_Argv( 1 ));
	Cvar_SetValue( "slayer_cam_yaw", slayer_cam_yaw.value + delta );
}

static void Cmd_SlayerCamPitch_f( void )
{
	float delta, v;

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( "usage: slayer_campitch <degrees>\n" );
		return;
	}

	delta = Q_atof( Cmd_Argv( 1 ));
	v = bound( -89.0f, slayer_cam_pitch.value + delta, 89.0f );
	Cvar_SetValue( "slayer_cam_pitch", v );
}

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

	Cmd_AddCommand( "slayer_camyaw", Cmd_SlayerCamYaw_f,
		"rotate Slayer3D free-look camera by N degrees on yaw axis" );
	Cmd_AddCommand( "slayer_campitch", Cmd_SlayerCamPitch_f,
		"tilt Slayer3D free-look camera by N degrees on pitch axis" );
}

qboolean V_IsSlayerThirdPerson( void )
{
	return slayer_thirdperson.value != 0.0f;
}

void V_ApplySlayerThirdPerson( ref_viewpass_t *rvp )
{
	// Tracks rising/falling edges of slayer_cam_free so we can snap the
	// camera angles to the player's current view at the moment free look
	// is activated, instead of jumping to (0, 0) which feels broken.
	// -1 = third-person off entirely; 0 = third-person on, free look off;
	// 1  = third-person on, free look on.
	static int free_state = -1;

	vec3_t    forward;
	vec3_t    camangles;
	vec3_t    ideal_org;
	float     ofs;
	pmtrace_t *tr;

	if( !V_IsSlayerThirdPerson( ))
	{
		free_state = -1;
		return;
	}

	// Free look uses the dedicated slayer_cam_pitch / slayer_cam_yaw
	// cvars so the camera can orbit without affecting the player's
	// aim. Bind keys to the slayer_camyaw / slayer_campitch console
	// commands (or use 'incrementvar') to actually rotate it.
	if( slayer_cam_free.value != 0.0f )
	{
		// Rising edge: anchor the cvars to the current player view so
		// the camera does not snap to whatever stale value was saved in
		// config.cfg (typically 0, 0).
		if( free_state != 1 )
		{
			Cvar_SetValue( "slayer_cam_pitch", rvp->viewangles[PITCH] );
			Cvar_SetValue( "slayer_cam_yaw",   rvp->viewangles[YAW] );
			free_state = 1;
		}

		camangles[PITCH] = slayer_cam_pitch.value;
		camangles[YAW]   = slayer_cam_yaw.value;
		camangles[ROLL]  = 0.0f;
	}
	else
	{
		free_state = 0;
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
