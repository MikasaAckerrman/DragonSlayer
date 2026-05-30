# dragon-fonts

Bundled UI font for the DragonSlayer (Xash3D-FWGS / CS 1.6) Android build.

The Xash3D engine on Android has no Tahoma in its system fonts, so the VGUI1
menu font lookup falls through to a bitmap fallback that does not render
Cyrillic well. This package ships **Noto Sans Regular** under the file name
`gfx/fonts/tahoma.ttf` so that:

- `mainui_cpp/font/FontManager::FindFontDataFile("Tahoma")` resolves to it
  out of the box (no resource override required);
- the menu has a clean Latin + full Cyrillic + Greek glyph set on devices
  that have no system Tahoma;
- a user who actually owns Tahoma can drop their own
  `gfx/fonts/tahoma.ttf` into the gamedir to override our shipped one
  (external pak files take priority over packaged ones in the engine VFS).

## License

Noto Sans is licensed under the SIL Open Font License, Version 1.1. The
full license is in `LICENSE.OFL`. Source: https://github.com/notofonts

We are not affiliated with the Noto Project.

## Why not Tahoma directly?

Tahoma is proprietary Microsoft font software. It can be used legally on a
Windows machine that owns it, but it cannot be bundled and redistributed
in a public APK. Pirate ports do this anyway and risk DMCA takedowns.

## Files

- `gfx/fonts/tahoma.ttf` &mdash; Noto Sans Regular, ~825 KB, 4515 glyphs,
  full Cyrillic (U+0400..U+04FF), full Latin Extended.
- `LICENSE.OFL` &mdash; SIL Open Font License 1.1.
