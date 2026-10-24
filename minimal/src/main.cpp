// Minimal Helper: companion mod for the Minimal texture pack.
//
// 1. Continuous background: the texture pack animates the menu backgrounds with
//    Happy Textures, but every screen starts its animation from scratch. Here
//    the same motion is driven from one clock, so it carries on between screens
//    (starting on the loading screen).
// 2. The main menu play button opens favorite levels (back returns to the menu).
// 3. Settings: the Rate button becomes a Levels button for the main levels.
// 4. Saved / favorite levels: the bottom-left info button is hidden and the
//    heart takes its place.
// 5. Pause menu + icon kit: every side button gets the same size.
// 6. Level page: GD's round loading ring becomes a thin square around the play box.
// 7. Sliders and the loading bar: the fill gets rounded ends like the groove.
// 8. Loading screen: the menu nebula instead of GD's blue squares.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJGarageLayer.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LoadingLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/OptionsLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/Slider.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

bool setting(char const* key) {
    return Mod::get()->getSettingValue<bool>(key);
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// Run something on a node a little later (after other mods and Happy Textures
// have finished setting the screen up).
void later(CCNode* node, float delay, std::function<void()> fn) {
    node->runAction(CCSequence::create(CCDelayTime::create(delay), CallFuncExt::create(std::move(fn)), nullptr));
}

// Run something on a node now and then every `interval` seconds while the node exists.
void every(CCNode* node, float interval, std::function<void()> fn) {
    node->runAction(CCRepeatForever::create(
        CCSequence::create(CallFuncExt::create(std::move(fn)), CCDelayTime::create(interval), nullptr)));
}

// A node's bounding rect expressed in another node's coordinate space.
CCRect rectIn(CCNode* space, CCNode* node) {
    auto size = node->getContentSize();
    if (node == space) return { 0, 0, size.width, size.height };
    auto a = space->convertToNodeSpace(node->convertToWorldSpace({ 0, 0 }));
    auto b = space->convertToNodeSpace(node->convertToWorldSpace({ size.width, size.height }));
    return { std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y) };
}

// How big one unit of `node`'s space is on screen.
float worldScale(CCNode* node) {
    auto a = node->convertToWorldSpace({ 0, 0 });
    auto b = node->convertToWorldSpace({ 100, 0 });
    return std::hypot(b.x - a.x, b.y - a.y) / 100.f;
}

// A white texture drawn in code and kept in GD's texture cache (so it is
// rebuilt automatically if GD ever clears the cache).
using Painter = std::function<float(float x, float y, int n)>; // returns alpha 0..1 for a pixel center

CCTexture2D* codeTexture(char const* key, int n, Painter const& paint) {
    auto cache = CCTextureCache::sharedTextureCache();
    if (auto tex = cache->textureForKey(key)) return tex;
    std::vector<unsigned char> px(static_cast<size_t>(n) * n * 4);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float a = std::clamp(paint(x + 0.5f, y + 0.5f, n), 0.f, 1.f);
            auto p = &px[(static_cast<size_t>(y) * n + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<unsigned char>(std::lround(a * 255.f));
        }
    }
    auto image = new CCImage();
    if (!image->initWithImageData(px.data(), static_cast<int>(px.size()), CCImage::kFmtRawData, n, n, 8)) {
        image->release();
        return nullptr;
    }
    auto tex = cache->addUIImage(image, key);
    image->release();
    return tex;
}

// Smooth white dot (caps of the rounded fill).
CCTexture2D* dotTexture() {
    return codeTexture("chaz.minimal-helper/dot", 32, [](float x, float y, int n) {
        float r = n / 2.f - 1.f;
        float d = std::hypot(x - n / 2.f, y - n / 2.f);
        return (r - d) / 1.2f + 0.5f;
    });
}

// Thin square outline (the level start progress). Line = 4 of 280 px, the same
// ratio as the play box's own outline once it is drawn 14% bigger than the box.
CCTexture2D* squareTexture() {
    return codeTexture("chaz.minimal-helper/square", 280, [](float x, float y, int n) {
        float edge = std::min({ x, y, n - x, n - y });
        return edge < 4.f ? 1.f : 0.f;
    });
}

// ---------------------------------------------------------------------------
// 1. Continuous background
// ---------------------------------------------------------------------------

constexpr double TAU = 6.283185307179586;

// Same numbers as the texture pack's Happy Textures loops: 7 s each way for the
// drift, 9 s each way for the zoom, 1.1 s / 1.6 s each way for the two star layers.
double g_clock = 0.0;

// --- Flowing nebula ----------------------------------------------------------
// Slowly flowing grayscale clouds drawn by a shader into a small texture (960 px
// wide, refreshed up to 120 times a second), which every menu background then
// shows. One shared texture + one clock = the same motion on every screen.

constexpr char const* FLOW_VERT = R"(
attribute vec4 a_position;
varying vec2 v_pos;
void main() {
    gl_Position = CC_MVPMatrix * a_position;
    v_pos = a_position.xy;
}
)";

constexpr char const* FLOW_FRAG = R"(
varying vec2 v_pos;
uniform vec2 u_size;
uniform vec4 u_offset;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise(vec2 x) {
    vec2 i = floor(x);
    vec2 f = fract(x);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float s = 0.0;
    float amp = 1.0;
    float tot = 0.0;
    for (int i = 0; i < 5; i++) {
        s += noise(p) * amp;
        tot += amp;
        p = mat2(1.6, 1.2, -1.2, 1.6) * p + vec2(17.1, 9.7);
        amp *= 0.5;
    }
    return s / tot;
}

void main() {
    vec2 uv = v_pos / u_size;
    vec2 p = mat2(0.8, 0.6, -0.6, 0.8) * (v_pos / u_size.y * 2.25) + vec2(40.0, 40.0);
    vec2 o = u_offset.xy;
    vec2 q = vec2(fbm(p + o), fbm(p + vec2(5.2, 1.3) + vec2(-o.y, o.x)));
    float v = fbm(p + 3.0 * q + u_offset.zw);
    v = pow(smoothstep(0.36, 0.88, v), 1.5);
    vec2 c = uv * 2.0 - 1.0;
    float vign = clamp(1.0 - 0.5 * dot(c, c), 0.0, 1.0);
    float g = (10.0 + 130.0 * v * vign + hash12(gl_FragCoord.xy) - 0.5) / 255.0;
    gl_FragColor = vec4(g, g, g, 1.0);
}
)";

class FlowQuad : public CCNode {
public:
    CCGLProgram* m_program = nullptr;
    GLint m_sizeLoc = -1;
    GLint m_offsetLoc = -1;
    float m_offset[4] = { 0, 0, 0, 0 };

    static FlowQuad* create(CCGLProgram* program) {
        auto ret = new FlowQuad();
        if (ret->init()) {
            ret->m_program = program;
            ret->m_sizeLoc = program->getUniformLocationForName("u_size");
            ret->m_offsetLoc = program->getUniformLocationForName("u_offset");
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void draw() override {
        auto size = this->getContentSize();
        m_program->use();
        m_program->setUniformsForBuiltins();
        m_program->setUniformLocationWith2f(m_sizeLoc, size.width, size.height);
        m_program->setUniformLocationWith4f(m_offsetLoc, m_offset[0], m_offset[1], m_offset[2], m_offset[3]);
        ccGLBindVAO(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position);
        GLfloat verts[8] = { 0, 0, size.width, 0, 0, size.height, size.width, size.height };
        glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, verts);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        CC_INCREMENT_GL_DRAWS(1);
    }
};

CCRenderTexture* g_flowTarget = nullptr;
FlowQuad* g_flowQuad = nullptr;
bool g_flowFailed = false;
double g_flowLastDraw = -1.0;

bool flowEnabled() {
    return setting("flowing-background");
}

CCGLProgram* flowProgram() {
    constexpr char const* key = "chaz.minimal-helper/flow";
    auto cache = CCShaderCache::sharedShaderCache();
    if (auto program = cache->programForKey(key)) return program;
    auto program = new CCGLProgram();
    if (!program->initWithVertexShaderByteArray(FLOW_VERT, FLOW_FRAG)) {
        log::error("flowing background: shader didn't compile");
        program->release();
        return nullptr;
    }
    program->addAttribute(kCCAttributeNamePosition, kCCVertexAttrib_Position);
    if (!program->link()) {
        log::error("flowing background: shader didn't link");
        program->release();
        return nullptr;
    }
    program->updateUniforms();
    cache->addProgram(program, key);
    program->release();
    return program;
}

float g_flowAspect = 0.f;

// The shared texture all backgrounds show, or nullptr if it can't be made.
CCTexture2D* flowTexture() {
    // remake it if the window changed shape, so the clouds never look stretched
    auto winNow = CCDirector::get()->getWinSize();
    if (g_flowTarget && winNow.height > 0 && std::abs(winNow.width / winNow.height - g_flowAspect) > 0.01f) {
        g_flowTarget->release();
        g_flowQuad->release();
        g_flowTarget = nullptr;
        g_flowQuad = nullptr;
    }
    if (!g_flowTarget) {
        if (g_flowFailed) return nullptr;
        auto program = flowProgram();
        auto win = CCDirector::get()->getWinSize();
        float csf = CC_CONTENT_SCALE_FACTOR();
        if (!program || win.width <= 0 || win.height <= 0 || csf <= 0) {
            g_flowFailed = true;
            return nullptr;
        }
        // 960 px wide, the screen's aspect ratio (sizes here are in points)
        int w = std::max(1, static_cast<int>(std::lround(960.f / csf)));
        int h = std::max(1, static_cast<int>(std::lround(960.f * win.height / win.width / csf)));
        auto target = CCRenderTexture::create(w, h, kCCTexture2DPixelFormat_RGBA8888);
        auto quad = FlowQuad::create(program);
        if (!target || !quad) {
            g_flowFailed = true;
            return nullptr;
        }
        target->retain();
        quad->retain();
        target->getSprite()->getTexture()->setAntiAliasTexParameters();
        quad->setContentSize({ static_cast<float>(w), static_cast<float>(h) });
        g_flowTarget = target;
        g_flowQuad = quad;
        g_flowLastDraw = -1.0;
        g_flowAspect = win.width / win.height;
    }
    return g_flowTarget->getSprite()->getTexture();
}

// Redraw the clouds for the current time (at most 120 times a second).
void drawFlow() {
    if (!flowTexture()) return;
    if (g_flowLastDraw >= 0.0 && g_clock - g_flowLastDraw < 1.0 / 120.0) return;
    g_flowLastDraw = g_clock;
    double speed = std::clamp(Mod::get()->getSettingValue<double>("background-speed"), 0.1, 5.0);
    // two slow circles with unrelated periods, so the motion never visibly repeats
    double a = TAU * g_clock * speed / 28.0;
    double b = TAU * g_clock * speed / 75.6;
    g_flowQuad->m_offset[0] = static_cast<float>(0.6 * std::cos(a));
    g_flowQuad->m_offset[1] = static_cast<float>(0.6 * std::sin(a));
    g_flowQuad->m_offset[2] = static_cast<float>(0.25 * std::cos(b));
    g_flowQuad->m_offset[3] = static_cast<float>(0.25 * std::sin(b));
    g_flowTarget->beginWithClear(0, 0, 0, 1);
    g_flowQuad->visit();
    g_flowTarget->end();
}

// Turn a background sprite into a window onto the flowing clouds, filling the screen.
void showFlow(CCSprite* sprite, CCTexture2D* tex, CCSize const& win) {
    sprite->stopAllActions();
    if (sprite->getTexture() != tex) {
        sprite->setUserObject("flow-host"_spr, CCBool::create(true));
        sprite->setTexture(tex);
        auto size = tex->getContentSize();
        sprite->setTextureRect({ 0, 0, size.width, size.height });
        sprite->setFlipY(true); // render textures are stored upside down
    }
    sprite->setColor({ 255, 255, 255 });
    sprite->setOpacity(255);
    auto size = sprite->getContentSize();
    if (size.width <= 0 || size.height <= 0) return;
    sprite->setAnchorPoint({ 0.5f, 0.5f });
    sprite->setScaleX(win.width / size.width * 1.02f);
    sprite->setScaleY(win.height / size.height * 1.02f);
    sprite->setPosition(win / 2);
}

float driftWave() { return static_cast<float>(std::sin(TAU * g_clock / 14.0)); }        // -1 .. 1
float zoomWave() { return static_cast<float>(0.5 - 0.5 * std::cos(TAU * g_clock / 18.0)); } // 0 .. 1

GLubyte starOpacity(double period, bool startVisible) {
    double v = 0.5 + 0.5 * std::cos(TAU * g_clock / period);
    if (!startVisible) v = 1.0 - v;
    return static_cast<GLubyte>(std::clamp(v, 0.0, 1.0) * 255.0);
}

// The pack's own moving nebula sprite (main menu, main levels page, loading screen).
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

void driveStars(CCNode* node, std::string_view id) {
    auto sprite = typeinfo_cast<CCSprite*>(node);
    if (!sprite) return;
    sprite->stopAllActions();
    if (endsWith(id, "chaz-stars1")) sprite->setOpacity(starOpacity(2.2, true));
    else sprite->setOpacity(starOpacity(3.2, false));
}

bool isStars(std::string_view id) {
    return endsWith(id, "chaz-stars1") || endsWith(id, "chaz-stars2");
}

struct Visit {
    CCSize win;
    CCTexture2D* flow = nullptr; // set when the flowing clouds replace the pack's nebula
    bool foundBackground = false;
};

void visit(CCNode* node, Visit& v, int depth) {
    if (!node || depth > 4) return;
    auto children = node->getChildren();
    // big containers (lists, object layers) never hold the background
    if (!children || children->count() > 200) return;
    for (auto child : CCArrayExt<CCNode*>(children)) {
        std::string_view id = child->getID();
        bool nebula = endsWith(id, "chaz-bg");
        bool gradient = false;
        if (!nebula && id == "background") {
            // the backgrounds the pack animates carry its star layers; GD's own menu
            // background sprite (GJ_gradientBG) counts too once clouds are on
            if (auto kids = child->getChildren()) {
                for (auto k : CCArrayExt<CCNode*>(kids)) {
                    if (isStars(k->getID())) { gradient = true; break; }
                }
            }
            if (!gradient && v.flow) {
                auto sprite = typeinfo_cast<CCSprite*>(child);
                gradient = sprite && (sprite->getUserObject("flow-host"_spr) || isSpriteName(sprite, "GJ_gradientBG.png"));
            }
        }
        if (nebula || gradient) {
            v.foundBackground = true;
            auto sprite = typeinfo_cast<CCSprite*>(child);
            if (v.flow && sprite) showFlow(sprite, v.flow, v.win);
            else if (nebula) driveNebula(child, v.win);
            else driveGradient(child, v.win);
        }
        else if (isStars(id)) {
            child->stopAllActions();
            if (v.flow) child->setVisible(false); // just the clouds, no dots
            else driveStars(child, id);
        }
        visit(child, v, depth + 1);
    }
}

class BackgroundDriver : public CCObject {
public:
    void tick(float dt) {
        g_clock += std::min(dt, 0.1f);
        if (!setting("continuous-background")) return;
        // nothing to animate while playing or editing a level
        if (PlayLayer::get() || LevelEditorLayer::get()) return;
        auto director = CCDirector::get();
        auto scene = director->getRunningScene();
        if (!scene) return;
        Visit v;
        v.win = director->getWinSize();
        v.flow = flowEnabled() ? flowTexture() : nullptr;
        // during a fade both screens are drawn by the transition, not as children
        if (auto transition = typeinfo_cast<CCTransitionScene*>(scene)) {
            visit(transition->m_pInScene, v, 0);
            visit(transition->m_pOutScene, v, 0);
        }
        visit(scene, v, 0);
        if (v.flow && v.foundBackground) drawFlow();
    }
};

void startBackgroundDriver() {
    static BackgroundDriver* driver = nullptr;
    if (driver) return;
    driver = new BackgroundDriver();
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(BackgroundDriver::tick), driver, 0.f, false);
}

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

// ---------------------------------------------------------------------------
// 5. Same size side buttons
// ---------------------------------------------------------------------------

// Every visible button in `menu` gets the same size: its longest side becomes `size` points.
void equalizeButtons(CCNode* menu, float size) {
    if (!menu || !menu->getChildren()) return;
    bool changed = false;
    for (auto item : CCArrayExt<CCNode*>(menu->getChildren())) {
        auto button = typeinfo_cast<CCMenuItemSpriteExtra*>(item);
        if (!button || !button->isVisible() || button->isSelected()) continue;
        auto content = button->getContentSize();
        float longest = std::max(content.width, content.height);
        if (longest <= 0) continue;
        // these menus use an auto-scaling layout that resets every button's scale
        // whenever it updates, so take the buttons out of its auto-scaling
        auto options = button->getLayoutOptions();
        if (!options) {
            button->setLayoutOptions(AxisLayoutOptions::create()->setAutoScale(false), false);
            changed = true;
        }
        else if (auto axis = typeinfo_cast<AxisLayoutOptions*>(options)) {
            if (axis->getAutoScale() != std::optional<bool>(false)) {
                axis->setAutoScale(false);
                changed = true;
            }
        }
        float scale = size / longest;
        if (std::abs(button->getScale() - scale) > 0.001f || std::abs(button->m_baseScale - scale) > 0.001f) {
            button->setScale(scale);
            button->m_baseScale = scale;
            changed = true;
        }
    }
    if (changed) menu->updateLayout();
}

constexpr float SIDE_BUTTON_SIZE = 34.f; // GD points, same in the pause menu and the icon kit

// ---------------------------------------------------------------------------
// 7. Rounded fill (sliders, loading bar)
// ---------------------------------------------------------------------------

// GD draws the fill as a tiled strip with square ends inside a rounded groove.
// The strip is made invisible and a rounded pill is drawn in its place: the left
// end sits in the groove's left curve with an even gap all around, the right end
// follows the strip. The pill lives inside the strip, so it hides when GD hides it.
void roundFill(CCSprite* bar, CCNode* groove) {
    if (!bar || !groove || !bar->getParent()) return;
    auto tex = dotTexture();
    if (!tex) return;
    auto parent = bar->getParent();

    auto g = rectIn(parent, groove);
    float h = g.size.height;
    if (h <= 0.f || bar->getScaleX() == 0.f || bar->getScaleY() == 0.f) return;
    float r = h * 0.22f;                       // fill is 44% of the groove height
    float cy = g.getMidY();
    float leftC = g.getMinX() + h / 2;         // same center as the groove's end curve
    float rightLimit = g.getMaxX() - h / 2;
    auto b = bar->boundingBox();
    float rightC = std::clamp(b.getMaxX() - r, leftC, std::max(leftC, rightLimit));
    bool show = b.size.width > 0.5f;

    bar->setCascadeOpacityEnabled(false);
    bar->setOpacity(0);

    auto texSize = tex->getContentSize();
    float onePx = 1.f / CC_CONTENT_SCALE_FACTOR();
    auto color = bar->getColor();
    ccColor3B fill = { static_cast<GLubyte>(color.r * 236 / 255), static_cast<GLubyte>(color.g * 236 / 255),
                       static_cast<GLubyte>(color.b * 236 / 255) };

    auto part = [&](char const* id, bool middle) -> CCSprite* {
        auto sprite = static_cast<CCSprite*>(bar->getChildByID(id));
        if (!sprite) {
            sprite = middle
                ? CCSprite::createWithTexture(tex, { texSize.width / 2 - onePx, 0, 2 * onePx, texSize.height })
                : CCSprite::createWithTexture(tex);
            if (!sprite) return nullptr;
            sprite->setID(id);
            bar->addChild(sprite);
        }
        else if (sprite->getTexture() != tex) {
            sprite->setTexture(tex);
            if (middle) sprite->setTextureRect({ texSize.width / 2 - onePx, 0, 2 * onePx, texSize.height });
            else sprite->setTextureRect({ 0, 0, texSize.width, texSize.height });
        }
        sprite->setColor(fill);
        sprite->setOpacity(255);
        return sprite;
    };
    // parent space -> the strip's own space
    auto local = [&](float x, float y) {
        return bar->convertToNodeSpace(parent->convertToWorldSpace({ x, y }));
    };
    float sx = 1.f / bar->getScaleX(), sy = 1.f / bar->getScaleY();

    auto left = part("fill-left"_spr, false);
    auto right = part("fill-right"_spr, false);
    auto mid = part("fill-middle"_spr, true);
    if (!left || !right || !mid) return;

    for (auto cap : { left, right }) {
        cap->setVisible(show);
        cap->setScaleX(2 * r / texSize.width * sx);
        cap->setScaleY(2 * r / texSize.height * sy);
    }
    left->setPosition(local(leftC, cy));
    right->setPosition(local(rightC, cy));

    float span = rightC - leftC;
    mid->setVisible(show && span > 0.01f);
    mid->setPosition(local((leftC + rightC) / 2, cy));
    mid->setScaleX(std::max(span, 0.01f) / (2 * onePx) * sx);
    mid->setScaleY(2 * r / texSize.height * sy);
}

// ---------------------------------------------------------------------------
// 8. Loading screen background
// ---------------------------------------------------------------------------

// The main menu's nebula + star layers (files from the texture pack). Their IDs
// end like the menu's, so the background driver moves them the same way and the
// motion carries straight on into the main menu.
void addMenuBackground(CCNode* layer) {
    auto win = CCDirector::get()->getWinSize();
    if (auto flow = flowEnabled() ? flowTexture() : nullptr) {
        // flowing clouds (the driver keeps it filling the screen and redraws it)
        auto clouds = CCSprite::createWithTexture(flow);
        if (clouds) {
            clouds->setID("loading-chaz-bg"_spr);
            showFlow(clouds, flow, win);
            layer->addChild(clouds, -100);
            return;
        }
    }
    auto nebula = CCSprite::create("chaz_menu_bg.png");
    if (!nebula) return; // texture pack not active
    nebula->setID("loading-chaz-bg"_spr);
    nebula->setPosition(win / 2);
    auto size = nebula->getContentSize();
    if (size.width > 0 && size.height > 0) {
        nebula->setScale(std::max(win.width / size.width, win.height / size.height) * 1.27f);
    }
    layer->addChild(nebula, -100);
    for (int i = 1; i <= 2; i++) {
        auto stars = CCSprite::create(fmt::format("chaz_stars{}.png", i).c_str());
        if (!stars) continue;
        stars->setID(fmt::format("{}/loading-chaz-stars{}", Mod::get()->getID(), i));
        stars->setPosition(win / 2);
        stars->setScale(1.25f);
        if (i == 2) stars->setOpacity(0);
        layer->addChild(stars, -99);
    }
}

} // namespace

$on_game(Loaded) {
    startBackgroundDriver();
}

class $modify(ChazLoadingLayer, LoadingLayer) {
    bool init(bool refresh) {
        if (!LoadingLayer::init(refresh)) return false;
        if (setting("clean-loading-screen")) {
            // GD's blue squares
            CCNode* bg = this->getChildByID("bg-texture");
            if (!bg) {
                for (auto child : CCArrayExt<CCNode*>(this->getChildren())) {
                    if (isSpriteName(child, "game_bg_01_001.png")) { bg = child; break; }
                }
            }
            if (bg) bg->setVisible(false);
            addMenuBackground(this);
            startBackgroundDriver();
        }
        if (setting("round-fill")) {
            this->schedule(schedule_selector(ChazLoadingLayer::chazLoadingTick));
        }
        return true;
    }

    void chazLoadingTick(float) {
        if (!m_sliderBar) return;
        CCNode* groove = m_sliderBar->getParent();
        if (!groove || groove == this) groove = this->getChildByID("progress-slider");
        roundFill(m_sliderBar, groove);
    }
};

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
// 5. Same size side buttons: pause menu + icon kit
// ---------------------------------------------------------------------------

class $modify(ChazPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!setting("equal-pause-buttons")) return;
        // keep checking: other mods add their buttons at different times
        every(this, 0.25f, [this] {
            equalizeButtons(this->getChildByID("left-button-menu"), SIDE_BUTTON_SIZE);
            equalizeButtons(this->getChildByID("right-button-menu"), SIDE_BUTTON_SIZE);
        });
    }
};

class $modify(ChazGarageLayer, GJGarageLayer) {
    bool init() {
        if (!GJGarageLayer::init()) return false;
        if (setting("equal-pause-buttons")) {
            every(this, 0.25f, [this] {
                equalizeButtons(this->getChildByIDRecursive("shards-menu"), SIDE_BUTTON_SIZE);
            });
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// 6. Level page: square loading progress instead of GD's ring
// ---------------------------------------------------------------------------

class $modify(ChazLevelInfoLayer, LevelInfoLayer) {
    struct Fields {
        bool m_watching = false;
    };

    void onPlay(CCObject* sender) {
        LevelInfoLayer::onPlay(sender);
        if (!setting("square-play-loading")) return;
        if (!m_fields->m_watching) {
            m_fields->m_watching = true;
            this->schedule(schedule_selector(ChazLevelInfoLayer::chazPlayTick));
        }
        this->chazPlayTick(0.f);
    }

    void chazPlayTick(float) {
        if (!m_playSprite) return;
        // GD dims the box and stacks its ring parts on it: keep the box as is
        m_playSprite->setColor({ 255, 255, 255 });
        m_playSprite->setOpacity(255);
        if (auto kids = m_playSprite->getChildren()) {
            for (auto kid : CCArrayExt<CCNode*>(kids)) {
                if (kid == m_progressTimer) continue;
                if (endsWith(std::string(kid->getID()), "chaz-play-icon")) continue;
                if (typeinfo_cast<CCSprite*>(kid)) kid->setVisible(false);
            }
        }
        auto timer = m_progressTimer;
        if (!timer || !timer->getParent()) return;
        auto tex = squareTexture();
        if (!tex) return;
        auto sprite = timer->getSprite();
        if (!sprite || sprite->getTexture() != tex) {
            auto square = CCSprite::createWithTexture(tex);
            if (!square) return;
            timer->setSprite(square);
            timer->setType(kCCProgressTimerTypeRadial);
            timer->setMidpoint({ 0.5f, 0.5f });
        }
        timer->setVisible(true);
        timer->setColor({ 255, 255, 255 });
        timer->setOpacity(255);
        timer->setAnchorPoint({ 0.5f, 0.5f });

        // the box is 246/314 of the play sprite's frame (see the texture pack);
        // the square sits 7% outside it on every side
        auto frame = m_playSprite->getContentSize();
        float boxSide = std::min(frame.width, frame.height) * 246.f / 314.f * worldScale(m_playSprite);
        auto center = m_playSprite->convertToWorldSpace(frame / 2);
        auto parent = timer->getParent();
        float parentScale = worldScale(parent);
        float texSide = timer->getContentSize().width;
        if (parentScale <= 0.f || texSide <= 0.f) return;
        timer->setPosition(parent->convertToNodeSpace(center));
        timer->setScale(boxSide * 1.14f / (texSide * parentScale));
        timer->setRotation(0.f);
    }
};

// ---------------------------------------------------------------------------
// 7. Sliders: rounded fill
// ---------------------------------------------------------------------------

class $modify(ChazSlider, Slider) {
    void updateBar() {
        Slider::updateBar();
        if (setting("round-fill")) roundFill(m_sliderBar, m_groove);
    }
};
