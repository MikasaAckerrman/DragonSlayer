#!/bin/bash
set -euo pipefail

# Slayer3D Android build script (dual-flavor).
#
# Builds two product flavors (scoped + legacy) of the 'continuous' build type.
# - scoped: uses Android scoped storage (Android/data/.../)
# - legacy: uses legacy external storage (/sdcard/xash)
#
# AGP signs both APKs during assembly (androidDebugKey signing config is
# attached to the continuous build type).
#
# Output:
#   artifacts/Slayer3D-android.apk         (scoped flavor)
#   artifacts/Slayer3D-android-legacy.apk  (legacy flavor)

unset ANDROID_SDK_ROOT
export JAVA_HOME="$GITHUB_WORKSPACE/java"
export ANDROID_HOME="$GITHUB_WORKSPACE/sdk"
export PATH="$PATH:$JAVA_HOME/bin:$ANDROID_HOME/tools:$ANDROID_HOME/tools/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/tools/bin"

# Quick-build mode: only scoped flavor, only arm64-v8a architecture
GRADLE_ABI_ARGS=""
if [ "${SLAYER_BUILD_MODE:-full}" = "quick" ]; then
	GRADLE_ABI_ARGS="-Pslayer.abiFilter=arm64-v8a"
fi

# 1. Build APKs.
pushd android
if [ "${SLAYER_BUILD_MODE:-full}" = "quick" ]; then
	./gradlew assembleScopedContinuous $GRADLE_ABI_ARGS --no-daemon
else
	./gradlew assembleScopedContinuous assembleLegacyContinuous $GRADLE_ABI_ARGS --no-daemon
fi
popd

# 2. Locate the produced APKs.
# AGP may output to concatenated (scopedContinuous) or nested (scoped/continuous) paths
# depending on the AGP version. Try concatenated first, fall back to nested.
SCOPED_APK_DIR="android/app/build/outputs/apk/scopedContinuous"
if [ ! -d "$SCOPED_APK_DIR" ]; then
	SCOPED_APK_DIR="android/app/build/outputs/apk/scoped/continuous"
fi

LEGACY_APK_DIR="android/app/build/outputs/apk/legacyContinuous"
if [ ! -d "$LEGACY_APK_DIR" ]; then
	LEGACY_APK_DIR="android/app/build/outputs/apk/legacy/continuous"
fi

SCOPED_APK="$(find "$SCOPED_APK_DIR" -maxdepth 1 -type f -name '*.apk' ! -name '*-unsigned.apk' | head -n 1)"

if [ -z "$SCOPED_APK" ]; then
	echo "::error::No signed scoped APK found under $SCOPED_APK_DIR"
	echo "Directory contents:"
	ls -la "$SCOPED_APK_DIR" || true
	exit 1
fi

echo "Picked scoped APK: $SCOPED_APK"

if [ "${SLAYER_BUILD_MODE:-full}" = "quick" ]; then
	echo "Quick build mode: only scoped flavor built"
else
	LEGACY_APK="$(find "$LEGACY_APK_DIR" -maxdepth 1 -type f -name '*.apk' ! -name '*-unsigned.apk' | head -n 1)"

	if [ -z "$LEGACY_APK" ]; then
		echo "::error::No signed legacy APK found under $LEGACY_APK_DIR"
		echo "Directory contents:"
		ls -la "$LEGACY_APK_DIR" || true
		exit 1
	fi

	echo "Picked legacy APK: $LEGACY_APK"
fi

# 3. Stage artifacts.
mkdir -p artifacts/
cp "$SCOPED_APK" artifacts/Slayer3D-android.apk
if [ "${SLAYER_BUILD_MODE:-full}" != "quick" ]; then
	cp "$LEGACY_APK" artifacts/Slayer3D-android-legacy.apk
fi

# 4. Bundle ProGuard/R8 mappings if present for each variant.
SCOPED_MAPPINGS_DIR="android/app/build/outputs/mapping/scopedContinuous"
if [ ! -d "$SCOPED_MAPPINGS_DIR" ]; then
	SCOPED_MAPPINGS_DIR="android/app/build/outputs/mapping/scoped/continuous"
fi
if [ -d "$SCOPED_MAPPINGS_DIR" ] && [ -n "$(ls -A "$SCOPED_MAPPINGS_DIR" 2>/dev/null || true)" ]; then
	tar -czf artifacts/Slayer3D-android-mappings.tar.gz -C "$SCOPED_MAPPINGS_DIR" .
else
	echo "No mappings directory at $SCOPED_MAPPINGS_DIR, skipping."
fi

LEGACY_MAPPINGS_DIR="android/app/build/outputs/mapping/legacyContinuous"
if [ ! -d "$LEGACY_MAPPINGS_DIR" ]; then
	LEGACY_MAPPINGS_DIR="android/app/build/outputs/mapping/legacy/continuous"
fi
if [ "${SLAYER_BUILD_MODE:-full}" != "quick" ] && [ -d "$LEGACY_MAPPINGS_DIR" ] && [ -n "$(ls -A "$LEGACY_MAPPINGS_DIR" 2>/dev/null || true)" ]; then
	tar -czf artifacts/Slayer3D-android-legacy-mappings.tar.gz -C "$LEGACY_MAPPINGS_DIR" .
else
	echo "No mappings directory at $LEGACY_MAPPINGS_DIR, skipping."
fi

echo "Artifacts prepared:"
ls -la artifacts/
