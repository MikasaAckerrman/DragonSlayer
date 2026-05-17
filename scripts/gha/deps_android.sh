#!/bin/bash
set -euo pipefail

cd "$GITHUB_WORKSPACE" || exit 1

# Required by the SDL2 download step. If the workflow forgets to set it,
# wget would happily fetch an empty 404 page and tar would later fail
# with a confusing "gzip: stdin: unexpected end of file" - fail loudly here.
: "${SDL_VERSION:?SDL_VERSION is not set; check the env: block in the workflow}"

ANDROID_COMMANDLINE_TOOLS_VER="14742923"
ANDROID_BUILD_TOOLS_VER="36.0.0"
ANDROID_PLATFORM_VER="android-35"
ANDROID_NDK_VERSION="29.0.14206865"

BUILD_ONCE_RUN_WITH_SPECIFIC_RUNTIME_VERSION="jbrsdk-21.0.10-linux-x64-b1163.110"

echo "Download JDK 17"
wget --tries=3 --retry-connrefused --waitretry=5 \
	"https://cache-redirector.jetbrains.com/intellij-jbr/$BUILD_ONCE_RUN_WITH_SPECIFIC_RUNTIME_VERSION.tar.gz" -qO- | tar -xzf -
mv $BUILD_ONCE_RUN_WITH_SPECIFIC_RUNTIME_VERSION java

export JAVA_HOME=$GITHUB_WORKSPACE/java
export PATH=$PATH:$JAVA_HOME/bin

echo "Download hlsdk-portable"
git clone --depth 1 --recursive https://github.com/FWGS/hlsdk-portable -b mobile_hacks 3rdparty/hlsdk-portable

echo "Download SDL $SDL_VERSION"
pushd 3rdparty
wget --tries=3 --retry-connrefused --waitretry=5 \
	"https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz" -qO- | tar -xzf -
mv "SDL2-$SDL_VERSION" SDL
popd

echo "Download Android SDK"
mkdir -p sdk
pushd sdk
wget --tries=3 --retry-connrefused --waitretry=5 \
	"https://dl.google.com/android/repository/commandlinetools-linux-${ANDROID_COMMANDLINE_TOOLS_VER}_latest.zip" -qO sdk.zip
unzip -q sdk.zip
mv cmdline-tools tools
mkdir -p cmdline-tools
mv tools cmdline-tools/tools
unset ANDROID_SDK_ROOT
export ANDROID_HOME=$GITHUB_WORKSPACE/sdk
export PATH=$PATH:$ANDROID_HOME/tools:$ANDROID_HOME/tools/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/tools/bin
popd

echo "Download all needed tools and Android NDK"
# yes(1) writes 'y' forever; when sdkmanager finishes accepting licenses
# and closes stdin, yes gets SIGPIPE and exits non-zero. With
# set -o pipefail that kills the whole script. Wrap the LHS so the
# brace group always exits 0; pipefail still catches real failures from
# sdkmanager itself. (who even reads licenses? :)
{ yes 2>/dev/null || true; } | sdkmanager --licenses > /dev/null 2>&1
sdkmanager --install "build-tools;${ANDROID_BUILD_TOOLS_VER}" platform-tools "platforms;${ANDROID_PLATFORM_VER}" "ndk;${ANDROID_NDK_VERSION}"
