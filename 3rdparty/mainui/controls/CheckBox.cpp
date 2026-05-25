/*
CheckBox.h - checkbox
Copyright (C) 2010 Uncle Mike
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

#include "extdll_menu.h"
#include "BaseMenu.h"
#include "CheckBox.h"
#include "Utils.h"
#include "WindowStyle.h"
#include "SchemeManager.h"

CMenuCheckBox::CMenuCheckBox() : BaseClass()
{
	SetCharSize( QM_DEFAULTFONT );
	SetSize( 32, 32 );
	SetPicture( UI_CHECKBOX_EMPTY,
		UI_CHECKBOX_FOCUS,
		UI_CHECKBOX_PRESSED,
		UI_CHECKBOX_ENABLED,
		UI_CHECKBOX_GRAYED );
	bChecked = false;
	eFocusAnimation = QM_HIGHLIGHTIFFOCUS;
	iFlags |= QMF_DROPSHADOW;
	bChangeOnPressed = false;
	colorBase = uiColorWhite;
	colorFocus = uiColorWhite;
	iMask = 0;
	bInvertMask = false;
}

/*
=================
CMenuCheckBox::Init
=================
*/
void CMenuCheckBox::VidInit( void )
{
	colorText.SetDefault( uiColorHelp );
	BaseClass::VidInit();
	m_scTextPos.x = m_scPos.x + ( m_scSize.w * 1.25f );
	m_scTextPos.y = m_scPos.y;

	m_scTextSize.w = g_FontMgr->GetTextWideScaled( font, szName, m_scChSize );
	m_scTextSize.h = m_scChSize;
}

bool CMenuCheckBox::KeyUp( int key )
{
	const char	*sound = 0;

	if( UI::Key::IsLeftMouse( key ) && FBitSet( iFlags, QMF_HASMOUSEFOCUS ))
		sound = uiStatic.sounds[SND_GLOW];
	else if( UI::Key::IsEnter( key ) && !FBitSet( iFlags, QMF_MOUSEONLY ))
		sound = uiStatic.sounds[SND_GLOW];

	if( sound )
	{
		_Event( QM_RELEASED );
		if( !bChangeOnPressed )
		{
			bChecked = !bChecked;	// apply on release
			SetCvarValue( bChecked );
			_Event( QM_CHANGED );
		}
		PlayLocalSound( sound );
	}

	return sound != NULL;
}

bool CMenuCheckBox::KeyDown( int key )
{
	const char	*sound = 0;

	if( UI::Key::IsLeftMouse( key ) && FBitSet( iFlags, QMF_HASMOUSEFOCUS ))
		sound = uiStatic.sounds[SND_GLOW];
	else if( UI::Key::IsEnter( key ) && !FBitSet( iFlags, QMF_MOUSEONLY ))
		sound = uiStatic.sounds[SND_GLOW];

	if( sound )
	{
		_Event( QM_PRESSED );
		if( bChangeOnPressed )
		{
			bChecked = !bChecked;	// apply on release
			SetCvarValue( bChecked );
			_Event( QM_CHANGED );
		}
		PlayLocalSound( sound );
	}

	return sound != NULL;
}


/*
=================
CMenuCheckBox::Draw
=================
*/
void CMenuCheckBox::Draw( void )
{
	uint textflags = ( iFlags & QMF_DROPSHADOW ? ETF_SHADOW : 0 ) | ETF_NOSIZELIMIT | ETF_FORCECOL;

	UI_DrawString( font, m_scTextPos, m_scTextSize, szName, colorText, m_scChSize, eTextAlignment, textflags );

	if( szStatusText && iFlags & QMF_NOTIFY )
	{
		Point coord;

		if( szName[0] )
			coord.x = ( uiStatic.buttons_draw_size.w + 40 ) * uiStatic.scaleX;
		else
			coord.x = m_scSize.w + 16 * uiStatic.scaleX;
		coord.x += m_scPos.x;
		coord.y = m_scPos.y + m_scSize.h / 2 - EngFuncs::ConsoleCharacterHeight() / 2;

		int	r, g, b;

		UnpackRGB( r, g, b, uiColorHelp );
		EngFuncs::DrawSetTextColor( r, g, b );
		EngFuncs::DrawConsoleString( coord, szStatusText );
	}

	// Flat CS 1.6 style rendering
	CSchemeManager *scheme = CSchemeManager::GetInstance();
	int boxSize = m_scSize.h; // use widget height as box size

	unsigned int inputBg = scheme->GetColor("InputBG");
	if( !inputBg ) inputBg = WndStyle::WidgetBgColor;

	if( iFlags & QMF_GRAYED )
	{
		WndStyle::DrawSunkenBevel( m_scPos.x, m_scPos.y, boxSize, boxSize );
		UI_FillRect( m_scPos.x + 2, m_scPos.y + 2, boxSize - 4, boxSize - 4, inputBg );
		return;
	}

	// Darken fill when pressed for tactile feedback
	unsigned int pressedBg = scheme->GetColor("TitleBarBG");
	if( !pressedBg ) pressedBg = WndStyle::TitleBarColor;
	unsigned int fillCol = inputBg;
	if( m_bPressed )
		fillCol = pressedBg;

	// Draw box background and border (sunken bevel)
	WndStyle::DrawSunkenBevel( m_scPos.x, m_scPos.y, boxSize, boxSize );
	UI_FillRect( m_scPos.x + 2, m_scPos.y + 2, boxSize - 4, boxSize - 4, fillCol );

	// Focus indicator: 1px bright border outside the bevel
	if( this == m_pParent->ItemAtCursor() )
		UI_DrawRectangleExt( m_scPos.x - 1, m_scPos.y - 1, boxSize + 2, boxSize + 2, WndStyle::WidgetFocusBorderColor, 1 );

	// Draw checkmark when checked
	if( bChecked )
	{
		unsigned int checkColor = scheme->GetColor("CheckMark");
		if( !checkColor ) checkColor = scheme->GetColor("ControlFG");
		if( !checkColor ) checkColor = WndStyle::WidgetTextColor;
		UI_DrawString( font, m_scPos.x, m_scPos.y, boxSize, boxSize,
			"v", checkColor, boxSize - 4, QM_CENTER, ETF_FORCECOL );
	}
}

void CMenuCheckBox::UpdateEditable()
{
	bChecked = !!CvarValue();
}
