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
#include "StringArrayModel.h"
#include "Window.h"
#include "TabControl.h"
#include "WindowStyle.h"

// ============================================================
// Tab page — a simple items holder for each settings category
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
//   Multiplayer / Keyboard / Mouse / Audio / Video / Voice / Lock
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

	// --- Audio page widgets ---
	CMenuSlider  m_sldSoundVol;
	CMenuSlider  m_sldMusicVol;
	CMenuSlider  m_sldSuitVol;
	CMenuCheckBox m_chkNoDSP;
	CMenuCheckBox m_chkMuteLostFocus;

	// --- Video page widgets ---
	CMenuCheckBox m_chkFullscreen;
	CMenuCheckBox m_chkVsync;

	// --- Multiplayer page placeholder ---
	CMenuAction m_lblMultiplayer;

	// --- Keyboard page placeholder ---
	CMenuAction m_lblKeyboard;

	// --- Mouse page placeholder ---
	CMenuAction m_lblMouse;

	// --- Voice page placeholder ---
	CMenuAction m_lblVoice;
};

// ---------------------------------------------------------------
// Init
// ---------------------------------------------------------------
void CMenuSettings::_Init()
{
	// Window geometry (virtual 1024x768 coords)
	SetRect( 80, 60, 700, 500 );

	// --- Tab control sits at top of content area ---
	m_tabControl.SetRect( 0, 0, 700 - WndStyle::BorderWidth * 2, 500 );
	m_tabControl.AddTab( "Multiplayer", &m_pageMultiplayer );
	m_tabControl.AddTab( "Keyboard",    &m_pageKeyboard );
	m_tabControl.AddTab( "Mouse",       &m_pageMouse );
	m_tabControl.AddTab( "Audio",       &m_pageAudio );
	m_tabControl.AddTab( "Video",       &m_pageVideo );
	m_tabControl.AddTab( "Voice",       &m_pageVoice );
	AddItem( m_tabControl );

	// --- Audio page ---
	int row = 40; // relative to page
	int col = 20;
	int slW = 280;

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

	// --- Video page ---
	m_chkFullscreen.szName = L( "Fullscreen" );
	m_chkFullscreen.SetCoord( 20, 40 );
	m_chkFullscreen.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_chkFullscreen );

	m_chkVsync.szName = L( "Vertical sync" );
	m_chkVsync.SetCoord( 20, 80 );
	m_chkVsync.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_chkVsync );

	// --- Placeholder pages ---
	m_lblMultiplayer.szName = "Multiplayer settings (TODO)";
	m_lblMultiplayer.iFlags |= QMF_INACTIVE;
	m_lblMultiplayer.SetCoord( 20, 40 );
	m_pageMultiplayer.AddItem( m_lblMultiplayer );

	m_lblKeyboard.szName = "Keyboard bindings (TODO)";
	m_lblKeyboard.iFlags |= QMF_INACTIVE;
	m_lblKeyboard.SetCoord( 20, 40 );
	m_pageKeyboard.AddItem( m_lblKeyboard );

	m_lblMouse.szName = "Mouse settings (TODO)";
	m_lblMouse.iFlags |= QMF_INACTIVE;
	m_lblMouse.SetCoord( 20, 40 );
	m_pageMouse.AddItem( m_lblMouse );

	m_lblVoice.szName = "Voice settings (TODO)";
	m_lblVoice.iFlags |= QMF_INACTIVE;
	m_lblVoice.SetCoord( 20, 40 );
	m_pageVoice.AddItem( m_lblVoice );

	// Add pages as items so they get Init/VidInit/Draw calls
	AddItem( m_pageMultiplayer );
	AddItem( m_pageKeyboard );
	AddItem( m_pageMouse );
	AddItem( m_pageAudio );
	AddItem( m_pageVideo );
	AddItem( m_pageVoice );
}

// ---------------------------------------------------------------
// VidInit
// ---------------------------------------------------------------
void CMenuSettings::_VidInit()
{
	// Link cvars
	m_sldSoundVol.LinkCvar( "volume" );
	m_sldMusicVol.LinkCvar( "MP3Volume" );
	m_sldSuitVol.LinkCvar( "suitvolume" );
	m_chkNoDSP.LinkCvar( "room_off" );
	m_chkMuteLostFocus.LinkCvar( "snd_mute_losefocus" );
	m_chkFullscreen.LinkCvar( "fullscreen" );
	m_chkVsync.LinkCvar( "gl_vsync" );

	// Start on Audio tab (most useful for quick test)
	m_tabControl.SetActiveTab( 3 );
}

// ---------------------------------------------------------------
// Save
// ---------------------------------------------------------------
void CMenuSettings::SaveAndPopMenu()
{
	m_sldSoundVol.WriteCvar();
	m_sldMusicVol.WriteCvar();
	m_sldSuitVol.WriteCvar();
	m_chkNoDSP.WriteCvar();
	m_chkMuteLostFocus.WriteCvar();
	m_chkFullscreen.WriteCvar();
	m_chkVsync.WriteCvar();
	CMenuWindow::SaveAndPopMenu();
}

// ---------------------------------------------------------------
// Menu entry point
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
