# Font preview - what gets rendered with the embedded font

These three PNGs show **exactly** how the menu looks with the font now
compiled into `libmenu.so`. The colours, bevels, tab merge, button
corners and grain are produced by the same code the engine actually
runs (`mainui_cpp/vgui1/src/Frame.cpp`, `TabPanel.cpp`, `Button.cpp`,
`TrackerScheme.cpp`); only the typeface is the input.

## What font is inside

`mainui_cpp/font/embedded_source/source.ttf` -> compiled into
`libmenu.so` via `font/embedded_font_data.cpp`. Currently:

| Property | Value |
|----------|-------|
| Logical slot in TrackerScheme.res | `Tahoma` (canon CS 1.6 PC name) |
| Actual glyphs | DejaVu Sans Condensed (Book) v2.37 |
| Family lineage | Bitstream Vera (Carter, the same designer as Tahoma) |
| Coverage | Latin 95/95, Cyrillic 255/255, Greek |
| Size in .so | ~680 KB |
| Licence | Bitstream Vera + DejaVu (free to redistribute) |

The slot name is `Tahoma` so `TrackerScheme.res` references the canon
PC font name and no override is needed; the actual rendered glyphs come
from whatever TTF was last passed to `gen_embedded.py`.

## Files

| File | Size | What it shows |
|------|------|---------------|
| `01_full_dialog.png` | 800x600 | Full Options dialog: title bar, tab strip with the canon content-area bevel, Multiplayer page (avatar + logo with corner-flush buttons, name + password fields, gold checkmark), and the OK/Cancel/disabled-Apply row. |
| `02_text_samples.png` | 1000x720 | Cyrillic and Latin samples at the four engine sizes (11/12/14/36 pt) for tab labels, hint text, titlebar. |
| `03_glyph_diff.png` | 900x360 | 96 pt close-up of the 8 letters (`д ж ы я R Q g a`) where free-font substitutes diverge most from real Tahoma, with per-letter notes. |

## How close to PC Tahoma is this?

Honestly: **close in spirit, not identical**. DejaVu Sans Condensed
shares Tahoma's Carter lineage and the same "low-res UI sans-serif"
goal, and condensed proportions mean letter widths are similar. Glyph
shapes in Cyrillic (`д`, `ж`, `ы`) still differ visibly side-by-side
with the original. ~90 % match for casual viewing, ~70 % for pixel-
perfect.

## Getting 100 % real Tahoma

The engine still respects external overrides:

1. Obtain `tahoma.ttf` from any Microsoft Windows install you own
   (`C:\Windows\Fonts\tahoma.ttf`).
2. Place at `<gamedir>/gfx/fonts/tahoma.ttf` on the device.
3. Restart. The engine's filesystem prefers external files, so your
   real Tahoma overrides the embedded DejaVu.

This is permitted by the Microsoft EULA on a Windows-licensed device.
We do not ship Tahoma in the binary because Microsoft does not licence
it for redistribution.

## Verifying the rendering matches the engine

`.audit/render_preview.py` uses the same RGB tuples as
`TrackerScheme.cpp` and the same bevel order as `Frame::paintBackground`.
If you spot a discrepancy between a preview here and the actual
on-device build, the bug is in the preview script - not the engine -
and the script should be fixed first.
