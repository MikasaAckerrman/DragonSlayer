/*
cl_slayer_log.h - Slayer3D file-based diagnostic logger
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

#ifndef CL_SLAYER_LOG_H
#define CL_SLAYER_LOG_H

// Registers the slayer_log cvar. Call once at client init.
void Slayer_Log_Init( void );

// Append a timestamped line to <gamedir>/logs/slayer_diag.log (and logcat on
// Android). No-op when the slayer_log cvar is 0. Printf-style.
void Slayer_Log_Printf( const char *fmt, ... );

#endif // CL_SLAYER_LOG_H
