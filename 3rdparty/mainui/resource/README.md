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

## Font Customization

TrackerScheme.res defines fonts under the `Fonts` section. Each font entry has an alias
and specifies a font face name, height (tall), and weight. The built-in aliases are:

- **Default** - Used for general UI text (e.g. table headers)
- **Title** - Used for window title bar text
- **Small** - Used for smaller UI elements and labels

The font name maps to a TTF file via FontManager. For example, the name `Tahoma` maps
to `gfx/fonts/tahoma.ttf`. To use a custom font, drop a compatible TTF file into
`<gamedir>/gfx/fonts/` with the matching filename (e.g. `tahoma.ttf` for `Tahoma`).

At startup, SchemeManager parses the font definitions and later creates HFont handles
through CFontBuilder when `CreateFonts()` is called during `UI_VidInit`. Controls then
query fonts by alias via `GetFont()` and fall back to `uiStatic.hDefaultFont` if the
scheme font is not available.
