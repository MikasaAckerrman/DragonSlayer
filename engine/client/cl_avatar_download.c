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

Async two-phase avatar download:
  Phase 1: GET /profiles/<steamid64>/?xml=1 from steamcommunity.com
           Parse <avatarMedium> tag to get image URL
  Phase 2: GET image from the parsed URL (akamai CDN)
           Save to avatars/<steamid64>.png
*/

#include <inttypes.h>
#include "common.h"
#include "client.h"
#include "net_ws_private.h"
#include "cl_avatar_download.h"

// ===========================================================================
// Configuration
// ===========================================================================

#define AVATAR_MAX_CONCURRENT    2       // max simultaneous downloads
#define AVATAR_TIMEOUT           15.0    // seconds before giving up
#define AVATAR_RETRY_DELAY       60.0    // seconds before retrying a failed download
#define AVATAR_MAX_RESPONSE      65536   // max HTTP response buffer (XML profile ~20KB)
#define AVATAR_MAX_IMAGE         131072  // max image size (128KB, avatars are ~5KB)
#define AVATAR_STEAM_HOST        "steamcommunity.com"
#define AVATAR_STEAM_PORT        80

// ===========================================================================
// Types
// ===========================================================================

typedef enum
{
	AVD_STATE_IDLE = 0,       // not downloading
	AVD_STATE_RESOLVE,        // resolving DNS
	AVD_STATE_CONNECT,        // connecting socket
	AVD_STATE_SEND,           // sending HTTP request
	AVD_STATE_RECV_HEADER,    // receiving HTTP response headers
	AVD_STATE_RECV_BODY,      // receiving HTTP response body
	AVD_STATE_DONE,           // completed (success or fail)
} avd_state_t;

typedef enum
{
	AVD_PHASE_XML = 0,        // fetching Steam profile XML
	AVD_PHASE_IMAGE,          // fetching avatar image
} avd_phase_t;

typedef struct avd_request_s
{
	struct avd_request_s *next;

	uint64_t  steamid64;
	int       slot;            // player slot (0-based)

	avd_state_t state;
	avd_phase_t phase;

	int       sock;            // socket fd (-1 when not connected)
	struct sockaddr_storage addr;

	// HTTP request/response buffers
	char      request[512];    // outgoing HTTP request
	int       request_len;
	int       bytes_sent;

	byte     *response;        // dynamically allocated response buffer
	int       response_len;    // current received length
	int       response_cap;    // allocated capacity
	int       content_length;  // from Content-Length header (-1 if unknown)
	qboolean  got_header;      // true once \r\n\r\n received
	int       header_end;      // offset where body starts

	// Image URL parsed from XML
	char      image_host[256];
	int       image_port;
	char      image_path[512];

	// Timing
	double    start_time;
	double    block_time;      // time spent blocked (for timeout)
} avd_request_t;

// ===========================================================================
// Static state
// ===========================================================================

static avd_request_t *avd_active;       // linked list of active downloads
static int            avd_active_count;

// Per-slot tracking: prevent duplicate requests
typedef enum
{
	AVD_SLOT_NONE = 0,     // never requested
	AVD_SLOT_PENDING,      // download in progress
	AVD_SLOT_DONE,         // downloaded successfully
	AVD_SLOT_FAILED,       // download failed
} avd_slot_state_t;

static avd_slot_state_t avd_slot_state[MAX_CLIENTS];
static double           avd_slot_fail_time[MAX_CLIENTS]; // when it failed (for retry)

static CVAR_DEFINE_AUTO( slayer_avatar_download, "1", FCVAR_ARCHIVE, "Slayer3D: auto-download Steam avatars (0=disabled)" );

// ===========================================================================
// Internal helpers
// ===========================================================================

static void AVD_FreeRequest( avd_request_t *req )
{
	if( req->sock != -1 )
	{
		closesocket( req->sock );
		req->sock = -1;
		avd_active_count--;
	}

	if( req->response )
	{
		Mem_Free( req->response );
		req->response = NULL;
	}
}

static void AVD_RemoveRequest( avd_request_t *req, qboolean success )
{
	avd_request_t **prev = &avd_active;

	// Update slot state
	if( req->slot >= 0 && req->slot < MAX_CLIENTS )
	{
		if( success )
			avd_slot_state[req->slot] = AVD_SLOT_DONE;
		else
		{
			avd_slot_state[req->slot] = AVD_SLOT_FAILED;
			avd_slot_fail_time[req->slot] = host.realtime;
		}
	}

	// Unlink from list
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

static qboolean AVD_CreateSocket( avd_request_t *req )
{
	req->sock = socket( req->addr.ss_family, SOCK_STREAM, IPPROTO_TCP );

	if( req->sock < 0 )
	{
		Con_DPrintf( "AvatarDL: socket() failed: %s\n", NET_ErrorString() );
		return false;
	}

	if( !NET_MakeSocketNonBlocking( req->sock ) )
	{
		Con_DPrintf( "AvatarDL: failed to set non-blocking\n" );
		closesocket( req->sock );
		req->sock = -1;
		return false;
	}

	avd_active_count++;
	return true;
}

static qboolean AVD_StartConnect( avd_request_t *req, const char *hostname, int port )
{
	net_gai_state_t res;

	memset( &req->addr, 0, sizeof( req->addr ) );
	res = NET_StringToSockaddr( hostname, &req->addr, false, AF_INET );

	if( res == NET_EAI_NONAME )
	{
		Con_DPrintf( "AvatarDL: DNS failed for %s\n", hostname );
		return false;
	}

	// Set port
	((struct sockaddr_in *)&req->addr)->sin_port = htons( (unsigned short)port );

	if( !AVD_CreateSocket( req ) )
		return false;

	req->state = AVD_STATE_CONNECT;
	req->block_time = 0;
	return true;
}

static void AVD_BuildXMLRequest( avd_request_t *req )
{
	req->request_len = Q_snprintf( req->request, sizeof( req->request ),
		"GET /profiles/%" PRIu64 "/?xml=1 HTTP/1.0\r\n"
		"Host: " AVATAR_STEAM_HOST "\r\n"
		"Connection: close\r\n"
		"Accept: */*\r\n"
		"\r\n",
		req->steamid64 );
	req->bytes_sent = 0;
}

static void AVD_BuildImageRequest( avd_request_t *req )
{
	req->request_len = Q_snprintf( req->request, sizeof( req->request ),
		"GET %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"Connection: close\r\n"
		"Accept: */*\r\n"
		"\r\n",
		req->image_path, req->image_host );
	req->bytes_sent = 0;
}

// Parse <avatarMedium> URL from Steam XML profile response
static qboolean AVD_ParseXMLForAvatar( avd_request_t *req )
{
	const char *body;
	const char *tag_start, *tag_end;
	const char *url;
	int url_len;
	const char *host_start, *host_end, *path_start;
	int host_len;

	if( !req->response || req->response_len <= req->header_end )
		return false;

	body = (const char *)req->response + req->header_end;

	// Find <avatarMedium> tag
	tag_start = Q_strstr( body, "<avatarMedium>" );
	if( !tag_start )
	{
		// Try <avatarFull> as fallback
		tag_start = Q_strstr( body, "<avatarFull>" );
		if( !tag_start )
			return false;
		tag_start += 12; // strlen("<avatarFull>")
		tag_end = Q_strstr( tag_start, "</avatarFull>" );
	}
	else
	{
		tag_start += 14; // strlen("<avatarMedium>")
		tag_end = Q_strstr( tag_start, "</avatarMedium>" );
	}

	if( !tag_end )
		return false;

	// Skip CDATA wrapper if present: <![CDATA[...]]>
	if( !Q_strncmp( tag_start, "<![CDATA[", 9 ) )
	{
		tag_start += 9;
		tag_end = Q_strstr( tag_start, "]]>" );
		if( !tag_end )
			return false;
	}

	url = tag_start;
	url_len = (int)( tag_end - tag_start );

	if( url_len <= 0 || url_len >= (int)sizeof( req->image_path ) + 256 )
		return false;

	// Parse the URL: expect https://avatars.steamstatic.com/path or similar
	// We use HTTP (port 80) since we don't have TLS
	// Actually Steam avatar CDN URLs are https - we need to handle this
	// Let's try to extract host and path

	// Skip http:// or https://
	if( !Q_strncmp( url, "https://", 8 ) )
		host_start = url + 8;
	else if( !Q_strncmp( url, "http://", 7 ) )
		host_start = url + 7;
	else
		return false;

	// Find end of host (first / after protocol)
	path_start = host_start;
	while( path_start < tag_end && *path_start != '/' )
		path_start++;

	host_end = path_start;
	host_len = (int)( host_end - host_start );

	if( host_len <= 0 || host_len >= (int)sizeof( req->image_host ) )
		return false;

	memcpy( req->image_host, host_start, host_len );
	req->image_host[host_len] = '\0';

	// Copy path
	{
		int path_len = (int)( tag_end - path_start );
		if( path_len <= 0 || path_len >= (int)sizeof( req->image_path ) )
			return false;
		memcpy( req->image_path, path_start, path_len );
		req->image_path[path_len] = '\0';
	}

	// Steam CDN uses HTTPS but many CDN hosts also respond to HTTP on port 80
	// For simplicity we use port 80 (no TLS in engine)
	req->image_port = 80;

	Con_DPrintf( "AvatarDL: parsed image URL -> host=%s path=%s\n", req->image_host, req->image_path );
	return true;
}

// Save image body to disk
static qboolean AVD_SaveImage( avd_request_t *req )
{
	char dir_path[64];
	char file_path[128];
	const byte *image_data;
	int image_size;

	if( !req->response || req->response_len <= req->header_end )
		return false;

	image_data = req->response + req->header_end;
	image_size = req->response_len - req->header_end;

	if( image_size <= 0 )
		return false;

	Q_snprintf( dir_path, sizeof( dir_path ), "avatars" );
	Q_snprintf( file_path, sizeof( file_path ), "avatars/%" PRIu64 ".png", req->steamid64 );

	// Create directory and write file
	FS_AllowDirectPaths( true );

	// Use WriteFile which creates parent dirs
	if( !FS_WriteFile( file_path, image_data, image_size ) )
	{
		// Try creating directory manually
		FS_AllowDirectPaths( false );
		Con_DPrintf( "AvatarDL: failed to write %s\n", file_path );
		return false;
	}

	FS_AllowDirectPaths( false );

	Con_DPrintf( "AvatarDL: saved avatar to %s (%d bytes)\n", file_path, image_size );
	return true;
}

// ===========================================================================
// State machine per-frame processing
// ===========================================================================

static void AVD_ProcessRequest( avd_request_t *req )
{
	int res;

	switch( req->state )
	{
	case AVD_STATE_IDLE:
	case AVD_STATE_DONE:
		return;

	case AVD_STATE_RESOLVE:
	{
		// Start connection to appropriate host
		const char *hostname;
		int port;

		if( req->phase == AVD_PHASE_XML )
		{
			hostname = AVATAR_STEAM_HOST;
			port = AVATAR_STEAM_PORT;
		}
		else
		{
			hostname = req->image_host;
			port = req->image_port;
		}

		if( !AVD_StartConnect( req, hostname, port ) )
		{
			req->state = AVD_STATE_DONE;
			return;
		}
		// Fall through to CONNECT state processing next frame
		return;
	}

	case AVD_STATE_CONNECT:
	{
		res = connect( req->sock, (struct sockaddr *)&req->addr, NET_SockAddrLen( &req->addr ) );

		if( res < 0 )
		{
			int err = WSAGetLastError();

			if( err == WSAEISCONN )
			{
				// Connected!
			}
			else if( err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY )
			{
				req->block_time += host.frametime;
				return;
			}
			else
			{
				Con_DPrintf( "AvatarDL: connect failed: %s\n", NET_ErrorString() );
				req->state = AVD_STATE_DONE;
				return;
			}
		}

		// Connected - build and send request
		req->block_time = 0;

		if( req->phase == AVD_PHASE_XML )
			AVD_BuildXMLRequest( req );
		else
			AVD_BuildImageRequest( req );

		req->state = AVD_STATE_SEND;
		return;
	}

	case AVD_STATE_SEND:
	{
		res = send( req->sock, req->request + req->bytes_sent,
			req->request_len - req->bytes_sent, 0 );

		if( res > 0 )
		{
			req->bytes_sent += res;
			req->block_time = 0;

			if( req->bytes_sent >= req->request_len )
			{
				// Request sent, prepare for response
				int cap = ( req->phase == AVD_PHASE_XML ) ? AVATAR_MAX_RESPONSE : AVATAR_MAX_IMAGE;
				req->response = Mem_Calloc( host.mempool, cap );
				req->response_cap = cap;
				req->response_len = 0;
				req->got_header = false;
				req->header_end = 0;
				req->content_length = -1;
				req->state = AVD_STATE_RECV_HEADER;
			}
		}
		else if( res < 0 )
		{
			int err = WSAGetLastError();
			if( err != WSAEWOULDBLOCK )
			{
				Con_DPrintf( "AvatarDL: send failed: %s\n", NET_ErrorString() );
				req->state = AVD_STATE_DONE;
				return;
			}
			req->block_time += host.frametime;
		}
		return;
	}

	case AVD_STATE_RECV_HEADER:
	case AVD_STATE_RECV_BODY:
	{
		byte tmpbuf[4096];
		int available = req->response_cap - req->response_len;

		if( available <= 0 )
		{
			// Buffer full
			if( !req->got_header )
			{
				Con_DPrintf( "AvatarDL: header too large\n" );
				req->state = AVD_STATE_DONE;
				return;
			}
			// For body, we have enough
			req->state = AVD_STATE_DONE;
			return;
		}

		res = recv( req->sock, (char *)tmpbuf, Q_min( (int)sizeof( tmpbuf ), available ), 0 );

		if( res > 0 )
		{
			memcpy( req->response + req->response_len, tmpbuf, res );
			req->response_len += res;
			req->block_time = 0;

			// Check for header end if we haven't found it yet
			if( !req->got_header )
			{
				// Null-terminate for strstr
				req->response[req->response_len] = '\0';
				char *hdr_end = Q_strstr( (char *)req->response, "\r\n\r\n" );

				if( hdr_end )
				{
					req->got_header = true;
					req->header_end = (int)( hdr_end - (char *)req->response ) + 4;

					// Check for HTTP 200 OK
					if( !Q_strstr( (char *)req->response, "200" ) )
					{
						// Check for redirect (301/302)
						if( Q_strstr( (char *)req->response, "301" ) || Q_strstr( (char *)req->response, "302" ) )
						{
							// Parse Location header for redirect
							char *loc = Q_stristr( (char *)req->response, "Location:" );
							if( loc && req->phase == AVD_PHASE_IMAGE )
							{
								// Extract redirect URL
								loc += 9;
								while( *loc == ' ' ) loc++;

								char *loc_end = Q_strstr( loc, "\r\n" );
								if( loc_end )
								{
									int loc_len = (int)( loc_end - loc );
									char redirect_url[512];

									if( loc_len > 0 && loc_len < (int)sizeof( redirect_url ) )
									{
										memcpy( redirect_url, loc, loc_len );
										redirect_url[loc_len] = '\0';

										// Parse new URL
										const char *new_host;
										if( !Q_strncmp( redirect_url, "https://", 8 ) )
											new_host = redirect_url + 8;
										else if( !Q_strncmp( redirect_url, "http://", 7 ) )
											new_host = redirect_url + 7;
										else
										{
											req->state = AVD_STATE_DONE;
											return;
										}

										// Split host/path
										const char *new_path = new_host;
										while( *new_path && *new_path != '/' )
											new_path++;

										int new_host_len = (int)( new_path - new_host );
										if( new_host_len > 0 && new_host_len < (int)sizeof( req->image_host ) )
										{
											memcpy( req->image_host, new_host, new_host_len );
											req->image_host[new_host_len] = '\0';
											Q_strncpy( req->image_path, new_path, sizeof( req->image_path ) );
											req->image_port = 80;

											// Restart connection for redirect
											closesocket( req->sock );
											req->sock = -1;
											avd_active_count--;

											Mem_Free( req->response );
											req->response = NULL;
											req->response_len = 0;

											req->state = AVD_STATE_RESOLVE;
											Con_DPrintf( "AvatarDL: following redirect to %s%s\n", req->image_host, req->image_path );
											return;
										}
									}
								}
							}
						}

						Con_DPrintf( "AvatarDL: HTTP error (not 200) for %" PRIu64 "\n", req->steamid64 );
						req->state = AVD_STATE_DONE;
						return;
					}

					// Parse Content-Length
					{
						char *cl_hdr = Q_stristr( (char *)req->response, "Content-Length:" );
						if( cl_hdr )
						{
							cl_hdr += 15;
							while( *cl_hdr == ' ' ) cl_hdr++;
							req->content_length = Q_atoi( cl_hdr );
						}
					}

					req->state = AVD_STATE_RECV_BODY;
				}
			}

			// Check if we've received the full body
			if( req->got_header && req->content_length > 0 )
			{
				int body_received = req->response_len - req->header_end;
				if( body_received >= req->content_length )
				{
					req->state = AVD_STATE_DONE;
					return;
				}
			}
		}
		else if( res == 0 )
		{
			// Connection closed by remote - we have all data
			req->state = AVD_STATE_DONE;
			return;
		}
		else
		{
			int err = WSAGetLastError();
			if( err != WSAEWOULDBLOCK && err != WSAEINPROGRESS )
			{
				Con_DPrintf( "AvatarDL: recv error: %s\n", NET_ErrorString() );
				req->state = AVD_STATE_DONE;
				return;
			}
			req->block_time += host.frametime;
		}
		return;
	}

	default:
		break;
	}
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
	memset( avd_slot_fail_time, 0, sizeof( avd_slot_fail_time ) );
}

void Slayer_AvatarDownload_Request( uint64_t steamid64, int slot )
{
	avd_request_t *req;
	char path[128];

	if( slayer_avatar_download.value == 0.0f )
		return;

	if( slot < 0 || slot >= MAX_CLIENTS )
		return;

	if( steamid64 == 0 )
		return;

	// Already handled?
	if( avd_slot_state[slot] == AVD_SLOT_DONE )
		return;

	if( avd_slot_state[slot] == AVD_SLOT_PENDING )
		return;

	// Failed recently? Wait for retry delay
	if( avd_slot_state[slot] == AVD_SLOT_FAILED )
	{
		if( host.realtime - avd_slot_fail_time[slot] < AVATAR_RETRY_DELAY )
			return;
	}

	// Already have the file on disk?
	Q_snprintf( path, sizeof( path ), "avatars/%" PRIu64 ".png", steamid64 );
	if( FS_FileExists( path, false ) )
	{
		avd_slot_state[slot] = AVD_SLOT_DONE;
		return;
	}

	// Too many concurrent downloads?
	if( avd_active_count >= AVATAR_MAX_CONCURRENT )
		return;

	// Create new request
	req = Mem_Calloc( host.mempool, sizeof( *req ) );
	req->steamid64 = steamid64;
	req->slot = slot;
	req->sock = -1;
	req->phase = AVD_PHASE_XML;
	req->state = AVD_STATE_RESOLVE;
	req->start_time = host.realtime;
	req->block_time = 0;
	req->response = NULL;

	// Add to active list
	req->next = avd_active;
	avd_active = req;

	avd_slot_state[slot] = AVD_SLOT_PENDING;

	Con_DPrintf( "AvatarDL: queued download for slot %d (%" PRIu64 ")\n", slot, steamid64 );
}

qboolean Slayer_AvatarDownload_Frame( void )
{
	avd_request_t *req, *next;
	qboolean any_completed = false;

	if( slayer_avatar_download.value == 0.0f )
		return false;

	for( req = avd_active; req; req = next )
	{
		next = req->next; // save next pointer since req might be freed

		// Timeout check
		if( host.realtime - req->start_time > AVATAR_TIMEOUT )
		{
			Con_DPrintf( "AvatarDL: timeout for slot %d\n", req->slot );
			AVD_RemoveRequest( req, false );
			continue;
		}

		// Process state machine
		AVD_ProcessRequest( req );

		// Check if done
		if( req->state == AVD_STATE_DONE )
		{
			if( req->phase == AVD_PHASE_XML )
			{
				// Parse XML to extract avatar URL
				if( req->got_header && AVD_ParseXMLForAvatar( req ) )
				{
					// Close current connection, start phase 2
					closesocket( req->sock );
					req->sock = -1;
					avd_active_count--;

					Mem_Free( req->response );
					req->response = NULL;
					req->response_len = 0;

					req->phase = AVD_PHASE_IMAGE;
					req->state = AVD_STATE_RESOLVE;
					req->start_time = host.realtime; // reset timeout for phase 2
					req->block_time = 0;

					Con_DPrintf( "AvatarDL: XML parsed, starting image download for slot %d\n", req->slot );
				}
				else
				{
					Con_DPrintf( "AvatarDL: XML parse failed for slot %d\n", req->slot );
					AVD_RemoveRequest( req, false );
				}
			}
			else // AVD_PHASE_IMAGE
			{
				// Save image to disk
				if( req->got_header && AVD_SaveImage( req ) )
				{
					Con_DPrintf( "AvatarDL: download complete for slot %d\n", req->slot );
					AVD_RemoveRequest( req, true );
					any_completed = true;
				}
				else
				{
					Con_DPrintf( "AvatarDL: image save failed for slot %d\n", req->slot );
					AVD_RemoveRequest( req, false );
				}
			}
		}
	}

	return any_completed;
}
