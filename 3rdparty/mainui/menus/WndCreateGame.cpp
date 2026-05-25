/*
WndCreateGame.cpp -- windowed create game dialog with tabs (CS 1.6 PC style)
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "Framework.h"
#include "Bitmap.h"
#include "PicButton.h"
#include "Action.h"
#include "Field.h"
#include "CheckBox.h"
#include "Window.h"
#include "TabControl.h"
#include "WindowStyle.h"

// ============================================================
// Tab page holder
// ============================================================
class CCreateGamePage : public CMenuItemsHolder
{
public:
	CCreateGamePage() {}
	void _Init() override {}
	void _VidInit() override {}
};

// ============================================================
// CMenuWndCreateGame — windowed create game
// Tabs: Server / Game / Bot Settings
// ============================================================
class CMenuWndCreateGame : public CMenuWindow
{
public:
	typedef CMenuWindow BaseClass;
	CMenuWndCreateGame() : CMenuWindow( "Create Server" ) {}

private:
	void _Init() override;
	void _VidInit() override;

	CMenuTabControl m_tabControl;

	CCreateGamePage m_pageServer;
	CCreateGamePage m_pageGame;
	CCreateGamePage m_pageBots;

	// Server page widgets
	CMenuField m_fldHostName;
	CMenuField m_fldMaxPlayers;
	CMenuField m_fldPassword;
	CMenuCheckBox m_chkNAT;

	// Placeholder labels for other pages
	CMenuAction m_lblGame;
	CMenuAction m_lblBots;
};

void CMenuWndCreateGame::_Init()
{
	SetRect( 120, 70, 640, 460 );

	int contentW = 640 - WndStyle::BorderWidth * 2;
	int pageH = 460 - WndStyle::TabHeight; // area below tabs
	// Tab control covers only the header row
	m_tabControl.SetRect( 0, 0, contentW, WndStyle::TabHeight );
	m_tabControl.AddTab( "Server",       &m_pageServer );
	m_tabControl.AddTab( "Game",         &m_pageGame );
	m_tabControl.AddTab( "Bot Settings", &m_pageBots );
	AddItem( m_tabControl );

	// --- Server page ---
	int row = 40;
	int col = 20;

	m_fldHostName.szName = "Server Name:";
	m_fldHostName.SetCoord( col, row );
	m_fldHostName.size.w = 300;
	m_fldHostName.size.h = 32;
	m_pageServer.AddItem( m_fldHostName );

	row += 50;
	m_fldMaxPlayers.szName = "Max Players:";
	m_fldMaxPlayers.SetCoord( col, row );
	m_fldMaxPlayers.size.w = 100;
	m_fldMaxPlayers.size.h = 32;
	m_pageServer.AddItem( m_fldMaxPlayers );

	row += 50;
	m_fldPassword.szName = "Password:";
	m_fldPassword.SetCoord( col, row );
	m_fldPassword.size.w = 200;
	m_fldPassword.size.h = 32;
	m_pageServer.AddItem( m_fldPassword );

	row += 50;
	m_chkNAT.szName = "NAT Bypass";
	m_chkNAT.SetCoord( col, row );
	m_pageServer.AddItem( m_chkNAT );

	// --- Game page placeholder ---
	m_lblGame.szName = "Game rules (TODO: map list, timelimit, fraglimit)";
	m_lblGame.iFlags |= QMF_INACTIVE;
	m_lblGame.SetCoord( 20, 40 );
	m_pageGame.AddItem( m_lblGame );

	// --- Bots page placeholder ---
	m_lblBots.szName = "Bot settings (TODO: difficulty, quota)";
	m_lblBots.iFlags |= QMF_INACTIVE;
	m_lblBots.SetCoord( 20, 40 );
	m_pageBots.AddItem( m_lblBots );

	// Position and size pages below tab header row
	m_pageServer.SetCoord( 0, WndStyle::TabHeight );
	m_pageServer.SetSize( contentW, pageH );
	m_pageGame.SetCoord( 0, WndStyle::TabHeight );
	m_pageGame.SetSize( contentW, pageH );
	m_pageBots.SetCoord( 0, WndStyle::TabHeight );
	m_pageBots.SetSize( contentW, pageH );

	AddItem( m_pageServer );
	AddItem( m_pageGame );
	AddItem( m_pageBots );
}

void CMenuWndCreateGame::_VidInit()
{
	m_fldHostName.LinkCvar( "hostname" );
	m_fldMaxPlayers.LinkCvar( "maxplayers" );
	m_fldPassword.LinkCvar( "sv_password" );
	m_chkNAT.LinkCvar( "sv_nat" );
}

// ---------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------
static CMenuWndCreateGame *g_pWndCreateGame = NULL;

static void UI_WndCreateGame_Precache( void )
{
	g_pWndCreateGame = new CMenuWndCreateGame();
}

static void UI_WndCreateGame_Shutdown( void )
{
	delete g_pWndCreateGame;
	g_pWndCreateGame = NULL;
}

void UI_WndCreateGame_Menu( void )
{
	if( g_pWndCreateGame )
		g_pWndCreateGame->Show();
}

ADD_MENU4( menu_wnd_creategame, UI_WndCreateGame_Precache, UI_WndCreateGame_Menu, UI_WndCreateGame_Shutdown );
