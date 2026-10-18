# v2.1.0
- **New:** Quick Settings key (F7 by default). Opens a small settings window without going through Geode. On Windows it's a separate window hidden from OBS / Discord capture. On other platforms it opens the normal settings page.
- Preset buttons in that window set every mode (except ship) to the old 2 / 4 / 8 / 16 tick stages.

# v2.0.0
Full rewrite.

- **Fixed:** the ghost player now gets your full state (including vertical speed) before each check. The old version only copied part of it, so its predictions could be wrong.
- **Fixed:** a tap (press + release) that gets delayed now keeps its original hold length. Before, the release could fire right after the press.
- **Fixed:** 2-player mode: each player is checked on their own instead of both being moved by player 1's input.
- **New:** settings menu instead of the F6 stage cycle. Per-gamemode max delay in ms.
- **New:** toggle keybind (F6 by default, rebindable).
- **New:** on-screen "how early was that" feedback and an optional counter.
- **New:** ship and swing support (ship is off by default).
- **New:** look-ahead and survival gain are adjustable.
- **Removed:** the separate console window. Use Debug Logging + the Geode console instead.
- Skips buffering while you're dashing or touching an orb, where the prediction can't be trusted.
- Builds for Windows, macOS, iOS and Android.

# v1.0.0
- First version (AI-generated test build).
