/*
SteamAuthActivity.java - sign in to Steam with a real credential.
Copyright (C) 2026 Slayer3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

The successor to SteamLoginActivity. That one runs Steam OpenID in a WebView
and its whole answer is a SteamID64: proof of who you are, and nothing you can
act with. This one runs Steam's token flow and ends up with a refresh_token,
which is what a CM session -- and therefore the "playing Counter-Strike"
status -- actually requires.

THREE THINGS THIS SCREEN GOT WRONG, all reported from the device, all fixed
here, and each one worth writing down because the shape of the mistake is not
obvious from the protocol docs:

  1. IT STARTED WITH THE QR FLOW. On the phone the user is signing in FROM,
     a QR code is nonsense -- there is nothing to photograph it with, and
     opening the s.team challenge link can land in the Steam app's QR SCANNER,
     pointed at a code that exists nowhere ("меня перебрасывает в стим чтобы
     отсканировать qr код какой?? Его нету"). Account name and password is now
     the default and the only thing on screen at start; app approval is a
     button for the case where the approving device is a different one.

  2. APPROVING IN THE STEAM APP DID NOTHING. Steam offers several guard types
     at once -- typically "type the authenticator code" AND "approve on the
     device" -- and the old code, on seeing a code type, blocked its worker
     thread on a UI gate until a code was typed. Nothing polled Steam in the
     meantime, so a push notification the user had already approved was never
     noticed. There is no gate any more: ONE loop polls Steam on its schedule
     and submits a code if and when one is typed, so either route completes,
     whichever the user actually takes.

  3. EVERY RETRY ADDED ANOTHER PAIR OF WIDGETS. The code field and its button
     were built inside the prompt, so a second sign-in attempt produced two
     "Submit code" buttons (the user's screenshot shows exactly that), a third
     three. Every widget is built ONCE in buildUi() and shown or hidden.

Deliberately no WebView: a login form inside an app-controlled WebView is
exactly the shape of a credential-phishing screen, and Steam's own guidance is
to use the app or the system browser. Everything here talks to
api.steampowered.com over HTTPS.
*/

package su.xash.engine;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import su.xash.engine.steam.SteamAuth;
import su.xash.engine.steam.SteamPresence;

public class SteamAuthActivity extends Activity
{
	private static final String TAG = "SteamAuth";

	private static final int STEAM_BLUE = 0xFF1B2838;
	private static final int STEAM_TEXT = 0xFFC7D5E0;

	// Steam asks us to poll on its own schedule; treat its interval as a floor.
	private static final int POLL_FLOOR_MS = 2000;
	private static final long POLL_DEADLINE_MS = 5 * 60 * 1000;

	private final Handler ui = new Handler( Looper.getMainLooper() );

	private TextView status;
	private LinearLayout root;
	private EditText accountField;
	private EditText passwordField;
	private Button signInButton;
	private Button appRouteButton;      // "use the Steam app instead"
	private EditText codeField;
	private Button codeButton;
	private TextView codeHint;

	/**
	 * Bumped whenever a flow starts. A thread whose generation is stale exits
	 * without touching anything -- this replaces the old shared `cancelled`
	 * flag, which two flows wrote to at once (the password flow set it to stop
	 * the QR poll and then cleared it, so a slow QR poll could resume against
	 * a session that no longer existed).
	 */
	private volatile int generation;
	private volatile Thread worker;

	/** A code the user typed, waiting for the loop to pick it up. */
	private volatile String pendingCode;

	@Override
	protected void onCreate( Bundle savedInstanceState )
	{
		super.onCreate( savedInstanceState );
		buildUi();
		// NO automatic QR flow. See note 1 in the header: the default route on
		// the signing-in device is a password, and starting a network flow the
		// user did not ask for also means the first thing the screen said was
		// about a QR code they have no way to scan.
		setStatus( R.string.steam_auth_intro );
	}

	@Override
	protected void onDestroy()
	{
		generation++;            // every running flow is now stale

		Thread w = worker;
		if( w != null )
			w.interrupt();

		super.onDestroy();
	}

	// -----------------------------------------------------------------------
	// UI, built in code to avoid adding layout resources for one screen
	// -----------------------------------------------------------------------

	private void buildUi()
	{
		int pad = dp( 20 );

		root = new LinearLayout( this );
		root.setOrientation( LinearLayout.VERTICAL );
		root.setBackgroundColor( STEAM_BLUE );
		root.setPadding( pad, pad, pad, pad );

		TextView title = new TextView( this );
		title.setText( R.string.steam_auth_title );
		title.setTextColor( Color.WHITE );
		title.setTextSize( 20 );
		root.addView( title );

		status = new TextView( this );
		status.setTextColor( STEAM_TEXT );
		status.setTextSize( 14 );
		status.setPadding( 0, dp( 12 ), 0, dp( 16 ));
		root.addView( status );

		accountField = new EditText( this );
		accountField.setHint( R.string.steam_auth_account_hint );
		accountField.setTextColor( Color.WHITE );
		accountField.setHintTextColor( STEAM_TEXT );
		// VISIBLE_PASSWORD, not a plain text field: an account name must not be
		// autocorrected or capitalised, and Steam names are case-sensitive.
		accountField.setInputType( InputType.TYPE_CLASS_TEXT
			| InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD );
		root.addView( accountField );

		passwordField = new EditText( this );
		passwordField.setHint( R.string.steam_auth_password_hint );
		passwordField.setTextColor( Color.WHITE );
		passwordField.setHintTextColor( STEAM_TEXT );
		passwordField.setInputType( InputType.TYPE_CLASS_TEXT
			| InputType.TYPE_TEXT_VARIATION_PASSWORD );
		root.addView( passwordField );

		signInButton = new Button( this );
		signInButton.setText( R.string.steam_auth_sign_in );
		signInButton.setOnClickListener( new View.OnClickListener()
		{
			public void onClick( View v )
			{
				startPasswordFlow();
			}
		});
		root.addView( signInButton );

		// --- Steam Guard, hidden until Steam says it wants one ---------------
		codeHint = new TextView( this );
		codeHint.setTextColor( STEAM_TEXT );
		codeHint.setTextSize( 14 );
		codeHint.setPadding( 0, dp( 16 ), 0, dp( 4 ));
		codeHint.setVisibility( View.GONE );
		root.addView( codeHint );

		codeField = new EditText( this );
		codeField.setTextColor( Color.WHITE );
		codeField.setHintTextColor( STEAM_TEXT );
		codeField.setInputType( InputType.TYPE_CLASS_TEXT
			| InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD );
		codeField.setVisibility( View.GONE );
		root.addView( codeField );

		codeButton = new Button( this );
		codeButton.setText( R.string.steam_auth_submit_code );
		codeButton.setVisibility( View.GONE );
		codeButton.setOnClickListener( new View.OnClickListener()
		{
			public void onClick( View v )
			{
				String code = codeField.getText().toString().trim();

				if( code.length() == 0 )
					return;

				// Handed to the loop rather than sent from here: the loop owns
				// the session, and two threads talking to one auth session is
				// how the old code lost track of it.
				pendingCode = code;
				codeButton.setEnabled( false );
				setStatus( R.string.steam_auth_checking_code );
			}
		});
		root.addView( codeButton );

		// --- the app route, secondary on purpose ----------------------------
		appRouteButton = new Button( this );
		appRouteButton.setText( R.string.steam_auth_app_route );
		appRouteButton.setOnClickListener( new View.OnClickListener()
		{
			public void onClick( View v )
			{
				startQrFlow();
			}
		});
		root.addView( appRouteButton );

		ScrollView scroll = new ScrollView( this );
		scroll.addView( root, new ViewGroup.LayoutParams(
			ViewGroup.LayoutParams.MATCH_PARENT,
			ViewGroup.LayoutParams.WRAP_CONTENT ));
		scroll.setBackgroundColor( STEAM_BLUE );
		setContentView( scroll );
	}

	private int dp( int v )
	{
		return (int)( v * getResources().getDisplayMetrics().density );
	}

	private void setStatus( final String text )
	{
		ui.post( new Runnable()
		{
			public void run()
			{
				status.setText( text );
			}
		});
	}

	private void setStatus( final int resId )
	{
		ui.post( new Runnable()
		{
			public void run()
			{
				status.setText( resId );
			}
		});
	}

	/** Re-enable the form after a failure, so a retry is possible in place. */
	private void reEnableForm()
	{
		ui.post( new Runnable()
		{
			public void run()
			{
				signInButton.setEnabled( true );
				appRouteButton.setEnabled( true );
				codeButton.setEnabled( true );
			}
		});
	}

	private boolean stale( int gen )
	{
		return gen != generation || isFinishing();
	}

	/** Starts a flow: invalidates any previous one and returns the new gen. */
	private int beginFlow()
	{
		generation++;

		Thread w = worker;
		if( w != null )
			w.interrupt();

		pendingCode = null;
		return generation;
	}

	// -----------------------------------------------------------------------
	// Password flow (the default)
	// -----------------------------------------------------------------------

	private void startPasswordFlow()
	{
		final String account = accountField.getText().toString().trim();
		final String password = passwordField.getText().toString();

		if( account.length() == 0 || password.length() == 0 )
		{
			setStatus( R.string.steam_auth_need_credentials );
			return;
		}

		final int gen = beginFlow();

		signInButton.setEnabled( false );
		appRouteButton.setEnabled( false );
		hideCodeUi();
		setStatus( R.string.steam_auth_signing_in );

		final SteamAuth auth = new SteamAuth( deviceName() );

		worker = new Thread( new Runnable()
		{
			public void run()
			{
				try
				{
					SteamAuth.Session session = auth.beginPassword( account, password );

					if( stale( gen ))
						return;

					// Show the code field if Steam offers a code type, and say
					// so in the status if it ALSO offers device approval. Both
					// are then live at once, which is the whole point: the user
					// takes whichever route they prefer and the loop below
					// notices either.
					announceGuard( session );
					runFlow( gen, auth, session );
				}
				catch( Exception e )
				{
					if( stale( gen ))
						return;

					Log.e( TAG, "password flow failed: " + e );
					setStatus( getString( R.string.steam_auth_failed, e.getMessage() ));
					reEnableForm();
				}
			}
		}, "SteamAuth-password" );
		worker.start();
	}

	private void announceGuard( final SteamAuth.Session session )
	{
		final boolean wantsCode = session.needsCode();
		final boolean wantsApproval = session.needsAppConfirmation();
		final boolean email = session.emailCode();

		ui.post( new Runnable()
		{
			public void run()
			{
				if( wantsCode )
				{
					codeField.setHint( email ? R.string.steam_auth_email_code
						: R.string.steam_auth_app_code );
					codeField.setText( "" );
					codeHint.setText( wantsApproval
						? R.string.steam_auth_code_or_approve
						: ( email ? R.string.steam_auth_enter_email_code
							: R.string.steam_auth_enter_app_code ));
					codeHint.setVisibility( View.VISIBLE );
					codeField.setVisibility( View.VISIBLE );
					codeButton.setVisibility( View.VISIBLE );
					codeButton.setEnabled( true );
					codeField.requestFocus();
				}
				else
				{
					hideCodeUi();
				}

				if( wantsApproval && !wantsCode )
					status.setText( R.string.steam_auth_waiting_approval );
				else if( !wantsCode )
					status.setText( R.string.steam_auth_signing_in );
			}
		});
	}

	private void hideCodeUi()
	{
		codeHint.setVisibility( View.GONE );
		codeField.setVisibility( View.GONE );
		codeButton.setVisibility( View.GONE );
	}

	// -----------------------------------------------------------------------
	// QR / mobile-app approval (secondary route, started only on request)
	// -----------------------------------------------------------------------

	private volatile String challengeUrl;

	private void startQrFlow()
	{
		final int gen = beginFlow();

		challengeUrl = null;
		signInButton.setEnabled( false );
		appRouteButton.setEnabled( false );
		hideCodeUi();
		setStatus( R.string.steam_auth_starting );

		final SteamAuth auth = new SteamAuth( deviceName() );

		worker = new Thread( new Runnable()
		{
			public void run()
			{
				try
				{
					SteamAuth.Session session = auth.beginQR();

					if( stale( gen ))
						return;

					challengeUrl = session.challengeUrl;

					ui.post( new Runnable()
					{
						public void run()
						{
							status.setText( getString( R.string.steam_auth_qr_ready,
								challengeUrl ));
							appRouteButton.setEnabled( true );
							appRouteButton.setText( R.string.steam_auth_open_app );
							appRouteButton.setOnClickListener( new View.OnClickListener()
							{
								public void onClick( View v )
								{
									openChallengeInSteamApp();
								}
							});
						}
					});

					runFlow( gen, auth, session );
				}
				catch( Exception e )
				{
					if( stale( gen ))
						return;

					Log.e( TAG, "QR flow failed: " + e );
					setStatus( getString( R.string.steam_auth_failed, e.getMessage() ));
					reEnableForm();
				}
			}
		}, "SteamAuth-qr" );
		worker.start();
	}

	private void openChallengeInSteamApp()
	{
		String url = challengeUrl;

		if( url == null )
			return;

		// The Steam app claims s.team links. If it is not installed the browser
		// takes it, which also works -- the page asks the user to confirm.
		try
		{
			startActivity( new Intent( Intent.ACTION_VIEW, Uri.parse( url )));
			setStatus( R.string.steam_auth_waiting_approval );
		}
		catch( Exception e )
		{
			Toast.makeText( this, getString( R.string.steam_auth_failed, e.getMessage() ),
				Toast.LENGTH_LONG ).show();
		}
	}

	// -----------------------------------------------------------------------
	// The one loop both routes end in
	// -----------------------------------------------------------------------

	/**
	 * Poll Steam until it hands over a token, submitting a Guard code if the
	 * user types one along the way.
	 *
	 * There is deliberately NO waiting for the user here. That is the fix for
	 * "I approved it in the Steam app and nothing happened": the old code
	 * blocked this thread on a UI gate whenever Steam mentioned a code type, so
	 * an approval that needed no code was never polled for. Polling is
	 * unconditional; a typed code is an event that joins the loop.
	 */
	private void runFlow( int gen, SteamAuth auth, SteamAuth.Session session )
		throws Exception
	{
		long deadline = System.currentTimeMillis() + POLL_DEADLINE_MS;

		while( !stale( gen ) && System.currentTimeMillis() < deadline )
		{
			String code = pendingCode;

			if( code != null )
			{
				pendingCode = null;

				try
				{
					auth.submitGuardCode( session, code );
					setStatus( R.string.steam_auth_code_accepted );
				}
				catch( SteamAuth.SteamAuthException e )
				{
					// A wrong code is not a dead session: Steam lets the user
					// try again, so the loop carries on rather than throwing the
					// whole sign-in away.
					Log.w( TAG, "guard code rejected: " + e );
					setStatus( getString( R.string.steam_auth_code_rejected,
						e.getMessage() ));
					ui.post( new Runnable()
					{
						public void run()
						{
							codeButton.setEnabled( true );
							codeField.setText( "" );
							codeField.requestFocus();
						}
					});
				}
			}

			SteamAuth.Poll poll = auth.poll( session );

			if( poll.done() )
			{
				finishWithToken( poll.accountName, poll.refreshToken );
				return;
			}

			// Steam rotates the challenge while waiting; if it did, the link the
			// user is looking at is stale and must be replaced.
			if( poll.newChallengeUrl != null && poll.newChallengeUrl.length() > 0 )
			{
				challengeUrl = session.challengeUrl;
				setStatus( getString( R.string.steam_auth_qr_ready, challengeUrl ));
			}

			long wait = Math.max( (long)( session.interval * 1000 ), POLL_FLOOR_MS );

			// Split the sleep so a code typed mid-interval is picked up promptly
			// instead of after the full interval -- the difference between a
			// screen that feels broken and one that does not.
			long slept = 0;

			while( slept < wait && !stale( gen ) && pendingCode == null )
			{
				try
				{
					Thread.sleep( 250 );
				}
				catch( InterruptedException e )
				{
					Thread.currentThread().interrupt();
					return;
				}

				slept += 250;
			}
		}

		if( !stale( gen ))
		{
			setStatus( R.string.steam_auth_timed_out );
			reEnableForm();
		}
	}

	private void finishWithToken( String accountName, String refreshToken )
	{
		long steamid = SteamAuth.steamidFromToken( refreshToken );

		if( steamid == 0 )
		{
			setStatus( getString( R.string.steam_auth_failed, "no steamid in token" ));
			reEnableForm();
			return;
		}

		SteamPresence.saveCredentials( this, accountName, refreshToken, steamid );

		// The engine reads steam_id from these same preferences for avatars, and
		// SteamLoginActivity used to be the only writer. Keeping the key in sync
		// means signing in here also fixes up the avatar path -- and, crucially,
		// that the two sign-in routes cannot disagree about who is signed in.
		final long id = steamid;

		ui.post( new Runnable()
		{
			public void run()
			{
				Toast.makeText( SteamAuthActivity.this,
					getString( R.string.steam_auth_signed_in, Long.toString( id )),
					Toast.LENGTH_LONG ).show();

				setResult( RESULT_OK, new Intent().putExtra( "steamid", id ));
				finish();
			}
		});
	}

	private static String deviceName()
	{
		String model = android.os.Build.MODEL;
		return ( model != null && model.length() > 0 ? model : "Android" ) + " (Slayer3D)";
	}
}
