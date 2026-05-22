#pragma once

// Ship attachments (Phase 9.4). An attachment occupies one of the ship's
// equip slots (meta.attach1 / attach2 in the save file). The IDs are
// persisted to disk, so their numeric values are STABLE — only ever
// append new attachments; never renumber or remove an existing one.
//
// "Equipped" state is read from the save at start_run and snapshotted
// into game_state (g->has_magnet, …); the equip UI in main.c writes the
// slots. The picker offers every catalogued attachment regardless of the
// unlock_* flags for now ("allow all attachments"); unlock gating is
// wired in Phase 11.
typedef enum {
    ATTACH_NONE = 0,   // empty slot
    ATTACH_MAGNET,     // pulls nearby pickups toward the ship's path
    ATTACH_ID_COUNT,   // sentinel — also the picker's row count
} attachment_id_t;

// Display name for the equip UI. ATTACH_NONE renders as "[empty]".
// Any out-of-range id also reads as "[empty]" so a future save written
// by a newer build degrades gracefully on an older one.
char const* attachment_name(attachment_id_t id);
