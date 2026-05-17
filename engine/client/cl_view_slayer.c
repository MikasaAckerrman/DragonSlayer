/*
cl_view_slayer.c - Slayer3D extension: kill-sound feedback

Plays a configurable sound effect on the local client whenever the
player dies. Three independent sound paths are supported and chosen
by priority:  teamkill > headshot > generic.

Hooks into:
- CL_ParseUserMessage  (engine/client/parse/cl_parse.c)  - DeathMsg / TeamInfo
- CL_InitLocal         (engine/client/cl_main.c)         - cvar registration
- CL_ClearState        (engine/client/cl_main.c)         - wipe team table
*/

#include "common.h"
#include "client.h"
#include "cl_view_slayer.h"
#include "xash3d_mathlib.h"   // bound()

// ---------------------------------------------------------------------------
// Cvars (all archived).
//
// Empty string disables a particular variant. If the more specific cvar
// (headshot / teamkill) is empty but the generic one is set, the generic
// sound is used as a fallback. This way users get one knob to turn the
// whole feature off (slayer_killsound "") without losing per-event tuning.
// ---------------------------------------------------------------------------
static CVAR_DEFINE_AUTO( slayer_killsound,          "", FCVAR_ARCHIVE, "sound played when local player dies (path under <mod>/sound/, empty = disabled)" );
static CVAR_DEFINE_AUTO( slayer_killsound_headshot, "", FCVAR_ARCHIVE, "sound played on headshot death (CS-only). Empty = use slayer_killsound" );
static CVAR_DEFINE_AUTO( slayer_killsound_teamkill, "", FCVAR_ARCHIVE, "sound played when killed by a teammate. Empty = use slayer_killsound" );
static CVAR_DEFINE_AUTO( slayer_killsound_volume,   "1.0", FCVAR_ARCHIVE, "kill-sound volume, 0..1" );

// ---------------------------------------------------------------------------
// Per-match team table.
//
// Slot 0 is intentionally unused: DeathMsg uses 1-based player indices,
// and a killer index of 0 means "world" (fall, trigger_hurt, worldspawn).
// MAX_TEAM_NAME 32 covers vanilla CS ("TERRORIST", "CT", "SPECTATOR",
// "UNASSIGNED") plus longer names from custom mods.
// ---------------------------------------------------------------------------
#define MAX_TEAM_NAME 32

static char slayer_player_team[MAX_CLIENTS + 1][MAX_TEAM_NAME];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Returns true when the current mod is a Counter-Strike-derived game and
// therefore sends DeathMsg in the {killer, victim, headshot, "weapon"} form.
// Other mods (HL/DM, TFC, DoD, ...) use {killer, victim, "weapon"} and
// must not be parsed for the headshot byte.
static qboolean Slayer_GameIsCStrike( void )
{
	const char *g;

	if( !GI )
		return false;

	g = GI->gamefolder;
	return !Q_stricmp( g, "cstrike" )
	    || !Q_stricmp( g, "czero" )
	    || !Q_stricmp( g, "czeror" );
}

// Returns true if both teams are known (non-empty) and equal,
// case-insensitively. SPECTATOR / UNASSIGNED count as "no team" - a
// just-connected player has nothing in their slot, and we must not flag
// that as a teamkill.
static qboolean Slayer_SameTeam( int killer, int victim )
{
	const char *kt, *vt;

	if( killer < 1 || killer > MAX_CLIENTS )
		return false;
	if( victim < 1 || victim > MAX_CLIENTS )
		return false;

	kt = slayer_player_team[killer];
	vt = slayer_player_team[victim];

	if( !kt[0] || !vt[0] )
		return false;

	if( !Q_stricmp( kt, "SPECTATOR" ) || !Q_stricmp( kt, "UNASSIGNED" ))
		return false;
	if( !Q_stricmp( vt, "SPECTATOR" ) || !Q_stricmp( vt, "UNASSIGNED" ))
		return false;

	return !Q_stricmp( kt, vt );
}

// Pick the most specific configured sound, falling back to the generic
// one. NULL means "do not play anything".
static const char *Slayer_PickKillSound( qboolean headshot, qboolean teamkill )
{
	if( teamkill && slayer_killsound_teamkill.string[0] )
		return slayer_killsound_teamkill.string;

	if( headshot && slayer_killsound_headshot.string[0] )
		return slayer_killsound_headshot.string;

	if( slayer_killsound.string[0] )
		return slayer_killsound.string;

	return NULL;
}

static void Slayer_PlayKillSound( const char *name )
{
	float vol;

	if( !name || !name[0] )
		return;

	vol = bound( 0.0f, slayer_killsound_volume.value, 1.0f );
	if( vol <= 0.0f )
		return;

	// reliable=true -> CHAN_STATIC: not evicted from the dynamic mixer
	// when many other local sounds are playing.
	S_StartLocalSound( name, vol, true );
}

// ---------------------------------------------------------------------------
// User-message hooks
// ---------------------------------------------------------------------------

void Slayer_OnTeamInfo( const byte *pbuf, int iSize )
{
	int  slot;
	int  i;
	char team[MAX_TEAM_NAME];

	// TeamInfo payload: { byte slot, char team_name[..] '\0' }.
	if( iSize < 2 )
		return;

	slot = pbuf[0];
	if( slot < 1 || slot > MAX_CLIENTS )
		return;

	// Copy the NUL-terminated team string defensively: the network buffer
	// is not guaranteed to contain a terminator within iSize.
	for( i = 0; i < (int)sizeof( team ) - 1 && (i + 1) < iSize; i++ )
	{
		char c = (char)pbuf[i + 1];
		if( !c )
			break;
		team[i] = c;
	}
	team[i] = '\0';

	Q_strncpy( slayer_player_team[slot], team, sizeof( slayer_player_team[slot] ));
}

qboolean Slayer_OnDeathMsg( const byte *pbuf, int iSize )
{
	int      killer, victim;
	qboolean headshot = false;
	qboolean teamkill = false;
	const    char *snd;

	// DeathMsg payload (HL/DM): { byte killer, byte victim, char weapon[] }.
	// DeathMsg payload (CS):    { byte killer, byte victim, byte headshot, char weapon[] }.
	if( iSize < 2 )
		return false;

	killer = pbuf[0];
	victim = pbuf[1];

	// Engine assigns each client an entity index of (cl.playernum + 1).
	if( victim != cl.playernum + 1 )
		return false;

	// Headshot byte is CS-specific. Validate twice: by gamefolder, and by
	// value range (0/1) - a stray non-CS mod that happens to send a
	// 4-byte header would otherwise misfire.
	if( iSize >= 3 && Slayer_GameIsCStrike() )
	{
		byte b = pbuf[2];
		if( b == 0 || b == 1 )
			headshot = ( b == 1 );
	}

	// Teamkill detection. World kills (killer == 0) and suicides
	// (killer == victim) are explicitly NOT teamkills.
	if( killer != 0 && killer != victim )
		teamkill = Slayer_SameTeam( killer, victim );

	snd = Slayer_PickKillSound( headshot, teamkill );
	Slayer_PlayKillSound( snd );

	return false; // never consume the message - client.dll still needs it
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void Slayer_Init( void )
{
	Cvar_RegisterVariable( &slayer_killsound );
	Cvar_RegisterVariable( &slayer_killsound_headshot );
	Cvar_RegisterVariable( &slayer_killsound_teamkill );
	Cvar_RegisterVariable( &slayer_killsound_volume );

	Slayer_ResetMatchState();
}

void Slayer_ResetMatchState( void )
{
	memset( slayer_player_team, 0, sizeof( slayer_player_team ));
}
