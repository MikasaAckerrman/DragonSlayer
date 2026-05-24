/*
WndServerBrowser.cpp -- windowed server browser with tabs (CS 1.6 PC style)
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
#include "Window.h"
#include "TabControl.h"
#include "WindowStyle.h"

// ============================================================
// Tab page holder
// ============================================================
class CServerBrowserPage : public CMenuItemsHolder
{
public:
	CServerBrowserPage() {}
	void _Init() override {}
	void _VidInit() override {}
};

// ============================================================
// CMenuWndServerBrowser — windowed server browser
// Tabs: Internet / Favorites / History / Spectate / LAN / Friends
// ============================================================
class CMenuWndServerBrowser : public CMenuWindow
{
public:
	typedef CMenuWindow BaseClass;
	CMenuWndServerBrowser() : CMenuWindow( "Servers" ) {}

private:
	void _Init() override;
	void _VidInit() override;

	CMenuTabControl m_tabControl;

	CServerBrowserPage m_pageInternet;
	CServerBrowserPage m_pageFavorites;
	CServerBrowserPage m_pageHistory;
	CServerBrowserPage m_pageSpectate;
	CServerBrowserPage m_pageLAN;
	CServerBrowserPage m_pageFriends;

	// Placeholder labels
	CMenuAction m_lblInternet;
	CMenuAction m_lblFavorites;
	CMenuAction m_lblHistory;
	CMenuAction m_lblSpectate;
	CMenuAction m_lblLAN;
	CMenuAction m_lblFriends;
};

void CMenuWndServerBrowser::_Init()
{
	SetRect( 60, 80, 750, 480 );

	int contentW = 750 - WndStyle::BorderWidth * 2;
	m_tabControl.SetRect( 0, 0, contentW, 480 );
	m_tabControl.AddTab( "Internet",  &m_pageInternet );
	m_tabControl.AddTab( "Favorites", &m_pageFavorites );
	m_tabControl.AddTab( "History",   &m_pageHistory );
	m_tabControl.AddTab( "Spectate",  &m_pageSpectate );
	m_tabControl.AddTab( "LAN",       &m_pageLAN );
	m_tabControl.AddTab( "Friends",   &m_pageFriends );
	AddItem( m_tabControl );

	// Placeholders
	struct { CMenuAction *lbl; CServerBrowserPage *page; const char *text; } tabs[] = {
		{ &m_lblInternet,  &m_pageInternet,  "Internet servers (TODO)" },
		{ &m_lblFavorites, &m_pageFavorites, "Favorite servers (TODO)" },
		{ &m_lblHistory,   &m_pageHistory,   "History (TODO)" },
		{ &m_lblSpectate,  &m_pageSpectate,  "Spectate (TODO)" },
		{ &m_lblLAN,       &m_pageLAN,       "LAN servers (TODO)" },
		{ &m_lblFriends,   &m_pageFriends,   "Friends (TODO)" },
	};

	for( int i = 0; i < 6; i++ )
	{
		tabs[i].lbl->szName = tabs[i].text;
		tabs[i].lbl->iFlags |= QMF_INACTIVE;
		tabs[i].lbl->SetCoord( 20, 40 );
		tabs[i].page->AddItem( *tabs[i].lbl );
	}

	AddItem( m_pageInternet );
	AddItem( m_pageFavorites );
	AddItem( m_pageHistory );
	AddItem( m_pageSpectate );
	AddItem( m_pageLAN );
	AddItem( m_pageFriends );
}

void CMenuWndServerBrowser::_VidInit()
{
}

// ---------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------
static CMenuWndServerBrowser *g_pWndServers = NULL;

static void UI_WndServerBrowser_Precache( void )
{
	g_pWndServers = new CMenuWndServerBrowser();
}

static void UI_WndServerBrowser_Shutdown( void )
{
	delete g_pWndServers;
	g_pWndServers = NULL;
}

void UI_WndServerBrowser_Menu( void )
{
	if( g_pWndServers )
		g_pWndServers->Show();
}

ADD_MENU4( menu_wnd_servers, UI_WndServerBrowser_Precache, UI_WndServerBrowser_Menu, UI_WndServerBrowser_Shutdown );
