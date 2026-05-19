/*
cl_scoreboard_slayer.h - Slayer3D custom scoreboard overlay
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

#ifndef CL_SCOREBOARD_SLAYER_H
#define CL_SCOREBOARD_SLAYER_H

#include "xash3d_types.h"

// Initialize scoreboard cvars and commands.
// Called once from V_InitSlayerCvars().
void Slayer_Scoreboard_Init( void );

// Draw the scoreboard overlay (call from V_PostRender 2D block).
// No-op when scoreboard is not active.
void Slayer_Scoreboard_Draw( void );

// Reset all score data (frags, deaths, flags).
// Called from Slayer_ResetMatchState() on map change/disconnect.
void Slayer_Scoreboard_Reset( void );

// Hook for server-sent "ScoreInfo" user message.
//   pbuf  - raw payload bytes
//   iSize - payload length
void Slayer_OnScoreInfo( const byte *pbuf, int iSize );

// Hook for server-sent "ScoreAttrib" user message.
//   pbuf  - raw payload bytes
//   iSize - payload length
void Slayer_OnScoreAttrib( const byte *pbuf, int iSize );

// Hook for health updates from cl_parse.c.
// Stores HP for the local/spectated player row in the scoreboard.
void Slayer_OnHealthUpdate( int hp );

#endif // CL_SCOREBOARD_SLAYER_H
