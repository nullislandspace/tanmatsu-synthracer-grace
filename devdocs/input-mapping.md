# Input Mapping

> Keyboard / gamepad bindings and the rationale behind them. Part of the [dev docs](README.md).

## Input Mapping

### Original *Race The Sun* controls

| Action | Keyboard | Gamepad |
|---|---|---|
| Steer left | A / Left arrow | D-pad left / left stick |
| Steer right | D / Right arrow | D-pad right / right stick |
| Use pickup (jump / shield / boost) | Space | A (south button) |
| Barrel roll (defensive) | Rapid alternating left↔right input | Same |
| Pause | Esc | Start |
| Menu confirm | Enter / Space | A |
| Menu cancel / back | Esc | B |
| Throttle | (none — automatic acceleration) | (none) |

### Tanmatsu mapping (Race the Synth)

The Tanmatsu has a full QWERTY keyboard, a D-pad/navigation cluster
(LEFT/RIGHT/UP/DOWN), gamepad face buttons (A/B/X/Y), F1–F12, and
volume keys. We use polled reads for steering (smoother than waiting on
event repeats) and the event queue for everything else.

| Action | Tanmatsu input | Read mode | Notes |
|---|---|---|---|
| Steer left | `BSP_INPUT_NAVIGATION_KEY_LEFT` **or** keyboard `a` **or** `BSP_INPUT_SCANCODE_ESC` (in-game only) | Polled (`bsp_input_read_navigation_key`) and polled scancode | Both edges accepted; held = continuous turn. ESC is the left thumb-rest key — ergonomic for long sessions. |
| Steer right | `BSP_INPUT_NAVIGATION_KEY_RIGHT` **or** keyboard `d` **or** `BSP_INPUT_SCANCODE_BACKSPACE` (in-game only) | Polled | Backspace is the right thumb-rest key — ergonomic mirror to ESC. |
| Use pickup | `BSP_INPUT_SCANCODE_SPACE` **or** `BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A` | Event (press edge) | Cycles through inventory: jump → shield → checkpoint, whichever the player has. Or we expose three slots — TBD; default to jump-first. |
| Barrel roll | Detected in `input.c` from rapid LEFT-edge → RIGHT-edge (any of the three left/right bindings) within 250 ms | Event timing | Cosmetic + small dodge invulnerability frame. Optional polish — defer to Phase 13 if implementation is hairy. |
| Pause | `BSP_INPUT_NAVIGATION_KEY_F4` | Event (press edge) | First press → pause overlay; second press resumes. **Note**: ESC was originally planned for pause, but is now reused as the left-steer thumb key during play; F4 is a free function key with no other role. |
| Title-menu navigate | `BSP_INPUT_NAVIGATION_KEY_UP` / `_DOWN` | Event (press edge) | |
| Title-menu confirm | `BSP_INPUT_SCANCODE_ENTER` **or** `_SPACE` **or** `GAMEPAD_A` | Event (press edge) | |
| Title-menu cancel | `BSP_INPUT_SCANCODE_ESC` **or** `GAMEPAD_B` | Event (press edge) | ESC is steering only **during PLAYING**; in menus it reverts to its conventional cancel role. |
| Custom-seed entry | Digit keys `0`–`9` from `INPUT_EVENT_TYPE_KEYBOARD.ascii` | Event (ASCII) | Backspace edits text, Enter confirms, Esc cancels — Backspace and Esc revert to their conventional roles inside the seed-entry dialog (no steering active there). |
| Volume up | `BSP_INPUT_NAVIGATION_KEY_VOLUME_UP` | Event (press edge) | Updates `system/speaker.volume` or `system/hp.volume`, applies via `bsp_audio_set_volume()`, briefly shows a HUD volume bar |
| Volume down | `BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN` | Event (press edge) | Same |
| Audio-jack toggle | `BSP_INPUT_ACTION_TYPE_AUDIO_JACK` | Event (action) | Re-reads NVS for the new output, calls `bsp_audio_set_amplifier(!hp_inserted)` and `bsp_audio_set_volume()` |
| Restart to launcher | `BSP_INPUT_NAVIGATION_KEY_F1` | Event (press edge) | Always available; matches template convention |
| Backlight dim / bright | `F2` / `F3` | Event (press edge) | Matches template convention |
| Power button | `BSP_INPUT_ACTION_TYPE_POWER_BUTTON` | Event (action) | Treated as exit-to-launcher (mirrors videoplayer) |

### Modal binding: ESC and Backspace

ESC and Backspace have **state-dependent meaning**, decided in
`input.c` based on `game.state`:

| State | ESC behavior | Backspace behavior |
|---|---|---|
| `STATE_TITLE` | menu cancel / back | (unbound, ignored) |
| `STATE_MENU_SEED` (custom-seed dialog) | cancel dialog | edit / delete digit |
| `STATE_PLAYING` | **steer left** (polled) | **steer right** (polled) |
| `STATE_PAUSED` | resume play | (unbound) |
| `STATE_GAME_OVER` | (unbound) | (unbound) |

This is unambiguous — the player is never steering during a menu and
never editing text mid-run, so the role swap never collides with itself.
The modal logic lives entirely in `input.c`'s polled-read path:
`is_steer_left = nav_left_held || a_held || (state==PLAYING && esc_held)`.

### Mapping rationale

- **Polled steering** matches `tanmatsu-placeinvaders-grace/main/main.c`
  (`bsp_input_read_navigation_key()` for LEFT/RIGHT). Event-queued
  steering depends on repeat-rate quirks and produces stutter.
- **Three steering options per side** (D-pad, WASD, Esc/Backspace)
  because the Tanmatsu's grip varies by player: D-pad for one-handed
  play, WASD for keyboard-style two-handed grip, and Esc/Backspace as
  thumb-rest keys at the corners of the keyboard for long-session
  ergonomics. Cost is three polled reads per side per tick — trivial.
- **Single "use pickup" button** mirrors the original — Race The Sun has
  one action button that consumes whichever pickup is at the head of the
  inventory queue. Cleaner than dedicated jump/shield/checkpoint keys.
- **Barrel roll detection** is *not* on the critical path. Phase 1–8
  ship without it; Phase 13 polish adds it if the timing window is
  tunable enough to feel right.
- **F1 always exits** — convention from template and every sibling
  graceloader game. **Pause moved to F4** so ESC can serve as the
  left-thumb steering key during play.
- **Volume keys are global** — mirrors launcher and videoplayer; writes
  to the shared `"system"` NVS namespace so the change persists across
  apps. Confirmed by reading
  `tanmatsu-launcher/main/global_event_handler.c:58–67` and the volume
  helpers at
  `tanmatsu-launcher/managed_components/nicolaielectronics__tanmatsu-settings/src/nvs_settings_hardware.c:32–46`.

### Things deliberately NOT mapped

- **Throttle / brake** — original has none, we don't either. Speed is
  managed by the sun timer + boost pickups + shadow slowdown.
- **Camera control** — fixed third-person chase camera. No swing.
- **Joystick stick press** (`BSP_INPUT_NAVIGATION_KEY_JOYSTICK_PRESS`) —
  reserved; ignored for now. Could map to "use pickup" as an alternate.
- **Multi-button chords** — none. Every action is a single key.

---

