/*
cl_view_slayer.h - Slayer3D extension: kill-sound feedback (generic / headshot / teamkill)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#ifndef CL_VIEW_SLAYER_H
#define CL_VIEW_SLAYER_H

#include "xash3d_types.h"

// Register cvars and reset internal state. Called once from CL_InitLocal().
void Slayer_Init( void );

// Wipe per-match state (team table, last-played timestamp, last health).
// Called from CL_ClearState() on disconnect / map change so that data
// does not leak between games.
void Slayer_ResetMatchState( void );

// User-message hooks. Both functions are read-only with respect to the
// network buffer: they parse a copy of the payload that has already
// been read by the engine, so they never disturb the regular client.dll
// dispatch path.
//
// pbuf / iSize - exactly the values that the engine is about to forward
// to clgame.msg[i].func( name, iSize, pbuf ).
//
// Slayer_OnDeathMsg returns true if it consumed the event (currently
// always returns false: we never swallow DeathMsg, we only react to it).
qboolean Slayer_OnDeathMsg( const byte *pbuf, int iSize );
void     Slayer_OnTeamInfo( const byte *pbuf, int iSize );

// Health-edge fallback. Call this immediately AFTER cl.local.health is
// updated from the network (clientdata in HL/Xash, STAT_HEALTH in Quake).
//
// The DeathMsg hook above does not fire in singleplayer / bot matches
// where the server's GameRules don't broadcast DeathMsg. To still get a
// kill-sound in those cases, this function watches the local player's
// health crossing the >0 -> <=0 boundary and plays slayer_killsound,
// but only if Slayer_OnDeathMsg has not already played a sound very
// recently (so we don't double-play in true multiplayer).
void Slayer_OnHealthUpdate( int new_health );

#endif // CL_VIEW_SLAYER_H
