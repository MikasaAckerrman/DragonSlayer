/*
android_lib.c - dynamic library code for Android OS
Copyright (C) 2018 Flying With Gauss

This program is free software: you can redistribute it and/sor modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include <dlfcn.h>
#include "common.h"
#include "library.h"
#include "filesystem.h"
#include "server.h"
#include "platform/android/lib_android.h"
#include "platform/android/dlsym-weak.h" // Android < 5.0

void *ANDROID_LoadLibrary( const char *path )
{
	const char *name = COM_FileWithoutPath( path );
	void *handle;
	// DragonSlayer: also mirror diagnostic info to menu_loader.log when
	// the requested library is the menu so the user can audit which
	// of (XASH3D_GAMELIBDIR | VFS | LD_LIBRARY_PATH) actually wins.
	const qboolean is_menu = ( Q_stristr( name, "menu" ) != NULL );

	Con_Reportf( "%s: loading \"%s\" (name: \"%s\")\n", __func__, path, name );
	if( is_menu )
		Slayer_MenuLoaderLog( "[android] ANDROID_LoadLibrary path=\"%s\" name=\"%s\"",
			path, name );

	// TODO: remove this once distributing games from APKs will be deprecated
	const char *gamelibdir = getenv( "XASH3D_GAMELIBDIR" );
	if( !COM_StringEmptyOrNULL( gamelibdir ))
	{
		char fullpath[MAX_SYSPATH];
		Q_snprintf( fullpath, sizeof( fullpath ), "%s/%s", gamelibdir, name );

		Con_Reportf( "%s: trying APK path \"%s\"\n", __func__, fullpath );
		if( is_menu )
			Slayer_MenuLoaderLog( "[android] step 1/3 XASH3D_GAMELIBDIR -> dlopen(\"%s\")",
				fullpath );

		handle = dlopen( fullpath, RTLD_NOW );
		if( handle )
		{
			Con_Reportf( "%s: loaded from APK path\n", __func__ );
			if( is_menu )
				Slayer_MenuLoaderLog( "[android] WIN: XASH3D_GAMELIBDIR loaded \"%s\"",
					fullpath );
			return handle;
		}

		Con_Reportf( "%s: APK path failed: %s\n", __func__, dlerror() );
		if( is_menu )
			Slayer_MenuLoaderLog( "[android] step 1/3 failed: %s", dlerror() );
		COM_PushLibraryError( dlerror() );
	}
	else if( is_menu )
	{
		Slayer_MenuLoaderLog( "[android] step 1/3 skipped (XASH3D_GAMELIBDIR not set)" );
	}

	// try VFS
	dll_user_t *hInst = FS_FindLibrary( path, false );
	if( hInst )
	{
		Con_Reportf( "%s: VFS found \"%s\"\n", __func__, hInst->fullPath );
		if( is_menu )
			Slayer_MenuLoaderLog( "[android] step 2/3 VFS found \"%s\" (custom_loader=%d)",
				hInst->fullPath, hInst->custom_loader );
		if( !hInst->custom_loader )
		{
			char libpath[MAX_SYSPATH];
			Q_strncpy( libpath, hInst->fullPath, sizeof( libpath ));
			Mem_Free( hInst );

			handle = dlopen( libpath, RTLD_NOW );
			if( handle )
			{
				Con_Reportf( "%s: loaded from VFS path\n", __func__ );
				if( is_menu )
					Slayer_MenuLoaderLog( "[android] WIN: VFS loaded \"%s\" (this is the user's data-folder menu.so, NOT the embedded one)",
						libpath );
				return handle;
			}
			Con_Reportf( "%s: VFS dlopen failed: %s\n", __func__, dlerror() );
			if( is_menu )
				Slayer_MenuLoaderLog( "[android] step 2/3 VFS dlopen failed: %s",
					dlerror() );
			COM_PushLibraryError( dlerror() );
		}
		else
		{
			Con_Reportf( "%s: VFS entry has custom loader, skipping\n", __func__ );
			if( is_menu )
				Slayer_MenuLoaderLog( "[android] step 2/3 VFS hit but has custom loader, skipped" );
			COM_PushLibraryError( "custom loader not available on Android" );
			Mem_Free( hInst );
		}
	}
	else if( is_menu )
	{
		Slayer_MenuLoaderLog( "[android] step 2/3 VFS lookup miss for \"%s\"", path );
	}

	// find in system search path (APK's LD_LIBRARY_PATH)
	Con_Reportf( "%s: trying LD_LIBRARY_PATH for \"%s\"\n", __func__, name );
	if( is_menu )
		Slayer_MenuLoaderLog( "[android] step 3/3 dlopen(\"%s\") via LD_LIBRARY_PATH (APK lib/<abi>/)",
			name );
	handle = dlopen( name, RTLD_NOW );
	if( handle )
	{
		Con_Reportf( "%s: loaded from LD_LIBRARY_PATH\n", __func__ );
		if( is_menu )
			Slayer_MenuLoaderLog( "[android] WIN: LD_LIBRARY_PATH loaded \"%s\" (this IS the embedded reskinned mainui)",
				name );
		return handle;
	}

	Con_Reportf( "%s: all paths failed for \"%s\"\n", __func__, path );
	if( is_menu )
		Slayer_MenuLoaderLog( "[android] FAIL: all 3 sources exhausted for \"%s\": %s",
			path, dlerror() );
	COM_PushLibraryError( dlerror() );

	return NULL;
}

void *ANDROID_GetProcAddress( void *hInstance, const char *name )
{
	void *p = dlsym( hInstance, name );

#ifndef XASH_64BIT
	if( p ) return p;

	p = dlsym_weak( hInstance, name );
#endif // XASH_64BIT

	return p;
}
