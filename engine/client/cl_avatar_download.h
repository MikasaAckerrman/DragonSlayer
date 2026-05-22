/*
cl_avatar_download.h - Slayer3D async Steam avatar downloader
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

#ifndef CL_AVATAR_DOWNLOAD_H
#define CL_AVATAR_DOWNLOAD_H

#include <inttypes.h>
#include "xash3d_types.h"

// Initialize avatar download system (register cvars).
void Slayer_AvatarDownload_Init( void );

// Shutdown avatar download system (close any pending connections).
void Slayer_AvatarDownload_Shutdown( void );

// Request avatar download for a player slot.
// Does nothing if already downloaded, in progress, or failed.
// steamid64: the player's SteamID64
// slot: player slot index (0-based)
void Slayer_AvatarDownload_Request( uint64_t steamid64, int slot );

// Pump the download state machine. Call once per frame.
// Returns true if any download completed this frame (texture ready to load).
qboolean Slayer_AvatarDownload_Frame( void );

// Reset all download state (on disconnect/map change).
void Slayer_AvatarDownload_Reset( void );

// Diagnostic: number of worker threads currently in flight (Android) or
// active sockets (non-Android). Useful for `slayer_avatar_diag` to surface
// when the AVD_MAX_CONCURRENT throttle is the reason scoreboard slots are
// stuck on PENDING.
int Slayer_AvatarDownload_GetActiveCount( void );

#endif // CL_AVATAR_DOWNLOAD_H
