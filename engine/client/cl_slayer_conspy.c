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

// ON by default. The counting cost is one template pass and at most 192 string
// compares per console line, which is nothing next to the printing itself -- and
// leaving it off meant that every report of "спам в консоли" arrived with no
// evidence in the log, so the answer had to be guessed from source reading. With
// this on, the log the player sends already names the offender.
static CVAR_DEFINE_AUTO( slayer_conspy, "1", FCVAR_ARCHIVE,
	"Slayer3D: count console messages by template to find spam (0 = off)" );

// Migration for configs archived while the default was 0. FCVAR_ARCHIVE means a
// new default reaches nobody who has already played -- the same trap as the
// scoreboard cvars. Turn it on once; an explicit later 0 is preserved.
static CVAR_DEFINE_AUTO( slayer_conspy_migrated, "0", FCVAR_ARCHIVE,
	"Slayer3D internal: conspy default migration completed" );

// =============================================================================
// Console transcript
// =============================================================================
//
// "нужно уже точно убрать с корнем спам в консоль, поэтому нужно добавить
// логирование консоли."
//
// The engine can already do this with -log, but that is a command-line switch,
// and on Android there is no command line the player can edit. So the transcript
// is a cvar, and it writes to <gamedir>/logs/slayer_console.log next to the two
// logs that are already there.
//
// THE POINT OF WRITING IT HERE, in the filter: a muted line is written to the
// file BEFORE the mute decision is applied. Hiding a line from the screen must
// not hide it from the diagnosis -- otherwise turning the noise off destroys the
// only evidence of what the noise was.
static CVAR_DEFINE_AUTO( slayer_conlog, "1", FCVAR_ARCHIVE,
	"Slayer3D: write every console line to <gamedir>/logs/slayer_console.log (0 = off)" );

// 4 MB, then it stops. A rotation would need a second file and a rename inside a
// path that runs for every console line; a hard stop with a final line saying so
// is honest and cannot eat the player's storage.
#define SLAYER_CONLOG_MAX_BYTES  ( 4 * 1024 * 1024 )

static file_t  *s_conlog_file;
static qboolean s_conlog_failed;      // open failed once: do not retry every line
static qboolean s_conlog_full;        // hit the size cap
static size_t   s_conlog_written;
static double   s_conlog_flush_time;
static qboolean s_in_conlog;          // re-entry guard: FS_ can print on error
static qboolean s_conlog_last_ended_line = true;  // next write starts a line

// Periodic report into the file log, so the next log a player sends already
// contains the spam table without them having to type a command.
static double   s_next_auto_report;

// =============================================================================
// Muting
// =============================================================================
//
// Counting found the spam; this is what removes it. The filter lives at the one
// choke point every console line passes through (Sys_Print) precisely because
// the noisy lines come from the game DLL and from engine paths that have no call
// site we could edit -- `Firing: (game_playerdie)` is the game's own developer
// output, and the `status` table is printed by the server command we issue
// ourselves.
//
// Two independent mechanisms, because the two problems are different in kind:
//
//   1. OUR OWN automated `status` requests. We know exactly when we asked, so
//      the reply can be swallowed for the duration of the parse window. A
//      `status` the player typed is NOT suppressed -- they asked to see it.
//      This is a window, not a pattern: it cannot mute anything the player did.
//
//   2. A user-editable substring list for everything else, empty by default.
//      Substrings rather than patterns: a regex engine on every console line
//      would be both slower and another thing to get wrong.
//
// Everything dropped is counted, and the counter is printed by the report, so
// the muting can never be silently hiding something that matters.

static CVAR_DEFINE_AUTO( slayer_console_quiet_status, "1", FCVAR_ARCHIVE,
	"Slayer3D: hide the reply to status requests the client makes by itself (0 = show)" );

static CVAR_DEFINE_AUTO( slayer_console_mute, "", FCVAR_ARCHIVE,
	"Slayer3D: semicolon-separated substrings to hide from the console" );

// Set by the scoreboard when it issues an automated `status`; while this is in
// the future, a line that looks like part of a status reply is dropped.
static double   s_quiet_status_until;
static unsigned s_muted_auto;      // lines dropped by the status window
static unsigned s_muted_user;      // lines dropped by the substring list

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

// =============================================================================
// Muting
// =============================================================================

void Slayer_ConSpy_QuietStatus( double seconds )
{
	if( seconds <= 0.0 )
	{
		s_quiet_status_until = 0.0;
		return;
	}
	s_quiet_status_until = host.realtime + seconds;
}

// Is this line part of a `status` reply?
//
// Matched by SHAPE, not by content, because the table is printed by the server
// and its exact columns differ between engines and mods.
//
// WIDENED after reading a real reply from the device transcript. The old version
// caught only the '#' rows and a "map: " prefix, and MEASURED against the actual
// server output that left five lines per reply on screen -- at 24 replies in 132
// seconds that is 120 visible lines, the bulk of the reported spam:
//
//   hostname:  .::Наши Люди 16+::. [Public+night VIP] ©
//   version :  48/1.1.2.7/Stdio 3937 secure  (10)
//   tcp/ip  :  62.122.215.127:27015
//   map     :  de_kabul at: 0 x, 0 y, 0 z
//   players :  20 active (32 max)
//   #      name userid uniqueid frag time ping loss adr     <- was caught
//   # 1 "SHYMKENT" 812 STEAM_5:0:12921341   4 02:52  116    <- was caught
//   20 users
//
// Note "map     :" -- the columns are space-padded to a fixed width, so a
// "map: " prefix test never matched. That is why the old rule looked right and
// did nothing.
static qboolean Slayer_ConSpy_LooksLikeStatus( const char *msg )
{
	static const char *headers[] = { "hostname", "version", "tcp/ip", "map",
	                                 "players", "edicts", NULL };
	const char *p = msg;
	int         i;

	while( *p == ' ' || *p == '\t' )
		p++;

	// "<keyword><spaces>:" -- the header block. Keyed on a known keyword rather
	// than "anything with a colon", or chat like "lol: ok" would vanish.
	for( i = 0; headers[i]; i++ )
	{
		size_t n = Q_strlen( headers[i] );

		if( !Q_strnicmp( p, headers[i], n ))
		{
			const char *q = p + n;

			while( *q == ' ' || *q == '\t' )
				q++;
			if( *q == ':' )
				return true;
		}
	}

	// "20 users" / "1 user" -- the trailer.
	if( *p >= '0' && *p <= '9' )
	{
		const char *q = p;

		while( *q >= '0' && *q <= '9' )
			q++;
		while( *q == ' ' || *q == '\t' )
			q++;
		if( !Q_strnicmp( q, "user", 4 ))
			return true;
	}

	if( *p != '#' )
		return false;
	p++;

	// "#" alone, "# score ping...", "#3 ...", "#  3 ..." -- all status shapes.
	if( *p == '\0' || *p == '\n' || *p == ' ' || *p == '\t'
	 || ( *p >= '0' && *p <= '9' ))
		return true;

	return false;
}

static qboolean Slayer_ConSpy_UserMuted( const char *msg )
{
	const char *list = slayer_console_mute.string;
	const char *p;

	if( COM_StringEmptyOrNULL( list ))
		return false;

	p = list;
	while( *p )
	{
		char        needle[64];
		size_t      n = 0;

		while( *p == ';' || *p == ' ' )
			p++;

		while( *p && *p != ';' && n < sizeof( needle ) - 1 )
			needle[n++] = *p++;

		// Trailing spaces would never match anything, and a list is usually
		// written with them ("a; b; c").
		while( n > 0 && needle[n - 1] == ' ' )
			n--;
		needle[n] = '\0';

		if( n > 0 && Q_stristr( msg, needle ))
			return true;
	}

	return false;
}

// =============================================================================
// Console transcript writing
// =============================================================================

/*
====================
Slayer_ConLog_Write

Append one console line to the transcript file.

Called for EVERY console line, including the ones about to be muted, and
including engine output from before the game is running. Three guards matter:

  * s_in_conlog -- FS_Open and FS_Printf can themselves print on failure, and
    that print comes straight back here. Without the guard the first write error
    is infinite recursion.
  * s_conlog_failed -- a failed open must not be retried on every line; on a
    device with no write permission that would be a filesystem call per message.
  * s_conlog_full -- the cap. Stops, says so once, and keeps counting nothing.
====================
*/
static void Slayer_ConLog_Write( const char *msg )
{
	size_t len;

	if( slayer_conlog.value == 0.0f || s_conlog_failed || s_conlog_full )
		return;
	if( s_in_conlog || COM_StringEmptyOrNULL( msg ))
		return;

	s_in_conlog = true;

	if( !s_conlog_file )
	{
		// "w", not "a": a transcript spanning sessions cannot be read, and the
		// interesting session is always the one that just happened. The two other
		// Slayer logs append because they are sparse; this one is not.
		s_conlog_file = FS_Open( "logs/slayer_console.log", "w", true );
		if( !s_conlog_file )
		{
			s_conlog_failed = true;
			s_in_conlog = false;
			return;
		}
		s_conlog_written = 0;
	}

	len = Q_strlen( msg );

	if( s_conlog_written + len > SLAYER_CONLOG_MAX_BYTES )
	{
		FS_Printf( s_conlog_file, "\n[%8.2f] --- console log full (%d MB), stopping ---\n",
			host.realtime, SLAYER_CONLOG_MAX_BYTES / ( 1024 * 1024 ));
		FS_Flush( s_conlog_file );
		s_conlog_full = true;
		s_in_conlog = false;
		return;
	}

	// Timestamp only at the start of a line. Con_Printf output arrives in
	// fragments -- "Slayer3D: " and "avatar ready\n" can be two calls -- and
	// stamping each fragment would break every line into pieces.
	if( s_conlog_written == 0 || s_conlog_last_ended_line )
		FS_Printf( s_conlog_file, "[%8.2f] ", host.realtime );

	FS_Print( s_conlog_file, msg );
	s_conlog_written += len;
	s_conlog_last_ended_line = ( len > 0 && msg[len - 1] == '\n' );

	// Flushing every line would be a write syscall per console message; a crash
	// loses at most a quarter second of transcript, and the interesting lines are
	// never the last ones before a crash -- they are the repeating ones.
	if( host.realtime - s_conlog_flush_time > 0.25 )
	{
		FS_Flush( s_conlog_file );
		s_conlog_flush_time = host.realtime;
	}

	s_in_conlog = false;
}

qboolean Slayer_ConSpy_Filter( const char *msg )
{
	// Account first: the report must show what was dropped, otherwise muting
	// turns into a way to hide a growing problem from ourselves.
	Slayer_ConSpy_Note( msg );

	// The transcript is written BEFORE the mute decision, on purpose: a line
	// hidden from the screen must still be in the file, or turning the noise off
	// would destroy the evidence of what the noise was.
	Slayer_ConLog_Write( msg );

	if( COM_StringEmptyOrNULL( msg ))
		return true;

	// Never mute while the report is printing, or the report mutes itself.
	if( s_in_report )
		return true;

	if( slayer_console_quiet_status.value != 0.0f
	 && s_quiet_status_until > 0.0
	 && host.realtime <= s_quiet_status_until
	 && Slayer_ConSpy_LooksLikeStatus( msg ))
	{
		s_muted_auto++;
		return false;
	}

	if( Slayer_ConSpy_UserMuted( msg ))
	{
		s_muted_user++;
		return false;
	}

	return true;
}

/*
====================
Slayer_ConSpy_SortTop

Fill `order` with slot indices sorted by count, descending. Returns the count.

Shared by the command and the periodic file dump so the two can never disagree
about what "the top offenders" means. Selection sort: at most 192 entries, run at
most once every three minutes.
====================
*/
static int Slayer_ConSpy_SortTop( int *order )
{
	int i, j;

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

	return s_slot_count;
}

/*
====================
Slayer_ConSpy_LogTop

Write the spam table to the FILE log only, never the console.

Console output here would be spam about spam, and would then appear in its own
next report. The file is also the only place a phone user can actually read a
15-line table.
====================
*/
static void Slayer_ConSpy_LogTop( int limit )
{
	int    order[SLAYER_CONSPY_SLOTS];
	int    i, n;
	double span;

	if( s_slot_count == 0 )
		return;

	span = host.realtime - s_since;
	if( span < 0.01 ) span = 0.01;

	n = Slayer_ConSpy_SortTop( order );

	Slayer_Log_Printf( "conspy: %u lines in %.0f s (%.1f/s), %d templates%s",
		s_total, span, (double)s_total / span, s_slot_count,
		s_dropped ? va( ", %u dropped", s_dropped ) : "" );

	if( s_muted_auto || s_muted_user )
	{
		Slayer_Log_Printf( "conspy: muted %u status line(s), %u by slayer_console_mute",
			s_muted_auto, s_muted_user );
	}

	for( i = 0; i < n && i < limit; i++ )
	{
		const slayer_conspy_slot_t *sl = &s_slots[order[i]];
		double life = sl->last_time - sl->first_time;

		if( life < 0.01 ) life = 0.01;

		Slayer_Log_Printf( "conspy: %6u  %5.1f/s  %s",
			sl->count, (double)sl->count / life, sl->tpl );
	}
}

static void Cmd_ConSpyReport_f( void )
{
	int    limit = 15;
	int    i, shown = 0;
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

	Slayer_ConSpy_SortTop( order );

	// The report itself goes through Con_Printf, so suppress accounting while it
	// prints -- otherwise the report becomes the top entry of the next report.
	s_in_report = true;

	Con_Printf( "=== console spam report: %u lines in %.0f s (%.1f/s), %d templates%s ===\n",
		s_total, span, (double)s_total / span, s_slot_count,
		s_dropped ? va( ", %u dropped", s_dropped ) : "" );

	// Muted counts belong in the report: hiding lines without saying how many
	// were hidden would make this tool a way to fool ourselves.
	if( s_muted_auto || s_muted_user )
	{
		Con_Printf( "    muted: %u status-reply line(s), %u by slayer_console_mute\n",
			s_muted_auto, s_muted_user );
	}

	for( i = 0; i < s_slot_count && shown < limit; i++, shown++ )
	{
		const slayer_conspy_slot_t *sl = &s_slots[order[i]];
		double life = sl->last_time - sl->first_time;

		if( life < 0.01 ) life = 0.01;

		Con_Printf( "%6u  %5.1f/s  %s\n",
			sl->count, (double)sl->count / life, sl->tpl );
	}

	s_in_report = false;

	// Also into the file log, because the in-game console scrolls and a phone is
	// a bad place to read a 15-line table. Same helper as the periodic dump, so
	// the two views cannot disagree.
	Slayer_ConSpy_LogTop( limit );
}

static void Cmd_ConSpyReset_f( void )
{
	memset( s_slots, 0, sizeof( s_slots ));
	s_slot_count = 0;
	s_total = 0;
	s_dropped = 0;
	s_muted_auto = 0;
	s_muted_user = 0;
	s_since = 0.0;
	s_next_auto_report = 0.0;   // restart the periodic dump from now
	Con_Printf( "conspy: counters cleared\n" );
}

void Slayer_ConSpy_Init( void )
{
	Cvar_RegisterVariable( &slayer_conspy );
	Cvar_RegisterVariable( &slayer_conspy_migrated );
	Cvar_RegisterVariable( &slayer_conlog );
	Cvar_RegisterVariable( &slayer_console_quiet_status );
	Cvar_RegisterVariable( &slayer_console_mute );

	// Counting shipped off, so an archived config still says 0 and the new
	// default would reach nobody who has already played -- which is exactly the
	// player reporting the spam. Turn it on once; an explicit later 0 is kept.
	if( slayer_conspy_migrated.value == 0.0f )
	{
		if( slayer_conspy.value == 0.0f )
			Cvar_SetValue( "slayer_conspy", 1.0f );
		Cvar_SetValue( "slayer_conspy_migrated", 1.0f );
	}

	Cmd_AddCommand( "slayer_conspy_report", Cmd_ConSpyReport_f,
		"Slayer3D: print the most frequent console messages (optional line limit)" );
	Cmd_AddCommand( "slayer_conspy_reset", Cmd_ConSpyReset_f,
		"Slayer3D: clear the console spam counters" );
}

/*
====================
Slayer_ConSpy_Frame

Write the spam table into the file log every few minutes.

WHY AUTOMATIC: the report exists already, but it only happens if the player types
a command, and the reports that arrive here come as "спам в консоли" with a log
that has no table in it. A periodic dump means the next log already answers the
question. Every 180 s, and only when something was actually counted.
====================
*/
void Slayer_ConSpy_Frame( void )
{
	if( slayer_conspy.value == 0.0f )
		return;

	if( s_next_auto_report == 0.0 )
	{
		s_next_auto_report = host.realtime + 180.0;
		return;
	}

	if( host.realtime < s_next_auto_report )
		return;

	s_next_auto_report = host.realtime + 180.0;

	if( s_total == 0 )
		return;

	// File only, never the console: a report printed to the console would be
	// spam about spam, and it would land in its own next report.
	Slayer_ConSpy_LogTop( 12 );
}
