/*
cl_slayer_toast.h - Slayer3D Steam-style corner notification
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

#ifndef CL_SLAYER_TOAST_H
#define CL_SLAYER_TOAST_H

// Registers the slayer_steam_toast cvar. Call once at client init.
void Slayer_Toast_Init( void );

// Queue a bottom-left Steam-style notification (header + one line of text).
void Slayer_Toast_Show( const char *header, const char *text );

// Fired when the connection to a server completes; shows the Steam status.
void Slayer_Toast_OnConnected( void );

// Draw the active toast (fade + slide). Call from V_PostRender.
void Slayer_Toast_Draw( void );

#endif // CL_SLAYER_TOAST_H
