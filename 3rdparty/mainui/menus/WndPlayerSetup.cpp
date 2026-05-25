/*
WndPlayerSetup.cpp -- Player Setup window (CS 1.6 PC style)
Copyright (C) 2024 DragonSlayer contributors
*/

#include "Framework.h"
#include "Action.h"
#include "PicButton.h"
#include "Field.h"
#include "SpinControl.h"
#include "StringArrayModel.h"
#include "Window.h"
#include "WindowStyle.h"

// Forward decl from Multiplayer Advanced dialog
extern void UI_WndMultiplayerAdvanced_Menu( void );

static const char *s_modelList[] = {
	"gign", "gsg9", "sas", "urban", "leet", "arctic", "guerilla", "terror"
};
static const char *s_logoList[] = {
	"8ball1", "8ball2", "scared", "scull", "kc", "smiley", "thumbsdn", "thumbsup"
};

class CMenuWndPlayerSetup : public CMenuWindow
{
public:
	typedef CMenuWindow BaseClass;
	CMenuWndPlayerSetup()
		: CMenuWindow( "Player Setup" ),
		  m_modelArr( s_modelList, 8 ),
		  m_logoArr( s_logoList, 8 )
	{}

private:
	void _Init() override;
	void _VidInit() override;
	void OnOK();
	void OnApply();
	void OnAdvanced();

	CStringArrayModel m_modelArr;
	CStringArrayModel m_logoArr;

	// Left side: avatar + model selector
	CMenuAction    m_lblAvatar;
	CMenuPicButton m_btnLoadAvatar;
	CMenuSpinControl m_spinModel;

	// Logo group
	CMenuAction    m_lblLogo;
	CMenuSpinControl m_spinLogo;
	CMenuPicButton m_btnLogoColor;
	CMenuAction    m_lblLogoHint;

	// Right side
	CMenuAction m_lblName;
	CMenuField  m_fldName;
	CMenuAction m_lblPassword;
	CMenuField  m_fldPassword;

	CMenuPicButton m_btnAdvanced;

	// Bottom
	CMenuPicButton m_btnOK;
	CMenuPicButton m_btnCancel;
	CMenuPicButton m_btnApply;
};

void CMenuWndPlayerSetup::_Init()
{
	SetRect( 200, 80, 700, 460 );

	int leftCol = 20;
	int rightCol = 360;
	int row = 30;

	// ===== LEFT: Avatar =====
	m_lblAvatar.szName = L( "Avatar" );
	m_lblAvatar.iFlags |= QMF_INACTIVE;
	m_lblAvatar.SetCoord( leftCol, row );
	AddItem( m_lblAvatar );

	row += 20;
	m_btnLoadAvatar.SetNameAndStatus( "Load...", "Load custom avatar" );
	m_btnLoadAvatar.SetCoord( leftCol, row );
	m_btnLoadAvatar.size.w = 120;
	m_btnLoadAvatar.size.h = 28;
	m_btnLoadAvatar.onReleased.SetCommand( false, "menu_filedialog\n" );
	AddItem( m_btnLoadAvatar );

	row += 40;
	m_spinModel.Setup( &m_modelArr );
	m_spinModel.SetCoord( leftCol, row );
	m_spinModel.size.w = 200;
	m_spinModel.size.h = 28;
	AddItem( m_spinModel );

	// ===== LEFT: Logo =====
	row += 60;
	m_lblLogo.szName = L( "Logo" );
	m_lblLogo.iFlags |= QMF_INACTIVE;
	m_lblLogo.SetCoord( leftCol, row );
	AddItem( m_lblLogo );

	row += 20;
	m_spinLogo.Setup( &m_logoArr );
	m_spinLogo.SetCoord( leftCol, row );
	m_spinLogo.size.w = 200;
	m_spinLogo.size.h = 28;
	AddItem( m_spinLogo );

	row += 40;
	m_btnLogoColor.SetNameAndStatus( "Change color", "Change logo color" );
	m_btnLogoColor.SetCoord( leftCol, row );
	m_btnLogoColor.size.w = 140;
	m_btnLogoColor.size.h = 28;
	AddItem( m_btnLogoColor );

	row += 40;
	m_lblLogoHint.szName = L( "Logo updates after\nconnecting to a server." );
	m_lblLogoHint.iFlags |= QMF_INACTIVE;
	m_lblLogoHint.SetCoord( leftCol, row );
	AddItem( m_lblLogoHint );

	// ===== RIGHT: Name + Password =====
	int rrow = 30;
	m_lblName.szName = L( "Player name" );
	m_lblName.iFlags |= QMF_INACTIVE;
	m_lblName.SetCoord( rightCol, rrow );
	AddItem( m_lblName );

	rrow += 20;
	m_fldName.iMaxLength = 32;
	m_fldName.SetCoord( rightCol, rrow );
	m_fldName.size.w = 280;
	m_fldName.size.h = 32;
	AddItem( m_fldName );

	rrow += 60;
	m_lblPassword.szName = L( "VIP/Admin password" );
	m_lblPassword.iFlags |= QMF_INACTIVE;
	m_lblPassword.SetCoord( rightCol, rrow );
	AddItem( m_lblPassword );

	rrow += 20;
	m_fldPassword.iMaxLength = 64;
	m_fldPassword.bHideInput = true;
	m_fldPassword.SetCoord( rightCol, rrow );
	m_fldPassword.size.w = 280;
	m_fldPassword.size.h = 32;
	AddItem( m_fldPassword );

	// Advanced button (left side, low)
	m_btnAdvanced.SetNameAndStatus( "Advanced...", "Multiplayer advanced settings" );
	m_btnAdvanced.SetCoord( leftCol, 320 );
	m_btnAdvanced.size.w = 140;
	m_btnAdvanced.size.h = 32;
	m_btnAdvanced.onReleased = VoidCb( &CMenuWndPlayerSetup::OnAdvanced );
	AddItem( m_btnAdvanced );

	// ===== Bottom button bar =====
	int btnY = 460 - 50;
	int btnW = 90;
	m_btnOK.SetNameAndStatus( "OK", "Apply and close" );
	m_btnOK.SetCoord( 700 - btnW * 3 - 30, btnY );
	m_btnOK.size.w = btnW;
	m_btnOK.size.h = 32;
	m_btnOK.onReleased = VoidCb( &CMenuWndPlayerSetup::OnOK );
	AddItem( m_btnOK );

	m_btnCancel.SetNameAndStatus( "Cancel", "Discard" );
	m_btnCancel.SetCoord( 700 - btnW * 2 - 20, btnY );
	m_btnCancel.size.w = btnW;
	m_btnCancel.size.h = 32;
	m_btnCancel.onReleased = VoidCb( &CMenuWindow::Hide );
	AddItem( m_btnCancel );

	m_btnApply.SetNameAndStatus( "Apply", "Apply without closing" );
	m_btnApply.SetCoord( 700 - btnW - 10, btnY );
	m_btnApply.size.w = btnW;
	m_btnApply.size.h = 32;
	m_btnApply.onReleased = VoidCb( &CMenuWndPlayerSetup::OnApply );
	AddItem( m_btnApply );
}

void CMenuWndPlayerSetup::_VidInit()
{
	m_fldName.LinkCvar( "name" );
	m_fldPassword.LinkCvar( "password" );
}

void CMenuWndPlayerSetup::OnApply()
{
	m_fldName.WriteCvar();
	m_fldPassword.WriteCvar();
}

void CMenuWndPlayerSetup::OnOK()
{
	OnApply();
	Hide();
}

void CMenuWndPlayerSetup::OnAdvanced()
{
	UI_WndMultiplayerAdvanced_Menu();
}

// ---------------------------------------------------------------
static CMenuWndPlayerSetup *g_pWndPS = NULL;

static void UI_WndPS_Precache( void ) { g_pWndPS = new CMenuWndPlayerSetup(); }
static void UI_WndPS_Shutdown( void ) { delete g_pWndPS; g_pWndPS = NULL; }
void UI_WndPlayerSetup_Menu( void ) { if( g_pWndPS ) g_pWndPS->Show(); }

ADD_MENU4( menu_wnd_playersetup, UI_WndPS_Precache, UI_WndPlayerSetup_Menu, UI_WndPS_Shutdown );
