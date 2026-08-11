/*
cl_slayer_conspy.h - Slayer3D console spam accounting
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

#ifndef CL_SLAYER_CONSPY_H
#define CL_SLAYER_CONSPY_H

#include "xash3d_types.h"

// Register the cvar and the commands. Called once at client init.
void Slayer_ConSpy_Init( void );

// Account one console line. Called from Sys_Print, i.e. from EVERY console
// message including the ones the game DLL and the engine emit. A no-op unless
// the cvar is on, so the cost when disabled is one float compare.
//
// WHY this exists: "the console is spammed" cannot be fixed by reading the
// source, because most lines come from the client DLL and from engine paths we
// did not write. What is needed is a COUNT per distinct message, so the top
// offender is a fact rather than a guess. Dumping the whole console to a file
// would produce megabytes to read by hand; this produces a table.
void Slayer_ConSpy_Note( const char *msg );

#endif // CL_SLAYER_CONSPY_H
