# Barony AI-NPC Mod — Project Context

Drop this in as `CLAUDE.md` in either repo (or paste at the start of a fresh chat).

---

## What this is

An AI-NPC mod for **Barony** (open-source C++ roguelike, v5.0.2), built from source on **Nobara Linux**. Thin C++ engine hooks ↔ a local Python HTTP service that owns lore, prompts, and social state, calling a local `llama3.1:8b` via Ollama. Fully local, no cloud.

**Hardware:** i7-9700K, RTX 2070 Super (8GB), 32GB RAM.

**Release intent:** Nexus Mods. Co-op friends are non-technical and on Windows; dev env is Linux. Design converges on: thin mod + configurable backend endpoint + setup guide, host-authoritative.

## Repos

| Repo | Path | Remote |
|---|---|---|
| Python service | `~/barony-ai/` | `origin` → `tyler-scott-parker/barony-ai` |
| Barony fork | `~/Barony/` (branch `mymod`) | `origin` → TurningWheel upstream; `mine` → `tyler-scott-parker/barony-mymod` |

Fetch upstream with `git fetch origin`; push your work with `git push mine mymod`.

## Key paths

- All mod C++ now lives in **`src/mymod/mymod.cpp`** + `mymod.hpp` (extracted so upstream merges stay clean)
- Listed in `src/CMakeLists.txt` under `GAME_SOURCES` — **not** in `EDITOR_SOURCES`
- Service: `~/barony-ai/service.py` (port 5001), plus `barony_lore_full.json` (449KB, 45 sections), `race_lore.json`, `race_books.json`, `comprehension.json`, `voice_bridge.py`
- Dev binary: `~/.local/share/Steam/steamapps/common/Barony/barony-modded`
- IPC via `/tmp/mymod_*.json` (Linux-specific; needs a portability pass before release)

## Upstream hooks (verify these after any upstream merge)

| File | Hook | Notes |
|---|---|---|
| `game.cpp` | `mymod_pollAI()`, `mymod_ambientTick()` | per-frame, in the game loop |
| `actmonster.cpp` | `mymod_recordEvent("recruitment", ...)` ×2 | `forceFollower` (~11186, guard `leader.behavior == &actPlayer`) and interact-recruit (~2170). **The interact path is the one `/friendly` and normal recruitment actually use.** |
| `files.cpp` | `new_run` event in `physfsLoadMapFile` at `levelToLoad <= 1`; plus a `__attribute__((weak))` `mymod_recordEvent` stub | The weak stub exists because the **editor** target compiles `files.cpp` without the mod and would otherwise fail to link |
| `monster_lich.cpp` | Herx debuff block after `my->setHardcoreStats(*myStats)` | inside the `!MONSTER_INIT` guard, so it can't double-apply |
| `consolecommand.cpp` | `/aicommand`, `/aiserver`, `/aitest` + `#include "../mymod/mymod.hpp"` | commands legitimately belong here |

## Build & run

```bash
cd ~/Barony/build && make -j$(nproc) 2>&1 | tail -10
cp ~/Barony/build/barony ~/.local/share/Steam/steamapps/common/Barony/barony-modded
cd ~/.local/share/Steam/steamapps/common/Barony && ./barony-modded 2>&1 | grep -E 'MYMOD|AI'
```

Configure (only needed if CMakeLists changes):
```bash
cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DOPENAL_ENABLED=OFF -DFMOD_ENABLED=OFF -DSTEAMWORKS_ENABLED=OFF -DEOS_ENABLED=OFF
```

Service + voice in separate terminals:
```bash
python3 ~/barony-ai/service.py
python3 ~/barony-ai/voice_bridge.py
```

In-game test harness: `/enablecheats` → `/summonall` → `/friendly`, then interact-recruit.

## Ollama configuration

Systemd drop-in at `/etc/systemd/system/ollama.service.d/override.conf`:
```
[Service]
Environment="OLLAMA_FLASH_ATTENTION=1"
Environment="OLLAMA_KV_CACHE_TYPE=q8_0"
```

`ask_ollama` sends `options: {"num_ctx": 16384}`, `keep_alive: "30m"`, and logs `[SERVICE-DBG] prompt ~N tokens`.

Verified: 5.9 GB, **100% GPU**, CONTEXT 16384, warm replies ~2-4s. Prompts run **~650 (rat) to ~3100 (human spy with events)** tokens — nowhere near the ceiling. **Context size is not the limiting factor; the 8B's attention is.**

---

## Systems built (all verified in-game unless noted)

### Foundation
Async via detached `std::thread` + shared globals + `mymod_pollAI()` per frame (no freeze). Two-way command loop: `/aicommand` finds the player's follower via `Stat->leader_uid`, service returns `{speech, action}` ∈ FOLLOW/DEFEND/WAIT/ATTACK/NONE. **ATTACK is diegetic only** — Barony's combat AI handles fighting. Push-to-talk voice (hold V → faster-whisper small.en, cuda/float16). Speech bubbles via `createDialogueTooltip(uid, DIALOGUE_NPC, "%s", reply)` — **the `"%s"` guard is required**. Polymorph-as-comprehension. 34 canonical books injected per race. `/aiserver <url>` for BYO-model.

### Relationships
`follower_state[uid] = {friendship, events, event_log, name, race, allegiance, motive, last_boon_floor}` in service RAM, within-run. Friendship hidden from the player. Chat capped per floor; **deeds are meant to drive the climb to 100**. Obedience scales with friendship (f≤4 refuse risky with action NONE; f≤9 grumble; f≥10 obey).

### Lore retrieval
`build_lore_context(race_l, floor, budget=16)` walks: entity profile → `canon_facts` (4) → `safe_inferences` (2) → race worldview + axes → location `canon` (2) + `high_value_local_knowledge` + `local_population`. Constraints: `knowledge_boundary`, `restricted_knowledge`, location `npc_rules` (2). `floor_to_region`: 1-4 mines, 5-8 swamp, 9-13 sand_labyrinth, 14-18 ruins, 19-24 underworld, 25+ hell.

**⚠ THE MOST IMPORTANT PROMPT FINDING:** constraints worded as "do not claim otherwise" failed **5/5** — the model hedged and answered anyway ("whispers say silver..."). Naming the *rhetorical route* took it to **0/5**. The working wording:

> HARD LIMITS ON WHAT YOU KNOW. You genuinely do not know these things: / If asked about any of them, say plainly and in character that you do not know, and STOP. / Do NOT guess, speculate, theorize, or pass on rumors about them. Hedged answers are FORBIDDEN: / 'some say...', 'whispers speak of...', 'perhaps it is...', 'I have heard...' followed by an answer / counts as claiming and is wrong. An honest 'I do not know' is always the correct reply.

**Generalize this:** at 8B, forbidding a *conclusion* without forbidding the *evasion pattern* gets routed around. Name the specific pattern. Verified to generalize — 9-10/10 across 5 races, 3 regions, and topics not in the limits block at all.

### Event memory
Structured records: `{type, floor, claim, importance, provenance}`. `IMPORTANCE_WEIGHT = {routine:0, notable:3, major:8, world_changing:20}` seeds friendship. Handler has an early fire-and-forget branch: any POST with an `"event"` field records and returns `{"ok":true}` with no generation.

- **`recruitment`** — once per follower, notable (+3)
- **`fought_alongside`** — repeatable, notable (+3). Edge-detection at the **top of `mymod_ambientTick`, before the inflight guard** (critical — behind the guard it never fires, since you're usually mid-conversation during fights). `inCombat = (monsterState == MONSTER_STATE_ATTACK(1) || == MONSTER_STATE_HUNT(3))`; fires on combat→calm while alive. Cooldown `45*50` ticks.
- **`new_run`** — clears `follower_state` and `HERX_STATE`

### Naming
Friendship ≥5 unlocks a nudge; follower names itself. `extract_name(raw, speech)` prefers a JSON `name` field, falls back to parsing speech — **the fallback is essential**, the 8B says the name but omits the field. Patterns use `(?i:...)` scoped flags and normalize typographic apostrophes (`\u2019`) — both were real bugs that silently disabled naming *and*, transitively, the Herx secret (which gates on the follower being named).

**The HUD rename is free:** `GameUI.cpp:2401` already reads `followerStats->name` and displays it when non-empty and not `"nothing"`. So `strncpy(Stat->name, ...)` renames the party UI with no rendering change. (`getMonsterLocalizedName` in `actmonster.cpp:224` does *not* use `Stat->name` for ordinary races — dead end.)

### Herx secret weakness
Gate: eligible race (skeleton/human) **and** named **and** friendship ≥50 **and** ≥4 `fought_alongside` events, then an escalating roll (+30% if the player asks about Herx directly, +25% if the follower is a spy). One reveal per run.

Four paired truth/debuff variants — the flavor always matches the mechanic. Reply carries `"secret": "<debuff>:<uid>"`, parsed from `::SECRET::`.

`initLich` applies it: **tier 1** = knowledge alone; **tier 2** = double, if the informant is alive at spawn (`uidToEntity(informant)`, `HP > 0`). Evaluated once at spawn, not continuously.

Barony's own naming calls Herx the **midpoint** (`MOVIE_MIDGAME_HERX`, `HerxMidpoint*`), which is why friendship 50 fits.

### Allegiance & spies
Weighted roll at recruitment: **loyal 70 / self_interested 15 / fearful 8 / spy 7**. Never shown to the player. Spies get a motive and behavioral tells: deflect on personal history, over-interest in the player's gear and plans, hesitation at Herx's name. `allegiance_section(st, says)` detects probing questions and sharpens the tell.

**Verified:** all 3 spies deflected on family; both loyal controls gave rich specific histories. One spy dodged a family question and immediately asked about the player's rations and map — a genuine, unscripted tell.

**Spies reveal FALSE weaknesses** from `HERX_FALSE_VARIANTS` (running water / silver / true name — all plausible undead lore, all wrong). Reported as `debuff 0`, so `initLich` applies nothing. Discovered through consequences, never announced — per the design doc's core rule.

### Boons
`boon_roll(st, floor)`: friendship ≥10, one per follower per floor, `chance = min(0.35, (f-10)/200)`. Measured: none at f9, ~2% at f15, ~9% at f30, ~19% at f45, ~25% at f60.

Types: **info** (a fact from the lore context, volunteered), **mundane item** (bread/cheese/glass gem/torch), **one good item per run** (healing potion/garnet, f≥40), **trap disarm** (gnome/automaton/kobold/goblin only, f≥30, rarest). Reply carries `"boon"`, parsed from `::BOON::`.

`mymod_disarmFloorTraps()` sets `actTrapSabotaged = 1` — **Barony's own sabotage flag**, checked by every trap type (arrow, boulder ×5, magic, spear). Items spawn via `newItem(...)` + `dropItemMonster(it, giver, ...)` at the follower's feet.

### Robustness
`parse_reply(raw)` — strict `json.loads` first, then a regex fallback for malformed 8B output. Tested against 9 patterns. Logs `(JSON malformed - recovered via fallback)`; observed firing in real play.

Display: status lines (thinking/listening/transcribing/executed) are `printlog` (terminal only). Ambient/taunt bubbles use `mymod_ambient_speaker_uid`. **Double-print fixed** — every line used to print twice (a `MESSAGE_MISC` *and* the `MESSAGE_CHAT` broadcast); now one chat line + one bubble, with `mymod_chat_prefix` carrying the `[taunt]`/`[overheard]` label.

---

## Open / next

**The playtest is the highest-value next step.** Every number below is a guess that has never met a real run:
- Is friendship 50 reachable by Herx? (~2 hours of play to reach him)
- Do boons read as companionship or noise?
- Are spy tells catchable *live*, not side-by-side?
- Do items at a follower's feet get noticed?
- Do the debuff numbers matter against a 1250 HP boss? (`initLich` logs the applied stats)

**Known gaps:**
- The **false secret has never been observed firing** — all test rolls landed non-spy. It's the one branch where a bug would be invisible.
- The **friendship-30 spy "crack"** doesn't land — reads as ordinary hedging. Needs a concrete behavioral instruction (an oblique warning, an almost-confession that stops) rather than an atmospheric one. Concrete has consistently beaten impressionistic here.
- **Chat history stores only the player's side**, so followers contradict themselves across turns (observed: a rat claimed to love cheese, then to not eat cheese). Storing both sides would fix it.
- Grounding is only in the main `build_prompt` and `build_taunt_prompt`; ambient babble has none.
- Prompt sizes vary wildly by race (652 rat → 3100 human) because book-lore injection is uneven. Thin races are where fabrication would reappear first.

**Designed, not built — the peaceful Herx route.** `actWinningPortal` already contains the answer: the portal **exists on the boss floor, invisible**, and reveals itself when its per-tick scan finds no `LICH` or `DEVIL` alive. So a spared-Herx ending needs only (a) a flag excluding a pacified Herx from that scan and (b) clearing his hostility. No new entity, no replaced code path. The chain that *earns* it — merchants, testimony, leverage — is the real project and is undesigned. Design the failure mode first: a half-fired peaceful path could strand the player with a non-hostile boss and no exit.

**DLC:** *Deserters & Disciples* shipped Jan 29 2026 with *Instruments of Destruction Part 1* (which removed Magic/Casting/Swimming and revamped magic into Sorcery/Thaumaturgy/Mysticism). **Part 2 is the upcoming one.** New races will need entries in `race_lore.json`, `comprehension.json`, and the lore file's denizen profiles or they fall through to generic defaults. Untested: whether a `STEAMWORKS_ENABLED=OFF` build can access DLC content at all.

---

## Working style

Prove each piece in isolation before wiring it — curl-test the service before touching the game. Build service-side first (testable in seconds), then the C++ hook. Read the game's own source rather than guessing at APIs; several features turned out to be near-free because Barony already had the mechanism (`actTrapSabotaged`, `Stat->name` in the HUD, the invisible winning portal).

Commit after each working session, not in big batches.

No unsolicited break suggestions or timeline estimates.

### Editing discipline (from painful experience)

These cost many rounds before Claude Code:
- **Verify edits by the most specific unique token** — a function *call* with its name, never a bare word. `fought_alongside` also appeared in an unrelated comment, so every "present: True" check passed while the actual code was never inserted (~8 rounds lost).
- **Never bound a replacement by "up to the next `def`/function"** — that silently deleted `build_prompt`'s grounding block, leaving conversation with no canonical grounding *and no hard limits* for several tests. The tell was the prompt token count not moving.
- When a change doesn't move the token count, **`grep -n "^def "` to map the file** is the first diagnostic, not the fourth.
- Assert anchor uniqueness before writing (there are four copies of `Entity* pl = players[clientnum]->entity;` in the old `consolecommand.cpp`).
- Watch tab-vs-space indentation; reconstructed multi-line anchors usually fail.
