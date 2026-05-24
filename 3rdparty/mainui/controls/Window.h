/*
Window.h -- draggable windowed dialog (CS 1.6 PC style)
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#ifndef MENUWINDOW_H
#define MENUWINDOW_H

#include "BaseWindow.h"

// ============================================================
// CMenuWindow — a draggable, closeable dialog window with
// title bar, icon, [X] button, and CS 1.6-style chrome.
//
// Usage:
//   class CMyDialog : public CMenuWindow { ... };
//   myDialog.SetTitle( "Settings" );
//   myDialog.SetRect( 100, 100, 500, 400 );
//   myDialog.Show();
// ============================================================

// Touch-friendly minimum sizes (dp converted to virtual 1024x768 coords)
#define WINDOW_TITLEBAR_HEIGHT  28  // scaled; ~44dp on 480dpi Android
#define WINDOW_CLOSE_BTN_SIZE   24
#define WINDOW_BORDER_WIDTH     2
#define WINDOW_ICON_SIZE        16
#define WINDOW_ICON_PAD         4
#define WINDOW_TITLE_PAD_LEFT   6

// Chrome colors (ABGR packed — engine convention)
// Green semi-transparent background
#define WINDOW_BG_COLOR         0xCC1E3C1E  // RGBA(30, 60, 30, 0.8)
// Dark gray thin border
#define WINDOW_BORDER_COLOR     0xFF3C3C3C  // RGBA(60, 60, 60, 1.0)
// Title bar — slightly lighter
#define WINDOW_TITLEBAR_COLOR   0xDD2A4A2A  // RGBA(42, 74, 42, 0.87)
// Close button hover
#define WINDOW_CLOSE_HOVER_COLOR 0xFF4444AA // reddish on hover
// Title text
#define WINDOW_TITLE_TEXT_COLOR 0xFFD0D0D0
// Close [X] text
#define WINDOW_CLOSE_TEXT_COLOR 0xFFCCCCCC

class CMenuWindow : public CMenuBaseWindow
{
public:
	typedef CMenuBaseWindow BaseClass;

	CMenuWindow( const char *title = "Window", CWindowStack *pStack = &uiStatic.menu );

	// --- Overrides ---
	void Draw() override;
	bool KeyDown( int key ) override;
	bool KeyUp( int key ) override;
	bool MouseMove( int x, int y ) override;

	bool IsRoot() const override { return false; }

	// --- Configuration ---
	void SetTitle( const char *title ) { m_szTitle = title; }
	const char *GetTitle() const { return m_szTitle; }

	void SetIcon( const char *iconPath ) { m_szIconPath = iconPath; }

	// Content area offset (below title bar)
	Point GetPositionOffset() const override;

protected:
	void DrawChrome();
	void DrawTitleBar();
	void DrawCloseButton();
	void DrawBorder();

	bool IsCursorInTitleBar() const;
	bool IsCursorOnCloseBtn() const;

	// Title-bar drag (overrides BaseWindow's generic drag)
	void TitleBarDragDrop( bool down );

private:
	const char *m_szTitle;
	const char *m_szIconPath;

	// Drag state
	bool m_bTitleDrag;
	Point m_dragOffset;

	// Close button hover state
	bool m_bCloseHover;

	// Scaled metrics (computed in Draw)
	int m_iTitleBarH;
	int m_iCloseBtnSize;
	int m_iBorderW;
};

#endif // MENUWINDOW_H
