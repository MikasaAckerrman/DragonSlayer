/*
cl_muzzle_slayer.c - Slayer3D: which attachment is the muzzle for the current
                     animation
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

// See cl_muzzle_slayer.h for the measurement this is built on.
//
// IMPLEMENTATION NOTES
//
// * NO ENGINE HEADERS, only <string.h>. Same reason as cl_model_extent_slayer.c:
//   the file is compiled into the host harness and run against the .mdl files in
//   the player's installation, so the thing under test is the shipping code and
//   not a paraphrase of it.
//
// * The .mdl layout is walked through named byte offsets taken from
//   engine/studio.h and common/studio_event.h. mstudioseqdesc_t is the one struct
//   here it is easy to get wrong -- it is 176 bytes with `numevents` at 48, not
//   at 44 as a naive reading of an older header suggests -- so the offsets are
//   asserted against real files by tests/muzzle_attach_test.c, which compares
//   against an independent Python implementation (tests/mdl_seq_events.py).

#include <string.h>

#include "cl_muzzle_slayer.h"

// ---------------------------------------------------------------------------
// .mdl layout, v10 (mirrors engine/studio.h; offsets in bytes from the header)
// ---------------------------------------------------------------------------

#define MZ_IDENT_IDST      0x54534449   // "IDST"
#define MZ_IDENT_IDSQ      0x51534449   // "IDSQ" -- a sequence group file
#define MZ_VERSION         10

#define MZ_HDR_IDENT       0
#define MZ_HDR_VERSION     4
#define MZ_HDR_LENGTH      72
#define MZ_HDR_NUMSEQ      164
#define MZ_HDR_SEQINDEX    168
#define MZ_HDR_NUMATTACH   212
#define MZ_HDR_ATTACHIDX   216
#define MZ_HDR_MIN_SIZE    244

// mstudioseqdesc_t. Only the three fields that matter, and the size, which is
// what makes indexing correct.
#define MZ_SEQ_SIZE        176
#define MZ_SEQ_NUMEVENTS   48
#define MZ_SEQ_EVENTINDEX  52

// mstudioevent_t: int32 frame; int32 event; int32 unused; char options[64];
#define MZ_EVENT_SIZE      76
#define MZ_EVENT_EVENT     4

static int MZ_ReadI32( const unsigned char *p )
{
	// Assembled byte by byte rather than cast: the offsets above are not
	// guaranteed to be 4-byte aligned on every field of every file, and an
	// unaligned int32 load is undefined behaviour on the ARM targets this ships to.
	return (int)( (unsigned int)p[0]
		| ( (unsigned int)p[1] << 8 )
		| ( (unsigned int)p[2] << 16 )
		| ( (unsigned int)p[3] << 24 ));
}

// Every read goes through here, so an out-of-range offset can only ever produce
// "unknown", never a wild pointer.
static int MZ_Field( const unsigned char *base, int len, int off, int *out )
{
	if( off < 0 || off + 4 > len )
		return 0;

	*out = MZ_ReadI32( base + off );
	return 1;
}

static int MZ_HeaderOk( const unsigned char *base, int len )
{
	int ident, version;

	if( !base || len < MZ_HDR_MIN_SIZE )
		return 0;

	if( !MZ_Field( base, len, MZ_HDR_IDENT, &ident ))
		return 0;
	if( !MZ_Field( base, len, MZ_HDR_VERSION, &version ))
		return 0;

	// IDSQ is a sequence-group file: it has animation data but no sequence
	// descriptions of its own, so asking it about muzzles is meaningless.
	if( ident != MZ_IDENT_IDST )
		return 0;
	if( version != MZ_VERSION )
		return 0;

	return 1;
}

int Slayer_Muzzle_NumAttachments( const void *data, int len )
{
	const unsigned char *base = (const unsigned char *)data;
	int n;

	if( !MZ_HeaderOk( base, len ))
		return 0;

	if( !MZ_Field( base, len, MZ_HDR_NUMATTACH, &n ))
		return 0;

	if( n < 0 )
		return 0;

	return n;
}

int Slayer_Muzzle_AttachmentForSequence( const void *data, int len, int sequence )
{
	const unsigned char *base = (const unsigned char *)data;
	int numseq, seqindex, numattach;
	int numevents, eventindex;
	int seq_off;
	int i;
	int found = -1;

	if( !MZ_HeaderOk( base, len ))
		return -1;

	if( !MZ_Field( base, len, MZ_HDR_NUMSEQ, &numseq ))
		return -1;
	if( !MZ_Field( base, len, MZ_HDR_SEQINDEX, &seqindex ))
		return -1;

	if( sequence < 0 || sequence >= numseq )
		return -1;

	numattach = Slayer_Muzzle_NumAttachments( data, len );

	// A model with one attachment has one possible answer and does not need to
	// carry an event to say so. Returning -1 here would make the caller treat a
	// perfectly ordinary rifle as "unknown".
	if( numattach <= 1 )
		return numattach == 1 ? 0 : -1;

	seq_off = seqindex + sequence * MZ_SEQ_SIZE;
	if( seq_off < 0 || seq_off + MZ_SEQ_SIZE > len )
		return -1;

	if( !MZ_Field( base, len, seq_off + MZ_SEQ_NUMEVENTS, &numevents ))
		return -1;
	if( !MZ_Field( base, len, seq_off + MZ_SEQ_EVENTINDEX, &eventindex ))
		return -1;

	if( numevents <= 0 )
		return -1;

	for( i = 0; i < numevents; i++ )
	{
		int ev_off = eventindex + i * MZ_EVENT_SIZE;
		int ev;
		int idx;

		if( ev_off < 0 || ev_off + MZ_EVENT_SIZE > len )
			break;   // truncated event list: keep whatever was found before it

		if( !MZ_Field( base, len, ev_off + MZ_EVENT_EVENT, &ev ))
			break;

		if( ev < SLAYER_MUZZLE_EVENT_BASE )
			continue;

		idx = ( ev - SLAYER_MUZZLE_EVENT_BASE );
		if( idx % SLAYER_MUZZLE_EVENT_STRIDE != 0 )
			continue;      // 5002 is a spark, 5004 a sound -- not muzzles

		idx /= SLAYER_MUZZLE_EVENT_STRIDE;
		if( idx < 0 || idx >= SLAYER_MUZZLE_ATTACH_MAX )
			continue;

		// The attachment has to EXIST on this model. A model edited down to fewer
		// attachments while keeping its event list would otherwise send the tracer
		// to a point the renderer never filled in.
		if( idx >= numattach )
			continue;

		// FIRST muzzle event in the sequence wins. Sequences with more than one
		// exist (a burst animation flashing twice), and the shot we are placing is
		// the one that started the animation, i.e. the earliest. Events are stored
		// in frame order, so the first in the list is the earliest in time.
		found = idx;
		break;
	}

	return found;
}
