/*
cl_steam_ticket_slayer.c - a real Steam auth ticket for the GoldSrc connect packet
Copyright (C) 2026 Slayer3D

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

See cl_steam_ticket_slayer.h for why this exists.
*/

#include "common.h"
#include "client.h"
#include "cl_steam_ticket_slayer.h"
#include "cl_slayer_log.h"

#if XASH_ANDROID
#include <SDL_system.h>
#include <jni.h>
#include <android/log.h>

static jclass    sticket_class;
static jmethodID sticket_fetch;      // byte[] steamFetchAuthTicket( int )
static jmethodID sticket_steamid;    // long steamAuthTicketSteamId()
static qboolean  sticket_init_done;

void Slayer_SteamTicket_Init( void )
{
	JNIEnv *env;
	jclass  local;

	if( sticket_init_done )
		return;

	sticket_init_done = true;

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();

	if( !env )
	{
		Con_DPrintf( "SteamTicket: no JNIEnv, real tickets unavailable\n" );
		return;
	}

	local = (*env)->FindClass( env, "su/xash/engine/XashActivity" );

	if( !local || (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionClear( env );
		Con_DPrintf( "SteamTicket: XashActivity not found\n" );
		return;
	}

	// A GLOBAL ref, because the local one dies with this frame and the methods
	// are called much later, from the connect path.
	sticket_class = (*env)->NewGlobalRef( env, local );
	(*env)->DeleteLocalRef( env, local );

	if( !sticket_class )
		return;

	sticket_fetch = (*env)->GetStaticMethodID( env, sticket_class,
		"steamFetchAuthTicket", "(I)[B" );
	sticket_steamid = (*env)->GetStaticMethodID( env, sticket_class,
		"steamAuthTicketSteamId", "()J" );

	if( !sticket_fetch || !sticket_steamid )
	{
		// R8 stripping a method has silently broken two features in this project
		// already, so say which one is missing rather than going quiet.
		(*env)->ExceptionClear( env );
		Con_Printf( S_WARN "SteamTicket: launcher lacks %s — real tickets disabled\n",
			!sticket_fetch ? "steamFetchAuthTicket" : "steamAuthTicketSteamId" );
		sticket_fetch = NULL;
		sticket_steamid = NULL;
		return;
	}

	Con_DPrintf( "SteamTicket: JNI bound\n" );
}

int Slayer_SteamTicket_Fetch( byte *buf, int buf_size, int timeout_ms,
	uint64_t *out_steamid )
{
	JNIEnv    *env;
	jbyteArray arr;
	jsize      len;
	jlong      sid;

	if( out_steamid )
		*out_steamid = 0;

	if( !buf || buf_size <= 0 )
		return 0;

	// Bind on first use rather than requiring an init order: this is called from
	// the connect path, which can run before anything thought to initialise us.
	if( !sticket_init_done )
		Slayer_SteamTicket_Init();

	if( !sticket_class || !sticket_fetch )
		return 0;

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();

	if( !env )
		return 0;

	arr = (jbyteArray)(*env)->CallStaticObjectMethod( env, sticket_class,
		sticket_fetch, (jint)timeout_ms );

	if( (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
		Slayer_Log_Printf( "ticket: JNI threw, falling back to the emulated ticket" );
		return 0;
	}

	if( !arr )
	{
		// The ORDINARY case: no Steam credentials stored, or the account does not
		// own CS 1.6, or Steam refused. Logged at DPrintf level so it does not
		// nag on every connect for the majority of players who never signed in.
		Con_DPrintf( "SteamTicket: no real ticket available, using the emulated one\n" );
		return 0;
	}

	len = (*env)->GetArrayLength( env, arr );

	if( len <= 0 || len > buf_size )
	{
		// A ticket that does not fit is refused rather than truncated: half a
		// signed blob is not a weaker ticket, it is a corrupt one, and the server
		// would drop the connect with no explanation.
		Slayer_Log_Printf( "ticket: %d bytes does not fit %d, using the emulated ticket",
			(int)len, buf_size );
		(*env)->DeleteLocalRef( env, arr );
		return 0;
	}

	(*env)->GetByteArrayRegion( env, arr, 0, len, (jbyte *)buf );
	(*env)->DeleteLocalRef( env, arr );

	if( (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionClear( env );
		return 0;
	}

	// The id comes from the same call, not from settings: see the header.
	sid = (*env)->CallStaticLongMethod( env, sticket_class, sticket_steamid );

	if( (*env)->ExceptionCheck( env ))
	{
		(*env)->ExceptionClear( env );
		sid = 0;
	}

	if( out_steamid )
		*out_steamid = (uint64_t)sid;

	Slayer_Log_Printf( "ticket: real Steam ticket, %d bytes, SteamID %" PRIu64,
		(int)len, (uint64_t)sid );

	return (int)len;
}

#else // !XASH_ANDROID

// Desktop builds keep the emulated ticket. The launcher-held Steam session is an
// Android arrangement; a desktop player who owns the game has the Steam client,
// which is what cl_ticket_generator "steam" (the broker) is already for.
void Slayer_SteamTicket_Init( void )
{
}

int Slayer_SteamTicket_Fetch( byte *buf, int buf_size, int timeout_ms,
	uint64_t *out_steamid )
{
	(void)buf; (void)buf_size; (void)timeout_ms;

	if( out_steamid )
		*out_steamid = 0;

	return 0;
}

#endif // XASH_ANDROID
