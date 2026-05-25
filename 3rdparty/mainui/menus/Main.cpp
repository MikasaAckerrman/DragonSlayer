/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "Window.h"
#include "WindowStyle.h"
#include "Action.h"
#include "PicButton.h"
#include "YesNoMessageBox.h"
#include "keydefs.h"
#include "MenuStrings.h"
#include "gameinfo.h"

// Helper: apply the standard olive-stroke style to a menu button
static void ApplyMenuButtonStyle( CMenuPicButton &btn )
{
	btn.bDrawStroke = true;
	btn.colorStroke = WndStyle::BorderColor;
	btn.iStrokeWidth = 1;
}

class CMenuMainWindow : public CMenuWindow
{
public:
	CMenuMainWindow() : CMenuWindow( "Counter-Strike 1.6" ) { }

	bool IsRoot() const override { return true; }
	void Draw() override;
	bool KeyDown( int key ) override;

private:
	void _Init() override;
	void _VidInit() override;
	void Think() override;

	void VidInit( bool connected );

	void QuitDialogCb();
	void DisconnectCb();
	void DisconnectDialogCb();
	void HazardCourseDialogCb();
	void HazardCourseCb();

	CMenuPicButton	console;
	CMenuPicButton	resumeGame;
	CMenuPicButton	disconnect;
	CMenuPicButton	newGame;
	CMenuPicButton	hazardCourse;
	CMenuPicButton	configuration;
	CMenuPicButton	saveRestore;
	CMenuPicButton	multiPlayer;
	CMenuPicButton	customGame;
	CMenuPicButton	previews;
	CMenuPicButton	quit;

	// quit dialog
	CMenuYesNoMessageBox dialog;

	bool bTrainMap;
	bool bCustomGame;
};

void CMenuMainWindow::QuitDialogCb()
{
	if( CL_IsActive() && EngFuncs::GetCvarFloat( "host_serverstate" ) && EngFuncs::GetCvarFloat( "maxplayers" ) == 1.0f )
		dialog.SetMessage( L( "StringsList_235" ) );
	else
		dialog.SetMessage( L( "GameUI_QuitConfirmationText" ) );

	dialog.onPositive.SetCommand( false, "quit\n" );
	dialog.Show();
}

void CMenuMainWindow::DisconnectCb()
{
	EngFuncs::ClientCmd( false, "disconnect\n" );
	VidInit( false );
	CalcPosition();
	CalcSizes();
	VidInitItems();
}

void CMenuMainWindow::DisconnectDialogCb()
{
	dialog.onPositive = VoidCb( &CMenuMainWindow::DisconnectCb );
	dialog.SetMessage( L( "Really disconnect?" ) );
	dialog.Show();
}

void CMenuMainWindow::HazardCourseDialogCb()
{
	dialog.onPositive = VoidCb( &CMenuMainWindow::HazardCourseCb );
	dialog.SetMessage( L( "StringsList_234" ) );
	dialog.Show();
}

/*
=================
CMenuMainWindow::Draw
Skip chrome (title bar, border, drag/resize) for the root window.
The root fills the entire screen -- chrome is invisible and would
only waste draw calls and allow accidental drag/resize.
=================
*/
void CMenuMainWindow::Draw()
{
	CMenuItemsHolder::Draw();
}

/*
=================
CMenuMainWindow::KeyDown
=================
*/
bool CMenuMainWindow::KeyDown( int key )
{
	if( UI::Key::IsEscape( key ) )
	{
		if( CL_IsActive() )
		{
			if( !dialog.IsVisible() )
				UI_CloseMenu();
		}
		else
		{
			QuitDialogCb();
		}
		return true;
	}
	return CMenuBaseWindow::KeyDown( key );
}

/*
=================
CMenuMainWindow::HazardCourseCb
=================
*/
void CMenuMainWindow::HazardCourseCb()
{
	if( EngFuncs::GetCvarFloat( "host_serverstate" ) && EngFuncs::GetCvarFloat( "maxplayers" ) > 1 )
		EngFuncs::HostEndGame( "end of the game" );

	EngFuncs::CvarSetValue( "skill", 1.0f );
	EngFuncs::CvarSetValue( "deathmatch", 0.0f );
	EngFuncs::CvarSetValue( "teamplay", 0.0f );
	EngFuncs::CvarSetValue( "pausable", 1.0f ); // singleplayer is always allowing pause
	EngFuncs::CvarSetValue( "coop", 0.0f );
	EngFuncs::CvarSetValue( "maxplayers", 1.0f ); // singleplayer

	EngFuncs::PlayBackgroundTrack( NULL, NULL );

	EngFuncs::ClientCmd( false, "hazardcourse\n" );
}

void CMenuMainWindow::_Init( void )
{
	// Use game title if available
	if( gMenu.m_gameinfo.title[0] )
		SetTitle( gMenu.m_gameinfo.title );

	// Root window fills the entire virtual coordinate space
	SetRect( 0, 0, 1024, 768 );

	// No close or maximize buttons for root menu
	SetShowCloseButton( false );
	SetShowMaxButton( false );

	if( gMenu.m_gameinfo.trainmap[0] && stricmp( gMenu.m_gameinfo.trainmap, gMenu.m_gameinfo.startmap ) != 0 )
		bTrainMap = true;
	else bTrainMap = false;

	if( EngFuncs::GetCvarFloat( "host_allow_changegame" ) )
		bCustomGame = true;
	else bCustomGame = false;

	// console
	console.SetNameAndStatus( L( "GameUI_Console" ), L( "Show console" ) );
	console.iFlags |= QMF_NOTIFY;
	console.SetPicture( PC_CONSOLE );
	console.SetVisibility( gpGlobals->developer );
	ApplyMenuButtonStyle( console );
	SET_EVENT_MULTI( console.onReleased,
	{
		UI_SetActiveMenu( false );
		EngFuncs::KEY_SetDest( KEY_CONSOLE );
	});

	resumeGame.SetNameAndStatus( L( "GameUI_GameMenu_ResumeGame" ), L( "StringsList_188" ) );
	resumeGame.SetPicture( PC_RESUME_GAME );
	resumeGame.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( resumeGame );
	resumeGame.onReleased = UI_CloseMenu;

	disconnect.SetNameAndStatus( L( "GameUI_GameMenu_Disconnect" ), L( "Disconnect from server" ) );
	disconnect.SetPicture( PC_DISCONNECT );
	disconnect.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( disconnect );
	disconnect.onReleased = VoidCb( &CMenuMainWindow::DisconnectDialogCb );

	newGame.SetNameAndStatus( L( "GameUI_NewGame" ), L( "StringsList_189" ) );
	newGame.SetPicture( PC_NEW_GAME );
	newGame.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( newGame );
	newGame.onReleased = UI_NewGame_Menu;

	hazardCourse.SetNameAndStatus( L( "GameUI_TrainingRoom" ), L( "StringsList_190" ) );
	hazardCourse.SetPicture( PC_HAZARD_COURSE );
	hazardCourse.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( hazardCourse );
	hazardCourse.onReleasedClActive = VoidCb( &CMenuMainWindow::HazardCourseDialogCb );
	hazardCourse.onReleased = VoidCb( &CMenuMainWindow::HazardCourseCb );

	multiPlayer.SetNameAndStatus( L( "GameUI_Multiplayer" ), L( "StringsList_198" ) );
	multiPlayer.SetPicture( PC_MULTIPLAYER );
	multiPlayer.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( multiPlayer );
	multiPlayer.onReleased = UI_MultiPlayer_Menu;

	configuration.SetNameAndStatus( L( "GameUI_Options" ), L( "StringsList_193" ) );
	configuration.SetPicture( PC_CONFIG );
	configuration.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( configuration );
	configuration.onReleased = UI_Settings_Menu;

	saveRestore.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( saveRestore );

	customGame.SetNameAndStatus( L( "GameUI_ChangeGame" ), L( "StringsList_530" ) );
	customGame.SetPicture( PC_CUSTOM_GAME );
	customGame.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( customGame );
	customGame.onReleased = UI_CustomGame_Menu;

	previews.SetNameAndStatus( L( "Previews" ), L( "StringsList_400" ) );
	previews.SetPicture( PC_PREVIEWS );
	previews.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( previews );
	SET_EVENT( previews.onReleased, EngFuncs::ShellExecute( MenuStrings[IDS_MEDIA_PREVIEWURL], NULL, false ) );

	quit.SetNameAndStatus( L( "GameUI_GameMenu_Quit" ), L( "GameUI_QuitConfirmationText" ) );
	quit.SetPicture( PC_QUIT );
	quit.iFlags |= QMF_NOTIFY;
	ApplyMenuButtonStyle( quit );
	quit.onReleased = VoidCb( &CMenuMainWindow::QuitDialogCb );

	if( gMenu.m_gameinfo.gamemode == GAME_MULTIPLAYER_ONLY || gMenu.m_gameinfo.startmap[0] == 0 )
		newGame.SetGrayed( true );

	if( gMenu.m_gameinfo.gamemode == GAME_SINGLEPLAYER_ONLY )
		multiPlayer.SetGrayed( true );

	if( gMenu.m_gameinfo.gamemode == GAME_MULTIPLAYER_ONLY )
	{
		saveRestore.SetGrayed( true );
		hazardCourse.SetGrayed( true );
	}

	// too short execute string - not a real command
	if( strlen( MenuStrings[IDS_MEDIA_PREVIEWURL] ) <= 3 )
	{
		previews.SetGrayed( true );
	}

	// server.dll needs for reading savefiles or startup newgame
	if( !EngFuncs::CheckGameDll() )
	{
		saveRestore.SetGrayed( true );
		hazardCourse.SetGrayed( true );
		newGame.SetGrayed( true );
	}

	dialog.Link( this );

	AddItem( console );
	AddItem( disconnect );
	AddItem( resumeGame );
	AddItem( newGame );

	if( bTrainMap )
		AddItem( hazardCourse );

	AddItem( configuration );
	AddItem( saveRestore );
	AddItem( multiPlayer );

	if( bCustomGame )
		AddItem( customGame );

	AddItem( previews );
	AddItem( quit );
}

/*
=================
CMenuMainWindow::VidInit
=================
*/
void CMenuMainWindow::VidInit( bool connected )
{
	// Vertical button layout starting position
	int xoffset = 72;
	int ystart = 180;
	int ygap = 42;
	int ypos = ystart;

	// Determine visibility first
	bool single = gpGlobals->maxClients < 2;
	bool showConsole = gpGlobals->developer != 0;
	bool showResume = connected;
	bool showDisconnect = connected && !single;

	console.SetVisibility( showConsole );
	resumeGame.SetVisibility( showResume );
	disconnect.SetVisibility( showDisconnect );

	// Assign positions only to visible buttons (no gaps)
	if( showConsole )
	{
		console.SetCoord( xoffset, ypos );
		ypos += ygap;
	}

	if( showDisconnect )
	{
		disconnect.SetCoord( xoffset, ypos );
		ypos += ygap;
	}

	if( showResume )
	{
		resumeGame.SetCoord( xoffset, ypos );
		ypos += ygap;
	}

	newGame.SetCoord( xoffset, ypos );
	ypos += ygap;

	if( bTrainMap )
	{
		hazardCourse.SetCoord( xoffset, ypos );
		ypos += ygap;
	}

	configuration.SetCoord( xoffset, ypos );
	ypos += ygap;

	saveRestore.SetCoord( xoffset, ypos );
	ypos += ygap;

	multiPlayer.SetCoord( xoffset, ypos );
	ypos += ygap;

	if( bCustomGame )
	{
		customGame.SetCoord( xoffset, ypos );
		ypos += ygap;
	}

	previews.SetCoord( xoffset, ypos );
	ypos += ygap;

	quit.SetCoord( xoffset, ypos );

	// Save/Load context
	if( connected && single )
	{
		saveRestore.SetNameAndStatus( L( "Save\\Load Game" ), L( "StringsList_192" ) );
		saveRestore.SetPicture( PC_SAVE_LOAD_GAME );
		saveRestore.onReleased = UI_SaveLoad_Menu;
	}
	else
	{
		saveRestore.SetNameAndStatus( L( "GameUI_LoadGame" ), L( "StringsList_191" ) );
		saveRestore.SetPicture( PC_LOAD_GAME );
		saveRestore.onReleased = UI_LoadGame_Menu;
	}
}

void CMenuMainWindow::_VidInit()
{
	VidInit( CL_IsActive() );
}

void CMenuMainWindow::Think()
{
	if( gpGlobals->developer )
	{
		if( !console.IsVisible() )
			console.Show();
	}
	else
	{
		if( console.IsVisible() )
			console.Hide();
	}

	CMenuWindow::Think();
}

ADD_MENU( menu_main, CMenuMainWindow, UI_Main_Menu );
