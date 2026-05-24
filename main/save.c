#include "save.h"

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "se_save.h"

// Defined in save_nbt.c — (de)serialiser for the game's save_data_t.
void save_write_state(NbtWriter* w, save_data_t const* s);
void save_read_state(NbtReader* r, save_data_t* s);

// Stamped into every slot's engine peek header.
#define SAVE_GAME_NAME    "Race the Synth"
#define SAVE_GAME_VERSION 1

// Engine save callbacks: adapt the typed (de)serialisers to the engine's
// void* contract. By the time these run the engine has already written
// the root compound + peek header (writer) or consumed the root tag
// (reader); save_read_state skips the engine's se_peek block as unknown.
static void game_serialize(NbtWriter* w, void const* game_data) {
    save_write_state(w, (save_data_t const*)game_data);
}
static void game_deserialize(NbtReader* r, void* game_data) {
    save_read_state(r, (save_data_t*)game_data);
}

void save_init_defaults(save_data_t* s) {
    memset(s, 0, sizeof(*s));
    s->meta.level = 1;
    // Phase 9.4: two open equip slots by default (meta-progression will
    // gate this to 0/1/2 later). attach1/attach2 stay ATTACH_NONE (0).
    s->meta.attach_slots = 2;
    // Phase 9.5: full battery by default (meta-progression will gate the
    // capacity 0/25/50/75/100 later).
    s->meta.battery_max_charge = (int32_t)GAME_BATTERY_MAX;
}

void save_init(void) {
    se_save_config_t const cfg = {
        .dir          = SAVE_PATH_PREFIX,
        .game_name    = SAVE_GAME_NAME,
        .game_version = SAVE_GAME_VERSION,
        .serialize    = game_serialize,
        .deserialize  = game_deserialize,
    };
    se_save_init(&cfg);
}

int save_slot_exists(int slot) {
    return se_save_slot_exists(slot);
}

int save_load_slot(int slot, save_data_t* s) {
    save_init_defaults(s);
    return se_save_load_slot(slot, s);
}

int save_write_slot(int slot, save_data_t* s) {
    // Build the one-line slot-select summary carried in the engine peek's
    // free-text `info` field (shown by draw_slot_select alongside the
    // engine's timestamp). Race the Synth only ever saves manually.
    char info[SE_SAVE_INFO_MAX];
    snprintf(info, sizeof(info), "best %lld  stage %d  runs %d",
             (long long)s->stats.all_time.score,
             (int)s->stats.all_time.stage_reached,
             (int)s->stats.all_time.runs_total);
    return se_save_write_slot(slot, SE_SAVE_MANUAL, s, info);
}

void run_stats_merge_into_all_time(run_stats_t* all, run_stats_t const* last) {
    if (last->score          > all->score)          all->score          = last->score;
    if (last->stage_reached  > all->stage_reached)  all->stage_reached  = last->stage_reached;
    if (last->multiplier_max > all->multiplier_max) all->multiplier_max = last->multiplier_max;

    all->distance            += last->distance;
    all->duration_s          += last->duration_s;
    all->pickups_speed_boost += last->pickups_speed_boost;
    all->pickups_tri         += last->pickups_tri;
    all->pickups_jump        += last->pickups_jump;
    all->pickups_shield      += last->pickups_shield;
    all->runs_total          += last->runs_total;
    all->runs_crashed        += last->runs_crashed;
    all->runs_stalled        += last->runs_stalled;
    all->runs_sunset         += last->runs_sunset;
    all->runs_quit           += last->runs_quit;
}

int save_commit_run_end(int slot, save_data_t* s, save_end_reason_t reason,
                        game_state_t const* g, int peak_stage, double run_seconds) {
    if (!s || !g) return -1;

    // Build the last-run snapshot from the live game state. Pickups
    // are sourced from the game struct; the four `runs_*` counters
    // are flags that the merge will sum into all_time.
    run_stats_t last = {0};
    last.score                = (int64_t)g->score;
    last.distance             = g->distance_traveled;
    last.stage_reached        = peak_stage;
    last.multiplier_max       = g->multiplier_max;
    last.duration_s           = run_seconds;
    last.pickups_speed_boost  = g->pickups_speed_boost;
    last.pickups_tri          = g->pickups_tri;
    last.pickups_jump         = g->pickups_jump;
    last.pickups_shield       = g->pickups_shield;
    last.runs_total           = 1;
    switch (reason) {
        case SAVE_END_CRASH:  last.runs_crashed = 1; break;
        case SAVE_END_STALL:  last.runs_stalled = 1; break;
        case SAVE_END_SUNSET: last.runs_sunset  = 1; break;
        case SAVE_END_QUIT:   last.runs_quit    = 1; break;
        default: break;
    }

    s->stats.last_run = last;
    run_stats_merge_into_all_time(&s->stats.all_time, &last);

    return save_write_slot(slot, s);
}
