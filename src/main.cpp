// Straight port of the original Predictive Input Buffer (ai.predictivebuffer
// v1.0.0, console "v4.7"), rebuilt from its compiled code. Same hooks, same
// order and priorities, same delay search, same F6 stages, same console output.

#include "Menu.hpp"
#include "Ui.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <random>
#include <vector>
#include <cstdio>
#include <cstring>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace {

// --- state (same defaults as the original) ---
PlayerObject* g_ghost1 = nullptr;
PlayerObject* g_ghost2 = nullptr;
bool g_sim = false;        // a ghost run is in progress
bool g_firing = false;     // we're feeding an input to GD ourselves
bool g_simDied = false;    // a ghost died during the run
bool g_held = false;       // the button as you're holding it
bool g_lastDown = false;   // the button as GD last got it
bool g_isP1 = true;
int g_stage = 0;           // F6 stage, 0..4 (4 = off)
bool g_enabled = true;
int g_limitAll = 2;
int g_limitWave = 2;
// Window settings (same defaults as the original + Randomize 3).
int g_minTicks = 1;
int g_randomize = 3;
bool g_releases = true;
bool g_ship = false;
int g_lastAttempt = -1;
int g_pending = -1;        // >= 0: steps until the held-back input fires
bool g_pendingDown = true;
bool g_flipAfter = false;  // the opposite input came in while one was held back
float g_dt = 0.25f;
int g_counter = 999;       // player steps since the last input (capped)
int g_burst = 0;           // quick inputs in a row

void readSettings() {
    auto mod = Mod::get();
    g_enabled = mod->getSettingValue<bool>("enabled");
    g_limitAll = static_cast<int>(mod->getSettingValue<int64_t>("buffer-ticks"));
    g_limitWave = std::min(g_limitAll, 8); // like the original's 16 / 8 stage
    g_minTicks = static_cast<int>(mod->getSettingValue<int64_t>("buffer-min-ticks"));
    g_randomize = static_cast<int>(mod->getSettingValue<int64_t>("randomize"));
    g_releases = mod->getSettingValue<bool>("buffer-releases");
    g_ship = mod->getSettingValue<bool>("buffer-ship");
}

int modeIndex(PlayerObject* p) {
    if (p->m_isShip) return 1;
    if (p->m_isBall) return 2;
    if (p->m_isBird) return 3;
    if (p->m_isDart) return 4;
    if (p->m_isRobot) return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing) return 7;
    return 0;
}

int levelPercent(PlayLayer* pl) {
    if (!pl || !pl->m_player1 || pl->m_levelLength <= 0.f) return -1;
    return static_cast<int>(std::clamp(pl->m_player1->getPositionX() / pl->m_levelLength * 100.f, 0.f, 100.f));
}

std::mt19937& rng() {
    static std::mt19937 gen { std::random_device {}() };
    return gen;
}

// Same raw fields the original copies onto the ghost (GD 2.2081 Windows).
template <class T>
void copyRaw(PlayerObject* to, PlayerObject* from, size_t offset) {
    std::memcpy(reinterpret_cast<char*>(to) + offset, reinterpret_cast<char const*>(from) + offset, sizeof(T));
}

void setupGhost(PlayerObject* ghost, PlayerObject* real) {
    ghost->setVisible(false);
    ghost->copyAttributes(real);
    ghost->setScale(real->getScale());
    ghost->setContentSize(real->getContentSize());
    copyRaw<float>(ghost, real, 0x2fc);
    copyRaw<float>(ghost, real, 0x300);
    copyRaw<float>(ghost, real, 0xb84);
    copyRaw<bool>(ghost, real, 0x9c1);
    copyRaw<bool>(ghost, real, 0x9bf);
    copyRaw<float>(ghost, real, 0x9f0);
    copyRaw<float>(ghost, real, 0x9f4);
}

void setButton(PlayerObject* ghost, bool down) {
    if (down) ghost->pushButton(PlayerButton::Jump);
    else ghost->releaseButton(PlayerButton::Jump);
}

void clearLogs(PlayerObject* ghost) {
    ghost->m_collisionLogTop->removeAllObjects();
    ghost->m_collisionLogBottom->removeAllObjects();
    ghost->m_collisionLogLeft->removeAllObjects();
    ghost->m_collisionLogRight->removeAllObjects();
}

// Runs the ghosts `delay` steps with the button as it is now, then applies the
// input and keeps going for `horizon` steps. Returns how many steps after the
// input it survived (`horizon` if it never died). `diedBeforeInput` is set if
// it died before the input even went in.
int simulate(PlayLayer* pl, PlayerObject* real1, PlayerObject* real2, bool dual,
             int delay, bool down, int horizon, bool* diedBeforeInput) {
    *diedBeforeInput = false;
    if (!g_ghost1) return horizon;

    g_sim = true;
    g_simDied = false;

    setupGhost(g_ghost1, real1);
    if (dual && g_ghost2 && real2) setupGhost(g_ghost2, real2);

    if (delay > 0) {
        setButton(g_ghost1, !down);
        if (dual && g_ghost2) setButton(g_ghost2, !down);
    }

    // Flying modes never count a landing as "safe".
    bool flying;
    if (real1->m_isDart || real1->m_isBird || real1->m_isSwing) flying = true;
    else if (!real2 || !dual) flying = false;
    else if (real2->m_isDart || real2->m_isBird) flying = true;
    else flying = real2->m_isSwing;

    int stepsAfter = 0;
    int total = horizon + delay;
    if (total <= 0) {
        g_sim = false;
        return 0;
    }

    float dt = (g_dt > 0.001f && g_dt < 2.f) ? g_dt : 0.25f;
    int grounded = 0;
    int airborne = 0;

    for (int step = 0; step != total; ++step) {
        if (step == delay) {
            setButton(g_ghost1, down);
            if (dual && g_ghost2) setButton(g_ghost2, down);
        }

        g_ghost1->update(dt);
        g_ghost1->updateRotation(dt);
        if (g_ghost2 && dual) {
            g_ghost2->update(dt);
            g_ghost2->updateRotation(dt);
        }
        if (g_simDied) {
            if (step < delay) *diedBeforeInput = true;
            g_sim = false;
            return stepsAfter;
        }

        clearLogs(g_ghost1);
        pl->checkCollisions(g_ghost1, dt, false);
        if (g_simDied) {
            if (step < delay) *diedBeforeInput = true;
            g_sim = false;
            return stepsAfter;
        }
        if (dual && g_ghost2 && real2) {
            clearLogs(g_ghost2);
            pl->checkCollisions(g_ghost2, dt, false);
            if (g_simDied) {
                if (step < delay) *diedBeforeInput = true;
                g_sim = false;
                return stepsAfter;
            }
        }

        if (step < delay) continue;
        stepsAfter++;
        if (flying) continue;

        bool bothGrounded = g_ghost1->m_isOnGround;
        if (dual && g_ghost2) bothGrounded = bothGrounded && g_ghost2->m_isOnGround;
        if (!bothGrounded) {
            airborne++;
            grounded = 0;
            continue;
        }
        if (airborne < 6) continue;
        if (grounded > 2) {
            g_sim = false;
            return horizon; // jumped and landed safely
        }
        grounded++;
    }

    g_sim = false;
    return stepsAfter;
}

void feed(PlayLayer* pl, bool down) {
    g_firing = true;
    g_lastDown = down;
    pl->handleButton(down, static_cast<int>(PlayerButton::Jump), g_isP1);
    g_firing = false;
}

void resetState() {
    g_sim = false;
    g_firing = false;
    g_pending = -1;
    g_flipAfter = false;
    g_burst = 0;
}

void releaseGhosts() {
    if (g_ghost1) {
        g_ghost1->release();
        g_ghost1 = nullptr;
    }
    if (g_ghost2) {
        g_ghost2->release();
        g_ghost2 = nullptr;
    }
}

PlayerObject* makeGhost(PlayLayer* pl) {
    auto ghost = PlayerObject::create(1, 1, pl, pl, true);
    ghost->retain();
    ghost->setPosition({ 0.f, 105.f });
    ghost->setVisible(false);
    pl->m_objectLayer->addChild(ghost);
    return ghost;
}

} // namespace

// ---------------------------------------------------------------------------

class $modify(PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", -1000000);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", -1000000);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bool ok = PlayLayer::init(level, useReplay, dontCreateObjects);
        if (ok && GameManager::sharedState()->m_playLayer) {
            auto pl = GameManager::sharedState()->m_playLayer;
            g_ghost1 = pl ? makeGhost(pl) : nullptr;
            pl = GameManager::sharedState()->m_playLayer;
            g_ghost2 = pl ? makeGhost(pl) : nullptr;
            g_pending = -1;
            g_flipAfter = false;
            g_held = false;
            g_lastDown = false;
            g_isP1 = true;
            g_sim = false;
            g_firing = false;
            g_counter = 999;
            g_burst = 0;
        }
        if (ok) {
            ui::attach(this);
            g_lastAttempt = m_attempts;
            pb::menu::attempt(m_attempts, true);
        }
        return ok;
    }

    void onQuit() {
        PlayLayer::onQuit();
        releaseGhosts();
        resetState();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_sim && (player == g_ghost1 || player == g_ghost2)) {
            g_simDied = true;
            return;
        }
        g_pending = -1;
        g_flipAfter = false;
        g_lastDown = false;
        g_burst = 0;
        PlayLayer::destroyPlayer(player, object);
        if (player == m_player1 && player->m_isDead) pb::menu::death(levelPercent(this));
    }

    void playEndAnimationToPos(CCPoint pos) {
        if (g_sim) return;
        PlayLayer::playEndAnimationToPos(pos);
    }
};

class $modify(LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        releaseGhosts();
        resetState();
        return ok;
    }
};

class $modify(GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", -1000000);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_sim || button != static_cast<int>(PlayerButton::Jump) || g_firing) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        auto pl = GameManager::sharedState()->m_playLayer;
        if (!pl) return GJBaseGameLayer::handleButton(down, button, isPlayer1);

        bool dual = pl->m_gameState.m_isDualMode && pl->m_player2;
        if (pl->m_level && !isPlayer1 && pl->m_level->m_twoPlayerMode) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        g_isP1 = isPlayer1;
        g_held = down;

        auto passThrough = [&] {
            g_lastDown = down;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
        };

        if (!g_enabled) return passThrough();

        auto p1 = m_player1;
        PlayerObject* p2 = dual ? pl->m_player2 : nullptr;
        bool hasP2 = p2 && dual;
        bool shipOff = !g_ship && p1 && (p1->m_isShip || (hasP2 && p2->m_isShip));
        if (!p1 || p1->m_isDead || shipOff) {
            g_pending = -1;
            g_flipAfter = false;
            return passThrough();
        }

        // An input is already held back: swallow this one, and remember if it
        // flips the button so the opposite fires right after.
        if (g_pending >= 0) {
            g_flipAfter = down != g_pendingDown;
            g_counter = 0;
            return;
        }
        if (g_pending == -2) {
            g_pending = -1;
            g_flipAfter = false;
            bool flip = !g_pendingDown;
            if (flip != g_lastDown) feed(pl, flip);
        }

        bool anyWave = p1->m_isDart || (hasP2 && p2->m_isDart);
        bool miniWave;
        if (p1->m_isDart) {
            miniWave = p1->m_vehicleSize < 1.f;
            if (!miniWave && hasP2) miniWave = p2->m_isDart && p2->m_vehicleSize < 1.f;
        }
        else {
            miniWave = hasP2 && p2->m_isDart && p2->m_vehicleSize < 1.f;
        }

        // Rapid inputs in wave: let them through.
        if (g_counter < 8) {
            int prev = g_burst;
            g_burst = prev + 1;
            g_counter = 0;
            if (prev >= 2 && anyWave) return passThrough();
        }
        else {
            g_counter = 0;
            g_burst = 0;
        }

        if (!down && !g_releases) {
            g_lastDown = false;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // Releases only matter for wave and robot (and ship, if Ship is on).
        bool shipRelease = g_ship && (p1->m_isShip || (hasP2 && p2->m_isShip));
        if (!p1->m_isDart && !shipRelease) {
            bool waveP2 = hasP2 && !p1->m_isRobot && p2->m_isDart;
            if (!waveP2) {
                bool robot = (p1->m_isRobot || !hasP2) ? p1->m_isRobot : p2->m_isRobot;
                if (!down && !robot) {
                    g_lastDown = false;
                    return GJBaseGameLayer::handleButton(down, button, isPlayer1);
                }
            }
        }

        char const* kind = down ? "PRESS" : "RELEASE";
        g_flipAfter = false;
        int horizon = miniWave ? 28 : (anyWave ? 38 : 48);
        int limit = anyWave ? g_limitWave : g_limitAll;

        bool before = false;
        int base = simulate(pl, p1, p2, dual, 0, down, horizon, &before);
        if (base >= horizon || limit <= 0) return passThrough();

        // The original's safety rule for a delay of d ticks (wave gets stricter
        // as d grows; same numbers as its own search).
        auto safe = [&](int d, int r) {
            if (anyWave) {
                int need = base + 10 + 2 * (d - 1);
                int gain = 6 + 2 * (d - 1);
                return r < horizon ? r >= need : (r - base >= gain || r >= need);
            }
            return r >= std::min(base + 12, horizon);
        };

        // Randomize: start the original's search at a random delay (+0 to +3
        // above where it normally starts) and take the first safe delay from
        // there. Every run before the pick is a failed one, exactly like the
        // original's search, so no run starts from a ghost left over from a
        // successful run. Nothing safe from there up: normal search from 1.
        int first = 1; // always from 1 tick, like the original
        int offset = g_randomize > 0 ? std::uniform_int_distribution<int>(0, g_randomize)(rng()) : 0;
        int start = std::min(first + offset, limit);
        int chosen = 0;
        int result = 0;
        bool fallback = false;
        for (int d = start; d <= limit; ++d) {
            result = simulate(pl, p1, p2, dual, d, down, horizon, &before);
            if (before) break;
            if (safe(d, result)) {
                chosen = d;
                break;
            }
        }
        if (chosen <= 0 && start > first) {
            fallback = true;
            for (int d = first; d < start; ++d) {
                result = simulate(pl, p1, p2, dual, d, down, horizon, &before);
                if (before) return passThrough();
                if (safe(d, result)) {
                    chosen = d;
                    break;
                }
            }
        }
        if (chosen <= 0) return passThrough();
        bool random = g_randomize > 0 && !fallback;
        std::string pickNote = g_randomize <= 0 ? std::string() : fallback ? std::string(", fallback")
                                                                : fmt::format(", random +{}", start - first);

        log::info("SAVED +{} ticks ({:.2f} frames @ 60fps) {} (Frame0: {} -> Delayed: {}) [Limit: {}]{}",
            chosen, chosen * 0.25, kind, base, result, limit, pickNote);
        g_pendingDown = down;
        g_pending = chosen;
        double ms = chosen * (1000.0 / 240.0);
        ui::popup(fmt::format("{} {:.1f}ms early ({} tick{}{})", down ? "Click" : "Release", ms,
            chosen, chosen == 1 ? "" : "s", pickNote));
        pb::menu::saved(chosen, ms, modeIndex(p1), !down, random, levelPercent(pl));
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        if (!g_sim || !objects) {
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }
        // Ghosts only touch blocks, spikes, slopes and collision objects.
        static gd::vector<GameObject*> filtered;
        filtered.clear();
        filtered.reserve(std::max(count, 0));
        int n = std::min<int>(count, static_cast<int>(objects->size()));
        for (int i = 0; i < n; ++i) {
            auto obj = (*objects)[i];
            if (!obj) continue;
            auto type = static_cast<unsigned>(obj->m_objectType);
            if (type <= 47 && ((0x808002000005ull >> type) & 1)) filtered.push_back(obj);
        }
        GJBaseGameLayer::collisionCheckObjects(player, &filtered, static_cast<int>(filtered.size()), dt);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (g_sim) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (g_sim) return;
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};

class $modify(PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", -1000000);
        (void)self.setHookPriority("PlayerObject::incrementJumps", -1000000);
        (void)self.setHookPriority("PlayerObject::ringJump", -1000000);
    }

    void update(float dt) {
        if (!g_sim) {
            auto pl = GameManager::sharedState()->m_playLayer;
            if (pl && this == pl->m_player1) {
#ifdef GEODE_IS_WINDOWS
                if (GetAsyncKeyState(VK_F6) & 1) {
                    g_stage = (g_stage + 1) % 5;
                    g_pending = -1;
                    g_flipAfter = false;
                    switch (g_stage) {
                        case 0:
                            g_enabled = true; g_limitAll = 2; g_limitWave = 2;
                            
                            break;
                        case 1:
                            g_enabled = true; g_limitAll = 4; g_limitWave = 4;
                            
                            break;
                        case 2:
                            g_enabled = true; g_limitAll = 8; g_limitWave = 8;
                            
                            break;
                        case 3:
                            g_enabled = true; g_limitAll = 16; g_limitWave = 8;
                            
                            break;
                        default:
                            g_enabled = false; g_limitAll = 0; g_limitWave = 0;
                            
                            break;
                    }
                    auto stageText = g_enabled ? fmt::format("Stage {}: {} / {} ticks", g_stage + 1, g_limitAll, g_limitWave)
                                               : std::string("Buffer OFF");
                    ui::popup(stageText, g_enabled);
                    pb::menu::log(stageText);
                    // Keep the window in sync with the stage.
                    Mod::get()->setSettingValue<bool>("enabled", g_enabled);
                    if (g_enabled) Mod::get()->setSettingValue<int64_t>("buffer-ticks", g_limitAll);
                }
#endif
                if (pl->m_attempts != g_lastAttempt) {
                    g_lastAttempt = pl->m_attempts;
                    pb::menu::attempt(g_lastAttempt);
                }
                if (g_enabled) {
                    if (g_counter <= 998) g_counter++;
                    bool ship = !g_ship && (m_isShip
                        || (pl->m_gameState.m_isDualMode && pl->m_player2 && pl->m_player2->m_isShip));
                    if (ship) {
                        g_pending = -1;
                        g_flipAfter = false;
                        if (g_held != g_lastDown) feed(pl, g_held);
                    }
                    else if (g_pending == 0) {
                        feed(pl, g_pendingDown);
                        if (g_flipAfter) {
                            g_pending = -2;
                            g_flipAfter = false;
                        }
                        else {
                            g_pending = -1;
                        }
                    }
                    else if (g_pending > 0) {
                        g_pending--;
                    }
                    else if (g_pending == -1) {
                        if (g_held != g_lastDown) feed(pl, g_held);
                    }
                    else if (g_pending == -2) {
                        feed(pl, !g_pendingDown);
                        g_flipAfter = false;
                        g_pending = -1;
                    }
                }
            }
        }

        PlayerObject::update(dt);

        auto pl = GameManager::sharedState()->m_playLayer;
        if (pl && !g_sim) {
            float warp = pl->m_gameState.m_timeWarp;
            g_dt = warp > 0.f ? dt / warp : dt;
        }
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (g_sim) return;
        PlayerObject::ringJump(ring, skipCheck);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (g_sim) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (g_sim) return;
        PlayerObject::incrementJumps();
    }
};

class $modify(EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", -1000000);
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (g_sim) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", -1000000);
    }

    void playShineEffect() {
        if (g_sim) return;
        GameObject::playShineEffect();
    }
};

class $modify(HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", -1000000);
    }

    void addPoint(CCPoint point) {
        if (g_sim) return;
        HardStreak::addPoint(point);
    }
};

$on_mod(Loaded) {
    readSettings();
    log::info("v4.7 LOADED (Zero-Lag + Up Arrow Fix). F6 cycles 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF, F7 shows the window.");

    listenForKeybindSettingPresses("menu-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) pb::menu::toggle();
    });

    listenForSettingChanges<bool>("enabled", [](bool enabled) {
        if (!enabled) {
            // Don't swallow an input that was waiting to fire.
            if (auto pl = PlayLayer::get(); pl && g_pending >= 0) feed(pl, g_pendingDown);
            g_pending = -1;
            g_flipAfter = false;
        }
    });

    listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) {
        readSettings();
        pb::menu::sync();
    });
}

$on_game(Loaded) {
    if (Mod::get()->getSettingValue<bool>("open-window")) pb::menu::open();
    else pb::menu::prepare();
}
