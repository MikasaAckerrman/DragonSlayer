#!/bin/bash
set -euo pipefail

# Slayer3D Android build script.
#
# Builds the 'continuous' build type (single APK, no product flavors).
# AGP signs the APK during assembly (androidDebugKey signing config is
# attached to the continuous build type).
#
# Output:
#   artifacts/Slayer3D-android.apk

unset ANDROID_SDK_ROOT
export JAVA_HOME="$GITHUB_WORKSPACE/java"
export ANDROID_HOME="$GITHUB_WORKSPACE/sdk"
export PATH="$PATH:$JAVA_HOME/bin:$ANDROID_HOME/tools:$ANDROID_HOME/tools/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/tools/bin"

# Quick-build mode: only arm64-v8a architecture
GRADLE_ABI_ARGS=""
if [ "${SLAYER_BUILD_MODE:-full}" = "quick" ]; then
	GRADLE_ABI_ARGS="-Pslayer.abiFilter=arm64-v8a"
fi

# 1. Build APK.
pushd android
./gradlew assembleContinuous $GRADLE_ABI_ARGS --no-daemon
popd

# 2. Locate the produced APK.
APK_DIR="android/app/build/outputs/apk/continuous"
APK="$(find "$APK_DIR" -maxdepth 1 -type f -name '*.apk' ! -name '*-unsigned.apk' | head -n 1)"

if [ -z "$APK" ]; then
	echo "::error::No signed APK found under $APK_DIR"
	echo "Directory contents:"
	ls -la "$APK_DIR" || true
	exit 1
fi

echo "Picked APK: $APK"

# 3. Stage artifacts.
mkdir -p artifacts/
cp "$APK" artifacts/Slayer3D-android.apk

# 4. Bundle ProGuard/R8 mappings if present.
MAPPINGS_DIR="android/app/build/outputs/mapping/continuous"
if [ -d "$MAPPINGS_DIR" ] && [ -n "$(ls -A "$MAPPINGS_DIR" 2>/dev/null || true)" ]; then
	tar -czf artifacts/Slayer3D-android-mappings.tar.gz -C "$MAPPINGS_DIR" .
else
	echo "No mappings directory at $MAPPINGS_DIR, skipping."
fi

echo "Artifacts prepared:"
ls -la artifacts/
