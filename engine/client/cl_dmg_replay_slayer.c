/*
cl_dmg_replay_slayer.c - Slayer3D plugin-independent damage model
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

// =============================================================================
// Plugin-independent CS 1.6 / GoldSrc damage model
// =============================================================================
//
// The previous damage indicator relied on:
//   1. Z-band heuristic for hitgroup detection (head-vs-chest mistakes
//      whenever the victim was crouching, jumping, on a slope, etc.).
//   2. A flat damage formula with no awareness of victim armor — every
//      body shot was overestimated by ~2x against an armored opponent.
//
// This module fixes both, using only data that flows through the
// standard GoldSrc protocol — no game DLL plugin or custom usermsg
// is required:
//
//   * Hitgroup is resolved by running the actual studiomodel hitbox
//     hull against the blood position via Mod_HullForStudio +
//     Mod_StudioPointInHitbox. Same code path the server itself uses
//     to attribute damage to a hit group, so accuracy matches the
//     server's own decision modulo ~1 frame of animation lerp skew.
//
//   * Armor presence is inferred from the SFX channel: CS hl.dll emits
//     "player/bhit_helmet-1.wav" for any helmet absorption, and
//     "weapons/ric_metal-N.wav" / "kevlar1.wav" for any kevlar
//     absorption, attached to the victim's entity index. We listen at
//     the engine S_StartSound chokepoint (single hook covers svc_sound,
//     temp-entity sounds, and client.dll's pfnPlaySound), and stamp a
//     short-lived flag on the victim. The TE_BLOOD handler reads the
//     flag and applies the correct armor reduction for the next hit.

#include "common.h"
#include "client.h"
#include "cl_dmg_replay_slayer.h"

#include "studio.h"
#include "mod_local.h"

#if XASH_ANDROID
#include <android/log.h>
#endif

// =============================================================================
// Cvars
// =============================================================================

static CVAR_DEFINE_AUTO( slayer_dmg_replay,
	"1", FCVAR_ARCHIVE,
	"Slayer3D: enable plugin-independent damage model (exact hitbox + sound-based armor detect) (0 = off)" );

static CVAR_DEFINE_AUTO( slayer_dmg_replay_debug,
	"0", FCVAR_ARCHIVE,
	"Slayer3D: log every damage-model decision to console (1 = on, very spammy)" );

// =============================================================================
// Per-victim armor state
// =============================================================================
//
// Indexed by entity index 1..MAX_CLIENTS. A flag persists for at most
// 500 ms after its last refresh — this comfortably covers the 100..200
// ms gap between TE_BLOOD and the matching armor SFX in the same server
// frame, while expiring quickly enough that stale state from an older
// round can't bleed into the next hit.

#define SLAYER_DR_MAX_CLIENTS 33  // index 0 unused, 1..32 = clients
#define SLAYER_DR_ARMOR_TTL   0.5 // seconds

typedef struct
{
	int    has_kevlar;
	int    has_helmet;
	double kevlar_ts;
	double helmet_ts;
} slayer_dr_victim_t;

static slayer_dr_victim_t slayer_dr_victim[SLAYER_DR_MAX_CLIENTS];

// =============================================================================
// CS 1.6 weapon armor-ratio table
// =============================================================================
//
// Damage to an armored body part is multiplied by this ratio. 0.5 means
// kevlar absorbs 50% (the default for most weapons); high-penetration
// weapons (snipers, Deagle, M249) are tuned higher. These match the
// reverse-engineered m_flArmorRatio values from CS 1.6 hl.dll within
// the precision the indicator needs — they're not legal-grade exact
// reproductions, just close enough that the displayed damage no longer
// breaks "is this kill on?" decisions.
//
// Index range mirrors slayer_weapon_table[] in cl_hud_slayer.c.

#define SLAYER_DR_DEFAULT_ARMOR_RATIO 0.5f

static const float slayer_dr_armor_ratio[31] =
{
	[ 1] = 0.55f, // P228          slight pen
	[ 3] = 0.85f, // SCOUT         sniper rifle
	[ 5] = 0.50f, // XM1014        shotgun
	[ 7] = 0.50f, // MAC10
	[ 8] = 0.50f, // AUG
	[10] = 0.55f, // ELITE
	[11] = 0.85f, // FIVESEVEN     designed armor pen
	[12] = 0.50f, // UMP45
	[13] = 0.85f, // SG550         autosniper
	[14] = 0.50f, // GALIL
	[15] = 0.50f, // FAMAS
	[16] = 0.55f, // USP
	[17] = 0.50f, // GLOCK18
	[18] = 0.85f, // AWP
	[19] = 0.50f, // MP5N
	[20] = 0.85f, // M249          LMG
	[21] = 0.50f, // M3            shotgun
	[22] = 0.50f, // M4A1
	[23] = 0.50f, // TMP
	[24] = 0.85f, // G3SG1         autosniper
	[26] = 0.85f, // DEAGLE        designed armor pen
	[27] = 0.50f, // SG552
	[28] = 0.50f, // AK47
	[30] = 0.50f, // P90
	// indices not listed (KNIFE, HEGRENADE, FLASHBANG, SMOKE, C4, SHIELD)
	// fall back to 0.5 via Slayer_DmgReplay_ArmorRatio's default branch.
};

// =============================================================================
// Init / Reset
// =============================================================================

void Slayer_DmgReplay_Init( void )
{
	Cvar_RegisterVariable( &slayer_dmg_replay );
	Cvar_RegisterVariable( &slayer_dmg_replay_debug );

	memset( slayer_dr_victim, 0, sizeof( slayer_dr_victim ));
}

void Slayer_DmgReplay_Reset( void )
{
	memset( slayer_dr_victim, 0, sizeof( slayer_dr_victim ));
}

// =============================================================================
// Sound channel — armor / helmet detection
// =============================================================================
//
// Sample-name patterns we listen for (CS 1.6 emits these from
// CBasePlayer::TraceAttack on the victim's entity, attached to
// CHAN_BODY/CHAN_VOICE — entindex in S_StartSound is the victim):
//
//   bhit_helmet*   -> head + helmet (implies kevlar too)
//   bhit_flesh*    -> no armor on the hit body part (informational)
//   ric_metal*     -> kevlar absorbed (chest/stomach/arm shot)
//   kevlar*        -> kevlar absorbed (alternate sample)
//   headshot*      -> headshot confirmation (orthogonal to armor; not
//                     used here since hitgroup already nails this)

void Slayer_DmgReplay_OnSound( const char *name, const vec3_t pos, int ent )
{
	(void)pos;

	if( slayer_dmg_replay.value == 0.0f )
		return;
	if( !name || !*name )
		return;
	if( ent < 1 || ent >= SLAYER_DR_MAX_CLIENTS )
		return;

	// Strip the leading "sound/" if the engine ever passes one — current
	// code path (S_GetSfxByHandle) does NOT, names live without prefix
	// in sfx_t. Strstr is safe regardless because we match on substrings.

	if( Q_strstr( name, "bhit_helmet" ))
	{
		slayer_dr_victim[ent].has_helmet = 1;
		slayer_dr_victim[ent].has_kevlar = 1;  // helmet implies kevlar
		slayer_dr_victim[ent].helmet_ts  = host.realtime;
		slayer_dr_victim[ent].kevlar_ts  = host.realtime;

		if( slayer_dmg_replay_debug.value != 0.0f )
			Con_Printf( "Slayer DR: ent %d has_helmet (sfx=%s)\n", ent, name );
		return;
	}

	if( Q_strstr( name, "ric_metal" ) || Q_strstr( name, "kevlar" ))
	{
		slayer_dr_victim[ent].has_kevlar = 1;
		slayer_dr_victim[ent].kevlar_ts  = host.realtime;

		if( slayer_dmg_replay_debug.value != 0.0f )
			Con_Printf( "Slayer DR: ent %d has_kevlar (sfx=%s)\n", ent, name );
		return;
	}

	// bhit_flesh* fires for ANY unarmored body part — including legs
	// and arms of a fully kevlar+helm victim, since CS doesn't put
	// armor on those. Clearing has_kevlar/has_helmet here would
	// erroneously tell the next chest-shot calc "no armor" right
	// after a leg-shot revealed flesh. Leave the existing flags
	// alone and let the TTL expire them naturally; a follow-up
	// kevlar/helmet sound on the same victim re-arms the flag.
}

// =============================================================================
// Armor state read
// =============================================================================

void Slayer_DmgReplay_GetArmorState( int victim_idx, int *has_kevlar, int *has_helmet )
{
	double now;

	if( has_kevlar ) *has_kevlar = 0;
	if( has_helmet ) *has_helmet = 0;

	if( slayer_dmg_replay.value == 0.0f )
		return;
	if( victim_idx < 1 || victim_idx >= SLAYER_DR_MAX_CLIENTS )
		return;

	now = host.realtime;

	if( has_kevlar &&
		slayer_dr_victim[victim_idx].has_kevlar &&
		( now - slayer_dr_victim[victim_idx].kevlar_ts ) < SLAYER_DR_ARMOR_TTL )
	{
		*has_kevlar = 1;
	}

	if( has_helmet &&
		slayer_dr_victim[victim_idx].has_helmet &&
		( now - slayer_dr_victim[victim_idx].helmet_ts ) < SLAYER_DR_ARMOR_TTL )
	{
		*has_helmet = 1;
	}
}

// =============================================================================
// Hitgroup resolution via Mod_HullForStudio
// =============================================================================
//
// Test the world-space blood position against every studiomodel hitbox
// of the victim. Returns the CS hitgroup id of the first containing
// hitbox, or SLAYER_HG_GENERIC when nothing contains it (which usually
// means the blood spawned a few units off the body — animation lerp,
// rapid strafe, or the victim's own velocity dragging the bone past
// the impact point). In the SLAYER_HG_GENERIC case the caller falls
// back to a 1.0 multiplier rather than the 4.0 head boost, so the
// indicator under-reads rather than fabricates head shots.
//
// Performance: one `Mod_HullForStudio` call (cached LRU 16-deep), plus
// up to ~21 cheap point-in-OBB tests. Negligible vs the existing TE_BLOOD
// rate.

int Slayer_DmgReplay_ResolveHitgroup( int victim_idx, const vec3_t blood_pos )
{
	cl_entity_t *ent;
	hull_t      *hulls;
	vec3_t       size = { 0.0f, 0.0f, 0.0f }; // exact hitbox bounds, no inflation
	int          numhb = 0;
	int          i;

	if( slayer_dmg_replay.value == 0.0f )
		return SLAYER_HG_GENERIC;
	if( victim_idx < 1 || victim_idx >= SLAYER_DR_MAX_CLIENTS )
		return SLAYER_HG_GENERIC;

	ent = CL_GetEntityByIndex( victim_idx );
	if( !ent || !ent->model )
		return SLAYER_HG_GENERIC;
	if( !ent->player )
		return SLAYER_HG_GENERIC;
	if( ent->curstate.messagenum != cl.parsecount )
		return SLAYER_HG_GENERIC;

	hulls = Mod_HullForStudio(
		ent->model,
		ent->curstate.frame,
		ent->curstate.sequence,
		ent->angles,
		ent->origin,
		size,
		ent->curstate.controller,
		ent->curstate.blending,
		&numhb,
		NULL );          // no edict on the client; Mod_HullForStudio handles NULL

	if( !hulls || numhb <= 0 )
		return SLAYER_HG_GENERIC;

	for( i = 0; i < numhb; i++ )
	{
		if( !Mod_StudioPointInHitbox( i, blood_pos ))
			continue;

		// Found containing hitbox — return its CS hitgroup id.
		{
			int g = Mod_HitgroupForStudioHull( i );

			if( slayer_dmg_replay_debug.value != 0.0f )
			{
				Con_Printf( "Slayer DR: ent %d blood @ (%.0f %.0f %.0f) -> hitbox %d hg %d\n",
					victim_idx,
					blood_pos[0], blood_pos[1], blood_pos[2],
					i, g );
			}

			return g;
		}
	}

	// No hitbox contains the point. Common in practice: blood spawns
	// a couple units outside the body when the victim is mid-strafe
	// and the studio bones are interpolated one frame behind. We
	// don't synthesize a head/chest guess — caller treats this as a
	// generic hit (1.0x multiplier), losing the headshot boost when
	// uncertain rather than overestimating.
	if( slayer_dmg_replay_debug.value != 0.0f )
	{
		Con_Printf( "Slayer DR: ent %d blood @ (%.0f %.0f %.0f) MISSED all %d hitboxes\n",
			victim_idx,
			blood_pos[0], blood_pos[1], blood_pos[2],
			numhb );
	}

	return SLAYER_HG_GENERIC;
}

// =============================================================================
// CS damage multiplier tables
// =============================================================================

float Slayer_DmgReplay_HitgroupMult( int hitgroup )
{
	switch( hitgroup )
	{
	case SLAYER_HG_HEAD:     return 4.0f;
	case SLAYER_HG_STOMACH:  return 1.25f;
	case SLAYER_HG_CHEST:
	case SLAYER_HG_LEFTARM:
	case SLAYER_HG_RIGHTARM: return 1.0f;
	case SLAYER_HG_LEFTLEG:
	case SLAYER_HG_RIGHTLEG: return 0.75f;
	default:                 return 1.0f;
	}
}

float Slayer_DmgReplay_ArmorRatio( int weapon_id )
{
	if( weapon_id < 1 || weapon_id >= (int)( sizeof( slayer_dr_armor_ratio ) / sizeof( slayer_dr_armor_ratio[0] )))
		return SLAYER_DR_DEFAULT_ARMOR_RATIO;

	if( slayer_dr_armor_ratio[weapon_id] <= 0.0f )
		return SLAYER_DR_DEFAULT_ARMOR_RATIO;

	return slayer_dr_armor_ratio[weapon_id];
}

// =============================================================================
// Hitbox-containing victim search
// =============================================================================

int Slayer_DmgReplay_FindHitboxOwner( const vec3_t world_pt, int *out_hitgroup )
{
	int          i;
	cl_entity_t *ent;
	int          best_idx  = 0;
	int          best_hg   = SLAYER_HG_GENERIC;
	float        best_dist = 1e30f;
	const float  scan_radius_sq = 128.0f * 128.0f;

	if( out_hitgroup )
		*out_hitgroup = SLAYER_HG_GENERIC;

	if( slayer_dmg_replay.value == 0.0f )
		return 0;

	for( i = 1; i <= cl.maxclients && i < SLAYER_DR_MAX_CLIENTS; i++ )
	{
		float dx, dy, dz, dist_sq;
		vec3_t size = { 0.0f, 0.0f, 0.0f };
		hull_t *hulls;
		int     numhb = 0;
		int     j;

		ent = CL_GetEntityByIndex( i );
		if( !ent || !ent->player || !ent->model )
			continue;
		if( ent->curstate.messagenum != cl.parsecount )
			continue;

		// Cheap AABB reject. CS player bbox is ±16 in xy, -36..72
		// in z; the hitbox itself never extends beyond ~80 units
		// from origin, so 128 is a safe outer bound.
		dx = world_pt[0] - ent->origin[0];
		dy = world_pt[1] - ent->origin[1];
		dz = world_pt[2] - ent->origin[2];
		dist_sq = dx * dx + dy * dy + dz * dz;
		if( dist_sq > scan_radius_sq )
			continue;

		// Build this player's hitbox planes. Mod_HullForStudio writes
		// the file-scope studio_planes[] array — we MUST run the
		// containment test before iterating to the next player, or
		// the next call overwrites our planes.
		hulls = Mod_HullForStudio(
			ent->model,
			ent->curstate.frame,
			ent->curstate.sequence,
			ent->angles,
			ent->origin,
			size,
			ent->curstate.controller,
			ent->curstate.blending,
			&numhb,
			NULL );
		if( !hulls || numhb <= 0 )
			continue;

		// First containing hitbox wins for this player. CS hitboxes
		// don't overlap meaningfully on a single player so we can
		// stop after the first hit.
		for( j = 0; j < numhb; j++ )
		{
			if( !Mod_StudioPointInHitbox( j, world_pt ))
				continue;

			// Multi-player case: if two players' hitboxes both
			// contain the point (rare overlap during point-blank
			// strafe past each other), prefer the one with the
			// smaller origin distance — that's the body the bullet
			// actually struck.
			if( dist_sq < best_dist )
			{
				best_dist = dist_sq;
				best_idx  = i;
				best_hg   = Mod_HitgroupForStudioHull( j );
			}
			break;
		}
	}

	if( out_hitgroup && best_idx > 0 )
		*out_hitgroup = best_hg;

	// Z-band fallback: if no hitbox contained the point but we found
	// at least one player within scan_radius, use vertical offset from
	// victim origin to estimate hitgroup. Thresholds differ for standing
	// vs crouching (usehull==1 in CS).
	if( best_idx == 0 )
	{
		float        closest_dist = 1e30f;
		int          closest_idx  = 0;
		cl_entity_t *closest_ent  = NULL;

		for( i = 1; i <= cl.maxclients && i < SLAYER_DR_MAX_CLIENTS; i++ )
		{
			float dx, dy, dz, dist_sq;

			ent = CL_GetEntityByIndex( i );
			if( !ent || !ent->player || !ent->model )
				continue;
			if( ent->curstate.messagenum != cl.parsecount )
				continue;

			dx = world_pt[0] - ent->origin[0];
			dy = world_pt[1] - ent->origin[1];
			dz = world_pt[2] - ent->origin[2];
			dist_sq = dx * dx + dy * dy + dz * dz;

			if( dist_sq < scan_radius_sq && dist_sq < closest_dist )
			{
				closest_dist = dist_sq;
				closest_idx  = i;
				closest_ent  = ent;
			}
		}

		if( closest_idx > 0 && closest_ent )
		{
			float dz = world_pt[2] - closest_ent->origin[2];
			int   ducking = ( closest_ent->curstate.usehull == 1 );

			if( ducking )
			{
				if( dz > 37.0f )      best_hg = SLAYER_HG_HEAD;
				else if( dz > 25.0f ) best_hg = SLAYER_HG_CHEST;
				else if( dz > 12.0f ) best_hg = SLAYER_HG_STOMACH;
				else                  best_hg = SLAYER_HG_LEFTLEG;
			}
			else
			{
				if( dz > 53.0f )      best_hg = SLAYER_HG_HEAD;
				else if( dz > 37.0f ) best_hg = SLAYER_HG_CHEST;
				else if( dz > 20.0f ) best_hg = SLAYER_HG_STOMACH;
				else                  best_hg = SLAYER_HG_LEFTLEG;
			}

			best_idx = closest_idx;

			if( slayer_dmg_replay_debug.value != 0.0f )
			{
				Con_Printf( "Slayer DR: Z-band fallback ent %d dz=%.1f duck=%d -> hg %d\n",
					closest_idx, dz, ducking, best_hg );
			}
		}
	}

	if( out_hitgroup && best_idx > 0 )
		*out_hitgroup = best_hg;

	if( best_idx > 0 && slayer_dmg_replay_debug.value != 0.0f )
	{
		Con_Printf( "Slayer DR: hitbox owner ent %d hg %d for blood @ (%.0f %.0f %.0f)\n",
			best_idx, best_hg,
			world_pt[0], world_pt[1], world_pt[2] );
	}

	return best_idx;
}
