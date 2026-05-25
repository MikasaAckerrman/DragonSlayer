/*
WindowStyle.h -- shared visual style constants and helpers for windowed UI
CS 1.6 olive/green theme palette
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
// Color palette (ARGB packed -- 0xAARRGGBB)
// CS 1.6 olive/green theme
// ============================================================
namespace WndStyle
{
	// Background (dark olive)
	static const unsigned int BgColor          = 0xFF3C4B4B;
	static const unsigned int BgColorSolid     = 0xFF3C4B4B;

	// Border (olive green)
	static const unsigned int BorderColor      = 0xFF5E8E6B;
	static const int          BorderWidth      = 2;

	// Title bar (darker olive)
	static const unsigned int TitleBarColor    = 0xFF2E3D3D;
	static const unsigned int TitleTextColor   = 0xFFC8D0C8; // light greenish white
	static const int          TitleBarHeight   = 26; // virtual coords

	// Close button
	static const unsigned int CloseBtnColor    = 0xFFCC2222; // red in ARGB
	static const unsigned int CloseTextColor   = 0xFFCCCCCC;
	static const int          CloseBtnSize     = 20;

	// Tabs (olive variants)
	static const unsigned int TabActiveColor   = 0xFF3C4B4B; // same as BgColor
	static const unsigned int TabInactiveColor = 0xFF2E3838; // darker olive
	static const unsigned int TabHoverColor    = 0xFF485858; // lighter olive
	static const unsigned int TabTextColor     = 0xFFA0A0A0; // gray text for inactive
	static const unsigned int TabTextActiveCol = 0xFFFFFFFF; // white for active
	static const unsigned int TabBorderColor   = 0xFF5E8E6B; // olive green border
	static const int          TabHeight        = 22;
	static const int          TabMinWidth      = 64;
	static const int          TabPadH          = 10;
	static const int          TabPadBetween    = 2;

	// Icon
	static const int          IconSize         = 16;
	static const int          IconPad          = 4;
	static const int          TitlePadLeft     = 6;

	// Widget style (CS 1.6 flat olive look)
	static const unsigned int WidgetBgColor          = 0xFF1E2828; // dark olive fill
	static const unsigned int WidgetBorderColor      = 0xFF5E8E6B; // olive green border
	static const unsigned int WidgetTextColor        = 0xFFC8D0C8; // light greenish text
	static const unsigned int WidgetFocusBorderColor = 0xFF7EAE8B; // brighter olive-green focus
	static const int          SliderTrackHeight      = 6;
	static const int          SliderThumbW           = 12;
	static const int          SliderThumbH           = 16;

	// Touch metrics (minimum tap target in virtual coords)
	static const int          TouchMinTarget   = 28;

	// Bevel and separator colors
	static const unsigned int InnerHighlightColor  = 0xFF6EA87B; // 1px lighter bevel inside window
	static const unsigned int TitleSeparatorColor  = 0xFF5E8E6B; // line below title bar

	// 3D Bevel colors (CS 1.6 raised/sunken frame edges)
	static const unsigned int BevelHighlight  = 0xFF8EBE9B; // brightest - outer top-left edge
	static const unsigned int BevelLight      = 0xFF6EA87B; // inner top-left edge
	static const unsigned int BevelShadow     = 0xFF3E5E4B; // inner bottom-right edge
	static const unsigned int BevelDarkShadow = 0xFF1E2E25; // darkest - outer bottom-right edge

	// ============================================================
	// Helper: scale a virtual-coord value to current screen
	// Enforces minimum touch target size
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

	// Draw a 2px raised 3D bevel frame (highlight on top-left, shadow on bottom-right)
	inline void DrawRaisedBevel( int x, int y, int w, int h )
	{
		// Outer edges
		UI_FillRect( x, y, w, 1, BevelHighlight );           // top outer
		UI_FillRect( x, y, 1, h, BevelHighlight );           // left outer
		UI_FillRect( x, y + h - 1, w, 1, BevelDarkShadow ); // bottom outer
		UI_FillRect( x + w - 1, y, 1, h, BevelDarkShadow ); // right outer
		// Inner edges
		UI_FillRect( x + 1, y + 1, w - 2, 1, BevelLight );       // top inner
		UI_FillRect( x + 1, y + 1, 1, h - 2, BevelLight );       // left inner
		UI_FillRect( x + 1, y + h - 2, w - 2, 1, BevelShadow );  // bottom inner
		UI_FillRect( x + w - 2, y + 1, 1, h - 2, BevelShadow );  // right inner
	}

	// Draw a 2px sunken 3D bevel frame (shadow on top-left, highlight on bottom-right)
	inline void DrawSunkenBevel( int x, int y, int w, int h )
	{
		// Outer edges
		UI_FillRect( x, y, w, 1, BevelDarkShadow );         // top outer
		UI_FillRect( x, y, 1, h, BevelDarkShadow );         // left outer
		UI_FillRect( x, y + h - 1, w, 1, BevelHighlight );  // bottom outer
		UI_FillRect( x + w - 1, y, 1, h, BevelHighlight );  // right outer
		// Inner edges
		UI_FillRect( x + 1, y + 1, w - 2, 1, BevelShadow );     // top inner
		UI_FillRect( x + 1, y + 1, 1, h - 2, BevelShadow );     // left inner
		UI_FillRect( x + 1, y + h - 2, w - 2, 1, BevelLight );  // bottom inner
		UI_FillRect( x + w - 2, y + 1, 1, h - 2, BevelLight );  // right inner
	}

	// ============================================================
	// Drawing helpers (inline, zero overhead)
	// ============================================================

	// Draw a window background with border, inner highlight bevel, and title separator
	inline void DrawWindowChrome( int x, int y, int w, int h )
	{
		// 3D raised bevel frame (2px)
		DrawRaisedBevel( x, y, w, h );

		// Fill background inside bevel
		int ix = x + 2;
		int iy = y + 2;
		int iw = w - 4;
		int ih = h - 4;
		UI_FillRect( ix, iy, iw, ih, BgColor );

		// Title bar separator line below title bar area
		int sepY = y + 2 + TitleBarHeight;
		UI_FillRect( ix, sepY, iw, 1, TitleSeparatorColor );
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

	// Draw close button with olive border
	inline void DrawCloseBtn( int x, int y, int size, bool hover )
	{
		unsigned int bgCol = hover ? CloseBtnColor : TitleBarColor;
		UI_FillRect( x, y, size, size, bgCol );
		int charH = (int)( size * 0.7f );
		UI_DrawString( uiStatic.hDefaultFont, x, y, size, size,
			"X", CloseTextColor, charH, QM_CENTER, ETF_SHADOW );
		// 1px olive border around the close button
		UI_DrawRectangleExt( x, y, size, size, BorderColor, 1 );
	}

	// Draw a flat button with hover state (for toolbar/dialog buttons)
	inline void DrawFlatButton( int x, int y, int w, int h, const char *label, bool hover )
	{
		unsigned int bg = hover ? TabHoverColor : BgColor;
		UI_FillRect( x + 2, y + 2, w - 4, h - 4, bg );
		DrawRaisedBevel( x, y, w, h );
		int charH = (int)( h * 0.6f );
		UI_DrawString( uiStatic.hDefaultFont, x + 2, y + 2, w - 4, h - 4,
			label, WidgetTextColor, charH, QM_CENTER, 0 );
	}
}

#endif // WINDOWSTYLE_H
