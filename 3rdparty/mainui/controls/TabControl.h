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

#define TAB_MAX_TABS 16

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

	void AddTab( const char *label, CMenuItemsHolder *page );
	void SetActiveTab( int index );
	int  GetActiveTab() const { return m_iActiveTab; }
	int  GetTabCount() const { return m_iTabCount; }

	Point GetContentOffset() const;
	Size  GetContentSize() const;

private:
	struct Tab
	{
		const char *label;
		CMenuItemsHolder *page;
		int renderX;
		int renderW;
	};

	Tab m_tabs[TAB_MAX_TABS];
	int m_iTabCount;
	int m_iActiveTab;
	int m_iHoverTab;
	int m_iScaledTabH;

	int TabHitTest() const;
};

#endif // MENUTABCONTROL_H
