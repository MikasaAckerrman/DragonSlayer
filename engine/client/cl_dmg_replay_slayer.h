/*
cl_dmg_replay_slayer.h - Slayer3D plugin-independent damage model
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

#ifndef CL_DMG_REPLAY_SLAYER_H
#define CL_DMG_REPLAY_SLAYER_H

#include "xash3d_types.h"

// CS 1.6 hitgroup IDs as written into mstudiobbox_t.group by hl.dll.
// Keep these in sync with public/com/cstrike/hldll values.
#define SLAYER_HG_GENERIC  0
#define SLAYER_HG_HEAD     1
#define SLAYER_HG_CHEST    2
#define SLAYER_HG_STOMACH  3
#define SLAYER_HG_LEFTARM  4
#define SLAYER_HG_RIGHTARM 5
#define SLAYER_HG_LEFTLEG  6
#define SLAYER_HG_RIGHTLEG 7

// Init / lifecycle. Called once from V_InitSlayerCvars() and on every
// CL_ClearState() via Slayer_ResetMatchState().
void Slayer_DmgReplay_Init( void );
void Slayer_DmgReplay_Reset( void );

// Hooked from S_StartSound (top of function, after sfx resolution).
// Inspects the resolved sample name; on match for a CS armor / helmet /
// flesh / headshot SFX, sets the per-victim flag indexed by the emitter
// entity (which is the *victim* in CS hl.dll's TraceAttack path).
//
// `name` is sfx->name (full path, e.g. "player/bhit_helmet-1.wav").
// `pos` may be NULL when the engine substitutes vieworg; we don't use
// it currently but keep the signature stable for future LoS gating.
void Slayer_DmgReplay_OnSound( const char *name, const vec3_t pos, int ent );

// Resolve the exact CS hitgroup of `blood_pos` against `victim_idx`'s
// studiomodel hitboxes. Returns SLAYER_HG_* (0 if no hitbox contains
// the point or the model isn't a studio model).
//
// This replaces the old Z-band heuristic which mis-classified head as
// chest whenever the player was crouching, leaning, or in a non-idle
// animation.
int Slayer_DmgReplay_ResolveHitgroup( int victim_idx, const vec3_t blood_pos );

// Find which player's studiomodel hitbox contains `world_pt`. Iterates
// every player within ~128 units of the point, calls Mod_HullForStudio
// for each, and tests `world_pt` against their hitboxes. The first
// containing hitbox wins; on multi-overlap (very close-quarters strafing)
// the player with the smaller origin distance wins.
//
// This is the correct disambiguator when two enemies stand close
// together: blood always lands inside the actually-hit player's
// hitbox, never inside the bystander's, even when origin-distance
// would say otherwise (recoil pushback skews body origin off-axis).
//
// Returns the 1-based entindex of the owner or 0 if nothing contains
// the point. When non-zero, *out_hitgroup is filled with the matched
// CS hitgroup id; on 0 it's set to SLAYER_HG_GENERIC.
//
// out_hitgroup may be NULL.
int Slayer_DmgReplay_FindHitboxOwner( const vec3_t world_pt, int *out_hitgroup );

// Read the latest armor flags observed for `victim_idx` via OnSound.
// Each flag carries an independent ~500ms TTL — hits more than 0.5s
// after the last armor sound from this victim are treated as "unknown
// armor" and the caller falls back to its conservative default.
//
// has_kevlar: kevlar / ric_metal / kevlar1 sound seen recently.
// has_helmet: bhit_helmet sound seen recently (implies kevlar too).
//
// Either out-pointer may be NULL.
void Slayer_DmgReplay_GetArmorState( int victim_idx, int *has_kevlar, int *has_helmet );

// Pure utility, no per-victim state. Returns the CS damage-multiplier
// for a hitgroup (head=4.0, chest=1.0, stomach=1.25, arm=1.0, leg=0.75,
// generic=1.0). Lives here so cl_hud_slayer.c stays free of CS-specific
// constants.
float Slayer_DmgReplay_HitgroupMult( int hitgroup );

// Per-weapon armor pass-through ratio. Damage *= this value when the
// hit body part is armored. Most weapons use 0.5 (50% block); high-
// penetration weapons (Deagle, snipers, M249) use higher values.
// `weapon_id` follows the same 1..30 indexing as the existing weapon
// table in cl_hud_slayer.c. Returns 0.5 for unknown weapons.
float Slayer_DmgReplay_ArmorRatio( int weapon_id );

#endif // CL_DMG_REPLAY_SLAYER_H
