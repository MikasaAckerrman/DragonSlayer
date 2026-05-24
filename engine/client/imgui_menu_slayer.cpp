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
#include "keydefs.h"
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

// --- Main Menu state ---
static bool g_MainMenuVisible = false;

// --- Console state ---
static bool g_ConsoleVisible = false;

#define CONSOLE_MAX_LINES 1024
#define CONSOLE_LINE_LEN  256

static char  g_ConsoleLines[CONSOLE_MAX_LINES][CONSOLE_LINE_LEN];
static int   g_ConsoleLineCount = 0;
static int   g_ConsoleWritePos = 0;  // next write slot in ring buffer
static bool  g_ConsoleScrollToBottom = true;
static char  g_ConsoleInputBuf[512] = "";

// Line accumulator for ConsolePrint (file-scope to make intent explicit)
static char  g_ConsoleLineBuf[CONSOLE_LINE_LEN];
static int   g_ConsoleLineBufPos = 0;

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

// ---- Connection Progress window ----
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
		if( g_ConnServerName[0] )
		{
			ImGui::TextUnformatted( g_ConnServerName );
			ImGui::Separator();
		}

		ImGui::TextWrapped( "%s", g_ConnStatusText );
		ImGui::Spacing();
		ImGui::ProgressBar( g_ConnProgress, ImVec2( -1.0f, 0.0f ) );
		ImGui::Spacing();

		float buttonWidth = 100.0f;
		ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - buttonWidth ) * 0.5f );
		if( ImGui::Button( "Cancel", ImVec2( buttonWidth, 0 ) ) )
		{
			Cbuf_AddText( "disconnect\n" );
		}
	}
	ImGui::End();
}

// ---- Main Menu window ----
static void DrawMainMenu( void )
{
	if( !g_MainMenuVisible )
		return;

	ImGuiIO &io = ImGui::GetIO();
	ImVec2 center( io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f );
	ImGui::SetNextWindowPos( center, ImGuiCond_Always, ImVec2( 0.5f, 0.5f ) );

	float winW = ( 300.0f < io.DisplaySize.x * 0.6f ) ? 300.0f : io.DisplaySize.x * 0.6f;
	float winH = ( 350.0f < io.DisplaySize.y * 0.7f ) ? 350.0f : io.DisplaySize.y * 0.7f;
	ImGui::SetNextWindowSize( ImVec2( winW, winH ), ImGuiCond_Always );

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar;

	if( ImGui::Begin( "##MainMenu", NULL, flags ) )
	{
		// Title centered at top
		const char *title = "Counter-Strike";
		float titleWidth = ImGui::CalcTextSize( title ).x;
		ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - titleWidth ) * 0.5f );
		ImGui::TextUnformatted( title );
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		float btnWidth = ImGui::GetContentRegionAvail().x;
		float btnHeight = 36.0f;

		if( ImGui::Button( "New Game", ImVec2( btnWidth, btnHeight ) ) )
		{
			Cbuf_AddText( "maxplayers 1\nmap de_dust2\n" );
		}
		if( ImGui::Button( "Find Servers", ImVec2( btnWidth, btnHeight ) ) )
		{
			Cbuf_AddText( "openserverbrowser\n" );
		}
		if( ImGui::Button( "Options", ImVec2( btnWidth, btnHeight ) ) )
		{
			Slayer_ImGui_Toggle();
		}
		if( ImGui::Button( "Console", ImVec2( btnWidth, btnHeight ) ) )
		{
			g_ConsoleVisible = true;
		}
		if( ImGui::Button( "Quit", ImVec2( btnWidth, btnHeight ) ) )
		{
			Cbuf_AddText( "quit\n" );
		}
	}
	ImGui::End();
}

// ---- Console window ----
static void DrawConsole( void )
{
	if( !g_ConsoleVisible )
		return;

	ImGuiIO &io = ImGui::GetIO();
	float winW = io.DisplaySize.x * 0.8f;
	float winH = io.DisplaySize.y * 0.6f;
	ImVec2 pos( io.DisplaySize.x * 0.5f, io.DisplaySize.y );
	ImGui::SetNextWindowPos( pos, ImGuiCond_Always, ImVec2( 0.5f, 1.0f ) );
	ImGui::SetNextWindowSize( ImVec2( winW, winH ), ImGuiCond_Always );

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
	if( ImGui::Begin( "Console", &g_ConsoleVisible, flags ) )
	{
		// Scrollable child region for log lines
		float footerHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing();
		if( ImGui::BeginChild( "ConsoleScroll", ImVec2( 0, -footerHeight ), false, ImGuiWindowFlags_HorizontalScrollbar ) )
		{
			int count = ( g_ConsoleLineCount < CONSOLE_MAX_LINES ) ? g_ConsoleLineCount : CONSOLE_MAX_LINES;
			for( int i = 0; i < count; i++ )
			{
				int idx;
				if( g_ConsoleLineCount <= CONSOLE_MAX_LINES )
					idx = i;
				else
					idx = ( g_ConsoleWritePos + i ) % CONSOLE_MAX_LINES;

				ImGui::TextUnformatted( g_ConsoleLines[idx] );
			}

			if( g_ConsoleScrollToBottom )
			{
				ImGui::SetScrollHereY( 1.0f );
				g_ConsoleScrollToBottom = false;
			}
		}
		ImGui::EndChild();

		// Input line
		ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue;
		bool reclaim_focus = false;

		if( ImGui::InputText( "##ConInput", g_ConsoleInputBuf, sizeof( g_ConsoleInputBuf ), inputFlags ) )
		{
			if( g_ConsoleInputBuf[0] )
			{
				// Add typed text to log
				char echo[CONSOLE_LINE_LEN];
				Q_snprintf( echo, sizeof( echo ), "> %s", g_ConsoleInputBuf );
				int slot = g_ConsoleWritePos % CONSOLE_MAX_LINES;
				Q_strncpy( g_ConsoleLines[slot], echo, CONSOLE_LINE_LEN );
				g_ConsoleWritePos++;
				if( g_ConsoleLineCount < CONSOLE_MAX_LINES )
					g_ConsoleLineCount++;
				g_ConsoleScrollToBottom = true;

				// Execute command
				Cbuf_AddTextf( "%s\n", g_ConsoleInputBuf );
				g_ConsoleInputBuf[0] = '\0';
			}
			reclaim_focus = true;
		}

		// Auto-focus input
		if( reclaim_focus )
			ImGui::SetKeyboardFocusHere( -1 );

		ImGui::SameLine();
		if( ImGui::Button( "Close" ) )
		{
			g_ConsoleVisible = false;
		}
	}
	ImGui::End();
}

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
	io.IniFilename = NULL; // Disable imgui.ini file
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	SetupCS16Style();

	io.FontGlobalScale = 2.0f;

	if( !ImGui_ImplXashGLES_Init() )
	{
		Con_Printf( S_ERROR "Slayer_ImGui_Init: GLES backend init failed\n" );
		ImGui::DestroyContext();
		return;
	}

	Cmd_AddCommand( "slayer_menu", Cmd_SlayerMenu_f, "Toggle Slayer3D settings menu" );
	Cmd_AddCommand( "slayer_console", Cmd_SlayerConsole_f, "Toggle Slayer3D ImGui console" );

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

	// Auto-clear connection progress when engine reaches active state
	if( g_ConnProgressState != CONNPROGRESS_NONE && cls.state == ca_active )
	{
		g_ConnProgressState = CONNPROGRESS_NONE;
		g_ConnProgress = 0.0f;
		g_ConnStatusText[0] = '\0';
		g_ConnServerName[0] = '\0';
	}

	// Update main menu visibility based on engine state
	g_MainMenuVisible = ( cls.state == ca_disconnected && cls.key_dest == key_menu );

	// If nothing to draw, skip frame
	if( !g_MenuVisible && !g_ConsoleVisible && !g_MainMenuVisible && g_ConnProgressState == CONNPROGRESS_NONE )
		return;

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2( (float)refState.width, (float)refState.height );
	io.DeltaTime = ( host.frametime > 0.0f ) ? host.frametime : ( 1.0f / 60.0f );

	ImGui_ImplXashGLES_NewFrame();
	ImGui::NewFrame();

	DrawConnectionProgress();
	DrawMainMenu();
	DrawConsole();

	if( g_MenuVisible )
		DrawSettingsMenu();

	ImGui::Render();
	ImGui_ImplXashGLES_RenderDrawData( ImGui::GetDrawData() );
}

int Slayer_ImGui_TouchEvent( int type, int fingerID, float x, float y, float dx, float dy )
{
	if( !g_Initialized )
		return 0;

	// Consume input when any ImGui window is visible
	if( !g_MenuVisible && !g_ConsoleVisible && !g_MainMenuVisible && g_ConnProgressState == CONNPROGRESS_NONE )
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

	// Consume all touch events only if ImGui wants the mouse
	return io.WantCaptureMouse ? 1 : 0;
}

int Slayer_ImGui_KeyEvent( int key, int down )
{
	if( !g_Initialized )
		return 0;

	// Don't process keys when no ImGui window is visible
	if( !g_MenuVisible && !g_ConsoleVisible && !g_MainMenuVisible && g_ConnProgressState == CONNPROGRESS_NONE )
		return 0;

	ImGuiIO &io = ImGui::GetIO();

	// Forward printable characters as text input for InputText widgets
	if( down && key >= 32 && key < 127 )
	{
		io.AddInputCharacter( (unsigned int)key );
	}

	// Forward basic key events to ImGui
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

	// Split input on newlines and add each line to ring buffer
	for( const char *p = text; *p; p++ )
	{
		if( *p == '\n' || g_ConsoleLineBufPos >= CONSOLE_LINE_LEN - 1 )
		{
			g_ConsoleLineBuf[g_ConsoleLineBufPos] = '\0';
			int slot = g_ConsoleWritePos % CONSOLE_MAX_LINES;
			Q_strncpy( g_ConsoleLines[slot], g_ConsoleLineBuf, CONSOLE_LINE_LEN );
			g_ConsoleWritePos++;
			if( g_ConsoleLineCount < CONSOLE_MAX_LINES )
				g_ConsoleLineCount++;
			g_ConsoleScrollToBottom = true;
			g_ConsoleLineBufPos = 0;
		}
		else
		{
			g_ConsoleLineBuf[g_ConsoleLineBufPos++] = *p;
		}
	}
}

} // extern "C"
