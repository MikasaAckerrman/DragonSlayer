/*
imgui_menu_slayer.cpp - Slayer3D Dear ImGui CS 1.6-style settings menu
Copyright (C) 2026 Slayer3D contributors

This module integrates Dear ImGui into the Xash3D engine to provide
a CS 1.6-inspired Configuration window with tabs for Game, Keyboard,
Mouse, Audio, Video, and Voice settings.
*/

extern "C"
{
#include "common.h"
#include "client.h"
#include "ref_common.h"
}

#include "imgui.h"
#include "imgui_impl_xash_gles.h"
#include "imgui_menu_slayer.h"

// ============================================================================
// State
// ============================================================================

static bool g_Initialized = false;
static bool g_MenuVisible = false;

// Pending settings (applied on OK/Apply)
static float g_Volume = 1.0f;
static float g_MusicVolume = 1.0f;
static float g_Sensitivity = 3.0f;
static float g_Pitch = 0.022f;
static float g_Brightness = 1.0f;
static float g_VoiceScale = 1.0f;
static int   g_Crosshair = 1;
static int   g_AutoSwitch = 1;
static int   g_RawInput = 0;
static int   g_ReverseMouse = 0;
static int   g_HQSound = 1;
static int   g_Surround = 0;
static int   g_VoiceEnable = 1;
static int   g_Vsync = 0;
static int   g_VoiceInputFromFile = 0;

// ============================================================================
// Helpers
// ============================================================================

static void LoadSettings( void )
{
	g_Volume       = Cvar_VariableValue( "volume" );
	g_MusicVolume  = Cvar_VariableValue( "MP3Volume" );
	g_Sensitivity  = Cvar_VariableValue( "sensitivity" );
	g_Pitch        = Cvar_VariableValue( "m_pitch" );
	g_Brightness   = Cvar_VariableValue( "gamma" );
	g_VoiceScale   = Cvar_VariableValue( "voice_scale" );
	g_Crosshair    = (int)Cvar_VariableValue( "crosshair" );
	g_AutoSwitch   = (int)Cvar_VariableValue( "hud_fastswitch" );
	g_RawInput     = (int)Cvar_VariableValue( "m_rawinput" );
	g_ReverseMouse = (int)Cvar_VariableValue( "m_reverse" );
	g_HQSound      = (int)Cvar_VariableValue( "s_lerping" );
	g_Surround     = (int)Cvar_VariableValue( "s_surround" );
	g_VoiceEnable  = (int)Cvar_VariableValue( "voice_enable" );
	g_Vsync        = (int)Cvar_VariableValue( "gl_vsync" );
	g_VoiceInputFromFile = (int)Cvar_VariableValue( "voice_inputfromfile" );
}

static void ApplySettings( void )
{
	Cvar_SetValue( "volume", g_Volume );
	Cvar_SetValue( "MP3Volume", g_MusicVolume );
	Cvar_SetValue( "sensitivity", g_Sensitivity );
	Cvar_SetValue( "m_pitch", g_Pitch );
	Cvar_SetValue( "gamma", g_Brightness );
	Cvar_SetValue( "voice_scale", g_VoiceScale );
	Cvar_SetValue( "crosshair", (float)g_Crosshair );
	Cvar_SetValue( "hud_fastswitch", (float)g_AutoSwitch );
	Cvar_SetValue( "m_rawinput", (float)g_RawInput );
	Cvar_SetValue( "m_reverse", (float)g_ReverseMouse );
	Cvar_SetValue( "s_lerping", (float)g_HQSound );
	Cvar_SetValue( "s_surround", (float)g_Surround );
	Cvar_SetValue( "voice_enable", (float)g_VoiceEnable );
	Cvar_SetValue( "voice_inputfromfile", (float)g_VoiceInputFromFile );

	// Use Cbuf_AddText so the cvar system sets FCVAR_CHANGED,
	// which is required for GL_UpdateSwapInterval to take effect.
	Cbuf_AddTextf( "gl_vsync %d\n", g_Vsync );
}

// ============================================================================
// ImGui style: CS 1.6 look
// ============================================================================

static void SetupCS16Style( void )
{
	ImGuiStyle &style = ImGui::GetStyle();
	ImVec4 *colors = style.Colors;

	style.WindowRounding   = 0.0f;
	style.FrameRounding    = 0.0f;
	style.ScrollbarRounding = 0.0f;
	style.TabRounding      = 0.0f;
	style.WindowBorderSize = 1.0f;
	style.FrameBorderSize  = 1.0f;
	style.WindowPadding    = ImVec2( 8, 8 );
	style.FramePadding     = ImVec2( 4, 3 );
	style.ItemSpacing      = ImVec2( 8, 4 );

	// Window background: CS 1.6 grey (#4D4D4D)
	colors[ImGuiCol_WindowBg]         = ImVec4( 0.30f, 0.30f, 0.30f, 1.00f );
	colors[ImGuiCol_TitleBg]          = ImVec4( 0.20f, 0.20f, 0.20f, 1.00f );
	colors[ImGuiCol_TitleBgActive]    = ImVec4( 0.25f, 0.25f, 0.25f, 1.00f );
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4( 0.15f, 0.15f, 0.15f, 1.00f );

	// Buttons
	colors[ImGuiCol_Button]           = ImVec4( 0.40f, 0.40f, 0.40f, 1.00f );
	colors[ImGuiCol_ButtonHovered]    = ImVec4( 0.50f, 0.50f, 0.50f, 1.00f );
	colors[ImGuiCol_ButtonActive]     = ImVec4( 0.35f, 0.35f, 0.35f, 1.00f );

	// Frame background (sliders, checkboxes)
	colors[ImGuiCol_FrameBg]          = ImVec4( 0.20f, 0.20f, 0.20f, 1.00f );
	colors[ImGuiCol_FrameBgHovered]   = ImVec4( 0.25f, 0.25f, 0.25f, 1.00f );
	colors[ImGuiCol_FrameBgActive]    = ImVec4( 0.30f, 0.30f, 0.30f, 1.00f );

	// Tabs
	colors[ImGuiCol_Tab]              = ImVec4( 0.25f, 0.25f, 0.25f, 1.00f );
	colors[ImGuiCol_TabHovered]       = ImVec4( 0.45f, 0.45f, 0.45f, 1.00f );
	colors[ImGuiCol_TabSelected]      = ImVec4( 0.35f, 0.35f, 0.35f, 1.00f );

	// Check mark / slider grab
	colors[ImGuiCol_CheckMark]        = ImVec4( 0.90f, 0.90f, 0.90f, 1.00f );
	colors[ImGuiCol_SliderGrab]       = ImVec4( 0.60f, 0.60f, 0.60f, 1.00f );
	colors[ImGuiCol_SliderGrabActive] = ImVec4( 0.80f, 0.80f, 0.80f, 1.00f );

	// Separator / Border
	colors[ImGuiCol_Border]           = ImVec4( 0.50f, 0.50f, 0.50f, 1.00f );
	colors[ImGuiCol_Separator]        = ImVec4( 0.50f, 0.50f, 0.50f, 1.00f );

	// Text
	colors[ImGuiCol_Text]             = ImVec4( 1.00f, 1.00f, 1.00f, 1.00f );
	colors[ImGuiCol_TextDisabled]     = ImVec4( 0.60f, 0.60f, 0.60f, 1.00f );

	// Header (for collapsing headers, group boxes)
	colors[ImGuiCol_Header]           = ImVec4( 0.35f, 0.35f, 0.35f, 1.00f );
	colors[ImGuiCol_HeaderHovered]    = ImVec4( 0.40f, 0.40f, 0.40f, 1.00f );
	colors[ImGuiCol_HeaderActive]     = ImVec4( 0.45f, 0.45f, 0.45f, 1.00f );

	// Popup / Tooltip
	colors[ImGuiCol_PopupBg]          = ImVec4( 0.25f, 0.25f, 0.25f, 1.00f );
}

// ============================================================================
// Menu drawing
// ============================================================================

static void DrawTabGame( void )
{
	bool cross = ( g_Crosshair != 0 );
	if( ImGui::Checkbox( "Crosshair", &cross ) )
		g_Crosshair = cross ? 1 : 0;

	bool autoswitch = ( g_AutoSwitch != 0 );
	if( ImGui::Checkbox( "Auto weapon switch", &autoswitch ) )
		g_AutoSwitch = autoswitch ? 1 : 0;

	ImGui::Separator();
	ImGui::TextUnformatted( "HUD Color" );
	ImGui::TextDisabled( "(Change via hud_color cvar)" );
}

static void DrawTabKeyboard( void )
{
	ImGui::TextWrapped(
		"Key bindings can be configured via the console.\n"
		"Use 'bind <key> <command>' to set bindings.\n"
		"Example: bind \"F5\" \"slayer_menu\""
	);
}

static void DrawTabMouse( void )
{
	ImGui::SliderFloat( "Sensitivity", &g_Sensitivity, 0.1f, 20.0f, "%.2f" );
	ImGui::SliderFloat( "m_pitch", &g_Pitch, 0.001f, 0.05f, "%.4f" );

	bool raw = ( g_RawInput != 0 );
	if( ImGui::Checkbox( "Raw input", &raw ) )
		g_RawInput = raw ? 1 : 0;

	bool rev = ( g_ReverseMouse != 0 );
	if( ImGui::Checkbox( "Reverse mouse", &rev ) )
		g_ReverseMouse = rev ? 1 : 0;
}

static void DrawTabAudio( void )
{
	ImGui::SliderFloat( "Volume", &g_Volume, 0.0f, 1.0f, "%.2f" );
	ImGui::SliderFloat( "Music Volume", &g_MusicVolume, 0.0f, 1.0f, "%.2f" );

	bool hq = ( g_HQSound != 0 );
	if( ImGui::Checkbox( "High quality sound", &hq ) )
		g_HQSound = hq ? 1 : 0;

	bool surr = ( g_Surround != 0 );
	if( ImGui::Checkbox( "Surround sound", &surr ) )
		g_Surround = surr ? 1 : 0;

	ImGui::Separator();
	ImGui::TextUnformatted( "Voice" );

	bool voice = ( g_VoiceEnable != 0 );
	if( ImGui::Checkbox( "Enable voice", &voice ) )
		g_VoiceEnable = voice ? 1 : 0;
}

static void DrawTabVideo( void )
{
	ImGui::SliderFloat( "Brightness / Gamma", &g_Brightness, 0.5f, 3.0f, "%.2f" );

	bool vsync = ( g_Vsync != 0 );
	if( ImGui::Checkbox( "VSync", &vsync ) )
		g_Vsync = vsync ? 1 : 0;
}

static void DrawTabVoice( void )
{
	bool ve = ( g_VoiceEnable != 0 );
	if( ImGui::Checkbox( "Enable voice communication", &ve ) )
		g_VoiceEnable = ve ? 1 : 0;

	ImGui::SliderFloat( "Voice scale", &g_VoiceScale, 0.0f, 2.0f, "%.2f" );

	bool fromfile = ( g_VoiceInputFromFile != 0 );
	if( ImGui::Checkbox( "Voice input from file", &fromfile ) )
		g_VoiceInputFromFile = fromfile ? 1 : 0;
}

static void DrawSettingsMenu( void )
{
	ImGuiIO &io = ImGui::GetIO();

	// Center the window on screen
	ImVec2 center( io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f );
	ImGui::SetNextWindowPos( center, ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );
	ImGui::SetNextWindowSize( ImVec2( 520, 400 ), ImGuiCond_Appearing );

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
	if( !ImGui::Begin( "Configuration", &g_MenuVisible, flags ) )
	{
		ImGui::End();
		return;
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

	ImGui::Separator();

	// OK / Cancel / Apply buttons
	float buttonWidth = 80.0f;
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	float totalWidth = buttonWidth * 3.0f + spacing * 2.0f;
	ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - totalWidth ) * 0.5f );

	if( ImGui::Button( "OK", ImVec2( buttonWidth, 0 ) ) )
	{
		ApplySettings();
		g_MenuVisible = false;
	}
	ImGui::SameLine();
	if( ImGui::Button( "Cancel", ImVec2( buttonWidth, 0 ) ) )
	{
		g_MenuVisible = false;
	}
	ImGui::SameLine();
	if( ImGui::Button( "Apply", ImVec2( buttonWidth, 0 ) ) )
	{
		ApplySettings();
	}

	ImGui::End();
}

// ============================================================================
// Console command callback
// ============================================================================

static void Cmd_SlayerMenu_f( void )
{
	g_MenuVisible = !g_MenuVisible;
	if( g_MenuVisible )
		LoadSettings();
}

// ============================================================================
// Public API (extern "C")
// ============================================================================

extern "C"
{

static convar_t *g_pCvarImguiMenu = NULL;

void Slayer_ImGui_Init( void )
{
	if( g_Initialized )
		return;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = NULL; // Disable imgui.ini file
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	SetupCS16Style();

	if( !ImGui_ImplXashGLES_Init() )
	{
		Con_Printf( S_ERROR "Slayer_ImGui_Init: GLES backend init failed\n" );
		ImGui::DestroyContext();
		return;
	}

	Cmd_AddCommand( "slayer_menu", Cmd_SlayerMenu_f, "Toggle Slayer3D settings menu" );

	g_Initialized = true;
	Con_Printf( "Slayer3D: ImGui menu initialized\n" );
}

void Slayer_ImGui_Shutdown( void )
{
	if( !g_Initialized )
		return;

	ImGui_ImplXashGLES_Shutdown();
	ImGui::DestroyContext();
	g_Initialized = false;
}

void Slayer_ImGui_Frame( void )
{
	if( !g_Initialized )
		return;

	if( !g_MenuVisible )
		return;

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2( (float)refState.width, (float)refState.height );
	io.DeltaTime = ( host.frametime > 0.0f ) ? host.frametime : ( 1.0f / 60.0f );

	ImGui_ImplXashGLES_NewFrame();
	ImGui::NewFrame();

	DrawSettingsMenu();

	ImGui::Render();
	ImGui_ImplXashGLES_RenderDrawData( ImGui::GetDrawData() );
}

int Slayer_ImGui_TouchEvent( int type, int fingerID, float x, float y, float dx, float dy )
{
	if( !g_Initialized || !g_MenuVisible )
		return 0;

	ImGuiIO &io = ImGui::GetIO();

	// Convert normalized coords (0..1) to display pixels
	float px = x * io.DisplaySize.x;
	float py = y * io.DisplaySize.y;

	// type: 0 = down, 1 = up, 2 = move (matching touchEventType enum)
	switch( type )
	{
	case 0: // finger down
		io.AddMousePosEvent( px, py );
		io.AddMouseButtonEvent( 0, true );
		break;
	case 1: // finger up
		io.AddMousePosEvent( px, py );
		io.AddMouseButtonEvent( 0, false );
		break;
	case 2: // finger move
		io.AddMousePosEvent( px, py );
		break;
	}

	// Consume all touch events while menu is open
	return 1;
}

int Slayer_ImGui_KeyEvent( int key, int down )
{
	if( !g_Initialized || !g_MenuVisible )
		return 0;

	// Consume key events while the menu is open
	return 1;
}

void Slayer_ImGui_Toggle( void )
{
	g_MenuVisible = !g_MenuVisible;
	if( g_MenuVisible )
		LoadSettings();
}

} // extern "C"
