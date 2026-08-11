/*
cl_slayer_conspy.c - Slayer3D console spam accounting
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

// Find out WHAT is spamming the console, by counting instead of dumping.
//
// The problem this solves: most console lines in a CS 1.6 session come from the
// client DLL or from engine paths, not from our code, so reading our sources
// cannot answer "what is spamming". Turning on the engine's -log gives a file
// with tens of thousands of lines that still has to be read by hand.
//
// So each line is reduced to a TEMPLATE (numbers, coordinates, names collapsed
// to placeholders) and counted. Two lines that differ only in a number are the
// same source line in the code, and that is exactly the grouping needed to point
// at the offender. `slayer_conspy_report` then prints the top templates sorted by
// count, with the rate per second, so a 6000-times-a-minute line cannot hide
// among a hundred one-off messages.

#include "common.h"
#include "client.h"
#include "cl_slayer_conspy.h"
#include "cl_slayer_log.h"

#define SLAYER_CONSPY_SLOTS   192   // distinct templates tracked
#define SLAYER_CONSPY_TEMPLATE 96   // chars kept per template

static CVAR_DEFINE_AUTO( slayer_conspy, "0", FCVAR_ARCHIVE,
	"Slayer3D: count console messages by template to find spam (0 = off)" );

typedef struct
{
	char     tpl[SLAYER_CONSPY_TEMPLATE];
	unsigned count;
	double   first_time;
	double   last_time;
} slayer_conspy_slot_t;

static slayer_conspy_slot_t s_slots[SLAYER_CONSPY_SLOTS];
static int      s_slot_count;
static unsigned s_total;
static unsigned s_dropped;      // lines that found no free slot
static double   s_since;
static qboolean s_in_report;    // re-entry guard: the report itself prints

// Reduce a message to a template: digit runs -> '#', anything non-printable or
// long -> cut. Colour codes are dropped so the same message with a different
// highlight is not counted twice.
//
// Deliberately crude. The goal is grouping, and a numeric placeholder is enough
// for that; a smarter tokenizer would be another thing to get wrong and would
// run on every console line.
static void Slayer_ConSpy_Template( const char *msg, char *out, size_t out_size )
{
	size_t o = 0;
	int    in_digits = 0;

	if( !out || out_size == 0 )
		return;

	while( *msg && o + 1 < out_size )
	{
		byte c = (byte)*msg;

		// Skip ^1..^9 colour codes.
		if( c == '^' && msg[1] >= '0' && msg[1] <= '9' )
		{
			msg += 2;
			continue;
		}

		if( c == '\n' || c == '\r' || c == '\t' )
		{
			msg++;
			continue;
		}

		if( c >= '0' && c <= '9' )
		{
			// Collapse a run of digits (and the decimal point inside it) so
			// "speed=412.7" and "speed=98.0" land in one bucket.
			if( !in_digits )
			{
				out[o++] = '#';
				in_digits = 1;
			}
			msg++;
			continue;
		}

		if( in_digits && ( c == '.' || c == '-' ))
		{
			msg++;
			continue;
		}

		in_digits = 0;
		out[o++] = (char)c;
		msg++;
	}

	out[o] = '\0';
}

void Slayer_ConSpy_Note( const char *msg )
{
	char tpl[SLAYER_CONSPY_TEMPLATE];
	int  i;

	if( slayer_conspy.value == 0.0f || s_in_report )
		return;
	if( COM_StringEmptyOrNULL( msg ))
		return;

	Slayer_ConSpy_Template( msg, tpl, sizeof( tpl ));
	if( tpl[0] == '\0' )
		return;

	if( s_since == 0.0 )
		s_since = host.realtime;

	s_total++;

	for( i = 0; i < s_slot_count; i++ )
	{
		if( !Q_strcmp( s_slots[i].tpl, tpl ))
		{
			s_slots[i].count++;
			s_slots[i].last_time = host.realtime;
			return;
		}
	}

	if( s_slot_count >= SLAYER_CONSPY_SLOTS )
	{
		s_dropped++;
		return;
	}

	Q_strncpy( s_slots[s_slot_count].tpl, tpl, sizeof( s_slots[s_slot_count].tpl ));
	s_slots[s_slot_count].count = 1;
	s_slots[s_slot_count].first_time = host.realtime;
	s_slots[s_slot_count].last_time = host.realtime;
	s_slot_count++;
}

static void Cmd_ConSpyReport_f( void )
{
	int    limit = 15;
	int    i, j, shown = 0;
	double span;
	int    order[SLAYER_CONSPY_SLOTS];

	if( Cmd_Argc() >= 2 )
	{
		limit = Q_atoi( Cmd_Argv( 1 ));
		if( limit < 1 ) limit = 1;
		if( limit > SLAYER_CONSPY_SLOTS ) limit = SLAYER_CONSPY_SLOTS;
	}

	if( slayer_conspy.value == 0.0f && s_total == 0 )
	{
		Con_Printf( "conspy is off. enable with: slayer_conspy 1\n" );
		Con_Printf( "then play for a while and run: slayer_conspy_report\n" );
		return;
	}

	if( s_slot_count == 0 )
	{
		Con_Printf( "conspy: nothing recorded yet\n" );
		return;
	}

	span = host.realtime - s_since;
	if( span < 0.01 ) span = 0.01;

	// Selection sort by count: at most 192 entries and it runs once on demand,
	// so there is nothing to gain from anything cleverer.
	for( i = 0; i < s_slot_count; i++ )
		order[i] = i;
	for( i = 0; i < s_slot_count; i++ )
	{
		for( j = i + 1; j < s_slot_count; j++ )
		{
			if( s_slots[order[j]].count > s_slots[order[i]].count )
			{
				int t = order[i];

				order[i] = order[j];
				order[j] = t;
			}
		}
	}

	// The report itself goes through Con_Printf, so suppress accounting while it
	// prints -- otherwise the report becomes the top entry of the next report.
	s_in_report = true;

	Con_Printf( "=== console spam report: %u lines in %.0f s (%.1f/s), %d templates%s ===\n",
		s_total, span, (double)s_total / span, s_slot_count,
		s_dropped ? va( ", %u dropped", s_dropped ) : "" );

	for( i = 0; i < s_slot_count && shown < limit; i++, shown++ )
	{
		const slayer_conspy_slot_t *sl = &s_slots[order[i]];
		double life = sl->last_time - sl->first_time;

		if( life < 0.01 ) life = 0.01;

		Con_Printf( "%6u  %5.1f/s  %s\n",
			sl->count, (double)sl->count / life, sl->tpl );
	}

	// Also into the file log, because the in-game console scrolls and a phone is
	// a bad place to read 15 lines of table.
	Slayer_Log_Printf( "conspy: %u lines in %.0f s (%.1f/s), %d templates",
		s_total, span, (double)s_total / span, s_slot_count );
	for( i = 0; i < s_slot_count && i < limit; i++ )
	{
		const slayer_conspy_slot_t *sl = &s_slots[order[i]];

		Slayer_Log_Printf( "conspy: %6u  %s", sl->count, sl->tpl );
	}

	s_in_report = false;
}

static void Cmd_ConSpyReset_f( void )
{
	memset( s_slots, 0, sizeof( s_slots ));
	s_slot_count = 0;
	s_total = 0;
	s_dropped = 0;
	s_since = 0.0;
	Con_Printf( "conspy: counters cleared\n" );
}

void Slayer_ConSpy_Init( void )
{
	Cvar_RegisterVariable( &slayer_conspy );

	Cmd_AddCommand( "slayer_conspy_report", Cmd_ConSpyReport_f,
		"Slayer3D: print the most frequent console messages (optional line limit)" );
	Cmd_AddCommand( "slayer_conspy_reset", Cmd_ConSpyReset_f,
		"Slayer3D: clear the console spam counters" );
}
