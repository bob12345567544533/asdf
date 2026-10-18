#include "Buffer.hpp"
#include "Menu.hpp"
#include "Overlay.hpp"

#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Level lifecycle: ghost players, overlay, cleanup
// ---------------------------------------------------------------------------

class $modify(PBPlayLayer, PlayLayer) {
    struct Fields {
        Ref<PlayerObject> ghost1;
        Ref<PlayerObject> ghost2;

        ~Fields() {
            pb::clearGhosts(ghost1.data(), ghost2.data());
        }
    };

    PlayerObject* makeGhost() {
        auto ghost = PlayerObject::create(1, 1, this, this, true);
        ghost->setPosition({ 0.f, 105.f });
        ghost->setVisible(false);
        m_objectLayer->addChild(ghost);
        return ghost;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        pb::clearQueues();
        pb::resetTracks();
        pb::g.tick = 0;
        pb::g.stats.attemptCount = 0;

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_fields->ghost1 = this->makeGhost();
        m_fields->ghost2 = this->makeGhost();
        pb::setGhosts(m_fields->ghost1.data(), m_fields->ghost2.data());

        if (auto overlay = pb::BufferOverlay::create()) {
            this->addChild(overlay, 10000);
        }
        return true;
    }

    void resetLevel() {
        pb::clearQueues();
        pb::resetTracks();
        pb::g.tick = 0;
        pb::g.stats.attemptCount = 0;
        PlayLayer::resetLevel();
        if (auto overlay = pb::BufferOverlay::get(this)) overlay->refresh();
    }

    void onQuit() {
        pb::clearQueues();
        pb::clearGhosts(m_fields->ghost1.data(), m_fields->ghost2.data());
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (pb::g_simulating) {
            // A ghost hit something. Note it and stop: no death effects, no reset.
            // Same hitbox rule as the real player so predictions match.
            if (pb::isGhost(player) && !pb::hitboxForgives(this, player, object)) pb::g_simDied = true;
            return;
        }
        // Hitbox forgiveness: a hazard that only grazed the outer edge doesn't count.
        if (pb::hitboxForgives(this, player, object)) return;
        PlayLayer::destroyPlayer(player, object);
        // Only drop held inputs if you actually died (noclip keeps you alive).
        if (player && player->m_isDead) pb::clearQueues();
    }

    void playEndAnimationToPos(CCPoint pos) {
        if (pb::g_simulating) return;
        PlayLayer::playEndAnimationToPos(pos);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        pb::onFrameEnd(this);
    }
};

// ---------------------------------------------------------------------------
// Input interception and side-effect blocking for the ghosts
// ---------------------------------------------------------------------------

class $modify(PBBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            if (pb::onButton(pl, down, button, isPlayer1)) return;
            pb::noteHeld(pl, down, button, isPlayer1);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void update(float dt) {
        // Danger slow-mo: run this frame a bit slower if the last check saw trouble.
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this && pb::g.slowFactor < 1.f) {
            dt *= pb::g.slowFactor;
        }
        GJBaseGameLayer::update(dt);
    }

    // Ghosts only collide with blocks, slopes and hazards. Orbs, pads, portals,
    // coins and triggers would change the real level state, so they're skipped.
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        if (!pb::g_simulating || !objects) {
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }

        static int depth = 0;
        static gd::vector<GameObject*> shared;
        gd::vector<GameObject*> local;
        auto& filtered = depth == 0 ? shared : local;
        filtered.clear();

        int n = std::min<int>(count, static_cast<int>(objects->size()));
        for (int i = 0; i < n; ++i) {
            auto obj = (*objects)[i];
            if (!obj) continue;
            switch (obj->m_objectType) {
                case GameObjectType::Solid:
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard:
                case GameObjectType::Slope:
                    filtered.push_back(obj);
                    break;
                default:
                    break;
            }
        }

        depth++;
        GJBaseGameLayer::collisionCheckObjects(player, &filtered, static_cast<int>(filtered.size()), dt);
        depth--;
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (pb::g_simulating) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (pb::g_simulating) return;
        if (auto pl = PlayLayer::get(); pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            pb::noteRingTouched(pl, player, ring);
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (pb::g_simulating) {
            // Keep the physics, skip everything else.
            if (player) player->m_isUpsideDown = flip;
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }
};

class $modify(PBPlayerObject, PlayerObject) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (!pb::g_simulating && pl) {
            if (this == pl->m_player1) pb::onStep(pl, dt);
            if (this == pl->m_player1 || this == pl->m_player2) pb::beforeRealUpdate(pl, this);
        }

        // Ship / UFO / robot assist: keep only part of this step's change in
        // vertical speed, so everything moves a little more gently. Ghosts get
        // it too, so the buffer's predictions match.
        float assist = pb::assistFor(pl, this);
        double before = m_yVelocity;

        PlayerObject::update(dt);

        if (assist > 0.f) {
            m_yVelocity = before + (m_yVelocity - before) * (1.0 - assist);
        }
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (pb::g_simulating) return;
        if (auto pl = PlayLayer::get()) pb::noteRingUsed(pl, this);
        PlayerObject::ringJump(ring, skipCheck);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (pb::g_simulating) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (pb::g_simulating) return;
        PlayerObject::incrementJumps();
    }
};

class $modify(EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (pb::g_simulating) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(GameObject) {
    void playShineEffect() {
        if (pb::g_simulating) return;
        GameObject::playShineEffect();
    }
};

class $modify(HardStreak) {
    void addPoint(CCPoint point) {
        if (pb::g_simulating) return;
        HardStreak::addPoint(point);
    }
};

// ---------------------------------------------------------------------------
// Toggle keybind and live settings
// ---------------------------------------------------------------------------

$on_mod(Loaded) {
    listenForKeybindSettingPresses("toggle-key", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;

        bool enabled = !Mod::get()->getSettingValue<bool>("enabled");
        Mod::get()->setSettingValue<bool>("enabled", enabled);

        auto pl = PlayLayer::get();
        if (!enabled && pl) {
            // Don't swallow anything that was waiting to fire.
            pb::flushChannel(pl, 0);
            pb::flushChannel(pl, 1);
        }

        pb::menu::log(enabled ? "Buffer ON" : "Buffer OFF");

        if (auto overlay = pb::BufferOverlay::get(pl)) {
            overlay->showToggle(enabled);
        }
        else {
            Notification::create(
                enabled ? "Buffer ON" : "Buffer OFF",
                enabled ? NotificationIcon::Success : NotificationIcon::Info
            )->show();
        }
    });

    listenForKeybindSettingPresses("menu-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) pb::menu::toggle();
    });

    pb::refreshCachedSettings();

    listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) {
        pb::refreshCachedSettings();
        if (auto overlay = pb::BufferOverlay::get(PlayLayer::get())) overlay->refresh();
        pb::menu::sync();
    });
}

// Open the window when the game starts, like the old console did.
$on_game(Loaded) {
    if (Mod::get()->getSettingValue<bool>("open-window")) pb::menu::open();
}
