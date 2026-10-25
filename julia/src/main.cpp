// Made for Julia: the cute moving background for Julia's pink texture pack.
// Soft pink clouds with little hearts and flowers floating up, drawn live by a
// shader into a small texture that every menu background shows. One clock for
// every screen, so it keeps floating smoothly when she switches menus.

#include <Geode/Geode.hpp>
#include <Geode/modify/LoadingLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/utils/ColorProvider.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

using namespace geode::prelude;

namespace {

bool setting(char const* key) {
    return Mod::get()->getSettingValue<bool>(key);
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

constexpr double TAU = 6.283185307179586;
double g_clock = 0.0;

constexpr char const* VERT = R"(
attribute vec4 a_position;
varying vec2 v_pos;
void main() {
    gl_Position = CC_MVPMatrix * a_position;
    v_pos = a_position.xy;
}
)";

constexpr char const* FRAG = R"(
varying vec2 v_pos;
uniform vec2 u_size;   // quad size (points)
uniform vec2 u_res;    // render target size (pixels)
uniform vec4 u_offset; // cloud motion
uniform float u_time;  // seconds (speed applied)

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
    for (int i = 0; i < 4; i++) {
        s += noise(p) * amp;
        tot += amp;
        p = mat2(1.6, 1.2, -1.2, 1.6) * p + vec2(17.1, 9.7);
        amp *= 0.5;
    }
    return s / tot;
}

float dot2(vec2 v) { return dot(v, v); }

// heart, about 1 unit tall, centred
float sdHeart(vec2 p) {
    p.y += 0.55;
    p.x = abs(p.x);
    if (p.y + p.x > 1.0) return sqrt(dot2(p - vec2(0.25, 0.75))) - 0.35355;
    return sqrt(min(dot2(p - vec2(0.0, 1.0)), dot2(p - 0.5 * max(p.x + p.y, 0.0)))) * sign(p.x - p.y);
}

// five round petals
float sdFlower(vec2 p) {
    float r = length(p);
    float a = atan(p.y, p.x);
    return r - (0.34 + 0.14 * cos(5.0 * a));
}

// One layer of floating hearts and flowers. Returns colour (rgb) and coverage (a).
vec4 floaters(vec2 uv, float scale, float speed, float seed, float alpha) {
    vec2 q = uv * scale + vec2(0.0, -u_time * speed);
    vec2 cell = floor(q);
    vec2 f = fract(q) - 0.5;
    float h = hash12(cell + seed);
    if (h > 0.42) return vec4(0.0);
    float h2 = hash12(cell + seed + 7.3);
    float h3 = hash12(cell + seed + 13.1);
    vec2 o = (vec2(h2, h3) - 0.5) * 0.45;
    o.x += 0.12 * sin(u_time * 0.9 + h * 40.0); // gentle sway
    float size = 0.22 + 0.12 * h2;
    vec2 p = (f - o) / size;
    float ang = 0.35 * sin(u_time * 0.6 + h3 * 30.0);
    p = mat2(cos(ang), -sin(ang), sin(ang), cos(ang)) * p;
    float aa = 1.6 * scale / (u_res.y * size);
    vec3 col = vec3(1.0);
    float d;
    if (h3 < 0.5) {
        d = sdHeart(p);
    } else {
        d = sdFlower(p);
        float centre = smoothstep(aa, -aa, length(p) - 0.14);
        col = mix(vec3(1.0), vec3(1.0, 0.76, 0.86), centre);
    }
    float cover = smoothstep(aa, -aa, d) * alpha;
    return vec4(col, cover);
}

void main() {
    vec2 uv = v_pos / u_size.y;               // height = 1
    float t = v_pos.y / u_size.y;              // 0 bottom .. 1 top
    vec3 top = vec3(1.0, 0.839, 0.910);
    vec3 bottom = vec3(0.965, 0.627, 0.769);
    vec3 col = mix(bottom, top, t);
    vec2 c = v_pos / u_size - 0.5;
    col = mix(col, vec3(1.0, 0.94, 0.968), 0.28 * clamp(1.0 - 2.2 * dot(c, c), 0.0, 1.0));

    // soft flowing clouds
    vec2 p = mat2(0.8, 0.6, -0.6, 0.8) * (uv * 1.8) + vec2(30.0, 30.0);
    vec2 o = u_offset.xy;
    vec2 qd = vec2(fbm(p + o), fbm(p + vec2(5.2, 1.3) + vec2(-o.y, o.x)));
    float cl = fbm(p + 2.6 * qd + u_offset.zw);
    cl = smoothstep(0.45, 0.85, cl);
    col = mix(col, vec3(1.0, 0.96, 0.98), cl * 0.38);

    // hearts and flowers, far to near
    vec4 l1 = floaters(uv, 7.0, 0.020, 3.0, 0.30);
    col = mix(col, l1.rgb, l1.a);
    vec4 l2 = floaters(uv, 4.6, 0.032, 11.0, 0.42);
    col = mix(col, l2.rgb, l2.a);
    vec4 l3 = floaters(uv, 3.0, 0.046, 29.0, 0.55);
    col = mix(col, l3.rgb, l3.a);

    col += (hash12(gl_FragCoord.xy) - 0.5) / 255.0;
    gl_FragColor = vec4(col, 1.0);
}
)";

class CuteQuad : public CCNode {
public:
    CCGLProgram* m_program = nullptr;
    GLint m_sizeLoc = -1;
    GLint m_resLoc = -1;
    GLint m_offsetLoc = -1;
    GLint m_timeLoc = -1;
    float m_res[2] = { 960.f, 540.f };
    float m_offset[4] = { 0, 0, 0, 0 };
    float m_time = 0.f;

    static CuteQuad* create(CCGLProgram* program) {
        auto ret = new CuteQuad();
        if (ret->init()) {
            ret->m_program = program;
            ret->m_sizeLoc = program->getUniformLocationForName("u_size");
            ret->m_resLoc = program->getUniformLocationForName("u_res");
            ret->m_offsetLoc = program->getUniformLocationForName("u_offset");
            ret->m_timeLoc = program->getUniformLocationForName("u_time");
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
        m_program->setUniformLocationWith2f(m_resLoc, m_res[0], m_res[1]);
        m_program->setUniformLocationWith4f(m_offsetLoc, m_offset[0], m_offset[1], m_offset[2], m_offset[3]);
        m_program->setUniformLocationWith1f(m_timeLoc, m_time);
        ccGLBindVAO(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position);
        GLfloat verts[8] = { 0, 0, size.width, 0, 0, size.height, size.width, size.height };
        glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, verts);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        CC_INCREMENT_GL_DRAWS(1);
    }
};

CCRenderTexture* g_target = nullptr;
CuteQuad* g_quad = nullptr;
bool g_failed = false;
double g_lastDraw = -1.0;
float g_aspect = 0.f;

CCGLProgram* program() {
    constexpr char const* key = "chaz.made-for-julia/cute";
    auto cache = CCShaderCache::sharedShaderCache();
    if (auto p = cache->programForKey(key)) return p;
    auto p = new CCGLProgram();
    if (!p->initWithVertexShaderByteArray(VERT, FRAG)) {
        log::error("cute background: shader didn't compile");
        p->release();
        return nullptr;
    }
    p->addAttribute(kCCAttributeNamePosition, kCCVertexAttrib_Position);
    if (!p->link()) {
        log::error("cute background: shader didn't link");
        p->release();
        return nullptr;
    }
    p->updateUniforms();
    cache->addProgram(p, key);
    p->release();
    return p;
}

// The shared texture every background shows (nullptr if it can't be made).
CCTexture2D* texture() {
    auto win = CCDirector::get()->getWinSize();
    if (g_target && win.height > 0 && std::abs(win.width / win.height - g_aspect) > 0.01f) {
        g_target->release();
        g_quad->release();
        g_target = nullptr;
        g_quad = nullptr;
    }
    if (!g_target) {
        if (g_failed) return nullptr;
        auto p = program();
        float csf = CC_CONTENT_SCALE_FACTOR();
        if (!p || win.width <= 0 || win.height <= 0 || csf <= 0) {
            g_failed = true;
            return nullptr;
        }
        // 1280 px wide (the hearts have crisp edges), the screen's shape; sizes in points
        float pxW = 1280.f;
        float pxH = std::round(1280.f * win.height / win.width);
        int w = std::max(1, static_cast<int>(std::lround(pxW / csf)));
        int h = std::max(1, static_cast<int>(std::lround(pxH / csf)));
        auto target = CCRenderTexture::create(w, h, kCCTexture2DPixelFormat_RGBA8888);
        auto quad = CuteQuad::create(p);
        if (!target || !quad) {
            g_failed = true;
            return nullptr;
        }
        target->retain();
        quad->retain();
        target->getSprite()->getTexture()->setAntiAliasTexParameters();
        quad->setContentSize({ static_cast<float>(w), static_cast<float>(h) });
        quad->m_res[0] = w * csf;
        quad->m_res[1] = h * csf;
        g_target = target;
        g_quad = quad;
        g_lastDraw = -1.0;
        g_aspect = win.width / win.height;
    }
    return g_target->getSprite()->getTexture();
}

// Redraw for the current time (at most 120 times a second).
void redraw() {
    if (!texture()) return;
    if (g_lastDraw >= 0.0 && g_clock - g_lastDraw < 1.0 / 120.0) return;
    g_lastDraw = g_clock;
    double speed = std::clamp(Mod::get()->getSettingValue<double>("speed"), 0.1, 5.0);
    double t = g_clock * speed;
    double a = TAU * t / 28.0;
    double b = TAU * t / 75.6;
    g_quad->m_offset[0] = static_cast<float>(0.6 * std::cos(a));
    g_quad->m_offset[1] = static_cast<float>(0.6 * std::sin(a));
    g_quad->m_offset[2] = static_cast<float>(0.25 * std::cos(b));
    g_quad->m_offset[3] = static_cast<float>(0.25 * std::sin(b));
    // the hearts float upward forever; wrap the clock now and then so floats stay precise
    g_quad->m_time = static_cast<float>(std::fmod(t, 3600.0));
    g_target->beginWithClear(0, 0, 0, 1);
    g_quad->visit();
    g_target->end();
}

// Turn a background sprite into a window onto the moving background, filling the screen.
void show(CCSprite* sprite, CCTexture2D* tex, CCSize const& win) {
    sprite->stopAllActions();
    if (sprite->getTexture() != tex) {
        sprite->setUserObject("cute-host"_spr, CCBool::create(true));
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

struct Visit {
    CCSize win;
    CCTexture2D* tex = nullptr;
    bool found = false;
};

void visit(CCNode* node, Visit& v, int depth) {
    if (!node || depth > 4) return;
    auto children = node->getChildren();
    if (!children || children->count() > 200) return;
    for (auto child : CCArrayExt<CCNode*>(children)) {
        if (typeinfo_cast<GJBaseGameLayer*>(child)) continue; // never inside a level
        std::string_view id = child->getID();
        bool host = endsWith(id, "julia-bg");
        if (!host && id == "background") {
            auto sprite = typeinfo_cast<CCSprite*>(child);
            host = sprite && (sprite->getUserObject("cute-host"_spr) || isSpriteName(sprite, "GJ_gradientBG.png"));
        }
        if (host) {
            if (auto sprite = typeinfo_cast<CCSprite*>(child)) {
                show(sprite, v.tex, v.win);
                v.found = true;
            }
        }
        visit(child, v, depth + 1);
    }
}

void visitScene(CCScene* scene, Visit& v) {
    if (!scene) return;
    if (auto transition = typeinfo_cast<CCTransitionScene*>(scene)) {
        visit(transition->m_pInScene, v, 0);
        visit(transition->m_pOutScene, v, 0);
    }
    visit(scene, v, 0);
}

class Driver : public CCObject {
public:
    void tick(float dt) {
        g_clock += std::min(dt, 0.1f);
        if (!setting("enabled")) return;
        auto director = CCDirector::get();
        Visit v;
        v.win = director->getWinSize();
        v.tex = texture();
        if (!v.tex) return;
        auto scene = director->getRunningScene();
        visitScene(scene, v);
        auto next = director->getNextScene(); // a screen about to appear: ready before its first frame
        if (next && next != scene) visitScene(next, v);
        if (v.found) redraw();
    }
};

void startDriver() {
    static Driver* driver = nullptr;
    if (driver) return;
    driver = new Driver();
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(Driver::tick), driver, 0.f, false);
}

// A full-screen host sprite (main menu, loading screen, Geode's mods screen).
CCSprite* makeHost(char const* id) {
    auto tex = texture();
    if (!tex) return nullptr;
    auto sprite = CCSprite::createWithTexture(tex);
    if (!sprite) return nullptr;
    sprite->setID(id);
    show(sprite, tex, CCDirector::get()->getWinSize());
    return sprite;
}

void addHost(CCNode* layer, char const* id, int z) {
    if (auto sprite = makeHost(id)) layer->addChild(sprite, z);
}

// Geode's mods screen in her pinks. Geode keeps these colors in its ColorProvider, which is
// made for exactly this. Geode puts its own back when the theme setting changes, so this runs
// again every time the mods screen opens.
void pinkGeode() {
    struct Pink { char const* id; ccColor4B color; };
    static Pink const colors[] = {
        { "mod-list-bg", { 255, 214, 230, 255 } },
        { "mod-list-search-bg", { 255, 190, 216, 255 } },
        { "mod-list-tab-deselected-bg", { 255, 255, 255, 255 } },   // the tab art as drawn
        { "mod-list-tab-selected-bg", { 255, 168, 204, 255 } },
        { "mod-list-tab-selected-bg-alt", { 255, 168, 204, 255 } },
        { "mod-list-version-label", { 255, 120, 172, 255 } },
        { "mod-list-featured-color", { 255, 150, 196, 255 } },
        { "keybinds-list-category-label", { 226, 84, 141, 255 } },
        { "mods-layer-gd-bg", { 255, 255, 255, 255 } },
        { "swelvy-bg-0", { 255, 222, 236, 255 } },
        { "swelvy-bg-1", { 255, 206, 226, 255 } },
        { "swelvy-bg-2", { 252, 190, 216, 255 } },
        { "swelvy-bg-3", { 248, 174, 206, 255 } },
        { "swelvy-bg-4", { 244, 158, 196, 255 } },
        { "swelvy-bg-5", { 238, 142, 186, 255 } },
    };
    for (auto const& c : colors) {
        ColorProvider::get()->override(std::string("geode.loader/") + c.id, c.color);
    }
}

} // namespace

$on_game(Loaded) {
    startDriver();
}

$on_mod(Loaded) {
    // after Geode has set up its own colors for the saved theme
    Loader::get()->queueInMainThread([] {
        if (setting("geode-menu")) pinkGeode();
    });
    // Geode asks for its mods screen background here; hand it the moving one
    NodeProvidingEvent("geode.loader/geode-background").listen([](CCNode*& nodeOut, std::string_view) {
        if (nodeOut || !setting("geode-menu")) return;
        pinkGeode();
        if (!setting("enabled")) return;
        if (auto sprite = makeHost("mods-julia-bg"_spr)) {
            nodeOut = sprite;
            startDriver();
        }
    }).leak();
}

class $modify(JuliaLoadingLayer, LoadingLayer) {
    bool init(bool refresh) {
        if (!LoadingLayer::init(refresh)) return false;
        if (setting("enabled") && setting("loading-screen")) {
            CCNode* bg = this->getChildByID("bg-texture");
            if (!bg) {
                for (auto child : CCArrayExt<CCNode*>(this->getChildren())) {
                    if (isSpriteName(child, "game_bg_01_001.png")) { bg = child; break; }
                }
            }
            if (bg) bg->setVisible(false);
            addHost(this, "loading-julia-bg"_spr, -100);
            startDriver();
        }
        return true;
    }
};

class $modify(JuliaMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (setting("enabled") && setting("main-menu")) {
            // the running icons' level background makes way for the cute one
            if (auto game = this->getChildByID("main-menu-bg")) game->setVisible(false);
            addHost(this, "menu-julia-bg"_spr, -100);
        }
        return true;
    }
};
