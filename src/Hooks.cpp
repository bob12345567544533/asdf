// Predictive Input Buffer: when you click a little too early and a hidden ghost
// run says that click kills you, the click is held back a few ticks (as little
// as needed) and fired when it's safe.
//
// Config   settings
// Ghost    hidden player copies that run ahead
// Buffer   holding inputs back and firing them
// Window   the F7 window (Windows), Popup the in-game timing line

#include "Buffer.hpp"
#include "Config.hpp"
#include "Ghost.hpp"
#include "Popup.hpp"
#include "Window.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace {

// Runs before other mods' hooks, so a ghost run never reaches them.
constexpr int FIRST = -1000000;

int levelPercent(PlayLayer* layer) {
    return static_cast<int>(std::clamp(layer->getCurrentPercent(), 0.f, 100.f));
}

// Portals, pads and teleports act on contact (speed portals only if they
// change the speed); orbs only while the button is held.
bool stopsGhost(GameObject* object, PlayerObject* ghost) {
    float speed = 0.f;
    switch (object->m_objectID) {
        case 200: speed = 0.7f; break;
        case 201: speed = 0.9f; break;
        case 202: speed = 1.1f; break;
        case 203: speed = 1.3f; break;
        case 1334: speed = 1.6f; break;
        default: break;
    }
    if (speed > 0.f) return std::abs(ghost->m_playerSpeed - speed) > 0.01f;
    switch (object->m_objectType) {
        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
        case GameObjectType::GravityTogglePortal:
        case GameObjectType::ShipPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::BallPortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal:
        case GameObjectType::InverseMirrorPortal:
        case GameObjectType::NormalMirrorPortal:
        case GameObjectType::RegularSizePortal:
        case GameObjectType::MiniSizePortal:
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::TeleportPortal:
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::SpiderPad:
            return true;
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::TeleportOrb:
            return pb::ghost::held(ghost);
        default:
            return false;
    }
}

#ifdef GEODE_IS_WINDOWS
// Safety net: paused or outside a level the cursor must be visible.
class CursorGuard : public CCObject {
public:
    void check(float) {
        auto layer = PlayLayer::get();
        if (layer && !layer->m_isPaused) return; // GD hides it while you play
        if (auto view = CCEGLView::get()) {
            view->toggleLockCursor(false);
            view->showCursor(true);
        }
        CURSORINFO info { sizeof(info) };
        if (GetCursorInfo(&info) && !(info.flags & CURSOR_SHOWING)) {
            for (int i = 0; i < 16 && ShowCursor(TRUE) < 0; ++i) {}
        }
    }
};
#endif

} // namespace

class $modify(PBPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", FIRST);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", FIRST);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        pb::buffer::levelStarted(this);
        return true;
    }

    void onQuit() {
        PlayLayer::onQuit();
        pb::buffer::levelEnded();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        pb::buffer::reset();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (pb::ghost::running() && pb::ghost::isGhost(player)) {
            pb::ghost::markDead();
            return;
        }
        PlayLayer::destroyPlayer(player, object);
        // only a real death drops what's held back (noclip calls this without dying)
        if (player && player->m_isDead) pb::buffer::reset();
        if (player == m_player1 && player->m_isDead) pb::window::death(levelPercent(this));
    }

    void playEndAnimationToPos(CCPoint pos) {
        if (pb::ghost::running()) return;
        PlayLayer::playEndAnimationToPos(pos);
    }
};

class $modify(LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        pb::buffer::levelEnded();
        return ok;
    }
};

class $modify(GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", FIRST);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", FIRST);
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", FIRST);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", FIRST);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (pb::ghost::running() || pb::buffer::feeding() || button != static_cast<int>(PlayerButton::Jump)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        auto layer = PlayLayer::get();
        if (!layer || static_cast<GJBaseGameLayer*>(layer) != static_cast<GJBaseGameLayer*>(this)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (pb::buffer::take(layer, down, isPlayer1)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Ghosts only touch blocks, spikes, slopes and collision objects. Portals,
    // pads and orbs (which they can't use) end the run as "can't tell".
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        if (!pb::ghost::running() || !objects) {
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }
        static gd::vector<GameObject*> solid;
        solid.clear();
        int n = std::min<int>(count, static_cast<int>(objects->size()));
        solid.reserve(std::max(n, 0));
        bool watchStops = pb::config().stopAtPortals;
        for (int i = 0; i < n; ++i) {
            auto obj = (*objects)[i];
            if (!obj) continue;
            auto type = static_cast<unsigned>(obj->m_objectType);
            if (type <= 47 && ((0x808002000005ull >> type) & 1)) {
                solid.push_back(obj);
            }
            else if (watchStops && !pb::ghost::stopped() && stopsGhost(obj, player)
                     && player->getObjectRect().intersectsRect(obj->getObjectRect())) {
                pb::ghost::markStopped();
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &solid, static_cast<int>(solid.size()), dt);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (pb::ghost::running()) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (pb::ghost::running()) return;
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};

class $modify(PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", FIRST);
        (void)self.setHookPriority("PlayerObject::incrementJumps", FIRST);
        (void)self.setHookPriority("PlayerObject::ringJump", FIRST);
    }

    void update(float dt) {
        if (pb::ghost::running()) return PlayerObject::update(dt);
        auto layer = PlayLayer::get();
        bool player1 = layer && this == layer->m_player1;
        if (player1) pb::buffer::beforeStep(layer, dt);
        PlayerObject::update(dt);
        if (player1) pb::buffer::afterStep(layer, dt);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (pb::ghost::running()) return;
        PlayerObject::ringJump(ring, skipCheck);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (pb::ghost::running()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (pb::ghost::running()) return;
        PlayerObject::incrementJumps();
    }
};

class $modify(EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", FIRST);
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (pb::ghost::running()) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", FIRST);
    }

    void playShineEffect() {
        if (pb::ghost::running()) return;
        GameObject::playShineEffect();
    }
};

class $modify(HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", FIRST);
    }

    void addPoint(CCPoint point) {
        if (pb::ghost::running()) return;
        HardStreak::addPoint(point);
    }
};

$on_mod(Loaded) {
    pb::loadConfig();
    log::info("Predictive Input Buffer loaded. {} toggles the window, {} cycles 2 -> 4 -> 8 -> 16 ticks -> off.",
        "F7", "F6");

    listenForKeybindSettingPresses("menu-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) pb::window::toggle();
    });
    listenForKeybindSettingPresses("toggle-key", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;
        auto line = pb::cycleStage(); // saves the settings, which reloads everything below
        if (pb::config().popup) pb::popup::show(line, pb::config().enabled);
        pb::window::note(line);
    });

    listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) {
        bool wasOn = pb::config().enabled && pb::config().maxTicks > 0;
        pb::loadConfig();
        bool isOn = pb::config().enabled && pb::config().maxTicks > 0;
        if (wasOn && !isOn) pb::buffer::flush(); // don't swallow an input that was waiting
        pb::window::sync();
    });
}

$on_game(Loaded) {
#ifdef GEODE_IS_WINDOWS
    static auto guard = new CursorGuard();
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(CursorGuard::check), guard, 0.5f, false);
#endif
    if (Mod::get()->getSettingValue<bool>("open-window")) pb::window::open();
    else pb::window::prepare();
}
