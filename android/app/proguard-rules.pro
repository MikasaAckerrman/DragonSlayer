# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
#-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile

# Keep ALL members of XashActivity — many are called from native C via JNI
# (FindClass + GetStaticMethodID). R8 full-mode strips them despite selective
# keep rules because it cannot trace JNI call graphs. Nuclear option required.
-keep class su.xash.engine.XashActivity { *; }

# Slayer3D Steam Web API helper — invoked from C via FindClass +
# GetStaticMethodID. Without this keep rule R8 strips/renames the class
# in release builds and FindClass returns NULL at runtime, silently
# disabling batch avatar fetch.
-keep class su.xash.engine.SteamAPIHelper {
    static int fetchBatchAvatars(java.lang.String, java.lang.String, java.lang.String);
}

# Slayer3D Steam OpenID login activity — launched by Intent from
# XashActivity.startSteamLogin(). Intent uses string class lookup,
# so keep the class itself (members reachable via standard Activity
# lifecycle don't need explicit listing).
-keep class su.xash.engine.SteamLoginActivity

-keep,includedescriptorclasses,allowoptimization class org.libsdl.app.SDLInputConnection {
    void nativeCommitText(java.lang.String, int);
    void nativeGenerateScancodeForUnichar(char);
}

-keep,includedescriptorclasses class org.libsdl.app.SDLActivity {
    # for some reason these aren't compatible with allowoptimization modifier
    boolean supportsRelativeMouse();
    void setWindowStyle(boolean);
}

-keep,includedescriptorclasses,allowoptimization class org.libsdl.app.SDLActivity {
    java.lang.String nativeGetHint(java.lang.String); # Java-side doesn't use this, so it gets minified, but C-side still tries to register it
    boolean onNativeSoftReturnKey();
    void onNativeKeyboardFocusLost();
    boolean isScreenKeyboardShown();
    android.util.DisplayMetrics getDisplayDPI();
    java.lang.String clipboardGetText();
    boolean clipboardHasText();
    void clipboardSetText(java.lang.String);
    int createCustomCursor(int[], int, int, int, int);
    void destroyCustomCursor(int);
    android.content.Context getContext();
    boolean getManifestEnvironmentVariables();
    android.view.Surface getNativeSurface();
    void initTouch();
    boolean isAndroidTV();
    boolean isChromebook();
    boolean isDeXMode();
    boolean isTablet();
    void manualBackButton();
    int messageboxShowMessageBox(int, java.lang.String, java.lang.String, int[], int[], java.lang.String[], int[]);
    void minimizeWindow();
    int openURL(java.lang.String);
    void requestPermission(java.lang.String, int);
    int showToast(java.lang.String, int, int, int, int);
    boolean sendMessage(int, int);
    boolean setActivityTitle(java.lang.String);
    boolean setCustomCursor(int);
    void setOrientation(int, int, boolean, java.lang.String);
    boolean setRelativeMouseEnabled(boolean);
    boolean setSystemCursor(int);
    boolean shouldMinimizeOnFocusLoss();
    boolean showTextInput(int, int, int, int);
}

-keep,includedescriptorclasses,allowoptimization class org.libsdl.app.HIDDeviceManager {
    boolean initialize(boolean, boolean);
    boolean openDevice(int);
    int sendOutputReport(int, byte[]);
    int sendFeatureReport(int, byte[]);
    boolean getFeatureReport(int, byte[]);
    void closeDevice(int);
}

-keep,includedescriptorclasses,allowoptimization class org.libsdl.app.SDLAudioManager {
    int[] getAudioOutputDevices();
    int[] getAudioInputDevices();
    int[] audioOpen(int, int, int, int, int);
    void audioWriteFloatBuffer(float[]);
    void audioWriteShortBuffer(short[]);
    void audioWriteByteBuffer(byte[]);
    void audioClose();
    int[] captureOpen(int, int, int, int, int);
    int captureReadFloatBuffer(float[], boolean);
    int captureReadShortBuffer(short[], boolean);
    int captureReadByteBuffer(byte[], boolean);
    void captureClose();
    void audioSetThreadPriority(boolean, int);
    native int nativeSetupJNI();
    native void removeAudioDevice(boolean, int);
    native void addAudioDevice(boolean, int);
}

-keep,includedescriptorclasses,allowoptimization class org.libsdl.app.SDLControllerManager {
    void pollInputDevices();
    void pollHapticDevices();
    void hapticRun(int, float, int);
    void hapticStop(int);
}

# Unexpected reference to missing service class: META-INF/services/javax.annotation.processing.Processor.
-dontwarn javax.annotation.processing.Processor
-dontwarn javax.annotation.processing.AbstractProcessor
-dontwarn javax.annotation.processing.SupportedOptions
