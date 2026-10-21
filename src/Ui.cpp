#include "Ui.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace {
constexpr char const* POPUP_ID = "predictive-buffer-popup";
}

namespace ui {

void attach(PlayLayer* layer) {
    if (!layer || layer->getChildByID(POPUP_ID)) return;
    auto holder = CCNode::create();
    holder->setID(POPUP_ID);
    layer->addChild(holder, 10000);
}

void popup(std::string const& text, bool good) {
    auto pl = PlayLayer::get();
    if (!pl) return;
    auto holder = pl->getChildByID(POPUP_ID);
    if (!holder) return;
    holder->removeAllChildren();

    auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    label->setScale(0.4f);
    label->setAnchorPoint({ 0.f, 0.f });
    label->setPosition({ 6.f, 6.f });
    label->setColor(good ? ccColor3B { 120, 255, 140 } : ccColor3B { 255, 120, 120 });
    label->setOpacity(200);
    holder->addChild(label);
    label->runAction(CCSequence::create(CCDelayTime::create(1.0f), CCFadeOut::create(0.4f), nullptr));
}

} // namespace ui
