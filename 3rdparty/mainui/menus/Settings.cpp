/*
Settings.cpp -- windowed settings dialog with tabs (CS 1.6 PC style)
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "Framework.h"
#include "Bitmap.h"
#include "PicButton.h"
#include "Slider.h"
#include "CheckBox.h"
#include "SpinControl.h"
#include "Action.h"
#include "Field.h"
#include "StringArrayModel.h"
#include "Window.h"
#include "TabControl.h"
#include "WindowStyle.h"

// ============================================================
// Tab page holder
// ============================================================
class CSettingsPage : public CMenuItemsHolder
{
public:
	CSettingsPage() { }
	void _Init() override {}
	void _VidInit() override {}
};

// ============================================================
// CMenuSettings — windowed settings with tabs:
//   Multiplayer / Keyboard / Mouse / Audio / Video / Voice
// ============================================================
class CMenuSettings : public CMenuWindow
{
public:
	typedef CMenuWindow BaseClass;
	CMenuSettings() : CMenuWindow( "Settings" ) {}

private:
	void _Init() override;
	void _VidInit() override;
	void SaveAndPopMenu() override;

	CMenuTabControl m_tabControl;

	// Tab pages
	CSettingsPage m_pageMultiplayer;
	CSettingsPage m_pageKeyboard;
	CSettingsPage m_pageMouse;
	CSettingsPage m_pageAudio;
	CSettingsPage m_pageVideo;
	CSettingsPage m_pageVoice;
	CSettingsPage m_pageHUD;
	CSettingsPage m_pageSystem;

	// Bottom button bar
	CMenuPicButton m_btnOK;
	CMenuPicButton m_btnCancel;
	CMenuPicButton m_btnApply;

	// --- Multiplayer page ---
	CMenuField    m_fldPlayerName;
	CMenuAction   m_lblModelHint;

	// --- Keyboard page ---
	CMenuAction   m_lblKeyboardHint;

	// --- Mouse page ---
	CMenuSlider   m_sldSensitivity;
	CMenuCheckBox m_chkInvertMouse;
	CMenuCheckBox m_chkRawInput;

	// --- Audio page ---
	CMenuSlider   m_sldSoundVol;
	CMenuSlider   m_sldMusicVol;
	CMenuSlider   m_sldSuitVol;
	CMenuCheckBox m_chkNoDSP;
	CMenuCheckBox m_chkMuteLostFocus;

	// --- Video page ---
	CMenuCheckBox m_chkFullscreen;
	CMenuCheckBox m_chkVsync;

	// --- Voice page ---
	CMenuCheckBox m_chkVoiceEnable;
	CMenuSlider   m_sldVoiceScale;

	// --- HUD page ---
	CMenuSlider   m_sldCrosshairSize;
	CMenuCheckBox m_chkFastSwitch;
	CMenuCheckBox m_chkCenterID;
	CMenuCheckBox m_chkAutoWepSwitch;

	// --- System page ---
	CMenuCheckBox m_chkDeveloper;
	CMenuField    m_fldFpsMax;
	CMenuCheckBox m_chkNetGraph;

	void OnApply();
	void OnOK();
	void OnCancel();
};

// ---------------------------------------------------------------
// Init
// ---------------------------------------------------------------
void CMenuSettings::_Init()
{
	SetRect( 80, 60, 700, 500 );

	int contentW = 700 - WndStyle::BorderWidth * 2;
	int pageH = 500 - WndStyle::TabHeight - 50; // area below tabs, above bottom buttons
	// Tab control covers only the header row
	m_tabControl.SetRect( 0, 0, contentW, WndStyle::TabHeight );
	m_tabControl.AddTab( "Multiplayer", &m_pageMultiplayer );
	m_tabControl.AddTab( "Keyboard",    &m_pageKeyboard );
	m_tabControl.AddTab( "Mouse",       &m_pageMouse );
	m_tabControl.AddTab( "Audio",       &m_pageAudio );
	m_tabControl.AddTab( "Video",       &m_pageVideo );
	m_tabControl.AddTab( "Voice",       &m_pageVoice );
	m_tabControl.AddTab( "HUD",         &m_pageHUD );
	m_tabControl.AddTab( "System",      &m_pageSystem );
	AddItem( m_tabControl );

	int col = 20;
	int slW = 280;

	// ===== Multiplayer =====
	m_fldPlayerName.szName = L( "GameUI_PlayerName" );
	m_fldPlayerName.iMaxLength = 32;
	m_fldPlayerName.SetCoord( col, 40 );
	m_fldPlayerName.size.w = 260;
	m_fldPlayerName.size.h = 32;
	m_pageMultiplayer.AddItem( m_fldPlayerName );

	m_lblModelHint.szName = L( "Model selection: use 'model' console command" );
	m_lblModelHint.iFlags |= QMF_INACTIVE;
	m_lblModelHint.SetCoord( col, 100 );
	m_pageMultiplayer.AddItem( m_lblModelHint );

	// ===== Keyboard =====
	m_lblKeyboardHint.szName = L( "Use 'bind' console command or Controls menu for key bindings" );
	m_lblKeyboardHint.iFlags |= QMF_INACTIVE;
	m_lblKeyboardHint.SetCoord( col, 40 );
	m_pageKeyboard.AddItem( m_lblKeyboardHint );

	// ===== Mouse =====
	m_sldSensitivity.szName = L( "GameUI_Sensitivity" );
	m_sldSensitivity.Setup( 0.1f, 20.0f, 0.1f );
	m_sldSensitivity.SetCoord( col, 40 );
	m_sldSensitivity.size.w = slW;
	m_sldSensitivity.onChanged = CMenuEditable::WriteCvarCb;
	m_pageMouse.AddItem( m_sldSensitivity );

	m_chkInvertMouse.szName = L( "GameUI_ReverseMouse" );
	m_chkInvertMouse.SetCoord( col, 100 );
	m_chkInvertMouse.onChanged = CMenuEditable::WriteCvarCb;
	m_pageMouse.AddItem( m_chkInvertMouse );

	m_chkRawInput.szName = L( "Raw mouse input" );
	m_chkRawInput.SetCoord( col, 140 );
	m_chkRawInput.onChanged = CMenuEditable::WriteCvarCb;
	m_pageMouse.AddItem( m_chkRawInput );

	// ===== Audio =====
	int row = 40;
	m_sldSoundVol.szName = L( "GameUI_SoundEffectVolume" );
	m_sldSoundVol.Setup( 0.0, 1.0, 0.05f );
	m_sldSoundVol.SetCoord( col, row );
	m_sldSoundVol.size.w = slW;
	m_sldSoundVol.onChanged = CMenuEditable::WriteCvarCb;
	m_pageAudio.AddItem( m_sldSoundVol );

	row += 50;
	m_sldMusicVol.szName = L( "GameUI_MP3Volume" );
	m_sldMusicVol.Setup( 0.0, 1.0, 0.05f );
	m_sldMusicVol.SetCoord( col, row );
	m_sldMusicVol.size.w = slW;
	m_sldMusicVol.onChanged = CMenuEditable::WriteCvarCb;
	m_pageAudio.AddItem( m_sldMusicVol );

	row += 50;
	m_sldSuitVol.szName = L( "GameUI_HEVSuitVolume" );
	m_sldSuitVol.Setup( 0.0, 1.0, 0.05f );
	m_sldSuitVol.SetCoord( col, row );
	m_sldSuitVol.size.w = slW;
	m_sldSuitVol.onChanged = CMenuEditable::WriteCvarCb;
	m_pageAudio.AddItem( m_sldSuitVol );

	row += 50;
	m_chkNoDSP.szName = L( "Disable DSP effects" );
	m_chkNoDSP.SetCoord( col, row );
	m_chkNoDSP.onChanged = CMenuEditable::WriteCvarCb;
	m_pageAudio.AddItem( m_chkNoDSP );

	row += 40;
	m_chkMuteLostFocus.szName = L( "Mute when inactive" );
	m_chkMuteLostFocus.SetCoord( col, row );
	m_chkMuteLostFocus.onChanged = CMenuEditable::WriteCvarCb;
	m_pageAudio.AddItem( m_chkMuteLostFocus );

	// ===== Video =====
	m_chkFullscreen.szName = L( "Fullscreen" );
	m_chkFullscreen.SetCoord( col, 40 );
	m_chkFullscreen.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_chkFullscreen );

	m_chkVsync.szName = L( "Vertical sync" );
	m_chkVsync.SetCoord( col, 80 );
	m_chkVsync.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_chkVsync );

	// ===== Voice =====
	m_chkVoiceEnable.szName = L( "Enable voice chat" );
	m_chkVoiceEnable.SetCoord( col, 40 );
	m_chkVoiceEnable.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVoice.AddItem( m_chkVoiceEnable );

	m_sldVoiceScale.szName = L( "Voice receive volume" );
	m_sldVoiceScale.Setup( 0.0f, 5.0f, 0.1f );
	m_sldVoiceScale.SetCoord( col, 90 );
	m_sldVoiceScale.size.w = slW;
	m_sldVoiceScale.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVoice.AddItem( m_sldVoiceScale );

	// ===== HUD =====
	m_sldCrosshairSize.szName = L( "Crosshair size" );
	m_sldCrosshairSize.Setup( 0, 3, 1 );
	m_sldCrosshairSize.SetCoord( col, 40 );
	m_sldCrosshairSize.size.w = slW;
	m_sldCrosshairSize.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_sldCrosshairSize );

	m_chkFastSwitch.szName = L( "Fast weapon switch" );
	m_chkFastSwitch.SetCoord( col, 100 );
	m_chkFastSwitch.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_chkFastSwitch );

	m_chkCenterID.szName = L( "Player names in center" );
	m_chkCenterID.SetCoord( col, 140 );
	m_chkCenterID.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_chkCenterID );

	m_chkAutoWepSwitch.szName = L( "Auto-switch to picked weapon" );
	m_chkAutoWepSwitch.SetCoord( col, 180 );
	m_chkAutoWepSwitch.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_chkAutoWepSwitch );

	// ===== System =====
	m_chkDeveloper.szName = L( "Developer console" );
	m_chkDeveloper.SetCoord( col, 40 );
	m_chkDeveloper.onChanged = CMenuEditable::WriteCvarCb;
	m_pageSystem.AddItem( m_chkDeveloper );

	m_fldFpsMax.szName = L( "Max FPS" );
	m_fldFpsMax.iMaxLength = 5;
	m_fldFpsMax.bNumbersOnly = true;
	m_fldFpsMax.SetCoord( col, 100 );
	m_fldFpsMax.size.w = 100;
	m_fldFpsMax.size.h = 32;
	m_pageSystem.AddItem( m_fldFpsMax );

	m_chkNetGraph.szName = L( "Show net graph" );
	m_chkNetGraph.SetCoord( col, 160 );
	m_chkNetGraph.onChanged = CMenuEditable::WriteCvarCb;
	m_pageSystem.AddItem( m_chkNetGraph );

	// Position and size pages below tab header row
	m_pageMultiplayer.SetCoord( 0, WndStyle::TabHeight );
	m_pageMultiplayer.SetSize( contentW, pageH );
	m_pageKeyboard.SetCoord( 0, WndStyle::TabHeight );
	m_pageKeyboard.SetSize( contentW, pageH );
	m_pageMouse.SetCoord( 0, WndStyle::TabHeight );
	m_pageMouse.SetSize( contentW, pageH );
	m_pageAudio.SetCoord( 0, WndStyle::TabHeight );
	m_pageAudio.SetSize( contentW, pageH );
	m_pageVideo.SetCoord( 0, WndStyle::TabHeight );
	m_pageVideo.SetSize( contentW, pageH );
	m_pageVoice.SetCoord( 0, WndStyle::TabHeight );
	m_pageVoice.SetSize( contentW, pageH );
	m_pageHUD.SetCoord( 0, WndStyle::TabHeight );
	m_pageHUD.SetSize( contentW, pageH );
	m_pageSystem.SetCoord( 0, WndStyle::TabHeight );
	m_pageSystem.SetSize( contentW, pageH );

	// Add pages so they get Init/VidInit/Draw calls
	AddItem( m_pageMultiplayer );
	AddItem( m_pageKeyboard );
	AddItem( m_pageMouse );
	AddItem( m_pageAudio );
	AddItem( m_pageVideo );
	AddItem( m_pageVoice );
	AddItem( m_pageHUD );
	AddItem( m_pageSystem );

	// ===== Bottom button bar (OK / Cancel / Apply) =====
	int btnY = 500 - 42;
	int btnW = 90;
	int btnH = 32;
	m_btnOK.SetNameAndStatus( "OK", "Apply and close" );
	m_btnOK.SetCoord( contentW - btnW * 3 - 30, btnY );
	m_btnOK.size.w = btnW;
	m_btnOK.size.h = btnH;
	m_btnOK.onReleased = VoidCb( &CMenuSettings::OnOK );
	AddItem( m_btnOK );

	m_btnCancel.SetNameAndStatus( "Cancel", "Discard changes" );
	m_btnCancel.SetCoord( contentW - btnW * 2 - 20, btnY );
	m_btnCancel.size.w = btnW;
	m_btnCancel.size.h = btnH;
	m_btnCancel.onReleased = VoidCb( &CMenuSettings::OnCancel );
	AddItem( m_btnCancel );

	m_btnApply.SetNameAndStatus( "Apply", "Apply without closing" );
	m_btnApply.SetCoord( contentW - btnW - 10, btnY );
	m_btnApply.size.w = btnW;
	m_btnApply.size.h = btnH;
	m_btnApply.onReleased = VoidCb( &CMenuSettings::OnApply );
	AddItem( m_btnApply );
}

// ---------------------------------------------------------------
// VidInit
// ---------------------------------------------------------------
void CMenuSettings::_VidInit()
{
	// Multiplayer
	m_fldPlayerName.LinkCvar( "name" );

	// Mouse
	m_sldSensitivity.LinkCvar( "sensitivity" );
	m_chkInvertMouse.LinkCvar( "m_pitch" ); // negative = inverted
	m_chkRawInput.LinkCvar( "m_rawinput" );

	// Audio
	m_sldSoundVol.LinkCvar( "volume" );
	m_sldMusicVol.LinkCvar( "MP3Volume" );
	m_sldSuitVol.LinkCvar( "suitvolume" );
	m_chkNoDSP.LinkCvar( "room_off" );
	m_chkMuteLostFocus.LinkCvar( "snd_mute_losefocus" );

	// Video
	m_chkFullscreen.LinkCvar( "fullscreen" );
	m_chkVsync.LinkCvar( "gl_vsync" );

	// Voice
	m_chkVoiceEnable.LinkCvar( "voice_enable" );
	m_sldVoiceScale.LinkCvar( "voice_scale" );

	// HUD
	m_sldCrosshairSize.LinkCvar( "cl_crosshair_size" );
	m_chkFastSwitch.LinkCvar( "hud_fastswitch" );
	m_chkCenterID.LinkCvar( "hud_centerid" );
	m_chkAutoWepSwitch.LinkCvar( "cl_autowepswitch" );

	// System
	m_chkDeveloper.LinkCvar( "developer" );
	m_fldFpsMax.LinkCvar( "fps_max" );
	m_chkNetGraph.LinkCvar( "net_graph" );

	// Don't reset active tab on resolution change — let user keep their tab.
	// (Default tab is index 0 set in CMenuTabControl ctor.)
}

// ---------------------------------------------------------------
// Bottom button bar handlers
// ---------------------------------------------------------------
void CMenuSettings::OnApply()
{
	m_fldPlayerName.WriteCvar();
	m_sldSensitivity.WriteCvar();
	m_chkInvertMouse.WriteCvar();
	m_chkRawInput.WriteCvar();
	m_sldSoundVol.WriteCvar();
	m_sldMusicVol.WriteCvar();
	m_sldSuitVol.WriteCvar();
	m_chkNoDSP.WriteCvar();
	m_chkMuteLostFocus.WriteCvar();
	m_chkFullscreen.WriteCvar();
	m_chkVsync.WriteCvar();
	m_chkVoiceEnable.WriteCvar();
	m_sldVoiceScale.WriteCvar();
	m_sldCrosshairSize.WriteCvar();
	m_chkFastSwitch.WriteCvar();
	m_chkCenterID.WriteCvar();
	m_chkAutoWepSwitch.WriteCvar();
	m_chkDeveloper.WriteCvar();
	m_fldFpsMax.WriteCvar();
	m_chkNetGraph.WriteCvar();
}

void CMenuSettings::OnOK()
{
	OnApply();
	Hide();
}

void CMenuSettings::OnCancel()
{
	Hide(); // discard: don't write cvars
}

// ---------------------------------------------------------------
// Save (called from CMenuWindow on close — bridges to OnOK)
// ---------------------------------------------------------------
void CMenuSettings::SaveAndPopMenu()
{
	OnApply();
	CMenuWindow::SaveAndPopMenu();
}

// ---------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------
static CMenuSettings *g_pSettings = NULL;

static void UI_Settings_Precache( void )
{
	g_pSettings = new CMenuSettings();
}

static void UI_Settings_Shutdown( void )
{
	delete g_pSettings;
	g_pSettings = NULL;
}

void UI_Settings_Menu( void )
{
	if( g_pSettings )
		g_pSettings->Show();
}

ADD_MENU4( menu_settings, UI_Settings_Precache, UI_Settings_Menu, UI_Settings_Shutdown );
