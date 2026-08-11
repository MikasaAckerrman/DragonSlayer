/*
cl_item_phys_slayer.h - Slayer3D physical look for dropped items
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
#ifndef CL_ITEM_PHYS_SLAYER_H
#define CL_ITEM_PHYS_SLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

struct cl_entity_s;

// The second consumer of the shared spin core (cl_spin_phys_slayer.c): weapons,
// shields and other props lying on the ground or in mid-air after being thrown.
//
// In vanilla CS a dropped weapon is drawn at a fixed orientation the server
// chose, so a rifle in a corner or on a slope stands upright INSIDE the geometry
// and is easy to miss entirely. Here the same server position gets a pose that
// tumbles while the item flies and then leans onto whatever surface it came to
// rest on -- so it reads as an object lying there.
//
// The POSITION is never touched. It belongs to the server, and moving it would
// desync what you see from what you can pick up.

// Register cvars. Called once from V_InitSlayerCvars().
void Slayer_ItemPhys_Init( void );

// Give `ent` a physical pose if it is a loose item. No-op for anything else, for
// players, and when the cvar is off. Called for each visible entity, right after
// the grenade tumble (which handles its own models and returns first).
void Slayer_ItemPhys_Apply( struct cl_entity_s *ent );

// Drop all tracked state. Called on map change.
void Slayer_ItemPhys_Reset( void );

#ifdef __cplusplus
}
#endif

#endif // CL_ITEM_PHYS_SLAYER_H
