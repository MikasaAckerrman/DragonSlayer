/*
imgui_menu_slayer.cpp - Slayer3D Dear ImGui CS 1.6-style settings menu
Copyright (C) 2026 Slayer3D contributors

Pixel-perfect CS 1.6 VGUI2 look with 3D beveled borders via ImDrawList.
*/

extern "C"
{
#include "common.h"
#include "client.h"
#include "keydefs.h"
#include "ref_common.h"
#include "platform/platform.h"
}

#include "imgui.h"
#include "imgui_impl_xash_gles.h"
#include "imgui_menu_slayer.h"

// ============================================================================
// CS 1.6 Bevel Colors
// ============================================================================

#define CS16_BEVEL_LIGHT  IM_COL32(160, 160, 160, 255)
#define CS16_BEVEL_DARK   IM_COL32(32, 32, 32, 255)

// ============================================================================
// State
// ============================================================================

static bool g_Initialized = false;
static bool g_GLInitialized = false;
static bool g_MenuVisible = false;

// --- Connection Progress state ---
enum connprogress_state_e
{
	CONNPROGRESS_NONE = 0,
	CONNPROGRESS_CONNECTING,
	CONNPROGRESS_DOWNLOADING,
	CONNPROGRESS_PRECACHING,
	CONNPROGRESS_CHANGELEVEL,
};

static connprogress_state_e g_ConnProgressState = CONNPROGRESS_NONE;
static char  g_ConnStatusText[512] = "";
static float g_ConnProgress = 0.0f;
static char  g_ConnServerName[256] = "";

// --- Console state ---
static bool g_ConsoleVisible = false;

#define CONSOLE_MAX_LINES 1024
#define CONSOLE_LINE_LEN  256

static char  g_ConsoleLines[CONSOLE_MAX_LINES][CONSOLE_LINE_LEN];
static unsigned int g_ConsoleWritePos = 0;
static bool  g_ConsoleScrollToBottom = true;
static char  g_ConsoleInputBuf[512] = "";

static char  g_ConsoleLineBuf[CONSOLE_LINE_LEN];
static int   g_ConsoleLineBufPos = 0;

// Pending settings
static float g_Volume = 1.0f;
static float g_MusicVolume = 1.0f;
static float g_Sensitivity = 3.0f;
static float g_Pitch = 0.022f;
static float g_Brightness = 1.0f;
static float g_Gamma = 1.0f;
static float g_VoiceScale = 1.0f;
static int   g_Crosshair = 1;
static int   g_AutoSwitch = 1;
static int   g_RawInput = 0;
static int   g_ReverseMouse = 0;
static int   g_MouseFilter = 0;
static int   g_HQSound = 1;
static int   g_Surround = 0;
static int   g_VoiceEnable = 1;
static int   g_Vsync = 0;
static int   g_VoiceInputFromFile = 0;
static char  g_PlayerName[64] = "Player";

// ============================================================================
// Helpers
// ============================================================================

static void LoadSettings( void )
{
	g_Volume       = Cvar_VariableValue( "volume" );
	g_MusicVolume  = Cvar_VariableValue( "MP3Volume" );
	g_Sensitivity  = Cvar_VariableValue( "sensitivity" );
	g_Pitch        = Cvar_VariableValue( "m_pitch" );
	g_Brightness   = Cvar_VariableValue( "brightness" );
	g_Gamma        = Cvar_VariableValue( "gamma" );
	g_VoiceScale   = Cvar_VariableValue( "voice_scale" );
	g_Crosshair    = (int)Cvar_VariableValue( "crosshair" );
	g_AutoSwitch   = (int)Cvar_VariableValue( "hud_fastswitch" );
	g_RawInput     = (int)Cvar_VariableValue( "m_rawinput" );
	g_ReverseMouse = (int)Cvar_VariableValue( "m_reverse" );
	g_MouseFilter  = (int)Cvar_VariableValue( "m_filter" );
	g_HQSound      = (int)Cvar_VariableValue( "s_lerping" );
	g_Surround     = (int)Cvar_VariableValue( "s_surround" );
	g_VoiceEnable  = (int)Cvar_VariableValue( "voice_enable" );
	g_Vsync        = (int)Cvar_VariableValue( "gl_vsync" );
	g_VoiceInputFromFile = (int)Cvar_VariableValue( "voice_inputfromfile" );

	const char *name = Cvar_VariableString( "name" );
	if( name && name[0] )
		Q_strncpy( g_PlayerName, name, sizeof( g_PlayerName ) );
}

static void ApplySettings( void )
{
	Cvar_SetValue( "volume", g_Volume );
	Cvar_SetValue( "MP3Volume", g_MusicVolume );
	Cvar_SetValue( "sensitivity", g_Sensitivity );
	Cvar_SetValue( "m_pitch", g_Pitch );
	Cvar_SetValue( "brightness", g_Brightness );
	Cvar_SetValue( "gamma", g_Gamma );
	Cvar_SetValue( "voice_scale", g_VoiceScale );
	Cvar_SetValue( "crosshair", (float)g_Crosshair );
	Cvar_SetValue( "hud_fastswitch", (float)g_AutoSwitch );
	Cvar_SetValue( "m_rawinput", (float)g_RawInput );
	Cvar_SetValue( "m_reverse", (float)g_ReverseMouse );
	Cvar_SetValue( "m_filter", (float)g_MouseFilter );
	Cvar_SetValue( "s_lerping", (float)g_HQSound );
	Cvar_SetValue( "s_surround", (float)g_Surround );
	Cvar_SetValue( "voice_enable", (float)g_VoiceEnable );
	Cvar_SetValue( "voice_inputfromfile", (float)g_VoiceInputFromFile );

	Cbuf_AddTextf( "gl_vsync %d\n", g_Vsync );
	Cvar_Set( "name", g_PlayerName );
}

// ============================================================================
// CS 1.6 Style Setup
// ============================================================================

static void SetupCS16Style( void )
{
	ImGuiStyle &style = ImGui::GetStyle();
	ImVec4 *colors = style.Colors;

	// Zero rounding for that sharp Win95/VGUI2 look
	style.WindowRounding    = 0.0f;
	style.FrameRounding     = 0.0f;
	style.ScrollbarRounding = 0.0f;
	style.TabRounding       = 0.0f;
	style.GrabRounding      = 0.0f;
	style.ChildRounding     = 0.0f;
	style.PopupRounding     = 0.0f;

	// We draw borders manually via ImDrawList
	style.WindowBorderSize = 0.0f;
	style.FrameBorderSize  = 0.0f;

	style.WindowPadding = ImVec2( 8, 8 );
	style.FramePadding  = ImVec2( 4, 3 );
	style.ItemSpacing   = ImVec2( 8, 4 );
	style.ScrollbarSize = 14.0f;
	style.GrabMinSize   = 10.0f;

	// Window BG: #585858
	colors[ImGuiCol_WindowBg]         = ImVec4( 0.345f, 0.345f, 0.345f, 1.00f );
	// Title bar: #3C3C3C
	colors[ImGuiCol_TitleBg]          = ImVec4( 0.235f, 0.235f, 0.235f, 1.00f );
	colors[ImGuiCol_TitleBgActive]    = ImVec4( 0.235f, 0.235f, 0.235f, 1.00f );
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4( 0.200f, 0.200f, 0.200f, 1.00f );
	colors[ImGuiCol_MenuBarBg]        = ImVec4( 0.235f, 0.235f, 0.235f, 1.00f );

	// Buttons: #646464
	colors[ImGuiCol_Button]           = ImVec4( 0.392f, 0.392f, 0.392f, 1.00f );
	colors[ImGuiCol_ButtonHovered]    = ImVec4( 0.440f, 0.440f, 0.440f, 1.00f );
	colors[ImGuiCol_ButtonActive]     = ImVec4( 0.340f, 0.340f, 0.340f, 1.00f );

	// Frame BG (inputs): #282828 dark sunken
	colors[ImGuiCol_FrameBg]          = ImVec4( 0.157f, 0.157f, 0.157f, 1.00f );
	colors[ImGuiCol_FrameBgHovered]   = ImVec4( 0.180f, 0.180f, 0.180f, 1.00f );
	colors[ImGuiCol_FrameBgActive]    = ImVec4( 0.200f, 0.200f, 0.200f, 1.00f );

	// Tabs: active = window bg #585858, inactive = #484848
	colors[ImGuiCol_Tab]              = ImVec4( 0.282f, 0.282f, 0.282f, 1.00f );
	colors[ImGuiCol_TabHovered]       = ImVec4( 0.380f, 0.380f, 0.380f, 1.00f );
	colors[ImGuiCol_TabSelected]      = ImVec4( 0.345f, 0.345f, 0.345f, 1.00f );
	colors[ImGuiCol_TabDimmed]        = ImVec4( 0.250f, 0.250f, 0.250f, 1.00f );
	colors[ImGuiCol_TabDimmedSelected] = ImVec4( 0.310f, 0.310f, 0.310f, 1.00f );

	// Text
	colors[ImGuiCol_Text]             = ImVec4( 1.00f, 1.00f, 1.00f, 1.00f );
	colors[ImGuiCol_TextDisabled]     = ImVec4( 0.627f, 0.627f, 0.627f, 1.00f );

	// CheckMark: white
	colors[ImGuiCol_CheckMark]        = ImVec4( 1.00f, 1.00f, 1.00f, 1.00f );

	// SliderGrab: #808080
	colors[ImGuiCol_SliderGrab]       = ImVec4( 0.502f, 0.502f, 0.502f, 1.00f );
	colors[ImGuiCol_SliderGrabActive] = ImVec4( 0.600f, 0.600f, 0.600f, 1.00f );

	// Scrollbar: bg #3C3C3C, grab #646464
	colors[ImGuiCol_ScrollbarBg]      = ImVec4( 0.235f, 0.235f, 0.235f, 1.00f );
	colors[ImGuiCol_ScrollbarGrab]    = ImVec4( 0.392f, 0.392f, 0.392f, 1.00f );
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4( 0.450f, 0.450f, 0.450f, 1.00f );
	colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4( 0.500f, 0.500f, 0.500f, 1.00f );

	// Popup BG: #484848
	colors[ImGuiCol_PopupBg]          = ImVec4( 0.282f, 0.282f, 0.282f, 1.00f );

	// Border: #000000 (manual override via bevel draws)
	colors[ImGuiCol_Border]           = ImVec4( 0.00f, 0.00f, 0.00f, 1.00f );
	colors[ImGuiCol_BorderShadow]     = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );

	// Separator: #404040
	colors[ImGuiCol_Separator]        = ImVec4( 0.251f, 0.251f, 0.251f, 1.00f );
	colors[ImGuiCol_SeparatorHovered] = ImVec4( 0.350f, 0.350f, 0.350f, 1.00f );
	colors[ImGuiCol_SeparatorActive]  = ImVec4( 0.400f, 0.400f, 0.400f, 1.00f );

	// Header (selectable, table headers)
	colors[ImGuiCol_Header]           = ImVec4( 0.300f, 0.300f, 0.300f, 1.00f );
	colors[ImGuiCol_HeaderHovered]    = ImVec4( 0.370f, 0.370f, 0.370f, 1.00f );
	colors[ImGuiCol_HeaderActive]     = ImVec4( 0.400f, 0.400f, 0.400f, 1.00f );

	// Resize grip
	colors[ImGuiCol_ResizeGrip]       = ImVec4( 0.400f, 0.400f, 0.400f, 0.50f );
	colors[ImGuiCol_ResizeGripHovered] = ImVec4( 0.550f, 0.550f, 0.550f, 0.70f );
	colors[ImGuiCol_ResizeGripActive] = ImVec4( 0.650f, 0.650f, 0.650f, 0.90f );
}

// ============================================================================
// 3D Beveled Border Helpers
// ============================================================================

static void DrawBeveledRect( ImDrawList *dl, ImVec2 mn, ImVec2 mx, bool raised )
{
	ImU32 tl = raised ? CS16_BEVEL_LIGHT : CS16_BEVEL_DARK;
	ImU32 br = raised ? CS16_BEVEL_DARK : CS16_BEVEL_LIGHT;
	// Top edge
	dl->AddLine( ImVec2( mn.x, mn.y ), ImVec2( mx.x - 1, mn.y ), tl );
	// Left edge
	dl->AddLine( ImVec2( mn.x, mn.y ), ImVec2( mn.x, mx.y - 1 ), tl );
	// Bottom edge
	dl->AddLine( ImVec2( mn.x, mx.y - 1 ), ImVec2( mx.x - 1, mx.y - 1 ), br );
	// Right edge
	dl->AddLine( ImVec2( mx.x - 1, mn.y ), ImVec2( mx.x - 1, mx.y - 1 ), br );
}

static void DrawCS16Separator( void )
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float width = ImGui::GetContentRegionAvail().x;
	dl->AddLine( ImVec2( pos.x, pos.y ), ImVec2( pos.x + width, pos.y ), CS16_BEVEL_DARK );
	dl->AddLine( ImVec2( pos.x, pos.y + 1 ), ImVec2( pos.x + width, pos.y + 1 ), CS16_BEVEL_LIGHT );
	ImGui::Dummy( ImVec2( 0, 4 ) );
}

// ============================================================================
// Beveled Widget Wrappers
// ============================================================================

static bool CS16Button( const char *label, ImVec2 size = ImVec2( 0, 0 ) )
{
	bool pressed = ImGui::Button( label, size );
	DrawBeveledRect( ImGui::GetWindowDrawList(),
		ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
		!ImGui::IsItemActive() );
	return pressed;
}

static bool CS16Checkbox( const char *label, bool *v )
{
	bool changed = ImGui::Checkbox( label, v );
	// Bevel only the checkbox square, not the full label area
	ImVec2 mn = ImGui::GetItemRectMin();
	float frameHeight = ImGui::GetFrameHeight();
	ImVec2 mx = ImVec2( mn.x + frameHeight, mn.y + frameHeight );
	DrawBeveledRect( ImGui::GetWindowDrawList(), mn, mx, false );
	return changed;
}

static bool CS16SliderFloat( const char *label, float *v, float v_min, float v_max, const char *format = "%.2f" )
{
	bool changed = ImGui::SliderFloat( label, v, v_min, v_max, format );
	DrawBeveledRect( ImGui::GetWindowDrawList(),
		ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false );
	return changed;
}

static bool CS16InputText( const char *label, char *buf, int buf_size, ImGuiInputTextFlags flags = 0 )
{
	bool changed = ImGui::InputText( label, buf, (size_t)buf_size, flags );
	DrawBeveledRect( ImGui::GetWindowDrawList(),
		ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false );
	return changed;
}

static bool CS16Combo( const char *label, int *current_item, const char *items_separated_by_zeros )
{
	bool changed = ImGui::Combo( label, current_item, items_separated_by_zeros );
	DrawBeveledRect( ImGui::GetWindowDrawList(),
		ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false );
	return changed;
}

// ============================================================================
// Tab Drawing Functions
// ============================================================================

static void DrawTabGame( void )
{
	ImGui::TextUnformatted( "Player Name:" );
	CS16InputText( "##PlayerName", g_PlayerName, sizeof( g_PlayerName ) );

	DrawCS16Separator();

	bool cross = ( g_Crosshair != 0 );
	if( CS16Checkbox( "Enable Crosshair", &cross ) )
		g_Crosshair = cross ? 1 : 0;

	DrawCS16Separator();

	bool autoswitch = ( g_AutoSwitch != 0 );
	if( CS16Checkbox( "Auto weapon switch", &autoswitch ) )
		g_AutoSwitch = autoswitch ? 1 : 0;
}

static void DrawTabKeyboard( void )
{
	ImGui::TextUnformatted( "Key Bindings:" );
	DrawCS16Separator();

	float childH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - 8.0f;
	if( childH < 100.0f ) childH = 100.0f;

	if( ImGui::BeginChild( "##KeyBindList", ImVec2( 0, childH ), true ) )
	{
		DrawBeveledRect( ImGui::GetWindowDrawList(),
			ImGui::GetWindowPos(),
			ImVec2( ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
			         ImGui::GetWindowPos().y + ImGui::GetWindowSize().y ), false );

		if( ImGui::BeginTable( "##KeyTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH ) )
		{
			ImGui::TableSetupColumn( "Action", ImGuiTableColumnFlags_WidthStretch );
			ImGui::TableSetupColumn( "Key", ImGuiTableColumnFlags_WidthFixed, 100.0f );
			ImGui::TableHeadersRow();

			const char *bindings[][2] = {
				{ "Move Forward", "W" },
				{ "Move Back", "S" },
				{ "Move Left", "A" },
				{ "Move Right", "D" },
				{ "Jump", "SPACE" },
				{ "Duck", "CTRL" },
				{ "Reload", "R" },
				{ "Use", "E" },
				{ "Primary Attack", "MOUSE1" },
				{ "Secondary Attack", "MOUSE2" },
				{ "Next Weapon", "MWHEELUP" },
				{ "Prev Weapon", "MWHEELDOWN" },
				{ "Drop Weapon", "G" },
				{ "Flashlight", "F" },
				{ "Spray Logo", "T" },
			};

			for( int i = 0; i < 15; i++ )
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex( 0 );
				ImGui::TextUnformatted( bindings[i][0] );
				ImGui::TableSetColumnIndex( 1 );
				ImGui::TextUnformatted( bindings[i][1] );
			}
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();

	if( CS16Button( "Use Defaults" ) )
	{
		Cbuf_AddText( "unbindall\nexec default.cfg\n" );
	}
}

static void DrawTabMouse( void )
{
	CS16SliderFloat( "Sensitivity", &g_Sensitivity, 0.1f, 20.0f, "%.1f" );
	CS16SliderFloat( "m_pitch", &g_Pitch, 0.001f, 0.05f, "%.4f" );

	DrawCS16Separator();

	bool raw = ( g_RawInput != 0 );
	if( CS16Checkbox( "Raw input", &raw ) )
		g_RawInput = raw ? 1 : 0;

	bool rev = ( g_ReverseMouse != 0 );
	if( CS16Checkbox( "Reverse mouse", &rev ) )
		g_ReverseMouse = rev ? 1 : 0;

	bool filter = ( g_MouseFilter != 0 );
	if( CS16Checkbox( "Mouse filter", &filter ) )
		g_MouseFilter = filter ? 1 : 0;
}

static void DrawTabAudio( void )
{
	CS16SliderFloat( "Volume", &g_Volume, 0.0f, 1.0f, "%.2f" );
	CS16SliderFloat( "Music Volume", &g_MusicVolume, 0.0f, 1.0f, "%.2f" );

	DrawCS16Separator();

	bool hq = ( g_HQSound != 0 );
	if( CS16Checkbox( "High quality sound", &hq ) )
		g_HQSound = hq ? 1 : 0;

	bool surr = ( g_Surround != 0 );
	if( CS16Checkbox( "Surround sound", &surr ) )
		g_Surround = surr ? 1 : 0;
}

static void DrawTabVideo( void )
{
	// Resolution combo is display-only; no vid_mode cvar write (requires restart)
	static int g_ResolutionIdx = 0;
	CS16Combo( "Resolution (info)", &g_ResolutionIdx,
		"640x480\0800x600\01024x768\01152x864\01280x720\01280x960\01280x1024\01366x768\01600x900\01920x1080\0" );

	DrawCS16Separator();

	CS16SliderFloat( "Brightness", &g_Brightness, 0.0f, 3.0f, "%.1f" );
	CS16SliderFloat( "Gamma", &g_Gamma, 0.5f, 3.0f, "%.1f" );

	DrawCS16Separator();

	bool vsync = ( g_Vsync != 0 );
	if( CS16Checkbox( "VSync", &vsync ) )
		g_Vsync = vsync ? 1 : 0;

	DrawCS16Separator();
	ImGui::TextDisabled( "Renderer: OpenGL ES 2.0" );
	ImGui::TextDisabled( "Note: Resolution changes may require restart." );
}

static void DrawTabVoice( void )
{
	bool ve = ( g_VoiceEnable != 0 );
	if( CS16Checkbox( "Enable voice communication", &ve ) )
		g_VoiceEnable = ve ? 1 : 0;

	DrawCS16Separator();

	CS16SliderFloat( "Receive Volume", &g_VoiceScale, 0.0f, 2.0f, "%.2f" );

	DrawCS16Separator();

	bool fromfile = ( g_VoiceInputFromFile != 0 );
	if( CS16Button( "Test Microphone" ) )
	{
		g_VoiceInputFromFile = !g_VoiceInputFromFile;
	}
	fromfile = ( g_VoiceInputFromFile != 0 );
	if( fromfile )
		ImGui::TextUnformatted( "Testing microphone..." );
}

// ============================================================================
// Settings Menu
// ============================================================================

static void DrawSettingsMenu( void )
{
	ImGuiIO &io = ImGui::GetIO();

	ImVec2 center( io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f );
	ImGui::SetNextWindowPos( center, ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );
	ImGui::SetNextWindowSize( ImVec2( 540, 420 ), ImGuiCond_Appearing );

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
	if( !ImGui::Begin( "Configuration", &g_MenuVisible, flags ) )
	{
		ImGui::End();
		return;
	}

	// Outer window bevel
	{
		ImVec2 wpos = ImGui::GetWindowPos();
		ImVec2 wsize = ImGui::GetWindowSize();
		DrawBeveledRect( ImGui::GetWindowDrawList(), wpos,
			ImVec2( wpos.x + wsize.x, wpos.y + wsize.y ), true );
	}

	if( ImGui::BeginTabBar( "SettingsTabs" ) )
	{
		if( ImGui::BeginTabItem( "Game" ) )
		{
			DrawTabGame();
			ImGui::EndTabItem();
		}
		if( ImGui::BeginTabItem( "Keyboard" ) )
		{
			DrawTabKeyboard();
			ImGui::EndTabItem();
		}
		if( ImGui::BeginTabItem( "Mouse" ) )
		{
			DrawTabMouse();
			ImGui::EndTabItem();
		}
		if( ImGui::BeginTabItem( "Audio" ) )
		{
			DrawTabAudio();
			ImGui::EndTabItem();
		}
		if( ImGui::BeginTabItem( "Video" ) )
		{
			DrawTabVideo();
			ImGui::EndTabItem();
		}
		if( ImGui::BeginTabItem( "Voice" ) )
		{
			DrawTabVoice();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	DrawCS16Separator();

	// OK / Cancel / Apply buttons
	float buttonWidth = 80.0f;
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	float totalWidth = buttonWidth * 3.0f + spacing * 2.0f;
	ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - totalWidth ) * 0.5f );

	if( CS16Button( "OK", ImVec2( buttonWidth, 0 ) ) )
	{
		ApplySettings();
		g_MenuVisible = false;
	}
	ImGui::SameLine();
	if( CS16Button( "Cancel", ImVec2( buttonWidth, 0 ) ) )
	{
		g_MenuVisible = false;
	}
	ImGui::SameLine();
	if( CS16Button( "Apply", ImVec2( buttonWidth, 0 ) ) )
	{
		ApplySettings();
	}

	ImGui::End();
}

// ============================================================================
// Connection Progress
// ============================================================================

static void DrawConnectionProgress( void )
{
	if( g_ConnProgressState == CONNPROGRESS_NONE )
		return;

	ImGuiIO &io = ImGui::GetIO();
	ImVec2 center( io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f );
	ImGui::SetNextWindowPos( center, ImGuiCond_Always, ImVec2( 0.5f, 0.5f ) );

	float winW = ( 400.0f < io.DisplaySize.x * 0.8f ) ? 400.0f : io.DisplaySize.x * 0.8f;
	float winH = ( 180.0f < io.DisplaySize.y * 0.5f ) ? 180.0f : io.DisplaySize.y * 0.5f;
	ImGui::SetNextWindowSize( ImVec2( winW, winH ), ImGuiCond_Always );

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
	if( ImGui::Begin( "Loading...", NULL, flags ) )
	{
		// Outer window bevel
		{
			ImVec2 wpos = ImGui::GetWindowPos();
			ImVec2 wsize = ImGui::GetWindowSize();
			DrawBeveledRect( ImGui::GetWindowDrawList(), wpos,
				ImVec2( wpos.x + wsize.x, wpos.y + wsize.y ), true );
		}

		if( g_ConnServerName[0] )
		{
			ImGui::TextUnformatted( g_ConnServerName );
			DrawCS16Separator();
		}

		ImGui::TextWrapped( "%s", g_ConnStatusText );
		ImGui::Spacing();

		// Progress bar with sunken bevel
		ImGui::ProgressBar( g_ConnProgress, ImVec2( -1.0f, 0.0f ) );
		DrawBeveledRect( ImGui::GetWindowDrawList(),
			ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false );

		ImGui::Spacing();

		float buttonWidth = 100.0f;
		ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - buttonWidth ) * 0.5f );
		if( CS16Button( "Cancel", ImVec2( buttonWidth, 0 ) ) )
		{
			Cbuf_AddText( "disconnect\n" );
		}
	}
	ImGui::End();
}

// ============================================================================
// Console
// ============================================================================

static void DrawConsole( void )
{
	if( !g_ConsoleVisible )
		return;

	ImGuiIO &io = ImGui::GetIO();
	float consoleHeight = io.DisplaySize.y * 0.5f;

	ImGui::SetNextWindowPos( ImVec2( 0, 0 ), ImGuiCond_Always );
	ImGui::SetNextWindowSize( ImVec2( io.DisplaySize.x, consoleHeight ), ImGuiCond_Always );

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	// CS 1.6 console background color (dark brownish-grey, semi-transparent)
	ImGui::PushStyleColor( ImGuiCol_WindowBg, ImVec4( 0.18f, 0.17f, 0.15f, 0.78f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 8, 4 ) );

	if( ImGui::Begin( "##Console", NULL, flags ) )
	{
		// --- Log area (scrollable child) ---
		float inputHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
		float separatorHeight = 2.0f;

		if( ImGui::BeginChild( "##ConsoleLog", ImVec2( 0, -inputHeight - separatorHeight ), false,
			ImGuiWindowFlags_HorizontalScrollbar ) )
		{
			ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.91f, 0.91f, 0.91f, 1.0f ) );

			int count = ( g_ConsoleWritePos <= CONSOLE_MAX_LINES ) ? (int)g_ConsoleWritePos : CONSOLE_MAX_LINES;
			for( int i = 0; i < count; i++ )
			{
				int idx;
				if( g_ConsoleWritePos <= CONSOLE_MAX_LINES )
					idx = i;
				else
					idx = ( g_ConsoleWritePos + i ) % CONSOLE_MAX_LINES;
				ImGui::TextUnformatted( g_ConsoleLines[idx] );
			}

			ImGui::PopStyleColor();

			if( g_ConsoleScrollToBottom )
			{
				ImGui::SetScrollHereY( 1.0f );
				g_ConsoleScrollToBottom = false;
			}
		}
		ImGui::EndChild();

		// --- Separator line (orange-brown, CS 1.6 style) ---
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddRectFilled(
			p, ImVec2( p.x + io.DisplaySize.x, p.y + separatorHeight ),
			IM_COL32( 124, 91, 40, 255 ) );
		ImGui::Dummy( ImVec2( 0, separatorHeight ) );

		// --- Input line (green text with ] prompt) ---
		ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.0f, 1.0f, 0.0f, 1.0f ) );
		ImGui::PushStyleColor( ImGuiCol_FrameBg, ImVec4( 0, 0, 0, 0 ) );
		ImGui::PushStyleColor( ImGuiCol_FrameBgActive, ImVec4( 0, 0, 0, 0 ) );
		ImGui::PushStyleColor( ImGuiCol_FrameBgHovered, ImVec4( 0, 0, 0, 0 ) );

		ImGui::Text( "]" );
		ImGui::SameLine();

		ImGui::PushItemWidth( -1 );

		ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue;
		if( ImGui::InputText( "##ConInput", g_ConsoleInputBuf, sizeof( g_ConsoleInputBuf ), inputFlags ) )
		{
			if( g_ConsoleInputBuf[0] )
			{
				char echo[CONSOLE_LINE_LEN];
				Q_snprintf( echo, sizeof( echo ), "] %s", g_ConsoleInputBuf );
				int slot = g_ConsoleWritePos % CONSOLE_MAX_LINES;
				Q_strncpy( g_ConsoleLines[slot], echo, CONSOLE_LINE_LEN );
				g_ConsoleWritePos++;
				g_ConsoleScrollToBottom = true;

				Cbuf_AddTextf( "%s\n", g_ConsoleInputBuf );
				g_ConsoleInputBuf[0] = '\0';
			}
			ImGui::SetKeyboardFocusHere( -1 );
		}

		// Draw subtle sunken bevel around the input field
		DrawBeveledRect( ImGui::GetWindowDrawList(),
			ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false );

		if( ImGui::IsWindowAppearing() )
			ImGui::SetKeyboardFocusHere( -1 );

		ImGui::PopItemWidth();
		ImGui::PopStyleColor( 4 );
	}
	ImGui::End();

	ImGui::PopStyleVar( 2 );
	ImGui::PopStyleColor();
}

// ============================================================================
// Console command callbacks
// ============================================================================

static void Cmd_SlayerMenu_f( void )
{
	g_MenuVisible = !g_MenuVisible;
	if( g_MenuVisible )
		LoadSettings();
}

static void Cmd_SlayerConsole_f( void )
{
	g_ConsoleVisible = !g_ConsoleVisible;
	if( g_ConsoleVisible )
		g_ConsoleScrollToBottom = true;
}

// ============================================================================
// Public API (extern "C")
// ============================================================================

extern "C"
{

void Slayer_ImGui_Init( void )
{
	if( g_Initialized )
		return;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = NULL;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	SetupCS16Style();

	io.FontGlobalScale = 2.0f;

	Cmd_AddCommand( "slayer_menu", Cmd_SlayerMenu_f, "Toggle Slayer3D settings menu" );
	Cmd_AddCommand( "slayer_console", Cmd_SlayerConsole_f, "Toggle Slayer3D ImGui console" );

	g_Initialized = true;
	Con_Printf( "Slayer3D: ImGui menu initialized (GL deferred)\n" );
}

void Slayer_ImGui_Shutdown( void )
{
	if( !g_Initialized )
		return;

	if( g_GLInitialized )
	{
		ImGui_ImplXashGLES_Shutdown();
		g_GLInitialized = false;
	}
	ImGui::DestroyContext();
	g_Initialized = false;
}

void Slayer_ImGui_Frame( void )
{
	if( !g_Initialized )
		return;

	if( !g_GLInitialized )
	{
		if( !ImGui_ImplXashGLES_Init() )
		{
			Con_Printf( S_ERROR "Slayer_ImGui_Frame: GLES backend init failed\n" );
			return;
		}
		g_GLInitialized = true;
		Con_Printf( "Slayer3D: ImGui GL backend initialized\n" );
	}

	// Auto-clear connection progress when engine reaches active state
	if( g_ConnProgressState != CONNPROGRESS_NONE && cls.state == ca_active )
	{
		g_ConnProgressState = CONNPROGRESS_NONE;
		g_ConnProgress = 0.0f;
		g_ConnStatusText[0] = '\0';
		g_ConnServerName[0] = '\0';
	}

	// Prevent 1-frame flash of connection progress at map load start.
	// Wait at least 2 frames after the state transitions to non-NONE before
	// actually rendering, so transient flickers are suppressed.
	{
		static connprogress_state_e prev_state = CONNPROGRESS_NONE;
		static int frames_in_state = 0;

		if( g_ConnProgressState != prev_state )
		{
			prev_state = g_ConnProgressState;
			frames_in_state = 0;
		}
		else
		{
			frames_in_state++;
		}

		if( g_ConnProgressState != CONNPROGRESS_NONE && frames_in_state < 2 )
			return;
	}

	// Guard against zero display size during GL init / map load transitions
	if( refState.width <= 0 || refState.height <= 0 )
		return;

	if( !g_MenuVisible && !g_ConsoleVisible && g_ConnProgressState == CONNPROGRESS_NONE )
		return;

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2( (float)refState.width, (float)refState.height );
	io.DeltaTime = ( host.frametime > 0.0f ) ? host.frametime : ( 1.0f / 60.0f );

	ImGui_ImplXashGLES_NewFrame();
	ImGui::NewFrame();

	DrawConnectionProgress();
	DrawConsole();

	if( g_MenuVisible )
		DrawSettingsMenu();

	ImGui::Render();
	ImGui_ImplXashGLES_RenderDrawData( ImGui::GetDrawData() );

	{
		ImGuiIO &ioPost = ImGui::GetIO();
		Platform_EnableTextInput( ioPost.WantTextInput ? 1 : 0 );
	}
}

int Slayer_ImGui_TouchEvent( int type, int fingerID, float x, float y, float dx, float dy )
{
	if( !g_Initialized )
		return 0;

	if( !g_MenuVisible && !g_ConsoleVisible && g_ConnProgressState == CONNPROGRESS_NONE )
		return 0;

	ImGuiIO &io = ImGui::GetIO();

	float px = x * io.DisplaySize.x;
	float py = y * io.DisplaySize.y;

	switch( type )
	{
	case 0:
		io.AddMousePosEvent( px, py );
		io.AddMouseButtonEvent( 0, true );
		break;
	case 1:
		io.AddMousePosEvent( px, py );
		io.AddMouseButtonEvent( 0, false );
		break;
	case 2:
		io.AddMousePosEvent( px, py );
		break;
	}

	return 1;
}

int Slayer_ImGui_KeyEvent( int key, int down )
{
	if( !g_Initialized )
		return 0;

	if( !g_MenuVisible && !g_ConsoleVisible && g_ConnProgressState == CONNPROGRESS_NONE )
		return 0;

	ImGuiIO &io = ImGui::GetIO();

	ImGuiKey imgui_key = ImGuiKey_None;
	switch( key )
	{
	case K_TAB:       imgui_key = ImGuiKey_Tab; break;
	case K_ENTER:     imgui_key = ImGuiKey_Enter; break;
	case K_ESCAPE:    imgui_key = ImGuiKey_Escape; break;
	case K_SPACE:     imgui_key = ImGuiKey_Space; break;
	case K_BACKSPACE: imgui_key = ImGuiKey_Backspace; break;
	case K_DEL:       imgui_key = ImGuiKey_Delete; break;
	case K_UPARROW:   imgui_key = ImGuiKey_UpArrow; break;
	case K_DOWNARROW: imgui_key = ImGuiKey_DownArrow; break;
	case K_LEFTARROW: imgui_key = ImGuiKey_LeftArrow; break;
	case K_RIGHTARROW:imgui_key = ImGuiKey_RightArrow; break;
	case K_HOME:      imgui_key = ImGuiKey_Home; break;
	case K_END:       imgui_key = ImGuiKey_End; break;
	case K_PGUP:      imgui_key = ImGuiKey_PageUp; break;
	case K_PGDN:      imgui_key = ImGuiKey_PageDown; break;
	case K_INS:       imgui_key = ImGuiKey_Insert; break;
	default: break;
	}

	if( imgui_key != ImGuiKey_None )
		io.AddKeyEvent( imgui_key, down != 0 );

	return io.WantCaptureKeyboard ? 1 : 0;
}

void Slayer_ImGui_Toggle( void )
{
	g_MenuVisible = !g_MenuVisible;
	if( g_MenuVisible )
		LoadSettings();
}

void Slayer_ImGui_ToggleConsole( void )
{
	g_ConsoleVisible = !g_ConsoleVisible;
	if( g_ConsoleVisible )
		g_ConsoleScrollToBottom = true;
}

int Slayer_ImGui_IsConsoleVisible( void )
{
	return g_ConsoleVisible ? 1 : 0;
}

void Slayer_ImGui_ConnectionProgress_Connect( const char *server )
{
	g_ConnProgressState = CONNPROGRESS_CONNECTING;
	g_ConnProgress = 0.0f;
	Q_strncpy( g_ConnStatusText, "Establishing connection...", sizeof( g_ConnStatusText ) );
	if( server )
		Q_strncpy( g_ConnServerName, server, sizeof( g_ConnServerName ) );
	else
		Q_strncpy( g_ConnServerName, "Local Server", sizeof( g_ConnServerName ) );
}

void Slayer_ImGui_ConnectionProgress_Disconnect( void )
{
	g_ConnProgressState = CONNPROGRESS_NONE;
	g_ConnProgress = 0.0f;
	g_ConnStatusText[0] = '\0';
	g_ConnServerName[0] = '\0';
}

void Slayer_ImGui_ConnectionProgress_Download( const char *pszFileName, const char *pszServerName, int iCurrent, int iTotal, const char *comment )
{
	g_ConnProgressState = CONNPROGRESS_DOWNLOADING;
	if( iTotal > 0 )
		g_ConnProgress = (float)iCurrent / (float)iTotal;
	else
		g_ConnProgress = 0.0f;

	if( comment && comment[0] )
		Q_snprintf( g_ConnStatusText, sizeof( g_ConnStatusText ), "%s\n%s from %s (%d/%d)", comment, pszFileName ? pszFileName : "", pszServerName ? pszServerName : "", iCurrent, iTotal );
	else
		Q_snprintf( g_ConnStatusText, sizeof( g_ConnStatusText ), "Downloading %s from %s (%d/%d)", pszFileName ? pszFileName : "", pszServerName ? pszServerName : "", iCurrent, iTotal );

	if( pszServerName )
		Q_strncpy( g_ConnServerName, pszServerName, sizeof( g_ConnServerName ) );
}

void Slayer_ImGui_ConnectionProgress_DownloadEnd( void )
{
	g_ConnProgressState = CONNPROGRESS_NONE;
	g_ConnProgress = 0.0f;
	g_ConnStatusText[0] = '\0';
	g_ConnServerName[0] = '\0';
}

void Slayer_ImGui_ConnectionProgress_Precache( void )
{
	g_ConnProgressState = CONNPROGRESS_PRECACHING;
	g_ConnProgress = 0.5f;
	Q_strncpy( g_ConnStatusText, "Precaching resources...", sizeof( g_ConnStatusText ) );
}

void Slayer_ImGui_ConnectionProgress_ChangeLevel( void )
{
	g_ConnProgressState = CONNPROGRESS_CHANGELEVEL;
	g_ConnProgress = 0.0f;
	Q_strncpy( g_ConnStatusText, "Changing level...", sizeof( g_ConnStatusText ) );
}

void Slayer_ImGui_ConsolePrint( const char *text )
{
	if( !g_Initialized )
		return;

	if( !text || !text[0] )
		return;

	for( const char *p = text; *p; p++ )
	{
		if( *p == '\n' || g_ConsoleLineBufPos >= CONSOLE_LINE_LEN - 1 )
		{
			g_ConsoleLineBuf[g_ConsoleLineBufPos] = '\0';
			int slot = g_ConsoleWritePos % CONSOLE_MAX_LINES;
			Q_strncpy( g_ConsoleLines[slot], g_ConsoleLineBuf, CONSOLE_LINE_LEN );
			g_ConsoleWritePos++;
			g_ConsoleScrollToBottom = true;
			g_ConsoleLineBufPos = 0;
		}
		else
		{
			g_ConsoleLineBuf[g_ConsoleLineBufPos++] = *p;
		}
	}
}

int Slayer_ImGui_IsActive( void )
{
	if( !g_Initialized )
		return 0;
	if( g_MenuVisible || g_ConsoleVisible || g_ConnProgressState != CONNPROGRESS_NONE )
		return 1;
	return 0;
}

int Slayer_ImGui_CharEvent( int key )
{
	if( !g_Initialized )
		return 0;
	if( !g_MenuVisible && !g_ConsoleVisible && g_ConnProgressState == CONNPROGRESS_NONE )
		return 0;

	ImGuiIO &io = ImGui::GetIO();
	if( key >= 32 )
		io.AddInputCharacter( (unsigned int)key );

	return io.WantTextInput ? 1 : 0;
}

} // extern "C"
