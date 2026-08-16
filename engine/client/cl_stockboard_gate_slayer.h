/*
cl_stockboard_gate_slayer.h - Slayer3D: drop the game's own scoreboard, whatever
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
#ifndef CL_STOCKBOARD_GATE_SLAYER_H
#define CL_STOCKBOARD_GATE_SLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

// WHY THE EXISTING BLOCK DOES NOT REMOVE IT
//
// slayer_scoreboard_block_stock level 1 skips VGui_Paint, which removes a stock
// board only if the board is a VGUI panel. In the client library this player
// actually runs it is not: CHudScoreboard is a HUD-list element
// (`m_iFlags |= HUD_DRAW`) that draws itself with DrawUtils::DrawRectangle and
// DrawUtils::DrawHudString -- i.e. through the engine's own
// pfnFillRGBA / pfnFillRGBABlend / pfnDrawCharacter exports. Skipping VGui_Paint
// cannot touch it. Level 2 does remove it, by skipping the client's entire HUD
// redraw -- and with it health, armour, ammo, money and the timer.
//
// Every primitive it draws, however, passes through THIS engine. So the board can
// be dropped at the point where it asks us to draw it, and nothing else has to be
// sacrificed.
//
// HOW THE BOARD IS RECOGNISED, and this is the part that makes it safe
//
// Not by a hardcoded rectangle. The band a stock board occupies (x from 0.125*W,
// y from 90 to H-90) covers most of the screen, and blanking it would eat chat,
// hints and anything else drawn there.
//
// Instead the board is recognised by ITS OWN BACKGROUND. CHudScoreboard::Draw
// begins with one DrawRectangle over the whole board -- a wide, tall, translucent
// fill -- and everything else it draws comes after that, in the same frame,
// inside that rectangle. So: watch for a fill with that shape, and from that
// moment until the end of the frame, drop primitives that land inside it.
//
// Consequences worth stating, because they are the reason for this design:
//   * chat drawn BEFORE the board in the frame is untouched;
//   * the HUD outside the rectangle is untouched;
//   * it adapts to whatever board the player's client library draws, including a
//     resized or reskinned one, because the rectangle comes from the board;
//   * it costs one rectangle comparison per primitive, and only while our own
//     board is open.

// Reset at the start of each client HUD redraw. Without this the latch from the
// previous frame would suppress the next frame's first primitives.
void Slayer_StockBoard_BeginFrame( void );

// A rectangle the client library is about to fill, in HUD coordinates.
// Returns true when it must be dropped.
//
// This is also where the board is DETECTED: a fill matching the board signature
// both latches the gate and is itself dropped.
int Slayer_StockBoard_FilterRect( int x, int y, int w, int h, int a );

// A primitive (glyph, sprite, line) the client library is about to draw, in HUD
// coordinates. Returns true when it must be dropped. Only ever true after a board
// background was seen in this frame.
int Slayer_StockBoard_FilterPrim( int x, int y, int w, int h );

// Diagnostics: how many primitives were dropped, and whether the gate has ever
// fired. A gate that silently never matches looks exactly like one that works.
void Slayer_StockBoard_Stats( int *out_latched, unsigned int *out_dropped );

void Slayer_StockBoard_Init( void );

#ifdef __cplusplus
}
#endif

#endif // CL_STOCKBOARD_GATE_SLAYER_H
