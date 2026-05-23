/*
cl_loading_slayer.c - Slayer3D PC-style loading screen overlay
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

#include "common.h"
#include "client.h"
#include "cl_loading_slayer.h"
#include "ref_common.h"

// ===========================================================================
// Cvars (DEFINE before registration)
// ===========================================================================
static CVAR_DEFINE_AUTO( slayer_loading_screen, "1", FCVAR_ARCHIVE, "enable Slayer3D loading overlay" );
static CVAR_DEFINE_AUTO( slayer_loading_console, "0", FCVAR_ARCHIVE, "show console text under loading panel" );
static CVAR_DEFINE_AUTO( slayer_loading_color, "40 40 40 200", FCVAR_ARCHIVE, "panel RGBA color" );

// ===========================================================================
// Internal state
// ===========================================================================
static qboolean s_dragging;
static int      s_drag_finger;
static int      s_panel_offset_x;  // user drag offset from center (pixels)
static int      s_panel_offset_y;

// ===========================================================================
// Helpers
// ===========================================================================

// Parse "R G B A" from cvar string into byte components
static void LoadingScreen_ParseColor( byte *r, byte *g, byte *b, byte *a )
{
	int ri = 40, gi = 40, bi = 40, ai = 200;
	sscanf( slayer_loading_color.string, "%d %d %d %d", &ri, &gi, &bi, &ai );
	*r = (byte)bound( 0, ri, 255 );
	*g = (byte)bound( 0, gi, 255 );
	*b = (byte)bound( 0, bi, 255 );
	*a = (byte)bound( 0, ai, 255 );
}

static void LoadingScreen_DrawRect( int x, int y, int w, int h, byte r, byte g, byte b, byte a )
{
	ref.dllFuncs.FillRGBA( kRenderTransTexture, x, y, w, h, r, g, b, a );
}

// Simple centered-text helper; returns text width drawn.
static int LoadingScreen_DrawText( int x, int y, const char *text, byte r, byte g, byte b, byte a )
{
	rgba_t color;
	MakeRGBA( color, r, g, b, a );
	return Con_DrawString( x, y, text, color );
}

// ===========================================================================
// Public API
// ===========================================================================

void Slayer_Loading_Init( void )
{
	Cvar_RegisterVariable( &slayer_loading_screen );
	Cvar_RegisterVariable( &slayer_loading_console );
	Cvar_RegisterVariable( &slayer_loading_color );
}

void Slayer_Loading_Reset( void )
{
	s_dragging = false;
	s_drag_finger = -1;
	s_panel_offset_x = 0;
	s_panel_offset_y = 0;
}

qboolean Slayer_Loading_Draw( void )
{
	int screen_w, screen_h;
	int panel_w, panel_h;
	int panel_x, panel_y;
	int cur_y;
	int bar_x, bar_w, bar_h;
	int text_w;
	byte bg_r, bg_g, bg_b, bg_a;
	float overall_pct, file_pct;
	char buf[256];
	int btn_x, btn_y, btn_w, btn_h;
	int char_h;

	if( !slayer_loading_screen.value )
		return false;

	screen_w = refState.width;
	screen_h = refState.height;

	// Panel dimensions: ~60% width, auto height
	panel_w = (int)( screen_w * 0.60f );
	panel_h = (int)( screen_h * 0.40f );

	// Minimum sizes
	if( panel_w < 320 ) panel_w = 320;
	if( panel_h < 200 ) panel_h = 200;

	// Center + drag offset
	panel_x = ( screen_w - panel_w ) / 2 + s_panel_offset_x;
	panel_y = ( screen_h - panel_h ) / 2 + s_panel_offset_y;

	// Clamp to screen
	panel_x = bound( 0, panel_x, screen_w - panel_w );
	panel_y = bound( 0, panel_y, screen_h - panel_h );

	// Get char height for text spacing
	Con_DrawStringLen( "A", NULL, &char_h );
	if( char_h < 8 ) char_h = 8;

	// Parse panel color
	LoadingScreen_ParseColor( &bg_r, &bg_g, &bg_b, &bg_a );

	// ==== Background panel ====
	LoadingScreen_DrawRect( panel_x, panel_y, panel_w, panel_h, bg_r, bg_g, bg_b, bg_a );

	// 1-px border
	LoadingScreen_DrawRect( panel_x, panel_y, panel_w, 1, 120, 120, 120, 255 );
	LoadingScreen_DrawRect( panel_x, panel_y + panel_h - 1, panel_w, 1, 120, 120, 120, 255 );
	LoadingScreen_DrawRect( panel_x, panel_y, 1, panel_h, 120, 120, 120, 255 );
	LoadingScreen_DrawRect( panel_x + panel_w - 1, panel_y, 1, panel_h, 120, 120, 120, 255 );

	// Inner padding
	cur_y = panel_y + char_h;
	bar_x = panel_x + (int)( panel_w * 0.05f );
	bar_w = panel_w - (int)( panel_w * 0.10f );
	bar_h = char_h + 4;

	// ==== Title: "Загрузка..." ====
	LoadingScreen_DrawText( bar_x, cur_y, "Загрузка...", 255, 255, 255, 255 );
	cur_y += char_h * 2;

	// ==== Subtitle: "Проверка и загрузка ресурсов..." ====
	LoadingScreen_DrawText( bar_x, cur_y, "Проверка и загрузка ресурсов...", 200, 200, 200, 255 );
	cur_y += char_h * 2;

	// ==== Overall progress bar (yellow) ====
	// Calculate overall progress from cls.dl
	if( cls.dl.nTotalToTransfer > 0 )
		overall_pct = 1.0f - ( (float)cls.dl.nRemainingToTransfer / (float)cls.dl.nTotalToTransfer );
	else
		overall_pct = 0.0f;
	overall_pct = bound( 0.0f, overall_pct, 1.0f );

	// Bar background (dark)
	LoadingScreen_DrawRect( bar_x, cur_y, bar_w, bar_h, 20, 20, 20, 255 );
	// Bar fill (yellow)
	if( overall_pct > 0.0f )
		LoadingScreen_DrawRect( bar_x + 1, cur_y + 1, (int)( ( bar_w - 2 ) * overall_pct ), bar_h - 2, 220, 200, 40, 255 );
	// Percentage text on bar
	Q_snprintf( buf, sizeof( buf ), "%d%%", (int)( overall_pct * 100.0f ) );
	Con_DrawStringLen( buf, &text_w, NULL );
	LoadingScreen_DrawText( bar_x + ( bar_w - text_w ) / 2, cur_y + 2, buf, 255, 255, 255, 255 );
	cur_y += bar_h + char_h;

	// ==== Current file name ====
	if( host.downloadfile[0] )
		Q_snprintf( buf, sizeof( buf ), "%s", host.downloadfile );
	else
		Q_strncpy( buf, "...", sizeof( buf ) );
	LoadingScreen_DrawText( bar_x, cur_y, buf, 180, 180, 180, 255 );
	cur_y += char_h + 4;

	// ==== "Осталось N файлов" ====
	if( host.downloadcount > 0 )
		Q_snprintf( buf, sizeof( buf ), "Осталось %d файл(ов)", host.downloadcount );
	else
		Q_strncpy( buf, "", sizeof( buf ) );
	LoadingScreen_DrawText( bar_x, cur_y, buf, 180, 180, 180, 255 );
	cur_y += char_h * 2;

	// ==== Per-file progress bar ====
	file_pct = scr_download.value;
	if( file_pct < 0.0f ) file_pct = 0.0f;
	file_pct = bound( 0.0f, file_pct / 100.0f, 1.0f );

	// Bar background
	LoadingScreen_DrawRect( bar_x, cur_y, bar_w, bar_h, 20, 20, 20, 255 );
	// Bar fill (lighter yellow/green)
	if( file_pct > 0.0f )
		LoadingScreen_DrawRect( bar_x + 1, cur_y + 1, (int)( ( bar_w - 2 ) * file_pct ), bar_h - 2, 100, 200, 100, 255 );
	Q_snprintf( buf, sizeof( buf ), "%d%%", (int)( file_pct * 100.0f ) );
	Con_DrawStringLen( buf, &text_w, NULL );
	LoadingScreen_DrawText( bar_x + ( bar_w - text_w ) / 2, cur_y + 2, buf, 255, 255, 255, 255 );
	cur_y += bar_h + char_h;

	// ==== Cancel button ====
	btn_w = (int)( panel_w * 0.25f );
	btn_h = char_h + 8;
	btn_x = panel_x + ( panel_w - btn_w ) / 2;
	btn_y = panel_y + panel_h - btn_h - char_h;

	// Button background
	LoadingScreen_DrawRect( btn_x, btn_y, btn_w, btn_h, 80, 30, 30, 230 );
	// Button border
	LoadingScreen_DrawRect( btn_x, btn_y, btn_w, 1, 200, 60, 60, 255 );
	LoadingScreen_DrawRect( btn_x, btn_y + btn_h - 1, btn_w, 1, 200, 60, 60, 255 );
	LoadingScreen_DrawRect( btn_x, btn_y, 1, btn_h, 200, 60, 60, 255 );
	LoadingScreen_DrawRect( btn_x + btn_w - 1, btn_y, 1, btn_h, 200, 60, 60, 255 );

	// Button text centered
	Con_DrawStringLen( "Отменить", &text_w, NULL );
	LoadingScreen_DrawText( btn_x + ( btn_w - text_w ) / 2, btn_y + 4, "Отменить", 255, 220, 220, 255 );

	// ==== Download URL (if HTTP) ====
	if( cl.http_download )
	{
		cur_y = btn_y - char_h * 2;
		LoadingScreen_DrawText( bar_x, cur_y, "Хостинг для размещения файлов:", 140, 140, 140, 255 );
		cur_y += char_h + 2;
		// The URL is stored internally; show placeholder or servername
		Q_snprintf( buf, sizeof( buf ), "%s", cls.servername );
		LoadingScreen_DrawText( bar_x, cur_y, buf, 100, 180, 255, 255 );
	}

	return true;
}

qboolean Slayer_Loading_TouchEvent( touchEventType type, int fingerID, float x, float y, float dx, float dy )
{
	int screen_w, screen_h;
	int panel_w, panel_h;
	int panel_x, panel_y;
	int btn_w, btn_h, btn_x, btn_y;
	int char_h;
	int px, py; // touch in pixels

	if( !slayer_loading_screen.value )
		return false;

	screen_w = refState.width;
	screen_h = refState.height;

	panel_w = (int)( screen_w * 0.60f );
	panel_h = (int)( screen_h * 0.40f );
	if( panel_w < 320 ) panel_w = 320;
	if( panel_h < 200 ) panel_h = 200;

	panel_x = ( screen_w - panel_w ) / 2 + s_panel_offset_x;
	panel_y = ( screen_h - panel_h ) / 2 + s_panel_offset_y;
	panel_x = bound( 0, panel_x, screen_w - panel_w );
	panel_y = bound( 0, panel_y, screen_h - panel_h );

	Con_DrawStringLen( "A", NULL, &char_h );
	if( char_h < 8 ) char_h = 8;

	px = (int)( x * screen_w );
	py = (int)( y * screen_h );

	// Cancel button rect
	btn_w = (int)( panel_w * 0.25f );
	btn_h = char_h + 8;
	btn_x = panel_x + ( panel_w - btn_w ) / 2;
	btn_y = panel_y + panel_h - btn_h - char_h;

	if( type == event_down )
	{
		// Check cancel button tap
		if( px >= btn_x && px <= btn_x + btn_w && py >= btn_y && py <= btn_y + btn_h )
		{
			Cbuf_AddText( "disconnect\n" );
			return true;
		}

		// Start drag if inside panel
		if( px >= panel_x && px <= panel_x + panel_w && py >= panel_y && py <= panel_y + panel_h )
		{
			s_dragging = true;
			s_drag_finger = fingerID;
			return true;
		}
	}
	else if( type == event_up )
	{
		if( s_dragging && fingerID == s_drag_finger )
		{
			s_dragging = false;
			s_drag_finger = -1;
			return true;
		}
	}
	else if( type == event_motion )
	{
		if( s_dragging && fingerID == s_drag_finger )
		{
			s_panel_offset_x += (int)( dx * screen_w );
			s_panel_offset_y += (int)( dy * screen_h );
			return true;
		}
	}

	return false;
}
