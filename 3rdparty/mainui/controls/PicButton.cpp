/*
PicButton.h - animated button with picture
Copyright (C) 2010 Uncle Mike
Copyright (C) 2017 a1batross

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "extdll_menu.h"
#include "BaseMenu.h"
#include "Bitmap.h"
#include "PicButton.h"
#include "Utils.h"
#include "Scissor.h"
#include "Btns.h"
#include <stdlib.h>
#include "Framework.h"

static int g_hotkeys[PC_BUTTONCOUNT] =
{
	'n', // PC_NEW_GAME = 0,
	'r', // PC_RESUME_GAME,
	'h', // PC_HAZARD_COURSE,
	'c', // PC_CONFIG,
	'l', // PC_LOAD_GAME,
	's', // PC_SAVE_LOAD_GAME,
	'v', // PC_VIEW_README,
	'q', // PC_QUIT,
	'm', // PC_MULTIPLAYER,
	'e', // PC_EASY,
	'm', // PC_MEDIUM,
	'd', // PC_DIFFICULT,
	's', // PC_SAVE_GAME,
	'l', // PC_LOAD_GAME2,
	'c', // PC_CANCEL,
	'g', // PC_GAME_OPTIONS,
	'v', // PC_VIDEO,
	'a', // PC_AUDIO,
	'c', // PC_CONTROLS,
	'd', // PC_DONE,
	'q', // PC_QUICKSTART,
	'u', // PC_USE_DEFAULTS,
	'o', // PC_OK,
	'v', // PC_VID_OPT,
	'm', // PC_VID_MODES,
	'a', // PC_ADV_CONTROLS,
	'o', // PC_ORDER_HL,
	'd', // PC_DELETE,
	'i', // PC_INET_GAME,
	'h', // PC_CHAT_ROOMS,
	'l', // PC_LAN_GAME,
	'u', // PC_CUSTOMIZE,
	's', // PC_SKIP,
	'e', // PC_EXIT,
	'c', // PC_CONNECT,
	'r', // PC_REFRESH,
	'f', // PC_FILTER,
	'f', // PC_FILTER2,
	'c', // PC_CREATE,
	't', // PC_CREATE_GAME,
	'h', // PC_CHAT_ROOMS2,
	'l', // PC_LIST_ROOMS,
	's', // PC_SEARCH,
	's', // PC_SERVERS,
	'j', // PC_JOIN,
	'f', // PC_FIND,
	'r', // PC_CREATE_ROOM,
	'j', // PC_JOIN_GAME,
	's', // PC_SEARCH_GAMES,
	'f', // PC_FIND_GAME,
	't', // PC_START_GAME,
	'v', // PC_VIEW_GAME_INFO,
	'u', // PC_UPDATE,
	'a', // PC_ADD_SERVER,
	'd', // PC_DISCONNECT,
	'o', // PC_CONSOLE,	// a1ba: set to O
	'o', // PC_CONTENT_CONTROL,
	'u', // PC_UPDATE2,
	'w', // PC_VISIT_WON,
	'p', // PC_PREVIEWS,
	'a', // PC_ADV_OPT,
	0, // PC_3DINFO_SITE,
	'u', // PC_CUSTOM_GAME,
	'a', // PC_ACTIVATE,
	'i', // PC_INSTALL,
	'v', // PC_VISIT_WEB_SITE,
	'r', // PC_REFRESH_LIST,
	'e', // PC_DEACTIVATE,
	'a', // PC_ADV_OPT2,
	's', // PC_SPECTATE_GAME,
	'p', // PC_SPECTATE_GAMES,
};

CMenuPicButton::CMenuPicButton() : BaseClass()
{
	bEnableTransitions = true;
	eFocusAnimation = QM_HIGHLIGHTIFFOCUS;
	iFlags = QMF_DROPSHADOW;

	iFocusStartTime = 0;

	eTextAlignment = QM_LEFT;

	hPic = 0;
	hotkey = 0;
	button_id = -1;
	iOldState = BUTTON_NOFOCUS;
	m_iLastFocusTime = -512;
	bPulse = false;

	size = uiStatic.buttons_draw_size;

	SetCharSize( QM_DEFAULTFONT );
}

/*
=================
CMenuPicButton::Key
=================
*/
bool CMenuPicButton::KeyUp( int key )
{
	bool handled = false;

	if( UI::Key::IsEnter( key ) && !(iFlags & QMF_MOUSEONLY) )
		handled = true;
	else if( UI::Key::IsLeftMouse( key ) && ( iFlags & QMF_HASMOUSEFOCUS ) )
		handled = true;

	if( handled )
		_Event( QM_RELEASED );

	return handled;
}

void CMenuPicButton::_Event( int ev )
{
	BaseClass::_Event( ev );
	if( ev == QM_RELEASED )
	{
		PlayLocalSound( uiStatic.sounds[SND_LAUNCH] );
		CheckWindowChanged( );
	}
}

void CMenuPicButton::CheckWindowChanged()
{
	// parent is not a window, ignore
	if( !m_pParent->IsWindow())
		return;

	CMenuBaseWindow *parentWindow = (CMenuBaseWindow*)m_pParent;
	CMenuBaseWindow *newWindow = parentWindow->WindowStack()->Current();

	// menu is closed, ignore
	if( !newWindow )
		return;

	// no change, ignore
	if( parentWindow == newWindow )
		return;

	// parent and new are not a root windows, ignore
	if( !parentWindow->IsRoot() || !newWindow->IsRoot() )
		return;

	// decide transition direction
	if( FBitSet( parentWindow->iFlags, QMF_CLOSING ))
	{
		// our parent window is closing right now
		// play backward animation
		// Con_NPrintf( 10, "%s banner down", parentWindow->szName );

		CMenuFramework *f = (CMenuFramework*)parentWindow;
		f->PrepareBannerAnimation( CMenuFramework::ANIM_CLOSING, nullptr );
	}
	else
	{
		// new window overlaps parent window
		// play forward animation
		// Con_NPrintf( 10, "%s banner up", newWindow->szName );

		CMenuFramework *f = (CMenuFramework*)newWindow;
		f->PrepareBannerAnimation( CMenuFramework::ANIM_OPENING, this );
	}
}

bool CMenuPicButton::KeyDown( int key )
{
	bool handled = false;

	if( UI::Key::IsEnter( key ) && !(iFlags & QMF_MOUSEONLY) )
		handled = true;
	else if( UI::Key::IsLeftMouse( key ) && ( iFlags & QMF_HASMOUSEFOCUS ) )
		handled = true;

	if( handled )
		_Event( QM_PRESSED );

	return handled;
}

/*
=================
CMenuPicButton::Draw

CS 1.6 PC flat skin: dark filled rectangle, orange 1px border + orange text on
focus/hover/press. WON-style btns_main.bmp bitmap and hHeavyBlur/hLightBlur
fancy paths are intentionally bypassed — this control now renders the same
across all detail levels and platforms.
=================
*/
void CMenuPicButton::Draw( )
{
	int state = BUTTON_NOFOCUS;

	if( iFlags & ( QMF_HASMOUSEFOCUS | QMF_HASKEYBOARDFOCUS ) )
		state = BUTTON_FOCUS;

	if( m_bPressed )
		state = BUTTON_PRESSED;

	if( iOldState == BUTTON_NOFOCUS && state != BUTTON_NOFOCUS )
		iFocusStartTime = uiStatic.realTime;

	// CS 1.6 PC palette
	const unsigned int bgColor      = 0xC0101010;
	const unsigned int bgColorHover = 0xC0202020;
	const unsigned int bgColorPress = 0xC0303030;
	const unsigned int borderColor  = 0xFFFFA000;
	const unsigned int textColor    = 0xFFC0C0C0;
	const unsigned int textHi       = 0xFFFFA000;

	// background fill
	unsigned int bg = bgColor;
	if( state == BUTTON_FOCUS )   bg = bgColorHover;
	if( state == BUTTON_PRESSED ) bg = bgColorPress;
	UI_FillRect( m_scPos, m_scSize, bg );

	// 1px orange border on focus/hover/press (skip when grayed)
	if( state != BUTTON_NOFOCUS && !( iFlags & QMF_GRAYED ) )
		UI_DrawRectangleExt( m_scPos, m_scSize, borderColor, 1 );

	// notify status hint to the right of the button
	if( szStatusText && FBitSet( iFlags, QMF_NOTIFY ) && !FBitSet( gMenu.m_gameinfo.flags, GFL_NOSKILLS ) )
	{
		Point coord;

		coord.x = m_scPos.x + m_scSize.w + 40 * uiStatic.scaleX;
		coord.y = m_scPos.y + m_scSize.h / 2 - EngFuncs::ConsoleCharacterHeight() / 2;

		int	r, g, b;

		UnpackRGB( r, g, b, uiColorHelp );
		EngFuncs::DrawSetTextColor( r, g, b );
		EngFuncs::DrawConsoleString( coord, szStatusText );
	}

	// text — left-aligned with 10px padding
	Point textPos  = m_scPos;
	Size  textSize = m_scSize;
	int padX = (int)( 10 * uiStatic.scaleX );
	if( textSize.w > padX * 2 )
	{
		textPos.x  += padX;
		textSize.w -= padX * 2;
	}

	uint textflags = ETF_NOSIZELIMIT | ETF_FORCECOL;
	if( iFlags & QMF_DROPSHADOW )
		SetBits( textflags, ETF_SHADOW );

	unsigned int color;
	if( iFlags & QMF_GRAYED )
		color = uiColorDkGrey;
	else if( state != BUTTON_NOFOCUS )
		color = textHi;
	else if( bPulse || eFocusAnimation == QM_PULSEIFFOCUS )
	{
		float pulsar = 0.5f + 0.5f * sin( (float)uiStatic.realTime / UI_PULSE_DIVISOR );
		color = InterpColor( textColor, textHi, pulsar );
	}
	else
		color = textColor;

	UI_DrawString( font, textPos, textSize, szName, color, m_scChSize, eTextAlignment, textflags );

	iOldState = state;
}

void CMenuPicButton::SetPicture( EDefaultBtns ID )
{
	if( ID < 0 || ID > PC_BUTTONCOUNT )
		return; // bad id

	hPic = uiStatic.btns.GetPic( ID );
	button_id = ID;
	hotkey = g_hotkeys[ID];
}

void CMenuPicButton::SetPicture( const char *filename, int hk )
{
	hPic = EngFuncs::PIC_Load( filename );
	hotkey = hk;
}

bool CMenuPicButton::HotKey( int key )
{
	return hotkey == key;
}
