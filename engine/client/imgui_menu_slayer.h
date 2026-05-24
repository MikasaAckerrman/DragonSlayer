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

#ifdef __cplusplus
}
#endif

#endif // IMGUI_MENU_SLAYER_H
