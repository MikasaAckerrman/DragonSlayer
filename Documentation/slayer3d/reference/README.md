# Reference assets

Kept in git on purpose. These previously lived only in chat, and then only in a
loose folder outside the repository, where any tidy-up would have destroyed
them. The measurements in `../scoreboard-pc-reference.md` are derived from
`pc-ref-oldschoolcs-1920x1080.jpg`, so losing it would mean losing the ability
to check or redo that work.

| File | What it is |
|---|---|
| `pc-ref-oldschoolcs-1920x1080.jpg` | PC CS 1.6 ScorePanel, 1920×1080, "OLD SCHOOL CS" server. **The canonical scoreboard reference** — every number in `scoreboard-pc-reference.md` was measured from these pixels. |
| `android-ours-2026-07-26.jpg` | Our Android scoreboard, intermediate state, for before/after comparison. |
| `slayer_diag-09bc9881.log` | Diagnostic log from build `09bc9881`, kept because it documents the avatar bug *before* the root cause was found (`Reset()` tearing down the JNI binding). |

Still missing: the second PC reference the user cited, "НОВИЧОК 18+ [Free VIP]"
(Russian column headers `Счет / Смертей / Задержка`). It only ever existed in
chat. It is needed to confirm the column geometry under the wider Russian
labels, since the English reference above cannot settle that.
