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

Two ways in:

  * Approve in the Steam Mobile app. Steam's QR challenge is a URL of the form
    https://s.team/q/1/<client_id>, and the Steam app registers for s.team
    links. On the very device that would display a QR code, asking the user to
    photograph their own screen is absurd -- so the link is opened directly and
    the app takes over. (A QR is still shown as text for the case where the
    approving device is a different one.)
  * Account name and password, with Steam Guard if the account has it. The
    password is RSA-encrypted with a per-account key from Steam and never
    stored.

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
	private static final int STEAM_PANEL = 0xFF2A475E;
	private static final int STEAM_TEXT = 0xFFC7D5E0;
	private static final int STEAM_ACCENT = 0xFF66C0F4;

	// Steam asks us to poll on its own schedule; treat its interval as a floor.
	private static final int POLL_FLOOR_MS = 2000;
	private static final long POLL_DEADLINE_MS = 5 * 60 * 1000;

	private final Handler ui = new Handler( Looper.getMainLooper() );

	private TextView status;
	private LinearLayout root;
	private Button appButton;
	private Button passwordButton;
	private EditText accountField;
	private EditText passwordField;
	private EditText codeField;
	private Button submitButton;

	private volatile boolean cancelled;
	private Thread worker;

	@Override
	protected void onCreate( Bundle savedInstanceState )
	{
		super.onCreate( savedInstanceState );
		buildUi();
		startQrFlow();
	}

	@Override
	protected void onDestroy()
	{
		cancelled = true;

		if( worker != null )
			worker.interrupt();

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
		status.setText( R.string.steam_auth_starting );
		root.addView( status );

		appButton = new Button( this );
		appButton.setText( R.string.steam_auth_open_app );
		appButton.setEnabled( false );
		appButton.setOnClickListener( new View.OnClickListener()
		{
			public void onClick( View v )
			{
				openChallengeInSteamApp();
			}
		});
		root.addView( appButton );

		passwordButton = new Button( this );
		passwordButton.setText( R.string.steam_auth_use_password );
		passwordButton.setOnClickListener( new View.OnClickListener()
		{
			public void onClick( View v )
			{
				showPasswordForm();
			}
		});
		root.addView( passwordButton );

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

	// -----------------------------------------------------------------------
	// QR / mobile-app approval
	// -----------------------------------------------------------------------

	private volatile String challengeUrl;

	private void startQrFlow()
	{
		final SteamAuth auth = new SteamAuth( deviceName() );

		worker = new Thread( new Runnable()
		{
			public void run()
			{
				try
				{
					SteamAuth.Session session = auth.beginQR();
					challengeUrl = session.challengeUrl;

					ui.post( new Runnable()
					{
						public void run()
						{
							appButton.setEnabled( true );
							status.setText( getString( R.string.steam_auth_qr_ready,
								challengeUrl ));
						}
					});

					pollToFinish( auth, session );
				}
				catch( Exception e )
				{
					Log.e( TAG, "QR flow failed: " + e );
					setStatus( getString( R.string.steam_auth_failed, e.getMessage() ));
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
	// Password flow
	// -----------------------------------------------------------------------

	private void showPasswordForm()
	{
		passwordButton.setVisibility( View.GONE );

		accountField = new EditText( this );
		accountField.setHint( R.string.steam_auth_account_hint );
		accountField.setTextColor( Color.WHITE );
		accountField.setInputType( InputType.TYPE_CLASS_TEXT
			| InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD );
		root.addView( accountField );

		passwordField = new EditText( this );
		passwordField.setHint( R.string.steam_auth_password_hint );
		passwordField.setTextColor( Color.WHITE );
		passwordField.setInputType( InputType.TYPE_CLASS_TEXT
			| InputType.TYPE_TEXT_VARIATION_PASSWORD );
		root.addView( passwordField );

		submitButton = new Button( this );
		submitButton.setText( R.string.steam_auth_sign_in );
		submitButton.setOnClickListener( new View.OnClickListener()
		{
			public void onClick( View v )
			{
				submitPassword();
			}
		});
		root.addView( submitButton );
	}

	private void submitPassword()
	{
		final String account = accountField.getText().toString().trim();
		final String password = passwordField.getText().toString();

		if( account.length() == 0 || password.length() == 0 )
		{
			setStatus( R.string.steam_auth_need_credentials );
			return;
		}

		submitButton.setEnabled( false );
		appButton.setEnabled( false );
		cancelled = true;              // stop the QR poll; one flow at a time

		if( worker != null )
			worker.interrupt();

		cancelled = false;
		setStatus( R.string.steam_auth_signing_in );

		final SteamAuth auth = new SteamAuth( deviceName() );

		worker = new Thread( new Runnable()
		{
			public void run()
			{
				try
				{
					SteamAuth.Session session = auth.beginPassword( account, password );

					if( session.needsCode() )
					{
						final boolean email = session.emailCode();
						final String code = promptForCode( email );

						if( code == null )
						{
							setStatus( R.string.steam_auth_cancelled );
							return;
						}

						auth.submitGuardCode( session, code );
					}
					else if( session.needsAppConfirmation() )
					{
						setStatus( R.string.steam_auth_waiting_approval );
					}

					pollToFinish( auth, session );
				}
				catch( Exception e )
				{
					Log.e( TAG, "password flow failed: " + e );
					setStatus( getString( R.string.steam_auth_failed, e.getMessage() ));
					ui.post( new Runnable()
					{
						public void run()
						{
							if( submitButton != null )
								submitButton.setEnabled( true );
						}
					});
				}
			}
		}, "SteamAuth-password" );
		worker.start();
	}

	/** Blocks the worker thread until the user types a Guard code. */
	private String promptForCode( final boolean email )
	{
		final Object gate = new Object();
		final String[] result = new String[1];
		final boolean[] done = new boolean[1];

		ui.post( new Runnable()
		{
			public void run()
			{
				codeField = new EditText( SteamAuthActivity.this );
				codeField.setHint( email ? R.string.steam_auth_email_code
					: R.string.steam_auth_app_code );
				codeField.setTextColor( Color.WHITE );
				codeField.setInputType( InputType.TYPE_CLASS_TEXT
					| InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD );
				root.addView( codeField );

				Button ok = new Button( SteamAuthActivity.this );
				ok.setText( R.string.steam_auth_submit_code );
				ok.setOnClickListener( new View.OnClickListener()
				{
					public void onClick( View v )
					{
						synchronized( gate )
						{
							result[0] = codeField.getText().toString().trim();
							done[0] = true;
							gate.notifyAll();
						}
					}
				});
				root.addView( ok );
				status.setText( email ? R.string.steam_auth_enter_email_code
					: R.string.steam_auth_enter_app_code );
			}
		});

		synchronized( gate )
		{
			while( !done[0] && !cancelled )
			{
				try
				{
					gate.wait( 500 );
				}
				catch( InterruptedException e )
				{
					Thread.currentThread().interrupt();
					return null;
				}
			}
		}

		return result[0];
	}

	// -----------------------------------------------------------------------
	// Shared tail: poll until Steam hands over the token
	// -----------------------------------------------------------------------

	private void pollToFinish( SteamAuth auth, SteamAuth.Session session )
		throws Exception
	{
		long deadline = System.currentTimeMillis() + POLL_DEADLINE_MS;

		while( !cancelled && System.currentTimeMillis() < deadline )
		{
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

			try
			{
				Thread.sleep( wait );
			}
			catch( InterruptedException e )
			{
				Thread.currentThread().interrupt();
				return;
			}
		}

		if( !cancelled )
			setStatus( R.string.steam_auth_timed_out );
	}

	private void finishWithToken( String accountName, String refreshToken )
	{
		long steamid = SteamAuth.steamidFromToken( refreshToken );

		if( steamid == 0 )
		{
			setStatus( getString( R.string.steam_auth_failed, "no steamid in token" ));
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
