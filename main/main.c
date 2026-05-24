// Race the Synth — Tanmatsu graceloader app.
//
// Phase 4: TITLE → PLAYING → GAME_OVER state machine with AABB
// collision against the world's obstacle pool. F1 exits to the
// launcher from any state. See DEVELOPMENT.md for the full plan.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "se_direct565.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "attachments.h"
#include "backdrop.h"
#include "se_audio.h"
#include "audio_settings.h"
#include "controls_settings.h"
#include "game.h"
#include "game_ui.h"
#include "hal/lcd_types.h"
#include "hud.h"
#include "keybind_ui.h"
#include "icons.h"
#include "input.h"
#include "magicnumbers.h"
#include "se_music_procedural.h"
#include "nvs_flash.h"
#include "pax_gfx.h"
#include "render.h"
#include "se_text.h"
#include "save.h"
#include "se_bindings.h"
#include "se_hw.h"
#include "se_run.h"
#include "se_scene.h"
#include "se_ui.h"
#include "sfx/sfx_crash.h"
#include "sfx/sfx_engine_hum.h"
#include "sfx/sfx_gong.h"
#include "sfx/sfx_pickup_ding.h"
#include "sfx/sfx_pickup_plink.h"
#include "sfx/sfx_scrape.h"
#include "synthwave.h"
#include "world.h"

// The PPA synthwave compositor (clients, layer caches, sky/sun/mountain
// submits) lives in backdrop.{c,h}; its layout constants moved to
// magicnumbers.h ("Synthwave backdrop / PPA compositor layout").

static char const TAG[] = "racethesynth";

// The `fb` framebuffer bridge + the shared draw primitives + menu
// palette/geometry live in game_ui.{c,h} (shared with the hud / screens
// / keybind_ui modules). on_backdrop / on_render set `fb` to the engine's
// live back buffer each frame. The raw display geometry + the PPA layer
// caches are owned by backdrop.{c,h} now.

typedef enum {
    APP_STATE_SLOT_SELECT = 0,  // first state on boot: pick save slot 0..2
    APP_STATE_MENU,             // main menu: Daily/Seeded/Upgrade/Stats/Settings/Exit
    APP_STATE_SEED_INPUT,       // numeric entry for the custom seed
    APP_STATE_STATS_VIEW,       // text dump of the active slot's stats
    APP_STATE_UPGRADE,          // equip screen: slot list, shows what's fitted
    APP_STATE_UPGRADE_PICK,     // attachment picker for the selected slot
    APP_STATE_CREDITS,          // auto-scrolling credits roll
    APP_STATE_SETTINGS,         // settings submenu: Controls / Display / Audio
    APP_STATE_CONTROLS,         // controls list: gyro checkbox + 4 keybinds
                                // (the keybind "press a key" capture is a
                                // blocking engine call, not a state)
    APP_STATE_DISPLAY,          // brightness sliders: screen / keyboard / LEDs
    APP_STATE_AUDIO_SETTINGS,   // volume slider + three checkboxes: music/SFX/hum
    APP_STATE_PLAYING,
    APP_STATE_PAUSED,           // pause overlay: Resume / Abort run
    APP_STATE_CRASHING,         // post-crash: ship → spark shower, world still flows
    APP_STATE_STALL_OUT,        // post-stall: frozen scene held a beat before GAME_OVER
    APP_STATE_GAME_OVER,
    APP_STATE_CHECKPOINT_REDO,  // post-crash with a checkpoint: run rewound, "Re-Do" dialog
} app_state_t;

// Pause-menu entries (STATE_PAUSED).
enum {
    PAUSE_ENTRY_RESUME = 0,
    PAUSE_ENTRY_SETTINGS,
    PAUSE_ENTRY_ABORT,
    PAUSE_ENTRY_COUNT,
};

// Menu entry indices for STATE_MENU. Order is the visible order.
enum {
    MENU_ENTRY_DAILY = 0,
    MENU_ENTRY_SEEDED,
    MENU_ENTRY_UPGRADE,
    MENU_ENTRY_STATS,
    MENU_ENTRY_SETTINGS,
    MENU_ENTRY_CREDITS,
    MENU_ENTRY_EXIT,
    MENU_ENTRY_COUNT,
};

// Settings submenu entries (STATE_SETTINGS).
enum {
    SETTINGS_ENTRY_CONTROLS = 0,
    SETTINGS_ENTRY_DISPLAY,
    SETTINGS_ENTRY_AUDIO,
    SETTINGS_ENTRY_COUNT,
};
static int s_settings_cursor = SETTINGS_ENTRY_CONTROLS;

// Where the settings family (Settings / Controls / Audio / key
// capture) returns to on Esc, and which scene renders behind it.
// APP_STATE_MENU when opened from the main menu (synthwave backdrop,
// scrolling menu floor); APP_STATE_PAUSED when opened from the pause
// menu (frozen game scene behind, run stays logically paused).
static app_state_t s_settings_origin = APP_STATE_MENU;

// Controls-menu entries (STATE_CONTROLS). The gyro checkbox first,
// then the four remappable keybinds in CONTROL_KEY_* order so the
// row index past the checkbox maps straight to a controls_key_t.
enum {
    CONTROLS_ENTRY_GYRO = 0,
    CONTROLS_ENTRY_LEFT,
    CONTROLS_ENTRY_RIGHT,
    CONTROLS_ENTRY_ITEM,
    CONTROLS_ENTRY_PAUSE,
    CONTROLS_ENTRY_COUNT,
};
static int s_controls_cursor = CONTROLS_ENTRY_GYRO;

// Display-settings cursor entries (STATE_DISPLAY): three device-global
// brightness sliders, adjusted LEFT/RIGHT, persisted + applied by se_hw.
enum {
    DISPLAY_ENTRY_SCREEN = 0,
    DISPLAY_ENTRY_KEYBOARD,
    DISPLAY_ENTRY_LEDS,
    DISPLAY_ENTRY_COUNT,
};
static int s_display_cursor = DISPLAY_ENTRY_SCREEN;

// Audio-settings cursor entries (STATE_AUDIO_SETTINGS). Volume is a
// device-global slider (se_hw, active output); the rest are app toggles.
enum {
    AUDIO_ENTRY_VOLUME = 0,
    AUDIO_ENTRY_MUSIC,
    AUDIO_ENTRY_SFX,
    AUDIO_ENTRY_HUM,
    AUDIO_ENTRY_COUNT,
};
static int s_audio_cursor = AUDIO_ENTRY_VOLUME;

// Persistence state owned by main. The active slot is sticky for the
// app lifetime; we load on slot select and write after every run.
static save_data_t s_save        = {0};
static int         s_active_slot = -1;

// Menu / seed-input cursor state. Each is the per-screen cursor for
// the corresponding STATE_* entry — main loop resets the cursor on
// entry to each screen.
static int  s_slot_cursor  = 0;            // STATE_SLOT_SELECT cursor (0..2)
static int  s_menu_cursor  = MENU_ENTRY_DAILY;
static int  s_pause_cursor = PAUSE_ENTRY_RESUME;
static int  s_upgrade_cursor      = 0;     // APP_STATE_UPGRADE: selected slot row
static int  s_upgrade_slot        = 0;     // slot index (0/1) being edited in the picker
static int  s_upgrade_pick_cursor = 0;     // APP_STATE_UPGRADE_PICK: selected attachment
static char s_seed_buf[11] = {0};          // STATE_SEED_INPUT decimal seed (max 10 digits)
static int  s_seed_len     = 0;

// Game-over flavour text pool. The displayed line is chosen on the
// PLAYING → GAME_OVER transition and stays put until the next run
// ends; same flavour is rendered on every frame while GAME_OVER is
// active. Add more lines by appending to the array — the count is
// derived from `sizeof`, no manual update needed.
static char const* const gameover_flavours[] = {
    "Failure. Expected and inevitable.",
    "Death. The Great Divide.",
    "Infinity failed. Things end.",
};
#define GAMEOVER_FLAVOUR_COUNT  ((int)(sizeof(gameover_flavours) / sizeof(gameover_flavours[0])))

static int s_gameover_flavour_idx = 0;

// Run-instrumentation captured at run start, used by
// save_commit_run_end on transition into GAME_OVER. `s_run_play_seconds`
// accumulates per-PLAYING-frame `dt` so paused time is excluded from
// the saved duration stat (wallclock since start would count F4
// pauses in the play time, which we don't want).
static int64_t s_run_started_us   = 0;
static double  s_run_play_seconds = 0.0;
static int     s_peak_stage       = 1;
static int     s_run_was_custom   = 0;
static int64_t s_run_seed_used    = 0;

// Calendar day for this whole play session, as yyyymmdd, captured
// once at startup by capture_session_date(). Everything day-
// dependent — the daily world seed, the day-rollover check, the
// (future) daily challenges — reads THIS, never the RTC directly,
// so the "current day" can't shift under the player mid-session
// (e.g. playing across midnight). 0 means the RTC was unset at
// boot (year < 2024) — no usable date.
static int64_t s_session_date = 0;

// End-of-run hold timers + cause. `s_run_was_crash` records whether
// the run ended on a head-on crash (true) or a stall/sunset (false)
// so GAME_OVER commits the correct save reason. The two timers
// accumulate per-frame dt while the matching hold state is active.
static bool   s_run_was_crash   = false;
static double s_crash_anim_time = 0.0;
static double s_stall_hold_time = 0.0;

// Debug godmode (toggled with the G key): crash and stall end-of-run
// conditions are suppressed so the run can be slowed down / inspected.
static bool   s_godmode         = false;

// Crash spark shower runs until game_crash_tick reports all sparks
// spent (≈ the crash SFX length); this cap just guards against an
// abnormally long dt stranding the run in CRASHING.
#define CRASH_ANIM_SECONDS  0.70
// After a stall, hold the frozen scene this long so the player
// registers what happened before the game-over panel appears.
#define STALL_HOLD_SECONDS  1.5

// ---- Menu / dialog rendering --------------------------------------
//
// The menu palette (MENU_COL_*), the hand-laid screen geometry
// (MENU_TEXT_INSET etc.), and the shared primitives menu_left_x /
// draw_left / draw_menu_panel_size / draw_chevron live in game_ui.{c,h}.

// The keybind value rendering (scancode -> icon/name/glyph, the keycap
// drawing, and the se_ui controls_keybind_draw callback) lives in
// keybind_ui.{c,h}.

// Apply a signed step to a percentage and clamp to [0,100] — used by the
// brightness/volume settings sliders' get-step-set. Clamping in `int`
// before the uint8_t cast is the point: a downward step past 0 must land
// on 0, not wrap to ~250 (which se_hw would then clamp up to 100).
static uint8_t pct_step(uint8_t cur, int delta) {
    int n = (int)cur + delta;
    if (n < 0)   n = 0;
    if (n > 100) n = 100;
    return (uint8_t)n;
}

// The in-game HUD readouts + indicators (score / multiplier panel / stage
// readout + banner, the v=/sun=/god= debug readouts, and the boost / jump
// / shield / checkpoint symbols + pause hint) live in hud.{c,h}.

// (menu_draw retired: every list menu now renders through the engine's
// se_ui se_menu_draw. draw_keybind_value lives on as the game's CUSTOM
// value drawer for the Controls keybind rows.)

// Render the depth-buffered 3D scene for a run: clear the z-buffer,
// emit every obstacle and (optionally) the ship, then rasterize the
// deferred wireframe. The backdrop / floor / shadows must already be
// in the framebuffer — they are 2D layers drawn before this.
static void render_run_scene(world_state_t const* w, game_state_t const* g,
                             bool draw_ship) {
    scene_begin(fb);
    render_submit_obstacles(w);
    if (draw_ship) game_submit_ship(g);
    scene_flush();
}

// Render the scene behind a settings screen. Opened from the pause
// menu, the frozen game (obstacles + ship in their last positions)
// shows through — matching the pause overlay. Opened from the main
// menu there is no run, so this is a no-op and the synthwave
// backdrop drawn earlier in the frame stands.
static void draw_settings_scene(world_state_t const* w, game_state_t const* g) {
    if (s_settings_origin != APP_STATE_PAUSED) return;
    render_run_scene(w, g, true);
}

static void draw_game_over_overlay(void) {
    // Panel sized to bound "GAME OVER" (64 px, top 30 %) + flavour
    // text (22 px, top 46 %) + "press space to retry" (22 px, top
    // 58 %). Width widened to fit the longest flavour line at the
    // current text size.
    float const fbw = pax_buf_get_widthf(fb);
    float const fbh = pax_buf_get_heightf(fb);
    int   const pw  = (int)(fbw * 0.66f);
    int   const ph  = (int)(fbh * 0.46f);
    int   const px  = (int)((fbw - (float)pw) * 0.5f);
    int   const py  = (int)(fbh * 0.22f);
    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(fb);
    direct_565_dim_rect(pixels, fb->reverse_endianness, px, py, pw, ph);

    float const lx = menu_left_x(0.66f);
    draw_left(lx, fbh * 0.30f, 64.0f, 0xFFF71FF1u, "GAME OVER");
    draw_left(lx, fbh * 0.46f, 22.0f, 0xFF31FBFBu, gameover_flavours[s_gameover_flavour_idx]);
    draw_left(lx, fbh * 0.58f, 22.0f, MENU_COL_NORMAL, "press space to retry");
}

// "Re-Do from checkpoint" dialog (Phase 9.3). Drawn over the
// restored, frozen run scene — pause-like, the run has not ended.
// Wider panel than GAME OVER so the two-line quote fits.
static void draw_checkpoint_redo_overlay(void) {
    float const fbw = pax_buf_get_widthf(fb);
    float const fbh = pax_buf_get_heightf(fb);
    int   const pw  = (int)(fbw * 0.78f);
    int   const ph  = (int)(fbh * 0.50f);
    int   const px  = (int)((fbw - (float)pw) * 0.5f);
    int   const py  = (int)(fbh * 0.21f);
    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(fb);
    direct_565_dim_rect(pixels, fb->reverse_endianness, px, py, pw, ph);

    float const lx = menu_left_x(0.78f);
    draw_left(lx, fbh * 0.29f, 44.0f, 0xFF31FBFBu, "Re-Do from checkpoint");
    // Churchill — split across two lines to fit the panel width.
    draw_left(lx, fbh * 0.44f, 21.0f, MENU_COL_NORMAL,
              "Success is not final, failure is not fatal:");
    draw_left(lx, fbh * 0.51f, 21.0f, MENU_COL_NORMAL,
              "it is the courage to continue that counts.");
    draw_left(lx, fbh * 0.63f, 22.0f, 0xFFFFFFFFu, "press space to continue");
}

// Format an int64 Unix time as "YYYY-MM-DD HH:MM" into out. Empty
// string if t == 0.
static void format_unix(int64_t t, char* out, size_t out_size) {
    if (t <= 0) {
        snprintf(out, out_size, "—");
        return;
    }
    time_t    tt = (time_t)t;
    struct tm lt = {0};
    localtime_r(&tt, &lt);
    snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min);
}

// Slot-select screen — three rows with summary stats (or [new] when
// the slot has no save file).
static void draw_slot_select(void) {
    // Two-line rows (slot title + summary) don't fit the generic
    // list renderer, but the chevron gutter convention is shared:
    // labels always start at text_x, the ">" is painted separately
    // into the gutter, so selecting a slot never shifts its text.
    draw_menu_panel_size(0.80f, 0.94f);
    float const fbh       = pax_buf_get_heightf(fb);
    float const chevron_x = menu_left_x(0.80f);
    float const text_x    = chevron_x + MENU_CHEVRON_GUTTER;
    draw_left(text_x, fbh * 0.12f, 48.0f, MENU_COL_TITLE, "RACE THE SYNTH");
    draw_left(text_x, fbh * 0.22f, 22.0f, MENU_COL_NORMAL, "select save slot");

    float const row_h = 56.0f;
    float const top   = fbh * 0.34f;
    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        se_save_peek_t info  = {0};
        int            exist = se_save_peek(i, &info) == 0;
        bool const     sel   = (i == s_slot_cursor);
        pax_col_t      title_col = sel ? MENU_COL_HILITE : MENU_COL_NORMAL;
        pax_col_t      sub_col   = sel ? MENU_COL_NORMAL : MENU_COL_SUB;

        char title[32];
        snprintf(title, sizeof(title), "Slot %d", i + 1);

        // Sized to hold info.info (64) + "  " + when (64) + NUL without
        // truncation, so the "%s  %s" format below is always safe.
        char sub[160];
        if (exist) {
            // info.info is the game's display summary built in
            // save_write_slot; blank on slots written before se_save, so
            // fall back to just the timestamp in that case.
            char when[64];
            format_unix(info.timestamp, when, sizeof(when));
            if (info.info[0] != '\0') {
                snprintf(sub, sizeof(sub), "%s  %s", info.info, when);
            } else {
                snprintf(sub, sizeof(sub), "%s", when);
            }
        } else {
            snprintf(sub, sizeof(sub), "[new]");
        }

        float const y = top + (float)i * row_h;
        if (sel) {
            draw_chevron(chevron_x, y, 24.0f);
        }
        draw_left(text_x, y, 24.0f, title_col, title);
        draw_left(text_x + 16.0f, y + 28.0f, 16.0f, sub_col, sub);
    }

    draw_left(text_x, fbh * 0.92f, 14.0f, MENU_COL_HINT,
              "up / down to choose, enter to confirm, F1 to exit");
}

// (Every list menu — main, settings, controls, audio, pause, upgrade
// slots + picker — is now rendered by the engine's se_ui directly from
// its APP_STATE_* case, so the game's bespoke menu_draw / menu_view_t are
// gone. The Controls keybind rows use se_ui's CUSTOM kind + the
// controls_keybind_draw callback above to keep the key-icon logic
// game-side, and the "press a key" rebind modal is the engine's blocking
// se_ui_capture_key. What remains below are the hand-laid NON-list
// screens: slot-select, seed entry, stats, credits.)

// Seed-input screen — numeric entry, prefilled from last_custom_seed.
static void draw_seed_input(void) {
    draw_menu_panel_size(0.70f, 0.76f);
    float const fbh = pax_buf_get_heightf(fb);
    float const lx  = menu_left_x(0.70f);
    draw_left(lx, fbh * 0.20f, 36.0f, MENU_COL_TITLE, "Seeded Run");
    draw_left(lx, fbh * 0.34f, 18.0f, MENU_COL_NORMAL, "enter seed (digits 0-9)");

    char display[16];
    if (s_seed_len == 0) {
        snprintf(display, sizeof(display), "_");
    } else {
        snprintf(display, sizeof(display), "%s_", s_seed_buf);
    }
    draw_left(lx, fbh * 0.50f, 48.0f, MENU_COL_TITLE, display);

    draw_left(lx, fbh * 0.78f, 16.0f, MENU_COL_HINT,
              "backspace edits, enter starts, esc cancels");
}

// Single-block stats renderer, run twice with different pointers
// (last_run, all_time) and labels. Returns the y of the next free
// line so the caller can stack blocks.
static float draw_stats_block(float tx, float y, char const* heading, run_stats_t const* rs) {
    float const text_h  = 18.0f;
    pax_col_t   hdr_col = 0xFFFFFF6Bu;
    pax_col_t   txt_col = 0xFFFFFFFFu;
    char        buf[128];

    rendertext_draw(fb, hdr_col, NULL, text_h, tx, y, heading); y += text_h + 4.0f;

    snprintf(buf, sizeof(buf), "  score        %lld",   (long long)rs->score);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  distance     %.1f u", rs->distance);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  stage        %d",     (int)rs->stage_reached);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  mult max     %dx",    (int)rs->multiplier_max);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  duration     %.1f s", rs->duration_s);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  pickups      boost %d  tri %d  jump %d  shield %d",
             (int)rs->pickups_speed_boost, (int)rs->pickups_tri,
             (int)rs->pickups_jump, (int)rs->pickups_shield);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  runs         %d (crash %d, stall %d, sunset %d, quit %d)",
             (int)rs->runs_total, (int)rs->runs_crashed,
             (int)rs->runs_stalled, (int)rs->runs_sunset, (int)rs->runs_quit);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 10.0f;
    return y;
}

static void draw_stats_view(void) {
    // Stats block is the tallest screen — title at 6%, footer at 95%
    // + 14 px → ~98%. Lines are also wider than the other menus so
    // we use a wider panel here.
    draw_menu_panel_size(0.92f, 0.98f);
    float const fbh = pax_buf_get_heightf(fb);
    float const fbw = pax_buf_get_widthf(fb);
    float const tx  = fbw * 0.10f;
    float       y   = fbh * 0.06f;

    draw_left(tx, y, 32.0f, MENU_COL_TITLE, "Stats");
    y += 44.0f;

    y = draw_stats_block(tx, y, "Last run", &s_save.stats.last_run);
    y = draw_stats_block(tx, y, "All time", &s_save.stats.all_time);

    char buf[64];
    snprintf(buf, sizeof(buf), "Level %d  points %d",
             (int)s_save.meta.level, (int)s_save.meta.points);
    rendertext_draw(fb, 0xFFFFFF6Bu, NULL, 18.0f, tx, y, buf);

    draw_left(tx, fbh * 0.95f, 14.0f, MENU_COL_HINT, "press enter or esc to return");
}

// Phase 9.4 equip UI. The ship has `attach_slots` (0/1/2) equip slots,
// stored in the save as attach1 / attach2 (attachment_id_t, 0 = empty).
// upgrade_slot_ptr maps a slot index to its save field.
static int32_t* upgrade_slot_ptr(int slot) {
    return (slot == 0) ? &s_save.meta.attach1 : &s_save.meta.attach2;
}

// Number of usable slots, clamped to the 2 the save struct provides.
static int upgrade_slot_count(void) {
    int n = s_save.meta.attach_slots;
    if (n < 0) n = 0;
    if (n > 2) n = 2;
    return n;
}

// (The Upgrade slot list + attachment picker are now rendered by the
// engine's se_ui from their APP_STATE_* cases.)

// ---- Credits ------------------------------------------------------
// The credits text is taller than the panel, so it is scrolled by
// hand with UP / DOWN. The Hershey text renderer writes pixels
// directly and ignores pax_clip, so lines that fall outside the
// viewport are culled whole rather than clipped.
#define CREDITS_PANEL_W     0.86f
#define CREDITS_PANEL_H     0.96f
#define CREDITS_LINE_H      24.0f          // row pitch
#define CREDITS_TEXT_H      18.0f          // glyph height
#define CREDITS_SCROLL_STEP CREDITS_LINE_H // px per UP/DOWN press = one line

static char const* const credits_lines[] = {
    "Race the Synth was inspired by the steam game \"Race the Sun\"",
    "",
    "Project lead:",
    "    Rene Schickbauer",
    "",
    "Ideas for features:",
    "    Rene Schickbauer",
    "    Renze Nicolai",
    "    People in the Tanmatsu Discord",
    "",
    "Coding:",
    "    Rene Schickbauer",
    "    Claude Code",
    "",
    "Music:",
    "    Claude Code",
    "    Random number generator 23",
    "",
    "Sound effects:",
    "    Rene Schickbauer",
    "    Claude Code",
    "",
    "Level Design:",
    "    Rene Schickbauer",
    "    Claude Code",
    "    Random number generator 42",
    "",
    "Synthwave backdrop:",
    "    Renze Nicolai",
    "",
    "Many thanks to:",
    "    Renze Nicolai for the awesome Tanmatsu device",
    "    Team Badge for making the many modules used in this software",
    "    Espressif for making such a wicked Microcontroller",
    "    Anthropic for Claude Code",
    "",
    "** All music in this game is procedurally generated **",
    "",
    "This software is under the MIT license",
    "https://opensource.org/license/mit",
    "",
    "(C) 2026 Rene \"cavac\" Schickbauer",
};
#define CREDITS_LINE_COUNT ((int)(sizeof(credits_lines) / sizeof(credits_lines[0])))

static float s_credits_scroll = 0.0f;   // px scrolled past the first line

// Height of the credits scroll viewport. Shared by the renderer and
// the scroll clamp so they always agree.
static float credits_scroll_h(void) {
    float const fbh        = pax_buf_get_heightf(fb);
    float const panel_y    = (fbh - fbh * CREDITS_PANEL_H) * 0.5f;
    float const panel_h    = fbh * CREDITS_PANEL_H;
    float const scroll_top = panel_y + MENU_TOP_PAD + 36.0f + 16.0f;
    float const scroll_bot = panel_y + panel_h - MENU_FOOTER_PAD - 10.0f;
    return scroll_bot - scroll_top;
}

// Largest useful scroll offset — the last line resting at the bottom
// of the viewport. 0 when the whole roll already fits.
static float credits_max_scroll(void) {
    float const content_h = (float)CREDITS_LINE_COUNT * CREDITS_LINE_H;
    float const max       = content_h - credits_scroll_h();
    return max > 0.0f ? max : 0.0f;
}

static void draw_credits(void) {
    draw_menu_panel_size(CREDITS_PANEL_W, CREDITS_PANEL_H);
    float const fbw        = pax_buf_get_widthf(fb);
    float const fbh        = pax_buf_get_heightf(fb);
    float const panel_x    = (fbw - fbw * CREDITS_PANEL_W) * 0.5f;
    float const panel_y    = (fbh - fbh * CREDITS_PANEL_H) * 0.5f;
    float const panel_h    = fbh * CREDITS_PANEL_H;
    float const text_x     = panel_x + MENU_TEXT_INSET;
    float const scroll_top = panel_y + MENU_TOP_PAD + 36.0f + 16.0f;
    float const scroll_bot = panel_y + panel_h - MENU_FOOTER_PAD - 10.0f;

    draw_left(text_x, panel_y + MENU_TOP_PAD, 36.0f, MENU_COL_TITLE, "Credits");

    for (int i = 0; i < CREDITS_LINE_COUNT; i++) {
        float const ly = scroll_top - s_credits_scroll + (float)i * CREDITS_LINE_H;
        // Cull whole lines outside the viewport — a partially
        // visible line can't be clipped and would spill over the
        // title / footer.
        if (ly < scroll_top) continue;
        if (ly + CREDITS_TEXT_H > scroll_bot) continue;
        char const*  line = credits_lines[i];
        size_t const len  = strlen(line);
        // Section headings end with ':' — tint them yellow.
        pax_col_t const col = (len > 0 && line[len - 1] == ':')
                                  ? MENU_COL_TITLE : MENU_COL_NORMAL;
        draw_left(text_x, ly, CREDITS_TEXT_H, col, line);
    }

    draw_left(text_x, panel_y + panel_h - MENU_FOOTER_PAD, 14.0f, MENU_COL_HINT,
              "up / down to scroll, enter or esc to return");
}

// Read the RTC once and cache the calendar day into s_session_date
// as `year*10000 + month*100 + day`. Call exactly once, early in
// app_main, before anything day-dependent runs. If the RTC is unset
// (year < 2024) s_session_date is left at 0 and the day-dependent
// features fall back to their RTC-unset behaviour.
static void capture_session_date(void) {
    time_t    now = time(NULL);
    struct tm lt  = {0};
    localtime_r(&now, &lt);
    int const year = lt.tm_year + 1900;
    if (year < 2024) {
        s_session_date = 0;  // RTC unset — no usable date
        return;
    }
    s_session_date = (int64_t)year * 10000
                   + (int64_t)(lt.tm_mon + 1) * 100
                   + (int64_t)lt.tm_mday;
}

// Build the daily world seed from the cached session date (see
// s_session_date) — `year*10000 + month*100 + day`. Same day →
// same seed → identical world, every run of the session, and the
// world can't reshuffle between runs if the player crosses
// midnight. Falls back to a fixed constant when the RTC was unset
// at boot, so the run is still reproducible within the session.
//
// The custom-seed menu bypasses this function and passes its own
// seed into start_run().
static uint32_t derive_daily_seed(void) {
    if (s_session_date == 0) {
        return 1u;
    }
    uint32_t const seed = (uint32_t)s_session_date;
    return seed ? seed : 1u;
}

// Day-rollover check. The daily challenge-completion flags
// (save_data.daily.daily_done_*) belong to one specific calendar
// day; once the day moves past the save's last_seen_date they are
// stale and must be cleared. Call once on a freshly loaded slot.
// It compares against the cached s_session_date (NOT the RTC), so
// the rollover is decided exactly once per session — it can never
// fire in the middle of a play session if the clock ticks past
// midnight. The reset is in-memory only — the next run-end commit
// persists it, and the check is idempotent across boots, so no
// extra flash write is forced here. A zero session date (RTC unset
// at boot) is a no-op: with no trustworthy date we can't tell the
// day has changed, so the flags are left untouched.
static void save_apply_day_rollover(save_data_t* s) {
    if (s_session_date == 0) return;                   // RTC unset at boot
    if (s->meta.last_seen_date == s_session_date) return;

    s->meta.last_seen_date  = s_session_date;
    s->daily.daily_done_1pt = 0;
    s->daily.daily_done_2pt = 0;
    s->daily.daily_done_3pt = 0;
}

// Reset run state and (re-)seed the world. The seed is supplied by
// the caller so the same world replays exactly on retry. is_custom
// tags the run for save bookkeeping (custom-seed runs will skip
// meta-progression once Phase 11 lands; for now we just record the
// flag so the stats and last_custom_seed updates are correctly
// scoped).
static void start_run(game_state_t* game, world_state_t* world, uint32_t seed, bool is_custom) {
    game_init(game);
    world_init(world, seed);

    // Phase 9.4: snapshot the equipped attachments for the run. Read
    // from the save once here so gameplay (magnet pull + ship-region
    // render gating) never touches the save mid-run. has_battery stays
    // off until Phase 9.5 gives the battery an install state.
    game->has_magnet = (s_save.meta.attach1 == ATTACH_MAGNET
                        || s_save.meta.attach2 == ATTACH_MAGNET);
    // Battery (Phase 9.5): an equippable attachment whose capacity is the
    // meta.battery_max_charge upgrade (0 = no capacity). The battery is
    // active only when it occupies a slot AND has capacity; it starts
    // each run full.
    bool const battery_equipped = (s_save.meta.attach1 == ATTACH_BATTERY
                                   || s_save.meta.attach2 == ATTACH_BATTERY);
    game->battery_max    = battery_equipped ? (float)s_save.meta.battery_max_charge : 0.0f;
    game->has_battery    = (game->battery_max > 0.0f);
    game->battery_charge = game->battery_max;

    input_set_mode(INPUT_MODE_PLAYING);
    s_run_started_us   = esp_timer_get_time();
    s_run_play_seconds = 0.0;
    s_peak_stage       = (int)world->stage;
    s_run_was_custom   = is_custom ? 1 : 0;
    s_run_seed_used    = (int64_t)seed;
    if (is_custom) {
        s_save.meta.last_custom_seed = (int64_t)seed;
    }

    // Install fresh procedural music seeded from the run seed. The
    // mixer takes ownership; on stop / new run it'll call our
    // shutdown callback which frees the struct. Start the engine
    // hum as a persistent SFX voice — but only if the player
    // hasn't muted it via the Audio settings menu. The hum render
    // function also defensively returns silence if the flag is off,
    // but checking here avoids registering the voice at all and
    // keeps the mixer fully idle when both music and the other
    // SFX are quiet.
    ESP_LOGI(TAG, "start_run: seed=%u is_custom=%d — bringing up audio",
             (unsigned)seed, (int)is_custom);
    music_source_t* music = music_procedural_create(seed);
    if (music == NULL) {
        ESP_LOGW(TAG, "music_procedural_create returned NULL — no music this run");
    }
    audio_mixer_set_music(music);
    if (audio_settings_hum_on()) {
        if (!sfx_engine_hum_start()) {
            ESP_LOGW(TAG, "sfx_engine_hum_start failed");
        }
    }
}

// Edge tracker for the scrape SFX, file-scope so the pause-menu
// and end-of-run transitions can reset it after silencing the
// scrape voice. Without that, scrape held over the boundary
// would skip its restart edge on resume.
static bool s_scrape_was_on = false;

// Tear down per-run audio (music + persistent SFX). Idle-drain in
// the mixer mutes the speaker amplifier within ~50 ms.
static void end_run_audio(void) {
    audio_mixer_set_music(NULL);
    sfx_engine_hum_stop();
    sfx_scrape_stop();
    s_scrape_was_on = false;
    // Any one-shot SFX in flight (a still-decaying crash sound on
    // the same frame the run ends) are allowed to play to their
    // natural end — they'll finish well inside the drain window.
}

// PLAYING → PAUSED. Music keeps playing (pause is "still in the
// run" per the audio design), but every SFX voice is silenced so
// the frozen scene also sounds frozen — no engine hum droning,
// no scrape ringing, no in-flight one-shot tails. The per-effect
// stop calls clear the singleton "is running" flags inside
// sfx_engine_hum / sfx_scrape so resume can cleanly restart them;
// audio_mixer_stop_all_voices() catches the one-shot voices that
// don't have a typed stop API (ding, crash, cube_bump).
static void pause_audio_for_pause_menu(void) {
    sfx_engine_hum_stop();
    sfx_scrape_stop();
    audio_mixer_stop_all_voices();
    s_scrape_was_on = false;
}

// PAUSED → PLAYING. Restart the persistent hum if the player has
// it enabled (the audio settings menu isn't reachable from PAUSED
// today, but reading the flag here makes the resume path future-
// proof against that). Scrape restarts naturally on the next
// collision frame via the s_scrape_was_on edge logic; one-shots
// only fire on their own trigger events.
static void resume_audio_from_pause_menu(void) {
    if (audio_settings_hum_on()) {
        sfx_engine_hum_start();
    }
}

// Classify the end-of-run cause and commit the run summary to the
// active slot. Called once on the PLAYING → GAME_OVER transition.
//
// The save itself is a `fastopen("wb")` + NBT serialise + close
// chain that hits flash synchronously — it can stall the main
// loop for hundreds of ms. We deliberately call this *after* the
// state-machine has already switched to GAME_OVER and the audio
// teardown has happened, so the audible / visible feedback the
// player sees on a crash is immediate and the slow flash write
// hides behind the first GAME_OVER frame.
static void commit_run_end(game_state_t const* g, world_state_t const* w, bool head_on) {
    save_end_reason_t reason;
    if (head_on) {
        reason = SAVE_END_CRASH;
    } else if (g->sun_y >= GAME_SUN_SINK_RANGE_PX) {
        reason = SAVE_END_SUNSET;
    } else {
        reason = SAVE_END_STALL;
    }
    int   const peak_stage   = (s_peak_stage > (int)w->stage) ? s_peak_stage : (int)w->stage;
    double const run_seconds = s_run_play_seconds;

    int64_t const t0 = esp_timer_get_time();
    save_commit_run_end(s_active_slot, &s_save, reason, g, peak_stage, run_seconds);
    int64_t const t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "save commit: reason=%d took %lld ms",
             (int)reason, (long long)((t1 - t0) / 1000));
}

// ---------------------------------------------------------------------
//  Application framework wiring (EF).
//
//  The engine (se_run) owns the device + display bootstrap, the two
//  framebuffers, the input-queue pump + device-global keys (volume /
//  audio-jack / F1-exit), vsync/blit, the buffer swap and the per-frame
//  delta time. The game is the callbacks below plus its content; events
//  the pump doesn't consume arrive via on_input → input_handle_event.
//  State the old monolithic app_main kept as loop locals is promoted to
//  file scope here so the callbacks share it.
// ---------------------------------------------------------------------

// Game + world simulation state. ~5 KB at the current pool size; static
// storage keeps it off any task stack.
static game_state_t  game;
static world_state_t world;

// Checkpoint run-state snapshot (Phase 9.3): collecting a checkpoint
// copies the whole world + game state here; a later head-on crash
// restores it. In-memory only — not persisted across power-off.
static world_state_t s_checkpoint_world;
static game_state_t  s_checkpoint_game;
static bool          s_checkpoint_valid = false;
// Swallows the use-button press-edge on the frame the Re-Do dialog opens,
// so crashing mid-jump (space held) can't instantly dismiss it.
static bool          s_redo_ignore_pickup = false;

// Top-level state machine + per-run bookkeeping (were app_main locals).
static app_state_t   app_state          = APP_STATE_SLOT_SELECT;
static bool          run_end_committed  = false;
static uint32_t      daily_seed         = 0;
// The menu/title floor scrolls with this fake speed so the scene reads as
// "live" instead of static. The world isn't advanced (no obstacles spawn
// yet) — start_run() initializes the world fresh each time PLAYING begins.
static float const   title_scroll_speed = 6.0f;

// This frame's delta time, published by on_update so on_backdrop (which
// has no dt parameter) can scroll the floor by the same amount.
static float         s_frame_dt = 0.0f;

// ---- Per-frame input snapshot ---------------------------------------
// The engine's input pump drains the BSP queue each frame (before
// on_update) and forwards the events it doesn't consume to on_input →
// input_handle_event, which latches them. on_update then consumes every
// one-shot into these statics, so the physics pass (on_update) and the
// render switch (on_render) read one consistent snapshot for the frame.
static bool  s_in_pickup      = false;
static float s_in_steer       = 0.0f;
static bool  s_in_steer_left  = false;
static bool  s_in_steer_right = false;
static int   s_in_menu_nav    = 0;
static int   s_in_menu_horiz  = 0;
static bool  s_in_menu_esc    = false;
static bool  s_in_menu_bs     = false;
static bool  s_in_pause       = false;
static int   s_in_typed       = -1;
static bool  s_in_typed_d     = false;
// End-of-run signals computed in the physics pass (on_update), consumed
// by the PLAYING render case to pick the post-run state.
static bool  s_crashed = false;
static bool  s_stalled = false;

// ---- Per-frame profiling --------------------------------------------
// Same stage breakdown as the pre-framework loop, re-anchored across the
// callbacks. blit + vsync + swap belong to the engine now, so they roll
// up into one "present" bucket measured as the gap between a frame's
// on_render end and the next frame's on_update start.
static int64_t s_t_phys_end     = 0;   // on_update end   (-> bgkick start)
static int64_t s_t_bg_end       = 0;   // on_backdrop end (-> obs start)
static int64_t s_prof_fg_end    = 0;   // last on_render end
static int64_t prof_input_us    = 0;
static int64_t prof_phys_us     = 0;
static int64_t prof_bgkick_us   = 0;
static int64_t prof_bgflr_us    = 0;
static int64_t prof_bgwait_us   = 0;
static int64_t prof_obs_us      = 0;
static int64_t prof_fgrest_us   = 0;
static int64_t prof_present_us  = 0;
static int     prof_frames      = 0;
static int64_t prof_window_start = 0;

// on_init — game-side bootstrap, run once after the engine has the
// display, framebuffers, scene buffers and audio mixer up (so
// se_display_info() is valid and the mixer gates can be pushed).
static void on_init(void* user) {
    (void)user;

    // The engine owns the display + framebuffers; pull the geometry it
    // resolved so backdrop_init can match the framebuffers' format /
    // endianness / orientation exactly.
    se_display_info_t di;
    se_display_info(&di);

    synthwave_init();
    icons_load();
    input_init();
    input_set_mode(INPUT_MODE_TITLE);

    // Load music/SFX enable flags and bring the mixer + I2S up.
    // Idempotent — safe even if a later call repeats it.
    audio_settings_load();
    // Control settings (gyro flag + remappable keybinds) — input.c
    // reads these, so load before the first frame.
    controls_settings_load();
    // Capture the calendar day once, now, for the whole session.
    // Every day-dependent feature (daily seed, day-rollover,
    // challenges) reads this snapshot — never the RTC live — so the
    // "current day" can't shift mid-session if the player crosses
    // midnight.
    capture_session_date();
    // The engine's se_run bootstrap already brought the mixer + I2S up
    // before on_init; here we only push our app-side toggle state into
    // its output gates.
    // Push the loaded audio toggles into the engine mixer's output gates.
    // The mixer no longer reads app settings itself (engine has no NVS
    // dependency); the app maps its mute categories onto mixer groups.
    audio_mixer_set_music_enabled(audio_settings_music_on());
    audio_mixer_set_group_enabled(AUDIO_SFX_GROUP_GENERAL, audio_settings_sfx_on());
    audio_mixer_set_group_enabled(AUDIO_SFX_GROUP_HUM, audio_settings_hum_on());
    // (Device-global hardware settings — display/keyboard/LED brightness,
    // speaker/headphone volume + initial audio-jack routing — are now
    // owned by the engine: se_run's bootstrap calls se_hw_init() after
    // audio_mixer_init(), and its input pump steps volume / re-routes on
    // the jack action. Nothing to do here.)

    // Bring up the PPA synthwave compositor: allocate + render the sun /
    // mountain layer caches in PSRAM and register the FILL/SRM/BLEND
    // clients. Must follow synthwave_init() above (the cache drawing uses
    // the pre-triangulated shapes); the band math uses the engine's raw
    // framebuffer geometry from `di`.
    backdrop_init(di.width, di.height, di.pax_format, di.reversed, di.orientation);

    game_init(&game);

    // Daily seed. Derived from today's calendar date so every run —
    // across restarts, across app reboots — uses the same world layout
    // until the next midnight rollover.
    daily_seed = derive_daily_seed();

    // Persistence setup: mkdir /int/synthracer if missing. Defaults until
    // the user picks a slot.
    save_init();
    save_init_defaults(&s_save);

    app_state         = APP_STATE_SLOT_SELECT;
    run_end_committed = false;

    ESP_LOGI(TAG, "Race the Synth: slot-select up");
}

// on_update — per-frame game logic: drain + snapshot input, run the
// debug knobs, then the physics pass (PLAYING) or the post-run holds.
// The engine has already computed and clamped `dt`.
static void on_update(float dt, void* user) {
    (void)user;

    int64_t const t_loop_start = esp_timer_get_time();
    // Account the previous frame's present (engine blit + vsync + swap)
    // and arm the profiling window on the first frame.
    if (s_prof_fg_end != 0)     prof_present_us  += t_loop_start - s_prof_fg_end;
    if (prof_window_start == 0) prof_window_start = t_loop_start;

        // The engine's input pump already drained the BSP queue this
        // frame (before on_update) and forwarded each non-global event to
        // on_input → input_handle_event, which latched it; it also handled
        // F1-exit + the volume/jack keys itself. Here we just consume the
        // latched one-shots into the per-frame snapshot, so the physics
        // pass below and the render switch (on_render) read one consistent
        // view of the frame.
        s_in_pickup   = input_consume_pickup();
        s_in_steer    = input_steering();
        input_steer_held(&s_in_steer_left, &s_in_steer_right);
        s_in_menu_nav   = input_consume_menu_nav();
        s_in_menu_horiz = input_consume_menu_horiz();
        s_in_menu_esc = input_consume_menu_cancel();
        s_in_menu_bs  = input_consume_backspace();
        s_in_pause    = input_consume_pause_toggle();
        s_in_typed    = -1;
        s_in_typed_d  = input_consume_digit(&s_in_typed);

        // Publish dt for on_backdrop (which has no dt parameter).
        s_frame_dt = dt;

        // Debug speed knob acts on the *base* speed so the scrape
        // ramps don't fight the player's tuning.
        int sd = input_consume_speed_delta();
        if (sd != 0) {
            game.ship_base_speed_z += (float)sd * 1.0f;
            if (game.ship_base_speed_z < 0.5f) game.ship_base_speed_z = 0.5f;
            if (game.ship_base_speed_z > 60.0f) game.ship_base_speed_z = 60.0f;
        }

        // Debug sun-position nudge (Q / A). 10 px per press is
        // ~4% of GAME_SUN_SINK_RANGE_PX — coarse enough to feel
        // each press, fine enough to dial in the "fully behind
        // mountains" threshold.
        int const sun_d = input_consume_sun_delta();
        if (sun_d != 0) {
            game.sun_y += (float)sun_d * 10.0f;
            if (game.sun_y < 0.0f)                     game.sun_y = 0.0f;
            if (game.sun_y > GAME_SUN_SINK_RANGE_PX)   game.sun_y = GAME_SUN_SINK_RANGE_PX;
        }

        // Read this frame's input snapshot (consumed at the top of
        // on_update) into the names the physics pass already uses.
        bool  const pickup_pressed = s_in_pickup;
        float const steer          = s_in_steer;
        bool        steer_left  = s_in_steer_left;
        bool        steer_right = s_in_steer_right;

        // Debug: TAB cuts the current area short and forces the
        // next one to a specific type. Currently hard-wired to the
        // simple_platform area; change the area_type_t argument here
        // to test a different generator. Only acts during PLAYING so
        // a stray TAB on a menu doesn't strand the world in an odd
        // state.
        if (input_consume_force_next_area() && app_state == APP_STATE_PLAYING) {
            world_force_next_area(&world, AREA_TYPE_SYNTHENGINE_AD);
        }
        // Debug: G toggles godmode (crash / stall suppressed below).
        if (input_consume_godmode_toggle()) {
            s_godmode = !s_godmode;
        }

        int64_t const t_after_input = esp_timer_get_time();
        // End-of-run signals for this frame, kept separate so the
        // render switch can pick the right post-run state: a head-on
        // crash plays the explosion, a stall just holds the scene.
        bool crashed = false;
        bool stalled = false;

        // Physics pass — only meaningful in PLAYING; the other states
        // record zero physics time so the breakdown stays honest.
        if (app_state == APP_STATE_PLAYING) {
            // 0. Use-item button triggers a jump (Phase 9.1 — still
            //    ungated; the jump-pickup inventory gate lands in
            //    9.1f). game_jump only fires if the ship is grounded;
            //    game_step then integrates the arc.
            // 1. Apply bank + lateral motion using this frame's steer.
            // 2. Collide: push the ship out of side-contact obstacles
            //    and set scrape flags (or return head-on).
            // 3. After-collide work that reads the flags: ramp speed,
            //    emit + advance sparks.
            if (pickup_pressed) {
                game_jump(&game);
            }
            game_step(&game, &world, dt, steer, steer_left, steer_right);
            crashed = game_collide(&game, &world, dt);
            // game_after_collide runs sun integration, shadow
            // detection, and speed dynamics. Returns true when the
            // ship has coasted to a halt in shadow — the stall
            // end-of-run signal.
            stalled = game_after_collide(&game, &world, dt);
            // Debug godmode: keep the run alive regardless. The ship
            // simply coasts through head-on hits and never stalls
            // out, so the world can be slowed down and inspected.
            if (s_godmode) {
                crashed = false;
                stalled = false;
            }

            // Phase 9.2 shield. Tick the invuln window + the SFX/
            // spark debounce, then let a shield turn a head-on crash
            // into a survivable hit.
            if (game.shield_timer > 0.0f) {
                game.shield_timer -= (float)dt;
                if (game.shield_timer < 0.0f) game.shield_timer = 0.0f;
            }
            if (game.shield_hit_cooldown > 0.0f) {
                game.shield_hit_cooldown -= (float)dt;
                if (game.shield_hit_cooldown < 0.0f) game.shield_hit_cooldown = 0.0f;
            }
            if (crashed) {
                // A banked charge opens the invuln window on the
                // crashing frame.
                if (game.shield_timer <= 0.0f && game.shield_charges > 0) {
                    game.shield_charges--;
                    game.shield_timer = GAME_SHIELD_DURATION;
                }
                // While the window is open the run survives; the
                // crash SFX + spark shower still fire (debounced) so
                // each hit reads.
                if (game.shield_timer > 0.0f) {
                    if (game.shield_hit_cooldown <= 0.0f) {
                        sfx_crash_play();
                        game_crash_burst(&game);
                        game.shield_hit_cooldown = GAME_SHIELD_HIT_COOLDOWN;
                    }
                    crashed = false;
                }
            }
            // Age any shielded-hit spark shower (harmless no-op when
            // the pool is empty).
            game_crash_tick(&game, (float)dt);

            world_advance(&world, dt, game.ship_speed_z, game.cam_x);

            // Phase 9.4 magnet: slide nearby pickups toward the ship's
            // path. After world_advance so it acts on this frame's
            // freshly-advanced positions, before collision sees them.
            if (game.has_magnet) {
                world_magnet_pull(&world, game.ship_x_world,
                                  SHIP_BASE_Y + game.ship_y, (float)dt);
            }

            // Phase 9.3 checkpoint rewind. If a head-on crash got
            // here unabsorbed (no shield) and a checkpoint snapshot
            // exists, rewind the *level* — world generation, RNG,
            // ship position, sun, inventory — to the snapshot and open
            // the Re-Do dialog instead of ending the run.
            //
            // The player's run *progress* is NOT rewound: score,
            // distance, multiplier (+ peak) and the per-run pickup
            // tallies carry forward from the crash-time state, so a
            // Re-Do never costs accumulated progress or the
            // meta-progression credit it feeds at run end. (Max stage
            // reached and play-time live in main.c statics — they
            // already survive the struct copy.) The multiplier pair is
            // carried with pickups_tri on purpose: the two are coupled
            // (the multiplier bumps every 5th Tri), so rewinding one
            // without the other would desync the cadence.
            //
            // Done after world_advance so the restored state is the
            // final word for the frame; `crashed` is consumed so the
            // render switch routes to the dialog, not CRASHING.
            if (crashed && s_checkpoint_valid) {
                sfx_crash_play();

                double const kept_distance     = game.distance_traveled;
                double const kept_score        = game.score;
                int    const kept_multiplier   = game.multiplier;
                int    const kept_mult_max     = game.multiplier_max;
                int    const kept_p_boost      = game.pickups_speed_boost;
                int    const kept_p_tri        = game.pickups_tri;
                int    const kept_p_jump       = game.pickups_jump;
                int    const kept_p_shield     = game.pickups_shield;
                int    const kept_p_checkpoint = game.pickups_checkpoint;

                world = s_checkpoint_world;
                game  = s_checkpoint_game;

                game.distance_traveled  = kept_distance;
                game.score              = kept_score;
                game.multiplier         = kept_multiplier;
                game.multiplier_max     = kept_mult_max;
                game.pickups_speed_boost = kept_p_boost;
                game.pickups_tri         = kept_p_tri;
                game.pickups_jump        = kept_p_jump;
                game.pickups_shield      = kept_p_shield;
                game.pickups_checkpoint  = kept_p_checkpoint;

                s_checkpoint_valid   = false;
                s_redo_ignore_pickup = true;
                app_state = APP_STATE_CHECKPOINT_REDO;
                input_set_mode(INPUT_MODE_GAME_OVER);
                crashed = false;
            }
            // Accumulate active play time (excludes paused frames).
            // Used by save_commit_run_end so the duration_s stat
            // doesn't count F4 pauses as gameplay time.
            s_run_play_seconds += (double)dt;

            // ---- Per-frame audio driving ----
            // Engine-hum pitch follows ship speed normalised against
            // the standard base speed; boosts push it past 1.0,
            // shadows pull it back.
            float const base_speed = (game.ship_base_speed_z > 0.1f) ? game.ship_base_speed_z : 1.0f;
            float       speed_norm = game.ship_speed_z / base_speed;
            // Map [0.0, 1.5] to [0.0, 1.0] so even cruise sits in
            // the middle of the pitch range and boosts run hot.
            speed_norm *= (1.0f / 1.5f);
            if (speed_norm < 0.0f) speed_norm = 0.0f;
            if (speed_norm > 1.0f) speed_norm = 1.0f;
            sfx_engine_hum_set_pitch(speed_norm);

            // Scrape is persistent — start/stop on edge transitions
            // so we don't pile up registrations. The "was on" flag
            // lives at module scope (s_scrape_was_on, declared
            // near the audio helpers) so the pause-menu transition
            // can reset it after silencing the scrape voice — without
            // that, scrape staying held across the pause boundary
            // would skip its restart edge.
            bool const  scrape_now      = (game.scrape_left || game.scrape_right);
            if (scrape_now && !s_scrape_was_on) {
                sfx_scrape_start();
            }
            if (scrape_now) {
                // Intensity rises with speed — bigger slams sound
                // meatier than a brushing graze.
                float const intensity = 0.4f + 0.6f * speed_norm;
                sfx_scrape_set_intensity(intensity > 1.0f ? 1.0f : intensity);
            }
            if (!scrape_now && s_scrape_was_on) {
                sfx_scrape_stop();
            }
            s_scrape_was_on = scrape_now;

            // Booster ding — edge flag cleared in game_collide each
            // frame, so this fires exactly once per pickup.
            if (game.just_picked_up_booster) {
                sfx_pickup_ding_play();
            }

            // Checkpoint pickup (Phase 9.3): snapshot the whole run
            // state — this frame, post-advance — so a later head-on
            // crash can rewind here, and play the gong. The edge
            // flag is cleared before the copy so the snapshot itself
            // records it false (otherwise a restore would re-fire).
            if (game.just_picked_up_checkpoint) {
                game.just_picked_up_checkpoint = false;
                s_checkpoint_world = world;
                s_checkpoint_game  = game;
                s_checkpoint_valid = true;
                sfx_gong_play();
            }

            // Tri plink — pitch steps with the in-cycle slot index
            // so the five pickups of a multiplier cycle form a
            // brief ascending pentatonic phrase (C5/D5/E5/G5/A5).
            // The 5th plink resolves on A5 at the exact frame the
            // HUD progress row resets — audio + visual confirm
            // the multiplier bump together.
            if (game.just_picked_up_tri) {
                sfx_pickup_plink_play(game.tri_pickup_slot);
            }
        } else if (app_state == APP_STATE_CRASHING) {
            // Ship is destroyed — no steering, no collision. Keep
            // the world scrolling at the crash-moment speed so the
            // scene still flows, and age the spark shower. Hold here
            // until the sparks burn out (≈ the crash SFX length),
            // then drop into GAME_OVER.
            world_advance(&world, dt, game.ship_speed_z, game.cam_x);
            s_crash_anim_time += (double)dt;
            bool const sparks_live = game_crash_tick(&game, dt);
            if (!sparks_live || s_crash_anim_time >= CRASH_ANIM_SECONDS) {
                app_state = APP_STATE_GAME_OVER;
            }
        } else if (app_state == APP_STATE_STALL_OUT) {
            // Ship has coasted to a halt. Hold the frozen scene a
            // beat so the player registers the stall, then GAME_OVER.
            s_stall_hold_time += (double)dt;
            if (s_stall_hold_time >= STALL_HOLD_SECONDS) {
                app_state = APP_STATE_GAME_OVER;
            }
        }
        int64_t const t_after_phys = esp_timer_get_time();
        prof_input_us += t_after_input - t_loop_start;
        prof_phys_us  += t_after_phys  - t_after_input;
        s_t_phys_end   = t_after_phys;
        // Publish the end-of-run signals for the PLAYING render case.
        s_crashed = crashed;
        s_stalled = stalled;
}

// on_backdrop — the synthwave backdrop: PPA sky/sun/mountain composite +
// the floor grid (with obstacle shadows). Runs after on_update, before
// on_render, on the engine's current back buffer.
static void on_backdrop(pax_buf_t* fb_param, void* user) {
    (void)user;
    fb = fb_param;
    // on_backdrop has no dt parameter; use the value on_update published.
    float const dt = s_frame_dt;

        // Background pass — FILL → SRM → BLEND, explicitly serialised.
        // PPA's dispatch order across different clients is *not*
        // guaranteed to match submission order: empirically the SRM
        // (sun) and BLEND (mountain) ops raced and the sun
        // overwrote the mountains in some frames. Adding a wait
        // between each submit pins the order at the cost of losing
        // CPU/PPA parallelism for the first two ops. The CPU floor
        // work still runs in parallel with the BLEND (the longest
        // single op besides the CPU work itself), so the bg-phase
        // wallclock is FILL + SRM + max(BLEND, floor).
        //
        // FILL is the per-frame guarantee that no stale obstacle
        // pixel from the previous frame remains in the sky band.
        backdrop_submit_fill_sky();
        backdrop_wait_one();
        // PPA SRM destination Y comes from game.sun_y, which the
        // physics step integrates each frame. In TITLE / GAME_OVER
        // states sun_y is wherever the last run left it (0 at start,
        // frozen at end of run).
        backdrop_submit_sun((int)game.sun_y);
        backdrop_wait_one();
        backdrop_submit_mountains();
        int64_t const t_after_bgkick = esp_timer_get_time();
        // Floor paint is split in three so the obstacle-shadow pass
        // can sit between the floor base and the grid lines:
        //   1. synthwave_step_base   — solid floor rectangle
        //   2. render_shadows        — darker quads where obstacles
        //                              cast shadows
        //   3. synthwave_step_lines  — magenta lane lines + stripes
        //                              on top of both
        // Lines on top of shadows keeps them visible in shadowed
        // regions without per-pixel blend math — much cheaper than
        // detecting per-pixel "am I in a shadow" while drawing the
        // grid.
        // The settings family counts as a "menu state" (scrolling
        // menu floor, no shadows) only when it was opened from the
        // main menu. Opened from the pause menu it renders like
        // PAUSED instead — frozen floor, frozen scene behind.
        bool const in_settings_family = (app_state == APP_STATE_SETTINGS
                                         || app_state == APP_STATE_CONTROLS
                                         || app_state == APP_STATE_DISPLAY
                                         || app_state == APP_STATE_AUDIO_SETTINGS);
        bool const is_menu_state = (app_state == APP_STATE_SLOT_SELECT
                                    || app_state == APP_STATE_MENU
                                    || app_state == APP_STATE_SEED_INPUT
                                    || app_state == APP_STATE_STATS_VIEW
                                    || app_state == APP_STATE_UPGRADE
                                    || app_state == APP_STATE_UPGRADE_PICK
                                    || app_state == APP_STATE_CREDITS
                                    || (in_settings_family
                                        && s_settings_origin != APP_STATE_PAUSED));
        // PAUSED freezes the world but keeps the existing scene
        // visible behind the overlay — same render path as
        // GAME_OVER (obstacles + shadows in their last positions,
        // but no scrolling and no fresh shadows).
        // CRASHING keeps the world advancing (the wreck's momentum),
        // so its floor scrolls like PLAYING; STALL_OUT / PAUSED /
        // GAME_OVER hold the floor still.
        bool const world_is_live = (app_state == APP_STATE_PLAYING
                                    || app_state == APP_STATE_CRASHING);
        float const floor_scroll = is_menu_state    ? title_scroll_speed * dt
                                   : world_is_live  ? game.ship_speed_z * dt
                                                    : 0.0f;
        float const floor_cam_x      = is_menu_state ? 0.0f : game.cam_x;
        // Camera Y follows a fraction of the ship's jump altitude
        // (GAME_CAM_Y_FOLLOW) so the ship stays in frame without the
        // world lurching; menu states sit at the resting height.
        // Publish the camera to the render module now — *before*
        // anything projects (render_shadows, the floor, obstacles
        // and the ship all read render_project's camera global).
        float const cam_y = is_menu_state
                              ? RENDER_CAM_Y
                              : RENDER_CAM_Y + GAME_CAM_Y_FOLLOW * game.ship_y;
        render_set_camera(floor_cam_x, cam_y);
        bool  const fully_shadowed   = !is_menu_state
                                       && (game.sun_y >= GAME_SUN_SINK_RANGE_PX);
        synthwave_step_base(fb, fully_shadowed);
        if (!is_menu_state) {
            render_shadows(fb, &world, game.cam_x, game.sun_y);
        }
        // (Phase 9.1d: the floor-pixel in_shadow sampler that used to
        // live here is gone. game_collide's shadow ray now computes
        // the gameplay shadow flag directly from geometry — no render
        // dependency, no one-frame lag, and it works at any altitude.
        // render_shadows above still paints the floor-shadow quads,
        // purely as a visual cue now.)
        synthwave_step_lines(fb, floor_scroll, floor_cam_x, cam_y);
        int64_t const t_after_bgflr = esp_timer_get_time();
        // Wait for the BLEND op to finish — obstacles and HUD text
        // can both write into the sky region, so the backdrop must
        // be in place before any foreground render touches it.
        backdrop_wait_one();
        int64_t const t_after_bg = esp_timer_get_time();
        prof_bgkick_us += t_after_bgkick - s_t_phys_end;
        prof_bgflr_us  += t_after_bgflr  - t_after_bgkick;
        prof_bgwait_us += t_after_bg     - t_after_bgflr;
        s_t_bg_end      = t_after_bg;
}

// on_render — the foreground: the 3D scene + HUD + menus + state
// transitions, switched on app_state. Runs after on_backdrop on the same
// back buffer; the engine blits + swaps once this returns.
static void on_render(pax_buf_t* fb_param, void* user) {
    (void)user;
    fb = fb_param;

        // Foreground pass — state-dependent dynamic content.
        // `obs` measures render_obstacles in isolation since it
        // dominates the gameplay frame; everything else (ship,
        // sparks, HUD, overlays) rolls up under `fgrest`.
        // Input was drained + snapshotted in on_update; read the frame's
        // values back into the names the switch already refers to.
        int  const menu_nav       = s_in_menu_nav;
        int  const menu_horiz     = s_in_menu_horiz;
        bool const menu_esc       = s_in_menu_esc;
        bool const menu_bs        = s_in_menu_bs;
        bool const pause_toggle   = s_in_pause;
        int        typed          = s_in_typed;
        bool const typed_d        = s_in_typed_d;
        bool const pickup_pressed = s_in_pickup;
        bool const crashed        = s_crashed;
        bool const stalled        = s_stalled;

        int64_t t_after_obs = 0;
        switch (app_state) {
            case APP_STATE_SLOT_SELECT: {
                t_after_obs = esp_timer_get_time();
                draw_slot_select();
                if (menu_nav != 0) {
                    // UP = -1 (toward index 0), DOWN = +1. menu_nav
                    // here is +1 for UP / -1 for DOWN (mirrors the
                    // speed-delta convention); flip the sign so
                    // the cursor moves the way the labels read.
                    s_slot_cursor -= menu_nav;
                    if (s_slot_cursor < 0)               s_slot_cursor = 0;
                    if (s_slot_cursor >= SAVE_SLOT_COUNT) s_slot_cursor = SAVE_SLOT_COUNT - 1;
                }
                if (pickup_pressed) {
                    s_active_slot = s_slot_cursor;
                    if (save_load_slot(s_active_slot, &s_save) != 0) {
                        // Missing or corrupt — start a fresh profile.
                        save_init_defaults(&s_save);
                    }
                    // New calendar day → clear yesterday's daily
                    // challenge-completion flags before play begins.
                    save_apply_day_rollover(&s_save);
                    if (s_save.meta.last_custom_seed > 0) {
                        // The seed is logically a uint32 (max 10
                        // digits), so we clamp into 10 chars + null
                        // before formatting; the static analyser
                        // can't deduce the range from a int64 field.
                        uint32_t v = (uint32_t)(s_save.meta.last_custom_seed & 0xFFFFFFFFu);
                        snprintf(s_seed_buf, sizeof(s_seed_buf), "%u", (unsigned)v);
                        s_seed_len = (int)strnlen(s_seed_buf, sizeof(s_seed_buf) - 1);
                    } else {
                        s_seed_buf[0] = '\0';
                        s_seed_len    = 0;
                    }
                    s_menu_cursor = MENU_ENTRY_DAILY;
                    app_state     = APP_STATE_MENU;
                    input_set_mode(INPUT_MODE_TITLE);
                }
                break;
            }

            case APP_STATE_MENU: {
                t_after_obs = esp_timer_get_time();
                // Engine-rendered main menu (se_ui). Subtitle shows the
                // active save slot.
                static char const* const labels[MENU_ENTRY_COUNT] = {
                    [MENU_ENTRY_DAILY]    = "Daily Run",
                    [MENU_ENTRY_SEEDED]   = "Seeded Run",
                    [MENU_ENTRY_UPGRADE]  = "Upgrade Ship",
                    [MENU_ENTRY_STATS]    = "Stats",
                    [MENU_ENTRY_SETTINGS] = "Settings",
                    [MENU_ENTRY_CREDITS]  = "Credits",
                    [MENU_ENTRY_EXIT]     = "Exit",
                };
                se_menu_row_t rows[MENU_ENTRY_COUNT] = {0};
                for (int i = 0; i < MENU_ENTRY_COUNT; i++) {
                    rows[i].label = labels[i];
                    rows[i].kind  = SE_MENU_VAL_NONE;
                }
                char subtitle[32];
                snprintf(subtitle, sizeof(subtitle), "slot %d", s_active_slot + 1);
                se_menu_def_t const def = {
                    .title = "RACE THE SYNTH", .title_h = 48.0f, .subtitle = subtitle,
                    .rows = rows, .row_count = MENU_ENTRY_COUNT, .row_h = 38.0f,
                    .hint = "up / down to choose, enter to confirm",
                    .panel_w = 0.80f, .panel_h = 0.94f, .value_dx = 0.0f,
                };
                se_menu_t menu = { .def = &def, .cursor = s_menu_cursor };
                se_menu_draw(&menu, fb);

                se_menu_result_t res = SE_MENU_RESULT_NONE;
                if (menu_nav > 0)      se_menu_input(&menu, SE_MENU_ACT_UP);
                else if (menu_nav < 0) se_menu_input(&menu, SE_MENU_ACT_DOWN);
                if (pickup_pressed)    res = se_menu_input(&menu, SE_MENU_ACT_ACTIVATE);
                s_menu_cursor = menu.cursor;

                if (res == SE_MENU_RESULT_ACTIVATED) {
                    switch (s_menu_cursor) {
                        case MENU_ENTRY_DAILY:
                            start_run(&game, &world, daily_seed, /*is_custom=*/false);
                            run_end_committed = false;
                            app_state = APP_STATE_PLAYING;
                            break;
                        case MENU_ENTRY_SEEDED:
                            input_set_mode(INPUT_MODE_MENU_SEED);
                            app_state = APP_STATE_SEED_INPUT;
                            break;
                        case MENU_ENTRY_UPGRADE:
                            s_upgrade_cursor = 0;
                            app_state = APP_STATE_UPGRADE;
                            break;
                        case MENU_ENTRY_STATS:
                            app_state = APP_STATE_STATS_VIEW;
                            break;
                        case MENU_ENTRY_SETTINGS:
                            s_settings_origin = APP_STATE_MENU;
                            s_settings_cursor = SETTINGS_ENTRY_CONTROLS;
                            app_state = APP_STATE_SETTINGS;
                            break;
                        case MENU_ENTRY_CREDITS:
                            s_credits_scroll = 0.0f;
                            app_state = APP_STATE_CREDITS;
                            break;
                        case MENU_ENTRY_EXIT:
                            audio_mixer_shutdown();
                            bsp_device_restart_to_launcher();
                            break;
                    }
                }
                break;
            }

            case APP_STATE_SEED_INPUT: {
                t_after_obs = esp_timer_get_time();
                draw_seed_input();
                if (typed_d && s_seed_len < (int)(sizeof(s_seed_buf) - 1)) {
                    s_seed_buf[s_seed_len++] = (char)('0' + typed);
                    s_seed_buf[s_seed_len]   = '\0';
                }
                if (menu_bs && s_seed_len > 0) {
                    s_seed_buf[--s_seed_len] = '\0';
                }
                if (menu_esc) {
                    app_state = APP_STATE_MENU;
                    input_set_mode(INPUT_MODE_TITLE);
                }
                if (pickup_pressed && s_seed_len > 0) {
                    uint64_t v   = strtoull(s_seed_buf, NULL, 10);
                    uint32_t seed = (v == 0) ? 1u : (uint32_t)(v & 0xFFFFFFFFu);
                    start_run(&game, &world, seed, /*is_custom=*/true);
                    run_end_committed = false;
                    app_state = APP_STATE_PLAYING;
                }
                break;
            }

            case APP_STATE_SETTINGS: {
                draw_settings_scene(&world, &game);
                t_after_obs = esp_timer_get_time();
                // Engine-rendered list menu (se_ui). The cursor lives in
                // s_settings_cursor; we draw with it, then feed this
                // frame's nav/confirm/cancel as engine menu actions.
                static char const* const labels[SETTINGS_ENTRY_COUNT] = {
                    [SETTINGS_ENTRY_CONTROLS] = "Controls",
                    [SETTINGS_ENTRY_DISPLAY]  = "Display",
                    [SETTINGS_ENTRY_AUDIO]    = "Audio",
                };
                se_menu_row_t rows[SETTINGS_ENTRY_COUNT] = {0};
                for (int i = 0; i < SETTINGS_ENTRY_COUNT; i++) {
                    rows[i].label = labels[i];
                    rows[i].kind  = SE_MENU_VAL_NONE;
                }
                se_menu_def_t const def = {
                    .title = "Settings", .title_h = 36.0f, .subtitle = NULL,
                    .rows = rows, .row_count = SETTINGS_ENTRY_COUNT, .row_h = 44.0f,
                    .hint = "up / down to choose, enter to open, esc to leave",
                    .panel_w = 0.60f, .panel_h = 0.70f, .value_dx = 0.0f,
                };
                se_menu_t menu = { .def = &def, .cursor = s_settings_cursor };
                se_menu_draw(&menu, fb);

                se_menu_result_t res = SE_MENU_RESULT_NONE;
                if (menu_nav > 0)      se_menu_input(&menu, SE_MENU_ACT_UP);
                else if (menu_nav < 0) se_menu_input(&menu, SE_MENU_ACT_DOWN);
                if (pickup_pressed)    res = se_menu_input(&menu, SE_MENU_ACT_ACTIVATE);
                else if (menu_esc)     res = se_menu_input(&menu, SE_MENU_ACT_BACK);
                s_settings_cursor = menu.cursor;

                if (res == SE_MENU_RESULT_ACTIVATED) {
                    switch (s_settings_cursor) {
                        case SETTINGS_ENTRY_CONTROLS:
                            s_controls_cursor = CONTROLS_ENTRY_GYRO;
                            app_state = APP_STATE_CONTROLS;
                            break;
                        case SETTINGS_ENTRY_DISPLAY:
                            s_display_cursor = DISPLAY_ENTRY_SCREEN;
                            app_state = APP_STATE_DISPLAY;
                            break;
                        case SETTINGS_ENTRY_AUDIO:
                            s_audio_cursor = AUDIO_ENTRY_VOLUME;
                            app_state = APP_STATE_AUDIO_SETTINGS;
                            break;
                    }
                } else if (res == SE_MENU_RESULT_BACK) {
                    // Back to wherever Settings was opened from — the
                    // main menu or the pause overlay.
                    app_state = s_settings_origin;
                }
                break;
            }

            case APP_STATE_CONTROLS: {
                draw_settings_scene(&world, &game);
                t_after_obs = esp_timer_get_time();
                // Engine-rendered Controls menu (se_ui): a gyro checkbox +
                // four keybind rows. The keybind value (a key icon/label)
                // is drawn by a game CUSTOM callback so the icon logic
                // stays game-side; the scancode rides in the row `ctx`.
                se_menu_row_t const rows[CONTROLS_ENTRY_COUNT] = {
                    [CONTROLS_ENTRY_GYRO]  = { .label = "Gyroscope", .kind = SE_MENU_VAL_CHECK,
                                               .checked = controls_settings_gyro_on() },
                    [CONTROLS_ENTRY_LEFT]  = { .label = "Left", .kind = SE_MENU_VAL_CUSTOM,
                                               .draw_value = controls_keybind_draw,
                                               .ctx = (void*)(uintptr_t)se_bindings_get(CONTROL_KEY_LEFT) },
                    [CONTROLS_ENTRY_RIGHT] = { .label = "Right", .kind = SE_MENU_VAL_CUSTOM,
                                               .draw_value = controls_keybind_draw,
                                               .ctx = (void*)(uintptr_t)se_bindings_get(CONTROL_KEY_RIGHT) },
                    [CONTROLS_ENTRY_ITEM]  = { .label = "Use item", .kind = SE_MENU_VAL_CUSTOM,
                                               .draw_value = controls_keybind_draw,
                                               .ctx = (void*)(uintptr_t)se_bindings_get(CONTROL_KEY_ITEM) },
                    [CONTROLS_ENTRY_PAUSE] = { .label = "Pause", .kind = SE_MENU_VAL_CUSTOM,
                                               .draw_value = controls_keybind_draw,
                                               .ctx = (void*)(uintptr_t)se_bindings_get(CONTROL_KEY_PAUSE) },
                };
                se_menu_def_t const def = {
                    .title = "Controls", .title_h = 36.0f, .subtitle = NULL,
                    .rows = rows, .row_count = CONTROLS_ENTRY_COUNT, .row_h = 46.0f,
                    .hint = "up / down to choose, enter to change, esc to leave",
                    .panel_w = 0.74f, .panel_h = 0.86f, .value_dx = 250.0f,
                };
                se_menu_t menu = { .def = &def, .cursor = s_controls_cursor };
                se_menu_draw(&menu, fb);

                se_menu_result_t res = SE_MENU_RESULT_NONE;
                if (menu_nav > 0)      se_menu_input(&menu, SE_MENU_ACT_UP);
                else if (menu_nav < 0) se_menu_input(&menu, SE_MENU_ACT_DOWN);
                if (pickup_pressed)    res = se_menu_input(&menu, SE_MENU_ACT_ACTIVATE);
                else if (menu_esc)     res = se_menu_input(&menu, SE_MENU_ACT_BACK);
                s_controls_cursor = menu.cursor;

                if (res == SE_MENU_RESULT_ACTIVATED) {
                    if (s_controls_cursor == CONTROLS_ENTRY_GYRO) {
                        controls_settings_set_gyro_on(!controls_settings_gyro_on());
                    } else {
                        // Rows past the gyro checkbox map 1:1 onto
                        // controls_key_t — the engine runs the blocking
                        // "press a key" capture and hands back a scancode.
                        controls_key_t const target =
                            (controls_key_t)(s_controls_cursor - CONTROLS_ENTRY_LEFT);
                        uint16_t const sc =
                            se_ui_capture_key(rows[s_controls_cursor].label);
                        if (sc != 0) se_bindings_set(target, sc);
                    }
                } else if (res == SE_MENU_RESULT_BACK) {
                    app_state = APP_STATE_SETTINGS;
                }
                break;
            }

            case APP_STATE_DISPLAY: {
                draw_settings_scene(&world, &game);
                t_after_obs = esp_timer_get_time();
                // Engine-rendered brightness sliders (se_ui RANGE rows).
                // The values are device-global (se_hw): read live each
                // frame, and LEFT/RIGHT step + persist + apply them via
                // se_hw. The screen slider has a floor (se_hw clamps) so a
                // sweep can't black the display out and trap the user.
                se_menu_row_t const rows[DISPLAY_ENTRY_COUNT] = {
                    [DISPLAY_ENTRY_SCREEN]   = { .label = "Screen",   .kind = SE_MENU_VAL_RANGE,
                                                 .range_pct = se_hw_get_display_brightness() },
                    [DISPLAY_ENTRY_KEYBOARD] = { .label = "Keyboard", .kind = SE_MENU_VAL_RANGE,
                                                 .range_pct = se_hw_get_keyboard_brightness() },
                    [DISPLAY_ENTRY_LEDS]     = { .label = "LEDs",     .kind = SE_MENU_VAL_RANGE,
                                                 .range_pct = se_hw_get_led_brightness() },
                };
                se_menu_def_t const def = {
                    .title = "Display", .title_h = 36.0f, .subtitle = NULL,
                    .rows = rows, .row_count = DISPLAY_ENTRY_COUNT, .row_h = 46.0f,
                    .hint = "up / down to choose, left / right to adjust, esc to leave",
                    .panel_w = 0.66f, .panel_h = 0.74f, .value_dx = 200.0f,
                };
                se_menu_t menu = { .def = &def, .cursor = s_display_cursor };
                se_menu_draw(&menu, fb);

                se_menu_result_t res = SE_MENU_RESULT_NONE;
                if (menu_nav > 0)        se_menu_input(&menu, SE_MENU_ACT_UP);
                else if (menu_nav < 0)   se_menu_input(&menu, SE_MENU_ACT_DOWN);
                if (menu_horiz < 0)      res = se_menu_input(&menu, SE_MENU_ACT_LEFT);
                else if (menu_horiz > 0) res = se_menu_input(&menu, SE_MENU_ACT_RIGHT);
                if (menu_esc)            res = se_menu_input(&menu, SE_MENU_ACT_BACK);
                s_display_cursor = menu.cursor;

                if (res == SE_MENU_RESULT_DECREMENT || res == SE_MENU_RESULT_INCREMENT) {
                    int const step = (res == SE_MENU_RESULT_INCREMENT)
                                         ? SE_HW_BRIGHTNESS_STEP_PCT
                                         : -SE_HW_BRIGHTNESS_STEP_PCT;
                    switch (s_display_cursor) {
                        case DISPLAY_ENTRY_SCREEN:
                            se_hw_set_display_brightness(
                                pct_step(se_hw_get_display_brightness(), step));
                            break;
                        case DISPLAY_ENTRY_KEYBOARD:
                            se_hw_set_keyboard_brightness(
                                pct_step(se_hw_get_keyboard_brightness(), step));
                            break;
                        case DISPLAY_ENTRY_LEDS:
                            se_hw_set_led_brightness(
                                pct_step(se_hw_get_led_brightness(), step));
                            break;
                    }
                } else if (res == SE_MENU_RESULT_BACK) {
                    app_state = APP_STATE_SETTINGS;
                }
                break;
            }

            case APP_STATE_AUDIO_SETTINGS: {
                draw_settings_scene(&world, &game);
                t_after_obs = esp_timer_get_time();
                // Engine-rendered menu (se_ui): a device-global Volume
                // slider (RANGE row, LEFT/RIGHT, persisted+applied by
                // se_hw on the active output) over the three app-audio
                // toggles. Rebuilt each frame so the value + [X]/[ ] states
                // track live.
                se_menu_row_t const rows[AUDIO_ENTRY_COUNT] = {
                    [AUDIO_ENTRY_VOLUME] = { .label = "Volume", .kind = SE_MENU_VAL_RANGE,
                                             .range_pct = se_hw_get_volume() },
                    [AUDIO_ENTRY_MUSIC]  = { .label = "Music", .kind = SE_MENU_VAL_CHECK,
                                             .checked = audio_settings_music_on() },
                    [AUDIO_ENTRY_SFX]    = { .label = "Sound effects", .kind = SE_MENU_VAL_CHECK,
                                             .checked = audio_settings_sfx_on() },
                    [AUDIO_ENTRY_HUM]    = { .label = "Engine hum", .kind = SE_MENU_VAL_CHECK,
                                             .checked = audio_settings_hum_on() },
                };
                se_menu_def_t const def = {
                    .title = "Audio", .title_h = 36.0f, .subtitle = NULL,
                    .rows = rows, .row_count = AUDIO_ENTRY_COUNT, .row_h = 44.0f,
                    .hint = "up / down choose, left / right adjust, enter toggle, esc back",
                    .panel_w = 0.74f, .panel_h = 0.72f, .value_dx = 230.0f,
                };
                se_menu_t menu = { .def = &def, .cursor = s_audio_cursor };
                se_menu_draw(&menu, fb);

                se_menu_result_t res = SE_MENU_RESULT_NONE;
                if (menu_nav > 0)        se_menu_input(&menu, SE_MENU_ACT_UP);
                else if (menu_nav < 0)   se_menu_input(&menu, SE_MENU_ACT_DOWN);
                if (menu_horiz < 0)      res = se_menu_input(&menu, SE_MENU_ACT_LEFT);
                else if (menu_horiz > 0) res = se_menu_input(&menu, SE_MENU_ACT_RIGHT);
                if (pickup_pressed)      res = se_menu_input(&menu, SE_MENU_ACT_ACTIVATE);
                else if (menu_esc)       res = se_menu_input(&menu, SE_MENU_ACT_BACK);
                s_audio_cursor = menu.cursor;

                if (res == SE_MENU_RESULT_ACTIVATED) {
                    switch (s_audio_cursor) {
                        case AUDIO_ENTRY_MUSIC:
                            audio_settings_set_music_on(!audio_settings_music_on());
                            break;
                        case AUDIO_ENTRY_SFX:
                            audio_settings_set_sfx_on(!audio_settings_sfx_on());
                            break;
                        case AUDIO_ENTRY_HUM:
                            audio_settings_set_hum_on(!audio_settings_hum_on());
                            break;
                    }
                } else if (res == SE_MENU_RESULT_DECREMENT || res == SE_MENU_RESULT_INCREMENT) {
                    // Only the Volume row reports DEC/INC (se_ui gates this
                    // on the RANGE kind), so no cursor switch is needed.
                    int const step = (res == SE_MENU_RESULT_INCREMENT)
                                         ? SE_HW_VOLUME_STEP_PCT : -SE_HW_VOLUME_STEP_PCT;
                    se_hw_set_volume(pct_step(se_hw_get_volume(), step));
                } else if (res == SE_MENU_RESULT_BACK) {
                    app_state = APP_STATE_SETTINGS;
                }
                break;
            }

            case APP_STATE_STATS_VIEW: {
                t_after_obs = esp_timer_get_time();
                draw_stats_view();
                if (pickup_pressed || menu_esc) {
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_UPGRADE: {
                t_after_obs = esp_timer_get_time();
                int const slots = upgrade_slot_count();
                if (slots == 0) {
                    // No attachment slots yet — bespoke message panel
                    // (not a list), enter/esc returns to the main menu.
                    draw_menu_panel_size(0.60f, 0.50f);
                    float const fbh = pax_buf_get_heightf(fb);
                    float const lx  = menu_left_x(0.60f);
                    draw_left(lx, fbh * 0.34f, 40.0f, MENU_COL_TITLE, "Upgrade Ship");
                    draw_left(lx, fbh * 0.52f, 20.0f, MENU_COL_NORMAL, "No attachment slots yet.");
                    draw_left(lx, fbh * 0.90f, 14.0f, MENU_COL_HINT, "press enter or esc to return");
                    if (pickup_pressed || menu_esc) app_state = APP_STATE_MENU;
                    break;
                }
                // Engine-rendered slot list (se_ui): one TEXT row per slot
                // showing the fitted attachment's name.
                char          labels[2][16];
                se_menu_row_t rows[2] = {0};
                for (int i = 0; i < slots; i++) {
                    snprintf(labels[i], sizeof(labels[i]), "Slot %d", i + 1);
                    rows[i].label = labels[i];
                    rows[i].kind  = SE_MENU_VAL_TEXT;
                    rows[i].value = attachment_name((attachment_id_t)*upgrade_slot_ptr(i));
                }
                se_menu_def_t const def = {
                    .title = "Upgrade Ship", .title_h = 36.0f, .subtitle = NULL,
                    .rows = rows, .row_count = slots, .row_h = 46.0f,
                    .hint = "up / down to choose a slot, enter to change, esc to leave",
                    .panel_w = 0.70f, .panel_h = 0.62f, .value_dx = 180.0f,
                };
                se_menu_t menu = { .def = &def, .cursor = s_upgrade_cursor };
                se_menu_draw(&menu, fb);

                se_menu_result_t res = SE_MENU_RESULT_NONE;
                if (menu_nav > 0)      se_menu_input(&menu, SE_MENU_ACT_UP);
                else if (menu_nav < 0) se_menu_input(&menu, SE_MENU_ACT_DOWN);
                if (pickup_pressed)    res = se_menu_input(&menu, SE_MENU_ACT_ACTIVATE);
                else if (menu_esc)     res = se_menu_input(&menu, SE_MENU_ACT_BACK);
                s_upgrade_cursor = menu.cursor;

                if (res == SE_MENU_RESULT_ACTIVATED) {
                    // Open the picker for the selected slot, starting on
                    // whatever it currently holds.
                    s_upgrade_slot        = s_upgrade_cursor;
                    s_upgrade_pick_cursor = *upgrade_slot_ptr(s_upgrade_slot);
                    if (s_upgrade_pick_cursor < 0
                        || s_upgrade_pick_cursor >= ATTACH_ID_COUNT) {
                        s_upgrade_pick_cursor = 0;
                    }
                    app_state = APP_STATE_UPGRADE_PICK;
                } else if (res == SE_MENU_RESULT_BACK) {
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_UPGRADE_PICK: {
                t_after_obs = esp_timer_get_time();
                // Engine-rendered attachment picker (se_ui): every
                // catalogued attachment + "[empty]"; the one fitted in the
                // OTHER slot is tagged "(in slot N)" and blocked below.
                int     const other_slot = (s_upgrade_slot == 0) ? 1 : 0;
                bool    const other_used = (other_slot < upgrade_slot_count());
                int32_t const other_val  = other_used ? *upgrade_slot_ptr(other_slot) : ATTACH_NONE;
                char          annot[ATTACH_ID_COUNT][24];
                se_menu_row_t rows[ATTACH_ID_COUNT] = {0};
                for (int i = 0; i < ATTACH_ID_COUNT; i++) {
                    rows[i].label = attachment_name((attachment_id_t)i);
                    rows[i].kind  = SE_MENU_VAL_NONE;
                    if (i != ATTACH_NONE && i == other_val) {
                        snprintf(annot[i], sizeof(annot[i]), "(in slot %d)", other_slot + 1);
                        rows[i].kind  = SE_MENU_VAL_TEXT;
                        rows[i].value = annot[i];
                    }
                }
                char title[24];
                snprintf(title, sizeof(title), "Slot %d", s_upgrade_slot + 1);
                se_menu_def_t const def = {
                    .title = title, .title_h = 36.0f, .subtitle = NULL,
                    .rows = rows, .row_count = ATTACH_ID_COUNT, .row_h = 44.0f,
                    .hint = "up / down to choose, enter to equip, esc to cancel",
                    .panel_w = 0.66f, .panel_h = 0.66f, .value_dx = 170.0f,
                };
                se_menu_t menu = { .def = &def, .cursor = s_upgrade_pick_cursor };
                se_menu_draw(&menu, fb);

                se_menu_result_t res = SE_MENU_RESULT_NONE;
                if (menu_nav > 0)      se_menu_input(&menu, SE_MENU_ACT_UP);
                else if (menu_nav < 0) se_menu_input(&menu, SE_MENU_ACT_DOWN);
                if (pickup_pressed)    res = se_menu_input(&menu, SE_MENU_ACT_ACTIVATE);
                else if (menu_esc)     res = se_menu_input(&menu, SE_MENU_ACT_BACK);
                s_upgrade_pick_cursor = menu.cursor;

                if (res == SE_MENU_RESULT_ACTIVATED) {
                    // Block equipping a (non-empty) attachment already in
                    // the other slot — no duplicates. Ignore the press.
                    if (s_upgrade_pick_cursor != ATTACH_NONE
                        && s_upgrade_pick_cursor == other_val) {
                        break;
                    }
                    *upgrade_slot_ptr(s_upgrade_slot) = s_upgrade_pick_cursor;
                    save_write_slot(s_active_slot, &s_save);
                    app_state = APP_STATE_UPGRADE;
                } else if (res == SE_MENU_RESULT_BACK) {
                    app_state = APP_STATE_UPGRADE;
                }
                break;
            }

            case APP_STATE_CREDITS: {
                t_after_obs = esp_timer_get_time();
                if (menu_nav != 0) {
                    // UP (menu_nav +1) scrolls toward the top of the
                    // roll, DOWN (-1) toward the bottom.
                    s_credits_scroll -= (float)menu_nav * CREDITS_SCROLL_STEP;
                    if (s_credits_scroll < 0.0f) s_credits_scroll = 0.0f;
                    float const max = credits_max_scroll();
                    if (s_credits_scroll > max) s_credits_scroll = max;
                }
                draw_credits();
                if (pickup_pressed || menu_esc) {
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_PLAYING: {
                // Shadows are already on the floor (drawn between
                // the floor base and the lines above); the depth-
                // buffered scene (obstacles + ship) goes on top.
                render_run_scene(&world, &game, true);
                t_after_obs = esp_timer_get_time();
                game_draw_sparks(fb, &game);
                // Spark shower from a shield-absorbed hit (Phase 9.2)
                // — only live during the invuln window; a no-op
                // otherwise.
                game_draw_crash_sparks(fb, &game);
                // Violet ring around the ship while the shield's
                // invuln window is open.
                game_draw_shield(fb, &game);
                if (stage_banner_visible(&world)) {
                    // Rest areas (pre-stage-1 lead-in + between-stage
                    // breathers) announce the upcoming stage number
                    // in their final stretch. w->stage is N during
                    // the rest that leads into stage N+1, and 0
                    // during the pre-run intro rest.
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_pause_hint();
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_speed_readout(game.ship_speed_z);
                draw_sun_readout(game.sun_y);
                draw_debug_readout(&game, s_godmode);
                draw_boost_indicator(&game);
                draw_jump_inventory(&game);
                draw_shield_inventory(&game);
                draw_checkpoint_inventory(&game);

                // Track peak stage reached this run.
                if ((int)world.stage > s_peak_stage) s_peak_stage = (int)world.stage;

                if (crashed) {
                    // Head-on crash. Fire the explosion SFX first —
                    // the audio task then has a fresh sample to play
                    // through any cache-disable stall — then tear
                    // down the run's persistent voices, spawn the
                    // spark shower in place of the ship, and enter
                    // CRASHING. The world keeps flowing and the
                    // sparks animate there until they burn out
                    // (≈ the crash SFX length), at which point the
                    // physics pass flips to GAME_OVER. The slow
                    // end-of-run flash save stays deferred to
                    // GAME_OVER's first frame.
                    sfx_crash_play();
                    end_run_audio();
                    game_crash_burst(&game);
                    s_run_was_crash   = true;
                    s_crash_anim_time = 0.0;
                    s_gameover_flavour_idx = (int)((uint32_t)esp_timer_get_time() % (uint32_t)GAMEOVER_FLAVOUR_COUNT);
                    app_state = APP_STATE_CRASHING;
                    input_set_mode(INPUT_MODE_GAME_OVER);
                } else if (stalled) {
                    // Ship coasted to a halt (shadow stall / sunset).
                    // No explosion SFX — just stop the run audio and
                    // hold the frozen scene a beat in STALL_OUT so
                    // the player registers the stall before the
                    // game-over panel appears.
                    end_run_audio();
                    s_run_was_crash   = false;
                    s_stall_hold_time = 0.0;
                    s_gameover_flavour_idx = (int)((uint32_t)esp_timer_get_time() % (uint32_t)GAMEOVER_FLAVOUR_COUNT);
                    app_state = APP_STATE_STALL_OUT;
                    input_set_mode(INPUT_MODE_GAME_OVER);
                } else if (pause_toggle) {
                    s_pause_cursor = PAUSE_ENTRY_RESUME;
                    app_state      = APP_STATE_PAUSED;
                    input_set_mode(INPUT_MODE_PAUSED);
                    pause_audio_for_pause_menu();
                }
                break;
            }

            case APP_STATE_CRASHING: {
                // The world still flows (physics advanced it above);
                // the ship is gone, replaced by the spark shower.
                // No pause hint, no input — the physics pass drops
                // us into GAME_OVER once the sparks burn out.
                render_run_scene(&world, &game, false);
                t_after_obs = esp_timer_get_time();
                game_draw_crash_sparks(fb, &game);
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                break;
            }

            case APP_STATE_STALL_OUT: {
                // Frozen scene held for a beat after the stall. The
                // ship is still drawn — it sat down, it didn't blow
                // up. No input; the physics pass times out into
                // GAME_OVER.
                render_run_scene(&world, &game, true);
                t_after_obs = esp_timer_get_time();
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                break;
            }

            case APP_STATE_PAUSED: {
                // Render the world frozen behind the overlay (same
                // approach as GAME_OVER — obstacles + ship in their
                // last positions). The physics step above is gated
                // on PLAYING so nothing moves.
                render_run_scene(&world, &game, true);
                t_after_obs = esp_timer_get_time();
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                // Engine-rendered pause overlay (se_ui), over the frozen run.
                static char const* const labels[PAUSE_ENTRY_COUNT] = {
                    [PAUSE_ENTRY_RESUME]   = "Resume",
                    [PAUSE_ENTRY_SETTINGS] = "Settings",
                    [PAUSE_ENTRY_ABORT]    = "Abort run",
                };
                se_menu_row_t rows[PAUSE_ENTRY_COUNT] = {0};
                for (int i = 0; i < PAUSE_ENTRY_COUNT; i++) {
                    rows[i].label = labels[i];
                    rows[i].kind  = SE_MENU_VAL_NONE;
                }
                se_menu_def_t const def = {
                    .title = "PAUSED", .title_h = 48.0f, .subtitle = NULL,
                    .rows = rows, .row_count = PAUSE_ENTRY_COUNT, .row_h = 44.0f,
                    .hint = "up / down to choose, enter to confirm, F4 to resume",
                    .panel_w = 0.55f, .panel_h = 0.62f, .value_dx = 0.0f,
                };
                se_menu_t menu = { .def = &def, .cursor = s_pause_cursor };
                se_menu_draw(&menu, fb);

                se_menu_result_t res = SE_MENU_RESULT_NONE;
                if (menu_nav > 0)      se_menu_input(&menu, SE_MENU_ACT_UP);
                else if (menu_nav < 0) se_menu_input(&menu, SE_MENU_ACT_DOWN);
                if (pickup_pressed)    res = se_menu_input(&menu, SE_MENU_ACT_ACTIVATE);
                s_pause_cursor = menu.cursor;

                if (pause_toggle) {
                    // F4 inside the pause overlay = Resume (matches
                    // the prompt at the bottom of the overlay).
                    app_state = APP_STATE_PLAYING;
                    input_set_mode(INPUT_MODE_PLAYING);
                    resume_audio_from_pause_menu();
                } else if (res == SE_MENU_RESULT_ACTIVATED) {
                    switch (s_pause_cursor) {
                        case PAUSE_ENTRY_RESUME:
                            app_state = APP_STATE_PLAYING;
                            input_set_mode(INPUT_MODE_PLAYING);
                            resume_audio_from_pause_menu();
                            break;
                        case PAUSE_ENTRY_SETTINGS:
                            // Open Settings over the frozen run. The
                            // run stays logically paused — no audio
                            // resume — and Esc walks back here.
                            s_settings_origin = APP_STATE_PAUSED;
                            s_settings_cursor = SETTINGS_ENTRY_CONTROLS;
                            app_state = APP_STATE_SETTINGS;
                            break;
                        case PAUSE_ENTRY_ABORT:
                            if (!run_end_committed) {
                                int    const peak_stage  = (s_peak_stage > (int)world.stage)
                                                             ? s_peak_stage : (int)world.stage;
                                double const run_seconds = s_run_play_seconds;
                                save_commit_run_end(s_active_slot, &s_save, SAVE_END_QUIT,
                                                    &game, peak_stage, run_seconds);
                                run_end_committed = true;
                            }
                            end_run_audio();
                            app_state = APP_STATE_MENU;
                            input_set_mode(INPUT_MODE_TITLE);
                            break;
                    }
                }
                break;
            }

            case APP_STATE_GAME_OVER: {
                // Deferred from the CRASHING / STALL_OUT hold states
                // so the slow flash write happens after the audio
                // teardown and the post-run animation. Runs once on
                // the first GAME_OVER frame; `run_end_committed`
                // keeps it from re-firing. `s_run_was_crash` carries
                // the real end cause (head-on crash vs stall/sunset)
                // so the save records the correct reason.
                if (!run_end_committed) {
                    commit_run_end(&game, &world, s_run_was_crash);
                    run_end_committed = true;
                }

                // World frozen at the end of the run. Sun readout
                // stays visible so Q/A nudging still works for
                // visually tuning the sunset threshold.
                // A crashed ship was blown to sparks during CRASHING
                // — don't resurrect it under the panel. A stalled
                // ship is still sitting on the track, so draw it.
                render_run_scene(&world, &game, !s_run_was_crash);
                t_after_obs = esp_timer_get_time();
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_game_over_overlay();
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);

                if (pickup_pressed) {
                    app_state = APP_STATE_MENU;
                    input_set_mode(INPUT_MODE_TITLE);
                }
                break;
            }

            case APP_STATE_CHECKPOINT_REDO: {
                // The run has been rewound to the checkpoint snapshot
                // (done in the physics pass). This is a pause-like
                // hold — the restored scene is frozen behind the
                // dialog (physics is gated on PLAYING), music keeps
                // playing, the run is NOT committed. Space resumes.
                render_run_scene(&world, &game, true);
                t_after_obs = esp_timer_get_time();
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                draw_checkpoint_redo_overlay();

                if (pickup_pressed && !s_redo_ignore_pickup) {
                    // Resume: spend the checkpoint and grant the
                    // shield's invulnerability window so the player
                    // gets a moment of grace on the way back in.
                    game.checkpoint_held = false;
                    game.shield_timer    = GAME_SHIELD_DURATION;
                    app_state = APP_STATE_PLAYING;
                    input_set_mode(INPUT_MODE_PLAYING);
                }
                s_redo_ignore_pickup = false;
                break;
            }
        }
        int64_t const t_after_fg = esp_timer_get_time();
        prof_obs_us    += t_after_obs - s_t_bg_end;
        prof_fgrest_us += t_after_fg  - t_after_obs;
        s_prof_fg_end   = t_after_fg;
        prof_frames    += 1;

        // FPS / per-stage breakdown over a ~1 s window. blit + vsync +
        // the buffer swap are the engine's now, so they fold into the
        // single "present" bucket (the gap between this on_render end and
        // the next on_update start, accumulated there).
        int64_t const window_us = t_after_fg - prof_window_start;
        if (window_us >= 1000000) {
            float const fps    = prof_frames * 1e6f / (float)window_us;
            float const inv_fr = 1.0f / (float)prof_frames;
            ESP_LOGI(TAG,
                     "FPS=%.1f  in=%.2f phys=%.2f bgkick=%.2f bgflr=%.2f bgwait=%.2f obs=%.2f fgrest=%.2f present=%.2f ms",
                     fps,
                     (float)prof_input_us   * inv_fr / 1000.0f,
                     (float)prof_phys_us    * inv_fr / 1000.0f,
                     (float)prof_bgkick_us  * inv_fr / 1000.0f,
                     (float)prof_bgflr_us   * inv_fr / 1000.0f,
                     (float)prof_bgwait_us  * inv_fr / 1000.0f,
                     (float)prof_obs_us     * inv_fr / 1000.0f,
                     (float)prof_fgrest_us  * inv_fr / 1000.0f,
                     (float)prof_present_us * inv_fr / 1000.0f);
            prof_input_us  = prof_phys_us = prof_bgkick_us = prof_bgflr_us = 0;
            prof_bgwait_us = prof_obs_us = prof_fgrest_us = prof_present_us = 0;
            prof_frames    = 0;
            prof_window_start = t_after_fg;
        }
}

// on_input — one input event the engine's pump didn't consume itself.
// Routes it into the game's input module, which latches it for the
// per-frame consume_* accessors read in on_update / on_render.
static void on_input(bsp_input_event_t const* ev, void* user) {
    (void)user;
    input_handle_event(ev);
}

// app_main — hand the run loop to the engine. The engine owns the device
// + display bootstrap, the framebuffers, vsync/blit, the input-queue pump
// (volume/jack + F1-exit) and the frame loop; the callbacks above supply
// the content. f1_exits=true: the engine returns to the launcher on F1.
void app_main(void) {
    static se_app_config_t const cfg = {
        .f1_exits      = true,
        .backdrop_argb = 0xFF000000u,
    };
    static se_app_callbacks_t const cb = {
        .on_init     = on_init,
        .on_input    = on_input,
        .on_update   = on_update,
        .on_backdrop = on_backdrop,
        .on_render   = on_render,
    };
    se_run(&cfg, &cb, NULL);
}
