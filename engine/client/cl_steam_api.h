/*
cl_steam_api.h - Slayer3D Steam Web API batch avatar downloader
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Uses Steam Web API GetPlayerSummaries/v2 to batch-download avatar URLs
for all players in one request (instead of one XML profile fetch per player).

Android: JNI -> Java HttpsURLConnection (native TLS).
Non-Android: HTTP socket (port 80) with redirect following.
             Steam API over plain HTTP may not work (redirects to HTTPS).
             Falls back to per-player XML method when API key is empty.
*/

#ifndef CL_STEAM_API_H
#define CL_STEAM_API_H

#include <inttypes.h>
#include "xash3d_types.h"

// Initialize Steam Web API module (register cvars).
// Called once from Slayer_Scoreboard_Init().
void Slayer_SteamAPI_Init( void );

// Reset all batch state (on disconnect / map change).
// Called from Slayer_Scoreboard_Reset().
void Slayer_SteamAPI_Reset( void );

// Request a batch avatar URL fetch for up to `count` players.
// `steamids` is an array of SteamID64 values (0 = skip).
// No-op if API key cvar is empty or a batch is already in progress.
// `count` must be <= MAX_CLIENTS (32).
void Slayer_SteamAPI_RequestBatch( const uint64_t *steamids, int count );

// Pump the batch download state machine. Call once per frame.
// Returns true if the batch completed this frame (avatar URLs resolved).
// On completion, triggers Slayer_AvatarDownload_Request() for each player
// whose avatar URL was successfully resolved.
qboolean Slayer_SteamAPI_Frame( void );

#endif // CL_STEAM_API_H
