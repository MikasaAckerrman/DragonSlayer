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
#include "DropDown.h"
#include "Table.h"
#include "BaseModel.h"

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
// Helper: apply flat bordered button style (CS 1.6 look)
// ============================================================
static void ApplyFlatStyle( CMenuPicButton &btn )
{
	btn.bDrawStroke = true;
	btn.colorStroke = 0xFF44584C;
	btn.iStrokeWidth = 1;
	btn.colorBase = 0xFF2D2B2E;
	btn.colorFocus = 0xFF383638;
	btn.eFocusAnimation = QM_HIGHLIGHTIFFOCUS;
	btn.eTextAlignment = QM_CENTER;
	btn.bEnableTransitions = false;
	btn.bPulse = false;
}

// ============================================================
// Key bindings model for Keyboard tab
// ============================================================
struct KeyBindEntry
{
	const char *actionName;
	const char *command;
};

static const KeyBindEntry s_keyBinds[] =
{
	{ "Move Forward",    "+forward" },
	{ "Move Back",       "+back" },
	{ "Move Left",       "+moveleft" },
	{ "Move Right",      "+moveright" },
	{ "Jump",            "+jump" },
	{ "Duck",            "+duck" },
	{ "Use",             "+use" },
	{ "Attack",          "+attack" },
	{ "Secondary Attack","+attack2" },
	{ "Reload",          "+reload" },
	{ "Drop Weapon",     "drop" },
	{ "Buy Menu",        "buy" },
	{ "Scoreboard",      "+showscores" },
	{ "Chat",            "messagemode" },
	{ "Team Chat",       "messagemode2" },
};

static const int s_numKeyBinds = sizeof( s_keyBinds ) / sizeof( s_keyBinds[0] );

class CKeyBindingsModel : public CMenuBaseModel
{
public:
	void Update() override {}
	int GetColumns() const override { return 2; }
	int GetRows() const override { return s_numKeyBinds; }
	const char *GetCellText( int line, int column ) override
	{
		if( line < 0 || line >= s_numKeyBinds )
			return "";
		if( column == 0 )
			return s_keyBinds[line].actionName;
		if( column == 1 )
		{
			// Find key bound to this command
			for( int i = 0; i < 256; i++ )
			{
				const char *binding = EngFuncs::KEY_GetBinding( i );
				if( !binding ) continue;
				if( !strcmp( binding, s_keyBinds[line].command ) )
					return EngFuncs::KeynumToString( i );
			}
			return "";
		}
		return "";
	}
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
	CMenuDropDownStr m_ddSprayLogo;
	CMenuDropDownStr m_ddPlayerModel;
	CMenuDropDownInt m_ddHand;
	CMenuAction   m_divMultiplayer1;
	CMenuAction   m_divMultiplayer2;
	CMenuAction   m_lblSprayPreview;
	CMenuAction   m_sprayPreview;

	// --- Keyboard page ---
	CKeyBindingsModel m_bindingsModel;
	CMenuTable    m_tblBindings;
	CMenuAction   m_lblBindNote;

	// --- Mouse page ---
	CMenuSlider   m_sldSensitivity;
	CMenuCheckBox m_chkInvertMouse;
	CMenuCheckBox m_chkRawInput;
	CMenuCheckBox m_chkMouseFilter;
	CMenuCheckBox m_chkMouseAccel;

	// --- Audio page ---
	CMenuSlider   m_sldSoundVol;
	CMenuSlider   m_sldMusicVol;
	CMenuSlider   m_sldSuitVol;
	CMenuCheckBox m_chkNoDSP;
	CMenuCheckBox m_chkMuteLostFocus;
	CMenuDropDownStr m_ddSoundQuality;

	// --- Video page ---
	CMenuDropDownInt m_ddResolution;
	CMenuDropDownInt m_ddDisplayMode;
	CMenuSlider   m_sldBrightness;
	CMenuSlider   m_sldGamma;
	CMenuCheckBox m_chkVsync;
	CMenuAction   m_lblRenderer;
	CMenuDropDownInt m_ddTexQuality;

	// --- Voice page ---
	CMenuCheckBox m_chkVoiceEnable;
	CMenuSlider   m_sldVoiceScale;
	CMenuDropDownStr m_ddVoiceQuality;
	CMenuDropDownInt m_ddTransmitMode;
	CMenuAction   m_lblVoiceTest;

	// --- HUD page ---
	CMenuSlider   m_sldCrosshairSize;
	CMenuDropDownStr m_ddCrosshairColor;
	CMenuCheckBox m_chkFastSwitch;
	CMenuCheckBox m_chkCenterID;
	CMenuCheckBox m_chkAutoWepSwitch;
	CMenuAction   m_divHUD1;
	CMenuAction   m_divHUD2;
	CMenuDropDownStr m_ddHUDColor;
	CMenuCheckBox m_chkLowAmmoWarn;

	// --- System page ---
	CMenuField    m_fldRate;
	CMenuField    m_fldUpdateRate;
	CMenuField    m_fldCmdRate;
	CMenuField    m_fldInterp;
	CMenuAction   m_divSystem1;
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

	m_divMultiplayer1.iFlags |= QMF_INACTIVE;
	m_divMultiplayer1.SetCoord( col, 85 );
	m_divMultiplayer1.SetSize( 300, 1 );
	m_divMultiplayer1.SetBackground( WndStyle::WidgetBorderColor );
	m_pageMultiplayer.AddItem( m_divMultiplayer1 );

	m_ddSprayLogo.szName = L( "Spray logo" );
	m_ddSprayLogo.SetCoord( col, 100 );
	m_ddSprayLogo.SetSize( 180, 28 );
	m_ddSprayLogo.AddItem( "Lambda", "lambda" );
	m_ddSprayLogo.AddItem( "Skull", "skull" );
	m_ddSprayLogo.AddItem( "Smiley", "smiley" );
	m_ddSprayLogo.onChanged = CMenuEditable::WriteCvarCb;
	m_pageMultiplayer.AddItem( m_ddSprayLogo );

	m_ddPlayerModel.szName = L( "Player model" );
	m_ddPlayerModel.SetCoord( col, 150 );
	m_ddPlayerModel.SetSize( 180, 28 );
	m_ddPlayerModel.AddItem( "Gordon", "gordon" );
	m_ddPlayerModel.AddItem( "Barney", "barney" );
	m_ddPlayerModel.AddItem( "Scientist", "scientist" );
	m_ddPlayerModel.AddItem( "Helmet", "helmet" );
	m_ddPlayerModel.onChanged = CMenuEditable::WriteCvarCb;
	m_pageMultiplayer.AddItem( m_ddPlayerModel );

	m_divMultiplayer2.iFlags |= QMF_INACTIVE;
	m_divMultiplayer2.SetCoord( col, 195 );
	m_divMultiplayer2.SetSize( 300, 1 );
	m_divMultiplayer2.SetBackground( WndStyle::WidgetBorderColor );
	m_pageMultiplayer.AddItem( m_divMultiplayer2 );

	m_ddHand.szName = L( "Weapon hand" );
	m_ddHand.SetCoord( col, 210 );
	m_ddHand.SetSize( 180, 28 );
	m_ddHand.AddItem( "Right", 0 );
	m_ddHand.AddItem( "Left", 1 );
	m_ddHand.AddItem( "Center", 2 );
	m_ddHand.onChanged = CMenuEditable::WriteCvarCb;
	m_pageMultiplayer.AddItem( m_ddHand );

	m_lblModelHint.szName = L( "Model selection: use 'model' console command" );
	m_lblModelHint.iFlags |= QMF_INACTIVE;
	m_lblModelHint.SetCoord( col, 260 );
	m_pageMultiplayer.AddItem( m_lblModelHint );

	m_lblSprayPreview.szName = L( "Spray preview" );
	m_lblSprayPreview.iFlags |= QMF_INACTIVE;
	m_lblSprayPreview.SetCoord( 240, 85 );
	m_pageMultiplayer.AddItem( m_lblSprayPreview );

	m_sprayPreview.iFlags |= QMF_INACTIVE;
	m_sprayPreview.SetCoord( 240, 100 );
	m_sprayPreview.SetSize( 64, 64 );
	m_sprayPreview.SetBackground( WndStyle::WidgetBorderColor );
	m_pageMultiplayer.AddItem( m_sprayPreview );

	// ===== Keyboard =====
	m_tblBindings.SetCoord( col, 10 );
	m_tblBindings.SetSize( 620, 280 );
	m_tblBindings.SetModel( &m_bindingsModel );
	m_tblBindings.SetupColumn( 0, "Action", 0.6f );
	m_tblBindings.SetupColumn( 1, "Key", 0.4f );
	m_pageKeyboard.AddItem( m_tblBindings );

	m_lblBindNote.szName = L( "Use 'bind' in console for custom binds" );
	m_lblBindNote.iFlags |= QMF_INACTIVE;
	m_lblBindNote.SetCoord( col, 295 );
	m_pageKeyboard.AddItem( m_lblBindNote );

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

	m_chkMouseFilter.szName = L( "Mouse filter" );
	m_chkMouseFilter.SetCoord( col, 180 );
	m_chkMouseFilter.onChanged = CMenuEditable::WriteCvarCb;
	m_pageMouse.AddItem( m_chkMouseFilter );

	m_chkMouseAccel.szName = L( "Mouse acceleration" );
	m_chkMouseAccel.SetCoord( col, 220 );
	m_chkMouseAccel.onChanged = CMenuEditable::WriteCvarCb;
	m_pageMouse.AddItem( m_chkMouseAccel );

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

	row += 40;
	m_ddSoundQuality.szName = L( "Sound quality" );
	m_ddSoundQuality.SetCoord( col, row );
	m_ddSoundQuality.SetSize( 180, 28 );
	m_ddSoundQuality.AddItem( "11 kHz (Low)", "11025" );
	m_ddSoundQuality.AddItem( "22 kHz (Medium)", "22050" );
	m_ddSoundQuality.AddItem( "44 kHz (High)", "44100" );
	m_ddSoundQuality.onChanged = CMenuEditable::WriteCvarCb;
	m_pageAudio.AddItem( m_ddSoundQuality );

	// ===== Video =====
	m_ddResolution.szName = L( "Resolution" );
	m_ddResolution.SetCoord( col, 40 );
	m_ddResolution.SetSize( 180, 28 );
	m_ddResolution.AddItem( "640x480", 0 );
	m_ddResolution.AddItem( "800x600", 1 );
	m_ddResolution.AddItem( "1024x768", 2 );
	m_ddResolution.AddItem( "1280x960", 3 );
	m_ddResolution.AddItem( "1280x1024", 4 );
	m_ddResolution.AddItem( "1600x1200", 5 );
	m_ddResolution.AddItem( "1920x1080", 6 );
	m_ddResolution.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_ddResolution );

	m_ddDisplayMode.szName = L( "Display mode" );
	m_ddDisplayMode.SetCoord( col, 80 );
	m_ddDisplayMode.SetSize( 180, 28 );
	m_ddDisplayMode.AddItem( "Fullscreen", 1 );
	m_ddDisplayMode.AddItem( "Windowed", 0 );
	m_ddDisplayMode.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_ddDisplayMode );

	m_sldBrightness.szName = L( "Brightness" );
	m_sldBrightness.Setup( 0.0f, 2.0f, 0.1f );
	m_sldBrightness.SetCoord( col, 120 );
	m_sldBrightness.size.w = slW;
	m_sldBrightness.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_sldBrightness );

	m_sldGamma.szName = L( "Gamma" );
	m_sldGamma.Setup( 1.0f, 3.0f, 0.1f );
	m_sldGamma.SetCoord( col, 170 );
	m_sldGamma.size.w = slW;
	m_sldGamma.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_sldGamma );

	m_chkVsync.szName = L( "Vertical sync" );
	m_chkVsync.SetCoord( col, 220 );
	m_chkVsync.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_chkVsync );

	m_ddTexQuality.szName = L( "Texture quality" );
	m_ddTexQuality.SetCoord( col, 260 );
	m_ddTexQuality.SetSize( 180, 28 );
	m_ddTexQuality.AddItem( "High", 0 );
	m_ddTexQuality.AddItem( "Medium", 1 );
	m_ddTexQuality.AddItem( "Low", 2 );
	m_ddTexQuality.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVideo.AddItem( m_ddTexQuality );

	m_lblRenderer.szName = L( "Renderer: OpenGL" );
	m_lblRenderer.iFlags |= QMF_INACTIVE;
	m_lblRenderer.SetCoord( col, 300 );
	m_pageVideo.AddItem( m_lblRenderer );

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

	m_ddVoiceQuality.szName = L( "Voice quality" );
	m_ddVoiceQuality.SetCoord( col, 140 );
	m_ddVoiceQuality.SetSize( 180, 28 );
	m_ddVoiceQuality.AddItem( "Low", "low" );
	m_ddVoiceQuality.AddItem( "Medium", "medium" );
	m_ddVoiceQuality.AddItem( "High", "high" );
	m_ddVoiceQuality.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVoice.AddItem( m_ddVoiceQuality );

	m_ddTransmitMode.szName = L( "Transmit mode" );
	m_ddTransmitMode.SetCoord( col, 190 );
	m_ddTransmitMode.SetSize( 180, 28 );
	m_ddTransmitMode.AddItem( "Push to Talk", 0 );
	m_ddTransmitMode.AddItem( "Open Mic", 1 );
	m_ddTransmitMode.onChanged = CMenuEditable::WriteCvarCb;
	m_pageVoice.AddItem( m_ddTransmitMode );

	m_lblVoiceTest.szName = L( "Voice test (placeholder)" );
	m_lblVoiceTest.iFlags |= QMF_INACTIVE;
	m_lblVoiceTest.SetCoord( col, 240 );
	m_pageVoice.AddItem( m_lblVoiceTest );

	// ===== HUD =====
	m_sldCrosshairSize.szName = L( "Crosshair size" );
	m_sldCrosshairSize.Setup( 0, 3, 1 );
	m_sldCrosshairSize.SetCoord( col, 40 );
	m_sldCrosshairSize.size.w = slW;
	m_sldCrosshairSize.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_sldCrosshairSize );

	m_ddCrosshairColor.szName = L( "Crosshair color" );
	m_ddCrosshairColor.SetCoord( col, 90 );
	m_ddCrosshairColor.SetSize( 180, 28 );
	m_ddCrosshairColor.AddItem( "Green", "green" );
	m_ddCrosshairColor.AddItem( "Red", "red" );
	m_ddCrosshairColor.AddItem( "Blue", "blue" );
	m_ddCrosshairColor.AddItem( "Yellow", "yellow" );
	m_ddCrosshairColor.AddItem( "Cyan", "cyan" );
	m_ddCrosshairColor.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_ddCrosshairColor );

	m_divHUD1.iFlags |= QMF_INACTIVE;
	m_divHUD1.SetCoord( col, 130 );
	m_divHUD1.SetSize( 300, 1 );
	m_divHUD1.SetBackground( WndStyle::WidgetBorderColor );
	m_pageHUD.AddItem( m_divHUD1 );

	m_chkFastSwitch.szName = L( "Fast weapon switch" );
	m_chkFastSwitch.SetCoord( col, 145 );
	m_chkFastSwitch.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_chkFastSwitch );

	m_chkCenterID.szName = L( "Player names in center" );
	m_chkCenterID.SetCoord( col, 185 );
	m_chkCenterID.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_chkCenterID );

	m_chkAutoWepSwitch.szName = L( "Auto-switch to picked weapon" );
	m_chkAutoWepSwitch.SetCoord( col, 225 );
	m_chkAutoWepSwitch.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_chkAutoWepSwitch );

	m_divHUD2.iFlags |= QMF_INACTIVE;
	m_divHUD2.SetCoord( col, 265 );
	m_divHUD2.SetSize( 300, 1 );
	m_divHUD2.SetBackground( WndStyle::WidgetBorderColor );
	m_pageHUD.AddItem( m_divHUD2 );

	m_ddHUDColor.szName = L( "HUD color" );
	m_ddHUDColor.SetCoord( col, 280 );
	m_ddHUDColor.SetSize( 180, 28 );
	m_ddHUDColor.AddItem( "Green", "green" );
	m_ddHUDColor.AddItem( "Amber", "amber" );
	m_ddHUDColor.AddItem( "Yellow", "yellow" );
	m_ddHUDColor.AddItem( "Blue", "blue" );
	m_ddHUDColor.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_ddHUDColor );

	m_chkLowAmmoWarn.szName = L( "Low ammo warning" );
	m_chkLowAmmoWarn.SetCoord( col, 320 );
	m_chkLowAmmoWarn.onChanged = CMenuEditable::WriteCvarCb;
	m_pageHUD.AddItem( m_chkLowAmmoWarn );

	// ===== System =====
	m_fldRate.szName = L( "Rate" );
	m_fldRate.iMaxLength = 6;
	m_fldRate.bNumbersOnly = true;
	m_fldRate.SetCoord( col, 40 );
	m_fldRate.size.w = 100;
	m_fldRate.size.h = 32;
	m_pageSystem.AddItem( m_fldRate );

	m_fldUpdateRate.szName = L( "Update rate" );
	m_fldUpdateRate.iMaxLength = 4;
	m_fldUpdateRate.bNumbersOnly = true;
	m_fldUpdateRate.SetCoord( col, 90 );
	m_fldUpdateRate.size.w = 100;
	m_fldUpdateRate.size.h = 32;
	m_pageSystem.AddItem( m_fldUpdateRate );

	m_fldCmdRate.szName = L( "Command rate" );
	m_fldCmdRate.iMaxLength = 4;
	m_fldCmdRate.bNumbersOnly = true;
	m_fldCmdRate.SetCoord( col, 140 );
	m_fldCmdRate.size.w = 100;
	m_fldCmdRate.size.h = 32;
	m_pageSystem.AddItem( m_fldCmdRate );

	m_fldInterp.szName = L( "Interpolation" );
	m_fldInterp.iMaxLength = 6;
	m_fldInterp.bNumbersOnly = false;
	m_fldInterp.SetCoord( col, 190 );
	m_fldInterp.size.w = 100;
	m_fldInterp.size.h = 32;
	m_pageSystem.AddItem( m_fldInterp );

	m_divSystem1.iFlags |= QMF_INACTIVE;
	m_divSystem1.SetCoord( col, 235 );
	m_divSystem1.SetSize( 300, 1 );
	m_divSystem1.SetBackground( WndStyle::WidgetBorderColor );
	m_pageSystem.AddItem( m_divSystem1 );

	m_chkDeveloper.szName = L( "Developer console" );
	m_chkDeveloper.SetCoord( col, 250 );
	m_chkDeveloper.onChanged = CMenuEditable::WriteCvarCb;
	m_pageSystem.AddItem( m_chkDeveloper );

	m_fldFpsMax.szName = L( "Max FPS" );
	m_fldFpsMax.iMaxLength = 5;
	m_fldFpsMax.bNumbersOnly = true;
	m_fldFpsMax.SetCoord( col, 300 );
	m_fldFpsMax.size.w = 100;
	m_fldFpsMax.size.h = 32;
	m_pageSystem.AddItem( m_fldFpsMax );

	m_chkNetGraph.szName = L( "Show net graph" );
	m_chkNetGraph.SetCoord( col, 350 );
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
	int btnH = 28;
	m_btnOK.SetNameAndStatus( "OK", "Apply and close" );
	m_btnOK.SetCoord( contentW - btnW * 3 - 30, btnY );
	m_btnOK.size.w = btnW;
	m_btnOK.size.h = btnH;
	ApplyFlatStyle( m_btnOK );
	m_btnOK.onReleased = VoidCb( &CMenuSettings::OnOK );
	AddItem( m_btnOK );

	m_btnCancel.SetNameAndStatus( "Cancel", "Discard changes" );
	m_btnCancel.SetCoord( contentW - btnW * 2 - 20, btnY );
	m_btnCancel.size.w = btnW;
	m_btnCancel.size.h = btnH;
	ApplyFlatStyle( m_btnCancel );
	m_btnCancel.onReleased = VoidCb( &CMenuSettings::OnCancel );
	AddItem( m_btnCancel );

	m_btnApply.SetNameAndStatus( "Apply", "Apply without closing" );
	m_btnApply.SetCoord( contentW - btnW - 10, btnY );
	m_btnApply.size.w = btnW;
	m_btnApply.size.h = btnH;
	ApplyFlatStyle( m_btnApply );
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
	m_ddSprayLogo.LinkCvar( "cl_logofile", CMenuEditable::CVAR_STRING );
	m_ddPlayerModel.LinkCvar( "model", CMenuEditable::CVAR_STRING );
	m_ddHand.LinkCvar( "cl_righthand", CMenuEditable::CVAR_VALUE );

	// Mouse
	m_sldSensitivity.LinkCvar( "sensitivity" );
	m_chkInvertMouse.bChecked = ( EngFuncs::GetCvarFloat( "m_pitch" ) < 0 );
	m_chkRawInput.LinkCvar( "m_rawinput" );
	m_chkMouseFilter.LinkCvar( "m_filter" );
	m_chkMouseAccel.LinkCvar( "m_customaccel" );

	// Audio
	m_sldSoundVol.LinkCvar( "volume" );
	m_sldMusicVol.LinkCvar( "MP3Volume" );
	m_sldSuitVol.LinkCvar( "suitvolume" );
	m_chkNoDSP.LinkCvar( "room_off" );
	m_chkMuteLostFocus.LinkCvar( "snd_mute_losefocus" );
	m_ddSoundQuality.LinkCvar( "s_khz", CMenuEditable::CVAR_STRING );

	// Video
	m_ddResolution.LinkCvar( "vid_mode", CMenuEditable::CVAR_VALUE );
	m_ddDisplayMode.LinkCvar( "fullscreen", CMenuEditable::CVAR_VALUE );
	m_sldBrightness.LinkCvar( "brightness" );
	m_sldGamma.LinkCvar( "gamma" );
	m_chkVsync.LinkCvar( "gl_vsync" );
	m_ddTexQuality.LinkCvar( "gl_picmip", CMenuEditable::CVAR_VALUE );

	// Voice
	m_chkVoiceEnable.LinkCvar( "voice_enable" );
	m_sldVoiceScale.LinkCvar( "voice_scale" );
	m_ddVoiceQuality.LinkCvar( "voice_quality", CMenuEditable::CVAR_STRING );
	m_ddTransmitMode.LinkCvar( "voice_vox", CMenuEditable::CVAR_VALUE );

	// HUD
	m_sldCrosshairSize.LinkCvar( "cl_crosshair_size" );
	m_ddCrosshairColor.LinkCvar( "cl_crosshair_color", CMenuEditable::CVAR_STRING );
	m_chkFastSwitch.LinkCvar( "hud_fastswitch" );
	m_chkCenterID.LinkCvar( "hud_centerid" );
	m_chkAutoWepSwitch.LinkCvar( "cl_autowepswitch" );
	m_ddHUDColor.LinkCvar( "cl_hudcolor", CMenuEditable::CVAR_STRING );
	m_chkLowAmmoWarn.LinkCvar( "hud_lowammowarning" );

	// System
	m_fldRate.LinkCvar( "rate" );
	m_fldUpdateRate.LinkCvar( "cl_updaterate" );
	m_fldCmdRate.LinkCvar( "cl_cmdrate" );
	m_fldInterp.LinkCvar( "ex_interp" );
	m_chkDeveloper.LinkCvar( "developer" );
	m_fldFpsMax.LinkCvar( "fps_max" );
	m_chkNetGraph.LinkCvar( "net_graph" );
}

// ---------------------------------------------------------------
// Bottom button bar handlers
// ---------------------------------------------------------------
void CMenuSettings::OnApply()
{
	m_fldPlayerName.WriteCvar();
	m_ddSprayLogo.WriteCvar();
	m_ddPlayerModel.WriteCvar();
	m_ddHand.WriteCvar();
	m_sldSensitivity.WriteCvar();
	EngFuncs::CvarSetValue( "m_pitch", m_chkInvertMouse.bChecked ? -0.022f : 0.022f );
	m_chkRawInput.WriteCvar();
	m_chkMouseFilter.WriteCvar();
	m_chkMouseAccel.WriteCvar();
	m_sldSoundVol.WriteCvar();
	m_sldMusicVol.WriteCvar();
	m_sldSuitVol.WriteCvar();
	m_chkNoDSP.WriteCvar();
	m_chkMuteLostFocus.WriteCvar();
	m_ddSoundQuality.WriteCvar();
	m_ddResolution.WriteCvar();
	m_ddDisplayMode.WriteCvar();
	m_sldBrightness.WriteCvar();
	m_sldGamma.WriteCvar();
	m_chkVsync.WriteCvar();
	m_ddTexQuality.WriteCvar();
	m_chkVoiceEnable.WriteCvar();
	m_sldVoiceScale.WriteCvar();
	m_ddVoiceQuality.WriteCvar();
	m_ddTransmitMode.WriteCvar();
	m_sldCrosshairSize.WriteCvar();
	m_ddCrosshairColor.WriteCvar();
	m_chkFastSwitch.WriteCvar();
	m_chkCenterID.WriteCvar();
	m_chkAutoWepSwitch.WriteCvar();
	m_ddHUDColor.WriteCvar();
	m_chkLowAmmoWarn.WriteCvar();
	m_fldRate.WriteCvar();
	m_fldUpdateRate.WriteCvar();
	m_fldCmdRate.WriteCvar();
	m_fldInterp.WriteCvar();
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
