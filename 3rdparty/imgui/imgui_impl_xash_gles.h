/*
imgui_impl_xash_gles.h - Dear ImGui backend for Xash3D (OpenGL ES 2.0)
Copyright (C) 2026 Slayer3D contributors

This is a minimal ImGui rendering backend using raw GLES2 calls.
It does NOT depend on GLEW, GLAD, or any loader library.
*/

#ifndef IMGUI_IMPL_XASH_GLES_H
#define IMGUI_IMPL_XASH_GLES_H

#include "imgui.h"

bool ImGui_ImplXashGLES_Init( void );
void ImGui_ImplXashGLES_Shutdown( void );
void ImGui_ImplXashGLES_NewFrame( void );
void ImGui_ImplXashGLES_RenderDrawData( ImDrawData *draw_data );

#endif // IMGUI_IMPL_XASH_GLES_H
