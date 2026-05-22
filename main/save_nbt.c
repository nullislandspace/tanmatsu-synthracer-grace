#include <string.h>

#include "nbt.h"
#include "save.h"

// All field names live in one place. Keep them stable so old saves
// continue to load even after the in-memory struct evolves.

static void write_peek(NbtWriter* w, save_data_t const* s) {
    nbt_write_compound(w, "peek");
    nbt_write_int64(w, "last_played_unix", s->last_played_unix);
    nbt_write_int64(w, "score_best",       s->peek_score_best);
    nbt_write_int32(w, "stage_best",       s->peek_stage_best);
    nbt_write_int32(w, "runs_total",       s->peek_runs_total);
    nbt_write_end(w);
}

// Symmetric writer for one run_stats_t — same field names in both
// `last_run` and `all_time` compounds so display + savetool reading
// is uniform.
static void write_run_stats(NbtWriter* w, char const* compound_name, run_stats_t const* rs) {
    nbt_write_compound(w, compound_name);
    nbt_write_int64 (w, "score",               rs->score);
    nbt_write_double(w, "distance",            rs->distance);
    nbt_write_int32 (w, "stage_reached",       rs->stage_reached);
    nbt_write_int32 (w, "multiplier_max",      rs->multiplier_max);
    nbt_write_double(w, "duration_s",          rs->duration_s);
    nbt_write_int32 (w, "pickups_speed_boost", rs->pickups_speed_boost);
    nbt_write_int32 (w, "pickups_tri",         rs->pickups_tri);
    nbt_write_int32 (w, "pickups_jump",        rs->pickups_jump);
    nbt_write_int32 (w, "pickups_shield",      rs->pickups_shield);
    nbt_write_int32 (w, "runs_total",          rs->runs_total);
    nbt_write_int32 (w, "runs_crashed",        rs->runs_crashed);
    nbt_write_int32 (w, "runs_stalled",        rs->runs_stalled);
    nbt_write_int32 (w, "runs_sunset",         rs->runs_sunset);
    nbt_write_int32 (w, "runs_quit",           rs->runs_quit);
    nbt_write_end(w);
}

static void write_stats(NbtWriter* w, save_data_t const* s) {
    nbt_write_compound(w, "stats");
    write_run_stats(w, "last_run", &s->stats.last_run);
    write_run_stats(w, "all_time", &s->stats.all_time);
    nbt_write_end(w);
}

static void write_meta(NbtWriter* w, save_data_t const* s) {
    nbt_write_compound(w, "meta");
    nbt_write_int32(w, "level",                       s->meta.level);
    nbt_write_int32(w, "points",                      s->meta.points);
    nbt_write_int32(w, "unlock_speed_boost",          s->meta.unlock_speed_boost);
    nbt_write_int32(w, "unlock_multiplier",           s->meta.unlock_multiplier);
    nbt_write_int32(w, "unlock_jump",                 s->meta.unlock_jump);
    nbt_write_int32(w, "unlock_magnet",               s->meta.unlock_magnet);
    nbt_write_int32(w, "unlock_starting_mult_2x",     s->meta.unlock_starting_mult_2x);
    nbt_write_int32(w, "unlock_portal_easier_world",  s->meta.unlock_portal_easier_world);
    nbt_write_int32(w, "unlock_double_jump",          s->meta.unlock_double_jump);
    nbt_write_int32(w, "unlock_shield",               s->meta.unlock_shield);
    nbt_write_int32(w, "unlock_shield_attachment",    s->meta.unlock_shield_attachment);
    nbt_write_int32(w, "unlock_apocalypse",           s->meta.unlock_apocalypse);
    nbt_write_int32(w, "unlock_starting_mult_3x",     s->meta.unlock_starting_mult_3x);
    nbt_write_int32(w, "unlock_attach_slot2",         s->meta.unlock_attach_slot2);
    nbt_write_int32(w, "unlock_left_wing_decal",      s->meta.unlock_left_wing_decal);
    nbt_write_int32(w, "unlock_double_portal",        s->meta.unlock_double_portal);
    nbt_write_int32(w, "unlock_checkpoint",           s->meta.unlock_checkpoint);
    nbt_write_int32(w, "unlock_power_turning1",       s->meta.unlock_power_turning1);
    nbt_write_int32(w, "unlock_power_turning2",       s->meta.unlock_power_turning2);
    nbt_write_int32(w, "unlock_triple_jump",          s->meta.unlock_triple_jump);
    nbt_write_int32(w, "unlock_checkpoint2",          s->meta.unlock_checkpoint2);
    nbt_write_int32(w, "unlock_enhanced_magnet",      s->meta.unlock_enhanced_magnet);
    nbt_write_int32(w, "unlock_right_wing_decal",     s->meta.unlock_right_wing_decal);
    nbt_write_int32(w, "unlock_starting_mult_4x",     s->meta.unlock_starting_mult_4x);
    nbt_write_int32(w, "unlock_starting_mult_max",    s->meta.unlock_starting_mult_max);
    nbt_write_int32(w, "unlock_labyrinth",            s->meta.unlock_labyrinth);
    nbt_write_int32(w, "attach_slots",                s->meta.attach_slots);
    nbt_write_int32(w, "attach1",                     s->meta.attach1);
    nbt_write_int32(w, "attach2",                     s->meta.attach2);
    nbt_write_int32(w, "battery_max_charge",          s->meta.battery_max_charge);
    nbt_write_int64(w, "last_custom_seed",            s->meta.last_custom_seed);
    nbt_write_int64(w, "last_seen_date",              s->meta.last_seen_date);
    nbt_write_end(w);
}

static void write_daily(NbtWriter* w, save_data_t const* s) {
    nbt_write_compound(w, "daily");
    nbt_write_int32(w, "daily_done_1pt", s->daily.daily_done_1pt);
    nbt_write_int32(w, "daily_done_2pt", s->daily.daily_done_2pt);
    nbt_write_int32(w, "daily_done_3pt", s->daily.daily_done_3pt);
    nbt_write_end(w);
}

void save_write_state(NbtWriter* w, save_data_t const* s) {
    write_peek(w, s);
    write_stats(w, s);
    write_meta(w, s);
    write_daily(w, s);
}

// --- Reader ---

static void read_peek(NbtReader* r, save_data_t* s) {
    char name[64];
    int type;
    while ((type = nbt_read_tag(r, name, sizeof(name))) != NBT_END) {
        if (type < 0) break;
        if      (type == NBT_INT64 && strcmp(name, "last_played_unix") == 0) s->last_played_unix = nbt_read_int64(r);
        else if (type == NBT_INT64 && strcmp(name, "score_best")       == 0) s->peek_score_best  = nbt_read_int64(r);
        else if (type == NBT_INT32 && strcmp(name, "stage_best")       == 0) s->peek_stage_best  = nbt_read_int32(r);
        else if (type == NBT_INT32 && strcmp(name, "runs_total")       == 0) s->peek_runs_total  = nbt_read_int32(r);
        else nbt_skip_payload(r, type);
    }
}

static void read_run_stats(NbtReader* r, run_stats_t* rs) {
    char name[64];
    int type;
    while ((type = nbt_read_tag(r, name, sizeof(name))) != NBT_END) {
        if (type < 0) break;
        if      (type == NBT_INT64  && !strcmp(name, "score"))               rs->score               = nbt_read_int64(r);
        else if (type == NBT_DOUBLE && !strcmp(name, "distance"))            rs->distance            = nbt_read_double(r);
        else if (type == NBT_INT32  && !strcmp(name, "stage_reached"))       rs->stage_reached       = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "multiplier_max"))      rs->multiplier_max      = nbt_read_int32(r);
        else if (type == NBT_DOUBLE && !strcmp(name, "duration_s"))          rs->duration_s          = nbt_read_double(r);
        else if (type == NBT_INT32  && !strcmp(name, "pickups_speed_boost")) rs->pickups_speed_boost = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "pickups_tri"))         rs->pickups_tri         = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "pickups_jump"))        rs->pickups_jump        = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "pickups_shield"))      rs->pickups_shield      = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "runs_total"))          rs->runs_total          = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "runs_crashed"))        rs->runs_crashed        = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "runs_stalled"))        rs->runs_stalled        = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "runs_sunset"))         rs->runs_sunset         = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "runs_quit"))           rs->runs_quit           = nbt_read_int32(r);
        else nbt_skip_payload(r, type);
    }
}

static void read_stats(NbtReader* r, save_data_t* s) {
    char name[64];
    int type;
    while ((type = nbt_read_tag(r, name, sizeof(name))) != NBT_END) {
        if (type < 0) break;
        if      (type == NBT_COMPOUND && !strcmp(name, "last_run")) read_run_stats(r, &s->stats.last_run);
        else if (type == NBT_COMPOUND && !strcmp(name, "all_time")) read_run_stats(r, &s->stats.all_time);
        else nbt_skip_payload(r, type);
    }
}

static void read_meta(NbtReader* r, save_data_t* s) {
    char name[64];
    int type;
    while ((type = nbt_read_tag(r, name, sizeof(name))) != NBT_END) {
        if (type < 0) break;
        if      (type == NBT_INT32 && !strcmp(name, "level"))                       s->meta.level                       = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "points"))                      s->meta.points                      = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_speed_boost"))          s->meta.unlock_speed_boost          = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_multiplier"))           s->meta.unlock_multiplier           = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_jump"))                 s->meta.unlock_jump                 = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_magnet"))               s->meta.unlock_magnet               = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_starting_mult_2x"))     s->meta.unlock_starting_mult_2x     = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_portal_easier_world"))  s->meta.unlock_portal_easier_world  = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_double_jump"))          s->meta.unlock_double_jump          = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_shield"))               s->meta.unlock_shield               = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_shield_attachment"))    s->meta.unlock_shield_attachment    = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_apocalypse"))           s->meta.unlock_apocalypse           = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_starting_mult_3x"))     s->meta.unlock_starting_mult_3x     = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_attach_slot2"))         s->meta.unlock_attach_slot2         = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_left_wing_decal"))      s->meta.unlock_left_wing_decal      = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_double_portal"))        s->meta.unlock_double_portal        = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_checkpoint"))           s->meta.unlock_checkpoint           = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_power_turning1"))       s->meta.unlock_power_turning1       = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_power_turning2"))       s->meta.unlock_power_turning2       = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_triple_jump"))          s->meta.unlock_triple_jump          = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_checkpoint2"))          s->meta.unlock_checkpoint2          = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_enhanced_magnet"))      s->meta.unlock_enhanced_magnet      = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_right_wing_decal"))     s->meta.unlock_right_wing_decal     = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_starting_mult_4x"))     s->meta.unlock_starting_mult_4x     = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_starting_mult_max"))    s->meta.unlock_starting_mult_max    = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "unlock_labyrinth"))            s->meta.unlock_labyrinth            = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "attach_slots"))                s->meta.attach_slots                = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "attach1"))                     s->meta.attach1                     = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "attach2"))                     s->meta.attach2                     = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "battery_max_charge"))          s->meta.battery_max_charge          = nbt_read_int32(r);
        else if (type == NBT_INT64 && !strcmp(name, "last_custom_seed"))            s->meta.last_custom_seed            = nbt_read_int64(r);
        else if (type == NBT_INT64 && !strcmp(name, "last_seen_date"))              s->meta.last_seen_date              = nbt_read_int64(r);
        else nbt_skip_payload(r, type);
    }
}

static void read_daily(NbtReader* r, save_data_t* s) {
    char name[64];
    int type;
    while ((type = nbt_read_tag(r, name, sizeof(name))) != NBT_END) {
        if (type < 0) break;
        if      (type == NBT_INT32 && !strcmp(name, "daily_done_1pt")) s->daily.daily_done_1pt = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "daily_done_2pt")) s->daily.daily_done_2pt = nbt_read_int32(r);
        else if (type == NBT_INT32 && !strcmp(name, "daily_done_3pt")) s->daily.daily_done_3pt = nbt_read_int32(r);
        else nbt_skip_payload(r, type);
    }
}

void save_read_state(NbtReader* r, save_data_t* s) {
    char name[64];
    int type;
    while ((type = nbt_read_tag(r, name, sizeof(name))) != NBT_END) {
        if (type < 0) break;
        if      (type == NBT_COMPOUND && !strcmp(name, "peek"))  read_peek (r, s);
        else if (type == NBT_COMPOUND && !strcmp(name, "stats")) read_stats(r, s);
        else if (type == NBT_COMPOUND && !strcmp(name, "meta"))  read_meta (r, s);
        else if (type == NBT_COMPOUND && !strcmp(name, "daily")) read_daily(r, s);
        else nbt_skip_payload(r, type);
    }
}
