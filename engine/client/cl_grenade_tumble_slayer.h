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

#ifdef __cplusplus
extern "C" {
#endif

struct cl_entity_s;

// Register cvars. Called once from cl_view_slayer.c.
void Slayer_GrenadeTumble_Init( void );

// Override ent->angles with a 3-axis tumble proportional to current linear
// velocity. Called from CL_AddVisibleEntity for each visible entity; the
// function is a no-op for non-grenade models or when the cvar is disabled.
void Slayer_GrenadeTumble_Apply( struct cl_entity_s *ent );

// Reset per-frame diagnostic counters (called from ImGui frame start)
void Slayer_GrenadeTumble_DiagReset( void );

// Real-time diagnostic overlay data (updated by Slayer_GrenadeTumble_Apply)
#define GT_DIAG_OVERLAY_MAX 8

typedef struct
{
	int    active_count;
	struct {
		char   model_name[64];
		float  speed;
		float  rate;
		float  accum_deg;
		float  origin[3];
		int    ent_index;
	} entries[GT_DIAG_OVERLAY_MAX];
	int    calls_this_frame;
	int    filtered_out;
} gt_diag_overlay_t;

extern gt_diag_overlay_t g_GT_DiagOverlay;

#ifdef __cplusplus
}
#endif

#endif // CL_GRENADE_TUMBLE_SLAYER_H
