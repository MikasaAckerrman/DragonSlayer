/*
 * SteamAPIHelper.java - Steam Web API helper for Slayer3D
 * Copyright (C) 2026 Slayer3D contributors
 *
 * Provides static methods called from native (JNI) to access
 * Steam Web API via HTTPS (HttpsURLConnection).
 */
package su.xash.engine;

import android.util.Log;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.URL;
import javax.net.ssl.HttpsURLConnection;

public class SteamAPIHelper
{
    private static final String TAG = "SteamAPIHelper";
    private static final int TIMEOUT_MS = 15000;
    private static final int MAX_RESPONSE = 262144; // 256KB


    /**
     * Call Steam Web API GetPlayerSummaries/v2.
     *
     * @param apiKey   Steam Web API key
     * @param steamIds Comma-separated list of SteamID64s
     * @return JSON response string, or null on error
     */
    public static String getPlayerSummaries( String apiKey, String steamIds )
    {
        HttpsURLConnection conn = null;

        try
        {
            String urlStr = "https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v2/"
                + "?key=" + apiKey + "&steamids=" + steamIds;

            Log.d( TAG, "getPlayerSummaries: requesting " + steamIds.split(",").length + " players" );

            URL url = new URL( urlStr );
            conn = (HttpsURLConnection) url.openConnection();
            conn.setRequestMethod( "GET" );
            conn.setConnectTimeout( TIMEOUT_MS );
            conn.setReadTimeout( TIMEOUT_MS );
            conn.setRequestProperty( "User-Agent", "Slayer3D/1.0" );
            conn.setRequestProperty( "Accept", "application/json" );

            int responseCode = conn.getResponseCode();
            if( responseCode != 200 )
            {
                Log.w( TAG, "getPlayerSummaries: HTTP " + responseCode );
                return null;
            }

            BufferedReader reader = new BufferedReader(
                new InputStreamReader( conn.getInputStream() ) );
            StringBuilder sb = new StringBuilder();
            char[] buf = new char[4096];
            int read;

            while( (read = reader.read( buf )) != -1 )
            {
                sb.append( buf, 0, read );
                if( sb.length() > MAX_RESPONSE )
                {
                    Log.w( TAG, "getPlayerSummaries: response too large" );
                    reader.close();
                    return null;
                }
            }
            reader.close();

            String result = sb.toString();
            Log.d( TAG, "getPlayerSummaries: got " + result.length() + " bytes" );
            return result;
        }
        catch( Exception e )
        {
            Log.e( TAG, "getPlayerSummaries failed: " + e.getMessage() );
            return null;
        }
        finally
        {
            if( conn != null )
                conn.disconnect();
        }
    }
}
