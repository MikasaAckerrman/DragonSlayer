/*
cl_stockboard_gate_slayer.c - Slayer3D: drop the game's own scoreboard, whatever
                              client library draws it
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

// See cl_stockboard_gate_slayer.h for what this recognises and why it recognises
// it that way rather than by a fixed rectangle.
//
// The detection thresholds below are derived from the client library's own code
// (cs16-client / the player's fork, cl_dll/hud/scoreboard.cpp):
//
//     xstart = 0.125 * ScreenWidth;   xend = ScreenWidth - xstart;
//     ystart = 90;                    yend = ScreenHeight - ystart;
//     DrawUtils::DrawRectangle( xstart, ystart, xend - xstart, yend - ystart,
//                               0, 0, 0, 153, drawStroke );
//
// so the background fill is 75 % of the screen wide, starts at y = 90, and is
// translucent. Those three facts are what the signature tests -- as fractions of
// the screen, not as pixels, so a different resolution or a reskinned board still
// matches.

#include "common.h"
#include "client.h"
#include "cl_stockboard_gate_slayer.h"
#include "cl_scoreboard_slayer.h"
#include "cl_slayer_log.h"

// The gate only ever engages while our own board is open, so these describe what
// the STOCK board looks like, not what is allowed on screen generally.
static CVAR_DEFINE_AUTO( slayer_stockboard_gate, "1", FCVAR_ARCHIVE,
	"Slayer3D: drop the game's own scoreboard primitives while ours is open (0 = off)" );

// Minimum width, as a fraction of the screen, for a fill to be considered a board
// background. The stock board is 0.75; a chat background or a hint box is far
// narrower. 0.5 leaves room for a modified board without matching anything else.
static CVAR_DEFINE_AUTO( slayer_stockboard_minwidth, "0.5", FCVAR_ARCHIVE,
	"Slayer3D: narrowest fill still treated as the stock board (fraction of screen width)" );

// Minimum height, likewise. The stock board is (H - 180)/H, about 0.83 at 1080p.
static CVAR_DEFINE_AUTO( slayer_stockboard_minheight, "0.35", FCVAR_ARCHIVE,
	"Slayer3D: shortest fill still treated as the stock board (fraction of screen height)" );

static CVAR_DEFINE_AUTO( slayer_stockboard_diag, "0", FCVAR_ARCHIVE,
	"Slayer3D: log what the stock-board gate matched and dropped" );

// --- per-frame state -------------------------------------------------------
//
// `latched` is cleared at the start of every client HUD redraw, so a board seen
// last frame cannot suppress anything this frame. That is what keeps the gate
// from leaking into the rest of the HUD if the client stops drawing its board.
static qboolean sb_latched;
static int      sb_x, sb_y, sb_w, sb_h;
static unsigned int sb_dropped;
static qboolean sb_ever_latched;
static double   sb_diag_last;

void Slayer_StockBoard_Init( void )
{
	Cvar_RegisterVariable( &slayer_stockboard_gate );
	Cvar_RegisterVariable( &slayer_stockboard_minwidth );
	Cvar_RegisterVariable( &slayer_stockboard_minheight );
	Cvar_RegisterVariable( &slayer_stockboard_diag );

	sb_latched = false;
	sb_ever_latched = false;
	sb_dropped = 0;
	sb_diag_last = 0.0;
}

void Slayer_StockBoard_BeginFrame( void )
{
	sb_latched = false;
	sb_x = sb_y = sb_w = sb_h = 0;
}

void Slayer_StockBoard_Stats( int *out_latched, unsigned int *out_dropped )
{
	if( out_latched )  *out_latched = (int)sb_ever_latched;
	if( out_dropped )  *out_dropped = sb_dropped;
}

/*
====================
SB_GateActive

Is the gate allowed to do anything at all right now?

Two conditions, and both matter. The cvar, obviously. And the game would OTHERWISE
be showing a stock board: our own board is open, OR the local player is dead and
the client library is auto-showing its board there. That second case is the fix
for the intermittent death board -- see Slayer_Scoreboard_ShouldGateStock. Outside
those windows the client's board is the only board there is, and dropping it would
leave the player with no scoreboard at all, which is worse than two.
====================
*/
static qboolean SB_GateActive( void )
{
	if( slayer_stockboard_gate.value == 0.0f )
		return false;

	return Slayer_Scoreboard_ShouldGateStock();
}

/*
====================
SB_LooksLikeBoard

Does this fill have the shape of a stock scoreboard background?

Wide, tall, and TRANSLUCENT. The alpha test is what separates a board background
from an opaque letterbox or a solid panel; the stock board uses 153 of 255, and
every reskin of it that is still readable has to stay translucent for the same
reason. An opaque fill of that size is a loading screen or a fade, and dropping
one of those would be a visible bug.

Screen size comes from `clgame.scrInfo`, which is the coordinate space the client
library draws in -- the same space these coordinates arrive in. Using refState
here would compare HUD coordinates against real pixels and fail on any device
where hud_scale is not 1.
====================
*/
static qboolean SB_LooksLikeBoard( int x, int y, int w, int h, int a )
{
	int   sw = clgame.scrInfo.iWidth;
	int   sh = clgame.scrInfo.iHeight;
	float min_w = slayer_stockboard_minwidth.value;
	float min_h = slayer_stockboard_minheight.value;

	if( sw <= 0 || sh <= 0 )
		return false;

	if( min_w < 0.2f ) min_w = 0.2f;      // a fill this narrow is not a board
	if( min_w > 1.0f ) min_w = 1.0f;
	if( min_h < 0.2f ) min_h = 0.2f;
	if( min_h > 1.0f ) min_h = 1.0f;

	if( w < (int)( (float)sw * min_w ))
		return false;
	if( h < (int)( (float)sh * min_h ))
		return false;

	// Translucent, and not invisible: alpha 0 is a no-op the client library uses
	// for spacing, and matching it would latch the gate on nothing.
	if( a <= 0 || a >= 250 )
		return false;

	// It has to be ON SCREEN. A fill placed off-screen is not being shown to
	// anyone, and matching it would latch the gate for the whole frame.
	if( x + w <= 0 || y + h <= 0 )
		return false;
	if( x >= sw || y >= sh )
		return false;

	return true;
}

static qboolean SB_Inside( int x, int y, int w, int h )
{
	// Centre-inside rather than fully-contained: a glyph or a divider that pokes a
	// pixel past the board edge is still part of the board, and requiring full
	// containment would leave a fringe of text behind.
	int cx = x + w / 2;
	int cy = y + h / 2;

	return ( cx >= sb_x && cx <= sb_x + sb_w
	      && cy >= sb_y && cy <= sb_y + sb_h );
}

int Slayer_StockBoard_FilterRect( int x, int y, int w, int h, int a )
{
	if( !SB_GateActive( ))
		return 0;

	if( w <= 0 || h <= 0 )
		return 0;

	if( !sb_latched && SB_LooksLikeBoard( x, y, w, h, a ))
	{
		sb_latched = true;
		sb_x = x;
		sb_y = y;
		sb_w = w;
		sb_h = h;
		sb_dropped++;

		if( !sb_ever_latched )
		{
			sb_ever_latched = true;
			Slayer_Log_Printf( "stock-board gate: matched a board background at %d %d %dx%d (screen %dx%d)",
				x, y, w, h, clgame.scrInfo.iWidth, clgame.scrInfo.iHeight );
		}

		if( slayer_stockboard_diag.value >= 1.0f
		 && cl.time - sb_diag_last >= 0.5 )
		{
			sb_diag_last = cl.time;
			Slayer_Log_Printf( "stock-board gate: latched %d %d %dx%d, dropped %u so far",
				x, y, w, h, sb_dropped );
		}

		return 1;
	}

	if( sb_latched && SB_Inside( x, y, w, h ))
	{
		sb_dropped++;
		return 1;
	}

	return 0;
}

int Slayer_StockBoard_FilterPrim( int x, int y, int w, int h )
{
	if( !sb_latched )
		return 0;
	if( !SB_GateActive( ))
		return 0;

	if( SB_Inside( x, y, w, h ))
	{
		sb_dropped++;
		return 1;
	}

	return 0;
}
