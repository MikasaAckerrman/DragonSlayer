/*
WndMultiplayerAdvanced.cpp -- Multiplayer Advanced dialog (CS 1.6 PC style)
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "Framework.h"
#include "Action.h"
#include "PicButton.h"
#include "CheckBox.h"
#include "SpinControl.h"
#include "StringArrayModel.h"
#include "Window.h"
#include "WindowStyle.h"

static const char *s_smokeColors[] = { "Default", "Red", "Green", "Blue", "Yellow" };
static const char *s_handList[]    = { "Right hand", "Left hand" };
static const char *s_buyMenuList[] = { "Old-style menu", "New-style menu" };

class CMenuWndMultiplayerAdvanced : public CMenuWindow
{
public:
	typedef CMenuWindow BaseClass;
	CMenuWndMultiplayerAdvanced()
		: CMenuWindow( "Multiplayer - Advanced" ),
		  m_smokeColors( s_smokeColors, 5 ),
		  m_handModel( s_handList, 2 ),
		  m_buyMenuModel( s_buyMenuList, 2 )
	{}

private:
	void _Init() override;
	void _VidInit() override;
	void OnYes();

	CStringArrayModel m_smokeColors;
	CStringArrayModel m_handModel;
	CStringArrayModel m_buyMenuModel;

	CMenuSpinControl m_spinSmokeColor;
	CMenuSpinControl m_spinHand;
	CMenuSpinControl m_spinBuyMenu;

	CMenuAction m_lblSmoke;
	CMenuAction m_lblHand;
	CMenuAction m_lblBuy;

	CMenuCheckBox m_chkAutoSwitchPower;
	CMenuCheckBox m_chkCenterNames;
	CMenuCheckBox m_chkFriendNames;
	CMenuCheckBox m_chkAutoHelp;
	CMenuCheckBox m_chkHideBlood;

	CMenuPicButton m_btnYes;
	CMenuPicButton m_btnCancel;
};

void CMenuWndMultiplayerAdvanced::_Init()
{
	SetRect( 200, 100, 480, 460 );

	int col = 20;
	int valX = 240;
	int valW = 200;
	int row = 30;

	m_lblSmoke.szName = L( "Smoke grenade color" );
	m_lblSmoke.iFlags |= QMF_INACTIVE;
	m_lblSmoke.SetCoord( col, row );
	AddItem( m_lblSmoke );
	m_spinSmokeColor.Setup( &m_smokeColors );
	m_spinSmokeColor.SetCoord( valX, row );
	m_spinSmokeColor.size.w = valW;
	m_spinSmokeColor.size.h = 28;
	AddItem( m_spinSmokeColor );

	row += 40;
	m_lblHand.szName = L( "Weapon hand" );
	m_lblHand.iFlags |= QMF_INACTIVE;
	m_lblHand.SetCoord( col, row );
	AddItem( m_lblHand );
	m_spinHand.Setup( &m_handModel );
	m_spinHand.SetCoord( valX, row );
	m_spinHand.size.w = valW;
	m_spinHand.size.h = 28;
	AddItem( m_spinHand );

	row += 40;
	m_lblBuy.szName = L( "Buy menu type" );
	m_lblBuy.iFlags |= QMF_INACTIVE;
	m_lblBuy.SetCoord( col, row );
	AddItem( m_lblBuy );
	m_spinBuyMenu.Setup( &m_buyMenuModel );
	m_spinBuyMenu.SetCoord( valX, row );
	m_spinBuyMenu.size.w = valW;
	m_spinBuyMenu.size.h = 28;
	AddItem( m_spinBuyMenu );

	row += 50;
	m_chkAutoSwitchPower.szName = L( "Auto-switch to picked powerful weapon" );
	m_chkAutoSwitchPower.SetCoord( col, row );
	AddItem( m_chkAutoSwitchPower );

	row += 35;
	m_chkCenterNames.szName = L( "Player names in center" );
	m_chkCenterNames.SetCoord( col, row );
	AddItem( m_chkCenterNames );

	row += 35;
	m_chkFriendNames.szName = L( "Friend names above team" );
	m_chkFriendNames.SetCoord( col, row );
	AddItem( m_chkFriendNames );

	row += 35;
	m_chkAutoHelp.szName = L( "Auto-help" );
	m_chkAutoHelp.SetCoord( col, row );
	AddItem( m_chkAutoHelp );

	row += 35;
	m_chkHideBlood.szName = L( "Hide blood" );
	m_chkHideBlood.SetCoord( col, row );
	AddItem( m_chkHideBlood );

	int btnY = 460 - 50;
	m_btnYes.SetNameAndStatus( "Yes", "Apply and close" );
	m_btnYes.SetCoord( 480 - 200, btnY );
	m_btnYes.size.w = 90;
	m_btnYes.size.h = 32;
	m_btnYes.onReleased = VoidCb( &CMenuWndMultiplayerAdvanced::OnYes );
	AddItem( m_btnYes );

	m_btnCancel.SetNameAndStatus( "Cancel", "Discard" );
	m_btnCancel.SetCoord( 480 - 100, btnY );
	m_btnCancel.size.w = 90;
	m_btnCancel.size.h = 32;
	m_btnCancel.onReleased = VoidCb( &CMenuWindow::Hide );
	AddItem( m_btnCancel );
}

void CMenuWndMultiplayerAdvanced::_VidInit()
{
	m_chkAutoSwitchPower.LinkCvar( "cl_autowepswitch" );
	m_chkCenterNames.LinkCvar( "hud_centerid" );
	m_chkAutoHelp.LinkCvar( "cl_autohelp" );
	m_chkHideBlood.LinkCvar( "violence_hblood" );
}

void CMenuWndMultiplayerAdvanced::OnYes()
{
	m_chkAutoSwitchPower.WriteCvar();
	m_chkCenterNames.WriteCvar();
	m_chkAutoHelp.WriteCvar();
	m_chkHideBlood.WriteCvar();
	Hide();
}

// ---------------------------------------------------------------
static CMenuWndMultiplayerAdvanced *g_pWndMpAdv = NULL;

static void UI_WndMpAdv_Precache( void ) { g_pWndMpAdv = new CMenuWndMultiplayerAdvanced(); }
static void UI_WndMpAdv_Shutdown( void ) { delete g_pWndMpAdv; g_pWndMpAdv = NULL; }
void UI_WndMultiplayerAdvanced_Menu( void ) { if( g_pWndMpAdv ) g_pWndMpAdv->Show(); }

ADD_MENU4( menu_wnd_mpadv, UI_WndMpAdv_Precache, UI_WndMultiplayerAdvanced_Menu, UI_WndMpAdv_Shutdown );
