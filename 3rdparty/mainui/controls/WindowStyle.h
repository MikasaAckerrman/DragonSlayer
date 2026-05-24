/*
WindowStyle.h -- shared visual style constants and helpers for windowed UI
Copyright (C) 2024 DragonSlayer contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#ifndef WINDOWSTYLE_H
#define WINDOWSTYLE_H

#include "BaseMenu.h"

// ============================================================
// Color palette (ABGR packed — engine convention)
// ============================================================
namespace WndStyle
{
	// Background
	static const unsigned int BgColor          = 0xFF2D2B2E; // opaque dark brownish-gray
	static const unsigned int BgColorSolid     = 0xFF2D2B2E; // same as BgColor

	// Border
	static const unsigned int BorderColor      = 0xFF44584C; // olive-green border
	static const int          BorderWidth      = 1;

	// Title bar
	static const unsigned int TitleBarColor    = 0xFF232120; // darker opaque
	static const unsigned int TitleTextColor   = 0xFFD0D0D0;
	static const int          TitleBarHeight   = 28; // virtual coords

	// Close button
	static const unsigned int CloseBtnColor    = 0xFF2222CC; // red hover (ABGR)
	static const unsigned int CloseTextColor   = 0xFFCCCCCC;
	static const int          CloseBtnSize     = 24;

	// Tabs
	static const unsigned int TabActiveColor   = 0xFF2D2B2E; // same as BgColor - merges with content
	static const unsigned int TabInactiveColor = 0xFF1E1C1E; // darker opaque
	static const unsigned int TabHoverColor    = 0xFF383638; // slightly lighter hover
	static const unsigned int TabTextColor     = 0xFF909090; // gray text for inactive
	static const unsigned int TabTextActiveCol = 0xFFFFFFFF; // white for active
	static const unsigned int TabBorderColor   = 0xFF44584C; // olive border
	static const int          TabHeight        = 26;
	static const int          TabMinWidth      = 64;
	static const int          TabPadH          = 10;
	static const int          TabPadBetween    = 2;

	// Icon
	static const int          IconSize         = 16;
	static const int          IconPad          = 4;
	static const int          TitlePadLeft     = 6;

	// Widget style (CS 1.6 flat look)
	static const unsigned int WidgetBgColor          = 0xFF2D2B2E; // dark fill for inputs/checkboxes
	static const unsigned int WidgetBorderColor      = 0xFF44584C; // olive border for widgets
	static const unsigned int WidgetTextColor        = 0xFFD0D0D0; // light gray text
	static const unsigned int WidgetFocusBorderColor = 0xFF5A7A64; // brighter olive for focus
	static const int          SliderTrackHeight      = 6;
	static const int          SliderThumbW           = 12;
	static const int          SliderThumbH           = 16;

	// Touch metrics (minimum tap target in virtual coords ≈ 44dp)
	static const int          TouchMinTarget   = 28; // ≈44dp at 480dpi mapped to 768 tall

	// ============================================================
	// Helper: scale a virtual-coord value to current screen
	// Enforces minimum touch target size (≥44dp ≈ 28 virtual px)
	// ============================================================
	inline int ScaleY( int v ) { return (int)( v * uiStatic.scaleY ); }
	inline int ScaleX( int v ) { return (int)( v * uiStatic.scaleX ); }

	// Scale with touch-minimum enforcement
	inline int ScaleYTouch( int v )
	{
		int scaled = (int)( v * uiStatic.scaleY );
		int minPx = (int)( TouchMinTarget * uiStatic.scaleY );
		return scaled > minPx ? scaled : minPx;
	}

	// ============================================================
	// Drawing helpers (inline, zero overhead)
	// ============================================================

	// Draw a window background with border
	inline void DrawWindowChrome( int x, int y, int w, int h )
	{
		// Border
		UI_DrawRectangleExt( x, y, w, h, BorderColor, BorderWidth );
		// Fill
		UI_FillRect( x + BorderWidth, y + BorderWidth,
			w - BorderWidth * 2, h - BorderWidth * 2, BgColor );
	}

	// Draw title bar fill
	inline void DrawTitleBar( int x, int y, int w, int h )
	{
		UI_FillRect( x, y, w, h, TitleBarColor );
	}

	// Draw a single tab button
	inline void DrawTab( int x, int y, int w, int h, bool active, bool hover, const char *label )
	{
		unsigned int bgCol = active ? TabActiveColor : ( hover ? TabHoverColor : TabInactiveColor );
		unsigned int textCol = active ? TabTextActiveCol : TabTextColor;

		UI_FillRect( x, y, w, h, bgCol );

		int sides = QM_TOP | QM_LEFT | QM_RIGHT;
		if( !active )
			sides |= QM_BOTTOM;

		UI_DrawRectangleExt( x, y, w, h, TabBorderColor, 1, sides );

		int charH = (int)( h * 0.6f );
		UI_DrawString( uiStatic.hDefaultFont, x + TabPadH, y, w - TabPadH * 2,
			h, label, textCol, charH, QM_LEFT, 0 );
	}

	// Draw close button
	inline void DrawCloseBtn( int x, int y, int size, bool hover )
	{
		unsigned int bgCol = hover ? CloseBtnColor : TitleBarColor;
		UI_FillRect( x, y, size, size, bgCol );
		int charH = (int)( size * 0.7f );
		UI_DrawString( uiStatic.hDefaultFont, x, y, size, size,
			"X", CloseTextColor, charH, QM_CENTER, ETF_SHADOW );
	}
}

#endif // WINDOWSTYLE_H
