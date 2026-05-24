/*
WndConsole.cpp -- windowed developer console (CS 1.6 PC style)
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "Framework.h"
#include "Action.h"
#include "Field.h"
#include "PicButton.h"
#include "Window.h"
#include "WindowStyle.h"

// ============================================================
// CMenuWndConsole — windowed console with input field + send btn
// Matches CS 1.6 PC style console window (see reference screenshots)
// ============================================================
class CMenuWndConsole : public CMenuWindow
{
public:
	typedef CMenuWindow BaseClass;
	CMenuWndConsole() : CMenuWindow( "Console" ) {}

private:
	void _Init() override;
	void _VidInit() override;
	void SubmitCommand();

	CMenuField m_fldInput;
	CMenuPicButton m_btnSend;
	CMenuAction m_lblHint;
};

void CMenuWndConsole::_Init()
{
	SetRect( 50, 50, 600, 380 );

	int contentW = 600 - WndStyle::BorderWidth * 2;
	int contentH = 380 - WndStyle::TitleBarHeight - WndStyle::BorderWidth * 2;

	// Hint label (console output is in engine, we just provide input)
	m_lblHint.szName = "Console output appears in the engine overlay (` key).";
	m_lblHint.iFlags |= QMF_INACTIVE;
	m_lblHint.SetCoord( 10, 10 );
	AddItem( m_lblHint );

	// Input field at bottom
	int fieldY = contentH - 40;
	m_fldInput.szName = "";
	m_fldInput.iMaxLength = 255;
	m_fldInput.SetCoord( 10, fieldY );
	m_fldInput.size.w = contentW - 100;
	m_fldInput.size.h = 32;
	AddItem( m_fldInput );

	// Send button
	m_btnSend.SetNameAndStatus( "Send", "Execute command" );
	m_btnSend.SetCoord( contentW - 80, fieldY );
	m_btnSend.size.w = 70;
	m_btnSend.size.h = 32;
	m_btnSend.onReleased = VoidCb( &CMenuWndConsole::SubmitCommand );
	AddItem( m_btnSend );
}

void CMenuWndConsole::_VidInit()
{
	m_fldInput.Clear();
}

void CMenuWndConsole::SubmitCommand()
{
	const char *cmd = m_fldInput.GetBuffer();
	if( cmd && cmd[0] )
	{
		EngFuncs::ClientCmd( false, cmd );
		EngFuncs::ClientCmd( false, "\n" );
		m_fldInput.Clear();
	}
}

// ---------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------
static CMenuWndConsole *g_pWndConsole = NULL;

static void UI_WndConsole_Precache( void )
{
	g_pWndConsole = new CMenuWndConsole();
}

static void UI_WndConsole_Shutdown( void )
{
	delete g_pWndConsole;
	g_pWndConsole = NULL;
}

void UI_WndConsole_Menu( void )
{
	if( g_pWndConsole )
		g_pWndConsole->Show();
}

ADD_MENU4( menu_wnd_console, UI_WndConsole_Precache, UI_WndConsole_Menu, UI_WndConsole_Shutdown );
