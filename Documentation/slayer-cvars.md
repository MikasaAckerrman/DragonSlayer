# Slayer3D cvar reference

This page documents every cvar and console command added by the Slayer3D
client. Defaults are shown in parentheses.

---

## Crosshair damage indicator (`cl_hud_slayer.c`)

A small fading number is drawn directly under the crosshair every time you
take or deal damage. Useful as a quick "did the bullet land" cue without
looking down at the HUD.

### Cvars

| cvar | default | meaning |
|---|---|---|
| `slayer_damage_indicator` | `0` | `0` off, `1` show damage **taken** (works on every server), `2` show damage **dealt** (synth +N on kills + custom server msgs), `3` both |
| `slayer_damage_indicator_color` | `255 80 80 255` | `R G B A` 0..255 — color of the **taken** number (default red) |
| `slayer_damage_indicator_color_dealt` | `180 255 180 255` | `R G B A` 0..255 — color of the **dealt** number (default green) |
| `slayer_damage_indicator_time` | `2.0` | total visible duration in seconds (50% hold + 50% linear fade) |
| `slayer_damage_indicator_offset` | `40` | vertical pixel offset below screen center |
| `slayer_damage_indicator_scale` | `1.0` | text scale (currently informational; renderer uses console font size) |
| `slayer_damage_indicator_sound` | (empty) | sound path played on **dealt** events that exceed the threshold (e.g. `weapons/zoom.wav`). Empty disables the sound. |
| `slayer_damage_indicator_sound_thr` | `50` | minimum dealt-damage value that triggers the sound |
| `slayer_damage_indicator_sound_volume` | `0.7` | sound volume 0..1 |
| `slayer_damage_indicator_kill_amount` | `100` | fallback synthetic dealt-damage value when the local player kills someone (`0` = no synth, only real DmgInd usermsgs) |

### Notes on damage **dealt**

Stock CS 1.6 / GoldSrc servers do **not** broadcast per-shot dealt-damage
information to the attacker. There is no engine-level `DmgInd` usermsg.
The indicator therefore relies on three sources, in order:

1. **Custom server usermsgs**: `DamageReport`, `DmgInd`, `DmgDealt`, `DmgInfo`
   — many AMX Mod X plugins broadcast these. If your server has one, the
   indicator will show real per-shot numbers immediately.
2. **DeathMsg fallback**: when the local player kills someone, a synthetic
   `+slayer_damage_indicator_kill_amount` event fires. This guarantees you
   see a confirmation on the kill itself even on vanilla servers.
3. **Sound feedback**: any dealt event >= threshold plays the configured
   sound, useful as a "big-hit" audio cue.

If neither (1) nor (2) is available on your server, only damage **taken**
will register (since the engine's `Damage` usermsg is universal).

---

## Side-Game Strafe / SGS (`cl_sgs_slayer.c`)

Automated air-strafe for touch screens. While engaged, the engine injects
a synthetic sin-wave yaw oscillation into the outgoing usercmd plus a
synchronized `sidemove`, which produces server-side air-strafe acceleration
without the player needing to rapidly whip the camera by themselves.

### How to use it (mobile)

1. Set `slayer_sgs 1` to enable the feature.
2. Either bind a touch button to `+slayer_sgs` to gate it manually, or just
   start swiping in the air — any non-trivial yaw input within
   `slayer_sgs_swipe_window` seconds counts as a swipe and engages SGS.
3. By default (`slayer_sgs_air_only 1`), SGS only activates while you are
   airborne (after a jump). On the ground you keep normal aim.
4. Jump. While in the air, slide your finger left or right on the look
   pad — the engine adds the strafe oscillation for you. You should keep
   accelerating as long as you stay airborne.

The rendered view is not shaken: only the angle the server sees oscillates.
Your aim point in the world is unchanged.

### Cvars

| cvar | default | meaning |
|---|---|---|
| `slayer_sgs` | `0` | master enable. `0` = SGS completely off. `1` = enabled when engagement gate is met. |
| `slayer_sgs_strength` | `8.0` | yaw oscillation amplitude in **degrees**. Larger = stronger turn but more noticeable shake on the server-visible angle. Clamped to 0..45. |
| `slayer_sgs_freq` | `8.0` | oscillation frequency in **Hz**. Practical range 6..12. Too low = doesn't strafe. Too high = server tickrate can't see all the cycles and gain drops. |
| `slayer_sgs_swipe_window` | `0.20` | seconds after the last user swipe that SGS stays engaged. A short tap-and-release of the look pad keeps SGS running for ~0.2s. |
| `slayer_sgs_swipe_min` | `0.20` | minimum per-frame yaw delta (in degrees) that counts as a swipe. Filters out joystick drift / dead-zone noise. |
| `slayer_sgs_air_only` | `1` | `1` = only oscillate while airborne (`cl.local.onground == -1`). `0` = also oscillate on the ground (almost never what you want; will visibly shake your aim). |
| `slayer_sgs_sidemove` | `400.0` | sidemove magnitude pushed into the usercmd in sync with the yaw derivative. The same value WASD users get from holding `+moveleft`/`+moveright`. |

### Console commands

| command | meaning |
|---|---|
| `+slayer_sgs` | engage SGS while held. Bind to a touch button or key for a "hold to bhop-strafe" feel. |
| `-slayer_sgs` | release the held key. |

### Engagement gate

SGS injects oscillation **only** when **all** of these hold:

- `slayer_sgs == 1`
- `slayer_sgs_air_only == 0` **OR** the player is airborne
- **Either** `+slayer_sgs` is currently held **OR** the user produced
  yaw input within the last `slayer_sgs_swipe_window` seconds

If the gate opens and closes, the phase accumulator is reset to 0 to avoid
a sudden jolt on re-entry.

### Tuning recipes

- "I jump but barely accelerate": raise `slayer_sgs_strength` to `10`–`12`,
  or raise `slayer_sgs_freq` to `10`. Make sure `slayer_sgs_air_only 1` and
  the gate is firing (your finger really is moving).
- "View looks shaky / detected as bhop hack": lower `slayer_sgs_strength`
  to `5` and `slayer_sgs_freq` to `6`. Smaller, slower oscillation.
- "Engages even when I'm not swiping": raise `slayer_sgs_swipe_min` to
  `0.5` so micro-deltas from joystick drift don't count.
- "Disengages too fast between swipes": raise `slayer_sgs_swipe_window` to
  `0.4` for a longer trailing engagement.

### Why the cvars look the way they do

Air-strafe gain in GoldSrc requires the player to turn their view AND hold
the strafe key in the same direction. Doing this manually at ~10 Hz on a
phone is essentially impossible. SGS reproduces the ideal pattern:

```
yaw_offset(t)   = strength * sin( 2π * freq * t )
sidemove(t)     = sign( cos( 2π * freq * t ) ) * sidemove_magnitude
```

So when the (synthetic) yaw is increasing, the strafe key flips to "right",
and vice versa — the canonical bhop strafe pattern, perfectly synchronized.
