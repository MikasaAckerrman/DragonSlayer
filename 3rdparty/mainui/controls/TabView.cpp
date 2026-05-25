/*
TabView.cpp -- tabbed view
Copyright (C) 2017 a1batross

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include "TabView.h"
#include "Scissor.h"
#include "WindowStyle.h"

CMenuTabView::CMenuTabView() : BaseClass()
{
	m_bWrapCursor = true;
	SetCharSize( QM_BOLDFONT );
	eTextAlignment = QM_CENTER;
}

Point CMenuTabView::GetPositionOffset() const
{
	Point ret = m_scPos;
	ret.y += m_scChSize * 1.5f;

	return ret;
}

void CMenuTabView::VidInit()
{
	CalcPosition();
	CalcSizes();

	_VidInit();
	VidInitItems();

	m_szTab.w = m_scSize.w / m_pItems.Count();
	m_szTab.h = m_scChSize * 1.5f;
}

void CMenuTabView::DrawTab(Point pt, const char *name, bool isEnd, bool isSelected, bool isHighlighted)
{
	unsigned int bgCol = isSelected ? WndStyle::TabActiveColor : ( isHighlighted ? WndStyle::TabHoverColor : WndStyle::TabInactiveColor );
	unsigned int textCol = isSelected ? WndStyle::TabTextActiveCol : WndStyle::TabTextColor;
	unsigned int textflags = ( iFlags & QMF_DROPSHADOW ) ? ETF_SHADOW : 0;

	// Fill tab background
	UI_FillRect( pt, m_szTab, bgCol );

	// 3D bevel on tabs (raised look)
	// Outer highlight top + left
	UI_FillRect( pt.x, pt.y, m_szTab.w, 1, WndStyle::BevelHighlight );
	UI_FillRect( pt.x, pt.y, 1, m_szTab.h, WndStyle::BevelHighlight );
	// Inner highlight
	UI_FillRect( pt.x + 1, pt.y + 1, m_szTab.w - 2, 1, WndStyle::BevelLight );
	UI_FillRect( pt.x + 1, pt.y + 1, 1, m_szTab.h - 2, WndStyle::BevelLight );
	// Outer shadow right
	UI_FillRect( pt.x + m_szTab.w - 1, pt.y, 1, m_szTab.h, WndStyle::BevelDarkShadow );
	// Inner shadow right
	UI_FillRect( pt.x + m_szTab.w - 2, pt.y + 1, 1, m_szTab.h - 2, WndStyle::BevelShadow );

	// Active tab: NO bottom border (merges with content)
	// Inactive tab: draw bottom border
	if( !isSelected )
	{
		UI_FillRect( pt.x, pt.y + m_szTab.h - 1, m_szTab.w, 1, WndStyle::BevelDarkShadow );
	}

	UI_DrawString( font, pt, m_szTab, name, textCol, m_scChSize, eTextAlignment, textflags );
}

void CMenuTabView::Draw()
{
	// Draw content area background
	Point contentOffset = Point( m_scPos.x, m_scPos.y + m_scChSize * 1.5f );
	Size contentSize = Size( m_scSize.w, m_scSize.h - m_scChSize * 1.5f );
	UI_FillRect( contentOffset, contentSize, WndStyle::BgColor );

	// Sunken bevel around content area
	WndStyle::DrawSunkenBevel( contentOffset.x, contentOffset.y, contentSize.w, contentSize.h );

	// Draw tabs
	Point tabOffset = m_scPos;
	FOR_EACH_VEC( m_pItems, i )
	{
		bool isEnd = i == ( m_pItems.Count() - 1 );
		bool isHighlighted = UI_CursorInRect( tabOffset, m_szTab );
		bool isSelected = i == m_iCursor;

		DrawTab( tabOffset, m_pItems[i]->szName, isEnd, isSelected, isHighlighted );
		tabOffset.x += m_szTab.w;
	}

	// Draw contents
	if( m_iCursor >= 0 && m_iCursor < m_pItems.Count() )
	{
		Point innerOffset = Point( contentOffset.x + 2, contentOffset.y + 2 );
		Size innerSize = Size( contentSize.w - 4, contentSize.h - 4 );
		UI::Scissor::PushScissor( innerOffset, innerSize );
			m_pItems[m_iCursor]->Draw();
		UI::Scissor::PopScissor();
	}
}
