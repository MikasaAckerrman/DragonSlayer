/*
imgui_menu_slayer.h - Slayer3D ImGui settings menu API
Copyright (C) 2026 Slayer3D contributors

Public C-linkage API so the menu can be called from engine C code.
*/

#ifndef IMGUI_MENU_SLAYER_H
#define IMGUI_MENU_SLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

void Slayer_ImGui_Init( void );
void Slayer_ImGui_Shutdown( void );
void Slayer_ImGui_Frame( void );
int  Slayer_ImGui_TouchEvent( int type, int fingerID, float x, float y, float dx, float dy );
int  Slayer_ImGui_KeyEvent( int key, int down );
void Slayer_ImGui_Toggle( void );

// Toggle ImGui console (replaces Con_ToggleConsole_f for tilde key)
void Slayer_ImGui_ToggleConsole( void );

// Returns 1 when ImGui console overlay is visible
int  Slayer_ImGui_IsConsoleVisible( void );

// Returns 1 when any ImGui window is being shown
int  Slayer_ImGui_IsActive( void );

// Text character input (called from CL_CharEvent)
int  Slayer_ImGui_CharEvent( int key );

// Connection progress hooks
void Slayer_ImGui_ConnectionProgress_Connect( const char *server );
void Slayer_ImGui_ConnectionProgress_Disconnect( void );
void Slayer_ImGui_ConnectionProgress_Download( const char *pszFileName, const char *pszServerName, int iCurrent, int iTotal, const char *comment );
void Slayer_ImGui_ConnectionProgress_DownloadEnd( void );
void Slayer_ImGui_ConnectionProgress_Precache( void );
void Slayer_ImGui_ConnectionProgress_ChangeLevel( void );

// Console output capture hook
void Slayer_ImGui_ConsolePrint( const char *text );

#ifdef __cplusplus
}
#endif

#endif // IMGUI_MENU_SLAYER_H
