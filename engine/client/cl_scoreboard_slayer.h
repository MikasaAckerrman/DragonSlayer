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

// Whether our scoreboard is on screen right now (held, intermission, or the
// auto-show while dead). Side-effect free, so it can be asked at any point in
// the frame.
qboolean Slayer_Scoreboard_IsVisible( void );

// Whether the game library's OWN scoreboard should be suppressed this frame.
// Wider than IsVisible: it also covers the dead period when the client library
// auto-shows its board but ours is dismissed, so the stock board cannot bleed
// through. Drives the pixel gate in cl_stockboard_gate_slayer.c.
qboolean Slayer_Scoreboard_ShouldGateStock( void );

// How hard to suppress the game's own scoreboard this frame:
// 0 = not at all, 1 = skip VGui_Paint, 2 = also skip the client HUD redraw.
// Driven by slayer_scoreboard_block_stock and only non-zero while ours is up.
int Slayer_Scoreboard_StockBlockLevel( void );

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

// DeathMsg named the local player as the victim.
// Second death signal for the ambiguous health == 1 case (a spectating dead
// player), because the ScoreAttrib dead bit is a CS convention some servers
// never send. Called from Slayer_OnDeathMsg.
void Slayer_Scoreboard_OnLocalDeath( void );

// Hook for server-sent "HealthInfo" user message.
// Some servers (ReGameDLL) broadcast per-player HP this way.
//   pbuf  - raw payload bytes (byte slot, byte health)
//   iSize - payload length
void Slayer_OnHealthInfo( const byte *pbuf, int iSize );

// Parse a status response line to extract SteamID for avatar loading.
// Called from cl_parse.c on each svc_print message.
void Slayer_ParseStatusLine( const char *line );

// Forward decl to avoid pulling cdll headers into this header.
struct usercmd_s;

// While the Slayer scoreboard is active, OR IN_SCORE into cmd->buttons
// so the server emits svc_pings every snapshot (matches vanilla GoldSrc
// +showscores behavior). Called from CL_CreateCmd in cl_main.c.
// No-op when scoreboard is inactive or cmd is NULL.
void Slayer_Scoreboard_PatchUsercmd( struct usercmd_s *cmd );

// Get team_id for a player slot (0-based). Returns SLAYER_TEAM_* constant.
// Used by killsound to detect teamkills.
int Slayer_GetPlayerTeam( int slot );

#endif // CL_SCOREBOARD_SLAYER_H
