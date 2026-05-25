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
#include "WindowStyle.h"
#include "ItemsHolder.h"

CMenuTabControl::CMenuTabControl()
{
	m_iTabCount = 0;
	m_iActiveTab = 0;
	m_iHoverTab = -1;
	m_iScaledTabH = WndStyle::TabHeight;
	memset( m_tabs, 0, sizeof( m_tabs ) );
}

void CMenuTabControl::VidInit()
{
	m_iScaledTabH = WndStyle::ScaleY( WndStyle::TabHeight );
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

	if( page )
		page->SetVisibility( m_iTabCount == m_iActiveTab );

	m_iTabCount++;
}

void CMenuTabControl::SetActiveTab( int index )
{
	if( index < 0 || index >= m_iTabCount )
		return;

	if( m_tabs[m_iActiveTab].page )
		m_tabs[m_iActiveTab].page->Hide();

	m_iActiveTab = index;

	if( m_tabs[m_iActiveTab].page )
		m_tabs[m_iActiveTab].page->Show();
}

// ---------------------------------------------------------------
// Content area
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

	m_iScaledTabH = WndStyle::ScaleY( WndStyle::TabHeight );

	int x = m_scPos.x;
	int y = m_scPos.y;
	int availW = m_scSize.w;

	int tabW = availW / m_iTabCount;
	int minW = WndStyle::ScaleX( WndStyle::TabMinWidth );
	if( tabW < minW )
		tabW = minW;

	for( int i = 0; i < m_iTabCount; i++ )
	{
		int tx = x + i * tabW;
		int tw = tabW - WndStyle::TabPadBetween;

		m_tabs[i].renderX = tx;
		m_tabs[i].renderW = tw;

		WndStyle::DrawTab( tx, y, tw, m_iScaledTabH,
			i == m_iActiveTab, i == m_iHoverTab, m_tabs[i].label );
	}

	// Bottom line with gap under active tab
	if( m_iActiveTab >= 0 && m_iActiveTab < m_iTabCount )
	{
		int activeX = m_tabs[m_iActiveTab].renderX;
		int activeW = m_tabs[m_iActiveTab].renderW;

		// Left segment (before active tab)
		if( activeX > x )
			UI_FillRect( x, y + m_iScaledTabH, activeX - x, 1, WndStyle::TabBorderColor );

		// Right segment (after active tab)
		int rightStart = activeX + activeW;
		int rightEnd = x + availW;
		if( rightStart < rightEnd )
			UI_FillRect( rightStart, y + m_iScaledTabH, rightEnd - rightStart, 1, WndStyle::TabBorderColor );
	}
	else
	{
		// Fallback: full line if no active tab
		UI_FillRect( x, y + m_iScaledTabH, availW, 1, WndStyle::TabBorderColor );
	}
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
