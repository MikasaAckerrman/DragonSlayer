#!/bin/bash

unset ANDROID_SDK_ROOT
export JAVA_HOME=$GITHUB_WORKSPACE/java
export ANDROID_HOME=$GITHUB_WORKSPACE/sdk
export PATH=$PATH:$JAVA_HOME/bin:$ANDROID_HOME/tools:$ANDROID_HOME/tools/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/tools/bin

# Slayer3D: build the dedicated build type so the resulting APK has its own
# applicationId (su.xash.engine.slayer3d) and does not collide with stock
# Xash3D-FWGS that may already be installed on the user's device.
BUILD_TYPE="slayer3d"
BUILD_TYPE_CAPITALIZED="Slayer3d"

pushd android || exit 1

./gradlew "assemble${BUILD_TYPE_CAPITALIZED}" --no-daemon || exit 1

pushd "app/build/outputs/apk/${BUILD_TYPE}" || exit 1

# This build type already has a signingConfig, so the APK Gradle produced is
# named app-${BUILD_TYPE}.apk (already signed). Older variants emitted
# app-${BUILD_TYPE}-unsigned.apk and required a manual apksigner step. Handle
# both for forward compatibility.
SIGNED_APK="app-${BUILD_TYPE}-signed.apk"
if [ -f "app-${BUILD_TYPE}.apk" ]; then
	cp "app-${BUILD_TYPE}.apk" "$SIGNED_APK"
elif [ -f "app-${BUILD_TYPE}-unsigned.apk" ]; then
	"$ANDROID_HOME/build-tools/36.0.0/apksigner" sign \
		--ks "$GITHUB_WORKSPACE/android/debug.keystore" \
		--ks-key-alias androiddebugkey \
		--ks-pass pass:android \
		--key-pass pass:android \
		--out "$SIGNED_APK" "app-${BUILD_TYPE}-unsigned.apk" || exit 1
else
	echo "ERROR: no APK found in app/build/outputs/apk/${BUILD_TYPE}" >&2
	ls -la
	exit 1
fi

popd || exit 1
popd || exit 1

mkdir -p artifacts/

mv "android/app/build/outputs/apk/${BUILD_TYPE}/app-${BUILD_TYPE}-signed.apk" artifacts/slayer3d-android.apk

# Mappings only exist if minify was actually run (release-style buildType).
MAPPING_DIR="android/app/build/outputs/mapping/${BUILD_TYPE}"
if [ -d "$MAPPING_DIR" ]; then
	tar -cJvf artifacts/slayer3d-android-mappings.tar.zst -C "$MAPPING_DIR" '.'
fi
