/*
cl_view_slayer.c - Slayer3D third-person camera + kill-sound module
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
#include "cl_scoreboard_slayer.h"

// ===========================================================================
// Cvars - Third-person camera
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_thirdperson, "0",   FCVAR_ARCHIVE, "enable Slayer3D third-person camera" );
static CVAR_DEFINE_AUTO( slayer_cam_ofs,     "120", FCVAR_ARCHIVE, "Slayer3D third-person camera distance" );
static CVAR_DEFINE_AUTO( slayer_cam_clip,    "1",   FCVAR_ARCHIVE, "Slayer3D third-person wall clipping (1=trace, 0=through walls)" );
static CVAR_DEFINE_AUTO( slayer_cam_free,    "0",   FCVAR_ARCHIVE, "Slayer3D third-person free look" );
static CVAR_DEFINE_AUTO( slayer_cam_pitch,   "0",   FCVAR_ARCHIVE, "Slayer3D free-look camera pitch" );
static CVAR_DEFINE_AUTO( slayer_cam_yaw,     "0",   FCVAR_ARCHIVE, "Slayer3D free-look camera yaw" );
static CVAR_DEFINE_AUTO( slayer_cam_snap,    "1",   FCVAR_ARCHIVE, "Slayer3D: snap player yaw to camera on RMB when cam_free is active" );
static CVAR_DEFINE_AUTO( slayer_cam_snap_cd, "0.15", FCVAR_ARCHIVE, "Slayer3D: cooldown in seconds between cam snaps" );

// ===========================================================================
// Cvars - Chat color
// ===========================================================================

CVAR_DEFINE_AUTO( slayer_chat_color, "", FCVAR_ARCHIVE, "Slayer3D: chat message body color R G B (empty = disabled)" );
CVAR_DEFINE_AUTO( slayer_chat_color_t, "", FCVAR_ARCHIVE, "Slayer3D: Terrorist name color R G B (empty = default)" );
CVAR_DEFINE_AUTO( slayer_chat_color_ct, "", FCVAR_ARCHIVE, "Slayer3D: CT name color R G B (empty = default)" );

// ===========================================================================
// Cvars - Movement tweaks
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_ducktap,    "0", FCVAR_ARCHIVE, "Slayer3D: enable ducktap (rapid duck toggling for duckrun speed)" );
static CVAR_DEFINE_AUTO( slayer_autostrafe, "0", FCVAR_ARCHIVE, "Slayer3D: enable automatic air-strafing based on yaw delta" );
static CVAR_DEFINE_AUTO( slayer_autojump,   "0", FCVAR_ARCHIVE, "Slayer3D: enable auto-bhop with ladder safety and anti-bhop bypass" );

// ===========================================================================
// Cvars - Smooth zoom
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_smooth_zoom, "1", FCVAR_ARCHIVE, "Slayer3D: enable smooth FOV zoom animation" );
static CVAR_DEFINE_AUTO( slayer_smooth_zoom_speed, "12.0", FCVAR_ARCHIVE, "Slayer3D: smooth zoom interpolation speed" );

// ===========================================================================
// Cvars - Kill-sound
// ===========================================================================

// Sound played when the local player KILLS someone. Empty = disabled.
static CVAR_DEFINE_AUTO( slayer_killsound,          "weapons/explode3.wav", FCVAR_ARCHIVE, "Slayer3D: sound when local player gets a kill (empty = off)" );
static CVAR_DEFINE_AUTO( slayer_killsound_headshot, "",                     FCVAR_ARCHIVE, "Slayer3D: sound for headshot kills (empty = use slayer_killsound)" );
static CVAR_DEFINE_AUTO( slayer_killsound_teamkill, "",                     FCVAR_ARCHIVE, "Slayer3D: sound for teamkills (empty = use slayer_killsound)" );
static CVAR_DEFINE_AUTO( slayer_killsound_volume,   "1.0",                  FCVAR_ARCHIVE, "Slayer3D: kill sound volume, 0..1" );

// ===========================================================================
// Tunables
// ===========================================================================

#define SLAYER_CAM_MIN_OFS  0.0f
#define SLAYER_CAM_MAX_OFS  256.0f
// Distance kept from a contact surface so the camera does not z-fight.
#define SLAYER_CAM_PADDING  4.0f

// ===========================================================================
// +ducktap / -ducktap command pair
// ===========================================================================

static int slayer_ducktap_active = 0;

// Movement tweak state (file-scoped so Slayer_ResetMatchState can clear them)
static float slayer_prev_yaw = 0.0f;
static int   slayer_prev_onground = 0;
static int   slayer_yaw_initialized = 0;

static void Cmd_DucktapDown_f( void )
{
	slayer_ducktap_active = 1;
}

static void Cmd_DucktapUp_f( void )
{
	slayer_ducktap_active = 0;
}

// ===========================================================================
// Console commands for binding camera rotation keys
// ===========================================================================

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

// ===========================================================================
// Public API - Cvar registration
// ===========================================================================

void V_InitSlayerCvars( void )
{
	// Camera
	Cvar_RegisterVariable( &slayer_thirdperson );
	Cvar_RegisterVariable( &slayer_cam_ofs );
	Cvar_RegisterVariable( &slayer_cam_clip );
	Cvar_RegisterVariable( &slayer_cam_free );
	Cvar_RegisterVariable( &slayer_cam_pitch );
	Cvar_RegisterVariable( &slayer_cam_yaw );
	Cvar_RegisterVariable( &slayer_cam_snap );
	Cvar_RegisterVariable( &slayer_cam_snap_cd );

	// Kill-sound
	Cvar_RegisterVariable( &slayer_killsound );

	// Chat color
	Cvar_RegisterVariable( &slayer_chat_color );
	Cvar_RegisterVariable( &slayer_chat_color_t );
	Cvar_RegisterVariable( &slayer_chat_color_ct );
	Cvar_RegisterVariable( &slayer_killsound_headshot );
	Cvar_RegisterVariable( &slayer_killsound_teamkill );
	Cvar_RegisterVariable( &slayer_killsound_volume );

	// Console commands for free-look camera rotation
	Cmd_AddCommand( "slayer_camyaw", Cmd_SlayerCamYaw_f,
		"rotate Slayer3D free-look camera by N degrees on yaw axis" );
	Cmd_AddCommand( "slayer_campitch", Cmd_SlayerCamPitch_f,
		"tilt Slayer3D free-look camera by N degrees on pitch axis" );

	// Movement tweaks
	Cvar_RegisterVariable( &slayer_ducktap );
	Cvar_RegisterVariable( &slayer_autostrafe );
	Cvar_RegisterVariable( &slayer_autojump );
	Cmd_AddCommand( "+ducktap", Cmd_DucktapDown_f,
		"begin rapid duck toggling (ducktap)" );
	Cmd_AddCommand( "-ducktap", Cmd_DucktapUp_f,
		"stop rapid duck toggling (ducktap)" );

	// Smooth zoom
	Cvar_RegisterVariable( &slayer_smooth_zoom );
	Cvar_RegisterVariable( &slayer_smooth_zoom_speed );

	// Initialize per-match state
	Slayer_ResetMatchState();

	// Scoreboard
	Slayer_Scoreboard_Init();

	Con_Printf( "Slayer3D: cvars initialized\n" );
}

// ===========================================================================
// Third-person camera
// ===========================================================================

qboolean V_IsSlayerThirdPerson( void )
{
	return slayer_thirdperson.value != 0.0f;
}

qboolean V_IsSlayerCamFree( void )
{
	return V_IsSlayerThirdPerson() && slayer_cam_free.value != 0.0f;
}

void V_SlayerCamLook( float yaw_delta, float pitch_delta )
{
	float p;

	// Apply yaw (unbounded, wraps around)
	// Positive yaw_delta = finger moves right = camera rotates right
	Cvar_SetValue( "slayer_cam_yaw", slayer_cam_yaw.value + yaw_delta );

	// Apply pitch (clamped to -89..89)
	p = bound( -89.0f, slayer_cam_pitch.value + pitch_delta, 89.0f );
	Cvar_SetValue( "slayer_cam_pitch", p );
}

void V_SlayerCamSnapCheck( usercmd_t *cmd )
{
	float yaw;

	// Feature must be enabled and free-look must be active
	if( slayer_cam_snap.value == 0.0f || !V_IsSlayerCamFree() )
		return;

	// Only act while RMB is held
	if( !( cmd->buttons & IN_ATTACK2 ) )
		return;

	// Strip IN_ATTACK2 so the game DLL does not perform a secondary attack
	cmd->buttons &= ~IN_ATTACK2;

	// Normalize yaw to 0..360 before writing into viewangles
	yaw = anglemod( slayer_cam_yaw.value );

	// Continuously align player view to camera direction while held
	cl.viewangles[YAW] = yaw;
	cl.viewangles[PITCH] = slayer_cam_pitch.value;
	cmd->viewangles[YAW] = yaw;
	cmd->viewangles[PITCH] = slayer_cam_pitch.value;
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

// ===========================================================================
// Kill-sound - Per-match state
// ===========================================================================

// Team name lookup populated by the server-sent "TeamInfo" message
// (CS / DM-style mods). Indexed by 1-based entindex so [0] is unused.
#define SLAYER_TEAM_LEN  16
static char slayer_player_team[MAX_CLIENTS + 1][SLAYER_TEAM_LEN];

void Slayer_ResetMatchState( void )
{
	memset( slayer_player_team, 0, sizeof( slayer_player_team ));

	// Reset movement tweak statics so stale values from a prior map
	// do not bleed into the first frame of a new one.
	slayer_prev_yaw = 0.0f;
	slayer_prev_onground = 0;
	slayer_yaw_initialized = 0;
	slayer_ducktap_active = 0; // clear sticky +ducktap on disconnect

	// Clear scoreboard score data
	Slayer_Scoreboard_Reset();
}

// ===========================================================================
// Kill-sound - Helpers
// ===========================================================================

// Returns true when both players have a known, non-empty team and
// those teams match. Suicide and self-kill are not teamkills.
static qboolean Slayer_IsTeamkill( int killer, int victim )
{
	const char *kt, *vt;

	if( killer == victim )
		return false;
	if( killer < 1 || killer > MAX_CLIENTS )
		return false;
	if( victim < 1 || victim > MAX_CLIENTS )
		return false;

	kt = slayer_player_team[killer];
	vt = slayer_player_team[victim];

	if( COM_StringEmptyOrNULL( kt ) || COM_StringEmptyOrNULL( vt ))
		return false;

	return Q_stricmp( kt, vt ) == 0 ? true : false;
}

// Pick which sound cvar to use for this kill. Priority is
// teamkill > headshot > generic. Falls back to slayer_killsound when
// the more specific cvar is empty.
static const char *Slayer_PickKillSound( qboolean is_teamkill, qboolean is_headshot )
{
	const char *fallback = slayer_killsound.string;

	if( is_teamkill && !COM_StringEmptyOrNULL( slayer_killsound_teamkill.string ))
		return slayer_killsound_teamkill.string;

	if( is_headshot && !COM_StringEmptyOrNULL( slayer_killsound_headshot.string ))
		return slayer_killsound_headshot.string;

	if( COM_StringEmptyOrNULL( fallback ))
		return NULL;

	return fallback;
}

// ===========================================================================
// Kill-sound - User message hooks
// ===========================================================================

void Slayer_OnTeamInfo( const byte *pbuf, int iSize )
{
	int slot;

	// TeamInfo layout: byte client_slot (1..MAX_CLIENTS), then a
	// NUL-terminated ASCII team name. Anything shorter than 2 bytes
	// is malformed, ignore.
	if( !pbuf || iSize < 2 )
		return;

	slot = pbuf[0];
	if( slot < 1 || slot > MAX_CLIENTS )
		return;

	// Defensive copy: pbuf is not necessarily NUL-terminated within
	// iSize, so we cap the read.
	{
		int max = iSize - 1;
		if( max >= SLAYER_TEAM_LEN )
			max = SLAYER_TEAM_LEN - 1;
		Q_strncpy( slayer_player_team[slot], (const char *)( pbuf + 1 ), max + 1 );
	}
}

void Slayer_OnDeathMsg( const byte *pbuf, int iSize )
{
	const char *snd;
	float       vol;
	int         killer, victim;
	qboolean    is_teamkill = false;
	qboolean    is_headshot = false;
	const char *weapon_str = NULL;

	// HL/CS DeathMsg layout: byte killer_id, byte victim_id, ...optional rest
	if( !pbuf || iSize < 2 )
		return;

	killer = pbuf[0];
	victim = pbuf[1];

	// Headshot detection. In CS/CSCZ the DeathMsg layout is:
	//   byte killer, byte victim, byte headshot(0/1), string weapon
	// In vanilla HL/DM it's:
	//   byte killer, byte victim, string weapon
	// We detect headshot if the third byte is exactly 0 or 1 AND there
	// are at least 4 bytes (room for the weapon string after the flag).
	// This heuristic works across CS, CSCZ, and bot mods that mimic
	// the CS format. For non-CS mods where pbuf[2] is the first char
	// of the weapon name (always >= 0x20), it safely won't trigger.
	if( iSize >= 4 && pbuf[2] <= 1 )
	{
		is_headshot = ( pbuf[2] == 1 );
		weapon_str = (const char *)( pbuf + 3 );
	}
	else if( iSize >= 3 )
	{
		weapon_str = (const char *)( pbuf + 2 );
	}

	// --- Killsound logic: only for LOCAL player kills ---
	if( killer == cl.playernum + 1 && killer != victim && killer != 0 )
	{
		is_teamkill = Slayer_IsTeamkill( killer, victim );

		snd = Slayer_PickKillSound( is_teamkill, is_headshot );
		if( !COM_StringEmptyOrNULL( snd ))
		{
			vol = bound( 0.0f, slayer_killsound_volume.value, 1.0f );
			if( vol > 0.0f )
				S_StartLocalSound( snd, vol, false );
		}
	}

	(void)weapon_str; // used by killsound path only for future extensions
}

// ===========================================================================
// Movement tweaks - ducktap, autostrafe, autojump
// ===========================================================================

void V_SlayerMovementTweaks( usercmd_t *cmd )
{
	int do_autojump = 0;

	// --- Ducktap ---
	if( slayer_ducktap.value != 0.0f && slayer_ducktap_active )
	{
		if( host.framecount & 1 )
			cmd->buttons &= ~IN_DUCK;
		else
			cmd->buttons |= IN_DUCK;

		// On ground: inject forward for duckrun speed boost
		// In air: do NOT inject forward (let autostrafe handle pure strafing)
		if( cl.local.onground >= 0 )
		{
			cmd->forwardmove = 400.0f;
			cmd->buttons |= IN_FORWARD;
		}
	}

	// --- Autostrafe ---
	if( slayer_autostrafe.value != 0.0f && cl.local.onground == -1 )
	{
		float delta;

		if( !slayer_yaw_initialized )
		{
			slayer_prev_yaw = cmd->viewangles[YAW];
			slayer_yaw_initialized = 1;
		}

		delta = cmd->viewangles[YAW] - slayer_prev_yaw;

		// Normalize to -180..180
		if( delta > 180.0f )
			delta -= 360.0f;
		if( delta < -180.0f )
			delta += 360.0f;

		if( delta < -0.1f )
		{
			cmd->sidemove = -400.0f;
			cmd->buttons |= IN_MOVELEFT;
			cmd->buttons &= ~IN_MOVERIGHT;
		}
		else if( delta > 0.1f )
		{
			cmd->sidemove = 400.0f;
			cmd->buttons |= IN_MOVERIGHT;
			cmd->buttons &= ~IN_MOVELEFT;
		}

		// In air: NEVER inject forwardmove - it kills air acceleration
		// Clear any forward/back input to maximize strafe gain
		cmd->forwardmove = 0.0f;
		cmd->buttons &= ~(IN_FORWARD | IN_BACK);
	}

	// Always update prev_yaw so delta is frame-to-frame
	slayer_prev_yaw = cmd->viewangles[YAW];
	slayer_yaw_initialized = 1;

	// --- Autojump ---
	// Determine if autojump should process this frame.
	// Ladder safety: do not auto-bhop on ladders (MOVETYPE_FLY).
	if( slayer_autojump.value != 0.0f && ( cmd->buttons & IN_JUMP ))
	{
		if( clgame.pmove == NULL || clgame.pmove->movetype != MOVETYPE_FLY )
			do_autojump = 1;
	}

	if( do_autojump )
	{
		if( cl.local.onground >= 0 )
		{
			// Just landed: release IN_JUMP for one frame (anti-bhop bypass)
			if( slayer_prev_onground == -1 )
				cmd->buttons &= ~IN_JUMP;
			else
				cmd->buttons |= IN_JUMP;
		}
	}

	slayer_prev_onground = cl.local.onground;
}

// ===========================================================================
// Smooth zoom - smooth FOV zoom animation
// ===========================================================================

void V_SlayerSmoothZoom( ref_viewpass_t *rvp )
{
	static float smooth_fov = 90.0f;
	float target, speed, diff;

	target = rvp->fov_x;

	// If disabled, pass through without smoothing
	if( slayer_smooth_zoom.value == 0.0f )
	{
		smooth_fov = target;
		return;
	}

	diff = target - smooth_fov;

	// Avoid permanent drift: snap when difference is negligible
	if( fabs( diff ) < 0.01f )
	{
		smooth_fov = target;
		rvp->fov_x = smooth_fov;
		rvp->fov_y = V_CalcFov( &rvp->fov_x, clgame.viewport[2], clgame.viewport[3] );
		return;
	}

	speed = slayer_smooth_zoom_speed.value;
	{
		float t = bound( 0.0f, speed * host.frametime, 1.0f );
		smooth_fov += diff * t;
	}

	rvp->fov_x = smooth_fov;
	rvp->fov_y = V_CalcFov( &rvp->fov_x, clgame.viewport[2], clgame.viewport[3] );
}
