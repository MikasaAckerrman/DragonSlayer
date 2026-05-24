/*
cl_grenade_tumble_slayer.h - Slayer3D client-side grenade tumble
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
#ifndef CL_GRENADE_TUMBLE_SLAYER_H
#define CL_GRENADE_TUMBLE_SLAYER_H

struct cl_entity_s;

// Register cvars. Called once from cl_view_slayer.c.
void Slayer_GrenadeTumble_Init( void );

// Override ent->angles with a 3-axis tumble proportional to current linear
// velocity. Called from CL_AddVisibleEntity for each visible entity; the
// function is a no-op for non-grenade models or when the cvar is disabled.
void Slayer_GrenadeTumble_Apply( struct cl_entity_s *ent );

#endif // CL_GRENADE_TUMBLE_SLAYER_H
