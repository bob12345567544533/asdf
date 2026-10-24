// Minimal Helper: companion mod for the Minimal texture pack.
//
// 1. Continuous background: the texture pack animates the menu backgrounds with
//    Happy Textures, but every screen starts its animation from scratch. Here
//    the same motion is driven from one clock, so it carries on between screens.
// 2. The main menu play button opens favorite levels (back returns to the menu).
// 3. Settings: the Rate button becomes a Levels button for the main levels.
// 4. Saved / favorite levels: the bottom-left info button is hidden and the
//    heart takes its place.
// 5. Pause menu: every side button gets the same size.

#include <Geode/Geode.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/OptionsLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

using namespace geode::prelude;

namespace {

bool setting(char const* key) {
    return Mod::get()->getSettingValue<bool>(key);
}

bool endsWith(std::string const& s, char const* suffix) {
    std::string_view suf(suffix);
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// ---------------------------------------------------------------------------
// 1. Continuous background
// ---------------------------------------------------------------------------

constexpr double TAU = 6.283185307179586;

// Same numbers as the texture pack's Happy Textures loops: 7 s each way for the
// drift, 9 s each way for the zoom, 1.1 s / 1.6 s each way for the two star layers.
double g_clock = 0.0;

float driftWave() { return static_cast<float>(std::sin(TAU * g_clock / 14.0)); }        // -1 .. 1
float zoomWave() { return static_cast<float>(0.5 - 0.5 * std::cos(TAU * g_clock / 18.0)); } // 0 .. 1

GLubyte starOpacity(double period, bool startVisible) {
    double v = 0.5 + 0.5 * std::cos(TAU * g_clock / period);
    if (!startVisible) v = 1.0 - v;
    return static_cast<GLubyte>(std::clamp(v, 0.0, 1.0) * 255.0);
}

// The pack's own moving nebula sprite (main menu, main levels page).
void driveNebula(CCNode* node, CCSize const& win) {
    node->stopAllActions();
    auto size = node->getContentSize();
    if (size.width <= 0 || size.height <= 0) return;
    float cover = std::max(win.width / size.width, win.height / size.height);
    node->setAnchorPoint({ 0.5f, 0.5f });
    node->setScale(cover * (1.27f + 0.38f * zoomWave()));
    float d = driftWave();
    node->setPosition({ win.width / 2 + 55.f * d, win.height / 2 + 27.5f * d });
}

// GD's menu background (GJ_gradientBG), stretched to the screen per axis.
void driveGradient(CCNode* node, CCSize const& win) {
    node->stopAllActions();
    auto size = node->getContentSize();
    if (size.width <= 0 || size.height <= 0) return;
    float z = 1.4f * (1.f + 0.25f * zoomWave());
    node->setAnchorPoint({ 0.5f, 0.5f });
    node->setScaleX(win.width / size.width * z);
    node->setScaleY(win.height / size.height * z);
    float d = driftWave();
    node->setPosition({ win.width / 2 + 20.f * d, win.height / 2 + 10.f * d });
}

void driveStars(CCNode* node) {
    auto sprite = typeinfo_cast<CCSprite*>(node);
    if (!sprite) return;
    sprite->stopAllActions();
    std::string id(node->getID());
    if (endsWith(id, "chaz-stars1")) sprite->setOpacity(starOpacity(2.2, true));
    else sprite->setOpacity(starOpacity(3.2, false));
}

bool isStars(std::string const& id) {
    return endsWith(id, "chaz-stars1") || endsWith(id, "chaz-stars2");
}

void visit(CCNode* node, CCSize const& win, int depth) {
    if (!node || depth > 4) return;
    auto children = node->getChildren();
    if (!children) return;
    for (auto child : CCArrayExt<CCNode*>(children)) {
        std::string id(child->getID());
        if (endsWith(id, "chaz-bg")) {
            driveNebula(child, win);
        }
        else if (isStars(id)) {
            driveStars(child);
        }
        else if (id == "background") {
            // only the backgrounds the pack animates (they carry its star layers)
            bool animated = false;
            if (auto kids = child->getChildren()) {
                for (auto k : CCArrayExt<CCNode*>(kids)) {
                    if (isStars(std::string(k->getID()))) { animated = true; break; }
                }
            }
            if (animated) driveGradient(child, win);
        }
        visit(child, win, depth + 1);
    }
}

class BackgroundDriver : public CCObject {
public:
    void tick(float dt) {
        g_clock += std::min(dt, 0.1f);
        if (!setting("continuous-background")) return;
        auto director = CCDirector::get();
        auto scene = director->getRunningScene();
        if (!scene) return;
        auto win = director->getWinSize();
        // during a fade both screens are drawn by the transition, not as children
        if (auto transition = typeinfo_cast<CCTransitionScene*>(scene)) {
            visit(transition->m_pInScene, win, 0);
            visit(transition->m_pOutScene, win, 0);
        }
        visit(scene, win, 0);
    }
};

// ---------------------------------------------------------------------------
// 2. Play -> favorite levels
// ---------------------------------------------------------------------------

bool g_favoritesFromMenu = false;

bool isLocalLevelList(GJSearchObject* search) {
    if (!search) return false;
    return search->m_searchType == SearchType::SavedLevels || search->m_searchType == SearchType::FavouriteLevels;
}

void goToMenu() {
    g_favoritesFromMenu = false;
    CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, MenuLayer::scene(false)));
}

// Run something on a node a little later (after other mods and Happy Textures
// have finished setting the screen up).
void later(CCNode* node, float delay, std::function<void()> fn) {
    node->runAction(CCSequence::create(CCDelayTime::create(delay), CallFuncExt::create(std::move(fn)), nullptr));
}

} // namespace

$on_game(Loaded) {
    static auto driver = new BackgroundDriver();
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(BackgroundDriver::tick), driver, 0.f, false);
}

class $modify(ChazMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        g_favoritesFromMenu = false;
        return true;
    }

    void onPlay(CCObject* sender) {
        if (!setting("play-opens-favorites")) return MenuLayer::onPlay(sender);
        g_favoritesFromMenu = true;
        auto scene = LevelBrowserLayer::scene(GJSearchObject::create(SearchType::FavouriteLevels));
        CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, scene));
    }
};

// ---------------------------------------------------------------------------
// 2 + 4. Saved / favorite levels
// ---------------------------------------------------------------------------

class $modify(ChazLevelBrowserLayer, LevelBrowserLayer) {
    bool init(GJSearchObject* search) {
        if (!LevelBrowserLayer::init(search)) return false;
        if (setting("clean-level-lists") && isLocalLevelList(search)) {
            later(this, 0.05f, [this] { this->moveHeart(); });
            later(this, 0.4f, [this] { this->moveHeart(); });
        }
        return true;
    }

    void moveHeart() {
        CCMenuItemSpriteExtra* info = nullptr;
        for (auto child : CCArrayExt<CCNode*>(this->getChildren())) {
            auto menu = typeinfo_cast<CCMenu*>(child);
            if (!menu || !menu->getChildren()) continue;
            for (auto item : CCArrayExt<CCNode*>(menu->getChildren())) {
                auto button = typeinfo_cast<CCMenuItemSpriteExtra*>(item);
                if (!button || !button->getNormalImage()) continue;
                auto image = button->getNormalImage();
                if (isSpriteFrameName(image, "GJ_infoIcon_001.png") || isSpriteFrameName(image, "GJ_infoBtn_001.png")) {
                    info = button;
                    break;
                }
            }
            if (info) break;
        }
        auto heart = typeinfo_cast<CCMenuItemSpriteExtra*>(this->getChildByIDRecursive("favorite-button"));
        if (!info || !heart || !info->getParent() || !heart->getParent()) return;
        if (info->isVisible()) {
            auto world = info->getParent()->convertToWorldSpace(info->getPosition());
            heart->setPosition(heart->getParent()->convertToNodeSpace(world));
            info->setVisible(false);
            info->setEnabled(false);
        }
    }

    void keyBackClicked() {
        if (g_favoritesFromMenu && isLocalLevelList(m_searchObject)) return goToMenu();
        LevelBrowserLayer::keyBackClicked();
    }

    void onBack(CCObject* sender) {
        if (g_favoritesFromMenu && isLocalLevelList(m_searchObject)) return goToMenu();
        LevelBrowserLayer::onBack(sender);
    }
};

// ---------------------------------------------------------------------------
// 3. Settings: Rate -> Levels
// ---------------------------------------------------------------------------

class $modify(ChazOptionsLayer, OptionsLayer) {
    void customSetup() {
        OptionsLayer::customSetup();
        if (!setting("levels-in-settings") || !m_mainLayer) return;
        auto menu = m_mainLayer->getChildByID("options-menu");
        auto rate = menu ? typeinfo_cast<CCMenuItemSpriteExtra*>(menu->getChildByID("rate-button")) : nullptr;
        if (!rate) return;
        // reuse the Rate button so it keeps the exact same size, style and spot
        rate->setTarget(this, menu_selector(ChazOptionsLayer::onChazLevels));
        if (auto sprite = typeinfo_cast<ButtonSprite*>(rate->getNormalImage())) sprite->setString("Levels");
        rate->setID("levels-button"_spr);
    }

    void onChazLevels(CCObject*) {
        CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, LevelSelectLayer::scene(0)));
    }
};

// ---------------------------------------------------------------------------
// 5. Pause menu: same size side buttons
// ---------------------------------------------------------------------------

class $modify(ChazPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!setting("equal-pause-buttons")) return;
        later(this, 0.05f, [this] { this->equalizeSideButtons(); });
        later(this, 0.4f, [this] { this->equalizeSideButtons(); });
    }

    void equalizeSideButtons() {
        constexpr float size = 30.f; // longest side of every button, in GD points
        for (auto id : { "left-button-menu", "right-button-menu" }) {
            auto menu = this->getChildByID(id);
            if (!menu || !menu->getChildren()) continue;
            for (auto item : CCArrayExt<CCNode*>(menu->getChildren())) {
                auto button = typeinfo_cast<CCMenuItemSpriteExtra*>(item);
                if (!button || !button->isVisible()) continue;
                auto content = button->getContentSize();
                float longest = std::max(content.width, content.height);
                if (longest <= 0) continue;
                float scale = size / longest;
                button->setScale(scale);
                button->m_baseScale = scale;
            }
            menu->updateLayout();
        }
    }
};
