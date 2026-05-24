/*
TabControl.h -- horizontal tab control widget (CS 1.6 style)
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#ifndef MENUTABCONTROL_H
#define MENUTABCONTROL_H

#include "BaseItem.h"

#define TAB_MAX_TABS        16
#define TAB_HEIGHT          26   // virtual coords, scaled at render
#define TAB_MIN_WIDTH       64
#define TAB_PAD_H           10   // horizontal padding inside tab
#define TAB_PAD_BETWEEN     2    // gap between tabs

// Colors (ABGR packed)
#define TAB_ACTIVE_COLOR    0xDD2A4A2A  // matches title bar
#define TAB_INACTIVE_COLOR  0xAA1A2A1A
#define TAB_HOVER_COLOR     0xCC254525
#define TAB_TEXT_COLOR      0xFFD0D0D0
#define TAB_TEXT_ACTIVE_COLOR 0xFFFFFFFF
#define TAB_BORDER_COLOR    0xFF3C3C3C

class CMenuItemsHolder;

// ============================================================
// CMenuTabControl — renders a row of tabs, shows/hides pages.
//
// Usage:
//   tabCtrl.SetRect( 0, 0, parentW, parentH );
//   tabCtrl.AddTab( "Video", &videoPage );
//   tabCtrl.AddTab( "Audio", &audioPage );
//   parent.AddItem( tabCtrl );
// ============================================================
class CMenuTabControl : public CMenuBaseItem
{
public:
	typedef CMenuBaseItem BaseClass;

	CMenuTabControl();

	void VidInit() override;
	void Draw() override;
	bool KeyDown( int key ) override;
	bool KeyUp( int key ) override;
	bool MouseMove( int x, int y ) override;

	// Add a tab. The page holder will be shown/hidden automatically.
	void AddTab( const char *label, CMenuItemsHolder *page );

	// Switch to tab by index
	void SetActiveTab( int index );
	int  GetActiveTab() const { return m_iActiveTab; }
	int  GetTabCount() const { return m_iTabCount; }

	// Get content area rect (below tabs row)
	Point GetContentOffset() const;
	Size  GetContentSize() const;

private:
	struct Tab
	{
		const char *label;
		CMenuItemsHolder *page;
		int renderX;  // computed at draw time
		int renderW;
	};

	Tab m_tabs[TAB_MAX_TABS];
	int m_iTabCount;
	int m_iActiveTab;
	int m_iHoverTab;

	int m_iScaledTabH; // scaled tab row height

	int TabHitTest() const; // returns tab index under cursor, or -1
};

#endif // MENUTABCONTROL_H
