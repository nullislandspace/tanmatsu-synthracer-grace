// =====================================================================
//  Race the Synth  --  gameplay states (see play_states.h)
//  Frame functions split out of main.c. The case bodies are verbatim
//  (wrapped in do{}while(0) so `break;` still exits the case); shared
//  controller state comes from game_app.h.
// =====================================================================

#include "play_states.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_timer.h"
#include "game.h"
#include "game_app.h"
#include "game_ui.h"
#include "hud.h"
#include "input.h"
#include "magicnumbers.h"
#include "save.h"
#include "screens.h"
#include "se_ui.h"
#include "sfx/sfx_crash.h"
#include "world.h"

// Game-over flavour overlays + the run-scene helper live in screens.c
// (shared); only the gameplay frame functions live here.

void play_playing_frame(void)
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

                // Shadows are already on the floor (drawn between
                // the floor base and the lines above); the depth-
                // buffered scene (obstacles + ship) goes on top.
                render_rasterize_scene();   // scene prepared in on_backdrop
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
    } while (0);
}

void play_crashing_frame(void)
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

                // The world still flows (physics advanced it above);
                // the ship is gone, replaced by the spark shower.
                // No pause hint, no input — the physics pass drops
                // us into GAME_OVER once the sparks burn out.
                render_rasterize_scene();   // scene prepared in on_backdrop
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
    } while (0);
}

void play_stall_out_frame(void)
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

                // Frozen scene held for a beat after the stall. The
                // ship is still drawn — it sat down, it didn't blow
                // up. No input; the physics pass times out into
                // GAME_OVER.
                render_rasterize_scene();   // scene prepared in on_backdrop
                t_after_obs = esp_timer_get_time();
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                break;
    } while (0);
}

void play_paused_frame(void)
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

                // Render the world frozen behind the overlay (same
                // approach as GAME_OVER — obstacles + ship in their
                // last positions). The physics step above is gated
                // on PLAYING so nothing moves.
                render_rasterize_scene();   // scene prepared in on_backdrop
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
    } while (0);
}

void play_game_over_frame(void)
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
                render_rasterize_scene();   // scene prepared in on_backdrop
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
    } while (0);
}

void play_checkpoint_redo_frame(void)
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

                // The run has been rewound to the checkpoint snapshot
                // (done in the physics pass). This is a pause-like
                // hold — the restored scene is frozen behind the
                // dialog (physics is gated on PLAYING), music keeps
                // playing, the run is NOT committed. Space resumes.
                render_rasterize_scene();   // scene prepared in on_backdrop
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
    } while (0);
}

