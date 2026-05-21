/*
cl_steam_login.h - Slayer3D Steam OpenID WebView login
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Steam OpenID Login: opens a WebView for the user to authenticate with Steam,
extracts SteamID64 from the callback URL, stores it in cvar slayer_steamid64.
Currently Android-only (WebView via Java Activity).
*/

#ifndef CL_STEAM_LOGIN_H
#define CL_STEAM_LOGIN_H

#include "xash3d_types.h"

// Initialize Steam login module (register console command).
void Slayer_SteamLogin_Init( void );

// Shutdown Steam login module.
void Slayer_SteamLogin_Shutdown( void );

// Check if login is in progress.
qboolean Slayer_SteamLogin_InProgress( void );

// Called from JNI when WebView login completes.
// steamid64_str: the SteamID64 extracted from claimed_id URL.
void Slayer_SteamLogin_OnComplete( const char *steamid64_str );

// Called from JNI when WebView login is cancelled/failed.
void Slayer_SteamLogin_OnFailed( void );

#endif // CL_STEAM_LOGIN_H
