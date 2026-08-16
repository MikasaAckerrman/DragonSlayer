/*
cl_observer_slayer_logic.h - pure observer focus selection for Slayer3D
Copyright (C) 2026 Slayer3D contributors

Kept free of engine types so the mode table can be exercised by a tiny C89
harness. GoldSrc keeps the observer mode in iuser1 and the followed entity in
iuser2; roaming/map-free may retain a stale iuser2 and must not use it.
*/
#ifndef CL_OBSERVER_SLAYER_LOGIC_H
#define CL_OBSERVER_SLAYER_LOGIC_H

#define SLAYER_OBS_NONE          0
#define SLAYER_OBS_CHASE_LOCKED  1
#define SLAYER_OBS_CHASE_FREE    2
#define SLAYER_OBS_ROAMING       3
#define SLAYER_OBS_IN_EYE        4
#define SLAYER_OBS_MAP_FREE      5
#define SLAYER_OBS_MAP_CHASE     6

static int Slayer_Observer_ModeFollowsTarget( int mode )
{
	return mode == SLAYER_OBS_CHASE_LOCKED ||
		mode == SLAYER_OBS_CHASE_FREE ||
		mode == SLAYER_OBS_IN_EYE ||
		mode == SLAYER_OBS_MAP_CHASE;
}

static int Slayer_Observer_SelectFocus( int local_entindex, int mode,
	int target_entindex, int maxclients )
{
	if( local_entindex < 1 || local_entindex > maxclients )
		return 0;

	if( Slayer_Observer_ModeFollowsTarget( mode ) &&
		target_entindex >= 1 && target_entindex <= maxclients )
		return target_entindex;

	return local_entindex;
}

#endif /* CL_OBSERVER_SLAYER_LOGIC_H */
