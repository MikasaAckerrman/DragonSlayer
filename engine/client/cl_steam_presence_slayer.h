/*
cl_steam_presence_slayer.h - "playing Counter-Strike" on the Steam profile
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

The engine's side of the status is deliberately thin: it decides *when* the
player is in a game and hands that over. Logging on, keeping the session
alive, reconnecting and clearing the status all live in
su.xash.engine.steam.SteamPresence, on its own thread, because a CM session
blocks on network I/O and the frame loop must never wait for it.

Note what this is NOT: Slayer_SteamLogin_* (cl_steam_login.h) does Steam
OpenID, whose entire answer is a SteamID64 -- an identity with no permission to
write anything back. The status needs a credential, which comes from
SteamAuthActivity. An account signed in the old way shows an avatar and no
status, which is why Slayer_SteamPresence_Available() exists: so the console
can say why instead of failing silently.
*/

#ifndef CL_STEAM_PRESENCE_SLAYER_H
#define CL_STEAM_PRESENCE_SLAYER_H

#include "xash3d_types.h"

// Register cvars and commands. Called once from Slayer_Scoreboard_Init().
void Slayer_SteamPresence_Init( void );

// Called every frame; sends an update only when the state actually changed.
void Slayer_SteamPresence_Frame( void );

// Drop the status and the session. Called on engine shutdown.
void Slayer_SteamPresence_Shutdown( void );

// Whether a usable Steam credential is stored (token, not just a SteamID).
qboolean Slayer_SteamPresence_Available( void );

#endif // CL_STEAM_PRESENCE_SLAYER_H
