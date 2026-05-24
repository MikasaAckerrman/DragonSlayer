/*
Window.cpp -- draggable windowed dialog (CS 1.6 PC style)
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#include "extdll_menu.h"
#include "BaseMenu.h"
#include "Utils.h"
#include "Window.h"

CMenuWindow::CMenuWindow( const char *title, CWindowStack *pStack )
	: BaseClass( title, pStack )
{
	m_szTitle = title;
	m_szIconPath = NULL;
	m_bTitleDrag = false;
	m_bCloseHover = false;
	m_dragOffset = Point( 0, 0 );

	// Enable drag at BaseWindow level as well (safety net)
	bAllowDrag = false; // we handle drag ourselves via title bar only

	m_iTitleBarH = WINDOW_TITLEBAR_HEIGHT;
	m_iCloseBtnSize = WINDOW_CLOSE_BTN_SIZE;
	m_iBorderW = WINDOW_BORDER_WIDTH;
}

// ---------------------------------------------------------------
// Position offset — child items start below the title bar
// ---------------------------------------------------------------
Point CMenuWindow::GetPositionOffset() const
{
	return Point( m_iBorderW, m_iTitleBarH + m_iBorderW );
}

// ---------------------------------------------------------------
// Draw
// ---------------------------------------------------------------
void CMenuWindow::Draw()
{
	// Scale metrics to current resolution
	m_iTitleBarH = WINDOW_TITLEBAR_HEIGHT * uiStatic.scaleY;
	m_iCloseBtnSize = WINDOW_CLOSE_BTN_SIZE * uiStatic.scaleY;
	m_iBorderW = WINDOW_BORDER_WIDTH; // thin, no scale needed

	// Handle title-bar dragging
	if( m_bTitleDrag )
	{
		int dx = uiStatic.cursorX - m_dragOffset.x;
		int dy = uiStatic.cursorY - m_dragOffset.y;
		m_scPos.x += dx;
		m_scPos.y += dy;
		m_dragOffset.x = uiStatic.cursorX;
		m_dragOffset.y = uiStatic.cursorY;
		CalcItemsPositions();
	}

	// Update close button hover
	m_bCloseHover = IsCursorOnCloseBtn();

	// --- Draw chrome ---
	DrawChrome();

	// --- Draw child items (content area) ---
	CMenuItemsHolder::Draw();
}

// ---------------------------------------------------------------
// Chrome rendering
// ---------------------------------------------------------------
void CMenuWindow::DrawChrome()
{
	DrawBorder();
	DrawTitleBar();
	DrawCloseButton();
}

void CMenuWindow::DrawBorder()
{
	// Outer border
	UI_DrawRectangleExt( m_scPos.x, m_scPos.y, m_scSize.w, m_scSize.h,
		WINDOW_BORDER_COLOR, m_iBorderW );

	// Background fill (inside border)
	UI_FillRect( m_scPos.x + m_iBorderW, m_scPos.y + m_iBorderW,
		m_scSize.w - m_iBorderW * 2, m_scSize.h - m_iBorderW * 2,
		WINDOW_BG_COLOR );
}

void CMenuWindow::DrawTitleBar()
{
	int x = m_scPos.x + m_iBorderW;
	int y = m_scPos.y + m_iBorderW;
	int w = m_scSize.w - m_iBorderW * 2;
	int h = m_iTitleBarH;

	// Title bar background
	UI_FillRect( x, y, w, h, WINDOW_TITLEBAR_COLOR );

	// Icon (if set)
	int textX = x + WINDOW_TITLE_PAD_LEFT;
	if( m_szIconPath )
	{
		int iconSz = WINDOW_ICON_SIZE * uiStatic.scaleY;
		int iconY = y + ( h - iconSz ) / 2;
		UI_DrawPic( textX, iconY, iconSz, iconSz, 0xFFFFFFFF, m_szIconPath, QM_DRAWTRANS );
		textX += iconSz + WINDOW_ICON_PAD;
	}

	// Title text
	int textW = w - ( textX - x ) - m_iCloseBtnSize - WINDOW_ICON_PAD;
	UI_DrawString( uiStatic.hDefaultFont, textX, y, textW, h,
		m_szTitle, WINDOW_TITLE_TEXT_COLOR, h * 0.7f, QM_LEFT, ETF_SHADOW );
}

void CMenuWindow::DrawCloseButton()
{
	int btnSize = m_iCloseBtnSize;
	int x = m_scPos.x + m_scSize.w - m_iBorderW - btnSize - 2;
	int y = m_scPos.y + m_iBorderW + ( m_iTitleBarH - btnSize ) / 2;

	unsigned int bgColor = m_bCloseHover ? WINDOW_CLOSE_HOVER_COLOR : WINDOW_TITLEBAR_COLOR;
	UI_FillRect( x, y, btnSize, btnSize, bgColor );

	// Draw "X" text centered
	UI_DrawString( uiStatic.hDefaultFont, x, y, btnSize, btnSize,
		"X", WINDOW_CLOSE_TEXT_COLOR, btnSize * 0.7f, QM_CENTER, ETF_SHADOW );
}

// ---------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------
bool CMenuWindow::IsCursorInTitleBar() const
{
	int x = m_scPos.x + m_iBorderW;
	int y = m_scPos.y + m_iBorderW;
	int w = m_scSize.w - m_iBorderW * 2 - m_iCloseBtnSize;
	int h = m_iTitleBarH;

	return UI_CursorInRect( x, y, w, h );
}

bool CMenuWindow::IsCursorOnCloseBtn() const
{
	int btnSize = m_iCloseBtnSize;
	int x = m_scPos.x + m_scSize.w - m_iBorderW - btnSize - 2;
	int y = m_scPos.y + m_iBorderW + ( m_iTitleBarH - btnSize ) / 2;

	return UI_CursorInRect( x, y, btnSize, btnSize );
}

// ---------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------
bool CMenuWindow::KeyDown( int key )
{
	if( UI::Key::IsLeftMouse( key ) )
	{
		// Close button click
		if( IsCursorOnCloseBtn() )
		{
			Hide();
			return true;
		}

		// Title bar drag start
		if( IsCursorInTitleBar() )
		{
			TitleBarDragDrop( true );
			return true;
		}
	}

	if( UI::Key::IsEscape( key ) )
	{
		Hide();
		return true;
	}

	return BaseClass::KeyDown( key );
}

bool CMenuWindow::KeyUp( int key )
{
	if( UI::Key::IsLeftMouse( key ) )
	{
		if( m_bTitleDrag )
		{
			TitleBarDragDrop( false );
			return true;
		}
	}

	return BaseClass::KeyUp( key );
}

bool CMenuWindow::MouseMove( int x, int y )
{
	// Update drag offset while dragging
	// (actual move happens in Draw to avoid jitter)
	return BaseClass::MouseMove( x, y );
}

// ---------------------------------------------------------------
// Drag helpers
// ---------------------------------------------------------------
void CMenuWindow::TitleBarDragDrop( bool down )
{
	m_bTitleDrag = down;
	if( down )
	{
		m_dragOffset.x = uiStatic.cursorX;
		m_dragOffset.y = uiStatic.cursorY;
	}
}
