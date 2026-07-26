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
#include "cl_avatar_download.h"

static CVAR_DEFINE_AUTO( slayer_steam_toast, "1", FCVAR_ARCHIVE,
	"Slayer3D: show the Steam sign-in notification when the client starts (0 = off)" );

// Delay before the start-up banner appears, so it lands over a drawn menu
// rather than the first frame of a still-initialising client.
#define TOAST_START_DELAY  1.5

#define TOAST_DURATION  7.5
#define TOAST_FADE_IN   0.35f
#define TOAST_FADE_OUT  0.70f

static char     toast_header[64];
static char     toast_text[160];
static double   toast_time;          // host.realtime when shown (0 = idle)
static int      toast_avatar_tex;    // 0 = untried, -1 = unavailable, >0 = texid
static qboolean toast_steam_shown;   // sign-in banner is once per launch

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

// Resolve the signed-in user's own avatar texture, for display in the banner.
// Loads the cached PNG the avatar downloader writes; if it isn't there yet,
// queues one download and shows the banner without an icon this time (the file
// will be present on the next launch). Returns 0 when there is nothing to draw.
static int Slayer_Toast_LocalAvatar( uint64_t id )
{
	static uint64_t loaded_for_id = 0;
	char path[128];

	if( id == 0 )
	{
		// Signed out: forget the icon so a later sign-in loads the new one.
		toast_avatar_tex = 0;
		loaded_for_id = 0;
		return 0;
	}

	if( id != loaded_for_id )
	{
		toast_avatar_tex = 0;      // different account -> reload
		loaded_for_id = id;
	}

	if( toast_avatar_tex != 0 )
		return ( toast_avatar_tex > 0 ) ? toast_avatar_tex : 0;

	Q_snprintf( path, sizeof( path ), "avatars/%"PRIu64".png", id );

	if( !FS_FileExists( path, false ))
	{
		toast_avatar_tex = -1;   // don't retry every frame
		Slayer_AvatarDownload_Request( id, 0 );
		return 0;
	}

	toast_avatar_tex = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, TF_IMAGE );
	if( toast_avatar_tex == 0 )
	{
		FS_Delete( path );       // bad cache; next launch re-downloads
		toast_avatar_tex = -1;
	}

	return ( toast_avatar_tex > 0 ) ? toast_avatar_tex : 0;
}

// Show the sign-in banner once per launch, shortly after the client comes up.
// The user asked for this at CLIENT start (i.e. over the menu), not on joining
// a server — joining is unrelated to whether you are signed in via Steam.
static void Slayer_Toast_MaybeShowSteamBanner( void )
{
	uint64_t id;

	if( toast_steam_shown )
		return;
	if( host.realtime < TOAST_START_DELAY )
		return;

	id = Slayer_SteamLogin_GetLocalID();
	if( id == 0 )
		return;   // not signed in — say nothing at all

	toast_steam_shown = true;
	Slayer_Toast_Show( "Steam", "Вы вошли через Steam" );
}

void Slayer_Toast_Draw( void )
{
	cl_font_t *font;
	rgba_t     col_hdr, col_txt;
	float      age, alpha;
	int        sw, sh, ch, pw, ph, px, py, maxw;
	int        hw, hh, tw, th;
	int        icon, text_x;
	byte       a8;

	if( slayer_steam_toast.value == 0.0f )
		return;

	Slayer_Toast_MaybeShowSteamBanner();

	if( toast_time <= 0.0 )
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

	ph = ch * 3;                              // two text lines + padding

	// Steam avatar on the left, when we have one cached. The panel grows to fit
	// it so the text never sits on top of the icon.
	icon = Slayer_Toast_LocalAvatar( Slayer_SteamLogin_GetLocalID()) ? ( ph - ch ) : 0;
	text_x = ch + ( icon ? icon + ch / 2 : 0 );

	pw = maxw + ch * 3 + ( icon ? icon + ch / 2 : 0 );
	px = sw - pw - (int)( sw * 0.018f );   // bottom-RIGHT corner
	py = sh - ph - (int)( sh * 0.035f );

	// slide in from the right edge during the fade-in
	if( age < TOAST_FADE_IN )
		px += (int)( (float)( sw - px + 4 ) * ( 1.0f - age / TOAST_FADE_IN ));

	// panel + Steam-blue accent bar
	a8 = (byte)( 235.0f * alpha );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, px, py, pw, ph, 20, 24, 30, a8 );
	a8 = (byte)( 255.0f * alpha );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, px, py, 3, ph, 102, 192, 244, a8 );

	if( icon > 0 )
	{
		ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
		ref.dllFuncs.Color4ub( 255, 255, 255, (byte)( 255.0f * alpha ));
		ref.dllFuncs.R_DrawStretchPic( (float)( px + ch / 2 ), (float)( py + ch / 2 ),
			(float)icon, (float)icon, 0, 0, 1, 1, toast_avatar_tex );
		ref.dllFuncs.Color4ub( 255, 255, 255, 255 );   // restore for the next drawer
	}

	MakeRGBA( col_hdr, 102, 192, 244, (byte)( 255.0f * alpha ));   // Steam blue
	MakeRGBA( col_txt, 235, 235, 235, (byte)( 255.0f * alpha ));

	CL_DrawString( (float)( px + text_x ), (float)( py + ch / 2 ),
		toast_header, col_hdr, font, FONT_DRAW_UTF8 );
	CL_DrawString( (float)( px + text_x ), (float)( py + ch + ch / 2 ),
		toast_text, col_txt, font, FONT_DRAW_UTF8 );
}
