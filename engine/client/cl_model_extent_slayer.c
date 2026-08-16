/*
cl_model_extent_slayer.c - Slayer3D: the REAL size and axes of a studio model
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

// See cl_model_extent_slayer.h for the measurement that motivates this file.
//
// IMPLEMENTATION NOTES
//
// * NO ENGINE HEADERS. Only <string.h> and <math.h>. The .mdl layout is walked
//   through explicit byte offsets rather than through the engine's studio structs,
//   for two reasons: the file can then be compiled and asserted on the host
//   against the real .mdl files on disk (tests/model_extent_test.c), and every
//   offset is bounds-checked against the buffer length, which the struct-cast
//   style silently is not.
//
// * The offsets are taken from engine/studio.h and re-stated here as named
//   constants with a compile-time-visible comment each. A wrong offset is the one
//   error that would produce plausible-looking nonsense, so tests/model_extent_test.c
//   cross-checks every result against an independent Python implementation.
//
// * Vertices are transformed by their bone's REFERENCE POSE, i.e. bone.value[0..5]
//   composed up the parent chain. That is the pose R_StudioSetupBones produces
//   with no animation applied, which is what an item lying on the ground is drawn
//   in. Doing this is the whole point: skipping it is exactly the bug in
//   Mod_StudioComputeBounds that made the engine's box unusable.

#include <string.h>
#include <math.h>

#include "cl_model_extent_slayer.h"

// ---------------------------------------------------------------------------
// .mdl layout, v10 (mirrors engine/studio.h; offsets in bytes from the header)
// ---------------------------------------------------------------------------

#define ME_IDENT_IDST      0x54534449   // "IDST" little-endian
#define ME_VERSION         10

#define ME_HDR_IDENT       0
#define ME_HDR_VERSION     4
#define ME_HDR_LENGTH      72
#define ME_HDR_NUMBONES    140
#define ME_HDR_BONEINDEX   144
#define ME_HDR_NUMBODYPARTS 204
#define ME_HDR_BODYPARTIDX 208
#define ME_HDR_MIN_SIZE    244          // enough to hold every field we read

// mstudiobone_t: char name[32]; int parent, flags; int bonecontroller[6];
//                float value[6]; float scale[6];
#define ME_BONE_SIZE       112
#define ME_BONE_PARENT     32
#define ME_BONE_VALUE      64           // pos[3] then rot[3], radians

// mstudiobodyparts_t: char name[64]; int nummodels, base, modelindex;
#define ME_BP_SIZE         76
#define ME_BP_NUMMODELS    64
#define ME_BP_MODELINDEX   72

// mstudiomodel_t: char name[64]; int unused; float unused2; int nummesh,
//   meshindex, numverts, vertinfoindex, vertindex, ...
#define ME_MODEL_SIZE      112
#define ME_MODEL_NUMVERTS  80
#define ME_MODEL_VERTINFO  84
#define ME_MODEL_VERTINDEX 88

// Sanity ceilings. A hostile or truncated file must be rejected, not trusted:
// numverts is an int read straight out of the file and is used to size a loop.
#define ME_MAX_BONES       256
#define ME_MAX_BODYPARTS   64
#define ME_MAX_MODELS      64
#define ME_MAX_VERTS       262144

typedef float me_matrix[3][4];

// ---------------------------------------------------------------------------
// Bounds-checked readers
// ---------------------------------------------------------------------------

static int ME_ReadInt( const unsigned char *base, int len, int off, int *out )
{
	if( off < 0 || off + 4 > len )
		return 0;

	memcpy( out, base + off, 4 );
	return 1;
}

static int ME_ReadFloat( const unsigned char *base, int len, int off, float *out )
{
	if( off < 0 || off + 4 > len )
		return 0;

	memcpy( out, base + off, 4 );
	return 1;
}

static int ME_ReadVec3( const unsigned char *base, int len, int off, float *out )
{
	int i;

	for( i = 0; i < 3; i++ )
	{
		if( !ME_ReadFloat( base, len, off + i * 4, out + i ))
			return 0;
	}
	return 1;
}

// ---------------------------------------------------------------------------
// Reference-pose bone matrices
// ---------------------------------------------------------------------------

/*
====================
ME_MatrixFromPosRot

The rotation the engine builds for a bone's reference pose, as a matrix.

R_StudioCalcBoneQuaternion turns bone.value[3..5] (Euler XYZ in radians) into a
quaternion via AngleQuaternion(..., studio = true), and Matrix3x4_FromOriginQuat
turns that into this matrix. Composing the two symbolically gives the ZYX product
written out below; doing it that way rather than going through a quaternion keeps
this file free of the engine's math library, and the harness checks the result
against the engine's own path for a set of real bones.
====================
*/
static void ME_MatrixFromPosRot( me_matrix out, const float *pos, const float *rot )
{
	float sx = (float)sin( (double)rot[0] ), cx = (float)cos( (double)rot[0] );
	float sy = (float)sin( (double)rot[1] ), cy = (float)cos( (double)rot[1] );
	float sz = (float)sin( (double)rot[2] ), cz = (float)cos( (double)rot[2] );

	out[0][0] = cy * cz;
	out[1][0] = cy * sz;
	out[2][0] = -sy;

	out[0][1] = sx * sy * cz - cx * sz;
	out[1][1] = sx * sy * sz + cx * cz;
	out[2][1] = sx * cy;

	out[0][2] = cx * sy * cz + sx * sz;
	out[1][2] = cx * sy * sz - sx * cz;
	out[2][2] = cx * cy;

	out[0][3] = pos[0];
	out[1][3] = pos[1];
	out[2][3] = pos[2];
}

static void ME_MatrixConcat( me_matrix out, const me_matrix a, const me_matrix b )
{
	int i;

	for( i = 0; i < 3; i++ )
	{
		out[i][0] = a[i][0] * b[0][0] + a[i][1] * b[1][0] + a[i][2] * b[2][0];
		out[i][1] = a[i][0] * b[0][1] + a[i][1] * b[1][1] + a[i][2] * b[2][1];
		out[i][2] = a[i][0] * b[0][2] + a[i][1] * b[1][2] + a[i][2] * b[2][2];
		out[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
	}
}

static void ME_Transform( const me_matrix m, const float *v, float *out )
{
	out[0] = m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2] + m[0][3];
	out[1] = m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2] + m[1][3];
	out[2] = m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] + m[2][3];
}

/*
====================
ME_BuildBones

Compose every bone's reference pose up its parent chain.

`parent` comes out of the file and is only trusted when it refers to an ALREADY
BUILT bone (parent < current index). Studio files are authored parents-first, and
a forward or self reference would either read an uninitialised matrix or loop --
so such a bone is treated as a root instead. That is a safety rule, not a
formatting assumption: it cannot go wrong on a well-formed file.
====================
*/
static int ME_BuildBones( const unsigned char *base, int len, int numbones,
	int boneindex, me_matrix *mats )
{
	int i;

	for( i = 0; i < numbones; i++ )
	{
		int   off = boneindex + i * ME_BONE_SIZE;
		int   parent = -1;
		float value[6];
		me_matrix local;
		int   j;

		if( !ME_ReadInt( base, len, off + ME_BONE_PARENT, &parent ))
			return 0;

		for( j = 0; j < 6; j++ )
		{
			if( !ME_ReadFloat( base, len, off + ME_BONE_VALUE + j * 4, &value[j] ))
				return 0;
		}

		ME_MatrixFromPosRot( local, &value[0], &value[3] );

		if( parent >= 0 && parent < i )
			ME_MatrixConcat( mats[i], mats[parent], local );
		else
			memcpy( mats[i], local, sizeof( local ));
	}

	return 1;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

int Slayer_ModelExtent_Measure( const void *data, int len, slayer_model_extent_t *out )
{
	const unsigned char *base = (const unsigned char *)data;
	me_matrix mats[ME_MAX_BONES];
	int   ident = 0, version = 0;
	int   numbones = 0, boneindex = 0;
	int   numbp = 0, bpindex = 0;
	int   i, j, k, axis;
	int   nverts = 0;
	float lo[3], hi[3];

	if( !out )
		return 0;

	memset( out, 0, sizeof( *out ));

	if( !base || len < ME_HDR_MIN_SIZE )
		return 0;

	if( !ME_ReadInt( base, len, ME_HDR_IDENT, &ident ) || ident != ME_IDENT_IDST )
		return 0;
	if( !ME_ReadInt( base, len, ME_HDR_VERSION, &version ) || version != ME_VERSION )
		return 0;

	if( !ME_ReadInt( base, len, ME_HDR_NUMBONES, &numbones ))
		return 0;
	if( !ME_ReadInt( base, len, ME_HDR_BONEINDEX, &boneindex ))
		return 0;
	if( !ME_ReadInt( base, len, ME_HDR_NUMBODYPARTS, &numbp ))
		return 0;
	if( !ME_ReadInt( base, len, ME_HDR_BODYPARTIDX, &bpindex ))
		return 0;

	if( numbones <= 0 || numbones > ME_MAX_BONES )
		return 0;
	if( numbp <= 0 || numbp > ME_MAX_BODYPARTS )
		return 0;

	if( !ME_BuildBones( base, len, numbones, boneindex, mats ))
		return 0;

	lo[0] = lo[1] = lo[2] =  1e30f;
	hi[0] = hi[1] = hi[2] = -1e30f;

	for( i = 0; i < numbp; i++ )
	{
		int off = bpindex + i * ME_BP_SIZE;
		int nmodels = 0, modelindex = 0;

		if( !ME_ReadInt( base, len, off + ME_BP_NUMMODELS, &nmodels ))
			return 0;
		if( !ME_ReadInt( base, len, off + ME_BP_MODELINDEX, &modelindex ))
			return 0;
		if( nmodels <= 0 || nmodels > ME_MAX_MODELS )
			continue;

		for( j = 0; j < nmodels; j++ )
		{
			int moff = modelindex + j * ME_MODEL_SIZE;
			int numverts = 0, vertinfo = 0, vertindex = 0;

			if( !ME_ReadInt( base, len, moff + ME_MODEL_NUMVERTS, &numverts ))
				return 0;
			if( !ME_ReadInt( base, len, moff + ME_MODEL_VERTINFO, &vertinfo ))
				return 0;
			if( !ME_ReadInt( base, len, moff + ME_MODEL_VERTINDEX, &vertindex ))
				return 0;

			if( numverts <= 0 || numverts > ME_MAX_VERTS )
				continue;
			if( vertindex < 0 || vertindex + numverts * 12 > len )
				continue;

			for( k = 0; k < numverts; k++ )
			{
				float v[3], w[3];
				int   bone = 0;

				if( !ME_ReadVec3( base, len, vertindex + k * 12, v ))
					return 0;

				// Vertex-to-bone assignment is ONE BYTE per vertex. A file with a
				// bogus vertinfoindex (or none) must not index the bone array out
				// of range -- fall back to bone 0, which for the single-bone world
				// models that matter here is the only bone anyway.
				if( vertinfo > 0 && vertinfo + k < len )
					bone = base[vertinfo + k];
				if( bone < 0 || bone >= numbones )
					bone = 0;

				ME_Transform( mats[bone], v, w );

				for( axis = 0; axis < 3; axis++ )
				{
					if( w[axis] < lo[axis] ) lo[axis] = w[axis];
					if( w[axis] > hi[axis] ) hi[axis] = w[axis];
				}
				nverts++;
			}
		}
	}

	if( nverts <= 0 )
		return 0;

	for( axis = 0; axis < 3; axis++ )
	{
		out->mins[axis]   = lo[axis];
		out->maxs[axis]   = hi[axis];
		out->half[axis]   = ( hi[axis] - lo[axis] ) * 0.5f;
		out->center[axis] = ( hi[axis] + lo[axis] ) * 0.5f;
	}

	out->long_axis  = 0;
	out->short_axis = 0;
	for( axis = 1; axis < 3; axis++ )
	{
		if( out->half[axis] > out->half[out->long_axis] )
			out->long_axis = axis;
		if( out->half[axis] < out->half[out->short_axis] )
			out->short_axis = axis;
	}

	out->nverts = nverts;
	out->nbones = numbones;
	out->valid  = 1;
	return 1;
}
