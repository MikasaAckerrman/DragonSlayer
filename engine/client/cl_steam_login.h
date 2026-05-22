/*
cl_steam_login.h - Slayer3D Steam OpenID login via WebView (Android)
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Steam OpenID 2.0 login flow:
1. Engine opens a WebView with Steam's OpenID login URL.
2. User logs in via their Steam account.
3. Steam redirects to our callback URL containing claimed_id with SteamID64.
4. Engine extracts SteamID64 from the URL and stores it.

Android: Uses a dedicated Activity with WebView (SteamLoginActivity).
Non-Android: Not yet implemented (would need an embedded browser or
             external browser + localhost HTTP callback).
*/

#ifndef CL_STEAM_LOGIN_H
#define CL_STEAM_LOGIN_H

#include <inttypes.h>
#include "xash3d_types.h"

// Initialize Steam login module (register cvars/commands).
// Called once from Slayer_Scoreboard_Init().
void Slayer_SteamLogin_Init( void );

// Start the Steam OpenID login flow.
// On Android: launches SteamLoginActivity via JNI.
// On other platforms: prints instructions to console (not yet implemented).
void Slayer_SteamLogin_Start( void );

// Get the locally stored SteamID64 of the logged-in user.
// Returns 0 if not logged in or if login hasn't completed.
uint64_t Slayer_SteamLogin_GetLocalID( void );

// Called from JNI callback when login completes (Android only).
// Sets the local SteamID64 and saves it to disk.
void Slayer_SteamLogin_OnComplete( uint64_t steamid64 );

#endif // CL_STEAM_LOGIN_H
