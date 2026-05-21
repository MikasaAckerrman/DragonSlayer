/*
cl_steam_login.c - Slayer3D Steam OpenID WebView login
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Android implementation: Calls Java SteamLoginActivity via JNI.
The Activity opens a WebView pointed at Steam's OpenID login endpoint.
On success, the callback URL contains the SteamID64 in claimed_id.
A JNI native callback delivers the result back to the engine.
*/

#include <inttypes.h>
#include <stdlib.h>
#include "common.h"
#include "client.h"
#include "cl_steam_login.h"
#include "cl_steam_api.h"

static qboolean slogin_in_progress = false;


#if XASH_ANDROID
#include <jni.h>
#include <SDL.h>
#include <android/log.h>

static JavaVM   *slogin_jvm;
static jclass    slogin_activity_class;
static jmethodID slogin_start_login_mid;

// Console command: "steam_login"
static void Cmd_SteamLogin_f( void )
{
	JNIEnv *env;

	if( slogin_in_progress )
	{
		Con_Printf( "Steam login already in progress\n" );
		return;
	}

	if( !slogin_start_login_mid )
	{
		Con_Printf( S_ERROR "Steam login not available (JNI not initialized)\n" );
		return;
	}

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if( !env )
	{
		Con_Printf( S_ERROR "Steam login: cannot get JNIEnv\n" );
		return;
	}

	// Call Java: static void XashActivity.startSteamLogin()
	(*env)->CallStaticVoidMethod( env, slogin_activity_class, slogin_start_login_mid );

	if( (*env)->ExceptionCheck( env ) )
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
		Con_Printf( S_ERROR "Steam login: Java exception\n" );
		return;
	}

	slogin_in_progress = true;
	Con_Printf( "Slayer3D: Steam login WebView opened\n" );
}


void Slayer_SteamLogin_Init( void )
{
	JNIEnv *env;
	jobject activity;
	jclass cls;

	slogin_in_progress = false;
	slogin_jvm = NULL;
	slogin_activity_class = NULL;
	slogin_start_login_mid = NULL;

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if( !env )
		return;

	if( (*env)->GetJavaVM( env, &slogin_jvm ) != 0 )
		return;

	activity = (jobject)SDL_AndroidGetActivity();
	if( !activity )
		return;

	cls = (*env)->GetObjectClass( env, activity );
	if( !cls )
	{
		(*env)->DeleteLocalRef( env, activity );
		return;
	}

	slogin_activity_class = (*env)->NewGlobalRef( env, cls );
	(*env)->DeleteLocalRef( env, cls );
	(*env)->DeleteLocalRef( env, activity );

	if( !slogin_activity_class )
		return;

	// Find method: static void startSteamLogin()
	slogin_start_login_mid = (*env)->GetStaticMethodID( env, slogin_activity_class,
		"startSteamLogin", "()V" );

	if( !slogin_start_login_mid )
	{
		if( (*env)->ExceptionCheck( env ) )
		{
			(*env)->ExceptionClear( env );
		}
		Con_DPrintf( "SteamLogin: startSteamLogin method not found\n" );
		(*env)->DeleteGlobalRef( env, slogin_activity_class );
		slogin_activity_class = NULL;
		return;
	}

	Cmd_AddCommand( "steam_login", Cmd_SteamLogin_f,
		"Open Steam OpenID login WebView" );

	Con_Printf( "Slayer3D: Steam login JNI init OK\n" );
}


void Slayer_SteamLogin_Shutdown( void )
{
	slogin_in_progress = false;
}

qboolean Slayer_SteamLogin_InProgress( void )
{
	return slogin_in_progress;
}

// JNI callback: called from Java when login succeeds.
// Package: su.xash.engine.XashActivity (engine's own Activity class).
// If the app uses a different wrapper (e.g. com.xash3d.cs16client), it should
// call this native via JNI forwarding or use the same native method name.
JNIEXPORT void JNICALL Java_su_xash_engine_XashActivity_nativeSteamLoginResult(
	JNIEnv *env, jclass cls, jstring j_steamid64 )
{
	const char *str;
	(void)cls;

	if( !j_steamid64 )
	{
		Slayer_SteamLogin_OnFailed();
		return;
	}

	str = (*env)->GetStringUTFChars( env, j_steamid64, NULL );
	if( str )
	{
		Slayer_SteamLogin_OnComplete( str );
		(*env)->ReleaseStringUTFChars( env, j_steamid64, str );
	}
	else
	{
		Slayer_SteamLogin_OnFailed();
	}
}

// JNI callback: called from Java when login is cancelled
JNIEXPORT void JNICALL Java_su_xash_engine_XashActivity_nativeSteamLoginCancelled(
	JNIEnv *env, jclass cls )
{
	(void)env;
	(void)cls;
	Slayer_SteamLogin_OnFailed();
}

#else /* !XASH_ANDROID */

// Non-Android: steam_login command just prints instructions
static void Cmd_SteamLogin_f( void )
{
	Con_Printf( "Steam OpenID login is only available on Android.\n" );
	Con_Printf( "Set your SteamID64 manually: slayer_steamid64 <your_steamid64>\n" );
}

void Slayer_SteamLogin_Init( void )
{
	slogin_in_progress = false;

	Cmd_AddCommand( "steam_login", Cmd_SteamLogin_f,
		"Open Steam OpenID login (Android only)" );

	Con_Printf( "Slayer3D: Steam login init (non-Android, manual only)\n" );
}

void Slayer_SteamLogin_Shutdown( void )
{
	slogin_in_progress = false;
}

qboolean Slayer_SteamLogin_InProgress( void )
{
	return slogin_in_progress;
}

#endif /* XASH_ANDROID */


// ===========================================================================
// Common callbacks (both platforms)
// ===========================================================================

void Slayer_SteamLogin_OnComplete( const char *steamid64_str )
{
	uint64_t steamid64;

	slogin_in_progress = false;

	if( !steamid64_str || steamid64_str[0] == '\0' )
	{
		Con_Printf( S_WARN "SteamLogin: empty SteamID64 received\n" );
		return;
	}

	steamid64 = strtoull( steamid64_str, NULL, 10 );
	if( steamid64 == 0 )
	{
		Con_Printf( S_WARN "SteamLogin: invalid SteamID64: %s\n", steamid64_str );
		return;
	}

	Slayer_SteamAPI_SetLocalSteamID( steamid64 );
	Con_Printf( "Slayer3D: Steam login success! SteamID64 = %" PRIu64 "\n", steamid64 );

#if XASH_ANDROID
	__android_log_print( ANDROID_LOG_INFO, "Xash",
		"Slayer: Steam login success, steamid64=%" PRIu64, steamid64 );
#endif
}

void Slayer_SteamLogin_OnFailed( void )
{
	slogin_in_progress = false;
	Con_Printf( "Slayer3D: Steam login cancelled or failed\n" );

#if XASH_ANDROID
	__android_log_print( ANDROID_LOG_WARN, "Xash", "Slayer: Steam login failed/cancelled" );
#endif
}
