/*
cl_steam_ticket_slayer.h - a real Steam auth ticket for the GoldSrc connect packet
Copyright (C) 2026 Slayer3D

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

WHAT THIS IS FOR

A GoldSrc server learns who you are from the ticket in the connect packet. Every
build so far has sent a FABRICATED one (GenerateRevEmu2013), so the SteamID the
server announces to everyone else is a hash of this device -- measured on the
reporting device: account id 2995798024, above 2^31, which no real Steam account
can be. Other players therefore cannot look you up, and your avatar cannot appear
for them.

The launcher already holds a logged-on Steam session (the one that sets the
"playing Counter-Strike" status). Steam will issue an app-ownership ticket over
that same session -- no Steam client, no PC -- and THAT ticket names the real
account. This file is the bridge to it.

WHY IT IS OPT-IN

A server running a Steam emulator may reject a genuine ticket, and a rejected
ticket means not joining at all. That is worse than an unrecognised avatar, so
cl_ticket_generator keeps its old default and the real ticket is one explicit
choice away. Everything here falls back to the emulated ticket rather than
failing a connect.
*/

#ifndef CL_STEAM_TICKET_SLAYER_H
#define CL_STEAM_TICKET_SLAYER_H

#include "xash3d_types.h"

// Bind the JNI methods. Safe to call when there is no launcher (returns quietly),
// and safe to call more than once.
void Slayer_SteamTicket_Init( void );

/*
Fetch a real app-ownership ticket for CS 1.6.

BLOCKS while Steam answers -- the connect packet cannot be built without it, and
this runs on the connect path, not in the frame loop. `timeout_ms` bounds the wait.

out_steamid receives the SteamID64 the ticket belongs to; it comes from the
session that obtained the ticket rather than from stored settings, because a
ticket only proves the account that was logged on when Steam issued it, and
announcing one account while proving another is how a server ends up refusing us.

Returns the number of bytes written to `buf`, or 0 when no real ticket is
available (no credentials, the account does not own the game, Steam refused, no
launcher). 0 is the ordinary case, not an error: the caller then uses the
emulated ticket, exactly as before.
*/
int Slayer_SteamTicket_Fetch( byte *buf, int buf_size, int timeout_ms,
	uint64_t *out_steamid, uint32_t server_ip, int server_port );

#endif // CL_STEAM_TICKET_SLAYER_H
