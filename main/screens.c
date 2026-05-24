// =====================================================================
//  Race the Synth  --  non-gameplay screen states (see screens.h)
//  Frame functions + their draw helpers, split out of main.c. The case
//  bodies are verbatim (wrapped in do{}while(0) so `break;` still exits
//  the case); shared controller state comes from game_app.h.
// =====================================================================

#include "screens.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/device.h"
#include "esp_timer.h"
#include "attachments.h"
#include "audio_settings.h"
#include "controls_settings.h"
#include "game.h"
#include "game_app.h"
#include "game_ui.h"
#include "icons.h"
#include "input.h"
#include "keybind_ui.h"
#include "magicnumbers.h"
#include "render.h"
#include "save.h"
#include "se_audio.h"
#include "se_bindings.h"
#include "se_hw.h"
#include "se_scene.h"
#include "se_ui.h"
#include "world.h"

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

// Render the depth-buffered 3D scene for a run: clear the z-buffer,
// emit every obstacle and (optionally) the ship, then rasterize the
// deferred wireframe. The backdrop / floor / shadows must already be
// in the framebuffer — they are 2D layers drawn before this.
void render_run_scene(world_state_t const* w, game_state_t const* g,
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

void draw_game_over_overlay(void) {
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
void draw_checkpoint_redo_overlay(void) {
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

void screen_slot_select_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

void screen_menu_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

void screen_seed_input_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

void screen_settings_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

void screen_controls_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

void screen_display_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

void screen_audio_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

void screen_stats_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

                t_after_obs = esp_timer_get_time();
                draw_stats_view();
                if (pickup_pressed || menu_esc) {
                    app_state = APP_STATE_MENU;
                }
                break;
    } while (0);
}

void screen_upgrade_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

void screen_upgrade_pick_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

void screen_credits_frame(void)
{
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
    (void)menu_nav; (void)menu_horiz; (void)menu_esc; (void)menu_bs;
    (void)pause_toggle; (void)typed; (void)typed_d; (void)pickup_pressed;
    (void)crashed; (void)stalled;
    do {

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
    } while (0);
}

