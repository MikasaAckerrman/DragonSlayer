/*
cl_steam_presence_slayer.c - "playing Counter-Strike" on the Steam profile
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

What this file does and does not do:

DOES decide when the player counts as "in a game" -- cls.state == ca_active and
a real server address -- and report that, plus the server, once per change.

DOES NOT speak to Steam. That is SteamPresence on the Java side, on its own
thread. The engine never blocks on the network: a status that stutters the game
would be worse than no status at all.

The engine calls into Java only on an edge (started playing, stopped playing,
changed server). Polling Java every frame across JNI to say "still playing"
would cost a JNI transition per frame for information that changes a handful of
times per session.
*/

#include <inttypes.h>
#include "common.h"
#include "client.h"
#include "netchan.h"
#include "net_ws.h"
#include "cl_steam_presence_slayer.h"
#include "cl_slayer_log.h"

// Counter-Strike's Steam app id. The status names an app, and the id is what
// makes Steam show the real name and header art instead of a bare string.
#define SPRESENCE_APPID_CS16 10

static CVAR_DEFINE_AUTO( slayer_steam_presence, "1", FCVAR_ARCHIVE,
	"show \"playing\" status on your Steam profile while in a game" );
static CVAR_DEFINE_AUTO( slayer_steam_presence_server, "1", FCVAR_ARCHIVE,
	"include the server address in the Steam status" );
static CVAR_DEFINE_AUTO( slayer_steam_presence_name, "", FCVAR_ARCHIVE,
	"custom title shown in the Steam status (empty = mod name)" );

// What Java was last told, so nothing is sent twice.
static qboolean spresence_playing;
static char     spresence_sent_ip[64];
static int      spresence_sent_port;
static char     spresence_sent_name[128];
static qboolean spresence_initialized;

// Set once when a sign-in is missing, so the console says it once and not
// every time the player joins a server.
static qboolean spresence_warned_no_token;


static void Slayer_SPresence_BuildTitle( char *out, size_t size )
{
	if( !COM_StringEmptyOrNULL( slayer_steam_presence_name.string ))
	{
		Q_strncpy( out, slayer_steam_presence_name.string, size );
		return;
	}

	// Default: the mod's own name, so playing a mod does not claim to be
	// vanilla Counter-Strike.
	if( !COM_StringEmptyOrNULL( GI->title ))
		Q_strncpy( out, GI->title, size );
	else
		Q_strncpy( out, "Counter-Strike 1.6", size );
}

/*
=================
Slayer_SPresence_CurrentServer

The address to show, or an empty string when there is nothing sensible to show
(single player, loopback, a listen server nobody can join).
=================
*/
static void Slayer_SPresence_CurrentServer( char *ip, size_t ip_size, int *port )
{
	netadrtype_t type;

	ip[0] = '\0';
	*port = 0;

	if( !slayer_steam_presence_server.value )
		return;

	type = NET_NetadrType( &cls.serveradr );

	// Loopback and LAN addresses say nothing useful to someone reading the
	// profile, and a local address is not something to publish either.
	if( type != NA_IP )
		return;

	if( cls.serveradr.ip[0] == 127 || cls.serveradr.ip[0] == 10
		|| ( cls.serveradr.ip[0] == 192 && cls.serveradr.ip[1] == 168 )
		|| ( cls.serveradr.ip[0] == 172 && cls.serveradr.ip[1] >= 16
			&& cls.serveradr.ip[1] <= 31 ))
		return;

	Q_snprintf( ip, ip_size, "%i.%i.%i.%i",
		cls.serveradr.ip[0], cls.serveradr.ip[1],
		cls.serveradr.ip[2], cls.serveradr.ip[3] );

	// The port is stored network byte order (big-endian). Swapped by hand rather
	// than with ntohs(): that needs <arpa/inet.h>, which the Android NDK does not
	// pull in through the engine headers this file includes, and the build broke
	// on exactly that (implicit ntohs -> conflicting-types error). A two-byte swap
	// has no header to get wrong. `unsigned int`, not `word`, so the host test
	// harness (which does not define the engine's `word` typedef) compiles it too.
	{
		unsigned int p = (unsigned int)cls.serveradr.port;
		*port = (int)(( ( p & 0xFF ) << 8 ) | ( ( p >> 8 ) & 0xFF ));
	}
}


#if XASH_ANDROID
// ===========================================================================
// ANDROID: hand the intent to SteamPresence through XashActivity
// ===========================================================================
#include <jni.h>
#include <SDL.h>
#include <android/log.h>

static jclass    spresence_class;
static jmethodID spresence_start;
static jmethodID spresence_stop;
static jmethodID spresence_shutdown;
static jmethodID spresence_available;

static void Slayer_SPresence_BindJNI( void )
{
	JNIEnv *env;
	jobject activity;
	jclass cls;

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();

	if( !env )
	{
		Con_Printf( S_WARN "SteamPresence: JNI unavailable\n" );
		return;
	}

	activity = (jobject)SDL_AndroidGetActivity();

	if( !activity )
	{
		Con_Printf( S_WARN "SteamPresence: no activity\n" );
		return;
	}

	cls = (*env)->GetObjectClass( env, activity );
	(*env)->DeleteLocalRef( env, activity );

	if( !cls )
		return;

	spresence_class = (*env)->NewGlobalRef( env, cls );
	(*env)->DeleteLocalRef( env, cls );

	if( !spresence_class )
		return;

	spresence_start = (*env)->GetStaticMethodID( env, spresence_class,
		"steamPresenceStart", "(JLjava/lang/String;Ljava/lang/String;I)I" );
	spresence_stop = (*env)->GetStaticMethodID( env, spresence_class,
		"steamPresenceStop", "()V" );
	spresence_shutdown = (*env)->GetStaticMethodID( env, spresence_class,
		"steamPresenceShutdown", "()V" );
	spresence_available = (*env)->GetStaticMethodID( env, spresence_class,
		"steamPresenceAvailable", "()I" );

	if( (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
	}

	// R8 stripping a JNI-only method has already broken this project once
	// (getSteamId), so an absent method is reported loudly rather than left to
	// look like "the feature just does nothing".
	if( !spresence_start || !spresence_stop || !spresence_available )
	{
		Con_Printf( S_WARN "SteamPresence: launcher methods missing — rebuild the APK\n" );
		Slayer_Log_Printf( "presence: JNI methods missing (start=%p stop=%p avail=%p)",
			(void *)spresence_start, (void *)spresence_stop, (void *)spresence_available );
		(*env)->DeleteGlobalRef( env, spresence_class );
		spresence_class = NULL;
		return;
	}

	Con_Printf( "Slayer3D: Steam presence init OK\n" );
}

qboolean Slayer_SteamPresence_Available( void )
{
	JNIEnv *env;
	jint result;

	if( !spresence_class || !spresence_available )
		return false;

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();

	if( !env )
		return false;

	result = (*env)->CallStaticIntMethod( env, spresence_class, spresence_available );

	if( (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
		return false;
	}

	return result != 0;
}

static void Slayer_SPresence_SendStart( const char *title, const char *ip, int port )
{
	JNIEnv *env;
	jstring j_title, j_ip;
	jint accepted;

	if( !spresence_class || !spresence_start )
		return;

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();

	if( !env )
		return;

	j_title = (*env)->NewStringUTF( env, title ? title : "" );
	j_ip = (*env)->NewStringUTF( env, ip ? ip : "" );

	if( !j_title || !j_ip )
	{
		if( j_title ) (*env)->DeleteLocalRef( env, j_title );
		if( j_ip ) (*env)->DeleteLocalRef( env, j_ip );
		return;
	}

	accepted = (*env)->CallStaticIntMethod( env, spresence_class, spresence_start,
		(jlong)SPRESENCE_APPID_CS16, j_title, j_ip, (jint)port );

	if( (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
		accepted = 0;
	}

	(*env)->DeleteLocalRef( env, j_title );
	(*env)->DeleteLocalRef( env, j_ip );

	if( !accepted && !spresence_warned_no_token )
	{
		// The distinction that matters to the player: the old OpenID sign-in
		// yields a SteamID and no credential, so avatars work and the status
		// cannot. Without saying so, this looks like a bug.
		Con_Printf( S_WARN "SteamPresence: no Steam sign-in with a token.\n" );
		Con_Printf( "  Sign in from the launcher's Settings (Steam account) to show the status.\n" );
		Slayer_Log_Printf( "presence: start refused, no token stored" );
		spresence_warned_no_token = true;
	}
	else if( accepted )
	{
		Slayer_Log_Printf( "presence: playing \"%s\"%s%s:%d", title,
			ip[0] ? " @ " : "", ip[0] ? ip : "", port );
	}
}

static void Slayer_SPresence_SendStop( void )
{
	JNIEnv *env;

	if( !spresence_class || !spresence_stop )
		return;

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();

	if( !env )
		return;

	(*env)->CallStaticVoidMethod( env, spresence_class, spresence_stop );

	if( (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
	}

	Slayer_Log_Printf( "presence: stopped" );
}

static void Slayer_SPresence_SendShutdown( void )
{
	JNIEnv *env;

	if( !spresence_class || !spresence_shutdown )
		return;

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();

	if( !env )
		return;

	(*env)->CallStaticVoidMethod( env, spresence_class, spresence_shutdown );

	if( (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
	}
}

#else /* !XASH_ANDROID */
// ===========================================================================
// NON-ANDROID: no launcher to talk to
// ===========================================================================

static void Slayer_SPresence_BindJNI( void )
{
}

qboolean Slayer_SteamPresence_Available( void )
{
	return false;
}

static void Slayer_SPresence_SendStart( const char *title, const char *ip, int port )
{
	(void)title;
	(void)ip;
	(void)port;
}

static void Slayer_SPresence_SendStop( void )
{
}

static void Slayer_SPresence_SendShutdown( void )
{
}

#endif /* XASH_ANDROID */


// ===========================================================================
// Console commands
// ===========================================================================

static void Cmd_SteamPresenceStatus_f( void )
{
	if( !Slayer_SteamPresence_Available( ))
	{
		Con_Printf( "Steam presence: unavailable (no signed-in account with a token).\n" );
		Con_Printf( "  Sign in from the launcher's Settings -> Steam account.\n" );
		return;
	}

	if( !slayer_steam_presence.value )
	{
		Con_Printf( "Steam presence: available, but turned off (slayer_steam_presence 0).\n" );
		return;
	}

	if( spresence_playing )
	{
		Con_Printf( "Steam presence: showing \"%s\"", spresence_sent_name );

		if( spresence_sent_ip[0] )
			Con_Printf( " @ %s:%d", spresence_sent_ip, spresence_sent_port );

		Con_Printf( "\n" );
	}
	else
	{
		Con_Printf( "Steam presence: ready, nothing shown (not in a game).\n" );
	}
}


void Slayer_SteamPresence_Init( void )
{
	Cvar_RegisterVariable( &slayer_steam_presence );
	Cvar_RegisterVariable( &slayer_steam_presence_server );
	Cvar_RegisterVariable( &slayer_steam_presence_name );

	Cmd_AddCommand( "slayer_steam_presence_status", Cmd_SteamPresenceStatus_f,
		"show whether the Steam \"playing\" status is active, and why not" );

	Slayer_SPresence_BindJNI();

	spresence_playing = false;
	spresence_sent_ip[0] = '\0';
	spresence_sent_port = 0;
	spresence_sent_name[0] = '\0';
	spresence_initialized = true;
}

/*
=================
Slayer_SteamPresence_Frame

Reports a change, and only a change.
=================
*/
void Slayer_SteamPresence_Frame( void )
{
	qboolean want_playing;
	char ip[64];
	int port;
	char title[128];

	if( !spresence_initialized )
		return;

	want_playing = ( cls.state == ca_active ) && slayer_steam_presence.value != 0.0f;

	if( !want_playing )
	{
		if( spresence_playing )
		{
			Slayer_SPresence_SendStop();
			spresence_playing = false;
			spresence_sent_ip[0] = '\0';
			spresence_sent_port = 0;
			spresence_sent_name[0] = '\0';
		}

		return;
	}

	Slayer_SPresence_CurrentServer( ip, sizeof( ip ), &port );
	Slayer_SPresence_BuildTitle( title, sizeof( title ));

	// Nothing changed: say nothing. Re-sending the same message every frame
	// would be a JNI call and a protobuf per frame for no new information.
	if( spresence_playing
		&& !Q_strcmp( ip, spresence_sent_ip )
		&& port == spresence_sent_port
		&& !Q_strcmp( title, spresence_sent_name ))
		return;

	Slayer_SPresence_SendStart( title, ip, port );

	spresence_playing = true;
	Q_strncpy( spresence_sent_ip, ip, sizeof( spresence_sent_ip ));
	spresence_sent_port = port;
	Q_strncpy( spresence_sent_name, title, sizeof( spresence_sent_name ));
}

void Slayer_SteamPresence_Shutdown( void )
{
	if( !spresence_initialized )
		return;

	// Clearing first, then tearing down: an abandoned "playing" status on the
	// profile is the one failure mode a user would actually notice.
	if( spresence_playing )
	{
		Slayer_SPresence_SendStop();
		spresence_playing = false;
	}

	Slayer_SPresence_SendShutdown();
	spresence_initialized = false;
}
