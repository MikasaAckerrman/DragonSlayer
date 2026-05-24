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
#include "WindowStyle.h"

CMenuWindow::CMenuWindow( const char *title, CWindowStack *pStack )
	: BaseClass( title, pStack )
{
	m_szTitle = title;
	m_szIconPath = NULL;
	m_bTitleDrag = false;
	m_bCloseHover = false;
	m_dragOffset = Point( 0, 0 );

	bAllowDrag = false; // we handle drag ourselves via title bar only

	m_iTitleBarH = WndStyle::TitleBarHeight;
	m_iCloseBtnSize = WndStyle::CloseBtnSize;
	m_iBorderW = WndStyle::BorderWidth;
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
	// Scale metrics
	m_iTitleBarH = WndStyle::ScaleY( WndStyle::TitleBarHeight );
	m_iCloseBtnSize = WndStyle::ScaleY( WndStyle::CloseBtnSize );
	m_iBorderW = WndStyle::BorderWidth;

	// Title-bar drag
	if( m_bTitleDrag )
	{
		m_scPos.x += uiStatic.cursorX - m_dragOffset.x;
		m_scPos.y += uiStatic.cursorY - m_dragOffset.y;
		m_dragOffset.x = uiStatic.cursorX;
		m_dragOffset.y = uiStatic.cursorY;
		CalcItemsPositions();
	}

	m_bCloseHover = IsCursorOnCloseBtn();

	// Chrome
	DrawChrome();

	// Child items
	CMenuItemsHolder::Draw();
}

// ---------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------
void CMenuWindow::DrawChrome()
{
	// Background + border
	WndStyle::DrawWindowChrome( m_scPos.x, m_scPos.y, m_scSize.w, m_scSize.h );

	// Title bar
	int tbX = m_scPos.x + m_iBorderW;
	int tbY = m_scPos.y + m_iBorderW;
	int tbW = m_scSize.w - m_iBorderW * 2;
	WndStyle::DrawTitleBar( tbX, tbY, tbW, m_iTitleBarH );

	// Icon + title text
	int textX = tbX + WndStyle::TitlePadLeft;
	if( m_szIconPath )
	{
		int iconSz = WndStyle::ScaleY( WndStyle::IconSize );
		int iconY = tbY + ( m_iTitleBarH - iconSz ) / 2;
		UI_DrawPic( textX, iconY, iconSz, iconSz, 0xFFFFFFFF, m_szIconPath, QM_DRAWTRANS );
		textX += iconSz + WndStyle::IconPad;
	}

	int textW = tbW - ( textX - tbX ) - m_iCloseBtnSize - WndStyle::IconPad;
	int charH = (int)( m_iTitleBarH * 0.7f );
	UI_DrawString( uiStatic.hDefaultFont, textX, tbY, textW, m_iTitleBarH,
		m_szTitle, WndStyle::TitleTextColor, charH, QM_LEFT, ETF_SHADOW );

	// Close button
	int cbX = m_scPos.x + m_scSize.w - m_iBorderW - m_iCloseBtnSize - 2;
	int cbY = m_scPos.y + m_iBorderW + ( m_iTitleBarH - m_iCloseBtnSize ) / 2;
	WndStyle::DrawCloseBtn( cbX, cbY, m_iCloseBtnSize, m_bCloseHover );
}

// ---------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------
bool CMenuWindow::IsCursorInTitleBar() const
{
	int x = m_scPos.x + m_iBorderW;
	int y = m_scPos.y + m_iBorderW;
	int w = m_scSize.w - m_iBorderW * 2 - m_iCloseBtnSize;
	return UI_CursorInRect( x, y, w, m_iTitleBarH );
}

bool CMenuWindow::IsCursorOnCloseBtn() const
{
	int btnSize = m_iCloseBtnSize;
	int x = m_scPos.x + m_scSize.w - m_iBorderW - btnSize - 2;
	int y = m_scPos.y + m_iBorderW + ( m_iTitleBarH - btnSize ) / 2;
	return UI_CursorInRect( x, y, btnSize, btnSize );
}

// ---------------------------------------------------------------
// Input
// ---------------------------------------------------------------
bool CMenuWindow::KeyDown( int key )
{
	if( UI::Key::IsLeftMouse( key ) )
	{
		if( IsCursorOnCloseBtn() )
		{
			Hide();
			return true;
		}
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
	if( UI::Key::IsLeftMouse( key ) && m_bTitleDrag )
	{
		TitleBarDragDrop( false );
		return true;
	}
	return BaseClass::KeyUp( key );
}

bool CMenuWindow::MouseMove( int x, int y )
{
	return BaseClass::MouseMove( x, y );
}

// ---------------------------------------------------------------
// Drag
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
