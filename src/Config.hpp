#pragma once

#include <algorithm>
#include <string>

namespace pb {

constexpr int MAX_TICKS = 24;              // largest value the settings allow
constexpr double TICK_MS = 1000.0 / 240.0; // one physics tick at 240 TPS

// Gamemodes, in the order the window and the per-mode limits use.
enum Mode { CUBE, SHIP, BALL, UFO, WAVE, ROBOT, SPIDER, SWING, MODES };

struct Config {
    bool enabled = true;
    int maxTicks = 2;      // largest delay the buffer may add (the toggle key cycles this)
    int randomize = 3;     // pick a random safe delay up to this many ticks above the smallest safe one
    int modeMax[MODES] { 24, 8, 24, 6, 8, 24, 24, 6 }; // per-mode caps on top of maxTicks
    bool releases = true;  // also buffer early releases (wave, robot, ship)
    bool ship = false;     // work in ship mode too
    bool popup = true;     // in-game timing popup
    bool practiceOnly = false;
    bool deathCheck = true; // say so when you die right after a held-back input
    // how inputs are judged and fired (each can be switched off)
    bool finerTiming = true;   // delays in quarter ticks, fired inside a physics step
    bool bestFix = true;       // compare the safe delays and take the one that lives longest
    bool confidence = true;    // only help when slightly different runs agree
    bool movingObjects = true; // ghost sees objects that are moving
    bool holdFix = true;       // wave / ship: let go for you when holding on would kill you
    bool throughPads = true;   // ghost uses pads, speed and gravity portals instead of stopping
    bool throughOrbs = true;   // ghost uses yellow / pink / red / blue / green / black orbs
    bool tapModel = true;      // ghost lets go after your usual tap instead of holding
    bool freshGhost = true;
    bool stopAtPortals = true;
    bool smartSafety = true;
    bool steadyTiming = true;

    int limit(int mode) const {
        if (mode < 0 || mode >= MODES) return maxTicks;
        return std::min(maxTicks, modeMax[mode]);
    }
};

// Current settings (main thread).
Config const& config();

// Re-reads every setting.
void loadConfig();

// Toggle key: cycles 2 -> 4 -> 8 -> 16 ticks -> off and saves it to the
// settings. Returns a short line describing the new stage.
std::string cycleStage();

} // namespace pb
