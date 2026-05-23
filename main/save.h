#pragma once

#include <stdint.h>
#include <time.h>

#include "se_save.h"   // engine save-slot framework (se_save_*, se_save_peek_t)

// Persistence layer. SE_SAVE_SLOT_COUNT independent slots, each an NBT
// file under SAVE_PATH_PREFIX, written through the engine's se_save
// framework: the engine owns the slot files + the peek header; this game
// owns the save_data_t schema and its (de)serialisation. No autosave
// slot — we write to the active slot after every run.

#define SAVE_SLOT_COUNT  SE_SAVE_SLOT_COUNT
#define SAVE_PATH_PREFIX "/int/synthracer"

// Run-end reason codes (committed into stats.run_end_reason).
typedef enum {
    SAVE_END_NONE    = 0,
    SAVE_END_CRASH   = 1,
    SAVE_END_STALL   = 2,
    SAVE_END_SUNSET  = 3,
    SAVE_END_QUIT    = 4,
} save_end_reason_t;

// Symmetric per-run stats block. Used for *both* "this run just ended"
// (`last_run`) and "merged across every run on this slot" (`all_time`).
// The merge function (save.c) is the single place that knows which
// fields are max-tracked (peak across runs) versus sum-tracked (career
// totals). For `last_run` the four `runs_*` flag-counters are 0 or 1
// depending on how the run ended (`runs_total = 1` for any finished
// run that the player chose to commit; 0 if the entry has never been
// populated). In `all_time` they accumulate.
typedef struct {
    int64_t score;                  // max in all_time
    double  distance;               // sum in all_time
    int32_t stage_reached;          // max in all_time
    int32_t multiplier_max;         // max in all_time
    double  duration_s;             // sum in all_time
    int32_t pickups_speed_boost;    // sum
    int32_t pickups_tri;            // sum (Phase 6)
    int32_t pickups_jump;           // sum (Phase 9)
    int32_t pickups_shield;         // sum (Phase 9)
    int32_t runs_total;             // sum (last_run sets 1 on commit)
    int32_t runs_crashed;           // sum (last_run sets 0/1)
    int32_t runs_stalled;           // sum
    int32_t runs_sunset;            // sum
    int32_t runs_quit;              // sum (set by an Abort-to-Menu commit)
} run_stats_t;

// On-disk top-level state. Same struct shape is reused for `last_run`
// and `all_time`, so display code can render both with identical logic.
typedef struct {
    // The slot's peek header (date / kind / a display summary) is owned
    // by the engine (se_save_peek_t) — no mirror fields here. The game's
    // serialiser writes only the compounds below; save_write_slot builds
    // the engine peek's `info` string from stats.all_time at write time.
    struct {
        run_stats_t last_run;
        run_stats_t all_time;
    } stats;

    struct {
        int32_t level;
        int32_t points;
        // Each unlock is its own explicit boolean (INT32 0/1) — easy to
        // edit by hand via the savetool round-trip.
        int32_t unlock_speed_boost;
        int32_t unlock_multiplier;
        int32_t unlock_jump;
        int32_t unlock_magnet;
        int32_t unlock_starting_mult_2x;
        int32_t unlock_portal_easier_world;
        int32_t unlock_double_jump;
        int32_t unlock_shield;
        int32_t unlock_shield_attachment;
        int32_t unlock_apocalypse;
        int32_t unlock_starting_mult_3x;
        int32_t unlock_attach_slot2;
        int32_t unlock_left_wing_decal;
        int32_t unlock_double_portal;
        int32_t unlock_checkpoint;
        int32_t unlock_power_turning1;
        int32_t unlock_power_turning2;
        int32_t unlock_triple_jump;
        int32_t unlock_checkpoint2;
        int32_t unlock_enhanced_magnet;
        int32_t unlock_right_wing_decal;
        int32_t unlock_starting_mult_4x;
        int32_t unlock_starting_mult_max;
        int32_t unlock_labyrinth;
        // Equip slots (Phase 9.4). `attach_slots` is how many slots are
        // available (0/1/2 — unlocked via meta-progression; defaults to 2
        // for now). `attach1`/`attach2` hold the equipped attachment_id_t
        // per slot (ATTACH_NONE = empty).
        int32_t attach_slots;
        int32_t attach1, attach2;
        // Battery upgrade capacity (Phase 9.5). Max achievable charge,
        // a multiple of GAME_BATTERY_PER_INDICATOR (0/25/50/75/100);
        // 0 = no battery fitted. Set per level by meta-progression in
        // Phase 11; defaults to 100 for now so the battery is testable.
        int32_t battery_max_charge;
        int64_t last_custom_seed;
        int64_t last_seen_date;     // yyyymmdd
    } meta;

    struct {
        int32_t daily_done_1pt;
        int32_t daily_done_2pt;
        int32_t daily_done_3pt;
    } daily;
} save_data_t;

// Forward-declare so the commit helper can take a game state without
// including game.h transitively.
struct game_state_s;

// Fill `s` with defaults (zero everything, meta.level = 1).
void save_init_defaults(save_data_t* s);

// mkdir /int/synthracer (idempotent).
void save_init(void);

// 1 if the slot file exists, 0 otherwise. (Slot-select code reads the
// engine peek directly via se_save_peek(); no game-side peek wrapper.)
int  save_slot_exists(int slot);

// Full load. `s` is initialised to defaults first; unknown / missing
// tags keep their default values. Returns 0 on success, -1 on
// missing/corrupt/IO error.
int  save_load_slot(int slot, save_data_t* s);

// Full write. Formats the engine peek's display `info` string from
// stats.all_time, then writes the slot via se_save (kind = manual).
int  save_write_slot(int slot, save_data_t* s);

// Merge a just-ended run's stats into the all-time accumulators.
// Max-tracked fields (score, stage_reached, multiplier_max) take the
// larger of the two; everything else (distance, duration, pickups,
// runs_*) is summed. The single place that knows the per-field rule —
// callers just hand it both pointers.
void run_stats_merge_into_all_time(run_stats_t* all_time, run_stats_t const* last);

// End-of-run helper. Writes `last_run` from the game state, merges it
// into `all_time`, then writes the file. Single call site in main.c
// for STATE_GAME_OVER and the pause-menu's Abort-to-Menu path. `g`
// supplies score / distance / multiplier_max / pickup counters;
// `peak_stage` is tracked by main.c; `run_seconds` is the wallclock
// duration of the run.
int  save_commit_run_end(int slot, save_data_t* s, save_end_reason_t reason,
                         struct game_state_s const* g, int peak_stage,
                         double run_seconds);
