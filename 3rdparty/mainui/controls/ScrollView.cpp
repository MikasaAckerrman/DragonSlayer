#include "ScrollView.h"
#include "Scissor.h"

CMenuScrollView::CMenuScrollView() : CMenuItemsHolder (),
	m_bHoldingMouse1( false )
{
}

void CMenuScrollView::VidInit()
{
	colorStroke.SetDefault( uiInputFgColor );

	BaseClass::VidInit();

	m_iMax = 0;
	m_iPos = 0;

	FOR_EACH_VEC( m_pItems, i )
	{
		Point pt = m_pItems[i]->pos;
		Size sz = m_pItems[i]->size;

		m_iMax += pt.y + sz.h;
	}
	m_bDisableScrolling = (m_iMax < size.h);

	m_iMax *= uiStatic.scaleX;
}

bool CMenuScrollView::KeyDown( int key )
{
	// act when key is pressed or repeated
	if( !m_bDisableScrolling )
	{
		int newPos = m_iPos;
		if( UI::Key::IsUpArrow( key ))
			newPos -= 20;
		else if( UI::Key::IsDownArrow( key ))
			newPos += 20;
		else if( UI::Key::IsPageUp( key ))
			newPos -= 100;
		else if( UI::Key::IsPageDown( key ))
			newPos += 100;
		else if( UI::Key::IsLeftMouse( key ))
		{
			// m_bHoldingMouse1 = down != 0;
			// m_HoldingPoint = Point( uiStatic.cursorX, uiStatic.cursorY );
			// drag & drop
			// scrollbar
		}

		// TODO: overscrolling
		newPos = bound( 0, newPos, m_iMax - m_scSize.h );

		// recalc
		if( newPos != m_iPos )
		{
			m_iPos = newPos;
			FOR_EACH_VEC( m_pItems, i )
			{
				CMenuBaseItem *pItem = m_pItems[i];

				pItem->VidInit();
			}
			CMenuItemsHolder::MouseMove( uiStatic.cursorX, uiStatic.cursorY );
		}
	}

	return CMenuItemsHolder::KeyDown( key );
}

Point CMenuScrollView::GetPositionOffset() const
{
	return Point( 0, -m_iPos ) + BaseClass::GetPositionOffset();
}

bool CMenuScrollView::MouseMove( int x, int y )
{
	return CMenuItemsHolder::MouseMove( x, y );
}

bool CMenuScrollView::IsRectVisible(Point pt, Size sz)
{
	bool x = isrange( m_scPos.x, pt.x, m_scPos.x + m_scSize.w ) ||
			 isrange( pt.x, m_scPos.x, pt.x + sz.w );

	bool y = isrange( m_scPos.y, pt.y, m_scPos.y + m_scSize.h ) ||
			 isrange( pt.y, m_scPos.y, pt.y + sz.h );

	return x && y;
}

void CMenuScrollView::Draw()
{
	if( EngFuncs::KEY_IsDown( K_MOUSE1 ) )
	{
		if( !m_bHoldingMouse1 )
		{
			m_bHoldingMouse1 = true;
			m_HoldingPoint = Point( uiStatic.cursorX, uiStatic.cursorY );
		}
	}
	else
	{
		if( m_bHoldingMouse1 ) m_bHoldingMouse1 = false;
	}

	if( m_bHoldingMouse1 && !m_bDisableScrolling )
	{
		int newPos = m_iPos;

		newPos -= ( uiStatic.cursorY - m_HoldingPoint.y ) / 2;

		// TODO: overscrolling
		newPos = bound( 0, newPos, m_iMax - m_scSize.h );

		// recalc
		if( newPos != m_iPos )
		{
			m_iPos = newPos;
			FOR_EACH_VEC( m_pItems, i )
			{
				CMenuBaseItem *pItem = m_pItems[i];

				pItem->VidInit();
			}
		}
		m_HoldingPoint = Point( uiStatic.cursorX, uiStatic.cursorY );
	}

	if( bDrawStroke )
	{
		UI_DrawRectangleExt( m_scPos, m_scSize, colorStroke, iStrokeWidth );
	}

	int drawn = 0, skipped = 0;
	FOR_EACH_VEC( m_pItems, i )
	{
		if( !IsRectVisible( m_pItems[i]->GetRenderPosition(), m_pItems[i]->GetRenderSize() ) )
		{
			m_pItems[i]->iFlags |= QMF_HIDDENBYPARENT;
			skipped++;
		}
		else
		{
			m_pItems[i]->iFlags &= ~QMF_HIDDENBYPARENT;
			drawn++;
		}
	}

	Con_NPrintf( 0, "Drawn: %i Skipped: %i", drawn, skipped );

	UI::Scissor::PushScissor( m_scPos, m_scSize );
		CMenuItemsHolder::Draw();
	UI::Scissor::PopScissor();

	// CS 1.6 PC flat scrollbar (slayer3d): narrow dark track + orange
	// thumb, drawn outside the scissor on the right edge of the view.
	// Purely visual indicator — drag-to-scroll already works anywhere
	// inside the view, so no input hit-test on the bar itself.
	if( !m_bDisableScrolling && m_iMax > m_scSize.h )
	{
		int barW = (int)( 4 * uiStatic.scaleX );
		if( barW < 4 ) barW = 4;
		int barX = m_scPos.x + m_scSize.w - barW;
		int barY = m_scPos.y;
		int barH = m_scSize.h;

		UI_FillRect( barX, barY, barW, barH, uiInputBgColor );

		int thumbH = (int)( (float)barH * (float)barH / (float)m_iMax );
		if( thumbH < 16 ) thumbH = 16;
		if( thumbH > barH ) thumbH = barH;

		int range = m_iMax - m_scSize.h;
		int thumbY = barY;
		if( range > 0 )
			thumbY += (int)( (float)( barH - thumbH ) * (float)m_iPos / (float)range );

		// Defensive clamp: m_iPos can theoretically exceed range during a
		// VidInit recalc race (item count changed mid-frame), which would
		// push the thumb outside the track. Clamp to keep it inside.
		if( thumbY < barY ) thumbY = barY;
		if( thumbY + thumbH > barY + barH ) thumbY = barY + barH - thumbH;

		UI_FillRect( barX, thumbY, barW, thumbH, uiInputTextColor );
	}
}
