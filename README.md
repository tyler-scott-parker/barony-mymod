# Adorcism — the Barony engine mod

A fork of **[TurningWheel/Barony](https://github.com/TurningWheel/Barony)** v5.0.2 carrying the
engine half of [Adorcism](https://github.com/tyler-scott-parker/barony-ai), which makes every
creature in the dungeon talk, remember, and have opinions about you.

Barony's own README is preserved as **[README.upstream.md](README.upstream.md)**. Everything
below is about the fork.

## Where the mod lives

Almost all of it is in **`src/mymod/`**, extracted so upstream merges stay clean:

| File | |
|---|---|
| `mymod.cpp` / `mymod.hpp` | the mod itself |
| `mymod_net.hpp` | transport to the service (SDL_net) and the reply reader |
| `mymod_voice.hpp` | push-to-talk resampling and WAV framing |
| `httptest.cpp`, `wavtest.cpp`, `packtest.cpp` | standalone tests, deliberately not in the build |

The rest of the diff is **hooks into upstream files**, kept as small as possible. The full list —
and *why each one is where it is* — is in `CLAUDE.md` in the
[service repo](https://github.com/tyler-scott-parker/barony-ai), which is the real design document
for this project. Read that before changing anything here.

| Upstream file | Why it is touched |
|---|---|
| `game.cpp` | per-frame poll and ambient tick |
| `actmonster.cpp` | recruitment, friendly fire, NPC engagement |
| `net.cpp` | seven mod packets, plus the chat bridge for unmodded clients |
| `items.cpp` | haggled shop prices, inside `buyValue`/`sellValue` |
| `shops.cpp` | merchant greeting on shop open |
| `player.cpp` | the client half of `/friendly` |
| `files.cpp` | new-run detection, plus a weak stub so the editor still links |
| `monster_lich.cpp` | applies the Baron's secret weakness |
| `consolecommand.cpp` | `/aicommand`, `/aiserver`, `/aiidentify`, `/aistatus`, `/ailog` |

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DOPENAL_ENABLED=OFF -DFMOD_ENABLED=OFF \
         -DSTEAMWORKS_ENABLED=OFF -DEOS_ENABLED=OFF
make -j$(nproc)
```

The binary runs from **anywhere** — what matters is the working directory, not where the
executable sits. Launch it with the Barony install as the working directory and it reads the
game's data in place, writing nothing into it. That is what `dist/setup.sh` in the service repo
automates, and it means Steam can verify or update Barony without ever disturbing the mod.

For a build that unlocks DLC against real Steam entitlement, see the Steamworks section of
`CLAUDE.md` — the short version is that `-DSTEAMWORKS_ENABLED=ON` alone does **not** define the
`STEAMWORKS` macro, and the SDK must be 1.53a.

## Status

**No Windows build yet.** The code is portable — no shell-outs, no hardcoded paths, SDL and
SDL_net for everything platform-facing — but nobody has compiled it with MSVC. Multiplayer is
implemented and verified by reading and by driving both sides locally, but has never met a second
machine.

## Licence

Barony is BSD 2-Clause, © 2013-2020 Turning Wheel LLC — see [LICENSE.txt](LICENSE.txt), which
applies to this fork and to any binary built from it.
