#include <stdio.h>
#include "cl_observer_slayer_logic.h"

static int failures;

static void expect_int( const char *name, int got, int want )
{
	if( got != want )
	{
		fprintf( stderr, "FAIL %s: got %d want %d\n", name, got, want );
		failures++;
	}
}

int main( void )
{
	expect_int( "alive uses local", Slayer_Observer_SelectFocus( 3, 0, 8, 32 ), 3 );
	expect_int( "chase locked uses target", Slayer_Observer_SelectFocus( 3, 1, 8, 32 ), 8 );
	expect_int( "chase free uses target", Slayer_Observer_SelectFocus( 3, 2, 8, 32 ), 8 );
	expect_int( "roaming ignores stale target", Slayer_Observer_SelectFocus( 3, 3, 8, 32 ), 3 );
	expect_int( "in-eye uses target", Slayer_Observer_SelectFocus( 3, 4, 8, 32 ), 8 );
	expect_int( "map free ignores stale target", Slayer_Observer_SelectFocus( 3, 5, 8, 32 ), 3 );
	expect_int( "map chase uses target", Slayer_Observer_SelectFocus( 3, 6, 8, 32 ), 8 );
	expect_int( "zero target falls back", Slayer_Observer_SelectFocus( 3, 4, 0, 32 ), 3 );
	expect_int( "oversized target falls back", Slayer_Observer_SelectFocus( 3, 2, 33, 32 ), 3 );
	expect_int( "invalid local returns zero", Slayer_Observer_SelectFocus( 0, 0, 8, 32 ), 0 );

	if( failures )
		return 1;

	printf( "observer_focus_test: 10 PASS\n" );
	return 0;
}
