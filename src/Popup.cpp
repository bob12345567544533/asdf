#include "Popup.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace pb::popup {

namespace {
constexpr char const* HOLDER_ID = "predictive-buffer-popup";
}

void attach(PlayLayer* layer) {
    if (!layer || layer->getChildByID(HOLDER_ID)) return;
    auto holder = CCNode::create();
    holder->setID(HOLDER_ID);
    layer->addChild(holder, 10000);
}

void show(std::string const& text, bool good) {
    auto layer = PlayLayer::get();
    if (!layer) return;
    auto holder = layer->getChildByID(HOLDER_ID);
    if (!holder) return;
    holder->removeAllChildren();

    auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    if (!label) return;
    label->setScale(0.4f);
    label->setAnchorPoint({ 0.f, 0.f });
    label->setPosition({ 6.f, 6.f });
    label->setColor(good ? ccColor3B { 120, 255, 140 } : ccColor3B { 255, 120, 120 });
    label->setOpacity(200);
    holder->addChild(label);
    label->runAction(CCSequence::create(CCDelayTime::create(1.f), CCFadeOut::create(0.4f), nullptr));
}

} // namespace pb::popup
