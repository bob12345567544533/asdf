# Predictive Input Buffer

Geode mod for Geometry Dash 2.2081 (Geode 5.11.2). Holds clicks that are a few ms too early and would kill you, then fires them on the step where they survive. Practice tool.

## Building

### Easiest: GitHub Actions
1. Make a GitHub repo and push this folder to it.
2. The **Build Geode Mod** workflow runs on every push (or start it from the Actions tab).
3. Download the `Build Output` artifact. It contains one `.geode` file with Windows, macOS, iOS and Android builds.
4. Drop the `.geode` into `Geometry Dash/geode/mods`.

### Locally on Windows
Needs Visual Studio Build Tools (C++ workload), CMake, and the [Geode CLI](https://docs.geode-sdk.org/getting-started/geode-cli).

```
geode sdk install
geode sdk install-binaries
geode build
```

The built `.geode` ends up in `build/`.

## How it works

Every jump input goes through `GJBaseGameLayer::handleButton`. When one comes in:

1. Two hidden ghost `PlayerObject`s are copied from the real player(s) with `spawnFromPlayer`.
2. The ghost runs the game's own physics (`checkCollisions`, `update`, `updateRotation`) for the look-ahead window with the input applied right away.
3. If the ghost survives, the input goes through untouched.
4. If it dies, the mod retries with the input delayed by 1, 2, 3… steps, up to the limit for your gamemode. The first delay that survives the whole window (or lives at least *Min Survival Gain* longer) wins.
5. That input is queued and fired from `PlayerObject::update` on the chosen step. Any inputs that come in while it's queued are shifted by the same amount, so your rhythm stays the same.

While a simulation runs, hooks block everything that would touch the real level: deaths, triggers, orbs, pads, portals, shine effects, wave trail points, jump counters and the end animation. The ghosts only collide with solids, hazards and slopes (the same filter GDMegaOverlay's Show Trajectory uses).

## Files
- `src/Buffer.cpp` – simulation, the delay search and the input queue.
- `src/Hooks.cpp` – all the hooks, plus the toggle keybind.
- `src/Overlay.cpp` – the on-screen text.
- `src/Menu.cpp` – the quick settings window (Win32 window with capture exclusion on Windows, Geode settings popup elsewhere).

## Known limits
- Orbs, pads and portals aren't simulated. Near them the prediction can be wrong, so the mod is less useful there. It skips buffering entirely while you're touching an orb.
- Moving objects are treated as frozen during the look-ahead.
- The simulation assumes you make no other inputs in the window. That's why the look-ahead is short.
