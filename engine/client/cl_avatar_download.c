/*
cl_avatar_download.c - Slayer3D async Steam avatar downloader
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Android: Uses JNI to call XashActivity.downloadAvatar() from worker pthreads.
Other platforms: Uses non-blocking HTTP sockets (port 80, no TLS).
*/

#include <inttypes.h>
#include "common.h"
#include "client.h"
#include "cl_avatar_download.h"

#if XASH_ANDROID
// ===========================================================================
// ANDROID IMPLEMENTATION - JNI + pthread
// ===========================================================================
#include <pthread.h>
#include <jni.h>
#include <SDL.h>
#include <stdlib.h>
#include <android/log.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define AVD_MAX_CONCURRENT  4
#define AVD_RETRY_DELAY     60.0

// Slot result values (written by worker thread, read by Frame)
#define AVD_RESULT_IDLE        0
#define AVD_RESULT_IN_PROGRESS 1
#define AVD_RESULT_SUCCESS     2
#define AVD_RESULT_FAIL        3
#define AVD_RESULT_DONE        4

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static JavaVM   *avd_jvm;             // cached from Init()
static jclass    avd_activity_class;   // global ref to XashActivity class
static jmethodID avd_download_method;  // downloadAvatar method ID

static volatile int avd_slot_result[MAX_CLIENTS];  // thread-safe via volatile + barriers
static uint64_t     avd_slot_id[MAX_CLIENTS];
static double       avd_slot_fail_time[MAX_CLIENTS];
static int          avd_active_count;              // number of in-progress downloads

static CVAR_DEFINE_AUTO( slayer_avatar_download, "1", FCVAR_ARCHIVE,
	"Slayer3D: auto-download Steam avatars (0=disabled)" );

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

typedef struct
{
	int      slot;
	uint64_t steamid64;
} avd_work_t;

static void *AVD_WorkerThread( void *arg )
{
	avd_work_t *work = (avd_work_t *)arg;
	JNIEnv *env = NULL;
	int result;
	char steamid_str[21];  // max uint64 is 20 digits
	char save_path[512];
	const char *basedir;
	const char *gamedir;
	jstring j_steamid, j_path;

	__android_log_print( ANDROID_LOG_DEBUG, "Xash", "AvatarDL: worker thread starting for slot=%d", work->slot );

	// Attach this thread to JVM
	if( (*avd_jvm)->AttachCurrentThread( avd_jvm, &env, NULL ) != JNI_OK )
	{
		__sync_synchronize();
		avd_slot_result[work->slot] = AVD_RESULT_FAIL;
		__sync_synchronize();
		free( work );
		return NULL;
	}

	// Build arguments
	Q_snprintf( steamid_str, sizeof( steamid_str ), "%" PRIu64, work->steamid64 );

	basedir = getenv( "XASH3D_BASEDIR" );
	if( !basedir || basedir[0] == '\0' )
		basedir = ".";

	gamedir = getenv( "XASH3D_GAME" );
	if( !gamedir || gamedir[0] == '\0' )
		gamedir = "valve";

	Q_snprintf( save_path, sizeof( save_path ), "%s/%s/avatars/%" PRIu64 ".png",
		basedir, gamedir, work->steamid64 );

	__android_log_print( ANDROID_LOG_DEBUG, "Xash", "AvatarDL: worker slot=%d steamid=%" PRIu64 " path=%s",
		work->slot, work->steamid64, save_path );

	j_steamid = (*env)->NewStringUTF( env, steamid_str );
	j_path = (*env)->NewStringUTF( env, save_path );

	if( !j_steamid || !j_path )
	{
		if( j_steamid ) (*env)->DeleteLocalRef( env, j_steamid );
		if( j_path ) (*env)->DeleteLocalRef( env, j_path );
		(*avd_jvm)->DetachCurrentThread( avd_jvm );
		__sync_synchronize();
		avd_slot_result[work->slot] = AVD_RESULT_FAIL;
		__sync_synchronize();
		free( work );
		return NULL;
	}

	// Call Java method
	result = (*env)->CallStaticIntMethod( env, avd_activity_class,
		avd_download_method, j_steamid, j_path );

	// Clear any pending JNI exception thrown by the Java method
	if( (*env)->ExceptionCheck( env ) )
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
		result = -1;  // force failure
	}

	// Defense in depth: even on JNI success, validate that what we wrote
	// to disk is actually a PNG. The Java side now re-encodes via
	// BitmapFactory + Bitmap.compress(PNG), but if that ever regresses,
	// or a partial write/truncation slipped through, the engine imagelib
	// would silently fail to load and the user would see no avatar with
	// no obvious cause. Catch it here and force a slot failure so the
	// scoreboard does not cache an in-memory texid for a corrupt file.
	if( result == 0 )
	{
		FILE *fp = fopen( save_path, "rb" );
		if( !fp )
		{
			__android_log_print( ANDROID_LOG_ERROR, "Xash",
				"AvatarDL: post-save fopen failed for slot=%d path=%s",
				work->slot, save_path );
			result = -1;
		}
		else
		{
			static const unsigned char png_magic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
			unsigned char header[8];
			size_t got = fread( header, 1, sizeof( header ), fp );
			fclose( fp );

			if( got != sizeof( header ) || memcmp( header, png_magic, sizeof( png_magic ) ) != 0 )
			{
				__android_log_print( ANDROID_LOG_ERROR, "Xash",
					"AvatarDL: bad PNG magic for slot=%d path=%s (got %zu bytes), deleting",
					work->slot, save_path, got );
				remove( save_path );
				result = -1;
			}
			else
			{
				__android_log_print( ANDROID_LOG_INFO, "Xash",
					"AvatarDL: valid PNG saved for slot=%d path=%s",
					work->slot, save_path );
			}
		}
	}

	// Cleanup JNI local refs
	(*env)->DeleteLocalRef( env, j_steamid );
	(*env)->DeleteLocalRef( env, j_path );

	// Detach from JVM
	(*avd_jvm)->DetachCurrentThread( avd_jvm );

	// Write result with memory barrier
	// Note: if Reset/Shutdown zeroed the slot array, this write is benign -
	// Frame() will just process it as a new completion next tick.
	__sync_synchronize();
	if( avd_download_method != NULL )
	{
		__android_log_print( ANDROID_LOG_DEBUG, "Xash", "AvatarDL: worker slot=%d result=%d", work->slot, result );
		avd_slot_result[work->slot] = ( result == 0 ) ? AVD_RESULT_SUCCESS : AVD_RESULT_FAIL;
	}
	__sync_synchronize();

	free( work );
	return NULL;
}

// ---------------------------------------------------------------------------
// Public API - Android
// ---------------------------------------------------------------------------

void Slayer_AvatarDownload_Init( void )
{
	JNIEnv *env;
	jobject activity;
	jclass cls;

	Cvar_RegisterVariable( &slayer_avatar_download );
	memset( (void *)avd_slot_result, 0, sizeof( avd_slot_result ) );
	memset( avd_slot_id, 0, sizeof( avd_slot_id ) );
	memset( avd_slot_fail_time, 0, sizeof( avd_slot_fail_time ) );
	avd_active_count = 0;
	avd_jvm = NULL;
	avd_activity_class = NULL;
	avd_download_method = NULL;

	// Get JNI env from SDL (only valid on main thread)
	env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if( !env )
	{
		// (file log only — no console spam)
		return;
	}

	// Cache JavaVM for worker threads
	if( (*env)->GetJavaVM( env, &avd_jvm ) != 0 )
	{
		return;
	}

	// Get XashActivity class
	activity = (jobject)SDL_AndroidGetActivity();
	if( !activity )
	{
		return;
	}

	cls = (*env)->GetObjectClass( env, activity );

	// Defensive: GetObjectClass should not throw, but if it did on some quirky
	// Android build, surface the exception and bail rather than carry it forward.
	if( (*env)->ExceptionCheck( env ) )
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
		if( cls ) (*env)->DeleteLocalRef( env, cls );
		(*env)->DeleteLocalRef( env, activity );
		avd_activity_class = NULL;
		return;
	}

	avd_activity_class = (*env)->NewGlobalRef( env, cls );
	(*env)->DeleteLocalRef( env, cls );
	(*env)->DeleteLocalRef( env, activity );

	// NewGlobalRef returns NULL if the JVM is out of memory or the local ref was NULL.
	if( !avd_activity_class )
	{
		if( (*env)->ExceptionCheck( env ) )
			(*env)->ExceptionClear( env );
		return;
	}

	// Clear any stale pending exception before the next JNI call.
	if( (*env)->ExceptionCheck( env ) )
	{
		(*env)->ExceptionDescribe( env );
		(*env)->ExceptionClear( env );
	}

	// Get downloadAvatar method: static int downloadAvatar(String, String)
	avd_download_method = (*env)->GetStaticMethodID( env, avd_activity_class,
		"downloadAvatar", "(Ljava/lang/String;Ljava/lang/String;)I" );

	if( !avd_download_method )
	{
		// On older or stripped builds the method may be missing - log,
		// clear the pending NoSuchMethodError, drop the global ref and bail.
		if( (*env)->ExceptionCheck( env ) )
		{
			(*env)->ExceptionDescribe( env );
			(*env)->ExceptionClear( env );
		}
		(*env)->DeleteGlobalRef( env, avd_activity_class );
		avd_activity_class = NULL;
		return;
	}

	__android_log_print( ANDROID_LOG_INFO, "Xash",
		"AvatarDL: JNI init SUCCESS — avd_jvm=%p class=%p method=%p",
		(void *)avd_jvm, (void *)avd_activity_class, (void *)avd_download_method );
}

void Slayer_AvatarDownload_Shutdown( void )
{
	// Worker threads are detached - they will finish on their own.
	// Null out download_method so racing threads skip their result write.
	avd_download_method = NULL;
	avd_active_count = 0;
}

void Slayer_AvatarDownload_Reset( void )
{
	Slayer_AvatarDownload_Shutdown();
	// Note: a racing detached thread may write to avd_slot_result after this memset.
	// This is benign - Frame() will just pick it up as a spurious completion.
	memset( (void *)avd_slot_result, 0, sizeof( avd_slot_result ) );
	memset( avd_slot_id, 0, sizeof( avd_slot_id ) );
	memset( avd_slot_fail_time, 0, sizeof( avd_slot_fail_time ) );
}

void Slayer_AvatarDownload_Request( uint64_t steamid64, int slot )
{
	char path[128];
	avd_work_t *work;
	pthread_t thread;

	if( slayer_avatar_download.value == 0.0f )
	{
		__android_log_print( ANDROID_LOG_DEBUG, "Xash",
			"AvatarDL: skip slot=%d steamid=%" PRIu64 " (cvar slayer_avatar_download=0)",
			slot, steamid64 );
		return;
	}

	if( slot < 0 || slot >= MAX_CLIENTS || steamid64 == 0 )
		return;

	if( !avd_download_method )
	{
		// File log only — no console spam
		{
			file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
			if( logf )
			{
				FS_Printf( logf, "[%.1f] DL_BLOCKED slot=%d id=%" PRIu64 " (method=NULL, R8 stripped?)\n",
					host.realtime, slot, steamid64 );
				FS_Close( logf );
			}
		}
		__android_log_print( ANDROID_LOG_ERROR, "Xash",
			"AvatarDL: BLOCKED slot=%d steamid=%" PRIu64 " — avd_download_method is NULL (JNI init failed or R8 stripped method)",
			slot, steamid64 );
		return;  // JNI not initialized
	}

	// Already done or in progress?
	if( avd_slot_result[slot] == AVD_RESULT_DONE ||
	    avd_slot_result[slot] == AVD_RESULT_IN_PROGRESS )
	{
		__android_log_print( ANDROID_LOG_DEBUG, "Xash",
			"AvatarDL: skip slot=%d steamid=%" PRIu64 " (slot busy: result=%d)",
			slot, steamid64, avd_slot_result[slot] );
		return;
	}

	// Recently failed - wait for retry delay
	if( avd_slot_result[slot] == AVD_RESULT_FAIL &&
	    host.realtime - avd_slot_fail_time[slot] < AVD_RETRY_DELAY )
	{
		__android_log_print( ANDROID_LOG_DEBUG, "Xash",
			"AvatarDL: skip slot=%d steamid=%" PRIu64 " (cooldown, %.1fs left)",
			slot, steamid64,
			AVD_RETRY_DELAY - ( host.realtime - avd_slot_fail_time[slot] ) );
		return;
	}

	// Max concurrent check
	if( avd_active_count >= AVD_MAX_CONCURRENT )
	{
		__android_log_print( ANDROID_LOG_DEBUG, "Xash",
			"AvatarDL: skip slot=%d steamid=%" PRIu64 " (concurrent limit %d reached)",
			slot, steamid64, AVD_MAX_CONCURRENT );
		return;
	}

	// Already cached on disk?
	Q_snprintf( path, sizeof( path ), "avatars/%" PRIu64 ".png", steamid64 );
	if( FS_FileExists( path, false ) )
	{
		__android_log_print( ANDROID_LOG_INFO, "Xash",
			"AvatarDL: cache hit slot=%d steamid=%" PRIu64 " path=%s",
			slot, steamid64, path );
		avd_slot_result[slot] = AVD_RESULT_DONE;
		return;
	}

	// Spawn worker thread
	work = (avd_work_t *)malloc( sizeof( avd_work_t ) );
	if( !work )
		return;

	work->slot = slot;
	work->steamid64 = steamid64;
	avd_slot_id[slot] = steamid64;
	avd_slot_result[slot] = AVD_RESULT_IN_PROGRESS;
	avd_active_count++;

	if( pthread_create( &thread, NULL, AVD_WorkerThread, work ) != 0 )
	{
		__android_log_print( ANDROID_LOG_ERROR, "Xash",
			"AvatarDL: pthread_create failed for slot=%d steamid=%" PRIu64,
			slot, steamid64 );
		free( work );
		avd_slot_result[slot] = AVD_RESULT_FAIL;
		avd_slot_fail_time[slot] = host.realtime;
		avd_active_count--;
		return;
	}

	pthread_detach( thread );
	__android_log_print( ANDROID_LOG_INFO, "Xash",
		"AvatarDL: queued slot=%d steamid=%" PRIu64 " (active=%d)",
		slot, steamid64, avd_active_count );
	{
		file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
		if( logf )
		{
			FS_Printf( logf, "[%.1f] DL_QUEUED slot=%d id=%" PRIu64 " active=%d\n",
				host.realtime, slot, steamid64, avd_active_count );
			FS_Close( logf );
		}
	}
}

qboolean Slayer_AvatarDownload_Frame( void )
{
	int i;
	qboolean any_completed = false;

	if( slayer_avatar_download.value == 0.0f )
		return false;

	__sync_synchronize();

	for( i = 0; i < MAX_CLIENTS; i++ )
	{
		if( avd_slot_result[i] == AVD_RESULT_SUCCESS )
		{
			avd_slot_result[i] = AVD_RESULT_DONE;
			if( avd_active_count > 0 )
				avd_active_count--;
			any_completed = true;
			__android_log_print( ANDROID_LOG_INFO, "Xash",
				"AvatarDL: download complete for slot %d", i );
			{
				file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
				if( logf )
				{
					FS_Printf( logf, "[%.1f] DL_OK slot=%d id=%" PRIu64 "\n",
						host.realtime, i, avd_slot_id[i] );
					FS_Close( logf );
				}
			}
		}
		else if( avd_slot_result[i] == AVD_RESULT_FAIL )
		{
			avd_slot_result[i] = AVD_RESULT_IDLE;
			avd_slot_fail_time[i] = host.realtime;
			if( avd_active_count > 0 )
				avd_active_count--;
			__android_log_print( ANDROID_LOG_ERROR, "Xash",
				"AvatarDL: download failed for slot %d", i );
			{
				file_t *logf = FS_Open( "avatars/status_log.txt", "a", false );
				if( logf )
				{
					FS_Printf( logf, "[%.1f] DL_FAIL slot=%d id=%" PRIu64 "\n",
						host.realtime, i, avd_slot_id[i] );
					FS_Close( logf );
				}
			}
		}
	}

	return any_completed;
}

#else /* !XASH_ANDROID */
// ===========================================================================
// NON-ANDROID IMPLEMENTATION - HTTP sockets (original)
// ===========================================================================
#include "net_ws_private.h"

// ===========================================================================
// Configuration
// ===========================================================================

#define AVATAR_MAX_CONCURRENT  4       // max simultaneous active downloads
#define AVATAR_TIMEOUT         15.0    // seconds before giving up
#define AVATAR_RETRY_DELAY     60.0    // seconds before retrying a failed download
#define AVATAR_MAX_RESPONSE    65536   // max XML profile response (~20KB typical)
#define AVATAR_MAX_IMAGE       131072  // max avatar image (avatars are ~5KB)
#define AVATAR_MAX_REDIRECTS   3       // bail out after this many redirects
#define AVATAR_STEAM_HOST      "steamcommunity.com"
#define AVATAR_STEAM_PORT      80

// ===========================================================================
// Types
// ===========================================================================

typedef enum
{
	AVD_STATE_RESOLVE = 0,    // resolving DNS / starting connection
	AVD_STATE_CONNECT,        // socket connect() in progress
	AVD_STATE_SEND,           // sending HTTP request
	AVD_STATE_RECV_HEADER,    // receiving HTTP response headers
	AVD_STATE_RECV_BODY,      // receiving HTTP response body
	AVD_STATE_DONE,           // current phase completed (success or fail)
} avd_state_t;

typedef enum
{
	AVD_PHASE_XML = 0,        // fetching Steam profile XML
	AVD_PHASE_IMAGE,          // fetching avatar image
} avd_phase_t;

typedef struct avd_request_s
{
	struct avd_request_s *next;

	uint64_t steamid64;
	int      slot;             // player slot (0-based)

	avd_state_t state;
	avd_phase_t phase;
	int         redirect_count;

	int                     sock;     // socket fd (-1 when not connected)
	struct sockaddr_storage addr;

	// HTTP request buffer
	char request[768];
	int  request_len;
	int  bytes_sent;

	// HTTP response buffer (heap-allocated, response_cap+1 bytes for safe null term)
	byte    *response;
	int      response_len;
	int      response_cap;
	int      content_length;   // -1 if unknown
	qboolean got_header;
	int      header_end;       // offset where body starts in response[]

	// Current target host/port/path (mutable across redirects)
	char target_host[256];
	int  target_port;
	char target_path[512];

	double start_time;
} avd_request_t;

typedef enum
{
	AVD_SLOT_NONE = 0,    // never requested
	AVD_SLOT_QUEUED,      // waiting for an active slot to free up
	AVD_SLOT_ACTIVE,      // download in progress
	AVD_SLOT_DONE,        // downloaded successfully
	AVD_SLOT_FAILED,      // download failed
} avd_slot_state_t;

// ===========================================================================
// Static state
// ===========================================================================

static avd_request_t *avd_active;            // linked list of active downloads
static int            avd_active_count;

static avd_slot_state_t avd_slot_state[MAX_CLIENTS];
static uint64_t         avd_slot_id[MAX_CLIENTS];        // SteamID per slot (used for retry)
static double           avd_slot_fail_time[MAX_CLIENTS]; // when slot failed (for retry delay)

static qboolean avd_resolving;  // throttle: only one DNS resolve in flight per frame

static CVAR_DEFINE_AUTO( slayer_avatar_download, "1", FCVAR_ARCHIVE,
	"Slayer3D: auto-download Steam avatars (0=disabled)" );

// ===========================================================================
// Internal helpers
// ===========================================================================

static void AVD_CloseSocket( avd_request_t *req )
{
	if( req->sock != -1 )
	{
		closesocket( req->sock );
		req->sock = -1;
		avd_active_count--;
	}
}

static void AVD_FreeResponse( avd_request_t *req )
{
	if( req->response )
	{
		Mem_Free( req->response );
		req->response = NULL;
	}
	req->response_len = 0;
	req->response_cap = 0;
	req->got_header = false;
	req->header_end = 0;
	req->content_length = -1;
}

static void AVD_FreeRequest( avd_request_t *req )
{
	AVD_CloseSocket( req );
	AVD_FreeResponse( req );
}

static void AVD_RemoveRequest( avd_request_t *req, qboolean success )
{
	avd_request_t **prev = &avd_active;

	if( req->slot >= 0 && req->slot < MAX_CLIENTS )
	{
		if( success )
		{
			avd_slot_state[req->slot] = AVD_SLOT_DONE;
		}
		else
		{
			avd_slot_state[req->slot] = AVD_SLOT_FAILED;
			avd_slot_fail_time[req->slot] = host.realtime;
		}
	}

	while( *prev )
	{
		if( *prev == req )
		{
			*prev = req->next;
			break;
		}
		prev = &(*prev)->next;
	}

	AVD_FreeRequest( req );
	Mem_Free( req );
}

// Parse HTTP status code from "HTTP/1.x SSS ...\r\n". Returns -1 on parse error.
static int AVD_ParseStatusCode( const char *buf, int len )
{
	int code;

	if( len < 13 || Q_strncmp( buf, "HTTP/1.", 7 ) != 0 )
		return -1;

	// Format: HTTP/1.x SSS ...
	if( buf[8] != ' ' )
		return -1;

	code = ( buf[9] - '0' ) * 100 + ( buf[10] - '0' ) * 10 + ( buf[11] - '0' );
	if( code < 100 || code > 599 )
		return -1;

	return code;
}

// Find header value. Returns pointer to value (skipping leading spaces) or NULL.
static const char *AVD_FindHeader( const char *headers, const char *name )
{
	int   name_len = (int)Q_strlen( name );
	const char *p = headers;

	while( *p )
	{
		if( Q_strnicmp( p, name, name_len ) == 0 && p[name_len] == ':' )
		{
			p += name_len + 1;
			while( *p == ' ' || *p == '\t' )
				p++;
			return p;
		}

		// Advance to next line
		while( *p && *p != '\n' )
			p++;
		if( *p == '\n' )
			p++;
	}

	return NULL;
}

// Split URL "http(s)://host[:port]/path" into target_host/target_port/target_path.
// Returns true on success. Always uses port 80 (no TLS support).
static qboolean AVD_ParseURL( avd_request_t *req, const char *url, int url_len )
{
	const char *host_start, *host_end, *path_start;
	int host_len, path_len;

	if( url_len > 8 && Q_strncmp( url, "https://", 8 ) == 0 )
		host_start = url + 8;
	else if( url_len > 7 && Q_strncmp( url, "http://", 7 ) == 0 )
		host_start = url + 7;
	else
		return false;

	// Find end of host (':' for port, '/' for path, or end)
	path_start = host_start;
	while( path_start < url + url_len && *path_start != '/' && *path_start != ':' )
		path_start++;

	host_end = path_start;
	host_len = (int)( host_end - host_start );

	if( host_len <= 0 || host_len >= (int)sizeof( req->target_host ) )
		return false;

	memcpy( req->target_host, host_start, host_len );
	req->target_host[host_len] = '\0';

	// Skip optional port (we ignore it - always use HTTP port 80)
	if( path_start < url + url_len && *path_start == ':' )
	{
		path_start++;
		while( path_start < url + url_len && *path_start != '/' )
			path_start++;
	}

	// Path
	if( path_start >= url + url_len )
	{
		// No path: default to "/"
		req->target_path[0] = '/';
		req->target_path[1] = '\0';
	}
	else
	{
		path_len = (int)( ( url + url_len ) - path_start );
		if( path_len <= 0 || path_len >= (int)sizeof( req->target_path ) )
			return false;
		memcpy( req->target_path, path_start, path_len );
		req->target_path[path_len] = '\0';
	}

	req->target_port = 80;
	return true;
}

// ===========================================================================
// Phase setup
// ===========================================================================

static void AVD_SetupXMLPhase( avd_request_t *req )
{
	Q_strncpy( req->target_host, AVATAR_STEAM_HOST, sizeof( req->target_host ) );
	req->target_port = AVATAR_STEAM_PORT;
	Q_snprintf( req->target_path, sizeof( req->target_path ),
		"/profiles/%" PRIu64 "/?xml=1", req->steamid64 );

	req->phase = AVD_PHASE_XML;
	req->state = AVD_STATE_RESOLVE;
}

static void AVD_BuildHTTPRequest( avd_request_t *req )
{
	req->request_len = Q_snprintf( req->request, sizeof( req->request ),
		"GET %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"User-Agent: Mozilla/5.0\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n"
		"\r\n",
		req->target_path, req->target_host );
	req->bytes_sent = 0;
}

// ===========================================================================
// XML parsing - extract avatar URL from Steam profile response
// ===========================================================================

static qboolean AVD_ParseXMLForAvatar( avd_request_t *req )
{
	const char *body, *body_end;
	const char *tag_start, *tag_end;
	const char *url;
	int  url_len;

	if( !req->response || !req->got_header || req->response_len <= req->header_end )
		return false;

	body = (const char *)req->response + req->header_end;
	body_end = (const char *)req->response + req->response_len;

	// Try <avatarMedium> first, fall back to <avatarFull>
	tag_start = Q_strstr( body, "<avatarMedium>" );
	if( tag_start )
	{
		tag_start += 14;
		tag_end = Q_strstr( tag_start, "</avatarMedium>" );
	}
	else
	{
		tag_start = Q_strstr( body, "<avatarFull>" );
		if( !tag_start )
			return false;
		tag_start += 12;
		tag_end = Q_strstr( tag_start, "</avatarFull>" );
	}

	if( !tag_end || tag_end > body_end )
		return false;

	// Skip optional CDATA wrapper: <![CDATA[...]]>
	if( tag_end - tag_start > 9 && Q_strncmp( tag_start, "<![CDATA[", 9 ) == 0 )
	{
		tag_start += 9;
		tag_end = Q_strstr( tag_start, "]]>" );
		if( !tag_end || tag_end > body_end )
			return false;
	}

	url = tag_start;
	url_len = (int)( tag_end - tag_start );

	// Skip Steam's stock default avatar (the "?" silhouette). Any URL
	// containing this SHA1 fragment is the same generic placeholder served
	// for accounts that never set a profile picture. Downloading it would
	// waste bandwidth and display a misleading icon on the scoreboard.
	{
		// Bounded search within [url, url+url_len) for the default hash.
		static const char default_hash[] = "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb";
		int hash_len = (int)sizeof( default_hash ) - 1;
		int i;

		if( url_len >= hash_len )
		{
			for( i = 0; i <= url_len - hash_len; i++ )
			{
				if( memcmp( url + i, default_hash, hash_len ) == 0 )
				{
					Con_DPrintf( "AvatarDL: skipping default avatar for slot %d\n", req->slot );
					return false;
				}
			}
		}
	}

	if( !AVD_ParseURL( req, url, url_len ) )
		return false;

	Con_DPrintf( "AvatarDL: parsed image URL host=%s path=%s\n",
		req->target_host, req->target_path );
	return true;
}

// ===========================================================================
// Save image body to disk
// ===========================================================================

static qboolean AVD_SaveImage( avd_request_t *req )
{
	char file_path[128];
	const byte *image_data;
	int image_size;

	if( !req->response || !req->got_header )
		return false;

	image_data = req->response + req->header_end;
	image_size = req->response_len - req->header_end;

	if( image_size <= 0 )
		return false;

	Q_snprintf( file_path, sizeof( file_path ),
		"avatars/%" PRIu64 ".png", req->steamid64 );

	FS_AllowDirectPaths( true );
	if( !FS_WriteFile( file_path, image_data, image_size ) )
	{
		FS_AllowDirectPaths( false );
		Con_DPrintf( "AvatarDL: failed to write %s\n", file_path );
		return false;
	}
	FS_AllowDirectPaths( false );

	Con_DPrintf( "AvatarDL: saved %s (%d bytes)\n", file_path, image_size );
	return true;
}

// ===========================================================================
// Redirect handling
// ===========================================================================

static qboolean AVD_FollowRedirect( avd_request_t *req, const char *location, int loc_len )
{
	if( ++req->redirect_count > AVATAR_MAX_REDIRECTS )
	{
		Con_DPrintf( "AvatarDL: too many redirects for slot %d\n", req->slot );
		return false;
	}

	if( !AVD_ParseURL( req, location, loc_len ) )
	{
		Con_DPrintf( "AvatarDL: bad redirect URL for slot %d\n", req->slot );
		return false;
	}

	// Reset connection state but keep phase
	AVD_CloseSocket( req );
	AVD_FreeResponse( req );
	req->state = AVD_STATE_RESOLVE;

	Con_DPrintf( "AvatarDL: redirect to %s%s\n", req->target_host, req->target_path );
	return true;
}

// ===========================================================================
// State machine - per-frame processing of one request
// ===========================================================================

static void AVD_StateResolve( avd_request_t *req )
{
	net_gai_state_t res;

	// Throttle: only one DNS resolve in flight at a time
	if( avd_resolving )
		return;

	memset( &req->addr, 0, sizeof( req->addr ) );
	res = NET_StringToSockaddr( req->target_host, &req->addr, true, AF_INET );

	if( res == NET_EAI_AGAIN )
	{
		avd_resolving = true;
		return;
	}

	if( res == NET_EAI_NONAME )
	{
		Con_DPrintf( "AvatarDL: DNS failed for %s\n", req->target_host );
		req->state = AVD_STATE_DONE;
		return;
	}

	// Set port (the engine convention)
	((struct sockaddr_in *)&req->addr)->sin_port = htons( (unsigned short)req->target_port );

	// Create non-blocking socket
	req->sock = socket( req->addr.ss_family, SOCK_STREAM, IPPROTO_TCP );
	if( req->sock < 0 )
	{
		Con_DPrintf( "AvatarDL: socket() failed: %s\n", NET_ErrorString() );
		req->state = AVD_STATE_DONE;
		return;
	}

	if( !NET_MakeSocketNonBlocking( req->sock ) )
	{
		Con_DPrintf( "AvatarDL: failed to set non-blocking\n" );
		closesocket( req->sock );
		req->sock = -1;
		req->state = AVD_STATE_DONE;
		return;
	}

	avd_active_count++;
	req->state = AVD_STATE_CONNECT;
}

static void AVD_StateConnect( avd_request_t *req )
{
	int res = connect( req->sock, (struct sockaddr *)&req->addr,
		NET_SockAddrLen( &req->addr ) );

	if( res < 0 )
	{
		int err = WSAGetLastError();

		if( err == WSAEISCONN )
		{
			// Already connected - fall through
		}
		else if( err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY )
		{
			return; // still connecting, retry next frame
		}
		else
		{
			Con_DPrintf( "AvatarDL: connect to %s failed: %s\n",
				req->target_host, NET_ErrorString() );
			req->state = AVD_STATE_DONE;
			return;
		}
	}

	AVD_BuildHTTPRequest( req );
	req->state = AVD_STATE_SEND;
}

static void AVD_StateSend( avd_request_t *req )
{
	int res = send( req->sock, req->request + req->bytes_sent,
		req->request_len - req->bytes_sent, 0 );

	if( res > 0 )
	{
		req->bytes_sent += res;

		if( req->bytes_sent >= req->request_len )
		{
			// Request fully sent - prepare response buffer (+1 for safe null term)
			int cap = ( req->phase == AVD_PHASE_XML ) ? AVATAR_MAX_RESPONSE : AVATAR_MAX_IMAGE;
			req->response = Mem_Malloc( host.mempool, cap + 1 );
			req->response_cap = cap;
			req->response_len = 0;
			req->got_header = false;
			req->header_end = 0;
			req->content_length = -1;
			req->state = AVD_STATE_RECV_HEADER;
		}
		return;
	}

	if( res < 0 )
	{
		int err = WSAGetLastError();
		if( err != WSAEWOULDBLOCK && err != WSAEINPROGRESS )
		{
			Con_DPrintf( "AvatarDL: send failed: %s\n", NET_ErrorString() );
			req->state = AVD_STATE_DONE;
		}
	}
}

// Returns true if request changed (e.g. redirect followed) - caller should not advance.
static qboolean AVD_HandleHeaderComplete( avd_request_t *req )
{
	int  status;
	const char *cl_hdr;

	// Null-terminate at end of received data (safe: we allocated cap+1)
	req->response[req->response_len] = '\0';

	status = AVD_ParseStatusCode( (char *)req->response, req->response_len );

	// Handle redirects
	if( status == 301 || status == 302 || status == 303 || status == 307 || status == 308 )
	{
		const char *loc = AVD_FindHeader( (char *)req->response, "Location" );
		if( loc )
		{
			const char *loc_end = Q_strstr( loc, "\r\n" );
			if( !loc_end )
				loc_end = Q_strstr( loc, "\n" );

			if( loc_end )
			{
				int loc_len = (int)( loc_end - loc );
				if( AVD_FollowRedirect( req, loc, loc_len ) )
					return true; // request restarted
			}
		}

		Con_DPrintf( "AvatarDL: redirect without Location header\n" );
		req->state = AVD_STATE_DONE;
		return true;
	}

	if( status != 200 )
	{
		Con_DPrintf( "AvatarDL: HTTP %d for %" PRIu64 "\n",
			status < 0 ? 0 : status, req->steamid64 );
		req->state = AVD_STATE_DONE;
		return true;
	}

	// Parse Content-Length (only meaningful for HTTP/1.0 with Connection: close)
	cl_hdr = AVD_FindHeader( (char *)req->response, "Content-Length" );
	if( cl_hdr )
	{
		req->content_length = Q_atoi( cl_hdr );

		// Reject oversized payload up front
		if( req->content_length > req->response_cap - req->header_end )
		{
			Con_DPrintf( "AvatarDL: payload too large (%d bytes) for slot %d\n",
				req->content_length, req->slot );
			req->state = AVD_STATE_DONE;
			return true;
		}
	}

	req->state = AVD_STATE_RECV_BODY;
	return false;
}

static void AVD_StateRecv( avd_request_t *req )
{
	byte tmpbuf[4096];
	int  available, res;

	available = req->response_cap - req->response_len;
	if( available <= 0 )
	{
		// Buffer full - finalize what we have
		req->state = AVD_STATE_DONE;
		return;
	}

	res = recv( req->sock, (char *)tmpbuf, Q_min( (int)sizeof( tmpbuf ), available ), 0 );

	if( res > 0 )
	{
		memcpy( req->response + req->response_len, tmpbuf, res );
		req->response_len += res;

		// Header detection (only if not yet found)
		if( !req->got_header )
		{
			char *hdr_end;

			req->response[req->response_len] = '\0';
			hdr_end = Q_strstr( (char *)req->response, "\r\n\r\n" );

			if( hdr_end )
			{
				req->got_header = true;
				req->header_end = (int)( hdr_end - (char *)req->response ) + 4;

				if( AVD_HandleHeaderComplete( req ) )
					return; // redirect or error
			}
		}

		// Body completion check
		if( req->got_header && req->content_length > 0 )
		{
			int body_received = req->response_len - req->header_end;
			if( body_received >= req->content_length )
				req->state = AVD_STATE_DONE;
		}
		return;
	}

	if( res == 0 )
	{
		// Remote closed connection - end of body
		req->state = AVD_STATE_DONE;
		return;
	}

	// res < 0
	{
		int err = WSAGetLastError();
		if( err != WSAEWOULDBLOCK && err != WSAEINPROGRESS )
		{
			Con_DPrintf( "AvatarDL: recv error: %s\n", NET_ErrorString() );
			req->state = AVD_STATE_DONE;
		}
	}
}

static void AVD_ProcessRequest( avd_request_t *req )
{
	// Run multiple states per frame when the next state can proceed without blocking
	int max_iters = 4;

	while( max_iters-- > 0 )
	{
		avd_state_t prev = req->state;

		switch( req->state )
		{
		case AVD_STATE_RESOLVE:     AVD_StateResolve( req ); break;
		case AVD_STATE_CONNECT:     AVD_StateConnect( req ); break;
		case AVD_STATE_SEND:        AVD_StateSend( req );    break;
		case AVD_STATE_RECV_HEADER:
		case AVD_STATE_RECV_BODY:   AVD_StateRecv( req );    break;
		case AVD_STATE_DONE:        return;
		}

		// Stop if state didn't advance (would block)
		if( req->state == prev )
			return;
	}
}

// ===========================================================================
// Phase completion / pending queue
// ===========================================================================

static qboolean AVD_OnPhaseDone( avd_request_t *req )
{
	if( req->phase == AVD_PHASE_XML )
	{
		if( !AVD_ParseXMLForAvatar( req ) )
		{
			Con_DPrintf( "AvatarDL: XML parse failed for slot %d\n", req->slot );
			AVD_RemoveRequest( req, false );
			return false;
		}

		// Transition to image phase
		AVD_CloseSocket( req );
		AVD_FreeResponse( req );
		req->phase = AVD_PHASE_IMAGE;
		req->state = AVD_STATE_RESOLVE;
		req->redirect_count = 0;
		req->start_time = host.realtime; // reset timeout for phase 2
		return false;
	}

	// Phase IMAGE done
	if( !AVD_SaveImage( req ) )
	{
		AVD_RemoveRequest( req, false );
		return false;
	}

	Con_DPrintf( "AvatarDL: download complete for slot %d\n", req->slot );
	AVD_RemoveRequest( req, true );
	return true;
}

static qboolean AVD_TryQueuePending( int slot )
{
	avd_request_t *req;

	if( avd_active_count >= AVATAR_MAX_CONCURRENT )
		return false;

	if( slot < 0 || slot >= MAX_CLIENTS )
		return false;

	if( avd_slot_state[slot] != AVD_SLOT_QUEUED )
		return false;

	if( avd_slot_id[slot] == 0 )
	{
		avd_slot_state[slot] = AVD_SLOT_NONE;
		return false;
	}

	req = Mem_Calloc( host.mempool, sizeof( *req ) );
	req->steamid64 = avd_slot_id[slot];
	req->slot = slot;
	req->sock = -1;
	req->start_time = host.realtime;
	req->content_length = -1;
	AVD_SetupXMLPhase( req );

	req->next = avd_active;
	avd_active = req;

	avd_slot_state[slot] = AVD_SLOT_ACTIVE;

	Con_DPrintf( "AvatarDL: starting download for slot %d (%" PRIu64 ")\n",
		slot, req->steamid64 );
	return true;
}

// ===========================================================================
// Public API
// ===========================================================================

void Slayer_AvatarDownload_Init( void )
{
	Cvar_RegisterVariable( &slayer_avatar_download );
	avd_active = NULL;
	avd_active_count = 0;
	memset( avd_slot_state, 0, sizeof( avd_slot_state ) );
	memset( avd_slot_id, 0, sizeof( avd_slot_id ) );
	memset( avd_slot_fail_time, 0, sizeof( avd_slot_fail_time ) );
	Con_Printf( "Slayer3D: avatar HTTP downloader initialized\n" );
}

void Slayer_AvatarDownload_Shutdown( void )
{
	avd_request_t *req, *next;

	for( req = avd_active; req; req = next )
	{
		next = req->next;
		AVD_FreeRequest( req );
		Mem_Free( req );
	}

	avd_active = NULL;
	avd_active_count = 0;
}

void Slayer_AvatarDownload_Reset( void )
{
	Slayer_AvatarDownload_Shutdown();
	memset( avd_slot_state, 0, sizeof( avd_slot_state ) );
	memset( avd_slot_id, 0, sizeof( avd_slot_id ) );
	memset( avd_slot_fail_time, 0, sizeof( avd_slot_fail_time ) );
}

void Slayer_AvatarDownload_Request( uint64_t steamid64, int slot )
{
	char path[128];

	if( slayer_avatar_download.value == 0.0f )
		return;

	if( slot < 0 || slot >= MAX_CLIENTS || steamid64 == 0 )
		return;

	// Already done or in progress?
	if( avd_slot_state[slot] == AVD_SLOT_DONE ||
	    avd_slot_state[slot] == AVD_SLOT_ACTIVE ||
	    avd_slot_state[slot] == AVD_SLOT_QUEUED )
	{
		return;
	}

	// Recently failed - wait for retry delay
	if( avd_slot_state[slot] == AVD_SLOT_FAILED &&
	    host.realtime - avd_slot_fail_time[slot] < AVATAR_RETRY_DELAY )
	{
		return;
	}

	// Already cached on disk?
	Q_snprintf( path, sizeof( path ), "avatars/%" PRIu64 ".png", steamid64 );
	if( FS_FileExists( path, false ) )
	{
		avd_slot_state[slot] = AVD_SLOT_DONE;
		return;
	}

	// Mark as queued. Frame() will promote to active when capacity is available.
	avd_slot_id[slot] = steamid64;
	avd_slot_state[slot] = AVD_SLOT_QUEUED;
	AVD_TryQueuePending( slot );
}

qboolean Slayer_AvatarDownload_Frame( void )
{
	avd_request_t *req, *next;
	qboolean       any_completed = false;
	int            i;

	if( slayer_avatar_download.value == 0.0f )
		return false;

	avd_resolving = false; // reset DNS throttle each frame

	// Pump active requests
	for( req = avd_active; req; req = next )
	{
		next = req->next;

		// Timeout check
		if( host.realtime - req->start_time > AVATAR_TIMEOUT )
		{
			Con_DPrintf( "AvatarDL: timeout for slot %d (phase %d)\n",
				req->slot, (int)req->phase );
			AVD_RemoveRequest( req, false );
			continue;
		}

		AVD_ProcessRequest( req );

		if( req->state == AVD_STATE_DONE )
		{
			if( AVD_OnPhaseDone( req ) )
				any_completed = true;
		}
	}

	// Promote queued slots to active when capacity is available
	for( i = 0; i < MAX_CLIENTS && avd_active_count < AVATAR_MAX_CONCURRENT; i++ )
	{
		if( avd_slot_state[i] == AVD_SLOT_QUEUED )
			AVD_TryQueuePending( i );
	}

	return any_completed;
}

#endif /* XASH_ANDROID */
