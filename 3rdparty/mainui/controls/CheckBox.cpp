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

	// CS 1.6 PC flat skin (slayer3d): drop bitmap textures, draw a plain
	// outlined square with an orange fill when checked. Bitmap pictures
	// (UI_CHECKBOX_*) are not shipped with most CS 1.6 paks on Android,
	// so the previous UI_DrawPic path rendered as a missing-asset stub.
	bool grayed  = ( iFlags & QMF_GRAYED ) != 0;
	bool focused = !grayed && m_pParent && ( this == m_pParent->ItemAtCursor() );
	bool pressed = m_bPressed;

	uint bgColor = pressed ? 0xC0303030 : 0xC0101010;
	uint borderColor;
	if( grayed )                       borderColor = uiInputFgColor;     // dimmed dark grey
	else if( focused || bChecked )     borderColor = uiInputTextColor;   // CS orange #FFA000
	else                               borderColor = uiInputFgColor;     // idle dark grey

	// background panel
	UI_FillRect( m_scPos, m_scSize, bgColor );
	// 1px border
	UI_DrawRectangle( m_scPos, m_scSize, borderColor );

	if( bChecked )
	{
		// solid orange square inset to ~50% of the box, centred
		Size innerSize;
		Point innerPos;
		innerSize.w = m_scSize.w / 2;
		innerSize.h = m_scSize.h / 2;
		innerPos.x  = m_scPos.x + ( m_scSize.w - innerSize.w ) / 2;
		innerPos.y  = m_scPos.y + ( m_scSize.h - innerSize.h ) / 2;

		uint fillColor = grayed ? uiInputFgColor : uiInputTextColor;
		UI_FillRect( innerPos, innerSize, fillColor );
	}
}

void CMenuCheckBox::UpdateEditable()
{
	bChecked = !!CvarValue();
}
