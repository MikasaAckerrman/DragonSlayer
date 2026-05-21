/*
cl_steam_api.h - Slayer3D Steam Web API integration
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Steam Web API module for batch avatar downloads via GetPlayerSummaries/v2.
Requires API key stored in cvar "slayer_steam_apikey".

Android: Uses JNI HttpsURLConnection (has TLS).
Non-Android: Uses plain HTTP sockets (port 80) - Steam API may not respond
without TLS, so this path falls back to per-player XML download automatically.
*/

#ifndef CL_STEAM_API_H
#define CL_STEAM_API_H

#include <inttypes.h>
#include "xash3d_types.h"

// Initialize Steam API module (register cvars).
void Slayer_SteamAPI_Init( void );

// Shutdown Steam API module.
void Slayer_SteamAPI_Shutdown( void );

// Request batch avatar download for all provided SteamIDs.
// Will call GetPlayerSummaries/v2 API, parse avatar URLs from JSON,
// and queue individual image downloads via Slayer_AvatarDownload_Request().
//
// steamids  - array of SteamID64 values (0 = skip)
// count     - number of entries in array (max MAX_CLIENTS)
//
// Does nothing if:
//  - slayer_steam_apikey cvar is empty
//  - a batch request is already in progress
//  - called too frequently (internal cooldown)
void Slayer_SteamAPI_RequestBatchAvatars( const uint64_t *steamids, int count );

// Pump the batch API state machine. Call once per frame.
// Returns true if batch completed this frame (avatar URLs resolved).
qboolean Slayer_SteamAPI_Frame( void );

// Reset state (on disconnect/map change).
void Slayer_SteamAPI_Reset( void );

// Returns true if the Steam API key cvar is set (non-empty).
qboolean Slayer_SteamAPI_HasKey( void );

// Get the local player's SteamID64 (from OpenID login or status parse).
// Returns 0 if not yet known.
uint64_t Slayer_SteamAPI_GetLocalSteamID( void );

// Set the local player's SteamID64 (called after OpenID login or status parse).
void Slayer_SteamAPI_SetLocalSteamID( uint64_t steamid64 );

#endif // CL_STEAM_API_H
