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

	bool IsCursorInTitleBar() const;
	bool IsCursorOnCloseBtn() const;

	void TitleBarDragDrop( bool down );

private:
	const char *m_szTitle;
	const char *m_szIconPath;

	bool m_bTitleDrag;
	Point m_dragOffset;
	bool m_bCloseHover;

	// Scaled metrics (recomputed each frame)
	int m_iTitleBarH;
	int m_iCloseBtnSize;
	int m_iBorderW;
};

#endif // MENUWINDOW_H
