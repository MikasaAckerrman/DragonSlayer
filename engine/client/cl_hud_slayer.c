/*
cl_hud_slayer.c - Slayer3D crosshair-area HUD overlay (damage indicator)
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
// Damage indicator
// =============================================================================
//
// Server plugins (AmxModX hit-feedback, ReGameDLL custom messages, ...)
// commonly broadcast a per-attacker user-message whenever a player lands a
// damaging shot. The payload is normally:
//
//   byte  damage  (0..255)
//   byte? hitgroup_or_victim  (optional, ignored here)
//
// We hook several known message names so the indicator works across server
// configurations without per-server tuning. Unknown servers simply produce
// no events and the feature is invisible.
//
// Each registered hit picks the next slot in a 16-entry ring with
// pre-computed halo offsets around the screen center. New hits never
// overwrite a still-fading older hit at the same pixel — the ring always
// rotates to the next halo position. Entries fade out linearly in their
// final 0.5s of life.

#include "common.h"
#include "client.h"
#include "cl_hud_slayer.h"
#include "cl_dmg_replay_slayer.h"

#if XASH_ANDROID
#include <android/log.h>
#endif

// =============================================================================
// Cvars
// =============================================================================

static CVAR_DEFINE_AUTO( slayer_damage_indicator,
	"1", FCVAR_ARCHIVE,
	"Slayer3D: show fading damage number near crosshair when you hit a player (0 = off)" );

static CVAR_DEFINE_AUTO( slayer_damage_indicator_color,
	"255 80 80 255", FCVAR_ARCHIVE,
	"Slayer3D: damage indicator RGBA color" );

static CVAR_DEFINE_AUTO( slayer_damage_indicator_duration,
	"1.5", FCVAR_ARCHIVE,
	"Slayer3D: damage indicator total visible time (seconds, fades over the last 0.5s)" );

// Per-victim hue rotation. With this on, every enemy gets a distinct,
// stable color for their damage numbers — close-quarters fights against
// two or three opponents become readable at a glance instead of a wall
// of identical red digits. The base slayer_damage_indicator_color cvar
// is used only when a hit cannot be attributed to a specific victim.
static CVAR_DEFINE_AUTO( slayer_damage_indicator_per_victim_color,
	"1", FCVAR_ARCHIVE,
	"Slayer3D: rotate hue per victim so adjacent enemies have distinct numbers (0 = single color)" );

// =============================================================================
// Halo offsets
// =============================================================================
//
// 16 deterministic positions around the crosshair. Consecutive hits
// step through these in order so the second, third, ... hits can never
// stomp on top of the first one before it has faded.
//
// Layout (rough): inner 4 corners -> cardinal axes -> outer 8 in a wider ring.

static const struct
{
	int x, y;
} slayer_dmg_halo[16] =
{
	{ -50, -25 }, {  50, -25 }, { -50,  25 }, {  50,  25 },
	{ -78,   0 }, {  78,   0 }, {   0, -45 }, {   0,  45 },
	{ -65, -55 }, {  65, -55 }, { -65,  55 }, {  65,  55 },
	{ -35, -68 }, {  35, -68 }, { -35,  68 }, {  35,  68 },
};
#define SLAYER_DMG_HALO_COUNT ( sizeof( slayer_dmg_halo ) / sizeof( slayer_dmg_halo[0] ) )

// =============================================================================
// Per-entry state
// =============================================================================

typedef struct
{
	int    damage;       // 0 = empty slot
	double expire_time;  // host.realtime when this entry must vanish
	int    halo_slot;    // index into slayer_dmg_halo
	int    victim_idx;   // 1..32 player entindex, 0 = unknown / generic
} slayer_dmg_entry_t;

// Ring of in-flight hits. SLAYER_DMG_HALO_COUNT also bounds the ring so
// the worst case is "one entry per halo position".
static slayer_dmg_entry_t slayer_dmg_entries[SLAYER_DMG_HALO_COUNT];
static int                slayer_dmg_next_slot;  // round-robin halo cursor

// Cached cvar parse state (rgba_t buffer + raw string).
static char   slayer_dmg_color_str[64] = "";
static rgba_t slayer_dmg_color_cached  = { 255, 80, 80, 255 };

// Anti-duplicate: timestamp of last plugin-sourced damage registration.
// TE_BLOOD handler skips if a plugin already registered damage within 100ms.
static double slayer_last_plugin_hit_time = 0;

// Current weapon ID tracked via CurWeapon usermsg.
static int slayer_cur_weapon_id = 0;

// Shotgun aggregation state.
static double slayer_blood_agg_time   = 0;
static int    slayer_blood_agg_damage = 0;
static int    slayer_blood_agg_count  = 0;
// Pellets that hit a DIFFERENT victim during the aggregate window
// must NOT be merged into the running aggregate — otherwise a shotgun
// burst into a crowd shows one merged number and you can't tell which
// enemy took how much. Track the victim of the running aggregate and
// flush + restart when a new pellet hits a different one.
static int    slayer_blood_agg_victim = 0;

// =============================================================================
// CS 1.6 Weapon damage table
// =============================================================================

typedef struct
{
	int   base_damage;
	float range_modifier;
	int   pellets; // 1 for normal weapons, 9 for M3, 6 for XM1014
} slayer_weapon_info_t;

static const slayer_weapon_info_t slayer_weapon_table[31] =
{
	// index 0 = unused
	{ 0, 0.0f, 0 },
	// WEAPON_P228 = 1
	{ 32, 0.80f, 1 },
	// 2 = unused (WEAPON_SHIELD)
	{ 0, 0.0f, 0 },
	// WEAPON_SCOUT = 3
	{ 75, 0.98f, 1 },
	// WEAPON_HEGRENADE = 4
	{ 0, 0.0f, 0 },
	// WEAPON_XM1014 = 5
	{ 20, 0.70f, 6 },
	// WEAPON_C4 = 6
	{ 0, 0.0f, 0 },
	// WEAPON_MAC10 = 7
	{ 29, 0.82f, 1 },
	// WEAPON_AUG = 8
	{ 32, 0.96f, 1 },
	// WEAPON_SMOKEGRENADE = 9
	{ 0, 0.0f, 0 },
	// WEAPON_ELITE = 10
	{ 36, 0.75f, 1 },
	// WEAPON_FIVESEVEN = 11
	{ 20, 0.885f, 1 },
	// WEAPON_UMP45 = 12
	{ 30, 0.82f, 1 },
	// WEAPON_SG550 = 13
	{ 70, 0.98f, 1 },
	// WEAPON_GALIL = 14
	{ 30, 0.98f, 1 },
	// WEAPON_FAMAS = 15
	{ 30, 0.96f, 1 },
	// WEAPON_USP = 16
	{ 34, 0.79f, 1 },
	// WEAPON_GLOCK18 = 17
	{ 25, 0.75f, 1 },
	// WEAPON_AWP = 18
	{ 115, 0.99f, 1 },
	// WEAPON_MP5N = 19
	{ 26, 0.84f, 1 },
	// WEAPON_M249 = 20
	{ 32, 0.97f, 1 },
	// WEAPON_M3 = 21
	{ 20, 0.70f, 9 },
	// WEAPON_M4A1 = 22
	{ 33, 0.97f, 1 },
	// WEAPON_TMP = 23
	{ 20, 0.84f, 1 },
	// WEAPON_G3SG1 = 24
	{ 80, 0.98f, 1 },
	// WEAPON_FLASHBANG = 25
	{ 0, 0.0f, 0 },
	// WEAPON_DEAGLE = 26
	{ 54, 0.81f, 1 },
	// WEAPON_SG552 = 27
	{ 33, 0.955f, 1 },
	// WEAPON_AK47 = 28
	{ 36, 0.98f, 1 },
	// WEAPON_KNIFE = 29
	{ 0, 0.0f, 0 },
	// WEAPON_P90 = 30
	{ 21, 0.885f, 1 },
};

// =============================================================================
// Helpers
// =============================================================================

// Parse "R G B" or "R G B A" cvar string. Tolerant of malformed input
// (falls back to 255,80,80,255). Used identically to the scoreboard's
// parser but kept local to avoid pulling in the whole scoreboard module.
static void Slayer_HUD_ParseColor( const char *str, rgba_t out )
{
	int r = 255, g = 80, b = 80, a = 255;
	int n;

	if( !str || str[0] == '\0' )
	{
		MakeRGBA( out, 255, 80, 80, 255 );
		return;
	}

	n = sscanf( str, "%d %d %d %d", &r, &g, &b, &a );
	if( n < 3 )
	{
		MakeRGBA( out, 255, 80, 80, 255 );
		return;
	}

	if( r < 0 ) r = 0; if( r > 255 ) r = 255;
	if( g < 0 ) g = 0; if( g > 255 ) g = 255;
	if( b < 0 ) b = 0; if( b > 255 ) b = 255;
	if( a < 0 ) a = 0; if( a > 255 ) a = 255;

	out[0] = (byte)r;
	out[1] = (byte)g;
	out[2] = (byte)b;
	out[3] = (byte)a;
}

// Returns true when the user-message name is one we recognize as a
// "you hit a player for N damage" feedback event. The list intentionally
// covers several mod-flavored conventions; servers that emit none of
// these names simply produce no indicators and the feature stays dormant.
static qboolean Slayer_HUD_IsDamageMessage( const char *name )
{
	static const char *const known[] =
	{
		"DmgIndicator",   // common AmxModX hit-feedback plugin
		"DmgInd",
		"DamageInd",
		"DmgFeedback",
		"HitFeedback",
		"AttackerDmg",
		"ReDmgInd",       // ReGameDLL custom variant
		"HitInd",
		NULL
	};
	int k;

	if( !name )
		return false;

	for( k = 0; known[k] != NULL; k++ )
	{
		if( !Q_strcmp( name, known[k] ))
			return true;
	}
	return false;
}

// Find a free or stalest slot to write a new damage entry into. Prefers
// truly empty slots; otherwise picks the entry whose expire_time is the
// closest (i.e. about to vanish anyway), which avoids overwriting a
// freshly-registered hit while the player is still reading it.
static int Slayer_HUD_PickWriteSlot( void )
{
	int    i;
	int    stalest = 0;
	double stalest_expire = (double)1e30;

	for( i = 0; i < (int)SLAYER_DMG_HALO_COUNT; i++ )
	{
		if( slayer_dmg_entries[i].damage == 0 )
			return i;
		if( slayer_dmg_entries[i].expire_time < stalest_expire )
		{
			stalest_expire = slayer_dmg_entries[i].expire_time;
			stalest = i;
		}
	}
	return stalest;
}

// =============================================================================
// Public API
// =============================================================================

void Slayer_HUD_Init( void )
{
	Cvar_RegisterVariable( &slayer_damage_indicator );
	Cvar_RegisterVariable( &slayer_damage_indicator_color );
	Cvar_RegisterVariable( &slayer_damage_indicator_duration );
	Cvar_RegisterVariable( &slayer_damage_indicator_per_victim_color );

	Slayer_HUD_Reset();
}

void Slayer_HUD_Reset( void )
{
	memset( slayer_dmg_entries, 0, sizeof( slayer_dmg_entries ));
	slayer_dmg_next_slot = 0;

	// Clear TE_BLOOD aggregation and weapon tracking state.
	slayer_blood_agg_damage     = 0;
	slayer_blood_agg_count      = 0;
	slayer_blood_agg_time       = 0;
	slayer_blood_agg_victim     = 0;
	slayer_cur_weapon_id        = 0;
	slayer_last_plugin_hit_time = 0;
}

void Slayer_HUD_OnDamageMessage( const char *msgname, const byte *pbuf, int iSize )
{
	int    damage;
	int    slot;
	double duration;

	if( slayer_damage_indicator.value == 0.0f )
		return;
	if( !pbuf || iSize <= 0 )
		return;
	if( !Slayer_HUD_IsDamageMessage( msgname ))
		return;

	// All known damage-feedback messages put the damage amount in the
	// first byte. Some variants follow it with a hit-group / victim slot
	// byte which we currently ignore — the on-screen number is enough.
	damage = pbuf[0];
	if( damage <= 0 || damage > 255 )
		return;

	duration = slayer_damage_indicator_duration.value;
	if( duration < 0.25 ) duration = 0.25;
	if( duration > 8.0  ) duration = 8.0;

	slot = Slayer_HUD_PickWriteSlot();
	slayer_dmg_entries[slot].damage      = damage;
	slayer_dmg_entries[slot].expire_time = host.realtime + duration;
	slayer_dmg_entries[slot].halo_slot   = slayer_dmg_next_slot;
	slayer_dmg_entries[slot].victim_idx  = 0; // plugin msg has no victim, use base color

	slayer_dmg_next_slot = ( slayer_dmg_next_slot + 1 ) % (int)SLAYER_DMG_HALO_COUNT;

	// Mark plugin-sourced hit time for TE_BLOOD anti-duplicate.
	slayer_last_plugin_hit_time = host.realtime;

	Con_DPrintf( "Slayer HUD: dmg %d via msg '%s', halo=%d slot=%d\n",
		damage, msgname, slayer_dmg_entries[slot].halo_slot, slot );
}

// ---------------------------------------------------------------------------
// Slayer_HUD_OnHudTextDamage — TE_TEXTMESSAGE path (AmxModX damager plugin)
// ---------------------------------------------------------------------------
// The "damager" plugin uses set_hudmessage + show_hudmessage which the
// engine delivers via CL_ParseTextMessage (TE_TEXTMESSAGE). The text is
// the raw damage number as a string. We accept any purely-numeric message
// with value 1..255 regardless of position, since on real servers the only
// HUD text that is purely digits *is* the damage indicator.
//
// x/y are left available for future tighter filtering (typical damager
// coords are ~0.45/0.50) but currently unused to keep it robust across
// server configs that place the number at varying coords.

void Slayer_HUD_OnHudTextDamage( const char *pMessage, float x, float y )
{
	int    damage;
	int    slot;
	double duration;
	const char *p;

	(void)x;
	(void)y;

	if( slayer_damage_indicator.value == 0.0f )
		return;
	if( !pMessage || pMessage[0] == '\0' )
		return;

	// Verify the message is purely digits (no letters, no minus, no spaces).
	for( p = pMessage; *p; p++ )
	{
		if( *p < '0' || *p > '9' )
			return;
	}

	damage = Q_atoi( pMessage );
	if( damage < 1 || damage > 255 )
		return;

	duration = slayer_damage_indicator_duration.value;
	if( duration < 0.25 ) duration = 0.25;
	if( duration > 8.0  ) duration = 8.0;

	slot = Slayer_HUD_PickWriteSlot();
	slayer_dmg_entries[slot].damage      = damage;
	slayer_dmg_entries[slot].expire_time = host.realtime + duration;
	slayer_dmg_entries[slot].halo_slot   = slayer_dmg_next_slot;
	slayer_dmg_entries[slot].victim_idx  = 0; // HudText damager has no victim, use base color

	slayer_dmg_next_slot = ( slayer_dmg_next_slot + 1 ) % (int)SLAYER_DMG_HALO_COUNT;

	// Mark plugin-sourced hit time for TE_BLOOD anti-duplicate.
	slayer_last_plugin_hit_time = host.realtime;

	Con_DPrintf( "Slayer HUD: dmg %d via TE_TEXTMESSAGE, halo=%d slot=%d\n",
		damage, slayer_dmg_entries[slot].halo_slot, slot );
#if XASH_ANDROID
	__android_log_print( ANDROID_LOG_INFO, "Xash",
		"Slayer HUD: dmg %d via TE_TEXTMESSAGE (text='%s'), halo=%d",
		damage, pMessage, slayer_dmg_entries[slot].halo_slot );
#endif
}

// =============================================================================
// TE_BLOOD damage indicator: CurWeapon tracking
// =============================================================================

void Slayer_HUD_OnCurWeapon( int weapon_id )
{
	if( weapon_id >= 0 )
		slayer_cur_weapon_id = weapon_id;
}

// =============================================================================
// TE_BLOOD damage indicator: entity lookup + hitgroup detection
// =============================================================================

// Find the closest player entity to a given world position. Returns
// the 1-based entindex or 0 if no player is within `max_dist` units.
// Used as the victim resolver for both the hitbox-based hitgroup test
// and the per-victim armor-state lookup.
static int Slayer_HUD_FindClosestPlayer( const vec3_t world_pos, float max_dist )
{
	int          i;
	cl_entity_t *ent;
	int          best_idx  = 0;
	float        best_dist = max_dist * max_dist; // squared

	for( i = 1; i <= cl.maxclients; i++ )
	{
		float dx, dy, dz, dist_sq;

		ent = CL_GetEntityByIndex( i );
		if( !ent || !ent->player )
			continue;
		if( ent->curstate.messagenum != cl.parsecount )
			continue;

		dx = world_pos[0] - ent->origin[0];
		dy = world_pos[1] - ent->origin[1];
		dz = world_pos[2] - ent->origin[2];
		dist_sq = dx * dx + dy * dy + dz * dz;

		if( dist_sq < best_dist )
		{
			best_dist = dist_sq;
			best_idx  = i;
		}
	}

	return best_idx;
}

// =============================================================================
// TE_BLOOD damage indicator: shotgun aggregate flush
// =============================================================================

static void Slayer_HUD_FlushBloodAggregate( void )
{
	int    slot;
	double duration;

	if( slayer_blood_agg_count <= 0 )
		return;

	duration = slayer_damage_indicator_duration.value;
	if( duration < 0.25 ) duration = 0.25;
	if( duration > 8.0  ) duration = 8.0;

	slot = Slayer_HUD_PickWriteSlot();
	slayer_dmg_entries[slot].damage      = slayer_blood_agg_damage;
	slayer_dmg_entries[slot].expire_time = host.realtime + duration;
	slayer_dmg_entries[slot].halo_slot   = slayer_dmg_next_slot;
	slayer_dmg_entries[slot].victim_idx  = slayer_blood_agg_victim;

	slayer_dmg_next_slot = ( slayer_dmg_next_slot + 1 ) % (int)SLAYER_DMG_HALO_COUNT;

	Con_DPrintf( "Slayer HUD: dmg %d via TE_BLOOD aggregate (%d pellets, victim=%d), halo=%d\n",
		slayer_blood_agg_damage, slayer_blood_agg_count, slayer_blood_agg_victim,
		slayer_dmg_entries[slot].halo_slot );

	slayer_blood_agg_damage = 0;
	slayer_blood_agg_count  = 0;
	slayer_blood_agg_time   = 0;
	slayer_blood_agg_victim = 0;
}

// =============================================================================
// TE_BLOOD damage indicator: main processing
// =============================================================================

void Slayer_HUD_OnBloodImpact( vec3_t pos, int count )
{
	vec3_t eye, forward, dir;
	float  dot, dist;
	int    damage;
	int    slot;
	double duration;
	const slayer_weapon_info_t *wpn;

	if( slayer_damage_indicator.value == 0.0f )
		return;

	// Anti-duplicate: skip if a plugin already registered damage recently.
	if( host.realtime - slayer_last_plugin_hit_time < 0.1 )
		return;

	// Guard: some mods send TE_BLOOD with count=0 for non-damage effects.
	if( count <= 0 )
		return;

	// Compute player eye position.
	eye[0] = cl.simorg[0] + cl.viewheight[0];
	eye[1] = cl.simorg[1] + cl.viewheight[1];
	eye[2] = cl.simorg[2] + cl.viewheight[2];

	// Compute forward direction from view angles.
	AngleVectors( cl.viewangles, forward, NULL, NULL );

	// Direction from eye to blood position.
	VectorSubtract( pos, eye, dir );
	dist = VectorLength( dir );

	// Distance check: must be within 4096 units.
	if( dist < 1.0f || dist > 4096.0f )
		return;

	// Normalize direction.
	dir[0] /= dist;
	dir[1] /= dist;
	dir[2] /= dist;

	// Geometric validation: blood must be roughly in front of us.
	dot = DotProduct( dir, forward );
	if( dot < 0.5f )
		return;

	// Damage calculation.
	if( slayer_cur_weapon_id >= 1 && slayer_cur_weapon_id <= 30 )
	{
		wpn = &slayer_weapon_table[slayer_cur_weapon_id];
		if( wpn->base_damage > 0 )
		{
			float raw_damage;
			int   victim_idx;
			int   hitgroup;
			float hitgroup_mult;
			int   has_kevlar = 0, has_helmet = 0;

			raw_damage = (float)wpn->base_damage *
				(float)pow( wpn->range_modifier, dist / 500.0 );

			// PRIMARY: which player's hitbox actually contains the
			// blood point. This is the only correct disambiguator
			// when two enemies stand close together — origin-based
			// "closest player" is fooled by recoil pushback and
			// crouch/lean offsets, hitbox containment is not.
			hitgroup   = SLAYER_HG_GENERIC;
			victim_idx = Slayer_DmgReplay_FindHitboxOwner( pos, &hitgroup );

			// FALLBACK: animation lerp can drag bones a few units
			// off the actual hit point at high strafe speeds, so a
			// hitbox containment miss is still a real hit on the
			// closest player. Use a wider 192u radius (vs 128u
			// in the hitbox scan) — CS player bbox + worst-case
			// lean/crouch offset can put origin ~80u from impact.
			if( !victim_idx )
			{
				victim_idx = Slayer_HUD_FindClosestPlayer( pos, 192.0f );
				// hitgroup stays SLAYER_HG_GENERIC (1.0x) so we
				// under-count rather than fabricate a 4x headshot
				// when we can't be sure where the bullet landed.
			}

			hitgroup_mult = Slayer_DmgReplay_HitgroupMult( hitgroup );
			raw_damage   *= hitgroup_mult;

			// Sound-channel armor inference. CS hl.dll emits
			// helmet / kevlar SFX *after* the TE_BLOOD in the same
			// server frame, but they arrive in the same client
			// packet, so we read whatever the previous bullet on
			// this victim revealed (and the next call will be more
			// accurate). 500ms TTL inside Slayer_DmgReplay handles
			// stale flags.
			Slayer_DmgReplay_GetArmorState( victim_idx, &has_kevlar, &has_helmet );

			if( hitgroup == SLAYER_HG_HEAD )
			{
				// Helmet halves the headshot before any other
				// modifier; legs/arms with helmet see no effect.
				if( has_helmet )
					raw_damage *= Slayer_DmgReplay_ArmorRatio( slayer_cur_weapon_id );
			}
			else if( hitgroup == SLAYER_HG_CHEST   ||
				 hitgroup == SLAYER_HG_STOMACH ||
				 hitgroup == SLAYER_HG_LEFTARM ||
				 hitgroup == SLAYER_HG_RIGHTARM )
			{
				// CS doesn't put kevlar on legs, only the torso
				// and arms. Apply per-weapon armor ratio when
				// the body is armored.
				if( has_kevlar )
					raw_damage *= Slayer_DmgReplay_ArmorRatio( slayer_cur_weapon_id );
			}
			// SLAYER_HG_LEFTLEG / SLAYER_HG_RIGHTLEG / GENERIC:
			// no armor reduction in CS 1.6.

			damage = (int)( raw_damage + 0.5f );
			if( damage < 1 ) damage = 1;

			// Shotgun aggregation: if weapon has multiple pellets,
			// aggregate TE_BLOOD events within a 50ms window — but
			// ONLY for pellets that hit the SAME victim. A spread
			// into a crowd that catches two enemies must produce
			// two distinct numbers, otherwise the player has no
			// way to tell who took how much.
			if( wpn->pellets > 1 )
			{
				if( slayer_blood_agg_count > 0 &&
					( host.realtime - slayer_blood_agg_time ) < 0.05 &&
					slayer_blood_agg_victim == victim_idx )
				{
					// Same victim, same window → merge.
					slayer_blood_agg_damage += damage;
					slayer_blood_agg_count++;
					return;
				}
				else
				{
					// Different victim or expired window:
					// flush the old aggregate (if any) into its
					// own halo slot, then start a fresh one for
					// this pellet's victim.
					if( slayer_blood_agg_count > 0 )
						Slayer_HUD_FlushBloodAggregate();
					slayer_blood_agg_time   = host.realtime;
					slayer_blood_agg_damage = damage;
					slayer_blood_agg_count  = 1;
					slayer_blood_agg_victim = victim_idx;
					return;
				}
			}

			// Non-shotgun, single-pellet: register immediately and
			// remember the victim for per-color rendering.
			duration = slayer_damage_indicator_duration.value;
			if( duration < 0.25 ) duration = 0.25;
			if( duration > 8.0  ) duration = 8.0;

			slot = Slayer_HUD_PickWriteSlot();
			slayer_dmg_entries[slot].damage      = damage;
			slayer_dmg_entries[slot].expire_time = host.realtime + duration;
			slayer_dmg_entries[slot].halo_slot   = slayer_dmg_next_slot;
			slayer_dmg_entries[slot].victim_idx  = victim_idx;

			slayer_dmg_next_slot = ( slayer_dmg_next_slot + 1 ) % (int)SLAYER_DMG_HALO_COUNT;

			Con_DPrintf( "Slayer HUD: dmg %d via TE_BLOOD (weapon=%d, dist=%.0f, victim=%d, hg=%d), halo=%d\n",
				damage, slayer_cur_weapon_id, dist, victim_idx, hitgroup,
				slayer_dmg_entries[slot].halo_slot );
			return;
		}
		else
		{
			// Weapon has no base_damage (grenade, knife, etc): fallback.
			damage = count * 10;
			if( damage < 1 ) damage = 1;
		}
	}
	else
	{
		// Unknown weapon: fallback.
		damage = count * 10;
		if( damage < 1 ) damage = 1;
	}

	// Fallback path (unknown weapon / grenade / knife): no victim
	// resolution since we don't have a damage formula anyway.
	duration = slayer_damage_indicator_duration.value;
	if( duration < 0.25 ) duration = 0.25;
	if( duration > 8.0  ) duration = 8.0;

	slot = Slayer_HUD_PickWriteSlot();
	slayer_dmg_entries[slot].damage      = damage;
	slayer_dmg_entries[slot].expire_time = host.realtime + duration;
	slayer_dmg_entries[slot].halo_slot   = slayer_dmg_next_slot;
	slayer_dmg_entries[slot].victim_idx  = 0; // unknown -> base color

	slayer_dmg_next_slot = ( slayer_dmg_next_slot + 1 ) % (int)SLAYER_DMG_HALO_COUNT;

	Con_DPrintf( "Slayer HUD: dmg %d via TE_BLOOD fallback (weapon=%d), halo=%d\n",
		damage, slayer_cur_weapon_id, slayer_dmg_entries[slot].halo_slot );
}

// HSV to RGB conversion (h in degrees 0..360, s/v in 0..1, output 0..1).
// Used to give each victim a stable distinct color for their damage
// numbers so the player can read 2-3 simultaneous fights at a glance.
// Standard formula, no alpha.
static void Slayer_HUD_HSVtoRGB( float h, float s, float v, float *r, float *g, float *b )
{
	float c, x, m;
	float h6;
	float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;

	c  = v * s;
	h6 = h / 60.0f;
	x  = c * ( 1.0f - fabsf( fmodf( h6, 2.0f ) - 1.0f ));

	if( h6 < 1.0f )      { r1 = c; g1 = x; b1 = 0; }
	else if( h6 < 2.0f ) { r1 = x; g1 = c; b1 = 0; }
	else if( h6 < 3.0f ) { r1 = 0; g1 = c; b1 = x; }
	else if( h6 < 4.0f ) { r1 = 0; g1 = x; b1 = c; }
	else if( h6 < 5.0f ) { r1 = x; g1 = 0; b1 = c; }
	else                 { r1 = c; g1 = 0; b1 = x; }

	m  = v - c;
	*r = r1 + m;
	*g = g1 + m;
	*b = b1 + m;
}

// Compute a stable, well-distributed hue for victim entindex `idx`
// in the 1..32 range. 137° is the golden-angle approximation that
// keeps adjacent indices visually distinct (avoids the cluster of
// near-identical reds you'd get from idx*10 or similar small strides).
static float Slayer_HUD_VictimHue( int idx )
{
	int h;

	if( idx <= 0 )
		return 0.0f;

	h = ( idx * 137 ) % 360;
	if( h < 0 ) h += 360;
	return (float)h;
}

void Slayer_HUD_Draw( void )
{
	int          i;
	int          screen_w, screen_h;
	int          cx, cy;
	rgba_t       base_color;
	double       now;
	cl_font_t   *font;
	const double fade_secs = 0.5; // last 0.5s of each entry's life is the fade-out

	if( slayer_damage_indicator.value == 0.0f )
		return;
	if( cls.state != ca_active )
		return;

	// Flush expired shotgun aggregate before rendering.
	if( slayer_blood_agg_count > 0 &&
		( host.realtime - slayer_blood_agg_time ) > 0.05 )
	{
		Slayer_HUD_FlushBloodAggregate();
	}

	screen_w = refState.width;
	screen_h = refState.height;
	if( screen_w <= 0 || screen_h <= 0 )
		return;

	// Refresh cached parsed color only when the cvar string actually changes.
	if( Q_strcmp( slayer_dmg_color_str, slayer_damage_indicator_color.string ))
	{
		Q_strncpy( slayer_dmg_color_str, slayer_damage_indicator_color.string,
			sizeof( slayer_dmg_color_str ));
		Slayer_HUD_ParseColor( slayer_dmg_color_str, slayer_dmg_color_cached );
	}
	memcpy( base_color, slayer_dmg_color_cached, sizeof( rgba_t ));

	font = Con_GetCurFont();
	if( !font )
		return;

	now = host.realtime;
	cx  = screen_w / 2;
	cy  = screen_h / 2;

	for( i = 0; i < (int)SLAYER_DMG_HALO_COUNT; i++ )
	{
		slayer_dmg_entry_t *e = &slayer_dmg_entries[i];
		double remaining;
		float  alpha_scale;
		rgba_t color;
		char   buf[16];
		int    halo_x, halo_y;
		int    text_w = 0, text_h_unused = 0;

		if( e->damage == 0 )
			continue;

		remaining = e->expire_time - now;
		if( remaining <= 0.0 )
		{
			e->damage = 0; // expired — release slot
			continue;
		}

		// Fade alpha over the last fade_secs only. Earlier in the entry's
		// life it's drawn at full configured alpha so the player has time
		// to actually read the number before it starts dimming.
		if( remaining < fade_secs )
			alpha_scale = (float)( remaining / fade_secs );
		else
			alpha_scale = 1.0f;

		color[0] = base_color[0];
		color[1] = base_color[1];
		color[2] = base_color[2];
		color[3] = (byte)( base_color[3] * alpha_scale );

		// Per-victim hue rotation. When enabled and the entry has a
		// known victim, replace the RGB channels with that victim's
		// stable color while keeping the configured alpha (and the
		// fade scaling we just applied).
		if( slayer_damage_indicator_per_victim_color.value != 0.0f && e->victim_idx > 0 )
		{
			float fr, fg, fb;
			float hue = Slayer_HUD_VictimHue( e->victim_idx );

			Slayer_HUD_HSVtoRGB( hue, 0.85f, 1.0f, &fr, &fg, &fb );

			color[0] = (byte)bound( 0, (int)( fr * 255.0f ), 255 );
			color[1] = (byte)bound( 0, (int)( fg * 255.0f ), 255 );
			color[2] = (byte)bound( 0, (int)( fb * 255.0f ), 255 );
		}

		if( e->halo_slot < 0 || e->halo_slot >= (int)SLAYER_DMG_HALO_COUNT )
			e->halo_slot = 0;
		halo_x = slayer_dmg_halo[e->halo_slot].x;
		halo_y = slayer_dmg_halo[e->halo_slot].y;

		Q_snprintf( buf, sizeof( buf ), "-%d", e->damage );

		// Center the number on the halo position so longer numbers
		// (e.g. -100) don't push past their slot's right edge.
		Con_DrawStringLen( buf, &text_w, &text_h_unused );
		Con_DrawString( cx + halo_x - ( text_w / 2 ),
			cy + halo_y - ( font->charHeight / 2 ),
			buf, color );
	}
}
