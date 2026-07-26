/*
cl_slayer_log.c - Slayer3D file-based diagnostic logger
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

// A tiny append-only diagnostic log the user can pull off the device without
// adb: it lands at <gamedir>/logs/slayer_diag.log. Used to trace the Steam
// avatar pipeline (status parse -> SteamID -> download -> texture) so failures
// can be diagnosed from the file instead of guessed at.

#include <stdarg.h>
#include "common.h"
#include "client.h"
#include "cl_slayer_log.h"

#if XASH_ANDROID
#include <android/log.h>
#endif

// On by default while the avatar pipeline is being brought up; set to 0 to stop.
static CVAR_DEFINE_AUTO( slayer_log, "1", FCVAR_ARCHIVE,
	"Slayer3D: write diagnostics to <gamedir>/logs/slayer_diag.log (0 = off)" );

static file_t  *slog_file;
static qboolean slog_open_failed;

void Slayer_Log_Init( void )
{
	Cvar_RegisterVariable( &slayer_log );
}

void Slayer_Log_Printf( const char *fmt, ... )
{
	va_list args;
	char    msg[1024];

	if( slayer_log.value == 0.0f )
		return;

	va_start( args, fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, args );
	va_end( args );

	// Mirror to logcat so `adb logcat -s Xash` shows the same trail.
#if XASH_ANDROID
	__android_log_print( ANDROID_LOG_INFO, "Xash", "SLOG %s", msg );
#endif

	// Open lazily (append) so the log survives map changes within a session.
	// The engine's write path creates the logs/ directory as needed.
	if( !slog_file && !slog_open_failed )
	{
		slog_file = FS_Open( "logs/slayer_diag.log", "a", true );
		if( !slog_file )
		{
			slog_open_failed = true;
			return;
		}
	}

	if( slog_file )
	{
		FS_Printf( slog_file, "[%8.2f] %s\n", host.realtime, msg );
		FS_Flush( slog_file ); // flush each line so a crash still leaves the trail
	}
}
