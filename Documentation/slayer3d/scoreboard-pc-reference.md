# PC CS 1.6 ScorePanel — measured reference

Everything here is **measured from pixels**, not estimated. Source screenshot:
`D:\projectGITHUB\_REFERENCE\pc-ref-oldschoolcs-1920x1080.jpg` (1920×1080, PC
CS 1.6, "OLD SCHOOL CS" server). Measurement scripts live in the session
scratchpad (`measure_ref.py`, `measure_ref2.py`, `sample_colors.py`).

This file exists because the numbers previously baked into
`cl_scoreboard_slayer.c` (0.82 board width, 0.6 glyph ratio, column fractions)
were eyeballed, and commit messages claiming "measured ~80-86%" overstated it.
These are the real values.

## Board

| Property | px @1920×1080 | fraction |
|---|---|---|
| Board width | 1619 | **84.3 % of screen width** |
| Board height | 833 | 77.1 % of screen height |
| Left margin | 157 | 8.18 % |
| Right margin | 144 | 7.50 % |

Engine currently uses `screen_w * 0.86` — within ~1.7 pp of the reference, i.e.
effectively correct; not worth churning.

## Vertical rhythm

| Property | px @1080p | notes |
|---|---|---|
| Player row pitch | **24** | 2.22 % of screen height |
| Glyph height | **14** | **glyph / pitch = 0.58** |
| Header band (board top → rule) | 32 | 3.84 % of board height |
| Team-header band (rule → rule) | 37 | |
| Rule → first player row | 8 | |

The 0.58 glyph-to-pitch ratio is the single most important number for "does it
look like PC". The engine currently produces ~0.75–0.85 (`row_pad =
charHeight/3`, clamped to 12), so **our rows are noticeably tighter than the
reference**. Loosening this is a layout-wide change that interacts with the
compression path, so it is deliberately NOT applied yet.

## Rules (separators)

| Rule | thickness @1080p | extent |
|---|---|---|
| Header separator | 1 px | full board width |
| Team rule (T / CT) | **2 px** (0.185 % of screen H) | **0.00 % → 99.94 % of board — NOT inset** |
| Spectator rule | 2 px | full board width |

Two findings drove the current fix:

1. The reference rules span the **full** board width. The engine was insetting
   the header separator by 3 % of board width on each side.
2. The reference rules read as **solid** — roughly twice the panel luma
   (e.g. T rule averages `(151,108,104)` against a `(68,56,49)` panel). The
   engine drew them at **alpha 100/255**, which is what made them look "too
   thin" to the eye even though the pixel thickness was already correct.

## Column right edges (right-aligned), as a fraction of board width

| Column | reference |
|---|---|
| HP | 59.2 % |
| Money | 70.4 % |
| Score | 79.4 % |
| Deaths | 88.5 % |
| Latency | 98.2 % |

The engine derives these right-to-left from **measured Russian label widths**
instead, because `Задержка`/`Смертей`/`Деньги` are wider than the English
labels on the reference. Hard-coding the fractions above would make the Russian
headers collide, so the reference column fractions are recorded here for
comparison but intentionally not applied verbatim.

## Text insets, as a fraction of board width

| Element | reference |
|---|---|
| Title (`OLD SCHOOL CS`) | 0.00 % (flush with board left edge) |
| Team name | 1.48 % |
| Player name | 1.30 % |

Team names and player names are effectively flush on the reference (team name
sits ~3 px further right). The engine draws both from `col_name_text_x`, so
they already share an origin.

## Still open

- The second reference the user cited ("НОВИЧОК 18+ [Free VIP]", Russian
  headers `Счет / Смертей / Задержка`) only ever existed in chat and is not on
  disk. It is needed to confirm the Russian-label column geometry.
- Row pitch (glyph/pitch 0.58) not yet applied — see above.
