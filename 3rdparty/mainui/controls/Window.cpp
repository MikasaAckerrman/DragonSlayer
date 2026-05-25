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
#include "SchemeManager.h"

CMenuWindow::CMenuWindow( const char *title, CWindowStack *pStack )
	: BaseClass( title, pStack )
{
	m_szTitle = title;
	m_szIconPath = NULL;
	m_bShowCloseBtn = true;
	m_bShowMaxBtn = true;
	m_bTitleDrag = false;
	m_bDragStarted = false;
	m_bCloseHover = false;
	m_bMaxHover = false;
	m_bMaximized = false;
	m_dragOffset = Point( 0, 0 );
	m_dragStartPos = Point( 0, 0 );
	m_savedPos = Point( 0, 0 );
	m_savedSize = Size( 0, 0 );

	m_iResizeEdge = RESIZE_NONE;
	m_bResizing = false;
	m_resizeStartCursor = Point( 0, 0 );
	m_resizeStartPos = Point( 0, 0 );
	m_resizeStartSize = Size( 0, 0 );

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
	return Point( m_scPos.x + m_iBorderW, m_scPos.y + m_iTitleBarH + m_iBorderW );
}

// ---------------------------------------------------------------
// Helper: clamp value (defensive — engine has no std::clamp)
// ---------------------------------------------------------------
static inline int Wnd_Clamp( int v, int lo, int hi )
{
	return v < lo ? lo : ( v > hi ? hi : v );
}

// ---------------------------------------------------------------
// Draw
// ---------------------------------------------------------------
void CMenuWindow::Draw()
{
	// Scale metrics (touch-friendly minimums)
	m_iTitleBarH = WndStyle::ScaleYTouch( WndStyle::TitleBarHeight );
	m_iCloseBtnSize = WndStyle::ScaleYTouch( WndStyle::CloseBtnSize );
	m_iBorderW = WndStyle::BorderWidth;

	// Resize in progress
	if( m_bResizing )
	{
		UpdateResize();
	}

	// Title-bar drag with touch threshold
	if( m_bTitleDrag )
	{
		if( !m_bDragStarted )
		{
			// Check if movement exceeds threshold
			int dx = uiStatic.cursorX - m_dragStartPos.x;
			int dy = uiStatic.cursorY - m_dragStartPos.y;
			if( dx * dx + dy * dy >= DRAG_THRESHOLD_SQ )
			{
				m_bDragStarted = true;
				m_dragOffset.x = uiStatic.cursorX;
				m_dragOffset.y = uiStatic.cursorY;
			}
		}

		if( m_bDragStarted )
		{
			m_scPos.x += uiStatic.cursorX - m_dragOffset.x;
			m_scPos.y += uiStatic.cursorY - m_dragOffset.y;
			m_dragOffset.x = uiStatic.cursorX;
			m_dragOffset.y = uiStatic.cursorY;

			// Clamp position so title bar stays reachable on screen
			int screenW = (int)( 1024 * uiStatic.scaleX );
			int screenH = (int)( 768 * uiStatic.scaleY );
			int minX = -m_scSize.w + 100;
			int maxX = screenW - 100;
			int minY = 0;
			int maxY = screenH - m_iTitleBarH - m_iBorderW;
			m_scPos.x = Wnd_Clamp( m_scPos.x, minX, maxX );
			m_scPos.y = Wnd_Clamp( m_scPos.y, minY, maxY );

			CalcItemsPositions();
		}
	}

	m_bCloseHover = IsCursorOnCloseBtn();
	m_bMaxHover = IsCursorOnMaxBtn();

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
	CSchemeManager *scheme = CSchemeManager::GetInstance();

	// Background + border
	WndStyle::DrawWindowChrome( m_scPos.x, m_scPos.y, m_scSize.w, m_scSize.h );

	// Title bar
	int tbX = m_scPos.x + m_iBorderW;
	int tbY = m_scPos.y + m_iBorderW;
	int tbW = m_scSize.w - m_iBorderW * 2;
	unsigned int titleBarBg = scheme->GetColor("TitleBarBG");
	if( !titleBarBg ) titleBarBg = WndStyle::TitleBarColor;
	UI_FillRect( tbX, tbY, tbW, m_iTitleBarH, titleBarBg );

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
	if( textW < 16 ) textW = 16;
	int charH = (int)( m_iTitleBarH * 0.7f );
	unsigned int titleTextCol = scheme->GetColor("TitleBarText");
	if( !titleTextCol ) titleTextCol = WndStyle::TitleTextColor;
	UI_DrawString( uiStatic.hDefaultFont, textX, tbY, textW, m_iTitleBarH,
		m_szTitle, titleTextCol, charH, QM_LEFT, ETF_SHADOW );

	// Close button
	int cbX = m_scPos.x + m_scSize.w - m_iBorderW - m_iCloseBtnSize - 2;
	int cbY = m_scPos.y + m_iBorderW + ( m_iTitleBarH - m_iCloseBtnSize ) / 2;
	if( m_bShowCloseBtn )
		WndStyle::DrawCloseBtn( cbX, cbY, m_iCloseBtnSize, m_bCloseHover );

	// Maximize/Restore button (left of close)
	if( m_bShowMaxBtn )
	{
		int mbX = cbX - ( m_bShowCloseBtn ? m_iCloseBtnSize + 2 : 0 );
		int mbY = cbY;
		unsigned int mbBg = m_bMaxHover ? (scheme->GetColor("TabHoverBG") ? scheme->GetColor("TabHoverBG") : WndStyle::TabHoverColor) : titleBarBg;
		UI_FillRect( mbX, mbY, m_iCloseBtnSize, m_iCloseBtnSize, mbBg );
		const char *mbLabel = m_bMaximized ? "-" : "+";
		int mbCharH = (int)( m_iCloseBtnSize * 0.7f );
		UI_DrawString( uiStatic.hDefaultFont, mbX, mbY, m_iCloseBtnSize, m_iCloseBtnSize,
			mbLabel, WndStyle::CloseTextColor, mbCharH, QM_CENTER, ETF_SHADOW );
	}
}

// ---------------------------------------------------------------
// Hit testing — accounts for hidden chrome buttons
// ---------------------------------------------------------------
bool CMenuWindow::IsCursorInTitleBar() const
{
	int btnsCount = ( m_bShowCloseBtn ? 1 : 0 ) + ( m_bShowMaxBtn ? 1 : 0 );
	int btnsW = btnsCount * ( m_iCloseBtnSize + 2 );

	int x = m_scPos.x + m_iBorderW;
	int y = m_scPos.y + m_iBorderW;
	int w = m_scSize.w - m_iBorderW * 2 - btnsW - 2;
	if( w < 0 ) w = 0;
	return UI_CursorInRect( x, y, w, m_iTitleBarH );
}

bool CMenuWindow::IsCursorOnCloseBtn() const
{
	if( !m_bShowCloseBtn ) return false;
	int btnSize = m_iCloseBtnSize;
	int x = m_scPos.x + m_scSize.w - m_iBorderW - btnSize - 2;
	int y = m_scPos.y + m_iBorderW + ( m_iTitleBarH - btnSize ) / 2;
	return UI_CursorInRect( x, y, btnSize, btnSize );
}

bool CMenuWindow::IsCursorOnMaxBtn() const
{
	if( !m_bShowMaxBtn ) return false;
	int btnSize = m_iCloseBtnSize;
	int closeX = m_scPos.x + m_scSize.w - m_iBorderW - btnSize - 2;
	int x = closeX - ( m_bShowCloseBtn ? btnSize + 2 : 0 );
	int y = m_scPos.y + m_iBorderW + ( m_iTitleBarH - btnSize ) / 2;
	return UI_CursorInRect( x, y, btnSize, btnSize );
}

// ---------------------------------------------------------------
// Maximize / Restore
// ---------------------------------------------------------------
void CMenuWindow::ToggleMaximize()
{
	if( !m_bMaximized )
	{
		// Save current pos/size
		m_savedPos = m_scPos;
		m_savedSize = m_scSize;
		// Maximize to full screen (virtual coords)
		m_scPos = Point( 0, 0 );
		m_scSize = Size( (int)(1024 * uiStatic.scaleX), (int)(768 * uiStatic.scaleY) );
		m_bMaximized = true;
	}
	else
	{
		// Restore — guard against zero-size (if Toggle called before save)
		if( m_savedSize.w > 0 && m_savedSize.h > 0 )
		{
			m_scPos = m_savedPos;
			m_scSize = m_savedSize;
		}
		m_bMaximized = false;
	}
	CalcItemsPositions();
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
			OnCloseClicked();
			return true;
		}
		if( IsCursorOnMaxBtn() )
		{
			ToggleMaximize();
			return true;
		}

		// Check resize edge before title bar drag
		int resizeEdge = DetectResizeEdge();
		if( resizeEdge != RESIZE_NONE )
		{
			StartResize( resizeEdge );
			return true;
		}

		if( IsCursorInTitleBar() && !m_bMaximized )
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
	if( UI::Key::IsLeftMouse( key ) && m_bResizing )
	{
		StopResize();
		return true;
	}
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
		m_bDragStarted = false;
		m_dragStartPos.x = uiStatic.cursorX;
		m_dragStartPos.y = uiStatic.cursorY;
		m_dragOffset.x = uiStatic.cursorX;
		m_dragOffset.y = uiStatic.cursorY;
	}
	else
	{
		m_bDragStarted = false;
	}
}

// ---------------------------------------------------------------
// Resize — edge/corner detection and drag resize
// ---------------------------------------------------------------
int CMenuWindow::DetectResizeEdge() const
{
	if( m_bMaximized ) return RESIZE_NONE;

	int border = WndStyle::ScaleX( RESIZE_BORDER );
	int cx = uiStatic.cursorX;
	int cy = uiStatic.cursorY;

	int wx = m_scPos.x;
	int wy = m_scPos.y;
	int ww = m_scSize.w;
	int wh = m_scSize.h;

	// Restrict detection to inside the window border only (no outside bleed)
	bool inHRange = ( cx >= wx && cx < wx + ww );
	bool inVRange = ( cy >= wy && cy < wy + wh );

	if( !inHRange || !inVRange ) return RESIZE_NONE;

	bool inLeft   = ( cx >= wx && cx < wx + border );
	bool inRight  = ( cx >= wx + ww - border && cx < wx + ww );
	bool inTop    = ( cy >= wy && cy < wy + border );
	bool inBottom = ( cy >= wy + wh - border && cy < wy + wh );

	int edge = RESIZE_NONE;
	if( inLeft )   edge |= RESIZE_LEFT;
	if( inRight )  edge |= RESIZE_RIGHT;
	if( inTop )    edge |= RESIZE_TOP;
	if( inBottom ) edge |= RESIZE_BOTTOM;

	return edge;
}

void CMenuWindow::StartResize( int edge )
{
	m_iResizeEdge = edge;
	m_bResizing = true;
	m_resizeStartCursor = Point( uiStatic.cursorX, uiStatic.cursorY );
	m_resizeStartPos = m_scPos;
	m_resizeStartSize = m_scSize;
}

void CMenuWindow::UpdateResize()
{
	int dx = uiStatic.cursorX - m_resizeStartCursor.x;
	int dy = uiStatic.cursorY - m_resizeStartCursor.y;

	int newX = m_resizeStartPos.x;
	int newY = m_resizeStartPos.y;
	int newW = m_resizeStartSize.w;
	int newH = m_resizeStartSize.h;

	if( m_iResizeEdge & RESIZE_RIGHT )
	{
		newW = m_resizeStartSize.w + dx;
	}
	if( m_iResizeEdge & RESIZE_LEFT )
	{
		newX = m_resizeStartPos.x + dx;
		newW = m_resizeStartSize.w - dx;
	}
	if( m_iResizeEdge & RESIZE_BOTTOM )
	{
		newH = m_resizeStartSize.h + dy;
	}
	if( m_iResizeEdge & RESIZE_TOP )
	{
		newY = m_resizeStartPos.y + dy;
		newH = m_resizeStartSize.h - dy;
	}

	// Enforce minimum size (scaled to screen coordinates)
	int minW = WndStyle::ScaleX( MIN_WINDOW_W );
	int minH = WndStyle::ScaleY( MIN_WINDOW_H );

	if( newW < minW )
	{
		if( m_iResizeEdge & RESIZE_LEFT )
			newX = m_resizeStartPos.x + m_resizeStartSize.w - minW;
		newW = minW;
	}
	if( newH < minH )
	{
		if( m_iResizeEdge & RESIZE_TOP )
			newY = m_resizeStartPos.y + m_resizeStartSize.h - minH;
		newH = minH;
	}

	// Clamp position so window stays reachable (same as title-bar drag)
	int screenW = (int)( 1024 * uiStatic.scaleX );
	int screenH = (int)( 768 * uiStatic.scaleY );
	int minX = -newW + 100;
	int maxX = screenW - 100;
	int minY = 0;
	int maxY = screenH - m_iTitleBarH - m_iBorderW;
	newX = Wnd_Clamp( newX, minX, maxX );
	newY = Wnd_Clamp( newY, minY, maxY );

	m_scPos.x = newX;
	m_scPos.y = newY;
	m_scSize.w = newW;
	m_scSize.h = newH;

	CalcItemsPositions();
}

void CMenuWindow::StopResize()
{
	m_bResizing = false;
	m_iResizeEdge = RESIZE_NONE;
}

VGUI_DefaultCursor CMenuWindow::CursorAction()
{
	int edge = DetectResizeEdge();
	switch( edge )
	{
	case RESIZE_TOPLEFT:
	case RESIZE_BOTTOMRIGHT:
		return dc_sizenwse;
	case RESIZE_TOPRIGHT:
	case RESIZE_BOTTOMLEFT:
		return dc_sizenesw;
	case RESIZE_LEFT:
	case RESIZE_RIGHT:
		return dc_sizewe;
	case RESIZE_TOP:
	case RESIZE_BOTTOM:
		return dc_sizens;
	default:
		return dc_arrow;
	}
}
