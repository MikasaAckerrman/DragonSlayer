#!/bin/bash
set -euo pipefail

# Slayer3D Android build script.
#
# The 'continuous' build type in android/app/build.gradle.kts already has a
# signingConfig attached (androidDebugKey), so AGP signs the APK during
# assembleContinuous and writes it to:
#     android/app/build/outputs/apk/continuous/app-continuous.apk
#
# This script just locates that output and copies it into artifacts/ for
# the GitHub workflow to upload. It does NOT run apksigner manually -
# that was what caused the previous CI failure (it tried to read
# app-continuous-unsigned.apk, which AGP no longer produces when a
# signing config is set on the build type).

unset ANDROID_SDK_ROOT
export JAVA_HOME="$GITHUB_WORKSPACE/java"
export ANDROID_HOME="$GITHUB_WORKSPACE/sdk"
export PATH="$PATH:$JAVA_HOME/bin:$ANDROID_HOME/tools:$ANDROID_HOME/tools/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/tools/bin"

# 1. Build the APK.
pushd android
./gradlew assembleContinuous --no-daemon
popd

# 2. Locate the produced APK. AGP filename has changed across versions;
#    auto-detecting is more future-proof than hard-coding it.
APK_DIR="android/app/build/outputs/apk/continuous"
APK_PATH="$(find "$APK_DIR" -maxdepth 1 -type f -name '*.apk' ! -name '*-unsigned.apk' | head -n 1)"

if [ -z "$APK_PATH" ]; then
	echo "::error::No signed APK found under $APK_DIR"
	echo "Directory contents:"
	ls -la "$APK_DIR" || true
	exit 1
fi

echo "Picked APK: $APK_PATH"

# 3. Stage artifacts.
mkdir -p artifacts/
cp "$APK_PATH" artifacts/Slayer3D-android.apk

# 4. Bundle ProGuard/R8 mappings if present (kept optional - if R8 is
#    disabled or the directory is missing, the build should still ship
#    a successful APK).
MAPPINGS_DIR="android/app/build/outputs/mapping/continuous"
if [ -d "$MAPPINGS_DIR" ] && [ -n "$(ls -A "$MAPPINGS_DIR" 2>/dev/null || true)" ]; then
	tar -czf artifacts/Slayer3D-android-mappings.tar.gz -C "$MAPPINGS_DIR" .
else
	echo "No mappings directory at $MAPPINGS_DIR, skipping."
fi

echo "Artifacts prepared:"
ls -la artifacts/
