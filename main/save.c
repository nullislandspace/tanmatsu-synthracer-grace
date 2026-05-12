#include "save.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "fastopen.h"
#include "game.h"
#include "nbt.h"

static char const* TAG = "save";

// Defined in save_nbt.c — serializer for save_data_t.
void save_write_state(NbtWriter* w, save_data_t const* s);
void save_read_state(NbtReader* r, save_data_t* s);

static void slot_path(int slot, char* buf, size_t size) {
    snprintf(buf, size, SAVE_PATH_PREFIX "/save%d.bin", slot);
}

void save_init_defaults(save_data_t* s) {
    memset(s, 0, sizeof(*s));
    s->meta.level = 1;
}

void save_init(void) {
    struct stat st;
    if (stat(SAVE_PATH_PREFIX, &st) != 0) {
        if (mkdir(SAVE_PATH_PREFIX, 0755) != 0) {
            ESP_LOGE(TAG, "mkdir %s failed", SAVE_PATH_PREFIX);
        } else {
            ESP_LOGI(TAG, "created %s", SAVE_PATH_PREFIX);
        }
    }
}

int save_slot_exists(int slot) {
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return 0;
    char path[64];
    slot_path(slot, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0;
}

int save_slot_peek(int slot, save_peek_info_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return -1;

    char path[64];
    slot_path(slot, path, sizeof(path));

    FILE* f = fastopen(path, "rb");
    if (!f) return -1;

    NbtReader r;
    if (nbt_read_open(&r, f) < 0) {
        fastclose(f);
        return -1;
    }

    // Root compound
    char name[64];
    int type = nbt_read_tag(&r, name, sizeof(name));
    if (type != NBT_COMPOUND) {
        fastclose(f);
        return -1;
    }

    // First child must be the peek compound (we write it first).
    type = nbt_read_tag(&r, name, sizeof(name));
    if (type == NBT_COMPOUND && strcmp(name, "peek") == 0) {
        while ((type = nbt_read_tag(&r, name, sizeof(name))) != NBT_END) {
            if (type < 0) break;
            if (type == NBT_INT64 && strcmp(name, "last_played_unix") == 0) {
                out->last_played_unix = nbt_read_int64(&r);
            } else if (type == NBT_INT64 && strcmp(name, "score_best") == 0) {
                out->score_best = nbt_read_int64(&r);
            } else if (type == NBT_INT32 && strcmp(name, "stage_best") == 0) {
                out->stage_best = nbt_read_int32(&r);
            } else if (type == NBT_INT32 && strcmp(name, "runs_total") == 0) {
                out->runs_total = nbt_read_int32(&r);
            } else {
                nbt_skip_payload(&r, type);
            }
        }
    }

    fastclose(f);
    return r.error ? -1 : 0;
}

int save_load_slot(int slot, save_data_t* s) {
    save_init_defaults(s);
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return -1;

    char path[64];
    slot_path(slot, path, sizeof(path));

    FILE* f = fastopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "no save file at %s", path);
        return -1;
    }

    NbtReader r;
    if (nbt_read_open(&r, f) < 0) {
        ESP_LOGE(TAG, "bad header in %s", path);
        fastclose(f);
        return -1;
    }

    // Root compound
    char name[64];
    int type = nbt_read_tag(&r, name, sizeof(name));
    if (type != NBT_COMPOUND) {
        fastclose(f);
        return -1;
    }

    save_read_state(&r, s);
    fastclose(f);

    if (r.error) {
        ESP_LOGE(TAG, "read error in %s", path);
        return -1;
    }

    ESP_LOGI(TAG, "loaded slot %d (v%d, swap=%d)", slot, r.version, r.swap);
    return 0;
}

int save_write_slot(int slot, save_data_t* s) {
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return -1;

    // Refresh peek block + timestamp.
    s->last_played_unix = (int64_t)time(NULL);
    s->peek_score_best  = s->stats.all_time.score;
    s->peek_stage_best  = s->stats.all_time.stage_reached;
    s->peek_runs_total  = s->stats.all_time.runs_total;

    char path[64];
    slot_path(slot, path, sizeof(path));

    FILE* f = fastopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s for write failed", path);
        return -1;
    }

    NbtWriter w;
    nbt_write_open(&w, f);
    nbt_write_compound(&w, "root");
    save_write_state(&w, s);
    nbt_write_end(&w);
    fastclose(f);

    if (w.error) {
        ESP_LOGE(TAG, "write error to %s", path);
        return -1;
    }

    ESP_LOGI(TAG, "saved slot %d", slot);
    return 0;
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
