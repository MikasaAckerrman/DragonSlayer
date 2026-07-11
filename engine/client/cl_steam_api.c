/*
cl_steam_api.c - Slayer3D Steam Web API batch avatar downloader
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Android: JNI call to SteamAPIHelper.fetchBatchAvatars() via pthread.
Non-Android: HTTP socket state machine (same pattern as cl_avatar_download.c).
*/

#include <inttypes.h>
#include <stdlib.h>
#include "common.h"
#include "client.h"
#include "cl_steam_api.h"
#include "cl_avatar_download.h"

#define SAPI_MAX_PLAYERS      32
#define SAPI_RESPONSE_MAX     65536
#define SAPI_TIMEOUT          15.0
#define SAPI_STEAM_API_HOST   "api.steampowered.com"
#define SAPI_STEAM_API_PORT   80
#define SAPI_MAX_REDIRECTS    3

// Cvar: Steam Web API key (user provides their own)
static CVAR_DEFINE_AUTO( slayer_steam_apikey, "", FCVAR_ARCHIVE | FCVAR_PROTECTED,
	"Slayer3D: Steam Web API key for batch avatar loading" );

// Batch request state
static qboolean sapi_initialized = false;
static qboolean sapi_batch_in_progress = false;
static uint64_t sapi_batch_ids[SAPI_MAX_PLAYERS];
static int      sapi_batch_slots[SAPI_MAX_PLAYERS]; // original player slot for each ID
static int      sapi_batch_count = 0;


#if XASH_ANDROID
// ===========================================================================
// ANDROID IMPLEMENTATION - JNI + pthread
// ===========================================================================
#include <pthread.h>
#include <jni.h>
#include <SDL.h>
#include <android/log.h>

// Result codes from worker thread
#define SAPI_RESULT_IDLE        0
#define SAPI_RESULT_IN_PROGRESS 1
#define SAPI_RESULT_SUCCESS     2
#define SAPI_RESULT_FAIL        3

static JavaVM           *sapi_jvm;
static jclass            sapi_helper_class;    // global ref to SteamAPIHelper
static jmethodID        sapi_fetch_method;     // fetchBatchAvatars(String key, String ids, String basePath)
static volatile int      sapi_result = SAPI_RESULT_IDLE;

// Worker thread downloads avatar images directly via Java HTTPS.
// The Java method fetches GetPlayerSummaries, parses avatar URLs,
// then downloads each PNG into the avatars/ directory.
// Returns 0 on success (at least one avatar saved), -1 on error.

typedef struct
{
	char apikey[64];
	char steamids[1024]; // comma-separated
	char basepath[512];
} sapi_work_t;


static void *SAPI_WorkerThread( void *arg )
{
	sapi_work_t *work = (sapi_work_t *)arg;
	JNIEnv *env = NULL;
	jstring j_key, j_ids, j_path;
	int result;

	__android_log_print( ANDROID_LOG_DEBUG, "Xash",
		"SteamAPI: worker thread starting" );

	if( (*sapi_jvm)->AttachCurrentThread( sapi_jvm, &env, NULL ) != JNI_OK )
	{
		__android_log_print( ANDROID_LOG_ERROR, "Xash",
			"SteamAPI: AttachCurrentThread failed" );
		__sync_synchronize();
		sapi_result = SAPI_RESULT_FAIL;
		__sync_synchronize();
		free( work );
		return NULL;
	}

	j_key  = (*env)->NewStringUTF( env, work->apikey );
	j_ids  = (*env)->NewStringUTF( env, work->steamids );
	j_path = (*env)->NewStringUTF( env, work->basepath );

	if( !j_key || !j_ids || !j_path )
	{
		if( j_key )  (*env)->DeleteLocalRef( env, j_key );
		if( j_ids )  (*env)->DeleteLocalRef( env, j_ids );
		if( j_path ) (*env)->DeleteLocalRef( env, j_path );
		(*sapi_jvm)->DetachCurrentThread( sapi_jvm );
		__sync_synchronize();
		sapi_result = SAPI_RESULT_FAIL;
		__sync_synchronize();
		free( work );
		return NULL;
	}

	result = (*env)->CallStaticIntMethod( env, sapi_helper_class,
		sapi_fetch_method, j_key, j_ids, j_path );

	if( (*env)->ExceptionCheck( env ) )
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
		result = -1;
	}

	(*env)->DeleteLocalRef( env, j_key );
	(*env)->DeleteLocalRef( env, j_ids );
	(*env)->DeleteLocalRef( env, j_path );
	(*sapi_jvm)->DetachCurrentThread( sapi_jvm );

	__android_log_print( ANDROID_LOG_INFO, "Xash",
		"SteamAPI: worker done, result=%d", result );

	__sync_synchronize();
	sapi_result = ( result >= 0 ) ? SAPI_RESULT_SUCCESS : SAPI_RESULT_FAIL;
	__sync_synchronize();

	free( work );
	return NULL;
}


// ---------------------------------------------------------------------------
// Android: Public API
// ---------------------------------------------------------------------------

void Slayer_SteamAPI_Init( void )
{
	JNIEnv *env;
	jclass cls;

	Cvar_RegisterVariable( &slayer_steam_apikey );
	sapi_initialized = false;
	sapi_batch_in_progress = false;
	sapi_result = SAPI_RESULT_IDLE;
	sapi_jvm = NULL;
	sapi_helper_class = NULL;
	sapi_fetch_method = NULL;

	env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if( !env )
	{
		Con_Printf( S_WARN "SteamAPI: failed to get JNIEnv\n" );
		return;
	}

	if( (*env)->GetJavaVM( env, &sapi_jvm ) != 0 )
	{
		Con_Printf( S_WARN "SteamAPI: failed to get JavaVM\n" );
		return;
	}

	cls = (*env)->FindClass( env, "su/xash/engine/SteamAPIHelper" );
	if( !cls )
	{
		if( (*env)->ExceptionCheck( env ) )
		{
			(*env)->ExceptionDescribe( env );
			(*env)->ExceptionClear( env );
		}
		Con_Printf( S_WARN "SteamAPI: SteamAPIHelper class not found\n" );
		return;
	}

	sapi_helper_class = (*env)->NewGlobalRef( env, cls );
	(*env)->DeleteLocalRef( env, cls );

	if( !sapi_helper_class )
	{
		Con_Printf( S_WARN "SteamAPI: NewGlobalRef failed\n" );
		return;
	}

	sapi_fetch_method = (*env)->GetStaticMethodID( env, sapi_helper_class,
		"fetchBatchAvatars",
		"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I" );

	if( !sapi_fetch_method )
	{
		if( (*env)->ExceptionCheck( env ) )
		{
			(*env)->ExceptionDescribe( env );
			(*env)->ExceptionClear( env );
		}
		Con_Printf( S_WARN "SteamAPI: fetchBatchAvatars method not found\n" );
		(*env)->DeleteGlobalRef( env, sapi_helper_class );
		sapi_helper_class = NULL;
		return;
	}

	sapi_initialized = true;
	Con_Printf( "Slayer3D: Steam Web API init OK (Android/JNI)\n" );
}


void Slayer_SteamAPI_Reset( void )
{
	sapi_batch_in_progress = false;
	sapi_batch_count = 0;
	sapi_result = SAPI_RESULT_IDLE;
}

void Slayer_SteamAPI_RequestBatch( const uint64_t *steamids, int count )
{
	sapi_work_t *work;
	pthread_t thread;
	const char *basedir, *gamedir;
	int i, written;
	char *p;

	if( !sapi_initialized || !sapi_fetch_method )
		return;

	if( sapi_batch_in_progress )
		return;

	if( slayer_steam_apikey.string[0] == '\0' )
		return;

	if( !steamids || count <= 0 )
		return;

	work = (sapi_work_t *)malloc( sizeof( sapi_work_t ) );
	if( !work )
		return;

	Q_strncpy( work->apikey, slayer_steam_apikey.string, sizeof( work->apikey ) );

	// Build comma-separated SteamID list
	p = work->steamids;
	written = 0;
	sapi_batch_count = 0;
	for( i = 0; i < count && i < SAPI_MAX_PLAYERS; i++ )
	{
		if( steamids[i] == 0 )
			continue;

		if( written > 0 )
		{
			*p++ = ',';
		}
		p += Q_snprintf( p, sizeof( work->steamids ) - (int)(p - work->steamids),
			"%" PRIu64, steamids[i] );
		written++;

		sapi_batch_ids[sapi_batch_count] = steamids[i];
		sapi_batch_slots[sapi_batch_count] = i;
		sapi_batch_count++;
	}
	*p = '\0';

	if( written == 0 )
	{
		free( work );
		return;
	}

	// Build save base path
	basedir = getenv( "XASH3D_BASEDIR" );
	if( !basedir || basedir[0] == '\0' )
		basedir = ".";
	gamedir = getenv( "XASH3D_GAME" );
	if( !gamedir || gamedir[0] == '\0' )
		gamedir = "valve";

	Q_snprintf( work->basepath, sizeof( work->basepath ),
		"%s/%s/avatars", basedir, gamedir );

	sapi_batch_in_progress = true;
	sapi_result = SAPI_RESULT_IN_PROGRESS;

	if( pthread_create( &thread, NULL, SAPI_WorkerThread, work ) != 0 )
	{
		__android_log_print( ANDROID_LOG_ERROR, "Xash",
			"SteamAPI: pthread_create failed" );
		free( work );
		sapi_batch_in_progress = false;
		sapi_result = SAPI_RESULT_IDLE;
		return;
	}

	pthread_detach( thread );
	Con_Printf( "Slayer3D: batch avatar request started (%d players)\n", written );
}


qboolean Slayer_SteamAPI_Frame( void )
{
	if( !sapi_batch_in_progress )
		return false;

	__sync_synchronize();

	if( sapi_result == SAPI_RESULT_SUCCESS )
	{
		int i;

		Con_Printf( "Slayer3D: batch avatar fetch completed\n" );

		// Mark avatar slots for reload — the Java side saved PNGs directly.
		// Trigger avatar download system to notice new files on disk.
		for( i = 0; i < sapi_batch_count; i++ )
		{
			if( sapi_batch_ids[i] != 0 )
			{
				// Use the original player slot, not the compressed index
				Slayer_AvatarDownload_Request( sapi_batch_ids[i], sapi_batch_slots[i] );
			}
		}

		sapi_batch_in_progress = false;
		sapi_batch_count = 0;
		sapi_result = SAPI_RESULT_IDLE;
		return true;
	}

	if( sapi_result == SAPI_RESULT_FAIL )
	{
		Con_Printf( S_WARN "SteamAPI: batch avatar fetch failed\n" );
		sapi_batch_in_progress = false;
		sapi_batch_count = 0;
		sapi_result = SAPI_RESULT_IDLE;
		return false;
	}

	return false; // still in progress
}

#else /* !XASH_ANDROID */
// ===========================================================================
// NON-ANDROID IMPLEMENTATION - HTTP socket state machine
// ===========================================================================
#include "net_ws_private.h"


typedef enum
{
	SAPI_STATE_IDLE = 0,
	SAPI_STATE_RESOLVE,
	SAPI_STATE_CONNECT,
	SAPI_STATE_SEND,
	SAPI_STATE_RECV_HEADER,
	SAPI_STATE_RECV_BODY,
	SAPI_STATE_DONE,
} sapi_state_t;

static sapi_state_t sapi_state = SAPI_STATE_IDLE;
static int          sapi_sock = -1;
static struct sockaddr_storage sapi_addr;
static qboolean     sapi_resolving = false;
static double       sapi_start_time = 0.0;
static int          sapi_redirect_count = 0;

// Request/response buffers
static char  sapi_request[2048];
static int   sapi_request_len = 0;
static int   sapi_bytes_sent = 0;
static byte *sapi_response = NULL;
static int   sapi_response_len = 0;
static int   sapi_response_cap = 0;
static qboolean sapi_got_header = false;
static int   sapi_header_end = 0;
static int   sapi_content_length = -1;

// Current target (may change on redirect)
static char  sapi_target_host[256];
static int   sapi_target_port = 80;
static char  sapi_target_path[1024];


// ---------------------------------------------------------------------------
// Helpers (same pattern as cl_avatar_download.c)
// ---------------------------------------------------------------------------

static void SAPI_CloseSocket( void )
{
	if( sapi_sock != -1 )
	{
		closesocket( sapi_sock );
		sapi_sock = -1;
	}
}

static void SAPI_FreeResponse( void )
{
	if( sapi_response )
	{
		Mem_Free( sapi_response );
		sapi_response = NULL;
	}
	sapi_response_len = 0;
	sapi_response_cap = 0;
	sapi_got_header = false;
	sapi_header_end = 0;
	sapi_content_length = -1;
}

static void SAPI_Cleanup( void )
{
	SAPI_CloseSocket();
	SAPI_FreeResponse();
	sapi_state = SAPI_STATE_IDLE;
	sapi_batch_in_progress = false;
	sapi_resolving = false;
}

static int SAPI_ParseStatusCode( const char *buf, int len )
{
	int code;
	if( len < 13 || Q_strncmp( buf, "HTTP/1.", 7 ) != 0 )
		return -1;
	if( buf[8] != ' ' )
		return -1;
	code = ( buf[9] - '0' ) * 100 + ( buf[10] - '0' ) * 10 + ( buf[11] - '0' );
	if( code < 100 || code > 599 )
		return -1;
	return code;
}

static const char *SAPI_FindHeader( const char *headers, const char *name )
{
	int name_len = (int)Q_strlen( name );
	const char *p = headers;

	while( *p )
	{
		if( Q_strnicmp( p, name, name_len ) == 0 && p[name_len] == ':' )
		{
			p += name_len + 1;
			while( *p == ' ' || *p == '\t' ) p++;
			return p;
		}
		while( *p && *p != '\n' ) p++;
		if( *p == '\n' ) p++;
	}
	return NULL;
}


static qboolean SAPI_ParseURL( const char *url, int url_len )
{
	const char *host_start, *host_end, *path_start;
	int host_len, path_len;

	if( url_len > 8 && Q_strncmp( url, "https://", 8 ) == 0 )
		host_start = url + 8;
	else if( url_len > 7 && Q_strncmp( url, "http://", 7 ) == 0 )
		host_start = url + 7;
	else
		return false;

	path_start = host_start;
	while( path_start < url + url_len && *path_start != '/' && *path_start != ':' )
		path_start++;

	host_end = path_start;
	host_len = (int)( host_end - host_start );
	if( host_len <= 0 || host_len >= (int)sizeof( sapi_target_host ) )
		return false;

	memcpy( sapi_target_host, host_start, host_len );
	sapi_target_host[host_len] = '\0';

	if( path_start < url + url_len && *path_start == ':' )
	{
		path_start++;
		while( path_start < url + url_len && *path_start != '/' )
			path_start++;
	}

	if( path_start >= url + url_len )
	{
		sapi_target_path[0] = '/';
		sapi_target_path[1] = '\0';
	}
	else
	{
		path_len = (int)( ( url + url_len ) - path_start );
		if( path_len <= 0 || path_len >= (int)sizeof( sapi_target_path ) )
			return false;
		memcpy( sapi_target_path, path_start, path_len );
		sapi_target_path[path_len] = '\0';
	}

	sapi_target_port = 80;
	return true;
}


// ---------------------------------------------------------------------------
// JSON response parsing (minimal, just extracts steamid + avatarmedium)
// ---------------------------------------------------------------------------

// Find the value string for a given key within a JSON substring.
// Returns pointer to first char of value (after opening quote), sets *out_len.
// Returns NULL if not found. Only handles string values.
static const char *SAPI_JsonGetString( const char *json, int json_len,
	const char *key, int *out_len )
{
	char search[64];
	const char *p, *end, *val_start, *val_end;

	Q_snprintf( search, sizeof( search ), "\"%s\"", key );
	end = json + json_len;

	p = Q_strstr( json, search );
	if( !p || p >= end )
		return NULL;

	// Skip key and colon: "key" : "value"
	p += Q_strlen( search );
	while( p < end && ( *p == ' ' || *p == ':' || *p == '\t' ) )
		p++;

	if( p >= end || *p != '"' )
		return NULL;

	val_start = ++p;
	val_end = val_start;
	while( val_end < end && *val_end != '"' )
	{
		if( *val_end == '\\' ) val_end++; // skip escaped char
		val_end++;
	}

	if( val_end >= end )
		return NULL;

	*out_len = (int)( val_end - val_start );
	return val_start;
}


// Parse the batch response and trigger individual avatar downloads.
// Response format: {"response":{"players":[{...},{...}]}}
// Each player object has "steamid" and "avatarmedium" (or "avatar").
static void SAPI_ParseResponse( void )
{
	const char *body, *body_end;
	const char *players_start, *obj_start, *obj_end;
	int body_len;

	if( !sapi_response || !sapi_got_header )
		return;

	body = (const char *)sapi_response + sapi_header_end;
	body_len = sapi_response_len - sapi_header_end;
	body_end = body + body_len;

	// Null-terminate body for string operations (safe: allocated cap+1)
	sapi_response[sapi_response_len] = '\0';

	// Find "players" array
	players_start = Q_strstr( body, "\"players\"" );
	if( !players_start )
	{
		Con_DPrintf( "SteamAPI: no 'players' key in response\n" );
		return;
	}

	// Walk through player objects
	obj_start = Q_strchr( players_start, '{' );
	while( obj_start && obj_start < body_end )
	{
		const char *sid_val, *avatar_val;
		int sid_len, avatar_len;
		uint64_t steamid64;
		int i;

		// Find end of this object (simple brace matching)
		obj_end = Q_strchr( obj_start + 1, '}' );
		if( !obj_end || obj_end >= body_end )
			break;

		// Extract steamid
		sid_val = SAPI_JsonGetString( obj_start,
			(int)(obj_end - obj_start), "steamid", &sid_len );
		if( !sid_val )
			goto next_obj;

		steamid64 = strtoull( sid_val, NULL, 10 );
		if( steamid64 == 0 )
			goto next_obj;

		// Extract avatar URL (prefer avatarmedium, fallback to avatar)
		avatar_val = SAPI_JsonGetString( obj_start,
			(int)(obj_end - obj_start), "avatarmedium", &avatar_len );
		if( !avatar_val || avatar_len <= 10 )
		{
			avatar_val = SAPI_JsonGetString( obj_start,
				(int)(obj_end - obj_start), "avatar", &avatar_len );
		}

		if( avatar_val && avatar_len > 10 )
		{
			// Slayer3D: skip the stock Steam "no avatar" silhouette so we
			// don't render the default head for accounts without a profile
			// picture (and for pirate connections that arrive via the
			// community XML under the same default hash).
			qboolean is_default = false;
			{
				const char *needle = "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb";
				const int   nlen   = 40;
				const char *p, *p_end;
				p_end = avatar_val + ( avatar_len - nlen );
				for( p = avatar_val; p <= p_end; p++ )
				{
					if( Q_strncmp( p, needle, nlen ) == 0 )
					{
						is_default = true;
						break;
					}
				}
			}
			if( is_default )
			{
				Con_DPrintf( "SteamAPI: skipping default Steam silhouette for %" PRIu64 "\n",
					steamid64 );
				goto next_obj;
			}

			// We got the avatar URL! Log it.
			// The non-Android path cannot download HTTPS images directly.
			// But if the URL happens to be HTTP or we add a per-image
			// download state machine later, we can use it.
			// For now, just trigger the existing XML-based downloader
			// which will notice the SteamID and fetch via XML profile.
			Con_DPrintf( "SteamAPI: resolved steamid=%" PRIu64 "\n", steamid64 );

			// Find player slot for this steamid and trigger download
			for( i = 0; i < sapi_batch_count; i++ )
			{
				if( sapi_batch_ids[i] == steamid64 )
				{
					Slayer_AvatarDownload_Request( steamid64, sapi_batch_slots[i] );
					break;
				}
			}
		}

	next_obj:
		// Advance to next object
		obj_start = Q_strchr( obj_end + 1, '{' );
	}
}


// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

static void SAPI_StateResolve( void )
{
	net_gai_state_t res;

	if( sapi_resolving )
		return;

	memset( &sapi_addr, 0, sizeof( sapi_addr ) );
	res = NET_StringToSockaddr( sapi_target_host, &sapi_addr, true, AF_INET );

	if( res == NET_EAI_AGAIN )
	{
		sapi_resolving = true;
		return;
	}

	if( res == NET_EAI_NONAME )
	{
		Con_DPrintf( "SteamAPI: DNS failed for %s\n", sapi_target_host );
		sapi_state = SAPI_STATE_DONE;
		return;
	}

	((struct sockaddr_in *)&sapi_addr)->sin_port = htons( (unsigned short)sapi_target_port );

	sapi_sock = socket( sapi_addr.ss_family, SOCK_STREAM, IPPROTO_TCP );
	if( sapi_sock < 0 )
	{
		Con_DPrintf( "SteamAPI: socket() failed: %s\n", NET_ErrorString() );
		sapi_state = SAPI_STATE_DONE;
		return;
	}

	if( !NET_MakeSocketNonBlocking( sapi_sock ) )
	{
		closesocket( sapi_sock );
		sapi_sock = -1;
		sapi_state = SAPI_STATE_DONE;
		return;
	}

	sapi_state = SAPI_STATE_CONNECT;
}

static void SAPI_StateConnect( void )
{
	int res = connect( sapi_sock, (struct sockaddr *)&sapi_addr,
		NET_SockAddrLen( &sapi_addr ) );

	if( res < 0 )
	{
		int err = WSAGetLastError();
		if( err == WSAEISCONN )
			; // fall through
		else if( err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY )
			return; // still connecting
		else
		{
			Con_DPrintf( "SteamAPI: connect failed: %s\n", NET_ErrorString() );
			sapi_state = SAPI_STATE_DONE;
			return;
		}
	}

	// Build HTTP request
	sapi_request_len = Q_snprintf( sapi_request, sizeof( sapi_request ),
		"GET %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"User-Agent: Mozilla/5.0\r\n"
		"Accept: application/json\r\n"
		"Connection: close\r\n"
		"\r\n",
		sapi_target_path, sapi_target_host );
	sapi_bytes_sent = 0;

	sapi_state = SAPI_STATE_SEND;
}


static void SAPI_StateSend( void )
{
	int res = send( sapi_sock, sapi_request + sapi_bytes_sent,
		sapi_request_len - sapi_bytes_sent, 0 );

	if( res > 0 )
	{
		sapi_bytes_sent += res;
		if( sapi_bytes_sent >= sapi_request_len )
		{
			// Allocate response buffer (+1 for null terminator)
			sapi_response_cap = SAPI_RESPONSE_MAX;
			sapi_response = Mem_Malloc( host.mempool, sapi_response_cap + 1 );
			sapi_response_len = 0;
			sapi_got_header = false;
			sapi_header_end = 0;
			sapi_content_length = -1;
			sapi_state = SAPI_STATE_RECV_HEADER;
		}
		return;
	}

	if( res < 0 )
	{
		int err = WSAGetLastError();
		if( err != WSAEWOULDBLOCK && err != WSAEINPROGRESS )
		{
			Con_DPrintf( "SteamAPI: send failed: %s\n", NET_ErrorString() );
			sapi_state = SAPI_STATE_DONE;
		}
	}
}

static qboolean SAPI_HandleRedirect( const char *loc, int loc_len )
{
	if( ++sapi_redirect_count > SAPI_MAX_REDIRECTS )
	{
		Con_DPrintf( "SteamAPI: too many redirects\n" );
		return false;
	}

	if( !SAPI_ParseURL( loc, loc_len ) )
	{
		Con_DPrintf( "SteamAPI: bad redirect URL\n" );
		return false;
	}

	SAPI_CloseSocket();
	SAPI_FreeResponse();
	sapi_state = SAPI_STATE_RESOLVE;
	return true;
}


static qboolean SAPI_HandleHeaderComplete( void )
{
	int status;
	const char *cl_hdr;

	sapi_response[sapi_response_len] = '\0';
	status = SAPI_ParseStatusCode( (char *)sapi_response, sapi_response_len );

	// Handle redirects
	if( status == 301 || status == 302 || status == 303 || status == 307 || status == 308 )
	{
		const char *loc = SAPI_FindHeader( (char *)sapi_response, "Location" );
		if( loc )
		{
			const char *loc_end = Q_strstr( loc, "\r\n" );
			if( !loc_end ) loc_end = Q_strstr( loc, "\n" );
			if( loc_end )
			{
				int loc_len = (int)( loc_end - loc );
				if( SAPI_HandleRedirect( loc, loc_len ) )
					return true;
			}
		}
		Con_DPrintf( "SteamAPI: redirect without Location\n" );
		sapi_state = SAPI_STATE_DONE;
		return true;
	}

	if( status != 200 )
	{
		Con_DPrintf( "SteamAPI: HTTP %d\n", status < 0 ? 0 : status );
		sapi_state = SAPI_STATE_DONE;
		return true;
	}

	cl_hdr = SAPI_FindHeader( (char *)sapi_response, "Content-Length" );
	if( cl_hdr )
	{
		sapi_content_length = Q_atoi( cl_hdr );
		if( sapi_content_length > sapi_response_cap - sapi_header_end )
		{
			Con_DPrintf( "SteamAPI: payload too large (%d)\n", sapi_content_length );
			sapi_state = SAPI_STATE_DONE;
			return true;
		}
	}

	sapi_state = SAPI_STATE_RECV_BODY;
	return false;
}

static void SAPI_StateRecv( void )
{
	byte tmpbuf[4096];
	int available, res;

	available = sapi_response_cap - sapi_response_len;
	if( available <= 0 )
	{
		sapi_state = SAPI_STATE_DONE;
		return;
	}

	res = recv( sapi_sock, (char *)tmpbuf,
		Q_min( (int)sizeof( tmpbuf ), available ), 0 );

	if( res > 0 )
	{
		memcpy( sapi_response + sapi_response_len, tmpbuf, res );
		sapi_response_len += res;

		if( !sapi_got_header )
		{
			char *hdr_end;
			sapi_response[sapi_response_len] = '\0';
			hdr_end = Q_strstr( (char *)sapi_response, "\r\n\r\n" );
			if( hdr_end )
			{
				sapi_got_header = true;
				sapi_header_end = (int)( hdr_end - (char *)sapi_response ) + 4;
				if( SAPI_HandleHeaderComplete() )
					return;
			}
		}

		if( sapi_got_header && sapi_content_length > 0 )
		{
			int body_received = sapi_response_len - sapi_header_end;
			if( body_received >= sapi_content_length )
				sapi_state = SAPI_STATE_DONE;
		}
		return;
	}

	if( res == 0 )
	{
		sapi_state = SAPI_STATE_DONE;
		return;
	}

	// res < 0
	{
		int err = WSAGetLastError();
		if( err != WSAEWOULDBLOCK && err != WSAEINPROGRESS )
		{
			Con_DPrintf( "SteamAPI: recv error: %s\n", NET_ErrorString() );
			sapi_state = SAPI_STATE_DONE;
		}
	}
}


static void SAPI_ProcessStateMachine( void )
{
	int max_iters = 4;

	while( max_iters-- > 0 )
	{
		sapi_state_t prev = sapi_state;

		switch( sapi_state )
		{
		case SAPI_STATE_RESOLVE:     SAPI_StateResolve(); break;
		case SAPI_STATE_CONNECT:     SAPI_StateConnect(); break;
		case SAPI_STATE_SEND:        SAPI_StateSend();    break;
		case SAPI_STATE_RECV_HEADER:
		case SAPI_STATE_RECV_BODY:   SAPI_StateRecv();    break;
		case SAPI_STATE_DONE:
		case SAPI_STATE_IDLE:        return;
		}

		if( sapi_state == prev )
			return; // would block
	}
}

// ---------------------------------------------------------------------------
// Non-Android: Public API
// ---------------------------------------------------------------------------

void Slayer_SteamAPI_Init( void )
{
	Cvar_RegisterVariable( &slayer_steam_apikey );
	sapi_initialized = true;
	sapi_batch_in_progress = false;
	sapi_state = SAPI_STATE_IDLE;
	sapi_sock = -1;
	sapi_response = NULL;
	Con_Printf( "Slayer3D: Steam Web API init OK (HTTP socket)\n" );
}

void Slayer_SteamAPI_Reset( void )
{
	SAPI_Cleanup();
	sapi_batch_count = 0;
}

void Slayer_SteamAPI_RequestBatch( const uint64_t *steamids, int count )
{
	int i, written;
	char ids_buf[768]; // 32 * (17+1) = 576 max
	char *p;

	if( !sapi_initialized )
		return;

	if( sapi_batch_in_progress )
		return;

	if( slayer_steam_apikey.string[0] == '\0' )
		return;

	if( !steamids || count <= 0 )
		return;

	// Build comma-separated SteamID list
	p = ids_buf;
	written = 0;
	sapi_batch_count = 0;

	for( i = 0; i < count && i < SAPI_MAX_PLAYERS; i++ )
	{
		if( steamids[i] == 0 )
			continue;

		if( written > 0 )
			*p++ = ',';

		p += Q_snprintf( p, sizeof( ids_buf ) - (int)(p - ids_buf),
			"%" PRIu64, steamids[i] );
		sapi_batch_ids[sapi_batch_count] = steamids[i];
		sapi_batch_slots[sapi_batch_count] = i;
		sapi_batch_count++;
		written++;
	}
	*p = '\0';

	if( written == 0 )
		return;

	// Setup HTTP target path
	Q_strncpy( sapi_target_host, SAPI_STEAM_API_HOST, sizeof( sapi_target_host ) );
	sapi_target_port = SAPI_STEAM_API_PORT;
	Q_snprintf( sapi_target_path, sizeof( sapi_target_path ),
		"/ISteamUser/GetPlayerSummaries/v2/?key=%s&steamids=%s",
		slayer_steam_apikey.string, ids_buf );

	sapi_state = SAPI_STATE_RESOLVE;
	sapi_batch_in_progress = true;
	sapi_start_time = host.realtime;
	sapi_redirect_count = 0;
	sapi_resolving = false;

	Con_Printf( "Slayer3D: batch avatar request started (%d players)\n", written );
}


qboolean Slayer_SteamAPI_Frame( void )
{
	if( !sapi_batch_in_progress )
		return false;

	// Timeout check
	if( host.realtime - sapi_start_time > SAPI_TIMEOUT )
	{
		Con_DPrintf( "SteamAPI: batch request timeout\n" );
		SAPI_Cleanup();
		return false;
	}

	sapi_resolving = false; // reset DNS throttle each frame

	SAPI_ProcessStateMachine();

	if( sapi_state == SAPI_STATE_DONE )
	{
		// Parse JSON response and trigger avatar downloads
		if( sapi_got_header && sapi_response_len > sapi_header_end )
		{
			SAPI_ParseResponse();
			Con_Printf( "Slayer3D: batch avatar response parsed\n" );
		}
		else
		{
			Con_DPrintf( "SteamAPI: empty or invalid response\n" );
		}

		SAPI_Cleanup();
		return true;
	}

	return false;
}

#endif /* XASH_ANDROID */
