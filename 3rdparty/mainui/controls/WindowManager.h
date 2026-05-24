/*
WindowManager.h -- multi-window z-order manager with focus routing
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#ifndef WINDOWMANAGER_H
#define WINDOWMANAGER_H

#include "WindowSystem.h"

class CMenuWindow;

// ============================================================
// CWindowManager — sits on top of CWindowStack, provides:
//   - Bring-to-front on click (focus follows click)
//   - Click-through prevention (events don't pass through windows)
//   - Z-order query
//
// Usage: call CWindowManager::OnMouseDown() from the stack's
// KeyDownEvent before forwarding to Current(). If it returns
// true, event was consumed (window was raised or click was
// inside a non-active window that is now active).
// ============================================================
class CWindowManager
{
public:
	// Call on left-mouse-down. Returns true if event consumed
	// (raised a window or blocked by click-through prevention).
	// pStack — the window stack to operate on.
	static bool OnMouseDown( CWindowStack *pStack );

	// Check if cursor is inside any CMenuWindow in the stack
	// (used for click-through prevention in underlying root menus).
	static bool IsCursorOverAnyWindow( CWindowStack *pStack );

	// Bring a specific window to front (make it active).
	static void BringToFront( CWindowStack *pStack, CMenuBaseWindow *wnd );
};

#endif // WINDOWMANAGER_H
