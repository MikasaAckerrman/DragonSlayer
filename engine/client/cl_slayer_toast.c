/*
cl_slayer_toast.c - Slayer3D Steam-style corner notification
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

// A small Steam-style notification that slides up from the bottom-left when
// you connect to a server: a dark panel with a Steam-blue accent bar telling
// you whether you're in via a real Steam account. Fades in and out.

#include <inttypes.h>
#include "common.h"
#include "client.h"
#include "cl_slayer_toast.h"
#include "cl_steam_login.h"

static CVAR_DEFINE_AUTO( slayer_steam_toast, "1", FCVAR_ARCHIVE,
	"Slayer3D: show a Steam-style notification on connect (0 = off)" );

#define TOAST_DURATION  7.5
#define TOAST_FADE_IN   0.35f
#define TOAST_FADE_OUT  0.70f

static char   toast_header[64];
static char   toast_text[160];
static double toast_time;   // host.realtime when shown (0 = idle)

void Slayer_Toast_Init( void )
{
	Cvar_RegisterVariable( &slayer_steam_toast );
}

void Slayer_Toast_Show( const char *header, const char *text )
{
	Q_strncpy( toast_header, header ? header : "", sizeof( toast_header ));
	Q_strncpy( toast_text, text ? text : "", sizeof( toast_text ));
	toast_time = host.realtime;
}

void Slayer_Toast_OnConnected( void )
{
	static qboolean shown_this_session = false;
	uint64_t id;

	if( slayer_steam_toast.value == 0.0f )
		return;

	// Only once per game launch, and only if actually signed in — so it reads
	// as "you're playing under Steam", not a banner that pops on every connect.
	if( shown_this_session )
		return;

	id = Slayer_SteamLogin_GetLocalID();
	if( id == 0 )
		return;

	shown_this_session = true;
	Slayer_Toast_Show( "Steam", "Вы вошли через Steam" );
}

void Slayer_Toast_Draw( void )
{
	cl_font_t *font;
	rgba_t     col_hdr, col_txt;
	float      age, alpha;
	int        sw, sh, ch, pw, ph, px, py, maxw;
	int        hw, hh, tw, th;
	byte       a8;

	if( slayer_steam_toast.value == 0.0f || toast_time <= 0.0 )
		return;

	age = (float)( host.realtime - toast_time );
	if( age < 0.0f || age > TOAST_DURATION )
		return;

	alpha = 1.0f;
	if( age < TOAST_FADE_IN )
		alpha = age / TOAST_FADE_IN;
	else if( age > TOAST_DURATION - TOAST_FADE_OUT )
		alpha = (float)( TOAST_DURATION - age ) / TOAST_FADE_OUT;
	if( alpha < 0.0f ) alpha = 0.0f;
	if( alpha > 1.0f ) alpha = 1.0f;

	font = Con_GetCurFont();
	if( !font || !font->valid )
		return;
	ch = font->charHeight;

	sw = refState.width;
	sh = refState.height;
	if( sw <= 0 || sh <= 0 )
		return;

	CL_DrawStringLen( font, toast_header, &hw, &hh, FONT_DRAW_UTF8 );
	CL_DrawStringLen( font, toast_text, &tw, &th, FONT_DRAW_UTF8 );
	maxw = ( hw > tw ) ? hw : tw;

	pw = maxw + ch * 3;
	ph = ch * 3;                              // two text lines + padding
	px = (int)( sw * 0.018f );
	py = sh - ph - (int)( sh * 0.035f );

	// slide in from the left edge during the fade-in
	if( age < TOAST_FADE_IN )
		px -= (int)( (float)( pw + px + 4 ) * ( 1.0f - age / TOAST_FADE_IN ));

	// panel + Steam-blue accent bar
	a8 = (byte)( 235.0f * alpha );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, px, py, pw, ph, 20, 24, 30, a8 );
	a8 = (byte)( 255.0f * alpha );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, px, py, 3, ph, 102, 192, 244, a8 );

	MakeRGBA( col_hdr, 102, 192, 244, (byte)( 255.0f * alpha ));   // Steam blue
	MakeRGBA( col_txt, 235, 235, 235, (byte)( 255.0f * alpha ));

	CL_DrawString( (float)( px + ch ), (float)( py + ch / 2 ),
		toast_header, col_hdr, font, FONT_DRAW_UTF8 );
	CL_DrawString( (float)( px + ch ), (float)( py + ch + ch / 2 ),
		toast_text, col_txt, font, FONT_DRAW_UTF8 );
}
