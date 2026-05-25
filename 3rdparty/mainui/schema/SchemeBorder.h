/*
SchemeBorder.h - Border system for VGUI2-style scheme rendering
Copyright (C) 2024

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#pragma once
#ifndef SCHEMEBORDER_H
#define SCHEMEBORDER_H

#include "BaseMenu.h"

struct BorderLayer
{
	unsigned int color;  // packed ARGB
	int offset;          // pixel offset from edge
};

struct BorderSide
{
	BorderLayer layers[4];
	int numLayers;
};

struct CSchemeBorder
{
	BorderSide left, right, top, bottom;
	int insetX, insetY;

	void Draw( int x, int y, int w, int h )
	{
		// Left side: vertical 1px lines
		for( int i = 0; i < left.numLayers; i++ )
		{
			UI_FillRect( x + left.layers[i].offset, y, 1, h, left.layers[i].color );
		}

		// Right side: vertical 1px lines
		for( int i = 0; i < right.numLayers; i++ )
		{
			UI_FillRect( x + w - 1 - right.layers[i].offset, y, 1, h, right.layers[i].color );
		}

		// Top side: horizontal 1px lines
		for( int i = 0; i < top.numLayers; i++ )
		{
			UI_FillRect( x, y + top.layers[i].offset, w, 1, top.layers[i].color );
		}

		// Bottom side: horizontal 1px lines
		for( int i = 0; i < bottom.numLayers; i++ )
		{
			UI_FillRect( x, y + h - 1 - bottom.layers[i].offset, w, 1, bottom.layers[i].color );
		}
	}

	void Clear()
	{
		left.numLayers = 0;
		right.numLayers = 0;
		top.numLayers = 0;
		bottom.numLayers = 0;
		insetX = 0;
		insetY = 0;
	}
};

#endif // SCHEMEBORDER_H
