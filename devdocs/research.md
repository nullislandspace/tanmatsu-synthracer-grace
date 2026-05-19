# Appendix A — Race The Sun Research

> Offline-cached research on the original game (Flippfly, 2013). Part of the [dev docs](README.md).

## Appendix A — Race The Sun Research Reference

This is the offline-cached research summary from web sources, included so
future sessions don't have to re-fetch. Sources cited at end.

### Core gameplay loop

- Player pilots a solar-powered craft along an endless, procedurally
  generated landscape under a setting sun.
- **Steer left/right only.** Acceleration is automatic; speed is constant
  in direct sunlight.
- **Speed decreases in shadow** (large obstacles, clouds). Stay shadowed
  too long → stall.
- The **sun continuously sets**. When it drops below the horizon, the
  craft loses power and the run ends.
- **Speed-boost pickups** push the sun back up briefly.
- Optional advanced inputs: **jump** (consumable pickup), **barrel roll**
  (rapid alternating left/right). Original used jump to clear obstacles.
- **Direct collision = instant death** (unless a Shield is consumed).

### Pickups

| Pickup | Effect |
|---|---|
| Blue Tri (pyramid) | Multiplier +1 after every 5 collected. Crash drops it. |
| Speed boost | Temporary speed up + raises sun. |
| Jump | Single-use; ship hops up and floats down. Stackable up to 3 with upgrades. |
| Shield / Emergency Portal | Single-use; absorbs one fatal hit and teleports forward/up. |
| Checkpoint | (Late-game) checkpoint storage upgrade enables multiple. |

### Scoring

- Continuous accumulation while moving forward.
- Multiplier driven by Tris (5 → +1×). Higher meta levels raise the
  multiplier floor (lv6→2, lv12→3, lv23→4, lv24→max).
- Crashing drops multiplier by ~5.

### World structure

- Pseudo-procedural regions (~7), each with distinct themes/obstacle sets
  and difficulty mutators.
- World layout is the **same for all players for the day** — daily seed,
  shared leaderboards. Resets every 24 hours.
- Modes: **Standard** (default), **Apocalypse** (faster, less forgiving,
  unlocks at lv11), **Labyrinth** (top-down maze, unlocks at lv25).

### Metaprogression

Two intertwined progression layers:

1. **Per-run** — sequential regions, increasing speed/density. Multiplier
   builds over the run.
2. **Persistent player level (1 → 25)** — each level grants a permanent
   unlock; this is the meta layer.

#### Challenge system

- Always **3 active challenges**: 1-point, 2-point, 3-point.
- Complete one → immediately replaced by a new one.
- Procedural variations on a small template set:
  - Reach region N
  - Collect N Tris (sometimes "in one region", sometimes "air tris")
  - Use pickup type X N times
  - Travel total distance N
  - Reach N× multiplier
  - Perfect-region (no crash) — N regions in one run
  - Movement-restricted ("only turn left", "only turn right")
- Points required per level scales: ~3 at low levels, up to ~8 near top.

#### Per-level unlocks (canonical 1–25 ladder)

| Lvl | Unlock |
|----|----|
| 1  | (start) |
| 2  | Speed-boost pickup |
| 3  | Multiplier system (Tris → +1×) |
| 4  | Jump pickup |
| 5  | Magnet attachment |
| 6  | Starting multiplier 2× |
| 7  | Portal to easier alternate world |
| 8  | Double jump storage |
| 9  | Shield pickup |
| 10 | Shield attachment |
| 11 | **Apocalypse mode** (harder/faster) |
| 12 | Starting multiplier 3× |
| 13 | Second attachment slot |
| 14 | Left-wing decal |
| 15 | Double portal storage |
| 16 | Checkpoint pickups |
| 17 | Power-turning attachment |
| 18 | Second power-turning attachment |
| 19 | Triple jump storage |
| 20 | Enhanced checkpoint storage |
| 21 | Enhanced magnet |
| 22 | Right-wing decal |
| 23 | Starting multiplier 4× |
| 24 | Final multiplier upgrade |
| 25 | New mode (Labyrinth) + battery upgrade ("complete") |

### Sources (research date: 2026-05-07)

- https://en.wikipedia.org/wiki/Race_the_Sun_(video_game)
- https://store.steampowered.com/app/253030/Race_The_Sun/
- https://steamcommunity.com/sharedfiles/filedetails/?id=203298348 (basic guide)
- https://steamcommunity.com/sharedfiles/filedetails/?id=207229205 (level unlocks)
- https://www.gamespot.com/reviews/race-the-sun-review/1900-6413902/
- https://portforward.com/games/walkthroughs/Race-The-Sun/The-Rest.htm
- https://psnprofiles.com/guide/5414-race-the-sun-trophy-guide
- http://flippfly.com/racethesun/releasenotes/

---

