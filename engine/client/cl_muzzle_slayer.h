/*
cl_muzzle_slayer.h - Slayer3D: WHICH attachment is the muzzle for the animation
                     that is playing right now
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
#ifndef CL_MUZZLE_SLAYER_H
#define CL_MUZZLE_SLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

// WHY THIS EXISTS
//
// The tracer started every shot at `attachment[0]`, on the reasoning that
// EF_MUZZLEFLASH is defined as "an elight on attachment 0". That is true of the
// ENGINE's own fallback flash, and it is wrong for any weapon with more than one
// barrel: with the Dual Berettas every tracer left the same gun, whichever gun
// had actually fired.
//
// The model states the answer itself, and this was MEASURED on the files in the
// player's own installation (tests/mdl_seq_events.py):
//
//   v_elite.mdl        4 attachments, 16 sequences
//     shoot_left1..5, shoot_leftlast    event 5001  -> attachment[0]
//                                       (bone v_weapon.m9a1_L_parent)
//     shoot_right1..5, shoot_rightlast  event 5011  -> attachment[1]
//                                       (bone v_weapon.m9a1_R_parent)
//
//   player/*/*.mdl (arctic, gign, gsg9, ... all identical in this respect)
//     ref_shoot_dualpistols_1           event 5011  -> attachment[1]
//     ref_shoot2_dualpistols_1          event 5001  -> attachment[0]
//     ref_shoot_onehanded / _rifle /
//       _carbine / _shotgun             event 5001  -> attachment[0]
//
// Studio events 5001 / 5011 / 5021 / 5031 mean "muzzle flash at attachment
// 0 / 1 / 2 / 3" -- that is how the client library implements HUD_StudioEvent
// (cs16-client cl_dll/entity.cpp) and how the engine's own R_StudioClientEvents
// treats EF_MUZZLEFLASH. So the muzzle for a shot is not a property of the
// WEAPON, it is a property of the SEQUENCE that is playing: the same model fires
// from a different attachment depending on which animation was started.
//
// NOTE the player models invert the indices relative to the viewmodel
// (attachment[0] is the RIGHT hand there, and the LEFT gun's sequence is the one
// that uses attachment[1]). That is exactly why the index must be read from the
// model being sampled and can never be carried from one model to another, and it
// is why "just use attachment[1] for pistols" would have been wrong in third
// person while looking right in first.
//
// Reading it costs a bounds-checked walk over the events of ONE sequence -- one
// to six entries on every model measured -- so there is nothing to cache.
//
// No engine headers here (only <string.h>), so tests/muzzle_attach_test.c runs
// this exact code against the real .mdl files on disk.

// The studio event numbers, as a named set rather than four bare integers.
#define SLAYER_MUZZLE_EVENT_BASE   5001   // attachment[0]
#define SLAYER_MUZZLE_EVENT_STRIDE 10     // 5001, 5011, 5021, 5031
#define SLAYER_MUZZLE_ATTACH_MAX   4      // cl_entity_t::attachment[4]

// Which attachment does `sequence` fire from?
//
// `data` is the studio header as loaded in memory, `len` its byte length (the
// header's own `length` field); every offset read is checked against it, so a
// truncated or hostile model returns a failure instead of reading past the end.
//
// Returns 0..SLAYER_MUZZLE_ATTACH_MAX-1, or -1 when this sequence says nothing
// about a muzzle -- which is the normal case for idle, reload and draw, and the
// signal for the caller to keep whatever it was doing before.
int Slayer_Muzzle_AttachmentForSequence( const void *data, int len, int sequence );

// How many attachments the model has at all. Used to tell "this model has one
// muzzle, index 0 is the only possible answer" from "this model has several and
// the sequence did not say", which are different situations for the caller.
int Slayer_Muzzle_NumAttachments( const void *data, int len );

#ifdef __cplusplus
}
#endif

#endif // CL_MUZZLE_SLAYER_H
