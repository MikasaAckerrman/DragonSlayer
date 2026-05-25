# TrackerScheme.res

This directory contains the VGUI2-style scheme resource file (`TrackerScheme.res`) used by
the mainui menu library to define colors, borders, and fonts.

## Installation paths

- **Android**: The file is bundled into the APK via the `game_assets/resource/` assets
  directory (configured in `android/app/build.gradle.kts`). The engine's asset manager
  finds it at `resource/TrackerScheme.res` relative to the assets root.

- **Desktop**: The file should be placed at `<gamedir>/resource/TrackerScheme.res`:
  - `valve/resource/TrackerScheme.res` for Half-Life
  - `cstrike/resource/TrackerScheme.res` for Counter-Strike 1.6
  - For distribution, it can also be included in `extras.pk3`.

## Overriding

Users can override the bundled version by placing their own copy in the game directory
on the device's storage. External storage is searched before APK assets, so user-placed
files take priority.
