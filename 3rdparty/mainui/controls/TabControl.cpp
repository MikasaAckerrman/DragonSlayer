/*
TabControl.cpp -- horizontal tab control widget (CS 1.6 style)
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#include "extdll_menu.h"
#include "BaseMenu.h"
#include "Utils.h"
#include "TabControl.h"
#include "ItemsHolder.h"

CMenuTabControl::CMenuTabControl()
{
	m_iTabCount = 0;
	m_iActiveTab = 0;
	m_iHoverTab = -1;
	m_iScaledTabH = TAB_HEIGHT;
	memset( m_tabs, 0, sizeof( m_tabs ) );
}

void CMenuTabControl::VidInit()
{
	m_iScaledTabH = TAB_HEIGHT * uiStatic.scaleY;
	BaseClass::VidInit();
}

// ---------------------------------------------------------------
// Tab management
// ---------------------------------------------------------------
void CMenuTabControl::AddTab( const char *label, CMenuItemsHolder *page )
{
	if( m_iTabCount >= TAB_MAX_TABS )
		return;

	Tab &t = m_tabs[m_iTabCount];
	t.label = label;
	t.page = page;
	t.renderX = 0;
	t.renderW = 0;

	// Only active tab page is visible
	if( page )
		page->SetVisibility( m_iTabCount == m_iActiveTab );

	m_iTabCount++;
}

void CMenuTabControl::SetActiveTab( int index )
{
	if( index < 0 || index >= m_iTabCount )
		return;

	// Hide previous page
	if( m_tabs[m_iActiveTab].page )
		m_tabs[m_iActiveTab].page->Hide();

	m_iActiveTab = index;

	// Show new page
	if( m_tabs[m_iActiveTab].page )
		m_tabs[m_iActiveTab].page->Show();
}

// ---------------------------------------------------------------
// Content area helpers
// ---------------------------------------------------------------
Point CMenuTabControl::GetContentOffset() const
{
	return Point( 0, m_iScaledTabH + 2 );
}

Size CMenuTabControl::GetContentSize() const
{
	return Size( m_scSize.w, m_scSize.h - m_iScaledTabH - 2 );
}

// ---------------------------------------------------------------
// Draw
// ---------------------------------------------------------------
void CMenuTabControl::Draw()
{
	if( m_iTabCount == 0 )
		return;

	m_iScaledTabH = TAB_HEIGHT * uiStatic.scaleY;

	int x = m_scPos.x;
	int y = m_scPos.y;
	int availW = m_scSize.w;

	// Calculate tab widths — distribute evenly but respect min width
	int tabW = availW / m_iTabCount;
	if( tabW < TAB_MIN_WIDTH * uiStatic.scaleX )
		tabW = TAB_MIN_WIDTH * uiStatic.scaleX;

	// Draw each tab
	for( int i = 0; i < m_iTabCount; i++ )
	{
		int tx = x + i * tabW;
		int tw = tabW - TAB_PAD_BETWEEN;

		m_tabs[i].renderX = tx;
		m_tabs[i].renderW = tw;

		// Pick color
		unsigned int bgCol;
		unsigned int textCol;

		if( i == m_iActiveTab )
		{
			bgCol = TAB_ACTIVE_COLOR;
			textCol = TAB_TEXT_ACTIVE_COLOR;
		}
		else if( i == m_iHoverTab )
		{
			bgCol = TAB_HOVER_COLOR;
			textCol = TAB_TEXT_COLOR;
		}
		else
		{
			bgCol = TAB_INACTIVE_COLOR;
			textCol = TAB_TEXT_COLOR;
		}

		// Tab background
		UI_FillRect( tx, y, tw, m_iScaledTabH, bgCol );

		// Tab border (top + sides, no bottom for active)
		UI_DrawRectangleExt( tx, y, tw, m_iScaledTabH, TAB_BORDER_COLOR, 1,
			QM_TOP | QM_LEFT | QM_RIGHT | ( i != m_iActiveTab ? QM_BOTTOM : 0 ) );

		// Tab label
		int charH = m_iScaledTabH * 0.6f;
		UI_DrawString( uiStatic.hSmallFont, tx + TAB_PAD_H, y, tw - TAB_PAD_H * 2,
			m_iScaledTabH, m_tabs[i].label, textCol, charH, QM_CENTER, ETF_SHADOW );
	}

	// Underline below inactive area (connects visual bottom)
	UI_FillRect( x, y + m_iScaledTabH, availW, 1, TAB_BORDER_COLOR );
}

// ---------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------
int CMenuTabControl::TabHitTest() const
{
	for( int i = 0; i < m_iTabCount; i++ )
	{
		if( UI_CursorInRect( m_tabs[i].renderX, m_scPos.y,
			m_tabs[i].renderW, m_iScaledTabH ) )
			return i;
	}
	return -1;
}

// ---------------------------------------------------------------
// Input
// ---------------------------------------------------------------
bool CMenuTabControl::KeyDown( int key )
{
	if( UI::Key::IsLeftMouse( key ) )
	{
		int hit = TabHitTest();
		if( hit >= 0 && hit != m_iActiveTab )
		{
			SetActiveTab( hit );
			return true;
		}
	}
	return false;
}

bool CMenuTabControl::KeyUp( int key )
{
	return false;
}

bool CMenuTabControl::MouseMove( int x, int y )
{
	m_iHoverTab = TabHitTest();
	return true;
}
