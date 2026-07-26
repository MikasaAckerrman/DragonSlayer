package su.xash.engine;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.LinearLayout;

/**
 * Steam OpenID 2.0 login Activity.
 *
 * Opens a WebView pointing to Steam's OpenID login page.
 * When the user completes login, Steam redirects to the callback URL
 * which contains the claimed_id with the user's SteamID64.
 *
 * Format: https://steamcommunity.com/openid/id/76561198012345678
 *
 * The Activity extracts the SteamID64 and passes it back to the engine
 * via JNI native callback (nativeSteamLoginResult on XashActivity).
 */
public class SteamLoginActivity extends Activity
{
	private static final String TAG = "SteamLogin";

	// Steam OpenID endpoint
	private static final String STEAM_OPENID_URL =
		"https://steamcommunity.com/openid/login";

	// Steam OpenID 2.0 requires http(s) realm/return_to — a custom scheme is
	// rejected and yields a blank page. We use a real-looking https URL that is
	// never actually fetched: the WebView intercepts the redirect to this host
	// (shouldOverrideUrlLoading) before it hits the network.
	private static final String CALLBACK_SCHEME = "https";
	private static final String CALLBACK_HOST = "login.slayer3d.dev";

	// OpenID claimed_id prefix that contains SteamID64
	private static final String CLAIMED_ID_PREFIX =
		"https://steamcommunity.com/openid/id/";

	private WebView mWebView;
	private String mRealm;
	private String mReturnTo;


	@SuppressLint("SetJavaScriptEnabled")
	@Override
	protected void onCreate( Bundle savedInstanceState )
	{
		super.onCreate( savedInstanceState );

		// Get realm and returnTo from intent
		Intent intent = getIntent();
		mRealm = intent.getStringExtra( "realm" );
		mReturnTo = intent.getStringExtra( "returnTo" );

		if( mRealm == null )
			mRealm = CALLBACK_SCHEME + "://" + CALLBACK_HOST + "/";
		if( mReturnTo == null )
			mReturnTo = CALLBACK_SCHEME + "://" + CALLBACK_HOST + "/callback";

		Log.d( TAG, "onCreate: realm=" + mRealm + " returnTo=" + mReturnTo );

		// Create WebView programmatically (no XML layout needed)
		mWebView = new WebView( this );
		mWebView.getSettings().setJavaScriptEnabled( true );
		mWebView.getSettings().setDomStorageEnabled( true );

		mWebView.getSettings().setUseWideViewPort( true );
		mWebView.getSettings().setLoadWithOverviewMode( true );
		mWebView.setBackgroundColor( 0xFF1B2838 );   // Steam dark blue, not pure black

		LinearLayout layout = new LinearLayout( this );
		layout.setOrientation( LinearLayout.VERTICAL );
		layout.setBackgroundColor( 0xFF1B2838 );
		layout.addView( mWebView, new LinearLayout.LayoutParams(
			LinearLayout.LayoutParams.MATCH_PARENT,
			LinearLayout.LayoutParams.MATCH_PARENT ) );
		setContentView( layout );

		mWebView.setWebViewClient( new WebViewClient()
		{
			@Override
			public boolean shouldOverrideUrlLoading( WebView view, WebResourceRequest request )
			{
				Uri uri = request.getUrl();
				return handleUrl( uri );
			}

			@Override
			public boolean shouldOverrideUrlLoading( WebView view, String url )
			{
				Uri uri = Uri.parse( url );
				return handleUrl( uri );
			}

			@Override
			public void onReceivedError( WebView view, WebResourceRequest req,
				android.webkit.WebResourceError err )
			{
				Log.e( TAG, "WebView error: " + err.getErrorCode()
					+ " " + err.getDescription() + " url=" + req.getUrl() );
			}
		} );

		// Build the Steam OpenID login URL
		String loginUrl = buildOpenIdUrl();
		Log.d( TAG, "Loading: " + loginUrl );
		mWebView.loadUrl( loginUrl );
	}


	/**
	 * Build the Steam OpenID 2.0 login URL with all required parameters.
	 */
	private String buildOpenIdUrl()
	{
		return STEAM_OPENID_URL
			+ "?openid.ns=" + Uri.encode( "http://specs.openid.net/auth/2.0" )
			+ "&openid.mode=checkid_setup"
			+ "&openid.return_to=" + Uri.encode( mReturnTo )
			+ "&openid.realm=" + Uri.encode( mRealm )
			+ "&openid.identity=" + Uri.encode( "http://specs.openid.net/auth/2.0/identifier_select" )
			+ "&openid.claimed_id=" + Uri.encode( "http://specs.openid.net/auth/2.0/identifier_select" );
	}

	/**
	 * Handle URL navigation. Intercepts our custom callback scheme.
	 * @return true if URL was handled (prevents WebView from loading it)
	 */
	private boolean handleUrl( Uri uri )
	{
		if( uri == null )
			return false;

		String scheme = uri.getScheme();
		String host = uri.getHost();

		// Check if this is our callback URL
		if( CALLBACK_SCHEME.equals( scheme ) && CALLBACK_HOST.equals( host ) )
		{
			Log.d( TAG, "Intercepted callback: " + uri.toString() );
			handleCallback( uri );
			return true;
		}

		return false;
	}

	/**
	 * Process the OpenID callback URL.
	 * Extract claimed_id parameter which contains the SteamID64.
	 */
	private void handleCallback( Uri uri )
	{
		// The claimed_id is in the query parameters of the callback URL
		String claimedId = uri.getQueryParameter( "openid.claimed_id" );

		if( claimedId == null || claimedId.isEmpty() )
		{
			Log.w( TAG, "No claimed_id in callback - login cancelled?" );
			notifyEngine( -1 );
			finish();
			return;
		}

		Log.d( TAG, "claimed_id: " + claimedId );

		// Extract SteamID64 from claimed_id URL
		// Format: https://steamcommunity.com/openid/id/76561198012345678
		if( !claimedId.startsWith( CLAIMED_ID_PREFIX ) )
		{
			Log.e( TAG, "Unexpected claimed_id format: " + claimedId );
			notifyEngine( -1 );
			finish();
			return;
		}

		String steamIdStr = claimedId.substring( CLAIMED_ID_PREFIX.length() );
		long steamId64;

		try
		{
			// Use parseLong — SteamID64 values are within signed long range
			// (max ~76561198999999999 < Long.MAX_VALUE)
			steamId64 = Long.parseLong( steamIdStr );
		}
		catch( NumberFormatException e )
		{
			Log.e( TAG, "Failed to parse SteamID64: " + steamIdStr );
			notifyEngine( -1 );
			finish();
			return;
		}

		Log.i( TAG, "Login successful! SteamID64 = " + steamId64 );
		persistSteamId( steamId64 );
		notifyEngine( steamId64 );
		finish();
	}


	/**
	 * Store the SteamID in the launcher's preferences.
	 *
	 * This is what makes signing in from the Settings screen work: the engine
	 * is not running then, so the JNI callback below is a no-op. Game.kt reads
	 * this value at launch and passes it to the engine on the command line.
	 */
	private void persistSteamId( long steamId64 )
	{
		try
		{
			getSharedPreferences( "app_preferences", MODE_PRIVATE )
				.edit()
				.putString( "steam_id", Long.toString( steamId64 ) )
				.apply();
		}
		catch( Exception e )
		{
			Log.e( TAG, "Failed to persist SteamID: " + e.getMessage() );
		}
	}


	/**
	 * Notify the native engine of the login result.
	 * Calls XashActivity.nativeSteamLoginResult(long steamid64) via JNI.
	 *
	 * @param steamId64 The SteamID64, or -1 if login failed/cancelled
	 */
	private void notifyEngine( long steamId64 )
	{
		try
		{
			XashActivity.nativeSteamLoginResult( steamId64 );
		}
		catch( UnsatisfiedLinkError e )
		{
			Log.e( TAG, "nativeSteamLoginResult not linked: " + e.getMessage() );
		}
	}

	@Override
	public void onBackPressed()
	{
		// User pressed back - treat as cancellation
		if( mWebView != null && mWebView.canGoBack() )
		{
			mWebView.goBack();
		}
		else
		{
			Log.d( TAG, "User cancelled login" );
			notifyEngine( -1 );
			super.onBackPressed();
		}
	}

	@Override
	protected void onDestroy()
	{
		if( mWebView != null )
		{
			mWebView.destroy();
			mWebView = null;
		}
		super.onDestroy();
	}
}
