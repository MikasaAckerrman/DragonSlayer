/*
cl_steam_api.c - Slayer3D Steam Web API integration
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Steam Web API: GetPlayerSummaries/v2 for batch avatar URL resolution.
Android: JNI -> Java SteamAPIHelper.getPlayerSummaries() -> JSON -> parse.
Non-Android: HTTP socket to api.steampowered.com:80 (limited - no TLS).
*/

#include <inttypes.h>
#include <stdlib.h>
#include "common.h"
#include "client.h"
#include "cl_steam_api.h"
#include "cl_avatar_download.h"


// ===========================================================================
// Configuration
// ===========================================================================

#define SAPI_BATCH_COOLDOWN    30.0    // seconds between batch requests
#define SAPI_TIMEOUT           20.0    // seconds before giving up on API request
#define SAPI_MAX_RESPONSE      262144  // 256KB max JSON response
#define SAPI_MAX_IDS_PER_REQ   32      // Steam API limit: 100, we use MAX_CLIENTS=32
#define SAPI_API_HOST          "api.steampowered.com"
#define SAPI_API_PORT          80

// ===========================================================================
// Cvars
// ===========================================================================

static CVAR_DEFINE_AUTO( slayer_steam_apikey, "", FCVAR_ARCHIVE | FCVAR_PROTECTED,
	"Steam Web API key for batch avatar downloads" );
static CVAR_DEFINE_AUTO( slayer_steamid64, "", FCVAR_ARCHIVE,
	"Local player SteamID64 (set by OpenID login or manually)" );

// ===========================================================================
// Static state
// ===========================================================================

static uint64_t sapi_local_steamid;         // local player's SteamID64
static double   sapi_last_batch_time;       // when last batch was sent
static qboolean sapi_batch_in_progress;     // true while request active


// Batch request: remembered SteamIDs for avatar download after URL parse
static uint64_t sapi_batch_ids[SAPI_MAX_IDS_PER_REQ];
static int      sapi_batch_count;

#if XASH_ANDROID
// ===========================================================================
// ANDROID IMPLEMENTATION - JNI + pthread
// ===========================================================================
#include <pthread.h>
#include <jni.h>
#include <SDL.h>
#include <android/log.h>

static JavaVM    *sapi_jvm;
static jclass     sapi_helper_class;       // global ref to SteamAPIHelper class
static jmethodID  sapi_get_summaries_mid;  // getPlayerSummaries(String apikey, String steamids) -> String (JSON)

// Worker thread result
static volatile int sapi_worker_state;  // 0=idle, 1=working, 2=done_ok, 3=done_fail
static char        *sapi_worker_result; // heap-allocated JSON string (or NULL)


typedef struct
{
	char apikey[64];
	char steamids_csv[768];  // comma-separated SteamID64 list
} sapi_work_t;

static void *SAPI_WorkerThread( void *arg )
{
	sapi_work_t *work = (sapi_work_t *)arg;
	JNIEnv *env = NULL;
	jstring j_apikey, j_steamids;
	jstring j_result;
	const char *result_str;

	__android_log_print( ANDROID_LOG_DEBUG, "Xash", "SteamAPI: worker starting" );

	if( (*sapi_jvm)->AttachCurrentThread( sapi_jvm, &env, NULL ) != JNI_OK )
	{
		__sync_synchronize();
		sapi_worker_state = 3;
		__sync_synchronize();
		free( work );
		return NULL;
	}

	j_apikey = (*env)->NewStringUTF( env, work->apikey );
	j_steamids = (*env)->NewStringUTF( env, work->steamids_csv );

	if( !j_apikey || !j_steamids )
	{
		if( j_apikey ) (*env)->DeleteLocalRef( env, j_apikey );
		if( j_steamids ) (*env)->DeleteLocalRef( env, j_steamids );
		(*sapi_jvm)->DetachCurrentThread( sapi_jvm );
		__sync_synchronize();
		sapi_worker_state = 3;
		__sync_synchronize();
		free( work );
		return NULL;
	}


	// Call Java: static String SteamAPIHelper.getPlayerSummaries(String key, String ids)
	j_result = (jstring)(*env)->CallStaticObjectMethod( env, sapi_helper_class,
		sapi_get_summaries_mid, j_apikey, j_steamids );

	if( (*env)->ExceptionCheck( env ) )
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
		(*env)->DeleteLocalRef( env, j_apikey );
		(*env)->DeleteLocalRef( env, j_steamids );
		(*sapi_jvm)->DetachCurrentThread( sapi_jvm );
		__sync_synchronize();
		sapi_worker_state = 3;
		__sync_synchronize();
		free( work );
		return NULL;
	}

	if( j_result )
	{
		result_str = (*env)->GetStringUTFChars( env, j_result, NULL );
		if( result_str )
		{
			int len = (int)Q_strlen( result_str );
			sapi_worker_result = (char *)malloc( len + 1 );
			if( sapi_worker_result )
				memcpy( sapi_worker_result, result_str, len + 1 );
			(*env)->ReleaseStringUTFChars( env, j_result, result_str );
		}
		(*env)->DeleteLocalRef( env, j_result );
	}

	(*env)->DeleteLocalRef( env, j_apikey );
	(*env)->DeleteLocalRef( env, j_steamids );
	(*sapi_jvm)->DetachCurrentThread( sapi_jvm );

	__sync_synchronize();
	sapi_worker_state = sapi_worker_result ? 2 : 3;
	__sync_synchronize();

	free( work );
	return NULL;
}


// Android: start batch request via JNI worker thread
static void SAPI_StartBatchRequest_Android( const char *apikey, const char *ids_csv )
{
	sapi_work_t *work;
	pthread_t thread;

	work = (sapi_work_t *)malloc( sizeof( sapi_work_t ) );
	if( !work )
		return;

	Q_strncpy( work->apikey, apikey, sizeof( work->apikey ) );
	Q_strncpy( work->steamids_csv, ids_csv, sizeof( work->steamids_csv ) );

	sapi_worker_state = 1;
	sapi_worker_result = NULL;

	if( pthread_create( &thread, NULL, SAPI_WorkerThread, work ) != 0 )
	{
		free( work );
		sapi_worker_state = 0;
		Con_Printf( S_WARN "SteamAPI: pthread_create failed\n" );
		return;
	}

	pthread_detach( thread );
	sapi_batch_in_progress = true;
}

void Slayer_SteamAPI_Init( void )
{
	JNIEnv *env;
	jobject activity;
	jclass cls;

	Cvar_RegisterVariable( &slayer_steam_apikey );
	Cvar_RegisterVariable( &slayer_steamid64 );

	sapi_local_steamid = 0;
	sapi_last_batch_time = 0.0;
	sapi_batch_in_progress = false;
	sapi_batch_count = 0;
	sapi_worker_state = 0;
	sapi_worker_result = NULL;
	sapi_jvm = NULL;
	sapi_helper_class = NULL;
	sapi_get_summaries_mid = NULL;


	// Restore local steamid from cvar
	if( slayer_steamid64.string[0] != '\0' )
	{
		sapi_local_steamid = strtoull( slayer_steamid64.string, NULL, 10 );
	}

	// Get JNI env from SDL
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

	// Find SteamAPIHelper class
	activity = (jobject)SDL_AndroidGetActivity();
	if( !activity )
	{
		Con_Printf( S_WARN "SteamAPI: failed to get activity\n" );
		return;
	}

	// Try to find our helper class (in engine package)
	cls = (*env)->FindClass( env, "su/xash/engine/SteamAPIHelper" );
	if( (*env)->ExceptionCheck( env ) )
	{
		(*env)->ExceptionClear( env );
		cls = NULL;
	}

	(*env)->DeleteLocalRef( env, activity );

	if( !cls )
	{
		Con_Printf( "SteamAPI: SteamAPIHelper class not found (batch API disabled)\n" );
		return;
	}


	sapi_helper_class = (*env)->NewGlobalRef( env, cls );
	(*env)->DeleteLocalRef( env, cls );

	if( !sapi_helper_class )
	{
		if( (*env)->ExceptionCheck( env ) )
			(*env)->ExceptionClear( env );
		Con_Printf( S_WARN "SteamAPI: NewGlobalRef failed\n" );
		return;
	}

	// Find method: static String getPlayerSummaries(String apikey, String steamids)
	sapi_get_summaries_mid = (*env)->GetStaticMethodID( env, sapi_helper_class,
		"getPlayerSummaries",
		"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;" );

	if( !sapi_get_summaries_mid )
	{
		if( (*env)->ExceptionCheck( env ) )
		{
			(*env)->ExceptionDescribe( env );
			(*env)->ExceptionClear( env );
		}
		Con_Printf( S_WARN "SteamAPI: getPlayerSummaries method not found\n" );
		(*env)->DeleteGlobalRef( env, sapi_helper_class );
		sapi_helper_class = NULL;
		return;
	}

	Con_Printf( "Slayer3D: Steam Web API JNI init OK\n" );
}

void Slayer_SteamAPI_Shutdown( void )
{
	sapi_batch_in_progress = false;
	sapi_worker_state = 0;
	if( sapi_worker_result )
	{
		free( sapi_worker_result );
		sapi_worker_result = NULL;
	}
}


void Slayer_SteamAPI_Reset( void )
{
	Slayer_SteamAPI_Shutdown();
	sapi_last_batch_time = 0.0;
	sapi_batch_count = 0;
	memset( sapi_batch_ids, 0, sizeof( sapi_batch_ids ) );
}

#else /* !XASH_ANDROID */
// ===========================================================================
// NON-ANDROID IMPLEMENTATION - HTTP sockets (port 80)
// ===========================================================================
#include "net_ws_private.h"

// State machine for HTTP request to Steam API
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

static sapi_state_t sapi_state;
static int          sapi_sock;
static struct sockaddr_storage sapi_addr;
static double       sapi_start_time;
static qboolean     sapi_resolving;

// HTTP request/response
static char   sapi_request[1024];
static int    sapi_request_len;
static int    sapi_bytes_sent;
static byte  *sapi_response;
static int    sapi_response_len;
static int    sapi_response_cap;
static int    sapi_content_length;
static qboolean sapi_got_header;
static int    sapi_header_end;


// Non-Android init
void Slayer_SteamAPI_Init( void )
{
	Cvar_RegisterVariable( &slayer_steam_apikey );
	Cvar_RegisterVariable( &slayer_steamid64 );

	sapi_local_steamid = 0;
	sapi_last_batch_time = 0.0;
	sapi_batch_in_progress = false;
	sapi_batch_count = 0;
	sapi_state = SAPI_STATE_IDLE;
	sapi_sock = -1;
	sapi_response = NULL;
	sapi_response_len = 0;
	sapi_response_cap = 0;
	sapi_got_header = false;
	sapi_resolving = false;

	if( slayer_steamid64.string[0] != '\0' )
	{
		sapi_local_steamid = strtoull( slayer_steamid64.string, NULL, 10 );
	}

	Con_Printf( "Slayer3D: Steam Web API HTTP init OK\n" );
}

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

void Slayer_SteamAPI_Shutdown( void )
{
	SAPI_CloseSocket();
	SAPI_FreeResponse();
	sapi_batch_in_progress = false;
	sapi_state = SAPI_STATE_IDLE;
}

void Slayer_SteamAPI_Reset( void )
{
	Slayer_SteamAPI_Shutdown();
	sapi_last_batch_time = 0.0;
	sapi_batch_count = 0;
	memset( sapi_batch_ids, 0, sizeof( sapi_batch_ids ) );
}


// Start HTTP request to Steam API
static void SAPI_StartBatchRequest_HTTP( const char *apikey, const char *ids_csv )
{
	// Build GET request path
	// /ISteamUser/GetPlayerSummaries/v2/?key=KEY&steamids=ID1,ID2,...
	sapi_request_len = Q_snprintf( sapi_request, sizeof( sapi_request ),
		"GET /ISteamUser/GetPlayerSummaries/v2/?key=%s&steamids=%s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"User-Agent: Mozilla/5.0\r\n"
		"Accept: application/json\r\n"
		"Connection: close\r\n"
		"\r\n",
		apikey, ids_csv, SAPI_API_HOST );

	sapi_bytes_sent = 0;
	sapi_state = SAPI_STATE_RESOLVE;
	sapi_start_time = host.realtime;
	sapi_batch_in_progress = true;
	sapi_resolving = false;

	Con_DPrintf( "SteamAPI: starting batch request (%s)\n", ids_csv );
}

// State: DNS resolve
static void SAPI_StateResolve( void )
{
	net_gai_state_t res;

	if( sapi_resolving )
		return;

	memset( &sapi_addr, 0, sizeof( sapi_addr ) );
	res = NET_StringToSockaddr( SAPI_API_HOST, &sapi_addr, true, AF_INET );

	if( res == NET_EAI_AGAIN )
	{
		sapi_resolving = true;
		return;
	}

	if( res == NET_EAI_NONAME )
	{
		Con_DPrintf( "SteamAPI: DNS failed for %s\n", SAPI_API_HOST );
		sapi_state = SAPI_STATE_DONE;
		return;
	}

	((struct sockaddr_in *)&sapi_addr)->sin_port = htons( SAPI_API_PORT );


	sapi_sock = socket( sapi_addr.ss_family, SOCK_STREAM, IPPROTO_TCP );
	if( sapi_sock < 0 )
	{
		Con_DPrintf( "SteamAPI: socket() failed\n" );
		sapi_state = SAPI_STATE_DONE;
		return;
	}

	if( !NET_MakeSocketNonBlocking( sapi_sock ) )
	{
		SAPI_CloseSocket();
		sapi_state = SAPI_STATE_DONE;
		return;
	}

	sapi_state = SAPI_STATE_CONNECT;
}

// State: TCP connect
static void SAPI_StateConnect( void )
{
	int res = connect( sapi_sock, (struct sockaddr *)&sapi_addr,
		NET_SockAddrLen( &sapi_addr ) );

	if( res < 0 )
	{
		int err = WSAGetLastError();
		if( err == WSAEISCONN )
			; // connected
		else if( err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY )
			return; // still connecting
		else
		{
			Con_DPrintf( "SteamAPI: connect failed\n" );
			sapi_state = SAPI_STATE_DONE;
			return;
		}
	}

	sapi_state = SAPI_STATE_SEND;
}

// State: send HTTP request
static void SAPI_StateSend( void )
{
	int res = send( sapi_sock, sapi_request + sapi_bytes_sent,
		sapi_request_len - sapi_bytes_sent, 0 );

	if( res > 0 )
	{
		sapi_bytes_sent += res;
		if( sapi_bytes_sent >= sapi_request_len )
		{
			sapi_response = Mem_Malloc( host.mempool, SAPI_MAX_RESPONSE + 1 );
			sapi_response_cap = SAPI_MAX_RESPONSE;
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
			Con_DPrintf( "SteamAPI: send failed\n" );
			sapi_state = SAPI_STATE_DONE;
		}
	}
}

// State: receive HTTP response
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

	res = recv( sapi_sock, (char *)tmpbuf, Q_min( (int)sizeof( tmpbuf ), available ), 0 );

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

				// Check HTTP status code. Steam API on port 80 likely
				// returns 301 redirect to HTTPS. Detect non-200 early
				// to fall back to XML without waiting for full body.
				{
					int status = 0;
					if( sapi_response_len >= 12 &&
					    Q_strncmp( (char *)sapi_response, "HTTP/1.", 7 ) == 0 &&
					    ((char *)sapi_response)[8] == ' ' )
					{
						status = ( ((char *)sapi_response)[9] - '0' ) * 100
						       + ( ((char *)sapi_response)[10] - '0' ) * 10
						       + ( ((char *)sapi_response)[11] - '0' );
					}

					if( status != 200 && status != 0 )
					{
						Con_DPrintf( "SteamAPI: HTTP %d (expected 200), falling back to XML\n", status );
						sapi_state = SAPI_STATE_DONE;
						return;
					}
				}

				sapi_state = SAPI_STATE_RECV_BODY;

				// Parse Content-Length if present
				{
					const char *cl_hdr = Q_strstr( (char *)sapi_response, "Content-Length:" );
					if( !cl_hdr )
						cl_hdr = Q_strstr( (char *)sapi_response, "content-length:" );
					if( cl_hdr )
					{
						cl_hdr += 15;
						while( *cl_hdr == ' ' ) cl_hdr++;
						sapi_content_length = Q_atoi( cl_hdr );
					}
				}
			}
		}


		// Check if body complete
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
		// Connection closed - done
		sapi_state = SAPI_STATE_DONE;
		return;
	}

	// res < 0
	{
		int err = WSAGetLastError();
		if( err != WSAEWOULDBLOCK && err != WSAEINPROGRESS )
		{
			Con_DPrintf( "SteamAPI: recv error\n" );
			sapi_state = SAPI_STATE_DONE;
		}
	}
}

#endif /* XASH_ANDROID */


// ===========================================================================
// JSON parsing - extract avatar URLs from GetPlayerSummaries response
// ===========================================================================
// Minimal JSON parser: we only need steamid + avatarmedium fields from
// each player object in the "players" array.

typedef struct
{
	uint64_t steamid64;
	char     avatar_url[512];
} sapi_player_info_t;

// Find a JSON string value for a given key within a JSON object substring.
// Returns pointer to the value start (after opening quote), or NULL.
// Sets *out_len to the length of the value (excluding quotes).
static const char *SAPI_JsonFindString( const char *json, const char *key, int *out_len )
{
	char search[64];
	const char *p, *val_start, *val_end;

	Q_snprintf( search, sizeof( search ), "\"%s\"", key );
	p = Q_strstr( json, search );
	if( !p )
		return NULL;

	// Skip key + colon
	p += Q_strlen( search );
	while( *p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r' )
		p++;

	if( *p != '"' )
		return NULL;

	val_start = p + 1;
	val_end = val_start;
	while( *val_end && *val_end != '"' )
	{
		if( *val_end == '\\' && val_end[1] )
			val_end++; // skip escaped char
		val_end++;
	}

	*out_len = (int)( val_end - val_start );
	return val_start;
}


// Parse the GetPlayerSummaries JSON and extract avatar URLs.
// For each player found, triggers avatar image download.
static void SAPI_ParseAndDownload( const char *json )
{
	const char *players_arr;
	const char *cursor;
	int count = 0;

	if( !json || json[0] == '\0' )
	{
		Con_DPrintf( "SteamAPI: empty response\n" );
		return;
	}

	// Find "players" array
	players_arr = Q_strstr( json, "\"players\"" );
	if( !players_arr )
	{
		Con_DPrintf( "SteamAPI: no players array in response\n" );
		return;
	}

	// Skip to array start '['
	cursor = players_arr + 9;
	while( *cursor && *cursor != '[' )
		cursor++;
	if( *cursor != '[' )
		return;
	cursor++;

	// Iterate through player objects
	while( *cursor )
	{
		const char *obj_start, *obj_end;
		const char *steamid_val, *avatar_val;
		int steamid_len, avatar_len;
		uint64_t steamid64;
		char steamid_buf[21];
		char avatar_url[512];
		int i;

		// Find next object '{'
		while( *cursor && *cursor != '{' && *cursor != ']' )
			cursor++;
		if( *cursor != '{' )
			break;

		obj_start = cursor;

		// Find matching '}'
		{
			int depth = 1;
			obj_end = obj_start + 1;
			while( *obj_end && depth > 0 )
			{
				if( *obj_end == '{' ) depth++;
				else if( *obj_end == '}' ) depth--;
				obj_end++;
			}
		}


		// Extract steamid
		steamid_val = SAPI_JsonFindString( obj_start, "steamid", &steamid_len );
		if( !steamid_val || steamid_len <= 0 || steamid_len >= (int)sizeof( steamid_buf ) )
		{
			cursor = obj_end;
			continue;
		}

		memcpy( steamid_buf, steamid_val, steamid_len );
		steamid_buf[steamid_len] = '\0';
		steamid64 = strtoull( steamid_buf, NULL, 10 );

		if( steamid64 == 0 )
		{
			cursor = obj_end;
			continue;
		}

		// Extract avatarmedium URL (preferred) or avatar
		avatar_val = SAPI_JsonFindString( obj_start, "avatarmedium", &avatar_len );
		if( !avatar_val || avatar_len <= 0 )
		{
			avatar_val = SAPI_JsonFindString( obj_start, "avatar", &avatar_len );
		}

		if( !avatar_val || avatar_len <= 0 || avatar_len >= (int)sizeof( avatar_url ) )
		{
			cursor = obj_end;
			continue;
		}

		memcpy( avatar_url, avatar_val, avatar_len );
		avatar_url[avatar_len] = '\0';

		// Find which slot this SteamID belongs to and trigger download
		for( i = 0; i < sapi_batch_count; i++ )
		{
			if( sapi_batch_ids[i] == steamid64 )
			{
				// Queue download for this avatar URL directly
				// The avatar download system will handle it
				Slayer_AvatarDownload_Request( steamid64, i );
				Con_DPrintf( "SteamAPI: resolved avatar for %" PRIu64 " -> %s\n",
					steamid64, avatar_url );
				count++;
				break;
			}
		}

		cursor = obj_end;
	}

	Con_Printf( "Slayer3D: Steam API resolved %d avatar(s)\n", count );
}


// ===========================================================================
// Common API (both platforms)
// ===========================================================================

qboolean Slayer_SteamAPI_HasKey( void )
{
	return ( slayer_steam_apikey.string[0] != '\0' );
}

uint64_t Slayer_SteamAPI_GetLocalSteamID( void )
{
	return sapi_local_steamid;
}

void Slayer_SteamAPI_SetLocalSteamID( uint64_t steamid64 )
{
	char buf[21];

	sapi_local_steamid = steamid64;

	// Persist to cvar
	Q_snprintf( buf, sizeof( buf ), "%" PRIu64, steamid64 );
	Cvar_Set( "slayer_steamid64", buf );

	Con_Printf( "Slayer3D: local SteamID64 set to %" PRIu64 "\n", steamid64 );
}

void Slayer_SteamAPI_RequestBatchAvatars( const uint64_t *steamids, int count )
{
	char ids_csv[768];
	int  ids_csv_len = 0;
	int  i, num_valid = 0;

	// Guard: need API key
	if( !Slayer_SteamAPI_HasKey() )
		return;

	// Guard: already in progress
	if( sapi_batch_in_progress )
		return;

	// Guard: cooldown
	if( host.realtime - sapi_last_batch_time < SAPI_BATCH_COOLDOWN )
		return;

	// Clamp count
	if( count > SAPI_MAX_IDS_PER_REQ )
		count = SAPI_MAX_IDS_PER_REQ;

	// Build CSV of SteamID64s
	ids_csv[0] = '\0';

	for( i = 0; i < count; i++ )
	{
		sapi_batch_ids[i] = steamids[i];

		if( steamids[i] == 0 )
			continue;


		if( num_valid > 0 )
		{
			ids_csv_len += Q_snprintf( ids_csv + ids_csv_len,
				sizeof( ids_csv ) - ids_csv_len, "," );
		}

		ids_csv_len += Q_snprintf( ids_csv + ids_csv_len,
			sizeof( ids_csv ) - ids_csv_len,
			"%" PRIu64, steamids[i] );

		num_valid++;
	}

	sapi_batch_count = count;

	if( num_valid == 0 )
		return;

	sapi_last_batch_time = host.realtime;

	Con_DPrintf( "SteamAPI: batch request for %d players\n", num_valid );

#if XASH_ANDROID
	if( sapi_get_summaries_mid )
		SAPI_StartBatchRequest_Android( slayer_steam_apikey.string, ids_csv );
	else
	{
		Con_DPrintf( "SteamAPI: JNI not available, falling back to per-player XML\n" );
		// Fall back: trigger individual downloads for each player
		for( i = 0; i < count; i++ )
		{
			if( steamids[i] != 0 )
				Slayer_AvatarDownload_Request( steamids[i], i );
		}
	}
#else
	SAPI_StartBatchRequest_HTTP( slayer_steam_apikey.string, ids_csv );
#endif
}


qboolean Slayer_SteamAPI_Frame( void )
{
#if XASH_ANDROID
	if( !sapi_batch_in_progress )
		return false;

	__sync_synchronize();

	if( sapi_worker_state == 2 )
	{
		// Success - parse JSON result
		sapi_batch_in_progress = false;
		sapi_worker_state = 0;

		if( sapi_worker_result )
		{
			SAPI_ParseAndDownload( sapi_worker_result );
			free( sapi_worker_result );
			sapi_worker_result = NULL;
		}
		return true;
	}
	else if( sapi_worker_state == 3 )
	{
		// Failure
		sapi_batch_in_progress = false;
		sapi_worker_state = 0;
		Con_DPrintf( "SteamAPI: batch request failed\n" );

		if( sapi_worker_result )
		{
			free( sapi_worker_result );
			sapi_worker_result = NULL;
		}

		// Fall back to individual XML downloads
		{
			int i;
			for( i = 0; i < sapi_batch_count; i++ )
			{
				if( sapi_batch_ids[i] != 0 )
					Slayer_AvatarDownload_Request( sapi_batch_ids[i], i );
			}
		}
		return false;
	}

	return false;

#else /* Non-Android */
	if( !sapi_batch_in_progress )
		return false;

	// Timeout check
	if( host.realtime - sapi_start_time > SAPI_TIMEOUT )
	{
		Con_DPrintf( "SteamAPI: request timed out\n" );
		SAPI_CloseSocket();
		SAPI_FreeResponse();
		sapi_state = SAPI_STATE_IDLE;
		sapi_batch_in_progress = false;


		// Fall back to individual XML downloads
		{
			int i;
			for( i = 0; i < sapi_batch_count; i++ )
			{
				if( sapi_batch_ids[i] != 0 )
					Slayer_AvatarDownload_Request( sapi_batch_ids[i], i );
			}
		}
		return false;
	}

	// Process state machine
	sapi_resolving = false;

	switch( sapi_state )
	{
	case SAPI_STATE_RESOLVE:     SAPI_StateResolve(); break;
	case SAPI_STATE_CONNECT:     SAPI_StateConnect(); break;
	case SAPI_STATE_SEND:        SAPI_StateSend();    break;
	case SAPI_STATE_RECV_HEADER:
	case SAPI_STATE_RECV_BODY:   SAPI_StateRecv();    break;
	case SAPI_STATE_DONE:
	{
		// Parse response body
		sapi_batch_in_progress = false;

		if( sapi_got_header && sapi_response && sapi_response_len > sapi_header_end )
		{
			sapi_response[sapi_response_len] = '\0';
			SAPI_ParseAndDownload( (const char *)sapi_response + sapi_header_end );
		}
		else
		{
			Con_DPrintf( "SteamAPI: no valid response body\n" );
			// Fall back to individual XML downloads
			{
				int i;
				for( i = 0; i < sapi_batch_count; i++ )
				{
					if( sapi_batch_ids[i] != 0 )
						Slayer_AvatarDownload_Request( sapi_batch_ids[i], i );
				}
			}
		}

		SAPI_CloseSocket();
		SAPI_FreeResponse();
		sapi_state = SAPI_STATE_IDLE;
		return true;
	}
	default:
		break;
	}

	return false;
#endif
}
