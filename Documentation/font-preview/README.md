# Font preview - bundled Noto Sans Regular

These three PNGs show **exactly** how the menu will look on device with
the font we ship in the APK (`3rdparty/dragon-fonts/gfx/fonts/tahoma.ttf`,
which is **Noto Sans Regular** under that file name for compatibility with
`FontManager::FindFontDataFile("Tahoma")`).

The colours, bevels, tab merge, button corners and grain are the same code
the engine actually runs (`mainui_cpp/vgui1/src/Frame.cpp`,
`TabPanel.cpp`, `Button.cpp`, `TrackerScheme.cpp`). Only the typeface is
the variable - so what you see here is what you get on device, modulo
FreeType vs PIL micro-differences in glyph rasterisation.

## Files

| File | Size | What it shows |
|------|------|---------------|
| `01_full_dialog_noto.png` | 800x600 | The full Options dialog: title bar, tab strip with the new content-area bevel, Multiplayer page (avatar + logo with corner-flush buttons, name + password fields, a checkbox), and the OK/Cancel/Apply row with disabled-Apply rendered without a darker fill. |
| `02_text_samples_noto.png` | 1000x720 | Cyrillic and Latin glyph samples at 11/12/14/36 pt. Use this to see whether Noto's stroke weight and letter shapes are close enough to your reference. |
| `03_glyph_diff_noto.png` | 900x360 | The 8 individual glyphs that differ MOST from Tahoma (`д ж ы я R Q g a`) at 96 pt + per-letter notes. Look here if you want to know exactly which characters will give away that this is not Tahoma. |

## What about real Tahoma?

If after looking at these previews you decide Noto is not close enough to
canon, you can override our shipped font on the device:

1. Copy `tahoma.ttf` from any Windows machine
   (`C:\Windows\Fonts\tahoma.ttf`).
2. Place at `<gamedir>/gfx/fonts/tahoma.ttf` on the Android device.
3. Restart the engine. The external file overrides our APK-bundled Noto -
   no rebuild needed.

This is legally fine if you own the Windows licence the font came with.
We don't bundle Tahoma in the public APK because Microsoft owns it.

## Why Noto Sans?

- OFL 1.1 - free to redistribute in our APK.
- Full Latin (95/95 ASCII) + full Cyrillic (255/255) + Greek + Vietnamese.
- Modern hinting that holds up on Android's variable DPI.
- ~825 KB for one weight - acceptable for a UI font in a 50 MB+ APK.

## Verifying the rendering matches the engine

The script that rendered these (`.audit/render_preview.py`) uses the same
RGB tuples as `TrackerScheme.cpp` and the same bevel order as
`Frame::paintBackground`. If you spot a discrepancy between a preview here
and the actual on-device build, the bug is in the preview script - not
the engine - and we should fix the script first.
