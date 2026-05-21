/*
 * SteamLoginActivity.java - Steam OpenID WebView login for Slayer3D
 * Copyright (C) 2026 Slayer3D contributors
 *
 * Opens a WebView with Steam's OpenID login page.
 * On successful authentication, extracts SteamID64 from the
 * claimed_id parameter in the callback URL and returns it
 * to the native engine via JNI.
 *
 * OpenID 2.0 flow:
 * 1. WebView navigates to steamcommunity.com/openid/login?...
 * 2. User authenticates on Steam's page
 * 3. Steam redirects to our callback URL with claimed_id containing SteamID64
 * 4. We intercept the redirect, parse SteamID64, call native callback
 */
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

public class SteamLoginActivity extends Activity
{
    private static final String TAG = "SteamLogin";


    // The realm/callback URL. Steam will redirect here after auth.
    // Using a custom scheme that we intercept in shouldOverrideUrlLoading.
    private static final String CALLBACK_URL = "https://slayer3d.localhost/openid/callback";
    private static final String CALLBACK_HOST = "slayer3d.localhost";

    // Steam OpenID endpoint
    private static final String STEAM_OPENID_URL =
        "https://steamcommunity.com/openid/login" +
        "?openid.ns=http://specs.openid.net/auth/2.0" +
        "&openid.mode=checkid_setup" +
        "&openid.return_to=" + CALLBACK_URL +
        "&openid.realm=" + CALLBACK_URL +
        "&openid.identity=http://specs.openid.net/auth/2.0/identifier_select" +
        "&openid.claimed_id=http://specs.openid.net/auth/2.0/identifier_select";

    private WebView webView;

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate( Bundle savedInstanceState )
    {
        super.onCreate( savedInstanceState );

        webView = new WebView( this );
        webView.getSettings().setJavaScriptEnabled( true );
        webView.getSettings().setDomStorageEnabled( true );

        webView.setWebViewClient( new WebViewClient()
        {
            @Override
            public boolean shouldOverrideUrlLoading( WebView view, WebResourceRequest request )
            {
                Uri uri = request.getUrl();
                return handleUrl( uri );
            }

            @SuppressWarnings("deprecation")
            @Override
            public boolean shouldOverrideUrlLoading( WebView view, String url )
            {
                return handleUrl( Uri.parse( url ) );
            }
        } );

        setContentView( webView );
        webView.loadUrl( STEAM_OPENID_URL );

        Log.i( TAG, "Steam OpenID WebView opened" );
    }


    private boolean handleUrl( Uri uri )
    {
        if( uri == null )
            return false;

        String host = uri.getHost();
        if( host == null || !host.equals( CALLBACK_HOST ) )
            return false;

        // This is our callback URL - extract SteamID64 from claimed_id
        // Format: https://steamcommunity.com/openid/id/76561198XXXXXXXXX
        String claimedId = uri.getQueryParameter( "openid.claimed_id" );

        if( claimedId != null && claimedId.contains( "/openid/id/" ) )
        {
            String steamid64 = claimedId.substring( claimedId.lastIndexOf( '/' ) + 1 );

            Log.i( TAG, "Login success, SteamID64: " + steamid64 );

            // Send result back to native
            XashActivity.nativeSteamLoginResult( steamid64 );

            finish();
            return true;
        }

        // Login failed or cancelled
        Log.w( TAG, "Login callback without valid claimed_id: " + uri.toString() );
        XashActivity.nativeSteamLoginCancelled();
        finish();
        return true;
    }

    @Override
    public void onBackPressed()
    {
        // User pressed back - treat as cancel
        XashActivity.nativeSteamLoginCancelled();
        super.onBackPressed();
    }

    @Override
    protected void onDestroy()
    {
        if( webView != null )
        {
            webView.destroy();
            webView = null;
        }
        super.onDestroy();
    }
}
