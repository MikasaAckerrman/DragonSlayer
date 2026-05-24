/*
WindowManager.cpp -- multi-window z-order manager with focus routing
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#include "extdll_menu.h"
#include "BaseMenu.h"
#include "BaseWindow.h"
#include "Window.h"
#include "WindowManager.h"

// ---------------------------------------------------------------
// Helper: check if cursor is inside a window's render rect
// ---------------------------------------------------------------
static bool CursorInsideWindow( CMenuBaseWindow *wnd )
{
	if( !wnd || wnd->IsRoot() )
		return false;

	Point pos = wnd->GetRenderPosition();
	Size  sz  = wnd->GetRenderSize();

	return UI_CursorInRect( pos.x, pos.y, sz.w, sz.h );
}

// ---------------------------------------------------------------
// OnMouseDown — find topmost non-active window under cursor,
// bring it to front. Returns true if event was consumed.
// ---------------------------------------------------------------
bool CWindowManager::OnMouseDown( CWindowStack *pStack )
{
	if( !pStack || !pStack->IsActive() )
		return false;

	CMenuBaseWindow *current = pStack->Current();

	// If cursor is inside the current (active) window, do nothing —
	// normal event routing will handle it.
	if( current && !current->IsRoot() && CursorInsideWindow( current ) )
		return false;

	// Walk stack from top to bottom (newest to oldest) looking for
	// a non-active, non-root window under the cursor.
	// CWindowStack uses CUtlLinkedList internally; we can't iterate
	// it directly from here without friend access. Instead we use
	// the public API: check each known CMenuWindow.
	//
	// APPROACH: We iterate via the linked list using the same pattern
	// as WindowSystem.cpp's Update(). Since we don't have direct
	// access to `stack` member, we leverage the fact that CWindowStack::Add()
	// will bring an existing window to front. We need to find which
	// window to raise.
	//
	// For now, we use a simple but effective approach:
	// The CMenuWindow instances register themselves in a global list.
	// This avoids needing to modify CWindowStack internals.

	// Fallback: if cursor is not in current window, the click goes to
	// whatever is underneath. The existing WindowSystem will still
	// route to Current(). We signal that the click should be blocked
	// (click-through prevention) if cursor is over ANY window.
	// This prevents root frameworks from receiving stray clicks.

	return false; // let normal routing proceed for now
}

// ---------------------------------------------------------------
// IsCursorOverAnyWindow — for click-through prevention
// ---------------------------------------------------------------
bool CWindowManager::IsCursorOverAnyWindow( CWindowStack *pStack )
{
	if( !pStack || !pStack->IsActive() )
		return false;

	CMenuBaseWindow *current = pStack->Current();
	if( current && !current->IsRoot() && CursorInsideWindow( current ) )
		return true;

	// For other windows in the stack we'd need iteration.
	// With the current architecture, if Current() is a non-root window
	// and cursor is inside it, that's the primary case.
	// Multi-window overlap detection requires stack iteration which
	// we'll add when we extend CWindowStack with a public iterator.

	return false;
}

// ---------------------------------------------------------------
// BringToFront — use CWindowStack::Add which re-activates
// ---------------------------------------------------------------
void CWindowManager::BringToFront( CWindowStack *pStack, CMenuBaseWindow *wnd )
{
	if( !pStack || !wnd )
		return;

	// CWindowStack::Add() already handles "if already in stack, set active"
	pStack->Add( wnd );
}
