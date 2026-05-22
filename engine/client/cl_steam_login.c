/*
cl_steam_login.c - Slayer3D Steam OpenID login via WebView (Android)
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Steam OpenID 2.0 login:
- Android: launches SteamLoginActivity (WebView) via JNI.
- The Activity intercepts the redirect URL containing claimed_id.
- claimed_id format: https://steamcommunity.com/openid/id/<steamid64>
- JNI native callback delivers the SteamID64 back to the engine.
- Engine saves it to slayer_steamid.cfg for persistence across sessions.

Non-Android: prints console message (not yet implemented).
*/

#include <inttypes.h>
#include <stdlib.h>
#include "common.h"
#include "client.h"
#include "cl_steam_login.h"

// Persistent local SteamID64 (0 = not logged in)
static uint64_t slogin_local_steamid64 = 0;
static qboolean slogin_initialized = false;

// Config file for persisting the SteamID
#define SLOGIN_CFG_FILE "slayer_steamid.cfg"


// ---------------------------------------------------------------------------
// Persistence: load/save SteamID64 from/to a simple config file
// ---------------------------------------------------------------------------

static void SLogin_LoadSavedID( void )
{
	byte *buf;
	fs_offset_t len;
	char tmp[32];

	buf = FS_LoadFile( SLOGIN_CFG_FILE, &len, false );
	if( !buf || len <= 0 )
		return;

	// Copy to stack buffer with null termination (FS_LoadFile may not terminate)
	if( len >= (fs_offset_t)sizeof( tmp ) )
		len = (fs_offset_t)sizeof( tmp ) - 1;
	memcpy( tmp, buf, (size_t)len );
	tmp[len] = '\0';
	Mem_Free( buf );

	// File contains just the SteamID64 as a decimal string
	slogin_local_steamid64 = strtoull( tmp, NULL, 10 );

	if( slogin_local_steamid64 != 0 )
	{
		Con_Printf( "Slayer3D: loaded saved SteamID64 = %" PRIu64 "\n",
			slogin_local_steamid64 );
	}
}

static void SLogin_SaveID( uint64_t steamid64 )
{
	char buf[32];

	if( steamid64 == 0 )
		return;

	Q_snprintf( buf, sizeof( buf ), "%" PRIu64, steamid64 );
	FS_WriteFile( SLOGIN_CFG_FILE, buf, Q_strlen( buf ) );
}

// ---------------------------------------------------------------------------
// Console command: slayer_steam_login
// ---------------------------------------------------------------------------

static void Cmd_SteamLogin_f( void )
{
	Slayer_SteamLogin_Start();
}

static void Cmd_SteamLogout_f( void )
{
	slogin_local_steamid64 = 0;
	FS_Delete( SLOGIN_CFG_FILE );
	Con_Printf( "Slayer3D: Steam login cleared\n" );
}

static void Cmd_SteamStatus_f( void )
{
	if( slogin_local_steamid64 != 0 )
	{
		Con_Printf( "Slayer3D: logged in as SteamID64 = %" PRIu64 "\n",
			slogin_local_steamid64 );
		Con_Printf( "  Profile: https://steamcommunity.com/profiles/%" PRIu64 "\n",
			slogin_local_steamid64 );
	}
	else
	{
		Con_Printf( "Slayer3D: not logged in. Use 'slayer_steam_login' to log in.\n" );
	}
}


#if XASH_ANDROID
// ===========================================================================
// ANDROID IMPLEMENTATION - JNI WebView
// ===========================================================================
#include <jni.h>
#include <SDL.h>
#include <android/log.h>

static jclass   slogin_activity_class = NULL;
static jmethodID slogin_start_method = NULL;

void Slayer_SteamLogin_Init( void )
{
	JNIEnv *env;
	jobject activity;
	jclass cls;

	slogin_initialized = false;

	Cmd_AddCommand( "slayer_steam_login", Cmd_SteamLogin_f,
		"Start Steam OpenID login via WebView" );
	Cmd_AddCommand( "slayer_steam_logout", Cmd_SteamLogout_f,
		"Clear saved Steam login" );
	Cmd_AddCommand( "slayer_steam_status", Cmd_SteamStatus_f,
		"Show current Steam login status" );

	SLogin_LoadSavedID();

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if( !env )
	{
		Con_Printf( S_WARN "SteamLogin: failed to get JNIEnv\n" );
		return;
	}

	activity = (jobject)SDL_AndroidGetActivity();
	if( !activity )
	{
		Con_Printf( S_WARN "SteamLogin: failed to get activity\n" );
		return;
	}

	cls = (*env)->GetObjectClass( env, activity );
	(*env)->DeleteLocalRef( env, activity );

	if( !cls )
	{
		Con_Printf( S_WARN "SteamLogin: GetObjectClass failed\n" );
		return;
	}

	slogin_activity_class = (*env)->NewGlobalRef( env, cls );
	(*env)->DeleteLocalRef( env, cls );

	if( !slogin_activity_class )
	{
		Con_Printf( S_WARN "SteamLogin: NewGlobalRef failed\n" );
		return;
	}

	// Method: static void startSteamLogin(String realm, String returnTo)
	slogin_start_method = (*env)->GetStaticMethodID( env, slogin_activity_class,
		"startSteamLogin",
		"(Ljava/lang/String;Ljava/lang/String;)V" );

	if( !slogin_start_method )
	{
		if( (*env)->ExceptionCheck( env ) )
		{
			(*env)->ExceptionDescribe( env );
			(*env)->ExceptionClear( env );
		}
		Con_Printf( S_WARN "SteamLogin: startSteamLogin method not found\n" );
		(*env)->DeleteGlobalRef( env, slogin_activity_class );
		slogin_activity_class = NULL;
		return;
	}

	slogin_initialized = true;
	Con_Printf( "Slayer3D: Steam login init OK (Android/WebView)\n" );
}


void Slayer_SteamLogin_Start( void )
{
	JNIEnv *env;
	jstring j_realm, j_return_to;

	if( !slogin_initialized || !slogin_start_method )
	{
		Con_Printf( S_ERROR "SteamLogin: not initialized\n" );
		return;
	}

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if( !env )
	{
		Con_Printf( S_ERROR "SteamLogin: failed to get JNIEnv\n" );
		return;
	}

	// OpenID realm and return_to URL
	// Using a custom scheme that the WebView will intercept
	j_realm = (*env)->NewStringUTF( env, "slayer3d://steam-login/" );
	j_return_to = (*env)->NewStringUTF( env, "slayer3d://steam-login/callback" );

	if( !j_realm || !j_return_to )
	{
		if( j_realm ) (*env)->DeleteLocalRef( env, j_realm );
		if( j_return_to ) (*env)->DeleteLocalRef( env, j_return_to );
		Con_Printf( S_ERROR "SteamLogin: JNI string creation failed\n" );
		return;
	}

	(*env)->CallStaticVoidMethod( env, slogin_activity_class,
		slogin_start_method, j_realm, j_return_to );

	if( (*env)->ExceptionCheck( env ) )
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
		Con_Printf( S_ERROR "SteamLogin: Java exception during start\n" );
	}
	else
	{
		Con_Printf( "Slayer3D: Steam login WebView launched\n" );
	}

	(*env)->DeleteLocalRef( env, j_realm );
	(*env)->DeleteLocalRef( env, j_return_to );
}

// ---------------------------------------------------------------------------
// JNI callback from SteamLoginActivity when login completes
// ---------------------------------------------------------------------------

JNIEXPORT void JNICALL
Java_su_xash_engine_XashActivity_nativeSteamLoginResult(
	JNIEnv *env, jclass cls, jlong steamid64 )
{
	(void)env;
	(void)cls;

	if( steamid64 <= 0 )
	{
		__android_log_print( ANDROID_LOG_WARN, "Xash",
			"SteamLogin: login cancelled or failed (id=%lld)",
			(long long)steamid64 );
		Con_Printf( S_WARN "SteamLogin: login cancelled or failed\n" );
		return;
	}

	slogin_local_steamid64 = (uint64_t)steamid64;
	SLogin_SaveID( slogin_local_steamid64 );

	__android_log_print( ANDROID_LOG_INFO, "Xash",
		"SteamLogin: login success, steamid64=%" PRIu64,
		slogin_local_steamid64 );
	Con_Printf( "Slayer3D: Steam login successful! ID = %" PRIu64 "\n",
		slogin_local_steamid64 );
}

#else /* !XASH_ANDROID */
// ===========================================================================
// NON-ANDROID STUB
// ===========================================================================

void Slayer_SteamLogin_Init( void )
{
	Cmd_AddCommand( "slayer_steam_login", Cmd_SteamLogin_f,
		"Start Steam OpenID login (Android only for now)" );
	Cmd_AddCommand( "slayer_steam_logout", Cmd_SteamLogout_f,
		"Clear saved Steam login" );
	Cmd_AddCommand( "slayer_steam_status", Cmd_SteamStatus_f,
		"Show current Steam login status" );

	SLogin_LoadSavedID();
	slogin_initialized = true;

	Con_Printf( "Slayer3D: Steam login init OK (non-Android stub)\n" );
}

void Slayer_SteamLogin_Start( void )
{
	Con_Printf( "Slayer3D: Steam OpenID login is only available on Android.\n" );
	Con_Printf( "  On desktop, set your SteamID64 manually:\n" );
	Con_Printf( "  Create file '%s' containing your SteamID64.\n", SLOGIN_CFG_FILE );
}

#endif /* XASH_ANDROID */


// ===========================================================================
// Platform-independent public API
// ===========================================================================

uint64_t Slayer_SteamLogin_GetLocalID( void )
{
	return slogin_local_steamid64;
}

void Slayer_SteamLogin_OnComplete( uint64_t steamid64 )
{
	if( steamid64 == 0 )
		return;

	slogin_local_steamid64 = steamid64;
	SLogin_SaveID( steamid64 );
	Con_Printf( "Slayer3D: Steam login set to %" PRIu64 "\n", steamid64 );
}
