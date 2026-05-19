# Appendix B — Tanmatsu API Reference

> Cached cheat-sheet of the SDK APIs used by the project. Part of the [dev docs](README.md).

## Appendix B — Tanmatsu API Reference (cached)

Cheat-sheet of the most relevant APIs found during exploration of the SDK
headers under `tanmatsu-synthracer-grace/include/`. Cited so we don't have
to re-grep next session.

### PAX graphics (2D only — no 3D math, hand-rolled projection required)

- `pax_buf_init(&fb, NULL, w, h, PAX_BUF_24_888RGB)` — `include/pax_gfx.h:94`
- `pax_buf_set_orientation()`, `pax_buf_get_pixels()` — same header
- 2D matrix stack: `pax_push_2d`, `pax_pop_2d`, `pax_apply_2d` — same header
- `pax_simple_tri/rect/line/circle` — `include/shapes/`
- `pax_draw_shape(buf, color, npts, points)` — triangulates each call.
- `pax_triang_concave(&indices, npts, points)` →
  `pax_draw_shape_triang(buf, color, npts, points, ntris, indices)` —
  pre-triangulate path. `include/pax_shapes.h:99,108`.
- `pax_draw_text`, `pax_center_text`, `pax_text_size` — `include/pax_text.h`
- Built-in fonts: `pax_font_sky`, `pax_font_sky_mono`, `pax_font_marker`,
  `pax_font_saira_condensed`, `pax_font_saira_regular` — `include/pax_fonts.h`
- `pax_clip(buf, x, y, w, h)` / `pax_noclip(buf)` for HUD scissoring.

### BSP — display, input, audio, LEDs

- `bsp_device_initialize(cfg)`; `bsp_device_restart_to_launcher()` — `bsp/device.h`
- `bsp_display_get_parameters()`, `bsp_display_blit(x,y,w,h,buf)` —
  `bsp/display.h:36, 79`
- Vsync: `bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING)` +
  `bsp_display_get_tearing_effect_semaphore(&sem)` then
  `xSemaphoreTake(sem, pdMS_TO_TICKS(50))`. Pattern from
  `tanmatsu-floppybird-grace/main/main.c:519–527, 748`.
- `bsp_input_get_queue(&q)` (event-driven), `bsp_input_read_navigation_key()`
  / `bsp_input_read_scancode()` / `bsp_input_read_action()` (polled) —
  `bsp/input.h:257, 273, 277, 281`.
- Nav key constants: `BSP_INPUT_NAVIGATION_KEY_{LEFT,RIGHT,UP,DOWN,F1..F12,
  GAMEPAD_A,B,X,Y,JOYSTICK_PRESS,VOLUME_UP,VOLUME_DOWN}` — `bsp/input.h:148–196`
- Action types include `BSP_INPUT_ACTION_TYPE_AUDIO_JACK`,
  `BSP_INPUT_ACTION_TYPE_SD_CARD`, `BSP_INPUT_ACTION_TYPE_POWER_BUTTON`.
- `bsp_audio_initialize(rate)`, `bsp_audio_get_i2s_handle(&h)`,
  `bsp_audio_set_amplifier(bool)`, `bsp_audio_set_volume(float pct)` —
  `bsp/audio.h`.
- `bsp_led_set_pixel(idx, rgb)`, `bsp_led_send()`, `bsp_led_set_mode(auto)`,
  `bsp_led_set_brightness(pct)` — `bsp/led.h`.

### NVS

- `nvs_flash_init()` — `nvs_flash.h:78`
- `nvs_open(ns, mode, &h)`, `nvs_close(h)`, `nvs_commit(h)` — `nvs.h:162, 590, 577`
- `nvs_set_u8/u16/u32/u64/i32/str/blob` and matching `nvs_get_*` —
  `nvs.h:233..501`. Key max 15 chars.
- **Shared "system" namespace keys** (used by launcher; we must use these
  for volume to integrate properly):
  - `"speaker.volume"` (u8 percentage 0..100)
  - `"hp.volume"` (u8 percentage 0..100, for headphones)
  - Helpers in launcher source at
    `tanmatsu-launcher/managed_components/nicolaielectronics__tanmatsu-settings/src/nvs_settings_hardware.c`.

### Time

- `time(NULL)` / `localtime()` / `clock_gettime(CLOCK_REALTIME, ...)` for
  wall-clock. `time.h`.
- `esp_timer_get_time()` returns int64 microseconds since boot —
  `esp_timer.h:223`. Use for frame timing and PRNG fallback seed.

### Reference projects (for patterns)

- `tanmatsu-launcher/main/synthwave.{c,h}` — synthwave backdrop +
  scrolling grid (we copy & refactor this).
- `tanmatsu-launcher/main/global_event_handler.c:19–67` — exact volume +
  audio-jack handling.
- `tanmatsu-floppybird-grace/main/main.c` — game loop, vsync, NVS
  highscore, audio mixer task, xorshift32 PRNG.
- `tanmatsu-placeinvaders-grace/main/{game,render,audio}.c, sprites.h,
  sounds.h` — multi-file layout, sprite/sound embedding.
- `tanmatsu-thecube-grace/main/{main,renderer}.c` — software 3D-ish
  rendering pattern, fixed-step pacing.
- `tanmatsu-videoplayer/main/main.c:769–787` — alternate
  volume-key handling pattern (in-app only).

---

