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
