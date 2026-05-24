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
};

// ---------------------------------------------------------------
// Init
// ---------------------------------------------------------------
void CMenuSettings::_Init()
{
	SetRect( 80, 60, 700, 500 );

	int contentW = 700 - WndStyle::BorderWidth * 2;
	m_tabControl.SetRect( 0, 0, contentW, 500 );
	m_tabControl.AddTab( "Multiplayer", &m_pageMultiplayer );
	m_tabControl.AddTab( "Keyboard",    &m_pageKeyboard );
	m_tabControl.AddTab( "Mouse",       &m_pageMouse );
	m_tabControl.AddTab( "Audio",       &m_pageAudio );
	m_tabControl.AddTab( "Video",       &m_pageVideo );
	m_tabControl.AddTab( "Voice",       &m_pageVoice );
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

	// Add pages so they get Init/VidInit/Draw calls
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

	// Default to Multiplayer tab
	m_tabControl.SetActiveTab( 0 );
}

// ---------------------------------------------------------------
// Save
// ---------------------------------------------------------------
void CMenuSettings::SaveAndPopMenu()
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
